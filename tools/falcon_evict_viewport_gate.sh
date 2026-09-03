#!/usr/bin/env bash
# SPDX-FileCopyrightText: 2026 Falcon Render
#
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Gate for FALCON_CYCLES_EVICT_VIEWPORT.
#
#   tools/falcon_evict_viewport_gate.sh <scene.blend> [label ...]
#
# Runs the scene once per label inside the AI virtual display, samples per
# process VRAM while each run is going, and then judges the evidence with
# falcon_evict_viewport_compare.py.
#
# The switch is on by default, so the labels mean:
#   OFF      FALCON_CYCLES_EVICT_VIEWPORT=0  -- the behaviour before the switch
#   ON       FALCON_CYCLES_EVICT_VIEWPORT=1  -- asked for explicitly
#   DEFAULT  variable unset                  -- what users actually get
#
# Two more checks run first and do not need a Rendered viewport:
#   bitcheck      the picture is the same across default / off / on / the binary
#                 from before the switch, in background and in the GUI
#   persistcheck  with no Rendered viewport, nothing is evicted and Persistent
#                 Data survives between renders
#
# The GUI is required: there is no 3D viewport in background Blender, so this
# cannot be a -b test.
#
# NOTE: `ai-display launch` must not run while this shell holds the GPU lock FD
# (a dbus daemon inherits it and the lock never comes back), so fd 9 is closed
# for the child.  See obs3 罠/2026-08-27_flockの中でai-displayを使うとロックが死ぬ.
#
# NOTE: the user preferences are used on purpose (no --factory-startup): the
# Cycles compute device lives in the preferences, and factory settings would
# quietly render on the CPU.

set -u

BLENDER="${BLENDER:-$HOME/Documents2/build_blender_5.2_dlss/bin/blender}"
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${FALCON_EVICT_OUT:-$HOME/Documents2/falcon-evict-lab/run}"
WARM="${FALCON_EVICT_WARM:-20}"
SAMPLES="${FALCON_EVICT_SAMPLES:-0}"
RES_PERCENT="${FALCON_EVICT_RES_PERCENT:-0}"
PREVIEW_SAMPLES="${FALCON_EVICT_PREVIEW_SAMPLES:-16}"
PROBE="${FALCON_EVICT_PROBE:-0.5}"
RENDER_MODE="${FALCON_EVICT_RENDER_MODE:-exec}"
EXTRA_ARGS="${FALCON_EVICT_EXTRA_ARGS:-}"
TIMEOUT="${FALCON_EVICT_TIMEOUT:-900}"
GPU_LOCK="${FALCON_GPU_LOCK:-/tmp/claude-1000/-home-mirai/gpu.lock}"
MIN_FREE_MIB="${FALCON_EVICT_MIN_FREE_MIB:-7000}"

SCENE="${1:-}"
if [ -z "$SCENE" ]; then
  echo "usage: $0 <scene.blend> [label ...]" >&2
  exit 2
