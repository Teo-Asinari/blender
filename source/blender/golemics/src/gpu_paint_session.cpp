#include "dcc/gpu_paint_session.hpp"

#include <algorithm>
#include <stdexcept>

namespace dcc {
namespace {
bool same(const std::array<float, 16>& a, const std::array<float, 16>& b) {
  return a == b;
}
bool valid_region(ImageRegion r, std::uint32_t width, std::uint32_t height) {
  return r.width != 0 && r.height != 0 && r.x <= width && r.y <= height &&
         r.width <= width - r.x && r.height <= height - r.y;
}
} // namespace

struct GpuPaintSession::Impl {
  PaintGpuResourceProvider* provider{};
  PaintGpuRenderer* renderer{};
  PaintGpuCommandContext* commands{};
  PaintGeometry geometry{};
  PaintView view{};
  std::uint32_t resolution{};
  std::array<PaintTextureId, kGpuPaintChannelCount> composite{};
  std::vector<std::array<PaintTextureId, kGpuPaintChannelCount>> layers;
  std::vector<PaintFramebufferId> framebuffers;
  std::vector<PaintLayerState> layer_state;
  bool abandoned{};
  bool stroke{};
  bool depth_valid{};
  PaintBrushState brush{};
  std::uint64_t uploaded_geometry_revision{~std::uint64_t{0}};

