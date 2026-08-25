/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/integrator/path_state.h"
#include "kernel/integrator/surface_shader.h"

#include "kernel/film/data_passes.h"
#include "kernel/film/denoising_passes.h"
#include "kernel/film/light_passes.h"

#include "kernel/light/sample.h"

#include "kernel/geom/motion_triangle.h"
#include "kernel/geom/triangle.h"

#include "kernel/integrator/falcon_sharc.h"
#include "kernel/integrator/falcon_dispersion.h"
#include "kernel/integrator/falcon_lighttrace.h"
#include "kernel/integrator/guiding.h"
#include "kernel/integrator/shadow_linking.h"
#include "kernel/integrator/subsurface.h"
#include "kernel/integrator/volume_stack.h"

#include "kernel/types.h"
#include "util/math_intersect.h"

CCL_NAMESPACE_BEGIN

ccl_device_forceinline void integrate_surface_shader_setup(KernelGlobals kg,
                                                           ConstIntegratorState state,
                                                           ccl_private ShaderData *sd)
{
  Intersection isect ccl_optional_struct_init;
  integrator_state_read_isect(state, &isect);

  Ray ray ccl_optional_struct_init;
  integrator_state_read_ray(state, &ray);

  shader_setup_from_ray(kg, sd, &ray, &isect);
}

ccl_device_forceinline float3 integrate_surface_ray_offset(KernelGlobals kg,
                                                           const ccl_private ShaderData *sd,
                                                           const float3 ray_P,
                                                           const float3 ray_D)
{
  /* No ray offset needed for other primitive types. */
  if (!(sd->type & PRIMITIVE_TRIANGLE)) {
    return ray_P;
  }

  /* Self intersection tests already account for the case where a ray hits the
   * same primitive. However precision issues can still cause neighboring
   * triangles to be hit. Here we test if the ray-triangle intersection with
   * the same primitive would miss, implying that a neighboring triangle would
   * be hit instead.
   *
   * This relies on triangle intersection to be watertight, and the object inverse
   * object transform to match the one used by ray intersection exactly.
   *
   * Potential improvements:
   * - It appears this happens when either barycentric coordinates are small,
   *   or dot(sd->Ng, ray_D)  is small. Detect such cases and skip test?
   * - Instead of ray offset, can we tweak P to lie within the triangle?
   */

  /* TODO: Investigate if there are better ray offsetting algorithms for each BVH.
   * Cycles and Custom BVH triangle tests aren't numerically identical, meaning
   * this method isn't ideal for them. */

  float3 verts[3];
  if (sd->type == PRIMITIVE_TRIANGLE) {
    triangle_vertices(kg, sd->object, sd->prim, verts);
  }
  else {
    kernel_assert(sd->type == PRIMITIVE_MOTION_TRIANGLE);
    motion_triangle_vertices(kg, sd->object, sd->prim, sd->time, verts);
  }

  float3 local_ray_P = ray_P;
  float3 local_ray_D = ray_D;

  if (!(sd->object_flag & SD_OBJECT_TRANSFORM_APPLIED)) {
    const Transform itfm = object_get_inverse_transform(kg, sd);
    local_ray_P = transform_point(&itfm, local_ray_P);
    local_ray_D = transform_direction(&itfm, local_ray_D);
  }

  if (ray_triangle_intersect_self(local_ray_P, local_ray_D, verts)) {
    return ray_P;
  }
  return ray_offset(ray_P, sd->Ng);
}

ccl_device_forceinline bool integrate_surface_holdout(KernelGlobals kg,
                                                      ConstIntegratorState state,
                                                      ccl_private ShaderData *sd,
                                                      ccl_global float *ccl_restrict render_buffer)
{
  /* Write holdout transparency to render buffer and stop if fully holdout. */
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (((sd->flag & SD_HOLDOUT) || (sd->object_flag & SD_OBJECT_HOLDOUT_MASK)) &&
      (path_flag & PATH_RAY_TRANSPARENT_BACKGROUND))
  {
    const Spectrum holdout_weight = surface_shader_apply_holdout(sd);
    const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
    const float transparent = average(holdout_weight * throughput);
    film_write_holdout(kg, state, path_flag, transparent, render_buffer);
    if (isequal(holdout_weight, one_spectrum())) {
      return false;
    }
  }

  return true;
}

ccl_device_forceinline void integrate_surface_emission(KernelGlobals kg,
                                                       IntegratorState state,
                                                       const ccl_private ShaderData *sd,
                                                       ccl_global float *ccl_restrict
                                                           render_buffer)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

#ifdef __LIGHT_LINKING__
  if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
      !light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object))
  {
    return;
  }
#endif

#ifdef __SHADOW_LINKING__
  /* Indirect emission of shadow-linked emissive surfaces is done via shadow rays to dedicated
   * light sources. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING) {
    if (!(path_visibility & PATH_RAY_VISIBILITY_CAMERA) &&
        kernel_data_fetch(objects, sd->object).shadow_set_membership != LIGHT_LINK_MASK_ALL)
    {
      return;
    }
  }
#endif

  /* Evaluate emissive closure. */
  const Spectrum L = surface_shader_emission(sd);

  const float mis_weight = light_sample_mis_weight_forward_surface(
      kg, state, path_visibility, path_flag, sd);

  guiding_record_surface_emission(kg, state, L, mis_weight);
#ifdef __FALCON_SHARC__
  /* ★2026-08-25: 光子パスでは発光メッシュに当たってもフィルムに書かない。
   * 理由は shade_light.h の同じ囲いを参照(発射番号の画素へ書くと一様な下駄になる)。 */
  if (!kernel_data.integrator.falcon_photon_pass)
#endif
  {
    film_write_surface_emission(
        kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
  }
}

ccl_device int integrate_surface_ray_portal(KernelGlobals kg,
                                            IntegratorState state,
                                            ccl_private ShaderData *sd,
                                            const ccl_private ShaderClosure *sc)
{
  const ccl_private RayPortalClosure *pc = (const ccl_private RayPortalClosure *)sc;

  float sum_sample_weight = 0.0f;
  for (int i = 0; i < sd->num_closure; i++) {
    const ccl_private ShaderClosure *sc = &sd->closure[i];

    if (CLOSURE_IS_BSDF_OR_BSSRDF(sc->type)) {
      sum_sample_weight += sc->sample_weight;
    }
  }
  if (sum_sample_weight <= 0.0f) {
    return LABEL_NONE;
  }

  if (len_squared(sd->P - pc->P) > 1e-9f) {
    /* if the ray origin is changed, unset the current object,
     * so we can potentially hit the same polygon again */
    INTEGRATOR_STATE_WRITE(state, isect, object) = OBJECT_NONE;
    INTEGRATOR_STATE_WRITE(state, ray, P) = pc->P;
  }
  else {
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, pc->P, pc->D);
  }
  INTEGRATOR_STATE_WRITE(state, ray, D) = pc->D;
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
  INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
  INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);
