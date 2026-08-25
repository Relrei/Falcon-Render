/* SPDX-FileCopyrightText: 2024 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup gpu
 *
 * bke::pbvh::Tree drawing.
 * Embeds GPU meshes inside of bke::pbvh::Tree nodes, used by mesh sculpt mode.
 */

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "BLI_map.hh"
#include "BLI_math_geom.h"
#include "BLI_time.h"
#include "BLI_math_vector_types.hh"
#include "BLI_task.hh"
#include "BLI_utildefines.h"
#include "BLI_vector.hh"

#include "DNA_object_types.h"

#include "BKE_attribute.hh"
#include "BKE_attribute_legacy_convert.hh"
#include "BKE_attribute_math.hh"
#include "BKE_customdata.hh"
#include "BKE_mesh.hh"
#include "BKE_paint.hh"
#include "BKE_paint_bvh.hh"
#include "BKE_subdiv_ccg.hh"

#include "DEG_depsgraph_query.hh"

#include "GPU_batch.hh"

#include "DRW_engine.hh"
#include "DRW_pbvh.hh"
#include "DRW_render.hh"

#include "attribute_convert.hh"
#include "bmesh.hh"

namespace blender {

template<> struct DefaultHash<draw::pbvh::AttributeRequest> {
  uint64_t operator()(const draw::pbvh::AttributeRequest &value) const
  {
    using namespace draw::pbvh;
    if (const CustomRequest *request_type = std::get_if<CustomRequest>(&value)) {
      return get_default_hash(*request_type);
    }
    const GenericRequest &attr = std::get<GenericRequest>(value);
    return get_default_hash(attr);
  }
};

namespace draw::pbvh {

uint64_t ViewportRequest::hash() const
{
  return get_default_hash(attributes, use_coarse_grids);
}

/**
 * Because many sculpt mode operations skip tagging dependency graph for reevaluation for
 * performance reasons, the relevant data must be retrieved directly from the original mesh rather
 * than the evaluated copy.
 */
struct OrigMeshData {
  StringRef active_color;
  StringRef default_color;
  StringRef active_uv_map;
  StringRef default_uv_map;
  int face_set_default;
  int face_set_seed;
  bke::AttributeAccessor attributes;
  OrigMeshData(const Mesh &mesh)
      : active_color(mesh.active_color_attribute),
        default_color(mesh.default_color_attribute),
        active_uv_map(mesh.active_uv_map_name()),
        default_uv_map(mesh.default_uv_map_name()),
        face_set_default(mesh.face_sets_color_default),
        face_set_seed(mesh.face_sets_color_seed),
        attributes(mesh.attributes())
  {
  }
};

/**
 * Stores the data necessary to draw the PBVH geometry. A separate `*Impl` class is used to hide
 * implementation details from the public header.
 */
class DrawCacheImpl : public DrawCache {
  struct AttributeData {
    /** A vertex buffer for each BVH node. If null, the draw data for the node must be created. */
    Vector<gpu::VertBufPtr> vbos;
    /**
     * A separate "dirty" bit per node. We track the dirty value separately from deleting the VBO
     * for a node in order to avoid recreating batches with new VBOs. It's also a necessary
     * addition to the flags stored in the PBVH which are cleared after it's used for drawing
     * (those aren't sufficient when multiple viewports are drawing with the same PBVH but request
     * different sets of attributes).
     */
    BitVector<> dirty_nodes;
    /**
     * Mark attribute values dirty for specific nodes. The next time the attribute is requested,
     * the values will be extracted again.
     */
    void tag_dirty(const IndexMask &node_mask);
  };

  /** Used to determine whether to use indexed VBO layouts for multires grids. */
  BitVector<> use_flat_layout_;
  /** The material index for each node. */
  Array<int> material_indices_;

  /** Index buffers for wireframe geometry for each node. */
  Vector<gpu::IndexBufPtr> lines_ibos_;
  /** Index buffers for coarse "fast navigate" wireframe geometry for each node. */
  Vector<gpu::IndexBufPtr> lines_ibos_coarse_;
  /** Index buffers for triangles for each node, only used for grids. */
  Vector<gpu::IndexBufPtr> tris_ibos_;
  /** Index buffers for coarse "fast navigate" triangles for each node, only used for grids. */
  Vector<gpu::IndexBufPtr> tris_ibos_coarse_;
  /**
   * GPU data and per-node dirty status for all requested attributes.
   * \note Currently we do not remove "stale" attributes that haven't been requested in a while.
   */
  Map<AttributeRequest, AttributeData> attribute_vbos_;

  /* ★頂点添字の並べ方(`FALCON_DRAW_WELD=1`)で使う分。角ごとの並べ方とは
   * バッファの中身も長さも別物なので、混ざらないように完全に分けて持つ。
   * 既定(オフ)ではどれも空のまま。 */
  Vector<gpu::IndexBufPtr> tris_ibos_welded_;
  Vector<gpu::IndexBufPtr> lines_ibos_welded_;
  Map<AttributeRequest, AttributeData> attribute_vbos_welded_;
  /** どちらの並べ方で作ったバッチかを覚えておく(要求ごと)。 */
  Map<ViewportRequest, bool> tris_batches_welded_;
  bool lines_batches_welded_ = false;
  /**
   * これまでに要求された属性を全部ためる。**並べ方はこの和で1つに決める。**
   *
   * ★要求ごとに決めると、同じ画面の中で並べ方が2通りになり、位置の頂点バッファが
   * **両方の形で毎ダブ作られる**(実測: 面セットがあると作り直しが7回→13回に倍増した)。
   * スカルプトのオーバーレイは面セットを別の要求として出してくるので、これは
   * 珍しい状況ではない。溶接が逆効果になるので、和で決めて1通りに揃える。
   */
  Vector<AttributeRequest> attrs_seen_;
  /** #attrs_seen_ を捨てる目印。PBVH が作り直されたら要求もため直す。 */
  int attrs_seen_nodes_num_ = -1;

  /** Batches for drawing wireframe geometry. */
  Vector<gpu::Batch *> lines_batches_;
  /** Batches for drawing coarse "fast navigate" wireframe geometry. */
  Vector<gpu::Batch *> lines_batches_coarse_;
  /**
   * Batches for drawing triangles, stored separately for each combination of attributes and
   * coarse-ness. Different viewports might request different sets of attributes, and we don't want
   * to recreate the batches on every redraw.
   */
  Map<ViewportRequest, Vector<gpu::Batch *>> tris_batches_;

  /**
   * Which nodes (might) have a different number of visible faces.
   *
   * \note Theoretically the dirty tag is redundant with checking for a different number of visible
   * triangles in the PBVH node on every redraw. We could do that too, but it's simpler overall to
   * just tag the node whenever there is such a topology change, and for now there is no real
   * downside.
   */
  BitVector<> dirty_topology_;

 public:
  ~DrawCacheImpl() override;

  void tag_positions_changed(const IndexMask &node_mask) override;
  void tag_visibility_changed(const IndexMask &node_mask) override;
  void tag_topology_changed(const IndexMask &node_mask) override;
  void tag_face_sets_changed(const IndexMask &node_mask) override;
  void tag_masks_changed(const IndexMask &node_mask) override;
  void tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name) override;

  Span<gpu::Batch *> ensure_tris_batches(const Object &object,
                                         const ViewportRequest &request,
                                         const IndexMask &nodes_to_update) override;

  Span<gpu::Batch *> ensure_lines_batches(const Object &object,
                                          const ViewportRequest &request,
                                          const IndexMask &nodes_to_update) override;

  Span<int> ensure_material_indices(const Object &object) override;

 private:
  /**
   * Free all GPU data for nodes with a changed visible triangle count. The next time the data is
   * requested it will be rebuilt.
   */
  void free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh);

  BitSpan ensure_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data);

  bool ensure_welded_layout(const Object &object,
                            const OrigMeshData &orig_mesh_data,
                            Span<AttributeRequest> attributes);

  Span<gpu::VertBufPtr> ensure_attribute_data(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const AttributeRequest &attr,
                                              const IndexMask &node_mask,
                                              bool welded);

  Span<gpu::IndexBufPtr> ensure_tri_indices(const Object &object,
                                            const OrigMeshData &orig_mesh_data,
                                            const IndexMask &node_mask,
                                            bool coarse,
                                            bool welded);

  Span<gpu::IndexBufPtr> ensure_lines_indices(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const IndexMask &node_mask,
                                              bool coarse,
                                              bool welded);
};

