# Falcon Light Tracing — design (2026-07-05, CyclesFQ)

Goal: a cache-FREE caustic method for CyclesFQ. Trace photons from the lights,
follow the specular chain, and at the first diffuse hit CONNECT DIRECTLY TO THE
CAMERA (project the vertex to a pixel, visibility-test, splat the flux there) —
instead of depositing into the point map and gathering at render time. This is
classic **light tracing / particle tracing** (Veach ch.10; PBRT 16.3 t=1
strategy), the light-side dual of path tracing.

## Why (the whole night's findings point here)

- The photon-MAP bake is a static one-shot: camera SPP does NOT reduce caustic
  noise (every sample reads the same frozen density estimate). Light tracing
  RE-TRACES photons every iteration -> the caustic CONVERGES with more samples,
  no permanent dots, no gather cost, no point cache. Perfect for FQ (quality
  first, render time ignored).
- It is NOT a replacement for the point map in general: light tracing can only
  splat when the diffuse vertex is DIRECTLY visible to the camera (L-S-D-eye).
  The L-S-D-S-eye case (caustic seen THROUGH/reflected-in glass) needs the last
  eye bounce to be specular -> the camera connection has zero density there, so
  those still need the map or MNEE. Complementary, not either/or.

## Confirmed feasible in Cycles (source hooks, 2026-07-05)

- Projection: `camera_world_to_ndc(kg, sd, P)` (kernel/camera/camera.h:546) ->
  NDC in [0,1]^2 for perspective/ortho; multiply by film res for raster (px,py).
- Arbitrary-pixel film write: the render buffer is flat, indexed
  `render_pixel_index = tile->offset + x + y*tile->stride`
  (integrator/path_state.h:32). Splatting to (px,py) = same formula with px,py.
  Film passes already do `atomic_add_and_fetch_float` (film/write.h:62,
  cryptomatte) so cross-pixel atomic accumulation is supported.
- Camera position / aperture: `camera_position(kg)` (camera.h:505). Visibility =
  a shadow ray vertex->camera (Cycles shadow-ray infra, shade_surface.h).
- REUSE from Falcon Photon bake: photon emission (init_from_camera bake path,
  sun/area/spot), specular chain tracing (shade_surface bake continues
  refract/reflect), first-diffuse-hit + caustic gate (bounce>0), flux clamp.

## Architecture

New render mode FALCON_LIGHTTRACE (env, like the bake). The render becomes a
light-tracing pass: init_from_camera emits photons (reuse the bake emitter);
shade_surface, at the first diffuse caustic hit, instead of depositing:
1. `ndc = camera_world_to_ndc(kg, &sd, sd.P)`; reject if outside [0,1] or behind.
2. `px = ndc.x * width; py = (1-ndc.y) * height` (film y flip); bounds-check tile.
3. Visibility: shadow ray sd.P -> camera_position(kg); reject if occluded.
4. Contribution = photon_flux * BSDF(sd, wi=toward camera) * We(camera) * G,
   where We is the perspective camera importance (needs calibration, below) and
   G folds the cos / distance^2 of the vertex-camera connection.
5. `idx = tile->offset + px + py*tile->stride`; atomic-add RGB into the COMBINED
   pass at `render_buffer + idx*pass_stride + combined_offset`.
