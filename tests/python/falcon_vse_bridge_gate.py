# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""falcon_vse_bridge の門。

  blender -b --factory-startup --python tests/python/falcon_vse_bridge_gate.py -- <作業先>

出力の最後に GATE_RESULT PASS / FAIL が1行出る(FAIL なら終了コード 1)。

`--` の後ろに渡せる旗:
  --stock            素の Blender で回す(「既定で有効」の門を問わない)
  --break-restore    壊し門: `render.filepath` を戻さない
  --break-workspace  壊し門: `workspace.sequencer_scene` を指さない
  --break-menu       壊し門: Sequencer の「追加」から入口を外す
"""

import os
import shutil
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
WORK = ARGS[0] if ARGS else "/tmp/falcon_vse_bridge_gate"
# 素の Blender では addons_core に置いていないので、既定で有効の門は問わない。
CHECK_DEFAULT_ENABLED = "--stock" not in ARGS

# 起動した時点で既に有効か = 既定で有効になっているか。
WAS_ENABLED = "falcon_vse_bridge" in sys.modules
if not WAS_ENABLED:
    bpy.ops.preferences.addon_enable(module="falcon_vse_bridge")

import falcon_vse_bridge as _m  # noqa: E402  addon_enable の後でないと入らない

# 壊し門: `render_complete` で `render.filepath` を戻さない版。
# これで「戻っている」の門が落ちることを確かめる(落ちなければ門が効いていない)。
if "--break-restore" in ARGS:
    _m.restore_output_name = lambda scene: None


def fresh(sub):
    path = os.path.join(WORK, sub)
    if os.path.isdir(path):
        shutil.rmtree(path)
    os.makedirs(path)
    return path + os.sep


def setup_scene(out, movie=False):
    scene = bpy.context.scene
    rd = scene.render
    rd.engine = 'BLENDER_WORKBENCH'
    rd.resolution_x, rd.resolution_y = 64, 36
    rd.resolution_percentage = 100
    scene.frame_start, scene.frame_end = 1, 3
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
    # 前の門のストリップを持ち越さない。
    if scene.sequence_editor is not None:
        scene.sequence_editor_clear()
    if hasattr(scene, "falcon_output_name"):
        scene.falcon_output_name = ""
    return scene


def strips(scene):
    if scene.sequence_editor is None:
        return []
    return list(scene.sequence_editor.strips)


# --- 1 / 2 : PNG 連番 -------------------------------------------------------

out = fresh("png")
scene = setup_scene(out)
scene.falcon_vse_share_output = True
bpy.ops.render.render(animation=True)

got = strips(scene)
check("png/ストリップが1本", len(got) == 1, "n=%d" % len(got))
if got:
    strip = got[0]
    check("png/種類が IMAGE", strip.type == 'IMAGE', strip.type)
    check("png/frame_start=1", int(strip.frame_start) == 1, str(strip.frame_start))
    check("png/長さ3", int(strip.frame_final_duration) == 3, str(strip.frame_final_duration))
    src = os.path.normpath(os.path.join(bpy.path.abspath(strip.directory),
                                        strip.elements[0].filename))
    want = os.path.normpath(os.path.join(out, "0001.png"))
    check("png/経路一致", src == want, src if src == want else "%s != %s" % (src, want))
    check("png/名前が出力ファイル名", strip.name.startswith("0001.png"), strip.name)

# もう一度レンダー -> 増えない(差し替え)
before_channel = got[0].channel if got else None
bpy.ops.render.render(animation=True)
got2 = strips(scene)
check("png/2回目でも1本(差し替え)", len(got2) == 1, "n=%d" % len(got2))
if got2 and before_channel is not None:
    check("png/段を引き継ぐ", int(got2[0].channel) == int(before_channel),
          "%s -> %s" % (before_channel, got2[0].channel))

# --- 3 : FFMPEG mp4 ---------------------------------------------------------

out = fresh("mp4")
scene = setup_scene(out, movie=True)
scene.falcon_vse_share_output = True
bpy.ops.render.render(animation=True)
got = strips(scene)
check("mp4/ストリップが1本", len(got) == 1, "n=%d" % len(got))
if got:
    strip = got[0]
    check("mp4/種類が MOVIE", strip.type == 'MOVIE', strip.type)
    check("mp4/長さ3", int(strip.frame_final_duration) == 3,
          str(strip.frame_final_duration))
    src = os.path.normpath(bpy.path.abspath(strip.filepath))
    want = os.path.normpath(os.path.join(out, "0001-0003.mp4"))
    check("mp4/経路一致", src == want, src if src == want else "%s != %s" % (src, want))

# --- 4 : チェック OFF -------------------------------------------------------

out = fresh("off")
scene = setup_scene(out)
scene.falcon_vse_share_output = False
bpy.ops.render.render(animation=True)
check("off/何も足さない", len(strips(scene)) == 0, "n=%d" % len(strips(scene)))
check("off/ファイル自体は出来ている",
      os.path.isfile(os.path.join(out, "0001.png")))

# --- 4b : F12 の静止画(ディスクに書かない)---------------------------------

out = fresh("still")
scene = setup_scene(out)
scene.falcon_vse_share_output = True
bpy.ops.render.render(write_still=False)
check("still/F12 は何も足さない", len(strips(scene)) == 0, "n=%d" % len(strips(scene)))
check("still/ディスクにも出ていない", not os.path.isfile(os.path.join(out, "0001.png")))


# --- 5 : 「VSE で編集」オペレータ ------------------------------------------
# 共有を切ったままレンダーし、ボタンだけで足せることを見る。

out = fresh("op")
scene = setup_scene(out)
scene.falcon_vse_share_output = False
bpy.ops.render.render(animation=True)
check("op/押す前は空", len(strips(scene)) == 0, "n=%d" % len(strips(scene)))

try:
    result = bpy.ops.falcon.vse_edit()
    ok = result == {'FINISHED'}
    detail = str(result)
except Exception as ex:  # noqa: BLE001
    ok = False
    detail = "例外 %r" % (ex,)
check("op/VSE で編集 が FINISHED", ok, detail)
got = strips(scene)
check("op/ストリップが1本足された", len(got) == 1, "n=%d" % len(got))
if got:
    check("op/選択されている", got[0].select is True)
    check("op/プレビュー範囲が合っている",
          scene.use_preview_range
          and int(scene.frame_preview_start) == int(got[0].frame_final_start)
          and int(scene.frame_preview_end) == int(got[0].frame_final_end) - 1,
          "%s..%s" % (scene.frame_preview_start, scene.frame_preview_end))

# VSE が読むシーン(Blender 5.x)。-b では window が無いので関数を直接見る。
class _FakeWorkspace:
    sequencer_scene = None


_ws = _FakeWorkspace()
_m.set_sequencer_scene(_ws, scene)
check("op/workspace.sequencer_scene を指す", _ws.sequencer_scene is scene,
      str(_ws.sequencer_scene))

# 出力先に何も無い時 -> CANCELLED(例外を出さない)
out = fresh("empty")
scene.render.filepath = out
try:
    result = bpy.ops.falcon.vse_edit()
    ok = result == {'CANCELLED'}
    detail = str(result)
except Exception as ex:  # noqa: BLE001
    ok = False
    detail = "例外 %r" % (ex,)
check("op/出力先が空なら CANCELLED", ok, detail)

# --- 6 : 出力名 -------------------------------------------------------------


def render_with_name(sub, name, movie=False, filepath=None, share=False):
    """出力名を入れてレンダーし、(作業先, レンダー前の filepath, プレビュー) を返す。"""
    out = fresh(sub)
    scene = setup_scene(out, movie=movie)
    if filepath is not None:
        scene.render.filepath = filepath
    scene.falcon_vse_share_output = share
    scene.falcon_output_name = name
    before = scene.render.filepath
    preview = _m.preview_path(scene, scene.frame_start)
    bpy.ops.render.render(animation=True)
    return out, before, preview


def made_files(out):
    return sorted(os.listdir(out))


# 6a : PNG 連番
out, before, preview = render_with_name("name_png", "shot01")
scene = bpy.context.scene
got = made_files(out)
want = ["shot010001.png", "shot010002.png", "shot010003.png"]
check("name/連番が出力名で出来る", got == want, str(got))
check("name/レンダー後に filepath が元へ戻っている", scene.render.filepath == before,
      "%s -> %s" % (before, scene.render.filepath))
check("name/プレビューが実物と一致",
      preview == os.path.join(os.path.normpath(out), "shot010001.png"), str(preview))

# 6b : 桁 (`##`)
out, before, preview = render_with_name("name_hash", "shot01_##")
scene = bpy.context.scene
got = made_files(out)
want = ["shot01_01.png", "shot01_02.png", "shot01_03.png"]
check("name/# の数だけ桁が効く", got == want, str(got))
check("name/桁ありでもプレビューが一致",
      preview == os.path.join(os.path.normpath(out), "shot01_01.png"), str(preview))

# 6c : 既に付いているファイル名を置き換える
out = os.path.join(WORK, "name_replace") + os.sep
out, before, preview = render_with_name(
    "name_replace", "shot01", filepath=out + "old_name")
scene = bpy.context.scene
got = made_files(out)
check("name/出力先のファイル名を置き換える",
      got == ["shot010001.png", "shot010002.png", "shot010003.png"], str(got))
check("name/元のファイル名では書かない",
      not any(f.startswith("old_name") for f in got), str(got))
check("name/置き換えても filepath は元へ戻る", scene.render.filepath == before,
      "%s -> %s" % (before, scene.render.filepath))

# 6d : FFMPEG mp4
out, before, preview = render_with_name("name_mp4", "shot01", movie=True)
scene = bpy.context.scene
got = made_files(out)
check("name/mp4 が出力名で出来る", got == ["shot010001-0003.mp4"], str(got))
check("name/mp4 でもプレビューが一致",
      preview == os.path.join(os.path.normpath(out), "shot010001-0003.mp4"),
      str(preview))
check("name/mp4 でも filepath が元へ戻る", scene.render.filepath == before,
      "%s -> %s" % (before, scene.render.filepath))

# 6e : VSE 共有 ON + 出力名
out, before, preview = render_with_name("name_share", "shot01", share=True)
scene = bpy.context.scene
got = strips(scene)
check("name/共有 ON でストリップが1本", len(got) == 1, "n=%d" % len(got))
if got:
    strip = got[0]
    src = os.path.normpath(os.path.join(bpy.path.abspath(strip.directory),
                                        strip.elements[0].filename))
    want = os.path.join(os.path.normpath(out), "shot010001.png")
    check("name/共有した経路が出力名の物", src == want,
          src if src == want else "%s != %s" % (src, want))
    check("name/ストリップ名が出力名の物", strip.name.startswith("shot010001.png"),
          strip.name)
check("name/共有 ON でも filepath が元へ戻る", scene.render.filepath == before,
      "%s -> %s" % (before, scene.render.filepath))

# 6f : 中断(render_cancel)でも戻る
out = fresh("name_cancel")
scene = setup_scene(out)
scene.falcon_output_name = "shot01"
before = scene.render.filepath
_m._on_render_init(scene)
mid = scene.render.filepath
_m._on_render_cancel(scene)
check("name/差し替えが効いている", mid == before + "shot01", mid)
check("name/中断でも filepath が戻る", scene.render.filepath == before,
      "%s -> %s" % (before, scene.render.filepath))

# 6g : 出力名が空なら今までどおり
scene.falcon_output_name = ""
check("name/空なら合成しない", _m.composed_filepath(scene) is None,
      str(_m.composed_filepath(scene)))
check("name/空ならプレビューは frame_path のまま",
      _m.preview_path(scene, scene.frame_start)
      == os.path.normpath(scene.render.frame_path(frame=scene.frame_start)),
      str(_m.preview_path(scene, scene.frame_start)))
_m._on_render_init(scene)
check("name/空なら filepath を触らない", scene.render.filepath == before,
      scene.render.filepath)
_m._on_render_cancel(scene)


# --- 7 : 登録 / 解除 --------------------------------------------------------

try:
    bpy.ops.preferences.addon_disable(module="falcon_vse_bridge")
    off_ok = (not hasattr(bpy.types.Scene, "falcon_vse_share_output")
              and "FALCON_OT_vse_edit" not in dir(bpy.types))
    detail = ""
except Exception as ex:  # noqa: BLE001
    off_ok = False
    detail = "例外 %r" % (ex,)
check("reg/解除が例外ゼロ", off_ok, detail)

try:
    bpy.ops.preferences.addon_enable(module="falcon_vse_bridge")
    on_ok = hasattr(bpy.types.Scene, "falcon_vse_share_output")
    detail = ""
except Exception as ex:  # noqa: BLE001
    on_ok = False
    detail = "例外 %r" % (ex,)
check("reg/登録が例外ゼロ", on_ok, detail)

# 出力名の欄も戻っているか
check("reg/出力名の欄が登録し直せる", hasattr(bpy.types.Scene, "falcon_output_name"))

# 既定で有効になっているか(この起動で一度も enable していない状態から)
if CHECK_DEFAULT_ENABLED:
    check("reg/既定で有効(起動時に読まれている)", WAS_ENABLED,
          "sys.modules に無かった" if not WAS_ENABLED else "")

# --- 8 : .blend に合成後の値を残さない --------------------------------------
# ★いちばん高くつく壊れ方 = レンダーの後に保存して、本人の .blend の出力先が
#   書き換わっていること。門は「保存 -> 開き直して読む」まで見る。

work = fresh("name_blend")
out = os.path.join(work, "shots") + os.sep
os.makedirs(out)
blend = os.path.join(work, "keep.blend")

scene = setup_scene(out)
scene.frame_end = 2
scene.falcon_vse_share_output = False
scene.falcon_output_name = "shot01"
bpy.ops.wm.save_as_mainfile(filepath=blend)
bpy.ops.render.render(animation=True)
bpy.ops.wm.save_mainfile()
bpy.ops.wm.open_mainfile(filepath=blend)

scene = bpy.context.scene
check("blend/保存し直しても出力先が元のまま", scene.render.filepath == out,
      "%s -> %s" % (out, scene.render.filepath))
check("blend/出力名は .blend に残る", scene.falcon_output_name == "shot01",
      repr(scene.falcon_output_name))
check("blend/実物は出力名で出来ている",
      sorted(os.listdir(out)) == ["shot010001.png", "shot010002.png"],
      str(sorted(os.listdir(out))))


# --- 9 : Video Editing の側から見えるか -------------------------------------
# ★2026-09-04 本人「レンダーした連番を VSE に持ってこれない /
#   Video Editing を開いても増えた機能が見当たらない」。
#   配布物にアドオンが入っていなかったのが最大の原因だが、入れても残る穴が2つある:
#
#   (a) 足したストリップは `scene.sequence_editor` に入る。ところが Blender 5.x の
#       Sequencer が読むのは **`workspace.sequencer_scene`** で、ここが空のままだと
#       Video Editing を開いても**空っぽに見える**(ストリップは在るのに)。
#   (b) このアドオンの入口が「出力プロパティ」と「画像エディタのヘッダ」にしか無く、
#       Sequencer 側からは存在がまったく見えなかった。
#
#   壊し門: --break-workspace / --break-menu で、それぞれの門が落ちること。

if "--break-workspace" in ARGS:
    _m.point_empty_workspaces = lambda scene: None

from bl_ui.space_sequencer import SEQUENCER_MT_add as _add_menu  # noqa: E402

if "--break-menu" in ARGS:
    _add_menu.remove(_m._draw_sequencer_add)

_menu_funcs = list(getattr(_add_menu.draw, "_draw_funcs", None) or [])
check("ws/Sequencer の追加メニューに出る", _m._draw_sequencer_add in _menu_funcs,
      "追加された draw は %d 個" % len(_menu_funcs))

# (a) レンダーが終わった後、空のワークスペースがこのシーンを指すか。
_ws_all = [w for w in bpy.data.workspaces if hasattr(w, "sequencer_scene")]
check("ws/ワークスペースが1つ以上ある", len(_ws_all) >= 2, "n=%d" % len(_ws_all))

if len(_ws_all) >= 2:
    # 既に別のシーンを指している所は、本人の選択なので触ってはいけない。
    _other = bpy.data.scenes.get("FalconGateOther") or bpy.data.scenes.new("FalconGateOther")
    _keeper = _ws_all[0]
    _keeper.sequencer_scene = _other
    for _w in _ws_all[1:]:
        _w.sequencer_scene = None

    out = fresh("ws")
    scene = setup_scene(out)
    scene.falcon_vse_share_output = True
    bpy.ops.render.render(animation=True)

    _pointed = [w.name for w in _ws_all if w.sequencer_scene is scene]
    check("ws/レンダー後に空のワークスペースがシーンを指す", len(_pointed) > 0,
          "指した: %s" % (_pointed or "無し"))
    check("ws/既に指している所は触らない", _keeper.sequencer_scene is _other,
          "%s -> %s" % (_other.name, getattr(_keeper.sequencer_scene, "name", None)))
    check("ws/ストリップ自体は足されている", len(strips(scene)) == 1,
          "n=%d" % len(strips(scene)))

# (b) 追加メニューから呼ぶ経路(ワークスペースへ移らない)。
out = fresh("ws_menu")
scene = setup_scene(out)
scene.falcon_vse_share_output = False
bpy.ops.render.render(animation=True)
try:
    result = bpy.ops.falcon.vse_edit(switch_workspace=False)
    ok = result == {'FINISHED'}
    detail = str(result)
except Exception as ex:  # noqa: BLE001
    ok = False
    detail = "例外 %r" % (ex,)
check("ws/追加メニュー経路が FINISHED", ok, detail)
check("ws/追加メニュー経路でストリップが1本", len(strips(scene)) == 1,
      "n=%d" % len(strips(scene)))


# --- 出力プロパティに何を出すか(本人 2026-09-04「VSE に居るのに VSE に共有する
#     項目がなぜかある」)----------------------------------------------------
#
# ★描画そのものは門から見えないので、`output_rows()` に「何を出すか」を寄せてある。
#   ここでは偽の context を渡して、その一覧を読む。

class _FakeArea:
    def __init__(self, area_type):
        self.type = area_type


class _FakeScreen:
    def __init__(self, types):
        self.areas = [_FakeArea(t) for t in types]


class _FakeWorkspace:
    def __init__(self, name):
        self.name = name


class _FakeContext:
    def __init__(self, scene, types, workspace_name="Layout"):
        self.scene = scene
        self.screen = _FakeScreen(types)
        self.workspace = _FakeWorkspace(workspace_name)


def _kinds(rows, kind):
    return [value for k, value in rows if k == kind]


_scene = bpy.context.scene

_rows_plain = _m.output_rows(_FakeContext(_scene, ['VIEW_3D', 'PROPERTIES', 'OUTLINER']))
check("ui/Sequencer が居ない画面では「VSE で編集」が出る",
      "falcon.vse_edit" in _kinds(_rows_plain, "operator"),
      str(_rows_plain))

_rows_vse = _m.output_rows(_FakeContext(_scene, ['SEQUENCE_EDITOR', 'PROPERTIES']))
check("ui/Sequencer が居る画面では「VSE で編集」を出さない",
      "falcon.vse_edit" not in _kinds(_rows_vse, "operator"),
      str(_rows_vse))

_rows_ws = _m.output_rows(
    _FakeContext(_scene, ['VIEW_3D', 'PROPERTIES'], workspace_name="Video Editing"))
check("ui/Video Editing のワークスペースでも出さない",
      "falcon.vse_edit" not in _kinds(_rows_ws, "operator"),
      str(_rows_ws))

# ★ボタンを消しても、共有そのもののつまみは残っていること。
check("ui/共有のチェックは Sequencer 側でも残る",
      "falcon_vse_share_output" in _kinds(_rows_vse, "prop"), str(_rows_vse))
check("ui/出力名の欄も Sequencer 側で残る",
      "falcon_output_name" in _kinds(_rows_vse, "prop"), str(_rows_vse))

# 直近の書き出しの1行(Falcon の版だけ)。ここまでで動画を書き出している。
_HAS_INFO = hasattr(_scene.render, "falcon_last_export_info")
if _HAS_INFO:
    _info = _m.last_export_info(_scene)
    check("ui/直近の書き出しの1行が空でない", bool(_info), repr(_info))
    check("ui/その1行が出力プロパティに並ぶ",
          _info in _kinds(_rows_plain, "label"), repr(_info))

# 壊し門: 1行が空なら label は出ない(出しっぱなしになっていないこと)。
_saved_last_export_info = _m.last_export_info
_m.last_export_info = lambda scene: ""
try:
    _rows_empty = _m.output_rows(_FakeContext(_scene, ['VIEW_3D']))
finally:
    _m.last_export_info = _saved_last_export_info
check("ui/1行が空なら label を出さない",
      len(_kinds(_rows_empty, "label")) == len(_kinds(_rows_plain, "label")) - (1 if _HAS_INFO else 0),
      str(_rows_empty))



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