#endif

  const float pick_pdf = pc->sample_weight / sum_sample_weight;
  INTEGRATOR_STATE_WRITE(state, path, throughput) *= pc->weight / pick_pdf;

  const int label = LABEL_TRANSMIT | LABEL_RAY_PORTAL;
  path_state_next(kg, state, label, sd->flag);

  return label;
}

/* Branch off a shadow path and initialize common part of it.
 * THe common is between the surface shading and configuration of a special shadow ray for the
 * shadow linking. */
ccl_device_inline IntegratorShadowState
integrate_direct_light_shadow_init_common(KernelGlobals kg,
                                          IntegratorState state,
                                          const ccl_private Ray *ccl_restrict ray,
                                          const Spectrum bsdf_spectrum,
                                          const int light_group,
                                          const int mnee_vertex_count,
                                          const bool constant_light_shader)
{
  const DeviceKernel next_kernel = (constant_light_shader) ?
                                       DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW :
                                       DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE;

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state;
#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    /* Reuse shadow path that was already allocated by intersect_mnee. */
    shadow_state = integrator_state_get_mnee_shadow_state(state);
    integrator_shadow_path_next(
        shadow_state, DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING, next_kernel);
  }
  else
#endif
  {
    shadow_state = integrator_shadow_path_init(kg, state, next_kernel, false);
  }

#ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, ray);
  integrator_state_write_shadow_ray_self(shadow_state, ray);

  /* Copy state from main path to shadow path. */
  const Spectrum unlit_throughput = INTEGRATOR_STATE(state, path, throughput);
  const Spectrum throughput = unlit_throughput * bsdf_spectrum;

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bsdf_eval_average) = average(bsdf_spectrum);
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = INTEGRATOR_STATE(
      state, path, transparent_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, glossy_bounce) = INTEGRATOR_STATE(
      state, path, glossy_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if ((kernel_data.kernel_features & KERNEL_FEATURE_NODE_PORTAL)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, portal_bounce) = INTEGRATOR_STATE(
        state, path, portal_bounce);
  }

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) =
        INTEGRATOR_STATE(state, path, transmission_bounce) + mnee_vertex_count - 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           diffuse_bounce) = INTEGRATOR_STATE(state, path, diffuse_bounce) + 1;
    INTEGRATOR_STATE_WRITE(shadow_state,
                           shadow_path,
                           bounce) = INTEGRATOR_STATE(state, path, bounce) + mnee_vertex_count;
  }
  else
#endif
  {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transmission_bounce) = INTEGRATOR_STATE(
        state, path, transmission_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, diffuse_bounce) = INTEGRATOR_STATE(
        state, path, diffuse_bounce);
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = INTEGRATOR_STATE(
        state, path, bounce);
  }

  /* Write Light-group, +1 as light-group is int but we need to encode into a uint8_t. */
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, lightgroup) = light_group + 1;

#if defined(__PATH_GUIDING__)
  if ((kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING)) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unlit_throughput) = unlit_throughput;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, path_segment) = INTEGRATOR_STATE(
        state, guiding, path_segment);
    INTEGRATOR_STATE(shadow_state, shadow_path, guiding_light_linking_mis_weight) = 0.0f;
  }
#endif

  return shadow_state;
}

/* Path tracing: sample point on light and evaluate light shader, then
 * queue shadow ray to be traced. */
template<uint node_feature_mask>
#if defined(__KERNEL_GPU__)
ccl_device_forceinline
#else
/* MSVC has very long compilation time (x20) if we force inline this function */
ccl_device
#endif
    ShaderEvalResult
    integrate_surface_direct_light(KernelGlobals kg,
                                   IntegratorState state,
                                   ccl_private ShaderData *sd,
                                   const ccl_private RNGState *rng_state)
{
  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
    return SHADER_EVAL_EMPTY;
  }

  LightSample ls ccl_optional_struct_init;
  int mnee_vertex_count = 0;  // NOLINT

#ifdef __MNEE__
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    /* MNEE already sampled a light and caustics casters. */
    integrator_state_read_mnee(state, &ls, &mnee_vertex_count);
  }
  else
#endif
  {
    /* Sample position on a light. */
    const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);
    const uint bounce = INTEGRATOR_STATE(state, path, bounce);
    const float3 rand_light = path_state_rng_3D(kg, rng_state, PRNG_LIGHT);

    if (!light_sample_from_position(kg,
                                    rand_light,
                                    sd->time,
                                    sd->P,
                                    sd->N,
                                    light_link_receiver_nee(kg, sd),
                                    sd->flag,
                                    bounce,
                                    path_flag,
                                    &ls))
    {
      return SHADER_EVAL_EMPTY;
    }
  }

  kernel_assert(ls.pdf != 0.0f);

  const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

  if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
    /* Skip self intersection if light direction lies in the same hemisphere as the geometric
     * normal. */
    if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
      return SHADER_EVAL_EMPTY;
    }
  }

#ifdef __MNEE__
  /* On a caustic caster, a caustic light's contribution is delivered to receivers by
   * MNEE and does not need to be computed again here. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_MNEE) {
    if (mnee_vertex_count == 0 && is_transmission &&
        (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER) && ls.type != LIGHT_TRIANGLE &&
        kernel_data_fetch(lights, ls.prim).use_caustics)
    {
      return SHADER_EVAL_EMPTY;
    }
  }
#endif

  /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
  Spectrum light_shader_eval ccl_optional_struct_init;
  const bool is_constant_light_shader = light_sample_shader_eval_nee_constant(
      kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

  /* Evaluate BSDF. */
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float avg_roughness_squared = 0.0f;
  const float bsdf_pdf = surface_shader_bsdf_eval(
      kg, state, sd, ls.D, &bsdf_eval, ls.shader, avg_roughness_squared);

  Ray ray ccl_optional_struct_init;

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    light_shader_eval *= integrator_state_read_mnee_throughput(state);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval);

    if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    integrator_state_read_mnee_ray(state, &ls, &ray);
  }
  else
#endif /* __MNEE__ */
  {
    const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);

    /* Path termination for constant light shader. */
    if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
      const float terminate = path_state_rng_light_termination(kg, rng_state);
      if (light_sample_terminate(kg, &bsdf_eval, terminate)) {
        return SHADER_EVAL_EMPTY;
      }
    }
    /* For non-constant light shader, probabilistic termination happens in
     * SHADE_LIGHT_NEE when the full contribution is known. */
    else if (bsdf_eval_is_zero(&bsdf_eval)) {
      return SHADER_EVAL_EMPTY;
    }

    /* Create shadow ray. */
    light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray);

#ifdef __RAY_DIFFERENTIALS__
    /* Widen ray differences, with same logic as forward sampling to ensure
     * both MIS strategies converge to the same result. */
    ray.dD = bsdf_widen_dD(INTEGRATOR_STATE(state, ray, dD), avg_roughness_squared);
