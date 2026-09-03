/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#pragma once

#include <cstdint>

#include "BLI_index_mask_fwd.hh"
#include "BLI_span.hh"
#include "BLI_vector.hh"

namespace blender {

struct Depsgraph;
struct Mesh;
struct Object;
struct Scene;
struct wmOperator;
namespace bke::pbvh {
class Node;
}

namespace ed::sculpt_paint::undo {

enum class Type : int8_t {
  None,
  Position,
  HideVert,
  HideFace,
  Mask,
  DyntopoBegin,
  DyntopoEnd,
  Geometry,
  FaceSet,
  Color,
};

struct StepData;

/**
 * Store undo data of the given type for a pbvh::Tree node. This function can be called by multiple
 * threads concurrently, as long as they don't pass the same pbvh::Tree node.
 *
 * This is only possible when building an undo step, in between #push_begin and #push_end.
 */
void push_node(const Depsgraph &depsgraph,
               const Object &object,
               const bke::pbvh::Node *node,
               undo::Type type);
void push_nodes(const Depsgraph &depsgraph,
                Object &object,
                const IndexMask &node_mask,
                undo::Type type);

/**
 * Pushes an undo step using the operator name. This is necessary for
 * redo panels to work; operators that do not support that may use
 * #push_begin_ex instead if so desired.
 */
void push_begin(const Scene &scene, Object &ob, const wmOperator *op);

/**
 * Pushes an undo step when entering Sculpt mode.
 *
 * Similar to geometry_push, this undo type does not need the PBVH to be constructed.
 */
void push_enter_sculpt_mode(const Scene &scene, Object &ob, const wmOperator *op);

/**
 * NOTE: #push_begin is preferred since `name`
 * must match operator name for redo panels to work.
 */
void push_begin_ex(const Scene &scene, Object &ob, const char *name);
void push_end(Object &ob);
void push_end_ex(Object &ob, bool use_nested_undo);

void restore_from_bmesh_enter_geometry(const StepData &step_data, Mesh &mesh);
bool has_bmesh_log_entry();

void restore_position_from_undo_step(const Depsgraph &depsgraph, Object &object);

namespace compression {

/**
 * Compress a span with ZSTD, using a prefiltering step that can improve compression speed and
 * ratios for certain data.
 */
template<typename T>
void filter_compress(const Span<T> src,
                     Vector<std::byte> &filter_buffer,
                     Vector<std::byte> &compress_buffer);

/** Decompress data compressed with #filter_compress. */
template<typename T>
void filter_decompress(const Span<std::byte> src, Vector<std::byte> &buffer, Vector<T> &dst);

}  // namespace compression

/**
 * ★2026-08-25: undo が「1ダブごと」でなく「ストローク中1ノードにつき1回」であることを
 * 数字で確かめるための計数。
 *
 * ブラシ全体をGPUへ移した後、undo のために毎ダブ読み戻すと消したはずの費用が全部戻る
 * (読み戻し 1.40〜8.48ms/ダブ)。だが `ensure_node` はハッシュに初回だけ足すので、
 * 実際に位置を複製するのは**新しく触れたノードだけ**のはず。
 * それが本当なら、GPU常駐でも「初回だけ GPU->GPU で控えを取る」で済み、
 * **毎ダブの遅延は増えない。**推測で決めずに測る。
 *
 * 既定でも数える(1ダブあたり数回の atomic なので費用は無視できる)。
 */
namespace falcon_stats {

struct UndoCounters {
  int64_t push_calls;      /* push_nodes が呼ばれた回数(≒ダブ数) */
  int64_t calls_with_new;  /* そのうち新しいノードが1つ以上出た回数 */
  int64_t new_nodes;       /* 新しく控えを取ったノードの総数 */
  int64_t new_verts;       /* その控えに入った頂点の総数(=複製した量) */
  double fill_seconds;     /* 控えを取るのに掛かった時間の合計 */
};

/** 現在の計数を取る。 */
UndoCounters get_counters();
/** 0 に戻す。ストロークの区切りで呼ぶ。 */
void reset_counters();

}  // namespace falcon_stats

}  // namespace ed::sculpt_paint::undo

}  // namespace blender
