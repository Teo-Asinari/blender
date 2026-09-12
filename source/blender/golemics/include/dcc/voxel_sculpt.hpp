#pragma once

#include "dcc/core.hpp"

#include <array>
#include <compare>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <set>
#include <span>
#include <unordered_map>
#include <vector>

namespace dcc {

struct VoxelCoord {
  std::int32_t x{}, y{}, z{};
  auto operator<=>(const VoxelCoord&) const = default;
};

// Dependency-free sparse scalar field. Missing cells are empty (density 0).
class SparseVoxelVolume {
public:
  explicit SparseVoxelVolume(float voxel_size = 0.1F);
  [[nodiscard]] float voxel_size() const noexcept { return voxel_size_; }
  [[nodiscard]] float density(VoxelCoord cell) const noexcept;
  void set_density(VoxelCoord cell, float density);
  [[nodiscard]] std::size_t active_voxel_count() const noexcept { return active_voxels_; }
  // Conservative occupied brick bounds in object coordinates, expanded for interpolation.
  [[nodiscard]] std::optional<std::pair<Vec3, Vec3>> bounds() const;
  // Explicit ordered snapshot for diagnostics/interchange; editing uses bricks directly.
  [[nodiscard]] std::map<VoxelCoord, float> cells() const;
  [[nodiscard]] std::size_t active_brick_count() const noexcept { return bricks_.size(); }