#endif
  }

  if (ray.self.object != OBJECT_NONE) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrate_direct_light_shadow_init_common(
      kg,
      state,
      &ray,
      bsdf_eval_sum(&bsdf_eval),
      ls.group,
      mnee_vertex_count,
      is_constant_light_shader);

  if (is_transmission) {
#ifdef __VOLUME__
    volume_stack_enter_exit<true>(kg, shadow_state, sd);
#endif
  }

  uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag);

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    PackedSpectrum pass_diffuse_weight;
    PackedSpectrum pass_glossy_weight;

    if (shadow_flag & PATH_RAY_ANY_PASS) {
      /* Indirect bounce, use weights from earlier surface or volume bounce. */
      pass_diffuse_weight = INTEGRATOR_STATE(state, path, pass_diffuse_weight);
      pass_glossy_weight = INTEGRATOR_STATE(state, path, pass_glossy_weight);
    }
    else {
      /* Direct light, use BSDFs at this bounce. */
      shadow_flag |= PATH_RAY_SURFACE_PASS;
      pass_diffuse_weight = PackedSpectrum(bsdf_eval_pass_diffuse_weight(&bsdf_eval));
      pass_glossy_weight = PackedSpectrum(bsdf_eval_pass_glossy_weight(&bsdf_eval));
    }

    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_diffuse_weight) = pass_diffuse_weight;
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, pass_glossy_weight) = pass_glossy_weight;
  }

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;

  return SHADER_EVAL_OK;
}

/* Path tracing: bounce off or through surface with new direction. */
ccl_device_forceinline int integrate_surface_bsdf_bssrdf_bounce(
    KernelGlobals kg,
    IntegratorState state,
    ccl_private ShaderData *sd,
    const ccl_private RNGState *rng_state)
{
  /* Sample BSDF or BSSRDF. */
  if (!(sd->flag & (SD_BSDF | SD_BSSRDF))) {
    return LABEL_NONE;
  }

  float3 rand_bsdf = path_state_rng_3D(kg, rng_state, PRNG_SURFACE_BSDF);
  const ccl_private ShaderClosure *sc = surface_shader_bsdf_bssrdf_pick(sd, &rand_bsdf);

#ifdef __SUBSURFACE__
  /* BSSRDF closure, we schedule subsurface intersection kernel. */
  if (CLOSURE_IS_BSSRDF(sc->type)) {
    return subsurface_bounce(kg, state, sd, sc);
  }
#endif
  if (CLOSURE_IS_RAY_PORTAL(sc->type)) {
    return integrate_surface_ray_portal(kg, state, sd, sc);
  }

#ifdef __FALCON_SHARC__
  /* Falcon Dispersion (FALCON_DISPERSION.md): on-demand spectral split at a
   * smooth refractive/glass closure. On the path's first such scatter, sample a
   * wavelength, tint the throughput by its white-balanced weight (mean over the
   * band = white, so energy is preserved) and remember it; then refract with
   * the Cauchy-offset IOR for that wavelength. Later dispersive hits reuse the
   * stored wavelength. Reflections inherit the tint (chroma noise that
   * converges with spp; documented, accepted). Runs for camera and photon-bake
   * paths alike, so baked caustics get rainbows for free. */
  if (kernel_data.integrator.falcon_dispersion_b > 0.0f &&
      (CLOSURE_IS_REFRACTION(sc->type) || CLOSURE_IS_GLASS(sc->type)))
  {
    ccl_private MicrofacetBsdf *mbsdf = (ccl_private MicrofacetBsdf *)sc;
    if (mbsdf->alpha_x * mbsdf->alpha_y <= BSDF_ROUGHNESS_SQ_THRESH) {
      float lambda = INTEGRATOR_STATE(state, path, dispersion_lambda);
      if (lambda == 0.0f) {
        const float u = path_state_rng_1D(kg, rng_state, PRNG_SURFACE_DISPERSION);
        lambda = FALCON_DISPERSION_LAMBDA_MIN + u * FALCON_DISPERSION_LAMBDA_SPAN;
        INTEGRATOR_STATE_WRITE(state, path, dispersion_lambda) = lambda;
        INTEGRATOR_STATE_WRITE(state, path, throughput) *= rgb_to_spectrum(
            falcon_dispersion_cie_weight(lambda));
      }
      mbsdf->ior += falcon_dispersion_ior_offset(kernel_data.integrator.falcon_dispersion_b,
                                                 lambda);
    }
  }
#endif

  /* BSDF closure, sample direction. */
  float bsdf_pdf = 0.0f;
  float unguided_bsdf_pdf = 0.0f;
  BsdfEval bsdf_eval ccl_optional_struct_init;
  float3 bsdf_wo ccl_optional_struct_init;
  int label;

  float2 bsdf_sampled_roughness = make_float2(1.0f, 1.0f);
  float bsdf_eta = 1.0f;
  float mis_pdf = 1.0f;

  float bsdf_avg_roughness_squared = 0.0f;

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
  if (kernel_data.integrator.use_surface_guiding &&
      (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING))
  {
    label = surface_shader_bsdf_guided_sample_closure(kg,
                                                      state,
                                                      sd,
                                                      sc,
                                                      rand_bsdf,
                                                      &bsdf_eval,
                                                      &bsdf_wo,
                                                      &bsdf_pdf,
                                                      &mis_pdf,
                                                      &unguided_bsdf_pdf,
                                                      &bsdf_sampled_roughness,
                                                      &bsdf_eta,
                                                      rng_state,
                                                      bsdf_avg_roughness_squared);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }

    INTEGRATOR_STATE_WRITE(state, path, unguided_throughput) *= bsdf_pdf / unguided_bsdf_pdf;
  }
  else
