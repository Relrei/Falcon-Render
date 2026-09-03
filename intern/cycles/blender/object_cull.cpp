/* SPDX-FileCopyrightText: 2011-2022 Blender Foundation
 *
 * SPDX-License-Identifier: Apache-2.0 */

#include <cstdlib>

#include "scene/camera.h"

#include "BLI_bounds.hh"

#include "blender/object_cull.h"
#include "blender/util.h"

CCL_NAMESPACE_BEGIN

BlenderObjectCulling::BlenderObjectCulling(Scene *scene, blender::Scene &b_scene)
    : use_scene_camera_cull_(false),
      use_camera_cull_(false),
      camera_cull_margin_(0.0f),
      use_scene_distance_cull_(false),
      use_distance_cull_(false),
      distance_cull_margin_(0.0f),
      use_render_region_(true)
{
  /* ★2026-09-01: パネルの「レンダー領域で絞る」で切れるようにした。
   *   レンダー領域が無効な時は Camera::border が [0,1] のままなので、
   *   真のままでも判定は従来と同じ式になる(= 効果が無い)。
   *   戻す口(headless など): FALCON_CULL_USE_BORDER=0 で強制的に切る。 */
  {
    const char *env = getenv("FALCON_CULL_USE_BORDER");
    if (env != nullptr && env[0] != '\0' && atoi(env) == 0) {
      use_render_region_ = false;
    }
  }
  if ((b_scene.r.mode & blender::R_SIMPLIFY) != 0) {
    blender::PointerRNA scene_rna_ptr = RNA_id_pointer_create(&b_scene.id);
    blender::PointerRNA cscene = RNA_pointer_get(&scene_rna_ptr, "cycles");

    const bool cam_supported = (scene->camera->get_camera_type() == CAMERA_PERSPECTIVE) ||
                               (scene->camera->get_camera_type() == CAMERA_ORTHOGRAPHIC);

    use_scene_camera_cull_ = cam_supported && ((b_scene.r.scemode & blender::R_MULTIVIEW) == 0) &&
                             get_boolean(cscene, "use_camera_cull");
    use_scene_distance_cull_ = cam_supported &&
                               ((b_scene.r.scemode & blender::R_MULTIVIEW) == 0) &&
                               get_boolean(cscene, "use_distance_cull");

    camera_cull_margin_ = get_float(cscene, "camera_cull_margin");
    distance_cull_margin_ = get_float(cscene, "distance_cull_margin");

    /* ★パネルの「レンダー領域で絞る」。既定は真だが、レンダー領域が無効なら効果は無い。 */
    if (!get_boolean(cscene, "use_camera_cull_border")) {
      use_render_region_ = false;
    }

    if (distance_cull_margin_ == 0.0f) {
      use_scene_distance_cull_ = false;
    }
  }
}

void BlenderObjectCulling::init_object(Scene *scene, blender::Object &b_ob)
{
  if (!use_scene_camera_cull_ && !use_scene_distance_cull_) {
    return;
  }

  blender::PointerRNA b_ob_rna_ptr = RNA_id_pointer_create(&b_ob.id);
  blender::PointerRNA cobject = RNA_pointer_get(&b_ob_rna_ptr, "cycles");

  use_camera_cull_ = use_scene_camera_cull_ && get_boolean(cobject, "use_camera_cull");
  use_distance_cull_ = use_scene_distance_cull_ && get_boolean(cobject, "use_distance_cull");

  if (use_camera_cull_ || use_distance_cull_) {
    /* Need to have proper projection matrix. */
    scene->camera->update(scene);
  }
}

