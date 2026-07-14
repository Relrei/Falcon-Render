/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/camera/camera.h"

#include "kernel/film/adaptive_sampling.h"
#include "kernel/film/light_passes.h"

#include "kernel/integrator/path_state.h"

#include "kernel/integrator/state_util.h"
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
    if (kernel_data.integrator.falcon_photon_is_sun) {
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      pray.P = make_float3(
          kernel_data.integrator.falcon_photon_sun_minx +
              rp.x * kernel_data.integrator.falcon_photon_sun_sizex,
          kernel_data.integrator.falcon_photon_sun_miny +
              rp.y * kernel_data.integrator.falcon_photon_sun_sizey,
          kernel_data.integrator.falcon_photon_sun_z);
      /* For distant lights co already holds the travel direction (light.cpp
       * stores -Z of the lamp transform). */
      pray.D = normalize(klight->co);
    }
    else if (kernel_data.integrator.falcon_photon_is_spot) {
      /* Spot: uniform solid-angle sampling of the cone around spot.dir.
       * cos(theta) = lerp(1, cos_half, u) is exactly uniform over the cap. */
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      /* spot.dir = the light's travel direction (light.cpp stores -Z of the
       * lamp transform), so photons emit straight along it. */
      const float3 n = klight->spot.dir;
      const float cos_theta = 1.0f - rd.x * (1.0f - klight->spot.cos_half_spot_angle);
      const float sin_theta = sqrtf(max(1.0f - cos_theta * cos_theta, 0.0f));
      const float phi = M_2PI_F * rd.y;
      float3 t, b;
      make_orthonormals(n, &t, &b);
      pray.P = klight->co;
      pray.D = normalize(t * (sin_theta * cosf(phi)) + b * (sin_theta * sinf(phi)) +
                         n * cos_theta);
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
    else {
      const ccl_global KernelLight *klight = &kernel_data_fetch(lights, 0);
      const float3 n = klight->area.dir;
      pray.P = klight->co +
               klight->area.axis_u * ((rp.x - 0.5f) * klight->area.len_u) +
               klight->area.axis_v * ((rp.y - 0.5f) * klight->area.len_v) + n * 1e-4f;
      /* Cosine-hemisphere emission: matches a Lambertian emitter, so every
       * photon carries equal flux. */
      float3 t, b;
      make_orthonormals(n, &t, &b);
      const float r = sqrtf(rd.x);
      const float phi = M_2PI_F * rd.y;
      pray.D = normalize(t * (r * cosf(phi)) + b * (r * sinf(phi)) +
                         n * sqrtf(max(1.0f - rd.x, 0.0f)));
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
    path_state_init_integrator(kg, state, sample, rng_pixel, one_spectrum());
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
