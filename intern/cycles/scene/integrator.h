/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "kernel/types.h"

#include "device/denoise.h" /* For the parameters and type enum. */
#include "graph/node.h"
#include "integrator/adaptive_sampling.h"
#include "integrator/guiding.h"

CCL_NAMESPACE_BEGIN

class Device;
class DeviceScene;
class Scene;

struct HaltonSequence {
  HaltonSequence()
  {
    reset();
  }

  void reset()
  {
    a2 = 0;
    b2 = 1;
    a3 = 0;
    b3 = 1;
  }
  float2 next();

  int a2, b2;
  int a3, b3;
};

#ifdef WITH_FALCON_SHARC
/* Falcon SHARC cache mode, mirroring the FALCON_SHARC_MODE strings the
 * environment bridge used ("warmup"/"blend"/"live"). */
enum FalconSharcMode {
  FALCON_SHARC_MODE_OFF = 0,
  FALCON_SHARC_MODE_WARMUP = 1,
  FALCON_SHARC_MODE_BLEND = 2,
  FALCON_SHARC_MODE_LIVE = 3,
};
#endif

class Integrator : public Node {
 public:
  NODE_DECLARE

  NODE_SOCKET_API(int, min_bounce)
  NODE_SOCKET_API(int, max_bounce)

  NODE_SOCKET_API(int, max_diffuse_bounce)
  NODE_SOCKET_API(int, max_glossy_bounce)
  NODE_SOCKET_API(int, max_transmission_bounce)
  NODE_SOCKET_API(int, max_volume_bounce)

#ifdef WITH_CYCLES_DEBUG
  NODE_SOCKET_API(DirectLightSamplingType, direct_light_sampling_type)
#endif

  NODE_SOCKET_API(int, transparent_min_bounce)
  NODE_SOCKET_API(int, transparent_max_bounce)

  NODE_SOCKET_API(int, ao_bounces)
  NODE_SOCKET_API(float, ao_factor)
  NODE_SOCKET_API(float, ao_distance)
  NODE_SOCKET_API(float, ao_additive_factor)

  NODE_SOCKET_API(bool, volume_ray_marching)
  NODE_SOCKET_API(int, volume_max_steps)
  NODE_SOCKET_API(float, volume_step_rate)

  NODE_SOCKET_API(bool, use_guiding);
  NODE_SOCKET_API(bool, deterministic_guiding);
  NODE_SOCKET_API(bool, use_surface_guiding);
  NODE_SOCKET_API(float, surface_guiding_probability);
  NODE_SOCKET_API(bool, use_volume_guiding);
  NODE_SOCKET_API(float, volume_guiding_probability);
  NODE_SOCKET_API(int, guiding_training_samples);
  NODE_SOCKET_API(bool, use_guiding_direct_light);
  NODE_SOCKET_API(bool, use_guiding_mis_weights);
  NODE_SOCKET_API(GuidingDistributionType, guiding_distribution_type);
  NODE_SOCKET_API(GuidingDirectionalSamplingType, guiding_directional_sampling_type);
  NODE_SOCKET_API(float, guiding_roughness_threshold);

  NODE_SOCKET_API(bool, caustics_reflective)
  NODE_SOCKET_API(bool, caustics_refractive)
  NODE_SOCKET_API(float, filter_glossy)

  NODE_SOCKET_API(bool, use_direct_light);
  NODE_SOCKET_API(bool, use_indirect_light);
  NODE_SOCKET_API(bool, use_diffuse);
  NODE_SOCKET_API(bool, use_glossy);
  NODE_SOCKET_API(bool, use_transmission);
  NODE_SOCKET_API(bool, use_emission);

  NODE_SOCKET_API(int, seed)

  NODE_SOCKET_API(float, sample_clamp_direct)
  NODE_SOCKET_API(float, sample_clamp_indirect)
  NODE_SOCKET_API(bool, motion_blur)

  /* Maximum number of samples, beyond which we are likely to run into
   * precision issues for sampling patterns. */
  static const int MAX_SAMPLES = (1 << 24);

  NODE_SOCKET_API(int, aa_samples)

  NODE_SOCKET_API(bool, use_sample_subset)
  NODE_SOCKET_API(int, sample_subset_offset)
  NODE_SOCKET_API(int, sample_subset_length)

  NODE_SOCKET_API(bool, use_light_tree)
  NODE_SOCKET_API(float, light_sampling_threshold)

  NODE_SOCKET_API(bool, use_adaptive_sampling)
  NODE_SOCKET_API(int, adaptive_min_samples)
  NODE_SOCKET_API(float, adaptive_threshold)

  NODE_SOCKET_API(SamplingPattern, sampling_pattern)
  NODE_SOCKET_API(float, scrambling_distance)

  NODE_SOCKET_API(bool, use_pixel_jitter);
  NODE_SOCKET_API(bool, use_custom_pixel_jitter_sample);
  NODE_SOCKET_API_ARRAY(array<float>, custom_pixel_jitter_sample);
  HaltonSequence pixel_jitter_state;
  int pixel_jitter_frame = 0;

