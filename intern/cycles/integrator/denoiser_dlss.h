/* SPDX-FileCopyrightText: 2025 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_DLSS

#  include <climits>

#  include "integrator/denoiser_gpu.h"

struct NVSDK_NGX_Handle;
struct NVSDK_NGX_CUDADevice;

CCL_NAMESPACE_BEGIN

/* Implementation of denoising API which uses DLSS. */
class DLSSDenoiser : public DenoiserGPU {
 public:
  DLSSDenoiser(Device *denoiser_device, const DenoiseParams &params);
  ~DLSSDenoiser();

  static bool is_device_supported(const DeviceInfo &device);

  /* Scene content changed: the history would ghost, restart cleanly. */
  void clear_temporal_history() override
  {
    have_history_ = false;
    preroll_history_poisoned_ = false;
    /* Dropping the history has to reach RR as Reset=1, and the old rule
     * (is_reset = frame_transition && !(carry && have_history_)) could never
     * deliver it: with carry on it is identically false, and with carry off it
     * still needs a frame transition that a time-limited render does not
     * produce. So a cut's clear was thrown away and the previous shot bled into
     * the new one. Latch it here and let the next evaluate spend it. */
    pending_reset_ = true;
  }

  void set_frame(int frame) override
  {
    frame_ = frame;
  }

  void set_same_frame_restart(bool same_frame_restart) override
  {
    same_frame_restart_ = same_frame_restart;
  }

  void set_preroll_pass(bool preroll_pass) override
  {
    preroll_pass_ = preroll_pass;
  }

  void set_camera_matrices(const float *world_to_view, const float *view_to_clip) override;

 private:
  bool denoise_create_if_needed(DenoiseContext &context) override;

  bool denoise_configure_if_needed(DenoiseContext &context) override;

  bool denoise_filter_color_preprocess(const DenoiseContext &context,
                                       const DenoisePass &pass) override;
  bool denoise_filter_color_postprocess(const DenoiseContext &context,
                                        const DenoisePass &pass) override;

  bool denoise_filter_guiding_preprocess(DenoiseContext &context) override;

  bool denoise_run(const DenoiseContext &context, const DenoisePass &pass) override;

  NVSDK_NGX_Handle *handle_ = nullptr;
  NVSDK_NGX_CUDADevice *ngx_device_ = nullptr;

  struct CUDATexture {
    void init(Device *device, int width, int height, int num_components);
    void destroy();

    void *array = nullptr;
    uint64_t texture_handle = 0;
    uint64_t surface_handle = 0;
  };
  CUDATexture tex_color_;
  CUDATexture tex_color_before_transparency_;
  /* FALCON_DLSS_LAYER_GUIDES=1: ColorBeforeParticles / ColorBeforeFog. */
  CUDATexture tex_color_before_particles_;
  CUDATexture tex_color_before_fog_;
  CUDATexture tex_depth_;
  CUDATexture tex_diffuse_albedo_;
  CUDATexture tex_specular_albedo_;
  CUDATexture tex_normal_roughness_;
  CUDATexture tex_motion_;
  CUDATexture tex_specular_motion_;
  CUDATexture tex_specular_hit_distance_;
  CUDATexture tex_emissive_;
  CUDATexture tex_output_;
  /* 1x1 exposure texture (FALCON_DLSS_EXPOSURE_TEX); only created when the env var is set. */
  CUDATexture tex_exposure_;

  /* Frame the current denoise belongs to, and the one the last evaluate saw.
   * See the frame-transition note in denoise_run. */
  int frame_ = 0;
  int last_frame_ = INT_MIN;

  /* clear_temporal_history() latched a Reset that no evaluate has spent yet. */
  bool pending_reset_ = false;

  int last_width_ = 0;
  int last_height_ = 0;
  /* The size the path trace actually hands in. The feature is built for one
   * input size (the runtime derives its dynamic-resolution window from it), so
   * a change here needs a recreate just as much as a change of the output
   * size -- which is what happens when playback renders frames smaller. */
  int last_in_width_ = 0;
  int last_in_height_ = 0;
  float last_upscale_factor_ = 0.0f;