fi
shift
LABELS=("$@")
if [ ${#LABELS[@]} -eq 0 ]; then
  LABELS=(OFF DEFAULT)
fi

mkdir -p "$OUT"

free_vram() {
  nvidia-smi --query-gpu=memory.free --format=csv,noheader,nounits | head -1
}

sample_vram() {
  # $1 = pid, $2 = output file. "<epoch> <MiB>" per line, one sample per 0.25 s.
  # The timestamp is what lets the peak be narrowed down to the final render.
  local pid="$1" out="$2"
  : >"$out"
  while kill -0 "$pid" 2>/dev/null; do
    nvidia-smi --query-compute-apps=pid,used_memory --format=csv,noheader,nounits 2>/dev/null |
      awk -F', *' -v p="$pid" -v t="$(date +%s.%N)" '$1 == p { print t, $2 }' >>"$out"
    sleep 0.25
  done
}

peak_in_window() {
  # $1 = samples file, $2 = start epoch (empty = from the beginning),
  # $3 = end epoch (empty = to the end).
  awk -v a="${2:-0}" -v b="${3:-9999999999}" \
    '$1 >= a && $1 <= b && $2 > m { m = $2 } END { print (m ? m : "n/a") }' "$1"
}

run_label() {
  local label="$1"
  local log="$OUT/${label}_blender.log"
  local vram="$OUT/${label}_vram.txt"
  local runner="$OUT/${label}_run.sh"
  # The switch is on by default now, so the labels mean:
  #   OFF     -> FALCON_CYCLES_EVICT_VIEWPORT=0 (the old behaviour)
  #   ON      -> FALCON_CYCLES_EVICT_VIEWPORT=1 (asked for explicitly)
  #   anything else, e.g. DEFAULT -> variable unset, which is what users get
  local evict_env="unset FALCON_CYCLES_EVICT_VIEWPORT"
  case "$label" in
    OFF*) evict_env="export FALCON_CYCLES_EVICT_VIEWPORT=0" ;;
    ON*) evict_env="export FALCON_CYCLES_EVICT_VIEWPORT=1" ;;
  esac

  local free_now
  free_now="$(free_vram)"
  echo "=== $label: free VRAM before = ${free_now} MiB"
  if [ "$free_now" -lt "$MIN_FREE_MIB" ]; then
    echo "!!! $label: less than ${MIN_FREE_MIB} MiB free on the GPU, refusing to start." >&2
    echo "!!! Deliberately running the GPU out of memory is off limits." >&2
    return 3
  fi

  # ai-display launch throws the application's output away, so go through a
  # small runner that keeps it.
  {
    echo '#!/usr/bin/env bash'
    echo "$evict_env"
    printf 'exec %q %q --python %q -- --out %q --label %q --warm %q --probe %q --preview-samples %q --samples %q --res-percent %q --render-mode %q %s >%q 2>&1\n' \
      "$BLENDER" "$SCENE" "$HERE/falcon_evict_viewport_gate.py" \
      "$OUT" "$label" "$WARM" "$PROBE" "$PREVIEW_SAMPLES" "$SAMPLES" "$RES_PERCENT" "$RENDER_MODE" "$EXTRA_ARGS" "$log"
  } >"$runner"
  chmod +x "$runner"

  # Take the shared GPU lock for the length of the run, but keep the FD away
  # from anything ai-display starts.
  exec 9>"$GPU_LOCK"
  flock -w 3600 9 || { echo "could not take $GPU_LOCK" >&2; return 3; }

  ai-display launch "$runner" 9>&- || { exec 9>&-; return 3; }

  # ai-display launch returns immediately; find the Blender process it started.
  local pid="" waited=0
  while [ "$waited" -lt 120 ]; do
    pid="$(pgrep -n -f -- "--label $label --warm" || true)"
    [ -n "$pid" ] && break
    sleep 1
    waited=$((waited + 1))
  done
  if [ -z "$pid" ]; then
    echo "!!! $label: Blender did not start (see $log)" >&2
    exec 9>&-
    return 3
  fi
  echo "=== $label: blender pid $pid"

  sample_vram "$pid" "$vram" &
  local sampler=$!

  local elapsed=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
      echo "!!! $label: timed out after ${TIMEOUT}s, killing $pid" >&2
      kill "$pid" 2>/dev/null
      sleep 5
      kill -9 "$pid" 2>/dev/null
      break
    fi
  done
  wait "$sampler" 2>/dev/null

  exec 9>&-

  local result="$OUT/${label}_result.json"
  local r_start="" r_end=""
  if [ -f "$result" ]; then
    r_start="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("render_start_time",""))' "$result")"
    r_end="$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1])).get("render_end_time",""))' "$result")"
  fi

  local peak_all peak_render
  peak_all="$(peak_in_window "$vram")"
  peak_render="$(peak_in_window "$vram" "$r_start" "$r_end")"
  echo "=== $label: peak VRAM  whole run = ${peak_all} MiB / during the final render = ${peak_render} MiB (${elapsed}s wall)"
  printf '%s %s\n' "$peak_all" "$peak_render" >"$OUT/${label}_peak_vram.txt"
  grep -a '^\[falcon-evict\]' "$log" | sed 's/^/    /'
  return 0
}

bitcheck() {
  # The switch must not touch the picture. Rendered on the CPU with everything
  # non-deterministic turned off, so the pixels can be compared exactly.
  #
  # Five renders, all of which must land on the same pixels:
  #   background, default (switch on)      -- what users get
  #   background, FALCON_..._VIEWPORT=0    -- the escape hatch
  #   background, FALCON_..._VIEWPORT=1    -- asked for explicitly
  #   background, the binary from before the switch existed
  #   GUI with the 3D viewport in Solid shading, default and =0
  # Background alone would not be enough: `blender -b` never reaches the render
  # operator where the eviction lives. The GUI runs with a Solid viewport are
  # the ones that actually walk through it and have to come out having done
  # nothing.
  local dir="$OUT/bitcheck"
  mkdir -p "$dir"
  echo "=== bitcheck: default / off / on / baseline binary, background and GUI"

  env -u FALCON_CYCLES_EVICT_VIEWPORT "$BLENDER" -b --factory-startup \
    --python "$HERE/falcon_evict_viewport_bitcheck.py" -- "$dir/bg_default" >/dev/null 2>&1
  FALCON_CYCLES_EVICT_VIEWPORT=0 "$BLENDER" -b --factory-startup \
    --python "$HERE/falcon_evict_viewport_bitcheck.py" -- "$dir/bg_off" >/dev/null 2>&1
  FALCON_CYCLES_EVICT_VIEWPORT=1 "$BLENDER" -b --factory-startup \
    --python "$HERE/falcon_evict_viewport_bitcheck.py" -- "$dir/bg_on" >/dev/null 2>&1

  local files=("$dir/bg_default.png" "$dir/bg_off.png" "$dir/bg_on.png")
  if [ -n "${FALCON_EVICT_BASELINE_BLENDER:-}" ] && [ -x "$FALCON_EVICT_BASELINE_BLENDER" ]; then
    env -u FALCON_CYCLES_EVICT_VIEWPORT "$FALCON_EVICT_BASELINE_BLENDER" -b --factory-startup \
      --python "$HERE/falcon_evict_viewport_bitcheck.py" -- "$dir/bg_baseline" >/dev/null 2>&1
    files+=("$dir/bg_baseline.png")
  fi

  # GUI, 3D viewport in Solid shading -- goes through the render operator.
  gui_reference default "$dir" && files+=("$dir/gui_default_1.png")
  gui_reference off "$dir" && files+=("$dir/gui_off_1.png")

  local missing=0
  for f in "${files[@]}"; do
    [ -f "$f" ] || { echo "  FAIL: $f was not rendered"; missing=1; }
  done
  [ "$missing" = 1 ] && return 1

  # Compare the pixels, not the file: Blender always stamps the render time and
  # the date into the PNG metadata, so the files never hash the same.
  python3 "$HERE/falcon_evict_viewport_pixelhash.py" "${files[@]}"
}

