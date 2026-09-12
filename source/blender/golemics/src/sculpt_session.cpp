#include "dcc/sculpt_session.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace dcc {
SculptSession::SculptSession() : SculptSession(SparseVoxelVolume{kSculptVoxelSize}) {
  const float step = volume_.voxel_size();
  for (int z = -7; z <= 7; ++z)
    for (int y = -7; y <= 7; ++y)
      for (int x = -7; x <= 7; ++x) {
        const float radius = step * std::sqrt(static_cast<float>(x*x + y*y + z*z));
        volume_.set_density({x,y,z}, std::clamp(0.5F + (0.45F-radius)/step, 0.F, 1.F));
      }
  bounds_ = volume_.bounds();
}
SculptSession::SculptSession(SparseVoxelVolume volume) : volume_(std::move(volume)) {
  bounds_ = volume_.bounds();
}
void SculptSession::expand_bounds(VoxelCoord cell) {
  const float voxel = volume_.voxel_size();
  const Vec3 lo{(float(cell.x)-1.F)*voxel, (float(cell.y)-1.F)*voxel, (float(cell.z)-1.F)*voxel};
  const Vec3 hi{(float(cell.x)+2.F)*voxel, (float(cell.y)+2.F)*voxel, (float(cell.z)+2.F)*voxel};
  if (!bounds_) { bounds_ = std::pair{lo, hi}; return; }
  auto& [min, max] = *bounds_;
  min = {std::min(min.x,lo.x), std::min(min.y,lo.y), std::min(min.z,lo.z)};
  max = {std::max(max.x,hi.x), std::max(max.y,hi.y), std::max(max.z,hi.z)};
}
void SculptSession::begin() {
  if (open_) return;
  open_ = true;
  dabs_ = 0;
  previous_.reset();
  stroke_.clear();
}

