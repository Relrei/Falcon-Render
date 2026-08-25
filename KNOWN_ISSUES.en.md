# Known issues

*[日本語版はこちら / Japanese version](KNOWN_ISSUES.md)*

This is where we collect bugs that are currently under investigation, or unresolved.
We will keep the reproduction conditions and the state of the isolation work updated as far as they are known.

## [v0.2] GPU rendering was unusable — fixed in v0.3

- **Symptom**: in the distributed v0.2, GPU rendering fails entirely. CPU rendering works
- **Cause**: the GPU kernel reads and compiles the bundled source **at run time**. The fork-specific
  `falcon_sharc_size.h` and `falcon_lighttrace.h` were not on that list
  (`SRC_KERNEL_*_HEADERS`), and so were not bundled.
  **Because the build and the CPU render both succeed, this is the class of bug a build can never find**
- **Fix**: added them to the list. In addition, a check
  (`tools/falcon_check_kernel_install.py`) that verifies the kernel is self-contained using only the
  distributed source was put into the packaging procedure, so that **no package is built unless it passes**

## [v0.2] 3 issues triggerable by opening someone else's `.blend` — fixed in v0.3

Settings specific to this build that are written in a `.blend` were used as-is, without validation at load time.
Blender's PropertyGroup is an IDProperty, so the RNA limits are not re-applied.

| | v0.2 | v0.3 |
|---|---|---|
| An arbitrary file is overwritten with 1 GiB | Triggerable | Closed (remapped to `~/.cache/falcon_photon/`) |
| Reading more than was allocated | Triggerable | Rejected, and the run completes |
| An abnormal radius makes it never return | Passed through unchecked | Clamped to 8.0 |

The normal path is unchanged (the caustics output for the same scene matches byte for byte; speed 0.963–1.021x).

## [v0.2] Light tracing brightens the whole image in scenes with a compositor set up — fixed in v0.3

- **Symptom**: in a scene with compositor nodes set up, the light-tracing layer uniformly brightens
  even pixels that have nothing to do with caustics. 10.9x the reference in the official demo "Barcelona Pavilion"
- **Cause**: the layer was being read back through a file, and what was being read was not the raw
  Cycles output but **the image after passing through the scene's compositor**. The beauty goes
  through the same path, so the constant amount the compositor adds to the whole image was **counted twice**.
  Our in-house test scenes had 0 compositor nodes, so this went unfound for about 1 month
- **After the fix** (same scene): sky 1.212→**1.0000** / wall 1.395→1.0002 / roof 1.277→**1.0000** /
  caustics on the water surface 3.160→**1.0162**. The layer's energy is 1/197 of what it was, and coverage went 99.999%→0.0998%

## [v0.2] Giving a light a size makes light tracing brighten the whole image — fixed in v0.3

- **Symptom**: when a spot light is given a radius, even pixels that have nothing to do with caustics
  get uniformly brighter (a +0.078 pedestal on every pixel. In the darkest background this looks like 5.8x the reference)
- **Cause**: when a photon hit the lamp itself, its emission was written to the film.
  The pixel index of a photon path is "the photon's emission index", so writing it scatters the value across the whole frame
- **After the fix**: the all-pixel pedestal went from +0.078 to **−0.0009**. The control regions are 0.996–1.000 relative to the reference

## Unresolved

- With **glass that interpenetrates the receiver surface**, the glass body itself becomes excessively
  bright (**20.7x** the brute-force ground truth).
  **The cause is geometry**: when the receiving surface pierces the wall of the glass, light guided along
  the wall falls onto the receiver surface that is **inside** the glass. There the contact circle is a
  line, so the fixed-cell density estimate diverges.

  | Relationship between the glass and the desk | Brightness of the glass body (relative to ground truth) |
  |---|---|
  | 1.1 mm interpenetration | **20.7x** |
  | 0.6 mm interpenetration | 20.1x |
  | **Exactly touching (gap 0)** | **1.90x** |
  | Floated 5 mm | 1.68x |
  | Floated 20 mm | 1.29x |

  **Workaround: do not let it interpenetrate the receiver surface** (touching is allowed, intersecting is not).
  The depth is irrelevant; only whether there is interpenetration decides it. It happens even at 0.6 mm
