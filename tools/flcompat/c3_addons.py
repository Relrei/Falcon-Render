# flcompat C3: 同梱アドオンを全部有効化して、①登録が例外ゼロ ②素の UI クラスが
# 乗っ取られていない/消えていない ことを見る。
#
#   blender --factory-startup -b --python c3_addons.py
#   環境変数 FLC_JSON(出力)・FLC_ENABLE_ALL=1(全部有効化する)
#
# ★②の物差し: 登録済みの bpy.types のうち Panel / Menu / Header / UIList について
#   「クラス名 -> (bl_idname, 場所, 親, draw/poll の定義元モジュールと行番号)」を出す。
#   素と Falcon で突き合わせて、**素に在って Falcon に無い**(= 継いで登録して元が消えた)
#   と **draw の定義元が変わった**(= 乗っ取り)を機械で拾う。
import json
import os
import sys
import traceback

import bpy
import addon_utils

JS = os.environ["FLC_JSON"]
ENABLE_ALL = bool(os.environ.get("FLC_ENABLE_ALL"))


def fn_sig(f):
    if f is None:
        return None
    try:
        code = f.__code__
        mod = f.__module__ or ""
        fname = code.co_filename
        # ビルドごとに違う絶対パスを消す(比較できる形にする)
        for marker in ("/scripts/", "/blender/"):
            i = fname.rfind(marker)
            if i >= 0:
                fname = fname[i:]
                break
        return {"mod": mod, "qual": getattr(f, "__qualname__", f.__name__),
                "file": fname, "line": code.co_firstlineno}
    except Exception:
        return {"mod": str(getattr(f, "__module__", "?")), "qual": repr(f)}


def ui_dump():
    out = {}
    for base_name in ("Panel", "Menu", "Header", "UIList"):
        base = getattr(bpy.types, base_name)
        for cls in base.__subclasses__():
            for c in [cls] + cls.__subclasses__():
                name = c.__name__
                if name in out:
                    continue
                out[name] = {
                    "base": base_name,
                    "bl_idname": getattr(c, "bl_idname", ""),
                    "space": getattr(c, "bl_space_type", ""),
                    "region": getattr(c, "bl_region_type", ""),
                    "context": getattr(c, "bl_context", ""),
                    "category": getattr(c, "bl_category", ""),
                    "parent": getattr(c, "bl_parent_id", ""),
                    "label": getattr(c, "bl_label", ""),
                    "order": getattr(c, "bl_order", 0),
                    "options": sorted(getattr(c, "bl_options", set()) or set()),
                    "draw": fn_sig(getattr(c, "draw", None)),
                    "poll": fn_sig(getattr(c, "poll", None)),
                    # ★bpy.types.X.append(fn) で足すと draw は _dyn_ui_initialize の
                    #   包みに差し替わり、元の draw は _draw_funcs[0] に残る。
                    #   「足した(安全)」と「継いで置き換えた(元が消える)」の区別はここ。
                    "draw_funcs": [fn_sig(f) for f in
                                   getattr(getattr(c, "draw", None), "_draw_funcs", []) or []],
                    "mro": [b.__name__ for b in c.__mro__[1:4]],
                }
    return out


def main():
    res = {"blender": list(bpy.app.version), "name": bpy.app.version_string,
           "enable_all": ENABLE_ALL}

    # ★同梱アドオンだけを対象にする(ビルドの中に在る物)。
    #   ~/.config/blender/5.2/scripts/addons の本人のアドオンは BLENDER_USER_RESOURCES を
    #   逃がしても addon_utils には見えてしまうので、パスで切る。
    #   ⇒ こうしないと「この機械にたまたま在る物」で門の結果が変わる。
    local = bpy.utils.resource_path("LOCAL")
    mods, foreign = [], []
    for m in addon_utils.modules():
        f = getattr(m, "__file__", "") or ""
        (mods if f.startswith(local) else foreign).append(m.__name__)
    mods = sorted(set(mods))
    res["bundled_addons"] = mods
    res["user_addons_seen"] = sorted(set(foreign))
    res["local"] = local
    res["enabled_before"] = sorted(bpy.context.preferences.addons.keys())

    res["ui_before"] = ui_dump()

    failed = []
    enabled = []
    if ENABLE_ALL:
        for name in mods:
            try:
                # ★default_set=True が要る。False だと preferences.addons に載らず、
                #   自分の設定を読むアドオン(rigify)が KeyError で落ちる = 門の側の誤り。
                #   BLENDER_USER_RESOURCES を逃がしてあるので本人の設定は汚れない
                #   (保存もしない)。
                m = addon_utils.enable(name, default_set=True, persistent=False,
                                       handle_error=None)
                if m is None:
                    failed.append({"addon": name, "err": "enable() returned None"})
                else:
                    enabled.append(name)
            except Exception as e:
                failed.append({"addon": name, "err": "%s: %s" % (type(e).__name__, e),
                               "tb": traceback.format_exc()[-800:]})
    res["enabled_ok"] = enabled
    res["enable_failed"] = failed
    res["enabled_after"] = sorted(bpy.context.preferences.addons.keys())

    res["ui_after"] = ui_dump()

    # 拡張(extensions)も数える
    try:
        res["extensions"] = sorted(
            r.module for r in bpy.context.preferences.filepaths.script_directories)
    except Exception:
        pass

    with open(JS, "w") as f:
        json.dump(res, f, ensure_ascii=False, indent=1, sort_keys=True)
    with open(JS + ".done", "w") as f:
        f.write("ok\n")


try:
    main()
except Exception:
    traceback.print_exc()
    sys.stderr.write("FLCOMPAT: c3_addons.py FAILED\n")
