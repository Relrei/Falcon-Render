# Falcon Photon Cache — proof tracer (Round 2).
#
# Traces caustic photons (light -> specular chain -> first diffuse hit) through
# the open Blender scene with depsgraph ray_cast and deposits them into a
# FALCON_SHARC_CACHE-format hash grid, so the existing CyclesF SHARC blend
# kernel (ALPHA=1, gate off) can visualize the photon map with zero kernel
# changes. See FALCON_PHOTON.md for the design and its measured grounding.
#
#   blender -b scene.blend --python falcon_photon_trace.py -- \
#       --photons 2000000 --out /path/cache.bin [--gain 1.0]
#
# Zoo-specific honesty: specular behavior is name-mapped (glass/water refract,
# mirror/gold reflect) and smooth normals are analytic per object (ray_cast
# only returns flat polygon normals). The product version reads Principled
# sockets instead.

import sys
import math

import bpy
import numpy as np
from mathutils import Vector

# ---------------------------------------------------------------------------
# SHARC hash grid (must match kernel/integrator/falcon_sharc.h exactly)

CELL_COUNT = 1 << 22
CELL_MASK = CELL_COUNT - 1
CELL_STRIDE = 4
CELL_SIZE = 0.2  # overridden by --cell

U32 = 0xFFFFFFFF


def _rot(x, k):
    return ((x << k) | (x >> (32 - k))) & U32


def hash_uint3_np(kx, ky, kz):
    """Vectorized Jenkins lookup3 final mix, matching util/hash.h hash_uint3."""
    a = np.full_like(kx, (0xDEADBEEF + (3 << 2) + 13) & U32)
    b = a.copy()
    c = a.copy()
    c = (c + kz) & U32
    b = (b + ky) & U32
    a = (a + kx) & U32
    # final(a, b, c)
    c ^= b
    c = (c - _rot(b, 14)) & U32
    a ^= c
    a = (a - _rot(c, 11)) & U32
    b ^= a
    b = (b - _rot(a, 25)) & U32
    c ^= b
    c = (c - _rot(b, 16)) & U32
    a ^= c
    a = (a - _rot(c, 4)) & U32
    b ^= a
    b = (b - _rot(a, 14)) & U32
    c ^= b
    c = (c - _rot(b, 24)) & U32
    return c


def cell_index(P):
    """P: (N,3) float64 world positions -> (N,) uint32 cell indices."""
    g = np.floor(P / CELL_SIZE).astype(np.int64).astype(np.uint32)
    return hash_uint3_np(g[:, 0].astype(np.uint64), g[:, 1].astype(np.uint64),
                         g[:, 2].astype(np.uint64)).astype(np.uint32) & CELL_MASK


SPLAT_SIGMA = 1.0  # in cells; overridden by --smooth


def cell_tag_np(g):
    """Collision tag stored in the count field: 1 + (20-bit second hash of the
    grid coords) * 2^-20. Must match kernel falcon_photon_tag exactly -- the
    lookup rejects cells whose stored tag differs from the tag of the queried
    grid coords (hash-collision false positives)."""
    t = hash_uint3_np((g[:, 0] ^ np.uint32(0x517cc1b7)).astype(np.uint64),
                      (g[:, 1] ^ np.uint32(0x27220a95)).astype(np.uint64),
                      (g[:, 2] ^ np.uint32(0xfe4db3af)).astype(np.uint64))
    return 1.0 + (t & np.uint64(0xFFFFF)).astype(np.float64) * (1.0 / 1048576.0)


