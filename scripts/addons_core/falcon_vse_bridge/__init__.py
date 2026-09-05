# SPDX-FileCopyrightText: 2026 Blender Authors
#
# SPDX-License-Identifier: GPL-2.0-or-later

"""レンダーの出力先を、そのまま同じシーンの VSE へ渡す。

- レンダーが終わって出力先にファイルが出来ていたら、Sequencer にストリップとして足す
  (動画 = Movie strip / 連番画像 = Image strip)。
- 「VSE で編集」で、同じ判定をその場で走らせて Video Editing へ移る。
- 「出力名」に名前を入れると、出力先の経路のディレクトリ部分 + その名前へ書く。
  連番の番号と拡張子は Blender の規則どおり。

bpy の公開 API だけで書いてある(素の Blender 5.2 でも動く)。
"""

bl_info = {
    "name": "Falcon VSE Bridge",
    "author": "Falcon Render",
    "version": (1, 0, 0),
    "blender": (5, 2, 0),
    "location": "Properties > Output > Output / Image Editor Header",
    "description": "レンダーの出力先に名前を付けて、そのまま VSE に共有する",
    "category": "Sequencer",
}

import contextlib
import os

import bpy
from bpy.app.handlers import persistent
from bpy.props import BoolProperty, StringProperty
from bpy.types import Operator


# -----------------------------------------------------------------------------
# 出力名
# -----------------------------------------------------------------------------
#
# 「出力名」が入っていたら、レンダーの間だけ `render.filepath` を
#   <出力先のディレクトリ部分> + <出力名>
# に差し替える。番号と拡張子は Blender が今までどおり付ける。
#
# ★差し替えたものは必ず戻す(render_complete / render_cancel / アドオンの解除)。
#   .blend に合成後の値を残さない。

# 差し替える前の `render.filepath`。{シーン名: 元の値}
_saved_filepath = {}

# 画像の拡張子(既に付いていたら「足す」でなく「差し替える」側に回るもの)。
_IMAGE_EXTS = (
    ".png", ".jpg", ".jpeg", ".exr", ".tif", ".tiff", ".tga", ".bmp", ".hdr",
    ".cin", ".dpx", ".jp2", ".j2c", ".webp", ".rgb", ".sgi", ".psd",
)

# 同じ形式の別綴り。Blender は一覧の**どれか**に一致すれば足さない。
_EXT_ALIASES = {
    ".jpg": (".jpeg",),
    ".jpeg": (".jpg",),
    ".tif": (".tiff",),
    ".tiff": (".tif",),
    ".jp2": (".j2c",),
    ".j2c": (".jp2",),
    ".mpg": (".mpeg",),
    ".mpeg": (".mpg",),
    ".dvd": (".vob", ".mpg", ".mpeg"),
    ".ogv": (".ogg",),
    ".ogg": (".ogv",),
}


def output_name(scene):
    """「出力名」。空(あるいは未登録)なら ""。"""
    return (getattr(scene, "falcon_output_name", "") or "").strip()


def _dir_part(path):
    """経路のディレクトリ部分(区切りまで)。無ければ ""。"""
    index = max(path.rfind("/"), path.rfind("\\"))
    return path[:index + 1] if index >= 0 else ""


def composed_filepath(scene):
    """出力名を入れた `render.filepath`。出力名が空なら None。"""
    name = output_name(scene)
    if not name:
        return None
    return _dir_part(scene.render.filepath) + name


# --- 番号と拡張子(BLI_path_frame / do_ensure_image_extension と同じ規則)-----

def _ensure_digits(name, digits):
    """ファイル名の側に `#` が1つも無ければ digits 個足す(ensure_digits)。"""
    base = name[max(name.rfind("/"), name.rfind("\\")) + 1:]
    if "#" in base:
        return name
    return name + "#" * digits


