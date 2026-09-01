/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include <cmath>
#include <cstdlib>

#include "node_shader_util.hh"

#include <algorithm>

#include "BKE_colortools.hh"

#include "BLI_math_vector.h"

#include "NOD_multi_function.hh"

#include "node_util.hh"

namespace blender {

#ifdef WITH_MATERIALX
namespace nodes::node_shader_curves_cc {

/* ★2026-08-30: CurveMapping の1本のカーブを MaterialX のノード列に組む。
 *
 * 形を3つに分ける。
 *   恒等   → 入力をそのまま返す（0 ノード）
 *   直線   → 乗算 + 加算（1〜3 ノード）
 *   その他 → 等間隔 N 点の折れ線を、ColorRamp（`node_shader_color_ramp.cc`）と同じ
 *            「左から mix で畳む」形にする。
 *              u   = (t - x0) / dx
 *              w_i = clamp(u - (i-1))
 *              acc = mix(acc, y[i], w_i)
 *            係数が両端で飽和するので、定義域の外は端の値で伸びる。
 *
 * ⚠恒等・直線の判定は BKE_curvemapping_is_map_identity を使わない。
 *   あの関数は curve[1] を (1,1) でなく (0,0) と比べているので実際のカーブでは
 *   常に false を返す（= 恒等でも畳み込みが走る。777 ノードの主因）。
 *   ここでは「実際に評価した値が端点を結ぶ直線と一致するか」で見る。
 *   Vector カーブ（定義域 -1..1）も同じ物差しで拾える。 */
enum class CurveShape { Identity, Affine, General };

static CurveShape classify_curve(const CurveMapping *cumap,
                                 int cur,
                                 bool use_clip,
                                 float x0,
                                 float x1,
                                 float &out_a,
                                 float &out_b)
{
  /* 戻す口: FALCON_MTLX_CURVE_FOLD=0 で畳み込みの削減を切り、全部 General にする。 */
  static const bool fold = []() {
    const char *e = std::getenv("FALCON_MTLX_CURVE_FOLD");
    return !(e && std::atoi(e) == 0);
  }();
  if (!fold) {
    return CurveShape::General;
  }

  auto eval = [&](float x) {
    float y = BKE_curvemap_evaluateF(cumap, &cumap->cm[cur], x);
    if (use_clip && (cumap->flag & CUMA_DO_CLIP)) {
      y = std::min(std::max(y, cumap->clipr.ymin), cumap->clipr.ymax);
    }
    return y;
  };

  const float ya = eval(x0);
  const float yb = eval(x1);
  const float a = (yb - ya) / (x1 - x0);
  const float b = ya - a * x0;

  /* 端点を結ぶ直線からのずれを 33 点で見る。 */
  const int probes = 33;
  float max_dev = 0.0f;
  for (int i = 0; i <= probes; i++) {
    const float x = x0 + (x1 - x0) * (float(i) / float(probes));
    max_dev = std::max(max_dev, std::abs(eval(x) - (a * x + b)));
  }
  if (max_dev > 1.0e-5f) {
    return CurveShape::General;
  }
  out_a = a;
  out_b = b;
  if (std::abs(a - 1.0f) <= 1.0e-6f && std::abs(b) <= 1.0e-6f) {
    return CurveShape::Identity;
  }
  return CurveShape::Affine;
}

static materialx::NodeItem curve_map_to_nodes(const CurveMapping *cumap,
                                              int cur,
                                              const materialx::NodeItem &t,
                                              bool use_clip)
{
  using NodeItem = materialx::NodeItem;

  /* 折れ線の点数。
   *
   * ⚠2026-08-30 実測: 32 点にすると mix の鎖が 1 カーブあたり約 124 ノードになり、
   * RGB カーブ 1 個(R/G/B + 合成 = 4 本)で材質 1 つが 777 ノードへ膨らむ。
   * classroom(60 材質)のシェーダー生成が 59 秒 → 21 分でも終わらなくなり、
   * 場面が録れなくなった。⇒ 32 点はやめる。既定は 12 点（classroom の実カーブで誤差 1/255 を切る最小点数。
   * 恒等/直線の畳み込みが入ったので、12 点でも 8-30 の 8 点より ND 合計は 27% 少ない）。
   *
   * FALCON_MTLX_CURVE_SAMPLES=N で点数を変えられる。
   * N <= 1 なら畳まずに入力をそのまま返す(= 2026-08-30 以前の素通しに戻る口)。 */
  static const int samples = []() {
    if (const char *e = std::getenv("FALCON_MTLX_CURVE_SAMPLES")) {
      const int v = std::atoi(e);
      return v > 64 ? 64 : v;
    }
    return 12;
  }();
  if (samples <= 1) {
    return t;
  }
  const float x0 = cumap->clipr.xmin;
  const float x1 = cumap->clipr.xmax;
  if (!(x1 > x0)) {
    return t;
  }

  float a = 1.0f, b = 0.0f;
  switch (classify_curve(cumap, cur, use_clip, x0, x1, a, b)) {
    case CurveShape::Identity:
      return t;
    case CurveShape::Affine: {
      NodeItem res = t;
      if (std::abs(a - 1.0f) > 1.0e-6f) {
        res = res * t.val(a);
      }
      if (std::abs(b) > 1.0e-6f) {
        res = res + t.val(b);
      }
      return res;
    }
    case CurveShape::General:
      break;
  }

  const float dx = (x1 - x0) / float(samples - 1);

  auto eval = [&](int i) {
    float y = BKE_curvemap_evaluateF(cumap, &cumap->cm[cur], x0 + dx * float(i));
    if (use_clip && (cumap->flag & CUMA_DO_CLIP)) {
      y = std::min(std::max(y, cumap->clipr.ymin), cumap->clipr.ymax);
    }
    return y;
  };

  /* 区間ごとに (t - xa)/dx を作り直すと除算が点数ぶん増える。
   * u = (t - x0)/dx を一度だけ作り、以降は整数の引き算で済ませる。 */
  NodeItem u = (t - t.val(x0)) * t.val(1.0f / dx);
  NodeItem res = t.val(eval(0));
  for (int i = 1; i < samples; i++) {
    NodeItem w = (i == 1) ? u.clamp() : (u - t.val(float(i - 1))).clamp();
    res = w.mix(res, t.val(eval(i)));
  }
  return res;
}

}  // namespace nodes::node_shader_curves_cc
#endif


namespace nodes::node_shader_curves_cc::vec {

static void sh_node_curve_vec_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr, "Fac"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .no_muted_links()
      .description("Amount of influence the node exerts on the output vector")
      .compositor_domain_priority(1);
  b.add_input<decl::Vector>("Vector"_ustr)
      .min(-1.0f)
      .max(1.0f)
      .description("Vector which would be mapped to the curve")
      .compositor_domain_priority(0);
  b.add_output<decl::Vector>("Vector"_ustr);
}

static void node_shader_init_curve_vec(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_curvemapping_add(3, -1.0f, -1.0f, 1.0f, 1.0f);
}

static int gpu_shader_curve_vec(GPUMaterial *mat,
                                bNode *node,
                                bNodeExecData * /*execdata*/,
                                GPUNodeStack *in,
                                GPUNodeStack *out)
{
  CurveMapping *curve_mapping = static_cast<CurveMapping *>(node->storage);

  BKE_curvemapping_init(curve_mapping);
  float *band_values;
  int band_size;
  BKE_curvemapping_table_RGBA(curve_mapping, &band_values, &band_size);
  float band_layer;
  GPUNodeLink *band_texture = GPU_color_band(mat, band_size, band_values, &band_layer);

  float start_slopes[CM_TOT];
  float end_slopes[CM_TOT];
  BKE_curvemapping_compute_slopes(curve_mapping, start_slopes, end_slopes);
  float range_minimums[CM_TOT];
  BKE_curvemapping_get_range_minimums(curve_mapping, range_minimums);
  float range_dividers[CM_TOT];
  BKE_curvemapping_compute_range_dividers(curve_mapping, range_dividers);

  return GPU_stack_link(mat,
                        node,
                        "curves_vector_mixed",
                        in,
                        out,
                        band_texture,
                        GPU_constant(&band_layer),
                        GPU_uniform(range_minimums),
                        GPU_uniform(range_dividers),
                        GPU_uniform(start_slopes),
                        GPU_uniform(end_slopes));
}

class CurveVecFunction : public mf::MultiFunction {
 private:
  /** Take ownership of the tree because it contains the curve mapping. */
  std::shared_ptr<const bNodeTree> tree_;
  const CurveMapping &cumap_;