  /* Track sample count to detect when the viewport accumulation restarted (navigation, scene
   * edit, timeline scrub). On a restart the accumulated sample count drops, which means the
   * temporal history DLSS holds is stale and would ghost, so we ask DLSS to reset it. */
  int last_num_samples_ = 0;

  /* Carry the RR temporal history across restarts instead of resetting: the
   * transition (sample count drop) feeds the real motion vectors so the
   * history is warped into alignment, mirroring how games run DLSS. Used by
   * final renders across animation frames and by the viewport across
   * navigation restarts (interactive motion passes). Reads params_ live so a
   * UI toggle applies to a running denoiser via set_params;
   * FALCON_DLSS_NO_CARRY forces it off for debugging.
   *
   * Carrying only pays while each frame is cheap. Measured on slowcam (8
   * frames, RMSE against 1024 spp): carry wins at 1/2/4 samples per frame
   * (0.0567 vs 0.0630, 0.0440 vs 0.0468, 0.0367 vs 0.0374) and loses from 8
   * upwards (0.0315 vs 0.0311, 0.0279 vs 0.0268, 0.0254 vs 0.0240), and at 32
   * it loses in all five test scenes. So the choice is made per frame from the
   * sample count: below FALCON_DLSS_CARRY_MAX_SPP (default 8) the history is
   * carried, at or above it the frame starts cold. Pre-roll passes are exempt
   * -- they are re-renders of one frame and their chain is what buys the clean
   * first frame. FALCON_DLSS_CARRY_AUTO=0 restores the old fixed behaviour. */
  bool use_carry_history(const DenoiseContext &context) const;

  /* True when this denoise starts a new frame rather than adding samples to the
   * one already being denoised. */
  bool is_frame_transition(const DenoiseContext &context) const;
  /* ColorBeforeTransparency guide is active when the transmission passes exist
   * in the render buffers (enabled via DENOISER_PASS_TRANSMISSION). */
  bool use_transparency_guide(const DenoiseContext &context) const;
  /* FALCON_DLSS_LAYER_GUIDES=1: pass all three ColorBefore* layers. */
  bool use_layer_guides() const;
  /* FALCON_DLSS_PARTICLES_GUIDE_RAW: baked "before particles" colour from a file. */
  bool load_particles_guide(const DenoiseContext &context, const char *pattern);
  /* Whether a real specular motion pass exists to hand to RR. */
  bool use_specular_motion(const DenoiseContext &context) const;
  /* Whether the specular hit distance pass and its camera matrices are ready. */
  bool use_specular_hit_distance(const DenoiseContext &context) const;
  bool use_emissive_guide(const DenoiseContext &context) const;

  /* Debug: save the guide textures RR is about to consume to raw files
   * (FALCON_DLSS_GUIDE_DUMP=dir), or overwrite them from such a dump
   * (FALCON_DLSS_GUIDE_LOAD=dir). Loading guides dumped from a high-spp render
   * into a low-spp one gives "perfect guides + noisy color" -- the upper bound
   * of what any guide prefiltering could buy. */
  bool transfer_guides(const DenoiseContext &context, bool dump, const char *dir);

  /* Row-major, left-multiply, as NGX wants. */
  float world_to_view_[16] = {};
  float view_to_clip_[16] = {};
  bool have_camera_matrices_ = false;
  /* Whether the current feature instance has produced at least one result
   * (i.e. there is a history worth carrying/aligning). */
  bool have_history_ = false;

  /* The sample count restart being denoised is a re-render of the same frame
   * (pre-roll pass), so the history needs no motion warp. */
  bool same_frame_restart_ = false;

  /* The current denoise is a pre-roll pass (its image is discarded). */
  bool preroll_pass_ = false;

  /* The history was built by pre-roll passes on a frame that never moved, so it
   * must not be reprojected onto the next frame (mode 2). */
  bool preroll_history_poisoned_ = false;
};

CCL_NAMESPACE_END

#endif
