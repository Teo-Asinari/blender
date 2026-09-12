#pragma once

/**
 * Optional live paint texture supplied by an integrated paint backend.
 *
 * This is intentionally only an ownership-neutral hook. The provider owns
 * the texture and must keep it alive until the next draw synchronization.
 * The EEVEE integration can bind it alongside the regular material resources
 * without copying through an Image datablock.
 */

#include "GPU_texture.hh"

#include "GPU_framebuffer.hh"

#include <array>
#include <cstdint>

namespace blender {
struct Object;
struct Image;
namespace eevee {

struct GolemicsPaintOverride {
  enum Channel : int {
    BaseColor = 0,
    Roughness,
    Metallic,
    Normal,
    Height,
    AmbientOcclusion,
    Emission,
    Count,
  };
  static constexpr int kChannelCount = Count;
  /* Base color, roughness, metallic, normal, height, ambient occlusion, emission. */
  std::array<gpu::Texture *, kChannelCount> channels{};
  std::array<blender::Image *, kChannelCount> images{};
  gpu::Texture *base_color = nullptr;
  uint64_t revision = 0;

  bool enabled() const
  {
    return base_color != nullptr || channels[0] != nullptr;
  }
};

using GolemicsPaintProviderFn = GolemicsPaintOverride (*)(const blender::Object *object);

void golemics_paint_set_provider(GolemicsPaintProviderFn provider);
GolemicsPaintOverride golemics_paint_override_for_object(const blender::Object *object);

static_assert(GolemicsPaintOverride::kChannelCount == 7);

/*
 * Binding note: EEVEE material textures are generated from the node tree and
 * bound by PassBase::material_set() in draw/intern/draw_pass.hh. A live paint
 * sampler must therefore be added to a shader permutation (and sampled by
 * generated material code) before it can be bound there. This optional field
 * intentionally does not alter existing shader layouts until that permutation
 * is implemented.
 */

/** GPU objects for one live paint target; no Image datablock is involved. */
class GolemicsPaintGpuResources {
  static constexpr int kChannelCount = GolemicsPaintOverride::kChannelCount;
  std::array<gpu::Texture *, kChannelCount> channels_{};
  gpu::FrameBuffer *framebuffer_ = nullptr;

 public:
  GolemicsPaintGpuResources() = default;
  GolemicsPaintGpuResources(const GolemicsPaintGpuResources &) = delete;
  GolemicsPaintGpuResources &operator=(const GolemicsPaintGpuResources &) = delete;
  ~GolemicsPaintGpuResources();

  void allocate(int width, int height);
  void release();

  gpu::Texture *base_color() const { return channels_[GolemicsPaintOverride::BaseColor]; }
  gpu::Texture *channel(const int index) const
  {
    return (index >= 0 && index < kChannelCount) ? channels_[index] : nullptr;
  }
  GolemicsPaintOverride make_override(const uint64_t revision) const
  {
    GolemicsPaintOverride result;
    result.channels = channels_;
    result.base_color = channels_[0];
    result.revision = revision;
    return result;
  }
  gpu::FrameBuffer *framebuffer() const { return framebuffer_; }
};

}  // namespace eevee
}  // namespace blender
