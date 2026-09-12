/* SPDX-FileCopyrightText: 2026 Blender Authors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later */

#include "golemics_paint_override.hh"

#include "BLI_assert.hh"

#include "GPU_texture.hh"

namespace blender::eevee {

static GolemicsPaintProviderFn g_paint_provider = nullptr;

void golemics_paint_set_provider(GolemicsPaintProviderFn provider)
{
  g_paint_provider = provider;
}

GolemicsPaintOverride golemics_paint_override_for_object(const blender::Object *object)
{
  return g_paint_provider != nullptr ? g_paint_provider(object) : GolemicsPaintOverride{};
}

GolemicsPaintGpuResources::~GolemicsPaintGpuResources()
{
  release();
}

void GolemicsPaintGpuResources::allocate(const int width, const int height)
{
  BLI_assert(width > 0 && height > 0);
  release();
  for (int channel = 0; channel < kChannelCount; channel++) {
    channels_[channel] = GPU_texture_create_2d("Golemics Paint Channel",
                                                width,
                                                height,
                                                1,
                                                gpu::TextureFormat::UNORM_8_8_8_8,
                                                GPU_TEXTURE_USAGE_SHADER_READ |
                                                    GPU_TEXTURE_USAGE_ATTACHMENT,
                                                nullptr);
    if (channels_[channel] == nullptr) {
      release();
      return;
    }
  }
  framebuffer_ = GPU_framebuffer_create("Golemics Paint Base Color");
  for (int channel = 0; channel < kChannelCount; channel++) {
    GPU_framebuffer_texture_attach(framebuffer_, channels_[channel], channel, 0);
  }
}

void GolemicsPaintGpuResources::release()
{
  if (framebuffer_ != nullptr) {
    GPU_framebuffer_free(framebuffer_);
    framebuffer_ = nullptr;
  }
  for (gpu::Texture *&channel : channels_) {
    if (channel != nullptr) {
      GPU_texture_free(channel);
      channel = nullptr;
    }
  }
}

}  // namespace blender::eevee
