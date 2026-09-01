/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

/* TODO(sergey): The integrator folder might not be the best. Is easy to move files around if the
 * better place is figured out. */

#include <functional>

#include "device/denoise.h"
#include "device/device.h"
#include "util/unique_ptr.h"

CCL_NAMESPACE_BEGIN

class BufferParams;
class Device;
class GraphicsInteropDevice;
class RenderBuffers;
class Progress;

bool use_dlss_denoiser(Device *denoiser_device, const DenoiseParams &params);

bool use_optix_denoiser(Device *denoiser_device, const DenoiseParams &params);

bool use_gpu_oidn_denoiser(Device *denoiser_device, const DenoiseParams &params);

DenoiseParams get_effective_denoise_params(Device *denoiser_device,
                                           Device *cpu_fallback_device,
                                           const DenoiseParams &params,
                                           const GraphicsInteropDevice &interop_device,
                                           Device *&single_denoiser_device);

/* Implementation of a specific denoising algorithm.
 *
 * This class takes care of breaking down denoising algorithm into a series of device calls or to
 * calls of an external API to denoise given input.
 *
 * TODO(sergey): Are we better with device or a queue here? */
class Denoiser {
 public:
  /* Create denoiser for the given path trace device.
   *
   * Notes:
   * - The denoiser must be configured. This means that `params.use` must be true.
   *   This is checked in debug builds.
   * - The device might be MultiDevice.
   * - If Denoiser from params is not supported by provided denoise device, then Blender will
   *   fallback on the OIDN CPU denoising and use provided cpu_fallback_device.
   * - Specifying the graphics interop device helps pick a more efficient denoising device.*/
  static unique_ptr<Denoiser> create(Device *denoiser_device,
                                     Device *cpu_fallback_device,
                                     const DenoiseParams &params,
                                     const GraphicsInteropDevice &interop_device);

  virtual ~Denoiser() = default;

  void set_params(const DenoiseParams &params);
  const DenoiseParams &get_params() const;

  /* Drop any temporal history the denoiser carries (DLSS-RR). Called when the
   * scene content changed (anything but the camera): motion vectors cannot
   * explain such edits, so carried history would ghost the old content. */
  virtual void clear_temporal_history() {}

  /* The next denoise restarts the sample count without changing the frame (a
   * DLSS-RR pre-roll pass), so the history is already aligned with it. */
  virtual void set_same_frame_restart(bool /*same_frame_restart*/) {}

  /* The denoise about to run belongs to a DLSS-RR pre-roll pass whose image is
   * discarded. */
  virtual void set_preroll_pass(bool /*preroll_pass*/) {}

  /* Number of the frame the next denoise belongs to (the scene's current frame).
   * DLSS-RR uses it to tell "a new frame" from "more samples on the same frame";
   * counting samples alone is wrong once a time limit or adaptive sampling makes
   * the per-frame sample count move up and down. */
  virtual void set_frame(int /*frame*/) {}

  /* Camera matrices for the frame being denoised, in the row-major left-multiply
   * form NGX wants. DLSS-RR needs them to make sense of the specular hit
   * distance, which is a world-space length. */
  virtual void set_camera_matrices(const float * /*world_to_view*/,
                                   const float * /*view_to_clip*/)
  {
  }

  static bool is_device_supported(DenoiserType type, const DeviceInfo &denoise_device_info);

  /* Recommended type for viewport denoising. */
  static DenoiserType automatic_viewport_denoiser_type(const DeviceInfo &denoise_device_info);

  /* Create devices and load kernels needed for denoising.
   * The progress is used to communicate state when kernels actually needs to be loaded.
   *
   * NOTE: The `progress` is an optional argument, can be nullptr. */
  virtual bool load_kernels(Progress *progress);

  /* Denoise the entire buffer.
   *
   * Buffer parameters denotes an effective parameters used during rendering. It could be
   * a lower resolution render into a bigger allocated buffer, which is used in viewport during
   * navigation and non-unit pixel size. Use that instead of render_buffers->params.
   *
   * The buffer might be coming from a "foreign" device from what this denoise is created for.
   * This means that in general case the denoiser will make sure the input data is available on
   * the denoiser device, perform denoising, and put data back to the device where the buffer
   * came from.
   *
   * The `num_samples` corresponds to the number of samples in the render buffers. It is used
   * to scale buffers down to the "final" value in algorithms which don't do automatic exposure,
   * or which needs "final" value for data passes.
   *
   * The `allow_inplace_modification` means that the denoiser is allowed to do in-place
   * modification of the input passes (scaling them down i.e.). This will lower the memory
   * footprint of the denoiser but will make input passes "invalid" (from path tracer) point of
   * view.
   *
   * Returns true when all passes are denoised. Will return false if there is a denoiser error (for
   * example, caused by misconfigured denoiser) or when user requested to cancel rendering. */
  virtual bool denoise_buffer(const BufferParams &buffer_params,
                              const BufferParams &denoised_buffer_params,
                              RenderBuffers *render_buffers,
                              int num_samples,
                              bool allow_inplace_modification,
                              float2 pixel_jitter = {}) = 0;

  /* Get a device which is used to perform actual denoising.
   *
   * Notes:
   *
   * - The device can be different from the path tracing device. This happens, for example, when
   *   using OptiX denoiser and rendering on CPU.
   *
   * - No threading safety is ensured in this call. This means, that it is up to caller to ensure
   *   that there is no threading-conflict between denoising task lazily initializing the device
   *   and access to this device happen. */
  Device *get_denoiser_device() const;

  std::function<bool(void)> is_cancelled_cb;

  bool is_cancelled() const
  {
    if (!is_cancelled_cb) {
      return false;
    }
    return is_cancelled_cb();
  }

  void set_error(const string &error)
  {
    denoiser_device_->set_error(error);
  }

 protected:
  Denoiser(Device *denoiser_device, const DenoiseParams &params);

  Device *denoiser_device_;
  bool denoise_kernels_are_loaded_;
  DenoiseParams params_;
};

CCL_NAMESPACE_END