def _hash_span(name):
    """ファイル名の側の**最後の** `#` の並び (start, end)。無ければ None。"""
    start = end = None
    index = 0
    length = len(name)
    while index < length:
        char = name[index]
        if char in "/\\":
            start = end = None
        elif char == "#":
            start = index
            stop = index + 1
            while stop < length and name[stop] == "#":
                stop += 1
            end = stop
            index = stop - 1
        index += 1
    if end is None:
        return None
    return (start, end)


def _path_frame(name, frame, digits=4):
    name = _ensure_digits(name, digits)
    span = _hash_span(name)
    if span is None:
        return name
    start, end = span
    return name[:start] + "%0*d" % (end - start, frame) + name[end:]


def _path_frame_range(name, start_frame, end_frame, digits=4):
    name = _ensure_digits(name, digits)
    span = _hash_span(name)
    if span is None:
        return name
    start, end = span
    width = end - start
    return (name[:start]
            + "%0*d-%0*d" % (width, start_frame, width, end_frame)
            + name[end:])


def _has_ext(name, ext):
    if not ext:
        return True
    lower = name.lower()
    if lower.endswith(ext.lower()):
        return True
    return any(lower.endswith(alias) for alias in _EXT_ALIASES.get(ext.lower(), ()))


def _with_ext(name, ext):
    """拡張子を足す。既に別の画像の拡張子が付いていたら差し替える。"""
    if _has_ext(name, ext):
        return name
    lower = name.lower()
    for known in _IMAGE_EXTS:
        if lower.endswith(known):
            return name[:len(name) - len(known)] + ext
    return name + ext


def preview_path(scene, frame=None):
    """実際に書かれる経路。出力名が空なら `render.frame_path()` そのまま。

    ★描画中は `render.filepath` を書けないので、`frame_path()` の結果から
      ディレクトリ(テンプレート展開・絶対化まで済んでいる)だけ借りて、
      ファイル名の側を同じ規則で自分で組む。門で実物と突き合わせてある。
    """
    render = scene.render
    if frame is None:
        frame = int(scene.frame_start)
    try:
        real = render.frame_path(frame=frame)
    except Exception:
        return None
    real = os.path.normpath(real)

    name = output_name(scene)
    if not name:
        return real

    directory = os.path.dirname(real)
    use_ext = bool(render.use_file_extension)

    if render.is_movie_format:
        # ★`render.file_extension` は動画では静止画側の拡張子を返す(実測)。
        #   容れ物の拡張子は `frame_path()` の結果から取る。
        ext = os.path.splitext(os.path.basename(real))[1] if use_ext else ""
        base = name
        if use_ext:
            if not _has_ext(base, ext):
                base = _path_frame_range(
                    base, int(scene.frame_start), int(scene.frame_end))
                base += ext
        elif _hash_span(base) is not None:
            base = _path_frame_range(
                base, int(scene.frame_start), int(scene.frame_end))
    else:
        ext = render.file_extension or ""
        base = _path_frame(name, int(frame))
        if use_ext:
            base = _with_ext(base, ext)

    return os.path.normpath(os.path.join(directory, base))


# --- 差し替えと、戻し ---------------------------------------------------------

def apply_output_name(scene):
    """`render.filepath` を出力名を入れた値へ差し替える。差し替えたら True。"""
    if scene is None:
        return False
    composed = composed_filepath(scene)
    if composed is None:
        return False
    if scene.name in _saved_filepath:
        # 既に掛かっている(まだ戻していない)。二重に掛けない。
        return False
    original = scene.render.filepath
    _saved_filepath[scene.name] = original
    try:
        scene.render.filepath = composed
    except Exception as ex:  # noqa: BLE001
        _saved_filepath.pop(scene.name, None)
        print("falcon_vse_bridge:", ex)
        return False
    return True


def restore_output_name(scene):
    """差し替えた `render.filepath` を元へ戻す。"""
    if scene is None:
        return
    original = _saved_filepath.pop(scene.name, None)
    if original is None:
        return
    try:
        scene.render.filepath = original
    except Exception as ex:  # noqa: BLE001
        print("falcon_vse_bridge:", ex)


