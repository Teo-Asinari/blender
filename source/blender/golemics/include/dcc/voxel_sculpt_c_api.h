#pragma once

/*
 * Stable, optional C ABI for DCC integrations.
 *
 * A host can load dcc_core dynamically or link it behind its own build option;
 * Blender itself does not need to include any C++ core headers.  Mesh memory
 * passed to callbacks is borrowed until the callback returns.
 */
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct dcc_voxel_mesh {
  const float *positions;       /* xyz, three floats per vertex */
  size_t vertex_count;
  const uint32_t *indices;      /* triangle list */
  size_t index_count;
  uint64_t revision;
} dcc_voxel_mesh;

typedef struct dcc_voxel_sculpt_callbacks {
  void *userdata;
  void (*replace_preview_mesh)(void *userdata, const dcc_voxel_mesh *mesh);
  void (*restore_source_mesh)(void *userdata);
  void (*tag_geometry_dirty)(void *userdata);
  void (*tag_viewport_redraw)(void *userdata);
} dcc_voxel_sculpt_callbacks;

typedef struct dcc_voxel_source_mesh {
  const float *positions;       /* xyz, three floats per vertex */
  size_t vertex_count;
  const uint32_t *indices;      /* triangle list */
  size_t index_count;
} dcc_voxel_source_mesh;

typedef struct dcc_voxel_bridge dcc_voxel_bridge;

/* Returns NULL for malformed input or allocation failure. */
dcc_voxel_bridge *dcc_voxel_bridge_create(dcc_voxel_source_mesh source,
                                          float voxel_size,
                                          dcc_voxel_sculpt_callbacks callbacks);
void dcc_voxel_bridge_destroy(dcc_voxel_bridge *bridge);
int dcc_voxel_bridge_enter(dcc_voxel_bridge *bridge);
void dcc_voxel_bridge_leave(dcc_voxel_bridge *bridge, int commit);
int dcc_voxel_bridge_begin_stroke(dcc_voxel_bridge *bridge, int brush_mode,
                                  float radius, float strength, float spacing);
int dcc_voxel_bridge_dab(dcc_voxel_bridge *bridge, const float origin[3],
                         const float direction[3], float pressure);
void dcc_voxel_bridge_end_stroke(dcc_voxel_bridge *bridge);
int dcc_voxel_bridge_undo(dcc_voxel_bridge *bridge);
int dcc_voxel_bridge_redo(dcc_voxel_bridge *bridge);

#ifdef __cplusplus
} /* extern "C" */
#endif