Then terminate the photon path (same as the bake's diffuse terminate).

Because this splats into the beauty's combined pass every sample, it accumulates
and averages with the film's own sample count -> converges. The camera path
tracer runs normally in the SAME render and supplies everything else; the
light-tracing splat only carries CAUSTIC paths (>=1 specular bounce), which NEE
through specular can't find, so no double count (same reasoning as photon add).

## Calibration (the known-hard part)

We (perspective camera importance) must be normalized so a splatted caustic has
the correct absolute brightness relative to the path-traced direct lighting.
PBRT: We for a pinhole/thin-lens perspective camera = 1 / (A_film * cos^4(theta))
per unit area on the image plane, times the lens area factor. Plan: derive from
kernel_data.cam, then VERIFY against a control scene where the same caustic is
also reachable by the path tracer (e.g. a large area light making a soft
caustic the PT can partly resolve) -- match the light-traced splat to the PT
result. Same discipline as the photon 1.002 calibration. Until calibrated, a
FALCON_LIGHTTRACE_GAIN knob scales it (labeled lie, like the photon gain).

## Progress log

**2026-07-05 P1 scaffold — plumbing works, projection verified, NOT yet enough splats.**
Implemented: falcon_lighttrace.h (falcon_lt_project via cam.worldtoraster +
worldtocamera z-reject; falcon_lt_splat atomic-add into combined pass);
shade_surface.h branch (splat instead of deposit when kintegrator->falcon_lighttrace);
integrator.cpp env FALCON_LIGHTTRACE + _GAIN; data_template.h 2 members + 2 pad.
Driver: scratchpad/lighttrace_p1.py (camera-res render, film KEPT = caustic-only).
- Verified projection is GEOMETRICALLY CORRECT: splat centroid (640,538) matches
  Blender's own world_to_camera_view for the floor point under the glass sphere.
- **Two build/logic traps hit and fixed:**
  1. Camera-space forward is +Z in Cycles (camera.h:82 `focaldistance / D.z`).
     First reject `Pcam.z >= 0` killed everything in front. Fixed to `<= 0`.
  2. **Editing falcon_lighttrace.h did NOT trigger a kernel recompile** — ninja
     left the OptiX ptx stale (timestamp older than the source edit) yet reported
     success, so the "fixed" kernel that got synced was the old one (output
     byte-identical). Had to `touch kernel/device/{cuda,optix}/kernel.cu` to force
     it. This is a NEW facet of the known hand-sync trap: the falcon integrator
     headers aren't in the kernel depfile. **After ANY falcon_*.h kernel edit,
     touch kernel.cu before building, and verify the ptx timestamp > source edit.**
     (Symbol-grep doesn't work: ccl_device_inline funcs are inlined, no names.)
**2026-07-05 P2+P3 DONE — physical connection, We derived, absolute brightness calibrated. Real caustic renders correct at GAIN=1.**
- The P1 "only 1478 px" was a MEASUREMENT ARTIFACT (nonzero threshold 1e-6 too
  high). Raw EXR: 665k px actually lit, per-pixel ratios exact — projection and
  the z-fix were fine all along. Do not chase "stale kernel" on byte-identical
  output before checking the measurement threshold.
- falcon_lt_connect() now computes the physical splat:
    L = flux * (diff/pi) * (W*H) * SPP * cos_surf / (A * cos_lens^3 * dist^2)
  where A = image-plane area at unit camera distance (rastertocamera of the two
  raster corners, divided by z), cos_lens = forward-cosine of the vertex->camera
  ray (Pcam.z/|Pcam|), cos_surf = |N . to_cam|, dist = |vertex - camera|.
  TWO factors were missing in the scaffold and each was found by a separate
  control:
    (a) W*H (~1.2e6): splatting to ONE pixel means that pixel covers only
        1/(W*H) of the image-plane area A, so We = (W*H)/(A*cos^4). Adding it
        gave a uniform per-pixel x19200 = W*H/SPP (measured p10=p50=p90=19200).
    (b) SPP (=samples): the photon flux is normalized by the TOTAL photon count
        N = W*H*SPP, so the summed splats ARE the full image; the film's
        /sample_count averaging must be undone by *SPP. Found via the direct-
        floor control: without it the floor sat at 0.0006 = 0.159/265 ~ 1/SPP.
- CALIBRATION CONTROL (falcon_lt_direct / FALCON_LT_DIRECT=1): also splat the
  bounce-0 direct diffuse hit, whose floor radiance is analytic E*albedo/pi =
  5*0.1/pi = 0.159 (and PT-confirmed at 0.17). With (W*H * SPP) baked in, the
  light tracer's floor = 0.1578 at GAIN=1 (ratio 0.993). Units locked.
- RESULT: real glass-sphere caustic at GAIN=1 — max 0.175, a sharp SMOOTH ring
  (rim-refraction focal caustic) + center focus, NO point-map quantization dots.
  This is the whole reason for light tracing. Images: scratchpad/lt_final_t{1,10}.png.
  (Faint grain in the outer glow = undersampling, P4 will show SPP kills it; the
  faceted ring edge is the UV-sphere mesh, not the method.)
- Kernel fields: falcon_lighttrace / _gain / falcon_lt_direct / falcon_lt_samples.
  Env: FALCON_LIGHTTRACE=1 / _GAIN (labeled knob, default 1) / _SAMPLES (MUST =
  cs.samples) / FALCON_LT_DIRECT. Fixed (non-adaptive) sampling only.
- CODE TREE: edits + build live in Documents2/blender-5.1-src (build_blender_5.1),
  NOT this cycles-main tree (design doc only). Drivers: scratchpad lighttrace_p2.py
  (caustic), calib_direct.py (calibration), pt_reference.py (PT control).
**2026-07-05 P4 DONE — convergence proven.** SPP sweep (16/64/256), two
independent seeds each; RMS(a-b)/sqrt(2) over the shared caustic mask = per-image
MC noise. 4x SPP HALVES the noise, matching 1/sqrt(N):
  16->64  noise ratio 0.399 (ideal 0.500)
  64->256 noise ratio 0.509 (ideal 0.500)
Relative noise 0.105 -> 0.044 -> 0.023; seed-diff max 0.62 -> 0.16 -> 0.04 (two
independent renders converging = independent confirmation). Signal flat
(0.0275/0.0260/0.0258 = structure unchanged). This is the FQ payoff a frozen
point map cannot give (camera SPP does not touch a baked density estimate).
256spp vis shows the multi-ring focal structure cleanly; a few isolated
fireflies remain (high-variance samples off the tight focus) for the 4x-flux
clamp or the perf knobs to handle. Driver: scratchpad/lighttrace_p4.py.

**2026-07-05 (2nd session) RE-VERIFICATION — P1-P4 numbers reproduced on a
fresh build.** The previous session's Bash outputs were suspect (fabrication
incident), so everything was re-run from scratch: touch kernel.cu -> full ninja
rebuild -> hand-copy cubin.zst + kernel_optix.ptx.zst to runtime lib/ (PTX
timestamp verified > source edit). Fresh scene (8x8 floor albedo 0.1, sun E=5
straight down, 1280x960), driver scratchpad/lt_verify.py:
- PT reference floor = 0.15915 = E*albedo/pi EXACTLY (ratio 1.0000).
- Light-traced direct floor (FALCON_LT_DIRECT=1, GAIN=1) = 0.15927, ratio
  **1.0007** vs analytic. Absolute We calibration CONFIRMED (previous 0.993).
- P4 convergence, spp 16/64/256 x 2 seeds, RMS(a-b)/sqrt(2) over a FIXED mask
  (mask from the 256spp mean — a per-level mean-dependent mask skews both the
  signal and the ratios; the earlier 0.399/0.509 was that artifact):
  16->64 = **0.498**, 64->256 = **0.499** (ideal 0.500) — textbook 1/sqrt(N).
  Signal flat (0.00304/0.00299/0.00295). Frame-mean energy flat at all spp.
- Visual: 256spp glass-sphere caustic = sharp smooth ring + center focus,
  16spp = sparse dots -> SPP genuinely converges the caustic (the whole point).
  Images: scratchpad lt_caustic_vis_{16,256}spp.png, EXRs lt_caustic_s*_seed*.exr.
- Visibility ray vertex->camera: confirmed still ABSENT (shade_surface.h
  comment "No visibility ray yet") — next work item, plus the perf knobs.
- Commit status: the vertical slice IS committed — b0d0b9e4 (2026-07-05 11:22)
  contains all 4 files, working tree clean. The "still uncommitted" candidate
  below is stale.

**2026-07-05 P5a DONE — splat-reconstruction blur (first non-physical perf
knob).** `FALCON_LT_SPLAT_RADIUS` (px, default 0 = physical single-pixel):
spreads each splat over a Gaussian footprint (sigma = radius/2, clamped 9x9),
weights normalized over IN-BOUNDS pixels only -> exactly energy-preserving
(measured ratio 1.0000000 vs r=0, same seeds). Struct got 4 new members
(splat_radius + reserved falcon_lt_visibility + 2 pads, keeps multiple-of-4).
Measured at 16spp on the glass-sphere scene, fixed 256spp mask:
  r0 noise 0.01175 / r2 0.00248 (4.7x less ~ 22x SPP) / r3 0.00148 (7.9x ~ 63x).
  16spp + r2 has LESS noise than 256spp physical (0.00292) — the knob buys
  >=16x SPP on the caustic. Visual: ring + focus read smooth at 16spp.
Honest cost note: splat atomics x25 make 16spp r2 (1.5s) ~= 256spp r0 (1.4s)
on THIS trivial scene where tracing is free; the win is on heavy scenes where
per-sample photon TRACING dominates (16x fewer samples = real savings).
Prior-art check: LuxCore force-disables the pixel filter for light tracing
(github BlendLuxCore b35fa12) — same physical default, blur strictly opt-in.
Driver: scratchpad lt_blur_ab.sh / lt_blur_analyze.py.

**2026-07-05 P5b DONE — visibility ray vertex->camera (FALCON_LT_VISIBILITY,
commit b5505763).** Occlusion test before splatting, MNEE probe-ray pattern
(self.object/prim excluded, PATH_RAY_CAMERA visibility, tmax*0.9999).
THE KERNEL-VARIANT TRAP AND ITS FIX: scene_intersect from plain shade_surface
runs from the CUDA cubin on an OptiX device -> it would walk a BVH2 that does
not exist. So the trace is compiled ONLY into the raytrace variant via
IF_KERNEL_FEATURE(NODE_RAYTRACE), intersect_closest routes ALL shading to
DEVICE_KERNEL_INTEGRATOR_SHADE_SURFACE_RAYTRACE while the knob is on (4 sites,
`use_raytrace_kernel |= falcon_lt_visibility`), and
Integrator::get_kernel_features() adds KERNEL_FEATURE_NODE_RAYTRACE so the
pipeline/sort counters exist. Verified:
- Calib control WITH visibility: floor ratio 1.0007 unchanged -> no false
  self-occlusion on the open floor (MNEE-style self exclusion suffices, no
  P offset needed), and the raytrace-variant routing itself is sound.
- Occluder scene (0.9 m plate between camera and caustic, 64spp): removed
  splats = 28,681 px, of which 99.94% inside the plate's projected quad or the
  glass sphere's silhouette (17 px outside = compiled-variant numerical
  divergence on knife-edge TIR photons, spike-scale, ~1e-7 of photons; same
  cause as 6 px where vis1>vis0). Kept region: mixed pixels legitimately keep
  plate-front splats while losing floor-behind splats.
