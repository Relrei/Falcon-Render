/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup shdnodes
 */

#include "DNA_texture_types.h"

#include "BKE_colorband.hh"

#include "BLI_color_types.hh"

#include "NOD_multi_function.hh"

#include "node_shader_util.hh"
#include "node_util.hh"

namespace blender {

namespace nodes::node_shader_color_ramp_cc {

static void sh_node_valtorgb_declare(NodeDeclarationBuilder &b)
{
  b.is_function_node();
  b.add_input<decl::Float>("Factor"_ustr, "Fac"_ustr)
      .default_value(0.5f)
      .min(0.0f)
      .max(1.0f)
      .subtype(PROP_FACTOR)
      .description(
          "The value used to map onto the color gradient. 0.0 results in the leftmost color, "
          "while 1.0 results in the rightmost");
  b.add_output<decl::Color>("Color"_ustr);
  b.add_output<decl::Float>("Alpha"_ustr);
}

static void node_shader_init_valtorgb(bNodeTree * /*ntree*/, bNode *node)
{
  node->storage = BKE_colorband_add(true);
}

static int gpu_shader_valtorgb(GPUMaterial *mat,
                               bNode *node,
                               bNodeExecData * /*execdata*/,
                               GPUNodeStack *in,
                               GPUNodeStack *out)
{
  ColorBand *coba = static_cast<ColorBand *>(node->storage);
  float *array, layer;
  int size;

  /* Common / easy case optimization. */
  if (coba->tot == 1) {
    return GPU_link(mat, "set_rgba", GPU_uniform(&coba->data[0].r), &out[0].link);
  }
  if ((coba->tot == 2) && (coba->color_mode == COLBAND_BLEND_RGB)) {
    float mul_bias[2];
    switch (coba->ipotype) {
      case COLBAND_INTERP_LINEAR:
        mul_bias[0] = 1.0f / (coba->data[1].pos - coba->data[0].pos);
        mul_bias[1] = -mul_bias[0] * coba->data[0].pos;
        return GPU_stack_link(mat,
                              node,
                              "valtorgb_opti_linear",
                              in,
                              out,
                              GPU_uniform(mul_bias),
                              GPU_uniform(&coba->data[0].r),
                              GPU_uniform(&coba->data[1].r));
      case COLBAND_INTERP_CONSTANT:
        mul_bias[1] = max_ff(coba->data[0].pos, coba->data[1].pos);
        return GPU_stack_link(mat,
                              node,
                              "valtorgb_opti_constant",
                              in,
                              out,
                              GPU_uniform(&mul_bias[1]),
                              GPU_uniform(&coba->data[0].r),
                              GPU_uniform(&coba->data[1].r));
      case COLBAND_INTERP_EASE:
        mul_bias[0] = 1.0f / (coba->data[1].pos - coba->data[0].pos);
        mul_bias[1] = -mul_bias[0] * coba->data[0].pos;
        return GPU_stack_link(mat,
                              node,
                              "valtorgb_opti_ease",
                              in,
                              out,
                              GPU_uniform(mul_bias),
                              GPU_uniform(&coba->data[0].r),
                              GPU_uniform(&coba->data[1].r));
      default:
        break;
    }
  }

  BKE_colorband_evaluate_table_rgba(coba, &array, &size);
  GPUNodeLink *tex = GPU_color_band(mat, size, array, &layer);

  if (coba->ipotype == COLBAND_INTERP_CONSTANT) {
    return GPU_stack_link(mat, node, "valtorgb_nearest", in, out, tex, GPU_constant(&layer));
  }

  return GPU_stack_link(mat, node, "valtorgb", in, out, tex, GPU_constant(&layer));
}

class ColorBandFunction : public mf::MultiFunction {
 private:
  /** Take ownership of the tree because it contains the color ramp. */
  std::shared_ptr<const bNodeTree> tree_;
  const ColorBand &color_band_;

 public:
  ColorBandFunction(const ColorBand &color_band, std::shared_ptr<const bNodeTree> tree)
      : tree_(tree), color_band_(color_band)
  {
    static const mf::Signature signature = []() {
      mf::Signature signature;
      mf::SignatureBuilder builder{"Color Band", signature};
      builder.single_input<float>("Value");
      builder.single_output<ColorGeometry4f>("Color");
      builder.single_output<float>("Alpha");
      return signature;
    }();
    this->set_signature(&signature);
  }

  void call(const IndexMask &mask, mf::Params params, mf::Context /*context*/) const override
  {
    const VArray<float> &values = params.readonly_single_input<float>(0, "Value");
    MutableSpan<ColorGeometry4f> colors = params.uninitialized_single_output<ColorGeometry4f>(
        1, "Color");
    MutableSpan<float> alphas = params.uninitialized_single_output<float>(2, "Alpha");

    mask.foreach_index([&](const int64_t i) {
      ColorGeometry4f color;
      BKE_colorband_evaluate(&color_band_, values[i], color);
      colors[i] = color;
      alphas[i] = color.a;
    });
  }

