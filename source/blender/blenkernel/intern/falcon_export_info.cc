/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup bke
 */

#include "BKE_falcon_export_info.hh"

#include "DNA_ID.h"
#include "DNA_scene_types.h"

#include "BLI_string.h"

#include <cstdio>
#include <map>
#include <mutex>

namespace blender::bke {

namespace {

struct ExportInfo {
  /* -1 = まだ決めていない / 0 = 使わなかった / 1 = 使った */
  int fastpath = -1;
  std::string fastpath_detail;
  std::string encoder;
  double seconds = -1.0;
};

std::mutex &info_mutex()
{
  static std::mutex mutex;
  return mutex;
}

std::map<std::string, ExportInfo> &info_map()
{
  static std::map<std::string, ExportInfo> map;
  return map;
}

/* ★鍵はシーンの ID 名。ポインタで持つとシーンが消えた後に指せる。 */
ExportInfo *info_find(const Scene *scene, const bool create)
{
  if (scene == nullptr) {
    return nullptr;
  }
  const std::string key(scene->id.name);
  std::map<std::string, ExportInfo> &map = info_map();
  auto it = map.find(key);
  if (it == map.end()) {
    if (!create) {
      return nullptr;
    }
    it = map.emplace(key, ExportInfo()).first;
  }
  return &it->second;
}

}  // namespace

void falcon_export_info_begin(const Scene *scene)
{
  std::lock_guard<std::mutex> lock(info_mutex());
  ExportInfo *info = info_find(scene, true);
  if (info != nullptr) {
    *info = ExportInfo();
  }
}

void falcon_export_info_set_fastpath(const Scene *scene, const bool used, const char *detail)
{
  std::lock_guard<std::mutex> lock(info_mutex());
  ExportInfo *info = info_find(scene, true);
  if (info == nullptr) {
    return;
  }
  info->fastpath = used ? 1 : 0;
  info->fastpath_detail = (detail != nullptr) ? detail : "";
}

void falcon_export_info_set_encoder(const Scene *scene,
                                    const bool hardware,
                                    const char *codec_name)
{
  std::lock_guard<std::mutex> lock(info_mutex());
  ExportInfo *info = info_find(scene, true);
  if (info == nullptr) {
    return;
  }
  std::string name = (codec_name != nullptr) ? codec_name : "";
  if (hardware) {
    /* "h264_nvenc" -> "h264"。名乗りの側に NVENC を出すので二度書かない。 */
    const size_t at = name.rfind("_nvenc");
    if (at != std::string::npos) {
      name.erase(at);
    }
    info->encoder = "NVENC " + name;
  }
  else {
    info->encoder = "CPU " + name;
  }
}

void falcon_export_info_end(const Scene *scene, const double seconds)
{
  std::lock_guard<std::mutex> lock(info_mutex());
  ExportInfo *info = info_find(scene, false);
  if (info != nullptr) {
    info->seconds = seconds;
  }
}

std::string falcon_export_info_get(const Scene *scene)
{
  std::lock_guard<std::mutex> lock(info_mutex());
  const ExportInfo *info = info_find(scene, false);
  if (info == nullptr || info->fastpath < 0) {
    return "";
  }

  std::string line = "fast path: ";
  if (info->fastpath == 1) {
    line += "使った";
    if (!info->fastpath_detail.empty()) {
      line += " (" + info->fastpath_detail + ")";
    }
  }
  else {
    line += "使わない";
    if (!info->fastpath_detail.empty()) {
      line += " — " + info->fastpath_detail;
    }
  }

  line += " / 符号化: ";
  if (!info->encoder.empty()) {
    line += info->encoder;
  }
  else if (info->fastpath == 1) {
    /* 切っただけの経路は素材のパケットをそのまま流すので符号化器を選ばない。 */
    line += "そのままコピー";
  }
  else {
    line += "—";
  }

  if (info->seconds >= 0.0) {
    char buf[64];
    SNPRINTF(buf, " / %.1f s", info->seconds);
    line += buf;
  }
  return line;
}

}  // namespace blender::bke
