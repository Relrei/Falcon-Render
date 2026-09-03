# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Judge the evidence produced by ``falcon_evict_viewport_gate.py``.

Run with the system Python (needs numpy + Pillow)::

    python3 tools/falcon_evict_viewport_compare.py DIR --labels OFF ON

Checks, per label:

* the run finished (``result.json`` says ``ok``) and the final render was written
* the viewport picture after the final render matches the one before it
  (this is what catches "does not come back" and "comes back black")
* the viewport picture is not flat (a black or single-colour viewport fails)

Across labels:

* the final render matches within the run-to-run noise of this build, so
  turning the switch on does not change the picture
* the viewport picture before the render is the same too

NOTE: a byte comparison of the final render is NOT usable on a production scene
here. Measured 2026-08-27 on classroom.blend: the *same* binary rendering the
*same* frame twice differs by mean 0.045 (GPU, denoised), 0.016 (GPU, no
denoiser) and also on the CPU. The byte comparison lives in
``falcon_evict_viewport_bitcheck.py``, on a scene where the render does
reproduce itself.

Exit code 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import argparse
import hashlib
import json
import os
import sys

import numpy as np
from PIL import Image

# Mean absolute difference, 0..255. The viewport is a path traced, temporally
# denoised image, so two captures of the same converged state are close but not
# identical.
VIEWPORT_TOLERANCE = 3.0
# A viewport with less spread than this is a blank / single colour image.
VIEWPORT_MIN_STDDEV = 1.0
# Final render, mean absolute difference between labels. The measured
# run-to-run noise of this build on classroom.blend is 0.045, so 0.5 is an order
# of magnitude of headroom while still catching a real change in the picture.
RENDER_TOLERANCE = 0.5


def load(path):
    img = Image.open(path).convert("RGB")
    return np.asarray(img, dtype=np.float64)


def mad(a, b):
    if a.shape != b.shape:
        return float("inf")
    return float(np.abs(a - b).mean())


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def main():
    parser = argparse.ArgumentParser(prog="falcon_evict_viewport_compare")
    parser.add_argument("directory")
    parser.add_argument("--labels", nargs="+", required=True)
    args = parser.parse_args()

    failures = []
    notes = []
    per_label = {}

    for label in args.labels:
        prefix = os.path.join(args.directory, label + "_")
        result_path = prefix + "result.json"
        if not os.path.exists(result_path):
            failures.append("%s: no result.json (Blender crashed or never finished)" % label)
            continue
        with open(result_path) as fh:
            result = json.load(fh)
        per_label[label] = result

        if result.get("status") != "ok":
            failures.append("%s: run reported %s (%s)"
                            % (label, result.get("status"), result.get("error", "")))
            continue

        if not result.get("render_written"):
            failures.append("%s: the final render was not written" % label)

        before_path, after_path = prefix + "viewport_before.png", prefix + "viewport_after.png"
        for path in (before_path, after_path):
            if not os.path.exists(path):
                failures.append("%s: missing %s" % (label, os.path.basename(path)))
        if not (os.path.exists(before_path) and os.path.exists(after_path)):
            continue

        before, after = load(before_path), load(after_path)
        diff = mad(before, after)
        std_after = float(after.std())
        mean_after = float(after.mean())
        result["_measured"] = {
            "viewport_before_after_mad": diff,
            "viewport_after_mean": mean_after,
            "viewport_after_stddev": std_after,
            "render_sha256": sha256(result["render_path"]) if result.get("render_written") else None,
        }

        back = result.get("viewport_back_seconds")
        notes.append("%-4s viewport before/after mean abs diff = %6.3f  "
                     "(after: mean %.1f, stddev %.1f)  render %.2f s  "
                     "viewport back in %s"
                     % (label, diff, mean_after, std_after,
                        result.get("render_seconds", -1),
                        ("%.2f s" % back) if back is not None else "never (within the probe window)"))
        curve = result.get("probe_curve") or []
        if curve:
            notes.append("     probes after the render: "
                         + "  ".join("%.1fs:%s" % (t, "n/a" if d is None else "%.1f" % d)
                                     for t, d in curve[:14]))

        if result.get("viewport_back_seconds") is None:
            failures.append("%s: the viewport never matched its pre-render picture again "
                            "within the probe window" % label)
        if std_after < VIEWPORT_MIN_STDDEV:
            failures.append("%s: the viewport came back flat (stddev %.3f < %.3f) -- black or "
                            "not rebuilt" % (label, std_after, VIEWPORT_MIN_STDDEV))
        if diff > VIEWPORT_TOLERANCE:
            failures.append("%s: the viewport after the final render does not match the one "
                            "before it (mean abs diff %.3f > %.3f)"
                            % (label, diff, VIEWPORT_TOLERANCE))

    labels_ok = [lab for lab in args.labels if lab in per_label
                 and per_label[lab].get("status") == "ok"]
    if len(labels_ok) >= 2:
        first = labels_ok[0]
        for other in labels_ok[1:]:
            a, b = per_label[first]["_measured"], per_label[other]["_measured"]
            ra = per_label[first].get("render_path")
            rb = per_label[other].get("render_path")
            if ra and rb and os.path.exists(ra) and os.path.exists(rb):
                d = mad(load(ra), load(rb))
                identical = a["render_sha256"] == b["render_sha256"]
                notes.append("final render %s vs %s: mean abs diff = %.4f%s"
                             % (first, other, d, " (byte identical)" if identical else ""))
                if d > RENDER_TOLERANCE:
                    failures.append("final render %s vs %s differs by more than the noise of "
                                    "this build (mean abs diff %.4f > %.4f)"
                                    % (first, other, d, RENDER_TOLERANCE))
            pa = os.path.join(args.directory, first + "_viewport_before.png")
            pb = os.path.join(args.directory, other + "_viewport_before.png")
            if os.path.exists(pa) and os.path.exists(pb):
                d = mad(load(pa), load(pb))
                notes.append("viewport before the render, %s vs %s: mean abs diff = %.3f"
                             % (first, other, d))
                if d > VIEWPORT_TOLERANCE:
                    failures.append("viewport before the render differs between %s and %s "
                                    "(mean abs diff %.3f > %.3f)"
                                    % (first, other, d, VIEWPORT_TOLERANCE))

    print("")
    for note in notes:
        print("  " + note)
    print("")
    if failures:
        for failure in failures:
            print("  FAIL: " + failure)
        print("\nfalcon_evict_viewport_gate: FAIL (%d)" % len(failures))
        return 1
    print("falcon_evict_viewport_gate: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