 public:
  CurveVecFunction(const CurveMapping &cumap, std::shared_ptr<const bNodeTree> tree)
      : tree_(std::move(tree)), cumap_(cumap)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Curve Vec", signature};
      builder.single_input<float>("Fac");
      builder.single_input<float3>("Vector");
      builder.single_output<float3>("Vector");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &fac = params.readonly_single_input<float>(0, "Fac");
    const VArray<float3> &vec_in = params.readonly_single_input<float3>(1, "Vector");
    MutableSpan<float3> vec_out = params.uninitialized_single_output<float3>(2, "Vector");

    mask.foreach_index([&](const int64_t i) {
      BKE_curvemapping_evaluate3F(&cumap_, vec_out[i], vec_in[i]);
      if (fac[i] != 1.0f) {
        interp_v3_v3v3(vec_out[i], vec_in[i], vec_out[i], fac[i]);
      }
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(&cumap_);
  }
};

static void sh_node_curve_vec_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  CurveMapping *cumap = static_cast<CurveMapping *>(bnode.storage);
  BKE_curvemapping_init(cumap);
  builder.construct_and_set_matching_fn<CurveVecFunction>(*cumap, builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  NodeItem vector = get_input_value("Vector", NodeItem::Type::Vector3);
  CurveMapping *cumap = static_cast<CurveMapping *>(node_->storage);
  if (cumap == nullptr) {
    return vector;
  }
  BKE_curvemapping_init(cumap);

  /* ★定数入力は CPU で評価(RGB カーブと同じ理由・2026-08-31)。 */
  if (vector.value && get_input_value("Fac", NodeItem::Type::Float).value) {
    const MaterialX::Vector3 v = vector.value->asA<MaterialX::Vector3>();
    const float fac_v = get_input_value("Fac", NodeItem::Type::Float).value->asA<float>();
    float in[3] = {v[0], v[1], v[2]}, out[3];
    BKE_curvemapping_evaluate3F(cumap, out, in);
    return vector.val(MaterialX::Vector3(in[0] + (out[0] - in[0]) * fac_v,
                                          in[1] + (out[1] - in[1]) * fac_v,
                                          in[2] + (out[2] - in[2]) * fac_v));
  }

  NodeItem x = curve_map_to_nodes(cumap, 0, vector[0], false);
  NodeItem y = curve_map_to_nodes(cumap, 1, vector[1], false);
  NodeItem z = curve_map_to_nodes(cumap, 2, vector[2], false);
  NodeItem mapped = create_node(
      "combine3", NodeItem::Type::Vector3, {{"in1", x}, {"in2", y}, {"in3", z}});

  NodeItem fac = get_input_value("Fac", NodeItem::Type::Float);
  return fac.mix(vector, mapped);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_curves_cc::vec

void register_node_type_sh_curve_vec()
{
  namespace file_ns = nodes::node_shader_curves_cc::vec;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeVectorCurve"_ustr, SH_NODE_CURVE_VEC);
  ntype.ui_name = "Vector Curves";
  ntype.ui_description = "Map input vector components with curves";
  ntype.enum_name_legacy = "CURVE_VEC";
  ntype.nclass = NODE_CLASS_OP_VECTOR;
  ntype.declare = file_ns::sh_node_curve_vec_declare;
  ntype.initfunc = file_ns::node_shader_init_curve_vec;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.gpu_fn = file_ns::gpu_shader_curve_vec;
  ntype.build_multi_function = file_ns::sh_node_curve_vec_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

/* **************** CURVE RGB  ******************** */

namespace nodes::node_shader_curves_cc::rgb {

static void sh_node_curve_rgb_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr, "Fac"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .no_muted_links()
      .description("Amount of influence the node exerts on the output color")
      .compositor_domain_priority(1);
  b.add_input<decl::Color>("Color"_ustr)
      .default_value({1.0f, 1.0f, 1.0f, 1.0f})
      .description("Color input on which correction will be applied")
      .compositor_domain_priority(0);
  b.add_output<decl::Color>("Color"_ustr);
}

static void node_shader_init_curve_rgb(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_curvemapping_add(4, 0.0f, 0.0f, 1.0f, 1.0f);
}

static int gpu_shader_curve_rgb(GPUMaterial *mat,
                                bNode *node,
                                bNodeExecData * /*execdata*/,
                                GPUNodeStack *in,
                                GPUNodeStack *out)
{
  CurveMapping *curve_mapping = static_cast<CurveMapping *>(node->storage);

  BKE_curvemapping_init(curve_mapping);
  float *band_values;
  int band_size;
  BKE_curvemapping_table_RGBA(curve_mapping, &band_values, &band_size);
  float band_layer;
  GPUNodeLink *band_texture = GPU_color_band(mat, band_size, band_values, &band_layer);

  float start_slopes[CM_TOT];
  float end_slopes[CM_TOT];
  BKE_curvemapping_compute_slopes(curve_mapping, start_slopes, end_slopes);
  float range_minimums[CM_TOT];
  BKE_curvemapping_get_range_minimums(curve_mapping, range_minimums);
  float range_dividers[CM_TOT];
  BKE_curvemapping_compute_range_dividers(curve_mapping, range_dividers);

  /* Shader nodes don't do white balancing. */
  float black_level[4] = {0.0f, 0.0f, 0.0f, 1.0f};
  float white_level[4] = {1.0f, 1.0f, 1.0f, 1.0f};

  /* If the RGB curves do nothing, use a function that skips RGB computations. */
  if (BKE_curvemapping_is_map_identity(curve_mapping, 0) &&
      BKE_curvemapping_is_map_identity(curve_mapping, 1) &&
      BKE_curvemapping_is_map_identity(curve_mapping, 2))
  {
    return GPU_stack_link(mat,
                          node,
                          "curves_combined_only",
                          in,
                          out,
                          GPU_constant(black_level),
                          GPU_constant(white_level),
                          band_texture,
                          GPU_constant(&band_layer),
                          GPU_uniform(&range_minimums[3]),
                          GPU_uniform(&range_dividers[3]),
                          GPU_uniform(&start_slopes[3]),
                          GPU_uniform(&end_slopes[3]));
  }

  return GPU_stack_link(mat,
                        node,
                        "curves_combined_rgb",
                        in,
                        out,
                        GPU_constant(black_level),
                        GPU_constant(white_level),
                        band_texture,
                        GPU_constant(&band_layer),
                        GPU_uniform(range_minimums),
                        GPU_uniform(range_dividers),
                        GPU_uniform(start_slopes),
                        GPU_uniform(end_slopes));
}

class CurveRGBFunction : public mf::MultiFunction {
 private:
  /** Take ownership of the tree because it contains the curve mapping. */
  std::shared_ptr<const bNodeTree> tree_;
  const CurveMapping &cumap_;

 public:
  CurveRGBFunction(const CurveMapping &cumap, std::shared_ptr<const bNodeTree> tree)
      : tree_(std::move(tree)), cumap_(cumap)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Curve RGB", signature};
      builder.single_input<float>("Fac");
      builder.single_input<ColorGeometry4f>("Color");
      builder.single_output<ColorGeometry4f>("Color");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &fac = params.readonly_single_input<float>(0, "Fac");
    const VArray<ColorGeometry4f> &col_in = params.readonly_single_input<ColorGeometry4f>(1,
                                                                                          "Color");
    MutableSpan<ColorGeometry4f> col_out = params.uninitialized_single_output<ColorGeometry4f>(
        2, "Color");

    mask.foreach_index([&](const int64_t i) {
      BKE_curvemapping_evaluateRGBF(&cumap_, col_out[i], col_in[i]);
      if (fac[i] != 1.0f) {
        interp_v3_v3v3(col_out[i], col_in[i], col_out[i], fac[i]);
      }
      col_out[i].a = 1.0f;
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(&cumap_);
  }
};

static void sh_node_curve_rgb_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  CurveMapping *cumap = static_cast<CurveMapping *>(bnode.storage);
  BKE_curvemapping_init(cumap);
  builder.construct_and_set_matching_fn<CurveRGBFunction>(*cumap, builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  NodeItem color = get_input_value("Color", NodeItem::Type::Color3);
  CurveMapping *cumap = static_cast<CurveMapping *>(node_->storage);
  if (cumap == nullptr) {
    return color;
  }
  BKE_curvemapping_init(cumap);

  /* ★入力が定数なら CPU で評価して定数のまま返す(2026-08-31)。
   *   ノードを1つも作らない = 誤差ゼロ・下流の「リテラルを見て判断する」処理
   *   (falcon-live のポータル復元など)が今までどおり動く。
   *   ⚠ これを入れる前は、Sky Texture が未対応で入力が既定の黒に落ちた材質で
   *     `mix(bg=(0,0,0), fg=combine3(0,0,0))` というノードが残り、
   *     「黒のリテラル」を探す側が見つけられずに窓の発光が消えた(classroom 実測)。 */
  if (color.value && get_input_value("Fac", NodeItem::Type::Float).value) {
    const MaterialX::Color3 c = color.value->asA<MaterialX::Color3>();
    const float fac_v = get_input_value("Fac", NodeItem::Type::Float).value->asA<float>();
    float in[3] = {c[0], c[1], c[2]}, out[3];
    BKE_curvemapping_evaluateRGBF(cumap, out, in);
    return color.val(MaterialX::Color3(in[0] + (out[0] - in[0]) * fac_v,
                                        in[1] + (out[1] - in[1]) * fac_v,
                                        in[2] + (out[2] - in[2]) * fac_v));
  }

  /* `BKE_curvemapping_evaluateRGBF()` と同じ順。合成カーブ（3番）が先、次に各チャンネル。 */
  NodeItem channel[3];
  for (int i = 0; i < 3; i++) {
    NodeItem v = curve_map_to_nodes(cumap, 3, color[i], false);
    channel[i] = curve_map_to_nodes(cumap, i, v, false);
  }
  NodeItem mapped = create_node(
      "combine3",
      NodeItem::Type::Color3,
      {{"in1", channel[0]}, {"in2", channel[1]}, {"in3", channel[2]}});

  NodeItem fac = get_input_value("Fac", NodeItem::Type::Float);
  return fac.mix(color, mapped);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_curves_cc::rgb

void register_node_type_sh_curve_rgb()
{
  namespace file_ns = nodes::node_shader_curves_cc::rgb;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeRGBCurve"_ustr, SH_NODE_CURVE_RGB);
  ntype.ui_name = "RGB Curves";
  ntype.ui_description = "Apply color corrections for each color channel";
  ntype.enum_name_legacy = "CURVE_RGB";
  ntype.nclass = NODE_CLASS_OP_COLOR;
  ntype.declare = file_ns::sh_node_curve_rgb_declare;
  ntype.initfunc = file_ns::node_shader_init_curve_rgb;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.gpu_fn = file_ns::gpu_shader_curve_rgb;
  ntype.build_multi_function = file_ns::sh_node_curve_rgb_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

/* **************** CURVE FLOAT  ******************** */

namespace nodes::node_shader_curves_cc::flt {

static void sh_node_curve_float_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr)
      .min(0.0f)
      .max(1.0f)
      .default_value(1.0f)
      .subtype(PROP_FACTOR)
      .no_muted_links()
      .compositor_domain_priority(1);
  b.add_input<decl::Float>("Value"_ustr)
      .default_value(1.0f)
      .is_default_link_socket()
      .compositor_domain_priority(0);
  b.add_output<decl::Float>("Value"_ustr);
}

static void node_shader_init_curve_float(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_curvemapping_add(1, 0.0f, 0.0f, 1.0f, 1.0f);
}

static int gpu_shader_curve_float(GPUMaterial *mat,
                                  bNode *node,
                                  bNodeExecData * /*execdata*/,
                                  GPUNodeStack *in,
                                  GPUNodeStack *out)
{
  CurveMapping *curve_mapping = static_cast<CurveMapping *>(node->storage);

  BKE_curvemapping_init(curve_mapping);
  float *band_values;
  int band_size;
  BKE_curvemapping_table_RGBA(curve_mapping, &band_values, &band_size);
  float band_layer;
  GPUNodeLink *band_texture = GPU_color_band(mat, band_size, band_values, &band_layer);

  float start_slopes[CM_TOT];
  float end_slopes[CM_TOT];
  BKE_curvemapping_compute_slopes(curve_mapping, start_slopes, end_slopes);
  float range_minimums[CM_TOT];
  BKE_curvemapping_get_range_minimums(curve_mapping, range_minimums);
  float range_dividers[CM_TOT];
  BKE_curvemapping_compute_range_dividers(curve_mapping, range_dividers);

  return GPU_stack_link(mat,
                        node,
                        "curves_float_mixed",
                        in,
                        out,
                        band_texture,
                        GPU_constant(&band_layer),
                        GPU_uniform(range_minimums),
                        GPU_uniform(range_dividers),
                        GPU_uniform(start_slopes),
                        GPU_uniform(end_slopes));
}

class CurveFloatFunction : public mf::MultiFunction {
 private:
  /** Take ownership of the tree because it contains the curve mapping. */
  std::shared_ptr<const bNodeTree> tree_;
  const CurveMapping &cumap_;

 public:
  CurveFloatFunction(const CurveMapping &cumap, std::shared_ptr<const bNodeTree> tree)
      : tree_(std::move(tree)), cumap_(cumap)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Curve Float", signature};
      builder.single_input<float>("Factor");
      builder.single_input<float>("Value");
      builder.single_output<float>("Value");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &fac = params.readonly_single_input<float>(0, "Factor");
    const VArray<float> &val_in = params.readonly_single_input<float>(1, "Value");
    MutableSpan<float> val_out = params.uninitialized_single_output<float>(2, "Value");

    mask.foreach_index([&](const int64_t i) {
      val_out[i] = BKE_curvemapping_evaluateF(&cumap_, 0, val_in[i]);
      if (fac[i] != 1.0f) {
        val_out[i] = (1.0f - fac[i]) * val_in[i] + fac[i] * val_out[i];
      }
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(&cumap_);
  }
};

static void sh_node_curve_float_build_multi_function(NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  CurveMapping *cumap = static_cast<CurveMapping *>(bnode.storage);
  BKE_curvemapping_init(cumap);
  builder.construct_and_set_matching_fn<CurveFloatFunction>(*cumap, builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  NodeItem value = get_input_value("Value", NodeItem::Type::Float);
  CurveMapping *cumap = static_cast<CurveMapping *>(node_->storage);
  if (cumap == nullptr) {
    return value;
  }
  BKE_curvemapping_init(cumap);

  /* ★定数入力は CPU で評価(RGB カーブと同じ理由・2026-08-31)。 */
  if (value.value && get_input_value("Factor", NodeItem::Type::Float).value) {
    const float in = value.value->asA<float>();
    const float fac_v = get_input_value("Factor", NodeItem::Type::Float).value->asA<float>();
    float out = BKE_curvemapping_evaluateF(cumap, 0, in);
    if (cumap->flag & CUMA_DO_CLIP) {
      out = std::min(std::max(out, cumap->clipr.ymin), cumap->clipr.ymax);
    }
    return value.val(in + (out - in) * fac_v);
  }

  NodeItem mapped = curve_map_to_nodes(cumap, 0, value, true);
  NodeItem fac = get_input_value("Factor", NodeItem::Type::Float);
  return fac.mix(value, mapped);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_curves_cc::flt

void register_node_type_sh_curve_float()
{
  namespace file_ns = nodes::node_shader_curves_cc::flt;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeFloatCurve"_ustr, SH_NODE_CURVE_FLOAT);
  ntype.ui_name = "Float Curve";
  ntype.ui_description = "Map an input float to a curve and outputs a float value";
  ntype.enum_name_legacy = "CURVE_FLOAT";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_curve_float_declare;
  ntype.initfunc = file_ns::node_shader_init_curve_float;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(ntype, "CurveMapping", node_free_curves, node_copy_curves);
  ntype.gpu_fn = file_ns::gpu_shader_curve_float;
  ntype.build_multi_function = file_ns::sh_node_curve_float_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