#endif
  {
    label = surface_shader_bsdf_sample_closure(kg,
                                               sd,
                                               sc,
                                               rand_bsdf,
                                               &bsdf_eval,
                                               &bsdf_wo,
                                               &bsdf_pdf,
                                               &bsdf_sampled_roughness,
                                               &bsdf_eta,
                                               bsdf_avg_roughness_squared);

    if (bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval)) {
      return LABEL_NONE;
    }
    mis_pdf = bsdf_pdf;
    unguided_bsdf_pdf = bsdf_pdf;
  }

  if (label & LABEL_TRANSPARENT) {
    /* Only need to modify start distance for transparent. */
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);
  }
  else {
    /* Setup ray with changed origin and direction. */
    const float3 D = normalize(bsdf_wo);
    INTEGRATOR_STATE_WRITE(state, ray, P) = integrate_surface_ray_offset(kg, sd, sd->P, D);
    INTEGRATOR_STATE_WRITE(state, ray, D) = D;
    INTEGRATOR_STATE_WRITE(state, ray, tmin) = 0.0f;
    INTEGRATOR_STATE_WRITE(state, ray, tmax) = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
    INTEGRATOR_STATE_WRITE(state, ray, dP) = differential_make_compact(sd->dP);

    /* Widen ray differences, with same logic as NEE sampling to ensure
     * both MIS strategies converge to the same result. */
    const float dD = bsdf_widen_dD(INTEGRATOR_STATE(state, ray, dD), bsdf_avg_roughness_squared);
    INTEGRATOR_STATE_WRITE(state, ray, dD) = dD;
#endif
  }

  /* Update throughput. */
  Spectrum bsdf_weight = bsdf_eval_sum(&bsdf_eval) / bsdf_pdf;

  /* Falcon: light tracing and photon baking carry FLUX from the light; camera
   * paths carry radiance. Cycles' refraction BTDF includes the eta^2
   * radiance-compression factor (bsdf_microfacet.h, the sqr(bsdf->ior *
   * inv_len_H) term) -- correct for a camera path, wrong for a photon, because
   * flux is conserved across a refraction while radiance is not. Reusing the
   * camera throughput for photons therefore makes every refracted photon land
   * eta^2 too bright. This is the classic non-symmetric scattering correction
   * (PBRT 16.1.3).
   *
   * Measured on ocean_caustics 2026-08-02, LT layer vs a 1024spp reference:
   *   IOR 1.33 -> 1.762x too much energy (eta^2 = 1.769)
   *   IOR 1.50 -> 2.302x                 (eta^2 = 2.250)
   * i.e. the excess tracked eta^2 and nothing else. The existing calibration
   * control (the E*albedo/pi direct floor, 0.158 vs 0.159 analytic) could not
   * see this because that path never refracts. */
  if ((kernel_data.integrator.falcon_lighttrace || kernel_data.integrator.falcon_photon_pass) &&
      (label & LABEL_TRANSMIT) && bsdf_eta != 1.0f)
  {
    bsdf_weight /= sqr(bsdf_eta);
  }

  INTEGRATOR_STATE_WRITE(state, path, throughput) *= bsdf_weight;

  if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_PASSES) {
    if (INTEGRATOR_STATE(state, path, bounce) == 0) {
      INTEGRATOR_STATE_WRITE(state, path, pass_diffuse_weight) = bsdf_eval_pass_diffuse_weight(
          &bsdf_eval);
      INTEGRATOR_STATE_WRITE(state, path, pass_glossy_weight) = bsdf_eval_pass_glossy_weight(
          &bsdf_eval);
    }
  }

  /* Update path state */
  if (!(label & LABEL_TRANSPARENT)) {
    const float min_ray_pdf = INTEGRATOR_STATE(state, path, min_ray_pdf);
    INTEGRATOR_STATE_WRITE(state, path, mis_ray_pdf) = mis_pdf;
    INTEGRATOR_STATE_WRITE(state, path, mis_origin_n) = sd->N;
    INTEGRATOR_STATE_WRITE(state, path, min_ray_pdf) = fminf(unguided_bsdf_pdf, min_ray_pdf);

#ifdef __LIGHT_LINKING__
    if (kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_LINKING) {
      INTEGRATOR_STATE_WRITE(state, path, mis_ray_object) = sd->object;
    }
#endif
  }

  path_state_next(kg, state, label, sd->flag);

  guiding_record_surface_bounce(kg,
                                state,
                                bsdf_weight,
                                bsdf_pdf,
                                sd->N,
                                normalize(bsdf_wo),
                                bsdf_sampled_roughness,
                                bsdf_eta);

  return label;
}

#ifdef __VOLUME__
ccl_device_forceinline int integrate_surface_volume_only_bounce(IntegratorState state,
                                                                ccl_private ShaderData *sd)
{
  if (!path_state_volume_next(state)) {
    return LABEL_NONE;
  }

  /* Only modify start distance. */
  INTEGRATOR_STATE_WRITE(state, ray, tmin) = intersection_t_offset(sd->ray_length);

  return LABEL_TRANSMIT | LABEL_TRANSPARENT;
}
#endif

ccl_device_forceinline bool integrate_surface_terminate(IntegratorState state,
                                                        const uint32_t path_flag)
{
  const float continuation_probability = (path_flag & PATH_RAY_TERMINATE_ON_NEXT_SURFACE) ?
                                             0.0f :
                                             INTEGRATOR_STATE(
                                                 state, path, continuation_probability);
  if (continuation_probability == 0.0f) {
    return true;
  }
  if (continuation_probability != 1.0f) {
    INTEGRATOR_STATE_WRITE(state, path, throughput) /= continuation_probability;
  }

  return false;
}

#if defined(__AO__)
ccl_device_forceinline void integrate_surface_ao(KernelGlobals kg,
                                                 IntegratorState state,
                                                 const ccl_private ShaderData *ccl_restrict sd,
                                                 const ccl_private RNGState *ccl_restrict
                                                     rng_state)
{
  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) &&
      !(path_visibility & PATH_RAY_VISIBILITY_CAMERA))
  {
    return;
  }

  /* Skip AO for paths that were split off for shadow catchers to avoid double-counting. */
  if (path_flag & PATH_RAY_SHADOW_CATCHER_PASS) {
    return;
  }

  const float2 rand_bsdf = path_state_rng_2D(kg, rng_state, PRNG_SURFACE_BSDF);

  float3 ao_N;
  const Spectrum ao_weight = surface_shader_ao(
      sd, kernel_data.integrator.ao_additive_factor, &ao_N);

  float3 ao_D;
  float ao_pdf;
  sample_cos_hemisphere(ao_N, rand_bsdf, &ao_D, &ao_pdf);

  bool skip_self = true;

  Ray ray ccl_optional_struct_init;
  ray.P = shadow_ray_offset(kg, sd, ao_D, &skip_self);
  ray.D = ao_D;
  if (skip_self) {
    ray.P = integrate_surface_ray_offset(kg, sd, ray.P, ray.D);
  }
  ray.tmin = 0.0f;
  ray.tmax = kernel_data.integrator.ao_bounces_distance;
  ray.time = sd->time;
  ray.self.object = (skip_self) ? sd->object : OBJECT_NONE;
  ray.self.prim = (skip_self) ? sd->prim : PRIM_NONE;
  ray.self.light_object = OBJECT_NONE;
  ray.self.light_prim = PRIM_NONE;
  ray.dP = differential_zero_compact();
  ray.dD = differential_zero_compact();

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg, state, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW, true);

#  ifdef __VOLUME__
  /* Copy volume stack and enter/exit volume. */
  integrator_state_copy_volume_stack_to_shadow(kg, shadow_state, state);