@contextlib.contextmanager
def output_name_applied(scene):
    """この中だけ `render.filepath` を出力名を入れた値にする。"""
    mine = apply_output_name(scene)
    try:
        yield
    finally:
        if mine:
            restore_output_name(scene)


# -----------------------------------------------------------------------------
# 出力先の判定
# -----------------------------------------------------------------------------

# レンダー中にディスクへ書かれたコマ数。{シーン名: 本数}
#
# ★コマ番号は取らない。Falcon の非同期保存では render_write が1コマ遅れて呼ばれ、
#   その時点の `frame_current` は既に次のコマを指している(素の Blender では一致する)。
#   「書かれたか」だけを見て、どのコマかは出力先の実物で決める。
_written = {}


def _abspath(path):
    try:
        return os.path.normpath(bpy.path.abspath(path))
    except Exception:
        return os.path.normpath(path)


def _frame_path(render, frame):
    """そのコマの出力ファイル。取れなければ None。"""
    try:
        return _abspath(render.frame_path(frame=frame))
    except Exception:
        return None


def output_target(scene):
    """出力先に「出来ている物」を返す。

    戻り値 = (kind, paths, frame_start) / 何も無ければ None。
      kind = 'MOVIE' なら paths は動画1本、'IMAGE' なら連番のファイル列。
    """
    rd = scene.render

    if rd.is_movie_format:
        # 動画は 1 本。名前にコマ範囲が入る(0001-0003.mp4)。
        path = _frame_path(rd, scene.frame_start)
        if path and os.path.isfile(path):
            return ('MOVIE', [path], int(scene.frame_start))
        return None

    # どのコマが出来ているかは、実物を見て決める。
    frames = range(int(scene.frame_start), int(scene.frame_end) + 1)

    paths = []
    first = None
    for f in frames:
        path = _frame_path(rd, f)
        if path and os.path.isfile(path):
            if first is None:
                first = f
            paths.append(path)
    if not paths:
        return None
    return ('IMAGE', paths, int(first))


# -----------------------------------------------------------------------------
# ストリップを足す / 差し替える
# -----------------------------------------------------------------------------

def _strip_source(strip):
    """ストリップが指しているファイル(先頭)の絶対パス。"""
    try:
        if strip.type == 'MOVIE':
            return _abspath(strip.filepath)
        if strip.type == 'IMAGE':
            elements = strip.elements
            if len(elements) == 0:
                return None
            return os.path.normpath(
                os.path.join(_abspath(strip.directory), elements[0].filename)
            )
    except Exception:
        pass
    return None


def _free_channel(sequence_editor):
    channel = 1
    for strip in sequence_editor.strips_all:
        channel = max(channel, int(strip.channel) + 1)
    return channel


def share_output(scene, target=None):
    """出力先の物を Sequencer へ足す。足した(あるいは差し替えた)ストリップを返す。

    ★`target` は「レンダーが終わった時点」で判定した物を渡せる。GUI では
      Sequencer へ足すのが1拍あと = その時には `render.filepath` を既に
      元へ戻しているので、判定をやり直すと出力名の分を見失う。
    """
    if target is None:
        target = output_target(scene)
    if target is None:
        return None
    kind, paths, frame_start = target

    sequence_editor = scene.sequence_editor
    if sequence_editor is None:
        sequence_editor = scene.sequence_editor_create()

    # 同じ経路の物が既にあれば取り除く(二重に増やさない)。段は引き継ぐ。
    channel = None
    for strip in list(sequence_editor.strips):
        if _strip_source(strip) == paths[0]:
            if channel is None:
                channel = int(strip.channel)
            else:
                channel = min(channel, int(strip.channel))
            sequence_editor.strips.remove(strip)
    if channel is None:
        channel = _free_channel(sequence_editor)

    name = os.path.basename(paths[0])
    if kind == 'MOVIE':
        strip = sequence_editor.strips.new_movie(
            name=name, filepath=paths[0], channel=channel, frame_start=frame_start,
        )
    else:
        strip = sequence_editor.strips.new_image(
            name=name, filepath=paths[0], channel=channel, frame_start=frame_start,
        )
        for path in paths[1:]:
            strip.elements.append(os.path.basename(path))
    return strip


