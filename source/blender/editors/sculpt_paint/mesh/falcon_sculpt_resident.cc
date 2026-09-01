/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "falcon_sculpt_resident.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BKE_attribute.hh"
#include "BKE_brush.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_array.hh"
#include "BLI_math_matrix.hh"
#include "BLI_math_matrix_types.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_utildefines.h"
#include "BLI_task.hh"
#include "BLI_time.h"
#include "BLI_vector.hh"

#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "ED_view3d.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::falcon_resident {

/* 落とし方の曲線を焼く段数。`falcon_sculpt_gpu.cc` と同じ。 */
static constexpr int CURVE_LUT_SIZE = 1024;
/* 1グループのスレッド数。シェーダ側の LOCAL_GROUP_SIZE と揃える。 */
static constexpr int GROUP_SIZE = 256;

enum class Mode { Off, Resident, Verify };

static Mode mode()
{
#ifndef WITH_FALCON_SCULPT_GPU
  /* release 版はこの道をビルドに入れない(FALCON_BUILD_FLAVOR=release)。
   * 環境変数を置かれても効かない。 */
  return Mode::Off;
#else
  static const Mode m = []() {
    const char *v = getenv("FALCON_SCULPT_GPU");
    if (v == nullptr) {
      return Mode::Off;
    }
    if (STREQ(v, "resident")) {
      return Mode::Resident;
    }
    if (STREQ(v, "resident-verify")) {
      return Mode::Verify;
    }
    return Mode::Off;
  }();
  return m;
#endif
}

bool enabled()
{
  return mode() != Mode::Off;
}

bool verify_mode()
{
  return mode() == Mode::Verify;
}

/* ------------------------------------------------------------------------- */
/** \name 常駐している物
 * \{ */

struct Buf {
  gpu::StorageBuf *buf = nullptr;
  size_t capacity = 0;
};

/* ★足りなければ大きくして、足りていれば使い回す(1ダブごとに大きさが変わる物)。
 *
 * ★最初の版(2026-08-16)は毎ダブ作り直していて、CPU 2.2ms に対し GPU 24ms だった。
 * 作る費用がそのまま毎回乗る。**使い回しは速度の話ではなく設計の前提。**
 *
 * ⚠**容量ぶんの CPU 側配列を必ず用意すること。**`GPU_storagebuf_update` /
 * `_read` は「今使う分」ではなく**確保した容量**を触る。実際に、触った頂点が
 * 前のダブより減った時に確保済み容量を読み戻して**ヒープを壊して落ちた**
 * (2026-08-25・tbbmalloc の中で落ちるので原因が見えにくい)。 */
static gpu::StorageBuf *ensure_grow(Buf &b, const size_t size, const char *name)
{
  const size_t want = std::max<size_t>(size, 16);
  if (b.buf != nullptr && b.capacity < want) {
    GPU_storagebuf_free(b.buf);
    b.buf = nullptr;
  }
  if (b.buf == nullptr) {
    b.buf = GPU_storagebuf_create_ex(want, nullptr, GPU_USAGE_DYNAMIC, name);
    b.capacity = want;
  }
  return b.buf;
}

/* 常駐する物は**ぴったりの大きさ**で持つ。作り直すのは滅多に無い
 * (メッシュが変わった時だけ)ので、容量のずれを持ち込む理由が無い。 */
static gpu::StorageBuf *ensure_exact(Buf &b, const size_t size, const char *name)
{
  const size_t want = std::max<size_t>(size, 16);
  if (b.buf != nullptr && b.capacity != want) {
    GPU_storagebuf_free(b.buf);
    b.buf = nullptr;
  }
  if (b.buf == nullptr) {
    b.buf = GPU_storagebuf_create_ex(want, nullptr, GPU_USAGE_DYNAMIC, name);
    b.capacity = want;
  }
  return b.buf;
}

