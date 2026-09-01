/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup imbuf
 *
 * See MOV_fastpath.hh for what this is and why both ends of every cut have to be
 * re-encoded. This file is:
 *
 *   1. `source_open` / `source_keyframes` -- what the source file is and where its
 *      groups of pictures begin
 *   2. `plan_pieces`                      -- split a cut into copy / re-encode runs
 *   3. `MOV_fastpath_write`               -- run the pieces through one muxer
 */

#include "MOV_fastpath.hh"

#ifdef WITH_FFMPEG

#  include <algorithm>
#  include <cmath>

#  include "BLI_fileops.h"
#  include "BLI_math_base.h"
#  include "BLI_path_utils.hh"
#  include "BLI_string.h"
#  include "BLI_vector.hh"

#  include "DNA_scene_types.h"

#  include "BKE_image_format.hh"
#  include "BKE_report.hh"

#  include "CLG_log.h"

#  include "MOV_write.hh"

#  include "movie_util.hh"
#  include "movie_write.hh"

static CLG_LogRef LOG = {"image.movie.fastpath"};

namespace blender {

/** fps の差をどこまで同じとみなすか。素材側に 30000/1001 のような丸めがある。 */
static const double FPS_TOLERANCE = 1e-3;

/** 焼き直す区間の復号を、頭より何コマ手前から始めるか(上の理由)。 */
static const int FASTPATH_SEEK_MARGIN = 64;

#  define FAIL(...) \
    do { \
      BLI_snprintf(r_reason, reason_maxncpy, __VA_ARGS__); \
      return false; \
    } while (false)

/* -------------------------------------------------------------------- */
/** \name 素材の素性
 * \{ */

struct SourceInfo {
  AVFormatContext *fmt = nullptr;
  int stream_index = -1;
  const AVStream *stream = nullptr;
  double fps = 0.0;
  /** ★コマ N の表示時刻は `start_time + N/fps`。落とすと丸ごと1コマずれる
   * (実測 0.033 秒 = ほぼ1コマ。同じコマが2回出て末尾が1コマ落ちた)。 */
  double start_time = 0.0;
};

static void source_close(SourceInfo *src)
{
  if (src->fmt != nullptr) {
    avformat_close_input(&src->fmt);
    src->fmt = nullptr;
    src->stream = nullptr;
  }
}

static bool source_open(const char *path, SourceInfo *src, char *r_reason, int reason_maxncpy)
{
  if (avformat_open_input(&src->fmt, path, nullptr, nullptr) < 0) {
    FAIL("Cannot open '%s'", BLI_path_basename(path));
  }
  if (avformat_find_stream_info(src->fmt, nullptr) < 0) {
    source_close(src);
    FAIL("Cannot read the streams of '%s'", BLI_path_basename(path));
  }
  src->stream_index = av_find_best_stream(src->fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  if (src->stream_index < 0) {
    source_close(src);
    FAIL("'%s' has no video stream", BLI_path_basename(path));
  }
  src->stream = src->fmt->streams[src->stream_index];

  const AVRational rate = src->stream->avg_frame_rate.num ? src->stream->avg_frame_rate :
                                                            src->stream->r_frame_rate;
  src->fps = (rate.den != 0) ? double(rate.num) / double(rate.den) : 0.0;
  if (src->fps <= 0.0) {
    source_close(src);
    FAIL("'%s' reports no frame rate", BLI_path_basename(path));
  }

  /* ★可変フレームレートの素材は入れない。
   *
   * ここから先は全部「コマ N の表示時刻は start_time + N/fps」で組み立てている。
   * 間隔が一定でない素材ではその式が成り立たず、**探索も突き合わせも静かに
   * ずれる**(落ちも警告も出ないまま、別のコマが並んだ動画ができる)。
   * 判定は「表に出ている最大レートと平均レートが食い違うか」。実測した VFR の
   * 素材は 30/1 に対し平均 386/15 = 25.73 だった。 */
  const AVRational raw = src->stream->r_frame_rate;
  if (raw.den != 0 && src->stream->avg_frame_rate.num != 0) {
    const double raw_fps = double(raw.num) / double(raw.den);
    if (fabs(raw_fps - src->fps) > 0.01) {
      source_close(src);
      FAIL("'%s' has a variable frame rate (%.3f vs %.3f fps), which cannot be copied "
           "frame for frame",
           BLI_path_basename(path),
           raw_fps,
           src->fps);
    }
  }
  /* ★色の素性(primaries / transfer / matrix)が既定と違う素材は入れない。
   *
   * 通常の書き出しは**色管理を通して描き直す**ので、HDR(bt2020 / PQ)の素材でも
   * **bt709 で出る**(実測)。こちらだけが素材の素性をそのまま運ぶと、
   * **同じ編集なのに色の解釈が違う動画**ができる。回転と同じ形。
   * 未指定(unspecified)は「特に主張していない」なので通す。 */
  {
    const AVCodecParameters *cp = src->stream->codecpar;
    const bool plain_primaries = ELEM(
        cp->color_primaries, AVCOL_PRI_UNSPECIFIED, AVCOL_PRI_BT709);
    const bool plain_trc = ELEM(cp->color_trc, AVCOL_TRC_UNSPECIFIED, AVCOL_TRC_BT709);
    const bool plain_space = ELEM(cp->color_space, AVCOL_SPC_UNSPECIFIED, AVCOL_SPC_BT709);
    if (!plain_primaries || !plain_trc || !plain_space) {
      source_close(src);
      FAIL("'%s' carries a colour space the normal render converts away (HDR or wide gamut)",
           BLI_path_basename(path));
    }
  }

  /* ★インターレースの素材は入れない。
   *
   * コピーした区間は飛び越し走査のまま、焼き直した区間は順次走査で出るので、
   * **1本の中で走査方式が混ざる**。しかも符号化器はそれを警告しない。 */
  const int field_order = src->stream->codecpar->field_order;
  if (!ELEM(field_order, AV_FIELD_UNKNOWN, AV_FIELD_PROGRESSIVE)) {
    source_close(src);
    FAIL("'%s' is interlaced, which cannot be mixed with re-encoded parts",
         BLI_path_basename(path));
  }

  /* ★回転メタデータの付いた素材は入れない(スマホ撮影で普通に来る)。
   *
   * 回転は画素でなく**メタデータ**で持つ。通常の書き出しは**回転を適用して
   * 描き直す**ので、パケットをそのまま流すのとは別の絵になる。実測: 同じ編集で
   * PSNR 18.1dB(= まったく違う絵)。運んでも落としても通常と一致しないので、
   * ここは降りるのが正しい。 */
  for (int i = 0; i < src->stream->codecpar->nb_coded_side_data; i++) {
    if (src->stream->codecpar->coded_side_data[i].type == AV_PKT_DATA_DISPLAYMATRIX) {
      source_close(src);
      FAIL("'%s' carries a rotation, which the normal render applies to the picture",
           BLI_path_basename(path));
    }
  }

  src->start_time = (src->stream->start_time == AV_NOPTS_VALUE) ?
                        0.0 :
                        double(src->stream->start_time) * av_q2d(src->stream->time_base);
  return true;
}

/** パケットの表示時刻を**コマ番号**へ直す。
 *
 * ⚠ 秒のまま突き合わせると丸めで転ぶので、拾った時点でコマ番号にする。
 */
static int source_frame_of(const SourceInfo &src, int64_t ts)
{
  if (ts == AV_NOPTS_VALUE) {
    return -1;
  }
  const double sec = double(ts) * av_q2d(src.stream->time_base);
  return int(lround((sec - src.start_time) * src.fps));
}

static int64_t source_seek_ts(const SourceInfo &src, int frame)
{
  const double sec = src.start_time + double(frame) / src.fps;
  return int64_t(std::llround(sec / av_q2d(src.stream->time_base)));
}

/** 範囲に掛かるキーフレームの位置(コマ番号)。 */
static bool source_keyframes(SourceInfo *src,
                             int from_frame,
                             int to_frame,
                             Vector<int> &r_keyframes,
                             char *r_reason,
                             int reason_maxncpy)
{
  /* 範囲の少し手前から読む。頭のキーフレームは範囲の外に在ることが多い。 */
  const int seek_frame = std::max(0, from_frame - 1);
  if (av_seek_frame(src->fmt, src->stream_index, source_seek_ts(*src, seek_frame), AVSEEK_FLAG_BACKWARD) < 0)
  {
    FAIL("Cannot seek in '%s'", BLI_path_basename(src->fmt->url));
  }

  AVPacket *pkt = av_packet_alloc();
  while (av_read_frame(src->fmt, pkt) >= 0) {
    if (pkt->stream_index == src->stream_index && (pkt->flags & AV_PKT_FLAG_KEY)) {
      const int frame = source_frame_of(*src, (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts);
      if (frame > to_frame) {
        av_packet_unref(pkt);
        break;
      }
      if (frame >= from_frame && (r_keyframes.is_empty() || r_keyframes.last() != frame)) {
        r_keyframes.append(frame);
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return true;
}

/** キーフレームの「後ろに付いてくる、表示順では前の絵」の本数。
 *
 * ★x265 の既定は **open GOP**。キーフレーム(CRA)の直後に、表示順ではその手前に
 * 出る絵(leading picture)が並ぶ。これらは**そのキーフレームを参照する**ので、
 * キーフレームの手前でコピーを終えると復号できない。
 *
 * 実測: HEVC で各コピー区間の末尾 3〜4コマが復号できず、300コマのはずが 293 に
 * なった(H.264 の素材では B が2枚しかなく起きなかった)。⇒ **コピーの終わりを
 * この本数だけ手前に引き、そのぶんは焼き直しへ回す。**
 */
static int source_leading_count(SourceInfo *src, int keyframe)
{
  if (av_seek_frame(src->fmt, src->stream_index, source_seek_ts(*src, keyframe),
                    AVSEEK_FLAG_BACKWARD) < 0)
  {
    return 0;
  }
  AVPacket *pkt = av_packet_alloc();
  bool seen = false;
  int lead = 0;
  while (av_read_frame(src->fmt, pkt) >= 0) {
    if (pkt->stream_index == src->stream_index) {
      const int f = source_frame_of(*src, (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts);
      if (!seen) {
        if (f == keyframe) {
          seen = true;
        }
      }
      else {
        if (f < keyframe) {
          lead++;
        }
        else {
          av_packet_unref(pkt);
          break;
        }
      }
    }
    av_packet_unref(pkt);
  }
  av_packet_free(&pkt);
  return lead;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 段2: 計画
 * \{ */

enum class PieceKind { Copy, Reencode };

struct Piece {
  PieceKind kind = PieceKind::Copy;
  int cut_index = 0;
  int start_frame = 0;
  int n_frames = 0;
};

/** ★コピーできるのは「キーフレームから次のキーフレームの直前まで」に限る。
 *
 * こうすると copy 区間は完全な group of pictures の集まりになり、復号順と表示順が
 * 同じ集合を覆う。半端に切ると、B フレームの参照先が範囲の外へ出る。
 */
static void plan_pieces(const Vector<int> &keyframes,
                        int lead_at_last,
                        int cut_index,
                        int in_frame,
                        int n_frames,
                        Vector<Piece> &r_pieces,
                        MovieFastPathReport *report)
{
  const int out_frame = in_frame + n_frames;
  report->total_frames += n_frames;

  if (keyframes.size() < 2) {
    /* 丸ごと入るキーフレーム区間が取れない。焼き直すしかない。 */
    r_pieces.append({PieceKind::Reencode, cut_index, in_frame, n_frames});
    report->reencoded_frames += n_frames;
    return;
  }

  const int kf_first = keyframes.first();
  /* open GOP の素材では、最後のキーフレームに付いてくる leading picture のぶん
   * だけコピーを早く終える(そこは焼き直しへ回る)。 */
  const int kf_last = keyframes.last() - lead_at_last;
  if (kf_last <= kf_first) {
    r_pieces.append({PieceKind::Reencode, cut_index, in_frame, n_frames});
    report->reencoded_frames += n_frames;
    return;
  }

  if (kf_first > in_frame) {
    const int head = kf_first - in_frame;
    r_pieces.append({PieceKind::Reencode, cut_index, in_frame, head});
    report->reencoded_frames += head;
  }
  r_pieces.append({PieceKind::Copy, cut_index, kf_first, kf_last - kf_first});
  report->copied_frames += kf_last - kf_first;

  if (kf_last < out_frame) {
    const int tail = out_frame - kf_last;
    r_pieces.append({PieceKind::Reencode, cut_index, kf_last, tail});
    report->reencoded_frames += tail;
  }
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 段3: 書く
 * \{ */

/** 出力コンテナが、素材の codec をそのまま入れられるか。 */
static bool container_takes_codec(const char *ext, AVCodecID codec_id)
{
  auto is = [&](const char *e) { return BLI_strcasecmp(ext, e) == 0; };
  switch (codec_id) {
    case AV_CODEC_ID_H264:
    case AV_CODEC_ID_H265:
      return is(".mp4") || is(".mkv") || is(".mov");
    case AV_CODEC_ID_MPEG4:
      return is(".mp4") || is(".mkv") || is(".mov") || is(".avi");
    case AV_CODEC_ID_AV1:
      return is(".mp4") || is(".mkv") || is(".webm");
    case AV_CODEC_ID_VP9:
      return is(".mkv") || is(".webm");
    default:
      return false;
  }
}

/** 半端な group of pictures を焼き直すための符号化器と復号器。
 *
 * ★出力は**素材と同じ codec・同じ寸法・同じ画素形式**で作る。違うと、コピーした
 * パケットと同じ1本のストリームに入れられない。
 */
/** ★符号化器は**区間ごとに作り直す**。
 *
 * 最初は1つを使い回し、区間の終わりに `avcodec_send_frame(nullptr)` で
 * 出し切ってから `avcodec_flush_buffers` で戻していた。**14コマ落ちた**
 * (最後の区間が丸ごと 10コマ + 途中で 4コマ)。一度出し切った符号化器を
 * また使うのは定義されていない。作り直しは数ミリ秒なので惜しくない。 */
struct Reencoder {
  AVCodecContext *dec = nullptr;
  AVCodecContext *enc = nullptr;
  /** ★いまの復号器がどの素材のものか。**素材が変わったら作り直す。**
   * 使い回していたので、2本目の素材の焼き直し区間が**1本目の素性で復号され**、
   * 30コマ(頭20 + 尻10)だけ別の絵になっていた。PSNR は 10.28dB で
   * 3件とも同じ値 — **同じ1つの原因**だった。 */
  int dec_source = -1;

  ~Reencoder()
  {
    avcodec_free_context(&dec);
    avcodec_free_context(&enc);
  }
};

static bool reencoder_open_encoder(const SourceInfo &src,
                                   int source_index,
                                   const RenderData *rd,
                                   Reencoder *re,
                                   char *r_reason,
                                   int reason_maxncpy)
{
  const AVCodecParameters *par = src.stream->codecpar;

  if (re->dec == nullptr || re->dec_source != source_index) {
    avcodec_free_context(&re->dec);
    re->dec_source = source_index;
    const AVCodec *dec_codec = avcodec_find_decoder(par->codec_id);
    if (dec_codec == nullptr) {
      FAIL("No decoder for the source codec");
    }
    re->dec = avcodec_alloc_context3(dec_codec);
    avcodec_parameters_to_context(re->dec, par);
    re->dec->pkt_timebase = src.stream->time_base;
    if (avcodec_open2(re->dec, dec_codec, nullptr) < 0) {
      FAIL("Cannot open the decoder for the source");
    }
  }
  avcodec_free_context(&re->enc);

  /* GPU の符号化器が在れば使う(既定)。無ければ CPU の物へ落ちる。 */
  const AVCodec *enc_codec = nullptr;
  /* ★焼き直しは**素材と同じ画素形式**で出さないと、コピーした区間と1本に
   * できない。NVENC は 10bit を `p010le` でしか受け取らないので、素材が
   * `yuv420p10le` だと開けずに **fast path 全体が降りる**(HDR 素材で実測)。
   * ⇒ NVENC が直接受け取れる形式の時だけ GPU を使う。 */
  const bool hw_takes_pix_fmt = (par->format == AV_PIX_FMT_YUV420P);
  if ((rd->ffcodecdata.flags & FFMPEG_NO_HARDWARE_ENCODER) == 0 && hw_takes_pix_fmt) {
    const char *hw_name = (par->codec_id == AV_CODEC_ID_H264)  ? "h264_nvenc" :
                          (par->codec_id == AV_CODEC_ID_H265) ? "hevc_nvenc" :
                                                                 nullptr;
    if (hw_name != nullptr) {
      enc_codec = avcodec_find_encoder_by_name(hw_name);
    }
  }
  if (enc_codec == nullptr) {
    enc_codec = avcodec_find_encoder(par->codec_id);
  }
  if (enc_codec == nullptr) {
    FAIL("No encoder for the source codec; the partial groups cannot be re-encoded");
  }

  re->enc = avcodec_alloc_context3(enc_codec);
  re->enc->width = par->width;
  re->enc->height = par->height;
  re->enc->pix_fmt = AVPixelFormat(par->format);
  re->enc->time_base = AVRational{rd->frs_sec_base, rd->frs_sec};
  re->enc->framerate = AVRational{rd->frs_sec, rd->frs_sec_base};
  re->enc->sample_aspect_ratio = par->sample_aspect_ratio;
  re->enc->color_range = AVColorRange(par->color_range);
  re->enc->colorspace = AVColorSpace(par->color_space);
  re->enc->color_primaries = AVColorPrimaries(par->color_primaries);
  re->enc->color_trc = AVColorTransferCharacteristic(par->color_trc);
  /* ★B フレームを作らせない。焼き直した区間の復号時刻が表示時刻と同じになるので、
   * コピーした区間と1本に混ぜても時刻が前後しない。 */
  re->enc->max_b_frames = 0;
  /* 焼き直すのは常に区間の頭からなので、毎コマがキーフレームでなくてよい。 */
  re->enc->gop_size = 12;
  /* ★これを立てないと符号化器は Annex-B(開始符号つき)で出す。mp4 は
   * 「長さ + NAL」なので、そのまま入れると復号側が長さとして開始符号を読み、
   * `Invalid NAL unit size 17039362` になる(実測。焼き直した区間だけ壊れた)。
   * 立てるとパラメータ集合が extradata へ回るので、区間の頭には自分で貼る。 */
  re->enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  /* 焼き直す区間の品質。
   *
   * ★**素材より上に置く。**ここは出力の2割ほどしかないが、コピーした8割は
   * 素材そのものなので、焼き直した所だけが目に見えて悪いと「切っただけなのに
   * 画質が変わった」という形で出る。容量より劣化を避ける。
   *
   * ★NVENC の cq は CPU の crf と**同じ 0-51 の目盛りだが意味が違う**。
   * 同じ数字で NVENC は 3〜5倍のバイトを使って僅かしか良くならないので、
   * 上へずらす。ずらし量は素材で +0.3〜+7.6 と動く(2026-08-31 の4クリップ掃引)
   * ので、本経路と同じ**中央値 H.264 +4 / H.265 +5** を使う。ここで独自の
   * 数字を置くと、同じ設定なのに経路で品質が変わる。 */
  const int base_crf = 16;
  AVDictionary *opts = nullptr;
  if (BLI_str_endswith(enc_codec->name, "_nvenc")) {
    const int cq_offset = (par->codec_id == AV_CODEC_ID_H265) ? 5 : 4;
    av_dict_set(&opts, "preset", "p5", 0);
    av_dict_set(&opts, "rc", "vbr", 0);
    av_dict_set_int(&opts, "cq", std::min(base_crf + cq_offset, 51), 0);
    /* ⚠ これが無いと cq は一切効かず、既定のビットレートで黙って符号化される。 */
    av_dict_set(&opts, "b:v", "0", 0);
  }
  else {
    av_dict_set_int(&opts, "crf", base_crf, 0);
  }

  const int rc = avcodec_open2(re->enc, enc_codec, &opts);
  av_dict_free(&opts);
  if (rc < 0) {
    FAIL("Cannot open the encoder for the partial groups");
  }
  return true;
}

/** 開始符号つき(Annex-B)の並びを「長さ + NAL」へ詰め直す。
 *
 * ★これが要る理由(実測でここに1時間掛かった):
 * libx264 は **常に Annex-B で出す**。mp4 の muxer はそれを長さ前置へ直してくれる
 * ことがあるが、直すのは**ストリームの extradata が Annex-B の時だけ**。こちらは
 * コピーを本体にするために extradata を**素材の avcC**(先頭が 0x01)から取るので、
 * muxer は「もう長さ前置だ」と判断して素通しする。⇒ 焼き直した区間だけが
 * `Invalid NAL unit size 17039362`(= 開始符号を長さとして読んだ値)で壊れる。
 * コピー区間は無傷なので、**復号できるコマ数も一致コマ数も正しく見えてしまう**。
 */
static Vector<uint8_t> annexb_to_length_prefixed(const uint8_t *data, int size, int len_size)
{
  Vector<uint8_t> out;
  int i = 0;
  auto start_code_len = [&](int at) -> int {
    if (at + 3 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 1) {
      return 3;
    }
    if (at + 4 <= size && data[at] == 0 && data[at + 1] == 0 && data[at + 2] == 0 &&
        data[at + 3] == 1)
    {
      return 4;
    }
    return 0;
  };

  int sc = start_code_len(0);
  if (sc == 0) {
    /* 開始符号で始まっていない = すでに長さ前置。そのまま返す。 */
    out.extend(Span<uint8_t>(data, size));
    return out;
  }
  i = sc;
  while (i < size) {
    int next = size;
    for (int j = i; j + 2 < size; j++) {
      const int n = start_code_len(j);
      if (n != 0) {
        next = j;
        break;
      }
    }
    const int nal_len = next - i;
    if (nal_len > 0) {
      for (int b = len_size - 1; b >= 0; b--) {
        out.append(uint8_t((uint32_t(nal_len) >> (8 * b)) & 0xff));
      }
      out.extend(Span<uint8_t>(data + i, nal_len));
    }
    if (next >= size) {
      break;
    }
    i = next + start_code_len(next);
  }
  return out;
}

/** 素材のパラメータ集合(SPS/PPS)を、パケットの中に置ける形で取り出す。
 *
 * ★これが要る理由: 焼き直した区間は自分の集合を持ち込むので、そのあとに続く
 * コピー区間が同じ id を指すと**焼き直し側の集合で復号されてしまう**。区間の
 * 頭で毎回素材の集合を貼り直して、どちらの区間も自立させる。
 *
 * mp4 の avcC は「2バイト長 + NAL」の並び。パケットの中では長さの幅が
 * avcC の指定(たいてい4バイト)なので、そこへ詰め直す。
 */
static Vector<uint8_t> parameter_sets_of(const uint8_t *ed, int size, AVCodecID codec_id)
{
  Vector<uint8_t> out;
  if (ed == nullptr || size < 7) {
    return out;
  }
  if (codec_id == AV_CODEC_ID_H265 && ed[0] == 1 && size > 23) {
    /* hvcC は avcC と並びが違う。長さの幅は 21 バイト目、そこから
     * 「配列の数」→ 各配列が「種類・本数・(2バイト長 + NAL)×本数」。
     * VPS/SPS/PPS が別々の配列に入るので、全部まとめて持っていく。 */
    const int len_size = (ed[21] & 0x03) + 1;
    int pos = 23;
    const int n_arrays = ed[22];
    for (int a = 0; a < n_arrays; a++) {
      if (pos + 3 > size) {
        out.clear();
        return out;
      }
      pos += 1; /* 種類。どれも持っていくので読まない。 */
      const int n_nalus = (ed[pos] << 8) | ed[pos + 1];
      pos += 2;
      for (int i = 0; i < n_nalus; i++) {
        if (pos + 2 > size) {
          out.clear();
          return out;
        }
        const int nal_len = (ed[pos] << 8) | ed[pos + 1];
        pos += 2;
        if (pos + nal_len > size) {
          out.clear();
          return out;
        }
        for (int b = len_size - 1; b >= 0; b--) {
          out.append(uint8_t((uint32_t(nal_len) >> (8 * b)) & 0xff));
        }
        out.extend(Span<uint8_t>(ed + pos, nal_len));
        pos += nal_len;
      }
    }
    return out;
  }

  if (codec_id != AV_CODEC_ID_H264 || ed[0] != 1) {
    /* Annex-B のままならそのまま置ける。それ以外の形は諦めて空を返す
     * (呼ぶ側は「貼れなかった」として通常の書き出しへ降りる)。 */
    if (size >= 4 && ed[0] == 0 && ed[1] == 0 && (ed[2] == 1 || (ed[2] == 0 && ed[3] == 1))) {
      return annexb_to_length_prefixed(ed, size, 4);
    }
    return out;
  }

  const int len_size = (ed[4] & 0x03) + 1;
  int pos = 5;
  auto take = [&](int count) -> bool {
    for (int i = 0; i < count; i++) {
      if (pos + 2 > size) {
        return false;
      }
      const int nal_len = (ed[pos] << 8) | ed[pos + 1];
      pos += 2;
      if (pos + nal_len > size) {
        return false;
      }
      for (int b = len_size - 1; b >= 0; b--) {
        out.append(uint8_t((nal_len >> (8 * b)) & 0xff));
      }
      out.extend(Span<uint8_t>(ed + pos, nal_len));
      pos += nal_len;
    }
    return true;
  };

  const int n_sps = ed[pos] & 0x1f;
  pos += 1;
  if (!take(n_sps)) {
    out.clear();
    return out;
  }
  if (pos >= size) {
    out.clear();
    return out;
  }
  const int n_pps = ed[pos];
  pos += 1;
  if (!take(n_pps)) {
    out.clear();
  }
  return out;
}

/** パケットの前にパラメータ集合を貼る。 */
static bool packet_prepend(AVPacket *pkt, Span<uint8_t> prefix)
{
  if (prefix.is_empty()) {
    return true;
  }
  AVPacket *tmp = av_packet_alloc();
  if (av_new_packet(tmp, int(prefix.size()) + pkt->size) < 0) {
    av_packet_free(&tmp);
    return false;
  }
  memcpy(tmp->data, prefix.data(), prefix.size());
  memcpy(tmp->data + prefix.size(), pkt->data, pkt->size);
  av_packet_copy_props(tmp, pkt);
  av_packet_unref(pkt);
  av_packet_move_ref(pkt, tmp);
  av_packet_free(&tmp);
  return true;
}

/** 出力へ1つ書く。復号時刻が戻らないように見張る。 */
struct OutputStream {
  AVFormatContext *fmt = nullptr;
  AVStream *stream = nullptr;
  /** こちらが数える単位 = 1コマ。 */
  AVRational frame_tb = {1, 1};
  int64_t last_dts = AV_NOPTS_VALUE;

  /** ★時刻はコマ番号で組み立て、渡す直前に**ストリームの単位**へ直す。
   *
   * `avformat_write_header` はコンテナの都合で `stream->time_base` を書き換える
   * (mp4 は 1/15360 等)。header の前に入れた値をそのまま信じてコマ番号を渡すと、
   * 300コマが 0.0195 秒の動画になる(実測)。 */
  bool write(AVPacket *pkt)
  {
    av_packet_rescale_ts(pkt, frame_tb, stream->time_base);
    pkt->duration = av_rescale_q(1, frame_tb, stream->time_base);

    /* ★混ぜた結果 dts が戻ると muxer が拒む。戻る時だけ1つ進めて通す。
     * 焼き直した区間は B フレームなし(dts == pts)、コピーした区間は素材の
     * 並べ替えの遅れをそのまま持つので、境目でだけ起こりうる。 */
    if (last_dts != AV_NOPTS_VALUE && pkt->dts <= last_dts) {
      const int64_t shift = last_dts + 1 - pkt->dts;
      pkt->dts += shift;
      pkt->pts = std::max(pkt->pts, pkt->dts);
    }
    last_dts = pkt->dts;
    pkt->stream_index = stream->index;
    return av_interleaved_write_frame(fmt, pkt) >= 0;
  }
};

/** コピー区間: パケットをそのまま流す。ここが Fast path の値打ちの本体。 */
static bool write_copy_piece(SourceInfo &src,
                             const Piece &piece,
                             int64_t out_base,
                             Span<uint8_t> param_sets,
                             OutputStream &out,
                             char *r_reason,
                             int reason_maxncpy)
{
  bool first_packet = true;
  if (av_seek_frame(
          src.fmt, src.stream_index, source_seek_ts(src, piece.start_frame), AVSEEK_FLAG_BACKWARD) <
      0)
  {
    FAIL("Cannot seek to frame %d of '%s'", piece.start_frame, BLI_path_basename(src.fmt->url));
  }

  const int end_frame = piece.start_frame + piece.n_frames;
  int written = 0;
  AVPacket *pkt = av_packet_alloc();
  bool ok = true;
  while (av_read_frame(src.fmt, pkt) >= 0) {
    if (pkt->stream_index != src.stream_index) {
      av_packet_unref(pkt);
      continue;
    }
    const int frame = source_frame_of(src, (pkt->pts != AV_NOPTS_VALUE) ? pkt->pts : pkt->dts);
    if (frame < piece.start_frame) {
      /* 並べ替えのぶんだけ範囲の手前が先に来る。まだ書かない。 */
      av_packet_unref(pkt);
      continue;
    }
    if (frame >= end_frame) {
      /* ★範囲の先のコマが**先に**来ることがある。並べ替えの深さは符号化器で
       * 違い、H.264(B 2枚)では起きなかったが HEVC では起きた(7コマ落ちた)。
       * ⇒ ここで打ち切らず、**書いた本数**で終わりを決める。次の group of
       * pictures の頭(キーフレーム)まで来たら、そこで確実に終わる。 */
      if (pkt->flags & AV_PKT_FLAG_KEY) {
        av_packet_unref(pkt);
        break;
      }
      av_packet_unref(pkt);
      continue;
    }

    /* ★時刻は作り直す。素材の並べ替えの遅れ (pts - dts) はそのまま持ち越す。 */
    const int64_t out_pts = out_base + (frame - piece.start_frame);
    int64_t delay = 0;
    if (pkt->pts != AV_NOPTS_VALUE && pkt->dts != AV_NOPTS_VALUE) {
      delay = int64_t(lround(double(pkt->pts - pkt->dts) * av_q2d(src.stream->time_base) * src.fps));
    }
    pkt->pts = out_pts;
    pkt->dts = out_pts - delay;
    pkt->pos = -1;

    if (first_packet) {
      /* 区間の頭は必ずキーフレーム。ここに素材の集合を貼って自立させる。 */
      if (!packet_prepend(pkt, param_sets)) {
        av_packet_unref(pkt);
        ok = false;
        break;
      }
      first_packet = false;
    }

    if (!out.write(pkt)) {
      ok = false;
      av_packet_unref(pkt);
      break;
    }
    av_packet_unref(pkt);
    if (++written >= piece.n_frames) {
      break;
    }
  }
  av_packet_free(&pkt);
  if (!ok) {
    FAIL("Writing a copied packet failed");
  }
  return true;
}

/** 焼き直し区間: 復号して符号化し直す。 */
static bool write_reencode_piece(SourceInfo &src,
                                 const Piece &piece,
                                 int64_t out_base,
                                 Span<uint8_t> param_sets,
                                 bool needs_length_prefix,
                                 Reencoder &re,
                                 OutputStream &out,
                                 char *r_reason,
                                 int reason_maxncpy)
{
  bool first_packet = true;
  /* ★区間の頭より**少し手前**から復号を始める。
   *
   * 探索は**復号時刻**で当たる。open GOP の素材ではキーフレーム(CRA)の復号時刻が
   * 表示時刻より手前にあるので、区間の頭を狙って戻ると**その CRA の後ろ**に
   * 着地する。すると CRA を参照する leading picture が復号できず、復号器が
   * 黙って捨てる。実測: HEVC で各焼き直し区間の頭 3〜4コマが消え、300コマの
   * はずが 293 になった(数え方でも打ち切り方でもなく、**始める場所**だった)。
   * 1 group of pictures ぶん手前から入れば、必ず手前のキーフレームに着く。
   * 余計に復号するのは焼き直す区間(全体の2割)の中だけなので安い。 */
  const int seek_from = std::max(0, piece.start_frame - FASTPATH_SEEK_MARGIN);
  if (av_seek_frame(
          src.fmt, src.stream_index, source_seek_ts(src, seek_from), AVSEEK_FLAG_BACKWARD) < 0)
  {
    FAIL("Cannot seek to frame %d of '%s'", piece.start_frame, BLI_path_basename(src.fmt->url));
  }
  avcodec_flush_buffers(re.dec);

  const int end_frame = piece.start_frame + piece.n_frames;
  AVPacket *pkt = av_packet_alloc();
  AVPacket *out_pkt = av_packet_alloc();
  AVFrame *frame = av_frame_alloc();
  bool ok = true;
  bool done = false;
  int encoded = 0;

  auto drain_encoder = [&]() {
    while (avcodec_receive_packet(re.enc, out_pkt) == 0) {
      av_packet_rescale_ts(out_pkt, re.enc->time_base, out.frame_tb);
      out_pkt->pos = -1;
      if (needs_length_prefix) {
        /* ★長さ前置へ直す。素通しだと開始符号が長さとして読まれる。 */
        const Vector<uint8_t> fixed = annexb_to_length_prefixed(
            out_pkt->data, out_pkt->size, 4);
        /* ⚠ 大きさで「直ったか」を判定してはいけない。**4バイトの開始符号を
         * 4バイトの長さへ置き換えると大きさは1バイトも変わらない**ので、
         * `size != size` の条件では一度も差し替えられなかった(実測)。 */
        if (!fixed.is_empty()) {
          AVPacket *tmp = av_packet_alloc();
          if (av_new_packet(tmp, int(fixed.size())) == 0) {
            memcpy(tmp->data, fixed.data(), fixed.size());
            av_packet_copy_props(tmp, out_pkt);
            av_packet_unref(out_pkt);
            av_packet_move_ref(out_pkt, tmp);
          }
          av_packet_free(&tmp);
        }
      }
      if (first_packet) {
        if (!packet_prepend(out_pkt, param_sets)) {
          ok = false;
          av_packet_unref(out_pkt);
          break;
        }
        first_packet = false;
      }
      if (!out.write(out_pkt)) {
        ok = false;
      }
      av_packet_unref(out_pkt);
    }
  };

  while (!done && ok && av_read_frame(src.fmt, pkt) >= 0) {
    if (pkt->stream_index != src.stream_index) {
      av_packet_unref(pkt);
      continue;
    }
    if (avcodec_send_packet(re.dec, pkt) == 0) {
      while (avcodec_receive_frame(re.dec, frame) == 0) {
        const int f = source_frame_of(src, frame->pts);
        if (f >= piece.start_frame && f < end_frame) {
          encoded++;
          frame->pts = out_base + (f - piece.start_frame);
          frame->pict_type = AV_PICTURE_TYPE_NONE;
          if (avcodec_send_frame(re.enc, frame) < 0) {
            ok = false;
          }
          drain_encoder();
        }
        av_frame_unref(frame);
        /* ★ここも本数で数える。表示順で先のコマが先に出てくることがある。 */
        if (encoded >= piece.n_frames) {
          done = true;
        }
      }
    }
    av_packet_unref(pkt);
  }

  /* 符号化器の中に残っているぶんを出す。★ここを忘れると尻のコマが落ちる。 */
  if (ok) {
    avcodec_send_frame(re.enc, nullptr);
    drain_encoder();
  }

  av_frame_free(&frame);
  av_packet_free(&out_pkt);
  av_packet_free(&pkt);
  if (!ok) {
    FAIL("Re-encoding a partial group of pictures failed");
  }
  return true;
}

/** \} */

/* -------------------------------------------------------------------- */
/** \name 入口
 * \{ */

bool MOV_fastpath_write(const Scene *scene,
                        const RenderData *rd,
                        const ImageFormatData * /*imf*/,
                        Span<MovieFastPathCut> cuts,
                        ReportList *reports,
                        MovieFastPathReport *r_report,
                        char *r_reason,
                        int reason_maxncpy)
{
  r_reason[0] = '\0';
  if (cuts.is_empty()) {
    FAIL("There are no cuts to write");
  }

  char out_path[FILE_MAX];
  MOV_filepath_from_settings(out_path, scene, rd, false, "", nullptr);
  const char *ext = BLI_path_extension(out_path);
  if (ext == nullptr) {
    FAIL("The output file name has no extension to pick a container from");
  }

  /* 素材を全部開いて素性を突き合わせる。 */
  Vector<SourceInfo> sources(cuts.size());
  auto close_all = [&]() {
    for (SourceInfo &s : sources) {
      source_close(&s);
    }
  };
  for (int i = 0; i < cuts.size(); i++) {
    if (!source_open(cuts[i].path, &sources[i], r_reason, reason_maxncpy)) {
      close_all();
      return false;
    }
  }

  const AVCodecParameters *first = sources[0].stream->codecpar;
  for (int i = 1; i < sources.size(); i++) {
    const AVCodecParameters *p = sources[i].stream->codecpar;
    if (p->codec_id != first->codec_id || p->width != first->width ||
        p->height != first->height || p->format != first->format)
    {
      close_all();
      FAIL("The sources are not encoded the same way; a stream copy needs identical sources");
    }
    if (fabs(sources[i].fps - sources[0].fps) > FPS_TOLERANCE) {
      close_all();
      FAIL("The sources differ in frame rate (%.4f vs %.4f)", sources[0].fps, sources[i].fps);
    }
  }

  if (rd->xsch != first->width || rd->ysch != first->height) {
    close_all();
    FAIL("Output is %dx%d but the source is %dx%d", rd->xsch, rd->ysch, first->width, first->height);
  }
  const double scene_fps = double(rd->frs_sec) / double(rd->frs_sec_base);
  if (fabs(scene_fps - sources[0].fps) > FPS_TOLERANCE) {
    close_all();
    FAIL("Output is %.4f fps but the source is %.4f fps", scene_fps, sources[0].fps);
  }
  if (!container_takes_codec(ext, first->codec_id)) {
    close_all();
    FAIL("The source codec cannot be put into '%s' as it is", ext);
  }
  /* ★出力の codec に何を選んでいるかを見る。
   *
   * fast path が書くのは**素材の codec そのもの**なので、出力設定が別の codec を
   * 指しているなら、頼まれた物と違う物を黙って書くことになる。速いかどうか以前に
   * 約束が違う。⇒ 食い違うなら降りて、普通に符号化させる。 */
  const AVCodecID want_codec = mov_av_codec_id_get(rd->ffcodecdata.codec_id_get());
  if (want_codec != first->codec_id) {
    close_all();
    FAIL("Output is set to a different codec than the source, so the frames cannot be "
         "copied as they are");
  }

  /* 段2: 部品へ割る。 */
  Vector<Piece> pieces;
  for (int i = 0; i < cuts.size(); i++) {
    Vector<int> keyframes;
    if (!source_keyframes(&sources[i],
                          cuts[i].in_frame,
                          cuts[i].in_frame + cuts[i].n_frames,
                          keyframes,
                          r_reason,
                          reason_maxncpy))
    {
      close_all();
      return false;
    }
    const int lead = keyframes.size() >= 2 ?
                         source_leading_count(&sources[i], keyframes.last()) :
                         0;
    plan_pieces(keyframes, lead, i, cuts[i].in_frame, cuts[i].n_frames, pieces, r_report);
  }
  r_report->pieces = pieces.size();

  /* 焼き直す区間が1つでもあるなら符号化器を用意する。 */
  Reencoder re;
  bool need_reencode = false;
  for (const Piece &p : pieces) {
    need_reencode |= (p.kind == PieceKind::Reencode);
  }
  if (need_reencode && !reencoder_open_encoder(sources[0], 0, rd, &re, r_reason, reason_maxncpy)) {
    close_all();
    return false;
  }

  /* 段3: 1本の muxer へ流す。 */
  OutputStream out;
  out.frame_tb = AVRational{rd->frs_sec_base, rd->frs_sec};
  if (avformat_alloc_output_context2(&out.fmt, nullptr, nullptr, out_path) < 0 ||
      out.fmt == nullptr)
  {
    close_all();
    FAIL("Cannot write '%s' with any of the muxers built in", ext);
  }
  out.stream = avformat_new_stream(out.fmt, nullptr);
  if (out.stream == nullptr) {
    avformat_free_context(out.fmt);
    close_all();
    FAIL("Cannot create the output video stream");
  }

  /* ★ストリームの素性は**素材から**取る。
   *
   * 最初は焼き直す側から取っていた。結果、コピーしたパケットが自分の
   * パラメータ集合を指せなくなり、300コマ中 240コマが `sps_id out of range` で
   * 復号できなかった(実測)。コピーが本体なので、素材側を正とする。
   * 焼き直した区間のほうは、下で毎キーフレームに自分の集合を持たせて自立させる。 */
  avcodec_parameters_copy(out.stream->codecpar, first);
  out.stream->codecpar->codec_tag = 0;
  /* ★色の素性は通常の書き出しに合わせて bt709 と名乗る。
   *
   * 素材が「未指定」のままだと、こちらの出力だけ `unknown` になり、通常の
   * 書き出し(いつも bt709 を書く)と食い違う。多くの再生側は HD を bt709 と
   * みなすので絵は同じに見えるが、**同じ編集で違うタグの動画が出る**のは
   * 約束と違う。bt709 以外の素材は上で降ろしてあるので、ここは常に bt709。 */
  out.stream->codecpar->color_primaries = AVCOL_PRI_BT709;
  out.stream->codecpar->color_trc = AVCOL_TRC_BT709;
  out.stream->codecpar->color_space = AVCOL_SPC_BT709;
  out.stream->time_base = out.frame_tb;
  out.stream->avg_frame_rate = AVRational{rd->frs_sec, rd->frs_sec_base};

  /* ★パラメータ集合を区間の頭へ貼り直すのが要るのは H.264/H.265 だけ。
   * VP9 と AV1 はキーフレームが自分で完結しているので、貼る物が無くても
   * 混ぜられる。ここを codec で分けないと、VP9/AV1 が「集合が読めない」で
   * 落ちる(実際は要らないのに)。 */
  const bool needs_param_sets = ELEM(first->codec_id, AV_CODEC_ID_H264, AV_CODEC_ID_H265);
  /* ★**素材ごと**に持つ。同じ codec・同じ寸法でも、別のファイルなら SPS/PPS は
   * 別物になりうる。1本目の集合を全部の区間に貼っていたので、2本目の素材から
   * コピーした所が `top block unavailable` で崩れ、PSNR が 10dB まで落ちた
   * (**素材1本の台では一度も出なかった**)。 */
  Vector<Vector<uint8_t>> param_sets(sources.size());
  if (needs_param_sets) {
    for (int i = 0; i < sources.size(); i++) {
      const AVCodecParameters *par = sources[i].stream->codecpar;
      param_sets[i] = parameter_sets_of(par->extradata, par->extradata_size, par->codec_id);
      if (need_reencode && param_sets[i].is_empty()) {
        avformat_free_context(out.fmt);
        close_all();
        FAIL("Cannot read the parameter sets of '%s'", BLI_path_basename(cuts[i].path));
      }
    }
  }
  /* ★焼き直した区間にも、自分のパラメータ集合を頭に貼る。素材の集合とは別物
   * なので、貼らないと直前のコピー区間の集合で復号される。 */

  /* ★音は運ばない。**Blender の普通のミックスダウンに作らせる。**
   *
   * 素材の音のパケットを切り貼りする道もあるが、音量・フェード・複数トラックの
   * 合成を全部作り直すことになるうえ、音の粒(AAC なら 1024 標本 ≒ 21ms)は
   * コマ境界に乗らないので、切り口で必ず隙間か重なりが出る。省きたいのは映像の
   * 復号と符号化であって、音の符号化は全体のごく一部。 */
  MovieWriter audio_ctx;
  audio_ctx.outfile = out.fmt;
  audio_ctx.ffmpeg_audio_bitrate = rd->ffcodecdata.audio_bitrate;
  const AVCodecID audio_codec_id = AVCodecID(rd->ffcodecdata.audio_codec);
  const bool want_audio = (audio_codec_id != AV_CODEC_ID_NONE);
  if (want_audio) {
    char audio_error[512] = "";
    audio_ctx.audio_stream = alloc_audio_stream(&audio_ctx,
                                                rd->ffcodecdata.audio_mixrate,
                                                rd->ffcodecdata.audio_channels,
                                                audio_codec_id,
                                                out.fmt,
                                                audio_error,
                                                sizeof(audio_error),
                                                reports);
    if (audio_ctx.audio_stream == nullptr) {
      avformat_free_context(out.fmt);
      close_all();
      FAIL("Cannot set up the audio stream%s%s",
           audio_error[0] ? ": " : "",
           audio_error[0] ? audio_error : "");
    }
  }

  bool ok = true;
  if (!(out.fmt->oformat->flags & AVFMT_NOFILE)) {
    if (avio_open(&out.fmt->pb, out_path, AVIO_FLAG_WRITE) < 0) {
      ok = false;
      BLI_snprintf(r_reason, reason_maxncpy, "Cannot open '%s' for writing", out_path);
    }
  }
  if (ok && avformat_write_header(out.fmt, nullptr) < 0) {
    ok = false;
    BLI_strncpy(r_reason, "Cannot write the file header", reason_maxncpy);
  }
  if (ok && want_audio &&
      !movie_audio_open(&audio_ctx,
                        scene,
                        rd->sfra,
                        rd->ffcodecdata.audio_mixrate,
                        rd->ffcodecdata.audio_volume,
                        reports))
  {
    ok = false;
    BLI_strncpy(r_reason, "Cannot open the audio mixdown", reason_maxncpy);
  }

  int64_t out_frame = 0;
  for (const Piece &piece : pieces) {
    if (!ok) {
      break;
    }
    SourceInfo &src = sources[piece.cut_index];
    Vector<uint8_t> enc_param_sets;
    if (piece.kind == PieceKind::Reencode) {
      if (!reencoder_open_encoder(src, piece.cut_index, rd, &re, r_reason, reason_maxncpy)) {
        ok = false;
        break;
      }
      /* ★符号化器は区間ごとに作り直すので、その集合も区間ごとに取り直す。 */
      if (needs_param_sets) {
        enc_param_sets = parameter_sets_of(
            re.enc->extradata, re.enc->extradata_size, re.enc->codec_id);
      }
    }
    ok = (piece.kind == PieceKind::Copy) ?
             write_copy_piece(src,
                              piece,
                              out_frame,
                              param_sets[piece.cut_index],
                              out,
                              r_reason,
                              reason_maxncpy) :
             write_reencode_piece(src,
                                  piece,
                                  out_frame,
                                  enc_param_sets,
                                  needs_param_sets,
                                  re,
                                  out,
                                  r_reason,
                                  reason_maxncpy);
    out_frame += piece.n_frames;
    if (ok && want_audio) {
      /* 映像がどこまで進んだかを秒で渡す。音はそこまで追いつく。 */
      write_audio_frames(&audio_ctx, double(out_frame) * av_q2d(out.frame_tb));
    }
  }

  if (ok && want_audio) {
    /* ★最後のひと押し。ここを忘れると尻の音が落ちる。 */
    write_audio_frames(&audio_ctx, double(out_frame + 1) * av_q2d(out.frame_tb));
  }
  if (want_audio) {
    movie_audio_close(&audio_ctx, false);
  }
  if (ok) {
    av_write_trailer(out.fmt);
  }
  if (out.fmt->pb != nullptr) {
    avio_closep(&out.fmt->pb);
  }
  avformat_free_context(out.fmt);
  close_all();

  if (!ok) {
    /* ★途中まで書けた物を残さない。中身の足りない動画は、失敗より始末が悪い。 */
    BLI_delete(out_path, false, false);
    return false;
  }

  CLOG_INFO(&LOG,
            "Fast path wrote '%s': %d frames, %d copied, %d re-encoded, %d pieces",
            out_path,
            r_report->total_frames,
            r_report->copied_frames,
            r_report->reencoded_frames,
            r_report->pieces);
  return true;
}

#  undef FAIL

/** \} */

}  // namespace blender

#else /* !WITH_FFMPEG */

namespace blender {

bool MOV_fastpath_write(const Scene * /*scene*/,
                        const RenderData * /*rd*/,
                        const ImageFormatData * /*imf*/,
                        Span<MovieFastPathCut> /*cuts*/,
                        ReportList * /*reports*/,
                        MovieFastPathReport * /*r_report*/,
                        char *r_reason,
                        int /*reason_maxncpy*/)
{
  r_reason[0] = '\0';
  return false;
}

}  // namespace blender

#endif /* WITH_FFMPEG */