# -----------------------------------------------------------------------------
# レンダーが終わった時
# -----------------------------------------------------------------------------

# レンダーが終わって、まだ Sequencer へ足していないシーン。
_pending = []


def flush_pending():
    """溜まっている分を Sequencer へ足す(タイマーから1拍おいて呼ばれる)。"""
    while _pending:
        scene_name, target = _pending.pop(0)
        scene = bpy.data.scenes.get(scene_name)
        if scene is None:
            continue
        try:
            share_output(scene, target)
            # ★足しただけでは Video Editing を開いても何も出ない。
            #   Blender 5.x の Sequencer は `workspace.sequencer_scene` を見るので、
            #   まだ何も指していないワークスペースにこのシーンを指しておく。
            #   (既に別のシーンを指している所は本人の選択なので触らない)
            point_empty_workspaces(scene)
        except Exception as ex:  # noqa: BLE001  UI を止めない
            print("falcon_vse_bridge:", ex)
    return None


def point_empty_workspaces(scene):
    """`sequencer_scene` がまだ空のワークスペースを、このシーンへ向ける。

    ★レンダーが終わって自動で足した時に要る。ストリップは `scene.sequence_editor`
      に入るが、Sequencer が読むのは `workspace.sequencer_scene` なので、
      ここが空のままだと **Video Editing を開いても空っぽに見える**。
    """
    if scene is None:
        return
    for workspace in bpy.data.workspaces:
        if not hasattr(workspace, "sequencer_scene"):
            continue
        if workspace.sequencer_scene is None:
            set_sequencer_scene(workspace, scene)


@persistent
def _on_render_init(scene, *args):
    if scene is None:
        return
    _written[scene.name] = 0
    # ★ここは `RE_RenderAnim` が `scene->r` を複製する前に呼ばれる
    #   (「so user can alter the render settings prior to copying」)。
    apply_output_name(scene)


@persistent
def _on_render_write(scene, *args):
    if scene is None:
        return
    _written[scene.name] = _written.get(scene.name, 0) + 1


@persistent
def _on_render_complete(scene, *args):
    if scene is None:
        return
    try:
        _render_complete(scene)
    finally:
        # ★何があっても `render.filepath` は元へ戻す。
        restore_output_name(scene)


def _render_complete(scene):
    written = _written.pop(scene.name, 0)
    if not written:
        # ディスクに何も書かれていない(F12 の静止画など)。
        return
    if not getattr(scene, "falcon_vse_share_output", False):
        return
    if not scene.render.save_output:
        return
    # ★判定は「戻す前」に済ませる(出力名の分は今の filepath にしか無い)。
    _pending.append((scene.name, output_target(scene)))
    if bpy.app.background:
        # -b では タイマーが回らない。止める UI も無いのでその場で足す。
        flush_pending()
    elif not bpy.app.timers.is_registered(flush_pending):
        # レンダー中の状態を触らないよう1拍おく。
        bpy.app.timers.register(flush_pending, first_interval=0.0)


@persistent
def _on_render_cancel(scene, *args):
    if scene is None:
        return
    _written.pop(scene.name, None)
    restore_output_name(scene)


# -----------------------------------------------------------------------------
# 「VSE で編集」
# -----------------------------------------------------------------------------

def _sequencer_area(window):
    screen = window.screen
    for area in screen.areas:
        if area.type == 'SEQUENCE_EDITOR':
            return area
    # 無ければ、いちばん広い所を Sequencer にする。
    candidates = [a for a in screen.areas if a.type != 'PROPERTIES']
    if not candidates:
        candidates = list(screen.areas)
    if not candidates:
        return None
    area = max(candidates, key=lambda a: a.width * a.height)
    area.type = 'SEQUENCE_EDITOR'
    space = area.spaces.active
    if hasattr(space, "view_type"):
        space.view_type = 'SEQUENCER'
    return area