/* CPU 側の配列を、GPU の**容量**に合わせる(上の ⚠ の対処)。 */
template<typename T> static MutableSpan<T> fit(Array<T> &a, const Buf &b)
{
  const int64_t n = int64_t(b.capacity / sizeof(T));
  if (a.size() != n) {
    a.reinitialize(n);
  }
  return a.as_mutable_span();
}

static void discard(Buf &b)
{
  if (b.buf != nullptr) {
    GPU_storagebuf_free(b.buf);
    b.buf = nullptr;
    b.capacity = 0;
  }
}

struct Resident {
  /* 常駐の持ち主。PBVH が建て直されたら全部捨てる。 */
  const void *pbvh = nullptr;
  int64_t nodes_num = 0;
  int64_t verts_num = 0;

  /* 全ノードの頂点番号をノード順に連結したもの。CPU 側にも同じ物を持つ
   * (GPU から戻した値を書き戻す時に使う)。 */
  Array<int> node_verts;
  /* ノードごとの先頭位置。要素数 = ノード数 + 1。 */
  Array<int> node_offsets;

  Buf b_node_verts, b_positions, b_mask, b_lut, b_normals, b_out, b_starts, b_toff;

  /* ストロークの初めに1回だけ位置を送る。 */
  bool positions_uploaded = false;
  /* 曲線を焼いた時の半径。変わったら焼き直す。 */
  float lut_radius = -1.0f;

  /* 1ダブぶんの作業領域(使い回す)。**大きさは GPU 側の容量に合わせる**
   * (`fit` の ⚠ を見ること)。 */
  Array<int> starts, toff;
  Array<float3> out_translations;
  Array<float3> normals_packed;
  /* verify の時だけ: CPU が動かす前の評価後の位置。 */
  Array<float3> before;

  /* verify のために取っておく前回の結果。 */
  int64_t last_total = 0;
  int64_t last_nodes = 0;
};

static Resident g;

void invalidate()
{
  g.positions_uploaded = false;
}

static void discard_all()
{
  discard(g.b_node_verts);
  discard(g.b_positions);
  discard(g.b_mask);
  discard(g.b_lut);
  discard(g.b_normals);
  discard(g.b_out);
  discard(g.b_starts);
  discard(g.b_toff);
  g.pbvh = nullptr;
  g.nodes_num = 0;
  g.verts_num = 0;
  g.positions_uploaded = false;
  g.lut_radius = -1.0f;
}

/** \} */

/* ------------------------------------------------------------------------- */
/** \name 計時
 * \{ */

static std::atomic<int64_t> t_setup{0}, t_upload{0}, t_dispatch{0}, t_read{0}, t_scatter{0},
    n_dabs{0}, n_verts{0};
/* 突き合わせ(verify)。float の atomic max は面倒なので 1e6 倍した整数で持つ。 */
static std::atomic<int64_t> v_max_micro{0}, v_count{0};

