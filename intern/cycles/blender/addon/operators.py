# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import random

import bpy
from bpy.types import Operator
from bpy.props import BoolProperty, StringProperty
from bpy.app.handlers import persistent

from bpy.app.translations import pgettext_tip as tip_


# Photon-caustics "add" state lives in the process environment, not the .blend,
# so it survives File > New / Open and bleeds a previous scene's baked caustics
# into the next file (and the "合成:有効" UI reads the same stale env). Clear it
# whenever a file loads; the user re-bakes per scene.
_FALCON_PHOTON_ADD_ENV = (
    "FALCON_PHOTON_MODE", "FALCON_SHARC_CACHE", "FALCON_SHARC_CELL",
    "FALCON_PHOTON_POINTS", "FALCON_PHOTON_RADIUS_M", "FALCON_PHOTON_NORMAL_DEG",
    "FALCON_PHOTON_GAIN", "FALCON_DISPERSION_B",
)


@persistent
def _falcon_reset_photon_state(*_args):
    import os
    for k in _FALCON_PHOTON_ADD_ENV:
        os.environ.pop(k, None)


def _falcon_flatten_name(name):
    """Turn an ID name into something that can only ever be one path component.

    Blender lets a .blend carry a scene called "../../etc/x", and these names go
    straight into the cache file name. Replacing the separators (and NUL) leaves
    ordinary names untouched -- only names that already produced an unusable
    path change, so no existing cache is orphaned.
    """
    import os
    out = name or ""
    for ch in ("/", "\\", os.sep, os.altsep or "/", "\x00"):
        out = out.replace(ch, "_")
    return out


def _falcon_photon_cache_paths(scene):
    """(grid cache, point cache) keyed by file *and* scene so distinct scenes —
    and any two unsaved files (both basename '') — never share a cache file.

    These live under the user cache directory, not the system temp dir. The grid
    cache is a fixed 1 GB per scene, and on this setup /tmp is a tmpfs: nine
    baked scenes filled 16 GB of RAM and the next bake died with "no space left"
    rather than anything that pointed at the cache. Disk-backed is also the right
    place for something that is meant to survive between sessions.

    The key is flattened first. Blender ID names may contain '/', so a .blend
    whose scene is called "../../x" used to build a path that resolves outside
    the cache directory. Measured 2026-08-22: the escape is real at the path
    level (realpath lands outside ~/.cache/falcon_photon) but the write does
    *not* land there -- the leading component "falcon_photon_<file>__.." is not
    an existing directory and nothing here creates it, so the bake just dies
    with ENOENT. Flattening keeps it that way for good: add one os.makedirs on
    the dirname later and the unflattened version would have become a real
    out-of-cache 1 GB write.
    """
    import os
    base = _falcon_flatten_name(bpy.path.basename(bpy.data.filepath)) or "scene"
    key = "%s__%s" % (base, _falcon_flatten_name(scene.name))
    cache_dir = os.path.join(
        os.environ.get("XDG_CACHE_HOME") or os.path.expanduser("~/.cache"),
        "falcon_photon")
    try:
        os.makedirs(cache_dir, exist_ok=True)
    except OSError:
        import tempfile
        cache_dir = tempfile.gettempdir()
    cache_path = os.path.join(cache_dir, "falcon_photon_%s.bin" % key)
    return cache_path, cache_path[:-4] + ".fph"


class CYCLES_OT_use_shading_nodes(Operator):
    """Enable nodes on a light"""
    bl_idname = "cycles.use_shading_nodes"
    bl_label = "Use Nodes"

    @classmethod
    def poll(cls, context):
        return getattr(context, "light", False)

    def execute(self, context):
        if context.light:
            context.light.use_nodes = True

        return {'FINISHED'}


class CYCLES_OT_denoise_animation(Operator):
    "Denoise rendered animation sequence using current scene and view " \
        "layer settings. Requires denoising data passes and output to " \
        "OpenEXR multilayer files"
    bl_idname = "cycles.denoise_animation"
    bl_label = "Denoise Animation"

    input_filepath: StringProperty(
        name='Input Filepath',
        description='File path for image to denoise. If not specified, uses the render file path and frame range from the scene',
        default='',
        subtype='FILE_PATH')

    output_filepath: StringProperty(
        name='Output Filepath',
        description='If not specified, renders will be denoised in-place',
        default='',
        subtype='FILE_PATH')

    def execute(self, context):
        import os

        preferences = context.preferences
        scene = context.scene
        view_layer = context.view_layer

        in_filepath = self.input_filepath
        out_filepath = self.output_filepath

        in_filepaths = []
        out_filepaths = []

        if in_filepath != '':
            # Denoise a single file
            if out_filepath == '':
                out_filepath = in_filepath

            in_filepaths.append(in_filepath)
            out_filepaths.append(out_filepath)
        else:
            # Denoise animation sequence with expanded frames matching
            # Blender render output file naming.
            in_filepath = scene.render.filepath
            if out_filepath == '':
                out_filepath = in_filepath

            # Backup since we will overwrite the scene path temporarily
            original_filepath = scene.render.filepath

            for frame in range(scene.frame_start, scene.frame_end + 1):
                scene.render.filepath = in_filepath
                filepath = scene.render.frame_path(frame=frame)
                in_filepaths.append(filepath)

                if not os.path.isfile(filepath):
                    scene.render.filepath = original_filepath
                    err_msg = tip_("Frame '%s' not found, animation must be complete") % filepath
                    self.report({'ERROR'}, err_msg)
                    return {'CANCELLED'}

                scene.render.filepath = out_filepath
                filepath = scene.render.frame_path(frame=frame)
                out_filepaths.append(filepath)

            scene.render.filepath = original_filepath

        # Run denoiser
        # TODO: support cancel and progress reports.
        import _cycles
        try:
            _cycles.denoise(preferences.as_pointer(),
                            scene.as_pointer(),
                            view_layer.as_pointer(),
                            input=in_filepaths,
                            output=out_filepaths)
        except Exception as e:
            self.report({'ERROR'}, str(e))
            return {'FINISHED'}

        self.report({'INFO'}, "Denoising completed")
        return {'FINISHED'}


class CYCLES_OT_merge_images(Operator):
    "Combine OpenEXR multi-layer images rendered with different sample " \
        "ranges into one image with reduced noise"
    bl_idname = "cycles.merge_images"
    bl_label = "Merge Images"

    input_filepath1: StringProperty(
        name='Input Filepath',
        description='File path for image to merge',
        default='',
        subtype='FILE_PATH')

    input_filepath2: StringProperty(
        name='Input Filepath',
        description='File path for image to merge',
        default='',
        subtype='FILE_PATH')

    output_filepath: StringProperty(
        name='Output Filepath',
        description='File path for merged image',
        default='',
        subtype='FILE_PATH')

    def execute(self, context):
        in_filepaths = [self.input_filepath1, self.input_filepath2]
        out_filepath = self.output_filepath

        import _cycles
        try:
            _cycles.merge(input=in_filepaths, output=out_filepath)
        except Exception as e:
            self.report({'ERROR'}, str(e))
            return {'FINISHED'}

        return {'FINISHED'}


class CYCLES_OT_falcon_near_realtime(Operator):
    """ビューポートを低sppプレビュー向けに設定: OIDN GPUデノイズをサンプル1から適用。実測でnear-realtimeの最大レバー(path guidingは低sppで無効)"""
    bl_idname = "cycles.falcon_near_realtime"
    bl_label = "Apply Near-Realtime Viewport"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        cscene = context.scene.cycles

        # NOTE: the render device is intentionally left alone. Choosing GPU is
        # the standard "Render Properties > Device > GPU Compute" control; this
        # button only touches denoising so the two are not managed in two places.

        # Viewport (preview) denoising: OIDN on the GPU, from sample 1.
        cscene.use_preview_denoising = True
        cscene.preview_denoiser = 'OPENIMAGEDENOISE'
        cscene.preview_denoising_use_gpu = True
        cscene.preview_denoising_start_sample = 1
        cscene.preview_denoising_input_passes = 'RGB_ALBEDO_NORMAL'
        cscene.preview_denoising_quality = 'FAST'
        cscene.use_preview_adaptive_sampling = True

        # Final render denoiser on the GPU too (OIDN CPU is the slow path).
        cscene.use_denoising = True
        cscene.denoiser = 'OPENIMAGEDENOISE'
        cscene.denoising_use_gpu = True

        self.report({'INFO'}, "CyclesF: OIDN GPU viewport denoising enabled")
        return {'FINISHED'}


class CYCLES_OT_falcon_final_quality(Operator):
    """アニメーション最終レンダー設定: OIDN GPU(accurate/high)+ノイズしきい値0.1。240フレームはしきい値を240回払うので0.1とし、安定性はTemporalフィルタで回収(実測: 0.01は7.9倍の時間でOIDN下では見えない7%改善)。SHARCはOFFに"""
    bl_idname = "cycles.falcon_final_quality"
    bl_label = "Apply Final Quality"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        cscene = context.scene.cycles

        # NOTE: the render device is intentionally left alone, same as the
        # near-realtime preset. With OptiX selected in Preferences the final
        # render uses OptiX and the viewport self-downgrades to CUDA.

        # Final denoise: OIDN on the GPU with the quality options up.
        cscene.use_denoising = True
        cscene.denoiser = 'OPENIMAGEDENOISE'
        cscene.denoising_use_gpu = True
        cscene.denoising_input_passes = 'RGB_ALBEDO_NORMAL'
        cscene.denoising_prefilter = 'ACCURATE'
        cscene.denoising_quality = 'HIGH'

        # Adaptive sampling. 0.1, not Blender's final default 0.01: measured on
        # backroom.blend f499 (2026-07-02), 0.01 costs 7.9x render time for ~7%
        # RMSE under OIDN — the denoiser floor makes the extra samples invisible.
        # Lower it by hand for the rare shot that needs it.
        cscene.use_adaptive_sampling = True
        cscene.adaptive_threshold = 0.1

        # SHARC blending under OIDN makes final frames worse at every alpha
        # (cell bias survives the denoiser), so the final preset forces it off.
        cscene.falcon_sharc_mode = 'OFF'

        self.report({'INFO'}, "CyclesF: final quality preset applied (OIDN GPU)")
        return {'FINISHED'}


class CYCLES_OT_falcon_still_quality(Operator):
    """静止画最終レンダー設定: OIDN GPU(accurate/high)+ノイズしきい値0.01・上限1024。1枚ならタイトなしきい値が払える(backroom級FHDで1.5〜2分)。こだわりの一枚は0.005へ手動調整。SHARCはOFFに"""
    bl_idname = "cycles.falcon_still_quality"
    bl_label = "Apply Still Quality"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        cscene = context.scene.cycles

        cscene.use_denoising = True
        cscene.denoiser = 'OPENIMAGEDENOISE'
        cscene.denoising_use_gpu = True
        cscene.denoising_input_passes = 'RGB_ALBEDO_NORMAL'
        cscene.denoising_prefilter = 'ACCURATE'
        cscene.denoising_quality = 'HIGH'

        cscene.use_adaptive_sampling = True
        cscene.adaptive_threshold = 0.01
        cscene.samples = 1024

        cscene.falcon_sharc_mode = 'OFF'

        self.report({'INFO'}, "CyclesF: still quality preset applied (thr 0.01, cap 1024)")
        return {'FINISHED'}


# Falcon Photon multi-light merge helpers. Each light bakes to its own file
# (baking twice into one live device buffer crashed the CUDA queue); these
# concatenate the per-light outputs afterward. Per-light point caps are split
# so the concatenation never exceeds the user cap -- no giant intermediate,
# no subsampling.
_FPH_MAGIC = 0x46504831  # 'FPH1'


def _falcon_merge_points(files, out_path):
    """Concatenate per-light .fph point files into one. Format:
    {uint32 magic, uint32 count} + count*9 float32 (pos3, flux3, normal3)."""
    import struct
    import numpy as np
    chunks = []
    for fp in files:
        try:
            with open(fp, "rb") as f:
                magic, cnt = struct.unpack("II", f.read(8))
                if magic != _FPH_MAGIC or cnt == 0:
                    continue
                chunks.append(np.fromfile(f, dtype=np.float32, count=cnt * 9))
        except OSError:
            continue
    total = sum(len(c) for c in chunks) // 9
    with open(out_path, "wb") as f:
        f.write(struct.pack("II", _FPH_MAGIC, total))
        for c in chunks:
            c.tofile(f)
    return total


