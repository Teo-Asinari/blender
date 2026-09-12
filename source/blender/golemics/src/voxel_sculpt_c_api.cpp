#include "dcc/voxel_sculpt_c_api.h"

#include "dcc/voxel_sculpt_host.hpp"

#include <memory>
#include <cmath>
#include <new>
#include <vector>

struct dcc_voxel_bridge {
  dcc::VoxelSculptHostBridge host;
  dcc_voxel_sculpt_callbacks callbacks{};
  std::vector<float> preview_positions;
  std::vector<uint32_t> preview_indices;

  dcc_voxel_bridge(dcc::Mesh source, dcc_voxel_sculpt_callbacks cb, float voxel_size)
      : host(std::move(source), {
          [this](const dcc::Mesh &mesh, uint64_t revision) { publish(mesh, revision); },
          [this]() { if (callbacks.restore_source_mesh) callbacks.restore_source_mesh(callbacks.userdata); },
          [this]() { if (callbacks.tag_geometry_dirty) callbacks.tag_geometry_dirty(callbacks.userdata); },
          [this]() { if (callbacks.tag_viewport_redraw) callbacks.tag_viewport_redraw(callbacks.userdata); },
        }, voxel_size), callbacks(cb) {}

  void publish(const dcc::Mesh &mesh, uint64_t revision) {
    if (!callbacks.replace_preview_mesh) return;
    preview_positions.clear();
    preview_positions.reserve(mesh.vertices.size() * 3);
    for (const dcc::Vertex &vertex : mesh.vertices) {
      preview_positions.insert(preview_positions.end(),
                               {vertex.position.x, vertex.position.y, vertex.position.z});
    }
    preview_indices = mesh.indices;
    const dcc_voxel_mesh view{preview_positions.data(), mesh.vertices.size(),
                              preview_indices.data(), preview_indices.size(), revision};
    callbacks.replace_preview_mesh(callbacks.userdata, &view);
  }
};

extern "C" dcc_voxel_bridge *dcc_voxel_bridge_create(
    dcc_voxel_source_mesh source, float voxel_size, dcc_voxel_sculpt_callbacks callbacks) {
  if (!source.positions || !source.indices || source.vertex_count == 0 ||
      source.index_count == 0 || source.index_count % 3 != 0 || !std::isfinite(voxel_size) || voxel_size <= 0.0F) return nullptr;
  try {
    std::vector<dcc::Vertex> vertices;
    vertices.reserve(source.vertex_count);
    for (size_t i = 0; i < source.vertex_count; ++i) {
      for (size_t j = 0; j < 3; ++j) if (!std::isfinite(source.positions[i * 3 + j])) return nullptr;
      vertices.push_back({{source.positions[i * 3], source.positions[i * 3 + 1],
                           source.positions[i * 3 + 2]}, {}});
    }
    std::vector<uint32_t> indices(source.indices, source.indices + source.index_count);
    for (uint32_t index : indices) if (index >= source.vertex_count) return nullptr;
    return new dcc_voxel_bridge({dcc::MeshId{1}, "Blender voxel sculpt", std::move(vertices),
                                 std::move(indices)}, callbacks, voxel_size);
  } catch (...) {
    return nullptr;
  }
}

extern "C" void dcc_voxel_bridge_destroy(dcc_voxel_bridge *bridge) { delete bridge; }
extern "C" int dcc_voxel_bridge_enter(dcc_voxel_bridge *bridge) {
  try { return bridge && bridge->host.enter() ? 1 : 0; } catch (...) { return 0; }
}
extern "C" void dcc_voxel_bridge_leave(dcc_voxel_bridge *bridge, int commit) {
  try { if (bridge) bridge->host.leave(commit != 0); } catch (...) {}
}
extern "C" int dcc_voxel_bridge_begin_stroke(dcc_voxel_bridge *bridge, int brush_mode,
                                               float radius, float strength, float spacing) {
  if (!bridge || brush_mode < 0 || brush_mode > static_cast<int>(dcc::SculptBrushMode::clay_strips) ||
      !std::isfinite(radius) || !std::isfinite(strength) || !std::isfinite(spacing) ||
      radius <= 0.0F || strength < 0.0F || spacing <= 0.0F) return 0;
  try {
    dcc::SculptBrush brush;
    brush.mode = static_cast<dcc::SculptBrushMode>(brush_mode);
    brush.radius = radius;
    brush.strength = strength;
    brush.spacing = spacing;
    bridge->host.begin_stroke(brush, {});
    return 1;
  } catch (...) { return 0; }
}
extern "C" int dcc_voxel_bridge_dab(dcc_voxel_bridge *bridge, const float origin[3],
                                     const float direction[3], float pressure) {
  if (!bridge || !origin || !direction) return 0;
  if (!std::isfinite(pressure) || pressure < 0.0F) return 0;
  for (int i = 0; i < 3; ++i) if (!std::isfinite(origin[i]) || !std::isfinite(direction[i])) return 0;
  if (direction[0] == 0 && direction[1] == 0 && direction[2] == 0) return 0;
  const dcc::VoxelSculptRay ray{{origin[0], origin[1], origin[2]},
                                {direction[0], direction[1], direction[2]}};
  try { return bridge->host.dab(ray, pressure) ? 1 : 0; } catch (...) { return 0; }
}
extern "C" void dcc_voxel_bridge_end_stroke(dcc_voxel_bridge *bridge) {
  try { if (bridge) bridge->host.end_stroke(); } catch (...) {}
}
extern "C" int dcc_voxel_bridge_undo(dcc_voxel_bridge *bridge) {
  try { return bridge && bridge->host.undo() ? 1 : 0; } catch (...) { return 0; }
}
extern "C" int dcc_voxel_bridge_redo(dcc_voxel_bridge *bridge) {
  try { return bridge && bridge->host.redo() ? 1 : 0; } catch (...) { return 0; }
}