gui_run() {
  # $1 = name, $2 = out dir, $3.. = extra args for the noviewport script.
  # Runs a GUI Blender with every 3D viewport put into Solid shading.
  local name="$1" dir="$2"
  shift 2
  local runner="$dir/${name}_run.sh"
  local log="$dir/${name}_blender.log"
  local scene_arg=("$SCENE")
  local evict_env="unset FALCON_CYCLES_EVICT_VIEWPORT"
  case "$name" in
    *off*) evict_env="export FALCON_CYCLES_EVICT_VIEWPORT=0" ;;
    *on*) evict_env="export FALCON_CYCLES_EVICT_VIEWPORT=1" ;;
  esac
  if [ "${GUI_FACTORY:-0}" = "1" ]; then
    scene_arg=(--factory-startup)
  fi

  {
    echo '#!/usr/bin/env bash'
    echo "$evict_env"
    printf 'exec %q' "$BLENDER"
    printf ' %q' "${scene_arg[@]}"
    printf ' --python %q -- --out %q' "$HERE/falcon_evict_viewport_noviewport.py" "$dir/$name"
    printf ' %q' "$@"
    printf ' >%q 2>&1\n' "$log"
  } >"$runner"
  chmod +x "$runner"

  exec 9>"$GPU_LOCK"
  flock -w 3600 9 || { echo "could not take $GPU_LOCK" >&2; return 3; }
  ai-display launch "$runner" 9>&- >/dev/null || { exec 9>&-; return 3; }

  local pid="" waited=0
  while [ "$waited" -lt 120 ]; do
    pid="$(pgrep -n -f -- "--out $dir/$name" || true)"
    [ -n "$pid" ] && break
    sleep 1
    waited=$((waited + 1))
  done
  if [ -z "$pid" ]; then
    echo "  FAIL: $name did not start (see $log)" >&2
    exec 9>&-
    return 3
  fi
  local elapsed=0
  while kill -0 "$pid" 2>/dev/null; do
    sleep 1
    elapsed=$((elapsed + 1))
    if [ "$elapsed" -ge "$TIMEOUT" ]; then
      echo "  FAIL: $name timed out" >&2
      kill -9 "$pid" 2>/dev/null
      break
    fi
  done
  exec 9>&-
  return 0
}

gui_reference() {
  # $1 = "default" or "off", $2 = out dir
  GUI_FACTORY=1 gui_run "gui_$1" "$2" --reference --renders 1
}

persistcheck() {
  # With no 3D viewport in Rendered shading the eviction has to do nothing at
  # all -- including leaving Persistent Data alone. Rendering the same frame
  # twice shows it: the second render must skip synchronisation.
  local dir="$OUT/persistcheck"
  mkdir -p "$dir"
  echo "=== persistcheck: Persistent Data with no Rendered viewport"

  local status=0
  for name in default off; do
    gui_run "$name" "$dir" --renders 2 \
      --res-percent "$RES_PERCENT" --samples "$SAMPLES" || status=1
    python3 "$HERE/falcon_evict_viewport_persist_check.py" "$dir/$name" \
      --log "$dir/${name}_blender.log" || status=1
  done
  return "$status"
}

BITCHECK_STATUS=0
bitcheck || BITCHECK_STATUS=1

PERSIST_STATUS=0
persistcheck || PERSIST_STATUS=1

for label in "${LABELS[@]}"; do
  run_label "$label" || exit $?
done

echo ""
echo "=== peak VRAM of the Blender process (MiB)"
printf '  %-5s %-12s %s\n' label "whole run" "during the final render"
for label in "${LABELS[@]}"; do
  # shellcheck disable=SC2046
  printf '  %-5s %-12s %s\n' "$label" $(cat "$OUT/${label}_peak_vram.txt" 2>/dev/null || echo "n/a n/a")
done

python3 "$HERE/falcon_evict_viewport_compare.py" "$OUT" --labels "${LABELS[@]}"
COMPARE_STATUS=$?

if [ "$BITCHECK_STATUS" != 0 ]; then
  echo "falcon_evict_viewport_gate: FAIL (bitcheck)"
  exit 1
fi
if [ "$PERSIST_STATUS" != 0 ]; then
  echo "falcon_evict_viewport_gate: FAIL (persistcheck)"
  exit 1
fi
exit "$COMPARE_STATUS"