def set_sequencer_scene(workspace, scene):
    """VSE が読むシーンをワークスペースに指す。

    ★Blender 5.x では Sequencer は `workspace.sequencer_scene` を見る。
      ここが空だと、Sequencer を開いてもストリップは出ず操作もできない
      (`ED_operator_sequencer_active` が通らない)。
    """
    if workspace is None or scene is None:
        return
    if getattr(workspace, "sequencer_scene", None) is not scene:
        try:
            workspace.sequencer_scene = scene
        except Exception:
            pass


def _go_to_vse(context):
    window = getattr(context, "window", None)
    if window is None:
        return
    scene = context.scene
    workspace = bpy.data.workspaces.get("Video Editing")
    if workspace is not None:
        window.workspace = workspace
    else:
        _sequencer_area(window)
        workspace = window.workspace
    set_sequencer_scene(workspace, scene)


class FALCON_OT_vse_edit(Operator):
    """出力先に出来た物を Sequencer へ足して、そこへ移る"""
    bl_idname = "falcon.vse_edit"
    bl_label = "VSE で編集"
    bl_options = {'REGISTER', 'UNDO'}

    # Sequencer の「追加」から呼ぶ時は、既にそこに居るので移らない。
    switch_workspace: BoolProperty(
        name="VSE へ移る",
        description="Video Editing のワークスペースへ移る",
        default=True,
        options={'HIDDEN', 'SKIP_SAVE'},
    )

    def execute(self, context):
        scene = context.scene
        with output_name_applied(scene):
            target = output_target(scene)
        if target is None:
            self.report({'WARNING'}, "出力先にファイルがありません")
            return {'CANCELLED'}

        strip = share_output(scene, target)
        if strip is None:
            return {'CANCELLED'}

        sequence_editor = scene.sequence_editor
        for other in sequence_editor.strips_all:
            other.select = False
        strip.select = True
        sequence_editor.active_strip = strip

        scene.use_preview_range = True
        scene.frame_preview_start = int(strip.frame_final_start)
        scene.frame_preview_end = max(
            int(strip.frame_final_start), int(strip.frame_final_end) - 1
        )

        if self.switch_workspace:
            _go_to_vse(context)
        else:
            # 移らない時も、今の所が読むシーンだけは合わせる。
            set_sequencer_scene(getattr(context, "workspace", None), scene)
        return {'FINISHED'}


# -----------------------------------------------------------------------------
# UI
# -----------------------------------------------------------------------------

def in_sequencer_context(context):
    """今の画面に既に Sequencer が居るか。

    ★居るなら「VSE で編集」の**ボタンは出さない** — 本人 (2026-09-04)
      「VSE に居るのに VSE に共有する項目がなぜかある」。
      移り先が今居る所と同じなので、押しても何も起きないように見える。
      足す口そのものは Sequencer の「追加 > レンダー出力」に在る。
    """
    screen = getattr(context, "screen", None)
    if screen is not None:
        for area in screen.areas:
            if area.type == 'SEQUENCE_EDITOR':
                return True
    workspace = getattr(context, "workspace", None)
    if workspace is not None and workspace.name == "Video Editing":
        return True
    return False


def last_export_info(scene):
    """直近の書き出しで何が効いたか(RNA が C++ 側から読む1行)。

    素の Blender には無いので、その時は空。
    """
    render = getattr(scene, "render", None)
    if render is None:
        return ""
    return (getattr(render, "falcon_last_export_info", "") or "").strip()


