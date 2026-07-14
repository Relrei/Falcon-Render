# SPDX-FileCopyrightText: 2026 Falcon CyclesF
#
# SPDX-License-Identifier: Apache-2.0

"""Headless runner for the Falcon "bake and render range" operator.

The GUI operator used to bake + render the frame range inside the GUI
process; a long range pushes one display update per frame through the
Vulkan viewport backend and that raced/crashed (2026-07-07, see
FALCON_PHOTON.md).  The operator now saves a temp copy of the file and
launches THIS script in a detached background Blender instead — no
display driver in the loop, which is also the documented-safe way to run
final range bakes.

Usage (what the operator spawns):
    blender -b <copy.blend> --python falcon_range.py -- --mode once
    blender -b <copy.blend> --python falcon_range.py -- --mode perframe

Modes:
    once     bake the caustics once at the file's current frame, then
             render frame_start..frame_end reusing that one world-space
             cache (static light + static casters; camera may move).
    perframe rebake at every frame before rendering it (moving casters/
             lights).  Costs one bake per frame.
"""

import os
import sys
import time

import bpy


def _log(msg):
    print("[falcon_range] %s" % msg, flush=True)


def _fail(msg):
    _log("ERROR: %s" % msg)
    sys.exit(1)


def _bake_or_die(frame):
    """Run the photon bake op and verify the map really landed on disk.

    A FINISHED bake with no file (no valid light, empty deposit) would
    make the add-mode render ask the kernel for a nonexistent cache —
    that was one half of the 2026-07-07 crash.  Same guard as the GUI op.
    """
    try:
        res = bpy.ops.cycles.falcon_photon_bake()
    except RuntimeError as e:
        _fail("frame %d: bake failed: %s" % (frame, e))
    if 'FINISHED' not in res:
        _fail("frame %d: bake did not finish" % frame)
    if os.environ.get("FALCON_PHOTON_MODE") != "add":
        _fail("frame %d: bake did not arm add mode (no map produced)" % frame)
    pts = os.environ.get("FALCON_PHOTON_POINTS")
    grid = os.environ.get("FALCON_SHARC_CACHE")
    map_path = pts if pts else grid
    if not map_path or not os.path.exists(map_path):
        _fail("frame %d: baked map missing on disk (%s)"
              % (frame, map_path or "no path set"))


def main():
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    mode = "once"
    if "--mode" in argv:
        mode = argv[argv.index("--mode") + 1]
    if mode not in ("once", "perframe"):
        _fail("unknown --mode %r" % mode)

    scene = bpy.context.scene
    r = scene.render
    if r.engine != 'CYCLES':
        _fail("render engine is %s, not CYCLES" % r.engine)
    f0, f1 = scene.frame_start, scene.frame_end
    step = scene.frame_step
    if f1 < f0:
        _fail("bad frame range (end < start)")
    if not r.filepath:
        _fail("no render output path set in the file")

    t0 = time.time()
    _log("mode=%s frames=%d..%d step=%d out=%s"
         % (mode, f0, f1, step, r.filepath))

    if mode == "once":
        _log("baking once at frame %d" % scene.frame_current)
        _bake_or_die(scene.frame_current)
        _log("bake ok (%.0fs), rendering range" % (time.time() - t0))
        bpy.ops.render.render(animation=True)
    else:
        base = r.filepath
        frames = list(range(f0, f1 + 1, max(1, step)))
        for i, f in enumerate(frames):
            tf = time.time()
            scene.frame_set(f)
            _bake_or_die(f)
            # frame_path() resolves ###/extension against the BASE template;
            # restore it first so a previous frame's concrete path never
            # becomes the template.
            r.filepath = base
            path = r.frame_path(frame=f)
            r.filepath = path
            bpy.ops.render.render(write_still=True)
            _log("frame %d (%d/%d) bake+render %.0fs -> %s"
                 % (f, i + 1, len(frames), time.time() - tf, path))
        r.filepath = base

    _log("done in %.0fs" % (time.time() - t0))


main()
