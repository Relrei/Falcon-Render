/* SPDX-FileCopyrightText: 2026 Falcon experiment
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon SHARC: Spatial Hash Radiance Cache (experimental).
 *
 * Modeled after NVIDIA SHARC. The cache hashes world-space positions into a
 * flat table of cells, each accumulating radiance and a sample count, so paths
 * that land in an already-sampled region can reuse cached lighting instead of
 * tracing more bounces.
 *
 * Step 1 (this file): the data structure only -- world-space hashing plus
 * deposit/lookup helpers over a plain float buffer. No path termination yet;
 * that is Step 2. Everything is gated behind __FALCON_SHARC__ so a stock build
 * is unchanged. */

#pragma once

#include "kernel/integrator/falcon_sharc_size.h"
#include "kernel/types.h"

#include "util/atomic.h"
#include "util/hash.h"

CCL_NAMESPACE_BEGIN

#ifdef __FALCON_SHARC__

/* Table size lives in falcon_sharc_size.h, which the host includes too.
 * Writing it out here as well is what made a subset-edit look like a valid
 * experiment (see that header). */

/* World-space voxel size used by the Step 1b-i hash debug visualization. */
#  ifndef FALCON_SHARC_DEBUG_CELL_SIZE
#    define FALCON_SHARC_DEBUG_CELL_SIZE 0.2f
#  endif

/* World-space voxel size for one cell.
 *
 * Constant grid for the first increment. Step 3 will switch to a distance-based
 * level-of-detail so far-away cells are coarser. `scene_scale` lets the caller
 * tie cell size to the scene's overall extent. */
ccl_device_inline float falcon_sharc_cell_size(const float scene_scale)
{
  return scene_scale;
}

/* Map a world position to a cell index in [0, FALCON_SHARC_CELL_COUNT). */
ccl_device_inline uint falcon_sharc_hash(const float3 P, const float cell_size)
{
  const float inv = 1.0f / cell_size;
  const uint gx = (uint)((int)floorf(P.x * inv));
  const uint gy = (uint)((int)floorf(P.y * inv));
  const uint gz = (uint)((int)floorf(P.z * inv));
  return hash_uint3(gx, gy, gz) & FALCON_SHARC_CELL_MASK;
}

/* Accumulate a radiance sample into the cell covering P. */
ccl_device_inline void falcon_sharc_deposit(ccl_global float *cache,
                                            const float3 P,
                                            const float cell_size,
                                            const float3 radiance)
{
  const uint base = falcon_sharc_hash(P, cell_size) * FALCON_SHARC_CELL_STRIDE;
  atomic_add_and_fetch_float(&cache[base + 0], radiance.x);
  atomic_add_and_fetch_float(&cache[base + 1], radiance.y);
  atomic_add_and_fetch_float(&cache[base + 2], radiance.z);
  atomic_add_and_fetch_float(&cache[base + 3], 1.0f);
}

/* Read the averaged radiance stored at P. Returns false for an empty cell. */
ccl_device_inline bool falcon_sharc_lookup(ccl_global const float *cache,
                                           const float3 P,
                                           const float cell_size,
                                           ccl_private float3 *radiance)
{
  const uint base = falcon_sharc_hash(P, cell_size) * FALCON_SHARC_CELL_STRIDE;
  const float count = cache[base + 3];
  if (count < 1.0f) {
    return false;
  }
  const float inv = 1.0f / count;
  *radiance = make_float3(cache[base + 0] * inv, cache[base + 1] * inv, cache[base + 2] * inv);
  return true;
}

/* Read the measured relative error at P. Returns a negative value when the
 * cell was never filled (the probe only reaches what the camera could see), so
 * callers can tell "no evidence" from "evidence of low error" -- an unfilled
 * cell must never be read as "converged, safe to stop". */
ccl_device_inline float falcon_error_lookup(ccl_global const float *field,
                                            const float3 P,
                                            const float cell_size)
{
  return field[falcon_sharc_hash(P, cell_size)];
}

