# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""The "no Rendered viewport" side of the FALCON_CYCLES_EVICT_VIEWPORT gate.

Runs inside a *GUI* Blender, with every 3D viewport put into Solid shading::

    blender <scene.blend> --python tools/falcon_evict_viewport_noviewport.py -- \
        --out DIR/PREFIX --renders 2

This is the case the eviction must keep its hands off entirely: nothing is in
Rendered shading, so nothing is taken away from the device and nothing has to be
given back. Two things have to hold:

* the picture is the same as before the switch existed (use ``--reference``,
  which renders the deterministic scene from
  ``falcon_evict_viewport_bitcheck.py`` so the pixels can be compared exactly),
* and Persistent Data survives between renders. That is why this renders twice:
  with Persistent Data on, the second render skips synchronisation, and Blender
  writes ``cycles.<layer>.synchronization_time`` into the PNG metadata. If the
  eviction had freed the cache, the second render would pay for the sync again.

It also has to be a GUI run: ``blender -b`` renders through ``RE_RenderFrame``
directly and never reaches the render operator where the eviction lives, so a
background run would prove nothing.
"""

from __future__ import annotations

import json
import os
import sys
import time

import bpy


def parse_args():
    argv = sys.argv
    argv = argv[argv.index("--") + 1:] if "--" in argv else []

    import argparse

    parser = argparse.ArgumentParser(prog="falcon_evict_viewport_noviewport")
    parser.add_argument("--out", required=True, help="Path prefix for the evidence files")
    parser.add_argument("--renders", type=int, default=2,
                        help="How many times to render the same frame")
    parser.add_argument("--reference", action="store_true",
                        help="Render the deterministic reference scene instead of the file's "
                             "own settings, so the pixels can be compared exactly")
    parser.add_argument("--res-percent", type=int, default=0,
                        help="Resolution percentage (0 = leave as is), ignored with --reference")
    parser.add_argument("--samples", type=int, default=0,
                        help="Sample count (0 = leave as is), ignored with --reference")
    return parser.parse_args(argv)


ARGS = parse_args()
RESULT = {
    "evict_env": os.environ.get("FALCON_CYCLES_EVICT_VIEWPORT", "<unset>"),
    "blend": bpy.data.filepath,
    "renders": [],
}


def log(msg):
    print("[noviewport] %s" % msg, flush=True)


def finish(status, error=None):
    RESULT["status"] = status
    if error:
        RESULT["error"] = error
    with open(ARGS.out + "_result.json", "w") as fh:
        json.dump(RESULT, fh, indent=2)
    log("status=%s" % status)
    bpy.ops.wm.quit_blender()
    return None


def step_run():
    scene = bpy.context.scene

    shadings = []
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                area.spaces.active.shading.type = 'SOLID'
                shadings.append('SOLID')
    if not shadings:
        return finish("fail", "this file has no 3D viewport, so the check would prove nothing")
    RESULT["viewports"] = len(shadings)
    log("%d 3D viewport(s) set to Solid shading" % len(shadings))

    if ARGS.reference:
        # Same settings as falcon_evict_viewport_bitcheck.py: everything that
        # makes a render vary from run to run is off, so the pixels are exact.
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
    else:
        if ARGS.res_percent:
            scene.render.resolution_percentage = ARGS.res_percent
        if ARGS.samples:
            scene.cycles.samples = ARGS.samples

    # The point of the second render: with this on, Cycles keeps the scene on
    # the device and the second render does not synchronise again.
    scene.render.use_persistent_data = True
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGB'
    scene.render.image_settings.color_depth = '8'

    for index in range(1, ARGS.renders + 1):
        scene.render.filepath = "%s_%d" % (ARGS.out, index)
        t0 = time.time()
        try:
            bpy.ops.render.render(write_still=True)
        except Exception as ex:  # noqa: BLE001 - the gate has to report, not raise
            return finish("fail", "render %d raised: %r" % (index, ex))
        path = "%s_%d.png" % (ARGS.out, index)
        RESULT["renders"].append({
            "index": index,
            "seconds": time.time() - t0,
            "path": path,
            "written": os.path.exists(path),
        })
        log("render %d done in %.2f s" % (index, RESULT["renders"][-1]["seconds"]))

    return finish("ok")


os.makedirs(os.path.dirname(ARGS.out) or ".", exist_ok=True)
bpy.app.timers.register(step_run, first_interval=2.0)
