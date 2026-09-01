# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""Judge a run of ``falcon_evict_viewport_noviewport.py``.

    python3 tools/falcon_evict_viewport_persist_check.py PREFIX [--log FILE]

With no 3D viewport in Rendered shading the eviction has to be a complete
no-op. This checks the two things that would show otherwise:

1. Nothing was evicted -- the Blender log has no ``[falcon-evict]`` line.
2. Persistent Data survived. Blender writes the Cycles timings into the PNG
   metadata, so the second render of the same frame must show a
   ``synchronization_time`` far below the first one. If the switch had freed the
   cache, the second render would synchronise from scratch again.

Exit code 0 = PASS, 1 = FAIL.
"""

from __future__ import annotations

import argparse
import json
import os
import sys

from PIL import Image

# The second render must synchronise in at most this fraction of the first
# render's synchronisation time. With Persistent Data working it is ~0.
SYNC_RATIO_MAX = 0.25
# Below this the first render's sync is too short to tell anything apart.
SYNC_FLOOR_SECONDS = 0.05


def parse_timecode(value):
    """'00:00.33' or '00:01:02.50' -> seconds."""
    parts = value.strip().split(":")
    seconds = 0.0
    for part in parts:
        seconds = seconds * 60.0 + float(part)
    return seconds


def sync_seconds(path):
    """Total Cycles synchronisation time recorded in a rendered PNG."""
    info = Image.open(path).info
    keys = [k for k in info if k.endswith(".synchronization_time")]
    if not keys:
        return None
    return sum(parse_timecode(info[k]) for k in keys)


def main():
    parser = argparse.ArgumentParser(prog="falcon_evict_viewport_persist_check")
    parser.add_argument("prefix")
    parser.add_argument("--log", default=None, help="Blender log to scan for eviction lines")
    args = parser.parse_args()

    failures = []
    notes = []

    result_path = args.prefix + "_result.json"
    if not os.path.exists(result_path):
        print("  FAIL: %s is missing (Blender crashed or never finished)" % result_path)
        print("\nfalcon_evict_viewport_persist_check: FAIL (1)")
        return 1
    with open(result_path) as fh:
        result = json.load(fh)

    label = result.get("evict_env", "?")
    if result.get("status") != "ok":
        failures.append("run reported %s (%s)" % (result.get("status"), result.get("error", "")))

    if args.log and os.path.exists(args.log):
        with open(args.log, "rb") as fh:
            evicted = [line for line in fh.read().splitlines() if b"[falcon-evict]" in line]
        if evicted:
            failures.append("something was evicted even though no viewport was in Rendered "
                            "shading: %s" % b" / ".join(evicted).decode("utf-8", "replace"))
        else:
            notes.append("no eviction happened (no [falcon-evict] line in the log)")

    renders = result.get("renders", [])
    if len(renders) < 2:
        failures.append("needs at least two renders to tell whether Persistent Data survived")
    else:
        syncs = []
        for entry in renders:
            if not entry.get("written"):
                failures.append("render %d was not written" % entry["index"])
                continue
            syncs.append((entry["index"], sync_seconds(entry["path"]), entry["seconds"]))
        for index, sync, wall in syncs:
            notes.append("render %d: %.2f s wall, synchronisation %s"
                         % (index, wall, "n/a" if sync is None else "%.3f s" % sync))
        if len(syncs) >= 2 and syncs[0][1] is not None and syncs[1][1] is not None:
            first, second = syncs[0][1], syncs[1][1]
            if first < SYNC_FLOOR_SECONDS:
                notes.append("first render synchronised in %.3f s, too little to tell "
                             "Persistent Data apart on this scene" % first)
            elif second > first * SYNC_RATIO_MAX:
                failures.append("Persistent Data did not survive: the second render "
                                "synchronised again (%.3f s vs %.3f s)" % (second, first))
            else:
                notes.append("Persistent Data survived: the second render skipped "
                             "synchronisation (%.3f s vs %.3f s)" % (second, first))

    print("")
    print("  FALCON_CYCLES_EVICT_VIEWPORT=%s, no Rendered viewport" % label)
    for note in notes:
        print("    " + note)
    if failures:
        for failure in failures:
            print("    FAIL: " + failure)
        print("\nfalcon_evict_viewport_persist_check: FAIL (%d)" % len(failures))
        return 1
    print("\nfalcon_evict_viewport_persist_check: PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
