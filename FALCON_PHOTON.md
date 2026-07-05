# Falcon Photon Cache — design (2026-07-02, Round 2 of the glass track)

Goal ("リアルときれい"): give CyclesF the caustics MNEE cannot do — reflective
caustics (ring/mirror: completely absent, measured zoo round 1) and correct
through-water SDS energy — without importing BiDir/Metropolis.

Measured grounding (glass_zoo, obs2 2026-07-02 note):
- BiDir+Metropolis 600s leaves the pool bottom black (0.05): BiDir cannot do
  SDS. LuxCore's water magic is **PhotonGI photon merging**, not BiDir.
- Unbiased PATH long-run pool luminance ≈ 0.36 (the energy referee).
  MNEE = 0.63, LuxCore PGI default = 0.90 — both bright-biased. Any photon
  cache must be energy-checked against the unbiased PATH value.

## Architecture (SHARC plumbing reused)

1. **Photon pass (host)**: emit photons from lights, follow specular chains
   (refract/reflect, Fresnel-RR), deposit at the first diffuse hit — but only
   for paths with >=1 specular bounce (caustic photons only; direct L->D is
   the path tracer's job, depositing it would double count).
2. **Storage**: the existing SHARC hash grid (hash_uint3 lookup3, 4M cells,
   stride 4 = RGB + count). Host converts per-cell flux to outgoing radiance
   L = sum(flux) * albedo / (pi * h^2) and writes count=1 (validity flag), so
   the existing mean-lookup returns L exactly. Cell size 0.2 is known-coarse
   for caustic detail (SHARC Step3 LOD debt); proof accepts it, product needs
   finer cells or per-cell area estimation.
3. **Lookup (kernel)**: v1 proof reuses the SHARC blend path with alpha=1 for
   visualization (camera-first-hit only; through-water checks render with the
   water hidden). Product patch: additive mode at ANY diffuse vertex —
   film_write_combined_pass(+L_cache * throughput) without the (1-alpha)
   throughput scale, gated to caustic-flagged receivers to avoid double count
   with MNEE (MNEE stays for refractive-direct, photons cover the rest).

## Proof tracer (this round)

tools/falcon_photon_trace.py — runs in Blender's Python on the open scene,
scene-generic ray casting via depsgraph ray_cast; specular behavior for the
zoo is name-mapped (glass/water = refract IOR 1.45/1.33, mirror/gold =
reflect) with analytic smooth normals (sphere/torus/ripple heightfield) since
ray_cast returns flat polygon normals. Emission: cosine-weighted cone toward
the scene, photon power = P_light * cos(theta) * Omega / (pi * N).
Output: FALCON_SHARC_CACHE-format bin, rendered with FALCON_SHARC_MODE=blend
ALPHA=1 GATE off.

Success criteria: ring reflective arc + mirror patch + pool bands visible in
the visualization; pool energy within ~2x of PATH referee (calibration knob
allowed; exact match deferred to the product integration).

## Round 5: calibration, generalization, UI (2026-07-03)

- **Energy calibrated**: flat-floor referee scene (direct-deposit mode) exposed
  a clean 0.50x loss — deposits on flat receivers sit exactly on cell
  boundaries and centered trilinear splatting threw half the flux into the
  cell behind the surface. Fix: nudge deposits half a cell along the hit
  normal. Calibration ratio after fix: **1.002**. The earlier "2.2x hot" area
  light was this half-loss composed with the pre-R5 albedo/area assumptions.
- **Per-deposit albedo** from the receiver's Principled Base Color (socket
  default; textured materials use their flat component).
- **Specular classification generalized**: Principled sockets (Transmission
  Weight > 0.5 + smooth -> refract with material IOR; Metallic > 0.5 + smooth
  -> mirror). Zoo name-map kept as fallback. Regression: zoo + ocean deposit
  counts unchanged.