std::optional<Vec3> SculptSession::stroke_point(Vec3 origin, Vec3 direction, float brush_radius) {
  const float len = std::sqrt(direction.x * direction.x + direction.y * direction.y +
                              direction.z * direction.z);
  if (!(len > 0.F)) return std::nullopt;
  const Vec3 unit{direction.x / len, direction.y / len, direction.z / len};
  static_cast<void>(brush_radius);
  // Raycast the density before this stroke. New clay cannot pull the next
  // dab toward the camera, even under pointer jitter or orthographic motion.
  return hit_surface(origin, unit, open_);
}
bool SculptSession::dab(Vec3 p, SculptBrush brush, float pressure) {
  if (!open_ || dabs_ >= kSculptStrokeDabs || !std::isfinite(p.x) || !std::isfinite(p.y) ||
      !std::isfinite(p.z)) return false;
  if (!std::isfinite(brush.radius) || brush.radius <= 0 ||
      !std::isfinite(brush.strength) || brush.strength <= 0 ||
      !std::isfinite(brush.spacing) || brush.spacing <= 0 ||
      !std::isfinite(pressure) || pressure <= 0) return false;
  if (previous_) {
    const float dx=p.x-previous_->x, dy=p.y-previous_->y, dz=p.z-previous_->z;
    const float spacing = brush.radius * brush.spacing;
    if (dx*dx+dy*dy+dz*dz < spacing*spacing) return false;
  }
  const SculptSample sample{p, std::clamp(std::isfinite(pressure) ? pressure : 1.0F, 0.0F, 1.0F)};
  auto tx = apply_sculpt_stroke(volume_, std::span{&sample, std::size_t{1}}, brush);
  for (const auto& change : tx.changes) {
    expand_bounds(change.cell);
    const auto [it, inserted] = stroke_.try_emplace(change.cell, change);
    if (!inserted) it->second.after = change.after;
  }
  previous_ = p;
  ++dabs_;
  if (!tx.changes.empty()) ++revision_;
  return !tx.changes.empty();
}
void SculptSession::finish() {
  if (!open_) return;
  open_ = false;
  SculptTransaction tx;
  for (const auto& [cell, change] : stroke_) {
    static_cast<void>(cell);
    if (change.before != change.after) tx.changes.push_back(change);
  }
  stroke_.clear();
  if (tx.changes.empty()) return;
  redo_.clear();
  undo_.push_back(std::move(tx));
  if (undo_.size() > kSculptUndoDepth) undo_.erase(undo_.begin());
}
void SculptSession::apply(const SculptTransaction& tx, bool forward) {
  for (const auto& c : tx.changes) {
    volume_.set_density(c.cell, forward ? c.after : c.before);
    expand_bounds(c.cell);
  }
  ++revision_;
}
bool SculptSession::undo() {
  if (open_ || undo_.empty()) return false;
  apply(undo_.back(), false);
  redo_.push_back(std::move(undo_.back())); undo_.pop_back(); return true;
}
bool SculptSession::redo() {
  if (open_ || redo_.empty()) return false;
  apply(redo_.back(), true);
  undo_.push_back(std::move(redo_.back())); redo_.pop_back(); return true;
}
Mesh SculptSession::mesh() const { return volume_.remesh(MeshId{1}, "Sculpt sphere"); }
Mesh SculptSession::mesh_full() const { return volume_.extract_surface(MeshId{1}, "Sculpt sphere"); }
float SculptSession::sample(Vec3 p, bool original) const {
  // Trilinear between cell centres, which is the field the extractor meshes. A
  // nearest-cell probe reports the isosurface a whole cell at a time, and the
  // brush centre it hands back is quantized to that same grid.
  const float voxel = volume_.voxel_size();
  const float ux = p.x/voxel-0.5F, uy = p.y/voxel-0.5F, uz = p.z/voxel-0.5F;
  const float fx = std::floor(ux), fy = std::floor(uy), fz = std::floor(uz);
  const auto ix = static_cast<std::int32_t>(fx), iy = static_cast<std::int32_t>(fy),
             iz = static_cast<std::int32_t>(fz);
  const float tx = ux-fx, ty = uy-fy, tz = uz-fz;
  float out = 0.F;
  const auto density = [&](VoxelCoord cell) {
    if (original) {
      const auto found = stroke_.find(cell);
      if (found != stroke_.end()) return found->second.before;
    }
    return volume_.density(cell);
  };
  for (int dz=0; dz<2; ++dz) for (int dy=0; dy<2; ++dy) for (int dx=0; dx<2; ++dx)
    out += density({ix+dx,iy+dy,iz+dz}) * (dx?tx:1.F-tx) * (dy?ty:1.F-ty) * (dz?tz:1.F-tz);
  return out;
}
std::optional<Vec3> SculptSession::hit(Vec3 origin, Vec3 direction) const {
  return hit_surface(origin, direction, false);
}
std::optional<Vec3> SculptSession::hit_surface(Vec3 origin, Vec3 direction, bool original) const {
  const float length = std::sqrt(direction.x*direction.x+direction.y*direction.y+direction.z*direction.z);
  if (!(length > 0.F)) return std::nullopt;
  direction = {direction.x/length,direction.y/length,direction.z/length};
  // Slab-test the occupied volume, not a fixed demo workspace around the origin.
  // The cached box expands with edits; ray casting never scans camera distance.
  if (!bounds_) return std::nullopt;
  const auto& [min, max] = *bounds_;
  const float lower[3]{min.x,min.y,min.z}, upper[3]{max.x,max.y,max.z};
  double near = 0., far = std::numeric_limits<double>::max();
  const float o[3]{origin.x,origin.y,origin.z}, d[3]{direction.x,direction.y,direction.z};
  for (int axis=0; axis<3; ++axis) {
    if (std::abs(d[axis]) < 1e-9F) { if (o[axis] < lower[axis] || o[axis] > upper[axis]) return std::nullopt; continue; }
    double lo = (double(lower[axis])-o[axis])/d[axis], hi = (double(upper[axis])-o[axis])/d[axis];
    if (lo > hi) std::swap(lo, hi);
    near = std::max(near, lo);
    far = std::min(far, hi);
  }
  if (near > far) return std::nullopt;
  // Half-voxel steps cannot step over a cell, and the crossing is then interpolated
  // rather than snapped: a brush that tracks the surface it is building needs the
  // hit to move as smoothly as the surface does.
  const float step = volume_.voxel_size()*0.5F;
  const auto point_at = [&](double t) { return Vec3{float(origin.x+t*direction.x),float(origin.y+t*direction.y),float(origin.z+t*direction.z)}; };
  double previous_t = near;
  float previous = sample(point_at(near), original);
  if (previous >= 0.5F) return point_at(near);
  for (double t = near+step; t <= far+step; t += step) {
    const double clamped = std::min(t, far);
    const float value = sample(point_at(clamped), original);
    if (value >= 0.5F) {
      const float span = value-previous;
      const double crossing = span > 0.F ? previous_t + (clamped-previous_t)*(0.5F-previous)/span : clamped;
      return point_at(crossing);
    }
    previous_t = clamped; previous = value;
    if (clamped >= far) break;
  }
  return std::nullopt;
}
} // namespace dcc