def splat_trilinear(P, F, flux_channel, axis=None, tag_channel=None):
    """Deposit flux F at positions P with a separable gaussian footprint of
    SPLAT_SIGMA cells (1.0 ~= the old trilinear support; 1.5-2.0 trades a
    little caustic sharpness for much less sparse-cell shot noise). Kernel
    lookup stays nearest-cell; total flux is normalized to be preserved."""
    q = P / CELL_SIZE - 0.5
    base = np.floor(q).astype(np.int64)
    frac = q - base
    r = max(1, int(math.ceil(SPLAT_SIGMA)))
    offs = range(-r + 1, r + 1)
    sig2 = 2.0 * (SPLAT_SIGMA * 0.5) ** 2

    def axis_w(i, d):
        """Per-axis weight: gaussian in the surface plane, trilinear along the
        deposit normal axis (spreading along the normal wastes flux into cells
        the surface lookup never reads - measured 0.64x on the ocean floor)."""
        g = np.exp(-((d - frac[:, i]) ** 2) / sig2)
        if axis is None:
            return g
        tri = np.clip(1.0 - np.abs(d - frac[:, i]), 0.0, 1.0)
        is_n = (axis == i)
        return np.where(is_n, tri, g)

    wsum = np.zeros(len(F))
    weights = {}
    for dx in offs:
        ex = axis_w(0, dx)
        for dy in offs:
            ey = axis_w(1, dy)
            for dz in offs:
                ez = axis_w(2, dz)
                w = ex * ey * ez
                weights[(dx, dy, dz)] = w
                wsum += w
    for (dx, dy, dz), w in weights.items():
        g = (base + np.array([dx, dy, dz])).astype(np.uint32)
        idx = (hash_uint3_np(g[:, 0].astype(np.uint64),
                             g[:, 1].astype(np.uint64),
                             g[:, 2].astype(np.uint64)).astype(np.uint32)
               & CELL_MASK)
        np.add.at(flux_channel, idx, F * w / wsum)
        if tag_channel is not None:
            sel = w > 0.0  # zero-weight writes must not steal the cell's tag
            tag_channel[idx[sel]] = cell_tag_np(g[sel])


# ---------------------------------------------------------------------------
# Zoo material map + analytic normals

REFRACT = {"glass": 1.45, "water_mat": 1.33}
REFLECT = {"mirror_mat", "gold"}
ALBEDO = 0.65  # checker floor mean


_classify_cache = {}


def classify(obj):
    """Specular behavior from the Principled sockets (name map = fallback for
    scenes without Principled). Transmission -> refract with the material IOR;
    smooth metal -> mirror; everything else diffuse."""
    if not obj.data.materials:
        return 'DIFFUSE', 0.0
    mat = obj.data.materials[0]
    hit = _classify_cache.get(mat.name)
    if hit is not None:
        return hit
    result = ('DIFFUSE', 0.0)
    if mat.name in REFRACT:
        result = ('REFRACT', REFRACT[mat.name])
    elif mat.name in REFLECT:
        result = ('REFLECT', 0.0)
    else:
        try:
            b = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
            trans = b.inputs['Transmission Weight'].default_value
            metal = b.inputs['Metallic'].default_value
            rough = b.inputs['Roughness'].default_value
            if trans > 0.5 and rough < 0.2:
                result = ('REFRACT', b.inputs['IOR'].default_value)
            elif metal > 0.5 and rough < 0.2:
                result = ('REFLECT', 0.0)
        except Exception:
            pass
        if result[0] == 'DIFFUSE':
            # dedicated Glass/Refraction/Glossy nodes (trees without Principled)
            try:
                for nd in mat.node_tree.nodes:
                    if nd.type in ('BSDF_GLASS', 'BSDF_REFRACTION'):
                        if nd.inputs['Roughness'].default_value < 0.2:
                            result = ('REFRACT', nd.inputs['IOR'].default_value)
                        break
                    if nd.type == 'BSDF_GLOSSY':
                        if nd.inputs['Roughness'].default_value < 0.2:
                            result = ('REFLECT', 0.0)
                        break
            except Exception:
                pass
    _classify_cache[mat.name] = result
    return result


def base_albedo(obj):
    """Mean Principled Base Color of the hit object's first material (textured
    materials fall back to the socket default = the flat component)."""
    try:
        mat = obj.data.materials[0]
        bsdf = next(n for n in mat.node_tree.nodes if n.type == 'BSDF_PRINCIPLED')
        c = bsdf.inputs['Base Color'].default_value
        return (c[0] + c[1] + c[2]) / 3.0
    except Exception:
        return ALBEDO


