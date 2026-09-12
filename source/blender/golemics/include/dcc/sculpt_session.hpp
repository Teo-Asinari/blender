#pragma once
#include "dcc/voxel_sculpt.hpp"
#include <optional>

namespace dcc {
// History is bounded; spatial extent follows the source volume and edited cells.
inline constexpr float kSculptVoxelSize = 0.075F;
inline constexpr std::size_t kSculptStrokeDabs = 1024;
inline constexpr std::size_t kSculptUndoDepth = 64;

// One drag is one undo record. Brush work budgets are enforced by the voxel engine,
// without silently changing the host-requested radius or strength.
class SculptSession {
public:
  SculptSession();
  explicit SculptSession(SparseVoxelVolume volume);
  void begin();
  bool dab(Vec3 position, SculptBrush brush, float pressure = 1.0F);
  void finish();
  bool undo();
  bool redo();
  // Incremental: only bricks the last dab touched are re-extracted.
  [[nodiscard]] Mesh mesh() const;
  // The from-scratch surface. Kept as the oracle the incremental path is pinned
  // against; interactive code has no reason to call it.
  [[nodiscard]] Mesh mesh_full() const;
  // Bumped whenever a dab, undo, or redo changes geometry, so a viewport can skip
  // an upload it does not owe.
  [[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }
  [[nodiscard]] const SparseVoxelVolume& volume() const noexcept { return volume_; }
  [[nodiscard]] std::optional<Vec3> hit(Vec3 origin, Vec3 direction) const;
  // Raycast the pre-stroke surface so repeated dabs cannot chase their own clay.
  [[nodiscard]] std::optional<Vec3> stroke_point(Vec3 origin, Vec3 direction, float brush_radius);
  [[nodiscard]] bool stroke_open() const noexcept { return open_; }
private:
  void apply(const SculptTransaction&, bool forward);
  void expand_bounds(VoxelCoord cell);
  std::optional<std::pair<Vec3, Vec3>> bounds_;
  [[nodiscard]] float sample(Vec3 point, bool original) const;
  [[nodiscard]] std::optional<Vec3> hit_surface(Vec3 origin, Vec3 direction, bool original) const;
  SparseVoxelVolume volume_{kSculptVoxelSize};
  std::map<VoxelCoord, VoxelChange> stroke_;
  std::vector<SculptTransaction> undo_, redo_;
  bool open_{};
  std::uint64_t revision_{};
  std::size_t dabs_{};
  std::optional<Vec3> previous_;
};
} // namespace dcc
