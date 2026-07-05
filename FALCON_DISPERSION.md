# Falcon Dispersion — on-demand spectral (design, 2026-07-03 night)

GO decision after the LuxCore look gate (spectral_gate.blend, v4 renders:
BK7 0.0042 = subtle edge fringe / flint 0.013 = clear split / movie 0.030 =
full rainbow). User accepted slower-than-stock for this ("早いことにいいことは
ない"), quality-first policy. **UX mandate: Octane-like automatic — ONE material
value, no workflow steps.** The previous spectral attempt died because accuracy
was judged after building; this one passed the look gate FIRST.

## What it is (and is not)

LuxCore-style on-demand dispersion (verified: LuxCore is an RGB engine that
samples a wavelength only at glass interactions, converts lambda->RGB, and
continues in RGB — glass dispersion is its only spectral feature). NOT the
rejected full spectral core: no spectral throughput, no spectral textures,
no film changes, upstream-rebase stays cheap.

## Single knob

Material-level float: dispersion strength as **Cauchy B (um^2)**, 0 = off.
UI presets: BK7 0.0042 / フリント 0.013 / 映画 0.030 (from the gate renders).
n(lambda) = ior + B * (1/lambda_um^2 - 1/0.5876^2)  — normalized so
n(587.6nm) == the material's IOR socket value (look doesn't shift when B
changes, only spreads).

## Delivery path (two stages)

- **v0 (land the whole look end-to-end, fastest)**: global knob
  `FALCON_DISPERSION_B` (env + CyclesF panel field) applied to every SMOOTH
  refractive microfacet closure. No SVM plumbing. Proves kernel math, OIDN
  interaction, photon integration. Limitation: per-scene not per-material.
- **v1 (the product)**: per-material. Property on material (cycles settings,
  addon-injected "分散" field next to displacement settings) -> read in
  blender/shader.cpp where Principled/Glass nodes are translated -> new float
  in the microfacet closure + SVM param. MNEE keeps center IOR in v1 (no
  rainbow in MNEE caustics yet; photon covers rainbow caustics).

## Kernel algorithm (camera/BSDF paths — NO baking needed)

Path state: one float `dispersion_lambda` (0 = unassigned).
At a refractive scatter with B > 0:
  if lambda unassigned:
      sample lambda ~ U(380, 730) nm
      throughput *= cie_weight(lambda)   (white-balanced linear-sRGB weight,
                                          sum over uniform lambda -> (1,1,1);
                                          math verified in the shelved
                                          falcon_spectral_20260703.py)
      store lambda
  refract with n(lambda).
Subsequent dispersive hits on the same path REUSE lambda (consistency).
Shadow rays / transparent shadows: unchanged (LuxCore makes the same
approximation). Reflections of dispersed transmission inherit correctly for
free (throughput tint).

## GPU photon bake wavelength (closes the diamond-ray gap)

Same lazy-lambda rule inside FALCON_PHOTON_MODE=bake: photon gets lambda at
its first dispersive interaction, flux weighted by the same cie_weight, IOR
offset identical. This upgrades the CPU-only --dispersion/--abbe feature
(a2c4a09, shelved patch) to the 2s GPU path, so **caustic rainbows (LuxCore
diamond look, user ref image 2026-07-03) appear automatically whenever a
dispersive material casts baked caustics**. Scene-level
falcon_photon_dispersion prop is superseded by the material value (v1).

## Expected costs / test items

- Color noise: one lambda per path -> chroma noise on dispersive paths;
  converges with spp. Dispersion shots = raw mode or high spp (accepted).
- **OIDN will likely smear fringes** (albedo guide is wavelength-blind).
  Measure; if confirmed, dispersion shots recommend raw or layer-split.
- Energy: white-through-flat-glass must stay white (cie_weight白バランスで
  保証されるはず — regression: flat slab, B=0.03, RMSE vs B=0).
- Referee: spectral_gate.blend (SAVED, LuxCore) — CyclesF wedge fringes at
  equal B must match LuxCore's width/hue progression.
- Zoo + gem scene for caustic rainbows (vs user's LuxCore diamond reference).

## Traps (from this codebase's history — read before editing)

- cubin AND kernel_optix*.ptx.zst must be hand-copied after kernel edits;
  verify once on OPTIX (CUDA-only verification is blind).
- Scripts must set engine='CYCLES' explicitly (EEVEE trap).
- Addon .py files also need copying to bin/.../addons_core/cycles/.
- Regenerate blender_5.1_sharc_port.patch with `git add -N` for new files.

## v0 LANDED + verified (2026-07-04, OptiX)

Global-knob v0 is implemented and verified live on OptiX (RTX 3080, GPU util
98% during the render, "Path tracing on: ... (OptiX)" confirmed — the OptiX PTX
artifact was hand-synced per FALCON_PHOTON.md, and stale-kernel would have shown
zero dispersion). Wiring:
- `FALCON_DISPERSION_B` env -> KernelIntegrator.falcon_dispersion_b
  (scene/integrator.cpp, same gate pattern as FALCON_SHARC_MODE; 0 = zero cost).
- path state float `dispersion_lambda` (state_template.h, init 0 in path_state.h).
- PRNG_SURFACE_DISPERSION = 7 (free slot in the surface block).
- Hook in integrate_surface_bsdf_bssrdf_bounce (shade_surface.h) right after the
  closure is picked, before sampling: for a SMOOTH (alpha_x*alpha_y <=
  BSDF_ROUGHNESS_SQ_THRESH) CLOSURE_IS_REFRACTION/CLOSURE_IS_GLASS closure, on the
  first dispersive scatter sample lambda, tint throughput by cie_weight, store it;
  then `bsdf->ior += ior_offset(B, lambda)` (the sampler reads bsdf->ior as m_eta,
  so no other change is needed). Later hits reuse the stored lambda.
- Math header falcon_dispersion.h (cie_weight + Cauchy offset); registered in
  kernel/CMakeLists.txt; port patch regenerated with `git add -N`.

Verification (backlit smooth glass sphere, 512 spp, raw/no-denoise, A/B B=0 vs
B=0.03): B=0 render is EXACTLY achromatic (total per-pixel chroma = 0.0);
B=0.03 produces strong chroma; **energy neutral, mean-luminance ratio
0.984** (cie_weight white-balance holds). A bright/black-edge backdrop shows a
real *structured* cyan/red rim fringe (not just noise). Scripts kept:
scratchpad disp_scene.py (sphere A/B), disp_scene2.py (edge), disp_prism.py.

Open follow-ups (not yet done): per-material v1 (SVM param), a clean full-rainbow
demo scene (point-through-prism framing landed off-frame — sphere+edge proved it
instead), and PHOTON dispersion — the bake path reuses this same shared hook so
baked caustics *should* get rainbows for free, but that is UNVERIFIED (the
"GPU has no dispersion" UI note in FALCON_PHOTON.md still stands until measured;
bake is sample-0-only so the LD wavelength may band -> may need a hashed lambda).

## Out of scope (explicit)

- Full spectral core (rejected; reinforced by the faintness lesson).
- Volumetric light shafts of the diamond rays (participating media — future).
- MNEE wavelength solve (v2 candidate).
- Polarization / fluorescence / thin-film spectral (not asked, heavy).
