/* SPDX-FileCopyrightText: 2026 Falcon experiment
 *
 * SPDX-License-Identifier: Apache-2.0 */

/* Falcon Light Tracing (CyclesFQ): cache-free caustics. A photon that reaches
 * its first diffuse hit connects DIRECTLY to the camera -- project the vertex to
 * a pixel and atomic-splat its flux there -- instead of depositing into the
 * point map. Re-traced every sample, so the caustic converges with SPP (no
 * frozen density estimate, no gather, no dots). See FALCON_LIGHTTRACE.md.
 *
 * P1 scaffold: projection + splat plumbing only. The light-trace render is a
 * full camera-resolution frame whose film buffer is KEPT (caustic-only image);
 * shade_surface splats a constant color at the projected pixel. No visibility,
 * BSDF, or camera importance yet -- those are P2/P3. */

#pragma once

#include "kernel/camera/camera.h"
#include "util/atomic.h"

CCL_NAMESPACE_BEGIN

#ifdef __FALCON_SHARC__

/* Project a world point to integer raster pixel. Returns false if behind the
 * camera or outside the frame. Perspective/ortho only (panorama caustics are a
 * later problem). worldtoraster maps world -> raster (pixels) directly. */
ccl_device_inline bool falcon_lt_project(KernelGlobals kg,
                                         const float3 P,
                                         ccl_private int *px,
                                         ccl_private int *py)
{
  if (kernel_data.cam.type != CAMERA_PERSPECTIVE &&
      kernel_data.cam.type != CAMERA_ORTHOGRAPHIC)
  {
    return false;
  }
  /* Reject points behind the camera. In Cycles camera space FORWARD = +Z
   * (camera_sample_perspective uses focaldistance / D.z with D.z > 0 in front,
   * camera.h:82), so points in front have z > 0; reject z <= 0. */
  const Transform worldtocamera = kernel_data.cam.worldtocamera;
  const float3 Pcam = transform_point(&worldtocamera, P);
  if (kernel_data.cam.type == CAMERA_PERSPECTIVE && Pcam.z <= 0.0f) {
    return false;
  }

  const ProjectionTransform worldtoraster = kernel_data.cam.worldtoraster;
  const float3 raster = transform_perspective(&worldtoraster, P);
  const int rx = (int)floorf(raster.x);
  const int ry = (int)floorf(raster.y);
  if (rx < 0 || rx >= (int)kernel_data.cam.width || ry < 0 ||
      ry >= (int)kernel_data.cam.height)
  {
    return false;
  }
  *px = rx;
  *py = ry;
  return true;
}

/* Physical connection of a diffuse caustic vertex to a pinhole/thin-lens
 * perspective camera (Veach t=1 strategy; PBRT PerspectiveCamera::We). A photon
 * carrying `flux` reaches vertex P with shading normal N on a Lambertian
 * receiver of albedo `diff`; the RGB energy splatted at its pixel is
 *   L = flux * (diff/pi) * cos_surf * We * cos_lens / dist^2
 * with We = 1 / (A * cos_lens^4), A = image-plane area at unit camera distance,
 * cos_lens = forward-cosine of the vertex->camera ray, cos_surf = |N . to_cam|,
 * dist = ||vertex - camera||. The result is scaled by `samples` to CANCEL the
 * film's /sample_count divide: the photon flux is already normalized by the
 * TOTAL photon count (watts/n_photons), so the per-sample average the combined
 * pass computes would otherwise come out `samples`x too dim. Valid for fixed
 * (non-adaptive) sampling. GAIN stays a labeled knob and should sit near 1 once
 * the physics is right. Returns zero if the vertex is behind/edge-on. */