  static SparseVoxelVolume voxelize(const Mesh& mesh, float voxel_size);
  void boolean_union(const SparseVoxelVolume& other);
  void boolean_subtract(const SparseVoxelVolume& other);
  void boolean_intersect(const SparseVoxelVolume& other);
  // Re-extracts every brick. This is the reference definition of the surface and
  // the oracle the incremental path is tested against; interactive callers want
  // remesh() instead.
  [[nodiscard]] Mesh extract_surface(MeshId id, std::string name = "Voxel surface",
                                     float iso = 0.5F) const;
  // Same surface as extract_surface, but only bricks whose samples moved since the
  // last call are re-extracted. Changing iso discards the cache, so a caller that
  // alternates isovalues pays the full price each time.
  [[nodiscard]] Mesh remesh(MeshId id, std::string name = "Voxel surface",
                            float iso = 0.5F) const;
  // Diagnostics for the incremental path: how much work the next remesh owes, and
  // how many bricks currently hold cached geometry.
  [[nodiscard]] std::size_t dirty_brick_count() const noexcept { return dirty_.size(); }
  [[nodiscard]] std::size_t cached_brick_count() const noexcept { return cache_.size(); }
private:
  struct Brick { std::array<float, 512> values{}; std::size_t active{}; };
  // Brick keys are looked up per sample on the extraction hot path, so they are
  // hashed rather than ordered; every ordered guarantee is restored explicitly.
  struct BrickHash { std::size_t operator()(const VoxelCoord& cell) const noexcept; };
  // A brick's own triangles, already welded inside the brick: `corners` indexes
  // `positions` three entries per triangle, wound outward. Welding inside the brick
  // is paid once, when the brick is extracted, and spares assembly two thirds of its
  // hash probes on every remesh. Normals are deliberately not stored: welding
  // averages them across bricks, so they are only meaningful after assembly.
  struct BrickGeometry {
    std::vector<Vec3> positions;
    std::vector<std::uint32_t> corners;
    [[nodiscard]] bool empty() const noexcept { return corners.empty(); }
  };
  void touch(VoxelCoord cell);
  void invalidate_cache() noexcept;
  [[nodiscard]] BrickGeometry extract_brick(VoxelCoord key, float iso) const;
  [[nodiscard]] std::map<VoxelCoord, BrickGeometry> build_all(float iso) const;
  [[nodiscard]] Mesh assemble(MeshId id, std::string name,
                              const std::map<VoxelCoord, BrickGeometry>& geometry) const;
  float voxel_size_;
  std::unordered_map<VoxelCoord, Brick, BrickHash> bricks_;
  std::size_t active_voxels_{};
  // Extraction state. It is a pure function of the samples, so const remesh() may
  // rebuild it; ordered containers keep assembly order independent of hashing.
  mutable std::map<VoxelCoord, BrickGeometry> cache_;
  mutable std::set<VoxelCoord> dirty_;
  mutable std::optional<VoxelCoord> touched_;
  mutable float cache_iso_{};
  mutable bool cache_ready_{};
};

// Half-width of the signed-distance band the field stores, in voxels. Density is
// `0.5 + d / (2 * kSculptBand)` clamped to 0..1, where `d` is the distance inside
// the surface measured in voxels, so 0.5 is the isosurface and the field
// saturates kSculptBand voxels either side of it.
//
// This is what makes displacement brushes possible at all. voxelize() still seeds
// a binary occupancy field -- 1 inside, 0 outside -- which carries no distance,
// and a brush written as "lerp the scalar" can then only move the isosurface when
// a cell happens to cross 0.5. Measured, that left flatten at exactly zero effect
// over twenty dabs and clay/inflate at about one voxel. Brushes reconstruct the
// distance locally (see apply_sculpt_stroke) and write band values back, so the
// field becomes a real band wherever a brush has been.
inline constexpr float kSculptBand = 3.0F;
[[nodiscard]] constexpr float sculpt_distance_of(float density) {
  return (density - 0.5F) * (2.0F * kSculptBand);
}
[[nodiscard]] constexpr float sculpt_density_of(float distance) {
  const float d = 0.5F + distance / (2.0F * kSculptBand);
  return d < 0.0F ? 0.0F : (d > 1.0F ? 1.0F : d);
}

enum class SculptBrushMode : std::uint8_t {
  add, subtract, smooth, flatten, inflate, crease, clay_add, clay_strips
};
enum class SculptFalloff : std::uint8_t { linear, smoothstep, constant };
struct SculptBrush {
  SculptBrushMode mode{SculptBrushMode::add};
  float radius{0.5F};
  float strength{0.25F};
  float spacing{0.2F}; // fraction of radius
  SculptFalloff falloff{SculptFalloff::smoothstep};
  bool symmetry_x{false};
  bool symmetry_y{false};
  bool symmetry_z{false};
};
struct SculptSample { Vec3 position; float pressure{1.0F}; };
struct VoxelChange { VoxelCoord cell; float before{}; float after{}; };
struct SculptTransaction { std::vector<VoxelChange> changes; };

[[nodiscard]] std::vector<SculptSample> sample_sculpt_stroke(
    std::span<const SculptSample> control_points, float spacing);
[[nodiscard]] SculptTransaction apply_sculpt_stroke(
    SparseVoxelVolume& volume, std::span<const SculptSample> control_points,
    const SculptBrush& brush);

// Undoable sculpt edits with an optional append-only, crash-tolerant journal.
class VoxelSculptHistory {
public:
  explicit VoxelSculptHistory(SparseVoxelVolume volume = SparseVoxelVolume{},
                              std::filesystem::path journal = {});
  void commit(std::span<const SculptSample> points, const SculptBrush& brush);
  [[nodiscard]] bool undo();
  [[nodiscard]] bool redo();
  [[nodiscard]] const SparseVoxelVolume& volume() const noexcept { return volume_; }
  [[nodiscard]] Mesh remesh(MeshId id, std::string name = "Sculpt") const;
  static VoxelSculptHistory recover(float voxel_size, const std::filesystem::path& journal);
private:
  void apply_changes(const SculptTransaction&, bool forward);
  void append_journal(const SculptTransaction&) const;
  SparseVoxelVolume volume_;
  std::filesystem::path journal_;
  std::vector<SculptTransaction> undo_, redo_;
};

} // namespace dcc
