/* SPDX-FileCopyrightText: Contributors to the OpenVDB Project
 * SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#ifdef WITH_NANOVDB

#  include "util/nanovdb.h"
#  include "util/log.h"
#  include "util/openvdb.h"

#  include <cstdlib>
#  include <cstring>

#  include <openvdb/tools/Activate.h>

#  include <nanovdb/util/ForEach.h>

#  if NANOVDB_MAJOR_VERSION_NUMBER > 32 || \
      (NANOVDB_MAJOR_VERSION_NUMBER == 32 && NANOVDB_MINOR_VERSION_NUMBER >= 7)
#    include <nanovdb/tools/CreateNanoGrid.h>
#  else
#    include <nanovdb/util/OpenToNanoVDB.h>
#  endif

CCL_NAMESPACE_BEGIN

/* Convert NanoVDB to OpenVDB mask grid.
 *
 * Implementation adapted from nanoToOpenVDB in nanovdb, this will create
 * a MaskGrid from any type of grid using the active voxels. */

template<typename NanoBuildT> class NanoToOpenVDBMask {
  using NanoNode0 = nanovdb::LeafNode<NanoBuildT, openvdb::Coord, openvdb::util::NodeMask>;
  using NanoNode1 = nanovdb::InternalNode<NanoNode0>;
  using NanoNode2 = nanovdb::InternalNode<NanoNode1>;
  using NanoRootT = nanovdb::RootNode<NanoNode2>;
  using NanoTreeT = nanovdb::Tree<NanoRootT>;
  using NanoGridT = nanovdb::Grid<NanoTreeT>;
  using NanoValueT = typename NanoGridT::ValueType;

  using OpenBuildT = openvdb::ValueMask;
  using OpenNode0 = openvdb::tree::LeafNode<OpenBuildT, NanoNode0::LOG2DIM>;
  using OpenNode1 = openvdb::tree::InternalNode<OpenNode0, NanoNode1::LOG2DIM>;
  using OpenNode2 = openvdb::tree::InternalNode<OpenNode1, NanoNode2::LOG2DIM>;
  using OpenRootT = openvdb::tree::RootNode<OpenNode2>;
  using OpenTreeT = openvdb::tree::Tree<OpenRootT>;
  using OpenGridT = openvdb::Grid<OpenTreeT>;
  using OpenValueT = typename OpenGridT::ValueType;

 public:
  NanoToOpenVDBMask() = default;
  typename OpenGridT::Ptr operator()(const nanovdb::NanoGrid<NanoBuildT> &grid = 0);

 private:
  template<typename NanoNodeT, typename OpenNodeT>
  OpenNodeT *processNode(const NanoNodeT * /*node*/);

  OpenNode2 *process(const NanoNode2 *node)
  {
    return this->template processNode<NanoNode2, OpenNode2>(node);
  }
  OpenNode1 *process(const NanoNode1 *node)
  {
    return this->template processNode<NanoNode1, OpenNode1>(node);
  }

  template<typename NanoLeafT> OpenNode0 *process(const NanoLeafT *node);
};

template<typename NanoBuildT>
typename NanoToOpenVDBMask<NanoBuildT>::OpenGridT::Ptr NanoToOpenVDBMask<NanoBuildT>::operator()(
    const nanovdb::NanoGrid<NanoBuildT> &grid)
{
  /* Since the input nanovdb grid might use nanovdb types (Coord, Mask, Vec3)
   * we cast to use openvdb types. */
  const NanoGridT *srcGrid = reinterpret_cast<const NanoGridT *>(&grid);
  auto dstGrid = openvdb::createGrid<OpenGridT>(OpenValueT());

  /* Set transform. */
  const nanovdb::Map &nanoMap = reinterpret_cast<const nanovdb::GridData *>(srcGrid)->mMap;
  auto mat = openvdb::math::Mat4<double>::identity();
  mat.setMat3(openvdb::math::Mat3<double>(nanoMap.mMatD));
  mat = mat.transpose(); /* The 3x3 in nanovdb is transposed relative to openvdb's 3x3. */
  mat.setTranslation(openvdb::math::Vec3<double>(nanoMap.mVecD));
  dstGrid->setTransform(openvdb::math::Transform::createLinearTransform(mat));

  /* Process root node. */
  auto &root = dstGrid->tree().root();
  auto *data = srcGrid->tree().root().data();
  for (uint32_t i = 0; i < data->mTableSize; ++i) {
    auto *tile = data->tile(i);
    if (tile->isChild()) {
      root.addChild(this->process(data->getChild(tile)));
    }
    else {
      root.addTile(tile->origin(), OpenValueT(), tile->state);
    }
  }

  return dstGrid;
}

