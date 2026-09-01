/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup sequencer
 *
 * Deciding whether a timeline is a plain set of cuts, and if it is, saying which
 * part of which source file each cut takes.
 *
 * This is the half of the "fast path" that has to know about strips. The half that
 * writes the packets lives in `imbuf/movie` and never sees a strip -- it is handed
 * the list this file produces.
 */

#include "BLI_string_ref.hh"
#include "BLI_vector.hh"

#include "DNA_scene_types.h"

namespace blender::seq {

/** One run of frames taken straight out of one source file. */
struct FastPathCut {
  /** Absolute path of the movie the frames come from. */
  char path[1024] = "";
  /** First frame used, counted from the start of the source. */
  int in_frame = 0;
  int n_frames = 0;
};

/**
 * Collect the cuts if this timeline is nothing but cuts of movie files.
 *
 * Returns false when it is not, with one line in \a r_reason saying which condition
 * was not met. Being told "no" is the normal case, not an error: the caller renders
 * the timeline the usual way.
 *
 * ★ A timeline that carries sound is refused here. The writer has no audio, and a
 * silent export is worse than a slow one -- the user would not find out until the
 * file was finished.
 */
bool fastpath_cuts_get(const Scene *scene,
                       const RenderData *rd,
                       Vector<FastPathCut> &r_cuts,
                       char *r_reason,
                       int reason_maxncpy);

}  // namespace blender::seq
