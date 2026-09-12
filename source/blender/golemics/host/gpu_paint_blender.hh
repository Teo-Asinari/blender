/* SPDX-License-Identifier: GPL-2.0-or-later */
#pragma once

#include "dcc/gpu_paint_session.hpp"

namespace blender::gpu { class Texture; }

namespace blender::golemics {

/** Blender GPU implementation. Construct, use and destroy with the host GPU context current.
 * Resource IDs are private to this provider, never OpenGL names. */
class PaintGPU final : public dcc::PaintGpuResourceProvider,
                       public dcc::PaintGpuCommandContext {
  struct Impl;
  Impl *impl_;

 public:
  PaintGPU();
  ~PaintGPU() override;
  PaintGPU(const PaintGPU &) = delete;
  PaintGPU &operator=(const PaintGPU &) = delete;
  gpu::Texture *texture(dcc::PaintTextureId) const;
  dcc::PaintTextureId create_texture(dcc::PaintTextureDesc) override;
  void destroy_texture(dcc::PaintTextureId) noexcept override;
  dcc::PaintFramebufferId create_framebuffer(std::span<const dcc::PaintTextureId>,
                                            dcc::PaintTextureId) override;
  void destroy_framebuffer(dcc::PaintFramebufferId) noexcept override;
  void readback(dcc::PaintTextureId, dcc::ImageRegion, std::span<uint8_t>) override;
  void upload(dcc::PaintTextureId, dcc::ImageRegion, std::span<const uint8_t>) override;
  void bind_framebuffer(dcc::PaintFramebufferId) override;
  void bind_texture(dcc::PaintTextureId, uint32_t) override;
  void upload_geometry(dcc::PaintGeometry) override;
  void draw_dabs(std::span<const dcc::PaintTextureId>, dcc::PaintFramebufferId,
                 dcc::PaintView, dcc::PaintDabBatch, dcc::PaintBrushState,
                 dcc::ImageRegion) override;
  void draw_composite(std::span<const dcc::PaintTextureId>,
                      std::span<const dcc::PaintTextureId>,
                      std::span<const dcc::PaintLayerState>, dcc::ImageRegion) override;
};
}  // namespace blender::golemics
