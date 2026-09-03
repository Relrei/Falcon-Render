/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup edsculpt
 *
 * ブラシの係数をGPUで出す。
 *
 * 実測(実データ52万面・Clay)で、1ダブ 10.3ms のうち **98% がカーネル本体**。
 * ノード探索 0.4% / undo積み 1.2% / 法線 0.1% / 描画バッファ 0.3%。
 * 触る頂点は1ダブあたり 14〜16万で、1頂点あたり 49〜105ns。
 *
 * ★出させるのは**係数だけ**。移動量・対称のクリップ・変形モディファイア経由の
 * 書き戻しはCPU側の既存コードに任せる。対応できない条件はCPUへ落とすだけで済み、
 * 書き戻しの経路を作り直さないので既存の動作を壊す面が小さい。
 *
 * `FALCON_SCULPT_GPU=1` の時だけ通る。**既定はCPU(公式通路)。**
 */

#include "BLI_index_mask.hh"
#include "BLI_vector.hh"

/* ★DNA の型は 5.x で `namespace blender` の中に入っている(下の宣言を大域に
 * 置くと別の型になり、.cc の定義と一致しない)。 */
namespace blender {
struct Brush;
struct Depsgraph;
struct Object;
struct Paint;
}  // namespace blender

namespace blender::ed::sculpt_paint::falcon_gpu {

/**
 * GPU で係数を出せる条件かどうか。
 *
 * 通らない条件は素直に false を返す。**推測で通してしまうと、絵が静かに変わる**
 * (係数は掛け算なので、抜けた項目のぶんだけ強くなるだけで、破綻して見えない)。
 */
bool can_use_factors(const Depsgraph &depsgraph,
                     const Paint &paint,
                     const Object &object,
                     const Brush &brush);

/**
 * `node_mask` の全ノード分の係数をまとめて1回のディスパッチで出す。
 *
 * ノードごとに投げるとディスパッチの回数がそのまま時間になる(Taichi の実測で
 * 同じことが起きた: GPU 0.9ms に対し壁時計 17.9ms、律速はカーネル発行回数)。
 *
 * \param r_offsets: ノードごとの先頭位置。要素数は node_mask の数 + 1。
 * \param r_factors: 連結された係数。
 * \return 出せたら true。false ならCPUへ落とす。
 */
bool calc_factors(const Depsgraph &depsgraph,
                  const Object &object,
                  const Brush &brush,
                  const IndexMask &node_mask,
                  Vector<int> &r_offsets,
                  Vector<float> &r_factors);

/** `FALCON_SCULPT_GPU` が立っているか(1回だけ環境変数を引く)。 */
bool enabled();

/**
 * `FALCON_SCULPT_GPU=verify` か。
 *
 * この時は**両方で出して突き合わせ、CPU の値を使う**。絵は1画素も変わらないまま
 * 差だけが分かるので、確認と実運用を同じビルドで回せる。
 * 「GPUに切り替えて絵を見比べる」だと、差が小さい時に見落とす。
 */
bool verify_mode();

/** 突き合わせの差を1つ記録する(スレッドから呼ばれる)。 */
void note_diff(float diff);

/** 溜めた差を出して0に戻す。ストロークの終わりに1回。 */
void report_verify();

/** GPU側の内訳(転送・実行・読み戻し)を出す。ストロークの終わりに1回。 */
void report_gpu_breakdown();

}  // namespace blender::ed::sculpt_paint::falcon_gpu
