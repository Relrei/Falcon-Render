/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#include "device/memory.h"
#include "graph/node.h"
#include "scene/pass.h"

#include "kernel/types.h"

#include "util/string.h"
#include "util/unique_ptr.h"
#include "util/vector.h"

CCL_NAMESPACE_BEGIN

class Device;
struct DeviceDrawParams;
struct float4;

/* NOTE: Is not a real scene node. Using Node API for ease of (de)serialization. */
class BufferPass : public Node {
 public:
  NODE_DECLARE

  PassType type = PASS_NONE;
  PassMode mode = PassMode::NOISY;
  ustring name;
  bool include_albedo = false;
  ustring lightgroup;

  int offset = -1;

  BufferPass();
  explicit BufferPass(const Pass *scene_pass);

  BufferPass(BufferPass &&other) noexcept = default;
  BufferPass(const BufferPass &other) = default;

  BufferPass &operator=(BufferPass &&other) = default;
  BufferPass &operator=(const BufferPass &other) = default;

  ~BufferPass() override = default;

  PassInfo get_info() const;

  bool operator==(const BufferPass &other) const
  {
    return type == other.type && mode == other.mode && name == other.name &&
           include_albedo == other.include_albedo && lightgroup == other.lightgroup &&
           offset == other.offset;
  }
  bool operator!=(const BufferPass &other) const
  {
    return !(*this == other);
  }
};

/* Buffer Parameters
 * Size of render buffer and how it fits in the full image (border render). */

/* NOTE: Is not a real scene node. Using Node API for ease of (de)serialization. */
class BufferParams : public Node {
 public:
  NODE_DECLARE

  /* Width/height of the physical buffer. */
  int width = 0;
  int height = 0;

  /* Windows defines which part of the buffers is visible. The part outside of the window is
   * considered an `overscan`.
   *
   * Window X and Y are relative to the position of the buffer in the full buffer. */
  int window_x = 0;
  int window_y = 0;
  int window_width = 0;
  int window_height = 0;

  /* Offset into and width/height of the full buffer. */
  int full_x = 0;
  int full_y = 0;
  int full_width = 0;
  int full_height = 0;

  /* Rectangle this buffer is drawn into on screen, in the coordinates the display driver draws
   * in, when that is not the same as the buffer's own geometry.
   *
   * Everything above describes the render: the buffer covers `width` x `height` pixels at
   * (`full_x`, `full_y`) of a `full_width` x `full_height` raster, and the camera is fitted to
   * that raster (Session::update_scene). Normally the render is also what is on screen, one
   * pixel per pixel, so the display driver reads the same fields. When the two are allowed to
   * differ -- currently only the viewport camera view with DLSS-RR, which renders a size that
   * the zoom cannot move -- these carry the on-screen side and the display stretches the
   * rendered result over them, the same way it already stretches a resolution-divided render.
   *
   * A zero `display_width` (the default) means "the render is the screen", which is upstream
   * behaviour. These are screen pixels, so the resolution divider must not scale them
   * (PathTrace::scale_buffer_params overwrites only the render fields, so they pass through). */
  int display_x = 0;
  int display_y = 0;
  int display_width = 0;
  int display_height = 0;
  int display_full_width = 0;
  int display_full_height = 0;

  /* Runtime fields, only valid after `update_passes()` or `update_offset_stride()`. */
  int offset = -1, stride = -1;

  /* Runtime fields, only valid after `update_passes()`. */
  int pass_stride = -1;

  /* Properties which are used for accessing buffer pixels outside of scene graph. */
  vector<BufferPass> passes;
  ustring layer;
  ustring view;
  int samples = 0;
  float exposure = 1.0f;
  bool use_approximate_shadow_catcher = false;
  bool use_transparent_background = false;

  BufferParams();

  BufferParams(BufferParams &&other) noexcept = default;
  BufferParams(const BufferParams &other) = default;

  BufferParams &operator=(BufferParams &&other) = default;
  BufferParams &operator=(const BufferParams &other) = default;

  ~BufferParams() override = default;

  /* Pre-calculate all fields which depends on the passes.
   *
   * When the scene passes are given, the buffer passes will be created from them and stored in
   * this params, and then params are updated for those passes.
   * The `update_passes()` without parameters updates offsets and strides which are stored outside
   * of the passes. */
  void update_passes();
  void update_passes(const unique_ptr_vector<Pass> &scene_passes);

  /* Returns PASS_UNUSED if there is no such pass in the buffer. */
  int get_pass_offset(PassType type, PassMode mode = PassMode::NOISY) const;

  /* Returns nullptr if pass with given name does not exist. */
  const BufferPass *find_pass(string_view name) const;
  const BufferPass *find_pass(PassType type, PassMode mode = PassMode::NOISY) const;

  /* Get display pass from its name.
   * Will do special logic to replace combined pass with shadow catcher matte. */
  const BufferPass *get_actual_display_pass(PassType type, PassMode mode = PassMode::NOISY) const;
  const BufferPass *get_actual_display_pass(const BufferPass *pass) const;

  void update_offset_stride();

  bool modified(const BufferParams &other) const;

 protected:
  void reset_pass_offset();

  /* Multiplied by 2 to be able to store noisy and denoised pass types. */
  static constexpr int kNumPassOffsets = PASS_NUM * 2;

  /* Indexed by an index derived from pass type and mode, indicates offset of the corresponding
   * pass in the buffer.
   * If there are multiple passes with same type and mode contains lowest offset of all of them. */
  int pass_offset_[kNumPassOffsets];
};

/* Render Buffers */

class RenderBuffers {
 public:
  /* buffer parameters */
  BufferParams params;

  /* float buffer */
  device_vector<float> buffer;

  explicit RenderBuffers(Device *device);
  ~RenderBuffers();

  void reset(const BufferParams &params);
  void zero();

  bool copy_from_device();
  void copy_to_device();
};

/* Copy denoised passes form source to destination.
 *
 * Buffer parameters are provided explicitly, allowing to copy pixels between render buffers which
 * content corresponds to a render result at a non-unit resolution divider.
 *
 * `src_offset` allows to offset source pixel index which is used when a fraction of the source
 * buffer is to be copied.
 *
 * Copy happens of the number of pixels in the destination. */
void render_buffers_host_copy_denoised(RenderBuffers *dst,
                                       const BufferParams &dst_params,
                                       const RenderBuffers *src,
                                       const BufferParams &src_params,
                                       const size_t src_offset = 0);

CCL_NAMESPACE_END