template<typename T>
template<typename SrcNodeT, typename DstNodeT>
DstNodeT *NanoToOpenVDBMask<T>::processNode(const SrcNodeT *srcNode)
{
  DstNodeT *dstNode = new DstNodeT(); /* Un-initialized for fast construction. */
  dstNode->setOrigin(srcNode->origin());
  const auto &childMask = srcNode->childMask();
  const_cast<typename DstNodeT::NodeMaskType &>(dstNode->getValueMask()) = srcNode->valueMask();
  const_cast<typename DstNodeT::NodeMaskType &>(dstNode->getChildMask()) = childMask;
  auto *dstTable = const_cast<typename DstNodeT::UnionType *>(dstNode->getTable());
  auto *srcData = srcNode->data();
  std::vector<std::pair<uint32_t, const typename SrcNodeT::ChildNodeType *>> childNodes;
  const auto childCount = childMask.countOn();
  childNodes.reserve(childCount);
  for (uint32_t n = 0; n < DstNodeT::NUM_VALUES; ++n) {
    if (childMask.isOn(n)) {
      childNodes.emplace_back(n, srcData->getChild(n));
    }
  }
  auto kernel = [&](const auto &r) {
    for (auto i = r.begin(); i != r.end(); ++i) {
      auto &p = childNodes[i];
      dstTable[p.first].setChild(this->process(p.second));
    }
  };

#  if NANOVDB_MAJOR_VERSION_NUMBER > 32 || \
      (NANOVDB_MAJOR_VERSION_NUMBER == 32 && NANOVDB_MINOR_VERSION_NUMBER >= 7)
  nanovdb::util::forEach(0, childCount, 1, kernel);
#  else
  nanovdb::forEach(0, childCount, 1, kernel);
#  endif

  return dstNode;
}

template<typename T>
template<typename NanoLeafT>
inline typename NanoToOpenVDBMask<T>::OpenNode0 *NanoToOpenVDBMask<T>::process(
    const NanoLeafT *srcNode)
{
  static_assert(std::is_same_v<NanoLeafT, NanoNode0>, "NanoToOpenVDBMask wrong leaf type");
  OpenNode0 *dstNode = new OpenNode0(); /* Un-initialized for fast construction. */
  dstNode->setOrigin(srcNode->origin());
  dstNode->setValueMask(srcNode->valueMask());

  return dstNode;
}

struct NanoToOpenVDBMaskOp {
  openvdb::MaskGrid::Ptr mask_grid;

  template<typename NanoBuildT> bool operator()(const nanovdb::NanoGrid<NanoBuildT> &grid)
  {
    NanoToOpenVDBMask<NanoBuildT> tmp;
    mask_grid = tmp(grid);
    return true;
  }
};

template<typename OpType>
bool nanovdb_grid_type_operation(const nanovdb::GridHandle<> &handle, OpType &&op)
{
  const int n = 0;

  if (const auto *grid = handle.template grid<float>(n)) {
    return op(*grid);
  }
  if (const auto *grid = handle.template grid<nanovdb::Fp16>(n)) {
    return op(*grid);
  }
  if (const auto *grid = handle.template grid<nanovdb::FpN>(n)) {
    return op(*grid);
  }
  if (const auto *grid = handle.template grid<nanovdb::Vec3f>(n)) {
    return op(*grid);
  }
  if (const auto *grid = handle.template grid<nanovdb::Vec4f>(n)) {
    return op(*grid);
  }

  assert(!"Unknown NanoVDB grid type");
  return true;
}

openvdb::MaskGrid::Ptr nanovdb_to_openvdb_mask(const nanovdb::GridHandle<> &handle)
{
  NanoToOpenVDBMaskOp op;
  nanovdb_grid_type_operation(handle, op);
  return op.mask_grid;
}