void DrawCacheImpl::AttributeData::tag_dirty(const IndexMask &node_mask)
{
  this->dirty_nodes.resize(std::max(this->dirty_nodes.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(this->dirty_nodes);
}

void DrawCacheImpl::tag_positions_changed(const IndexMask &node_mask)
{
  for (Map<AttributeRequest, AttributeData> *map : {&attribute_vbos_, &attribute_vbos_welded_}) {
    if (DrawCacheImpl::AttributeData *data = map->lookup_ptr(CustomRequest::Position)) {
      data->tag_dirty(node_mask);
    }
    if (DrawCacheImpl::AttributeData *data = map->lookup_ptr(CustomRequest::Normal)) {
      data->tag_dirty(node_mask);
    }
  }
}

void DrawCacheImpl::tag_visibility_changed(const IndexMask &node_mask)
{
  dirty_topology_.resize(std::max(dirty_topology_.size(), node_mask.min_array_size()), false);
  node_mask.set_bits(dirty_topology_);
}

void DrawCacheImpl::tag_topology_changed(const IndexMask &node_mask)
{
  /** Currently the only times where topology changes are for BMesh dynamic topology, where tagging
   * a visibility update deletes all the GPU data anyway. */
  this->tag_visibility_changed(node_mask);
}

void DrawCacheImpl::tag_face_sets_changed(const IndexMask &node_mask)
{
  for (Map<AttributeRequest, AttributeData> *map : {&attribute_vbos_, &attribute_vbos_welded_}) {
    if (DrawCacheImpl::AttributeData *data = map->lookup_ptr(CustomRequest::FaceSet)) {
      data->tag_dirty(node_mask);
    }
  }
}

void DrawCacheImpl::tag_masks_changed(const IndexMask &node_mask)
{
  for (Map<AttributeRequest, AttributeData> *map : {&attribute_vbos_, &attribute_vbos_welded_}) {
    if (DrawCacheImpl::AttributeData *data = map->lookup_ptr(CustomRequest::Mask)) {
      data->tag_dirty(node_mask);
    }
  }
}

void DrawCacheImpl::tag_attribute_changed(const IndexMask &node_mask, StringRef attribute_name)
{
  for (Map<AttributeRequest, AttributeData> *map : {&attribute_vbos_, &attribute_vbos_welded_}) {
    for (const auto &[data_request, data] : map->items()) {
      if (const GenericRequest *request = std::get_if<GenericRequest>(&data_request)) {
        if (*request == attribute_name) {
          data.tag_dirty(node_mask);
        }
      }
    }
  }
}

DrawCache &ensure_draw_data(std::unique_ptr<bke::pbvh::DrawCache> &ptr)
{
  if (!ptr) {
    ptr = std::make_unique<DrawCacheImpl>();
  }
  return dynamic_cast<DrawCache &>(*ptr);
}

BLI_NOINLINE static void free_ibos(const MutableSpan<gpu::IndexBufPtr> ibos,
                                   const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, ibos.index_range(), memory);
  mask.foreach_index([&](const int i) { ibos[i].reset(); });
}

BLI_NOINLINE static void free_vbos(const MutableSpan<gpu::VertBufPtr> vbos,
                                   const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, vbos.index_range(), memory);
  mask.foreach_index([&](const int i) { vbos[i].reset(); });
}

BLI_NOINLINE static void free_batches(const MutableSpan<gpu::Batch *> batches,
                                      const IndexMask &node_mask)
{
  IndexMaskMemory memory;
  const IndexMask mask = IndexMask::from_intersection(node_mask, batches.index_range(), memory);
  mask.foreach_index([&](const int i) { GPU_BATCH_DISCARD_SAFE(batches[i]); });
}

static const GPUVertFormat &position_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "pos", gpu::VertAttrType::SFLOAT_32_32_32);
  return format;
}

static const GPUVertFormat &normal_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "nor", gpu::VertAttrType::SNORM_16_16_16_16);
  return format;
}

static const GPUVertFormat &mask_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute("msk",
                                                                    gpu::VertAttrType::SFLOAT_32);
  return format;
}

static const GPUVertFormat &face_set_format()
{
  static const GPUVertFormat format = GPU_vertformat_from_attribute(
      "fset", gpu::VertAttrType::UNORM_8_8_8_8);
  return format;
}

static GPUVertFormat attribute_format(const OrigMeshData &orig_mesh_data,
                                      const StringRef name,
                                      const bke::AttrType data_type)
{
  GPUVertFormat format = init_format_for_attribute(data_type, "data");

  bool is_render, is_active;
  const char *prefix = "a";

  if (CD_TYPE_AS_MASK(*bke::attr_type_to_custom_data_type(data_type))) {
    prefix = "c";
    is_active = orig_mesh_data.active_color == name;
    is_render = orig_mesh_data.default_color == name;
  }
  if (data_type == bke::AttrType::Float2) {
    prefix = "u";
    is_active = orig_mesh_data.active_uv_map == name;
    is_render = orig_mesh_data.default_uv_map == name;
  }

  DRW_cdlayer_attr_aliases_add(&format, prefix, data_type, name, is_render, is_active);
  return format;
}

inline short4 normal_float_to_short(const float3 &value)
{
  short3 result;
  normal_float_to_short_v3(result, value);
  return short4(result.x, result.y, result.z, 0);
}

template<typename T>
void extract_data_vert_mesh(const OffsetIndices<int> faces,
                            const Span<int> corner_verts,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    for (const int vert : corner_verts.slice(faces[face])) {
      *data = Converter::convert(attribute[vert]);
      data++;
    }
  }
}

/* ★頂点添字の並べ方(`FALCON_DRAW_WELD=1` の時だけ)。
 *
 * 上の #extract_data_vert_mesh は面の**角ごと**に値を展開する。実データでは
 * 角は頂点の 4.00倍 あるので、彫るたびに 4倍の量を作って GPU へ送っている。
 * こちらはノードが持つ頂点の並び(#MeshNode::all_verts)にそのまま入れる。
 * 三角形は添字バッファ側で頂点を指す。
 *
 * Blender 自身が #calc_use_flat_layout の Mesh の枝に
 * 「面の角の属性・シャープな面・面セットが無ければ理論上は頂点添字のバッファを
 * 使える」と書いている。できないのではなく、やっていないだけ。 */
template<typename T>
void extract_data_vert_mesh_welded(const Span<T> attribute,
                                   const Span<int> verts,
                                   gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  MutableSpan<VBOType> data = vbo.data<VBOType>();
  for (const int i : verts.index_range()) {
    data[i] = Converter::convert(attribute[verts[i]]);
  }
}

template<typename T>
void extract_data_face_mesh(const OffsetIndices<int> faces,
                            const Span<T> attribute,
                            const Span<int> face_indices,
                            gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    const int face_size = faces[face].size();
    std::fill_n(data, face_size, Converter::convert(attribute[face]));
    data += face_size;
  }
}

template<typename T>
void extract_data_corner_mesh(const OffsetIndices<int> faces,
                              const Span<T> attribute,
                              const Span<int> face_indices,
                              gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;

  VBOType *data = vbo.data<VBOType>().data();
  for (const int face : face_indices) {
    for (const int corner : faces[face]) {
      *data = Converter::convert(attribute[corner]);
      data++;
    }
  }
}

template<typename T> const T &bmesh_cd_vert_get(const BMVert &vert, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(vert.head.data, offset));
}

template<typename T> const T &bmesh_cd_loop_get(const BMLoop &loop, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(loop.head.data, offset));
}

template<typename T> const T &bmesh_cd_face_get(const BMFace &face, const int offset)
{
  return *static_cast<const T *>(POINTER_OFFSET(face.head.data, offset));
}

template<typename T>
void extract_data_vert_bmesh(const Set<BMFace *, 0> &faces, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->prev->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->v, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_vert_get<T>(*l->next->v, cd_offset));
    data++;
  }
}

template<typename T>
void extract_data_face_bmesh(const Set<BMFace *, 0> &faces, const int cd_offset, gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    std::fill_n(data, 3, Converter::convert(bmesh_cd_face_get<T>(*face, cd_offset)));
    data += 3;
  }
}

template<typename T>
void extract_data_corner_bmesh(const Set<BMFace *, 0> &faces,
                               const int cd_offset,
                               gpu::VertBuf &vbo)
{
  using Converter = AttributeConverter<T>;
  using VBOType = typename Converter::VBOType;
  VBOType *data = vbo.data<VBOType>().data();

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }
    const BMLoop *l = face->l_first;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->prev, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l, cd_offset));
    data++;
    *data = Converter::convert(bmesh_cd_loop_get<T>(*l->next, cd_offset));
    data++;
  }
}

static int count_visible_tris_bmesh(const Set<BMFace *, 0> &faces)
{
  return std::count_if(faces.begin(), faces.end(), [&](const BMFace *face) {
    return !BM_elem_flag_test_bool(face, BM_ELEM_HIDDEN);
  });
}

DrawCacheImpl::~DrawCacheImpl()
{
  free_batches(lines_batches_, lines_batches_.index_range());
  free_batches(lines_batches_coarse_, lines_batches_coarse_.index_range());
  for (MutableSpan<gpu::Batch *> batches : tris_batches_.values()) {
    free_batches(batches, batches.index_range());
  }
}

void DrawCacheImpl::free_nodes_with_changed_topology(const bke::pbvh::Tree &pbvh)
{
  /* NOTE: Theoretically we shouldn't need to free batches with a changed triangle count, but
   * currently it's the simplest way to reallocate all the GPU data while keeping everything in a
   * consistent state. */
  IndexMaskMemory memory;
  const IndexMask nodes_to_free = IndexMask::from_bits(dirty_topology_, memory);
  if (nodes_to_free.is_empty()) {
    return;
  }

  dirty_topology_.clear_and_shrink();

  free_ibos(lines_ibos_, nodes_to_free);
  free_ibos(lines_ibos_coarse_, nodes_to_free);
  free_ibos(tris_ibos_, nodes_to_free);
  free_ibos(tris_ibos_coarse_, nodes_to_free);
  free_ibos(lines_ibos_welded_, nodes_to_free);
  free_ibos(tris_ibos_welded_, nodes_to_free);
  if (pbvh.type() == bke::pbvh::Type::BMesh) {
    /* For BMesh, VBOs are only filled with data for visible triangles, and topology can also
     * completely change due to dynamic topology, so VBOs must be rebuilt from scratch. For other
     * types, actual topology doesn't change, and visibility changes are accounted for by the index
     * buffers. */
    for (AttributeData &data : attribute_vbos_.values()) {
      free_vbos(data.vbos, nodes_to_free);
    }
  }

  free_batches(lines_batches_, nodes_to_free);
  free_batches(lines_batches_coarse_, nodes_to_free);
  for (MutableSpan<gpu::Batch *> batches : tris_batches_.values()) {
    free_batches(batches, nodes_to_free);
  }
}

