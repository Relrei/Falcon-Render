# flcompat C2: 開いている .blend の「中身」を JSON へ落とす(往復で壊れないかの物差し)。
#
#   blender --factory-startup -b <blend> --python dump_state.py
#   環境変数 FLC_JSON(出力)・FLC_SAVE_AS(あれば保存し直す)
#
# ★falcon_* は別の袋に分けて出す。素の Blender はこれを黙って捨てるので、
#   本体の差分(falcon_* を除いた物)がゼロであることを合格の線にする。
import json
import os
import sys

import bpy

JS = os.environ["FLC_JSON"]
SAVE_AS = os.environ.get("FLC_SAVE_AS", "")


def r6(x):
    return round(float(x), 6)


def prop_value(owner, prop):
    t = prop.type
    try:
        v = getattr(owner, prop.identifier)
    except Exception:
        return None
    if t in ("BOOLEAN", "INT", "STRING", "ENUM"):
        if prop.is_array if hasattr(prop, "is_array") else False:
            return list(v)
        return v if t != "ENUM" else str(v)
    if t == "FLOAT":
        try:
            return r6(v)
        except TypeError:
            return [r6(x) for x in v]
    return None


def dump_rna(owner, skip_prefix="falcon"):
    """RNA の単純な値だけを拾う。ポインタ・コレクションは辿らない。"""
    main, falcon = {}, {}
    for prop in owner.bl_rna.properties:
        pid = prop.identifier
        if pid == "rna_type" or prop.is_readonly and prop.type not in ("BOOLEAN", "INT", "FLOAT", "STRING", "ENUM"):
            continue
        if prop.type in ("POINTER", "COLLECTION"):
            continue
        v = prop_value(owner, prop)
        if v is None:
            continue
        (falcon if pid.startswith(skip_prefix) else main)[pid] = v
    return main, falcon


def custom_props(owner):
    out = {}
    try:
        for k in owner.keys():
            if k in ("_RNA_UI", "cycles"):
                continue
            v = owner[k]
            try:
                json.dumps(v)
                out[k] = v
            except Exception:
                out[k] = repr(type(v))
    except Exception:
        pass
    return out


def node_tree_sig(nt):
    if nt is None:
        return None
    return {
        "nodes": sorted((n.name, n.bl_idname) for n in nt.nodes),
        "links": sorted(
            (l.from_node.name, l.from_socket.identifier, l.to_node.name, l.to_socket.identifier)
            for l in nt.links
        ),
    }


def main():
    d = bpy.data
    out = {"filepath": d.filepath, "version": list(bpy.app.version), "app": bpy.app.build_commit_date}

    out["counts"] = {k: len(getattr(d, k)) for k in (
        "scenes", "objects", "meshes", "curves", "materials", "images", "lights",
        "cameras", "collections", "node_groups", "actions", "worlds", "texts",
        "armatures", "particles", "textures", "brushes", "linestyles", "grease_pencils",
    ) if hasattr(d, k)}
    for k in ("objects", "meshes", "materials", "images", "lights", "cameras",
              "collections", "node_groups", "actions", "worlds", "texts"):
        out.setdefault("names", {})[k] = sorted(x.name for x in getattr(d, k))

    scenes = {}
    for sc in d.scenes:
        m, f = dump_rna(sc.render)
        s = {"render": m, "render_falcon": f}
        s["frame"] = [sc.frame_start, sc.frame_end, sc.frame_current, sc.frame_step]
        s["camera"] = sc.camera.name if sc.camera else None
        s["world"] = sc.world.name if sc.world else None
        s["objects"] = sorted(o.name for o in sc.objects)
        # Blender 5.x で scene.node_tree は無くなり compositing_node_group になった
        cnt = getattr(sc, "compositing_node_group", None)
        if cnt is None:
            cnt = getattr(sc, "node_tree", None)
        s["use_nodes"] = cnt is not None
        s["compositor"] = node_tree_sig(cnt)
        s["custom"] = custom_props(sc)
        vs = sc.view_settings
        s["view"] = {"view_transform": vs.view_transform, "look": vs.look,
                     "exposure": r6(vs.exposure), "gamma": r6(vs.gamma),
                     "display_device": sc.display_settings.display_device,
                     "sequencer_colorspace": sc.sequencer_colorspace_settings.name}
        if hasattr(sc, "cycles"):
            m, f = dump_rna(sc.cycles)
            s["cycles"] = m
            s["cycles_falcon"] = f
        if hasattr(sc, "eevee"):
            m, _ = dump_rna(sc.eevee)
            s["eevee"] = m
        # 音・VSE
        s["sequences"] = sorted((st.name, st.type, st.frame_final_start, st.frame_final_duration)
                                for st in (sc.sequence_editor.strips if sc.sequence_editor else []))
        scenes[sc.name] = s
    out["scenes"] = scenes

    objs = {}
    for o in d.objects:
        e = {"type": o.type,
             "parent": o.parent.name if o.parent else None,
             "data": o.data.name if o.data else None,
             "matrix": [r6(v) for row in o.matrix_world for v in row],
             "modifiers": [(m.name, m.type) for m in o.modifiers],
             "constraints": [(c.name, c.type) for c in o.constraints],
             "materials": [ms.material.name if ms.material else None for ms in o.material_slots],
             "hide_render": bool(o.hide_render),
             "custom": custom_props(o)}
        if o.type == "MESH" and o.data:
            me = o.data
            e["mesh"] = [len(me.vertices), len(me.edges), len(me.polygons),
                         len(me.uv_layers), len(me.color_attributes)]
        objs[o.name] = e
    out["objects"] = objs

    mats = {}
    for m in d.materials:
        mats[m.name] = {"use_nodes": bool(m.use_nodes),
                        "tree": node_tree_sig(m.node_tree) if m.use_nodes else None,
                        "custom": custom_props(m)}
        if hasattr(m, "cycles"):
            mm, mf = dump_rna(m.cycles)
            mats[m.name]["cycles"] = mm
            if mf:
                mats[m.name]["cycles_falcon"] = mf
    out["materials"] = mats

    out["worlds"] = {w.name: {"tree": node_tree_sig(w.node_tree) if w.use_nodes else None,
                              "custom": custom_props(w)} for w in d.worlds}
    out["images"] = {i.name: {"filepath": i.filepath, "source": i.source,
                              "size": list(i.size), "colorspace": i.colorspace_settings.name}
                     for i in d.images}

    def _fallback(o):
        # ★bytes を返す RNA(BYTE_STRING)がある。ここで落とすと段1 で止まる
        if isinstance(o, bytes):
            return "bytes:" + o.decode("utf-8", "replace")
        return repr(o)

    with open(JS, "w") as f:
        json.dump(out, f, ensure_ascii=False, indent=1, sort_keys=True, default=_fallback)

    if SAVE_AS:
        # ★relative_remap=True: 元と違う場所へ置くので、相対パス(//textures/..)を
        #   繋ぎ直す。繋ぎ直さないと画像が読めず size=0 になり、往復のせいだと誤読する。
        #   段0..段3 を全部同じディレクトリへ保存するので、一度繋ぎ直した後は不変。
        bpy.ops.wm.save_as_mainfile(filepath=SAVE_AS, compress=False,
                                    relative_remap=True, copy=False)
        if not os.path.exists(SAVE_AS):
            raise RuntimeError("save_as_mainfile did not produce " + SAVE_AS)

    with open(JS + ".done", "w") as f:
        f.write("ok\n")


try:
    main()
except Exception:
    import traceback
    traceback.print_exc()
    sys.stderr.write("FLCOMPAT: dump_state.py FAILED\n")
