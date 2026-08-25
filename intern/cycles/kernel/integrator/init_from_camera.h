/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/camera/camera.h"

#include "kernel/film/adaptive_sampling.h"
#include "kernel/film/light_passes.h"

#include "kernel/integrator/path_state.h"

#include "kernel/integrator/state_util.h"
#include "kernel/sample/mapping.h"
#include "kernel/sample/pattern.h"

CCL_NAMESPACE_BEGIN

ccl_device_inline Spectrum integrate_camera_sample(KernelGlobals kg,
                                                   const int sample,
                                                   const int x,
                                                   const int y,
                                                   const uint rng_pixel,
                                                   ccl_private Ray *ray,
                                                   ccl_private int &r_cache_miss)
{
  /* Filter sampling. */
  const float2 rand_filter = (sample == 0) ? make_float2(0.5f, 0.5f) :
                                             path_rng_2D(kg, rng_pixel, sample, PRNG_FILTER);

  /* Motion blur (time) and depth of field (lens) sampling. (time, lens_x, lens_y) */
  const bool use_motionblur = kernel_data.cam.shuttertime != -1.0f;
  const bool use_dof = kernel_data.cam.aperturesize > 0.0f;
  const bool use_custom_cam = kernel_data.cam.type == CAMERA_CUSTOM;
  const float3 rand_time_lens = (use_motionblur || use_dof || use_custom_cam) ?
                                    path_rng_3D(kg, rng_pixel, sample, PRNG_LENS_TIME) :
                                    zero_float3();

  /* We use x for time and y,z for lens because in practice with Sobol
   * sampling this seems to give better convergence when an object is
   * both motion blurred and out of focus, without significantly harming
   * convergence for focal blur alone.  This is a little surprising,
   * because one would expect using x,y for lens (the 2d part) would be
   * best, since x,y are the best stratified.  Since it's not entirely
   * clear why this is, this is probably worth revisiting at some point
   * to investigate further. */
  const float rand_time = rand_time_lens.x;
  const float2 rand_lens = make_float2(rand_time_lens.y, rand_time_lens.z);

  /* Generate camera ray. */
  return camera_sample(kg, x, y, rand_filter, rand_time, rand_lens, ray, r_cache_miss);
}

/* Return false to indicate that this pixel is finished.
 * Used by CPU implementation to not attempt to sample pixel for multiple samples once its known
 * that the pixel did converge. */