void report_breakdown()
{
  const int64_t n = n_dabs.exchange(0, std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  const double d = double(n);
  const double su = double(t_setup.exchange(0, std::memory_order_relaxed)) / 1.0e6 / d;
  const double up = double(t_upload.exchange(0, std::memory_order_relaxed)) / 1.0e6 / d;
  const double di = double(t_dispatch.exchange(0, std::memory_order_relaxed)) / 1.0e6 / d;
  const double rd = double(t_read.exchange(0, std::memory_order_relaxed)) / 1.0e6 / d;
  const double sc = double(t_scatter.exchange(0, std::memory_order_relaxed)) / 1.0e6 / d;
  const int64_t nv = n_verts.exchange(0, std::memory_order_relaxed);
  printf(
      "FALCON_SCULPT 常駐GPU %ldダブ 触った頂点 %ld/ダブ | 支度 %.2f / 送り %.2f / "
      "実行 %.2f / 読み戻し %.2f / 書き戻し %.2f ms = 計 %.2f ms\n",
      long(n),
      long(double(nv) / d),
      su,
      up,
      di,
      rd,
      sc,
      su + up + di + rd + sc);
  const int64_t vc = v_count.exchange(0, std::memory_order_relaxed);
  if (vc != 0) {
    printf("FALCON_SCULPT 常駐GPU 突き合わせ %ld頂点  最大差 %.7f\n",
           long(vc),
           double(v_max_micro.exchange(0, std::memory_order_relaxed)) * 1.0e-6);
  }
  fflush(stdout);
}

/** \} */

/* ------------------------------------------------------------------------- */
/** \name 通してよい条件
 * \{ */

/* 通らない条件は素直に false。**推測で通すと絵が静かに変わる**
 * (係数は掛け算なので、抜けた項目のぶんだけ強くなるだけで破綻して見えない)。 */
/* ★通らなかった理由を**1回だけ**刷る。
 * 「GPUの道が黙って通っていない」は、実測でいちばん時間を溶かす形
 * (数字は出るが CPU のまま = 速くならない理由が分からない)。 */
static bool refuse(const char *why)
{
  static const char *said = nullptr;
  if (said == nullptr || !STREQ(said, why)) {
    said = why;
    printf("FALCON_SCULPT 常駐GPU 通らない: %s\n", why);
    fflush(stdout);
  }
  return false;
}

static bool can_use(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &object,
                    const Brush &brush)
{
  if (GPU_context_active_get() == nullptr) {
    return refuse("GPUの文脈が無い");
  }
  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr) {
    return refuse("スカルプトの状態が無い");
  }
  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(object);
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return refuse("PBVH が Mesh 型でない");
  }

  /* 距離の出し方が違うので TUBE は別物。 */
  if (eBrushFalloffShape(brush.falloff_shape) != PAINT_FALLOFF_SHAPE_SPHERE) {
    return refuse("落とし方が球でない(TUBE)");
  }

  /* 隠した頂点・自動マスク・テクスチャは**まだ再現していない**。 */
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  if (mesh.attributes().contains(".hide_vert")) {
    return refuse("隠した頂点がある(.hide_vert)");
  }
  if (auto_mask::is_enabled(sd.paint, object, &brush)) {
    return refuse("自動マスクが有効");
  }
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (mtex && mtex->tex) {
    return refuse("マスクのテクスチャがある");
  }

  /* 視点のクリップ面。 */
  const RegionView3D *rv3d = ss->cache->vc ? ss->cache->vc->rv3d : ss->rv3d;
  const View3D *v3d = ss->cache->vc ? ss->cache->vc->v3d : ss->v3d;
  if (RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return refuse("視点のクリップ面が有効");
  }

  /* ★書き戻しの経路を1本に絞る。`PositionDeformData::deform` が
   * 元の配列だけを触る条件と**同じ3つ**にする(あちらの実装を写した):
   *   1. 評価後の位置と元の位置が同じ配列(`eval_mut_` が付かない)… `offset_positions` で見る
   *   2. crazyspace の逆行列が無い(`deform_imats_` が付かない)
   *   3. シェイプキーが無い(`ShapeKeyData::from_object` が nullopt = `mesh.key == nullptr`)
   *
   * ⚠ `ss->deform_cos` は**条件に入れない**。頂点ペイント経路でも埋まる上、
   * 埋まっていても 1 が成り立てば書き戻しは同じ1本になる(実測: 試験台の
   * オブジェクトはここで落ちていた)。 */
  /* ⚠ crazyspace(`ss->deform_imats`)は**弾かない**。Blender 4.1 以降は
   * 「スムーズシェード(角度)」のジオメトリノードが既定で付くため、
   * ここで弾くと**実データではほぼ一度も通らない**(試験台で実際にそうなった)。
   * 逆行列は書き戻しの時に掛ける。 */
  if (mesh.key != nullptr) {
    return refuse("シェイプキーがある");
  }
  UNUSED_VARS(depsgraph);

  /* 鏡のクリップ(ミラーモディファイアの Clipping)は再現していない。
   * 軸の固定(`SCULPT_LOCK_*`)だけは GPU 側で掛ける。 */
  const StrokeCache &cache = *ss->cache;
  if (cache.mirror_modifier_clip.flag != 0) {
    return refuse("鏡のクリップが有効");
  }

  /* アンカー/ドラッグドットは毎歩 undo から復元するので、常駐が古くなる。 */
  if (ELEM(brush.stroke_method, BRUSH_STROKE_ANCHORED, BRUSH_STROKE_DRAG_DOT)) {
    return refuse("アンカー/ドラッグドットの引き方");
  }

  return true;
}

