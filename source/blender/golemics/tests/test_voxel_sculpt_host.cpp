#include "dcc/voxel_sculpt_host.hpp"
#include "dcc/voxel_sculpt_c_api.h"
#include <cassert>
#include <limits>
#include <stdexcept>
int main() {
  using namespace dcc;
  Mesh source{MeshId{42}, "host source",
              {{Vec3{-0.5F, -0.5F, -0.5F}}, {Vec3{0.5F, -0.5F, -0.5F}},
               {Vec3{0.0F, 0.5F, 0.0F}}, {Vec3{0.0F, 0.0F, 0.5F}}},
              {0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0}};
  int previews = 0, restores = 0, redraws = 0;
  VoxelSculptHostBridge bridge(source,
      {.replace_preview_mesh = [&](const Mesh&, std::uint64_t) { ++previews; },
       .restore_source_mesh = [&] { ++restores; }, .tag_geometry_dirty = {},
       .tag_viewport_redraw = [&] { ++redraws; }});
  assert(bridge.enter());
  bridge.begin_stroke(SculptBrush{}, {{0.F, 0.F, 3.F}, {0.F, 0.F, -1.F}});
  static_cast<void>(bridge.dab({{0.F, 0.F, 3.F}, {0.F, 0.F, -1.F}}));
  bridge.end_stroke();
  assert(previews >= 1); assert(redraws >= 1);
  bridge.leave(false); assert(restores == 1); assert(!bridge.in_session());

  int committed_previews = 0; std::uint64_t committed_revision = 0;
  VoxelSculptHostBridge committed(source,
      {.replace_preview_mesh = [&](const Mesh&, std::uint64_t revision) {
         ++committed_previews; committed_revision = revision;
       },
       .restore_source_mesh = {}, .tag_geometry_dirty = {}, .tag_viewport_redraw = {}});
  assert(committed.enter());
  committed.begin_stroke(SculptBrush{}, {{0.F, 0.F, 3.F}, {0.F, 0.F, -1.F}});
  static_cast<void>(committed.dab({{0.F, 0.F, 3.F}, {0.F, 0.F, -1.F}}));
  committed.end_stroke();
  const int previews_before_commit = committed_previews;
  committed.leave(true);
  assert(committed_previews == previews_before_commit); // Commit keeps the current preview.
  assert(committed_previews >= 2);
  assert(committed_revision != 0);

  /* Exercise the optional C boundary used by DCCs that cannot include the C++
   * core headers.  Callback mesh storage is borrowed only for this call. */
  const float positions[] = {-0.5F, -0.5F, -0.5F, 0.5F, -0.5F, -0.5F,
                             0.0F, 0.5F, 0.0F, 0.0F, 0.0F, 0.5F};
  const uint32_t indices[] = {0, 1, 2, 0, 3, 1, 1, 3, 2, 2, 3, 0};
  int c_previews = 0;
  const dcc_voxel_sculpt_callbacks c_callbacks{
      &c_previews,
      [](void *data, const dcc_voxel_mesh *mesh) {
        assert(mesh && mesh->vertex_count && mesh->index_count);
        ++*static_cast<int *>(data);
      }, nullptr, nullptr, nullptr};
  const dcc_voxel_source_mesh c_source{positions, 4, indices, 12};
  dcc_voxel_bridge *c_bridge = dcc_voxel_bridge_create(c_source, 0.1F, c_callbacks);
  assert(c_bridge);
  assert(dcc_voxel_bridge_enter(c_bridge));
  assert(dcc_voxel_bridge_begin_stroke(c_bridge, 0, 0.5F, 0.25F, 0.2F));
  const float origin[] = {0.0F, 0.0F, 3.0F};
  const float direction[] = {0.0F, 0.0F, -1.0F};
  assert(dcc_voxel_bridge_dab(c_bridge, origin, direction, 1.0F));
  // Stroke undo is forbidden until the transaction is closed.
  assert(!dcc_voxel_bridge_undo(c_bridge));
  dcc_voxel_bridge_end_stroke(c_bridge);
  const int previews_before_undo = c_previews;
  assert(dcc_voxel_bridge_undo(c_bridge));
  assert(c_previews == previews_before_undo + 1);
  assert(dcc_voxel_bridge_redo(c_bridge));
  assert(c_previews == previews_before_undo + 2);
  dcc_voxel_bridge_leave(c_bridge, 0);
  assert(c_previews >= 1);
  assert(!dcc_voxel_bridge_dab(c_bridge, origin, direction, std::numeric_limits<float>::quiet_NaN()));
  assert(!dcc_voxel_bridge_begin_stroke(c_bridge, 0, std::numeric_limits<float>::quiet_NaN(), 1, 0.2F));
  dcc_voxel_bridge_destroy(c_bridge);
  assert(!dcc_voxel_bridge_create(c_source, std::numeric_limits<float>::quiet_NaN(), c_callbacks));
  // Exceptions from preview extraction/callbacks must never cross Blender's C boundary.
  auto throwing_callbacks = c_callbacks;
  throwing_callbacks.replace_preview_mesh = [](void *, const dcc_voxel_mesh *) {
    throw std::runtime_error("preview failure");
  };
  c_bridge = dcc_voxel_bridge_create(c_source, 0.1F, throwing_callbacks);
  assert(c_bridge);
  assert(!dcc_voxel_bridge_enter(c_bridge));
  dcc_voxel_bridge_leave(c_bridge, 1);
  dcc_voxel_bridge_destroy(c_bridge);
  // Blender mesh coordinates and brush sizes are not restricted to the starter sphere.
  SparseVoxelVolume offset_volume(0.2F);
  for (int z = 20; z < 25; ++z)
    for (int y = -100; y < -95; ++y)
      for (int x = 100; x < 105; ++x)
        offset_volume.set_density({x, y, z}, 1.F);
  SculptSession offset_session(offset_volume);
  const auto offset_hit = offset_session.hit({20.5F, -19.5F, 10.F}, {0.F, 0.F, -1.F});
  assert(offset_hit && offset_hit->z > 4.F && offset_hit->z < 5.2F);
  assert(!offset_session.hit({0.F, 0.F, 10.F}, {0.F, 0.F, -1.F}));
  SculptBrush wide_brush;
  wide_brush.radius = 0.7F;
  wide_brush.strength = 0.8F;
  offset_session.begin();
  auto zero_brush = wide_brush;
  zero_brush.strength = 0.F;
  assert(!offset_session.dab(*offset_hit, zero_brush));
  assert(offset_session.volume().cells() == offset_volume.cells());
  const SculptSample expected_dab{*offset_hit, 1.F};
  const auto expected_tx = apply_sculpt_stroke(offset_volume, std::span{&expected_dab, std::size_t{1}}, wide_brush);
  assert(!expected_tx.changes.empty());
  assert(offset_session.dab(*offset_hit, wide_brush));
  assert(offset_session.volume().cells() == offset_volume.cells());
  offset_session.finish();
  assert(offset_session.undo());
  assert(offset_session.redo());
  assert(offset_session.volume().cells() == offset_volume.cells());

}
