/* SPDX-FileCopyrightText: 2011 by Nicholas Bishop.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup modifiers
 */

#include <cstdio>
#include <algorithm>

#include "MEM_guardedalloc.h"

#include "BLI_fileops.h"
#include "BLI_math_base.h"
#include "BLI_offset_indices.hh"
#include "BLI_array.hh"
#include "BLI_math_vector.hh"
#include "BLI_span.hh"
#include "BLI_mutex.hh"
#include "BLI_utildefines.h"

#include "BLT_translation.hh"

#include "DNA_modifier_types.h"
#include "DNA_screen_types.h"

#include "BKE_mesh.hh"
#include "BKE_lib_id.hh"
#include "BKE_attribute.hh"
#include "BKE_shrinkwrap.hh"
#include "BKE_mesh_remesh_voxel.hh"
#include "BKE_mesh_runtime.hh"

#include "UI_interface_layout.hh"
#include "UI_resources.hh"

#include "RNA_access.hh"
#include "RNA_prototypes.hh"

#include "MOD_modifiertypes.hh"
#include "MOD_ui_common.hh"

#include "GEO_randomize.hh"

#include <cmath>
#include <cstdlib>
#include <cstring>

#ifdef WITH_MOD_REMESH
#  include "BLI_math_vector.h"

#  include "dualcon.h"
#endif

namespace blender {

static void init_data(ModifierData *md)
{
  RemeshModifierData *rmd = reinterpret_cast<RemeshModifierData *>(md);
  INIT_DEFAULT_STRUCT_AFTER(rmd, modifier);
}

#ifdef WITH_MOD_REMESH

static void init_dualcon_mesh(DualConInput *input, Mesh *mesh)
{
  memset(input, 0, sizeof(DualConInput));

  input->co = (DualConCo)mesh->vert_positions().data();
  input->co_stride = sizeof(float3);
  input->totco = mesh->verts_num;

  input->corner_verts = (DualConCornerVerts)mesh->corner_verts().data();
  input->corner_verts_stride = sizeof(int);

  input->corner_tris = (DualConTri)mesh->corner_tris().data();
  input->tri_stride = sizeof(int3);
  input->tottri = BKE_mesh_runtime_corner_tris_len(mesh);

  const Bounds<float3> bounds = *mesh->bounds_min_max();
  copy_v3_v3(input->min, bounds.min);
  copy_v3_v3(input->max, bounds.max);
}

/* simple structure to hold the output: a CDDM and two counters to
 * keep track of the current elements */
struct DualConOutput {
  Mesh *mesh;
  float3 *vert_positions;
  int *face_offsets;
  int *corner_verts;
  int curvert, curface;
};

/* allocate and initialize a DualConOutput */
static void *dualcon_alloc_output(int totvert, int totquad)
{
  DualConOutput *output = MEM_new_zeroed<DualConOutput>(__func__);

  if (!output) {
    return nullptr;
  }

  output->mesh = BKE_mesh_new_nomain(totvert, 0, totquad, 4 * totquad);
  output->vert_positions = output->mesh->vert_positions_for_write().data();
  output->face_offsets = output->mesh->face_offsets_for_write().data();
  output->corner_verts = output->mesh->corner_verts_for_write().data();

  return output;
}

static void dualcon_add_vert(void *output_v, const float co[3])
{
  DualConOutput *output = static_cast<DualConOutput *>(output_v);

  BLI_assert(output->curvert < output->mesh->verts_num);

  copy_v3_v3(output->vert_positions[output->curvert], co);
  output->curvert++;
}

static void dualcon_add_quad(void *output_v, const int vert_indices[4])
{
  DualConOutput *output = static_cast<DualConOutput *>(output_v);
  Mesh *mesh = output->mesh;
  int i;

  BLI_assert(output->curface < mesh->faces_num);
  UNUSED_VARS_NDEBUG(mesh);

  output->face_offsets[output->curface] = output->curface * 4;
  for (i = 0; i < 4; i++) {
    output->corner_verts[output->curface * 4 + i] = vert_indices[i];
  }

  output->curface++;
}

/* ★目標の面数が大きすぎる時は**走らせない**(2026-08-17に警告から変更)。
 *
 * 「警告を出して止めない」形にしていたが、実際に**マシンごと固まった。**
 * 数字を直接打つと 21億まで通り、目標面数が大きいほどボクセルが細かくなって
 * OpenVDB の格子が膨らむ。31GB の機械で 3000万面を入れたら、メモリ 28GB・
 * SWAP 18GB まで行って戻ってこなくなった(CPU 100% のまま5分以上)。
 * **警告は画面に出るが、その頃には操作を受け付けない。**
 *
 * 実測(正20面体 subdiv=5 を入力・8GBの枠の中で測定):
 *   目標50万 -> 1.18GB / 100万 -> 1.52 / 200万 -> 1.93 / 400万 -> 2.69 / 800万 -> 4.31
 * → **100万面あたり約 0.42GB**(0.8GB は Blender 自身の基礎)。
 *   面数のずれはどれも ±0.0% で、精度の方は問題ない。
 * この直線だと 1億面で 43GB。31GB の機械では必ず落ちる。 */
static constexpr double MOD_REMESH_GB_BASE = 0.8;
static constexpr double MOD_REMESH_GB_PER_MILLION = 0.42;
/* 空きメモリのうち、ここまでなら使ってよいという割合。 */
static constexpr double MOD_REMESH_MEM_BUDGET = 0.7;

/**
 * いま実際に確保できるメモリ(バイト)。取れなければ 0。
 *
 * ★`BLI_system_memory_max_in_megabytes` は**アドレス空間の上限**を返すので使えない
 * (物理メモリとは無関係の巨大な値)。
 * Linux の `MemAvailable` は「swap に行かずに確保できる量」で、まさに知りたい値。
 */
static size_t remesh_available_memory_bytes()
{
#ifdef __linux__
  FILE *f = BLI_fopen("/proc/meminfo", "r");
  if (f == nullptr) {
    return 0;
  }
  char line[256];
  size_t bytes = 0;
  while (fgets(line, sizeof(line), f)) {
    unsigned long long kb = 0;
    if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
      bytes = size_t(kb) * 1024;
      break;
    }
  }
  fclose(f);
  return bytes;
#else
  return 0;
#endif
}