ccl_device bool integrator_init_from_camera(KernelGlobals kg,
                                            IntegratorState state,
                                            const ccl_global KernelWorkTile *ccl_restrict tile,
                                            ccl_global float *render_buffer,
                                            const int x_,
                                            const int y_,
                                            const int scheduled_sample)
{
  PROFILING_INIT(kg, PROFILING_RAY_SETUP);

  int x, y, sample;

  if (tile == nullptr) {
    /* Restart from miss. Reconstruct x, y, sample from state. */
    const uint pixel_index = INTEGRATOR_STATE(state, path, render_pixel_index);
    x = pixel_index % (int)kernel_data.cam.width;
    y = pixel_index / (int)kernel_data.cam.width;
    sample = INTEGRATOR_STATE(state, path, sample);
  }
  else {
    x = x_;
    y = y_;

    /* Initialize path state to give basic buffer access and allow early outputs. */
    path_state_init(state, tile, x, y);

    /* Check whether the pixel has converged and should not be sampled anymore. */
    if (!film_need_sample_pixel(kg, state, render_buffer)) {
      return false;
    }

    /* Count the sample and get an effective sample for this pixel. */
    sample = film_write_sample(kg, state, render_buffer, scheduled_sample, tile->sample_offset);
  }

  /* Initialize random number seed for path. */
  const uint rng_pixel = path_rng_pixel_init(kg, sample, x, y);

#ifdef __FALCON_SHARC__
  /* Falcon Photon bake pass: this "render" traces light-emitted photons
   * instead of camera rays; shade_surface deposits them into the photon cache
   * at their first diffuse hit. The film output is discarded by the driver. */
  if (kernel_data.integrator.falcon_photon_pass) {
    /* White-noise emission RNG: the low-discrepancy pattern quantizes across
     * pixels at sample 0 (measured: 16.7M photons collapsed onto 37k cells),
     * photons need plain independent uniforms. */
    const float2 rp = make_float2(hash_uint3_to_float(x, y, 0x51ab3f27u ^ sample),
                                  hash_uint3_to_float(x, y, 0x9e3779b9u ^ sample));
    const float2 rd = make_float2(hash_uint3_to_float(y, x, 0x85ebca6bu ^ sample),
                                  hash_uint3_to_float(y, x, 0xc2b2ae35u ^ sample));
    Ray pray;
    /* 射出時の重み。スポットの縁の減衰をここに載せる(既定 1.0)。
     * throughput の初期値へ掛けるので、堆積時の flux はそのままでよい。 */
    float emit_weight = 1.0f;
    if (kernel_data.integrator.falcon_photon_is_sun) {
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      pray.P = make_float3(
          kernel_data.integrator.falcon_photon_sun_minx +
              rp.x * kernel_data.integrator.falcon_photon_sun_sizex,
          kernel_data.integrator.falcon_photon_sun_miny +
              rp.y * kernel_data.integrator.falcon_photon_sun_sizey,
          kernel_data.integrator.falcon_photon_sun_z);
      /* For distant lights co already holds the travel direction (light.cpp
       * stores -Z of the lamp transform).
       *
       * The sun is a DISK, not a point. Path tracing samples that disk
       * (sun_light_sample), and its angular size is the whole reason a real
       * caustic has soft edges: 0.53 degrees blurs the pattern by ~9 mm for
       * every metre between the water surface and the seabed, which is a whole
       * grid cell at the shot's resolution. Emitting a perfectly parallel beam
       * baked the caustic of a POINT sun -- the right total light in a pattern
       * that is too sharp. Measured against a brute-force judge (widened sun,
       * data/caustic/ocean_truth + tools/caustic_bands.py) the layer put +32%
       * of the reference into the top 1% of the frame and -7% into the middle
       * bands, and light tracing -- same emitter -- showed the same excess.
       *
       * Uniform cone sampling is exactly what the light sampling uses, so
       * every photon still carries the same flux and the total is untouched.
       * rd is otherwise unused in this branch. */
      float cos_theta, cone_pdf;
      pray.D = sample_uniform_cone(
          normalize(klight->co), klight->sun.one_minus_cosangle, rd, &cos_theta, &cone_pdf);
    }
    else if (kernel_data.integrator.falcon_photon_is_spot) {
      /* Spot: uniform solid-angle sampling of the cone around spot.dir.
       * cos(theta) = lerp(1, cos_half, u) is exactly uniform over the cap. */
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      /* spot.dir = the light's travel direction (light.cpp stores -Z of the
       * lamp transform), so photons emit straight along it. */
      const float3 n = klight->spot.dir;
      /* Emission window (launch tiling): restrict (u, azimuth) to a sub-square
       * of the cone's sampling domain. Uniform cone sampling makes that an
       * exact partition of the solid angle; the host scales flux/photon by the
       * window's area and rides the window in the otherwise unused sun_*
       * fields. Zero size = no window, i.e. the whole cone as before. */
      float u = rd.x, v = rd.y;
      const float win_sx = kernel_data.integrator.falcon_photon_sun_sizex;
      if (win_sx > 0.0f) {
        u = kernel_data.integrator.falcon_photon_sun_minx + u * win_sx;
        v = kernel_data.integrator.falcon_photon_sun_miny +
            v * kernel_data.integrator.falcon_photon_sun_sizey;
      }
      const float cos_theta = 1.0f - u * (1.0f - klight->spot.cos_half_spot_angle);
      const float sin_theta = sqrtf(max(1.0f - cos_theta * cos_theta, 0.0f));
      const float phi = M_2PI_F * v;
      float3 t, b;
      make_orthonormals(n, &t, &b);
      /* ★ランプの半径を持たせる(2026-08-16)。ここまで無限小の1点から出して
       * いたので、パストレーサ側がランプを面としてサンプルするのに対して
       * 光子だけが点光源だった —— 太陽で根治したのと同じ穴が SPOT にだけ
       * 残っていた(芯 1.326 -> 1.010 の件、memory issue_falcon_diagnosis_20260816)。
       * 位置は球の上の一様な点。向きは円錐内で決まるので独立に取る:
       * 受光面から見た「光源の広がり」がそのまま半影になる。 */
      const float cz = 1.0f - 2.0f * rp.x;
      const float sz = sqrtf(max(1.0f - cz * cz, 0.0f));
      const float sphi = M_2PI_F * rp.y;
      const float3 sn = make_float3(sz * cosf(sphi), sz * sinf(sphi), cz);
      pray.P = klight->co + sn * klight->spot.radius;
      pray.D = normalize(t * (sin_theta * cosf(phi)) + b * (sin_theta * sinf(phi)) +
                         n * cos_theta);
      /* ★縁の減衰。Cycles 本体の spot_light_attenuation(kernel/light/spot.h:30)
       * と同じ式。ローカル z = 円錐軸方向の成分 = ここの cos_theta。
       * 一様な円錐で撒いたまま重みで落とすので、総パワーは本体と一致する
       * (ホスト側の watts * Omega/(4pi) は減衰を含まない値なので、
       *  減った分だけ暗くなるのが正しい)。 */
      emit_weight *= smoothstepf((cos_theta - klight->spot.cos_half_spot_angle) *
                                 klight->spot.spot_smooth);
      if (!(emit_weight > 0.0f)) {
        return false;   /* 円錐の外周: この光子は捨てる */
      }
    }
    else if (kernel_data.integrator.falcon_photon_is_world) {
      /* Uniform environment: sample a direction uniformly over the sphere,
       * then launch a parallel beam through the target sphere's cross-section
       * disk from outside the scene (pbrt UniformInfiniteLight emission;
       * host flux = 4 pi^2 r^2 L / N so every photon carries equal flux).
       * Geometry rides in the repurposed sun_* fields (see data_template.h).
       * No lights[] access: this pass runs with every lamp hidden. */
      const float cos_theta = 1.0f - 2.0f * rd.x;
      const float sin_theta = sqrtf(max(1.0f - cos_theta * cos_theta, 0.0f));
      const float dphi = M_2PI_F * rd.y;
      const float3 d = make_float3(
          sin_theta * cosf(dphi), sin_theta * sinf(dphi), cos_theta);
      const float3 c = make_float3(kernel_data.integrator.falcon_photon_sun_minx,
                                   kernel_data.integrator.falcon_photon_sun_miny,
                                   kernel_data.integrator.falcon_photon_sun_z);
      const float disk_r = kernel_data.integrator.falcon_photon_sun_sizex;
      const float backoff = kernel_data.integrator.falcon_photon_sun_sizey;
      float3 t, b;
      make_orthonormals(d, &t, &b);
      const float pr = disk_r * sqrtf(rp.x);
      const float pphi = M_2PI_F * rp.y;
      pray.P = c - d * backoff + t * (pr * cosf(pphi)) + b * (pr * sinf(pphi));
      pray.D = d;
    }
    else if (kernel_data_fetch(lights, 0).type == LIGHT_POINT) {
      /* A POINT lamp is NOT an area lamp. KernelLight keeps spot/area/sun in a
       * UNION, and PointLight::copy_to_kernel fills only spot.{radius,eval_fac,
       * is_sphere} -- so the area branch below was reading spot's memory as
       * axis_u/len_u/dir: an unset direction, make_orthonormals on ~zero, and
       * photons launched nowhere. Measured on data/scenes/point_glass.blend
       * (tools/make_point_scene.py): baking a point-lit glass ball DELETED the
       * caustic instead of improving it -- energy 0.79 of plain path tracing
       * and 芯 0.035, because the bake also switches Cycles' own caustic paths
       * off and nothing replaced them.
       *
       * A Lambertian sphere: pick a point on it, emit cosine-weighted around
       * that point's normal. With radius 0 this stays correct -- a cosine
       * hemisphere around a uniformly random normal is uniform over the
       * sphere, which is what a bare point light emits. Flux per photon
       * (watts/N) is unchanged either way. */
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      const float cz = 1.0f - 2.0f * rp.x;
      const float sz = sqrtf(max(1.0f - cz * cz, 0.0f));
      const float pphi = M_2PI_F * rp.y;
      const float3 n = make_float3(sz * cosf(pphi), sz * sinf(pphi), cz);
      pray.P = klight->co + n * klight->spot.radius;
      float3 t, b;
      make_orthonormals(n, &t, &b);
      const float r = sqrtf(rd.x);
      const float phi = M_2PI_F * rd.y;
      pray.D = normalize(t * (r * cosf(phi)) + b * (r * sinf(phi)) +
                         n * sqrtf(max(1.0f - rd.x, 0.0f)));
    }
    else {
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      const float3 n = klight->area.dir;
      /* ★形を合わせる(2026-08-16)。ここまで **常に外接矩形の全面**から
       * 出していたので、ディスク/楕円のランプでも四隅から光子が飛び、
       * 模様が「正方形ランプの形」になっていた。総エネルギーは flux=watts/N
       * なので保存されるが、**分布だけが間違っていた**。
       * 楕円の印は invarea の負号(scene/light.cpp:241 "Negative inverse area
       * indicates ellipse")。内接楕円へは単位円の一様サンプルを uv に乗せる。 */
      float ou;
      float ov;
      if (klight->area.invarea < 0.0f) {
        const float rr = sqrtf(rp.x);
        const float pp = M_2PI_F * rp.y;
        ou = 0.5f * rr * cosf(pp);
        ov = 0.5f * rr * sinf(pp);
      }
      else {
        ou = rp.x - 0.5f;
        ov = rp.y - 0.5f;
      }
      pray.P = klight->co + klight->area.axis_u * (ou * klight->area.len_u) +
               klight->area.axis_v * (ov * klight->area.len_v) + n * 1e-4f;
      /* Cosine-hemisphere emission: matches a Lambertian emitter, so every
       * photon carries equal flux. */
      float3 t, b;
      make_orthonormals(n, &t, &b);
      const float r = sqrtf(rd.x);
      const float phi = M_2PI_F * rd.y;
      pray.D = normalize(t * (r * cosf(phi)) + b * (r * sinf(phi)) +
                         n * sqrtf(max(1.0f - rd.x, 0.0f)));
      /* ★Spread(ソフトボックスの格子)。本体 area_light_spread_attenuation
       * (kernel/light/area.h:106)と同じ式を、include 順に依存しないよう
       * ここで直に書く。tan_a は **出て行く向きと法線の角度**
       * (本体は「点→光源」の D を受けて -D で反転するので、こちらは
       *  pray.D をそのまま使う)。
       * 正規化 normalize_spread は「コサイン半球に掛けた時の積分が
       * spread によらない」ように作られている(light.cpp:260-268)ので、
       * 重みとして掛けるだけで総パワーはホストの watts と整合する。 */
      const float ths = klight->area.tan_half_spread;
      if (ths < FLT_MAX) {
        const float cs = dot(pray.D, n);
        const float sn = sqrtf(max(1.0f - cs * cs, 0.0f));
        const float tan_a = (cs > 1e-6f) ? (sn / cs) : FLT_MAX;
        const float atten = (ths == 0.0f) ?
                                ((tan_a > 1e-5f) ? 0.0f : M_PI_F) :
                                max((ths - tan_a) * klight->area.normalize_spread, 0.0f);
        emit_weight *= atten;
        if (!(emit_weight > 0.0f)) {
          return false; /* 格子に隠れる向き: この光子は捨てる */
        }
      }
    }
    pray.tmin = 0.0f;
    pray.tmax = FLT_MAX;
    pray.time = 0.5f;
    pray.self.object = OBJECT_NONE;
    pray.self.prim = PRIM_NONE;
    pray.self.light_object = OBJECT_NONE;
    pray.self.light_prim = PRIM_NONE;
    pray.dP = differential_zero_compact();
    pray.dD = differential_zero_compact();
    integrator_state_write_ray(state, &pray);
    /* Throughput starts at 1.0 (RELATIVE, like a camera path); the physical
     * per-photon flux is multiplied in at deposit time. Initializing with the
     * absolute flux (~1e-9) fed russian roulette a "black" path: survival
     * probability sqrt(throughput) ~= 1e-4 killed 99.99% of the photons after
     * the first specular bounce (measured: 64M photons -> 5178 deposits =
     * exactly sqrt(flux), the reason through-glass caustics starved while the
     * survivors' RR boost was eaten by the 4x flux clamp). */
    path_state_init_integrator(kg, state, sample, rng_pixel, one_spectrum() * emit_weight);
    integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
    return true;
  }
#endif

  /* Generate camera ray. */
  Ray ray;
  int cache_miss = 0;
  Spectrum T = integrate_camera_sample(kg, sample, x, y, rng_pixel, &ray, cache_miss);
  if (cache_miss) {
    if (tile != nullptr) {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
      INTEGRATOR_STATE_WRITE(state, path, sample) = sample;
    }
    integrator_path_cache_miss(state, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
    return true;
  }

  if (is_zero(T)) {
    if (tile == nullptr) {
      integrator_path_terminate(
          kg, state, render_buffer, DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA);
    }
    return true;
  }

  /* Write camera ray to state. */
  integrator_state_write_ray(state, &ray);

  if (tile == nullptr) {
    /* Re-initialize path state for path integration. */
    path_state_init_integrator(kg, state, sample, rng_pixel, T);
    integrator_path_next(state,
                         DEVICE_KERNEL_INTEGRATOR_INIT_FROM_CAMERA,
                         kernel_data.cam.is_inside_volume ?
                             DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK :
                             DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
  }
  else {
    /* Initialize path state for path integration. */
    path_state_init_integrator(kg, state, sample, rng_pixel, T);

    /* Continue with intersect_closest kernel, optionally initializing volume
     * stack before that if the camera may be inside a volume. */
    if (kernel_data.cam.is_inside_volume) {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_VOLUME_STACK);
    }
    else {
      integrator_path_init(state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
    }
  }

  return true;
}

CCL_NAMESPACE_END