def smooth_normal(obj, P, flat_n):
    """Analytic normals for the zoo's curved objects; flat elsewhere. Any
    heightfield water can publish its wave spec via a "falcon_waves" custom
    property (flat list of amp,k,dx,dy,phase per component) and gets exact
    gradient normals regardless of tessellation."""
    spec = obj.get("falcon_waves")
    if spec is not None:
        inv = obj.matrix_world.inverted()
        p = inv @ P
        x, y = p.x, p.y
        dfdx = dfdy = 0.0
        for j in range(0, len(spec), 5):
            amp, k, dx, dy, ph = spec[j:j + 5]
            c = amp * k * math.cos(k * (dx * x + dy * y) + ph)
            dfdx += c * dx
            dfdy += c * dy
        return (obj.matrix_world.to_3x3() @ Vector((-dfdx, -dfdy, 1.0))).normalized()
    tor = obj.get("falcon_torus")
    if tor is not None:
        # analytic torus normal, local space: falcon_torus = [major_R, minor_r]
        inv = obj.matrix_world.inverted()
        p = inv @ P
        ring = Vector((p.x, p.y, 0.0))
        if ring.length < 1e-6:
            return flat_n
        center = ring.normalized() * float(tor[0])
        n_local = p - center
        if n_local.length < 1e-6:
            return flat_n
        return (obj.matrix_world.to_3x3() @ n_local).normalized()
    name = obj.name
    if name == "glass_sphere":
        c = obj.matrix_world.translation
        n = (P - c).normalized()
        return n
    if name == "metal_ring":
        # torus in local space: major R=0.45, minor r=0.06
        inv = obj.matrix_world.inverted()
        p = inv @ P
        ring = Vector((p.x, p.y, 0.0))
        if ring.length < 1e-6:
            return flat_n
        center = ring.normalized() * 0.45
        n_local = (p - center)
        if n_local.length < 1e-6:
            return flat_n
        n = (obj.matrix_world.to_3x3() @ n_local).normalized()
        return n
    if name == "water":
        # ripple heightfield: z = f(x,y) in local coords (object at origin offset)
        inv = obj.matrix_world.inverted()
        p = inv @ P
        x, y = p.x, p.y
        dfdx = (0.016 * 9.0 * math.cos(9.0 * x + 1.3) * math.sin(7.5 * y)
                + 0.010 * 14.0 * math.cos(14.0 * (x + y) + 0.7))
        dfdy = (0.016 * 7.5 * math.sin(9.0 * x + 1.3) * math.cos(7.5 * y)
                + 0.010 * 14.0 * math.cos(14.0 * (x + y) + 0.7))
        n = (obj.matrix_world.to_3x3() @ Vector((-dfdx, -dfdy, 1.0))).normalized()
        return n
    return flat_n


# ---------------------------------------------------------------------------
# Optics

def refract_dir(I, N, ior):
    """I: incoming (pointing at surface), N: normal. Returns (dir, is_tir)."""
    cosi = -I.dot(N)
    n = N
    eta = 1.0 / ior
    if cosi < 0.0:  # exiting the medium
        cosi = -cosi
        n = -N
        eta = ior
    k = 1.0 - eta * eta * (1.0 - cosi * cosi)
    if k < 0.0:
        return I.reflect(n), True  # total internal reflection
    return (I * eta + n * (eta * cosi - math.sqrt(k))).normalized(), False


def fresnel(I, N, ior):
    cosi = abs(I.dot(N))
    # Schlick
    r0 = ((1.0 - ior) / (1.0 + ior)) ** 2
    return r0 + (1.0 - r0) * (1.0 - cosi) ** 5


# ---------------------------------------------------------------------------
# Main