BLI_NOINLINE static void ensure_vbos_allocated_mesh(const Object &object,
                                                    const GPUVertFormat &format,
                                                    const IndexMask &node_mask,
                                                    const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        GPU_vertbuf_data_alloc(*vbos[i], nodes[i].corners_num());
      },
      exec_mode::grain_size(64));
}

/* 頂点添字の並べ方での大きさ = ノードが使う頂点の数(角ではない)。 */
BLI_NOINLINE static void ensure_vbos_allocated_mesh_welded(
    const Object &object,
    const GPUVertFormat &format,
    const IndexMask &node_mask,
    const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        GPU_vertbuf_data_alloc(*vbos[i], nodes[i].all_verts().size());
      },
      exec_mode::grain_size(64));
}

BLI_NOINLINE static void ensure_vbos_allocated_grids(const Object &object,
                                                     const GPUVertFormat &format,
                                                     const BitSpan use_flat_layout,
                                                     const IndexMask &node_mask,
                                                     const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        const int verts_per_grid = use_flat_layout[i] ? square_i(subdiv_ccg.grid_size - 1) * 4 :
                                                        square_i(subdiv_ccg.grid_size);
        const int verts_num = nodes[i].grids().size() * verts_per_grid;
        GPU_vertbuf_data_alloc(*vbos[i], verts_num);
      },
      exec_mode::grain_size(64));
}

BLI_NOINLINE static void ensure_vbos_allocated_bmesh(const Object &object,
                                                     const GPUVertFormat &format,
                                                     const IndexMask &node_mask,
                                                     const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  node_mask.foreach_index(
      [&](const int i) {
        if (!vbos[i]) {
          vbos[i] = gpu::VertBufPtr(GPU_vertbuf_create_with_format(format));
        }
        const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
            &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
        const int verts_num = count_visible_tris_bmesh(faces) * 3;
        GPU_vertbuf_data_alloc(*vbos[i], verts_num);
      },
      exec_mode::grain_size(64));
}

/* ★描画側で位置を作り直す費用を測る(`FALCON_SCULPT_TIMING=1` の時だけ)。
 *
 * 2026-08-17: ここまでの計測は**ストロークの計算だけ**で、描画側を含んでいなかった
 * (EXEC 経路ではダブの間に描画が走らないため)。実際に手で彫る時は毎ダブ通る。
 *
 * しかもここは**面の角ごとに位置を展開する**(頂点の共有をやめている。法線が
 * 角ごとに来るため)。四角面なら1面4個、三角なら3個書くので、頂点数の数倍になる。
 * falcon-live 側が同じ形の無駄を頂点の溶接で潰して 5.2倍にした、という共有を受けて
 * こちら側の実際の量を数えることにした。**「6倍くらいのはず」で進めない。** */
namespace falcon_draw_timing {
static std::atomic<int64_t> g_ns{0}, g_corners{0}, g_verts{0}, g_written{0}, g_calls{0};

static bool enabled()
{
  static const bool on = (getenv("FALCON_SCULPT_TIMING") != nullptr);
  return on;
}

static void report()
{
  const int64_t calls = g_calls.exchange(0, std::memory_order_relaxed);
  if (calls == 0) {
    return;
  }
  const int64_t ns = g_ns.exchange(0, std::memory_order_relaxed);
  const int64_t corners = g_corners.exchange(0, std::memory_order_relaxed);
  const int64_t verts = g_verts.exchange(0, std::memory_order_relaxed);
  const int64_t written = g_written.exchange(0, std::memory_order_relaxed);
  /* 「書いた数」= 実際に頂点バッファへ入れた要素の数。角ごとの並べ方なら角の数、
   * 頂点添字の並べ方ならノードが使う頂点の数。ここが減れば作る量も送る量も減る。 */
  printf(
      "FALCON_DRAW 位置の作り直し %.2fms/回  書いた数%ld  "
      "(角%ld / 頂点%ld = %.2f倍)  %.1fMB/回  [%ld回]\n",
      double(ns) / 1.0e6 / double(calls),
      long(written / calls),
      long(corners / calls),
      long(verts / calls),
      verts > 0 ? double(corners) / double(verts) : 0.0,
      double(written / calls) * 12.0 / 1024.0 / 1024.0,
      long(calls));
  fflush(stdout);
}
}  // namespace falcon_draw_timing

void falcon_draw_report_positions()
{
  falcon_draw_timing::report();
}

/* ★頂点添字の並べ方を使うかどうか。既定はオン(2026-08-25 に反転)。
 *
 * これまでは角ごとに位置を展開して GPU の頂点バッファへ入れ直していた。角は実測で
 * 頂点の 4.00 倍あるので、その分だけ作り直しと転送を余計に払っていた。実測(400万面):
 * 位置の作り直し 6.90 -> 1.59ms / 転送量 1/3.80 / 描画は 200万面で 51.2 -> 42.6ms。
 * 条件別 11 件すべて対照の中に収まっている。
 *
 * 戻す口は FALCON_DRAW_WELD=0 だけ。空文字は「未設定」と同じ扱い(=オン)にしてある:
 * getenv は `FALCON_DRAW_WELD=` の空文字でも非 nullptr を返すので、
 * `value != nullptr` だけで判定すると外したつもりが外れない。 */
namespace falcon_weld {
static bool enabled()
{
  static const bool on = []() {
    const char *value = getenv("FALCON_DRAW_WELD");
    return !(value != nullptr && value[0] == '0');
  }();
  return on;
}
}  // namespace falcon_weld

static void update_positions_mesh(const Object &object,
                                  const IndexMask &node_mask,
                                  const bool welded,
                                  MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_positions = bke::pbvh::vert_positions_eval_from_eval(object);
  if (welded) {
    ensure_vbos_allocated_mesh_welded(object, position_format(), node_mask, vbos);
  }
  else {
    ensure_vbos_allocated_mesh(object, position_format(), node_mask, vbos);
  }
  const bool prf = falcon_draw_timing::enabled();
  const double t0 = prf ? BLI_time_now_seconds() : 0.0;
  node_mask.foreach_index(
      [&](const int i) {
        if (welded) {
          extract_data_vert_mesh_welded<float3>(vert_positions, nodes[i].all_verts(), *vbos[i]);
        }
        else {
          extract_data_vert_mesh<float3>(
              faces, corner_verts, vert_positions, nodes[i].faces(), *vbos[i]);
        }
      },
      exec_mode::grain_size(1));
  if (prf) {
    falcon_draw_timing::g_ns.fetch_add(int64_t((BLI_time_now_seconds() - t0) * 1.0e9),
                                       std::memory_order_relaxed);
    int64_t corners = 0;
    int64_t verts = 0;
    int64_t written = 0;
    node_mask.foreach_index([&](const int i) {
      for (const int face : nodes[i].faces()) {
        corners += faces[face].size();
      }
      verts += nodes[i].verts().size();
      written += welded ? nodes[i].all_verts().size() : nodes[i].corners_num();
    });
    falcon_draw_timing::g_corners.fetch_add(corners, std::memory_order_relaxed);
    falcon_draw_timing::g_verts.fetch_add(verts, std::memory_order_relaxed);
    falcon_draw_timing::g_written.fetch_add(written, std::memory_order_relaxed);
    falcon_draw_timing::g_calls.fetch_add(1, std::memory_order_relaxed);
    /* 10回ごとに出す。ストロークの終わりを跨いで呼ぶ配線を足すより、
     * ここで完結させるほうが触る面が小さい。 */
    if (falcon_draw_timing::g_calls.load(std::memory_order_relaxed) >= 1) {
      falcon_draw_timing::report();
    }
  }
}

