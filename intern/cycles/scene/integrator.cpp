/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include "device/device.h"

#ifdef WITH_FALCON_SHARC
#  include <cmath>
#  include <cstdio>
#  include <cstdlib>
#  include <cstring>
#  include <vector>
#endif

#include "scene/background.h"
#include "scene/bake.h"
#include "scene/camera.h"
#include "scene/film.h"
#include "scene/integrator.h"
#include "scene/light.h"
#include "scene/object.h"
#include "scene/scene.h"
#include "scene/shader.h"
#include "scene/stats.h"
#include "scene/tabulated_sobol.h"
#include "scene/volume.h"

#include "kernel/types.h"

#include "util/hash.h"
#include "util/log.h"
#include "util/task.h"
#include "util/time.h"

CCL_NAMESPACE_BEGIN

NODE_DEFINE(Integrator)
{
  NodeType *type = NodeType::add("integrator", create);

  SOCKET_INT(min_bounce, "Min Bounce", 0);
  SOCKET_INT(max_bounce, "Max Bounce", 7);

  SOCKET_INT(max_diffuse_bounce, "Max Diffuse Bounce", 7);
  SOCKET_INT(max_glossy_bounce, "Max Glossy Bounce", 7);
  SOCKET_INT(max_transmission_bounce, "Max Transmission Bounce", 7);
  SOCKET_INT(max_volume_bounce, "Max Volume Bounce", 7);

  SOCKET_INT(transparent_min_bounce, "Transparent Min Bounce", 0);
  SOCKET_INT(transparent_max_bounce, "Transparent Max Bounce", 7);

#ifdef WITH_CYCLES_DEBUG
  static NodeEnum direct_light_sampling_type_enum;
  direct_light_sampling_type_enum.insert("multiple_importance_sampling",
                                         DIRECT_LIGHT_SAMPLING_MIS);
  direct_light_sampling_type_enum.insert("forward_path_tracing", DIRECT_LIGHT_SAMPLING_FORWARD);
  direct_light_sampling_type_enum.insert("next_event_estimation", DIRECT_LIGHT_SAMPLING_NEE);
  SOCKET_ENUM(direct_light_sampling_type,
              "Direct Light Sampling Type",
              direct_light_sampling_type_enum,
              DIRECT_LIGHT_SAMPLING_MIS);
#endif

  SOCKET_INT(ao_bounces, "AO Bounces", 0);
  SOCKET_FLOAT(ao_factor, "AO Factor", 0.0f);
  SOCKET_FLOAT(ao_distance, "AO Distance", FLT_MAX);
  SOCKET_FLOAT(ao_additive_factor, "AO Additive Factor", 0.0f);

  SOCKET_BOOLEAN(volume_ray_marching, "Biased", false);
  SOCKET_INT(volume_max_steps, "Volume Max Steps", 1024);
  SOCKET_FLOAT(volume_step_rate, "Volume Step Rate", 1.0f);

  static NodeEnum guiding_distribution_enum;
  guiding_distribution_enum.insert("PARALLAX_AWARE_VMM", GUIDING_TYPE_PARALLAX_AWARE_VMM);
  guiding_distribution_enum.insert("DIRECTIONAL_QUAD_TREE", GUIDING_TYPE_DIRECTIONAL_QUAD_TREE);
  guiding_distribution_enum.insert("VMM", GUIDING_TYPE_VMM);

  static NodeEnum guiding_directional_sampling_type_enum;
  guiding_directional_sampling_type_enum.insert("MIS",
                                                GUIDING_DIRECTIONAL_SAMPLING_TYPE_PRODUCT_MIS);
  guiding_directional_sampling_type_enum.insert("RIS", GUIDING_DIRECTIONAL_SAMPLING_TYPE_RIS);
  guiding_directional_sampling_type_enum.insert("ROUGHNESS",
                                                GUIDING_DIRECTIONAL_SAMPLING_TYPE_ROUGHNESS);

  SOCKET_BOOLEAN(use_guiding, "Guiding", false);
  SOCKET_BOOLEAN(deterministic_guiding, "Deterministic Guiding", true);
  SOCKET_BOOLEAN(use_surface_guiding, "Surface Guiding", true);
  SOCKET_FLOAT(surface_guiding_probability, "Surface Guiding Probability", 0.5f);
  SOCKET_BOOLEAN(use_volume_guiding, "Volume Guiding", true);
  SOCKET_FLOAT(volume_guiding_probability, "Volume Guiding Probability", 0.5f);
  SOCKET_INT(guiding_training_samples, "Training Samples", 128);
  SOCKET_BOOLEAN(use_guiding_direct_light, "Guide Direct Light", true);
  SOCKET_BOOLEAN(use_guiding_mis_weights, "Use MIS Weights", true);
  SOCKET_ENUM(guiding_distribution_type,
              "Guiding Distribution Type",
              guiding_distribution_enum,
              GUIDING_TYPE_PARALLAX_AWARE_VMM);
  SOCKET_ENUM(guiding_directional_sampling_type,
              "Guiding Directional Sampling Type",
              guiding_directional_sampling_type_enum,
              GUIDING_DIRECTIONAL_SAMPLING_TYPE_RIS);
  SOCKET_FLOAT(guiding_roughness_threshold, "Guiding Roughness Threshold", 0.05f);

  SOCKET_BOOLEAN(caustics_reflective, "Reflective Caustics", true);
  SOCKET_BOOLEAN(caustics_refractive, "Refractive Caustics", true);
  SOCKET_FLOAT(filter_glossy, "Filter Glossy", 0.0f);

  SOCKET_BOOLEAN(use_direct_light, "Use Direct Light", true);
  SOCKET_BOOLEAN(use_indirect_light, "Use Indirect Light", true);
  SOCKET_BOOLEAN(use_diffuse, "Use Diffuse", true);
  SOCKET_BOOLEAN(use_glossy, "Use Glossy", true);
  SOCKET_BOOLEAN(use_transmission, "Use Transmission", true);
  SOCKET_BOOLEAN(use_emission, "Use Emission", true);

  SOCKET_INT(seed, "Seed", 0);
  SOCKET_FLOAT(sample_clamp_direct, "Sample Clamp Direct", 0.0f);
  SOCKET_FLOAT(sample_clamp_indirect, "Sample Clamp Indirect", 10.0f);
  SOCKET_BOOLEAN(motion_blur, "Motion Blur", false);

  SOCKET_INT(aa_samples, "AA Samples", 0);
  SOCKET_BOOLEAN(use_sample_subset, "Use Sample Subset", false);
  SOCKET_INT(sample_subset_offset, "Sample Subset Offset", 0);
  SOCKET_INT(sample_subset_length, "Sample Subset Length", MAX_SAMPLES);

  SOCKET_BOOLEAN(use_adaptive_sampling, "Use Adaptive Sampling", true);
  SOCKET_FLOAT(adaptive_threshold, "Adaptive Threshold", 0.01f);
  SOCKET_INT(adaptive_min_samples, "Adaptive Min Samples", 0);

  SOCKET_BOOLEAN(use_light_tree, "Use light tree to optimize many light sampling", true);
  SOCKET_FLOAT(light_sampling_threshold, "Light Sampling Threshold", 0.0f);

  static NodeEnum sampling_pattern_enum;
  sampling_pattern_enum.insert("sobol_burley", SAMPLING_PATTERN_SOBOL_BURLEY);
  sampling_pattern_enum.insert("tabulated_sobol", SAMPLING_PATTERN_TABULATED_SOBOL);
  sampling_pattern_enum.insert("blue_noise_pure", SAMPLING_PATTERN_BLUE_NOISE_PURE);
  sampling_pattern_enum.insert("blue_noise_round", SAMPLING_PATTERN_BLUE_NOISE_ROUND);
  sampling_pattern_enum.insert("blue_noise_first", SAMPLING_PATTERN_BLUE_NOISE_FIRST);
  SOCKET_ENUM(sampling_pattern,
              "Sampling Pattern",
              sampling_pattern_enum,
              SAMPLING_PATTERN_TABULATED_SOBOL);
  SOCKET_FLOAT(scrambling_distance, "Scrambling Distance", 1.0f);

  static NodeEnum denoiser_type_enum;
  denoiser_type_enum.insert("none", DENOISER_NONE);
  denoiser_type_enum.insert("optix", DENOISER_OPTIX);
  denoiser_type_enum.insert("openimagedenoise", DENOISER_OPENIMAGEDENOISE);

  static NodeEnum denoiser_prefilter_enum;
  denoiser_prefilter_enum.insert("none", DENOISER_PREFILTER_NONE);
  denoiser_prefilter_enum.insert("fast", DENOISER_PREFILTER_FAST);
  denoiser_prefilter_enum.insert("accurate", DENOISER_PREFILTER_ACCURATE);

  static NodeEnum denoiser_quality_enum;
  denoiser_quality_enum.insert("high", DENOISER_QUALITY_HIGH);
  denoiser_quality_enum.insert("balanced", DENOISER_QUALITY_BALANCED);
  denoiser_quality_enum.insert("fast", DENOISER_QUALITY_FAST);

  /* Default to accurate denoising with OpenImageDenoise. For interactive viewport
   * it's best use OptiX and disable the normal pass since it does not always have
   * the desired effect for that denoiser. */
  SOCKET_BOOLEAN(use_denoise, "Use Denoiser", false);
  SOCKET_ENUM(denoiser_type, "Denoiser Type", denoiser_type_enum, DENOISER_OPENIMAGEDENOISE);
  SOCKET_INT(denoise_start_sample, "Start Sample to Denoise", 0);
  SOCKET_BOOLEAN(use_denoise_pass_albedo, "Use Albedo Pass for Denoiser", true);
  SOCKET_BOOLEAN(use_denoise_pass_normal, "Use Normal Pass for Denoiser", true);
  SOCKET_ENUM(denoiser_prefilter,
              "Denoiser Prefilter",
              denoiser_prefilter_enum,
              DENOISER_PREFILTER_ACCURATE);
  SOCKET_BOOLEAN(denoise_use_gpu, "Denoise on GPU", true);
  SOCKET_ENUM(denoiser_quality, "Denoiser Quality", denoiser_quality_enum, DENOISER_QUALITY_HIGH);

  return type;
}