  void hash_unique(UniqueHashBytes &hash) const override
  {
    static constexpr int8_t id = 0;
    hash.add(&id);
    hash.add(&color_band_);
  }
};

static void sh_node_valtorgb_build_multi_function(nodes::NodeMultiFunctionBuilder &builder)
{
  const bNode &bnode = builder.node();
  const ColorBand *color_band = static_cast<const ColorBand *>(bnode.storage);
  builder.construct_and_set_matching_fn<ColorBandFunction>(*color_band, builder.shared_tree());
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* カラーランプ(ColorBand)を MaterialX のノード列で組む。1D の LUT は使わない。
   *
   * 停止点が位置の順に並んでいることを使って、左から順に
   *   acc = mix(acc, 停止点 i の色, clamp((t - p[i-1]) / (p[i] - p[i-1])))
   * と畳んでいく。係数が両端で飽和するので、
   *   ・最初の停止点より左 → 係数 0 が続いて acc = 停止点 0 の色
   *   ・最後の停止点より右 → 係数 1 が続いて acc = 最後の色
   * となり、`BKE_colorband_evaluate()` の端の扱いとそのまま一致する。
   *
   * ★対応するのは RGB の LINEAR / EASE / CONSTANT まで。
   *   B_SPLINE と CARDINAL は 4 点の重みが要るので LINEAR で近似する。
   *   HSV / HSL の色モードも RGB の直線補間で近似する
   *   (Blender 側も RGB 以外では補間形を LINEAR に落としている)。 */
  const ColorBand *coba = static_cast<const ColorBand *>(node_->storage);
  if (coba == nullptr || coba->tot <= 0) {
    return empty();
  }

  auto stop_color = [&](int i) {
    const CBData &d = coba->data[i];
    return val(MaterialX::Color4(d.r, d.g, d.b, d.a));
  };

  const int ipotype = (coba->color_mode == COLBAND_BLEND_RGB) ? int(coba->ipotype) :
                                                                int(COLBAND_INTERP_LINEAR);
  if (!ELEM(ipotype, COLBAND_INTERP_LINEAR, COLBAND_INTERP_EASE, COLBAND_INTERP_CONSTANT)) {
    CLOG_WARN(materialx::LOG_IO_MATERIALX,
              "ColorRamp: %s は書き出せないので直線で近似します",
              ipotype == COLBAND_INTERP_CARDINAL ? "Cardinal" : "B-Spline");
  }

  NodeItem fac = get_input_value("Fac", NodeItem::Type::Float);
  NodeItem res = stop_color(0);
  for (int i = 1; i < coba->tot; i++) {
    const float p0 = coba->data[i - 1].pos;
    const float p1 = coba->data[i].pos;
    NodeItem w = empty();
    if (ipotype == COLBAND_INTERP_CONSTANT || !(p1 > p0)) {
      /* 段状。位置が重なっている時もこちら(0 除算を避ける)。 */
      w = fac.if_else(NodeItem::CompareOp::GreaterEq, val(p1), val(1.0f), val(0.0f));
    }
    else {
      w = ((fac - val(p0)) / val(p1 - p0)).clamp();
      if (ipotype == COLBAND_INTERP_EASE) {
        w = w * w * (val(3.0f) - val(2.0f) * w);
      }
    }
    res = w.mix(res, stop_color(i));
  }

  if (STREQ(socket_out_->identifier, "Alpha")) {
    res = res[3];
  }
  return res;
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_color_ramp_cc

void register_node_type_sh_valtorgb()
{
  namespace file_ns = nodes::node_shader_color_ramp_cc;

  static bke::bNodeType ntype;

  common_node_type_base(&ntype, "ShaderNodeValToRGB"_ustr, SH_NODE_VALTORGB);
  ntype.ui_name = "Color Ramp";
  ntype.ui_description = "Map values to colors with the use of a gradient";
  ntype.enum_name_legacy = "VALTORGB";
  ntype.nclass = NODE_CLASS_CONVERTER;
  ntype.declare = file_ns::sh_node_valtorgb_declare;
  ntype.initfunc = file_ns::node_shader_init_valtorgb;
  ntype.default_width = bke::NodeWidth::_240;
  bke::node_type_storage(
      ntype, "ColorBand", node_free_standard_storage, node_copy_standard_storage);
  ntype.gpu_fn = file_ns::gpu_shader_valtorgb;
  ntype.build_multi_function = file_ns::sh_node_valtorgb_build_multi_function;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