def _falcon_merge_grid(files, out_path):
    """Sum per-light grid caches (4M cells x [R,G,B,tag]). RGB accumulates; the
    tag is position-deterministic so max() keeps the valid tag for shared cells
    and picks up singly-written cells. Grid mode is the point-map-off fallback."""
    import numpy as np
    acc = None
    for fp in files:
        try:
            a = np.fromfile(fp, dtype=np.float32)
        except OSError:
            continue
        if a.size == 0:
            continue
        a = a.reshape(-1, 4)
        if acc is None:
            acc = a.copy()
        else:
            acc[:, :3] += a[:, :3]
            np.maximum(acc[:, 3], a[:, 3], out=acc[:, 3])
    if acc is not None:
        acc.tofile(out_path)
        return True
    return False


def _falcon_specular_mat(mat, rough_max=0.2):
    """True if the material is refractive/metallic — a photon-worthy
    specular caster in the bake's sense.

    Roughness is part of the test. A Glossy node on its own is not evidence
    of a caustic caster: pavillon_barcelone is a pre-Principled file whose
    floors, walls and marble are all Diffuse+Glossy mixes, so treating every
    BSDF_GLOSSY as specular tagged 78 of its 90 meshes and inflated the sun
    photon target sphere to a 110m radius. With the gate it is 14 meshes and
    24m -- the glass and the polished stone, which is what a caster means
    here. (Measured 2026-08-02.)

    A roughness input driven by a texture counts as not-specular: a surface
    whose roughness varies across it is not the mirror or lens that focuses
    light into a caustic.
    """
    if mat is None or not mat.use_nodes:
        return False

    def smooth(sock):
        return sock is not None and not sock.is_linked and sock.default_value < rough_max

    for n in mat.node_tree.nodes:
        if n.type == 'BSDF_PRINCIPLED':
            if (n.inputs['Transmission Weight'].default_value > 0.5
                    or (n.inputs['Metallic'].default_value > 0.5
                        and smooth(n.inputs.get('Roughness')))):
                return True
        elif n.type in ('BSDF_GLASS', 'BSDF_REFRACTION'):
            return True
        elif n.type == 'BSDF_GLOSSY':
            if smooth(n.inputs.get('Roughness')):
                return True
    return False


def _falcon_range_wants_perframe(scene):
    """True if a photon-relevant ID (specular caster mesh, light — or one of
    their parents) is animated, so a one-shot bake would freeze its caustics.
    Camera-only animation stays False: the world-space cache is view-free."""
    def _animated(id_):
        ad = getattr(id_, "animation_data", None)
        return bool(ad and (ad.action or ad.drivers or ad.nla_tracks))

    for ob in scene.objects:
        if ob.hide_render:
            continue
        if ob.type == 'LIGHT':
            pass
        elif ob.type == 'MESH' and ob.material_slots:
            if not _falcon_specular_mat(ob.material_slots[0].material):
                continue
        else:
            continue
        if _animated(ob) or _animated(ob.data):
            return True
        parent = ob.parent
        while parent is not None:
            if _animated(parent):
                return True
            parent = parent.parent
    return False


def _falcon_caster_sphere(scene):
    """Bounding sphere over the objects that can actually cast a caustic
    (refractive/metallic first material). Returns (center, radius) or None."""
    from mathutils import Vector
    corners = []
    for ob in scene.objects:
        if ob.type != 'MESH' or ob.hide_render or not ob.material_slots:
            continue
        if _falcon_specular_mat(ob.material_slots[0].material):
            corners += [ob.matrix_world @ Vector(c) for c in ob.bound_box]
    if not corners:
        return None
    cx = sum(c.x for c in corners) / len(corners)
    cy = sum(c.y for c in corners) / len(corners)
    cz = sum(c.z for c in corners) / len(corners)
    rad = max(((c.x - cx) ** 2 + (c.y - cy) ** 2 + (c.z - cz) ** 2) ** 0.5
              for c in corners) + 0.05
    return (Vector((cx, cy, cz)), rad)


def _falcon_sun_target(scene):
    """FALCON_PHOTON_TARGET string, or None if the scene has no caster."""
    sph = _falcon_caster_sphere(scene)
    if sph is None:
        return None
    c, rad = sph
    return "%.4f,%.4f,%.4f,%.4f" % (c.x, c.y, c.z, rad)


def _falcon_light_reach(light_ob, sphere):
    """How much of this lamp's emission can even arrive at the casters, as a
    fraction of what it emits (1.0 = all of it).

    ★★★2026-08-15 これが無いと**明るい灯が予算を持って行く**。classroom は
    窓の外の補助光(1963W)が光子の**97%**を取り、その光子は室内にある唯一の
    ガラス箱にほとんど届かない——焼き339秒の中身がそれだった。ワット数は
    「部屋の明るさ」であって「集光の作りやすさ」ではない。

    見込み角で割る: ランプから見たキャスター外接球の立体角 ÷ そのランプが
    配る立体角。太陽は既に発射窓をキャスターへ向けているので 1.0。
    球の中にランプが居る(囲まれている)場合も 1.0。
    """
    import math
    if sphere is None:
        return 1.0
    kind = light_ob.data.type
    if kind == 'SUN':
        return 1.0            # 発射窓がキャスターを狙っている(FALCON_PHOTON_TARGET)
    center, rad = sphere
    d = (light_ob.matrix_world.translation - center).length
    if d <= rad:
        return 1.0
    sin_half = min(1.0, rad / d)
    cos_half = math.sqrt(max(0.0, 1.0 - sin_half * sin_half))
    omega = 2.0 * math.pi * (1.0 - cos_half)
    if kind == 'SPOT':
        half = max(1e-4, light_ob.data.spot_size * 0.5)
        full = 2.0 * math.pi * (1.0 - math.cos(min(half, math.pi)))
    elif kind == 'AREA':
        full = 2.0 * math.pi   # 面光源は片側の半球へ出す
    else:
        full = 4.0 * math.pi   # 点光源は全球
    return min(1.0, omega / max(full, 1e-6))


def _falcon_world_radiance(scene):
    """Uniform world radiance (gray average) for the LT world photon pass, or
    0.0 when there is nothing to emit (no world, black background, or a
    textured/HDRI background the uniform emitter cannot represent)."""
    w = scene.world
    if w is None:
        return 0.0
    if not w.use_nodes:
        c = w.color
        return (c[0] + c[1] + c[2]) / 3.0
    out = w.node_tree.get_output_node('CYCLES')
    if out is None or not out.inputs['Surface'].is_linked:
        return 0.0
    bg = out.inputs['Surface'].links[0].from_node
    if bg.type != 'BACKGROUND':
        return 0.0
    if bg.inputs['Color'].is_linked or bg.inputs['Strength'].is_linked:
        return 0.0
    c = bg.inputs['Color'].default_value
    return bg.inputs['Strength'].default_value * (c[0] + c[1] + c[2]) / 3.0


def _falcon_transmissive_objects(scene):
    """Mesh objects whose material can transmit a camera ray (glass/refraction/
    Principled transmission). During the LT pass these are tagged caustics
    caster so the kernel's camera-connection visibility ray passes through
    their bodies instead of killing every splat seen through the glass."""
    out = []
    for ob in scene.objects:
        if ob.type != 'MESH' or ob.hide_render:
            continue
        trans = False
        for slot in ob.material_slots:
            mat = slot.material
            if mat is None or not mat.use_nodes:
                continue
            for n in mat.node_tree.nodes:
                if n.type in ('BSDF_GLASS', 'BSDF_REFRACTION', 'BSDF_TRANSPARENT'):
                    trans = True
                elif n.type == 'BSDF_PRINCIPLED':
                    sock = n.inputs['Transmission Weight']
                    if sock.is_linked or sock.default_value > 0.0:
                        trans = True
        if trans:
            out.append(ob)
    return out


def _falcon_lt_flood_risk(scene):
    """Cheap heuristic for the LT 'flood' failure mode (returns a short warning
    string or None; never blocks). The LT camera-connection pass passes glass
    bodies straight through (LuxCore's caustics-caster approximation). When a
    lamp is embedded near the *base* of a glass object that sits right on a big
    diffuse receiver, that receiver looks almost straight at the light through
    only a thin slab of glass -> the near-unattenuated light splats across the
    whole surface and the caustic layer floods (the lt_fan_test v1 case).
    A lamp partway *up* inside the glass (v2) refracts through slanted walls and
    is safe, so we only warn on the low/embedded geometry."""
    from mathutils import Vector

    def _wbox(ob):
        wc = [ob.matrix_world @ Vector(c) for c in ob.bound_box]
        return (min(c.x for c in wc), max(c.x for c in wc),
                min(c.y for c in wc), max(c.y for c in wc),
                min(c.z for c in wc), max(c.z for c in wc))

    glass = []
    for ob in scene.objects:
        if ob.type != 'MESH' or ob.hide_render or not ob.material_slots:
            continue
        if _falcon_specular_mat(ob.material_slots[0].material):
            glass.append((ob, _wbox(ob)))
    if not glass:
        return None
    # large diffuse receivers = big flat meshes that are not specular casters
    receivers = []
    for ob in scene.objects:
        if ob.type != 'MESH' or ob.hide_render:
            continue
        if ob.material_slots and _falcon_specular_mat(ob.material_slots[0].material):
            continue
        x0, x1, y0, y1, z0, z1 = _wbox(ob)
        if max(x1 - x0, y1 - y0) >= 4.0:
            receivers.append(z1)  # top surface height
    if not receivers:
        return None
    lights = [o for o in scene.objects if o.type == 'LIGHT' and not o.hide_render]
    for li in lights:
        p = li.matrix_world.translation
        for ob, (x0, x1, y0, y1, z0, z1) in glass:
            if not (x0 - 1e-4 <= p.x <= x1 + 1e-4 and y0 - 1e-4 <= p.y <= y1 + 1e-4):
                continue  # light not over/inside this glass footprint
            frac = (p.z - z0) / max(z1 - z0, 1e-4)  # 0 = base, 1 = top
            if frac > 0.25:
                continue  # mid/high inside the glass -> walls refract (safe)
            for rz1 in receivers:
                if rz1 <= p.z + 1e-3 and (p.z - rz1) <= 0.6:
                    return ("LT氾濫の危険: ライト「%s」がガラス「%s」の底部に埋込 → "
                            "大きな拡散面がガラス越しにライトを直視 (中腹へ上げると改善)"
                            % (li.name, ob.name))
    return None


def _falcon_scene_has_caustics(scene):
    """AUTO detection: does this scene have both a photon-worthy specular caster
    (glass / refraction / Principled transmission / smooth metal, via the first
    material slot) AND a light that can emit photons? Cheap scan (first slot
    only) so it is safe to call from panel draw. No caster or no light => no
    caustics to make, so the AUTO UI stays silent and no cost is paid."""
    has_light = any(
        o.type == 'LIGHT' and not o.hide_render
        and getattr(o.data, "type", None) in ('SUN', 'AREA', 'SPOT', 'POINT')
        for o in scene.objects)
    if not has_light:
        return False
    for ob in scene.objects:
        if ob.type == 'MESH' and not ob.hide_render and ob.material_slots:
            if _falcon_specular_mat(ob.material_slots[0].material):
                return True
    return False


def _falcon_caustics_active():
    """True when a photon bake has been composited into the beauty render this
    session (env set by falcon_photon_bake, cleared on file load / clear)."""
    import os
    return os.environ.get("FALCON_PHOTON_MODE") == "add"


class CYCLES_OT_falcon_auto_caustics(Operator):
    """ガラス+ライトのコースティクス(集光模様)を、実証済みの推奨設定で焼く。
    レシピ設定は不要。完了後は通常のレンダー(F12/ビューポート)に自動で合成される。
    強さはあとから焼き直し無しで調整できる。"""
    bl_idname = "cycles.falcon_auto_caustics"
    bl_label = "コースティクスを出す"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        cscene = context.scene.cycles
        if not _falcon_scene_has_caustics(context.scene):
            self.report({'ERROR'},
                        "ガラス/屈折マテリアルとライト(SUN/AREA/SPOT/POINT)が要ります")
            return {'CANCELLED'}
        # Guarantee only the delivery mechanism: GPU point-map is the path that
        # composites in-kernel into the normal render (the Octane-like result).
        # Leave radius / gain / maxpts / dispersion to the property defaults or
        # the user's own values -- those are artistic knobs, not correctness.
        cscene.falcon_photon_gpu = True
        cscene.falcon_photon_point = True
        return bpy.ops.cycles.falcon_photon_bake()


