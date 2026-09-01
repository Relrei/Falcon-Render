/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup sequencer
 */

#include <algorithm>
#include <cstdlib>

#include "BLI_listbase.h"
#include "BLI_path_utils.hh"
#include "BLI_string.h"

#include "BKE_main.hh"

#include "DNA_scene_types.h"
#include "DNA_sequence_types.h"

#include "SEQ_fastpath.hh"
#include "SEQ_sequencer.hh"

namespace blender::seq {

#define FAIL(...) \
  do { \
    BLI_snprintf(r_reason, reason_maxncpy, __VA_ARGS__); \
    return false; \
  } while (false)

/** 1本のストリップが「切っただけ」か。
 *
 * 判定の順は「直しやすい物から」。最初に当たった1件だけを理由として返す。
 */
static bool strip_is_plain_cut(const Strip *strip, char *r_reason, int reason_maxncpy)
{
  const char *n = strip->name + 2;

  if (strip->type != STRIP_TYPE_MOVIE) {
    FAIL("Strip '%s' is not a movie strip", n);
  }
  if (!BLI_listbase_is_empty(&strip->modifiers)) {
    FAIL("Strip '%s' has a modifier on it", n);
  }
  if (strip->data == nullptr || strip->data->stripdata == nullptr) {
    FAIL("Strip '%s' has no source file", n);
  }
  if (const StripTransform *tr = strip->data->transform) {
    if (tr->xofs != 0.0f || tr->yofs != 0.0f) {
      FAIL("Strip '%s' is moved", n);
    }
    if (tr->scale_x != 1.0f || tr->scale_y != 1.0f) {
      FAIL("Strip '%s' is scaled", n);
    }
    if (tr->rotation != 0.0f) {
      FAIL("Strip '%s' is rotated", n);
    }
  }
  if (const StripCrop *cr = strip->data->crop) {
    if (cr->top || cr->bottom || cr->left || cr->right) {
      FAIL("Strip '%s' is cropped", n);
    }
  }
  if (strip->sat != 1.0f || strip->mul != 1.0f) {
    FAIL("Strip '%s' has a colour change on it", n);
  }
  /* ⚠ 動画ストリップの既定は REPLACE ではなく **ALPHA_OVER**(実機で確認)。
   * 不透明な素材を全開で乗せるのは REPLACE と同じ絵になるので、両方を通す。
   * ★ここを REPLACE だけにすると**素の動画ストリップが全部落ちて、機能が一度も
   * 発動しない**。`strobe` の既定が 0.0 で同じ穴を踏んだのと同じ形。 */
  if (!ELEM(strip->blend_mode, STRIP_BLEND_REPLACE, STRIP_BLEND_ALPHAOVER)) {
    FAIL("Strip '%s' is blended into what is under it", n);
  }
  if (strip->blend_opacity != 100.0f) {
    FAIL("Strip '%s' is not fully opaque (opacity %.0f%%)", n, strip->blend_opacity);
  }
  if (strip->flag & (SEQ_FLIPX | SEQ_FLIPY)) {
    FAIL("Strip '%s' is flipped", n);
  }
  if (strip->flag & SEQ_REVERSE_FRAMES) {
    FAIL("Strip '%s' plays in reverse", n);
  }
  if (strip->flag & SEQ_USE_PROXY) {
    FAIL("Strip '%s' renders from a proxy, not the original", n);
  }
  if (strip->flag & SEQ_MULTIPLY_ALPHA) {
    FAIL("Strip '%s' multiplies alpha", n);
  }
  /* ⚠ `strobe` の既定は 0.0 で、1.0 ではない。どちらも「毎コマ出す」の意味。
   * `!= 1.0f` で弾くと**素の動画ストリップが全部落ちる**(アドオン側で実測済み)。 */
  if (strip->strobe != 0.0f && strip->strobe != 1.0f) {
    FAIL("Strip '%s' uses strobe", n);
  }
  if (strip->retiming_keys_num > 2) {
    FAIL("Strip '%s' is retimed", n);
  }
  if (strip->speed_factor != 0.0f && strip->speed_factor != 1.0f) {
    FAIL("Strip '%s' plays at a changed speed", n);
  }
  return true;
}

/** 戻す口。`FALCON_VSE_FASTPATH=0` で切ると、従来どおり全部を描き直す。
 *
 * ★既定は ON。切れるようにしてあるのは、**壊れた時に本人が1つの環境変数で
 * 元の挙動へ戻せる**ようにするため(絵が変わったように見える時の切り分けにも使う)。 */
static bool fastpath_enabled()
{
  static const bool enabled = [] {
    const char *env = getenv("FALCON_VSE_FASTPATH");
    if (env == nullptr || env[0] == '\0') {
      return true;
    }
    return atoi(env) != 0;
  }();
  return enabled;
}

bool fastpath_cuts_get(const Scene *scene,
                       const RenderData *rd,
                       Vector<FastPathCut> &r_cuts,
                       char *r_reason,
                       int reason_maxncpy)
{
  r_reason[0] = '\0';

  if (!fastpath_enabled()) {
    FAIL("Switched off with FALCON_VSE_FASTPATH=0");
  }

  const Editing *ed = editing_get(scene);
  if (ed == nullptr) {
    FAIL("This scene has no sequencer");
  }
  if ((rd->scemode & R_DOSEQ) == 0) {
    FAIL("The sequencer is switched off for this scene");
  }
  if (rd->size != 100) {
    FAIL("Output resolution is %d%%; the fast path needs 100%%", rd->size);
  }
  if (rd->scemode & R_MULTIVIEW) {
    FAIL("Multi-view output cannot be copied straight through");
  }

  Vector<const Strip *> strips;
  for (const Strip &strip : ed->seqbase) {
    if (strip.flag & SEQ_MUTE) {
      continue;
    }
    /* ★音のストリップは素通しする。**音は運ばずに、Blender の普通のミックス
     * ダウンにそのまま作らせる**(fast path が省くのは映像の復号と符号化だけ)。
     * こうすると音量もフェードも普通に効くので、判定で縛る必要がない。 */
    if (ELEM(strip.type, STRIP_TYPE_SOUND, STRIP_TYPE_SOUND_HD)) {
      continue;
    }
    if (!strip_is_plain_cut(&strip, r_reason, reason_maxncpy)) {
      return false;
    }
    strips.append(&strip);
  }
  if (strips.is_empty()) {
    FAIL("The timeline has no unmuted strip");
  }

  std::sort(strips.begin(), strips.end(), [](const Strip *a, const Strip *b) {
    return a->left_handle() < b->left_handle();
  });

  /* 重なっていると合成が要るので、そのままは流せない。 */
  for (int i = 1; i < strips.size(); i++) {
    if (strips[i]->left_handle() < strips[i - 1]->right_handle(scene)) {
      FAIL("Strips '%s' and '%s' overlap in time; that needs compositing",
           strips[i - 1]->name + 2,
           strips[i]->name + 2);
    }
  }

  /* ★書くのは**レンダー範囲**であって、タイムライン全体ではない。
   *
   * ここを見ていないと、開始/終了を絞って書き出したのに**タイムラインの全部**が
   * 出る。しかも本数が違うだけで絵は正しいので、書き出しが終わるまで気づけない。 */
  const int range_start = rd->sfra;
  const int range_end = rd->efra + 1; /* 終端は開いた区間で持つ。 */
  if (range_end <= range_start) {
    FAIL("The render range is empty");
  }

  const char *blendfile_path = BKE_main_blendfile_path_from_global();
  int covered_until = range_start;
  for (const Strip *strip : strips) {
    /* ⚠ `start` は「素材の0コマ目が来るタイムライン位置」であって、ストリップの
     * 左端ではない。取り違えると切り出す位置が丸ごとずれる(絵は出るので
     * 気づきにくい)。左端は `left_handle()`。 */
    const int left = strip->left_handle();
    const int right = strip->right_handle(scene);
    const int from = std::max(left, range_start);
    const int to = std::min(right, range_end);
    if (to <= from) {
      continue; /* レンダー範囲の外。 */
    }
    /* ★範囲の中に隙間があってはいけない。通常の書き出しは隙間を黒で埋めるが、
     * こちらは詰めて繋いでしまうので、**尺は同じで中身がずれた動画**になる。 */
    if (from > covered_until) {
      FAIL("There is a gap in the timeline at frame %d; the normal render fills it", covered_until);
    }
    covered_until = to;

    FastPathCut cut;
    cut.in_frame = std::max(0, int(lround(from - strip->start)));
    cut.n_frames = to - from;
    BLI_path_join(
        cut.path, sizeof(cut.path), strip->data->dirpath, strip->data->stripdata->filename);
    BLI_path_abs(cut.path, blendfile_path);
    r_cuts.append(cut);
  }
  if (r_cuts.is_empty()) {
    FAIL("The render range has no strip in it");
  }
  if (covered_until < range_end) {
    FAIL("The timeline stops at frame %d but the render range goes to %d",
         covered_until,
         range_end - 1);
  }
  return true;
}

#undef FAIL

}  // namespace blender::seq