static void update_normals_mesh(const Object &object,
                                const IndexMask &node_mask,
                                const bool welded,
                                MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const Span<float3> vert_normals = bke::pbvh::vert_normals_eval_from_eval(object);
  const Span<float3> face_normals = bke::pbvh::face_normals_eval_from_eval(object);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  if (welded) {
    /* 頂点添字の並べ方はシャープな面が無い時だけ選ばれる(#mesh_can_use_welded)。
     * 面ごとの法線は入り得ないので、頂点法線をそのまま並べる。 */
    ensure_vbos_allocated_mesh_welded(object, normal_format(), node_mask, vbos);
    node_mask.foreach_index(
        [&](const int i) {
          MutableSpan<short4> data = vbos[i]->data<short4>();
          const Span<int> verts = nodes[i].all_verts();
          for (const int j : verts.index_range()) {
            data[j] = normal_float_to_short(vert_normals[verts[j]]);
          }
        },
        exec_mode::grain_size(1));
    return;
  }
  ensure_vbos_allocated_mesh(object, normal_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();

        for (const int face : nodes[i].faces()) {
          if (!sharp_faces.is_empty() && sharp_faces[face]) {
            const int face_size = faces[face].size();
            std::fill_n(data, face_size, normal_float_to_short(face_normals[face]));
            data += face_size;
          }
          else {
            for (const int vert : corner_verts.slice(faces[face])) {
              *data = normal_float_to_short(vert_normals[vert]);
              data++;
            }
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_mesh(const Object &object,
                                           const OrigMeshData &orig_mesh_data,
                                           const IndexMask &node_mask,
                                           const bool welded,
                                           MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const VArraySpan mask = *orig_mesh_data.attributes.lookup<float>(".sculpt_mask",
                                                                   bke::AttrDomain::Point);
  if (welded) {
    ensure_vbos_allocated_mesh_welded(object, mask_format(), node_mask, vbos);
  }
  else {
    ensure_vbos_allocated_mesh(object, mask_format(), node_mask, vbos);
  }
  if (!mask.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          if (welded) {
            MutableSpan<float> data = vbos[i]->data<float>();
            const Span<int> verts = nodes[i].all_verts();
            for (const int j : verts.index_range()) {
              data[j] = mask[verts[j]];
            }
            return;
          }
          float *data = vbos[i]->data<float>().data();
          for (const int face : nodes[i].faces()) {
            for (const int vert : corner_verts.slice(faces[face])) {
              *data = mask[vert];
              data++;
            }
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_face_sets_mesh(const Object &object,
                                               const OrigMeshData &orig_mesh_data,
                                               const IndexMask &node_mask,
                                               const bool welded,
                                               MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const VArraySpan face_sets = *orig_mesh_data.attributes.lookup<int>(".sculpt_face_set",
                                                                      bke::AttrDomain::Face);
  if (welded) {
    /* 面セットが無い時は全部白で埋めるだけなので、並べ方を問わず同じ絵になる。
     * 中身がある面セットは面ドメインなので #mesh_can_use_welded が弾く。 */
    BLI_assert(face_sets.is_empty());
    ensure_vbos_allocated_mesh_welded(object, face_set_format(), node_mask, vbos);
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4(255)); },
                            exec_mode::grain_size(64));
    return;
  }
  ensure_vbos_allocated_mesh(object, face_set_format(), node_mask, vbos);
  if (!face_sets.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const int face : nodes[i].faces()) {
            const int id = face_sets[face];

            uchar4 fset_color(UCHAR_MAX);
            if (id != color_default) {
              BKE_paint_face_set_overlay_color_get(id, color_seed, fset_color);
            }
            else {
              /* Skip for the default color face set to render it white. */
              fset_color[0] = fset_color[1] = fset_color[2] = UCHAR_MAX;
            }

            const int face_size = faces[face].size();
            std::fill_n(data, face_size, fset_color);
            data += face_size;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4(255)); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_mesh(const Object &object,
                                                       const OrigMeshData &orig_mesh_data,
                                                       const IndexMask &node_mask,
                                                       const StringRef name,
                                                       const bool welded,
                                                       MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const OffsetIndices<int> faces = mesh.faces();
  const Span<int> corner_verts = mesh.corner_verts();
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const bke::GAttributeReader attr = attributes.lookup(name);
  if (!attr || attr.domain == bke::AttrDomain::Edge) {
    return;
  }
  const bke::AttrType data_type = bke::cpp_type_to_attribute_type(attr.varray.type());
  if (welded) {
    /* 頂点添字の並べ方は頂点ドメインの属性しか要求されていない時だけ選ばれる。 */
    BLI_assert(attr.domain == bke::AttrDomain::Point);
    ensure_vbos_allocated_mesh_welded(
        object, attribute_format(orig_mesh_data, name, data_type), node_mask, vbos);
  }
  else {
    ensure_vbos_allocated_mesh(
        object, attribute_format(orig_mesh_data, name, data_type), node_mask, vbos);
  }
  node_mask.foreach_index(
      [&](const int i) {
        bke::attribute_math::to_static_type(attr.varray.type(), [&]<typename T>() {
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            const VArraySpan<T> src = attr.varray.typed<T>();
            if (welded) {
              extract_data_vert_mesh_welded<T>(src, nodes[i].all_verts(), *vbos[i]);
              return;
            }
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_mesh<T>(faces, corner_verts, src, nodes[i].faces(), *vbos[i]);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_mesh<T>(faces, src, nodes[i].faces(), *vbos[i]);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_mesh<T>(faces, src, nodes[i].faces(), *vbos[i]);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_positions_grids(const Object &object,
                                              const BitSpan use_flat_layout,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  ensure_vbos_allocated_grids(object, position_format(), use_flat_layout, node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        float3 *data = vbos[i]->data<float3>().data();
        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            for (int y = 0; y < grid_size_1; y++) {
              for (int x = 0; x < grid_size_1; x++) {
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                data++;
                *data = grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                data++;
              }
            }
          }
        }
        else {
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            std::copy_n(grid_positions.data(), grid_positions.size(), data);
            data += grid_positions.size();
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_normals_grids(const Object &object,
                                            const OrigMeshData &orig_mesh_data,
                                            const BitSpan use_flat_layout,
                                            const IndexMask &node_mask,
                                            const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const Span<float3> positions = subdiv_ccg.positions;
  const Span<float3> normals = subdiv_ccg.normals;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
  ensure_vbos_allocated_grids(object, normal_format(), use_flat_layout, node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();

        if (use_flat_layout[i]) {
          const int grid_size_1 = key.grid_size - 1;
          for (const int grid : nodes[i].grids()) {
            const Span<float3> grid_positions = positions.slice(bke::ccg::grid_range(key, grid));
            const Span<float3> grid_normals = normals.slice(bke::ccg::grid_range(key, grid));
            if (!sharp_faces.is_empty() && sharp_faces[grid_to_face_map[grid]]) {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  float3 no;
                  normal_quad_v3(no,
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x + 1, y)],
                                 grid_positions[CCG_grid_xy_to_index(key.grid_size, x, y)]);
                  std::fill_n(data, 4, normal_float_to_short(no));
                  data += 4;
                }
              }
            }
            else {
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  std::fill_n(data,
                              4,
                              normal_float_to_short(
                                  grid_normals[CCG_grid_xy_to_index(key.grid_size, x, y)]));
                  data += 4;
                }
              }
            }
          }
        }
        else {
          /* The non-flat VBO layout does not support sharp faces. */
          for (const int grid : nodes[i].grids()) {
            for (const float3 &normal : normals.slice(bke::ccg::grid_range(key, grid))) {
              *data = normal_float_to_short(normal);
              data++;
            }
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void fill_masks_grids(const Object &object,
                                          const BitSpan use_flat_layout,
                                          const IndexMask &node_mask,
                                          const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const Span<float> masks = subdiv_ccg.masks;
  ensure_vbos_allocated_grids(object, mask_format(), use_flat_layout, node_mask, vbos);
  if (!masks.is_empty()) {
    node_mask.foreach_index(
        [&](const int i) {
          float *data = vbos[i]->data<float>().data();
          if (use_flat_layout[i]) {
            const int grid_size_1 = key.grid_size - 1;
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
              for (int y = 0; y < grid_size_1; y++) {
                for (int x = 0; x < grid_size_1; x++) {
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x + 1, y + 1)];
                  data++;
                  *data = grid_masks[CCG_grid_xy_to_index(key.grid_size, x, y + 1)];
                  data++;
                }
              }
            }
          }
          else {
            for (const int grid : nodes[i].grids()) {
              const Span<float> grid_masks = masks.slice(bke::ccg::grid_range(key, grid));
              std::copy_n(grid_masks.data(), grid_masks.size(), data);
              data += grid_masks.size();
            }
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void fill_face_sets_grids(const Object &object,
                                              const OrigMeshData &orig_mesh_data,
                                              const BitSpan use_flat_layout,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
  const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
  const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;
  const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
  ensure_vbos_allocated_grids(object, face_set_format(), use_flat_layout, node_mask, vbos);
  if (const VArray<int> face_sets = *attributes.lookup<int>(".sculpt_face_set",
                                                            bke::AttrDomain::Face))
  {
    const VArraySpan<int> face_sets_span(face_sets);
    node_mask.foreach_index(
        [&](const int i) {
          const Span<int> grids = nodes[i].grids();
          const int verts_per_grid = use_flat_layout[i] ? square_i(key.grid_size - 1) * 4 :
                                                          square_i(key.grid_size);
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const int i : grids.index_range()) {
            uchar4 color{UCHAR_MAX};
            const int fset = face_sets[grid_to_face_map[grids[i]]];
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }

            std::fill_n(data, verts_per_grid, color);
            data += verts_per_grid;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4{UCHAR_MAX}); },
                            exec_mode::grain_size(1));
  }
}

BLI_NOINLINE static void update_positions_bmesh(const Object &object,
                                                const IndexMask &node_mask,
                                                const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  ensure_vbos_allocated_bmesh(object, position_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        float3 *data = vbos[i]->data<float3>().data();
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          const BMLoop *l = face->l_first;
          *data = l->prev->v->co;
          data++;
          *data = l->v->co;
          data++;
          *data = l->next->v->co;
          data++;
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_normals_bmesh(const Object &object,
                                              const IndexMask &node_mask,
                                              const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  ensure_vbos_allocated_bmesh(object, normal_format(), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        short4 *data = vbos[i]->data<short4>().data();
        for (const BMFace *face :
             BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
        {
          if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
            continue;
          }
          if (BM_elem_flag_test(face, BM_ELEM_SMOOTH)) {
            const BMLoop *l = face->l_first;
            *data = normal_float_to_short(l->prev->v->no);
            data++;
            *data = normal_float_to_short(l->v->no);
            data++;
            *data = normal_float_to_short(l->next->v->no);
            data++;
          }
          else {
            std::fill_n(data, 3, normal_float_to_short(face->no));
            data += 3;
          }
        }
      },
      exec_mode::grain_size(1));
}

BLI_NOINLINE static void update_masks_bmesh(const Object &object,
                                            const IndexMask &node_mask,
                                            const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int cd_offset = CustomData_get_offset_named(&bm.vdata, CD_PROP_FLOAT, ".sculpt_mask");
  ensure_vbos_allocated_bmesh(object, mask_format(), node_mask, vbos);
  if (cd_offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          float *data = vbos[i]->data<float>().data();
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            const BMLoop *l = face->l_first;
            *data = bmesh_cd_vert_get<float>(*l->prev->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->v, cd_offset);
            data++;
            *data = bmesh_cd_vert_get<float>(*l->next->v, cd_offset);
            data++;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float>().fill(0.0f); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_face_sets_bmesh(const Object &object,
                                                const OrigMeshData &orig_mesh_data,
                                                const IndexMask &node_mask,
                                                const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const int color_default = orig_mesh_data.face_set_default;
  const int color_seed = orig_mesh_data.face_set_seed;
  const int offset = CustomData_get_offset_named(&bm.pdata, CD_PROP_INT32, ".sculpt_face_set");
  ensure_vbos_allocated_bmesh(object, face_set_format(), node_mask, vbos);
  if (offset != -1) {
    node_mask.foreach_index(
        [&](const int i) {
          uchar4 *data = vbos[i]->data<uchar4>().data();
          for (const BMFace *face :
               BKE_pbvh_bmesh_node_faces(&const_cast<bke::pbvh::BMeshNode &>(nodes[i])))
          {
            if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
              continue;
            }
            uchar4 color{UCHAR_MAX};
            const int fset = bmesh_cd_face_get<int>(*face, offset);
            if (fset != color_default) {
              BKE_paint_face_set_overlay_color_get(fset, color_seed, color);
            }
            std::fill_n(data, 3, color);
            data += 3;
          }
        },
        exec_mode::grain_size(1));
  }
  else {
    node_mask.foreach_index([&](const int i) { vbos[i]->data<uchar4>().fill(uchar4(255)); },
                            exec_mode::grain_size(64));
  }
}

BLI_NOINLINE static void update_generic_attribute_bmesh(const Object &object,
                                                        const OrigMeshData &orig_mesh_data,
                                                        const IndexMask &node_mask,
                                                        const StringRef name,
                                                        const MutableSpan<gpu::VertBufPtr> vbos)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
  const BMesh &bm = *object.runtime->sculpt_session->bm;
  const BMDataLayerLookup attr = BM_data_layer_lookup(bm, name);
  if (attr.domain == bke::AttrDomain::Edge) {
    return;
  }

  if (!attr) {
    ensure_vbos_allocated_bmesh(
        object, attribute_format(orig_mesh_data, name, bke::AttrType::Float3), node_mask, vbos);
    node_mask.foreach_index([&](const int i) { vbos[i]->data<float3>().fill(float3(0.0f)); },
                            exec_mode::grain_size(1));
    return;
  }

  ensure_vbos_allocated_bmesh(
      object, attribute_format(orig_mesh_data, name, attr.type), node_mask, vbos);
  node_mask.foreach_index(
      [&](const int i) {
        bke::attribute_math::to_static_type(attr.type, [&]<typename T>() {
          const auto &faces = BKE_pbvh_bmesh_node_faces(
              &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
          if constexpr (!std::is_void_v<typename AttributeConverter<T>::VBOType>) {
            switch (attr.domain) {
              case bke::AttrDomain::Point:
                extract_data_vert_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              case bke::AttrDomain::Face:
                extract_data_face_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              case bke::AttrDomain::Corner:
                extract_data_corner_bmesh<T>(faces, attr.offset, *vbos[i]);
                break;
              default:
                BLI_assert_unreachable();
            }
          }
        });
      },
      exec_mode::grain_size(1));
}

static gpu::IndexBufPtr create_lines_index_faces(const OffsetIndices<int> faces,
                                                 const Span<bool> hide_poly,
                                                 const Span<int> face_indices)
{
  int corners_count = 0;
  for (const int face : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    corners_count += faces[face].size();
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, corners_count, INT_MAX);
  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();

  int node_corner_offset = 0;
  int line_index = 0;
  for (const int face_index : face_indices) {
    const int face_size = faces[face_index].size();
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face_size;
      continue;
    }
    for (const int i : IndexRange(face_size)) {
      const int next = (i == face_size - 1) ? 0 : i + 1;
      data[line_index] = uint2(i, next) + node_corner_offset;
      line_index++;
    }

    node_corner_offset += face_size;
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node_corner_offset, false));
}

static gpu::IndexBufPtr create_lines_index_bmesh(const Set<BMFace *, 0> &faces,
                                                 const int visible_faces_num)
{
  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, visible_faces_num * 3, INT_MAX);

  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();

  int line_index = 0;
  int vert_index = 0;

  for (const BMFace *face : faces) {
    if (BM_elem_flag_test(face, BM_ELEM_HIDDEN)) {
      continue;
    }

    data[line_index] = uint2(vert_index, vert_index + 1);
    line_index++;
    data[line_index] = uint2(vert_index + 1, vert_index + 2);
    line_index++;
    data[line_index] = uint2(vert_index + 2, vert_index);
    line_index++;

    vert_index += 3;
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, visible_faces_num * 3, false));
}

static int create_tri_index_grids(const Span<int> grid_indices,
                                  const BitGroupVector<> &grid_hidden,
                                  const int gridsize,
                                  const int skip,
                                  const int totgrid,
                                  MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = 0;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }

  return tri_index;
}

static int create_tri_index_grids_flat_layout(const Span<int> grid_indices,
                                              const BitGroupVector<> &grid_hidden,
                                              const int gridsize,
                                              const int skip,
                                              const int totgrid,
                                              MutableSpan<uint3> data)
{
  int tri_index = 0;
  int offset = 0;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[tri_index] = uint3(v0, v2, v1);
        tri_index++;
        data[tri_index] = uint3(v0, v3, v2);
        tri_index++;
      }
    }
  }
  return tri_index;
}

static void create_lines_index_grids(const Span<int> grid_indices,
                                     int display_gridsize,
                                     const BitGroupVector<> &grid_hidden,
                                     const int gridsize,
                                     const int skip,
                                     const int totgrid,
                                     MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = 0;
  const int grid_vert_len = gridsize * gridsize;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    uint v0, v1, v2, v3;
    bool grid_visible = false;

    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }
        /* Indices in a Clockwise QUAD disposition. */
        v0 = offset + CCG_grid_xy_to_index(gridsize, x, y);
        v1 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y);
        v2 = offset + CCG_grid_xy_to_index(gridsize, x + skip, y + skip);
        v3 = offset + CCG_grid_xy_to_index(gridsize, x, y + skip);

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static void create_lines_index_grids_flat_layout(const Span<int> grid_indices,
                                                 int display_gridsize,
                                                 const BitGroupVector<> &grid_hidden,
                                                 const int gridsize,
                                                 const int skip,
                                                 const int totgrid,
                                                 MutableSpan<uint2> data)
{
  int line_index = 0;
  int offset = 0;
  const int grid_vert_len = square_uint(gridsize - 1) * 4;
  for (int i = 0; i < totgrid; i++, offset += grid_vert_len) {
    bool grid_visible = false;
    const BoundedBitSpan gh = grid_hidden.is_empty() ? BoundedBitSpan() :
                                                       grid_hidden[grid_indices[i]];

    uint v0, v1, v2, v3;
    for (int y = 0; y < gridsize - skip; y += skip) {
      for (int x = 0; x < gridsize - skip; x += skip) {
        /* Skip hidden grid face */
        if (!gh.is_empty() && paint_is_grid_face_hidden(gh, gridsize, x, y)) {
          continue;
        }

        v0 = (y * (gridsize - 1) + x) * 4;

        if (skip > 1) {
          v1 = (y * (gridsize - 1) + x + skip - 1) * 4;
          v2 = ((y + skip - 1) * (gridsize - 1) + x + skip - 1) * 4;
          v3 = ((y + skip - 1) * (gridsize - 1) + x) * 4;
        }
        else {
          v1 = v2 = v3 = v0;
        }

        /* VBO data are in a Clockwise QUAD disposition.  Note
         * that vertices might be in different quads if we're
         * building a coarse index buffer.
         */
        v0 += offset;
        v1 += offset + 1;
        v2 += offset + 2;
        v3 += offset + 3;

        data[line_index] = uint2(v0, v1);
        line_index++;
        data[line_index] = uint2(v0, v3);
        line_index++;

        if (y / skip + 2 == display_gridsize) {
          data[line_index] = uint2(v2, v3);
          line_index++;
        }
        grid_visible = true;
      }

      if (grid_visible) {
        data[line_index] = uint2(v1, v2);
        line_index++;
      }
    }
  }
}

static Array<int> calc_material_indices(const Object &object, const OrigMeshData &orig_mesh_data)
{
  const SculptSession &ss = *object.runtime->sculpt_session;
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const bke::AttributeAccessor attributes = mesh.attributes();
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> face_indices = nodes[i].faces();
          if (face_indices.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[face_indices.first()];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      /* Use original mesh data because evaluated mesh is empty. */
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArray material_indices = *attributes.lookup<int>("material_index",
                                                              bke::AttrDomain::Face);
      if (!material_indices) {
        return {};
      }
      Array<int> node_materials(nodes.size());
      const SubdivCCG &subdiv_ccg = *ss.subdiv_ccg;
      const Span<int> grid_faces = subdiv_ccg.grid_to_face_map;
      threading::parallel_for(nodes.index_range(), 64, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          node_materials[i] = material_indices[grid_faces[grids.first()]];
        }
      });
      return node_materials;
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

/* ★このメッシュとこの要求の組み合わせで、頂点添字の並べ方を使えるか。
 *
 * Blender の元コメントは「面の角の属性・シャープな面・面セットが無ければ」だが、
 * それを**メッシュが持っているか**で判定すると、UVMap を持つ普通のメッシュが
 * 全部弾かれる(実データがまさにそれ)。スカルプト中の描画が実際に要求するのは
 * 位置・法線・マスクだけなので、**要求された属性のドメイン**で判定する。 */
static bool mesh_can_use_welded(const Object &object,
                                const OrigMeshData &orig_mesh_data,
                                const Span<AttributeRequest> attributes)
{
  /* 弾いた理由を1度だけ出す。**「効いているはず」で先へ進まない。**
   * 実際、最初の実装では要求の中に UVMap が入っていて丸ごと弾かれていた。 */
  const auto reject = [&](const char *why) {
    /* ★計時とは別に出す。手で試す時は計時を入れたくない
     * (毎ダブ全角を数え直すので実測で 6.8ms/ダブ = 16% 乗る)。 */
    if (!falcon_weld::enabled()) {
      return false;
    }
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) < 8) {
      std::string names;
      for (const AttributeRequest &attr : attributes) {
        if (const CustomRequest *type = std::get_if<CustomRequest>(&attr)) {
          const char *labels[] = {"位置", "法線", "マスク", "面セット"};
          names += labels[int(*type)];
        }
        else {
          names += std::get<GenericRequest>(attr);
        }
        names += " ";
      }
      printf("FALCON_WELD 見送り: %s  [要求: %s]\n", why, names.c_str());
      fflush(stdout);
    }
    return false;
  };

  if (!falcon_weld::enabled()) {
    return false;
  }
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (pbvh.type() != bke::pbvh::Type::Mesh) {
    return reject("PBVHがメッシュ型でない");
  }
  /* シャープな面があると法線が面ごとになり、頂点で共有できない。
   * #update_normals_mesh は評価後のメッシュから引くので、そちらを見る。
   *
   * ★属性が「有るか」ではなく「中身が真の面が1つでも有るか」で見る。
   * 実データは `sharp_face` を全部 false のまま持っており、有無で判定すると
   * 一度もシャープにしていないメッシュまで全部弾かれた(実際に弾かれた)。 */
  const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
  const VArraySpan sharp_faces = *mesh.attributes().lookup<bool>("sharp_face",
                                                                 bke::AttrDomain::Face);
  if (!sharp_faces.is_empty()) {
    const bool any_sharp = threading::parallel_reduce(
        sharp_faces.index_range(),
        4096,
        false,
        [&](const IndexRange range, const bool init) {
          if (init) {
            return true;
          }
          return std::any_of(sharp_faces.slice(range).begin(),
                             sharp_faces.slice(range).end(),
                             [](const bool value) { return value; });
        },
        [](const bool a, const bool b) { return a || b; });
    if (any_sharp) {
      return reject("シャープな面がある");
    }
  }
  for (const AttributeRequest &attr : attributes) {
    if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
      if (*request_type == CustomRequest::FaceSet) {
        /* 面セットは面ドメイン。ただし属性そのものが無い時は全部白で埋めるだけなので、
         * 並べ方を問わず同じ絵になる(スカルプトのオーバーレイは面セットが無くても
         * 要求してくるので、ここを通さないと大半のメッシュで溶接が効かない)。 */
        if (orig_mesh_data.attributes.contains(".sculpt_face_set")) {
          return reject("中身のある面セットがある");
        }
      }
      continue;
    }
    const bke::GAttributeReader reader = orig_mesh_data.attributes.lookup(
        std::get<GenericRequest>(attr));
    if (!reader) {
      return reject("要求された属性が見つからない");
    }
    if (reader.domain != bke::AttrDomain::Point) {
      return reject("頂点ドメインでない属性が要求されている");
    }
  }
  /* 通った時も1度だけ出す。**見送りだけ出していると「何も出ない = 効いている」
   * という読み方になり、起動に失敗しても同じ見え方になる。** */
  {
    static std::atomic<int> printed{0};
    if (printed.fetch_add(1, std::memory_order_relaxed) == 0) {
      printf("FALCON_WELD 有効: 頂点添字の並べ方で描いています\n");
      fflush(stdout);
    }
  }
  return true;
}