def main(argv):
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("--photons", type=int, default=1000000)
    ap.add_argument("--out", required=True)
    ap.add_argument("--gain", type=float, default=1.0)
    ap.add_argument("--cell", type=float, default=0.2)
    ap.add_argument("--dispersion", type=float, default=0.0,
                    help="Cauchy-style IOR spread (e.g. 0.02: R=ior-d, B=ior+d)")
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--smooth", type=float, default=1.0,
                    help="splat footprint in cells (1=trilinear-ish, 2=smoother)")
    ap.add_argument("--allow-direct", action="store_true",
                    help="deposit direct (non-caustic) photons too - calibration only")
    ap.add_argument("--dump-points", default=None,
                    help="dev: save raw photon points to an .npz for offline KDE")
    args = ap.parse_args(argv)

    global CELL_SIZE, SPLAT_SIGMA
    CELL_SIZE = args.cell
    SPLAT_SIGMA = args.smooth
    rng = np.random.default_rng(args.seed)
    deps = bpy.context.evaluated_depsgraph_get()
    scene = bpy.context.scene

    light = next(o for o in scene.objects if o.type == 'LIGHT')
    L_pos = light.matrix_world.translation
    L_dir = (light.matrix_world.to_3x3() @ Vector((0, 0, -1))).normalized()
    P_watt = light.data.energy
    is_sun = light.data.type == 'SUN' 

    # emission cone toward the scene contents (bounding sphere of meshes)
    pts = [o.matrix_world.translation for o in scene.objects if o.type == 'MESH']
    center = sum(pts, Vector()) / len(pts)
    radius = max((o.matrix_world.translation - center).length for o in scene.objects
                 if o.type == 'MESH') + 2.0
    to_c = (center - L_pos)
    dist = to_c.length
    axis = to_c.normalized()
    cos_half = math.sqrt(max(0.0, 1.0 - (radius / dist) ** 2)) if dist > radius else -1.0
    omega = 2.0 * math.pi * (1.0 - cos_half)

    # orthonormal basis around the cone axis
    up = Vector((0, 0, 1)) if abs(axis.z) < 0.9 else Vector((1, 0, 0))
    t1 = axis.cross(up).normalized()
    t2 = axis.cross(t1)

    N = args.photons
    deposits_P = []
    deposits_flux = []
    deposits_ch = []
    deposits_axis = []
    # spectral photons: each carries one RGB channel, flux x3 to conserve energy
    channels = rng.integers(0, 3, N) if args.dispersion > 0.0 else None
    n_caustic = 0
    n_hit_spec = 0

    ray_cast = scene.ray_cast
    EPS = 1e-4

    u = rng.random(N)
    v = rng.random(N)
    cos_t = 1.0 - u * (1.0 - cos_half)
    sin_t = np.sqrt(np.maximum(0.0, 1.0 - cos_t ** 2))
    phi = 2.0 * math.pi * v

    if is_sun:
        # Parallel rays swept over a square PERPENDICULAR to the sun direction,
        # placed upstream of the specular targets' bounding sphere. (Launching
        # over a horizontal footprint missed the target for non-vertical suns:
        # rays start above but travel diagonally and leave the footprint before
        # reaching the object.) Blender sun energy = irradiance (W/m^2) normal to
        # the rays, so power over the perpendicular launch square = E * A_perp.
        spec_objs = [o for o in scene.objects if o.type == 'MESH'
                     and classify(o)[0] != 'DIFFUSE']
        allbb = [o.matrix_world @ Vector(c) for o in spec_objs for c in o.bound_box]
        C = sum(allbb, Vector()) / len(allbb)
        Rt = max((p - C).length for p in allbb) + 0.1
        upv = Vector((0, 0, 1)) if abs(L_dir.z) < 0.9 else Vector((1, 0, 0))
        e1 = L_dir.cross(upv).normalized()
        e2 = L_dir.cross(e1).normalized()
        plane_center = C - L_dir * (Rt + 1.0)
        sun_flux = P_watt * (2.0 * Rt) ** 2 / N
        su = (rng.random(N) - 0.5) * 2.0 * Rt
        sv = (rng.random(N) - 0.5) * 2.0 * Rt

    for i in range(N):
        if is_sun:
            pos = plane_center + e1 * float(su[i]) + e2 * float(sv[i])
            dirv = L_dir
            flux = sun_flux
        else:
            d = (axis * float(cos_t[i]) + t1 * float(sin_t[i] * math.cos(phi[i]))
                 + t2 * float(sin_t[i] * math.sin(phi[i])))
            cos_emit = d.dot(L_dir)
            if cos_emit <= 0.0:
                continue
            # Lambertian emitter, uniform-cone sampling: Phi = P*cos*Omega/(pi*N)
            flux = P_watt * cos_emit * omega / (math.pi * N)
            pos = L_pos
            dirv = d
        bounced_specular = False
        ch = int(channels[i]) if channels is not None else -1
        # wavelength-dependent IOR offset: R softer, B stronger (normal dispersion)
        ior_off = args.dispersion * (ch - 1) if ch >= 0 else 0.0
        for _ in range(8):
            hit, P, n_flat, _, obj, _ = ray_cast(deps, pos + dirv * EPS, dirv)
            if not hit:
                break
            kind, ior = classify(obj)
            if kind == 'DIFFUSE':
                if bounced_specular or args.allow_direct:
                    # Nudge the deposit half a cell along the hit normal (to the cell
                    # center in the normal axis): flat receivers sit exactly on cell
                    # boundaries (floor z=0) and centered trilinear splatting would
                    # leak flux into the cell behind the surface (measured: 0.50x at
                    # no offset, 0.75x at quarter-cell, ~1.0 at half-cell).
                    n_off = n_flat if n_flat.dot(dirv) < 0 else -n_flat
                    Pd = P + n_off * (0.5 * CELL_SIZE)
                    deposits_P.append((Pd.x, Pd.y, Pd.z))
                    deposits_axis.append(max(range(3), key=lambda a: abs(n_off[a])))
                    a = base_albedo(obj)
                    deposits_flux.append(flux * (3.0 if ch >= 0 else 1.0) * a)
                    deposits_ch.append(ch)
                    n_caustic += 1
                break
            n = smooth_normal(obj, P, n_flat)
            n_hit_spec += 1
            bounced_specular = True
            if kind == 'REFLECT':
                dirv = dirv.reflect(n).normalized()
                flux *= 0.9  # metal reflectance approx
            else:
                fr = fresnel(dirv, n, ior)
                if rng.random() < fr:
                    dirv = dirv.reflect(n if dirv.dot(n) < 0 else -n).normalized()
                else:
                    dirv, _ = refract_dir(dirv, n, ior + ior_off)
            pos = P

    print(f"PHOTON,emitted,{N},spec_hits,{n_hit_spec},caustic_deposits,{n_caustic}",
          flush=True)

    # flux -> cell radiance: L = sum(flux) * albedo / (pi * h^2)
    cache = np.zeros(CELL_COUNT * CELL_STRIDE, dtype=np.float32)
    if deposits_P:
        Pw = np.asarray(deposits_P, dtype=np.float64)
        F = np.asarray(deposits_flux, dtype=np.float64)
        CH = np.asarray(deposits_ch, dtype=np.int64)
        AX = np.asarray(deposits_axis, dtype=np.int64)
        # Dev: dump raw photon points (position + per-channel flux + normal axis)
        # so the photon-map density estimator can be prototyped offline without
        # re-tracing. Photon-mapping upgrade (Milestone A/B).
        if getattr(args, "dump_points", None):
            np.savez(args.dump_points, P=Pw.astype(np.float32), F=F.astype(np.float32),
                     CH=CH.astype(np.int32), AX=AX.astype(np.int32),
                     cell=np.float32(CELL_SIZE))
            print(f"PHOTON,dumped,{args.dump_points},{len(Pw)}", flush=True)
        flux_rgb = np.zeros((CELL_COUNT, 3), dtype=np.float64)
        tags = np.zeros(CELL_COUNT, dtype=np.float64)
        if args.dispersion > 0.0:
            for ch in range(3):
                sel = CH == ch
                splat_trilinear(Pw[sel], F[sel], flux_rgb[:, ch], axis=AX[sel],
                                tag_channel=tags)
        else:
            for ch in range(3):
                splat_trilinear(Pw, F, flux_rgb[:, ch], axis=AX,
                                tag_channel=tags)
        used = np.nonzero(flux_rgb.sum(axis=1))[0]
        L = flux_rgb[used] * args.gain / (math.pi * CELL_SIZE * CELL_SIZE)
        for ch in range(3):
            cache[used * CELL_STRIDE + ch] = L[:, ch]
        cache[used * CELL_STRIDE + 3] = tags[used]
        print(f"PHOTON,cells,{len(used)},L_mean,{float(L.mean()):.4f},"
              f"L_max,{float(L.max()):.4f}", flush=True)
    cache.tofile(args.out)
    print(f"PHOTON,saved,{args.out}", flush=True)


if __name__ == "__main__":
    argv = sys.argv[sys.argv.index("--") + 1:] if "--" in sys.argv else []
    main(argv)
