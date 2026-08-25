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