Integrator::Integrator() : Node(get_node_type()) {}

Integrator::~Integrator() = default;

void Integrator::device_update(Device *device, DeviceScene *dscene, Scene *scene)
{
  if (!is_modified()) {
    return;
  }

  const scoped_callback_timer timer([scene](double time) {
    if (scene->update_stats) {
      scene->update_stats->integrator.times.add_entry({"device_update", time});
    }
  });

  KernelIntegrator *kintegrator = &dscene->data.integrator;

  device_free(device, dscene);

  /* integrator parameters */

  /* Plus one so that a bounce of 0 indicates no global illumination, only direct illumination. */
  kintegrator->min_bounce = min_bounce + 1;
  kintegrator->max_bounce = max_bounce + 1;

  kintegrator->max_diffuse_bounce = max_diffuse_bounce + 1;
  kintegrator->max_glossy_bounce = max_glossy_bounce + 1;
  kintegrator->max_transmission_bounce = max_transmission_bounce + 1;
  kintegrator->max_volume_bounce = max_volume_bounce + 1;

  kintegrator->transparent_min_bounce = transparent_min_bounce + 1;

  /* Unlike other type of bounces, 0 transparent bounce means there is no transparent bounce in the
   * scene. */
  kintegrator->transparent_max_bounce = transparent_max_bounce;

  kintegrator->ao_bounces = (ao_factor != 0.0f) ? ao_bounces : 0;
  kintegrator->ao_bounces_distance = ao_distance;
  kintegrator->ao_bounces_factor = ao_factor;
  kintegrator->ao_additive_factor = ao_additive_factor;

#ifdef WITH_CYCLES_DEBUG
  kintegrator->direct_light_sampling_type = direct_light_sampling_type;
#else
  kintegrator->direct_light_sampling_type = DIRECT_LIGHT_SAMPLING_MIS;
#endif

  /* Transparent Shadows
   * We only need to enable transparent shadows, if we actually have
   * transparent shaders in the scene. Otherwise we can disable it
   * to improve performance a bit. */
  kintegrator->transparent_shadows = false;
  for (Shader *shader : scene->shaders) {
    if (shader->reference_count() == 0) {
      continue;
    }
    /* keep this in sync with SD_HAS_TRANSPARENT_SHADOW in shader.cpp */
    if ((shader->has_surface_transparent && shader->get_use_transparent_shadow()) ||
        shader->has_volume)
    {
      kintegrator->transparent_shadows = true;
      break;
    }
  }

  kintegrator->volume_ray_marching = volume_ray_marching;
  kintegrator->volume_max_steps = volume_max_steps;

  kintegrator->caustics_reflective = caustics_reflective;
  kintegrator->caustics_refractive = caustics_refractive;
  kintegrator->filter_glossy = (filter_glossy == 0.0f) ? FLT_MAX : 1.0f / filter_glossy;

  kintegrator->filter_closures = 0;
  if (!use_direct_light) {
    kintegrator->filter_closures |= FILTER_CLOSURE_DIRECT_LIGHT;
  }
  if (!use_indirect_light) {
    kintegrator->min_bounce = 1;
    kintegrator->max_bounce = 1;
  }
  if (!use_diffuse) {
    kintegrator->filter_closures |= FILTER_CLOSURE_DIFFUSE;
  }
  if (!use_glossy) {
    kintegrator->filter_closures |= FILTER_CLOSURE_GLOSSY;
  }
  if (!use_transmission) {
    kintegrator->filter_closures |= FILTER_CLOSURE_TRANSMISSION;
  }
  if (!use_emission) {
    kintegrator->filter_closures |= FILTER_CLOSURE_EMISSION;
  }
  if (scene->bake_manager->get_baking()) {
    /* Baking does not need to trace through transparency, we only want to bake
     * the object itself. */
    kintegrator->filter_closures |= FILTER_CLOSURE_TRANSPARENT;
  }

  const GuidingParams guiding_params = get_guiding_params(device);
  kintegrator->use_guiding = guiding_params.use;
  kintegrator->train_guiding = kintegrator->use_guiding;
  kintegrator->use_surface_guiding = guiding_params.use_surface_guiding;
  kintegrator->use_volume_guiding = guiding_params.use_volume_guiding;
  kintegrator->surface_guiding_probability = surface_guiding_probability;
  kintegrator->volume_guiding_probability = volume_guiding_probability;
  kintegrator->use_guiding_direct_light = use_guiding_direct_light;
  kintegrator->use_guiding_mis_weights = use_guiding_mis_weights;
  kintegrator->guiding_distribution_type = guiding_params.type;
  kintegrator->guiding_directional_sampling_type = guiding_params.sampling_type;
  kintegrator->guiding_roughness_threshold = guiding_params.roughness_threshold;

  kintegrator->sample_clamp_direct = (sample_clamp_direct == 0.0f) ? FLT_MAX :
                                                                     sample_clamp_direct * 3.0f;
  kintegrator->sample_clamp_indirect = (sample_clamp_indirect == 0.0f) ?
                                           FLT_MAX :
                                           sample_clamp_indirect * 3.0f;

  const int clamped_aa_samples = min(aa_samples, MAX_SAMPLES);

  kintegrator->sampling_pattern = sampling_pattern;
  kintegrator->scrambling_distance = scrambling_distance;
  kintegrator->sobol_index_mask = reverse_integer_bits(next_power_of_two(clamped_aa_samples - 1) -
                                                       1);
  kintegrator->blue_noise_sequence_length = clamped_aa_samples;
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_ROUND) {
    if (!is_power_of_two(clamped_aa_samples)) {
      kintegrator->blue_noise_sequence_length = next_power_of_two(clamped_aa_samples);
    }
    kintegrator->sampling_pattern = SAMPLING_PATTERN_BLUE_NOISE_PURE;
  }
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_FIRST) {
    kintegrator->blue_noise_sequence_length -= 1;
  }

  /* The blue-noise sampler needs a randomized seed to scramble properly, providing e.g. 0 won't
   * work properly. Therefore, hash the seed in those cases. */
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_FIRST ||
      kintegrator->sampling_pattern == SAMPLING_PATTERN_BLUE_NOISE_PURE)
  {
    kintegrator->seed = hash_uint(seed);
  }
  else {
    kintegrator->seed = seed;
  }

  /* NOTE: The kintegrator->use_light_tree is assigned to the efficient value in the light manager,
   * and the synchronization code is expected to tag the light manager for update when the
   * `use_light_tree` is changed. */
  if (light_sampling_threshold > 0.0f && !kintegrator->use_light_tree) {
    kintegrator->light_inv_rr_threshold = scene->film->get_exposure() / light_sampling_threshold;
  }
  else {
    kintegrator->light_inv_rr_threshold = 0.0f;
  }

  /* Build pre-tabulated Sobol samples if needed. */
  const int sequence_size = clamp(
      next_power_of_two(clamped_aa_samples - 1), MIN_TAB_SOBOL_SAMPLES, MAX_TAB_SOBOL_SAMPLES);
  const int table_size = sequence_size * NUM_TAB_SOBOL_PATTERNS * NUM_TAB_SOBOL_DIMENSIONS;
  if (kintegrator->sampling_pattern == SAMPLING_PATTERN_TABULATED_SOBOL &&
      dscene->sample_pattern_lut.size() != table_size)
  {
    kintegrator->tabulated_sobol_sequence_size = sequence_size;

    if (dscene->sample_pattern_lut.size() != 0) {
      dscene->sample_pattern_lut.free();
    }
    float4 *directions = (float4 *)dscene->sample_pattern_lut.alloc(table_size);
    TaskPool pool;
    for (int j = 0; j < NUM_TAB_SOBOL_PATTERNS; ++j) {
      float4 *sequence = directions + j * sequence_size;
      pool.push([sequence, sequence_size, j] {
        tabulated_sobol_generate_4D(sequence, sequence_size, j);
      });
    }
    pool.wait_work();

    dscene->sample_pattern_lut.copy_to_device();
  }

  kintegrator->has_shadow_catcher = scene->has_shadow_catcher();

  dscene->sample_pattern_lut.clear_modified();