- Killing through-glass splats is CORRECT: those L-S-D-S-eye paths cannot be
  placed by light tracing (design section above); vis=0 painted them wrongly
  onto the sphere's own pixels.
- Cost: occluder 64spp 1.5s(vis0, plain kernel) vs 0.9s(vis1, raytrace kernel;
  fewer splats + trace = net FASTER here). Driver: scratchpad lt_vis_ab.sh /
  lt_vis_analyze.py / lt_vis_geom.py.

**2026-07-05 P6 DONE — PM-vs-LT A/B, panel wiring, real-scene E2E.**
- A/B vs the point map (same scene, 78.6M-photon PM bake r=0.015 / LT):
  16spp->256spp STRUCTURE change PM rel 0.02 (frozen density estimate — camera
  SPP does nothing) vs LT rel 4.11 (actively converging). Visual: PM ring is
  thick/soft with density-estimate mottle; LT 256spp ring is razor-thin. Bonus
  visible in the PM layer: through-glass caustics glow in the sphere body
  (L-S-D-S-eye) — the exact case LT correctly kills and PM/MNEE own.
  Complementarity proven in one image pair. Images: obs2 picture/
  2026-07-05_LT-AB_*.png; driver scratchpad lt_ab_pm.sh / lt_ab_analyze.py.
