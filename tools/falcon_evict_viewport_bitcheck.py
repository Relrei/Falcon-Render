# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Deterministic reference render for the FALCON_CYCLES_EVICT_VIEWPORT gate.

    blender -b --factory-startup --python tools/falcon_evict_viewport_bitcheck.py -- OUT

Renders the factory startup cube on the CPU with denoising, adaptive sampling
and stamping off. Everything that makes a render vary from run to run is turned
off here on purpose, so the resulting PNG can be compared byte for byte:

* the same binary must reproduce itself,
* the binary with the switch on must match the binary with it off (in
  background there is no viewport, so the switch has nothing to do),
* and the binary must match the one built before the switch existed.

NOTE: a heavy production scene will *not* reproduce itself byte for byte in this
build even with the same binary (measured 2026-08-27 on classroom.blend, CPU and
GPU alike), which is why the byte comparison uses this scene and not that one.
"""

import sys

import bpy

out = sys.argv[sys.argv.index("--") + 1]

scene = bpy.context.scene
scene.render.engine = 'CYCLES'
scene.cycles.device = 'CPU'
scene.render.resolution_x = 160
scene.render.resolution_y = 120
scene.render.resolution_percentage = 100
scene.cycles.samples = 16
scene.cycles.use_denoising = False
scene.cycles.use_adaptive_sampling = False
scene.render.threads_mode = 'FIXED'
scene.render.threads = 4
scene.render.use_stamp = False
scene.render.filepath = out
scene.render.image_settings.file_format = 'PNG'

bpy.ops.render.render(write_still=True)
