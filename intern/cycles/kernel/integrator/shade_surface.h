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

#include "kernel/integrator/mnee.h"

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
    triangle_vertices(kg, sd->prim, verts);
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
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

#ifdef __LIGHT_LINKING__
  if (!light_link_object_match(kg, light_link_receiver_forward(kg, state), sd->object) &&
      !(path_flag & PATH_RAY_CAMERA))
  {
    return;
  }
#endif

#ifdef __SHADOW_LINKING__
  /* Indirect emission of shadow-linked emissive surfaces is done via shadow rays to dedicated
   * light sources. */
  if (kernel_data.kernel_features & KERNEL_FEATURE_SHADOW_LINKING) {
    if (!(path_flag & PATH_RAY_CAMERA) &&
        kernel_data_fetch(objects, sd->object).shadow_set_membership != LIGHT_LINK_MASK_ALL)
    {
      return;
    }
  }
#endif

  /* Evaluate emissive closure. */
  const Spectrum L = surface_shader_emission(sd);

  const float mis_weight = light_sample_mis_weight_forward_surface(kg, state, path_flag, sd);

  guiding_record_surface_emission(kg, state, L, mis_weight);
  film_write_surface_emission(
      kg, state, L, mis_weight, render_buffer, object_lightgroup(kg, sd->object));
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

  /* Branch off shadow kernel. */
  IntegratorShadowState shadow_state = integrator_shadow_path_init(
      kg,
      state,
      (constant_light_shader) ? DEVICE_KERNEL_INTEGRATOR_INTERSECT_SHADOW :
                                DEVICE_KERNEL_INTEGRATOR_SHADE_LIGHT_NEE,
      false);

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
    INTEGRATOR_STATE(shadow_state, shadow_path, guiding_mis_weight) = 0.0f;
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
    void
    integrate_surface_direct_light(KernelGlobals kg,
                                   IntegratorState state,
                                   ccl_private ShaderData *sd,
                                   const ccl_private RNGState *rng_state)
{
  /* Test if there is a light or BSDF that needs direct light. */
  if (!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL))) {
    return;
  }

  /* Sample position on a light. */
  LightSample ls ccl_optional_struct_init;
  {
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
      return;
    }
  }

  kernel_assert(ls.pdf != 0.0f);

  const bool is_transmission = dot(ls.D, sd->N) < 0.0f;

  if (ls.prim != PRIM_NONE && ls.prim == sd->prim && ls.object == sd->object) {
    /* Skip self intersection if light direction lies in the same hemisphere as the geometric
     * normal. */
    if (dot(ls.D, is_transmission ? -sd->Ng : sd->Ng) > 0.0f) {
      return;
    }
  }

  Ray ray ccl_optional_struct_init;
  BsdfEval bsdf_eval ccl_optional_struct_init;

  int mnee_vertex_count = 0;  // NOLINT
#ifdef __MNEE__
  IF_KERNEL_FEATURE(MNEE)
  {
    if (ls.type != LIGHT_TRIANGLE) {
      /* Is this a caustic light? */
      const bool use_caustics = kernel_data_fetch(lights, ls.prim).use_caustics;
      if (use_caustics) {
        /* Are we on a caustic caster? */
        if (is_transmission && (sd->object_flag & SD_OBJECT_CAUSTICS_CASTER)) {
          return;
        }

        /* Are we on a caustic receiver? */
        if (!is_transmission && (sd->object_flag & SD_OBJECT_CAUSTICS_RECEIVER)) {
          ShaderDataCausticsStorage emission_sd_storage;
          ccl_private ShaderData *emission_sd = AS_SHADER_DATA(&emission_sd_storage);

          mnee_vertex_count = kernel_path_mnee_sample(
              kg, state, sd, emission_sd, rng_state, &ls, &bsdf_eval);

          if (mnee_vertex_count > 0) {
            /* Create shadow ray after successful manifold walk:
             * emission_sd contains the last interface intersection and
             * the light sample ls has been updated */
            light_sample_to_surface_shadow_ray(kg, emission_sd, &ls, &ray);
          }
        }
      }
    }
  }
#endif

  /* Evaluate constant part of light shader, rest will optionally be done in another kernel. */
  Spectrum light_shader_eval ccl_optional_struct_init;
  const bool is_constant_light_shader = light_sample_shader_eval_nee_constant(
      kg, ls.shader, ls.prim, ls.type != LIGHT_TRIANGLE, light_shader_eval);

#ifdef __MNEE__
  if (mnee_vertex_count > 0) {
    bsdf_eval_mul(&bsdf_eval, light_shader_eval);
  }
  else
#endif /* __MNEE__ */
  {
    /* Evaluate BSDF. */
    const float bsdf_pdf = surface_shader_bsdf_eval(kg, state, sd, ls.D, &bsdf_eval, ls.shader);
    const float mis_weight = light_sample_mis_weight_nee(kg, ls.pdf, bsdf_pdf);
    bsdf_eval_mul(&bsdf_eval, light_shader_eval * ls.eval_fac / ls.pdf * mis_weight);

    /* Path termination for constant light shader. */
    if (is_constant_light_shader && !(kernel_data.kernel_features & KERNEL_FEATURE_LIGHT_TREE)) {
      const float terminate = path_state_rng_light_termination(kg, rng_state);
      if (light_sample_terminate(kg, &bsdf_eval, terminate)) {
        return;
      }
    }
    /* For non-constant light shader, probabilistic termination happens in
     * SHADE_LIGHT_NEE when the full contribution is known. */
    else if (bsdf_eval_is_zero(&bsdf_eval)) {
      return;
    }

    /* Create shadow ray. */
    light_sample_to_surface_shadow_ray(kg, sd, &ls, &ray);
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

  INTEGRATOR_STATE_WRITE(shadow_state, shadow_path, flag) = shadow_flag;
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
                                                      rng_state);

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
                                               &bsdf_eta);

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
#endif
  }

  /* Update throughput. */
  const Spectrum bsdf_weight = bsdf_eval_sum(&bsdf_eval) / bsdf_pdf;
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
  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  if (!(kernel_data.kernel_features & KERNEL_FEATURE_AO_ADDITIVE) &&
      !(path_flag & PATH_RAY_CAMERA))
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

  const uint32_t path_flag = INTEGRATOR_STATE(state, path, flag);

  /* Skip most work for volume bounding surface. */