#ifdef WITH_FALCON_SHARC
  /* Falcon SHARC is only active when FALCON_SHARC_MODE selects warmup or blend.
   * Everything below is gated on this so a normal render pays nothing: no 64 MB
   * cache allocation and (via falcon_sharc_active) no in-kernel cache lookup. */
  {
    const char *mode = getenv("FALCON_SHARC_MODE");
    const bool sharc_active = mode &&
                              (strcmp(mode, "warmup") == 0 || strcmp(mode, "blend") == 0 ||
                               strcmp(mode, "live") == 0);
    kintegrator->falcon_sharc_active = sharc_active ? 1 : 0;

    /* Grid resolution shared by SHARC and the photon cache. Runtime knob so
     * caustic-scale grids (0.05-0.1) need no rebuild; must match the cell
     * size the host tracer used when depositing. */
    float cell = 0.2f;
    const char *cell_env = getenv("FALCON_SHARC_CELL");
    if (cell_env) {
      cell = (float)atof(cell_env);
      cell = cell > 1e-4f ? cell : 0.2f;
    }
    kintegrator->falcon_sharc_cell_size = cell;

    /* Falcon Dispersion v0: global on-demand spectral knob. Cauchy B in um^2
     * (BK7 0.0042 / flint 0.013 / movie 0.030); 0 = off, zero overhead. */
    float dispersion_b = 0.0f;
    const char *dispersion_env = getenv("FALCON_DISPERSION_B");
    if (dispersion_env) {
      dispersion_b = (float)atof(dispersion_env);
      dispersion_b = dispersion_b > 0.0f ? dispersion_b : 0.0f;
    }
    kintegrator->falcon_dispersion_b = dispersion_b;

    /* Falcon Photon map deposit radius (cells). FALCON_PHOTON_RADIUS, default 3
     * = kernel density estimation footprint; <=1 falls back to the 2x2x2 splat. */
    float photon_radius = 3.0f;
    const char *pradius_env = getenv("FALCON_PHOTON_RADIUS");
    if (pradius_env) {
      const float pr = (float)atof(pradius_env);
      photon_radius = pr > 0.0f ? pr : 3.0f;
    }
    kintegrator->falcon_photon_radius = photon_radius;

    /* Falcon Photon Cache: FALCON_PHOTON_MODE=add + FALCON_SHARC_CACHE file
     * (written by tools/falcon_photon_trace.py). Loads into the same buffer as
     * SHARC -- the two modes share storage and are not meant to run together. */
    kintegrator->falcon_photon_add = 0;
    kintegrator->falcon_photon_pass = 0;
    kintegrator->falcon_photon_point_store = 0;
    kintegrator->falcon_photon_point_mode = 0;
    kintegrator->falcon_photon_point_max = 0;
    kintegrator->falcon_photon_point_gain = 1.0f;
    kintegrator->falcon_lighttrace = 0;
    kintegrator->falcon_lighttrace_gain = 1.0f;
    kintegrator->falcon_lt_direct = 0;
    kintegrator->falcon_lt_samples = 1;
    kintegrator->falcon_lt_splat_radius = 0.0f;
    kintegrator->falcon_lt_visibility = 0;
    const char *photon_mode = getenv("FALCON_PHOTON_MODE");

    /* Falcon Light Tracing (FQ): FALCON_PHOTON_MODE=bake + FALCON_LIGHTTRACE=1.
     * The bake pass, instead of depositing, projects the first diffuse caustic
     * hit to the camera and splats (see falcon_lighttrace.h). The bake operator
     * renders at CAMERA resolution and keeps the film. GAIN is a labeled knob
     * until the camera importance We is calibrated. */
    if (getenv("FALCON_LIGHTTRACE") && strcmp(getenv("FALCON_LIGHTTRACE"), "1") == 0) {
      kintegrator->falcon_lighttrace = 1;
      const char *ltg = getenv("FALCON_LIGHTTRACE_GAIN");
      kintegrator->falcon_lighttrace_gain = ltg ? (float)atof(ltg) : 1.0f;
      /* FALCON_LT_DIRECT=1 also splats the bounce-0 direct diffuse hit (not a
       * caustic) so its floor radiance can be matched to E*albedo/pi -- the
       * absolute-brightness calibration control. Off for real caustic renders. */
      kintegrator->falcon_lt_direct = getenv("FALCON_LT_DIRECT") ? 1 : 0;
      /* SPP of the driving render: the splat is multiplied by it to cancel the
       * combined pass's /sample_count divide (see falcon_lighttrace.h). Must
       * match cs.samples; fixed (non-adaptive) sampling only. */
      const char *lts = getenv("FALCON_LIGHTTRACE_SAMPLES");
      kintegrator->falcon_lt_samples = lts ? atoi(lts) : 1;
      /* FALCON_LT_SPLAT_RADIUS (px): Gaussian splat-reconstruction blur, the
       * labeled non-physical smoothness-for-photons trade (0 = physical). */
      const char *ltsr = getenv("FALCON_LT_SPLAT_RADIUS");
      if (ltsr) {
        const float r = (float)atof(ltsr);
        kintegrator->falcon_lt_splat_radius = r > 0.0f ? r : 0.0f;
      }
    }

    /* GPU photon bake: the render becomes a photon pass (init_from_camera
     * emits from the first light, shade_surface deposits into the cache,
     * path_trace saves it). FALCON_PHOTON_N = total photons (pixels*samples of
     * the driving render), FALCON_PHOTON_GAIN = calibration knob. */
    Light *photon_light = nullptr;
    Object *photon_light_ob = nullptr;
    if (photon_mode && strcmp(photon_mode, "bake") == 0) {
      for (Object *ob : scene->objects) {
        if (ob->get_geometry() && ob->get_geometry()->is_light()) {
          Light *l = static_cast<Light *>(ob->get_geometry());
          if (l->get_is_enabled()) {
            photon_light = l;
            photon_light_ob = ob;
            break;
          }
        }
      }
    }
    /* Multi-light bake (operators.py): each light gets its own fully independent
     * bake call (separate cache/point files); the host merges the resulting
     * files afterward. An earlier attempt kept ONE device buffer alive across
     * repeated bpy.ops.render.render() calls (skipping the reset below on all
     * but the first light) to accumulate on-device -- this reliably crashed
     * the CUDA queue on the second render call ("Illegal address ... in
     * integrator_sorted_paths_array"), so device_vector state apparently isn't
     * safe to keep across separate render invocations. File-level merging
     * avoids touching that lifecycle at all. Kept as `false` rather than
     * removed so the reset logic stays in one place if revisited. */
    const bool accumulate = false;
    if (photon_light) {
      const size_t floats = (size_t(1) << 22) * 4;
      if (dscene->falcon_sharc_cache.size() != floats) {
        if (dscene->falcon_sharc_cache.size() != 0) {
          dscene->falcon_sharc_cache.free();
        }
        dscene->falcon_sharc_cache.alloc(floats);
      }
      if (!accumulate) {
        memset(dscene->falcon_sharc_cache.data(), 0, floats * sizeof(float));
        dscene->falcon_sharc_cache.copy_to_device();
      }
      dscene->falcon_sharc_cache.clear_modified();

      Light *light = photon_light;
      const float3 strength = light->get_strength();
      const float watts = (strength.x + strength.y + strength.z) / 3.0f;
      double n_photons = 1e6;
      const char *n_env = getenv("FALCON_PHOTON_N");
      if (n_env) {
        n_photons = atof(n_env);
      }
      float gain = 1.0f;
      const char *gain_env = getenv("FALCON_PHOTON_GAIN");
      if (gain_env) {
        gain = (float)atof(gain_env);
      }
      kintegrator->falcon_photon_pass = 1;
      kintegrator->falcon_photon_is_sun = (light->get_light_type() == LIGHT_DISTANT) ? 1 : 0;
      kintegrator->falcon_photon_is_spot = (light->get_light_type() == LIGHT_SPOT) ? 1 : 0;
      kintegrator->falcon_photon_flux = (float)(watts * gain / n_photons);

      if (kintegrator->falcon_photon_is_spot) {
        /* Blender spot = point light (intensity W / 4 pi) clipped to the cone,
         * so the power actually leaving through the cone is W * Omega/(4 pi),
         * Omega = 2 pi (1 - cos(half angle)). Uniform cone sampling in the
         * kernel then gives every photon equal flux. */
        const float cos_half = cosf(light->get_spot_angle() * 0.5f);
        const float omega = M_2PI_F * (1.0f - cos_half);
        kintegrator->falcon_photon_flux = (float)(watts * (omega / (4.0f * M_PI_F)) * gain /
                                                  n_photons);
      }

      if (kintegrator->falcon_photon_is_sun) {
        /* Parallel photons over the scene footprint. */
        BoundBox bbox = BoundBox::empty;
        for (Object *ob : scene->objects) {
          if (ob->get_geometry() && ob->bounds.valid()) {
            bbox.grow(ob->bounds);
          }
        }
        if (bbox.valid()) {
          kintegrator->falcon_photon_sun_minx = bbox.min.x;
          kintegrator->falcon_photon_sun_miny = bbox.min.y;
          kintegrator->falcon_photon_sun_sizex = bbox.max.x - bbox.min.x;
          kintegrator->falcon_photon_sun_sizey = bbox.max.y - bbox.min.y;
          kintegrator->falcon_photon_sun_z = bbox.max.z + 1.0f;
          /* Sun energy is irradiance; power over the footprint = E*cos*A.
           * The emission direction is the light object's -Z axis. */
          const Transform tfm = photon_light_ob->get_tfm();
          const float3 dir = -transform_get_column(&tfm, 2);
          const float3 ndir = normalize(dir);
          const float cos_zen = fabsf(ndir.z);

          /* FALCON_PHOTON_TARGET="cx,cy,cz,r": shrink the launch square to the
           * specular targets' bounding sphere instead of the whole scene
           * footprint (a 40 m floor wastes 99.9% of the photons on direct
           * floor hits that never deposit -- the GPU twin of the CPU tracer's
           * Round 8 sun fix). The sphere center is projected UP the sun
           * direction onto the launch plane so diagonal travel still hits it,
           * and the radius is stretched by 1/cos_zen (slanted footprint of a
           * sphere) plus 15% margin. Host-only: the kernel still launches from
           * an axis-aligned horizontal square. */
          const char *tgt_env = getenv("FALCON_PHOTON_TARGET");
          float tc[4];
          if (tgt_env && ndir.z < -0.05f &&
              sscanf(tgt_env, "%f,%f,%f,%f", &tc[0], &tc[1], &tc[2], &tc[3]) == 4) {
            const float t_up = (kintegrator->falcon_photon_sun_z - tc[2]) / (-ndir.z);
            const float cx = tc[0] - ndir.x * t_up;
            const float cy = tc[1] - ndir.y * t_up;
            const float pad = tc[3] * 1.15f / cos_zen;
            kintegrator->falcon_photon_sun_minx = cx - pad;
            kintegrator->falcon_photon_sun_miny = cy - pad;
            kintegrator->falcon_photon_sun_sizex = 2.0f * pad;
            kintegrator->falcon_photon_sun_sizey = 2.0f * pad;
            LOG_INFO << "Falcon Photon: sun launch targeted at (" << tc[0] << ", " << tc[1]
                     << ", " << tc[2] << ") r " << tc[3] << ", pad " << pad;
          }

          const float area = kintegrator->falcon_photon_sun_sizex *
                             kintegrator->falcon_photon_sun_sizey;
          kintegrator->falcon_photon_flux = (float)(watts * cos_zen * area * gain / n_photons);
        }
      }
      LOG_INFO << "Falcon Photon: GPU bake pass, flux/photon "
               << kintegrator->falcon_photon_flux << " sun " << kintegrator->falcon_photon_is_sun;

      /* Point map (Round 9): allocate the raw-photon append buffer + counter.
       * FALCON_PHOTON_MAXPTS caps it (default 12M points = 432 MB); 0 disables
       * point storage (grid-only bake). Contents beyond the counter are never
       * read, so no zeroing of the point buffer itself. */
      int max_pts = 12000000;
      const char *maxpts_env = getenv("FALCON_PHOTON_MAXPTS");
      if (maxpts_env) {
        max_pts = atoi(maxpts_env);
      }
      if (max_pts > 0) {
        const size_t pfloats = (size_t)max_pts * 9;
        const bool points_realloc = dscene->falcon_photon_points.size() != pfloats;
        if (points_realloc) {
          if (dscene->falcon_photon_points.size() != 0) {
            dscene->falcon_photon_points.free();
          }
          dscene->falcon_photon_points.alloc(pfloats);
        }
        if (!accumulate || points_realloc) {
          /* Skipped when accumulating into an existing device buffer: the host
           * copy is stale (photon deposits only ever land device-side), so
           * uploading it here would overwrite the previous light's points with
           * whatever garbage/zeros the host array holds. */
          dscene->falcon_photon_points.copy_to_device();
        }
        dscene->falcon_photon_points.clear_modified();
        if (dscene->falcon_photon_pcount.size() != 1) {
          if (dscene->falcon_photon_pcount.size() != 0) {
            dscene->falcon_photon_pcount.free();
          }
          dscene->falcon_photon_pcount.alloc(1);
        }
        if (!accumulate) {
          dscene->falcon_photon_pcount.data()[0] = 0;
          dscene->falcon_photon_pcount.copy_to_device();
        }
        dscene->falcon_photon_pcount.clear_modified();
        kintegrator->falcon_photon_point_store = 1;
        kintegrator->falcon_photon_point_max = max_pts;
        LOG_INFO << "Falcon Photon: point buffer ready, cap " << max_pts << " points"
                 << (accumulate ? " (accumulating)" : "");
      }
    }
    if (photon_mode && strcmp(photon_mode, "add") == 0 && !sharc_active) {
      /* Point map (Round 9) takes priority: FALCON_PHOTON_POINTS file =
       * header {magic 'FPH1', uint32 count} + count * 9 floats (pos3, flux3,
       * normal3). The neighbor grid is rebuilt here every scene update, so the
       * lookup radius is a pure render-time knob. */
      const char *pts_env = getenv("FALCON_PHOTON_POINTS");
      FILE *pf = pts_env ? fopen(pts_env, "rb") : nullptr;
      if (pf) {
        uint32_t header[2] = {0, 0};
        size_t count = 0;
        if (fread(header, sizeof(uint32_t), 2, pf) == 2 && header[0] == 0x46504831u) {
          count = header[1];
        }
        if (count > 0) {
          const size_t pfloats = count * 9;
          if (dscene->falcon_photon_points.size() != pfloats) {
            if (dscene->falcon_photon_points.size() != 0) {
              dscene->falcon_photon_points.free();
            }
            dscene->falcon_photon_points.alloc(pfloats);
          }
          float *pts = dscene->falcon_photon_points.data();
          if (fread(pts, sizeof(float), pfloats, pf) == pfloats) {
            float radius = 0.03f;
            const char *radius_env = getenv("FALCON_PHOTON_RADIUS_M");
            if (radius_env) {
              radius = (float)atof(radius_env);
            }
            radius = radius > 1e-4f ? radius : 1e-4f;
            float normal_deg = 30.0f;
            const char *ndeg_env = getenv("FALCON_PHOTON_NORMAL_DEG");
            if (ndeg_env) {
              normal_deg = (float)atof(ndeg_env);
            }
            float gain = 1.0f;
            const char *gain_env2 = getenv("FALCON_PHOTON_GAIN");
            if (gain_env2) {
              gain = (float)atof(gain_env2);
            }

            /* Counting sort of photon indices into the uniform neighbor grid
             * (cell = radius; slot = hash_uint3 of cell coords). */
            const uint32_t GRID = 1u << 22;
            const uint32_t MASK = GRID - 1u;
            std::vector<uint32_t> keys(count);
            const float inv = 1.0f / radius;
            for (size_t i = 0; i < count; i++) {
              const float *p = pts + i * 9;
              const uint gx = (uint)((int)floorf(p[0] * inv));
              const uint gy = (uint)((int)floorf(p[1] * inv));
              const uint gz = (uint)((int)floorf(p[2] * inv));
              keys[i] = hash_uint3(gx, gy, gz) & MASK;
            }
            if (dscene->falcon_photon_grid_start.size() != GRID) {
              if (dscene->falcon_photon_grid_start.size() != 0) {
                dscene->falcon_photon_grid_start.free();
                dscene->falcon_photon_grid_count.free();
              }
              dscene->falcon_photon_grid_start.alloc(GRID);
              dscene->falcon_photon_grid_count.alloc(GRID);
            }
            uint *gstart = dscene->falcon_photon_grid_start.data();
            uint *gcount = dscene->falcon_photon_grid_count.data();
            memset(gcount, 0, GRID * sizeof(uint));
            for (size_t i = 0; i < count; i++) {
              gcount[keys[i]]++;
            }
            uint32_t run = 0;
            for (uint32_t k = 0; k < GRID; k++) {
              gstart[k] = run;
              run += gcount[k];
            }
            if (dscene->falcon_photon_index.size() != count) {
              if (dscene->falcon_photon_index.size() != 0) {
                dscene->falcon_photon_index.free();
              }
              dscene->falcon_photon_index.alloc(count);
            }
            uint *gindex = dscene->falcon_photon_index.data();
            std::vector<uint32_t> cursor(gstart, gstart + GRID);
            for (size_t i = 0; i < count; i++) {
              gindex[cursor[keys[i]]++] = (uint32_t)i;
            }

            dscene->falcon_photon_points.copy_to_device();
            dscene->falcon_photon_points.clear_modified();
            dscene->falcon_photon_grid_start.copy_to_device();
            dscene->falcon_photon_grid_start.clear_modified();
            dscene->falcon_photon_grid_count.copy_to_device();
            dscene->falcon_photon_grid_count.clear_modified();
            dscene->falcon_photon_index.copy_to_device();
            dscene->falcon_photon_index.clear_modified();

            kintegrator->falcon_photon_add = 1;
            kintegrator->falcon_photon_point_mode = 1;
            kintegrator->falcon_photon_point_radius = radius;
            kintegrator->falcon_photon_point_cos = cosf(normal_deg * (float)M_PI / 180.0f);
            kintegrator->falcon_photon_point_gain = gain;
            LOG_INFO << "Falcon Photon: point map loaded, " << count << " points from "
                     << pts_env << " (radius " << radius << " m, normal " << normal_deg
                     << " deg, gain " << gain << ")";
          }
        }
        fclose(pf);
      }

      if (!kintegrator->falcon_photon_point_mode) {
        const char *cache_env = getenv("FALCON_SHARC_CACHE");
        const char *path = cache_env ? cache_env : "/tmp/falcon_sharc_cache.bin";
        FILE *f = fopen(path, "rb");
        if (f) {
          const size_t floats = (size_t(1) << 22) * 4;
          if (dscene->falcon_sharc_cache.size() != floats) {
            if (dscene->falcon_sharc_cache.size() != 0) {
              dscene->falcon_sharc_cache.free();
            }
            dscene->falcon_sharc_cache.alloc(floats);
          }
          if (fread(dscene->falcon_sharc_cache.data(), sizeof(float), floats, f) == floats) {
            dscene->falcon_sharc_cache.copy_to_device();
            dscene->falcon_sharc_cache.clear_modified();
            kintegrator->falcon_photon_add = 1;
            LOG_INFO << "Falcon Photon: additive caustic cache loaded from " << path
                     << " (cell " << cell << ")";
          }
          fclose(f);
        }
      }
    }

    if (sharc_active) {
      /* Allocate and zero the spatial hash radiance cache. Size must match
       * FALCON_SHARC_CELL_COUNT * FALCON_SHARC_CELL_STRIDE in
       * kernel/integrator/falcon_sharc.h (4M cells * 4 floats = 64 MB). Read-only
       * from the kernel; the host fills it during the warmup pass. */
      const size_t falcon_sharc_floats = (size_t(1) << 22) * 4;
      if (dscene->falcon_sharc_cache.size() != falcon_sharc_floats) {
        if (dscene->falcon_sharc_cache.size() != 0) {
          dscene->falcon_sharc_cache.free();
        }
        float *cache = (float *)dscene->falcon_sharc_cache.alloc(falcon_sharc_floats);
        memset(cache, 0, falcon_sharc_floats * sizeof(float));
        dscene->falcon_sharc_cache.copy_to_device();
      }
      dscene->falcon_sharc_cache.clear_modified();

      /* Blend factor: runtime knob via FALCON_SHARC_ALPHA (default 0.7, clamped
       * to [0,1]). Uploaded in kernel_data so tuning needs no recompile. */
      float alpha = 0.7f;
      const char *alpha_env = getenv("FALCON_SHARC_ALPHA");
      if (alpha_env) {
        alpha = (float)atof(alpha_env);
        alpha = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
      }

      /* Auto GI gate: SHARC helps GI-dominated scenes but hurts direct-lit ones
       * (Phase0 data), so scale alpha by the scene's GI dominance measured during
       * warmup (indirect / (direct + indirect), stored in <cache>.meta). The gate
       * smoothsteps from off below GATE_LOW to full above GATE_HIGH, so a direct-
       * lit scene self-disables SHARC. Only for blend/live (warmup's blend is a
       * no-op). Missing meta or FALCON_SHARC_GATE=0 -> no gating (alpha as-is). */
      const bool is_blend = strcmp(mode, "blend") == 0 || strcmp(mode, "live") == 0;
      const char *gate_env = getenv("FALCON_SHARC_GATE");
      const bool gate_on = !(gate_env && strcmp(gate_env, "0") == 0);
      if (is_blend && gate_on) {
        const char *cache_env = getenv("FALCON_SHARC_CACHE");
        const string meta_path = string(cache_env ? cache_env : "/tmp/falcon_sharc_cache.bin") +
                                 ".meta";
        FILE *mf = fopen(meta_path.c_str(), "r");
        if (mf) {
          float gi_ratio = 1.0f;
          if (fscanf(mf, "%f", &gi_ratio) == 1) {
            float low = 0.15f, high = 0.40f;
            const char *lo = getenv("FALCON_SHARC_GATE_LOW");
            const char *hi = getenv("FALCON_SHARC_GATE_HIGH");
            if (lo) {
              low = (float)atof(lo);
            }
            if (hi) {
              high = (float)atof(hi);
            }
            float t = (high > low) ? (gi_ratio - low) / (high - low) : (gi_ratio >= high);
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            const float gate = t * t * (3.0f - 2.0f * t);
            LOG_INFO << "Falcon SHARC: GI gate " << gate << " (ratio " << gi_ratio << ", alpha "
                     << alpha << " -> " << (alpha * gate) << ")";
            alpha *= gate;
          }
          fclose(mf);
        }
      }
      kintegrator->falcon_sharc_alpha = alpha;
    }
    else {
      /* Not active: release the cache if a previous active render allocated it,
       * so a normal render reserves no extra VRAM. (Not when the photon cache
       * is using the same buffer -- add mode reads it, bake mode writes it.) */
      if (!kintegrator->falcon_photon_add && !kintegrator->falcon_photon_pass &&
          dscene->falcon_sharc_cache.size() != 0) {
        dscene->falcon_sharc_cache.free();
      }
      kintegrator->falcon_sharc_alpha = 0.0f;
    }
  }

  /* Falcon DAS (denoiser-aware sampling). FALCON_DAS_MAP points at a per-pixel
   * threshold-scale map built by the OIDN probe (raw file: int32 width, int32
   * height, then width*height float32 scales, render_pixel_index order). Only
   * valid for full-frame renders at exactly that resolution: the kernel indexes
   * the map by render_pixel_index, so border/tiled renders would misalign. */
  {
    kintegrator->falcon_das_active = 0;
    kintegrator->falcon_das_strength = 0.0f;
    const char *map_path = getenv("FALCON_DAS_MAP");
    if (map_path && map_path[0]) {
      FILE *f = fopen(map_path, "rb");
      if (f) {
        int32_t dims[2] = {0, 0};
        if (fread(dims, sizeof(int32_t), 2, f) == 2 && dims[0] > 0 && dims[1] > 0) {
          const size_t count = (size_t)dims[0] * (size_t)dims[1];
          if (dscene->falcon_das_scale.size() != count) {
            if (dscene->falcon_das_scale.size() != 0) {
              dscene->falcon_das_scale.free();
            }
            dscene->falcon_das_scale.alloc(count);
          }
          float *map = dscene->falcon_das_scale.data();
          if (fread(map, sizeof(float), count, f) == count) {
            dscene->falcon_das_scale.copy_to_device();
            dscene->falcon_das_scale.clear_modified();
            float strength = 1.0f;
            const char *strength_env = getenv("FALCON_DAS_STRENGTH");
            if (strength_env) {
              strength = (float)atof(strength_env);
            }
            kintegrator->falcon_das_active = 1;
            kintegrator->falcon_das_strength = strength;
            LOG_INFO << "Falcon DAS: loaded " << dims[0] << "x" << dims[1]
                     << " threshold-scale map (strength " << strength << ")";
          }
        }
        fclose(f);
      }
    }
    if (!kintegrator->falcon_das_active && dscene->falcon_das_scale.size() != 0) {
      dscene->falcon_das_scale.free();
    }
  }