- **The "core" of a caustic in contact with the ground is not reproduced by any of the approximations.**
  Looking at the light of the contact circle in the top 1% of the ground truth:
  accumulation grid 0.075 / light tracing 0.031 / official MNEE 0.053 (only brute-force path tracing reaches 0.964).
  Even when made to touch exactly, the glass body remains at 1.90x (doubling the cell size halves the
  excess = a fundamental limit of density estimation)
- **In scenes where light bounces many times, such as interiors or underwater**, the light-tracing layer
  comes out on the dark side (0.556x the reference underwater)
- With light tracing, the bake takes up most of a short budget (57 of a 60-second budget is baking)

## [v0.1] Sequence renders with DLSS consume many times the specified sample count (#2, #3) — fixed in v0.2

- **Symptom**: with DLSS-RR enabled, a sequence (animation) render keeps consuming far more than the
  specified sample count. Does not occur for stills
- **Cause**: `intern/cycles/blender/session.cpp`. When Persistent Data (OFF by default) is OFF,
  `BlenderSession` (and the internal `Session`/`RenderScheduler`) is destroyed and rebuilt in its
  entirety on every frame of an animation. The first-time flag `dlss_history_cold_` of the DLSS-RR
  mechanism that "warms the temporal history with 5x samples on the first frame only"
  (`get_dlss_preroll_passes()`) returns to its initial value `true` on every such rebuild, so
  **every frame is treated as the "first" one and consumes 5x every time**
- **Fix**: on completion of `BlenderSession::render()`, record that "the DLSS-RR history has been
  warmed in this job", and when the Session is next rebuilt, pass that record along to set
  `dlss_history_cold_` back to `false`. The record has to be kept in a `static` member
  (`dlss_history_warmed_this_job`), not in an instance member of `BlenderSession`. Because
  `BlenderSession` itself is recreated every frame, the value does not survive in an instance member
  (we failed once this way first)
- **Side effect of the fix, and the re-fix**: right after making it `static`, a bug in the opposite
  direction appeared (once warmed even once in the whole process, thereafter
  **even the true first frame of a new job was treated as x1**, so noise increased from insufficient accumulation).
  Resolved by using `cfra == sfra` (the current frame equals the start frame of the range) as the test
  for "the first frame of a new job", and forcing a return to cold only at that moment.
  Verified with 2 consecutive jobs (2 frames each); in both, the first frame was cold and the second was warm
- **Verified on real hardware (classroom scene, 2 frames, Persistent Data OFF)**:
  20.8–24.4 seconds before the fix → 14.5 seconds after (8 spp). The debug log also confirmed that
  `warmed_flag` for frame 2 changed from `0` to `1`
- **Affected commits**: `intern/cycles/integrator/render_scheduler.{h,cpp}`,
  `intern/cycles/session/session.{h,cpp}`, `intern/cycles/blender/session.{h,cpp}`
  (Blender-5.2.C2-dlss, on the local source side)

## Notes

- ~~`FALCON_DLSS_PREROLL` is not exposed in the UI (environment variable only)~~ → **resolved in v0.2**.
  **初回蓄積レンダリング回数** ("Initial accumulation render count") (default 4; 0 disables) was added below **履歴持ち越し** ("Carry over history")
  in the denoise panel. The environment variable still works as before, and when specified it takes
  precedence over the UI (for A/B measurement).
  Note that this setting applies **only to the first frame of an animation render**. It intentionally
  does not fire for a single still render (the design being that stills honor the specified sample count)
- The matter on the viewport (RENDERED display) side, where `preview_denoising_carry_history`
  (OFF by default) replaces `get_num_samples()` with `MAX_SAMPLES`, is a different path from this
  sequence problem. Not addressed
- The debug logging (enabled with the `FALCON_DEBUG_LIFECYCLE` environment variable; prints the
  creation and destruction of BlenderSession/Session and the value of warmed_flag) is left in the
  source. It can be reused for the next investigation of the same kind

## Remarks

This repository is for build distribution and documentation, and does not contain the source code of
the Cycles / DLSS-RR integration that actually performs the rendering (see [SOURCE.en.md](SOURCE.en.md)
for details). Investigating the cause of this problem and fixing the implementation therefore has to be
done on the side of the separate (local) repository created by extracting
`falcon-render-v0.3-beta-src.tar.xz`. On this repository side, we only record the symptoms and track
the state of the isolation work.