#ifdef __VOLUME__
  if (!(sd.flag & SD_HAS_ONLY_VOLUME)) {
#endif
    guiding_record_surface_segment(kg, state, &sd);

#ifdef __SUBSURFACE__
    /* Can skip shader evaluation for BSSRDF exit point without bump mapping. */
    if (!(path_flag & PATH_RAY_SUBSURFACE) || ((sd.flag & SD_HAS_BSSRDF_BUMP)))
#endif
    {
      /* Evaluate shader. */
      PROFILING_EVENT(PROFILING_SHADE_SURFACE_EVAL);
      surface_shader_eval<node_feature_mask>(kg, state, &sd, render_buffer, path_flag);

      /* Initialize additional RNG for BSDFs. */
      if (sd.flag & SD_BSDF_NEEDS_LCG) {
        sd.lcg_state = lcg_state_init(INTEGRATOR_STATE(state, path, rng_pixel),
                                      INTEGRATOR_STATE(state, path, rng_offset),
                                      INTEGRATOR_STATE(state, path, sample),
                                      0xb4bc3953);
      }
    }

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
    if ((path_flag & PATH_RAY_CAMERA) && kernel_data.integrator.falcon_sharc_active) {
      const float cell_size = falcon_sharc_cell_size(kernel_data.integrator.falcon_sharc_cell_size);
      ccl_global const float *cache = kernel_data_array(falcon_sharc_cache);
      float3 cached;
      if (falcon_sharc_lookup(cache, sd.P, cell_size, &cached)) {
        const float alpha = kernel_data.integrator.falcon_sharc_alpha;
        ccl_global float *pixel = film_pass_pixel_render_buffer(kg, state, render_buffer);
        const int sample = INTEGRATOR_STATE(state, path, sample);
        film_write_combined_pass(kg, path_flag, sample, rgb_to_spectrum(alpha * cached), pixel);
        INTEGRATOR_STATE_WRITE(state, path, throughput) *= (1.0f - alpha);
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
      const float3 n_off = (dot(sd.Ng, sd.wi) > 0.0f) ? sd.Ng : -sd.Ng;
      float3 cached;
      bool hit = false;
      if (kernel_data.integrator.falcon_photon_point_mode) {
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
        hit = falcon_photon_lookup(
            cache, Pq, cell_size, falcon_photon_normal_axis(n_off), &cached);
      }
      if (hit) {
        const Spectrum throughput = INTEGRATOR_STATE(state, path, throughput);
        ccl_global float *pixel = film_pass_pixel_render_buffer(kg, state, render_buffer);
        const int sample = INTEGRATOR_STATE(state, path, sample);
        film_write_combined_pass(
            kg, path_flag, sample, throughput * rgb_to_spectrum(cached), pixel);
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
        if (kernel_data.integrator.falcon_lighttrace) {
          /* Light tracing (FQ): connect the diffuse hit directly to the camera
           * and splat (see falcon_lighttrace.h). Caustic paths need >=1 specular
           * bounce; FALCON_LT_DIRECT=1 also splats the bounce-0 direct hit, whose
           * floor radiance E*albedo/pi is analytically known and PT-confirmed --
           * the absolute calibration control. No visibility ray yet (open-floor
           * test has nothing between the diffuse hit and the camera; occlusion is
           * P4). */
          if (d_avg > 0.0f && (bounce > 0 || kernel_data.integrator.falcon_lt_direct)) {
            const float fcap = 4.0f * kernel_data.integrator.falcon_photon_flux;
            const float3 flux = min(
                spectrum_to_rgb(INTEGRATOR_STATE(state, path, throughput)),
                make_float3(fcap, fcap, fcap));
            int px, py;
            if (falcon_lt_project(kg, sd.P, &px, &py)) {
              const float3 L = falcon_lt_connect(kg, sd.P, sd.N, flux, diff);
              falcon_lt_splat(kg, render_buffer, px, py, L);
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
              spectrum_to_rgb(INTEGRATOR_STATE(state, path, throughput)),
              make_float3(fcap, fcap, fcap));

          const float radius_cells = kernel_data.integrator.falcon_photon_radius;
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
      surface_shader_prepare_closures(kg, state, &sd, path_flag);

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
      integrate_surface_direct_light<node_feature_mask>(kg, state, &sd, &rng_state);
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

ccl_device_forceinline void integrator_shade_surface_mnee(
    KernelGlobals kg, IntegratorState state, ccl_global float *ccl_restrict render_buffer)
{
#ifdef __MNEE__
  integrator_shade_surface<(KERNEL_FEATURE_NODE_MASK_SURFACE & ~KERNEL_FEATURE_NODE_RAYTRACE) |
                               KERNEL_FEATURE_MNEE,
                           DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_MNEE>(kg, state, render_buffer);
#endif
}

CCL_NAMESPACE_END
