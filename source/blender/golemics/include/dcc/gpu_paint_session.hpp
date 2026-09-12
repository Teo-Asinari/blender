#pragma once

#include "dcc/core.hpp"

#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace dcc {

inline constexpr std::size_t kGpuPaintChannelCount = 7;
using PaintTextureId = std::uint64_t;
using PaintFramebufferId = std::uint64_t;

enum class PaintTextureFormat : std::uint8_t { rgba8 };
enum class PaintTextureUsage : std::uint8_t { sampled_render_target, depth_sample };

struct PaintTextureDesc {
  std::uint32_t width{}, height{};
  PaintTextureFormat format{PaintTextureFormat::rgba8};
  PaintTextureUsage usage{PaintTextureUsage::sampled_render_target};
};

struct PaintVertex {
  Vec3 position{};
  float u{}, v{};
};

struct PaintGeometry {
  std::span<const PaintVertex> vertices;
  std::span<const std::uint32_t> indices;
  std::uint64_t revision{};
};

// Matrices are column-major, matching the OpenGL and Blender conventions. The
// session compares these values only to invalidate cached depth visibility.
struct PaintView {
  std::array<float, 16> model{};
  std::array<float, 16> view_projection{};
  PaintTextureId scene_depth{};
  std::uint32_t viewport_width{}, viewport_height{};
};

struct PaintDab {
  float screen_x{}, screen_y{}, radius{}, pressure{1.F};
};
struct PaintDabBatch {
  std::span<const PaintDab> dabs;
};

struct PaintBrushState {
  std::array<float, kGpuPaintChannelCount * 4> channel_values{};
  float hardness{0.5F};
  float opacity{1.F};
};

struct PaintTargetSet {
  std::array<PaintTextureId, kGpuPaintChannelCount> ids{};
  std::size_t count{};

  [[nodiscard]] std::span<const PaintTextureId> span() const noexcept
  {
    return std::span<const PaintTextureId>{ids}.first(count);
  }
};

struct PaintFlushResult {
  ImageRegion dirty{};
  std::size_t dabs{};
  bool submitted{};
  bool performed_readback{};
};

struct PaintLayerState {
  bool visible{true};
  float opacity{1.F};
};

// Commands are deliberately abstract instead of exposing OpenGL types. A
// Blender adapter can resolve opaque IDs through its GPU API, while the native
// renderer can resolve them to GL names. None of these commands may perform a
// CPU readback.
class PaintGpuCommandContext {
public:
  virtual ~PaintGpuCommandContext() = default;
  virtual void bind_framebuffer(PaintFramebufferId) = 0;
  virtual void bind_texture(PaintTextureId, std::uint32_t unit) = 0;
  virtual void upload_geometry(PaintGeometry) = 0;
  virtual void draw_dabs(std::span<const PaintTextureId> target_channels,
                         PaintFramebufferId target_framebuffer, PaintView,
                         PaintDabBatch, PaintBrushState, ImageRegion dirty_hint) = 0;
  virtual void draw_composite(std::span<const PaintTextureId> output_channels,
                              std::span<const PaintTextureId> layer_channels,
                              std::span<const PaintLayerState> layers,
                              ImageRegion region) = 0;
};

class PaintGpuRenderer {
public:
  virtual ~PaintGpuRenderer() = default;
  [[nodiscard]] virtual PaintFlushResult submit_dabs(
      PaintTextureId target, PaintGeometry, PaintView, PaintDabBatch,
      PaintBrushState, ImageRegion dirty_hint) = 0;
  // Submit one dab batch to every active PBR channel. The compatibility
  // default preserves older renderers while allowing a Blender adapter to
  // issue a single multi-render-target draw instead of flushing per channel.
  [[nodiscard]] virtual PaintFlushResult submit_dabs_multi(
      std::span<const PaintTextureId> targets, PaintGeometry geometry, PaintView view,
      PaintDabBatch batch, PaintBrushState brush, ImageRegion dirty_hint)
  {
    if (targets.empty()) {
      return {dirty_hint, batch.dabs.size(), false, false};
    }
    PaintFlushResult result{dirty_hint, batch.dabs.size(), false, false};
    /* Compatibility path: older renderers paint each target separately. New
     * Blender adapters should override this with one MRT submission. */
    for (const PaintTextureId target : targets) {
      const PaintFlushResult channel = submit_dabs(target, geometry, view, batch, brush, dirty_hint);
      result.dirty = channel.dirty;
      result.submitted = result.submitted || channel.submitted;
      result.performed_readback = result.performed_readback || channel.performed_readback;
    }
    return result;
  }
  virtual void composite(std::span<const PaintTextureId> output_channels,
                          std::span<const PaintTextureId> layer_channels,
                          std::span<const PaintLayerState> layers,
                          ImageRegion region) = 0;
};

