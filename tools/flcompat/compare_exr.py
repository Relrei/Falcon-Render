# flcompat C1: 2枚の EXR を画素で比べる。
# ★素の Blender の中で走らせる(この環境の system python には EXR を読む物が無い)。
#   blender --factory-startup -b --python compare_exr.py
#   環境変数 FLC_A / FLC_B / FLC_JSON
#
# 出す物: 完全一致か / 違う画素の数と割合 / 最大差 / 相対差の分位 / 差の地図(PNG)
import json
import os
import sys

import bpy
import numpy as np

A = os.environ["FLC_A"]
B = os.environ["FLC_B"]
JS = os.environ["FLC_JSON"]
MAP = os.environ.get("FLC_MAP", "")


def load(path):
    img = bpy.data.images.load(path)
    w, h = img.size
    ch = img.channels
    buf = np.empty(w * h * ch, dtype=np.float32)
    img.pixels.foreach_get(buf)
    bpy.data.images.remove(img)
    return buf.reshape(h, w, ch), w, h


def main():
    a, w, h = load(A)
    b, w2, h2 = load(B)
    res = {"a": A, "b": B, "w": w, "h": h}
    if (w, h) != (w2, h2) or a.shape != b.shape:
        res["verdict"] = "SHAPE_MISMATCH"
        res["b_shape"] = [w2, h2]
    else:
        rgb_a = a[:, :, :3]
        rgb_b = b[:, :, :3]
        d = np.abs(rgb_a - rgb_b)
        # 画素単位(3成分のどれかが違えばその画素は違う)
        px_diff = np.any(d > 0.0, axis=2)
        npx = int(px_diff.sum())
        res["pixels_total"] = int(w * h)
        res["pixels_diff"] = npx
        res["pixels_diff_pct"] = round(100.0 * npx / (w * h), 6)
        res["max_abs"] = float(d.max())
        denom = np.maximum(np.abs(rgb_a), np.abs(rgb_b))
        rel = np.where(denom > 1e-8, d / np.maximum(denom, 1e-8), 0.0)
        res["max_rel"] = float(rel.max())
        res["mean_abs"] = float(d.mean())
        if npx:
            nz = d[px_diff]
            res["q50_abs_of_diff"] = float(np.median(nz))
            res["q99_abs_of_diff"] = float(np.quantile(nz, 0.99))
        # 全体の RMSE(相対)
        res["rmse"] = float(np.sqrt(((rgb_a - rgb_b) ** 2).mean()))
        res["verdict"] = "BIT_IDENTICAL" if npx == 0 else "DIFF"

        if MAP and npx:
            # 差の地図: log10(|差|) を 0..1 へ。差ゼロは黒。
            m = d.max(axis=2)
            v = np.zeros_like(m)
            nzm = m > 0
            v[nzm] = np.clip((np.log10(m[nzm]) + 8.0) / 8.0, 0.0, 1.0)
            out = np.zeros((h, w, 4), dtype=np.float32)
            out[:, :, 0] = v
            out[:, :, 1] = v * 0.4
            out[:, :, 2] = 1.0 - v
            out[:, :, 3] = 1.0
            img = bpy.data.images.new("flcmap", w, h, alpha=True, float_buffer=True)
            img.pixels.foreach_set(out.reshape(-1))
            img.filepath_raw = MAP
            img.file_format = "PNG"
            img.save()
            res["map"] = MAP

    with open(JS, "w") as f:
        json.dump(res, f, ensure_ascii=False, indent=1)
    print("FLCOMPAT_COMPARE " + json.dumps(res, ensure_ascii=False))


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stderr.write("FLCOMPAT: compare_exr.py FAILED\n")