#  endif

  /* Write shadow ray and associated state to global memory. */
  integrator_state_write_shadow_ray(shadow_state, &ray);
  integrator_state_write_shadow_ray_self(shadow_state, &ray);

  /* Copy state from main path to shadow path. */
  const uint16_t bounce = INTEGRATOR_STATE(state, path, bounce);
  const uint16_t transparent_bounce = INTEGRATOR_STATE(state, path, transparent_bounce);
  const uint32_t shadow_flag = INTEGRATOR_STATE(state, path, flag) | PATH_RAY_SHADOW_FOR_AO;
  const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput) * surface_shader_alpha(sd);

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, render_pixel_index) = INTEGRATOR_STATE(
      state, path, render_pixel_index);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_offset) = INTEGRATOR_STATE(
      state, path, rng_offset);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, rng_pixel) = INTEGRATOR_STATE(
      state, path, rng_pixel);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, sample) = INTEGRATOR_STATE(
      state, path, sample);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, visibility) = INTEGRATOR_STATE(
      state, path, visibility);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, bounce) = bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, transparent_bounce) = transparent_bounce;
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, volume_bounds_bounce) = INTEGRATOR_STATE(
      state, path, volume_bounds_bounce);
  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, throughput) = throughput;

  if (kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) {
    INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, unshadowed_throughput) = ao_weight;
  }
}
#endif /* defined(__AO__) */

template<uint node_feature_mask>
ccl_device int integrate_surface(KernelGlobals kg,
                                 IntegratorState state,
                                 ccl_global float *ccl_restrict render_buffer)