/* Falcon: 箱を求めるためだけの OpenVDB マスク作り直しを飛ばす。
 *
 * 既定 (volume_biased=False) では境界メッシュは箱の6面しか作らないのに、その箱を求めるために
 * 毎コマ全グリッドを NanoVDB -> OpenVDB マスクへ戻して和を取り、活性ボクセルの bbox を
 * 評価していた。この段だけが解像度に対して res^2.74 で伸びる (2026-09-04 実測)。
 * NanoVDB は同じ bbox を自分のヘッダに持っているので、そこから読む。
 *
 * ⚠ ただし bbox は StatsMode::Disable では書かれない (GridStats.h: Disable は即 return し、
 *   葉の mBBoxDif は 0 のまま、根の mBBox は空)。openvdb_to_nanovdb 側で StatsMode::BBox を
 *   選ばせる必要がある。 */

bool falcon_volume_bbox_fast()
{
  /* 空文字は「未設定」でなく「有効」。無効にするのは明示的な "0" だけ。 */
  const char *value = getenv("FALCON_VOLUME_BBOX_FAST");
  return value ? strcmp(value, "0") != 0 : true;
}

struct NanoBBoxOp {
  openvdb::CoordBBox bbox;
  openvdb::math::Transform::Ptr transform;

  template<typename NanoBuildT> bool operator()(const nanovdb::NanoGrid<NanoBuildT> &grid)
  {
    /* 変換の組み立ては NanoToOpenVDBMask::operator() と同一。ここがずれると頂点がずれる。 */
    const nanovdb::GridData *grid_data = reinterpret_cast<const nanovdb::GridData *>(&grid);
    const nanovdb::Map &nanoMap = grid_data->mMap;
    auto mat = openvdb::math::Mat4<double>::identity();
    mat.setMat3(openvdb::math::Mat3<double>(nanoMap.mMatD));
    mat = mat.transpose(); /* The 3x3 in nanovdb is transposed relative to openvdb's 3x3. */
    mat.setTranslation(openvdb::math::Vec3<double>(nanoMap.mVecD));
    transform = openvdb::math::Transform::createLinearTransform(mat);

    /* nanovdb の CoordBBox は max が内包 (inclusive)。openvdb の
     * evalActiveVoxelBoundingBox と同じ約束。空のグリッドでは min>max の反転した箱が返り、
     * openvdb::CoordBBox::empty() が真になる。
     * Grid::indexBBox() はこの版ではコメントアウトされているので GridData 側から読む。 */
    const nanovdb::CoordBBox &nb = grid_data->indexBBox();
    bbox = openvdb::CoordBBox(openvdb::Coord(nb.min()[0], nb.min()[1], nb.min()[2]),
                              openvdb::Coord(nb.max()[0], nb.max()[1], nb.max()[2]));
    return true;
  }
};

bool nanovdb_grid_bbox_and_transform(const nanovdb::GridHandle<> &handle,
                                     openvdb::CoordBBox &bbox,
                                     openvdb::math::Transform::Ptr &transform)
{
  NanoBBoxOp op;
  if (!nanovdb_grid_type_operation(handle, op)) {
    return false;
  }
  if (!op.transform) {
    return false;
  }
  bbox = op.bbox;
  transform = op.transform;
  return true;
}

/* Convert OpenVDB to NanoVDB grid. */

struct ToNanoOp {
  nanovdb::GridHandle<> nanogrid;
  int precision = 16;
  float clipping = 0.0f;