/** \} */

/* ------------------------------------------------------------------------- */
/** \name 曲線を表に焼く
 * \{ */

/* `BKE_brush_calc_curve_factors` は係数に**掛け算で足す**ので、1.0 で埋めてから通す。
 * プリセットにも自作カーブにも同じ関数を通すので GPU 側で分岐を再現しない。 */
static void bake_curve_lut(const Brush &brush, const float radius, MutableSpan<float> lut)
{
  Array<float> distances(lut.size());
  for (const int i : lut.index_range()) {
    distances[i] = radius * float(i) / float(lut.size() - 1);
    lut[i] = 1.0f;
  }
  BKE_brush_calc_curve_factors(eBrushCurvePreset(brush.curve_distance_falloff_preset),
                               brush.curve_distance_falloff,
                               distances,
                               radius,
                               lut);
}

/** \} */

/* ------------------------------------------------------------------------- */
/** \name 本体
 * \{ */

/* ノードの頂点表を1本に連結して常駐させる。**ストロークの間は作り直さない。**
 * ここを毎ダブやると「番号の集め直し」が CPU に残り、前の設計と同じ所で負ける。 */
static bool ensure_topology(bke::pbvh::Tree &pbvh, const int64_t verts_num)
{
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  if (g.pbvh == &pbvh && g.nodes_num == nodes.size() && g.verts_num == verts_num) {
    return true;
  }
  discard_all();

  g.node_offsets.reinitialize(nodes.size() + 1);
  int64_t total = 0;
  for (const int i : nodes.index_range()) {
    g.node_offsets[i] = int(total);
    total += nodes[i].verts().size();
  }
  g.node_offsets[nodes.size()] = int(total);

  g.node_verts.reinitialize(total);
  threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
    for (const int i : range) {
      const Span<int> verts = nodes[i].verts();
      std::copy(verts.begin(), verts.end(), g.node_verts.begin() + g.node_offsets[i]);
    }
  });

  gpu::StorageBuf *buf = ensure_exact(g.b_node_verts, size_t(total) * sizeof(int), "falcon_node_verts");
  GPU_storagebuf_usage_size_set(buf, size_t(total) * sizeof(int));
  GPU_storagebuf_update(buf, g.node_verts.data());

  g.pbvh = &pbvh;
  g.nodes_num = nodes.size();
  g.verts_num = verts_num;
  g.positions_uploaded = false;
  return true;
}