ccl_device_inline float3 falcon_lt_connect(KernelGlobals kg,
                                           const float3 P,
                                           const float3 N,
                                           const float3 flux,
                                           const float3 diff)
{
  const Transform worldtocamera = kernel_data.cam.worldtocamera;
  const float3 Pcam = transform_point(&worldtocamera, P);
  const float dist2 = dot(Pcam, Pcam);
  if (!(dist2 > 0.0f) || Pcam.z <= 0.0f) {
    return zero_float3();
  }
  const float inv_dist = 1.0f / sqrtf(dist2);
  const float cos_lens = Pcam.z * inv_dist; /* connection ray vs optical axis */
  if (cos_lens <= 1e-4f) {
    return zero_float3();
  }

  /* Image-plane area at unit camera distance (PBRT: raster corners -> z=1). */
  const ProjectionTransform rastertocamera = kernel_data.cam.rastertocamera;
  float3 c0 = transform_perspective(&rastertocamera, make_float3(0.0f, 0.0f, 0.0f));
  float3 c1 = transform_perspective(
      &rastertocamera, make_float3(kernel_data.cam.width, kernel_data.cam.height, 0.0f));
  c0 /= c0.z;
  c1 /= c1.z;
  const float A = fabsf((c1.x - c0.x) * (c1.y - c0.y));
  if (!(A > 0.0f)) {
    return zero_float3();
  }

  const float3 to_cam = camera_position(kg) - P;
  /* ★2026-08-16: fabsf をやめた。N は光子の入射側を向くよう flip 済みなので、
   * dot(N, to_cam) < 0 は「カメラが光子と反対側」= 片面 Lambert なら寄与ゼロ。
   * 絶対値を取っていたので、**受光面の裏をカメラが見ているときも全額
   * スプラットしていた**(薄い一枚板の地形・屋根・床の裏)。
   * 遮蔽レイ(falcon_lt_visibility)は既定オフな上、自分の prim を self 除外
   * するので一枚ポリの裏抜けは止められない。
   * [[issue_falcon_lt_flood_pabellon]] の候補機構。**未検証**。 */
  const float cos_surf = fmaxf(dot(N, to_cam) * inv_dist, 0.0f); /* |to_cam| == 1/inv_dist */
  if (!(cos_surf > 0.0f)) {
    return zero_float3();
  }

  /* We = (W*H) / (A * cos_lens^4): the PER-PIXEL camera importance. Each pixel
   * covers 1/(W*H) of the image-plane area A, so a photon splatted to one pixel
   * must be scaled up by the pixel count (this factor -- ~1.2e6 -- was the whole
   * brightness deficit). Folded with the connection geometry G = cos_lens/dist^2:
   *   contribution = flux * (diff/pi) * cos_surf * We * cos_lens / dist^2
   *                = flux * (diff/pi) * (W*H) * cos_surf / (A * cos_lens^3 * dist^2).
   * The `samples` factor (= SPP) CANCELS the combined pass's /sample_count divide:
   * the photon flux is normalized by the TOTAL photon count (N = W*H*SPP), so the
   * summed splats are the full light-traced image and the film's per-sample
   * averaging must be undone. Verified against the E*albedo/pi direct-floor
   * control -- (W*H * SPP) lands the floor at 0.158 vs the analytic 0.159 at
   * GAIN=1. Correct only for fixed (non-adaptive) sampling. */
  const float cl3 = cos_lens * cos_lens * cos_lens;
  const float npix = kernel_data.cam.width * kernel_data.cam.height;
  const float samples = (float)kernel_data.integrator.falcon_lt_samples;
  const float w = npix * samples * cos_surf / (A * cl3 * dist2);

  const float g = kernel_data.integrator.falcon_lighttrace_gain;
  return flux * diff * ((1.0f / M_PI_F) * w * g);
}

ccl_device_inline void falcon_lt_splat_px(
    KernelGlobals kg, ccl_global float *render_buffer, const int x, const int y, const float3 rgb)
{
  const uint64_t idx = (uint64_t)(x + y * (int)kernel_data.cam.width);
  ccl_global float *buf = render_buffer + idx * kernel_data.film.pass_stride +
                          kernel_data.film.pass_combined;
  atomic_add_and_fetch_float(buf + 0, rgb.x);
  atomic_add_and_fetch_float(buf + 1, rgb.y);
  atomic_add_and_fetch_float(buf + 2, rgb.z);
  /* Alpha/weight left untouched: the splat adds emission-like energy the camera
   * path tracer did not already account for (caustic-only paths). */
}

/* Atomic-splat an RGB contribution into the combined pass around pixel (px,py).
 * Single full-frame tile assumed (offset 0, stride = width) -- the light-trace
 * render is one tile (camera res <= 4096).
 *
 * FALCON_LT_SPLAT_RADIUS > 0 spreads the splat over a Gaussian footprint
 * (sigma = radius/2, footprint clamped to 9x9): the image-space reconstruction
 * blur that makes FEW photons/SPP look smooth -- a labeled NON-physical knob
 * (default 0 = physical single-pixel splat; LuxCore ships light tracing with
 * the pixel filter forced OFF for the same physical baseline). Weights are
 * normalized over the in-bounds pixels only, so total energy is preserved even
 * at the frame edge. Fireflies are already tamed upstream by the 4x flux clamp,
 * so the blur does not smear unclamped spikes. */
ccl_device_inline void falcon_lt_splat(KernelGlobals kg,
                                       ccl_global float *render_buffer,
                                       const int px,
                                       const int py,
                                       const float3 rgb)
{
  const float radius = kernel_data.integrator.falcon_lt_splat_radius;
  if (radius < 0.5f) {
    falcon_lt_splat_px(kg, render_buffer, px, py, rgb);
    return;
  }

  const int W = (int)kernel_data.cam.width;
  const int H = (int)kernel_data.cam.height;
  const int R = min((int)ceilf(radius), 4);
  /* sigma = radius/2 -> 1/(2 sigma^2) = 2/radius^2. */
  const float k = 2.0f / (radius * radius);

  float wsum = 0.0f;
  for (int dy = -R; dy <= R; dy++) {
    for (int dx = -R; dx <= R; dx++) {
      const int x = px + dx;
      const int y = py + dy;
      if (x < 0 || x >= W || y < 0 || y >= H) {
        continue;
      }
      wsum += expf(-(float)(dx * dx + dy * dy) * k);
    }
  }
  if (!(wsum > 0.0f)) {
    return;
  }
  const float inv_wsum = 1.0f / wsum;
  for (int dy = -R; dy <= R; dy++) {
    for (int dx = -R; dx <= R; dx++) {
      const int x = px + dx;
      const int y = py + dy;
      if (x < 0 || x >= W || y < 0 || y >= H) {
        continue;
      }
      const float w = expf(-(float)(dx * dx + dy * dy) * k) * inv_wsum;
      falcon_lt_splat_px(kg, render_buffer, x, y, rgb * w);
    }
  }
}

#endif /* __FALCON_SHARC__ */

CCL_NAMESPACE_END
