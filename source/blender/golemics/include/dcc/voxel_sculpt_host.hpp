#pragma once

#include "dcc/sculpt_session.hpp"

#include <cstdint>
#include <functional>

namespace dcc {

// Small host boundary for embedding the voxel session in a DCC viewport. A
// Blender C++ operator can implement these callbacks with evaluated-mesh
// replacement and DEG/viewport tagging; the core remains independent of
// Blender headers and can therefore be compiled and tested here.
struct VoxelSculptHostCallbacks {
  std::function<void(const Mesh&, std::uint64_t)> replace_preview_mesh;
  std::function<void()> restore_source_mesh;
  std::function<void()> tag_geometry_dirty;
  std::function<void()> tag_viewport_redraw;
};

struct VoxelSculptRay { Vec3 origin{}; Vec3 direction{}; };

class VoxelSculptHostBridge {
public:
  VoxelSculptHostBridge(Mesh source, VoxelSculptHostCallbacks callbacks,
                        float voxel_size = kSculptVoxelSize);
  [[nodiscard]] bool enter();
  void leave(bool commit);
  [[nodiscard]] bool in_session() const noexcept { return active_; }
  void begin_stroke(SculptBrush brush, VoxelSculptRay ray);
  [[nodiscard]] bool dab(VoxelSculptRay ray, float pressure = 1.0F);
  void end_stroke();
  [[nodiscard]] bool undo();
  [[nodiscard]] bool redo();
  [[nodiscard]] const Mesh& source_mesh() const noexcept { return source_; }
  [[nodiscard]] const SculptSession& session() const noexcept { return session_; }
private:
  void publish_preview();
  Mesh source_;
  SparseVoxelVolume initial_volume_;
  SculptSession session_;
  VoxelSculptHostCallbacks callbacks_;
  SculptBrush brush_{};
  bool active_{};
  bool stroke_{};
};

} // namespace dcc
