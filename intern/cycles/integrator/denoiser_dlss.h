/* SPDX-FileCopyrightText: 2025 NVIDIA Corporation
 * SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_DLSS

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
  }

  void set_same_frame_restart(bool same_frame_restart) override
  {
    same_frame_restart_ = same_frame_restart;
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
  CUDATexture tex_depth_;
  CUDATexture tex_diffuse_albedo_;
  CUDATexture tex_specular_albedo_;
  CUDATexture tex_normal_roughness_;
  CUDATexture tex_motion_;
  CUDATexture tex_specular_motion_;
  CUDATexture tex_specular_hit_distance_;
  CUDATexture tex_output_;

  int last_width_ = 0;
  int last_height_ = 0;
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
   * FALCON_DLSS_NO_CARRY forces it off for debugging. */
  bool use_carry_history() const;
  /* ColorBeforeTransparency guide is active when the transmission passes exist
   * in the render buffers (enabled via DENOISER_PASS_TRANSMISSION). */
  bool use_transparency_guide(const DenoiseContext &context) const;
  /* Whether a real specular motion pass exists to hand to RR. */
  bool use_specular_motion(const DenoiseContext &context) const;
  /* Whether the specular hit distance pass and its camera matrices are ready. */
  bool use_specular_hit_distance(const DenoiseContext &context) const;

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
};

CCL_NAMESPACE_END

#endif