  explicit Impl(PaintGpuResourceProvider& p, PaintGeometry g, std::uint32_t size)
      : provider(&p), geometry(g), resolution(size) {
    if (resolution == 0) throw std::invalid_argument("paint resolution must be positive");
    if (geometry.indices.size() % 3 != 0) throw std::invalid_argument("paint indices must be triangles");
  }
  void allocate_array(std::array<PaintTextureId, kGpuPaintChannelCount>& out) {
    try {
      for (auto& id : out) id = provider->create_texture({resolution, resolution});
    } catch (...) {
      for (auto& id : out) if (id != 0) provider->destroy_texture(id);
      out.fill(0);
      throw;
    }
  }
  void destroy_array(std::array<PaintTextureId, kGpuPaintChannelCount>& ids) noexcept {
    if (abandoned) { ids.fill(0); return; }
    for (auto& id : ids) if (id != 0) provider->destroy_texture(id);
    ids.fill(0);
  }
  void allocate_all() {
    allocate_array(composite);
    try {
      for (auto& layer : layers) allocate_array(layer);
      framebuffers.push_back(provider->create_framebuffer(composite, 0));
      for (auto& layer : layers) framebuffers.push_back(provider->create_framebuffer(layer, 0));
    }
    catch (...) { destroy_framebuffers(); for (auto& layer : layers) destroy_array(layer); destroy_array(composite); throw; }
  }
  void destroy_framebuffers() noexcept {
    if (!abandoned) for (const auto id : framebuffers) if (id != 0) provider->destroy_framebuffer(id);
    framebuffers.clear();
  }
};

GpuPaintSession::GpuPaintSession(PaintGpuResourceProvider& provider, PaintGeometry geometry,
                                 std::uint32_t resolution)
    : impl_(new Impl(provider, geometry, resolution)) {
  try {
    impl_->layers.emplace_back();
    impl_->layer_state.emplace_back();
    impl_->allocate_all();
  } catch (...) { delete impl_; impl_ = nullptr; throw; }
}
GpuPaintSession::GpuPaintSession(PaintGpuResourceProvider& provider, PaintGpuRenderer& renderer,
                                 PaintGeometry geometry, std::uint32_t resolution)
    : impl_(new Impl(provider, geometry, resolution)) {
  impl_->renderer = &renderer;
  try {
    impl_->layers.emplace_back();
    impl_->layer_state.emplace_back();
    impl_->allocate_all();
  } catch (...) { delete impl_; impl_ = nullptr; throw; }
}
GpuPaintSession::GpuPaintSession(PaintGpuResourceProvider& provider, PaintGpuCommandContext& commands,
                                 PaintGeometry geometry, std::uint32_t resolution)
    : impl_(new Impl(provider, geometry, resolution)) {
  impl_->commands = &commands;
  try {
    impl_->layers.emplace_back();
    impl_->layer_state.emplace_back();
    impl_->allocate_all();
  } catch (...) { delete impl_; impl_ = nullptr; throw; }
}
GpuPaintSession::~GpuPaintSession() {
  if (impl_ != nullptr) {
    impl_->destroy_framebuffers();
    impl_->destroy_array(impl_->composite);
    for (auto& layer : impl_->layers) impl_->destroy_array(layer);
    delete impl_;
  }
}
void GpuPaintSession::set_geometry(PaintGeometry geometry) {
  if (impl_->stroke) throw std::logic_error("cannot replace paint geometry during a stroke");
  if (geometry.indices.size() % 3 != 0) throw std::invalid_argument("paint indices must be triangles");
  impl_->geometry = geometry; impl_->depth_valid = false;
}
void GpuPaintSession::set_view(PaintView view) {
  impl_->depth_valid = impl_->depth_valid && impl_->view.scene_depth == view.scene_depth &&
      impl_->view.viewport_width == view.viewport_width && impl_->view.viewport_height == view.viewport_height &&
      same(impl_->view.model, view.model) && same(impl_->view.view_projection, view.view_projection);
  impl_->view = view;
}
std::uint64_t GpuPaintSession::geometry_revision() const noexcept { return impl_->geometry.revision; }
bool GpuPaintSession::depth_cache_valid() const noexcept { return impl_->depth_valid; }
void GpuPaintSession::begin_stroke() { if (impl_->stroke) throw std::logic_error("paint stroke already open"); impl_->stroke = true; }
PaintFlushResult GpuPaintSession::submit(PaintDabBatch batch) {
  if (!impl_->stroke) throw std::logic_error("paint stroke is not open");
  PaintFlushResult out; out.dabs = batch.dabs.size(); out.submitted = !batch.dabs.empty();
  if (impl_->renderer != nullptr && out.submitted) {
    const PaintTargetSet targets = active_channel_targets();
    out = impl_->renderer->submit_dabs_multi(
        targets.span(), impl_->geometry, impl_->view, batch,
        impl_->brush, {});
    out.dabs = batch.dabs.size();
  }
  if (impl_->commands != nullptr && out.submitted) {
    static constexpr std::size_t kBaseLayer = 0;
    if (impl_->geometry.revision != impl_->uploaded_geometry_revision) {
      impl_->commands->upload_geometry(impl_->geometry);
      impl_->uploaded_geometry_revision = impl_->geometry.revision;
    }
    impl_->commands->bind_framebuffer(impl_->framebuffers[1 + kBaseLayer]);
    const PaintTargetSet targets = active_channel_targets();
    impl_->commands->draw_dabs(targets.span(),
                               impl_->framebuffers[1 + kBaseLayer], impl_->view, batch,
                               impl_->brush, {});
  }
  return out;
}
void GpuPaintSession::set_brush(PaintBrushState brush) { impl_->brush = brush; }
PaintTargetSet GpuPaintSession::active_channel_targets() const noexcept {
  PaintTargetSet targets;
  if (impl_->layers.empty()) return targets;
  for (std::size_t channel = 0; channel < kGpuPaintChannelCount; ++channel) {
    if (impl_->brush.channel_values[channel * 4 + 3] > 0.F) {
      targets.ids[targets.count++] = impl_->layers[0][channel];
    }
  }
  return targets;
}
void GpuPaintSession::end_stroke() { if (!impl_->stroke) throw std::logic_error("paint stroke is not open"); impl_->stroke = false; }
std::array<PaintTextureId, kGpuPaintChannelCount> GpuPaintSession::channel_textures() const noexcept { return impl_->composite; }
PaintTextureId GpuPaintSession::layer_texture(std::size_t layer, std::size_t channel) const {
  if (layer >= impl_->layers.size() || channel >= kGpuPaintChannelCount) throw std::out_of_range("paint texture index");
  return impl_->layers[layer][channel];
}
std::size_t GpuPaintSession::add_layer(PaintLayerState state) {
  std::array<PaintTextureId, kGpuPaintChannelCount> layer{};
  impl_->allocate_array(layer);
  try {
    const auto framebuffer = impl_->provider->create_framebuffer(layer, 0);
    impl_->layers.push_back(layer);
    impl_->framebuffers.push_back(framebuffer);
    impl_->layer_state.push_back(state);
  } catch (...) { impl_->destroy_array(layer); throw; }
  return impl_->layers.size() - 1;
}
void GpuPaintSession::set_layer(std::size_t layer, PaintLayerState state) { if (layer >= impl_->layer_state.size()) throw std::out_of_range("paint layer index"); impl_->layer_state[layer] = state; }
void GpuPaintSession::composite(ImageRegion region) {
  if (region.width != 0 && !valid_region(region, impl_->resolution, impl_->resolution)) throw std::out_of_range("paint composite region");
  if (impl_->renderer != nullptr) {
    std::vector<PaintTextureId> channels;
    channels.reserve(impl_->layers.size() * kGpuPaintChannelCount);
    for (const auto& layer : impl_->layers) channels.insert(channels.end(), layer.begin(), layer.end());
    impl_->renderer->composite(impl_->composite, channels, impl_->layer_state, region);
  }
  if (impl_->commands != nullptr) {
    if (impl_->geometry.revision != impl_->uploaded_geometry_revision) {
      impl_->commands->upload_geometry(impl_->geometry);
      impl_->uploaded_geometry_revision = impl_->geometry.revision;
    }
    impl_->commands->bind_framebuffer(impl_->framebuffers[0]);
    std::vector<PaintTextureId> channels;
    channels.reserve(impl_->layers.size() * kGpuPaintChannelCount);
    for (const auto& layer : impl_->layers) channels.insert(channels.end(), layer.begin(), layer.end());
    impl_->commands->draw_composite(impl_->composite, channels, impl_->layer_state, region);
  }
}
std::vector<PaintReadback> GpuPaintSession::readback(std::span<const PaintReadbackRequest> requests) {
  if (impl_->abandoned) throw std::logic_error("paint context is abandoned");
  std::vector<PaintReadback> out; out.reserve(requests.size());
  for (const auto& request : requests) {
    if (!valid_region(request.region, impl_->resolution, impl_->resolution)) throw std::out_of_range("paint readback region");
    PaintReadback value{request, std::vector<std::uint8_t>(static_cast<std::size_t>(request.region.width) * request.region.height * 4)};
    impl_->provider->readback(request.texture, request.region, value.rgba); out.push_back(std::move(value));
  }
  return out;
}
void GpuPaintSession::restore(std::span<const PaintReadback> values) {
  if (impl_->abandoned) throw std::logic_error("paint context is abandoned");
  for (const auto& value : values) {
    const auto bytes = static_cast<std::size_t>(value.request.region.width) * value.request.region.height * 4;
    if (!valid_region(value.request.region, impl_->resolution, impl_->resolution) || value.rgba.size() != bytes) throw std::invalid_argument("invalid paint restore");
    impl_->provider->upload(value.request.texture, value.request.region, value.rgba);
  }
}
void GpuPaintSession::abandon_context() noexcept { impl_->abandoned = true; impl_->framebuffers.clear(); impl_->composite.fill(0); for (auto& layer : impl_->layers) layer.fill(0); }
void GpuPaintSession::context_restored() { if (!impl_->abandoned) throw std::logic_error("paint context is not abandoned"); impl_->abandoned = false; impl_->uploaded_geometry_revision = ~std::uint64_t{0}; impl_->allocate_all(); }
bool GpuPaintSession::context_abandoned() const noexcept { return impl_->abandoned; }
} // namespace dcc
