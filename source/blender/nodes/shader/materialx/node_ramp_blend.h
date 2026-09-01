/* SPDX-FileCopyrightText: 2011-2022 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#pragma once

#include "node_item.h"

namespace blender::nodes::materialx {

/**
 * Build the MaterialX node network equivalent of `ramp_blend()`
 * (`BKE_material`, `source/blender/blenkernel/intern/material.cc`).
 *
 * `blend_type` is one of the `MA_RAMP_*` values. `color1` is the base ("A"),
 * `color2` the layer ("B") and `fac` the blending factor. Both colors are
 * expected to be `Color3`, `fac` a `Float`.
 */
NodeItem ramp_blend(int blend_type,
                    const NodeItem &color1,
                    const NodeItem &color2,
                    const NodeItem &fac);

}  // namespace blender::nodes::materialx
