#include "dcc/gpu_paint_session.hpp"

#include <cassert>
#include <cstdint>
#include <vector>

namespace {
struct Provider final : dcc::PaintGpuResourceProvider {
  std::uint64_t next{1};
  std::vector<std::uint64_t> created, destroyed, reads, uploads;
  dcc::PaintTextureId create_texture(dcc::PaintTextureDesc) override { created.push_back(next); return next++; }
  void destroy_texture(dcc::PaintTextureId id) noexcept override { destroyed.push_back(id); }
  dcc::PaintFramebufferId create_framebuffer(std::span<const dcc::PaintTextureId>, dcc::PaintTextureId) override { return 1; }
  void destroy_framebuffer(dcc::PaintFramebufferId) noexcept override {}
  void readback(dcc::PaintTextureId id, dcc::ImageRegion, std::span<std::uint8_t> bytes) override {
    reads.push_back(id); for (auto& byte : bytes) byte = 42;
  }
  void upload(dcc::PaintTextureId id, dcc::ImageRegion, std::span<const std::uint8_t>) override { uploads.push_back(id); }
};
struct Renderer final : dcc::PaintGpuRenderer {
  std::size_t submits{}, composites{};
  dcc::PaintFlushResult submit_dabs(dcc::PaintTextureId target, dcc::PaintGeometry geometry,
                                    dcc::PaintView view, dcc::PaintDabBatch batch,
                                    dcc::PaintBrushState, dcc::ImageRegion) override {
    assert(target != 0 && geometry.revision == 7 && view.viewport_width == 100);
    assert(batch.dabs.size() == 1); ++submits;
    return {{2, 3, 4, 5}, batch.dabs.size(), true, false};
  }
  dcc::PaintFlushResult submit_dabs_multi(std::span<const dcc::PaintTextureId> targets,
                                          dcc::PaintGeometry geometry, dcc::PaintView view,
                                          dcc::PaintDabBatch batch, dcc::PaintBrushState,
                                          dcc::ImageRegion) override {
    assert(targets.size() == 2 && geometry.revision == 7 && view.viewport_width == 100);
    assert(batch.dabs.size() == 1);
    ++submits;
    return {{2, 3, 4, 5}, batch.dabs.size(), true, false};
  }
  void composite(std::span<const dcc::PaintTextureId> output,
                 std::span<const dcc::PaintTextureId> layers,
                 std::span<const dcc::PaintLayerState> states, dcc::ImageRegion) override {
    assert(output.size() == 7 && layers.size() == 7 && states.size() == 1); ++composites;
  }
};
struct LegacyRenderer final : dcc::PaintGpuRenderer {
  std::vector<dcc::PaintTextureId> targets;
  dcc::PaintFlushResult submit_dabs(dcc::PaintTextureId target, dcc::PaintGeometry,
                                    dcc::PaintView, dcc::PaintDabBatch batch,
                                    dcc::PaintBrushState, dcc::ImageRegion dirty) override {
    targets.push_back(target);
    return {dirty, batch.dabs.size(), true, false};
  }
  void composite(std::span<const dcc::PaintTextureId>, std::span<const dcc::PaintTextureId>,
                 std::span<const dcc::PaintLayerState>, dcc::ImageRegion) override {}
};
struct Commands final : dcc::PaintGpuCommandContext {
  std::size_t uploads{}, binds{}, dabs{}, composites{};
  void bind_framebuffer(dcc::PaintFramebufferId id) override { assert(id != 0); ++binds; }
  void bind_texture(dcc::PaintTextureId, std::uint32_t) override {}
  void upload_geometry(dcc::PaintGeometry geometry) override { assert(geometry.revision == 7); ++uploads; }
  void draw_dabs(std::span<const dcc::PaintTextureId> targets, dcc::PaintFramebufferId framebuffer,
                 dcc::PaintView view, dcc::PaintDabBatch batch, dcc::PaintBrushState,
                 dcc::ImageRegion) override {
    assert(targets.size() == 2 && targets[0] != 0 && targets[1] != 0 && framebuffer != 0 && view.scene_depth == 99 && batch.dabs.size() == 1); ++dabs;
  }
  void draw_composite(std::span<const dcc::PaintTextureId> output,
                      std::span<const dcc::PaintTextureId> layers,
                      std::span<const dcc::PaintLayerState> states, dcc::ImageRegion) override {
    assert(output.size() == 7 && layers.size() == 7 && states.size() == 1); ++composites;
  }
};
} // namespace

