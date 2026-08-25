# Weaknesses and when to use what

*[日本語版はこちら / Japanese version](WEAKNESSES.md)*

These are the limits we know from measurement. We write them down here rather than let you
find them out after buying. **All the numbers are measured here** (GPU: RTX 3080). The scene
differs per item, so each item states which scene it was measured on.

## Choosing a denoiser

| Situation | Recommended | Reason |
|---|---|---|
| Overall fidelity of a still | **OIDN** | 8.5dB higher in PSNR than DLSS. Edges come out straight, and the grain of textures is kept |
| Animation (sequences) | **DLSS-RR** | About 19% less flicker between frames than OIDN. This is the only merit DLSS has |
| Shots where metal is the subject | **OIDN** | With DLSS, chrome highlights melt. We tried every available measure (SpecularHitDistance / SpecularMotion) and none worked. It is a structural problem on the runtime side, which we cannot fix at our end |
| Volumes (fog, light shafts) | **OIDN** | DLSS produces grain in volumes |
| Using depth of field (DOF) | **OIDN**, or apply DOF in compositing | Blur produced by the lens is outside what DLSS assumes (NVIDIA itself writes "do DOF downstream"). Blurred areas become blotchy |
| Comparison and verification against other renderers | **OIDN, or no denoising** | DLSS paints in detail that is not there, so it must not be used as the basis of a comparison |

## Cautions when using DLSS

- **The first 2–4 frames of an animation come out noisy.** DLSS gets its quality by accumulating
  past frames, so for the first few nothing has accumulated yet. **Render 2–4 frames ahead of your
  real start frame and throw them away** and it evens out (for 200 frames that is a +2% cost)
- **The same thing happens when the shot changes.** Right after switching cameras with a marker, 2–4 frames are needed as well
- **Do not use upscaling.** Once you drop the internal resolution, no amount of extra samples brings
  it back (it degrades to the point where the writing on the blackboard is unreadable). What is lost is
  not noise but resolution itself. Only native scale (DLAA) is practical

## Cautions for the error-field cache (cachefield)

- **It is for roughs and previews.** Around 64 spp it is faster than the plain state and the error is
  smaller too, but once you raise it to 256 spp the error stalls around 0.07 (plain Cycles keeps
  dropping in proportion to the samples you add).
  **Turn it off for shots aiming at final quality**
- The crossover point is somewhere between 64 and 256 spp, and we have not pinned it down yet

## Scenes where light tracing cannot be used, or is not suited

- **It does not work in scenes where the light sources are not light objects.** Only Sun / Area / Spot
  are supported. In scenes lit only by mesh emission (HDRI-driven photo sets, neon, carefully built indirect lighting)
  it stops with "no supported lights"
- ~~**In outdoor scenes where a wide water surface or a large sheet of glass covers most of the receiver surface, it can become extremely bright.**
  Measured at 10.9x in the official demo "Barcelona Pavilion" (2026-08-02, cause unidentified)~~
  → **Fixed in v0.3.** The real cause was that the light-tracing layer was being read back through a
  file, and what was being read was not the raw Cycles output but **the image after passing through the scene's compositor**.
  The beauty goes through the same path, so the constant amount the compositor adds was being **counted twice**.
  **It happens only in scenes that have compositor nodes set up** (our in-house test scenes had 0 nodes,
  so it went unfound for nearly 1 month). Ratios for the same scene after the fix:

  | Region | Before the fix | **After the fix** |
  |---|---|---|
  | Sky | 1.212 | **1.0000** |
  | Wall | 1.395 | **1.0002** |
  | Roof | 1.277 | **1.0000** |
  | Caustics on the water surface | 3.160 | **1.0162** |
- **In scenes where indirect light is the subject, it only costs time and makes no difference.**
  Measured on the classroom scene: against the official 2.7 seconds it spent 79 seconds, and the
  difference from the reference was almost the same (0.107 vs 0.109)

## Accuracy of caustics

**On 2026-08-25 we re-shot the reference and recomputed every number.** The "+23–33% at the core of
the caustic" that was listed here up to v0.2 was measured on 2026-08-12, and it predates the 6 fixes that landed afterwards.

The reference is brute-force path tracing with no approximations at all. **For each scene we shot 2
independent runs and recorded their disagreement as the noise floor (0.001–0.222%).**

### There are 2 yardsticks, and they give different answers

Even for the same scene, the ranking changes depending on whether you look at the **average over a
region** or at the **brightest 1% (the core)**.
Showing only one of them would be dishonest, so we list both.

**Average over the region**

