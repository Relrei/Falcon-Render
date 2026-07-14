# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

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


def _falcon_photon_cache_paths(scene):
    """(grid cache, point cache) keyed by file *and* scene so distinct scenes —
    and any two unsaved files (both basename '') — never share a cache file."""
    import os
    import tempfile
    base = bpy.path.basename(bpy.data.filepath) or "scene"
    key = "%s__%s" % (base, scene.name)
    cache_path = os.path.join(tempfile.gettempdir(), "falcon_photon_%s.bin" % key)
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


def _falcon_specular_mat(mat):
    """True if the material is refractive/metallic — a photon-worthy
    specular caster in the bake's sense."""
    if mat is None or not mat.use_nodes:
        return False
    for n in mat.node_tree.nodes:
        if n.type == 'BSDF_PRINCIPLED':
            if (n.inputs['Transmission Weight'].default_value > 0.5
                    or n.inputs['Metallic'].default_value > 0.5):
                return True
        elif n.type in ('BSDF_GLASS', 'BSDF_REFRACTION', 'BSDF_GLOSSY'):
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


def _falcon_sun_target(scene):
    """FALCON_PHOTON_TARGET string: bounding sphere over objects whose first
    material is refractive/metallic (the photon-worthy specular casters), or
    None if the scene has none."""
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
    return "%.4f,%.4f,%.4f,%.4f" % (cx, cy, cz, rad)


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
            total_energy = sum(max(li.data.energy, 1e-6) for li in lights)
            saved_hide = {li.name: li.hide_render for li in lights}
            single = len(lights) == 1

            r = scene.render
            saved = (r.resolution_x, r.resolution_y, r.resolution_percentage,
                     cscene.samples, cscene.use_adaptive_sampling,
                     cscene.max_bounces, cscene.transmission_bounces,
                     cscene.glossy_bounces)
            os.environ["FALCON_PHOTON_MODE"] = "bake"
            os.environ["FALCON_SHARC_CELL"] = "%.4f" % cscene.falcon_photon_cell
            # Photon-map density-estimation radius (cells): wider = smoother
            # caustics/rainbow rays from fewer photons.
            os.environ["FALCON_PHOTON_RADIUS"] = "%.3f" % cscene.falcon_photon_radius
            # Dispersion works on the GPU path too: the shared kernel hook
            # (falcon_dispersion.h) fires for photon paths when this env is set,
            # so refracted photons carry a wavelength and deposit rainbow flux.
            if disp > 0.0:
                os.environ["FALCON_DISPERSION_B"] = "%.4f" % disp

            per_light_cache = []
            per_light_pts = []
            try:
                for i, light in enumerate(lights):
                    for other in lights:
                        other.hide_render = (other is not light)

                    weight = max(light.data.energy, 1e-6) / total_energy
                    n_photons = max(10000, round(cscene.falcon_photon_photons * weight))
                    # res capped at 4096 so the frame stays ONE tile (a bigger
                    # frame gets tiled and per-tile buffer re-uploads wipe the
                    # device deposits); extra photons come from extra samples.
                    res = max(64, min(4096, int(round(n_photons ** 0.5))))
                    samples = max(1, round(n_photons / (res * res)))
                    os.environ["FALCON_PHOTON_N"] = str(res * res * samples)

                    # per-light output files (single light => write straight to
                    # the final paths, skipping the merge/copy)
                    li_cache = cache_path if single else (cache_path + ".L%d" % i)
                    li_pts = points_path if single else (points_path + ".L%d" % i)
                    os.environ["FALCON_SHARC_CACHE"] = li_cache
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

            # Merge per-light files into the final cache/points (no-op for a
            # single light -- it already wrote the final paths). Clean up temps.
            if not single:
                try:
                    if use_points:
                        _falcon_merge_points(per_light_pts, points_path)
                    else:
                        _falcon_merge_grid(per_light_cache, cache_path)
                except Exception as e:
                    self.report({'ERROR'}, "フォトンマージ失敗: %s" % e)
                    return {'CANCELLED'}
                for fp in per_light_cache + per_light_pts:
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
        os.environ["FALCON_SHARC_CACHE"] = cache_path
        os.environ["FALCON_SHARC_CELL"] = "%.4f" % cscene.falcon_photon_cell
        if use_points and os.path.exists(points_path):
            # Point map wins over the grid cache at load; radius/gain are
            # render-time knobs (the sliders re-set these envs live).
            os.environ["FALCON_PHOTON_POINTS"] = points_path
            os.environ["FALCON_PHOTON_RADIUS_M"] = "%.4f" % cscene.falcon_photon_point_radius
            os.environ["FALCON_PHOTON_NORMAL_DEG"] = "30"
            os.environ["FALCON_PHOTON_GAIN"] = "%.3f" % cscene.falcon_photon_point_gain
        else:
            os.environ.pop("FALCON_PHOTON_POINTS", None)
        # Match the camera render to the bake: with dispersion on, through-glass
        # camera paths should split too (the gem body rainbow), not just the
        # baked floor caustic. Persist/clear the global knob accordingly.
        if disp > 0.0:
            os.environ["FALCON_DISPERSION_B"] = "%.4f" % disp
        else:
            os.environ.pop("FALCON_DISPERSION_B", None)
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