{
  PROFILING_INIT_FOR_SHADER(kg, PROFILING_SHADE_SURFACE_SETUP);

  /* Setup shader data. */
  ShaderData sd;
  integrate_surface_shader_setup(kg, state, &sd);
  PROFILING_SHADER(sd.object, sd.shader);

  int continue_path_label = 0;

  const PathRayVisibility path_visibility = INTEGRATOR_STATE(state, path, visibility);
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
  if (!(sd.flag & SD_HAS_ONLY_VOLUME)) {
#endif
#ifdef __SUBSURFACE__
    /* Can skip shader evaluation for BSSRDF exit point without bump mapping. */
    if (!(path_flag & PATH_RAY_SUBSURFACE) || ((sd.flag & SD_HAS_BSSRDF_BUMP)))
#endif
    {
      /* Evaluate shader. */
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_EVAL);
      surface_shader_eval<node_feature_mask>(
          kg, state, &sd, render_buffer, path_visibility, path_flag);
    }

    if (sd.flag & SD_CACHE_MISS) {
      return LABEL_CACHE_MISS;
    }

    /* After shader evaluation, in case of texture cache miss. */
    guiding_record_surface_segment(kg, state, &sd);

#ifdef __FALCON_SHARC__
    /* Falcon SHARC in-kernel render-buffer feedback blend (GPU offload).
     * At the camera first hit, look up the read-only cache (filled by a host
     * warmup run). If the cell is valid, write alpha * L_cache straight into the
     * pixel and scale the path throughput by (1 - alpha); after film accumulation
     * the pixel converges to lerp(L_path, L_cache, alpha) with no extra pass.
     * When the cache is empty (warmup), the lookup fails and rendering is
     * untouched, so the same build serves both warmup and blend. Unlike the
     * standalone 5.2 version there is no per-pixel cell buffer: the host warmup
     * deposits via the Position pass instead. */
    if ((path_visibility & PATH_RAY_VISIBILITY_CAMERA) && kernel_data.integrator.falcon_sharc_active) {
      const float cell_size = falcon_sharc_cell_size(kernel_data.integrator.falcon_sharc_cell_size);
      ccl_global const float *cache = kernel_data_array(falcon_sharc_cache);
      float3 cached;
      if (falcon_sharc_lookup(cache, sd.P, cell_size, &cached)) {
        float alpha = kernel_data.integrator.falcon_sharc_alpha;

        /* Path termination on measured evidence. The blend above already has
         * the shape of a termination -- take alpha of the cached radiance and
         * scale the path's remaining contribution by (1 - alpha) -- so the
         * error field only has to decide when alpha may go all the way to 1.
         * At alpha = 1 the pixel gets the cache's answer and the path is
         * finished here instead of tracing bounces whose result the cache
         * already knows to within the measured error.
         *
         * The threshold is compared against the *measured* relative error of
         * this cell (PT vs LT double estimator), not against a variance guess,
         * and a cell the probe never filled reads negative and is skipped: no
         * evidence must never be read as "converged". */
        const float thr = kernel_data.integrator.falcon_error_threshold;
        const bool raise_alpha = kernel_data.integrator.falcon_error_raise_alpha != 0;
        bool terminate = false;
        if (thr > 0.0f && (raise_alpha || alpha >= 0.999f)) {
          /* Only when the cache is *already* being taken in full. Measured
           * 2026-07-29: raising alpha to 1 on the strength of the error field
           * cost 29% of the image's energy, because the field measures how
           * settled the LIGHT TRACER is on a cell, not how well the radiance
           * cache stands in for the rest of the path -- and the auto GI gate
           * had set alpha to 0 for that scene precisely because its cache was
           * not trustworthy. So termination is now purely a speed shortcut of
           * a substitution the blend was going to make anyway: the image is
           * identical with and without it, and the error field only decides
           * where the shortcut is allowed to skip the remaining trace. */
          const float err = falcon_error_lookup(kernel_data_array(falcon_error_field),
                                                sd.P,
                                                kernel_data.integrator.falcon_error_cell_size);
          if (err >= 0.0f && err <= thr) {
            /* Measured on classroom 2026-07-30: cells that pass a
             * count>=16 gate at spread<=0.15 sit 1.9% (median) from a 512spp
             * reference, p90 5.5%, against 4.2%/17.0% for the cache at large.
             * That is the evidence for answering the path from the cache here
             * and stopping; elsewhere the scene-level blend stands. */
            if (raise_alpha) {
              alpha = 1.0f;
            }
            terminate = true;
          }
        }

        ccl_global float *pixel = film_pass_pixel_render_buffer(kg, state, render_buffer);
        const int sample = INTEGRATOR_STATE(state, path, sample);
        film_write_combined_pass(
            kg, path_visibility, path_flag, sample, rgb_to_spectrum(alpha * cached), pixel);
        INTEGRATOR_STATE_WRITE(state, path, throughput) *= (1.0f - alpha);
        if (terminate) {
          return LABEL_NONE;
        }
      }
    }

    /* Falcon Photon Cache: additive caustic radiance (FALCON_PHOTON_MODE=add).
     * The cache holds photon-estimated *caustic-only* outgoing radiance of
     * diffuse surfaces (host photon pass, caustic photons only), so it is
     * added like emission the path cannot find on its own: at any surface
     * vertex, pixel += throughput * L_cache. Throughput is left untouched --
     * this is extra transport, not a reweighting. Photons are only deposited
     * on diffuse surfaces, so lookups on specular hits mostly miss; residual
     * false positives at shared cells are an accepted v1 approximation
     * (FALCON_PHOTON.md). Avoid combining with MNEE on the same lights:
     * refractive-direct caustics would be counted twice. */
    if (kernel_data.integrator.falcon_photon_add) {
      /* Only ask the cache about surfaces it could have stored anything on. The
       * deposit below keeps photons on surfaces with a diffuse component; the
       * lookup used to run at every vertex regardless, so glass and mirrors --
       * where nothing was ever deposited -- still picked up whatever photons
       * shared their grid cell and added it as if the mirror itself glowed. The
       * cache holds outgoing radiance of *diffuse* surfaces, so there is no
       * reading of it that makes a specular vertex correct.
       *
       * LuxCore gates its lookups the same way (IsPhotonGIEnabled,
       * photongicache.cpp:66-74: a surface with transmit/specular events, or a
       * glossy one sharper than the glossiness threshold, is not a cache
       * location). Matching the deposit test exactly is the version of that
       * which cannot disagree with our own bake. */
      const float3 lookup_diff = spectrum_to_rgb(surface_shader_diffuse(kg, &sd));
      bool photon_receiver = (lookup_diff.x + lookup_diff.y + lookup_diff.z) > 0.0f ||
                             !kernel_data.integrator.falcon_photon_lookup_gate;
      /* How many diffuse vertices along ONE camera path may add the layer.
       *
       * The layer is added like emission at every diffuse vertex, which is the
       * right shape for "caustic light that then bounces": the second vertex
       * carries the caustic of the first one further into the room.
       *
       * ⚠ This knob does NOT fix the glass-on-table defect it was written for.
       * Measured on cupgap01 against a brute-force judge (2026-08-15): limits
       * of 1/2/4 all render within 1e-5 per pixel of no limit at all. The
       * multi-diffuse re-add is simply not where that light comes from -- the
       * bounce-cap sweep that suggested it (+0.21/+0.40/+0.57/+0.68 at caps
       * 2/4/8/32) was cutting the FOUR refractions a camera ray needs to get
       * through the cup wall, not the diffuse vertices. Kept because it is the
       * one knob that bounds how far cached caustic light may propagate, and
       * because measuring it again should not cost another rebuild.
       *
       * 0 = no limit (the behaviour up to 2026-08-15, and still the default). */
      const int lim = kernel_data.integrator.falcon_photon_lookup_bounces;
      if (lim > 0 && (int)INTEGRATOR_STATE(state, path, diffuse_bounce) >= lim) {
        photon_receiver = false;
      }
      const float3 n_off = (dot(sd.Ng, sd.wi) > 0.0f) ? sd.Ng : -sd.Ng;
      float3 cached;
      bool hit = false;
      if (!photon_receiver) {
        /* nothing to gather here */
      }
      else if (kernel_data.integrator.falcon_photon_point_mode) {
        /* Point map (Round 9): fixed-radius gather at the exact shading point,
         * radius/normal-angle/gain are render-time knobs. */
        if (falcon_photon_point_lookup(kernel_data_array(falcon_photon_points),
                                       kernel_data_array(falcon_photon_grid_start),
                                       kernel_data_array(falcon_photon_grid_count),
                                       kernel_data_array(falcon_photon_index),
                                       sd.P,
                                       n_off,
                                       kernel_data.integrator.falcon_photon_point_radius,
                                       kernel_data.integrator.falcon_photon_point_cos,
                                       &cached)) {
          cached *= kernel_data.integrator.falcon_photon_point_gain;
          hit = true;
        }
      }
      else {
        const float cell_size = falcon_sharc_cell_size(
            kernel_data.integrator.falcon_sharc_cell_size);
        ccl_global const float *cache = kernel_data_array(falcon_sharc_cache);
        /* Same half-cell normal offset and stencil as the deposit so the gather
         * reads the field the splat wrote (front side w.r.t. the viewing ray). */
        const float3 Pq = sd.P + n_off * (0.5f * cell_size);
        hit = falcon_photon_lookup(cache,
                                   Pq,
                                   cell_size,
                                   falcon_photon_normal_axis(n_off),
                                   kernel_data.integrator.falcon_photon_nearest,
                                   &cached);
      }
      if (hit) {
        const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
        ccl_global float *pixel = film_pass_pixel_render_buffer(kg, state, render_buffer);
        const int sample = INTEGRATOR_STATE(state, path, sample);
        film_write_combined_pass(
            kg, path_visibility, path_flag, sample, throughput * rgb_to_spectrum(cached), pixel);
      }
    }

    /* Falcon Photon bake pass: this path IS a photon (see init_from_camera).
     * Diffuse receiver -> deposit caustic radiance (only if the photon has
     * bounced off something specular first; direct light is the path tracer's
     * job) and terminate. Specular surface -> fall through, the regular BSDF
     * sampling continues the refract/reflect chain. Deposit mirrors the host
     * tracer: half-cell offset along the normal (flat receivers sit on cell
     * boundaries), L = flux * albedo / (pi * h^2). */
    if (kernel_data.integrator.falcon_photon_pass) {
      const float3 diff = spectrum_to_rgb(surface_shader_diffuse(kg, &sd));
      const float3 spec = spectrum_to_rgb(surface_shader_glossy(kg, &sd)) +
                          spectrum_to_rgb(surface_shader_transmission(kg, &sd));
      const float d_avg = (diff.x + diff.y + diff.z) / 3.0f;
      const float s_avg = (spec.x + spec.y + spec.z) / 3.0f;
      /* Receiver test: any diffuse component -> deposit and terminate. The old
       * `d_avg >= s_avg` compared FRESNEL-WEIGHTED closures, so grazing hits on
       * a dark floor always counted as specular and the whole far caustic field
       * (shallow exit rays, r > ~1.6 m on the ring scene = 25% of the flux) was
       * silently dropped -- the reason dark-floor hero bakes starved for
       * photons at ANY emission count. Pure speculars (d = 0, s > 0) still fall
       * through to BSDF sampling; dead surfaces (d = 0, s = 0) terminate. */
      if (d_avg > 0.0f || s_avg == 0.0f) {
        const int bounce = INTEGRATOR_STATE(state, path, bounce);
        /* World photons are emitted over the FULL sphere, so a single-sided
         * receiver (e.g. a floor plane) is hit from BELOW as well; those
         * wrong-side photons would deposit too and double-count the irradiance
         * (measured: open-floor calibration ~2x). A physical Lambertian only
         * collects light arriving on its front side, so drop back-side hits.
         * NOTE: shader_setup already flips sd.Ng to face the incoming ray on a
         * backface hit (dot(Ng, wi) >= 0 always afterwards), so testing the
         * dot product here is a no-op -- the actual back/front information lives
         * in the SD_BACKFACING flag, which is what we must check.
         * Lamp/sun/spot photons emit into a hemisphere and never hit backs, so
         * this is gated to the world pass to keep their paths untouched. */
        const bool world_frontface = !kernel_data.integrator.falcon_photon_is_world ||
                                      !(sd.flag & SD_BACKFACING);
        if (kernel_data.integrator.falcon_lighttrace) {
          /* Light tracing (FQ): connect the diffuse hit directly to the camera
           * and splat (see falcon_lighttrace.h). Caustic paths need >=1 specular
           * bounce; FALCON_LT_DIRECT=1 also splats the bounce-0 direct hit, whose
           * floor radiance E*albedo/pi is analytically known and PT-confirmed --
           * the absolute calibration control. */
          /* Note (2026-08-02): a PATH_RAY_DIFFUSE_ANCESTOR test was tried here,
           * on the theory that a caustic is L (S+) D E and photons that had
           * already scattered diffusely were being double counted against the
           * beauty pass. It was a complete no-op -- pabellon stayed at 10.9085
           * and ocean moved 0.9717 -> 0.9719 -- so every splat LT makes is
           * already a first diffuse hit. Removed rather than left in: it costs
           * a state read on a hot path and buys nothing. The 10.9x on pabellon
           * remains unexplained (memory/issue_falcon_lt_flood_pabellon.md). */
          if (d_avg > 0.0f && world_frontface &&
              (bounce > 0 || kernel_data.integrator.falcon_lt_direct)) {
            /* Throughput is RELATIVE (init 1.0, see init_from_camera); the
             * physical per-photon flux converts it here. The 4x cap now
             * clamps relative spikes (microfacet eval/pdf fireflies), not
             * russian-roulette boosts. */
            const float fcap = 4.0f * kernel_data.integrator.falcon_photon_flux;
            const float3 flux = min(
                spectrum_to_rgb(INTEGRATOR_STATE(state, path, throughput)) *
                    kernel_data.integrator.falcon_photon_flux,
                make_float3(fcap, fcap, fcap));
            int px, py;
            if (falcon_lt_project(kg, sd.P, &px, &py)) {
              bool occluded = false;
              /* FALCON_LT_VISIBILITY: occlusion ray vertex->camera. Kills
               * splats whose vertex the camera cannot actually see -- both
               * behind occluders (energy otherwise painted ON the occluder's
               * pixels) and seen THROUGH glass (an L-S-D-S-eye path light
               * tracing cannot place; the point map/MNEE own that case).
               * Compiled only into the raytrace kernel variant;
               * intersect_closest routes ALL shading there while the knob is
               * on (the plain cubin kernel must never trace on an OptiX
               * device -- it has no BVH2 to walk). */
              IF_KERNEL_FEATURE(NODE_RAYTRACE) {
                if (kernel_data.integrator.falcon_lt_visibility) {
                  Ray vis_ray;
                  vis_ray.self.object = sd.object;
                  vis_ray.self.prim = sd.prim;
                  vis_ray.self.light_object = OBJECT_NONE;
                  vis_ray.self.light_prim = PRIM_NONE;
                  vis_ray.P = sd.P;
                  vis_ray.D = normalize_len(camera_position(kg) - sd.P, &vis_ray.tmax);
                  vis_ray.tmin = 0.0f;
                  vis_ray.tmax *= 0.9999f;
                  vis_ray.dP = differential_make_compact(sd.dP);
                  vis_ray.dD = differential_zero_compact();
                  vis_ray.time = sd.time;
                  /* Caustics casters (glass) pass the connection ray through
                   * instead of blocking it: the camera does see the splat
                   * region through the glass body (refraction-shifted), and
                   * hard-killing it starves every caustic the camera views
                   * through or near the caster (75% of the LT energy on the
                   * LuxCore comparison scene). Same straight-line-through-
                   * specular approximation LuxCore's light tracing uses for
                   * its camera connections. Opaque hits still occlude. */
                  for (int vis_step = 0; vis_step < 16; vis_step++) {
                    Intersection vis_isect;
                    if (!scene_intersect(kg, &vis_ray, PATH_RAY_VISIBILITY_CAMERA, &vis_isect)) {
                      break; /* reached the camera */
                    }
                    const uint vis_obj_flag = kernel_data_fetch(object_flag, vis_isect.object);
                    if (!(vis_obj_flag & SD_OBJECT_CAUSTICS_CASTER)) {
                      occluded = true;
                      break;
                    }
                    vis_ray.self.object = vis_isect.object;
                    vis_ray.self.prim = vis_isect.prim;
                    vis_ray.tmin = intersection_t_offset(vis_isect.t);
                  }
                }
              }
              if (!occluded) {
                const float3 L = falcon_lt_connect(kg, sd.P, sd.N, flux, diff);
                falcon_lt_splat(kg, render_buffer, px, py, L);
              }
            }
          }
          return LABEL_NONE;
        }
        if (bounce > 0 && d_avg > 0.0f) {
          const float cell_size =
              falcon_sharc_cell_size(kernel_data.integrator.falcon_sharc_cell_size);
          ccl_global float *cache =
              (ccl_global float *)kernel_data_array(falcon_sharc_cache);
          const float3 n_off = (dot(sd.Ng, sd.wi) > 0.0f) ? sd.Ng : -sd.Ng;
          const float3 Pd = sd.P + n_off * (0.5f * cell_size);
          /* Photon energy clamp: along a physical specular chain the flux can
           * only shrink (Fresnel/albedo <= 1), so any growth is a microfacet
           * eval/pdf spike (the PT firefly mechanism) that would otherwise be
           * baked into the cache as a permanent bright dot (measured: p50
           * 2.4e-7 vs max 3.7e-2 = 150000x on the cushion scene). Clamp per
           * channel at 4x emission flux (headroom for dispersion's
           * single-channel packing). */
          const float fcap = 4.0f * kernel_data.integrator.falcon_photon_flux;
          const float3 flux = min(
              spectrum_to_rgb(INTEGRATOR_STATE(state, path, throughput)) *
                  kernel_data.integrator.falcon_photon_flux,
              make_float3(fcap, fcap, fcap));

          float radius_cells = kernel_data.integrator.falcon_photon_radius;
          /* Contact caustics get a wider estimate, and only they do. A photon
           * whose last segment is shorter than two cells came off a specular
           * surface that is sitting on this receiver, so its caustic is pinned
           * to the contact LINE; dividing that by a cell area is what makes the
           * stored radiance grow as 1/cell instead of settling. Widening only
           * here keeps the shipped Caustic Smoothness at 1.0, which is what
           * holds hash occupancy down on heavy scenes (raising it globally took
           * glasszoo 37% -> 70%, properties.py). */
          const float contact_rc = kernel_data.integrator.falcon_photon_contact_radius;
          if (contact_rc > radius_cells &&
              sd.ray_length <
                  kernel_data.integrator.falcon_photon_contact_cells * cell_size) {
            radius_cells = contact_rc;
          }
          if (radius_cells > 1.0f) {
            /* Photon-map density estimation: cone supplies 1/(pi r^2). */
            falcon_photon_deposit_wide(cache, Pd, cell_size, radius_cells, flux * diff,
                                       falcon_photon_normal_axis(n_off));
          }
          else {
            const float inv_area = 1.0f / (M_PI_F * cell_size * cell_size);
            falcon_photon_deposit(
                cache, Pd, cell_size, flux * diff * inv_area, falcon_photon_normal_axis(n_off));
          }
          if (kernel_data.integrator.falcon_photon_point_store) {
            /* Point map (Round 9): also store the raw photon (exact position,
             * flux * receiver albedo, deposit normal) for lookup-time density
             * estimation. */
            ccl_global float *pts = (ccl_global float *)kernel_data_array(falcon_photon_points);
            ccl_global uint *cnt = (ccl_global uint *)kernel_data_array(falcon_photon_pcount);
            falcon_photon_point_append(pts,
                                       cnt,
                                       kernel_data.integrator.falcon_photon_point_max,
                                       sd.P,
                                       flux * diff,
                                       n_off);
          }
        }
        return LABEL_NONE;
      }
    }
#endif

#ifdef __SUBSURFACE__
    if (path_flag & PATH_RAY_SUBSURFACE) {
      /* When coming from inside subsurface scattering, setup a diffuse
       * closure to perform lighting at the exit point. */
      subsurface_shader_data_setup(kg, &sd);
      INTEGRATOR_STATE_WRITE(state, path, flag) &= ~PATH_RAY_SUBSURFACE;
    }
    else
#endif
    {
      /* Filter closures. */
      surface_shader_prepare_closures(kg, state, &sd, path_visibility);

      /* Evaluate holdout. */
      if (!integrate_surface_holdout(kg, state, &sd, render_buffer)) {
        return LABEL_NONE;
      }

      /* Write emission. */
      if (sd.flag & SD_EMISSION) {
        integrate_surface_emission(kg, state, &sd, render_buffer);
      }

      /* Perform path termination. Most paths have already been terminated in
       * the intersect_closest kernel, this is just for emission and for dividing
       * throughput by the probability at the right moment.
       *
       * Also ensure we don't do it twice for SSS at both the entry and exit point. */
      if (integrate_surface_terminate(state, path_flag)) {
        return LABEL_NONE;
      }

      /* Write render passes. */
#ifdef __PASSES__
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_PASSES);
      film_write_data_passes(kg, state, &sd, render_buffer);
#endif

#ifdef __DENOISING_FEATURES__
      film_write_denoising_features_surface(kg, state, &sd, render_buffer);
      /* Written separately: this measures the ray that *left* the primary
       * surface, so it can only be known here, at the next hit. */
      film_write_denoising_specular_hit_distance(kg, state, sd.ray_length, render_buffer);
#endif
    }

    /* Load random number state. */
    RNGState rng_state;
    path_state_rng_load(state, &rng_state);