| Scene | Where measured | Plain PT | Official MNEE | This build, grid | This build, LT |
|---|---|---|---|---|---|
| Glass sphere | Core of the caustic | 1.000 | **0.265** | **1.007** | 1.007 |
| Glass sphere | Mottled part of the caustic | 1.000 | **0.424** | 0.992 | 0.967 |
| Water surface | Caustic band | 1.000 | **1.001** | **0.965** | 0.963 |
| Water surface | Sea floor | 1.001 | 0.973 | 0.974 | 0.973 |
| 12 glass bodies | Caustic, right | 1.003 | **0.160** | **0.998** | 0.983 |
| 12 glass bodies | Inside the shadow | 1.003 | 0.432 | 0.979 | 0.963 |
| 12 glass bodies | Center | 0.996 | 0.916 | 0.972 | 0.950 |
| Control (non-caustic area) | — | 1.000–1.001 | 0.988–1.000 | 0.997–1.000 | 0.998–1.000 |

**Brightest 1% (the core)**

| Scene | Where measured | Plain PT | Official MNEE | This build, grid | This build, LT |
|---|---|---|---|---|---|
| Glass sphere | Core of the caustic | 1.002 | **0.243** | 1.005 | 0.997 |
| Water surface | Sea floor | 0.979 | **0.703** | 0.987 | 0.980 |
| Water surface | Caustic band | **0.942** | 0.922 | 0.925 | 0.929 |
| 12 glass bodies | Caustic, right | 0.989 | **0.014** | 0.974 | 0.958 |
| 12 glass bodies | Inside the shadow | 0.998 | **0.092** | 0.926 | 0.884 |

### ★How to read this table (the self-check of the yardstick itself)

**The plain-PT column is itself the check on the yardstick.** Plain path tracing is the same
computation as the reference, so it should be 1.00, and **however far it deviates from that is how far
the yardstick on that row is not working.**

- Glass sphere 1.002 / 12 glass bodies 0.989–0.998 → **the core numbers can be trusted**
- **Water surface, caustic band 0.942 → off by 5.8%. Do not use the core numbers here as evidence.**
  The reason is that the top 1% of pixels in the reference still holds spikes that have not fully converged, which no condition can reproduce
- **In the control regions (sky, flat floor) the core yardstick puts every condition around 0.94.**
  (Not shown in the table above. On a flat, non-caustic surface the reference's "top 1%" is just noise
  spikes, which no condition can reproduce.) = **the core yardstick has no meaning in control regions**

### What this tells us

- **In scenes with multiple pieces of glass, the official Shadow Caustics (MNEE) drops a large part of the caustic**
  (for the right-hand caustic in the 12-glass-bodies scene: average 0.160, core 0.014). This build is 0.998 on average
- **On the water surface it is the other way round: viewed as an average, the official one is closer** (1.001 vs 0.965).
  However, at the core on the sea floor the official one falls to 0.703. **It is not erasing it, it is smoothing it out**
- **Grain**: in the 12-glass-bodies scene, plain path tracing is 1.53 while this build's grid is 0.046
  = **33x smoother** (same 60 seconds)

### ⚠ Limits of this measurement (left in as they are)

- **These are measurements under widened light sources** (sun 3 degrees, lamp radius 0.15 m). With the
  shipping sun (0.53 degrees) or with zero-area light sources, the brute-force path tracing that serves
  as the basis of comparison **cannot capture caustics in principle**,
  so **absolute verification at normal light sizes has not been done yet**
- **The cross-check against another renderer (LuxCore) has not been updated since the 2026-08-02 results.**
  When we checked on 2026-08-25, our LuxCore settings were off by 5.9–7.7x from the reference in
  non-caustic regions, so **as things stand it cannot serve as a judge.** Until the calibration is done, please read this item as on hold
- With **glass that interpenetrates the receiver surface**, it is not the caustic but the **glass body
  itself** that comes out too bright (20.7x the ground truth).
  **Sitting it exactly on the surface brings that down to 1.90x** (how to avoid it, and the numbers, are
  in [KNOWN_ISSUES.en.md](KNOWN_ISSUES.en.md))
- **The calibration of direct light agrees with the analytic solution** (0.983–0.988 relative to theory
  on an open floor). What deviates is the distribution of the concentrated light, not the total amount of light
- **Interfaces where light "only goes in and does not come out", like a water surface** (pools, liquid
  surfaces, single-sided sheet glass) came out brighter by the square of the index of refraction in versions before 2026-08-02. This has been fixed

## Other

- **Linux only.** There is no Windows build
- If you turn DLSS's **ナビ履歴持ち越し** ("carry over navigation history") ON while navigating the viewport,
  scenes with volumes leave trails (the default is OFF)
- Do not leave the viewport in **レンダー** ("Rendered") viewport shading during a bake (it is guarded against, but with an
  old .blend where the add-on has not been reloaded it can still crash)
