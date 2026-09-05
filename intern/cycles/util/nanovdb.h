/* SPDX-FileCopyrightText: Contributors to the OpenVDB Project
 * SPDX-FileCopyrightText: 2025 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#pragma once

#ifdef WITH_NANOVDB

#  include <openvdb/openvdb.h>

#  define NANOVDB_USE_OPENVDB
#  define NANOVDB_USE_TBB

#  include <nanovdb/NanoVDB.h>  // manages and streams the raw memory buffer of a NanoVDB grid.

#  if NANOVDB_MAJOR_VERSION_NUMBER > 32 || \
      (NANOVDB_MAJOR_VERSION_NUMBER == 32 && NANOVDB_MINOR_VERSION_NUMBER >= 7)
#    include <nanovdb/GridHandle.h>
#  else
#    include <nanovdb/util/GridHandle.h>
#  endif

CCL_NAMESPACE_BEGIN

/* Convert NanoVDB to OpenVDB mask grid that represents just the topology. */
openvdb::MaskGrid::Ptr nanovdb_to_openvdb_mask(const nanovdb::GridHandle<> &handle);

/* Falcon: NanoVDB のヘッダから、活性ボクセルの添字空間 bbox と index->world 変換を
 * そのまま読む。トポロジを OpenVDB マスクへ戻さないので、活性ボクセル数に比例しない。
 * 変換の作り方は nanovdb_to_openvdb_mask と同一 (同じ頂点が出ること = 門 G2)。
 * 戻り値 false = 型が判別できなかった (呼び出し側は従来の経路へ戻すこと)。 */
bool nanovdb_grid_bbox_and_transform(const nanovdb::GridHandle<> &handle,
                                     openvdb::CoordBBox &bbox,
                                     openvdb::math::Transform::Ptr &transform);

/* Falcon: 箱の再構築を飛ばす経路が有効か。既定は有効、FALCON_VOLUME_BBOX_FAST=0 で従来どおり。
 * この旗は openvdb_to_nanovdb 側の StatsMode も切り替える (bbox を書かせるため) ので、
 * 一箇所で読む。 */
bool falcon_volume_bbox_fast();

/* Convert OpenVDB to NanoVDB grid. */
nanovdb::GridHandle<> openvdb_to_nanovdb(const openvdb::GridBase::ConstPtr &grid,
                                         const int precision,
                                         const float clipping);

CCL_NAMESPACE_END

#endif