  template<typename GridType, typename FloatDataType, const int channels>
  bool operator()(const typename GridType::ConstPtr &grid)
  {
    if constexpr (std::is_same_v<GridType, openvdb::MaskGrid>) {
      return false;
    }

    try {
#  if NANOVDB_MAJOR_VERSION_NUMBER > 32 || \
      (NANOVDB_MAJOR_VERSION_NUMBER == 32 && NANOVDB_MINOR_VERSION_NUMBER >= 6)
#    if NANOVDB_MAJOR_VERSION_NUMBER > 32 || \
        (NANOVDB_MAJOR_VERSION_NUMBER == 32 && NANOVDB_MINOR_VERSION_NUMBER >= 7)
      /* OpenVDB 12. */
      using nanovdb::tools::createNanoGrid;
      using nanovdb::tools::StatsMode;
#    else
      /* OpenVDB 11. */
      using nanovdb::createNanoGrid;
      using nanovdb::StatsMode;
#    endif

      /* Falcon: StatsMode::Disable だと活性ボクセルの bbox が書かれない (葉の mBBoxDif が 0 の
       * まま、根の mBBox は空)。BBox は最小・最大の統計を取らず bbox だけ埋めるので、
       * 葉あたり 64語の走査で済む。これが nanovdb_grid_bbox_and_transform の前提。 */
      const StatsMode stats_mode = falcon_volume_bbox_fast() ? StatsMode::BBox :
                                                               StatsMode::Disable;
      (void)stats_mode; /* 速度グリッド (Vec3f) の実体化では使われない。 */
      if constexpr (std::is_same_v<GridType, openvdb::FloatGrid>) {
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        if (precision == 0) {
          nanogrid = createNanoGrid<openvdb::FloatGrid, nanovdb::FpN>(*floatgrid, stats_mode);
        }
        else if (precision == 16) {
          nanogrid = createNanoGrid<openvdb::FloatGrid, nanovdb::Fp16>(*floatgrid, stats_mode);
        }
        else {
          nanogrid = createNanoGrid<openvdb::FloatGrid, float>(*floatgrid, stats_mode);
        }
      }
      else if constexpr (std::is_same_v<GridType, openvdb::Vec3fGrid>) {
        /* Enable stats for velocity grid. Weak, but there seems to be no simple iterator over all
         * values in the grid? */
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        nanogrid = createNanoGrid<openvdb::Vec3fGrid, nanovdb::Vec3f>(*floatgrid,
                                                                      StatsMode::MinMax);
      }
      else if constexpr (std::is_same_v<GridType, openvdb::Vec4fGrid>) {
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        nanogrid = createNanoGrid<openvdb::Vec4fGrid, nanovdb::Vec4f>(*floatgrid, stats_mode);
      }
#  else
      /* OpenVDB 10. */
      if constexpr (std::is_same_v<GridType, openvdb::FloatGrid>) {
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        if (precision == 0) {
          nanogrid = nanovdb::openToNanoVDB<nanovdb::HostBuffer, openvdb::FloatTree, nanovdb::FpN>(
              *floatgrid);
        }
        else if (precision == 16) {
          nanogrid =
              nanovdb::openToNanoVDB<nanovdb::HostBuffer, openvdb::FloatTree, nanovdb::Fp16>(
                  *floatgrid);
        }
        else {
          nanogrid = nanovdb::openToNanoVDB(*floatgrid);
        }
      }
      else if constexpr (std::is_same_v<FloatGridType, openvdb::Vec3fGrid>) {
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        nanogrid = nanovdb::openToNanoVDB(*floatgrid);
      }
      else if constexpr (std::is_same_v<FloatGridType, openvdb::Vec4fGrid>) {
        typename GridType::ConstPtr floatgrid = apply_clipping<GridType>(grid);
        nanogrid = nanovdb::openToNanoVDB(*floatgrid);
      }
#  endif
    }
    catch (const std::exception &e) {
      LOG_ERROR << "Error converting OpenVDB to NanoVDB grid: " << e.what();
    }
    catch (...) {
      LOG_ERROR << "Error converting OpenVDB to NanoVDB grid: Unknown error";
    }
    return true;
  }

  template<typename GridT>
  typename GridT::ConstPtr apply_clipping(const typename GridT::ConstPtr &grid) const
  {
    if (clipping == 0.0f) {
      return grid;
    }

    /* TODO: Ideally would apply clipping during createNanoGrid, this seems slow. */
    typename GridT::Ptr newgrid = grid->deepCopy();
    openvdb::tools::deactivate(
        *newgrid, typename GridT::ValueType(0.0f), typename GridT::ValueType(clipping));
    return newgrid;
  }
};

nanovdb::GridHandle<> openvdb_to_nanovdb(const openvdb::GridBase::ConstPtr &grid,
                                         const int precision,
                                         const float clipping)
{
  ToNanoOp op;
  op.precision = precision;
  op.clipping = clipping;
  openvdb_grid_type_operation(grid, op);
  return std::move(op.nanogrid);
}

CCL_NAMESPACE_END

#endif