def output_rows(context):
    """`_draw_output` が出す物の一覧。

    ★描画そのものは門から見えないので、**出す物の決め方をここ1箇所に寄せる**。
      `("prop", 名前)` / `("label", 文)` / `("separator", None)` /
      `("operator", bl_idname)`。
    """
    scene = context.scene
    rows = [("prop", "falcon_output_name")]

    path = preview_path(scene)
    if path:
        rows.append(("label", path))

    rows.append(("separator", None))
    rows.append(("prop", "falcon_vse_share_output"))

    info = last_export_info(scene)
    if info:
        rows.append(("label", info))

    if not in_sequencer_context(context):
        rows.append(("operator", "falcon.vse_edit"))
    return rows


def _draw_output(self, context):
    layout = self.layout
    layout.separator()
    layout.use_property_split = False
    column = layout.column()
    scene = context.scene
    for kind, value in output_rows(context):
        if kind == "prop":
            column.prop(scene, value)
        elif kind == "label":
            row = column.row()
            row.active = False
            row.label(text=value)
        elif kind == "separator":
            column.separator()
        elif kind == "operator":
            column.operator(value, icon='SEQUENCE')


def _draw_sequencer_add(self, context):
    """Sequencer の「追加」に、レンダー出力を足す口を出す。

    ★これが無いと、Video Editing のワークスペースを開いた側からは
      このアドオンの存在がまったく見えない(出力プロパティと画像エディタの
      ヘッダにしか出ていなかった)。
    """
    layout = self.layout
    layout.separator()
    props = layout.operator(
        "falcon.vse_edit", text="レンダー出力", icon='RENDER_RESULT',
    )
    props.switch_workspace = False


def _draw_image_header(self, context):
    space = context.space_data
    image = getattr(space, "image", None)
    if image is None or image.type != 'RENDER_RESULT':
        return
    self.layout.operator("falcon.vse_edit", icon='SEQUENCE')


# -----------------------------------------------------------------------------
# 登録
# -----------------------------------------------------------------------------

_handlers = (
    ("render_init", _on_render_init),
    ("render_write", _on_render_write),
    ("render_complete", _on_render_complete),
    ("render_cancel", _on_render_cancel),
)

classes = (
    FALCON_OT_vse_edit,
)


def register():
    for cls in classes:
        bpy.utils.register_class(cls)

    bpy.types.Scene.falcon_output_name = StringProperty(
        name="出力名",
        description=(
            "レンダーで書き出すファイルの名前。"
            "空なら出力先の経路をそのまま使う"
        ),
        default="",
    )

    bpy.types.Scene.falcon_vse_share_output = BoolProperty(
        name="出力先を VSE に共有",
        description="レンダーが終わったら、出力先に出来た物を Sequencer へ足す",
        default=True,
    )

    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function not in handler:
            handler.append(function)

    from bl_ui.properties_output import RENDER_PT_output
    RENDER_PT_output.append(_draw_output)
    from bl_ui.space_image import IMAGE_HT_header
    IMAGE_HT_header.append(_draw_image_header)
    from bl_ui.space_sequencer import SEQUENCER_MT_add
    SEQUENCER_MT_add.append(_draw_sequencer_add)


def unregister():
    from bl_ui.space_sequencer import SEQUENCER_MT_add
    SEQUENCER_MT_add.remove(_draw_sequencer_add)
    from bl_ui.space_image import IMAGE_HT_header
    IMAGE_HT_header.remove(_draw_image_header)
    from bl_ui.properties_output import RENDER_PT_output
    RENDER_PT_output.remove(_draw_output)

    for name, function in _handlers:
        handler = getattr(bpy.app.handlers, name)
        if function in handler:
            handler.remove(function)

    if bpy.app.timers.is_registered(flush_pending):
        bpy.app.timers.unregister(flush_pending)
    _written.clear()
    _pending.clear()

    # ★差し替えたままアドオンを外されても、.blend に残さない。
    for scene_name in list(_saved_filepath):
        restore_output_name(bpy.data.scenes.get(scene_name))
    _saved_filepath.clear()

    del bpy.types.Scene.falcon_vse_share_output
    del bpy.types.Scene.falcon_output_name

    for cls in reversed(classes):
        bpy.utils.unregister_class(cls)