- Panel wiring (commit 248a4253): cycles.falcon_lighttrace_render = per-light
  LT caustic passes + beauty render + additive composite ("Falcon LT合成"
  image + EXR). Panel: ぼかし(falcon_lt_blur)/ゲイン(falcon_lt_gain)/可視性
  (falcon_lt_visibility, default ON). Sun-target extracted to
  _falcon_sun_target (shared with photon bake). Guards: CYCLES-only, <=4096px
  (single-tile splat), SUN/AREA/SPOT lights, adaptive forced off + restored,
  env canary-tested. E2E: composite == beauty + LT layer to float32 eps.
- Real-scene E2E ("grass costic test .blend", 13 meshes, SPOT, 1920x1080,
  64spp, blur 1.5, gain 4): operator FINISHED in 5.1s total. LT layer shows
  sharp filament caustics (disc arcs, suzanne fan, gem facet flashes, torus
  ring). Residual speckle at 64spp = undersampled halo (gain 4 amplifies);
  production = more SPP (FQ) or more blur. Note: FALCON_PHOTON_TARGET is
  sun-only; spot cones already aim themselves. Images: obs2 picture/
  2026-07-05_LTパネル_gct_*.png.

The P1-P4 vertical slice is complete: light tracing works end to end (project,
physical We, calibrated units, convergent). NEXT candidates:
- NON-PHYSICAL performance knobs (splat-reconstruction blur so FEW photons look
  smooth / photon reduction / bounce cap) -- the direct answer to "must render
  on heavy scenes; physics-purity is negotiable" (2026-07-05). Most aligned.