#if defined(__PATH_GUIDING__) && PATH_GUIDING_LEVEL >= 4
    if (kernel_data.kernel_features & KERNEL_FEATURE_PATH_GUIDING) {
      surface_shader_prepare_guiding(kg, state, &sd, &rng_state);
      guiding_write_debug_passes(kg, state, &sd, render_buffer);
    }
#endif
    /* Direct light. (Skipped for photon-bake paths: photons carry flux
     * forward only; next-event estimation would write garbage to the film.) */
#ifdef __FALCON_SHARC__
    if (!kernel_data.integrator.falcon_photon_pass)
#endif
    {
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_DIRECT_LIGHT);
      const ShaderEvalResult result = integrate_surface_direct_light<node_feature_mask>(
          kg, state, &sd, &rng_state);
      if (result == SHADER_EVAL_CACHE_MISS) {
        return LABEL_CACHE_MISS;
      }
    }

#if defined(__AO__)
    /* Ambient occlusion pass. */
    if (kernel_data.kernel_features & KERNEL_FEATURE_AO) {
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_AO);
      integrate_surface_ao(kg, state, &sd, &rng_state);
    }
#endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_bsdf_bssrdf_bounce(kg, state, &sd, &rng_state);
#ifdef __VOLUME__
  }
  else {
    if (integrate_surface_terminate(state, path_flag)) {
      return LABEL_NONE;
    }

#  ifdef __DENOISING_FEATURES__
    film_write_denoising_features_surface_volume(kg, state, &sd, render_buffer);
#  endif

    PROFILING_EVENT(PROFILING_SHADE_SURFACE_INDIRECT_LIGHT);
    continue_path_label = integrate_surface_volume_only_bounce(state, &sd);
  }

  if (continue_path_label & LABEL_TRANSMIT) {
    /* Enter/Exit volume. */
    volume_stack_enter_exit<false>(kg, state, &sd);
  }
