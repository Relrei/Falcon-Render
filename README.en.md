# Falcon Render v0.3 beta

*[日本語版はこちら / Japanese version](README.md)*

A rendering-focused build based on Blender 5.2.0 LTS. It changes the caustics
computation and the speed of preview rendering. Target environment: Linux x86_64 / NVIDIA GPU.

Version shown at startup: `Falcon Render (5.2LTS)`
(the part in parentheses is the version of Blender it is built on. The product version number is not part of the display name. It is separate from the version number of the distribution package)

---

## About this release (demo version, free)

**v0.3 beta is a demo version distributed free of charge. There are no feature restrictions.**
Everything listed under "Differences from the standard build" below is usable in this package.

Later releases are planned to be distributed for a fee. The form will be **older versions free,
the latest version paid**, and because it is distributed under the same GPL as Blender,
**the source for the version you receive always comes with it**.

---

## Changes in v0.3

This is a fix-centered update. **It includes the bug that made GPU rendering unusable in v0.2.**

### ★Fixed the bug that made GPU rendering unusable in the distributed package

The v0.2 package was missing 2 headers required to assemble the GPU kernel.
The GPU kernel reads and compiles its source at run time, so **the build and the CPU
render both succeed, and only the GPU render fails**. If you could not use v0.2 on the GPU, this is why.

To stop this from happening again, we built a check into the packaging procedure that verifies
the kernel is self-contained using only the distributed source.

### Closed 3 issues triggerable by opening someone else's `.blend`

Settings specific to this build that are written in a `.blend` were used as-is, without validation at load time.

| | v0.2 | v0.3 |
|---|---|---|
| An arbitrary file is overwritten with 1 GiB | Triggerable | Closed (the cache is remapped to `~/.cache/falcon_photon/`) |
| Reading more than was allocated | Triggerable | Rejected, and the run completes |
| An abnormal radius makes it never return | Passed through unchecked | Clamped to 8.0 |

**The normal path is unchanged**: the caustics output for the same scene matches byte for byte
(the only difference is the timestamp in the header), and the speed is 0.963–1.021x.
We also added an inspector that lists what will be executed and the values specific to this build,
so you can look before opening someone else's file.

### A "target face count" for remeshing

- You can now specify the face count directly (it hits the target in 1 pass from the projected area)
- When combined with adaptivity, it iterates to approach the target
- It respects sculpt masks
- **Fixed UVs and vertex colors being lost**
- If the target is too large, it does not run at all instead of warning (because it would never return)

### Faster sculpt drawing (welding on by default)

Vertices at the same position are now merged before being sent to the GPU. This is on by default.

| | v0.2 | v0.3 |
|---|---|---|
| Rebuilding the position buffer (4 million faces) | 6.90 ms | **1.59 ms** |
| Amount sent in 1 transfer | 30.4 MB | **8.0 MB** |
| Whole draw (2 million faces) | 51.2 ms | **42.6 ms** |

Under conditions where merging is not possible — flat shading, face sets, UV texture display and
so on — it automatically falls back to the previous behavior. `FALCON_DRAW_WELD=0` turns it off.

### Caustics fixes

- The sun is now shot as a "disk" (it used to bake the sun as a point light)
- Point lights, area lights (ellipse, spread) and spots (radius, edge falloff) now each shoot photons with the correct shape
- Fixed the white dots that appeared when the grid collided
- The photon budget is now distributed to the lights by "the fraction that actually arrives" rather than by wattage
- Changed the default photon count from 20 million to 1 billion (the value from the CPU era was still in place)
- Fixed light-tracing compositing clobbering the baked photon cache with 1 GB of zeros
- Fixed light tracing double-counting the scene's compositor output
- Stopped light tracing from also adding the full amount to the back side of the receiver surface
- **Fixed light tracing brightening the whole image uniformly when a light is given a size**

### Display name

The startup display is now `Falcon Render (5.2LTS)` (the part in parentheses is the version of Blender it is built on).

---

## Differences from the standard build

