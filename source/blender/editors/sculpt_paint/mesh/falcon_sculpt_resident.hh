/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * ★ブラシ本体をGPUで回す(頂点をGPUに常駐させる形)。
 *
 * ひとつ前の形([[falcon_sculpt_gpu.hh]] = 係数だけGPU)は**実測で負けた**。
 * 400万面・1ダブで CPU 71.7ms に対し、転送も読み戻しも消した probe ですら 110.9ms。
 * 負けていたのは転送ではなく、GPUの周りに残った CPU の世話
 * (ノードごとの番号の集め直し・係数の適用・書き戻し)だった。
 *
 * ここではその3つを GPU へ移す:
 *
 *  - **番号の集め直しをやめる**。ノードの頂点表(`node_verts` / `node_offsets`)を
 *    ストロークの初めに1回だけ送る。1ダブで送るのは**触ったノードの番号だけ**
 *    (数百 int = 数KB)。スレッドは触ったノードの前置和を二分探索して自分の頂点を引く。
 *  - **係数の適用と書き戻しを GPU で**。位置は書き換えたまま常駐する。
 *  - CPU へ返すのは、触った頂点ぶんを詰めた1本だけ(この段ではまだ毎ダブ返す。
 *    法線・境界・undo を GPU へ移した段で返さなくなる)。
 *
 * 実測の内訳(400万面・1ダブ 22.43ms): カーネル 20.49ms(91.3%)/ 法線 0.65 /
 * undo積み 0.38 / ノード探索 0.06 / 後始末 0.01 = **CPU に残る床は 0.45ms**。
 *
 * `FALCON_SCULPT_GPU=resident` の時だけ通る。**既定はCPU(公式通路)。**
 * `FALCON_SCULPT_GPU=resident-verify` は常駐側を書き換えず、CPU の通路を
 * そのまま走らせて差だけを測る(**絵は1画素も変わらない**)。
 */

#include "BLI_index_mask.hh"
#include "BLI_math_vector_types.hh"

/* ★DNA の型は 5.x で `namespace blender` の中に入った。前方宣言を大域に置くと
 * **別の型**を宣言することになり、.cc 側の定義と一致しない(実害は
 * -Wmissing-declarations だけだが、宣言が黙って死ぬので直しておく)。 */
namespace blender {
struct Brush;
struct Depsgraph;
struct Object;
struct Paint;
struct Sculpt;
}  // namespace blender

namespace blender::ed::sculpt_paint::falcon_resident {

/** `FALCON_SCULPT_GPU=resident` 系が立っているか。 */
bool enabled();

/** `resident-verify` か(GPUは走るが位置は CPU の値のまま)。 */
bool verify_mode();

/**
 * offset 系ブラシ(Draw / Nudge / Gravity)の1ダブをまるごと GPU で回す。
 *
 * \return true = 位置は GPU が書き換え、CPU 側の配列にも戻した。呼び手は
 *         ノードの境界更新だけ行えばよい。false = 何もしていないので CPU の通路へ。
 *
 * 通らない条件は素直に false を返す。**推測で通すと絵が静かに変わる**
 * (係数は掛け算なので、抜けた項目のぶんだけ強くなるだけで、破綻して見えない)。
 */
bool offset_positions(const Depsgraph &depsgraph,
                      const Sculpt &sd,
                      Object &object,
                      const Brush &brush,
                      const float3 &offset,
                      const IndexMask &node_mask);

/**
 * `resident-verify` の時に、CPU の通路が走り終わった後で突き合わせる。
 * GPU は位置を書き換えていないので、**絵は1画素も変わらないまま差だけが出る**。
 * 「GPUに切り替えて絵を見比べる」だと差が小さい時に見落とす。
 */
void note_after_cpu(const Depsgraph &depsgraph, const Object &object, const IndexMask &node_mask);

/** ストロークの終わりに1回。内訳を刷って0に戻す。 */
void report_breakdown();

/** 常駐している物を捨てる(メッシュが変わった・ストロークが終わった)。 */
void invalidate();

}  // namespace blender::ed::sculpt_paint::falcon_resident
