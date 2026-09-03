/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup imbuf
 *
 * Fast path -- writing a "cut only" edit without decoding and re-encoding it.
 *
 * When the frames the sequencer would hand the encoder are the frames already
 * sitting in the source file, decoding and re-encoding them spends a lot of time
 * arriving back where it started. Copy the compressed packets instead.
 *
 * ★ Cut points almost never land on a keyframe (measured on one 30fps source: the
 * keyframe interval is 2.00s median, so about 2% of cut points do). A copy can only
 * start at a keyframe, so each cut is split into "the partial group of pictures at
 * each end, re-encoded" plus "the whole groups in between, copied".
 *
 * ★ Both ends have to be re-encoded, not just the head. B-frames reference frames
 * that come later in display order, so a copy that stops in the middle of a group
 * loses what its last frames point at. (Measured: source frame 248 was followed by
 * 252 because `-frames:v` counts packets in decode order, not display order.)
 *
 * This half knows nothing about strips; `SEQ_fastpath.hh` decides what the cuts are.
 */

#include "BLI_span.hh"

namespace blender {

struct ImageFormatData;
struct RenderData;
struct ReportList;
struct Scene;

/** One run of frames taken straight out of one source file. */
struct MovieFastPathCut {
  const char *path = nullptr;
  int in_frame = 0;
  int n_frames = 0;
};

/** What the fast path did, for the caller to report. */
struct MovieFastPathReport {
  int total_frames = 0;
  int copied_frames = 0;
  int reencoded_frames = 0;
  int pieces = 0;
};

/**
 * Write \a cuts to the movie file the render settings name, copying packets.
 *
 * Returns false when the sources or the output settings do not allow a straight
 * copy, with one line in \a r_reason. Being told "no" is the normal case, not an
 * error: the caller renders the timeline the usual way.
 *
 * \note Never leaves a partial file behind: a failure part way through removes it.
 */
bool MOV_fastpath_write(const Scene *scene,
                        const RenderData *rd,
                        const ImageFormatData *imf,
                        Span<MovieFastPathCut> cuts,
                        ReportList *reports,
                        MovieFastPathReport *r_report,
                        char *r_reason,
                        int reason_maxncpy);

}  // namespace blender
