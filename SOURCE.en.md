# Getting the source code

*[日本語版はこちら / Japanese version](SOURCE.md)*

Blender is under the GNU GPL, so **you have the right to receive the source code corresponding to
this build** (whether it was paid or free). For the exact license text, the bundled `license/` is authoritative.

## The corresponding source

- Base: Blender `v5.2.0` (`fbe6228777e`)
- Modified parts: the above with the Falcon commits layered on top
- The commit of this build: `5eb3ab668e4`

## How to get it

**`falcon-render-v0.3-beta-src.tar.xz` is placed in the same location as this build.**
Anyone who downloaded the build can get that as well.

```bash
tar xf falcon-render-v0.3-beta-src.tar.xz
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
  ..
ninja -j6
```

- `-DWITH_FALCON_SHARC=ON` is required (OFF by default)
- If you have touched the CUDA kernel, you must `touch intern/cycles/kernel/device/cuda/kernel.cu`
  before building or it will not be recompiled
- Keep the parallel job count to around 6 (any more and it uses up all the memory)