/* Collision tag for photon cells. hash_uint3 picks the cell, so two distant
 * world cells can share a slot and a lookup on one silently reads the other's
 * flux (bright squares floating off-surface). A second, input-decorrelated
 * hash of the SAME grid coords is stored in the count field as
 * 1.0 + tag * 2^-20 (exactly representable; still >= 1.0 for the generic
 * validity test, and the SHARC mean-lookup's divide is off by at most 1e-6).
 * The photon lookup recomputes the expected tag and rejects mismatched cells,
 * turning collision false-positives into holes the gather fills from valid
 * neighbors. Same-cell rewrites are idempotent; a real collision leaves the
 * last writer's tag so at least one site rejects the mixed cell. */
ccl_device_inline float falcon_photon_tag(const uint gx, const uint gy, const uint gz)
{
  const uint t = hash_uint3(gx ^ 0x517cc1b7u, gy ^ 0x27220a95u, gz ^ 0xfe4db3afu) & 0xFFFFFu;
  return 1.0f + (float)t * (1.0f / 1048576.0f);
}

/* Linear probing over a short chain, so two sites that hash together do not
 * have to share one slot.
 *
 * The tag alone only tells the LOSER of a collision to stay away; the winner
 * still reads its own light plus the other site's and prints it. When the other
 * site is a caustic core and this one is dim seabed, that is a single pixel at
 * the core's brightness in the middle of nothing -- the isolated white dots
 * measured on ocean (火の粉 45 vs 3 for the beauty alone, and 1 for light
 * tracing, tools/caustic_metrics.py).
 *
 * The step is a second, independent hash of the same coordinates, so two sites
 * that collide at i=0 do not walk the same chain afterwards. Making it odd
 * keeps the chain inside the whole table (the size is a power of two). */
#define FALCON_PHOTON_PROBES 4

ccl_device_inline uint falcon_photon_slot(const uint gx, const uint gy, const uint gz, const int i)
{
  const uint h = hash_uint3(gx, gy, gz);
  const uint step = hash_uint3(gz ^ 0x9e3779b9u, gx ^ 0x85ebca6bu, gy ^ 0xc2b2ae35u) | 1u;
  return ((h + (uint)i * step) & FALCON_SHARC_CELL_MASK) * FALCON_SHARC_CELL_STRIDE;
}

/* Where this cell may accumulate: its own slot if it already claimed one, else
 * the first free slot on the chain, claimed with a compare-and-swap. Returns -1
 * when the whole chain belongs to other sites; the caller drops the photon.
 *
 * The CAS is not decoration. Claiming with a plain store loses light: two sites
 * both see a slot free, both add radiance there, and only one tag survives --
 * the loser's photons sit in a slot it can never read back. Measured that way
 * on ocean against the brute-force judge, the layer came out 5% short in EVERY
 * band (0.973 -> 0.924). Probing turns a collision from "too bright" into
 * "missing" unless the claim is atomic.
 *
 * Dropping the photon on a full chain is deliberate: adding it to a slot owned
 * by somebody else is precisely the bright-dot bug this exists to remove, and
 * at the default cell size (11% occupancy) four consecutive collisions are
 * rare enough that losing those photons is the cheaper error. */
ccl_device_inline int falcon_photon_slot_write(ccl_global float *cache,
                                               const uint gx,
                                               const uint gy,
                                               const uint gz,
                                               const float tag)
{
  for (int i = 0; i < FALCON_PHOTON_PROBES; i++) {
    const uint base = falcon_photon_slot(gx, gy, gz, i);
    const float t = cache[base + 3];
    if (t == tag) {
      return (int)base;
    }
    if (t == 0.0f) {
      const float prev = atomic_compare_and_swap_float(&cache[base + 3], 0.0f, tag);
      if (prev == 0.0f || prev == tag) {
        return (int)base;
      }
    }
  }
  return -1;
}

/* Where this cell's light actually is, or -1 if it never got a slot. An empty
 * slot ends the chain: nothing past it can belong to this cell, because a
 * deposit would have taken that free slot first. */
