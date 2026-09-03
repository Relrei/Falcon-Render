# Getting the source code

*[日本語版はこちら / Japanese version](SOURCE.md)*

Blender is under the GNU GPL, so **you have the right to receive the source code corresponding to
this build** (whether it was paid or free). For the exact license text, the bundled `license/` is authoritative.

## The corresponding source

- Base: Blender `v5.2.1` (`9e2066aef7e`)
- Modified parts: the above with the Falcon commits layered on top
- The commit of this build: `5a1ade52865`

## How to get it

**`falcon-render-v0.4-src.tar.xz` is placed in the same location as this build.**
Anyone who downloaded the build can get that as well.

```bash
tar xf falcon-render-v0.4-src.tar.xz
```

The contents are a snapshot at the time of the build. The commit history from development is not
included (because what the GPL requires is "the source corresponding to this binary", not the process of development).

`lib/` (the set of pre-built external libraries) is not included. This is the same form as Blender's
official source distribution. To build, obtain it separately:

```bash
git submodule update --init --checkout lib/linux_x64
```

## Explanation of how it works

Rather than the code itself, we write separate explanations of **what we thought and how we built it**.
Those should be the more interesting read (please see the article list on Patreon).

## How to build

```
cmake -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DWITH_CYCLES_DEVICE_OPTIX=ON \
  -DWITH_CYCLES_CUDA_BINARIES=ON \
  -DCYCLES_CUDA_BINARIES_ARCH=sm_86 \
  -DOPTIX_ROOT_DIR=<path to the OptiX SDK> \
  -DWITH_FALCON_SHARC=ON \
  -DFALCON_BUILD_FLAVOR=release \
  -DFALCON_DIST=ON \
  ..
ninja -j6
```

- `-DWITH_FALCON_SHARC=ON` is required (OFF by default)
- If you have touched the CUDA kernel, you must `touch intern/cycles/kernel/device/cuda/kernel.cu`
  before building or it will not be recompiled
- Keep the parallel job count to around 6 (any more and it uses up all the memory)

## The source is also public on GitHub

**https://github.com/Relrei/Falcon-Render** (this repo: `main`, tag `v0.4-r2`)
> 2026-09-04: the source now lives in this same repo (`main` = the snapshot series on Blender 5.2.1). The old `blender-cyclesf-falcon` is archived (read-only, history kept).


The source corresponding to this build is available either from the bundled
`falcon-render-v0.4-src.tar.xz` or from the **`v0.4` tag** on GitHub —
they are the same tree.

The source is public because it should be, not because GPL forces a minimum:
GPL only requires source to be given **to the people who received the binary**.