bool BlenderObjectCulling::test(Scene *scene, blender::Object &b_ob, const Transform &tfm)
{
  if (!use_camera_cull_ && !use_distance_cull_) {
    return false;
  }

  /* Compute world space bounding box corners. */
  float3 bb[8];
  std::array<blender::float3, 8> boundbox;
  if (const std::optional<blender::Bounds<blender::float3>> bounds =
          BKE_object_boundbox_eval_cached_get(&b_ob))
  {
    boundbox = blender::bounds::corners(*bounds);
  }
  else {
    boundbox.fill(blender::float3(0));
  }
  for (int i = 0; i < 8; ++i) {
    const float3 p = make_float3(boundbox[i].x, boundbox[i].y, boundbox[i].z);
    bb[i] = transform_point(&tfm, p);
  }

  const bool camera_culled = use_camera_cull_ && test_camera(scene, bb);
  const bool distance_culled = use_distance_cull_ && test_distance(scene, bb);

  return ((camera_culled && distance_culled) || (camera_culled && !use_distance_cull_) ||
          (distance_culled && !use_camera_cull_));
}

/* TODO(sergey): Not really optimal, consider approaches based on k-DOP in order
 * to reduce number of objects which are wrongly considered visible.
 */
bool BlenderObjectCulling::test_camera(Scene *scene, const float3 bb[8])
{
  Camera *cam = scene->camera;
  const ProjectionTransform &worldtondc = cam->worldtondc;
  float3 bb_min = make_float3(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bb_max = make_float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);
  bool all_behind = true;
  for (int i = 0; i < 8; ++i) {
    float3 p = bb[i];
    const float4 b = make_float4(p, 1.0f);
    const float4 c = make_float4(
        dot(worldtondc.x, b), dot(worldtondc.y, b), dot(worldtondc.z, b), dot(worldtondc.w, b));
    p = make_float3(c / c.w);
    if (c.z < 0.0f) {
      p.x = 1.0f - p.x;
      p.y = 1.0f - p.y;
    }
    if (c.z >= -camera_cull_margin_) {
      all_behind = false;
    }
    bb_min = min(bb_min, p);
    bb_max = max(bb_max, p);
  }
  if (all_behind) {
    return true;
  }
  /* ★2026-09-01 Falcon: レンダー領域(Render Region)に従う。
   *
   * それまではここが常に NDC の [0,1] = **カメラのフル画面**と比べていたので、
   * レンダー領域を小さく切っても、領域の外にあるだけの物は読み込まれていた。
   * Camera::border は R_BORDER の時だけ領域が入り、それ以外は [0,1] のままなので、
   * ここで border を使えば「領域が無い時は今までどおり」が自動的に成り立つ。
   *
   * カリングの余白(camera_cull_margin)は今までどおり **NDC の絶対量**として扱う
   * (領域の幅に対する相対にすると、領域を小さくした時に余白まで縮んで意味が変わる)。
   *
   * 戻す口: FALCON_CULL_USE_BORDER=0 で従来どおりフル画面と比べる。 */
  float left = 0.0f;
  float right = 1.0f;
  float bottom = 0.0f;
  float top = 1.0f;
  if (use_render_region_) {
    left = cam->get_border_left();
    right = cam->get_border_right();
    bottom = cam->get_border_bottom();
    top = cam->get_border_top();
  }
  return (bb_min.x >= right + camera_cull_margin_ || bb_min.y >= top + camera_cull_margin_ ||
          bb_max.x <= left - camera_cull_margin_ || bb_max.y <= bottom - camera_cull_margin_);
}

bool BlenderObjectCulling::test_distance(Scene *scene, const float3 bb[8])
{
  const float3 camera_position = transform_get_column(&scene->camera->get_matrix(), 3);
  float3 bb_min = make_float3(FLT_MAX, FLT_MAX, FLT_MAX);
  float3 bb_max = make_float3(-FLT_MAX, -FLT_MAX, -FLT_MAX);

  /* Find min & max points for x & y & z on bounding box */
  for (int i = 0; i < 8; ++i) {
    const float3 p = bb[i];
    bb_min = min(bb_min, p);
    bb_max = max(bb_max, p);
  }

  const float3 closest_point = max(min(bb_max, camera_position), bb_min);
  return (len_squared(camera_position - closest_point) >
          distance_cull_margin_ * distance_cull_margin_);
}

CCL_NAMESPACE_END