#endif

  clear_modified();
}

void Integrator::device_free(Device * /*unused*/, DeviceScene *dscene, bool force_free)
{
  dscene->sample_pattern_lut.free_if_need_realloc(force_free);
#ifdef WITH_FALCON_SHARC
  dscene->falcon_sharc_cache.free_if_need_realloc(force_free);
  dscene->falcon_das_scale.free_if_need_realloc(force_free);
#endif
}

bool Integrator::is_modified() const
{
  return Node::is_modified() || shadow_catcher_needs_recalc_;
}

void Integrator::clear_modified()
{
  Node::clear_modified();
  shadow_catcher_needs_recalc_ = false;
}

void Integrator::tag_update(Scene *scene, const uint32_t flag)
{
  if (flag == UPDATE_ALL) {
    tag_modified();
  }

  if (flag & AO_PASS_MODIFIED) {
    /* tag only the ao_bounces socket as modified so we avoid updating sample_pattern_lut
     * unnecessarily */
    tag_ao_bounces_modified();
  }

  if (flag & OBJECT_MANAGER) {
    shadow_catcher_needs_recalc_ = true;
  }

  if (motion_blur_is_modified()) {
    scene->object_manager->tag_update(scene, ObjectManager::MOTION_BLUR_MODIFIED);
    scene->camera->tag_modified();
  }

  if (volume_ray_marching_is_modified()) {
    scene->volume_manager->tag_update_algorithm();
    scene->geometry_manager->tag_update(scene, GeometryManager::VOLUME_MODIFIED);
  }
}