  /* DLSS-RR rebuilds sub-pixel detail from the jitter moving between frames, so
   * every sample of one frame has to be taken at the same sub-pixel point --
   * that is what a game's one-sample-per-frame render hands it. Cycles advanced
   * the jitter on every scene update, which in a progressive render means once
   * per sample batch: a 32-sample frame averaged three different sub-pixel
   * positions, so the detail RR was meant to reconstruct was already gone by the
   * time it saw the buffer, and the jitter we reported described only the last
   * batch. Pin it for the frame in final renders; the viewport keeps advancing,
   * where each update is a fresh sample that RR converges over its own history. */
  void pin_pixel_jitter_per_frame(bool pin)
  {
    pixel_jitter_pinned_ = pin;
  }
  void advance_pixel_jitter()
  {
    pixel_jitter_pending_ = true;
  }
  bool pixel_jitter_pinned_ = false;
  bool pixel_jitter_pending_ = true;
  float2 pixel_jitter_current_ = make_float2(0.0f, 0.0f);

  NODE_SOCKET_API(bool, use_denoise);
  NODE_SOCKET_API(DenoiserType, denoiser_type);
  NODE_SOCKET_API(int, denoise_start_sample);
  NODE_SOCKET_API(DenoiserPassMask, denoiser_passes);
  NODE_SOCKET_API(DenoiserPrefilter, denoiser_prefilter);
  NODE_SOCKET_API(bool, denoise_use_gpu);
  NODE_SOCKET_API(DenoiserQuality, denoiser_quality);
  NODE_SOCKET_API(float, denoiser_upscale_factor);
  NODE_SOCKET_API(bool, denoiser_carry_history);
  NODE_SOCKET_API(int, denoiser_preroll_passes);
  NODE_SOCKET_API(int, denoiser_preroll_passes_cut);
  NODE_SOCKET_API(bool, denoiser_cut_warmup);

#ifdef WITH_FALCON_SHARC
  /* Falcon knobs. These used to be read straight from the environment inside
   * device_update(), which made them process-global: the viewport and the final
   * render could not hold different values, and nothing survived a .blend save.
   * They are sockets now, fed from the scene's Cycles properties in sync.cpp.
   * The environment variables still win when set, so the A/B harnesses under
   * obs2/claude_memo/ベンチマーク keep working unchanged; a GUI session sets no
   * environment and therefore rides on these sockets.
   *
   * Per-pass state that an operator computes (FALCON_PHOTON_MODE/N/TARGET/WORLD/
   * MAXPTS/POINTS, FALCON_LIGHTTRACE[_SAMPLES]) is deliberately NOT here -- it
   * is not a user knob and still travels by environment. */
  NODE_SOCKET_API(int, falcon_sharc_mode);
  NODE_SOCKET_API(float, falcon_sharc_cell);
  NODE_SOCKET_API(float, falcon_sharc_alpha);
  NODE_SOCKET_API(float, falcon_sharc_keep);
  NODE_SOCKET_API(ustring, falcon_sharc_cache);
  NODE_SOCKET_API(bool, falcon_sharc_gate);
  NODE_SOCKET_API(float, falcon_sharc_gate_low);
  NODE_SOCKET_API(float, falcon_sharc_gate_high);
  NODE_SOCKET_API(float, falcon_dispersion_b);
  NODE_SOCKET_API(float, falcon_photon_radius);
  NODE_SOCKET_API(float, falcon_photon_point_radius_m);
  NODE_SOCKET_API(float, falcon_photon_point_normal_deg);
  NODE_SOCKET_API(float, falcon_photon_point_gain);
  NODE_SOCKET_API(float, falcon_lt_gain);
  NODE_SOCKET_API(float, falcon_lt_splat_radius);
  NODE_SOCKET_API(bool, falcon_lt_visibility);
  NODE_SOCKET_API(bool, falcon_lt_direct);
  NODE_SOCKET_API(ustring, falcon_das_map);
  NODE_SOCKET_API(float, falcon_das_strength);
  NODE_SOCKET_API(ustring, falcon_error_map);
  NODE_SOCKET_API(float, falcon_error_cell);
  NODE_SOCKET_API(float, falcon_error_threshold);
  NODE_SOCKET_API(bool, falcon_error_raise_alpha);
#endif

  enum : uint32_t {
    AO_PASS_MODIFIED = (1 << 0),
    OBJECT_MANAGER = (1 << 1),

    /* tag everything in the manager for an update */
    UPDATE_ALL = ~0u,

    UPDATE_NONE = 0u,
  };

  bool shadow_catcher_needs_recalc_ = true;

  Integrator();
  ~Integrator() override;

  void device_update(Device *device, DeviceScene *dscene, Scene *scene);
  void device_free(Device *device, DeviceScene *dscene, bool force_free = false);

  void tag_update(Scene *scene, const uint32_t flag);

  uint get_kernel_features() const;

  AdaptiveSampling get_adaptive_sampling() const;
  DenoiseParams get_denoise_params() const;
  GuidingParams get_guiding_params(const Device *device) const;

  bool is_modified() const;
  void clear_modified();
};

CCL_NAMESPACE_END
