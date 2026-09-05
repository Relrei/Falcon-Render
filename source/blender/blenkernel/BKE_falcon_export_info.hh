/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

/** \file
 * \ingroup bke
 *
 * 直近の書き出しで「何が効いたか」を1行に溜める所。
 *
 * ★決めているのは別々の層 -- 切っただけの経路を採ったかは `render/`、
 *   符号化を GPU でやったかは `imbuf/movie/` が決める。どちらも UI を知らない。
 *   ここに置いた文字列を RNA (`RenderSettings.falcon_last_export_info`) が読み、
 *   出力プロパティに1行出す。**保存はしない**(実行時だけの値)。
 */

#include <string>

namespace blender {

struct Scene;

namespace bke {

/** 書き出しの始め。前の回の値を捨てる。 */
void falcon_export_info_begin(const Scene *scene);

/** 切っただけの経路を採ったか。`detail` は採った時は "3 cuts"、外れた時は理由。 */
void falcon_export_info_set_fastpath(const Scene *scene, bool used, const char *detail);

/** 符号化器。`codec_name` は FFmpeg の名前("h264_nvenc" / "libx264" など)。 */
void falcon_export_info_set_encoder(const Scene *scene, bool hardware, const char *codec_name);

/** 書き出しの終わり。掛かった秒。 */
void falcon_export_info_end(const Scene *scene, double seconds);

/** 1行にして返す。まだ1回も書き出していなければ空。 */
std::string falcon_export_info_get(const Scene *scene);

}  // namespace bke

}  // namespace blender
