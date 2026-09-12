# Golemics Blender Integration — Resume Notes

Date: 2026-09-12

## Current implementation work (2026-09-12, modular backend)

This section supersedes the external-library instructions below. The current
objective is both native features with their modular backend source owned and
compiled inside Blender. Completion is not yet established.

- `source/blender/golemics` now builds `bf_golemics_backend` from five imported
  backend translation units. No external Golemics checkout/archive is required.
- The sculpt editor links `bf::golemics_backend` unconditionally. The old
  `GOLEMICS_DCC_CORE_DIR`/`GOLEMICS_DCC_CORE_LIBRARY` configuration was removed.
- `bf_golemics_host` builds the Blender GPU adapter separately from the
  host-neutral backend. Native paint operator and ordinary material image
  sampler substitution are under active implementation.
- The voxel adapter now uses actual vertex indices, object-space rays, valid
  preview edges, native stroke history and commit/cancel. Its scripted smoke
  test is `tests/python/golemics_voxel_sculpt.py`; runtime execution is pending
  the full Blender executable.
- The three imported standalone backend tests passed in
  `/tmp/blender-golemics-backend`. The initial import also passed address,
  undefined-behavior and leak sanitizers in `/tmp/blender-golemics-sanitized`.
  Rebuild/rerun after ongoing backend edits before claiming final verification.
- Blender configuration succeeds; its generated executable link command has
  `libbf_golemics_backend.a` and no external `libdcc_core` dependency.
- Full executable has not yet linked. Target build logs are
  `/tmp/golemics-targets.log` and `/tmp/golemics-sculpt-build.log`; the full
  build log is `/tmp/golemics-build.log`. Observe live process handles before
  restarting any active build.

Remaining acceptance: full compile/link; real scripted and modal sculpt runtime,
GPU paint correctness/no per-dab readback, UV/material behavior, save/reopen,
undo/cancel and lifecycle; responsive interactions in an actual viewport.

## Objective

Integrate Golemics multi-channel GPU PBR painting and voxel sculpting into a Blender fork while keeping Blender's normal viewport/material path and Golemics-style UX.

## Repositories

- Golemics core: `/home/tasinari/my_repos/golemics`
- Blender fork: `/home/tasinari/my_repos/blender`
- Blender build directory: `/tmp/golemics-blender-build`
- Golemics build directory: `/home/tasinari/my_repos/golemics/build-gl`

## Current Blender changes

The Blender fork contains these Golemics files/changes:

- `source/blender/draw/engines/eevee/golemics_paint_override.hh/.cc`
- `source/blender/draw/engines/eevee/eevee_material.cc/.hh`
- `source/blender/draw/engines/eevee/eevee_shader.cc`
- `source/blender/draw/intern/draw_pass.hh`
- `source/blender/gpu/GPU_material.hh`
- `source/blender/editors/sculpt_paint/mesh/golemics_voxel_sculpt.cc`
- `source/blender/editors/sculpt_paint/CMakeLists.txt`
- `source/blender/editors/sculpt_paint/paint_intern.hh`
- `source/blender/editors/sculpt_paint/paint_ops.cc`

The voxel operator supports optional linking to Golemics through `GOLEMICS_DCC_CORE_DIR`, modal press/move/release strokes, radius/strength controls, Shift inversion, real 3D view rays, preview mesh replacement, commit/cancel, and Escape cleanup.

The EEVEE path declares seven live paint samplers and applies base color, roughness, normal, height, ambient occlusion, and emission data to generated closures. The current UV lookup is still a temporary screen-space lookup; a proper per-object UV attribute bridge is still needed.

## Golemics backend changes

Golemics contains:

- `include/dcc/gpu_paint_session.hpp` and `src/gpu_paint_session.cpp`
- `include/dcc/voxel_sculpt_host.hpp` and `src/voxel_sculpt_host.cpp`
- `include/dcc/voxel_sculpt_c_api.h` and `src/voxel_sculpt_c_api.cpp`

The voxel C ABI exposes bridge creation, enter/leave, begin/end stroke, ray dabs, undo/redo, and preview callbacks. `dcc_core` includes the implementation.

## Patch artifacts

From the Golemics repo:

- `patches/blender-golemics-integration.patch` — base integration
- `patches/blender-golemics-paint-provider.patch` — per-object paint provider hook
- `patches/blender-golemics-multichannel-closure.patch` — seven-channel closure application
- `patches/blender-golemics-voxel-backend.patch` — optional `dcc_core` link and voxel ABI/modal path
- `patches/blender-golemics-voxel-modal.patch` — earlier modal-only patch
- `patches/blender-linux-compat.patch` — Linux compiler compatibility fixes

The focused voxel backend patch is the current version; the older modal-only patch is retained for reference.

## Validation already completed

Golemics:

```sh
cd /home/tasinari/my_repos/golemics
cmake --build build-gl --target dcc_core dcc_voxel_sculpt_host_tests -j2
ctest --test-dir build-gl --output-on-failure
```

Result: all 29 tests passed.

Blender:

```sh
cmake --build /tmp/golemics-blender-build --target bf_draw -j2
cmake --build /tmp/golemics-blender-build --target bf_editor_sculpt_paint -j2
```

Both targeted libraries built successfully. The build cache contains:

```text
GOLEMICS_DCC_CORE_DIR=/home/tasinari/my_repos/golemics
```

The full `blender` target was being built incrementally. Resume with:

```sh
cmake --build /tmp/golemics-blender-build --target blender -j2
```

A baseline missing `<charconv>` include in `source/blender/editors/asset/intern/asset_indexer_remote_listing_v1.cc` was added so the bundled Clang build could proceed.

## Remaining work

1. Let the full Blender target finish and fix any integration compile/link errors.
2. Verify the ABI-enabled sculpt target links against `dcc_core` and runs a real dab through the callback path.
3. Implement a proper evaluated-mesh UV attribute bridge for paint sampling.
4. Connect `GpuPaintSession`/its resource provider to Blender GPU textures and framebuffers so normal strokes use resident GPU resources without image flushing.
5. Add save/undo/context-loss synchronization and runtime smoke tests.

Do not claim the feature is production-complete until those runtime paths are exercised.
