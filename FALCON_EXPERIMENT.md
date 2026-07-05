# Falcon Cycles Experiment

This tree is currently used as a small standalone Cycles testbed before deeper
Blender integration.

## Current hook

`src/app/cycles_standalone.cpp` adds two experimental flags:

```sh
--falcon-preview
--falcon-gpu-scene
```

`--falcon-preview` applies light preview defaults when explicit values are not
provided. `--falcon-gpu-scene` prints scene diagnostics after XML/USD scene sync
and before `Session::reset()`, which is the first safe insertion point for a
future GPU scene-cache path.

## Build used locally

The bundled `lib/linux_x64` directory is empty, so the local build uses system
libraries:

```sh
cmake -S . -B build-cuda \
  -DWITH_LIBS_PRECOMPILED=OFF \
  -DPYTHON_VERSION=3.10 \
  -DPYTHON_INCLUDE_DIR=/home/mirai/.pyenv/versions/3.10.6/include/python3.10 \
  -DPYTHON_LIBRARY=/home/mirai/.pyenv/versions/3.10.6/lib/libpython3.10.so \
  -DPYTHON_LIBPATH=/home/mirai/.pyenv/versions/3.10.6/lib \
  -DWITH_CYCLES_DEVICE_CUDA=ON \
  -DWITH_CYCLES_DEVICE_OPTIX=OFF \
  -DWITH_CYCLES_CUDA_BINARIES=OFF \
  -DWITH_CYCLES_DEVICE_HIP=OFF \
  -DWITH_CYCLES_USD=OFF \
  -DWITH_CYCLES_OSL=OFF \
  -DWITH_CYCLES_OPENVDB=OFF \
  -DWITH_CYCLES_NANOVDB=OFF \
  -DWITH_CYCLES_ALEMBIC=OFF \
  -DWITH_CYCLES_OPENSUBDIV=OFF \
  -DWITH_CYCLES_OPENIMAGEDENOISE=OFF \
  -DWITH_CYCLES_PUGIXML=ON

cmake --build build-cuda --config Release --target cycles -j$(nproc)
```

Runtime CUDA compilation expects kernel source next to the executable. If
`cmake --install build-cuda` stops while installing Hydra, the source files may
still be copied to `install/source`; link them for direct `bin/cycles` runs:

```sh
ln -s ../../install/source build-cuda/bin/source
```

## Smoke tests

CPU:

```sh
./build-system/bin/cycles \
  --falcon-preview --falcon-gpu-scene \
  --samples 2 --width 128 --height 72 \
  --output /tmp/falcon_cycles_test.png \
  examples/scene_cube_surface.xml
```

CUDA, outside the sandbox so the RTX device is visible:

```sh
./build-cuda/bin/cycles \
  --device CUDA \
  --falcon-preview --falcon-gpu-scene \
  --samples 1 --width 64 --height 36 \
  --output /tmp/falcon_cycles_cuda_test.png \
  examples/scene_cube_surface.xml
```

First CUDA run compiled `sm_86` kernels with CUDA 13.3 and took about 283s.
After the cubin was cached, the same one-sample test finished in about 0.24s.

## Blender add-on smoke bridge

The test add-on lives at:

```text
blender_addons/falcon_cycles_bridge/__init__.py
```

It adds a render properties panel named `Falcon Cycles Bridge`. The current MVP
can either render the fixed smoke-test XML or export the active Blender scene to
a minimal Cycles XML file, launch standalone CUDA Cycles, write a PNG, and load
the result into Blender.

Background test:

```sh
/home/mirai/Documents/program-file/blender-5.1.1-linux-x64/blender \
  --background --factory-startup \
  --python tools/test_falcon_cycles_bridge_addon.py
```

Verified result:

```text
FALCON_ADDON_RESULT {'FINISHED'}
FALCON_ADDON_LAST_IMAGE falcon_cycles_addon_test.png
```

The add-on writes:

```text
/tmp/falcon_cycles_addon_test.png
/tmp/falcon_cycles_addon_test.log
```

Current-scene export smoke test:

```text
/tmp/falcon_cycles_current_scene_test2.xml
/tmp/falcon_cycles_current_scene_test2.png
```

Verified result:

```text
FALCON_CURRENT_SCENE_RESULT {'FINISHED'}
FALCON_CURRENT_SCENE_IMAGE falcon_cycles_current_scene_test2.png
```

Current limitations:

- Mesh export is evaluated world-space triangle data.
- Materials are reduced to diffuse base color.
- Per-face material slots, textures, node graphs, volumes, hair, particles, and
  full Cycles material compatibility are not implemented yet.

## Next code areas

- `src/scene/scene.h`: owns `geometry`, `objects`, `shaders`, and `DeviceScene`.
- `src/scene/devicescene.h`: GPU-side scene buffers.
- `src/scene/geometry*.cpp` and `src/scene/object.cpp`: mesh/object packing and
  transfer to the device.
- `src/bvh/*.cpp`: BVH build, packing, and transfer.
- `src/integrator/path_trace_work_gpu.*`: GPU path work scheduling and render
  buffer movement.
- `src/session/buffers.*` and `src/integrator/pass_accessor_gpu.*`: later
  denoise/upscale/postprocess buffer access.