static BitVector<> calc_use_flat_layout(const Object &object, const OrigMeshData &orig_mesh_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh:
      /* NOTE: Theoretically it would be possible to used vertex indexed buffers if there are no
       * face corner attributes, sharp faces, or face sets. */
      return {};
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan sharp_faces = *attributes.lookup<bool>("sharp_face", bke::AttrDomain::Face);
      if (sharp_faces.is_empty()) {
        return BitVector<>(nodes.size(), false);
      }

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const Span<int> grid_to_face_map = subdiv_ccg.grid_to_face_map;

      /* Use boolean array instead of #BitVector for parallelized writing. */
      Array<bool> use_flat_layout(nodes.size());
      threading::parallel_for(nodes.index_range(), 4, [&](const IndexRange range) {
        for (const int i : range) {
          const Span<int> grids = nodes[i].grids();
          if (grids.is_empty()) {
            continue;
          }
          use_flat_layout[i] = std::any_of(grids.begin(), grids.end(), [&](const int grid) {
            return sharp_faces[grid_to_face_map[grid]];
          });
        }
      });
      return BitVector<>(use_flat_layout);
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

static gpu::IndexBufPtr create_tri_index_mesh(const OffsetIndices<int> faces,
                                              const Span<int3> corner_tris,
                                              const Span<bool> hide_poly,
                                              const bke::pbvh::MeshNode &node)
{
  const Span<int> face_indices = node.faces();
  int tris_num = 0;
  if (hide_poly.is_empty()) {
    tris_num = poly_to_tri_count(face_indices.size(), node.corners_num());
  }
  else {
    for (const int face : face_indices) {
      if (hide_poly[face]) {
        continue;
      }
      tris_num += bke::mesh::face_triangles_num(faces[face].size());
    }
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, tris_num, INT_MAX);
  MutableSpan<uint3> data = GPU_indexbuf_get_data(&builder).cast<uint3>();

  int tri_index = 0;
  int node_corner_offset = 0;
  for (const int face_index : face_indices) {
    const IndexRange face = faces[face_index];
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      node_corner_offset += face.size();
      continue;
    }
    for (const int3 &tri : corner_tris.slice(bke::mesh::face_triangles_range(faces, face_index))) {
      for (int i : IndexRange(3)) {
        const int corner = tri[i];
        const int index_in_face = corner - face.first();
        data[tri_index][i] = node_corner_offset + index_in_face;
      }
      tri_index++;
    }
    node_corner_offset += face.size();
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node_corner_offset, false));
}