def _falcon_photon_auto_radius(context, pixels=1.0, samples=512):
    """Pick the point-map lookup radius by measuring how big a pixel is out where
    the caustics land.

    A density estimate is blurry over its lookup radius, so the radius states how
    much detail is being given up. In metres that is unanswerable without knowing
    the shot -- 1cm is fine on a tabletop and invisible across a room -- which is
    why the manual value had to be retuned per scene. In pixels it has one answer:
    keep the blur under what the frame can resolve and nothing else matters.

    So: shoot camera rays, carry them through glass and mirrors the way the
    photons will travel, and at the first surface that can actually receive a
    caustic, ask how wide one pixel is in metres there. The average is the radius.

    This is LuxCore's Film2SceneRadius machinery (film2sceneradius.cpp) aimed at a
    different target. LuxCore only ever runs it for the indirect cache and the
    visibility particles, at 2% of the frame -- and says outright that the caustic
    radius is far smaller than that ("Caustic radius is too small for visibility
    check", photongicache.cpp:418); its caustic radius stays a hand-set number.
    Measured here, 2% of the frame is 38 pixels, and the radius this scene was
    hand-tuned to over months lands at 0.8 pixels. A pixel is the target that
    reproduces hand tuning, and it makes rendering the same shot at 4K sharpen the
    caustics on its own instead of needing the number retyped.

    Returns None when there is no camera or nothing was hit, so the caller keeps
    whatever the user had.
    """
    from . import falcon_photon

    scene = context.scene
    cam = scene.camera
    if cam is None or cam.type != 'CAMERA':
        return None

    depsgraph = context.evaluated_depsgraph_get()
    try:
        frame = cam.data.view_frame(scene=scene)
    except Exception:
        return None
    # view_frame gives the corners at unit depth in camera space, so directions
    # to any point on the frame come from lerping them -- and the separation
    # between two such directions IS the beam spread per metre travelled.
    top_right, bottom_right, bottom_left, top_left = frame
    rot = cam.matrix_world.to_3x3()
    origin = cam.matrix_world.translation

    def direction(u, v):
        top = top_left.lerp(top_right, u)
        bottom = bottom_left.lerp(bottom_right, u)
        return (rot @ bottom.lerp(top, v)).normalized()

    # The render resolution is what "one pixel" means, so the same scene baked
    # for a 4K delivery gets a smaller radius than one baked for a preview.
    pct = scene.render.resolution_percentage / 100.0
    res_x = max(int(scene.render.resolution_x * pct), 1)
    res_y = max(int(scene.render.resolution_y * pct), 1)
    du = pixels / res_x
    dv = pixels / res_y

    rng = random.Random(0x5EED)          # same radius every bake of the same shot
    accumulated = 0.0
    count = 0
    for _ in range(samples):
        u = rng.random()
        v = rng.random()
        d = direction(u, v)
        # One pixel, as a world-space width per metre of travel.
        spread = max((direction(min(u + du, 1.0), v) - d).length,
                     (direction(u, min(v + dv, 1.0)) - d).length)
        if spread <= 0.0:
            continue

        pos = origin
        travelled = 0.0
        for _bounce in range(8):
            hit, location, _normal, _index, obj, _matrix = scene.ray_cast(depsgraph, pos, d)
            if not hit:
                break
            travelled += (location - pos).length
            if falcon_photon.classify(obj)[0] == 'DIFFUSE':
                # A surface that can hold a caustic: this is the distance that
                # decides how wide the estimate is allowed to be.
                accumulated += spread * travelled
                count += 1
                break
            # Glass and mirrors are where caustics come from, not where they
            # land. Carry on through them; the extra path length is the point,
            # since a caustic seen through a window is further away than the
            # window. Straight through is close enough for a 2% quantity -- the
            # bend does not change the distance much.
            pos = location + d * 1e-4
        else:
            continue

    if count == 0:
        return None
    return accumulated / count


class CYCLES_OT_falcon_photon_bake(Operator):
    """このシーンでフォトントレースを実行し、コースティクス(ガラス/水/鏡の集光)を"""     """キャッシュに焼く。完了後のレンダーに自動で合成される(加算・レンダー時間ほぼ増なし)。"""     """対象: Principledの透過(ガラス/水)・滑らかな金属。光源は最初のライト1灯(SUN対応)。"""     """注意: 水/ガラスの「透明の影」をOFFにすると物理的に正しい合成になる"""
    bl_idname = "cycles.falcon_photon_bake"
    bl_label = "コースティクスを焼く"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        # Stability guard (2026-07-12): a live RENDERED 3D viewport runs its own
        # Cycles session that reads the same process-global FALCON_* envs. During
        # the bake it would fire the photon bake pass too and fight the offline
        # bake over the shared falcon_photon_points device buffer -- the realloc
        # under OIDN raised a CUDA illegal-address and crashed Blender. Drop every
        # RENDERED viewport to SOLID for the duration of the bake, and restore it
        # afterwards (by then MODE is 'add', a read-only lookup that is safe live).
        saved_rendered = []
        wm = context.window_manager
        if wm is not None:
            for win in wm.windows:
                screen = win.screen
                if screen is None:
                    continue
                for area in screen.areas:
                    if area.type != 'VIEW_3D':
                        continue
                    for space in area.spaces:
                        if space.type == 'VIEW_3D' and space.shading.type == 'RENDERED':
                            saved_rendered.append(space)
                            space.shading.type = 'SOLID'
        try:
            return self._bake_impl(context)
        finally:
            for space in saved_rendered:
                try:
                    space.shading.type = 'RENDERED'
                except Exception:
                    pass

    def _bake_impl(self, context):
        import os
        import time
        from . import falcon_photon

        scene = context.scene
        cscene = scene.cycles
        if scene.render.engine != 'CYCLES':
            # EEVEE trap (FALCON_PHOTON.md): a non-Cycles engine renders the
            # photon frame as a normal EEVEE frame — no deposits, stale cache,
            # and it looks exactly like a kernel regression. Hard-fail instead.
            self.report({'ERROR'}, "レンダーエンジンがCYCLESではありません")
            return {'CANCELLED'}
        cache_path, points_path = _falcon_photon_cache_paths(scene)

        # Delivery guarantee: only the GPU point-map path composites into the
        # normal render. With either toggle off this operator used to "succeed"
        # while nothing appeared (and stock caustics still got disabled below).
        # Which of the two stores gets used is the scene's own setting now; the
        # environment only overrides it for A/B (FALCON_PHOTON_GRID=1 /
        # FALCON_PHOTON_POINT=1).
        cscene.falcon_photon_gpu = True
        if os.environ.get("FALCON_PHOTON_GRID") == "1":
            cscene.falcon_photon_point = False
        elif os.environ.get("FALCON_PHOTON_POINT") == "1":
            cscene.falcon_photon_point = True

        # Measure the radius before baking, while the camera is where the user
        # left it. It is a lookup-time value, so this does not constrain the
        # bake -- it just means the number is chosen from the shot instead of
        # typed in metres.
        if cscene.falcon_photon_point_radius_auto:
            measured = _falcon_photon_auto_radius(
                context, pixels=cscene.falcon_photon_point_radius_px)
            if measured is None:
                self.report({'WARNING'},
                            "半径の自動測定に失敗(カメラが無い/何にも当たらない)。"
                            "%.3fm をそのまま使います" % cscene.falcon_photon_point_radius)
            else:
                measured = min(max(measured, 0.001), 0.5)
                self.report({'INFO'}, "半径を自動決定: %.4fm (%.1f画素相当)"
                            % (measured, cscene.falcon_photon_point_radius_px))
                cscene.falcon_photon_point_radius = measured
                # The accumulation grid wants its cell at the same size and for
                # the same reason: one pixel's footprint is where quantization
                # stops being visible. Same measurement, second consumer.
                if not cscene.falcon_photon_point:
                    cscene.falcon_photon_cell = measured

        disp = cscene.falcon_photon_dispersion
        use_points = cscene.falcon_photon_point and cscene.falcon_photon_gpu
        t0 = time.time()
        if cscene.falcon_photon_gpu:
            # Multi-light bake: each visible light bakes INDEPENDENTLY to its own
            # output file (baking a second light into the same live device buffer
            # crashed the CUDA queue -- "Illegal address in
            # integrator_sorted_paths_array"), then the per-light files are
            # merged below. The photon budget and the point cap are both split by
            # light energy, so the merged point count never exceeds the cap and
            # no giant intermediate/subsample is needed. hide_render isolates one
            # light per pass to match integrator.cpp's "first enabled light" pick.
            lights = [o for o in scene.objects if o.type == 'LIGHT' and not o.hide_render]
            if not lights:
                self.report({'ERROR'}, "ライトが見つかりません(1灯必要)")
                return {'CANCELLED'}
            # ★★★2026-08-15 予算は「ワット数 × 届く割合」で配る(→ [_falcon_light_reach])。
            #   ワット数だけで割ると、集光に関係のない明るい灯が全部持って行く
            #   (classroom: 窓の外の補助光が97%=9.7億発を取り、焼き339秒の大半が
            #   ガラスに当たらない光子だった)。
            # ★★分母も同じ重みで割る=**取り上げた分は届く灯へ回り、総数は変わらない**。
            #   最初は分母をワット数のままにして総数を減らしたが、それだと届く割合が
            #   中くらいの灯で**粒が72%増えた**(`reachglass`台・1.7%で 0.735 -> 1.267)。
            #   「光子数」の設定は**実際に撃つ数**であるべきで、シーンによって黙って
            #   減らしてはいけない。速くなるのは「安い灯へ移る」ぶんだけにする。
            # A/B用の逃げ道: FALCON_PHOTON_NO_REACH=1 でワット数だけの旧配分に戻す。
            caster_sphere = None if os.environ.get("FALCON_PHOTON_NO_REACH") else \
                _falcon_caster_sphere(scene)
            reach = {li.name: _falcon_light_reach(li, caster_sphere) for li in lights}
            for li in lights:
                print("SHOOT_INFO reach %-20s %-6s %.4f" % (li.name, li.data.type,
                                                            reach[li.name]))
            total_energy = sum(max(li.data.energy, 1e-6) * reach[li.name] for li in lights)
            if total_energy <= 0.0:
                total_energy = sum(max(li.data.energy, 1e-6) for li in lights)
                reach = {li.name: 1.0 for li in lights}
            saved_hide = {li.name: li.hide_render for li in lights}
            single = len(lights) == 1

            r = scene.render
            saved = (r.resolution_x, r.resolution_y, r.resolution_percentage,
                     cscene.samples, cscene.use_adaptive_sampling,
                     cscene.max_bounces, cscene.transmission_bounces,
                     cscene.glossy_bounces)
            # Cell size / deposit radius / dispersion ride on the scene
            # properties now (integrator sockets), so nothing to export here.
            # Only the per-pass state below still travels by environment.
            os.environ["FALCON_PHOTON_MODE"] = "bake"

            per_light_cache = []
            per_light_pts = []
            try:
                for i, light in enumerate(lights):
                    for other in lights:
                        other.hide_render = (other is not light)

                    weight = (max(light.data.energy, 1e-6) / total_energy) * reach[light.name]
                    n_photons = max(10000, round(cscene.falcon_photon_photons * weight))
                    # res capped at 4096 so the frame stays ONE tile (a bigger
                    # frame gets tiled and per-tile buffer re-uploads wipe the
                    # device deposits); extra photons come from extra samples.
                    res = max(64, min(4096, int(round(n_photons ** 0.5))))
                    samples = max(1, round(n_photons / (res * res)))
                    os.environ["FALCON_PHOTON_N"] = str(res * res * samples)

                    # per-light output files (single light => write straight to
                    # the final paths, skipping the merge/copy)
                    #
                    # The grid does not use them: it accumulates through the
                    # file instead, every pass writing and re-reading the one
                    # cache. Summing separately baked grids cannot survive
                    # collision probing (falcon_sharc.h) -- a site takes the
                    # first free slot on its chain, so two independent passes
                    # put it in different slots and an element-wise sum mixes
                    # strangers: 253 dark specks on ocean at shipping
                    # resolution, in a frame that had 0.
                    li_cache = cache_path if (single or not use_points) else (
                        cache_path + ".L%d" % i)
                    li_pts = points_path if single else (points_path + ".L%d" % i)
                    os.environ["FALCON_SHARC_CACHE"] = li_cache
                    if use_points or i == 0:
                        os.environ.pop("FALCON_PHOTON_ACCUM", None)
                    else:
                        os.environ["FALCON_PHOTON_ACCUM"] = "1"
                    if use_points:
                        os.environ["FALCON_PHOTON_POINTS"] = li_pts
                        # cap split by energy so the merged total <= user cap
                        li_cap = max(100000, int(cscene.falcon_photon_point_maxpts * weight))
                        os.environ["FALCON_PHOTON_MAXPTS"] = str(li_cap)
                    else:
                        os.environ.pop("FALCON_PHOTON_POINTS", None)

                    # Sun bakes: aim the launch square at the specular targets
                    # instead of the whole scene footprint (a big floor eats
                    # 99.9% of photons as direct hits that never deposit).
                    # Recomputed per light (direction-dependent).
                    os.environ.pop("FALCON_PHOTON_TARGET", None)
                    if light.data.type == 'SUN':
                        tgt = _falcon_sun_target(scene)
                        if tgt:
                            os.environ["FALCON_PHOTON_TARGET"] = tgt

                    r.resolution_x = r.resolution_y = res
                    r.resolution_percentage = 100
                    cscene.samples = samples
                    cscene.use_adaptive_sampling = False
                    # grazing photons live through long TIR chains inside glass;
                    # low scene bounce caps kill them mid-flight and eat the far
                    # caustic tails
                    cscene.max_bounces = max(cscene.max_bounces, 32)
                    cscene.transmission_bounces = max(cscene.transmission_bounces, 32)
                    cscene.glossy_bounces = max(cscene.glossy_bounces, 16)
                    bpy.ops.render.render(write_still=False)
                    per_light_cache.append(li_cache)
                    if use_points:
                        per_light_pts.append(li_pts)

                # --- world (sky) photon pass -----------------------------
                # The sky refracts through the same water and glass and lands
                # as a caustic too, and nothing else in the frame carries it:
                # the bake switches Cycles' own caustic paths off, and the lamp
                # passes only shoot lamps. Light tracing has fired this pass
                # since the beginning -- the bake never did, and that gap IS
                # the layer's shortfall. Measured on ocean against a
                # brute-force judge (tools/caustic_bands.py): grid 0.924, LT
                # with its world pass 0.976, LT WITHOUT it 0.922. The two
                # numbers that match are the two without a sky.
                # Budget: one emitter's share, so a scene with a sky pays about
                # one extra lamp's worth of bake time. Off = FALCON_PHOTON_NO_WORLD.
                world_l = (0.0 if os.environ.get("FALCON_PHOTON_NO_WORLD")
                           else _falcon_world_radiance(scene))
                if world_l > 0.0:
                    for other in lights:
                        other.hide_render = True
                    n_photons = max(10000, round(cscene.falcon_photon_photons /
                                                 (len(lights) + 1)))
                    res = max(64, min(4096, int(round(n_photons ** 0.5))))
                    samples = max(1, round(n_photons / (res * res)))
                    os.environ["FALCON_PHOTON_N"] = str(res * res * samples)
                    os.environ["FALCON_PHOTON_WORLD"] = "%.6g" % world_l
                    w_cache = (cache_path + ".W") if use_points else cache_path
                    os.environ["FALCON_SHARC_CACHE"] = w_cache
                    if use_points:
                        w_pts = points_path + ".W"
                        os.environ["FALCON_PHOTON_POINTS"] = w_pts
                    else:
                        os.environ["FALCON_PHOTON_ACCUM"] = "1"
                    tgt = _falcon_sun_target(scene)
                    if tgt:
                        os.environ["FALCON_PHOTON_TARGET"] = tgt
                    else:
                        os.environ.pop("FALCON_PHOTON_TARGET", None)
                    r.resolution_x = r.resolution_y = res
                    r.resolution_percentage = 100
                    cscene.samples = samples
                    bpy.ops.render.render(write_still=False)
                    os.environ.pop("FALCON_PHOTON_WORLD", None)
                    if use_points:
                        per_light_cache.append(w_cache)
                        per_light_pts.append(w_pts)
                        # the point map still merges files, so the merge has to
                        # run even when there was only one lamp
                        single = False
            except Exception as e:
                self.report({'ERROR'}, "GPUフォトンベイク失敗: %s" % e)
                return {'CANCELLED'}
            finally:
                for li in lights:
                    li.hide_render = saved_hide[li.name]
                (r.resolution_x, r.resolution_y, r.resolution_percentage,
                 cscene.samples, cscene.use_adaptive_sampling,
                 cscene.max_bounces, cscene.transmission_bounces,
                 cscene.glossy_bounces) = saved
                os.environ.pop("FALCON_PHOTON_N", None)
                os.environ.pop("FALCON_PHOTON_MAXPTS", None)
                os.environ.pop("FALCON_PHOTON_TARGET", None)
                os.environ.pop("FALCON_PHOTON_ACCUM", None)

            # Merge per-light files into the final cache/points (no-op for a
            # single light -- it already wrote the final paths). Clean up temps.
            if not single and use_points:
                try:
                    _falcon_merge_points(per_light_pts, points_path)
                except Exception as e:
                    self.report({'ERROR'}, "フォトンマージ失敗: %s" % e)
                    return {'CANCELLED'}
                for fp in per_light_cache + per_light_pts:
                    if fp in (cache_path, points_path):
                        continue  # a single lamp wrote the final file itself
                    try:
                        os.remove(fp)
                    except OSError:
                        pass
            os.environ["FALCON_SHARC_CACHE"] = cache_path
        else:
            argv = ["--photons", str(cscene.falcon_photon_photons),
                    "--cell", "%.4f" % cscene.falcon_photon_cell,
                    "--out", cache_path]
            if cscene.falcon_photon_dispersion > 0.0:
                argv += ["--dispersion", "%.4f" % cscene.falcon_photon_dispersion]
            try:
                falcon_photon.main(argv)
            except StopIteration:
                self.report({'ERROR'}, "ライトが見つかりません(1灯必要)")
                return {'CANCELLED'}
            except Exception as e:
                self.report({'ERROR'}, "フォトントレース失敗: %s" % e)
                return {'CANCELLED'}

        os.environ["FALCON_PHOTON_MODE"] = "add"
        # The cache path is computed per bake (per-light files), so it stays a
        # per-pass value. Cell size, lookup radius, normal cone and gain are
        # scene properties the integrator reads directly -- editing a slider
        # now takes effect on the next render without any environment hop.
        os.environ["FALCON_SHARC_CACHE"] = cache_path
        if use_points and os.path.exists(points_path):
            # Point map wins over the grid cache at load.
            os.environ["FALCON_PHOTON_POINTS"] = points_path
        else:
            os.environ.pop("FALCON_PHOTON_POINTS", None)
        # The photon layer now carries the caustic paths exclusively. Kill
        # PT's own caustics: with soft/large lights PT finds the same light
        # itself and the add mode double-counts (audit 2026-07-05: truth-base
        # ~=0, so everything the layer added was a second copy).
        cscene.caustics_reflective = False
        cscene.caustics_refractive = False
        mode_txt = "GPU点マップ" if (use_points and os.path.exists(points_path)) else (
            "GPU" if cscene.falcon_photon_gpu else "CPU")
        self.report({'INFO'}, "コースティクス焼き完了 (%s, %.0f秒) — 次のレンダーから有効"
                    % (mode_txt, time.time() - t0))
        return {'FINISHED'}


