/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "infos/falcon_sculpt_infos.hh"

COMPUTE_SHADER_CREATE_INFO(falcon_sculpt_offset)

/* ブラシ本体(offset 系 = Draw / Nudge / Gravity)を丸ごとGPUで回す。
 *
 * CPU 側 `offset_positions` → `calc_faces` の順番をそのままなぞる:
 *
 *   0. マスクから初期値              factor = 1 - mask
 *   1. 表向きの面だけに絞る          calc_front_face
 *   2. ブラシ中心からの距離          calc_brush_distances (SPHERE)
 *   3. 半径の外を落とす              filter_distances_with_radius
 *   4. 硬さを距離に反映              apply_hardness_to_distances
 *   5. 落とし方の曲線を掛ける        calc_brush_strength_factors
 *   6. 移動量                        translations_from_offset_and_factors
 *   7. 軸の固定                      clip_and_lock_translations(ロックのみ)
 *   8. 書き戻し                      PositionDeformData::deform
 *
 * ★7 の鏡のクリップ(ミラーモディファイアの Clipping)はここでは再現しない。
 *   立っている時は CPU へ落とす。**推測で通すと絵が静かに変わる。**
 * ★順番を入れ替えてはいけない。硬さは**距離**を書き換えるので曲線より前。 */

/* 自分がどの触ったノードに属するかを、前置和の二分探索で引く。
 * ノード数は数百なので 10 段ほど。**番号の配列を毎ダブ作り直す**のをやめるための仕掛け。 */
int find_node_slot(int i)
{
  int lo = 0;
  int hi = node_count; /* touched_offsets の要素数は node_count + 1 */
  while (lo + 1 < hi) {
    int mid = (lo + hi) / 2;
    if (touched_offsets[mid] <= i) {
      lo = mid;
    }
    else {
      hi = mid;
    }
  }
  return lo;
}

float3 load_position(int vert)
{
  int b = vert * 3;
  return float3(positions[b], positions[b + 1], positions[b + 2]);
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

  int slot = find_node_slot(i);
  int vert = node_verts[node_starts[slot] + (i - touched_offsets[slot])];

  float3 pos = load_position(vert);

  float factor = use_mask ? (1.0f - vert_mask[vert]) : 1.0f;

  if (use_front_face) {
    int nb = i * 3;
    float3 nor = float3(normals[nb], normals[nb + 1], normals[nb + 2]);
    factor *= max(dot(view_normal, nor), 0.0f);
  }

  float dist = distance(pos, brush_location);

  if (dist >= brush_radius) {
    factor = 0.0f;
  }
  else {
    /* 硬さ。CPU 側 `apply_hardness_to_distances` と同じ形。 */
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
    factor *= curve_strength(dist);
  }

  /* ★出すのは**移動量**であって新しい位置ではない。
   * 変形モディファイアがあると、評価後の位置には移動量をそのまま足し、
   * 元の位置には crazyspace の逆行列を掛けてから足す(CPU 側
   * `apply_crazyspace_to_translations` と同じ)。位置を返すと
   * この2つを作り分けられない。
   *
   * ★半径の外の頂点も書く。移動量は 0 なので値は変わらないが、
   * **詰めた結果の並びは触った頂点ぶん全部**でないと CPU 側で戻す時にずれる。 */
  float3 translation = brush_offset * factor * axis_scale;

  if (write_inplace) {
    float3 new_pos = pos + translation;
    int b = vert * 3;
    positions[b] = new_pos.x;
    positions[b + 1] = new_pos.y;
    positions[b + 2] = new_pos.z;
  }

  int o = i * 3;
  out_translations[o] = translation.x;
  out_translations[o + 1] = translation.y;
  out_translations[o + 2] = translation.z;
}