uint Integrator::get_kernel_features() const
{
  uint kernel_features = 0;

  if (ao_additive_factor != 0.0f) {
    kernel_features |= KERNEL_FEATURE_AO_ADDITIVE;
  }

  if (get_use_light_tree()) {
    kernel_features |= KERNEL_FEATURE_LIGHT_TREE;
  }

  return kernel_features;
}

AdaptiveSampling Integrator::get_adaptive_sampling() const
{
  AdaptiveSampling adaptive_sampling;

  adaptive_sampling.use = use_adaptive_sampling;

  if (!adaptive_sampling.use) {
    return adaptive_sampling;
  }

  const int clamped_aa_samples = min(aa_samples, MAX_SAMPLES);

  if (clamped_aa_samples > 0 && adaptive_threshold == 0.0f) {
    adaptive_sampling.threshold = max(0.001f, 1.0f / (float)aa_samples);
    LOG_INFO << "Adaptive sampling: automatic threshold = " << adaptive_sampling.threshold;
  }
  else {
    adaptive_sampling.threshold = adaptive_threshold;
  }

  if (use_sample_subset && clamped_aa_samples > 0) {
    const int subset_samples = max(
        min(sample_subset_offset + sample_subset_length, clamped_aa_samples) -
            sample_subset_offset,
        0);

    adaptive_sampling.threshold *= sqrtf((float)subset_samples / (float)clamped_aa_samples);
  }

  if (adaptive_sampling.threshold > 0 && adaptive_min_samples == 0) {
    /* Threshold 0.1 -> 32, 0.01 -> 64, 0.001 -> 128.
     * This is highly scene dependent, we make a guess that seemed to work well
     * in various test scenes. */
    const int min_samples = (int)ceilf(16.0f / powf(adaptive_sampling.threshold, 0.3f));
    adaptive_sampling.min_samples = max(4, min_samples);
    LOG_INFO << "Adaptive sampling: automatic min samples = " << adaptive_sampling.min_samples;
  }
  else {
    adaptive_sampling.min_samples = max(4, adaptive_min_samples);
  }

  /* Arbitrary factor that makes the threshold more similar to what is was before,
   * and gives arguably more intuitive values. */
  adaptive_sampling.threshold *= 5.0f;

  adaptive_sampling.adaptive_step = 16;

  DCHECK(is_power_of_two(adaptive_sampling.adaptive_step))
      << "Adaptive step must be a power of two for bitwise operations to work";

  return adaptive_sampling;
}