- **Gaussian splat** (--smooth, cells): sparse-cell shot noise ("sparkles") is
  not an outlier problem (p99.5 clamp: no visible change) but under-sampling;
  a wider deposit footprint trades a little filament sharpness for smoothness.
- **UI**: CyclesF panel section コースティクス (Falcon Photon) — photons /
  cell / dispersion props + "コースティクスを焼く" (runs the bundled
  addon/falcon_photon.py on the open scene, writes the cache to tempdir, sets
  the env for subsequent renders) + disable button. The tracer ships inside
  the cycles addon; tools/falcon_photon_trace.py remains the dev copy (keep in
  sync).
- **Referee honesty note**: for through-water SDS the unbiased PATH referee
  UNDERESTIMATES (it cannot find the transport — the reason this feature
  exists). Ocean energy validated against the analytic layer instead:
  sky + E*cos(zenith)*T_fresnel*albedo/pi. Photon layer lands ~0.9-1.2x of
  analytic; the transparent-shadow fake attenuates too aggressively (0.21 vs
  0.55 analytic total) and is not a usable referee either.
- **EEVEE trap** (burned an hour): a .blend saved without
  scene.render.engine='CYCLES' renders EEVEE headless — glass/water/mirror go
  black (no raytraced transmission) and it looks exactly like a kernel
  regression. Scene builders must set the engine explicitly.

Known limits (v2 candidates): Python tracer speed (16M photons ~= 25 min;
C++/GPU port for animation), lookup-time neighbor gather as the proper
sparkle fix, HDRI/multi-light emission, per-frame animation caching.

## Round 6: GPU bake finished (2026-07-03)

The GPU photon bake (FALCON_PHOTON_MODE=bake, a one-sample "fake render" whose
init_from_camera emits light photons instead of camera rays) is now finished.

- **Area-light calibration (item 1): not broken, within tolerance.** The bake
  emits P/N per photon over a cosine hemisphere (watts = the light's power in W,
  so no area/pi factor is needed, unlike the sun path which starts from
  irradiance). Measured on the zoo against the CPU tracer (the calibrated 1.002
  reference), same alpha=1 water-hidden pool measurement: GPU 0.987 vs CPU 1.250
  = **0.79x**, the same order as the sun path's 0.566/0.596 = 0.95x. The 21% is
  the cosine-hemisphere-vs-cone emission distribution (same total energy,
  different density), not a formula error -- a magic gain would hide physics, so
  none was added. (Note: the 0.36 PATH referee is the full through-water
  transport, a different quantity than the alpha=1 caustic-only lookup; do not
  compare the two directly.)
- **Kernel splat (item 2): done.** falcon_photon_deposit was nearest-cell only;
  the CPU tracer's tangent-plane gaussian splat (splat_trilinear: gaussian in
  the two tangent axes, trilinear along the normal axis, per-photon wsum
  normalization) is now ported into the kernel (falcon_sharc.h), with the
  deposit normal axis passed from shade_surface.h. inv_area/albedo/half-cell
  offset were already in the GPU path, so the splat was the only gap. Verified
  on the zoo (total-variation smoothness of the alpha=1 visualization): full
  floor 0.0591 -> 0.0438 (CPU ref 0.0436, i.e. GPU now matches CPU), pool region
  0.0785 -> 0.0275. Salt-and-pepper -26% overall, -65% in the pool.
- **UI GPU toggle (item 3): done.** cycles.falcon_photon_gpu drives the bake
  operator's GPU branch: it renders a square photon frame sized to the photon
  budget (res = sqrt(photons), samples=1) with the bake env set, saves the
  cache, restores the render state, and sets add mode. Falls back to the CPU
  tracer when off. GPU has no chromatic dispersion (CPU-only) -- noted in the
  UI. Verified headless (operator FINISHED, cache written, state restored).

## Round 7: lookup gather + collision tags (2026-07-03 night)

