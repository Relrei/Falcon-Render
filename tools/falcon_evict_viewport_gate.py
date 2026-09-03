# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Gate for FALCON_CYCLES_EVICT_VIEWPORT.

Runs inside a *GUI* Blender (there is no viewport in ``-b``)::

    blender <scene.blend> --python tools/falcon_evict_viewport_gate.py -- \
        --out DIR --label ON --warm 20

What it does, in order:

1. Puts the first 3D viewport into Rendered shading with overlays and gizmos
   off, so a screenshot of that area is the Cycles viewport image and nothing
   else.
2. Waits ``--warm`` seconds for the viewport render to settle, then captures it
   (``viewport_before.png``).
3. Runs a final render (F12 equivalent) and writes it to ``render.png``.
4. Probes the viewport every ``--probe`` seconds afterwards and records how far
   each probe is from the picture in step 2, so the time it takes for the
   viewport to come back is measured rather than assumed.
5. Waits ``--warm`` seconds in total and captures the viewport a second time
   (``viewport_after.png``).
6. Writes ``result.json`` and quits.

The comparison and the PASS/FAIL verdict are done by the driver
(``falcon_evict_viewport_gate.sh``), which also compares the two labels against
each other. This script only produces evidence; it does not judge.

A crash, a viewport that never comes back, or a viewport that comes back black
all show up as a missing file or as a mismatch between ``viewport_before`` and
``viewport_after``.
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

    parser = argparse.ArgumentParser(prog="falcon_evict_viewport_gate")
    parser.add_argument("--out", required=True, help="Directory for the evidence files")
    parser.add_argument("--label", required=True, help="Name of this run, e.g. ON or OFF")
    parser.add_argument("--warm", type=float, default=20.0,
                        help="Seconds to let the viewport settle before a capture")
    parser.add_argument("--probe", type=float, default=0.5,
                        help="Seconds between the probes that measure how long the viewport "
                             "takes to come back after the final render")
    parser.add_argument("--preview-samples", type=int, default=16,
                        help="Fixed viewport sample count, so both captures converge to the "
                             "same image")
    parser.add_argument("--samples", type=int, default=0,
                        help="Final render samples (0 = leave the scene as it is)")
    parser.add_argument("--res-percent", type=int, default=0,
                        help="Final render resolution percentage (0 = leave as is)")
    parser.add_argument("--render-mode", choices=("exec", "invoke"), default="exec",
                        help="exec = blocking render (deterministic, the default). "
                             "invoke = the render job F12 starts, where the viewport keeps "
                             "being drawn while the render runs")
    parser.add_argument("--maximize-viewport", action="store_true",
                        help="Make the 3D viewport the only visible area first, for files "
                             "saved with another editor in the way")
    parser.add_argument("--job-timeout", type=float, default=900.0,
                        help="Seconds to wait for the render job in --render-mode invoke")
    return parser.parse_args(argv)


ARGS = parse_args()
RESULT = {
    "label": ARGS.label,
    "evict_env": os.environ.get("FALCON_CYCLES_EVICT_VIEWPORT", ""),
    "blend": bpy.data.filepath,
    "steps": [],
}
STATE = {"view3d": None, "window": None, "before_pixels": None}

# Mean absolute difference (0..255) at which the viewport counts as "back".
BACK_TOLERANCE = 3.0


def out_path(name):
    return os.path.join(ARGS.out, "%s_%s" % (ARGS.label, name))


def log(msg):
    print("[gate:%s] %s" % (ARGS.label, msg), flush=True)


def finish(status, error=None):
    RESULT["status"] = status
    if error:
        RESULT["error"] = error
    with open(out_path("result.json"), "w") as fh:
        json.dump(RESULT, fh, indent=2)
    log("status=%s" % status)
    bpy.ops.wm.quit_blender()
    return None


def read_pixels(path):
    """PNG on disk -> flat numpy array in 0..255, using Blender's own loader."""
    import numpy as np

    image = bpy.data.images.load(path, check_existing=False)
    try:
        buf = np.empty(len(image.pixels), dtype=np.float32)
        image.pixels.foreach_get(buf)
    finally:
        bpy.data.images.remove(image)
    return buf * 255.0


def mean_abs_diff(a, b):
    import numpy as np

    if a is None or b is None or a.shape != b.shape:
        return float("inf")
    return float(np.abs(a - b).mean())


def find_view3d():
    for window in bpy.context.window_manager.windows:
        for area in window.screen.areas:
            if area.type == 'VIEW_3D':
                return window, area
    return None, None


def grab(path):
    """Save the 3D viewport area to `path`.

    NOTE: this reads what the window last drew. Forcing a draw first with
    `wm.redraw_timer` was tried and gives a flat image, so the probe timing
    below is only accurate to one probe interval: a probe can still be showing
    the frame from just before it fired.
    """
    window, area = STATE["window"], STATE["view3d"]
    with bpy.context.temp_override(window=window, area=area):
        bpy.ops.screen.screenshot_area(filepath=path)


def capture(name):
    path = out_path(name + ".png")
    grab(path)
    ok = os.path.exists(path)
    log("captured %s -> %s" % (name, "ok" if ok else "MISSING"))
    RESULT["steps"].append({"step": name, "path": path, "exists": ok, "t": time.time()})
    return ok