class CYCLES_OT_falcon_lighttrace_render(Operator):
    """ライトトレース合成レンダー(FQ静止画)。光源から光子を追いカメラへ直接つなぐ"""     """キャッシュ無しコースティクス: サンプル数で本当に収束する(点マップのドット無し)。"""     """各ライトのLTパス+通常レンダーを実行し、加算合成した画像を保存する。"""     """重い: サンプル数=シーンのサンプル数。まず低サンプルで試す。固定サンプリングで実行される"""
    bl_idname = "cycles.falcon_lighttrace_render"
    bl_label = "ライトトレース合成レンダー"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

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
        lights = [o for o in scene.objects if o.type == 'LIGHT' and not o.hide_render
                  and o.data.type in ('SUN', 'AREA', 'SPOT')]
        if not lights:
            self.report({'ERROR'}, "対応ライトがありません (SUN/AREA/SPOT)")
            return {'CANCELLED'}
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

        env_keys = ("FALCON_PHOTON_MODE", "FALCON_LIGHTTRACE", "FALCON_LIGHTTRACE_SAMPLES",
                    "FALCON_PHOTON_N", "FALCON_LIGHTTRACE_GAIN", "FALCON_LT_SPLAT_RADIUS",
                    "FALCON_LT_VISIBILITY", "FALCON_PHOTON_MAXPTS", "FALCON_PHOTON_TARGET",
                    "FALCON_PHOTON_POINTS", "FALCON_PHOTON_WORLD")
        saved_env = {k: os.environ.get(k) for k in env_keys}
        saved_hide = {li.name: li.hide_render for li in lights}
        img_set = r.image_settings
        saved = (cscene.use_adaptive_sampling, cscene.max_bounces,
                 cscene.transmission_bounces, cscene.glossy_bounces,
                 r.filepath, img_set.file_format, img_set.color_depth,
                 img_set.color_mode)
        saved_denoise = cscene.use_denoising
        saved_caustics = (cscene.caustics_reflective, cscene.caustics_refractive)
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
        try:
            # --- LT pass, one render per light (integrator emits from the
            # first enabled light; layers add linearly) ---
            for ob in caster_objs:
                ob.cycles.is_caustics_caster = True
            os.environ["FALCON_PHOTON_MODE"] = "bake"
            os.environ["FALCON_LIGHTTRACE"] = "1"
            os.environ["FALCON_LIGHTTRACE_SAMPLES"] = str(spp)
            os.environ["FALCON_PHOTON_N"] = str(w * h * spp)
            os.environ["FALCON_LIGHTTRACE_GAIN"] = "%.3f" % cscene.falcon_lt_gain
            if cscene.falcon_lt_blur > 0.0:
                os.environ["FALCON_LT_SPLAT_RADIUS"] = "%.2f" % cscene.falcon_lt_blur
            else:
                os.environ.pop("FALCON_LT_SPLAT_RADIUS", None)
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

            for i, light in enumerate(lights):
                for other in lights:
                    other.hide_render = (other is not light)
                os.environ.pop("FALCON_PHOTON_TARGET", None)
                if light.data.type == 'SUN':
                    tgt = _falcon_sun_target(scene)
                    if tgt:
                        os.environ["FALCON_PHOTON_TARGET"] = tgt
                r.filepath = "%s_pass%d.exr" % (stem, i)
                bpy.ops.render.render(write_still=True)
                layer = _load_rgba(r.filepath)[:, :, :3]
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
            r.filepath = stem + "_beauty.exr"
            bpy.ops.render.render(write_still=True)
            beauty = _load_rgba(r.filepath)

            # --- additive composite (LT carries only caustic paths the camera
            # tracer cannot find, same no-double-count argument as photon add) ---
            comp = beauty.copy()
            comp[:, :, :3] += lt_sum
            out_path = stem + "_composite.exr"
            name = "Falcon LT合成"
            if name in bpy.data.images:
                bpy.data.images.remove(bpy.data.images[name])
            out = bpy.data.images.new(name, w, h, alpha=True, float_buffer=True)
            out.pixels[:] = comp.ravel()
            out.filepath_raw = out_path
            out.file_format = 'OPEN_EXR'
            out.save()
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
            (cscene.use_adaptive_sampling, cscene.max_bounces,
             cscene.transmission_bounces, cscene.glossy_bounces,
             r.filepath, img_set.file_format, img_set.color_depth,
             img_set.color_mode) = saved
            cscene.use_denoising = saved_denoise
            (cscene.caustics_reflective, cscene.caustics_refractive) = saved_caustics

        self.report({'INFO'}, "LT合成完了 (%d灯%s, %dspp, %.0f秒) → 画像「Falcon LT合成」 %s"
                    % (len(lights), "+ワールド" if world_l > 0.0 else "",
                       spp, time.time() - t0, out_path))
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
        curves = _falcon_camera_fcurves(scene.camera)
        saved = [(fc, fc.extrapolation) for fc in curves]
        for fc in curves:
            fc.extrapolation = 'LINEAR'

        try:
            for i, cut in enumerate(cuts):
                shot_end = (cuts[i + 1] - 1) if i + 1 < len(cuts) else scene.frame_end

                # 捨て焼き。連番で焼くのが必須(フレームが飛ぶとカット扱いで履歴が捨てられる)。
                for f in range(cut - warm, cut):
                    scene.frame_set(f)
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
    CYCLES_OT_falcon_photon_bake,
    CYCLES_OT_falcon_photon_clear,
    CYCLES_OT_falcon_bake_and_render_range,
    CYCLES_OT_falcon_lighttrace_render,
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