The two v2 quality debts are closed. Verification harness: bake a real 16M GPU
cache on the ocean, then (a) numpy A/B of the old vs new lookup on the same
cache over a seabed sample grid (the lookup change in isolation), (b) render
base/add pairs and measure the photon layer (top-down rows; note bpy image
pixels are bottom-up and must be flipped).

- **Symmetric lookup gather** (falcon_photon_lookup): reads the same 2x2x2
  cell-centered stencil as the splat with the same tangent-gaussian/normal-tent
  weights, averages valid cells, renormalizes by their weight sum. The caller
  passes the SAME half-cell normal-offset point used at deposit time — the old
  nearest-cell lookup read the un-offset surface point, which mismatched
  deposits near cell boundaries (and missed entirely for back-facing layers at
  exact boundaries). Measured on the seabed grid: energy mean 0.29883 -> 0.29881
  (neutral), sparkle p99.9/p50 4.82 -> 3.83 (-21%), TV -12%, coverage 0.900 ->
  0.907. Render: seabed sparkle visually gone, filament network intact.
- **Collision tags** (falcon_photon_tag): hash_uint3 picks the cell, so distant
  world cells share slots and a lookup on one reads the other's flux — the
  floating bright squares in the sky above the ocean (1364 px > 0.05 in the
  720p layer). A second input-decorrelated hash of the grid coords is stored in
  the count field as 1.0 + tag * 2^-20 (exact in float; still passes the >= 1
  validity test elsewhere, and the SHARC mean-lookup divide is off by at most
  1e-6, photon caches are never read through it anyway). Deposit writes the
  tag (only for w > 0 writes — a zero-weight write must not steal the cell's
  tag); lookup recomputes and rejects mismatches, and the gather fills the hole
  from valid neighbors. **Sky false positives 1364 px -> 0**, seabed energy
  0.3144 -> 0.3128 (-0.5%, neutral), sparkle/TV/coverage unchanged.
- Host tracer writes the same tags (cell_tag_np; numpy XOR must be applied in
  uint32 BEFORE the uint64 cast to match the kernel). Three tracer copies
  synced: tools/ (dev master), blender addon source, runtime addons_core.
  **Old caches are invalidated** by the tag check (count=1.0 never matches a
  tag) — rebake, it is 2 s on GPU.
- The port patch (blender_5.1_sharc_port.patch) previously MISSED the new
  untracked files (falcon_sharc.h, falcon_photon.py) because `git diff` skips
  untracked paths; fixed with `git add -N` before regenerating. The patch is
  self-contained now.

Remaining v2 candidates: slant-area factor (h^2/cos underestimates slanted
receiver area -> slight overbrightness on slopes), HDRI/multi-light emission,
per-frame animation caching.
GPU dispersion: DONE (2026-07-04, see FALCON_DISPERSION.md; the panel Dispersion
slider drives it on the GPU bake).

## Round 8: photon-map density estimation (2026-07-04)

The 2x2x2 binning splat's known LOD debt became the blocker for hero dispersive
caustics: with dispersion on, sparse deposits print as hard COLORED cells (the
noise is now chromatic and far more visible), and no photon count fixed it
(measured to 200M). Root cause = fixed-grid binning, not RGB or photon count.

Fix (validated offline first -- CPU photon dump `--dump-points` -> numpy KDE ->
render -- before touching the kernel): fixed-radius kernel density estimation.
falcon_photon_deposit_wide spreads each photon over a tangent-plane 2D cone of
radius = falcon_photon_radius cells (norm 3/(pi r^2), 1-cell normal tent), so
the accumulated field is smooth. The existing 2x2x2 lookup interpolates it
unchanged. Panel "Caustic Smoothness" (FALCON_PHOTON_RADIUS, default 3), env-
driven through the bake operator; radius<=1 = legacy splat. Energy within ~6% of
legacy on the gem scene; MAIN caustic is now smooth instead of blocky.
Also fixed the CPU tracer's sun emission (horizontal-footprint launch missed
non-vertical suns -> 0 deposits; now a plane perpendicular to the sun).

