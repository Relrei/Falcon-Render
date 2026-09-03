# flcompat C1: 素の Blender と Falcon Render で「同じ設定」に正規化して1枚撮る。
#
# ★正規化するのは「時間と乱数」だけ。場面の中身(材質・カメラ・コンポジット)は
#   保存されたまま触らない。ユーザーが何も触らずに開いた状態を測るのが C1 の意味。
#
# 使い方: blender --factory-startup -b <blend> --python render_one.py
#         環境変数 FLC_OUT(拡張子なしの出力先)・FLC_SAMPLES・FLC_RES_X/Y・FLC_THREADS
#
# ⚠ Blender は Python が落ちても rc=0 で完走する(obs3 罠/2026-08-31)。
#    ⇒ 最後に FLC_OUT.done を書き、呼ぶ側はそれで判定する。
import os
import sys
import json

import bpy

OUT = os.environ["FLC_OUT"]
SAMPLES = int(os.environ.get("FLC_SAMPLES", "8"))
RES_X = int(os.environ.get("FLC_RES_X", "960"))
RES_Y = int(os.environ.get("FLC_RES_Y", "540"))
THREADS = int(os.environ.get("FLC_THREADS", "4"))


def main():
    scene = bpy.context.scene
    r = scene.render

    info = {
        "engine_saved": r.engine,
        "blend": bpy.data.filepath,
        "frame": scene.frame_current,
        "use_nodes": bool(scene.use_nodes),
        "use_compositing": bool(r.use_compositing),
        "use_sequencer": bool(r.use_sequencer),
    }

    r.engine = "CYCLES"
    c = scene.cycles

    # --- 時間と乱数だけを固定する ---
    c.device = "CPU"                      # GPU は他の枠と取り合うので使わない
    c.samples = SAMPLES
    c.use_adaptive_sampling = False
    c.time_limit = 0.0
    c.use_denoising = False
    c.seed = 0
    c.use_animated_seed = False
    if hasattr(c, "use_preview_denoising"):
        c.use_preview_denoising = False
    if hasattr(c, "use_auto_tile"):
        c.use_auto_tile = False           # タイル分割は結果を変えないが、条件を揃える

    r.resolution_x = RES_X
    r.resolution_y = RES_Y
    r.resolution_percentage = 100
    r.threads_mode = "FIXED"
    r.threads = THREADS
    r.use_persistent_data = False
    r.use_sequencer = False               # VSE が刺さっている場合に画を上書きされない
    r.use_border = False
    r.use_stamp = False

    # --- 出力は 32bit float の無圧縮 EXR(量子化で差を隠さない) ---
    im = r.image_settings
    im.file_format = "OPEN_EXR"
    im.color_mode = "RGB"
    im.color_depth = "32"
    im.exr_codec = "NONE"
    r.use_file_extension = True
    r.filepath = OUT

    # --- Falcon の既定を1つずつ切るための口(C1 の原因特定に使う) ---
    for kv in os.environ.get("FLC_SET", "").split(";"):
        kv = kv.strip()
        if not kv:
            continue
        path, _, val = kv.partition("=")
        obj = scene
        parts = path.split(".")
        for p in parts[:-1]:
            obj = getattr(obj, p)
        cur = getattr(obj, parts[-1])
        if isinstance(cur, bool):
            val = val not in ("0", "False", "false", "")
        elif isinstance(cur, int):
            val = int(val)
        elif isinstance(cur, float):
            val = float(val)
        setattr(obj, parts[-1], val)
        info.setdefault("set", []).append(kv)

    if os.environ.get("FLC_NOCOMP"):
        scene.use_nodes = False
        r.use_compositing = False
        info["nocomp"] = True

    bpy.ops.render.render(write_still=True)

    path = bpy.path.abspath(r.filepath) + ".exr"
    info["out"] = path
    info["out_bytes"] = os.path.getsize(path)
    with open(OUT + ".done", "w") as f:
        json.dump(info, f, ensure_ascii=False, indent=1)


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stderr.write("FLCOMPAT: render_one.py FAILED\n")
