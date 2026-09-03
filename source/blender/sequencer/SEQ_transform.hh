/* SPDX-FileCopyrightText: 2004 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 */

#include <cmath>
#include <cstdlib>

#include "DNA_listBase.h"

#include "BLI_array.hh"
#include "BLI_bounds_types.hh"
#include "BLI_math_base.h"
#include "BLI_math_matrix_types.hh"
#include "BLI_span.hh"

/* For MAX_CHANNELS, needed by the channel<->Y flip mapping below. */
#include "SEQ_sequencer.hh"

namespace blender {

struct Scene;
struct Strip;
struct SeqTimelineChannel;
struct TimeMarker;

namespace seq {

bool transform_strip_can_be_translated(const Strip *strip);
/**
 * Checks whether the strip functions as a single static display,
 * which means it has only one unique frame of content and does not draw holds.
 * This includes non-sequence image strips and all effect strips with no inputs (e.g. color, text).
 */
bool transform_single_image_check(const Strip *strip);
bool transform_test_overlap(const Scene *scene, ListBaseT<Strip> *seqbasep, Strip *test);
bool transform_test_overlap(const Scene *scene, Strip *strip1, Strip *strip2);
void transform_translate_strip(Scene *evil_scene, Strip *strip, int delta);
/**
 * \return 0 if there weren't enough space.
 */
bool transform_seqbase_shuffle_ex(ListBaseT<Strip> *seqbasep,
                                  Strip *test,
                                  Scene *evil_scene,
                                  int channel_delta);
bool transform_seqbase_shuffle(ListBaseT<Strip> *seqbasep, Strip *test, Scene *evil_scene);
bool transform_seqbase_shuffle_time(Span<Strip *> strips_to_shuffle,
                                    Span<Strip *> time_dependent_strips,
                                    ListBaseT<Strip> *seqbasep,
                                    Scene *evil_scene,
                                    ListBaseT<TimeMarker> *markers,
                                    bool use_sync_markers);
bool transform_seqbase_shuffle_time(Span<Strip *> strips_to_shuffle,
                                    ListBaseT<Strip> *seqbasep,
                                    Scene *evil_scene,
                                    ListBaseT<TimeMarker> *markers,
                                    bool use_sync_markers);

void transform_handle_overlap(Scene *scene,
                              ListBaseT<Strip> *seqbasep,
                              Span<Strip *> transformed_strips,
                              Span<Strip *> time_dependent_strips,
                              bool use_sync_markers);
void transform_handle_overlap(Scene *scene,
                              ListBaseT<Strip> *seqbasep,
                              Span<Strip *> transformed_strips,
                              bool use_sync_markers);
/**
 * Set strip channel. This value is clamped to valid values.
 */
void strip_channel_set(Strip *strip, int channel);

/**
 * Whether the timeline's channel<->Y mapping is flipped (channel 1 drawn at the top, higher
 * channel numbers stacking downward) instead of the historical layout (channel 1 at the bottom).
 *
 * Controlled by the `FALCON_VSE_FLIP_CHANNELS` environment variable (default off, i.e.
 * unchanged behavior). Read once via `getenv()` on first use and cached in a function-local
 * static, so per-frame drawing code never calls `getenv()` again. Because this is defined in a
 * header, the static is still a single instance program-wide (inline-function semantics), so
 * every translation unit that includes this header shares the same cached value.
 */
inline bool channel_flip_enabled()
{
  static const bool flip = []() {
    const char *env = std::getenv("FALCON_VSE_FLIP_CHANNELS");
    return env != nullptr && env[0] != '\0' && env[0] != '0';
  }();
  return flip;
}

/**
 * Convert a strip channel number to the View2D Y coordinate used to draw/pick it.
 * This is the single place that defines how channels map onto the timeline's vertical
 * axis.
 *
 * When #channel_flip_enabled is false (default), this is a pass-through
 * (`channel_to_y(c) == float(c)`), matching the historical layout.
 *
 * When enabled, the mapping folds back into the positive range as
 * `MAX_CHANNELS + 1 - channel` rather than negating it (`-channel`). A negated Y would go
 * negative, which collides with several places elsewhere in the sequencer/transform code that
 * treat a non-positive Y as a special value or clamp bound: the snap tool's `all_channels == 0`
 * sentinel, the `1 - strip->channel` / `MAX_CHANNELS - strip->channel` offset clamps in
 * `transform_convert_sequencer.cc`, and the `int()` vs `floorf()` mismatch that only differs for
 * negative inputs. Folding into `[1, MAX_CHANNELS]` avoids all three by construction, since the
 * result is always in the same positive range the rest of the code already assumes.
 *
 * \note Callers typically still add `STRIP_OFSBOTTOM`/`STRIP_OFSTOP` on top of this to get the
 * drawn top/bottom of a strip's body within its channel row. Those offsets don't need to change
 * for the flip: they describe where within a row's `[y, y + 1)` span a strip's box sits, and
 * that span keeps the same shape regardless of which channel it belongs to.
 */
inline float channel_to_y(int channel)
{
  if (channel_flip_enabled()) {
    return float(MAX_CHANNELS + 1 - channel);
  }
  return float(channel);
}

/**
 * Convert a View2D Y coordinate (timeline space) to the strip channel number occupying it.
 * Inverse of #channel_to_y. Currently `int(floorf(y))` (or its flipped counterpart), matching
 * the channel's "row" of `[channel, channel + 1)` in view space.
 */
inline int y_to_channel(float y)
{
  if (channel_flip_enabled()) {
    return MAX_CHANNELS + 1 - int(std::floor(y));
  }
  return int(std::floor(y));
}

/**
 * Like #y_to_channel, but rounds to the nearest channel row instead of flooring into whichever
 * row contains `y`. Used while dragging strips, where "nearest" gives a more natural drop point
 * than "the row that currently contains this Y" would.
 */
inline int y_to_channel_round(float y)
{
  if (channel_flip_enabled()) {
    return MAX_CHANNELS + 1 - round_fl_to_int(y);
  }
  return round_fl_to_int(y);
}

/**
 * Move strips and markers (if not locked) that start after timeline_frame by delta frames
 *
 * \param scene: Scene in which strips are located
 * \param seqbase: List in which strips are located
 * \param delta: offset in frames to be applied
 * \param timeline_frame: frame on timeline from where strips are moved
 */
void transform_offset_after_frame(Scene *scene,
                                  ListBaseT<Strip> *seqbase,
                                  int delta,
                                  int timeline_frame);

/**
 * Check if `strip` can be moved.
 * This function also checks `SeqTimelineChannel` flag.
 */
bool transform_is_locked(const ListBaseT<SeqTimelineChannel> *channels, const Strip *strip);

/* Image transformation. */

/**
 * Get per-axis mirror factors for a \a strip image.
 * \return float2 where each component is 1.0f (normal) or -1.0f (mirrored). */
float2 image_transform_mirror_factor_get(const Strip *strip);

/**
 * Get the \a strip origin as a fraction of its rendered image. This origin can be anywhere, but
 * (0,0) corresponds to the bottom left of the image, and (1,1) the top right.
 *
 * NOTE: #StripTransform::origin is stored relative to the strip box
 * (#image_transform_box_size_get), which for text strips is smaller than their rendered image.
 * This function properly converts it to be relative to the rendered image for the render pipeline
 * to use. Being a fraction, it is independent of proxy render size.
 */
float2 image_transform_origin_get(const Scene *scene, const Strip *strip);

/**
 * Get the \a strip origin's offset in view-space pixels from the preview's center, including axis
 * mirror and viewport pixel aspect.
 */
float2 image_transform_origin_preview_offset_get(const Scene *scene, const Strip *strip);

/**
 * Get \a strip image transformation matrix relative to its origin in view-space, including axis
 * mirror and viewport pixel aspect.
 */
float3x3 image_transform_matrix_get(const Scene *scene, const Strip *strip);

/**
 * Get the size of the drawn \a strip quad before any cropping, scaling, or transformation.
 * This is the size of the rendered `ImBuf` for every type but text strips, where it is the tighter
 * bounding box of the text glyphs.
 *
 * For the fully-processed quad, see #image_transform_quad_get.
 * For the bounding box of the quad, see #image_transform_bounding_box_from_strips_get.
 *
 * \return float2 with (width, height) in view-space pixels
 */
float2 image_transform_box_size_get(const Scene *scene, const Strip *strip);

/**
 * Get 4 corner points of strip image. Corner vectors are in viewport space.
 * Indices correspond to following corners (assuming no rotation):
 * 3--0
 * |  |
 * 2--1
 *
 * \param strip: Strip to calculate transformed image quad
 * \return array of four 2D points
 */
Array<float2> image_transform_quad_get(const Scene *scene, const Strip *strip);

float2 image_preview_unit_to_px(const Scene *scene, float2 co_src);
float2 image_preview_unit_from_px(const Scene *scene, float2 co_src);

/**
 * Get viewport axis aligned bounding box from multiple strips.
 * \param strips: Span of strips to calculate the bounding box for
 */
Bounds<float2> image_transform_bounding_box_from_strips_get(Scene *scene, Span<Strip *> strips);

}  // namespace seq
}  // namespace blender