def step_setup():
    window, area = find_view3d()
    if area is None:
        return finish("fail", "no 3D viewport in this file")
    STATE["window"], STATE["view3d"] = window, area
    log("areas in this screen: %s"
        % ", ".join("%s %dx%d" % (a.type, a.width, a.height) for a in window.screen.areas))

    if ARGS.maximize_viewport and len(window.screen.areas) > 1:
        with bpy.context.temp_override(window=window, area=area):
            bpy.ops.screen.screen_full_area(use_hide_panels=False)
        window, area = find_view3d()
        STATE["window"], STATE["view3d"] = window, area
        log("3D viewport maximized to %dx%d" % (area.width, area.height))

    space = area.spaces.active
    space.shading.type = 'RENDERED'
    space.overlay.show_overlays = False
    space.show_gizmo = False
    space.show_region_header = False
    space.show_region_toolbar = False
    space.show_region_ui = False

    scene = bpy.context.scene
    scene.cycles.preview_samples = ARGS.preview_samples
    scene.cycles.preview_pause = False
    if ARGS.samples:
        scene.cycles.samples = ARGS.samples
    if ARGS.res_percent:
        scene.render.resolution_percentage = ARGS.res_percent

    RESULT["engine"] = scene.render.engine
    RESULT["device"] = scene.cycles.device
    RESULT["samples"] = scene.cycles.samples
    RESULT["preview_samples"] = scene.cycles.preview_samples
    RESULT["resolution"] = [
        int(scene.render.resolution_x * scene.render.resolution_percentage / 100),
        int(scene.render.resolution_y * scene.render.resolution_percentage / 100),
    ]
    log("viewport set to Rendered, %dx%d / %d spp / preview %d spp"
        % (RESULT["resolution"][0], RESULT["resolution"][1],
           RESULT["samples"], RESULT["preview_samples"]))

    bpy.app.timers.register(step_capture_before, first_interval=ARGS.warm)
    return None


def step_capture_before():
    if not capture("viewport_before"):
        return finish("fail", "could not capture the viewport before the render")
    try:
        STATE["before_pixels"] = read_pixels(out_path("viewport_before.png"))
    except Exception as ex:  # noqa: BLE001
        return finish("fail", "could not read back the viewport capture: %r" % (ex,))
    bpy.app.timers.register(step_render, first_interval=0.5)
    return None


def step_render():
    scene = bpy.context.scene
    scene.render.filepath = out_path("render")
    scene.render.image_settings.file_format = 'PNG'
    scene.render.image_settings.color_mode = 'RGB'
    scene.render.image_settings.color_depth = '8'

    log("final render starting (%s)" % ARGS.render_mode)
    t0 = time.time()
    RESULT["render_start_time"] = t0
    RESULT["render_mode"] = ARGS.render_mode
    try:
        if ARGS.render_mode == "invoke":
            # The path F12 takes: a render job on its own thread, with the
            # viewport still being drawn next to it.
            window, area = STATE["window"], STATE["view3d"]
            with bpy.context.temp_override(window=window, area=area):
                bpy.ops.render.render('INVOKE_DEFAULT', write_still=True)
            STATE["render_t0"] = t0
            bpy.app.timers.register(step_wait_for_job, first_interval=0.5)
            return None
        bpy.ops.render.render(write_still=True)
    except Exception as ex:  # noqa: BLE001 - the gate has to report, not raise
        RESULT["render_seconds"] = time.time() - t0
        return finish("fail", "final render raised: %r" % (ex,))
    return step_render_finished(t0)


def step_wait_for_job():
    """Wait for the render job started by the invoke path to end."""
    if bpy.app.is_job_running('RENDER'):
        if time.time() - STATE["render_t0"] > ARGS.job_timeout:
            return finish("fail", "the render job did not end within %.0f s" % ARGS.job_timeout)
        return 0.5
    return step_render_finished(STATE["render_t0"])


def step_render_finished(t0):
    RESULT["render_seconds"] = time.time() - t0
    RESULT["render_path"] = out_path("render.png")
    RESULT["render_written"] = os.path.exists(RESULT["render_path"])
    log("final render done in %.2f s (written=%s)"
        % (RESULT["render_seconds"], RESULT["render_written"]))

    RESULT["render_end_time"] = time.time()
    RESULT["probe_curve"] = []
    bpy.app.timers.register(step_probe, first_interval=ARGS.probe)
    return None


def step_probe():
    """Watch the viewport come back, one capture at a time.

    The distance is measured against the picture taken before the final render,
    so "back" means "showing the same thing again", not just "not crashed".
    """
    elapsed = time.time() - RESULT["render_end_time"]
    path = out_path("probe.png")
    try:
        grab(path)
        diff = mean_abs_diff(STATE.get("before_pixels"), read_pixels(path))
    except Exception as ex:  # noqa: BLE001
        diff = float("inf")
        log("probe at %.2f s failed: %r" % (elapsed, ex))

    RESULT["probe_curve"].append([round(elapsed, 3), None if diff == float("inf") else diff])
    log("probe %.2f s -> mean abs diff %.3f" % (elapsed, diff))

    if "viewport_back_seconds" not in RESULT and diff <= BACK_TOLERANCE:
        RESULT["viewport_back_seconds"] = elapsed
        log("viewport is back after %.2f s" % elapsed)

    if elapsed >= ARGS.warm:
        bpy.app.timers.register(step_capture_after, first_interval=0.1)
        return None
    return ARGS.probe


def step_capture_after():
    RESULT["viewport_settle_seconds"] = time.time() - RESULT["render_end_time"]
    if not capture("viewport_after"):
        return finish("fail", "the viewport never came back after the render")
    return finish("ok")


os.makedirs(ARGS.out, exist_ok=True)
bpy.app.timers.register(step_setup, first_interval=2.0)
