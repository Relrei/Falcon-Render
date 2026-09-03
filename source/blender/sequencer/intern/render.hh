/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include "DNA_listBase.h"

#include "BLI_math_vector_types.hh"
#include "BLI_set.hh"

namespace blender {

struct Depsgraph;
struct ImBuf;
struct Mask;
struct RenderData;
struct Scene;
struct SeqTimelineChannel;
struct Strip;

namespace seq {

/* Recursion protection while rendering a single sequencer frame.
 * If the same scene or strip is seen, recursion stops. */
struct SeqRenderState {
  Set<Scene *> scenes_in_progress;
  Set<Strip *> strips_in_progress;

  /* Is the top-level render request for the scene's currently evaluated frame. */
  bool is_current_frame = false;
};

/* Strip corner coordinates in screen pixel space. Note that they might not be
 * axis aligned when rotation is present. */
struct StripScreenQuad {
  float2 v0, v1, v2, v3;

  bool is_empty() const
  {
    return v0 == v1 && v2 == v3 && v0 == v2;
  }
};

/**
 * Result of rendering a strip: the produced image,
 * plus some auxiliary data.
 */
struct SeqResult {
  bool is_valid() const
  {
    return image != nullptr;
  }

  ImBuf *image = nullptr;
  /* How much the resulting image should be translated, in pixels. */
  float2 translation = float2(0, 0);
  bool is_opaque_before_transform = false;
};

SeqResult seq_render_give_ibuf_seqbase(const RenderData *context,
                                       SeqRenderState *state,
                                       float timeline_frame,
                                       int chan_shown,
                                       ListBaseT<SeqTimelineChannel> *channels,
                                       ListBaseT<Strip> *seqbasep);
SeqResult seq_render_strip(const RenderData *context,
                           SeqRenderState *state,
                           Strip *strip,
                           float timeline_frame);

/* Renders Mask into an image suitable for sequencer:
 * RGB channels contain mask intensity; alpha channel is opaque. */
ImBuf *seq_render_mask(Depsgraph *depsgraph,
                       int width,
                       int height,
                       const Mask *mask,
                       float frame_index,
                       bool make_float);

/* Converts image to sequencer color space, if needed. */
void ensure_ibuf_is_sequencer_space(const Scene *scene, ImBuf *ibuf, bool make_float);

void seq_imbuf_assign_spaces(const Scene *scene, ImBuf *ibuf);

StripScreenQuad get_strip_screen_quad(const RenderData *context, const Strip *strip);

void convert_multilayer_ibuf(ImBuf *ibuf);
bool seq_image_strip_is_multiview_render(const Scene *scene,
                                         const Strip *strip,
                                         int totfiles,
                                         const char *filepath,
                                         char *r_prefix,
                                         const char *r_ext);

/**
 * Block until every in-flight BL_VSE_PREFETCH_N background decode task
 * (see render.cc) has finished. Must be called before the source image
 * cache it writes into is torn down (source_image_cache_destroy()) --
 * otherwise a still-running task can call source_image_cache_put() on a
 * Scene that has already been freed (use-after-free). No-op, without
 * touching the task pool at all, when BL_VSE_PREFETCH_N=0 (read-ahead
 * is on by default with N=8).
 */
void vse_prefetch_wait_all();

/**
 * Block until every currently in-flight BL_VSE_PREFETCH_N background
 * decode task for this exact Strip (see render.cc) has finished. Must be
 * called before the caller frees `strip` -- otherwise a still-running
 * task's vse_prefetch_task_run() can dereference `strip` (its `->start`,
 * via source_image_cache_put()) after it has already been freed
 * (use-after-free). Cheap when no task is currently reading ahead for
 * this particular strip (the common case): a single lookup under the
 * prefetch state's own mutex, no task-pool wait at all. No-op, without
 * touching anything, when BL_VSE_PREFETCH_N=0 (read-ahead is on by
 * default with N=8).
 */
void vse_prefetch_wait_for_strip(const Strip *strip);

}  // namespace seq
}  // namespace blender