| Feature | What it does |
|---|---|
| Accumulation grid caustics | Accumulates photons into a fixed-size grid. Draw time is constant regardless of how many are shot |
| Falcon light tracing | Traces rays from the light side and directly computes caustics from water surfaces, glass, metal reflections and so on |
| World photons | Applies the same method to caustics originating from environment lighting (HDRI) |
| SHARC cache | Stores radiance in a spatial hash and reuses it |
| Error-field cache | Judges the convergence state per cell and cuts off tracing of already-converged paths. Shortens preview render time by about 20% |
| Emission guiding | Distributes photon emission directions based on measured data. Reduces noise by 9.6% at the same sample count |
| LT recompositing | The intensity and blur of the caustics layer can be readjusted in 0.1 seconds (previously this required re-running the render, 197 seconds) |
| DLSS-RR | NVIDIA's denoiser. Conditions apply (see below). Suppresses flicker in animation |
| Remesh target face count | Runs voxel remeshing with a directly specified face count. Can be combined with adaptivity |
| Welding of duplicate vertices in sculpt drawing | Merges vertices at the same position before sending them to the GPU. On by default (`FALCON_DRAW_WELD=0` turns it off) |

---

## A note on the interface language

**The settings this build adds are labelled in Japanese, and stay Japanese even when Blender's
interface language is set to English.** Where this documentation names one of those controls, the
on-screen Japanese label is given first, with an English gloss in parentheses — search the panel for
the Japanese text. Everything Blender itself provides follows Blender's own language setting as usual.

There are 9 such settings and 61 panel strings. Translating them is on the list; it is not done yet.

## Installation

```bash
tar xf falcon-render-v0.3-beta-linux-x86_64.tar.xz
cd falcon-render-v0.3-beta-linux-x86_64
./falcon-render.sh
```

Launch it from `falcon-render.sh`. Running `./blender` directly also works, but the
launch script sets the following 2 things.

- `OPTIX_CACHE_PATH`: if unset, OptiX recompiles 10 MB of PTX on the first render and
  stalls for up to 20 minutes. If you force-quit while it is working, the cache is not saved and the same wait happens again
- `__NGX_DISABLE_UPDATER=1`: disables the network wait caused by the DLSS update check

### System requirements

- NVIDIA GPU (the RTX series is recommended because OptiX is used. The development environment is an RTX 3080)
- Linux x86_64

---

## About DLSS

DLSS-RR is an optional feature that can be used only when the environment requirements are met,
and it is **disabled by default**. All the other features work without DLSS.

**This package does not bundle the DLSS runtime library** (same as v0.2).
Because the Linux version of DLSS is designed so that the driver loads only signed libraries,
DLSS appears in the denoiser options only when you place a legitimately obtained
`libnvidia-ngx-dlssd.so` directly in the extracted directory.

> As for bundling: the NVIDIA RTX SDKs License permits redistribution in object code form,
> but it also requires prior notification (§3) and display of the NVIDIA trademark in the
> splash/about box (§6.1(b); a sample of the display must be submitted 2 weeks in advance
> and approved beforehand, §6.2(c)).
> **We will not bundle it until those 2 are done.**

DLSS 4.5 RR is expected to be officially integrated in Blender 5.3 (planned for autumn 2026)
(a patch has already been submitted from NVIDIA to upstream). Once integrated, this procedure will not be needed.
**However, as of 2026-08-12 it is not yet in the 5.3.0 Alpha (`main.8eb257c2b2df`).**
We confirmed that the denoiser options are only OptiX and OpenImageDenoise, and that no
NGX-related libraries are bundled either. We consider the timing of the integration undecided.

The v0.1 restriction of "DLSS final renders only up to 1920x1080" was **removed in v0.2**.
At the time, the path taken when tiling occurs at high resolution had not been verified, and
there was a bug that terminated abnormally when the restriction was hit, so we erred on the
safe side. In v0.2 we fixed the abnormal termination and changed the criterion from resolution
to whether tiling occurs (because what broke was not the resolution itself but the tiling).

As far as we checked on 2026-08-12, both 2560x1440 and 1920x1080 with tiling forced complete
normally. However, **we have not yet exercised long renders at 4K or above enough**.
If a problem shows up, we would appreciate a report rather than `FALCON_DLSS_ALLOW_HIGHRES=0`.

---

## Known issues

See [KNOWN_ISSUES.en.md](KNOWN_ISSUES.en.md).

## Weaknesses

See [WEAKNESSES.en.md](WEAKNESSES.en.md).

## Source code

Blender and this build are under the GPL, so the corresponding source code is available.
See [SOURCE.en.md](SOURCE.en.md) for details.

## License

- Blender itself and the modified parts of this build: GNU GPL v3 (see `license/`)
- Bundled libraries: subject to each library's own license (under `license/`)