ccl_device_inline int falcon_photon_slot_read(ccl_global const float *cache,
                                              const uint gx,
                                              const uint gy,
                                              const uint gz,
                                              const float tag)
{
  for (int i = 0; i < FALCON_PHOTON_PROBES; i++) {
    const uint base = falcon_photon_slot(gx, gy, gz, i);
    const float t = cache[base + 3];
    if (t == tag) {
      return (int)base;
    }
    if (t == 0.0f) {
      return -1;
    }
  }
  return -1;
}

/* Photon-pass deposit with a tangent-plane gaussian splat (mirrors the host
 * tracer's splat_trilinear). Each photon spreads its radiance over a 2x2x2
 * cell-centered stencil (q = P/h - 0.5, so cell centers sit at integer q):
 * gaussian in the two surface-tangent axes, trilinear (tent) along the deposit
 * normal axis -- spreading along the normal leaks flux into cells the surface
 * lookup never reads (the host measured 0.64x on the ocean floor). Weights are
 * normalized per photon so total flux is preserved; nearest-cell lookup then
 * reads a smooth field instead of isolated single-cell shot noise. Radiance is
 * accumulated as a plain SUM with count pinned to 1 as a validity flag (unlike
 * falcon_sharc_deposit, whose count divides into a mean). normal_axis in
 * {0,1,2} selects the trilinear axis. */
/* Photon-map deposit: fixed-radius kernel density estimation. Each photon
 * spreads its flux over a disk of radius r = radius_cells * cell_size in the
 * surface tangent plane (2D cone kernel W(t) = (3/pi r^2)(1 - t), which
 * integrates to 1 over the disk, so total flux is conserved), and a 1-cell tent
 * along the deposit normal (so flux does not leak into cells the surface lookup
 * never reads). Accumulating this over all photons gives a smooth radiance
 * field -- sparse caustic regions become dim smooth glow instead of the hard
 * colored cells the 2x2x2 splat produced (validated offline, Milestone A).
 * `flux_albedo` is the photon's per-channel flux * receiver albedo (NO area
 * division: the cone normalization supplies 1/(pi r^2)). Falls back to the
 * legacy narrow splat when radius_cells <= 1. */
ccl_device_inline void falcon_photon_deposit_wide(ccl_global float *cache,
                                                  const float3 P,
                                                  const float cell_size,
                                                  const float radius_cells,
                                                  const float3 flux_albedo,
                                                  const int normal_axis)
{
  const float inv = 1.0f / cell_size;
  const float r = radius_cells * cell_size;
  /* Two divisions by pi, and they are not the same pi. The first normalizes the
   * 2D cone so it integrates to 1 over the disk (that is the 3/(pi r^2)); the
   * second is the Lambertian BRDF's 1/pi, turning the irradiance this photon
   * deposits into the outgoing radiance the lookup reads back. The narrow
   * rc <= 1 path applies the second one at the call site as inv_area =
   * 1/(pi cell^2); this path was missing it, so every wide splat came out pi
   * times too bright -- the same 1/pi that was found and fixed for the point
   * map on 2026-07-05 and has since regressed here. Measured on glasszoo by
   * sweeping radius_cells: energy/ref 1.1059 at rc=1 against 1.4062 / 1.4069 /
   * 1.4073 at rc=2/3/4, which solves to a caustic component exactly pi times
   * over (the flat response across rc>1 confirms the cone itself is right). */
  const float cone = 3.0f / (M_PI_F * M_PI_F * r * r);
  const int R = (int)ceilf(radius_cells);
  const int bx = (int)floorf(P.x * inv);
  const int by = (int)floorf(P.y * inv);
  const int bz = (int)floorf(P.z * inv);
  for (int dx = -R; dx <= R; dx++) {
    for (int dy = -R; dy <= R; dy++) {
      for (int dz = -R; dz <= R; dz++) {
        const int gx = bx + dx, gy = by + dy, gz = bz + dz;
        const float cx = ((float)gx + 0.5f) * cell_size;
        const float cy = ((float)gy + 0.5f) * cell_size;
        const float cz = ((float)gz + 0.5f) * cell_size;
        const float ox = cx - P.x, oy = cy - P.y, oz = cz - P.z;
        /* thin (tent, +-1 cell) along the normal axis; cone over the tangent */
        float on, ot0, ot1;
        if (normal_axis == 0) { on = ox; ot0 = oy; ot1 = oz; }
        else if (normal_axis == 1) { on = oy; ot0 = ox; ot1 = oz; }
        else { on = oz; ot0 = ox; ot1 = oy; }
        const float tent = fmaxf(0.0f, 1.0f - fabsf(on) * inv);
        if (tent <= 0.0f) {
          continue;
        }
        const float dtan = sqrtf(ot0 * ot0 + ot1 * ot1);
        const float t = dtan / r;
        if (t >= 1.0f) {
          continue;
        }
        const float w = cone * (1.0f - t) * tent;
        const uint ux = (uint)gx, uy = (uint)gy, uz = (uint)gz;
        const int slot = falcon_photon_slot_write(cache, ux, uy, uz,
                                                  falcon_photon_tag(ux, uy, uz));
        if (slot < 0) {
          continue;
        }
        const uint base = (uint)slot;
        atomic_add_and_fetch_float(&cache[base + 0], flux_albedo.x * w);
        atomic_add_and_fetch_float(&cache[base + 1], flux_albedo.y * w);
        atomic_add_and_fetch_float(&cache[base + 2], flux_albedo.z * w);
      }
    }
  }
}