/* ★頂点添字の並べ方での三角形の添字。角の通し番号でなく、ノード内の頂点番号を指す。
 * #MeshNode::vert_indices_ は VectorSet なので、全体の頂点番号 → ノード内の番号は
 * そのまま引ける(自前で対応表を作る必要はない)。 */
static gpu::IndexBufPtr create_tri_index_mesh_welded(const OffsetIndices<int> faces,
                                                     const Span<int3> corner_tris,
                                                     const Span<int> corner_verts,
                                                     const Span<bool> hide_poly,
                                                     const bke::pbvh::MeshNode &node)
{
  const Span<int> face_indices = node.faces();
  int tris_num = 0;
  if (hide_poly.is_empty()) {
    tris_num = poly_to_tri_count(face_indices.size(), node.corners_num());
  }
  else {
    for (const int face : face_indices) {
      if (hide_poly[face]) {
        continue;
      }
      tris_num += bke::mesh::face_triangles_num(faces[face].size());
    }
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, tris_num, INT_MAX);
  MutableSpan<uint3> data = GPU_indexbuf_get_data(&builder).cast<uint3>();

  int tri_index = 0;
  for (const int face_index : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      continue;
    }
    for (const int3 &tri : corner_tris.slice(bke::mesh::face_triangles_range(faces, face_index))) {
      for (const int i : IndexRange(3)) {
        data[tri_index][i] = uint(node.vert_indices_.index_of(corner_verts[tri[i]]));
      }
      tri_index++;
    }
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node.all_verts().size(), false));
}