bool offset_positions(const Depsgraph &depsgraph,
                      const Sculpt &sd,
                      Object &object,
                      const Brush &brush,
                      const float3 &offset,
                      const IndexMask &node_mask)
{
  if (mode() == Mode::Off) {
    return false;
  }
  if (!can_use(depsgraph, sd, object, brush)) {
    return false;
  }

  const double t0 = BLI_time_now_seconds();

  SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  Mesh &mesh = *id_cast<Mesh *>(object.data);
  /* ★常駐させるのは**評価後の位置**。ブラシが読むのも PBVH と描画が見るのもこちら。
   * 元の位置(`orig`)には、書き戻しの時に crazyspace を掛けた移動量を足す。
   * `for_write` を先に呼ぶ — 共有されている配列はここで複製されることがあり、
   * 先に const 側で比べると同じに見えて後から別物になる。 */
  MutableSpan<float3> positions = bke::pbvh::vert_positions_eval_for_write(depsgraph, object);
  MutableSpan<float3> positions_orig = mesh.vert_positions_for_write();
  if (positions.is_empty() || positions_orig.is_empty()) {
    return false;
  }
  const bool eval_is_orig = positions.data() == positions_orig.data();
  const Span<float3x3> deform_imats = ss.deform_imats;

  if (!ensure_topology(pbvh, positions.size())) {
    return false;
  }

  /* --- 1ダブぶん: 触ったノードの位置だけを作る(数百 int) --- */
  const int64_t nodes_touched = node_mask.size();
  gpu::StorageBuf *b_starts = ensure_grow(
      g.b_starts, size_t(nodes_touched) * sizeof(int), "falcon_resident_starts");
  gpu::StorageBuf *b_toff = ensure_grow(
      g.b_toff, size_t(nodes_touched + 1) * sizeof(int), "falcon_resident_toff");
  const MutableSpan<int> starts = fit(g.starts, g.b_starts);
  const MutableSpan<int> toff = fit(g.toff, g.b_toff);
  int64_t total = 0;
  int64_t slot = 0;
  node_mask.foreach_index([&](const int i) {
    starts[slot] = g.node_offsets[i];
    toff[slot] = int(total);
    total += nodes[i].verts().size();
    slot++;
  });
  toff[nodes_touched] = int(total);
  if (total == 0) {
    return false;
  }

  const bool use_front_face = (brush.flag & BRUSH_FRONTFACE) != 0;

  gpu::StorageBuf *b_normals = ensure_grow(
      g.b_normals,
      use_front_face ? size_t(total) * sizeof(float3) : sizeof(float3),
      "falcon_resident_normals");
  gpu::StorageBuf *b_out = ensure_grow(
      g.b_out, size_t(total) * sizeof(float3), "falcon_resident_out");

  /* 表向きの面だけに絞る時だけ法線が要る。**既定では要らない**ので、
   * 既定の道では 1ダブあたりの CPU の仕事はここまでで終わる。 */
  if (use_front_face) {
    const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
    if (vert_normals.is_empty()) {
      return false;
    }
    fit(g.normals_packed, g.b_normals);
    threading::parallel_for(IndexRange(nodes_touched), 8, [&](const IndexRange range) {
      for (const int64_t j : range) {
        const int base = starts[j];
        const int at = toff[j];
        const int n = toff[j + 1] - at;
        for (const int k : IndexRange(n)) {
          g.normals_packed[at + k] = vert_normals[g.node_verts[base + k]];
        }
      }
    });
  }

  const double t1 = BLI_time_now_seconds();

  /* --- 送る --- */
  gpu::StorageBuf *b_positions = ensure_exact(
      g.b_positions, size_t(positions.size()) * sizeof(float3), "falcon_resident_positions");
  const bool upload_positions = !g.positions_uploaded || mode() == Mode::Verify;
  if (upload_positions) {
    GPU_storagebuf_update(b_positions, positions.data());
    g.positions_uploaded = true;
  }

  /* マスク。無ければダミーを1つ結ぶ(GLSL 側は結ばれていないと読めない)。
   * ★「マスクが無い時は CPU へ落とす」にはできない。一度でもマスクを使った
   * メッシュは全部消した後も値が全部ゼロのまま `.sculpt_mask` を持ち続ける。 */
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<float> mask = *attributes.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
  const bool use_mask = !mask.is_empty();
  const float mask_dummy = 0.0f;
  const size_t mask_bytes = use_mask ? size_t(mask.size()) * sizeof(float) : sizeof(float);
  const bool mask_is_new = (g.b_mask.buf == nullptr) || (g.b_mask.capacity != mask_bytes);
  gpu::StorageBuf *b_mask = ensure_exact(g.b_mask, mask_bytes, "falcon_resident_mask");
  /* ★マスクはストロークの間ずっと同じなので、送るのは1回だけ。
   * 毎ダブ送っていた時は 400万頂点で **5.12ms/ダブ**を払っていた(実測)。
   * 位置と同じ時に送り直す(`positions_uploaded` が落ちた時 = ストロークの初め)。 */
  if (mask_is_new || upload_positions) {
    GPU_storagebuf_update(b_mask,
                          use_mask ? static_cast<const void *>(mask.data()) :
                                     static_cast<const void *>(&mask_dummy));
  }

  /* 曲線。半径が変わった時だけ焼き直す(筆圧で毎ダブ変わることもある)。 */
  gpu::StorageBuf *b_lut = ensure_exact(
      g.b_lut, size_t(CURVE_LUT_SIZE) * sizeof(float), "falcon_resident_lut");
  if (g.lut_radius != cache.radius) {
    Array<float> lut(CURVE_LUT_SIZE);
    bake_curve_lut(brush, cache.radius, lut);
    GPU_storagebuf_usage_size_set(b_lut, size_t(CURVE_LUT_SIZE) * sizeof(float));
    GPU_storagebuf_update(b_lut, lut.data());
    g.lut_radius = cache.radius;
  }

  GPU_storagebuf_update(b_starts, starts.data());
  GPU_storagebuf_update(b_toff, toff.data());

  if (use_front_face) {
    GPU_storagebuf_update(b_normals, g.normals_packed.data());
  }
  else {
    const float3 normal_dummy(0.0f);
    GPU_storagebuf_update(b_normals, &normal_dummy);
  }

  const double t2 = BLI_time_now_seconds();

  /* --- 走らせる --- */
  static gpu::Shader *shader = nullptr;
  if (shader == nullptr) {
    shader = GPU_shader_create_from_info_name("falcon_sculpt_offset");
    if (shader == nullptr) {
      return false;
    }
  }
  GPU_shader_bind(shader);

  float3 axis_scale(1.0f);
  for (const int axis : IndexRange(3)) {
    if (sd.flags & (SCULPT_LOCK_X << axis)) {
      axis_scale[axis] = 0.0f;
    }
  }

  GPU_shader_uniform_3fv(shader, "brush_location", cache.location_symm);
  GPU_shader_uniform_3fv(shader, "view_normal", cache.view_normal_symm);
  GPU_shader_uniform_3fv(shader, "brush_offset", offset);
  GPU_shader_uniform_3fv(shader, "axis_scale", axis_scale);
  GPU_shader_uniform_1f(shader, "brush_radius", cache.radius);
  GPU_shader_uniform_1f(shader, "brush_hardness", cache.hardness);
  GPU_shader_uniform_1i(shader, "vert_count", int(total));
  GPU_shader_uniform_1i(shader, "curve_lut_size", CURVE_LUT_SIZE);
  GPU_shader_uniform_1i(shader, "node_count", int(nodes_touched));
  GPU_shader_uniform_1b(shader, "use_front_face", use_front_face);
  GPU_shader_uniform_1b(shader, "use_mask", use_mask);
  GPU_shader_uniform_1b(shader, "write_inplace", mode() != Mode::Verify);

  GPU_storagebuf_bind(g.b_node_verts.buf, GPU_shader_get_ssbo_binding(shader, "node_verts"));
  GPU_storagebuf_bind(b_positions, GPU_shader_get_ssbo_binding(shader, "positions"));
  GPU_storagebuf_bind(b_normals, GPU_shader_get_ssbo_binding(shader, "normals"));
  GPU_storagebuf_bind(b_lut, GPU_shader_get_ssbo_binding(shader, "curve_lut"));
  GPU_storagebuf_bind(b_mask, GPU_shader_get_ssbo_binding(shader, "vert_mask"));
  GPU_storagebuf_bind(b_out, GPU_shader_get_ssbo_binding(shader, "out_translations"));
  GPU_storagebuf_bind(b_starts, GPU_shader_get_ssbo_binding(shader, "node_starts"));
  GPU_storagebuf_bind(b_toff, GPU_shader_get_ssbo_binding(shader, "touched_offsets"));

  GPU_compute_dispatch(shader, uint((total + GROUP_SIZE - 1) / GROUP_SIZE), 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  GPU_shader_unbind();

  const double t3 = BLI_time_now_seconds();

  /* --- 戻す ---
   * ★この段ではまだ毎ダブ戻している。法線・境界・undo を GPU へ移した段で
   * 「描画の時だけ戻す」に変える。ここが今どれだけ効いているかを別に測っておく。 */
  fit(g.out_translations, g.b_out);
  GPU_storagebuf_read(b_out, g.out_translations.data());

  const double t4 = BLI_time_now_seconds();

  if (mode() == Mode::Verify) {
    /* 位置は書き換えていない。CPU の通路がこの後そのまま走る。
     * 突き合わせのために、CPU が動かす**前**の評価後の位置を控える。 */
    g.before.reinitialize(total);
    threading::parallel_for(IndexRange(nodes_touched), 8, [&](const IndexRange range) {
      for (const int64_t j : range) {
        const int base = starts[j];
        const int at = toff[j];
        const int n = toff[j + 1] - at;
        for (const int k : IndexRange(n)) {
          g.before[at + k] = positions[g.node_verts[base + k]];
        }
      }
    });
    g.last_total = total;
    g.last_nodes = nodes_touched;
  }
  else {
    /* ★GPU は**評価後の位置**だけを書き換えた。CPU 側には
     *   ① 評価後の配列に同じ移動量を足す(常駐と CPU をそろえる)
     *   ② 元の配列に crazyspace を掛けた移動量を足す
     * を入れる。CPU 側 `PositionDeformData::deform` と同じ順番。 */
    threading::parallel_for(IndexRange(nodes_touched), 8, [&](const IndexRange range) {
      for (const int64_t j : range) {
        const int base = starts[j];
        const int at = toff[j];
        const int n = toff[j + 1] - at;
        for (const int k : IndexRange(n)) {
          const int vert = g.node_verts[base + k];
          float3 t = g.out_translations[at + k];
          if (!eval_is_orig) {
            positions[vert] += t;
          }
          if (!deform_imats.is_empty()) {
            t = math::transform_point(deform_imats[vert], t);
          }
          positions_orig[vert] += t;
        }
      }
    });
  }

  const double t5 = BLI_time_now_seconds();

  t_setup.fetch_add(int64_t((t1 - t0) * 1.0e9), std::memory_order_relaxed);
  t_upload.fetch_add(int64_t((t2 - t1) * 1.0e9), std::memory_order_relaxed);
  t_dispatch.fetch_add(int64_t((t3 - t2) * 1.0e9), std::memory_order_relaxed);
  t_read.fetch_add(int64_t((t4 - t3) * 1.0e9), std::memory_order_relaxed);
  t_scatter.fetch_add(int64_t((t5 - t4) * 1.0e9), std::memory_order_relaxed);
  n_dabs.fetch_add(1, std::memory_order_relaxed);
  n_verts.fetch_add(total, std::memory_order_relaxed);

  return mode() != Mode::Verify;
}

void note_after_cpu(const Depsgraph &depsgraph, const Object &object, const IndexMask &node_mask)
{
  if (mode() != Mode::Verify || g.last_total == 0) {
    return;
  }
  UNUSED_VARS(node_mask);
  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);

  int64_t max_micro = 0;
  for (const int64_t j : IndexRange(g.last_nodes)) {
    const int base = g.starts[j];
    const int at = g.toff[j];
    const int n = g.toff[j + 1] - at;
    for (const int k : IndexRange(n)) {
      /* CPU が実際に動かした量 = 今の位置 - 動かす前の位置。GPU の移動量と比べる。 */
      const float3 cpu_t = positions[g.node_verts[base + k]] - g.before[at + k];
      const float3 diff = cpu_t - g.out_translations[at + k];
      const float d = math::max(math::max(std::abs(diff.x), std::abs(diff.y)), std::abs(diff.z));
      max_micro = std::max(max_micro, int64_t(d * 1.0e6f));
    }
  }
  int64_t prev = v_max_micro.load(std::memory_order_relaxed);
  while (max_micro > prev &&
         !v_max_micro.compare_exchange_weak(prev, max_micro, std::memory_order_relaxed))
  {
  }
  v_count.fetch_add(g.last_total, std::memory_order_relaxed);
  g.last_total = 0;
}

/** \} */

}  // namespace blender::ed::sculpt_paint::falcon_resident