class CYCLES_OT_falcon_photon_clear(Operator):
    """フォトンコースティクスの合成を無効にする(キャッシュは残る)"""
    bl_idname = "cycles.falcon_photon_clear"
    bl_label = "コースティクスを無効化"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        import os
        os.environ.pop("FALCON_PHOTON_MODE", None)
        cscene = context.scene.cycles
        cscene.caustics_reflective = True
        cscene.caustics_refractive = True
        self.report({'INFO'}, "フォトンコースティクス無効化")
        return {'FINISHED'}


class CYCLES_OT_falcon_bake_and_render_range(Operator):
    """コースティクスを焼いてフレーム範囲(start..end)を連番レンダーする。"""     """GUIを通さず別プロセス(background)で実行するのでVulkanビューポートの"""     """クラッシュを踏まない。既定=現フレームで1回焼いてキャッシュ使い回し"""     """(静止ガラス/ライト・カメラは動いてOK)。「毎フレーム焼き直し」ONで"""     """動くガラス/ライトにも対応(ベイク時間×フレーム数)。"""     """出力先/形式は通常のアニメ出力設定に従う。ログは出力先の隣に書く。"""
    bl_idname = "cycles.falcon_bake_and_render_range"
    bl_label = "焼いて範囲レンダー (別プロセス)"

    per_frame: BoolProperty(
        name="毎フレーム焼き直し",
        description="ガラス/水/鏡やライトが動くショット用に各フレームでベイクし直す"
                    "(1回焼きの約N倍のベイク時間)。OFF=現フレームで1回だけ焼いて全フレームで使い回す",
        default=False,
    )

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def invoke(self, context, event):
        # Misclick guard: this launches a long external render burst, so it
        # was disabled after accidental clicks kept crashing the old in-GUI
        # version. Always confirm (and pick the mode) in a dialog first.
        # Auto-dispatch (anime C): default the mode from whether a caster or
        # light is actually animated; the checkbox stays user-overridable.
        self.per_frame = _falcon_range_wants_perframe(context.scene)
        return context.window_manager.invoke_props_dialog(self, width=380)

    def draw(self, context):
        scene = context.scene
        cscene = scene.cycles
        col = self.layout.column()
        col.label(text="フレーム %d–%d を別プロセスでレンダーします"
                       % (scene.frame_start, scene.frame_end))
        col.label(text="出力: %s" % (scene.render.filepath or "(未設定)"))
        if _falcon_range_wants_perframe(scene):
            col.label(text="動くガラス/ライトを検出 → 毎フレーム焼きを既定にしました",
                      icon='INFO')
        col.prop(self, "per_frame")
        # Preflight for the two defaults that ruined runs on 2026-07-11:
        # too few photons flicker frame-to-frame, and the CPU bake is ~100x
        # slower than the GPU one (a 48f range turns into hours).
        if self.per_frame and cscene.falcon_photon_photons < 16000000:
            col.label(text="光子%dMは毎フレーム焼きでちらつきます — 64M推奨"
                           % max(1, cscene.falcon_photon_photons // 1000000),
                      icon='ERROR')
            col.prop(cscene, "falcon_photon_photons")
        if not cscene.falcon_photon_gpu:
            col.label(text="GPUベイクがOFFです (CPUは約100倍遅い)", icon='ERROR')
            col.prop(cscene, "falcon_photon_gpu")

    def execute(self, context):
        import os
        import subprocess
        import tempfile
        scene = context.scene
        r = scene.render
        if r.engine != 'CYCLES':
            self.report({'ERROR'}, "レンダーエンジンがCYCLESではありません")
            return {'CANCELLED'}
        if scene.frame_end < scene.frame_start:
            self.report({'ERROR'}, "フレーム範囲が不正です (end < start)")
            return {'CANCELLED'}
        if not r.filepath:
            self.report({'ERROR'}, "アニメ出力先(Output Properties)が未設定です")
            return {'CANCELLED'}

        # Snapshot the current session (unsaved edits included) to a temp
        # copy; the background process renders the copy so the user can keep
        # editing — and a crash there can never take the GUI down with it.
        stem = bpy.path.display_name_from_filepath(bpy.data.filepath) or "scene"
        tmp_blend = os.path.join(tempfile.gettempdir(),
                                 "falcon_range_%s.blend" % stem)
        try:
            bpy.ops.wm.save_as_mainfile(filepath=tmp_blend, copy=True)
        except RuntimeError as e:
            self.report({'ERROR'}, "一時保存に失敗しました: %s" % e)
            return {'CANCELLED'}

        runner = os.path.join(os.path.dirname(__file__), "falcon_range.py")
        out_dir = os.path.dirname(bpy.path.abspath(r.filepath)) or tempfile.gettempdir()
        os.makedirs(out_dir, exist_ok=True)
        log_path = os.path.join(out_dir, "falcon_range_%s.log" % stem)

        # The bake in the child re-derives every FALCON_* env from the scene
        # properties; scrub inherited ones so a live GUI add-mode/knob can't
        # leak a stale map path or gain into the final render.
        env = {k: v for k, v in os.environ.items()
               if not k.startswith("FALCON_")}
        mode = "perframe" if self.per_frame else "once"
        try:
            log_f = open(log_path, "w")
            proc = subprocess.Popen(
                [bpy.app.binary_path, "-b", tmp_blend,
                 "--python", runner, "--", "--mode", mode],
                stdout=log_f, stderr=subprocess.STDOUT,
                start_new_session=True, env=env)
        except OSError as e:
            self.report({'ERROR'}, "起動に失敗しました: %s" % e)
            return {'CANCELLED'}

        self.report({'INFO'},
                    "範囲レンダー開始 (%s, PID %d) — 進捗: %s"
                    % ("毎フレーム焼き直し" if self.per_frame else "1回焼き",
                       proc.pid, log_path))
        return {'FINISHED'}


def _falcon_lt_normconv(layer, radius):
    """Composite-stage replica of the kernel splat blur (falcon_lt_splat):
    per-splat gaussian with sigma = radius/2, footprint clamped to 9x9,
    weights normalized over the in-bounds pixels of each SOURCE pixel.
    Splatting raw (radius 0, gain 1) and blurring the accumulated layer here
    is exactly the same image, because every splat shares one kernel and the
    edge normalization depends only on the source pixel:
        blurred = conv(raw / conv(ones, w), w)
    (separable, zero padding = out-of-bounds contributes nothing). Moving the
    blur here makes it re-tunable without a re-render (falcon_lt_recomposite).
    Verified against a brute-force per-splat simulator, 2026-07-28."""
    import numpy as np
    if radius < 0.5:  # kernel early-out: physical single-pixel splat
        return layer
    R = min(int(np.ceil(radius)), 4)
    k = 2.0 / (radius * radius)  # sigma = radius/2 -> 1/(2 sigma^2)
    v = np.exp(-k * np.arange(-R, R + 1, dtype=np.float64) ** 2)

    def conv1d(img, axis):
        pad = [(0, 0)] * img.ndim
        pad[axis] = (R, R)
        p = np.pad(img, pad)
        out = np.zeros(img.shape, dtype=np.float64)
        for j, wj in enumerate(v):
            sl = [slice(None)] * img.ndim
            sl[axis] = slice(j, j + img.shape[axis])
            out += wj * p[tuple(sl)]
        return out

    h, w = layer.shape[:2]
    wsum = conv1d(conv1d(np.ones((h, w)), 0), 1)
    return conv1d(conv1d(layer / wsum[:, :, None], 0), 1).astype(np.float32)


def _falcon_lt_guide_from_pair(a, b, block=8):
    """Where is the light tracer still noisy? Two independent estimates of the
    same layer disagree exactly where the estimator has not converged, so
    |a - b| / mean is a reference-free noise map. Averaged over blocks because
    per-pixel it is itself mostly noise (2026-07-29: the world-cell version of
    this is the error-field route's guide field; blocks are its screen-space
    stand-in, which needs no Position pass)."""
    import numpy as np

    h, w = a.shape[:2]
    lum = lambda x: 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
    la, lb = lum(a).astype(np.float64), lum(b).astype(np.float64)
    bh, bw = (h + block - 1) // block, (w + block - 1) // block

    def pool(x):
        pad = np.zeros((bh * block, bw * block), dtype=np.float64)
        pad[:h, :w] = x
        return pad.reshape(bh, block, bw, block).sum(axis=(1, 3))

    mean = 0.5 * (pool(la) + pool(lb))
    diff = pool(np.abs(la - lb))
    guide = np.zeros_like(diff)
    floor = np.percentile(mean[mean > 0], 20) if (mean > 0).any() else 0.0
    live = mean > floor
    guide[live] = diff[live] / mean[live]
    return np.repeat(np.repeat(guide, block, axis=0), block, axis=1)[:h, :w]


def _falcon_lt_tile_scores(layers, guide):
    """Score each launch tile by how much of the still-noisy transport it
    carries: sum over pixels of guide * layer. A tile that lands only where the
    layer has already converged scores low even if it carries most of the
    energy -- measured 2026-07-29, weighting by energy instead leaves the
    fringe noise untouched (-0.1%) while this weighting cuts it 15.9%."""
    import numpy as np

    lum = lambda x: 0.2126 * x[..., 0] + 0.7152 * x[..., 1] + 0.0722 * x[..., 2]
    return [float((guide * lum(l)).sum()) for l in layers]


def _falcon_lt_allocate(scores, energies, budget):
    """Split a sample budget over the tiles by score. Tiles that carry no
    transport at all (the probe found nothing) are dropped -- on the probe
    scene that was 5 of 16 tiles, a third of the cone spent on directions that
    miss the caster entirely. Every tile that does carry transport keeps at
    least one sample: dropping it would remove its energy from the layer, which
    is bias, not noise."""
    live = [i for i, e in enumerate(energies) if e > 0.0]
    if not live:
        return {}
    total = sum(scores[i] for i in live)
    if total <= 0.0:
        share = {i: 1.0 / len(live) for i in live}
    else:
        share = {i: scores[i] / total for i in live}
    alloc = {i: max(1, int(round(share[i] * budget))) for i in live}
    while sum(alloc.values()) > budget and any(v > 1 for v in alloc.values()):
        k = max(alloc, key=lambda i: alloc[i])
        alloc[k] -= 1
    return alloc


def _falcon_lt_write_exr(path, layer, w, h):
    """Write an RGB float layer out as the raw pass EXR the composite reads."""
    import numpy as np

    name = "Falcon LT raw"
    if name in bpy.data.images:
        bpy.data.images.remove(bpy.data.images[name])
    img = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    rgba = np.concatenate([layer, np.ones((h, w, 1), dtype=layer.dtype)], axis=2)
    img.pixels[:] = rgba.ravel()
    img.filepath_raw = path
    img.file_format = 'OPEN_EXR'
    img.save()
    bpy.data.images.remove(img)


def _falcon_lt_publish(context, scene, stem, comp, w, h, report):
    """Composite array -> "Falcon LT合成" EXR datablock + color-managed 8-bit
    display PNG pushed into every open image editor. Shared tail of the LT
    render and the recomposite operator. Returns (exr_path, png_path|None)."""
    out_path = stem + "_composite.exr"
    name = "Falcon LT合成"
    if name in bpy.data.images:
        bpy.data.images.remove(bpy.data.images[name])
    out = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
    out.pixels[:] = comp.ravel()
    out.filepath_raw = out_path
    out.file_format = 'OPEN_EXR'
    out.save()

    # Bake the scene's view transform + exposure + look into an 8-bit sRGB PNG
    # via save_render (runs the render color management pipeline on the linear
    # composite), then surface it as the front-and-center result image.
    disp_path = stem + "_composite.png"
    img_set = scene.render.image_settings
    try:
        _fmt = (img_set.file_format, img_set.color_mode, img_set.color_depth)
        img_set.file_format = 'PNG'
        img_set.color_mode = 'RGBA'
        img_set.color_depth = '8'
        try:
            out.save_render(disp_path, scene=scene)
        finally:
            (img_set.file_format, img_set.color_mode, img_set.color_depth) = _fmt
    except Exception as e:
        report({'WARNING'}, "表示画像の書き出し失敗(EXRは保存済): %s" % e)
        return out_path, None

    dname = "Falcon LT合成 (表示)"
    if dname in bpy.data.images:
        bpy.data.images.remove(bpy.data.images[dname])
    disp = bpy.data.images.load(disp_path)
    disp.name = dname
    wm = context.window_manager
    for win in getattr(wm, "windows", []):
        scr = getattr(win, "screen", None)
        if scr is None:
            continue
        for area in scr.areas:
            if area.type == 'IMAGE_EDITOR':
                area.spaces.active.image = disp
    return out_path, disp_path


class CYCLES_OT_falcon_lighttrace_render(Operator):
    """ライトトレース合成レンダー(FQ静止画)。光源から光子を追いカメラへ直接つなぐ"""     """キャッシュ無しコースティクス: サンプル数で本当に収束する(点マップのドット無し)。"""     """各ライトのLTパス+通常レンダーを実行し、加算合成した画像を保存する。"""     """重い: サンプル数=シーンのサンプル数。まず低サンプルで試す。固定サンプリングで実行される"""
    bl_idname = "cycles.falcon_lighttrace_render"
    bl_label = "ライトトレース合成レンダー"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def _guided_pass(self, scene, cscene, r, w, h, spp, n, stem, load_rgba):
        """Guided emission: spend this light's photon budget where the light
        tracer is still noisy instead of spreading it evenly over the emission
        cone. Three stages, every one of them an unbiased estimate of the same
        layer, so all three are kept and averaged by photon count:

          1. probe   -- each of the n*n launch tiles at `probe` samples
          2. control -- the whole cone twice at `probe` samples; the pair is the
                        reference-free noise map (see _falcon_lt_guide_from_pair)
          3. guided  -- what is left of the budget, split over the tiles by how
                        much of the *noisy* transport each one carries

        Measured on the luxcore-vs-CyclesF probe scene at an equal 32-sample
        budget (2026-07-29): relative noise -17.9% against the plain cone-wide
        render. Weighting by energy instead of by noise gives -8.9%, and simply
        dropping the tiles that carry nothing gives -11.7%, so both halves --
        skipping the void and steering the rest -- carry real weight."""
        import os
        import numpy as np
        probe = max(1, spp // (4 * n * n))
        fixed = n * n * probe + 2 * probe
        if fixed >= spp:
            self.report({'WARNING'},
                        "誘導%dx%d はサンプル数%dでは足りません(最低%d)。通常のLTで焼きます"
                        % (n, n, spp, fixed + 1))
            os.environ.pop("FALCON_PHOTON_TILE", None)
            r.filepath = "%s_guided_fallback.exr" % stem
            bpy.ops.render.render(write_still=True)
            return load_rgba(r.filepath)[:, :, :3].astype(np.float64)

        saved = (cscene.samples, cscene.seed)
        tmp = "%s_guidetmp.exr" % stem

        def shoot(tile, samples, seed):
            cscene.samples = samples
            cscene.seed = seed
            os.environ["FALCON_LIGHTTRACE_SAMPLES"] = str(samples)
            os.environ["FALCON_PHOTON_N"] = str(w * h * samples)
            if tile is None:
                os.environ.pop("FALCON_PHOTON_TILE", None)
            else:
                os.environ["FALCON_PHOTON_TILE"] = "%d,%d,%d" % (tile[0], tile[1], n)
            r.filepath = tmp
            bpy.ops.render.render(write_still=True)
            return load_rgba(tmp)[:, :, :3].astype(np.float64)

        try:
            base_seed = saved[1]
            control_a = shoot(None, probe, base_seed)
            control_b = shoot(None, probe, base_seed + 1)
            tiles = [(i, j) for i in range(n) for j in range(n)]
            layers = [shoot(t, probe, base_seed + 2) for t in tiles]

            guide = _falcon_lt_guide_from_pair(control_a, control_b)
            scores = _falcon_lt_tile_scores(layers, guide)
            energies = [float(l.sum()) for l in layers]
            budget = spp - fixed
            alloc = _falcon_lt_allocate(scores, energies, budget)
            print("Falcon LT guided: probe %d spp/tile, %d/%d tiles carry transport, "
                  "%d spp to spend (probe_sum %.4e, control %.4e / %.4e)"
                  % (probe, len(alloc), len(tiles), budget,
                     float(sum(energies)), float(control_a.sum()), float(control_b.sum())))

            probe_sum = layers[0]
            for l in layers[1:]:
                probe_sum = probe_sum + l
            weighted = (n * n * probe) * probe_sum + probe * control_a + probe * control_b
            total_w = n * n * probe + 2 * probe
            if alloc:
                guided_sum = None
                for idx, samples in sorted(alloc.items()):
                    got = shoot(tiles[idx], samples, base_seed + 3)
                    guided_sum = got if guided_sum is None else guided_sum + got
                spent = sum(alloc.values())
                print("Falcon LT guided: guided stage %d spp over %d tiles, sum %.4e"
                      % (spent, len(alloc), float(guided_sum.sum())))
                weighted = weighted + spent * guided_sum
                total_w += spent
            return weighted / float(total_w)
        finally:
            os.environ.pop("FALCON_PHOTON_TILE", None)
            cscene.samples, cscene.seed = saved
            os.environ["FALCON_LIGHTTRACE_SAMPLES"] = str(saved[0])
            os.environ["FALCON_PHOTON_N"] = str(w * h * saved[0])
            for junk in (tmp,):
                if os.path.exists(junk):
                    os.remove(junk)

    def execute(self, context):
        import os
        import time
        import tempfile
        import numpy as np

        scene = context.scene
        cscene = scene.cycles
        r = scene.render
        if r.engine != 'CYCLES':
            self.report({'ERROR'}, "レンダーエンジンがCYCLESではありません")
            return {'CANCELLED'}
        # POINT is here because the photon emitter grew a POINT branch
        # (init_from_camera.h, 2026-08-15) and the flux for it is the generic
        # watts/N this host already computes -- a point lamp radiates its whole
        # power into 4 pi, which is exactly what that expression means. Leaving
        # it out of this list was the only thing stopping bake-free caustics on
        # a point-lit scene.
        lights = [o for o in scene.objects if o.type == 'LIGHT' and not o.hide_render
                  and o.data.type in ('SUN', 'AREA', 'SPOT', 'POINT')]
        if not lights:
            self.report({'ERROR'}, "対応ライトがありません (SUN/AREA/SPOT/POINT)")
            return {'CANCELLED'}
        # Flood-risk heuristic: warn (do not block) when the geometry is the LT
        # 'flood' failure mode (embedded base lamp over a big diffuse plane).
        flood_msg = _falcon_lt_flood_risk(scene)
        if flood_msg:
            self.report({'WARNING'}, flood_msg)
        w = int(r.resolution_x * r.resolution_percentage / 100)
        h = int(r.resolution_y * r.resolution_percentage / 100)
        if max(w, h) > 4096:
            # falcon_lt_splat assumes a single full-frame tile.
            self.report({'ERROR'}, "LTは4096px以下のみ (スプラットが1タイル前提)")
            return {'CANCELLED'}
        spp = cscene.samples

        stem = os.path.join(
            tempfile.gettempdir(),
            "falcon_lt_%s" % (bpy.path.basename(bpy.data.filepath) or "scene"))

        # FALCON_SHARC_CACHE is in this list because leaving it out DESTROYED the
        # user's bake. The LT passes run with FALCON_PHOTON_MODE=bake, and the
        # save hook (path_trace.cpp) writes the device buffer to whatever
        # FALCON_SHARC_CACHE points at whenever the mode is "bake" -- while the
        # LT branch never deposits, so that buffer is still the zeros it was
        # allocated with. After a photon bake the variable is left pointing at
        # the real cache file (see the end of _bake_impl), so running LT in the
        # same session overwrote a good bake with 1 GB of zeros and every later
        # render silently lost its caustics.
        #
        # Reproduced 2026-08-16 in one process (tools/repro_lt_wipes_cache.py in
        # cyclesf-lab): after the bake the cache held 378848 non-zero floats in
        # its first 20M, after the LT run it held 0. The measurement harness
        # could never see this -- it runs one render per process, so the
        # variable never survives to the next step. Only a GUI session (or that
        # script) keeps one process alive across bake and LT.
        env_keys = ("FALCON_PHOTON_MODE", "FALCON_LIGHTTRACE", "FALCON_LIGHTTRACE_SAMPLES",
                    "FALCON_PHOTON_N", "FALCON_LIGHTTRACE_GAIN", "FALCON_LT_SPLAT_RADIUS",
                    "FALCON_LT_VISIBILITY", "FALCON_PHOTON_MAXPTS", "FALCON_PHOTON_TARGET",
                    "FALCON_PHOTON_POINTS", "FALCON_PHOTON_WORLD", "FALCON_SHARC_CACHE")
        saved_env = {k: os.environ.get(k) for k in env_keys}
        saved_hide = {li.name: li.hide_render for li in lights}
        img_set = r.image_settings
        saved = (cscene.use_adaptive_sampling, cscene.max_bounces,
                 cscene.transmission_bounces, cscene.glossy_bounces,
                 r.filepath, img_set.file_format, img_set.color_depth,
                 img_set.color_mode)
        saved_denoise = cscene.use_denoising
        saved_caustics = (cscene.caustics_reflective, cscene.caustics_refractive)
        # The LT and world passes are intermediates: they are written to disk and
        # then ADDED to beauty. bpy.ops.render.render() writes the scene's
        # COMPOSITOR output, not the raw Combined pass, so every compositor node
        # that adds a constant (vignette overlay, glare, gamma, curves) lands in
        # the layer and is counted a SECOND time on top of beauty.
        # Measured on pabellon 2026-08-25: with the shipped 37-node tree the
        # layer covered 99.999 % of the frame at mean 0.11483 instead of a sparse
        # caustic; with compositing off, 0.0998 % at 0.000584 (197x less energy).
        # That pedestal is the whole of the +21 % sky and of the 10.9x "flood"
        # that had been open and unexplained since 2026-08-02. The lab scenes all
        # have ZERO compositor nodes, which is why only production scenes hit it.
        # Beauty keeps the user's compositor -- it is restored just before it.
        saved_comp = (r.use_compositing, r.use_sequencer)
        r.use_compositing = False
        r.use_sequencer = False
        # Glass bodies must not occlude the LT camera-connection ray: the
        # kernel passes SD_OBJECT_CAUSTICS_CASTER objects through (straight-
        # line-through-specular, LuxCore's own approximation). Tag every
        # transmissive object for the LT pass only; beauty gets them back.
        caster_objs = _falcon_transmissive_objects(scene)
        saved_caster = {ob.name: ob.cycles.is_caustics_caster for ob in caster_objs}

        def _restore_casters():
            for ob in caster_objs:
                ob.cycles.is_caustics_caster = saved_caster[ob.name]

        def _load_rgba(path):
            img = bpy.data.images.load(path)
            px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
            bpy.data.images.remove(img)
            return px

        t0 = time.time()
        lt_sum = None
        pass_files = []
        try:
            # --- LT pass, one render per light (integrator emits from the
            # first enabled light; layers add linearly) ---
            for ob in caster_objs:
                ob.cycles.is_caustics_caster = True
            os.environ["FALCON_PHOTON_MODE"] = "bake"
            os.environ["FALCON_LIGHTTRACE"] = "1"
            # The save hook cannot be told "do not save" from here, so it is
            # pointed at a scratch file that is deleted in the finally below.
            # NOT under /tmp: that is tmpfs on this machine and the hook writes
            # the full 1 GB table once per pass.
            lt_scratch = os.path.join(
                os.path.dirname(_falcon_photon_cache_paths(scene)[0]), "_lt_scratch.bin")
            os.environ["FALCON_SHARC_CACHE"] = lt_scratch
            os.environ["FALCON_LIGHTTRACE_SAMPLES"] = str(spp)
            os.environ["FALCON_PHOTON_N"] = str(w * h * spp)
            # Raw physical baseline (gain 1, no splat blur): both knobs are
            # linear over the LT layer -- gain is a pure multiply after the 4x
            # flux clamp, the blur is one shared splat kernel -- so they moved
            # to the composite stage (_falcon_lt_normconv below) where
            # re-tuning them costs seconds, not a re-render.
            # Both are forced here rather than cleared: with the knobs on scene
            # properties now, an unset variable means "use the scene value", and
            # the scene's gain/blur are exactly what this pass must not bake in.
            os.environ["FALCON_LIGHTTRACE_GAIN"] = "1.000"
            os.environ["FALCON_LT_SPLAT_RADIUS"] = "0.0"
            if cscene.falcon_lt_visibility:
                os.environ["FALCON_LT_VISIBILITY"] = "1"
            else:
                os.environ.pop("FALCON_LT_VISIBILITY", None)
            # no point buffer during LT (432 MB VRAM it never reads)
            os.environ["FALCON_PHOTON_MAXPTS"] = "0"
            os.environ.pop("FALCON_PHOTON_POINTS", None)

            cscene.use_adaptive_sampling = False  # SPP cancellation needs fixed
            # Denoising the LT layer eats ~12% of its energy and blurs the
            # fine filaments; only the beauty pass gets the user's setting.
            cscene.use_denoising = False
            cscene.max_bounces = max(cscene.max_bounces, 32)
            cscene.transmission_bounces = max(cscene.transmission_bounces, 32)
            cscene.glossy_bounces = max(cscene.glossy_bounces, 16)
            img_set.file_format = 'OPEN_EXR'
            img_set.color_depth = '32'
            img_set.color_mode = 'RGB'

            # ★発射窓(タイル)は SUN と SPOT にしか実装がない
            #   (scene/integrator.cpp の sun 枝と is_spot 枝だけが
            #    FALCON_PHOTON_TILE を読む)。POINT/AREA では無視されるので、
            #   **各タイルのパスが全球を撒いたフル層になり、それを n^2 枚
            #   足し込む**。2026-08-16 に cupgap64(POINT灯)で実測:
            #     4x4  energy 1.2450 / 誤差 0.4305 / 描画 69.9秒(四角い塊が散る)
            #     オフ energy 0.9855 / 誤差 0.0528 / 描画  4.5秒
            #   出荷の既定はオフなので出荷には出ないが、ホスト側に光源種別の
            #   チェックが無かったのが本体。窓を持てない灯が1つでも混ざる
            #   なら、その灯だけ誘導を切る。
            tiles = int(cscene.falcon_lt_guide_tiles)
            unguided = [li.name for li in lights
                        if li.data.type not in ('SUN', 'SPOT')]
            if tiles > 1 and unguided:
                self.report({'WARNING'},
                            "発射窓は SUN/SPOT のみ対応のため、%s は誘導なしで焼きます"
                            % "/".join(unguided))
            for i, light in enumerate(lights):
                for other in lights:
                    other.hide_render = (other is not light)
                os.environ.pop("FALCON_PHOTON_TARGET", None)
                if light.data.type == 'SUN':
                    tgt = _falcon_sun_target(scene)
                    if tgt:
                        os.environ["FALCON_PHOTON_TARGET"] = tgt
                out_path = "%s_pass%d.exr" % (stem, i)
                can_guide = light.data.type in ('SUN', 'SPOT')
                if tiles > 1 and can_guide:
                    # the guided pass renders to its own scratch file, so the
                    # destination has to be remembered here rather than read
                    # back off r.filepath afterwards
                    layer = self._guided_pass(scene, cscene, r, w, h, spp, tiles,
                                              stem, _load_rgba)
                    _falcon_lt_write_exr(out_path, layer, w, h)
                else:
                    r.filepath = out_path
                    bpy.ops.render.render(write_still=True)
                    layer = _load_rgba(out_path)[:, :, :3]
                pass_files.append(out_path)
                lt_sum = layer if lt_sum is None else (lt_sum + layer)

            # --- world pass: uniform-environment photons (world->glass->
            # shadow embedded caustics; beauty runs caustics-off, so no lamp
            # LT pass nor beauty carries this component otherwise) ---
            world_l = _falcon_world_radiance(scene) if cscene.falcon_lt_world else 0.0
            if world_l > 0.0:
                for other in lights:
                    other.hide_render = True
                tgt = _falcon_sun_target(scene)
                if tgt:
                    os.environ["FALCON_PHOTON_TARGET"] = tgt
                else:
                    os.environ.pop("FALCON_PHOTON_TARGET", None)
                os.environ["FALCON_PHOTON_WORLD"] = "%.6g" % world_l
                r.filepath = stem + "_passworld.exr"
                bpy.ops.render.render(write_still=True)
                os.environ.pop("FALCON_PHOTON_WORLD", None)
                pass_files.append(r.filepath)
                lt_sum = lt_sum + _load_rgba(r.filepath)[:, :, :3]

            # --- beauty pass with the user's own settings/envs restored ---
            _restore_casters()
            for li in lights:
                li.hide_render = saved_hide[li.name]
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            try:
                os.remove(os.path.join(
                    os.path.dirname(_falcon_photon_cache_paths(scene)[0]), "_lt_scratch.bin"))
            except OSError:
                pass
            (cscene.use_adaptive_sampling, cscene.max_bounces,
             cscene.transmission_bounces, cscene.glossy_bounces,
             _, _, _, _) = saved
            cscene.use_denoising = saved_denoise
            # The LT layer carries the caustic paths exclusively; PT finds the
            # same light itself under soft/large lights, so beauty must not
            # (audit 2026-07-05: the layer was a pure second copy otherwise).
            cscene.caustics_reflective = False
            cscene.caustics_refractive = False
            img_set.file_format = 'OPEN_EXR'
            img_set.color_depth = '32'
            img_set.color_mode = 'RGBA'
            # Beauty is what the user actually looks at, so it gets their own
            # compositor back. Only the added LT layer had to stay raw.
            (r.use_compositing, r.use_sequencer) = saved_comp
            r.filepath = stem + "_beauty.exr"
            bpy.ops.render.render(write_still=True)
            beauty = _load_rgba(r.filepath)

            # --- additive composite (LT carries only caustic paths the camera
            # tracer cannot find, same no-double-count argument as photon add).
            # Gain and blur are applied HERE, on the raw LT layer -- exactly
            # equivalent to the old in-kernel path (see _falcon_lt_normconv). ---
            # Measured self-check on the layer we are about to add. A caustic
            # layer is SPARSE: most pixels are exactly zero. If nearly every
            # pixel is lit, the layer is carrying something that is not a
            # caustic (a compositor pedestal, a background write, a leak).
            # _falcon_lt_flood_risk above only predicts from scene properties --
            # a counter that is always broken says nothing when it breaks.
            lit_frac = float(np.count_nonzero(lt_sum[:, :, :3])) / lt_sum[:, :, :3].size
            if lit_frac > 0.95:
                self.report({'WARNING'},
                            "LT層が画面の%.1f%%を覆っています(集光層は疎なはず)。"
                            "コンポジター出力や背景が層に混ざっている可能性があります"
                            % (100.0 * lit_frac))
            comp = beauty.copy()
            comp[:, :, :3] += cscene.falcon_lt_gain * _falcon_lt_normconv(
                lt_sum, cscene.falcon_lt_blur)
            # Manifest so falcon_lt_recomposite can re-tune gain/blur from the
            # raw passes without re-rendering. raw_gain/raw_blur document what
            # is baked into the pass files (must stay 1.0/0.0).
            import json
            with open(stem + "_manifest.json", "w") as f:
                json.dump({"w": w, "h": h, "spp": spp, "raw_gain": 1.0,
                           "raw_blur": 0.0, "passes": pass_files,
                           "beauty": stem + "_beauty.exr"}, f)
            out_path, disp_path = _falcon_lt_publish(
                context, scene, stem, comp, w, h, self.report)
        except Exception as e:
            self.report({'ERROR'}, "LTレンダー失敗: %s" % e)
            return {'CANCELLED'}
        finally:
            _restore_casters()
            for li in lights:
                li.hide_render = saved_hide[li.name]
            for k, v in saved_env.items():
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v
            try:
                os.remove(os.path.join(
                    os.path.dirname(_falcon_photon_cache_paths(scene)[0]), "_lt_scratch.bin"))
            except OSError:
                pass
            (cscene.use_adaptive_sampling, cscene.max_bounces,
             cscene.transmission_bounces, cscene.glossy_bounces,
             r.filepath, img_set.file_format, img_set.color_depth,
             img_set.color_mode) = saved
            cscene.use_denoising = saved_denoise
            (cscene.caustics_reflective, cscene.caustics_refractive) = saved_caustics
            (r.use_compositing, r.use_sequencer) = saved_comp

        self.report({'INFO'}, "LT合成完了 (%d灯%s, %dspp, %.0f秒)%s → 表示画像「Falcon LT合成 (表示)」 %s"
                    % (len(lights), "+ワールド" if world_l > 0.0 else "",
                       spp, time.time() - t0,
                       " ⚠氾濫の危険あり" if flood_msg else "",
                       disp_path if disp_path else out_path))
        return {'FINISHED'}


class CYCLES_OT_falcon_lt_clean_caustics(Operator):
    """清書コースティクス(LT)。数分かかる最終静止画用の一本道。フォトンの点マップと違い、"""     """サンプル数で本当に収束した滑らかな集光(点々なし・段々のファセット構造まで)を出す。"""     """実行後はカラーマネジメント(露出/ビュー変換)適用済みの表示画像として画像エディタに開く。"""     """まず気軽な[コースティクスを出す](フォトン)で構図を決め、仕上げにこちらを使う想定。"""
    bl_idname = "cycles.falcon_lt_clean_caustics"
    bl_label = "清書コースティクス (LT)"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        scene = context.scene
        cscene = scene.cycles
        if not _falcon_scene_has_caustics(scene):
            self.report({'ERROR'},
                        "ガラス/屈折マテリアルとライト(SUN/AREA/SPOT)が要ります")
            return {'CANCELLED'}
        # 'Clean' floor so a single press converges even from a rough scene:
        # fixed sampling handled by the LT op; here we lift very-low sample
        # counts and give a small default blur. Temporary -- restored after.
        saved = (cscene.samples, cscene.falcon_lt_blur)
        if cscene.samples < 512:
            cscene.samples = 512
        if cscene.falcon_lt_blur <= 0.0:
            cscene.falcon_lt_blur = 2.0
        try:
            return bpy.ops.cycles.falcon_lighttrace_render()
        finally:
            (cscene.samples, cscene.falcon_lt_blur) = saved


class CYCLES_OT_falcon_lt_recomposite(Operator):
    """LT再合成: 直前のLT合成レンダーが残した生パス(ゲイン1/ぼかし0)に、"""     """現在のゲイン/ぼかし値を適用し直して合成画像だけ作り直す。再レンダー無し・数秒。"""     """ゲインとぼかしはLT層に対して線形なので、レンダーし直した場合と結果は厳密に一致する。"""
    bl_idname = "cycles.falcon_lt_recomposite"
    bl_label = "LT再合成 (ゲイン/ぼかし変更・再レンダー無し)"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        import os
        import json
        import time
        import tempfile
        import numpy as np

        scene = context.scene
        cscene = scene.cycles
        stem = os.path.join(
            tempfile.gettempdir(),
            "falcon_lt_%s" % (bpy.path.basename(bpy.data.filepath) or "scene"))
        mpath = stem + "_manifest.json"
        if not os.path.exists(mpath):
            self.report({'ERROR'},
                        "生パスがありません。先に[ライトトレース合成レンダー]を実行してください")
            return {'CANCELLED'}
        try:
            with open(mpath) as f:
                m = json.load(f)
            w, h = m["w"], m["h"]

            def _load(path):
                img = bpy.data.images.load(path)
                px = np.array(img.pixels[:], dtype=np.float32).reshape(h, w, 4)
                bpy.data.images.remove(img)
                return px

            t0 = time.time()
            lt_sum = None
            for p in m["passes"]:
                layer = _load(p)[:, :, :3]
                lt_sum = layer if lt_sum is None else (lt_sum + layer)
            comp = _load(m["beauty"])
            comp[:, :, :3] += cscene.falcon_lt_gain * _falcon_lt_normconv(
                lt_sum, cscene.falcon_lt_blur)
            out_path, disp_path = _falcon_lt_publish(
                context, scene, stem, comp, w, h, self.report)
        except Exception as e:
            self.report({'ERROR'}, "再合成失敗 (生パス欠損/破損?): %s" % e)
            return {'CANCELLED'}
        self.report({'INFO'},
                    "LT再合成完了 (%dパス, ゲイン%.2f ぼかし%.1f, %.1f秒) → %s"
                    % (len(m["passes"]), cscene.falcon_lt_gain,
                       cscene.falcon_lt_blur, time.time() - t0,
                       disp_path if disp_path else out_path))
        return {'FINISHED'}


class CYCLES_OT_falcon_temporal_setup(Operator):
    """アニメレンダー中、各フレームの画像とモーションベクトルを<レンダー出力>/temporal/に自動保存する設定を組み込む(このボタン自体はレンダーしない)。保存した素材に tools/falcon_temporal.py を掛けるとちらつきを除去できる(実測: 16spp+フィルタが64spp生より安定)"""
    bl_idname = "cycles.falcon_temporal_setup"
    bl_label = "Setup Temporal Output"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        import bpy
        scene = context.scene
        context.view_layer.use_pass_vector = True

        tree = scene.compositing_node_group
        if tree is None:
            tree = bpy.data.node_groups.new("Falcon Temporal IO", 'CompositorNodeTree')
            scene.compositing_node_group = tree
            tree.interface.new_socket("Image", in_out='OUTPUT',
                                      socket_type='NodeSocketColor')

        rlayers = next((n for n in tree.nodes
                        if n.bl_idname == 'CompositorNodeRLayers'), None)
        if rlayers is None:
            rlayers = tree.nodes.new('CompositorNodeRLayers')
        gout = next((n for n in tree.nodes if n.bl_idname == 'NodeGroupOutput'), None)
        if gout is None:
            gout = tree.nodes.new('NodeGroupOutput')
        if gout.inputs and not gout.inputs[0].is_linked:
            tree.links.new(rlayers.outputs['Image'], gout.inputs[0])

        if any(n.label == "Falcon Temporal Capture" for n in tree.nodes):
            self.report({'INFO'}, "CyclesF: temporal capture already set up")
            return {'FINISHED'}

        import os
        fo = tree.nodes.new('CompositorNodeOutputFile')
        fo.label = "Falcon Temporal Capture"
        # Next to the render output ("//" would silently fail on unsaved files).
        out_dir = os.path.dirname(bpy.path.abspath(scene.render.filepath))
        fo.directory = os.path.join(out_dir, "temporal")
        fo.file_name = "cap_"
        fo.format.file_format = 'OPEN_EXR_MULTILAYER'
        fo.format.color_depth = '32'
        while len(fo.file_output_items) < 2:
            fo.file_output_items.new('RGBA', "x")
        fo.file_output_items[0].name = "img"
        fo.file_output_items[1].name = "vec"
        tree.links.new(rlayers.outputs['Image'], fo.inputs[0])
        tree.links.new(rlayers.outputs['Vector'], fo.inputs[1])

        self.report({'INFO'},
                    "CyclesF: temporal capture -> //temporal/cap_####.exr "
                    "(stabilize with tools/falcon_temporal.py)")
        return {'FINISHED'}


# --- Falcon DLSS warm-up render ----------------------------------------
# DLSSの時間履歴は「別視点の実フレーム」からしか育たない(同じ絵を焼き直しても、
# サンプルを盛っても、綺麗な色を注いでも温まらない=実測で確認済み)。だから
# 冷えた1枚目は必ずノイジーで、カットの直後も同じ。手前の実フレームを数枚
# 捨て焼きすれば埋まる(2枚でほぼ・4枚で頭打ち)。それを自動でやる。

def _falcon_camera_fcurves(cam_obj):
    """カメラの変換Fカーブ。5.2はAction.fcurves廃止=layers/strips/channelbags経由。"""
    if cam_obj is None or cam_obj.animation_data is None:
        return []
    action = cam_obj.animation_data.action
    if action is None:
        return []
    curves = []
    for layer in action.layers:
        for strip in layer.strips:
            for bag in strip.channelbags:
                curves.extend(bag.fcurves)
    return curves


def _falcon_shot_camera(scene, cut):
    """そのカットで有効になるカメラ。マーカーが無ければシーンのカメラ。

    ★frame_set はマーカーの束縛を再適用するので、カットの手前のフレームでは
    「前のショットのカメラ」に戻ってしまう。ウォームアップはこれを打ち消す。
    """
    cam = None
    for m in sorted((m for m in scene.timeline_markers if m.camera), key=lambda m: m.frame):
        if m.frame <= cut:
            cam = m.camera
    return cam or scene.camera


def _falcon_cut_frames(scene):
    """カットの開始フレーム。マーカーでカメラが切り替わる所=履歴が捨てられる所。"""
    cuts = [scene.frame_start]
    markers = sorted((m for m in scene.timeline_markers if m.camera),
                     key=lambda m: m.frame)
    prev_cam = None
    for m in markers:
        if m.camera is not prev_cam and scene.frame_start < m.frame <= scene.frame_end:
            cuts.append(m.frame)
        prev_cam = m.camera
    return sorted(set(cuts))


class CYCLES_OT_falcon_warmup_render(Operator):
    """DLSSの履歴を温めてからアニメーションをレンダーする。開始フレームと各カットの直前に実フレームを数枚焼いて捨てるので、1枚目とカット直後のノイズが消える(実測: ノイズ指標40.1→35.1)。手前にカメラの動きが無い場合は補外して視差を作る"""
    bl_idname = "cycles.falcon_warmup_render"
    bl_label = "ウォームアップ付きレンダー"

    @classmethod
    def poll(cls, context):
        scene = context.scene
        return (scene is not None and scene.render.engine == 'CYCLES' and
                scene.cycles.use_denoising and scene.cycles.denoiser == 'DLSS')

    def execute(self, context):
        import bpy
        import os

        scene = context.scene
        warm = scene.cycles.denoising_warmup_frames
        if warm <= 0:
            self.report({'ERROR'}, "ウォームアップ枚数が0です")
            return {'CANCELLED'}

        cuts = _falcon_cut_frames(scene)
        original_frame = scene.frame_current
        base_filepath = scene.render.filepath

        # ショットの手前にキーが無いとカメラは静止したままで、視差が出ない=温まらない。
        # 補外を直線にすると「そのまま動き続けていた」状態になり、実フレーム相当の視差が出る。
        # ★ショットごとのカメラすべてに掛ける。1台だけだと2本目以降のカットで効かない。
        saved = []
        for cam in {c for c in (_falcon_shot_camera(scene, cut) for cut in cuts) if c}:
            for fc in _falcon_camera_fcurves(cam):
                saved.append((fc, fc.extrapolation))
                fc.extrapolation = 'LINEAR'

        original_camera = scene.camera
        try:
            for i, cut in enumerate(cuts):
                shot_end = (cuts[i + 1] - 1) if i + 1 < len(cuts) else scene.frame_end
                shot_cam = _falcon_shot_camera(scene, cut)

                # 捨て焼き。連番で焼くのが必須(フレームが飛ぶとカット扱いで履歴が捨てられる)。
                #
                # ★カットの手前のフレームは、マーカーの束縛では**前のショットのカメラ**が
                #   有効になる(実測: cut=15 の warm 4枚は 11〜14 が全部 CamA だった)。
                #   それで温めると、カットで捨てられる側の絵で履歴を埋めることになる。
                #   frame_set の後に、そのショットのカメラへ差し戻す。
                for f in range(cut - warm, cut):
                    scene.frame_set(f)
                    if shot_cam is not None:
                        scene.camera = shot_cam
                    bpy.ops.render.render(write_still=False)

                # 本番。ここから履歴は温まっている。write_stillは連番を付けないので、
                # アニメーションレンダーと同じ名前になるようパスを自前で組む。
                for f in range(cut, shot_end + 1):
                    scene.frame_set(f)
                    scene.render.filepath = base_filepath
                    frame_path = scene.render.frame_path(frame=f)
                    scene.render.filepath = os.path.splitext(frame_path)[0]
                    bpy.ops.render.render(write_still=True)
        finally:
            scene.render.filepath = base_filepath
            for fc, mode in saved:
                fc.extrapolation = mode
            scene.frame_set(original_frame)

        self.report({'INFO'},
                    "CyclesF: %d カット x %d 枚のウォームアップで焼きました" %
                    (len(cuts), warm))
        return {'FINISHED'}


# --- Falcon auto culling -----------------------------------------------
# 「カメラに映らない物」をアニメ全レンジ掃引で検出し、Cycles既存の
# カメラカリング(BVH除外)へ自動opt-inする。カリングされた物は反射/GI/影
# からも消えるため、発光体などは安全フィルタで外し、適用前に選択表示で
# ユーザーが確認できるようにする。

def _falcon_socket_on(sock, is_color):
    """ソケットが実質非ゼロか。リンク済みは値が読めないので安全側=True。"""
    if sock is None:
        return True
    if sock.is_linked:
        return True
    if is_color:
        return max(sock.default_value[:3]) > 0.0
    return sock.default_value > 0.0


def _falcon_object_is_emissive(obj):
    """発光マテリアルを持つか(判定不能は安全側に倒す)。
    Principled の Emission Strength は既定 1.0 (色は黒) なので
    強度だけ見ると全マテリアルが発光扱いになる — 強度×色の両方で判定する。"""
    for slot in obj.material_slots:
        mat = slot.material
        if mat is None:
            continue
        if not mat.use_nodes:
            continue
        for node in mat.node_tree.nodes:
            if node.type == 'EMISSION':
                if (_falcon_socket_on(node.inputs.get("Strength"), False)
                        and _falcon_socket_on(node.inputs.get("Color"), True)):
                    return True
            if node.type == 'BSDF_PRINCIPLED':
                if (_falcon_socket_on(node.inputs.get("Emission Strength"), False)
                        and _falcon_socket_on(node.inputs.get("Emission Color"), True)):
                    return True
    return False


def _falcon_corners_outside(scene, cam_obj, matrix, bbox, margin):
    """bbox 8隅の全部が視錐台の同じ外側にあれば True(保守的判定)。"""
    from bpy_extras.object_utils import world_to_camera_view
    xs, ys, zs = [], [], []
    for c in bbox:
        co = matrix @ c
        v = world_to_camera_view(scene, cam_obj, co)
        xs.append(v.x)
        ys.append(v.y)
        zs.append(v.z)
    if all(z < 0.0 for z in zs):        # 全隅がカメラの後ろ
        return True
    if all(x < -margin for x in xs):    # 全隅が左外
        return True
    if all(x > 1.0 + margin for x in xs):
        return True
    if all(y < -margin for y in ys):
        return True
    if all(y > 1.0 + margin for y in ys):
        return True
    return False


class CYCLES_OT_falcon_auto_cull(Operator):
    """アニメ全レンジを掃引し、一度もカメラに映らないオブジェクトへカメラカリングを自動付与。発光体と巨大オブジェクトは安全のため除外。適用後は対象が選択状態になるので目視確認できる(Undo可)"""
    bl_idname = "cycles.falcon_auto_cull"
    bl_label = "映らない物を自動カリング"
    bl_options = {'REGISTER', 'UNDO'}

    frame_step: bpy.props.IntProperty(
        name="掃引ステップ(フレーム)",
        description="何フレームおきに視錐台を判定するか。小さいほど正確で遅い",
        default=10, min=1, max=100,
    )
    margin: bpy.props.FloatProperty(
        name="マージン",
        description="視錐台の外側に取る安全帯(画面幅比)。モーションブラー等のはみ出し対策",
        default=0.15, min=0.0, max=1.0,
    )
    big_object_ratio: bpy.props.FloatProperty(
        name="巨大物の除外比",
        description="バウンディングボックス対角がシーン対角のこの比を超える物はGI寄与が大きいとみなし除外",
        default=0.5, min=0.05, max=1.0,
    )

    @classmethod
    def poll(cls, context):
        return context.scene is not None and context.scene.camera is not None

    def execute(self, context):
        import mathutils
        scene = context.scene
        cam = scene.camera
        if cam.data.type == 'PANO':
            self.report({'ERROR'}, "パノラマカメラはカリング非対応 (Cycles側の制限)")
            return {'CANCELLED'}

        types_ok = {'MESH', 'CURVE', 'SURFACE', 'META', 'FONT'}
        candidates = [ob for ob in scene.objects
                      if ob.type in types_ok and not ob.hide_render]
        if not candidates:
            self.report({'WARNING'}, "対象オブジェクトがない")
            return {'CANCELLED'}

        # シーン対角(巨大物フィルタの基準)と各オブジェクトのワールド対角
        pts_min = mathutils.Vector((1e18,) * 3)
        pts_max = mathutils.Vector((-1e18,) * 3)
        obj_diag = {}
        for ob in candidates:
            o_min = mathutils.Vector((1e18,) * 3)
            o_max = mathutils.Vector((-1e18,) * 3)
            for c in ob.bound_box:
                w = ob.matrix_world @ mathutils.Vector(c)
                o_min = mathutils.Vector(map(min, o_min, w))
                o_max = mathutils.Vector(map(max, o_max, w))
                pts_min = mathutils.Vector(map(min, pts_min, w))
                pts_max = mathutils.Vector(map(max, pts_max, w))
            obj_diag[ob.name] = (o_max - o_min).length
        scene_diag = (pts_max - pts_min).length or 1.0

        skipped_emissive = []
        skipped_big = []
        check = []
        for ob in candidates:
            if _falcon_object_is_emissive(ob):
                skipped_emissive.append(ob)
                continue
            if obj_diag[ob.name] > scene_diag * self.big_object_ratio:
                skipped_big.append(ob)
                continue
            check.append(ob)

        # 全レンジ掃引: 一度でも映った物は candidates から外していく
        frame_orig = scene.frame_current
        never_visible = set(ob.name for ob in check)
        frames = list(range(scene.frame_start, scene.frame_end + 1, self.frame_step))
        if frames[-1] != scene.frame_end:
            frames.append(scene.frame_end)
        try:
            for f in frames:
                if not never_visible:
                    break
                scene.frame_set(f)
                for name in list(never_visible):
                    ob = scene.objects[name]
                    bbox = [mathutils.Vector(c) for c in ob.bound_box]
                    if not _falcon_corners_outside(scene, cam, ob.matrix_world,
                                                   bbox, self.margin):
                        never_visible.discard(name)  # 映った
        finally:
            scene.frame_set(frame_orig)

        # 適用: シーン側スイッチ+オブジェクトopt-in
        culled = sorted(never_visible)
        for name in culled:
            scene.objects[name].cycles.use_camera_cull = True
        if culled:
            scene.render.use_simplify = True
            scene.cycles.use_camera_cull = True
            if scene.cycles.camera_cull_margin < self.margin:
                scene.cycles.camera_cull_margin = self.margin
        # 確認しやすいよう対象を選択状態に
        for ob in context.selectable_objects:
            ob.select_set(ob.name in never_visible)

        self.report({'INFO'},
                    "カリング %d 個 (発光除外 %d / 巨大除外 %d / 対象%d中) — 選択中の物が対象"
                    % (len(culled), len(skipped_emissive), len(skipped_big),
                       len(candidates)))
        return {'FINISHED'}


class CYCLES_OT_falcon_auto_cull_verify(Operator):
    """低解像度の検証レンダーでカリングの副作用(光漏れ/影消え/映り込み消え)を検出し、原因オブジェクトを二分探索で特定してカリングから自動除外する"""
    bl_idname = "cycles.falcon_auto_cull_verify"
    bl_label = "カリング検証 (誤殺を自動検出)"
    bl_options = {'REGISTER', 'UNDO'}

    threshold: bpy.props.FloatProperty(
        name="許容差(RMSE)",
        description="有り/無しの画素差がこれを超えたら副作用ありとみなす",
        default=0.005, min=0.0005, max=0.1,
    )

    @classmethod
    def poll(cls, context):
        sc = context.scene
        return sc is not None and any(
            getattr(ob.cycles, "use_camera_cull", False) for ob in sc.objects)

    def _render_pixels(self, scene):
        import numpy as np
        import tempfile
        import os
        path = os.path.join(tempfile.gettempdir(),
                            "falcon_cull_verify_%d.png" % os.getpid())
        scene.render.filepath = path
        bpy.ops.render.render(write_still=True)
        img = bpy.data.images.load(path)
        try:
            px = np.array(img.pixels[:], dtype=np.float32)
        finally:
            bpy.data.images.remove(img)
        return px

    def _diff(self, a, b):
        import numpy as np
        return float(np.sqrt(np.mean((a - b) ** 2)))

    def execute(self, context):
        scene = context.scene
        culled = [ob.name for ob in scene.objects
                  if getattr(ob.cycles, "use_camera_cull", False)]
        if not culled:
            self.report({'WARNING'}, "カリング中のオブジェクトがない")
            return {'CANCELLED'}

        # 検証用に軽い設定へ一時変更(終了時に復元)
        r, c = scene.render, scene.cycles
        saved = (r.resolution_percentage, c.samples, c.use_adaptive_sampling,
                 c.use_denoising, r.filepath, r.use_simplify,
                 scene.cycles.use_camera_cull, r.use_persistent_data)
        r.resolution_percentage = 25
        c.samples = 8
        c.use_adaptive_sampling = False
        c.use_denoising = False  # デノイザの非決定性を排除(同シードのノイズは同一)
        # persistent dataはフラグ変更を再同期しない(検証レンダーが全部同じ画になる罠)
        r.use_persistent_data = False

        def set_culled(names):
            names = set(names)
            for ob in scene.objects:
                if ob.name in culled:
                    ob.cycles.use_camera_cull = ob.name in names
                    ob.update_tag()

        offenders = []
        try:
            # 基準=カリング全OFF
            set_culled([])
            base = self._render_pixels(scene)

            def diff_of(subset):
                set_culled(subset)
                return self._diff(self._render_pixels(scene), base)

            if diff_of(culled) <= self.threshold:
                self.report({'INFO'}, "副作用なし (差 %.4f 以下) — %d 個そのまま"
                            % (self.threshold, len(culled)))
                set_culled(culled)
                return {'FINISHED'}

            # 二分探索: 差を生むオブジェクトを特定
            def bisect(subset):
                if not subset:
                    return
                if diff_of(subset) <= self.threshold:
                    return  # この集合は無害
                if len(subset) == 1:
                    offenders.append(subset[0])
                    return
                mid = len(subset) // 2
                bisect(subset[:mid])
                bisect(subset[mid:])

            bisect(list(culled))

            # 犯人を除外して適用し直す
            keep = [n for n in culled if n not in offenders]
            set_culled(keep)
            # 最終確認
            final_diff = diff_of(keep) if keep else 0.0
            self.report({'INFO'},
                        "誤殺 %d 個を除外 (%s%s) — 残り %d 個で差 %.4f"
                        % (len(offenders),
                           ", ".join(offenders[:5]),
                           "…" if len(offenders) > 5 else "",
                           len(keep), final_diff))
        finally:
            (r.resolution_percentage, c.samples, c.use_adaptive_sampling,
             c.use_denoising, r.filepath, r.use_simplify,
             scene.cycles.use_camera_cull, r.use_persistent_data) = saved
        return {'FINISHED'}


class CYCLES_OT_falcon_auto_cull_clear(Operator):
    """全オブジェクトのカメラカリング指定を解除する(自動カリングのやり直し用)"""
    bl_idname = "cycles.falcon_auto_cull_clear"
    bl_label = "カリング指定を全解除"
    bl_options = {'REGISTER', 'UNDO'}

    def execute(self, context):
        n = 0
        for ob in context.scene.objects:
            if getattr(ob.cycles, "use_camera_cull", False):
                ob.cycles.use_camera_cull = False
                n += 1
        context.scene.cycles.use_camera_cull = False
        self.report({'INFO'}, "解除 %d 個" % n)
        return {'FINISHED'}


classes = (
    CYCLES_OT_use_shading_nodes,
    CYCLES_OT_denoise_animation,
    CYCLES_OT_merge_images,
    CYCLES_OT_falcon_auto_cull,
    CYCLES_OT_falcon_auto_cull_verify,
    CYCLES_OT_falcon_auto_cull_clear,
    CYCLES_OT_falcon_near_realtime,
    CYCLES_OT_falcon_final_quality,
    CYCLES_OT_falcon_still_quality,
    CYCLES_OT_falcon_auto_caustics,
    CYCLES_OT_falcon_photon_bake,
    CYCLES_OT_falcon_photon_clear,
    CYCLES_OT_falcon_bake_and_render_range,
    CYCLES_OT_falcon_lighttrace_render,
    CYCLES_OT_falcon_lt_clean_caustics,
    CYCLES_OT_falcon_lt_recomposite,
    CYCLES_OT_falcon_temporal_setup,
    CYCLES_OT_falcon_warmup_render,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)
    if _falcon_reset_photon_state not in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.append(_falcon_reset_photon_state)


def unregister():
    from bpy.utils import unregister_class
    if _falcon_reset_photon_state in bpy.app.handlers.load_post:
        bpy.app.handlers.load_post.remove(_falcon_reset_photon_state)
    for cls in classes:
        unregister_class(cls)