ccl_device_inline void falcon_photon_deposit(ccl_global float *cache,
                                             const float3 P,
                                             const float cell_size,
                                             const float3 radiance,
                                             const int normal_axis)
{
  const float inv = 1.0f / cell_size;
  /* Cell-centered coordinate: cell centers sit at integer q. */
  const float qx = P.x * inv - 0.5f;
  const float qy = P.y * inv - 0.5f;
  const float qz = P.z * inv - 0.5f;
  const int bx = (int)floorf(qx);
  const int by = (int)floorf(qy);
  const int bz = (int)floorf(qz);
  const float fx = qx - (float)bx;
  const float fy = qy - (float)by;
  const float fz = qz - (float)bz;
  /* SPLAT_SIGMA = 1.0 cell (matches the host default); sig2 = 2*(sigma*0.5)^2. */
  const float sig2 = 0.5f;

  /* Separable per-axis weights for the two offsets d in {0, 1}. */
  float wx[2], wy[2], wz[2];
  for (int d = 0; d < 2; d++) {
    const float ax = (float)d - fx;
    const float ay = (float)d - fy;
    const float az = (float)d - fz;
    wx[d] = (normal_axis == 0) ? fmaxf(0.0f, 1.0f - fabsf(ax)) : expf(-(ax * ax) / sig2);
    wy[d] = (normal_axis == 1) ? fmaxf(0.0f, 1.0f - fabsf(ay)) : expf(-(ay * ay) / sig2);
    wz[d] = (normal_axis == 2) ? fmaxf(0.0f, 1.0f - fabsf(az)) : expf(-(az * az) / sig2);
  }

  float wsum = 0.0f;
  for (int dx = 0; dx < 2; dx++) {
    for (int dy = 0; dy < 2; dy++) {
      for (int dz = 0; dz < 2; dz++) {
        wsum += wx[dx] * wy[dy] * wz[dz];
      }
    }
  }
  if (!(wsum > 0.0f)) {
    return;
  }
  const float norm = 1.0f / wsum;

  for (int dx = 0; dx < 2; dx++) {
    for (int dy = 0; dy < 2; dy++) {
      for (int dz = 0; dz < 2; dz++) {
        const float w = wx[dx] * wy[dy] * wz[dz] * norm;
        if (!(w > 0.0f)) {
          continue;
        }
        const uint gx = (uint)(bx + dx);
        const uint gy = (uint)(by + dy);
        const uint gz = (uint)(bz + dz);
        const int slot = falcon_photon_slot_write(cache, gx, gy, gz,
                                                  falcon_photon_tag(gx, gy, gz));
        if (slot < 0) {
          continue;
        }
        const uint base = (uint)slot;
        atomic_add_and_fetch_float(&cache[base + 0], radiance.x * w);
        atomic_add_and_fetch_float(&cache[base + 1], radiance.y * w);
        atomic_add_and_fetch_float(&cache[base + 2], radiance.z * w);
      }
    }
  }
}