int main() {
  Provider provider;
  std::vector<dcc::PaintVertex> vertices{{{}, 0.F, 0.F}, {{1.F, 0.F, 0.F}, 1.F, 0.F}, {{}, 0.F, 1.F}};
  std::vector<std::uint32_t> indices{0, 1, 2};
  dcc::GpuPaintSession session(provider, {vertices, indices, 7}, 16);
  assert(provider.created.size() == 14); // composite + one base layer
  assert(session.geometry_revision() == 7);
  assert(!session.depth_cache_valid());

  dcc::PaintView view{}; view.scene_depth = 99; view.viewport_width = 100; view.viewport_height = 80;
  session.set_view(view); assert(!session.depth_cache_valid());
  session.begin_stroke();
  const std::vector<dcc::PaintDab> dabs{{10.F, 10.F, 2.F, 1.F}};
  const auto result = session.submit({dabs});
  assert(result.submitted && result.dabs == 1 && !result.performed_readback);
  session.end_stroke();

  const dcc::PaintReadbackRequest request{session.layer_texture(0, 0), {0, 0, 2, 2}};
  const auto saved = session.readback(std::span{&request, 1});
  assert(provider.reads.size() == 1 && saved[0].rgba.size() == 16 && saved[0].rgba[0] == 42);
  session.restore(saved); assert(provider.uploads.size() == 1);

  Renderer renderer;
  dcc::GpuPaintSession rendered(provider, renderer, {vertices, indices, 7}, 16);
  rendered.set_view(view); dcc::PaintBrushState rendered_brush{};
  rendered_brush.channel_values[3] = 1.F; rendered_brush.channel_values[7] = 1.F;
  rendered.set_brush(rendered_brush);
  const auto rendered_targets = rendered.active_channel_targets();
  assert(rendered_targets.count == 2 && rendered_targets.ids[0] == rendered.layer_texture(0, 0) &&
         rendered_targets.ids[1] == rendered.layer_texture(0, 1));
  rendered.begin_stroke();
  const auto rendered_result = rendered.submit({dabs}); rendered.end_stroke();
  const dcc::ImageRegion expected_dirty{2, 3, 4, 5};
  assert(rendered_result.dirty == expected_dirty && renderer.submits == 1);
  rendered.composite(); assert(renderer.composites == 1);

  LegacyRenderer legacy;
  dcc::GpuPaintSession legacy_session(provider, legacy, {vertices, indices, 7}, 16);
  legacy_session.set_view(view); legacy_session.set_brush(rendered_brush); legacy_session.begin_stroke();
  static_cast<void>(legacy_session.submit({dabs})); legacy_session.end_stroke();
  assert(legacy.targets.size() == 2 && legacy.targets[0] != legacy.targets[1]);

  Commands commands;
  dcc::GpuPaintSession commanded(provider, commands, {vertices, indices, 7}, 16);
  commanded.set_view(view); dcc::PaintBrushState brush{}; brush.channel_values[3] = 1.F; brush.channel_values[7] = 1.F; commanded.set_brush(brush); commanded.begin_stroke();
  static_cast<void>(commanded.submit({dabs})); commanded.end_stroke();
  assert(commands.uploads == 1 && commands.dabs == 1 && commands.binds == 1);
  commanded.composite(); assert(commands.uploads == 1 && commands.composites == 1 && commands.binds == 2);

  const auto before = provider.created.size();
  std::vector<dcc::PaintVertex> changed = vertices; changed[0].u = 0.25F;
  session.set_geometry({changed, indices, 8}); assert(session.geometry_revision() == 8);
  session.abandon_context(); assert(session.context_abandoned());
  const auto destroyed_before = provider.destroyed.size();
  session.context_restored(); assert(!session.context_abandoned());
  assert(provider.created.size() == before + 14);
  assert(provider.destroyed.size() == destroyed_before);
  return 0;
}
