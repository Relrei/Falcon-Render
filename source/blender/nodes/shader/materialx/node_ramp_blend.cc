/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "node_ramp_blend.h"

#include "DNA_material_types.h"

namespace blender::nodes::materialx {

using Type = NodeItem::Type;
using CompareOp = NodeItem::CompareOp;

/* Guard used where Blender special-cases a division by zero. Picking a tiny
 * positive number keeps the saturating branches (dodge/burn) on the correct
 * side without needing a per-channel conditional. */
static const float DIVIDE_GUARD = 1e-7f;

static NodeItem combine_rgb(const NodeItem &ctx,
                            const NodeItem &r,
                            const NodeItem &g,
                            const NodeItem &b)
{
  return ctx.create_node("combine3", Type::Color3, {{"in1", r}, {"in2", g}, {"in3", b}});
}

/* Blender leaves a channel untouched when the divisor is exactly zero. */
static NodeItem blend_divide(const NodeItem &a, const NodeItem &b)
{
  NodeItem out[3] = {a.empty(), a.empty(), a.empty()};
  for (int i = 0; i < 3; i++) {
    NodeItem bi = b[i];
    NodeItem ai = a[i];
    out[i] = bi.if_else(CompareOp::NotEq, bi.val(0.0f), ai / bi, ai);
  }
  return combine_rgb(a, out[0], out[1], out[2]);
}

static NodeItem rgb_to_hsv(const NodeItem &color)
{
  return color.create_node("rgbtohsv", Type::Color3, {{"in", color}});
}

static NodeItem hsv_to_rgb(const NodeItem &ctx,
                           const NodeItem &h,
                           const NodeItem &s,
                           const NodeItem &v)
{
  NodeItem hsv = combine_rgb(ctx, h, s, v);
  return ctx.create_node("hsvtorgb", Type::Color3, {{"in", hsv}});
}

NodeItem ramp_blend(int blend_type,
                    const NodeItem &color1,
                    const NodeItem &color2,
                    const NodeItem &fac)
{
  const NodeItem &a = color1;
  const NodeItem &b = color2;
  const NodeItem &f = fac;
  NodeItem one = a.val(1.0f);
  NodeItem zero = a.val(0.0f);
  NodeItem two = a.val(2.0f);
  NodeItem fm = one - f;

  switch (blend_type) {
    case MA_RAMP_BLEND:
      return f.mix(a, b);

    case MA_RAMP_ADD:
      return a + b * f;

    case MA_RAMP_SUB:
      return a - b * f;

    case MA_RAMP_MULT:
      return f.mix(a, a * b);

    case MA_RAMP_SCREEN:
      return f.mix(a, one - (one - a) * (one - b));

    case MA_RAMP_OVERLAY: {
      /* `ND_overlay_*` branches on `bg`, exactly like `ramp_blend()` branches
       * on the base color, so `bg` takes A and `fg` takes B. `mix` is left at
       * 1 and the factor is folded in afterwards: `ramp_blend()` interpolates
       * towards A, while the MaterialX node would interpolate towards `bg`. */
      NodeItem full = a.create_node(
          "overlay", Type::Color3, {{"fg", b}, {"bg", a}, {"mix", one}});
      return f.mix(a, full);
    }

    case MA_RAMP_DIV:
      return f.mix(a, blend_divide(a, b));

    case MA_RAMP_DIFF:
      return f.mix(a, (a - b).abs());

    case MA_RAMP_EXCLUSION:
      return f.mix(a, a + b - two * a * b).max(zero);

    case MA_RAMP_DARK:
      return f.mix(a, a.min(b));

    case MA_RAMP_LIGHT:
      return f.mix(a, a.max(b));

    case MA_RAMP_DODGE:
      /* `a / (1 - fac * b)`, clamped to 1. A zero base stays zero and a
       * non-positive divisor saturates, matching `ramp_blend()`. */
      return (a / (one - b * f).max(a.val(DIVIDE_GUARD))).min(one);

    case MA_RAMP_BURN:
      return (one - (one - a) / (fm + f * b).max(a.val(DIVIDE_GUARD))).clamp(0.0f, 1.0f);

    case MA_RAMP_SOFT: {
      NodeItem screen = one - (one - b) * (one - a);
      return f.mix(a, (one - a) * b * a + a * screen);
    }

    case MA_RAMP_LINEAR:
      /* Both branches of `ramp_blend()` reduce to the same expression. */
      return a + f * (two * b - one);

    case MA_RAMP_HUE: {
      NodeItem hsv_b = rgb_to_hsv(b);
      NodeItem hsv_a = rgb_to_hsv(a);
      NodeItem tmp = hsv_to_rgb(a, hsv_b[0], hsv_a[1], hsv_a[2]);
      NodeItem sat_b = hsv_b[1];
      return sat_b.if_else(CompareOp::NotEq, zero, f.mix(a, tmp), a);
    }

    case MA_RAMP_SAT: {
      NodeItem hsv_a = rgb_to_hsv(a);
      NodeItem hsv_b = rgb_to_hsv(b);
      NodeItem sat_a = hsv_a[1];
      NodeItem tmp = hsv_to_rgb(a, hsv_a[0], fm * sat_a + f * hsv_b[1], hsv_a[2]);
      return sat_a.if_else(CompareOp::NotEq, zero, tmp, a);
    }

    case MA_RAMP_VAL: {
      NodeItem hsv_a = rgb_to_hsv(a);
      NodeItem hsv_b = rgb_to_hsv(b);
      return hsv_to_rgb(a, hsv_a[0], hsv_a[1], fm * hsv_a[2] + f * hsv_b[2]);
    }

    case MA_RAMP_COLOR: {
      NodeItem hsv_b = rgb_to_hsv(b);
      NodeItem hsv_a = rgb_to_hsv(a);
      NodeItem sat_b = hsv_b[1];
      NodeItem tmp = hsv_to_rgb(a, hsv_b[0], sat_b, hsv_a[2]);
      return sat_b.if_else(CompareOp::NotEq, zero, f.mix(a, tmp), a);
    }

    default:
      return f.mix(a, b);
  }
}

}  // namespace blender::nodes::materialx
