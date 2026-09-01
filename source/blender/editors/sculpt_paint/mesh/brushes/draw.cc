/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "editors/sculpt_paint/mesh/brushes/brushes.hh"

#include "DNA_brush_types.h"
#include "DNA_mesh_types.h"
#include "DNA_object_types.h"
#include "DNA_scene_types.h"

#include "BKE_mesh.hh"
#include "BKE_object_types.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "BLI_enumerable_thread_specific.hh"
#include "BLI_math_vector.hh"
#include "BLI_task.hh"

#include "editors/sculpt_paint/mesh/falcon_sculpt_gpu.hh"
#include "editors/sculpt_paint/mesh/falcon_sculpt_resident.hh"
#include "editors/sculpt_paint/mesh/mesh_brush_common.hh"
#include "editors/sculpt_paint/mesh/sculpt_automask.hh"
#include "editors/sculpt_paint/mesh/sculpt_intern.hh"

#include "bmesh.hh"

namespace blender::ed::sculpt_paint::brushes {

inline namespace draw_cc {

struct LocalData {
  Vector<float3> positions;
  Vector<float> factors;
  Vector<float> distances;
  Vector<float3> translations;
};

static void calc_faces(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       const Brush &brush,
                       const float3 &offset,
                       const MeshAttributeData &attribute_data,
                       const Span<float3> vert_normals,
                       const bke::pbvh::MeshNode &node,
                       Object &object,
                       LocalData &tls,
                       const PositionDeformData &position_data,
                       const Span<float> gpu_factors)
{
  const SculptSession &ss = *object.runtime->sculpt_session;

  const Span<int> verts = node.verts();

  /* ★GPUが出した係数があればそれを使う。空ならCPU(公式通路)。
   * `verify` の時は**両方出して突き合わせ、CPU の値を使う**ので、
   * 絵は1画素も変わらないまま差だけが分かる。 */
  if (gpu_factors.is_empty() || falcon_gpu::verify_mode()) {
    calc_factors_common_mesh_indexed(depsgraph,
                                     brush,
                                     object,
                                     attribute_data,
                                     position_data.eval,
                                     vert_normals,
                                     node,
                                     tls.factors,
                                     tls.distances);
    if (!gpu_factors.is_empty()) {
      BLI_assert(gpu_factors.size() == tls.factors.size());
      for (const int i : tls.factors.index_range()) {
        falcon_gpu::note_diff(std::abs(tls.factors[i] - gpu_factors[i]));
      }
    }
  }
  else {
    tls.factors.resize(verts.size());
    tls.factors.as_mutable_span().copy_from(gpu_factors);
  }

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, position_data.eval, verts, translations);
  position_data.deform(translations, verts);
}

static void calc_grids(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &offset,
                       const bke::pbvh::GridsNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;
  SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;

  const Span<int> grids = node.grids();
  const MutableSpan positions = gather_grids_positions(subdiv_ccg, grids, tls.positions);

  calc_factors_common_grids(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  tls.translations.resize(positions.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, grids, subdiv_ccg);
}

static void calc_bmesh(const Depsgraph &depsgraph,
                       const Sculpt &sd,
                       Object &object,
                       const Brush &brush,
                       const float3 &offset,
                       bke::pbvh::BMeshNode &node,
                       LocalData &tls)
{
  SculptSession &ss = *object.runtime->sculpt_session;

  const Set<BMVert *, 0> &verts = BKE_pbvh_bmesh_node_unique_verts(&node);
  const MutableSpan positions = gather_bmesh_positions(verts, tls.positions);

  calc_factors_common_bmesh(depsgraph, brush, object, positions, node, tls.factors, tls.distances);

  tls.translations.resize(verts.size());
  const MutableSpan<float3> translations = tls.translations;
  translations_from_offset_and_factors(offset, tls.factors, translations);

  clip_and_lock_translations(sd, ss, positions, translations);
  apply_translations(translations, verts);
}

}  // namespace draw_cc

static void offset_positions(const Depsgraph &depsgraph,
                             const Sculpt &sd,
                             Object &object,
                             const float3 &offset,
                             const IndexMask &node_mask)
{
  bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  /* ★係数をGPUで出す(`FALCON_SCULPT_GPU` が立っている時だけ)。
   * ノードごとに投げず**1回のディスパッチでまとめて**出す。回数がそのまま
   * 時間になるため(Taichi の実測で GPU 0.9ms に対し壁時計 17.9ms、
   * 律速はカーネル発行回数だった)。
   * 条件が合わなければ false が返り、そのままCPUの通路を通る。 */
  Vector<int> gpu_offsets;
  Vector<float> gpu_factors;
  bool use_gpu = false;

  threading::EnumerableThreadSpecific<LocalData> all_tls;
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      /* ★ブラシ本体をまるごとGPUで回す(`FALCON_SCULPT_GPU=resident`)。
       * 頂点はGPUに常駐し、係数の適用も書き戻しもGPUで済む。CPU に残るのは
       * ノード探索とノードの境界だけ = 実測の床 0.45ms/ダブ(400万面)。
       *
       * 下の `falcon_gpu::calc_factors`(係数だけGPU)とは**別の道**。
       * そちらは 2026-08-17 に実測で負けたことが分かっている
       * (転送も読み戻しも消した probe で 110.9ms 対 CPU 71.7ms)。
       * 負けた原因はGPUの周りに残った CPU の世話なので、それごと移したのがこちら。 */
      if (falcon_resident::offset_positions(depsgraph, sd, object, brush, offset, node_mask)) {
        const PositionDeformData position_data(depsgraph, object);
        MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
        node_mask.foreach_index(
            [&](const int i) {
              bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
            },
            exec_mode::grain_size(1));
        break;
      }
      if (falcon_gpu::can_use_factors(depsgraph, sd.paint, object, brush)) {
        use_gpu = falcon_gpu::calc_factors(
            depsgraph, object, brush, node_mask, gpu_offsets, gpu_factors);
      }
      const Mesh &mesh = *id_cast<Mesh *>(object.data);
      const MeshAttributeData attribute_data(mesh);
      const PositionDeformData position_data(depsgraph, object);
      const Span<float3> vert_normals = bke::pbvh::vert_normals_eval(depsgraph, object);
      MutableSpan<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      node_mask.foreach_index(
          [&](const int i, const int64_t pos) {
            LocalData &tls = all_tls.local();
            const Span<float> node_gpu_factors =
                use_gpu ? Span<float>(gpu_factors).slice(gpu_offsets[pos],
                                                         gpu_offsets[pos + 1] - gpu_offsets[pos]) :
                          Span<float>();
            calc_faces(depsgraph,
                       sd,
                       brush,
                       offset,
                       attribute_data,
                       vert_normals,
                       nodes[i],
                       object,
                       tls,
                       position_data,
                       node_gpu_factors);
            bke::pbvh::update_node_bounds_mesh(position_data.eval, nodes[i]);
          },
          exec_mode::grain_size(1));
      /* `resident-verify` の時だけ、CPU が出した位置と GPU の結果を突き合わせる。
       * GPU は書き込んでいないので**絵は1画素も変わらない**。 */
      falcon_resident::note_after_cpu(depsgraph, object, node_mask);
      break;
    }
    case bke::pbvh::Type::Grids: {
      SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      MutableSpan<float3> positions = subdiv_ccg.positions;
      MutableSpan<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_grids(depsgraph, sd, object, brush, offset, nodes[i], tls);
            bke::pbvh::update_node_bounds_grids(subdiv_ccg.grid_area, positions, nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      MutableSpan<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      node_mask.foreach_index(
          [&](const int i) {
            LocalData &tls = all_tls.local();
            calc_bmesh(depsgraph, sd, object, brush, offset, nodes[i], tls);
            bke::pbvh::update_node_bounds_bmesh(nodes[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
  }
  pbvh.tag_positions_changed(node_mask);
  pbvh.flush_bounds_to_parents();
}

void do_draw_brush(const Depsgraph &depsgraph,
                   const Sculpt &sd,
                   Object &object,
                   const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;
  const Brush &brush = *BKE_paint_brush_for_read(&sd.paint);

  const float3 effective_normal = tilt_effective_normal_get(ss, brush);

  const float3 offset = effective_normal * ss.cache->radius * ss.cache->scale *
                        ss.cache->bstrength;

  offset_positions(depsgraph, sd, object, offset, node_mask);
}

void do_nudge_brush(const Depsgraph &depsgraph,
                    const Sculpt &sd,
                    Object &object,
                    const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;

  const float3 offset = math::cross(
      math::cross(ss.cache->sculpt_normal_symm, ss.cache->grab_delta_symm),
      ss.cache->sculpt_normal_symm);

  offset_positions(depsgraph, sd, object, offset * ss.cache->bstrength, node_mask);
}

void do_gravity_brush(const Depsgraph &depsgraph,
                      const Sculpt &sd,
                      Object &object,
                      const IndexMask &node_mask)
{
  PRF_scope(ProfileCategory::Editor);
  const SculptSession &ss = *object.runtime->sculpt_session;

  const float3 offset = ss.cache->gravity_direction_symm * -ss.cache->radius_squared *
                        ss.cache->scale * sd.gravity_factor;

  offset_positions(depsgraph, sd, object, offset, node_mask);
}

}  // namespace blender::ed::sculpt_paint::brushes
