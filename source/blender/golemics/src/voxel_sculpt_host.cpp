#include "dcc/voxel_sculpt_host.hpp"

#include <stdexcept>
#include <utility>

namespace dcc {
VoxelSculptHostBridge::VoxelSculptHostBridge(Mesh source,
                                             VoxelSculptHostCallbacks callbacks,
                                             float voxel_size)
    : source_(std::move(source)),
      initial_volume_(SparseVoxelVolume::voxelize(source_, voxel_size)),
      session_(initial_volume_),
      callbacks_(std::move(callbacks)) {}
bool VoxelSculptHostBridge::enter() {
  if (active_) return false;
  active_ = true;
  publish_preview();
  return true;
}
void VoxelSculptHostBridge::leave(bool commit) {
  if (!active_) return;
  if (stroke_) end_stroke();
  if (commit) {
    source_ = session_.mesh();
    // Enter and each successful dab already published this exact surface.
    // Commit hands off ownership without rebuilding the host mesh a second time.
    if (callbacks_.tag_geometry_dirty) callbacks_.tag_geometry_dirty();
  } else if (callbacks_.restore_source_mesh) {
    callbacks_.restore_source_mesh();
  }
  active_ = false;
}
void VoxelSculptHostBridge::begin_stroke(SculptBrush brush, VoxelSculptRay ray) {
  if (!active_) throw std::logic_error("voxel sculpt session is not active");
  if (stroke_) throw std::logic_error("voxel sculpt stroke is already open");
  brush_ = brush;
  session_.begin();
  stroke_ = true;
  static_cast<void>(ray);
}
bool VoxelSculptHostBridge::dab(VoxelSculptRay ray, float pressure) {
  if (!active_ || !stroke_) return false;
  const auto point = session_.stroke_point(ray.origin, ray.direction, brush_.radius);
  if (!point || !session_.dab(*point, brush_, pressure)) return false;
  publish_preview();
  return true;
}
void VoxelSculptHostBridge::end_stroke() { if (stroke_) { session_.finish(); stroke_ = false; } }
bool VoxelSculptHostBridge::undo() {
  if (!active_ || stroke_ || !session_.undo()) return false;
  publish_preview(); return true;
}
bool VoxelSculptHostBridge::redo() {
  if (!active_ || stroke_ || !session_.redo()) return false;
  publish_preview(); return true;
}
void VoxelSculptHostBridge::publish_preview() {
  if (callbacks_.replace_preview_mesh) callbacks_.replace_preview_mesh(session_.mesh(), session_.revision());
  if (callbacks_.tag_viewport_redraw) callbacks_.tag_viewport_redraw();
}
} // namespace dcc