#endif

  return continue_path_label;
}

template<DeviceKernel current_kernel>
ccl_device_forceinline void integrator_shade_surface_next_kernel(IntegratorState state)
{
  if (INTEGRATOR_STATE(state, path, flag) & PATH_RAY_SUBSURFACE) {
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_SUBSURFACE);
  }
  else {
    kernel_assert(INTEGRATOR_STATE(state, ray, tmax) != 0.0f);
    integrator_path_next(state, current_kernel, DEVICE_KERNEL_INTEGRATOR_INTERSECT_CLOSEST);
  }
}

template<uint node_feature_mask = KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE,
         DeviceKernel current_kernel = DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE>
ccl_device_forceinline void integrator_shade_surface(KernelGlobals kg,
                                                     IntegratorState state,
                                                     ccl_global float *ccl_restrict render_buffer)
{
  const int continue_path_label = integrate_surface<node_feature_mask>(kg, state, render_buffer);
  if (continue_path_label == LABEL_CACHE_MISS) {
    integrator_path_cache_miss_sorted(state, current_kernel);
    return;
  }

#ifdef __MNEE__
  /* Cleanup MNEE flag and shadow path if it was not reused for shadow trace. */
  if ((kernel_data.kernel_features & KERNEL_FEATURE_MNEE) &&
      (INTEGRATOR_STATE(state, path, mnee) & PATH_MNEE_SAMPLED))
  {
    INTEGRATOR_STATE_WRITE(state, path, mnee) &= ~PATH_MNEE_SAMPLED;

    const IntegratorShadowState shadow_state = integrator_state_get_mnee_shadow_state(state);
    if (INTEGRATOR_STATE(shadow_state, shadow_path, queued_kernel) ==
        DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING)
    {
      integrator_shadow_path_terminate(shadow_state,
                                       DEVICE_KERNEL_INTEGRATOR_SHADOW_PATH_MNEE_PENDING);
    }
  }
#endif

  if (continue_path_label == LABEL_NONE) {
    integrator_path_terminate(kg, state, render_buffer, current_kernel);
    return;
  }

#ifdef __SHADOW_LINKING__
  /* No need to cast shadow linking rays at a transparent bounce: the lights will be accumulated
   * via the main path in this case. BSSRDF bounces continue with intersect_subsurface. */
  if ((continue_path_label & (LABEL_TRANSPARENT | LABEL_SUBSURFACE_SCATTER)) == 0) {
    if (shadow_linking_schedule_intersection_kernel<current_kernel>(kg, state)) {
      return;
    }
  }
#endif

  integrator_shade_surface_next_kernel<current_kernel>(state);
}

ccl_device_forceinline void integrator_shade_surface_raytrace(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
  integrator_shade_surface<KERNEL_FEATURE_NODE_MASK_SURFACE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE>(
      kg, state, render_buffer);
}

CCL_NAMESPACE_END
