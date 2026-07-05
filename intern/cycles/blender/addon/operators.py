# SPDX-FileCopyrightText: 2011-2022 Blender Foundation
#
# SPDX-License-Identifier: Apache-2.0

from __future__ import annotations

import bpy
from bpy.types import Operator
from bpy.props import StringProperty

from bpy.app.translations import pgettext_tip as tip_


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


class CYCLES_OT_falcon_photon_bake(Operator):
    """このシーンでフォトントレースを実行し、コースティクス(ガラス/水/鏡の集光)を"""     """キャッシュに焼く。完了後のレンダーに自動で合成される(加算・レンダー時間ほぼ増なし)。"""     """対象: Principledの透過(ガラス/水)・滑らかな金属。光源は最初のライト1灯(SUN対応)。"""     """注意: 水/ガラスの「透明の影」をOFFにすると物理的に正しい合成になる"""
    bl_idname = "cycles.falcon_photon_bake"
    bl_label = "コースティクスを焼く"

    @classmethod
    def poll(cls, context):
        return context.scene is not None

    def execute(self, context):
        import os
        import time
        import tempfile
        from . import falcon_photon

        scene = context.scene
        cscene = scene.cycles
        if scene.render.engine != 'CYCLES':
            # EEVEE trap (FALCON_PHOTON.md): a non-Cycles engine renders the
            # photon frame as a normal EEVEE frame — no deposits, stale cache,
            # and it looks exactly like a kernel regression. Hard-fail instead.
            self.report({'ERROR'}, "レンダーエンジンがCYCLESではありません")
            return {'CANCELLED'}
        cache_path = os.path.join(
            tempfile.gettempdir(),
            "falcon_photon_%s.bin" % (bpy.path.basename(bpy.data.filepath) or "scene"))
        points_path = cache_path[:-4] + ".fph"

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
                    # Bounding sphere over objects with a refractive/metallic
                    # first material. Recomputed per light (direction-dependent).
                    os.environ.pop("FALCON_PHOTON_TARGET", None)
                    if light.data.type == 'SUN':
                        corners = []
                        for ob in scene.objects:
                            if ob.type != 'MESH' or ob.hide_render or not ob.material_slots:
                                continue
                            mat = ob.material_slots[0].material
                            if mat is None or not mat.use_nodes:
                                continue
                            spec = False
                            for n in mat.node_tree.nodes:
                                if n.type == 'BSDF_PRINCIPLED':
                                    if (n.inputs['Transmission Weight'].default_value > 0.5
                                            or n.inputs['Metallic'].default_value > 0.5):
                                        spec = True
                                elif n.type in ('BSDF_GLASS', 'BSDF_REFRACTION', 'BSDF_GLOSSY'):
                                    spec = True
                            if spec:
                                from mathutils import Vector
                                corners += [ob.matrix_world @ Vector(c) for c in ob.bound_box]
                        if corners:
                            cx = sum(c.x for c in corners) / len(corners)
                            cy = sum(c.y for c in corners) / len(corners)
                            cz = sum(c.z for c in corners) / len(corners)
                            rad = max(((c.x - cx) ** 2 + (c.y - cy) ** 2 + (c.z - cz) ** 2) ** 0.5
                                      for c in corners) + 0.05
                            os.environ["FALCON_PHOTON_TARGET"] = ("%.4f,%.4f,%.4f,%.4f"
                                                                  % (cx, cy, cz, rad))

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
        self.report({'INFO'}, "フォトンコースティクス無効化")
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


classes = (
    CYCLES_OT_use_shading_nodes,
    CYCLES_OT_denoise_animation,
    CYCLES_OT_merge_images,
    CYCLES_OT_falcon_near_realtime,
    CYCLES_OT_falcon_final_quality,
    CYCLES_OT_falcon_still_quality,
    CYCLES_OT_falcon_photon_bake,
    CYCLES_OT_falcon_photon_clear,
    CYCLES_OT_falcon_temporal_setup,
)


def register():
    from bpy.utils import register_class
    for cls in classes:
        register_class(cls)


def unregister():
    from bpy.utils import unregister_class
    for cls in classes:
        unregister_class(cls)
