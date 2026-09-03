/* SPDX-FileCopyrightText: 2005 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_shader_util.hh"

namespace blender {

namespace nodes::node_shader_fresnel_cc {

static void node_declare(NodeDeclarationBuilder &b)
{
  b.add_input<decl::Float>("IOR"_ustr).default_value(1.5f).min(0.0f).max(1000.0f);
  b.add_input<decl::Vector>("Normal"_ustr).hide_value();
  b.add_output<decl::Float>("Factor"_ustr, "Fac"_ustr);
}

static int node_shader_gpu_fresnel(GPUMaterial *mat,
                                   bNode *node,
                                   bNodeExecData * /*execdata*/,
                                   GPUNodeStack *in,
                                   GPUNodeStack *out)
{
  if (!in[1].link) {
    GPU_link(mat, "world_normals_get", &in[1].link);
  }

  return GPU_stack_link(mat, node, "node_fresnel", in, out);
}

NODE_SHADER_MATERIALX_BEGIN
#ifdef WITH_MATERIALX
{
  /* ★2026-08-30: これまで IOR をそのまま素通ししていた（= 係数が 1.5 などの定数になっていた）。
   *   `gpu_shader_material_fresnel.glsl` の `fresnel_dielectric_cos()` と同じ式をノードで組む。
   *   視線は MaterialX の `viewdirection`（視点から面へ向かう）。
   *   Blender 側は面から視点へ向かう向きだが、式が `abs(cos)` を取るので符号は効かない。
   *   ⚠裏向きの面で eta を逆数にする分岐は MaterialX に口が無いので表向き固定。 */
  NodeItem ior = get_input_value("IOR", NodeItem::Type::Float);
  NodeItem normal = get_input_link("Normal", NodeItem::Type::Vector3);
  if (!normal) {
    normal = create_node(
        "normal", NodeItem::Type::Vector3, {{"space", val(std::string("world"))}});
  }
  NodeItem view = create_node(
      "viewdirection", NodeItem::Type::Vector3, {{"space", val(std::string("world"))}});

  NodeItem c = normal.normalize().dotproduct(view).abs();
  NodeItem eta = ior.max(val(0.00001f));
  NodeItem g2 = eta * eta - val(1.0f) + c * c;
  NodeItem g = g2.max(val(0.0f)).sqrt();
  NodeItem a = (g - c) / (g + c);
  NodeItem b = (c * (g + c) - val(1.0f)) / (c * (g - c) + val(1.0f));
  NodeItem fac = (val(0.5f) * a * a * (val(1.0f) + b * b)).clamp();
  /* g2 <= 0 は全反射。 */
  return g2.if_else(NodeItem::CompareOp::Greater, val(0.0f), fac, val(1.0f));
}
#endif
NODE_SHADER_MATERIALX_END

}  // namespace nodes::node_shader_fresnel_cc

/* node type definition */
void register_node_type_sh_fresnel()
{
  namespace file_ns = nodes::node_shader_fresnel_cc;

  static bke::bNodeType ntype;

  sh_node_type_base(&ntype, "ShaderNodeFresnel"_ustr, SH_NODE_FRESNEL);
  ntype.ui_name = "Fresnel";
  ntype.ui_description =
      "Produce a blending factor depending on the angle between the surface normal and the view "
      "direction using Fresnel equations.\nTypically used for mixing reflections at grazing "
      "angles";
  ntype.enum_name_legacy = "FRESNEL";
  ntype.nclass = NODE_CLASS_INPUT;
  ntype.declare = file_ns::node_declare;
  ntype.gpu_fn = file_ns::node_shader_gpu_fresnel;
  ntype.materialx_fn = file_ns::node_shader_materialx;

  bke::node_register_type(ntype);
}

}  // namespace blender
