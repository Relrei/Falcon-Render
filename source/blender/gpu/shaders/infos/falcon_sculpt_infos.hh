/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/* ★スカルプトのブラシ係数をGPUで出す(2026-08-16 追加)。
 *
 * 実測(実データ52万面・Clay)で、1ダブ 10.3ms のうち **98% がカーネル本体**だった。
 * ノード探索 0.4% / undo積み 1.2% / 法線 0.1% / 描画バッファ 0.3% なので、
 * 縮められるのはここだけ。触る頂点は1ダブあたり 14〜16万で、1頂点あたり 49〜105ns。
 *
 * ここでGPUに出させるのは **係数(factor)だけ**。移動量の計算・対称のクリップ・
 * 変形モディファイア経由の書き戻しは CPU 側の既存コードにそのまま任せる。
 * そうすると:
 *   - 対応できない条件(マスク・自動マスク・テクスチャ等)は**CPUへ落とすだけ**で済む
 *   - 書き戻しの経路を作り直さないので、既存の動作を壊す面が小さい
 *
 * 落とし方の曲線(`BKE_brush_calc_curve_factors`)は、プリセットにも自作カーブにも
 * なるので、**CPU側で一度だけ表に焼いてから渡す**。GPU側で分岐を再現しない。
 */

#include "gpu_shader_create_info.hh"

GPU_SHADER_CREATE_INFO(falcon_sculpt_factors)
LOCAL_GROUP_SIZE(256, 1)
/* 触る頂点の番号(全ノード分を1本に連結したもの) */
STORAGE_BUF(0, read, int, vert_indices[])
/* メッシュ全体の位置と法線(頂点番号で引く) */
STORAGE_BUF(1, read, float, positions[])
STORAGE_BUF(2, read, float, normals[])
/* 落とし方の曲線を焼いた表。0..1 の距離比で引く */
STORAGE_BUF(3, read, float, curve_lut[])
/* スカルプトのマスク(頂点番号で引く)。無い時もダミーを1つ結ぶ。
 * ★「マスクが無い時はCPUへ落とす」にはできない。一度でもマスクを使った
 * メッシュは、全部消した後も値が全部ゼロのまま `.sculpt_mask` を持ち続けるので、
 * 実データはほぼ必ずこれを持っている = GPU側が一度も通らなくなる。 */
STORAGE_BUF(4, read, float, vert_mask[])
/* 出力 = 係数 */
STORAGE_BUF(5, write, float, factors[])
PUSH_CONSTANT(float3, brush_location)
PUSH_CONSTANT(float3, view_normal)
PUSH_CONSTANT(float, brush_radius)
PUSH_CONSTANT(float, brush_hardness)
PUSH_CONSTANT(int, vert_count)
PUSH_CONSTANT(int, curve_lut_size)
PUSH_CONSTANT(bool, use_front_face)
PUSH_CONSTANT(bool, use_mask)
COMPUTE_SOURCE("falcon_sculpt_factors_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()

/* ★ブラシ本体をGPUで回す(2026-08-25 追加)。上の `falcon_sculpt_factors` の次の段。
 *
 * 係数だけを出して CPU へ返す形は実測で負けた(400万面・1ダブ:
 * CPU 71.7ms に対し、転送も読み戻しも消した probe ですら 110.9ms)。
 * 負けた理由は転送ではなく、**GPUの周りに残った CPU の世話**
 * (ノードごとの番号の集め直し・係数の適用・書き戻し)だった。
 *
 * ここではその3つも GPU へ移す:
 *  - ノードの頂点表(`node_verts`)は**ストロークの初めに1回だけ**送る。
 *    1ダブで送るのは触ったノードの位置(`node_starts` / `touched_offsets`)だけ = 数KB。
 *  - スレッドは `touched_offsets` を二分探索して自分のノードを見つけ、
 *    `node_verts[node_starts[j] + local]` で頂点番号を引く。
 *  - 位置は**書き換えて常駐したまま**。CPU へ返すのは詰めた1本(`out_positions`)。
 *
 * 内訳の実測 = 1ダブ 22.43ms のうちカーネル 20.49ms(91.3%)。
 * CPU に残るのはノード探索 0.06ms + 後始末 0.01ms = **床 0.45ms**。
 *
 * 係数の出し方は `falcon_sculpt_factors_comp.glsl` と**同じ順番**でなければならない
 * (硬さは距離を書き換えるので曲線より前)。突き合わせ済みの式をそのまま持ってくる。 */
GPU_SHADER_CREATE_INFO(falcon_sculpt_offset)
LOCAL_GROUP_SIZE(256, 1)
/* ★常駐: 全ノードの頂点番号をノード順に連結したもの。
 * `MeshNode::verts()` はノード間で重ならないので、番号は重複しない
 * = 同じ場所へ2回書かない(並列で書いても競合しない)。 */
STORAGE_BUF(0, read, int, node_verts[])
/* ★常駐: 位置。**ここを書き換える**(読み書き) */
STORAGE_BUF(1, read_write, float, positions[])
/* 触った頂点ぶんに詰めた法線(添字は連番 i)。表向きの面だけに絞る時だけ中身が入る */
STORAGE_BUF(2, read, float, normals[])
/* 落とし方の曲線を焼いた表 */
STORAGE_BUF(3, read, float, curve_lut[])
/* ★常駐: スカルプトのマスク(添字は頂点番号)。無い時もダミーを1つ結ぶ */
STORAGE_BUF(4, read, float, vert_mask[])
/* 触った頂点ぶんに詰めた**移動量**(新しい位置ではない)。
 * 変形モディファイアがあると、評価後の位置と元の位置で足す物が違う
 * (元の位置には crazyspace の逆行列を掛ける)ので、移動量で返す。 */
STORAGE_BUF(5, write, float, out_translations[])
/* 触ったノード j の、`node_verts` の中での開始位置 */
STORAGE_BUF(6, read, int, node_starts[])
/* 触ったノードの前置和(要素数 = 触ったノード数 + 1) */
STORAGE_BUF(7, read, int, touched_offsets[])
PUSH_CONSTANT(float3, brush_location)
PUSH_CONSTANT(float3, view_normal)
/* 移動の向き×強さ。CPU 側 `offset_positions` の `offset` そのもの */
PUSH_CONSTANT(float3, brush_offset)
/* 軸の固定。自由なら 1.0・固定なら 0.0(`SCULPT_LOCK_X` 等) */
PUSH_CONSTANT(float3, axis_scale)
PUSH_CONSTANT(float, brush_radius)
PUSH_CONSTANT(float, brush_hardness)
PUSH_CONSTANT(int, vert_count)
PUSH_CONSTANT(int, curve_lut_size)
PUSH_CONSTANT(int, node_count)
PUSH_CONSTANT(bool, use_front_face)
PUSH_CONSTANT(bool, use_mask)
/* ★突き合わせの時は false。常駐側を書き換えず、詰めた結果だけ出す。
 * こうすると CPU の通路をそのまま走らせて、絵を1画素も変えずに差が測れる。 */
PUSH_CONSTANT(bool, write_inplace)
COMPUTE_SOURCE("falcon_sculpt_offset_comp.glsl")
DO_STATIC_COMPILATION()
GPU_SHADER_CREATE_END()