/* 目標の面数に合わせる焼き直しの上限。adaptivity 併用時だけ通る。 */
static constexpr int MOD_REMESH_TARGET_PASSES = 4;

static Mesh *modify_mesh(ModifierData *md, const ModifierEvalContext *ctx, Mesh *mesh)
{
  RemeshModifierData *rmd = (RemeshModifierData *)md;
  Mesh *result;

  if (rmd->mode == MOD_REMESH_VOXEL) {
    /* OpenVDB modes. */
    float voxel_size = rmd->voxel_size;

    /* ★目標の面数から寸法を逆算する(2026-08-16 追加)。
     *
     * ボクセルリメッシュは「ボクセル寸法」で指定するが、**欲しいのは面数**
     * であることが多い(20万ポリで、等)。寸法と面数の関係はメッシュの
     * 表面積で決まるので、手で当てるしかなかった。
     *
     * ★割るのは表面積ではなく**軸へ投影した面積の和**。
     *
     * ボクセル面は階段状なので、軸に対して傾いた面ほど枚数が増える。
     * 面 1 枚が生む階段の枚数は、その法線を3軸へ投影した和
     * (|nx| + |ny| + |nz|)に比例する:
     *   - 軸に平行な板   -> 1.0
     *   - Z軸の円柱      -> |nx|+|ny| の平均 = 4/pi = 1.27
     *   - 球            -> |nx|+|ny|+|nz| の平均 = 1.5
     * 素の面積で割った時、実測でちょうどこの比(板1.00 / 円柱1.24〜1.31 /
     * 球1.50 / 凹凸1.46〜1.51)になった。**形の中では目標を50倍振っても
     * 比が動かない**ので、式の形は正しく係数だけが形で決まっていた。
     *
     *     faces ~= S / voxel^2,  S = Σ area_i * (|nx|+|ny|+|nz|)
     *     -> voxel = sqrt(S / faces)
     *
     * これなら**1回で当たる**。二分探索でリメッシュを何度も回す必要は無い
     * (リメッシュは重いので、回数がそのまま待ち時間になる)。
     *
     * 面積は入力メッシュのもの。リメッシュ後は少し変わるが、寸法は平方根で
     * 効くので、面積の数%のずれは寸法の数%にしかならない。 */
    if (rmd->target_faces > 0) {
      const Span<float3> positions = mesh->vert_positions();
      const OffsetIndices faces = mesh->faces();
      const Span<int> corner_verts = mesh->corner_verts();
      const Span<float3> face_normals = mesh->face_normals();
      float projected = 0.0f;
      for (const int i : faces.index_range()) {
        const float a = bke::mesh::face_area_calc(positions, corner_verts.slice(faces[i]));
        const float3 n = face_normals[i];
        projected += a * (std::abs(n.x) + std::abs(n.y) + std::abs(n.z));
      }
      if (projected > 0.0f) {
        voxel_size = std::sqrt(projected / float(rmd->target_faces));
      }
    }

    if (voxel_size == 0.0f) {
      BKE_modifier_set_error(ctx->object, md, "Zero voxel size cannot be solved");
      return nullptr;
    }

    /* ★走らせる前に止める(2026-08-17)。走り出してからでは操作を受け付けない。
     * 時間は 1000万面でも 11秒で困らない。伸びるのは**メモリだけ**。 */
    if (rmd->target_faces > 0) {
      const double need_gb = MOD_REMESH_GB_BASE +
                             MOD_REMESH_GB_PER_MILLION * (double(rmd->target_faces) / 1.0e6);
      const size_t avail = remesh_available_memory_bytes();
      if (avail > 0) {
        const double budget_gb = double(avail) / (1024.0 * 1024.0 * 1024.0) *
                                 MOD_REMESH_MEM_BUDGET;
        if (need_gb > budget_gb) {
          /* ★案内する面数は**下へ丸める**。見積もりは空きメモリから作るので
           * 呼ぶたびに数万面ずれる(実測で 20361384 / 20315868 / 20222828)。
           * 8桁そのまま出すと「その数なら必ず通る正確な値」に見えてしまう。
           * 丸めた値の方が、揺れても同じ数字が出て信用できる。 */
          int can_do = int((budget_gb - MOD_REMESH_GB_BASE) /
                           MOD_REMESH_GB_PER_MILLION * 1.0e6);
          if (can_do >= 1000000) {
            can_do = (can_do / 1000000) * 1000000;
          }
          else if (can_do >= 100000) {
            can_do = (can_do / 100000) * 100000;
          }
          else {
            can_do = 0;
          }
          if (can_do > 0) {
            BKE_modifier_set_error(ctx->object,
                                   md,
                                   "Target Faces %d needs about %.0f GB, but only %.0f GB is "
                                   "usable. Try %d or less",
                                   rmd->target_faces,
                                   need_gb,
                                   budget_gb,
                                   can_do);
          }
          else {
            /* 空きが基礎の分にも足りない。**「0 以下を試せ」とは言わない。** */
            BKE_modifier_set_error(ctx->object,
                                   md,
                                   "Target Faces %d needs about %.0f GB, but only %.0f GB is "
                                   "usable. Free some memory first",
                                   rmd->target_faces,
                                   need_gb,
                                   budget_gb);
          }
          return nullptr;
        }
      }
    }

    result = BKE_mesh_remesh_voxel(mesh, voxel_size, rmd->adaptivity, 0.0f, ctx->object, md);
    if (result == nullptr) {
      return nullptr;
    }

    /* ★目標の面数と adaptivity を併用した時だけ、もう一度焼いて合わせる。
     *
     * adaptivity は平らな所を粗くするので、面数が目標を大きく下回る
     * (実測: 平らな側が 1/57 に減る一方、凹凸側は 1/2.3 にしか減らない)。
     * その減り方は曲率の分布で決まるので、面積からは予測できない。
     *
     * ★**安い粗い試し焼きでは代用できない。**減り率はボクセル寸法で
     * 0.51(0.08m)->0.22(0.01m)と倍以上動くと実測した。細かくするほど凹凸が
     * 「曲がっている」と判定されて残るため。よって本番と同じ細かさで1回焼き、
     * 出た面数から寸法を補正して焼き直す、の2段になる。
     *
     * 代償は時間だけで、**落ちやすくはならない**(順番に走るのでピークの
     * メモリは1回分と同じ)。実用域(100〜500万面)なら2回でも2〜8秒。
     * adaptivity が 0 の時は1回で 2% 以内に当たるので、この段は通らない。 */
    if (rmd->target_faces > 0 && rmd->adaptivity > 0.0f) {
      /* ★1回の補正では届かない。**補正で細かくすると adaptivity の減り率も
       * さらに下がる**(実測: 減り率が 0.51(0.08m) -> 0.22(0.01m))ので、
       * 補正が自分で打ち消し合う。1回だけだと比 0.45〜0.71 で止まった。
       * 縮んでいく方向ではあるので、5% に入るまで繰り返す。
       *
       * 上限4回は待ち時間の歯止め。実測で1回あたり 0.1〜1.0秒(30万面・
       * adaptivity 1.0)なので、4回でも数秒に収まる。届かないまま抜けた時は
       * 最後の結果をそのまま返す(**目標に届かないことより、止まらないことの
       * 方が困る**)。 */
      /* ★1周ごとに「寸法と面数の関係」を測り直して当てにいく(2026-08-16 追加)。
       *
       * adaptivity が 0 なら faces ∝ 1/voxel^2 で、指数は 2 で当たる。
       * adaptivity を入れると**指数が 2 より大きくなる**: 細かくするほど凹凸が
       * 「曲がっている」と判定されて残るので、面数が 1/voxel^2 より速く増える。
       * 指数 2 のまま補正すると毎回足りず、4周かけて近づくことになっていた。
       *
       * 直前の2点 (voxel, faces) から、その場の指数を出して次を決める:
       *     faces ∝ voxel^-k  ->  k = ln(f_prev/f_now) / ln(v_now/v_prev)
       *     次の voxel = voxel * off^(1/k)
       *
       * これは「adaptivity ごとの正しい指数」を表から引くのではなく、
       * **そのメッシュのその場で測る**やり方。曲率の分布はメッシュごとに違うので、
       * 定数として持たせても当たらない(実測で減り率が 0.51 -> 0.22 と倍以上動く)。 */
      float prev_voxel = 0.0f;
      int prev_faces = 0;
      for (int pass = 0; pass < MOD_REMESH_TARGET_PASSES; pass++) {
        const int got = result->faces_num;
        if (got <= 0) {
          break;
        }
        const float off = float(got) / float(rmd->target_faces);
        if (off > 0.95f && off < 1.05f) {
          break;
        }

        /* 既定は 2(adaptivity 0 と同じ。1周目は測る材料が無いのでここを通る) */
        float k = 2.0f;
        if (prev_faces > 0 && prev_voxel > 0.0f && prev_voxel != voxel_size && got != prev_faces) {
          const float fitted = std::log(float(prev_faces) / float(got)) /
                               std::log(voxel_size / prev_voxel);
          /* 外れ値を採らない。ノイズで指数が 0 近くまで落ちると次の寸法が
           * 発散して、**目標の何十倍もの面数を焼こうとしてメモリを食う**。
           * 想定の範囲(1〜8)に入っている時だけ採用する。 */
          if (std::isfinite(fitted) && fitted > 1.0f && fitted < 8.0f) {
            k = fitted;
          }
        }

        prev_voxel = voxel_size;
        prev_faces = got;
        voxel_size *= std::pow(off, 1.0f / k);

        Mesh *next = BKE_mesh_remesh_voxel(
            mesh, voxel_size, rmd->adaptivity, 0.0f, ctx->object, md);
        if (next == nullptr) {
          break;
        }
        BKE_id_free(nullptr, result);
        result = next;
      }
    }
  }
  else {
    if (rmd->scale == 0.0f) {
      BKE_modifier_set_error(ctx->object, md, "Zero scale cannot be solved");
      return nullptr;
    }

    DualConOutput *output;
    DualConInput input;
    DualConFlags flags = DualConFlags(0);
    DualConMode mode = DualConMode(0);

    /* Dualcon modes. */
    init_dualcon_mesh(&input, mesh);

    if (rmd->flag & MOD_REMESH_FLOOD_FILL) {
      flags = DualConFlags(flags | DUALCON_FLOOD_FILL);
    }

    switch (rmd->mode) {
      case MOD_REMESH_CENTROID:
        mode = DUALCON_CENTROID;
        break;
      case MOD_REMESH_MASS_POINT:
        mode = DUALCON_MASS_POINT;
        break;
      case MOD_REMESH_SHARP_FEATURES:
        mode = DUALCON_SHARP_FEATURES;
        break;
      case MOD_REMESH_VOXEL:
        /* Should have been processed before as an OpenVDB operation. */
        BLI_assert(false);
        break;
    }
    /* TODO(jbakker): Dualcon crashes when run in parallel. Could be related to incorrect
     * input data or that the library isn't thread safe.
     * This was identified when changing the task isolation's during #76553. */
    static Mutex dualcon_mutex;
    {
      std::scoped_lock lock(dualcon_mutex);
      output = static_cast<DualConOutput *>(dualcon(&input,
                                                    dualcon_alloc_output,
                                                    dualcon_add_vert,
                                                    dualcon_add_quad,
                                                    flags,
                                                    mode,
                                                    rmd->threshold,
                                                    rmd->hermite_num,
                                                    rmd->scale,
                                                    rmd->depth));
    }
    result = output->mesh;
    MEM_delete(output);
  }

  /* ★マスクを尊重する(2026-08-16 追加)。
   *
   * リメッシュはボクセルの粗さでしか形を残せないので、細かい所は丸くなる。
   * スカルプトでマスクを塗るのは「ここは触るな」という意思表示なのに、
   * **リメッシュ側はマスクを一度も見ていなかった**(実装にもオペレータにも
   * 参照ゼロ)。属性としては引き継がれるが、形は他と同じように潰れる。
   *
   * やり方: 出来たメッシュを元の面へ投影し直し、**マスクの値で混ぜて戻す**。
   * マスク1の頂点は元の面まで完全に引き戻り、0の頂点は新しいまま。間は滑らかに
   * 変わるので、**継ぎ目ができない**(領域を切り分けて別々にリメッシュして
   * 縫う方式だと、境界が必ず不連続になる)。
   *
   * 形は戻るがトポロジーは新しいまま、という割り切り。マスクした所の
   * **頂点配置まで完全に保つ**にはメッシュの縫合が要り、別物の難しさになる。
   * ここで狙うのは「masked な細部が丸まって消えない」ところまで。
   *
   * `mesh_remesh_reproject_attributes` の後に置く必要がある(混ぜるための
   * マスクは、そこで引き継がれた側の値を使うため)。 */
  auto restore_masked = [&](Mesh &dst) {
    const bke::AttributeAccessor src_attrs = mesh->attributes();
    if (!src_attrs.contains(".sculpt_mask")) {
      return;
    }
    const bke::AttributeAccessor dst_attrs = dst.attributes();
    const VArray<float> mask = *dst_attrs.lookup<float>(".sculpt_mask", bke::AttrDomain::Point);
    if (!mask) {
      return;
    }

    /* ★塗られていないなら投影ごと飛ばす。
     *
     * 一度でもマスクを使ったメッシュは、**全部消した後も `.sculpt_mask` を持ち続ける**
     * (値が全部ゼロで残る)。属性の有無だけで判定すると、そういうメッシュ全部が
     * 毎回フルの shrinkwrap 投影を払って、結果は最後に 100% 捨てられる。
     * m=0 の頂点は必ず投影前の位置へ戻るので、**捨てると分かっている計算**。
     * 先に最大値を見る1周(頂点数に線形・BVHもメモリも要らない)で丸ごと省ける。 */
    bool any_masked = false;
    for (const int i : mask.index_range()) {
      if (mask[i] > 1e-4f) {
        any_masked = true;
        break;
      }
    }
    if (!any_masked) {
      return;
    }

    MutableSpan<float3> positions = dst.vert_positions_for_write();
    Array<float3> before(positions.size());
    before.as_mutable_span().copy_from(positions);

    BKE_shrinkwrap_remesh_target_project(&dst, const_cast<Mesh *>(mesh), ctx->object);

    /* 投影は全頂点を動かすので、マスクの値で元へ戻す。
     * m=1 -> 投影後(元の面の上) / m=0 -> リメッシュのまま */
    for (const int i : positions.index_range()) {
      const float m = std::clamp(mask[i], 0.0f, 1.0f);
      positions[i] = math::interpolate(before[i], positions[i], m);
    }
    dst.tag_positions_changed();
  };

  /* ★属性の引き継ぎ(2026-08-16 追加)。
   *
   * リメッシュは頂点を作り直すので、UV・頂点カラー・マスクは黙って落ちる。
   * スカルプトモードの Voxel Remesh(Ctrl+R)は
   * `bke::mesh_remesh_reproject_attributes` を通してこれを引き継いでいる
   * (editors/object/object_remesh.cc:154)のに、**モディファイア経路だけが
   * 呼んでいなかった**。同じメッシュを同じ設定でリメッシュしても、
   * 経路によって結果が違う状態だった。
   *
   * 判定は**メッシュ側の既存フラグ**を見る。モディファイア独自の設定を
   * 足さないのは、同じことを2箇所で切り替えられるようにすると
   * 「どちらが効いているか」を確かめる組み合わせが倍になるため。
   * `ME_REMESH_REPROJECT_ATTRIBUTES` は既定でオン
   * (DNA_mesh_types.h:252)なので、既定の挙動が「引き継ぐ」に変わる。
   *
   * 実装側は属性が無ければ index map も BVH も作らずに返るので、
   * 属性を持たないメッシュでは実質ただの分岐1つ。 */
  if (mesh->flag & ME_REMESH_REPROJECT_ATTRIBUTES) {
    bke::mesh_remesh_reproject_attributes(*mesh, *result);
    restore_masked(*result);
  }

  bke::mesh_smooth_set(*result, rmd->flag & MOD_REMESH_SMOOTH_SHADING);

  BKE_mesh_copy_parameters_for_eval(result, mesh);
  bke::mesh_calc_edges(*result, true, false);

  geometry::debug_randomize_mesh_order(result);

  return result;
}