KNOWN LIMIT (the next task): peripheral rainbow RAYS stay under-sampled (dotty/
streaky). Fixed radius cannot be both sharp at the focus and solid in the sparse
rays -- large radius blurs the focus without filling the rays, and the streaks
ARE the image-#6/#7 rays, just too few photons each. The real fix is ADAPTIVE
(kNN) radius: sparse ray regions grow the radius to fill solid, the dense focus
keeps a small radius sharp. That needs either a 2-pass density (count grid ->
per-photon radius) or a lookup-time kNN gather (store per-cell photon count +
raw flux, grow the lookup radius until k photons). Both change the cache format
(stride 4 -> +count) and are a real redesign -> Round 9.

### Build gotcha (cost real time; record for next kernel edit)

This Blender 5.1 build is configured WITH_CYCLES_CUDA_BINARIES=ON, so the CUDA
cubin is a build-time artifact (kernel_sm_86.cubin.zst), not a runtime nvrtc
cache. `ninja install` does NOT copy the freshly built cubin OR the kernel
source into the runtime datafiles -- the runtime keeps loading the stale
bin/5.1/scripts/addons_core/cycles/lib/kernel_sm_86.cubin.zst (and .../source/
kernel/*.h). After a kernel edit you must manually copy, or the change silently
never runs:
  cp intern/cycles/kernel/device/cuda/kernel_sm_86.cubin.zst \
     bin/5.1/scripts/addons_core/cycles/lib/
  cp intern/cycles/kernel/device/optix/*.ptx.zst \
     bin/5.1/scripts/addons_core/cycles/lib/          # OPTIX TOO (see below)
  cp intern/cycles/kernel/integrator/{falcon_sharc,shade_surface}.h \
     bin/5.1/scripts/addons_core/cycles/source/kernel/integrator/

OPTIX IS A SEPARATE ARTIFACT SET: the OptiX kernels ship as
kernel_optix*.ptx.zst next to the cubin; syncing only the cubin leaves OptiX
renders on the stale kernel. CUDA-only verification is blind to it — verify
once on OPTIX after any kernel edit.

POST-MORTEM of the broken 48-frame run (2026-07-03 night; two traps stacked,
one misdiagnosed): the run's actual killer was the KNOWN EEVEE trap — the
driving script omitted scene.render.engine='CYCLES' and ocean_caustics.blend
is engine-unsaved, so every bake AND render ran EEVEE. Signature to
recognize it FAST next time: bake finishes in ~1.3 s instead of ~6 s (the
operator's internal 4000^2 render runs as an EEVEE frame), NO cache file is
written (the host save-hook is Cycles-only), per-frame "caches" are
byte-identical because the previous file just survives untouched, renders
are ~0.2 s with water/glass black. The stale-PTX theory fit the same
symptoms but was wrong — disproved by a CUDA control run failing
identically; the decisive test was a fresh-scene bake with the cache file
DELETED (no file appeared = the bake never deposited on any device). Scripts
must set the engine explicitly, always; the bake operator should probably
hard-fail on non-Cycles engines (future hardening).
The same install-sync gap applies to the Python addon (properties/operators/
ui.py) -- copy those to bin/5.1/scripts/addons_core/cycles/ too.

## Round 9 DESIGN: LuxCore-faithful point-based photon map (2026-07-04)

User goal locked (image #15 diagram + "Aで"): CAUSTICS are the hero, spectral is
the coloring layer; target = dramatic radiating caustics (image #6 / #15-left),
dark stage. The fixed-radius GRID density estimation (Round 8) is a memory-cheap
approximation whose quality ceiling below LuxCore is (a) hash collisions (distant
world cells share slots) and (b) cell quantization. Verified LuxCore's actual
caustic algorithm from source (LuxCoreRender/LuxCore
src/slg/engines/caches/photongi/): caustic photons stored as POINTS
(std::vector<Photon>: pos, dir, flux/alpha, NORMAL) in a BVH; lookup =
FIXED-radius density estimation (params.caustic.lookUpRadius; the SPPM
radiusReduction/minLookUpRadius is the INDIRECT cache, NOT caustic) with
NORMAL-ANGLE rejection (Dot(photon.n, n) > cosLookUpNormalAngle); cache capped at
maxSize (a few M points), many photons traced. So the fix is not adaptive radius
-- it is point storage + normal rejection + enough photons.

Reproduction plan in the Cycles bake architecture (uniform grid replaces the BVH
for neighbor search -- simpler, same result):
- Storage: capped point buffer (~12M photons x [pos3, flux3, normal3] = 432 MB).
  Bake deposit atomic-appends (atomic_fetch_and_inc_uint32 a counter; skip if
  full) instead of splatting into the sharc grid.
- Host post-bake: read back points+count, build a uniform neighbor grid
  (cell = lookUpRadius): counting-sort photon indices into per-cell ranges
  (grid_start[], grid_count[], photon_index[]); upload.
- Kernel lookup at a diffuse hit: compute cell, loop 3x3x3 neighbor cells, for
  each photon test dist<r AND Dot(n_photon, n)>cosAngle, accumulate
  flux*cone(dist/r); L = sum / (pi*r^2). No hash collisions (per-cell index
  ranges), no quantization (continuous photon pos).
- Params (data_template): store flag, max_points, lookup_radius, cos_normal,
  traced_count (flux norm), grid origin + inv_cell + res.

Two build forms (both valid):
  (A) OFFLINE first (no kernel rebuild, fast iteration for stills): dump points
      (--dump-points exists; add the normal), host numpy point-KDE with normal
      rejection into a fine cache, render through the existing lookup. Already
      validated smooth in Round 8 Milestone A (kde_fixed.png).
  (B) FULL GPU: the point buffer + neighbor grid + kernel gather above (fast
      bakes, the product form).

### Round 9 Milestone A: offline surface-KDE proof = PASSED (2026-07-04)

Harness (tools/): ring_build.py (glass-ring scene + falcon_torus prop),
falcon_photon_trace.py --dump-points (now also classifies Glass/Glossy NODES,
not just Principled, and takes analytic torus normals from a falcon_torus
custom prop), kde2d_bake.py (the estimator under test), ring_render.py
(add-mode render driver, cache "none" = base), compare_layers.py (layer =
arm - base, adaptive off, same seed; energy/sparkle/TV/chroma metrics).

- 16M emitted -> 2.35M caustic points on the floor (CPU trace ~4 min,
  --dispersion 0.03: every photon single-channel, R/G/B balanced).
- **Trap found: hashed 3D KDE splat is unusable.** The old kde_bake.py
  (Round 8 prototype) splats r=3-cell 3D cones straight into the 4M-slot hash:
  97% occupancy, colliding cells MERGE flux (np.add.at) -> total energy x3.5.
  Also the 2D-cone-normalized kernel stacked over dz overcounts vertically.
- Fix = kde2d_bake.py: dense tangent-plane 2D grid over the hero box (|x|,|y|
  <= 5 m) aligned to the global lattice, per-photon 2D cone (norm 3/pi r^2),
  then ONE hash write per nonzero cell, collisions resolved brighter-wins
  (kernel tag semantics), energy calibrated to the grid-arm cache total
  (scale printed; ~0.318 ~= the 1/pi the raw-flux dump is missing).
- **Equal-photon, equal-energy A/B (render totals within 2%)**:
  grid splat TV 0.186 / chroma_mean 0.326; KDE r=0.018: TV 0.101 / 0.225;
  KDE r=0.045: TV 0.070 / 0.162. Visual: colored confetti GONE, smooth wash;
  the nephroid fan + left tail stay sharp at r=0.045; residual = faint
  low-frequency color mottle in sparse regions (photon-count limited).
  Dense-region interiors sum to near-white (physically right: rainbow lives
  at fold edges, not inside the fan).
- Hash still loses cells at write (r045: 518k of 2.27M collide -> holes filled
  by gather; 6% energy) = exactly the ceiling point storage (B-form) removes.
- **Scaling check (64M emitted -> 9.4M points, ~16 min CPU)**: r=0.030 at 64M
  matches r=0.045 at 16M smoothness (TV 0.078 vs 0.070, chroma 0.153 vs 0.162)
  with a 1.5x sharper focus; r=0.045 mottle 0.162 -> 0.140. More points buy
  sharp AND smooth together, slowly (~sqrt). 9.4M points ~= the planned B-form
  GPU point-buffer cap, so the 64M renders are the product-quality preview
  (bright16_kde64_030.png; judgment set saved to obs2 picture/
  2026-07-04_round9A_*.png).
- **Gain gap found**: FALCON_PHOTON_GAIN is read ONLY in the bake branch
  (integrator.cpp:447, folded into photon flux at bake time); add-mode cache
  load has no gain, so offline caches render at gain 1 and the env knob is
  dead for them. Milestone A worked around it by scaling the .bin (x16).
  **B-form requirement: gain must be a lookup/composite-time knob** (the
  approved "presentation lie" must not require a rebake).
- VERDICT: mechanism proven on equal photons + equal energy; the residual
  color mottle is photon-count-limited, not estimator-limited. GO for B-form
  (GPU point buffer + neighbor grid + kernel gather + normal rejection +
  render-time gain/radius).

### Round 9 Milestone B (GPU point map) — session log 2026-07-04 evening

Implementation (blender-5.1-src, ALL of it): kernel point append during bake
(falcon_photon_point_append, atomic counter, stride 9 = pos/flux/normal) +
host neighbor grid (counting sort, cell = lookup radius, hash slots, collisions
resolved by the distance test — no tags needed) + kernel gather lookup
(falcon_photon_point_lookup: 3x3x3 cells, cone kernel, LuxCore-style
normal-angle rejection) + RENDER-TIME knobs FALCON_PHOTON_RADIUS_M /
FALCON_PHOTON_NORMAL_DEG / FALCON_PHOTON_GAIN (gain finally works in add mode;
the old env only ever applied at bake). Point file: FALCON_PHOTON_POINTS =
{magic FPH1, uint32 count} + count*9 floats; saved by the path_trace bake hook.

THREE structural bake bugs found while bringing it up (all pre-existed Round 9;
they explain the "GPU bakes plateau / photon counts don't help" history):

1. **Huge photon frames get tiled and the tile re-uploads wipe device
   accumulation.** res = sqrt(photons) at 1 sample (the operator's scheme)
   made 600M-1B photon bakes render 24k^2-31k^2 frames -> tiling. Fix (driver):
   res capped 4096, photons come from SAMPLES (res^2 * samples), one tile.
2. **Sun emission sprayed the whole scene footprint.** The 40 m floor bbox ate
   99.9% of photons as bounce-0 floor hits (the CPU tracer got the
   perpendicular targeted launch in Round 8; the GPU never did). Fix (host,
   integrator.cpp): FALCON_PHOTON_TARGET="cx,cy,cz,r" shrinks the launch
   square to the specular target's bounding sphere, center projected UP the
   sun direction onto the launch plane (diagonal travel), radius * 1.15 /
   cos_zen. 644x more deposits on the ring scene.
3. **The diffuse-receiver test compared FRESNEL-WEIGHTED closures**
   (d_avg >= s_avg with evaluated closures): at grazing incidence every
   dielectric floor turns "specular", so shallow far-flying caustic rays NEVER
   deposited (hard r < 1.6 m cutoff on the ring scene; CPU reference says 25%
   of the flux lands beyond that). On a DARK floor (albedo 0.06) even
   mid-angle hits lost to the Fresnel term -> the chronic photon starvation of
   every dark-floor hero scene. Fix (kernel): deposit if d_avg > 0 (any
   diffuse component), pure speculars continue, dead surfaces terminate.
   Disproved along the way: bounce caps (24 vs 64 = bit-identical counts) and
   the dispersion hook (B=0 identical) were NOT the eaters.

B-form E2E results (same ring scene, layer metrics vs the fixed-bake grid arm):
- 134M emitted -> 1.7M pts: r=0.03 sparkle 5.66 (grid 7.02, best yet), smooth
  wings; r=0.015 crisper but sparse wings go dotty.
- 1B emitted -> 13.7M pts (bake 8.8 s): r=0.015 wings become defined curved
  filaments (the "crisp radiating" target), residual dotting only at the far
  tail; r=0.010 slightly sharper still, sparkle 14.6.
- Radius is now the sharp/smooth knob and needs NO rebake; photons fill
  whatever the radius can't smooth. Next photons step = MAXPTS 30M + 4B.
- COST (the open Milestone C item): the gather runs at every diffuse vertex
  every sample -- r=0.015/13.7M pts = 646 s at 500 spp 1600x900 (grid add:
  8 s). Obvious fixes: gate to camera-first diffuse hit, or bin lookups into
  a per-pixel cache; not done this session.
- Port patch regenerated with all point-map changes (28 files, self-contained).

### Spot-light photon emission (2026-07-04 night) — bake now covers SUN/AREA/SPOT

User scene "grass costic test .blend" (7 glass objects, SPOT light) forced the
long-standing gap. Kernel (init_from_camera.h): uniform solid-angle cone
sampling around klight->spot.dir (dir = the light's travel direction — light.cpp
stores -Z of the lamp transform; sampling formula cos_theta = 1 - u*(1 -
cos_half)). Host (integrator.cpp): flux = watts * Omega/(4 pi) / N with Omega =
2 pi (1 - cos(spot_angle/2)) — a Blender spot is a point light (intensity
W/4pi) clipped to the cone, so no magic gain. E2E on the user scene: 1B photons
-> 1.94M pts in 7 s; 4.29B -> 7.75M pts in 15 s; add-render overhead vs plain
PT only +4-14 s at 200 spp 1080p (2-8M pts is much lighter to gather than the
ring's 13.7M). Caustics appear inside every object's shadow (cone/torus/rings/
cylinder) at gain 4; judgment set in obs2 picture/2026-07-04_round9B_gct_*.png.
Open questions from this scene: Suzanne's shadow stays dark (complex geometry
spreads its photons — likely genuine, worth a normal-angle sweep), and bright
slivers near the diamond's rim need a closer look (real gem caustics vs leak).

### Panel wiring for the point map (2026-07-04 night) — product path complete

Addon (properties/operators/ui.py, synced to runtime): 点マップ toggle
(default ON, GPU-only) + 半径(m) + ゲイン sliders in the コースティクス
section. The sliders carry update callbacks that rewrite
FALCON_PHOTON_RADIUS_M / FALCON_PHOTON_GAIN live when a bake is active =
**retune without rebaking, straight from the panel**. Bake operator hardening
rolled in: res capped 4096 + photons carried by samples (tiling counter-wipe
fix in the product path), auto FALCON_PHOTON_TARGET for sun scenes (bounding
sphere over objects whose first material is refractive/metallic), bounce caps
raised to >=32 during the bake (TIR chains), hard-fail on non-Cycles engine
(EEVEE trap), photons max raised to 2B. E2E-verified headless via the
operator on the user's glass scene: bake 8.6 s (1B), live slider -> env
confirmed, render picks up the point map. Also verified live: A/B of
use_transparent_shadow on pure-Glass scenes is a NO-OP (only Transparent BSDF
nodes pass shadow rays — Cycles glass already blocks them), so the
shadow/caustic separation the user asked about already holds architecturally;
Round 3's double-count applied to Transparent-node water shaders only.

### Photon flux clamp — permanent-firefly fix (2026-07-04 late night)

Chasing the reference cushion-block caustic (user scene, Cube.001) exposed
white dots in the photon layer that survived sample_clamp_indirect=1 (so not
PT fireflies). Cache forensics on a 40M-point bake: flux p50 2.4e-7 vs max
3.7e-2 = a 150000x outlier population. Root cause: the bake continues
specular chains via regular BSDF sampling, so a microfacet eval/pdf spike
(the standard PT firefly mechanism) lands in a photon's throughput and gets
BAKED into the cache as a permanent bright dot at whatever radius the lookup
uses. Physical fact: along a specular chain photon flux can only shrink
(Fresnel/albedo <= 1). Fix (shade_surface.h deposit block, applies to grid
AND point paths): clamp per channel at 4x emission flux (headroom for
dispersion's single-channel packing). Verified on the cushion scene: dots
gone, real thin filament arcs remain. Likely also the true identity of the
older "残存白点" seen in grid-era bakes.

Cushion-repro findings (art side, for the GUI session): at 8 deg light
elevation the entry-face incidence is ~82 deg and Fresnel reflects almost
everything -> the through-body strip starves no matter how many photons; at
~20 deg the strip filaments appear. The reference's dense bright strip is a
light-elevation/orientation coupling question — iterate interactively with
the panel's live radius/gain sliders (the tech is no longer the bottleneck).

## CyclesFQ split + multi-light bake (2026-07-05)

Project reframed: **CyclesFQ** (quality-first, stills; load everything on, bake
time ignored) is built first, then forked to **CyclesFF** (fast, animation) by
stripping what FQ proves droppable. This is the forced stop-point the caustic
track lacked. GitHub push deferred.

- **Multi-light bake (was: first enabled light only).** The GPU bake selects
  scene->lights[0], so a scene with N lights only ever cast one light's
  caustics. Fixed at the operator level: each visible light bakes INDEPENDENTLY
  to its own file (photon budget + point cap split by light energy), then
  _falcon_merge_points concatenates the per-light .fph files (or
  _falcon_merge_grid sums the grid .bin caches for the point-map-off path).
  hide_render isolates one light per sub-pass to match the kernel's [0] pick.
  Verified: two spots on opposite sides of a glass sphere -> both caustic
  directions present in one bake (previously only one).
  - **DEAD END recorded: on-device accumulation crashes.** First tried keeping
    ONE device buffer across the per-light render() calls (FALCON_PHOTON_ACCUMULATE
    env skipped the copy_to_device/reset for lights 2..N). Second render() call
    reliably crashed: "Illegal address in CUDA queue copy_from_device
    (integrator_sorted_paths_array prefix_sum)". device_vector state is NOT safe
    to keep live across separate render invocations. Two FULLY INDEPENDENT bakes
    (each a full reset) in one process are fine -- so file-level merge, not
    on-device accumulate. integrator.cpp keeps `accumulate = false` hardwired.
- **New panel knob: 点上限 (falcon_photon_point_maxpts, default 40M).** Point cap
  is now user-facing (was hardcoded 16M). FQ default is generous since bake time
  isn't the bottleneck; per-light caps are this value split by energy.
- Env-driven diagnostics unchanged; render/bake still GPU (OptiX). Confirmed OIDN
  2.4.1 with the CUDA device backend is what actually loads, so GPU denoise is
  already the effective path (preset already sets denoising_use_gpu).

SEPARATE finding (art, not tech): the gem-on-dark-floor caustic was DIM
(rawmax 7.4, photon-INDEPENDENT 300M->500M) -- a faceted gem spreads light thin,
it does not concentrate into bright rays. A smooth sphere concentrates (rawmax
13844) but to a point. A DRAMATIC bright caustic needs a light-concentrating
geometry (lens/cardioid/water ripple), found by art, PRIOR to photon-map
quality. Solve geometry + point-map together.
