/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/falcon_sculpt_infos.hh"

COMPUTE_SHADER_CREATE_INFO(falcon_sculpt_factors)

/* CPU 側(`sculpt.cc`)の順番をそのままなぞる:
 *
 *   0. マスクから初期値              fill_factor_from_hide_and_mask (factor = 1 - mask)
 *   1. 表向きの面だけに絞る          calc_front_face
 *   2. ブラシ中心からの距離          calc_brush_distances (SPHERE)
 *   3. 半径の外を落とす              filter_distances_with_radius
 *   4. 硬さを距離に反映              apply_hardness_to_distances
 *   5. 落とし方の曲線を掛ける        calc_brush_strength_factors
 *
 * ★順番を入れ替えてはいけない。硬さは**距離**を書き換えるので、曲線より前に
 * 来る必要がある。逆にすると硬さが効かなくなる(絵で見ても分かりにくい種類の差)。
 *
 * 対応しない条件(マスク・自動マスク・テクスチャ・領域クリップ・TUBE)は、
 * そもそもGPUを呼ばずCPUへ落とす。ここでは判定しない。 */

float3 load_vec3(int base)
{
  return float3(positions[base], positions[base + 1], positions[base + 2]);
}

float3 load_normal(int base)
{
  return float3(normals[base], normals[base + 1], normals[base + 2]);
}

/* 焼いた表を線形に引く。CPU の曲線評価そのものではなく**表の補間**なので、
 * わずかな差が出る。表の細かさで詰める(既定 1024 段)。 */
float curve_strength(float dist)
{
  float t = clamp(dist / brush_radius, 0.0f, 1.0f);
  float x = t * float(curve_lut_size - 1);
  int i0 = int(x);
  int i1 = min(i0 + 1, curve_lut_size - 1);
  return mix(curve_lut[i0], curve_lut[i1], x - float(i0));
}

void main()
{
  int i = int(gl_GlobalInvocationID.x);
  if (i >= vert_count) {
    return;
  }

  int vert = vert_indices[i];
  float3 pos = load_vec3(vert * 3);

  float factor = use_mask ? (1.0f - vert_mask[vert]) : 1.0f;

  if (use_front_face) {
    float d = dot(view_normal, load_normal(vert * 3));
    factor *= max(d, 0.0f);
  }

  float dist = distance(pos, brush_location);

  if (dist >= brush_radius) {
    factors[i] = 0.0f;
    return;
  }

  /* 硬さ。CPU 側 `apply_hardness_to_distances` と同じ形にする。
   * hardness == 1 は 0 か radius かの二値になるが、ここは radius に
   * 届いた時点で上で返しているので、実質 0 になる側だけが残る。 */
  if (brush_hardness > 0.0f) {
    float threshold = brush_hardness * brush_radius;
    if (dist < threshold) {
      dist = 0.0f;
    }
    else if (brush_hardness >= 1.0f) {
      dist = brush_radius;
    }
    else {
      float radius_factor = (dist / brush_radius - brush_hardness) / (1.0f - brush_hardness);
      dist = radius_factor * brush_radius;
    }
  }

  factors[i] = factor * curve_strength(dist);
}