- Visibility shadow ray vertex->camera (needed once occluders sit between the
  caustic and the camera; the open-floor test does not need it).
- Direct A/B vs the point map on one scene (visualize that its dots do NOT drop
  with SPP while light tracing does).
- Commit the vertical slice (4 files in blender-5.1-src, still uncommitted).

## Phases

- P1 scaffold: emit photons, first diffuse hit, project + splat a CONSTANT color
  (no visibility, no BSDF, no We). Verify: pixels light up at the geometrically
  correct place (caustic-shaped blob where the glass focuses). Plumbing only.
- P2 visibility + BSDF + flux: real photon flux * BSDF, shadow ray to camera.
  Verify: caustic appears in the right place with the right shape, brightness
  uncalibrated (use GAIN).
- P3 calibrate We against a PT control; drop GAIN to 1 physical.
- P4 converge/quality: confirm SPP reduces noise (the whole point), no dots;
  compare to the point-map render on the same scene.

## Open risks

- Splatting into the combined pass mid-render may fight adaptive sampling /
  denoiser assumptions -> P1 uses fixed sampling, denoise off (FQ anyway).
- Multi-tile: splat target may be in a DIFFERENT tile than the emitting photon's
  home. For a single-tile render (<=4096, the bake already forces one tile) this
  is a non-issue; large frames need per-tile bounds rejection (splat dropped if
  outside the current tile) or a full-frame splat buffer. FQ can render one big
  tile; revisit only if needed.