static gpu::IndexBufPtr create_lines_index_faces_welded(const OffsetIndices<int> faces,
                                                        const Span<int> corner_verts,
                                                        const Span<bool> hide_poly,
                                                        const bke::pbvh::MeshNode &node)
{
  const Span<int> face_indices = node.faces();
  int corners_count = 0;
  for (const int face : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face]) {
      continue;
    }
    corners_count += faces[face].size();
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_LINES, corners_count, INT_MAX);
  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();

  int line_index = 0;
  for (const int face_index : face_indices) {
    if (!hide_poly.is_empty() && hide_poly[face_index]) {
      continue;
    }
    const IndexRange face = faces[face_index];
    const int face_size = face.size();
    for (const int i : IndexRange(face_size)) {
      const int next = (i == face_size - 1) ? 0 : i + 1;
      data[line_index] = uint2(uint(node.vert_indices_.index_of(corner_verts[face[i]])),
                               uint(node.vert_indices_.index_of(corner_verts[face[next]])));
      line_index++;
    }
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(&builder, 0, node.all_verts().size(), false));
}

static gpu::IndexBufPtr create_tri_index_grids(const CCGKey &key,
                                               const BitGroupVector<> &grid_hidden,
                                               const bool do_coarse,
                                               const Span<int> grid_indices,
                                               const bool use_flat_layout)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  uint visible_quad_len = bke::pbvh::count_grid_quads(
      grid_hidden, grid_indices, key.grid_size, display_gridsize);

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(&builder, GPU_PRIM_TRIS, 2 * visible_quad_len, INT_MAX);

  MutableSpan<uint3> data = GPU_indexbuf_get_data(&builder).cast<uint3>();

  int tri_count;
  if (use_flat_layout) {
    tri_count = create_tri_index_grids_flat_layout(
        grid_indices, grid_hidden, gridsize, skip, totgrid, data);
  }
  else {
    tri_count = create_tri_index_grids(grid_indices, grid_hidden, gridsize, skip, totgrid, data);
  }

  builder.index_len = tri_count * 3;
  builder.index_min = 0;
  builder.index_max = 6 * visible_quad_len;
  builder.uses_restart_indices = false;
  gpu::IndexBufPtr result = gpu::IndexBufPtr(GPU_indexbuf_calloc());
  GPU_indexbuf_build_in_place(&builder, result.get());
  return result;
}

static gpu::IndexBufPtr create_lines_index_grids(const CCGKey &key,
                                                 const BitGroupVector<> &grid_hidden,
                                                 const bool do_coarse,
                                                 const Span<int> grid_indices,
                                                 const bool use_flat_layout)
{
  int gridsize = key.grid_size;
  int display_gridsize = gridsize;
  int totgrid = grid_indices.size();
  int skip = 1;

  const int display_level = do_coarse ? 0 : key.level;

  if (display_level < key.level) {
    display_gridsize = (1 << display_level) + 1;
    skip = 1 << (key.level - display_level - 1);
  }

  GPUIndexBufBuilder builder;
  GPU_indexbuf_init(
      &builder, GPU_PRIM_LINES, 2 * totgrid * display_gridsize * (display_gridsize - 1), INT_MAX);

  MutableSpan<uint2> data = GPU_indexbuf_get_data(&builder).cast<uint2>();
  /* The buffer might contain hidden elements which are not initialized but still accounted. We
   * don't count them to skip from allocation, so must fill that gaps by 0 to hide redundant edges.
   */
  data.fill(uint2(0));

  if (use_flat_layout) {
    create_lines_index_grids_flat_layout(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, data);
  }
  else {
    create_lines_index_grids(
        grid_indices, display_gridsize, grid_hidden, gridsize, skip, totgrid, data);
  }

  return gpu::IndexBufPtr(GPU_indexbuf_build_ex(
      &builder, 0, 2 * totgrid * display_gridsize * (display_gridsize - 1), false));
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_lines_indices(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const IndexMask &node_mask,
                                                           const bool coarse,
                                                           const bool welded)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  Vector<gpu::IndexBufPtr> &ibos = welded ? lines_ibos_welded_ :
                                            (coarse ? lines_ibos_coarse_ : lines_ibos_);
  ibos.resize(pbvh.nodes_num());

  IndexMaskMemory memory;
  const IndexMask nodes_to_calculate = IndexMask::from_predicate(
      node_mask, memory, [&](const int i) { return !ibos[i]; });

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();
      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = welded ?
                          create_lines_index_faces_welded(faces, corner_verts, hide_poly, nodes[i]) :
                          create_lines_index_faces(faces, hide_poly, nodes[i].faces());
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::Grids: {
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
            const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);
            ibos[i] = create_lines_index_grids(
                key, subdiv_ccg.grid_hidden, coarse, nodes[i].grids(), use_flat_layout_[i]);
          },
          exec_mode::grain_size(1));
      break;
    }
    case bke::pbvh::Type::BMesh: {
      const Span<bke::pbvh::BMeshNode> nodes = pbvh.nodes<bke::pbvh::BMeshNode>();
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            const Set<BMFace *, 0> &faces = BKE_pbvh_bmesh_node_faces(
                &const_cast<bke::pbvh::BMeshNode &>(nodes[i]));
            const int visible_faces_num = count_visible_tris_bmesh(faces);
            ibos[i] = create_lines_index_bmesh(faces, visible_faces_num);
          },
          exec_mode::grain_size(1));
      break;
    }
  }

  return ibos;
}

BitSpan DrawCacheImpl::ensure_use_flat_layout(const Object &object,
                                              const OrigMeshData &orig_mesh_data)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (use_flat_layout_.size() != pbvh.nodes_num()) {
    use_flat_layout_ = calc_use_flat_layout(object, orig_mesh_data);
  }
  return use_flat_layout_;
}

/* これまでに見た要求をためて、その和で並べ方を1つに決める。 */
bool DrawCacheImpl::ensure_welded_layout(const Object &object,
                                         const OrigMeshData &orig_mesh_data,
                                         const Span<AttributeRequest> attributes)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  /* ★ここで #use_flat_layout_ の大きさを目印に使ってはいけない。メッシュ型では
   * #calc_use_flat_layout が空を返すので、その比較は**毎回真になる**。
   * ためた要求が毎回捨てられ、要求ごとに並べ方が分かれて二重に作られていた。 */
  if (attrs_seen_nodes_num_ != pbvh.nodes_num()) {
    attrs_seen_.clear();
    attrs_seen_nodes_num_ = pbvh.nodes_num();
  }
  for (const AttributeRequest &attr : attributes) {
    if (!attrs_seen_.contains(attr)) {
      attrs_seen_.append(attr);
    }
  }
  return mesh_can_use_welded(object, orig_mesh_data, attrs_seen_);
}

BLI_NOINLINE static void flush_vbo_data(const Span<gpu::VertBufPtr> vbos,
                                        const IndexMask &node_mask)
{
  node_mask.foreach_index([&](const int i) { GPU_vertbuf_use(vbos[i].get()); });
}

Span<gpu::VertBufPtr> DrawCacheImpl::ensure_attribute_data(const Object &object,
                                                           const OrigMeshData &orig_mesh_data,
                                                           const AttributeRequest &attr,
                                                           const IndexMask &node_mask,
                                                           const bool welded)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  AttributeData &data = (welded ? attribute_vbos_welded_ : attribute_vbos_)
                            .lookup_or_add_default(attr);
  Vector<gpu::VertBufPtr> &vbos = data.vbos;
  vbos.resize(pbvh.nodes_num());

  /* The nodes we recompute here are a combination of:
   *   1. null VBOs, which correspond to nodes that either haven't been drawn before, or have been
   *      cleared completely by #free_nodes_with_changed_topology.
   *   2. Nodes that have been tagged dirty as their values are changed.
   * We also only process a subset of the nodes referenced by the caller, for example to only
   * recompute visible nodes. */
  IndexMaskMemory memory;
  const IndexMask empty_mask = IndexMask::from_predicate(
      node_mask, memory, [&](const int i) { return !vbos[i]; });
  const IndexMask dirty_mask = IndexMask::from_bits(
      node_mask.slice_content(data.dirty_nodes.index_range()), data.dirty_nodes, memory);
  const IndexMask mask = IndexMask::from_union(empty_mask, dirty_mask, memory);

  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_mesh(object, mask, welded, vbos);
            break;
          case CustomRequest::Normal:
            update_normals_mesh(object, mask, welded, vbos);
            break;
          case CustomRequest::Mask:
            update_masks_mesh(object, orig_mesh_data, mask, welded, vbos);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_mesh(object, orig_mesh_data, mask, welded, vbos);
            break;
        }
      }
      else {
        update_generic_attribute_mesh(
            object, orig_mesh_data, mask, std::get<GenericRequest>(attr), welded, vbos);
      }
      break;
    }
    case bke::pbvh::Type::Grids: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            fill_positions_grids(object, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::Normal:
            fill_normals_grids(object, orig_mesh_data, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::Mask:
            fill_masks_grids(object, use_flat_layout_, mask, vbos);
            break;
          case CustomRequest::FaceSet:
            fill_face_sets_grids(object, orig_mesh_data, use_flat_layout_, mask, vbos);
            break;
        }
      }
      else {
        ensure_vbos_allocated_grids(
            object,
            attribute_format(orig_mesh_data, "Dummy", bke::AttrType::Float3),
            use_flat_layout_,
            mask,
            vbos);
        mask.foreach_index([&](const int i) { vbos[i]->data<float3>().fill(float3(0.0f)); },
                           exec_mode::grain_size(1));
      }
      break;
    }
    case bke::pbvh::Type::BMesh: {
      if (const CustomRequest *request_type = std::get_if<CustomRequest>(&attr)) {
        switch (*request_type) {
          case CustomRequest::Position:
            update_positions_bmesh(object, mask, vbos);
            break;
          case CustomRequest::Normal:
            update_normals_bmesh(object, mask, vbos);
            break;
          case CustomRequest::Mask:
            update_masks_bmesh(object, mask, vbos);
            break;
          case CustomRequest::FaceSet:
            update_face_sets_bmesh(object, orig_mesh_data, mask, vbos);
            break;
        }
      }
      else {
        update_generic_attribute_bmesh(
            object, orig_mesh_data, mask, std::get<GenericRequest>(attr), vbos);
      }
      break;
    }
  }

  /* TODO: It would be good to deallocate the bit vector if all of the bits have been reset to
   * avoid unnecessary processing in subsequent redraws. */
  dirty_mask.foreach_index_optimized<int>([&](const int i) { data.dirty_nodes[i].reset(); });

  flush_vbo_data(vbos, mask);

  return vbos;
}

