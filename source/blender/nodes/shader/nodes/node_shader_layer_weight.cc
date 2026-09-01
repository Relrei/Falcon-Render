/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_layer_weight_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("Blend"_ustr).default_value(0.5f).min(0.0f).max(1.0f);
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
  b.add_output<decl::Float>("Fresnel"_ustr);
  b.add_output<decl::Float>("Facing"_ustr);
}

static int node_shader_gpu_layer_weight(GPUMaterial *mat,
                                        bNode *node,
                                        bNodeExecData * /*execdata*/,
                                        GPUNodeStack *in,
                                        GPUNodeStack *out)
{
  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", &in[1].link);
  }

  return GPU_stack_link(mat, node, "node_layer_weight", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* ★2026-08-30: これまで Blend の繋ぎ先をそのまま返していた（値入力の時は空が返っていた）。
   *   `gpu_shader_material_layer_weight.glsl` と同じ式をノードで組み、
   *   Fresnel と Facing の2出力を分ける。
   *   ⚠裏向きの面で eta を逆数にする分岐は MaterialX に口が無いので表向き固定。 */
  NodeItem blend = get_input_value("Blend", NodeItem::Type::Float);
  NodeItem normal = get_input_link("Normal", NodeItem::Type::Vector3);
  if (!normal) {
    normal = create_node(
        "normal", NodeItem::Type::Vector3, {{"space", val(std::string("world"))}});
  }
  NodeItem view = create_node(
      "viewdirection", NodeItem::Type::Vector3, {{"space", val(std::string("world"))}});
  NodeItem c = normal.normalize().dotproduct(view).abs();

  if (STREQ(socket_out_->identifier, "Fresnel")) {
    /* 表向きの面では 1/eta を使う（GLSL 側と同じ）。 */
    NodeItem eta = val(1.0f) / (val(1.0f) - blend).max(val(0.00001f));
    NodeItem g2 = eta * eta - val(1.0f) + c * c;
    NodeItem g = g2.max(val(0.0f)).sqrt();
    NodeItem a = (g - c) / (g + c);
    NodeItem b = (c * (g + c) - val(1.0f)) / (c * (g - c) + val(1.0f));
    NodeItem fac = (val(0.5f) * a * a * (val(1.0f) + b * b)).clamp();
    return g2.if_else(NodeItem::CompareOp::Greater, val(0.0f), fac, val(1.0f));
  }

  /* Facing。blend = 0.5 の時は指数が 1 になるので、GLSL 側の分岐は要らない。 */
  NodeItem b = blend.clamp(0.0f, 0.99999f);
  NodeItem exponent = b.if_else(
      NodeItem::CompareOp::Less, val(0.5f), val(2.0f) * b, val(0.5f) / (val(1.0f) - b));
  return val(1.0f) - (c ^ exponent);
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_layer_weight_cc

/* node type definition */
void register_node_type_sh_layer_weight()
{
  namespace file_ns = nodes::node_shader_layer_weight_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeLayerWeight"_ustr, SH_NODE_LAYER_WEIGHT);
  ntype.ui_name = "Layer Weight";
  ntype.ui_description =
      "Produce a blending factor depending on the angle between the surface normal and the view "
      "direction.\nTypically used for layering shaders with the Mix Shader node";
  ntype.enum_name_legacy = "LAYER_WEIGHT";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_layer_weight;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