/* Photon-cache lookup with a gather symmetric to falcon_photon_deposit: read
 * the same 2x2x2 cell-centered stencil with the same tangent-gaussian /
 * normal-tent weights, average the *valid* cells and renormalize by their
 * weight sum. A single hot undersampled cell then contributes to its
 * neighborhood with weight < 1 instead of printing as an isolated sparkle,
 * and empty neighbors clamp the interpolation at filament edges instead of
 * darkening them. The caller passes the SAME half-cell normal-offset point
 * used at deposit time so the two stencils align exactly (the old
 * nearest-cell lookup read the un-offset surface point, which mismatched
 * deposits near cell boundaries). Cells store outgoing radiance (intensive),
 * so averaging is interpolation, not double counting. */
ccl_device_inline bool falcon_photon_lookup(ccl_global const float *cache,
                                            const float3 P,
                                            const float cell_size,
                                            const int normal_axis,
                                            const int nearest,
                                            ccl_private float3 *radiance)
{
  const float inv = 1.0f / cell_size;
  if (nearest) {
    /* The deposit already spread this photon over the stencil with weights that
     * sum to one, so the cell holds the reconstructed radiance at its own
     * centre. Averaging the stencil again applies the same kernel a second time
     * (w^2), which is only harmless where the photon density is flat -- exactly
     * what a caustic is not. Read the one cell the shading point lands in. */
    /* Same cell-centred convention as the deposit (q = P/h - 0.5, centres on
     * integer q). Without the -0.5 this picked the neighbouring cell whenever
     * the surface sat past the half-cell mark, so the two layers summed below
     * were not the two the deposit had written -- half the phases were read one
     * cell too far and kept their error. */
    const int cx = (int)floorf(P.x * inv - 0.5f);
    const int cy = (int)floorf(P.y * inv - 0.5f);
    const int cz = (int)floorf(P.z * inv - 0.5f);
    const uint gx = (uint)cx, gy = (uint)cy, gz = (uint)cz;
    const uint base = (hash_uint3(gx, gy, gz) & FALCON_SHARC_CELL_MASK) *
                      FALCON_SHARC_CELL_STRIDE;
    /* Sum along the normal, do not average. The deposit splits one photon
     * across the two layers that straddle the surface with weights that add to
     * one, so reading a single layer returns only that layer's share -- and
     * which share depends on where the surface happens to fall between cell
     * boundaries. Measured on ocean by sliding the seabed through one cell:
     * the caustic core swings 0.85 / 1.04 / 0.85 / 1.04 with a half-cell
     * period, purely from that phase. Adding both layers back recovers the
     * whole photon regardless of phase. The tangent plane is a different
     * question -- there the cells are neighbouring estimates of one field, so
     * the fallback below still interpolates across them. */
    /* When only one of the two layers is readable -- the other lost its slot to
     * a distant site -- summing what is there returns just that layer's share
     * of the photon, which is whatever the tent gave it. That share is not
     * random noise but a fixed fraction, so the pixel comes out at a definite
     * step below its neighbours: measured on ocean, the dark specks sit at
     * 0.489x the surrounding median, one pixel wide. Recover the whole by
     * dividing by the weight actually collected. The tent weights are the same
     * ones the deposit used, so they can be recomputed here from the position. */
    const float qn = ((normal_axis == 0) ? P.x : ((normal_axis == 1) ? P.y : P.z)) * inv - 0.5f;
    const float fn = qn - floorf(qn);
    /* nearest == 2: average the two tangential neighbours into each layer
     * before summing the layers. Cells hold outgoing radiance (intensive) and
     * the tangential weights below are renormalised to 1, so this is a blur,
     * not a second division -- the total light is untouched and only the
     * cell-scale shot noise gets smoothed. (The generic gather further down is
     * a different thing: it averages ACROSS the normal layers too, which
     * returns half a photon; that path exists only to fill holes.) */
    const int soft = (nearest > 1) ? 1 : 0;
    float tw[2][2];
    if (soft) {
      const int ta = (normal_axis == 0) ? 1 : 0;             /* tangent axes */
      const int tb = (normal_axis == 2) ? 1 : 2;
      const float qa = ((ta == 0) ? P.x : ((ta == 1) ? P.y : P.z)) * inv - 0.5f;
      const float qb = ((tb == 0) ? P.x : ((tb == 1) ? P.y : P.z)) * inv - 0.5f;
      const float fa = qa - floorf(qa), fb = qb - floorf(qb);
      /* same gaussian the deposit splats with (sig2 = 0.5), renormalised */
      const float a0 = expf(-(fa * fa) / 0.5f), a1 = expf(-((1.0f - fa) * (1.0f - fa)) / 0.5f);
      const float b0 = expf(-(fb * fb) / 0.5f), b1 = expf(-((1.0f - fb) * (1.0f - fb)) / 0.5f);
      const float na = 1.0f / (a0 + a1), nb = 1.0f / (b0 + b1);
      tw[0][0] = a0 * na * b0 * nb;
      tw[0][1] = a0 * na * b1 * nb;
      tw[1][0] = a1 * na * b0 * nb;
      tw[1][1] = a1 * na * b1 * nb;
    }
    float3 acc = make_float3(0.0f, 0.0f, 0.0f);
    float wn = 0.0f;
    for (int dn = 0; dn < 2; dn++) {
      const uint nx = gx + ((normal_axis == 0) ? (uint)dn : 0u);
      const uint ny = gy + ((normal_axis == 1) ? (uint)dn : 0u);
      const uint nz = gz + ((normal_axis == 2) ? (uint)dn : 0u);
      if (soft) {
        const int ta = (normal_axis == 0) ? 1 : 0;
        const int tb = (normal_axis == 2) ? 1 : 2;
        float3 lay = make_float3(0.0f, 0.0f, 0.0f);
        float lw = 0.0f;
        for (int da = 0; da < 2; da++) {
          for (int db = 0; db < 2; db++) {
            const uint sx = nx + ((ta == 0) ? (uint)da : ((tb == 0) ? (uint)db : 0u));
            const uint sy = ny + ((ta == 1) ? (uint)da : ((tb == 1) ? (uint)db : 0u));
            const uint sz = nz + ((ta == 2) ? (uint)da : ((tb == 2) ? (uint)db : 0u));
            const int s = falcon_photon_slot_read(cache, sx, sy, sz,
                                                  falcon_photon_tag(sx, sy, sz));
            if (s < 0) {
              continue;
            }
            const float w = tw[da][db];
            lay.x += cache[(uint)s + 0] * w;
            lay.y += cache[(uint)s + 1] * w;
            lay.z += cache[(uint)s + 2] * w;
            lw += w;
          }
        }
        if (lw > 1e-6f) {
          /* Renormalise by the weight actually collected: a missing neighbour
           * must not darken the layer, the same argument as the tent below. */
          const float r = 1.0f / lw;
          acc.x += lay.x * r;
          acc.y += lay.y * r;
          acc.z += lay.z * r;
          wn += (dn == 0) ? (1.0f - fn) : fn;
        }
        continue;
      }
      const int nslot = falcon_photon_slot_read(cache, nx, ny, nz,
                                                falcon_photon_tag(nx, ny, nz));
      if (nslot < 0) {
        continue;
      }
      const uint nbase = (uint)nslot;
      acc.x += cache[nbase + 0];
      acc.y += cache[nbase + 1];
      acc.z += cache[nbase + 2];
      wn += (dn == 0) ? (1.0f - fn) : fn;
    }
    if (wn > 1e-4f) {
      const float rn = 1.0f / wn;
      *radiance = make_float3(acc.x * rn, acc.y * rn, acc.z * rn);
      return true;
    }
    /* The cell exists but a distant site took its slot, so its own flux is not
     * readable. Fall through to the interpolating gather, which fills the hole
     * from whichever neighbours still own their slots -- that is what the
     * interpolation was there for. Reading nearest *first* keeps the deposit's
     * own reconstruction intact everywhere it survived; only collisions pay the
     * second kernel application. */
  }
  const float qx = P.x * inv - 0.5f;
  const float qy = P.y * inv - 0.5f;
  const float qz = P.z * inv - 0.5f;
  const int bx = (int)floorf(qx);
  const int by = (int)floorf(qy);
  const int bz = (int)floorf(qz);
  const float fx = qx - (float)bx;
  const float fy = qy - (float)by;
  const float fz = qz - (float)bz;
  const float sig2 = 0.5f; /* must match falcon_photon_deposit */

  float wx[2], wy[2], wz[2];
  for (int d = 0; d < 2; d++) {
    const float ax = (float)d - fx;
    const float ay = (float)d - fy;
    const float az = (float)d - fz;
    wx[d] = (normal_axis == 0) ? fmaxf(0.0f, 1.0f - fabsf(ax)) : expf(-(ax * ax) / sig2);
    wy[d] = (normal_axis == 1) ? fmaxf(0.0f, 1.0f - fabsf(ay)) : expf(-(ay * ay) / sig2);
    wz[d] = (normal_axis == 2) ? fmaxf(0.0f, 1.0f - fabsf(az)) : expf(-(az * az) / sig2);
  }

  float3 acc = make_float3(0.0f, 0.0f, 0.0f);
  float wsum = 0.0f;
  for (int dx = 0; dx < 2; dx++) {
    for (int dy = 0; dy < 2; dy++) {
      for (int dz = 0; dz < 2; dz++) {
        const float w = wx[dx] * wy[dy] * wz[dz];
        if (!(w > 0.0f)) {
          continue;
        }
        const uint gx = (uint)(bx + dx);
        const uint gy = (uint)(by + dy);
        const uint gz = (uint)(bz + dz);
        const int slot = falcon_photon_slot_read(cache, gx, gy, gz,
                                                 falcon_photon_tag(gx, gy, gz));
        if (slot < 0) {
          continue; /* empty, or no slot left on this cell's probe chain */
        }
        const uint base = (uint)slot;
        acc.x += cache[base + 0] * w;
        acc.y += cache[base + 1] * w;
        acc.z += cache[base + 2] * w;
        wsum += w;
      }
    }
  }
  if (!(wsum > 0.0f)) {
    return false;
  }
  const float norm = 1.0f / wsum;
  *radiance = make_float3(acc.x * norm, acc.y * norm, acc.z * norm);
  return true;
}

