# Golemics backend inside Blender

This module contains the painting session and sparse voxel sculpt backend
source, compiled by Blender as `bf_golemics_backend`. It does not load or link
an external Golemics build. Blender owns viewport input, mesh conversion,
GPU resources, material bindings, and file/undo integration in its host modules.

The initial source was imported from the local Golemics working tree based on
commit `ddb266b`, including its uncommitted GPU session and voxel host changes,
on 2026-09-12. The `dcc` namespace and include layout are retained to make
comparison with the original implementation straightforward. Source files had
no license headers and the source repository had no root LICENSE file; this
import does not invent or change their licensing metadata.

Only the required backend sources and transitive headers are included. There
is no SDL, raw OpenGL, Python, scene loader, or standalone application build
dependency. `core.hpp` contains only the shared mesh/value types needed by these backends;
standalone scene and tiled-image APIs are excluded.

To test the backend independently:

```sh
cmake -S source/blender/golemics -B /tmp/blender-golemics-backend -DBUILD_TESTING=ON
cmake --build /tmp/blender-golemics-backend -j2
ctest --test-dir /tmp/blender-golemics-backend --output-on-failure
```

These tests cover backend behavior, not Blender viewport integration.
