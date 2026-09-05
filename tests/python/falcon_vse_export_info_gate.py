# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""「直近の書き出しで何が効いたか」の1行の門。

  blender -b --factory-startup --python tests/python/falcon_vse_export_info_gate.py -- <作業先>

本人 (2026-09-04)「VSE 内に GPU 処理や CPU の処理が機能してるのかも正直わからん」。
切っただけの経路を採ったか / 符号化が GPU だったか / 何秒だったかを、
`scene.render.falcon_last_export_info` の1行から読めることを確かめる。

★`FALCON_VSE_FASTPATH` は C++ 側で**最初の1回だけ**読んで固める(static)。
  同じプロセスの中では倒せないので、その門だけ**自分をもう1本起こして**測る。

出力の最後に GATE_RESULT PASS / FAIL が1行出る(FAIL なら終了コード 1)。
"""

import os
import shutil
import subprocess
import sys

import bpy

FAILS = []
LOG = []


def check(name, ok, detail=""):
    LOG.append("GATE %-46s %s  %s" % (name, "PASS" if ok else "FAIL", detail))
    if not ok:
        FAILS.append(name)


def gate_args():
    argv = sys.argv
    if "--" in argv:
        return argv[argv.index("--") + 1:]
    return []


ARGS = gate_args()
WORK = ARGS[0] if ARGS else "/tmp/falcon_vse_export_info_gate"
# 子(FALCON_VSE_FASTPATH=0 で起こし直した側)は、その1件だけを測って帰る。
CHILD = "--fastpath-off" in ARGS

if "falcon_vse_bridge" not in sys.modules:
    bpy.ops.preferences.addon_enable(module="falcon_vse_bridge")


def fresh(sub):
    path = os.path.join(WORK, sub)
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path + os.sep


def info(scene):
    return (getattr(scene.render, "falcon_last_export_info", "") or "").strip()


# ★NVENC には最小の大きさがある。64x36 では `avcodec_open2` が開かず**黙って CPU に
#   戻る**ので、「GPU が効いているか」を測る台にならない(実測: 64x36 は CPU、
#   640x360 は NVENC)。
WIDTH, HEIGHT = 640, 360


def setup(out, movie=True, share=False):
    scene = bpy.context.scene
    rd = scene.render
    rd.engine = 'BLENDER_WORKBENCH'
    rd.resolution_x, rd.resolution_y = WIDTH, HEIGHT
    rd.resolution_percentage = 100
    rd.filepath = out
    settings = rd.image_settings
    if movie:
        settings.media_type = 'VIDEO'
        settings.file_format = 'FFMPEG'
        rd.ffmpeg.format = 'MPEG4'
        rd.ffmpeg.codec = 'H264'
        rd.ffmpeg.audio_codec = 'NONE'
    else:
        settings.media_type = 'IMAGE'
        settings.file_format = 'PNG'
    rd.use_sequencer = False
    if scene.sequence_editor is not None:
        scene.sequence_editor_clear()
    # 素材を撮る間に、アドオンが出来た物を Sequencer へ足さないように。
    scene.falcon_vse_share_output = share
    scene.falcon_output_name = ""
    return scene


scene = bpy.context.scene

check("info/RNA の口がある", hasattr(scene.render, "falcon_last_export_info"))
if not hasattr(scene.render, "falcon_last_export_info"):
    print("GATE_RESULT FAIL  (RNA が無い)")
    sys.exit(1)

check("info/書けない(読み取り専用)",
      scene.render.bl_rna.properties["falcon_last_export_info"].is_readonly)


# --- 素材の動画を1本撮る(切っただけの経路の入力)---------------------------

SRC_DIR = fresh("src")
setup(SRC_DIR + "src")
scene.frame_start, scene.frame_end = 1, 8
bpy.ops.render.render(animation=True)
SRC = [f for f in os.listdir(SRC_DIR) if f.startswith("src")]
check("info/素材の動画が出来た", len(SRC) == 1, str(os.listdir(SRC_DIR)))
SRC_PATH = os.path.join(SRC_DIR, SRC[0]) if SRC else ""


def render_sequencer_cut(sub):
    """素材を1本切って、Sequencer だけの書き出しを回す。"""
    out = fresh(sub)
    sc = setup(out + "out")
    sc.render.use_sequencer = True
    editor = sc.sequence_editor_create()
    strip = editor.strips.new_movie("cut", SRC_PATH, 1, 1)
    strip.frame_final_start = 1
    strip.frame_final_end = 4
    sc.frame_start, sc.frame_end = 1, 3
    bpy.ops.render.render(animation=True)
    return sc


if CHILD:
    # 子はここだけ。親のプロセスでは `FALCON_VSE_FASTPATH` を倒せない。
    sc = render_sequencer_cut("fastpath_off")
    line = info(sc)
    check("info/口を切ると「使わない」になる", "使わない" in line, repr(line))
    check("info/理由に FALCON_VSE_FASTPATH が出る",
          "FALCON_VSE_FASTPATH" in line, repr(line))
else:
    # (1) 静止画列 = そもそも動画の書き出しではない。
    out = fresh("png")
    sc = setup(out + "shot", movie=False)
    sc.frame_start, sc.frame_end = 1, 2
    bpy.ops.render.render(animation=True)
    line = info(sc)
    check("info/静止画列では「動画の書き出しではない」",
          "使わない" in line and "動画の書き出しではない" in line, repr(line))

    # (2) 動画・Sequencer 無し = 切っただけの経路は外れ、符号化器が選ばれる。
    out = fresh("movie")
    sc = setup(out + "clip")
    sc.frame_start, sc.frame_end = 1, 3
    sc.render.ffmpeg.use_hardware_encoder = True
    bpy.ops.render.render(animation=True)
    line = info(sc)
    check("info/切っただけの経路が外れた理由が出る",
          "使わない" in line and "—" in line, repr(line))
    check("info/符号化器が NVENC か CPU で出る",
          ("NVENC" in line) or ("CPU" in line), repr(line))
    HW_LINE = line

    # (3) GPU 符号化を切ると CPU と出る。
    out = fresh("movie_cpu")
    sc = setup(out + "clip")
    sc.frame_start, sc.frame_end = 1, 3
    sc.render.ffmpeg.use_hardware_encoder = False
    bpy.ops.render.render(animation=True)
    line = info(sc)
    check("info/GPU 符号化を切ると CPU と出る",
          "符号化: CPU" in line and "NVENC" not in line, repr(line))
    # ★ここが「同じ台で向きが出た」証拠。片方だけでは物差しにならない。
    check("info/GPU を入れた時と出た文字が違う", line != HW_LINE,
          "%r / %r" % (HW_LINE, line))
    sc.render.ffmpeg.use_hardware_encoder = True

    # (4) Sequencer を切っただけの書き出し = 切っただけの経路が効く。
    sc = render_sequencer_cut("fastpath_on")
    line = info(sc)
    check("info/切っただけの書き出しで「使った」", "使った" in line, repr(line))
    check("info/何本切ったかが出る", "cuts" in line, repr(line))
    check("info/符号化はそのままコピー", "そのままコピー" in line, repr(line))
    check("info/秒が末尾に出る", line.endswith(" s"), repr(line))

    # (5) 戻す口 `FALCON_VSE_FASTPATH=0`。★静的に固めるので別プロセスで測る。
    env = dict(os.environ)
    env["FALCON_VSE_FASTPATH"] = "0"
    child = subprocess.run(
        [bpy.app.binary_path, "-b", "--factory-startup", "--python", os.path.abspath(__file__),
         "--", WORK, "--fastpath-off"],
        env=env, capture_output=True, text=True,
    )
    for out_line in child.stdout.splitlines():
        if out_line.startswith("GATE "):
            LOG.append(out_line)
            if " FAIL " in out_line:
                FAILS.append(out_line.split()[1])
    check("info/戻す口の門が回った", "GATE_RESULT" in child.stdout,
          "rc=%d" % child.returncode)


# --- 結果 -------------------------------------------------------------------

print()
for line in LOG:
    print(line)
print()
print("GATE_RESULT %s  (%d/%d)" % ("PASS" if not FAILS else "FAIL",
                                   len(LOG) - len(FAILS), len(LOG)))
if FAILS:
    print("GATE_FAILED " + ", ".join(FAILS))
sys.exit(1 if FAILS else 0)