DenoiseParams Integrator::get_denoise_params() const
{
  DenoiseParams denoise_params;

  denoise_params.use = use_denoise;

  denoise_params.type = denoiser_type;

  denoise_params.use_gpu = denoise_use_gpu;

  denoise_params.start_sample = denoise_start_sample;

  denoise_params.use_pass_albedo = use_denoise_pass_albedo;
  denoise_params.use_pass_normal = use_denoise_pass_normal;

  denoise_params.prefilter = denoiser_prefilter;
  denoise_params.quality = denoiser_quality;

  return denoise_params;
}

GuidingParams Integrator::get_guiding_params(const Device *device) const
{
  const bool use = use_guiding && device->info.has_guiding;

  GuidingParams guiding_params;
  guiding_params.use_surface_guiding = use && use_surface_guiding &&
                                       surface_guiding_probability > 0.0f;
  guiding_params.use_volume_guiding = use && use_volume_guiding &&
                                      volume_guiding_probability > 0.0f;
  guiding_params.use = guiding_params.use_surface_guiding || guiding_params.use_volume_guiding;
  guiding_params.type = guiding_distribution_type;
  guiding_params.training_samples = guiding_training_samples;
  guiding_params.deterministic = deterministic_guiding;
  guiding_params.sampling_type = guiding_directional_sampling_type;
  // In Blender/Cycles the user set roughness is squared to behave more linear.
  guiding_params.roughness_threshold = guiding_roughness_threshold * guiding_roughness_threshold;
  return guiding_params;
}
CCL_NAMESPACE_END