/* ---- Falcon Photon POINT map (Round 9, LuxCore-faithful) ----------------
 *
 * Photons stored as raw points: stride 9 floats = pos3, flux3 (per-channel
 * flux * receiver albedo, single-channel colored under dispersion), normal3.
 * The bake appends via an atomic counter; the host then sorts photon indices
 * into a uniform neighbor grid whose cell size EQUALS the lookup radius, so a
 * radius-r gather only ever touches the 3x3x3 neighborhood. Grid slots come
 * from the same hash_uint3; slot collisions merely add photons that fail the
 * distance test (correctness is position-based, no tags needed). Lookup is
 * fixed-radius density estimation with a 2D cone kernel (3/(pi r^2))(1 - d/r)
 * and LuxCore-style normal-angle rejection. No quantization anywhere: this is
 * the estimator Milestone A validated, evaluated at the exact shading point. */
#  define FALCON_PHOTON_POINT_STRIDE 9
#  define FALCON_PHOTON_GRID_SIZE (1 << 22)
#  define FALCON_PHOTON_GRID_MASK (FALCON_PHOTON_GRID_SIZE - 1)

ccl_device_inline uint falcon_photon_grid_key(const int gx, const int gy, const int gz)
{
  return hash_uint3((uint)gx, (uint)gy, (uint)gz) & FALCON_PHOTON_GRID_MASK;
}