Span<gpu::IndexBufPtr> DrawCacheImpl::ensure_tri_indices(const Object &object,
                                                         const OrigMeshData &orig_mesh_data,
                                                         const IndexMask &node_mask,
                                                         const bool coarse,
                                                         const bool welded)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  switch (pbvh.type()) {
    case bke::pbvh::Type::Mesh: {
      const Span<bke::pbvh::MeshNode> nodes = pbvh.nodes<bke::pbvh::MeshNode>();

      Vector<gpu::IndexBufPtr> &ibos = welded ? tris_ibos_welded_ : tris_ibos_;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) { return !ibos[i]; });

      const Mesh &mesh = DRW_object_get_data_for_drawing<Mesh>(object);
      const OffsetIndices<int> faces = mesh.faces();
      const Span<int> corner_verts = mesh.corner_verts();
      const Span<int3> corner_tris = mesh.corner_tris();
      const bke::AttributeAccessor attributes = orig_mesh_data.attributes;
      const VArraySpan hide_poly = *attributes.lookup<bool>(".hide_poly", bke::AttrDomain::Face);
      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = welded ? create_tri_index_mesh_welded(
                                   faces, corner_tris, corner_verts, hide_poly, nodes[i]) :
                               create_tri_index_mesh(faces, corner_tris, hide_poly, nodes[i]);
          },
          exec_mode::grain_size(1));
      return ibos;
    }
    case bke::pbvh::Type::Grids: {
      /* Unlike the other geometry types, multires grids use indexed vertex buffers because when
       * there are no flat faces, vertices can be shared between neighboring quads. This results in
       * a 4x decrease in the amount of data uploaded. Theoretically it also means freeing VBOs
       * because of visibility changes is unnecessary.
       *
       * TODO: With the "flat layout" and no hidden faces, the index buffers are unnecessary, we
       * should avoid creating them in that case. */
      const Span<bke::pbvh::GridsNode> nodes = pbvh.nodes<bke::pbvh::GridsNode>();

      Vector<gpu::IndexBufPtr> &ibos = coarse ? tris_ibos_coarse_ : tris_ibos_;
      ibos.resize(nodes.size());

      /* Whenever a node's visible triangle count has changed the index buffers are freed, so we
       * only recalculate null IBOs here. A new mask is recalculated for more even task
       * distribution between threads. */
      IndexMaskMemory memory;
      const IndexMask nodes_to_calculate = IndexMask::from_predicate(
          node_mask, memory, [&](const int i) { return !ibos[i]; });

      const SubdivCCG &subdiv_ccg = *object.runtime->sculpt_session->subdiv_ccg;
      const CCGKey key = BKE_subdiv_ccg_key_top_level(subdiv_ccg);

      nodes_to_calculate.foreach_index(
          [&](const int i) {
            ibos[i] = create_tri_index_grids(
                key, subdiv_ccg.grid_hidden, coarse, nodes[i].grids(), use_flat_layout_[i]);
          },
          exec_mode::grain_size(1));
      return ibos;
    }
    case bke::pbvh::Type::BMesh:
      return {};
  }
  BLI_assert_unreachable();
  return {};
}

Span<gpu::Batch *> DrawCacheImpl::ensure_tris_batches(const Object &object,
                                                      const ViewportRequest &request,
                                                      const IndexMask &nodes_to_update)
{
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data{*id_cast<const Mesh *>(object_orig.data)};
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->ensure_use_flat_layout(object, orig_mesh_data);
  this->free_nodes_with_changed_topology(pbvh);

  const bool welded = this->ensure_welded_layout(object, orig_mesh_data, request.attributes);
  /* 並べ方が変わったらバッチは中身が食い違うので、作り直す。
   * (要求が同じでも、面セットやシャープな面が後から足されると判定が反転する) */
  bool &batches_welded = tris_batches_welded_.lookup_or_add(request, welded);
  if (batches_welded != welded) {
    batches_welded = welded;
    if (Vector<gpu::Batch *> *stale = tris_batches_.lookup_ptr(request)) {
      free_batches(*stale, stale->index_range());
      stale->clear();
    }
  }

  const Span<gpu::IndexBufPtr> ibos = this->ensure_tri_indices(
      object, orig_mesh_data, nodes_to_update, request.use_coarse_grids, welded);

  for (const AttributeRequest &attr : request.attributes) {
    this->ensure_attribute_data(object, orig_mesh_data, attr, nodes_to_update, welded);
  }

  /* Collect VBO spans in a different loop because #ensure_attribute_data invalidates the allocated
   * arrays when its map is changed. */
  Vector<Span<gpu::VertBufPtr>> attr_vbos;
  for (const AttributeRequest &attr : request.attributes) {
    if (const AttributeData *attr_data = (welded ? attribute_vbos_welded_ : attribute_vbos_)
                                             .lookup_ptr(attr))
    {
      attr_vbos.append(attr_data->vbos);
    }
  }

  /* Except for the first iteration of the draw loop, we only need to rebuild batches for nodes
   * with changed topology (visible triangle count). */
  Vector<gpu::Batch *> &batches = tris_batches_.lookup_or_add_default(request);
  batches.resize(pbvh.nodes_num(), nullptr);
  nodes_to_update.foreach_index(
      [&](const int i) {
        if (!batches[i]) {
          batches[i] = GPU_batch_create(
              GPU_PRIM_TRIS, nullptr, ibos.is_empty() ? nullptr : ibos[i].get());
          for (const Span<gpu::VertBufPtr> vbos : attr_vbos) {
            GPU_batch_vertbuf_add(batches[i], vbos[i].get(), false);
          }
        }
      },
      exec_mode::grain_size(64));

  return batches;
}

Span<gpu::Batch *> DrawCacheImpl::ensure_lines_batches(const Object &object,
                                                       const ViewportRequest &request,
                                                       const IndexMask &nodes_to_update)
{
  const Object &object_orig = *DEG_get_original(&object);
  const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);

  this->ensure_use_flat_layout(object, orig_mesh_data);
  this->free_nodes_with_changed_topology(pbvh);

  /* 線は位置しか要らないが、頂点バッファを三角形側と共有するので、
   * 判定も同じ要求で行って並べ方を揃える(食い違うと同じ VBO を2通りで作り直す)。 */
  const bool welded = this->ensure_welded_layout(object, orig_mesh_data, request.attributes);

  const Span<gpu::VertBufPtr> position = this->ensure_attribute_data(
      object, orig_mesh_data, CustomRequest::Position, nodes_to_update, welded);
  const Span<gpu::IndexBufPtr> lines = this->ensure_lines_indices(
      object, orig_mesh_data, nodes_to_update, request.use_coarse_grids, welded);

  /* Except for the first iteration of the draw loop, we only need to rebuild batches for nodes
   * with changed topology (visible triangle count). */
  Vector<gpu::Batch *> &batches = request.use_coarse_grids ? lines_batches_coarse_ :
                                                             lines_batches_;
  if (lines_batches_welded_ != welded) {
    lines_batches_welded_ = welded;
    free_batches(batches, batches.index_range());
    batches.clear();
  }
  batches.resize(pbvh.nodes_num(), nullptr);
  nodes_to_update.foreach_index(
      [&](const int i) {
        if (!batches[i]) {
          batches[i] = GPU_batch_create(GPU_PRIM_LINES, nullptr, lines[i].get());
          GPU_batch_vertbuf_add(batches[i], position[i].get(), false);
        }
      },
      exec_mode::grain_size(64));

  return batches;
}

Span<int> DrawCacheImpl::ensure_material_indices(const Object &object)
{
  const bke::pbvh::Tree &pbvh = *bke::object::pbvh_get(object);
  if (material_indices_.size() != pbvh.nodes_num()) {
    const Object &object_orig = *DEG_get_original(&object);
    const OrigMeshData orig_mesh_data(*id_cast<const Mesh *>(object_orig.data));
    material_indices_ = calc_material_indices(object, orig_mesh_data);
  }
  return material_indices_;
}

}  // namespace draw::pbvh
}  // namespace blender