#else /* !WITH_MOD_REMESH */

static Mesh *modify_mesh(ModifierData * /*md*/, const ModifierEvalContext * /*ctx*/, Mesh *mesh)
{
  return mesh;
}

#endif /* !WITH_MOD_REMESH */

static void panel_draw(const bContext * /*C*/, Panel *panel)
{
  ui::Layout &layout = *panel->layout;
#ifdef WITH_MOD_REMESH

  PointerRNA ob_ptr;
  PointerRNA *ptr = modifier_panel_get_property_pointers(panel, &ob_ptr);

  int mode = RNA_enum_get(ptr, "mode");

  layout.prop(ptr, "mode", ui::ITEM_R_EXPAND, std::nullopt, ICON_NONE);

  layout.use_property_split_set(true);

  ui::Layout &col = layout.column(false);
  if (mode == MOD_REMESH_VOXEL) {
    /* 目標の面数が入っている間は Voxel Size は使われないので、
     * 触れないようにして「どちらが効いているか」を見えるようにする。
     * 両方が有効に見えると、片方を変えても効かない理由が分からなくなる。 */
    const int target_faces = RNA_int_get(ptr, "target_faces");
    ui::Layout &sub = col.column(false);
    sub.active_set(target_faces == 0);
    sub.prop(ptr, "voxel_size", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "target_faces", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "adaptivity", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  else {
    col.prop(ptr, "octree_depth", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    col.prop(ptr, "scale", UI_ITEM_NONE, std::nullopt, ICON_NONE);

    if (mode == MOD_REMESH_SHARP_FEATURES) {
      col.prop(ptr, "sharpness", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    }

    layout.prop(ptr, "use_remove_disconnected", UI_ITEM_NONE, std::nullopt, ICON_NONE);
    ui::Layout &row = layout.row(false);
    row.active_set(RNA_boolean_get(ptr, "use_remove_disconnected"));
    row.prop(ptr, "threshold", UI_ITEM_NONE, std::nullopt, ICON_NONE);
  }
  layout.prop(ptr, "use_smooth_shade", UI_ITEM_NONE, std::nullopt, ICON_NONE);

  modifier_error_message_draw(layout, ptr);

#else  /* WITH_MOD_REMESH */
  layout.label(RPT_("Built without Remesh modifier"), ICON_NONE);
#endif /* WITH_MOD_REMESH */
}

static void panel_register(ARegionType *region_type)
{
  modifier_panel_register(region_type, eModifierType_Remesh, panel_draw);
}

ModifierTypeInfo modifierType_Remesh = {
    /*idname*/ "Remesh",
    /*name*/ N_("Remesh"),
    /*struct_name*/ "RemeshModifierData",
    /*struct_size*/ sizeof(RemeshModifierData),
    /*srna*/ &RNA_RemeshModifier,
    /*type*/ ModifierTypeType::Nonconstructive,
    /*flags*/ eModifierTypeFlag_AcceptsMesh | eModifierTypeFlag_AcceptsCVs |
        eModifierTypeFlag_SupportsEditmode,
    /*icon*/ ICON_MOD_REMESH,

    /*copy_data*/ BKE_modifier_copydata_generic,

    /*deform_verts*/ nullptr,
    /*deform_matrices*/ nullptr,
    /*deform_verts_EM*/ nullptr,
    /*deform_matrices_EM*/ nullptr,
    /*modify_mesh*/ modify_mesh,
    /*modify_geometry_set*/ nullptr,

    /*init_data*/ init_data,
    /*required_data_mask*/ nullptr,
    /*free_data*/ nullptr,
    /*is_disabled*/ nullptr,
    /*update_depsgraph*/ nullptr,
    /*depends_on_time*/ nullptr,
    /*depends_on_normals*/ nullptr,
    /*foreach_ID_link*/ nullptr,
    /*foreach_tex_link*/ nullptr,
    /*free_runtime_data*/ nullptr,
    /*panel_register*/ panel_register,
    /*blend_write*/ nullptr,
    /*blend_read*/ nullptr,
    /*foreach_cache*/ nullptr,
    /*foreach_working_space_color*/ nullptr,
};

}  // namespace blender
