/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 */

#include "falcon_sculpt_gpu.hh"

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"
#include "DNA_view3d_types.h"

#include "BKE_brush.hh"
#include "BKE_attribute.hh"
#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"

#include "BLI_math_vector.hh"
#include "BLI_span.hh"

#include "BLI_time.h"

#include "GPU_compute.hh"
#include "GPU_context.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_storage_buffer.hh"

#include "ED_view3d.hh"

#include "mesh_brush_common.hh"
#include "sculpt_automask.hh"
#include "sculpt_intern.hh"

namespace blender::ed::sculpt_paint::falcon_gpu {

/* 落とし方の曲線を焼く段数。
 * 表を引いて線形に補間するので、CPU の曲線評価とは**わずかにずれる**。
 * 1024 段なら、半径の 1/1024 の距離差でしか差が出ない。 */
static constexpr int CURVE_LUT_SIZE = 1024;

/* 1グループのスレッド数。シェーダ側の LOCAL_GROUP_SIZE と揃える。 */
static constexpr int GROUP_SIZE = 256;

bool enabled()
{
  static const bool on = (getenv("FALCON_SCULPT_GPU") != nullptr);
  return on;
}

static const char *env_value()
{
  static const char *v = getenv("FALCON_SCULPT_GPU");
  return v;
}

bool verify_mode()
{
  const char *v = env_value();
  return v != nullptr && STREQ(v, "verify");
}

/* ★`FALCON_SCULPT_GPU=probe` = **時間だけ測るための嘘のモード**。
 *
 * 位置と法線の送り直しをやめ(最初の1回だけ送る)、読み戻しもやめる。
 * 当然**絵は正しくない**(古い位置で係数を出し、結果は持ち帰らない)。
 *
 * 何のために要るか: 「位置をGPUに置きっぱなしにして何も持ち帰らない」形に
 * 作り替えた場合の**天井**を、作り替える前に知るため。PBVH・undo・描画の
 * 3つに手を入れてから「思ったより速くならなかった」では戻すのが重い。
 *
 * 実測(2026-08-17)の内訳は 転送 1.1〜1.5ms / 実行 0.01ms / 読み戻し 1.4〜8.5ms。
 * この2つを外した時に何ms残るかが、作り替えで狙える下限になる。 */
static bool probe_mode()
{
  const char *v = env_value();
  return v != nullptr && STREQ(v, "probe");
}

/* 突き合わせの結果。スレッドから触るので atomic にする。
 * float の atomic max は面倒なので、**1e6 倍した整数**で持つ
 * (差は 0..1 の係数なので、この分解能で足りる)。 */
static std::atomic<int64_t> g_max_diff_micro{0};
static std::atomic<int64_t> g_diff_count{0};
static std::atomic<int64_t> g_sum_diff_micro{0};

void note_diff(const float diff)
{
  const int64_t micro = int64_t(diff * 1.0e6f);
  int64_t prev = g_max_diff_micro.load(std::memory_order_relaxed);
  while (micro > prev &&
         !g_max_diff_micro.compare_exchange_weak(prev, micro, std::memory_order_relaxed))
  {
    /* prev は失敗時に更新される */
  }
  g_sum_diff_micro.fetch_add(micro, std::memory_order_relaxed);
  g_diff_count.fetch_add(1, std::memory_order_relaxed);
}

/* GPU側の内訳。転送・実行・読み戻しのどれが効いているかで、
 * 次の設計(位置をGPUに置きっぱなしにするか)の判断が変わる。 */
static std::atomic<int64_t> g_up_ns{0}, g_dispatch_ns{0}, g_read_ns{0}, g_dabs{0};

void report_gpu_breakdown()
{
  const int64_t n = g_dabs.exchange(0, std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  const double up = double(g_up_ns.exchange(0, std::memory_order_relaxed)) / 1.0e6 / double(n);
  const double di = double(g_dispatch_ns.exchange(0, std::memory_order_relaxed)) / 1.0e6 /
                    double(n);
  const double rd = double(g_read_ns.exchange(0, std::memory_order_relaxed)) / 1.0e6 / double(n);
  printf("FALCON_SCULPT_GPU 内訳 転送 %.2fms / 実行 %.2fms / 読み戻し %.2fms (ダブ%ld回)\n",
         up,
         di,
         rd,
         long(n));
  fflush(stdout);
}

void report_verify()
{
  const int64_t n = g_diff_count.exchange(0, std::memory_order_relaxed);
  if (n == 0) {
    return;
  }
  const int64_t max_micro = g_max_diff_micro.exchange(0, std::memory_order_relaxed);
  const int64_t sum_micro = g_sum_diff_micro.exchange(0, std::memory_order_relaxed);
  printf("FALCON_SCULPT_GPU 突き合わせ %ld頂点  最大差 %.6f  平均差 %.8f\n",
         long(n),
         double(max_micro) * 1.0e-6,
         double(sum_micro) * 1.0e-6 / double(n));
  fflush(stdout);
}

bool can_use_factors(const Depsgraph &depsgraph,
                     const Paint &paint,
                     const Object &object,
                     const Brush &brush)
{
  if (!enabled()) {
    return false;
  }

  /* GPU の文脈が無ければ何も作れない。ダブの最中は生きていると実測したが、
   * 経路によっては無いことがあるので毎回見る(落ちるより落とすほうがよい)。 */
  if (GPU_context_active_get() == nullptr) {
    return false;
  }

  const SculptSession *ss = object.runtime->sculpt_session;
  if (ss == nullptr || ss->cache == nullptr) {
    return false;
  }

  const bke::pbvh::Tree *pbvh = bke::object::pbvh_get(const_cast<Object &>(object));
  if (pbvh == nullptr || pbvh->type() != bke::pbvh::Type::Mesh) {
    return false;
  }

  /* 距離の出し方が違うので、TUBE は別物として扱う。 */
  if (eBrushFalloffShape(brush.falloff_shape) != PAINT_FALLOFF_SHAPE_SPHERE) {
    return false;
  }

  /* 隠した頂点・自動マスク・テクスチャは**まだ再現していない**。
   * 通してしまうと、抜けた項目のぶんだけ静かに強くなる。 */
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  if (mesh.attributes().contains(".hide_vert")) {
    return false;
  }
  if (auto_mask::is_enabled(paint, object, &brush)) {
    return false;
  }
  const MTex *mtex = BKE_brush_mask_texture_get(&brush, OB_MODE_SCULPT);
  if (mtex && mtex->tex) {
    return false;
  }

  /* 視点のクリップ面が入っていると、面の外を落とす処理が要る。 */
  const RegionView3D *rv3d = ss->cache->vc ? ss->cache->vc->rv3d : ss->rv3d;
  const View3D *v3d = ss->cache->vc ? ss->cache->vc->v3d : ss->v3d;
  if (RV3D_CLIPPING_ENABLED(v3d, rv3d)) {
    return false;
  }

  return true;
}

/* 曲線を表に焼く。`BKE_brush_calc_curve_factors` は係数に**掛け算で足す**ので、
 * 1.0 で埋めてから通して、出てきた値をそのまま表にする。
 * プリセットにも自作カーブにも同じ関数を通すので、**GPU側で分岐を再現しない**。 */
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

bool calc_factors(const Depsgraph &depsgraph,
                  const Object &object,
                  const Brush &brush,
                  const IndexMask &node_mask,
                  Vector<int> &r_offsets,
                  Vector<float> &r_factors)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const StrokeCache &cache = *ss.cache;

  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(const_cast<Object &>(object));
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

  /* --- 触る頂点を1本に連結する --- */
  r_offsets.clear();
  r_offsets.reserve(node_mask.size() + 1);
  int total = 0;
  node_mask.foreach_index([&](const int i) {
    r_offsets.append(total);
    total += nodes[i].verts().size();
  });
  r_offsets.append(total);
  if (total == 0) {
    return false;
  }

  Array<int> vert_indices(total);
  int at = 0;
  node_mask.foreach_index([&](const int i) {
    const Span<int> verts = nodes[i].verts();
    std::copy(verts.begin(), verts.end(), vert_indices.begin() + at);
    at += verts.size();
  });

  const Span<float3> positions = bke::pbvh::vert_positions_eval(depsgraph, object);
  const Span<float3> normals = bke::pbvh::vert_normals_eval(depsgraph, object);
  if (positions.is_empty() || normals.is_empty()) {
    return false;
  }

  Array<float> lut(CURVE_LUT_SIZE);
  bake_curve_lut(brush, cache.radius, lut);

  /* マスク。無ければダミーを1つだけ結ぶ(GLSL側は結ばれていないと読めない)。 */
  const Mesh &mesh = *id_cast<const Mesh *>(object.data);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan<float> mask = *attributes.lookup<float>(".sculpt_mask",
                                                           bke::AttrDomain::Point);
  const bool use_mask = !mask.is_empty();
  const float mask_dummy = 0.0f;

  /* ★シェーダとバッファは使い回す。
   *
   * 最初の版は**毎ダブ作り直していた**。結果は CPU 2.2ms に対して GPU 24ms で、
   * 10倍遅かった。作る費用がそのまま毎回乗る。
   * 大きさが変わった時だけ作り直す形にする。 */
  static gpu::Shader *shader = nullptr;
  if (shader == nullptr) {
    shader = GPU_shader_create_from_info_name("falcon_sculpt_factors");
    if (shader == nullptr) {
      return false;
    }
  }

  struct CachedBuf {
    gpu::StorageBuf *buf = nullptr;
    size_t size = 0;
  };
  static CachedBuf c_indices, c_positions, c_normals, c_lut, c_mask, c_out;

  auto ensure = [](CachedBuf &c, const size_t size, const void *data, const char *name) {
    if (c.buf != nullptr && c.size != size) {
      GPU_storagebuf_free(c.buf);
      c.buf = nullptr;
    }
    if (c.buf == nullptr) {
      c.buf = GPU_storagebuf_create_ex(size, data, GPU_USAGE_DYNAMIC, name);
      c.size = size;
    }
    else if (data != nullptr) {
      GPU_storagebuf_update(c.buf, data);
    }
    return c.buf;
  };

  const double t_up0 = BLI_time_now_seconds();
  gpu::StorageBuf *buf_indices = ensure(
      c_indices, size_t(total) * sizeof(int), vert_indices.data(), "falcon_sculpt_indices");
  /* probe の時は2回目以降そのままにする(= GPU常駐を装う) */
  const bool skip_resend = probe_mode() && c_positions.buf != nullptr;
  gpu::StorageBuf *buf_positions = ensure(c_positions,
                                          size_t(positions.size()) * sizeof(float3),
                                          skip_resend ? nullptr : positions.data(),
                                          "falcon_sculpt_positions");
  gpu::StorageBuf *buf_normals = ensure(c_normals,
                                        size_t(normals.size()) * sizeof(float3),
                                        skip_resend ? nullptr : normals.data(),
                                        "falcon_sculpt_normals");
  gpu::StorageBuf *buf_lut = ensure(
      c_lut, size_t(lut.size()) * sizeof(float), lut.data(), "falcon_sculpt_lut");
  gpu::StorageBuf *buf_mask = ensure(c_mask,
                                     use_mask ? size_t(mask.size()) * sizeof(float) :
                                                sizeof(float),
                                     use_mask ? static_cast<const void *>(mask.data()) :
                                                static_cast<const void *>(&mask_dummy),
                                     "falcon_sculpt_mask");
  gpu::StorageBuf *buf_out = ensure(
      c_out, size_t(total) * sizeof(float), nullptr, "falcon_sculpt_factors_out");

  const double t_up1 = BLI_time_now_seconds();

  GPU_shader_bind(shader);

  const float3 location = cache.location_symm;
  const float3 view_normal = cache.view_normal_symm;
  GPU_shader_uniform_3fv(shader, "brush_location", location);
  GPU_shader_uniform_3fv(shader, "view_normal", view_normal);
  GPU_shader_uniform_1f(shader, "brush_radius", cache.radius);
  GPU_shader_uniform_1f(shader, "brush_hardness", cache.hardness);
  GPU_shader_uniform_1i(shader, "vert_count", total);
  GPU_shader_uniform_1i(shader, "curve_lut_size", CURVE_LUT_SIZE);
  GPU_shader_uniform_1b(shader, "use_front_face", (brush.flag & BRUSH_FRONTFACE) != 0);
  GPU_shader_uniform_1b(shader, "use_mask", use_mask);

  GPU_storagebuf_bind(buf_indices, GPU_shader_get_ssbo_binding(shader, "vert_indices"));
  GPU_storagebuf_bind(buf_positions, GPU_shader_get_ssbo_binding(shader, "positions"));
  GPU_storagebuf_bind(buf_normals, GPU_shader_get_ssbo_binding(shader, "normals"));
  GPU_storagebuf_bind(buf_lut, GPU_shader_get_ssbo_binding(shader, "curve_lut"));
  GPU_storagebuf_bind(buf_mask, GPU_shader_get_ssbo_binding(shader, "vert_mask"));
  GPU_storagebuf_bind(buf_out, GPU_shader_get_ssbo_binding(shader, "factors"));

  const double t_d0 = BLI_time_now_seconds();
  GPU_compute_dispatch(shader, (total + GROUP_SIZE - 1) / GROUP_SIZE, 1, 1);
  GPU_memory_barrier(GPU_BARRIER_SHADER_STORAGE);
  const double t_d1 = BLI_time_now_seconds();

  r_factors.resize(total);
  if (!probe_mode()) {
    GPU_storagebuf_read(buf_out, r_factors.data());
  }
  const double t_r1 = BLI_time_now_seconds();

  g_up_ns.fetch_add(int64_t((t_up1 - t_up0) * 1.0e9), std::memory_order_relaxed);
  g_dispatch_ns.fetch_add(int64_t((t_d1 - t_d0) * 1.0e9), std::memory_order_relaxed);
  g_read_ns.fetch_add(int64_t((t_r1 - t_d1) * 1.0e9), std::memory_order_relaxed);
  g_dabs.fetch_add(1, std::memory_order_relaxed);

  GPU_shader_unbind();
  /* ★ここでは解放しない。使い回すのが目的なので、大きさが変わった時に
   * `ensure` の中で作り直す。終了時に残るが、環境変数で入る実験の通路なので
   * 今はそれでよい(常用に格上げする時に GPU の後始末へ登録する)。 */

  return true;
}

}  // namespace blender::ed::sculpt_paint::falcon_gpu