struct PaintReadbackRequest {
  PaintTextureId texture{};
  ImageRegion region{};
};
struct PaintReadback {
  PaintReadbackRequest request;
  std::vector<std::uint8_t> rgba;
};

// The host owns the graphics context. Implementations must be called on the
// graphics thread with that context current and must never import a resource
// from another context. No implicit synchronization is permitted.
class PaintGpuResourceProvider {
public:
  virtual ~PaintGpuResourceProvider() = default;
  [[nodiscard]] virtual PaintTextureId create_texture(PaintTextureDesc) = 0;
  virtual void destroy_texture(PaintTextureId) noexcept = 0;
  [[nodiscard]] virtual PaintFramebufferId create_framebuffer(
      std::span<const PaintTextureId> color, PaintTextureId depth_stencil) = 0;
  virtual void destroy_framebuffer(PaintFramebufferId) noexcept = 0;
  virtual void readback(PaintTextureId, ImageRegion, std::span<std::uint8_t>) = 0;
  virtual void upload(PaintTextureId, ImageRegion, std::span<const std::uint8_t>) = 0;
};

// Host-neutral ownership and synchronization skeleton. The standalone
// viewport remains on its existing path until a renderer implements the GPU
// submit/composite operations behind this boundary.
class GpuPaintSession {
public:
  GpuPaintSession(PaintGpuResourceProvider&, PaintGeometry, std::uint32_t resolution);
  GpuPaintSession(PaintGpuResourceProvider&, PaintGpuRenderer&, PaintGeometry,
                  std::uint32_t resolution);
  GpuPaintSession(PaintGpuResourceProvider&, PaintGpuCommandContext&, PaintGeometry,
                  std::uint32_t resolution);
  ~GpuPaintSession();
  GpuPaintSession(const GpuPaintSession&) = delete;
  GpuPaintSession& operator=(const GpuPaintSession&) = delete;

  void set_geometry(PaintGeometry);
  void set_view(PaintView);
  [[nodiscard]] std::uint64_t geometry_revision() const noexcept;
  [[nodiscard]] bool depth_cache_valid() const noexcept;

  void begin_stroke();
  [[nodiscard]] PaintFlushResult submit(PaintDabBatch);
  void set_brush(PaintBrushState);
  [[nodiscard]] PaintTargetSet active_channel_targets() const noexcept;
  void end_stroke();

  [[nodiscard]] std::array<PaintTextureId, kGpuPaintChannelCount> channel_textures() const noexcept;
  [[nodiscard]] PaintTextureId layer_texture(std::size_t layer,
                                              std::size_t channel) const;
  std::size_t add_layer(PaintLayerState = {});
  void set_layer(std::size_t, PaintLayerState);
  void composite(ImageRegion = {});

  // These operations are the only CPU/GPU synchronization boundary exposed by
  // the session. A normal submit never calls either method.
  [[nodiscard]] std::vector<PaintReadback> readback(
      std::span<const PaintReadbackRequest>);
  void restore(std::span<const PaintReadback>);

  // Context loss makes all names invalid without calling the provider. The
  // logical session survives and context_restored recreates its resources.
  void abandon_context() noexcept;
  void context_restored();
  [[nodiscard]] bool context_abandoned() const noexcept;

private:
  struct Impl;
  Impl* impl_{};
};

} // namespace dcc