/* Bake: append one photon. Plain stores after an atomic slot grab; the buffer
 * is only read after the bake pass completes. Returns silently when full. */
ccl_device_inline void falcon_photon_point_append(ccl_global float *points,
                                                  ccl_global uint *counter,
                                                  const int max_points,
                                                  const float3 P,
                                                  const float3 flux,
                                                  const float3 n)
{
  const uint slot = atomic_fetch_and_add_uint32(counter, 1u);
  if (slot >= (uint)max_points) {
    return;
  }
  ccl_global float *p = points + (size_t)slot * FALCON_PHOTON_POINT_STRIDE;
  p[0] = P.x;
  p[1] = P.y;
  p[2] = P.z;
  p[3] = flux.x;
  p[4] = flux.y;
  p[5] = flux.z;
  p[6] = n.x;
  p[7] = n.y;
  p[8] = n.z;
}

/* Add-mode gather at a shading point: L = sum(flux_i * (1 - d_i/r)) * 3/(pi r^2)
 * over photons within r whose deposit normal agrees with the shading normal
 * (dot > cos_min). Returns false when nothing was gathered. */
ccl_device_inline bool falcon_photon_point_lookup(ccl_global const float *points,
                                                  ccl_global const uint *grid_start,
                                                  ccl_global const uint *grid_count,
                                                  ccl_global const uint *grid_index,
                                                  const float3 P,
                                                  const float3 Ns,
                                                  const float radius,
                                                  const float cos_min,
                                                  ccl_private float3 *radiance)
{
  const float inv = 1.0f / radius;
  const float r2 = radius * radius;
  const int bx = (int)floorf(P.x * inv);
  const int by = (int)floorf(P.y * inv);
  const int bz = (int)floorf(P.z * inv);
  float3 acc = make_float3(0.0f, 0.0f, 0.0f);
  bool any = false;
  for (int dx = -1; dx <= 1; dx++) {
    for (int dy = -1; dy <= 1; dy++) {
      for (int dz = -1; dz <= 1; dz++) {
        const uint key = falcon_photon_grid_key(bx + dx, by + dy, bz + dz);
        const uint start = grid_start[key];
        const uint count = grid_count[key];
        for (uint i = 0; i < count; i++) {
          ccl_global const float *p = points +
                                      (size_t)grid_index[start + i] * FALCON_PHOTON_POINT_STRIDE;
          const float ox = p[0] - P.x, oy = p[1] - P.y, oz = p[2] - P.z;
          const float d2 = ox * ox + oy * oy + oz * oz;
          if (d2 >= r2) {
            continue;
          }
          if (p[6] * Ns.x + p[7] * Ns.y + p[8] * Ns.z < cos_min) {
            continue;
          }
          const float w = 1.0f - sqrtf(d2) * inv;
          acc.x += p[3] * w;
          acc.y += p[4] * w;
          acc.z += p[5] * w;
          any = true;
        }
      }
    }
  }
  if (!any) {
    return false;
  }
  /* Density estimation gives E * albedo (the albedo is baked into the stored
   * flux); the outgoing radiance of a Lambertian receiver is E * albedo / pi.
   * This 1/pi was missing: the point map measured pi x the calibrated LT
   * layer on the same scene (audit 2026-07-05, ratio 2.91 ~= pi). */
  const float cone = 3.0f / (M_PI_F * M_PI_F * r2);
  *radiance = make_float3(acc.x * cone, acc.y * cone, acc.z * cone);
  return true;
}

/* Dominant axis of a normal (argmax |n|, low index on ties): the trilinear
 * axis shared by the photon splat and its symmetric lookup gather. */
ccl_device_inline int falcon_photon_normal_axis(const float3 n)
{
  int axis = 0;
  float amax = fabsf(n.x);
  if (fabsf(n.y) > amax) {
    amax = fabsf(n.y);
    axis = 1;
  }
  if (fabsf(n.z) > amax) {
    axis = 2;
  }
  return axis;
}

#endif /* __FALCON_SHARC__ */

CCL_NAMESPACE_END
