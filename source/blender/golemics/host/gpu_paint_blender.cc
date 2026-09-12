/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "gpu_paint_blender.hh"

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <unordered_map>
#include <vector>

#include "GPU_batch.hh"
#include "GPU_framebuffer.hh"
#include "GPU_shader.hh"
#include "GPU_state.hh"
#include "GPU_texture.hh"
#include "GPU_vertex_buffer.hh"
#include "GPU_vertex_format.hh"
#include "gpu_shader_create_info.hh"

namespace blender::golemics {
namespace {
using namespace gpu::shader;
struct State {
  gpu::FrameBuffer *fb = GPU_framebuffer_active_get();
  GPUBlend blend = GPU_blend_get();
  GPUDepthTest depth = GPU_depth_test_get();
  GPUFaceCullTest cull = GPU_face_culling_get();
  bool mask = GPU_depth_mask_get();
  int viewport[4];
  State() { GPU_viewport_size_get_i(viewport); }
  ~State()
  {
    GPU_framebuffer_bind(fb);
    GPU_viewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    GPU_blend(blend);
    GPU_depth_test(depth);
    GPU_depth_mask(mask);
    GPU_face_culling(cull);
  }
};

gpu::Shader *compile(ShaderCreateInfo &info)
{
  auto *shader = GPU_shader_create_from_info_python(
      reinterpret_cast<GPUShaderCreateInfo *>(&info));
  if (!shader) {
    throw std::runtime_error("Golemics paint GPU shader compilation failed");
  }
  return shader;
}
}  // namespace

struct PaintGPU::Impl {
  uint64_t next_id = 1;
  std::unordered_map<uint64_t, gpu::Texture *> textures;
  std::unordered_map<uint64_t, gpu::FrameBuffer *> framebuffers;
  gpu::Batch *geometry = nullptr;
  gpu::Batch *fullscreen = nullptr;
  gpu::Shader *dab = nullptr;
  gpu::Shader *depth = nullptr;
  gpu::Shader *composite = nullptr;
  gpu::Texture *visibility = nullptr;
  gpu::FrameBuffer *visibility_fb = nullptr;

  void shaders()
  {
    if (dab) {
      return;
    }
    StageInterfaceInfo iface("golemics_paint_iface", "paint");
    iface.smooth(Type::float4_t, "projected");
    ShaderCreateInfo info("golemics_paint_dab");
    info.vertex_in(0, Type::float3_t, "position")
        .vertex_in(1, Type::float2_t, "uv")
        .vertex_out(iface)
        .push_constant(Type::float4x4_t, "model")
        .push_constant(Type::float4x4_t, "view_projection")
        .push_constant(Type::float4_t, "dab_data")
        .push_constant(Type::float2_t, "viewport_size")
        .push_constant(Type::float2_t, "brush_shape")
        .push_constant(Type::float4_t, "channel_values", 7)
        .sampler(0, ImageType::Float2D, "scene_depth");
    static const char *outputs[] = {"color0", "color1", "color2", "color3", "color4", "color5", "color6"};
    for (int i = 0; i < 7; i++) {
      info.fragment_out(i, Type::float4_t, outputs[i]);
    }
    info.vertex_source_generated = R"(
void main() {
  paint.projected = view_projection * model * vec4(position, 1.0);
  gl_Position = vec4(uv * 2.0 - 1.0, 0.0, 1.0);
})";
    info.fragment_source_generated = R"(
void main() {
  if (paint.projected.w <= 0.0) discard;
  vec3 projected = paint.projected.xyz / paint.projected.w;
  vec2 screen_uv = projected.xy * 0.5 + 0.5;
  if (any(lessThan(screen_uv, vec2(0.0))) || any(greaterThan(screen_uv, vec2(1.0)))) discard;
  float z = projected.z * 0.5 + 0.5;
  float visible_z = texture(scene_depth, screen_uv).r;
  float tolerance = max(0.00001, 2.0 * (abs(dFdx(z)) + abs(dFdy(z))));
  if (z > visible_z + tolerance || z < 0.0 || z > 1.0) discard;
  float distance_to_dab = length(screen_uv * viewport_size - dab_data.xy) / max(dab_data.z, 0.001);
  if (distance_to_dab >= 1.0) discard;
  float coverage = (1.0 - smoothstep(min(brush_shape.x, 0.999), 1.0, distance_to_dab)) * brush_shape.y * dab_data.w;
  color0 = vec4(channel_values[0].rgb, channel_values[0].a * coverage);
  color1 = vec4(channel_values[1].rgb, channel_values[1].a * coverage);
  color2 = vec4(channel_values[2].rgb, channel_values[2].a * coverage);
  color3 = vec4(channel_values[3].rgb, channel_values[3].a * coverage);
  color4 = vec4(channel_values[4].rgb, channel_values[4].a * coverage);
  color5 = vec4(channel_values[5].rgb, channel_values[5].a * coverage);
  color6 = vec4(channel_values[6].rgb, channel_values[6].a * coverage);
})";
    dab = compile(info);
    ShaderCreateInfo depth_info("golemics_paint_depth");
    depth_info.vertex_in(0, Type::float3_t, "position")
        .push_constant(Type::float4x4_t, "model")
        .push_constant(Type::float4x4_t, "view_projection");
    depth_info.vertex_source_generated = R"(
void main() { gl_Position = view_projection * model * vec4(position, 1.0); })";
    depth_info.fragment_source_generated = "void main() {}";
    depth = compile(depth_info);
    ShaderCreateInfo comp_info("golemics_paint_composite");
    comp_info.sampler(0, ImageType::Float2D, "layer_texture")
        .push_constant(Type::float_t, "opacity")
        .fragment_out(0, Type::float4_t, "color");
    comp_info.vertex_source_generated = R"(
void main() {
 vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
 gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    comp_info.fragment_source_generated = R"(
void main() {
 vec4 value = texelFetch(layer_texture, ivec2(gl_FragCoord.xy), 0);
 color = value * opacity;
})";
    composite = compile(comp_info);
    fullscreen = GPU_batch_create_procedural(GPU_PRIM_TRIS, 3);
  }
};

PaintGPU::PaintGPU() : impl_(new Impl) {}
PaintGPU::~PaintGPU()
{
  if (impl_->geometry) GPU_batch_discard(impl_->geometry);
  if (impl_->fullscreen) GPU_batch_discard(impl_->fullscreen);
  if (impl_->dab) GPU_shader_free(impl_->dab);
  if (impl_->depth) GPU_shader_free(impl_->depth);
  if (impl_->composite) GPU_shader_free(impl_->composite);
  if (impl_->visibility_fb) GPU_framebuffer_free(impl_->visibility_fb);
  if (impl_->visibility) GPU_texture_free(impl_->visibility);
  for (const auto &[id, fb] : impl_->framebuffers) GPU_framebuffer_free(fb);
  for (const auto &[id, tex] : impl_->textures) GPU_texture_free(tex);
  delete impl_;
}

gpu::Texture *PaintGPU::texture(uint64_t id) const { return impl_->textures.at(id); }
uint64_t PaintGPU::create_texture(dcc::PaintTextureDesc desc)
{
  auto *tex = GPU_texture_create_2d("Golemics paint", desc.width, desc.height, 1,
      gpu::TextureFormat::UNORM_8_8_8_8,
      GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT | GPU_TEXTURE_USAGE_HOST_READ, nullptr);
  if (!tex) throw std::runtime_error("Cannot allocate Golemics paint texture");
  const float transparent[4] = {};
  GPU_texture_clear(tex, GPU_DATA_FLOAT, transparent);
  GPU_texture_filter_mode(tex, true);
  const uint64_t id = impl_->next_id++;
  impl_->textures.emplace(id, tex);
  return id;
}
void PaintGPU::destroy_texture(uint64_t id) noexcept
{
  auto it = impl_->textures.find(id);
  if (it != impl_->textures.end()) { GPU_texture_free(it->second); impl_->textures.erase(it); }
}
uint64_t PaintGPU::create_framebuffer(std::span<const uint64_t> colors, uint64_t depth)
{
  auto *fb = GPU_framebuffer_create("Golemics paint");
  for (size_t i = 0; i < colors.size(); i++) GPU_framebuffer_texture_attach(fb, texture(colors[i]), i, 0);
  if (depth) GPU_framebuffer_texture_attach(fb, texture(depth), 0, 0);
  char error[256];
  if (!GPU_framebuffer_check_valid(fb, error)) {
    GPU_framebuffer_free(fb);
    throw std::runtime_error(error);
  }
  const uint64_t id = impl_->next_id++;
  impl_->framebuffers.emplace(id, fb);
  return id;
}
void PaintGPU::destroy_framebuffer(uint64_t id) noexcept
{
  auto it = impl_->framebuffers.find(id);
  if (it != impl_->framebuffers.end()) { GPU_framebuffer_free(it->second); impl_->framebuffers.erase(it); }
}
void PaintGPU::readback(uint64_t id, dcc::ImageRegion region, std::span<uint8_t> out)
{
  auto *tex = texture(id);
  const uint32_t width = GPU_texture_width(tex), height = GPU_texture_height(tex);
  if (region.x + uint64_t(region.width) > width || region.y + uint64_t(region.height) > height ||
      out.size() != size_t(region.width) * region.height * 4) throw std::out_of_range("paint readback region");
  std::vector<uint8_t> pixels(size_t(width) * height * 4);
  GPU_texture_read(tex, GPU_DATA_UBYTE, 0, pixels.data());
  for (uint32_t y = 0; y < region.height; y++) {
    std::memcpy(out.data() + size_t(y) * region.width * 4,
                pixels.data() + (size_t(region.y + y) * width + region.x) * 4,
                size_t(region.width) * 4);
  }
}
void PaintGPU::upload(uint64_t id, dcc::ImageRegion r, std::span<const uint8_t> pixels)
{
  auto *tex = texture(id);
  if (r.x + uint64_t(r.width) > uint64_t(GPU_texture_width(tex)) ||
      r.y + uint64_t(r.height) > uint64_t(GPU_texture_height(tex)) ||
      pixels.size() != size_t(r.width) * r.height * 4) throw std::out_of_range("paint upload region");
  GPU_texture_update_sub(tex, GPU_DATA_UBYTE, pixels.data(), r.x, r.y, 0, r.width, r.height, 1);
}
void PaintGPU::bind_framebuffer(uint64_t id)
{
  /* Binding happens inside draw_dabs so its state guard can restore the caller's framebuffer. */
  (void)impl_->framebuffers.at(id);
}
void PaintGPU::bind_texture(uint64_t id, uint32_t unit) { GPU_texture_bind(texture(id), unit); }
void PaintGPU::upload_geometry(dcc::PaintGeometry geometry)
{
  std::vector<dcc::PaintVertex> expanded;
  expanded.reserve(geometry.indices.size());
  for (uint32_t index : geometry.indices) {
    if (index >= geometry.vertices.size()) throw std::out_of_range("paint vertex index");
    expanded.push_back(geometry.vertices[index]);
  }
  GPUVertFormat format{};
  const uint pos = GPU_vertformat_attr_add(&format, "position", gpu::VertAttrType::SFLOAT_32_32_32);
  const uint uv = GPU_vertformat_attr_add(&format, "uv", gpu::VertAttrType::SFLOAT_32_32);
  auto *vbo = GPU_vertbuf_create_with_format(format);
  GPU_vertbuf_data_alloc(*vbo, expanded.size());
  if (!expanded.empty()) {
    GPU_vertbuf_attr_fill_stride(vbo, pos, sizeof(dcc::PaintVertex), &expanded[0].position);
    GPU_vertbuf_attr_fill_stride(vbo, uv, sizeof(dcc::PaintVertex), &expanded[0].u);
  }
  if (impl_->geometry) GPU_batch_discard(impl_->geometry);
  impl_->geometry = GPU_batch_create_ex(GPU_PRIM_TRIS, vbo, nullptr, GPU_BATCH_OWNS_VBO);
}
void PaintGPU::draw_dabs(std::span<const uint64_t> targets, uint64_t framebuffer,
                        dcc::PaintView view, dcc::PaintDabBatch batch,
                        dcc::PaintBrushState brush, dcc::ImageRegion)
{
  if (targets.empty() || batch.dabs.empty() || !impl_->geometry) return;
  State state;
  impl_->shaders();
  if (!impl_->visibility || GPU_texture_width(impl_->visibility) != int(view.viewport_width) ||
      GPU_texture_height(impl_->visibility) != int(view.viewport_height)) {
    if (impl_->visibility_fb) GPU_framebuffer_free(impl_->visibility_fb);
    if (impl_->visibility) GPU_texture_free(impl_->visibility);
    impl_->visibility = GPU_texture_create_2d("Golemics visibility", view.viewport_width,
        view.viewport_height, 1, gpu::TextureFormat::SFLOAT_32_DEPTH,
        GPU_TEXTURE_USAGE_SHADER_READ | GPU_TEXTURE_USAGE_ATTACHMENT, nullptr);
    impl_->visibility_fb = GPU_framebuffer_create("Golemics visibility");
    GPU_framebuffer_texture_attach(impl_->visibility_fb, impl_->visibility, 0, 0);
  }
  GPU_face_culling(GPU_CULL_NONE);
  GPU_blend(GPU_BLEND_NONE);
  GPU_depth_test(GPU_DEPTH_LESS_EQUAL);
  GPU_depth_mask(true);
  GPU_framebuffer_bind(impl_->visibility_fb);
  GPU_framebuffer_clear_depth(impl_->visibility_fb, 1.0f);
  GPU_batch_set_shader(impl_->geometry, impl_->depth);
  GPU_shader_uniform_mat4(impl_->depth, "model", reinterpret_cast<const float (*)[4]>(view.model.data()));
  GPU_shader_uniform_mat4(impl_->depth, "view_projection", reinterpret_cast<const float (*)[4]>(view.view_projection.data()));
  GPU_batch_draw(impl_->geometry);
  GPU_framebuffer_bind(impl_->framebuffers.at(framebuffer));
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_blend(GPU_BLEND_ALPHA);
  GPU_batch_set_shader(impl_->geometry, impl_->dab);
  GPU_shader_uniform_mat4(impl_->dab, "model", reinterpret_cast<const float (*)[4]>(view.model.data()));
  GPU_shader_uniform_mat4(impl_->dab, "view_projection", reinterpret_cast<const float (*)[4]>(view.view_projection.data()));
  GPU_shader_uniform_2f(impl_->dab, "viewport_size", view.viewport_width, view.viewport_height);
  GPU_shader_uniform_2f(impl_->dab, "brush_shape", brush.hardness, brush.opacity);
  GPU_shader_uniform_4fv_array(impl_->dab, "channel_values", 7, reinterpret_cast<const float (*)[4]>(brush.channel_values.data()));
  GPU_texture_bind(impl_->visibility, 0);
  for (const dcc::PaintDab &dab : batch.dabs) {
    GPU_shader_uniform_4f(impl_->dab, "dab_data", dab.screen_x, dab.screen_y, dab.radius, dab.pressure);
    GPU_batch_draw(impl_->geometry);
  }
  GPU_texture_unbind(impl_->visibility);
}
void PaintGPU::draw_composite(std::span<const uint64_t> outputs,
                             std::span<const uint64_t> channels,
                             std::span<const dcc::PaintLayerState> layers, dcc::ImageRegion)
{
  if (channels.size() != outputs.size() * layers.size()) throw std::invalid_argument("paint layers");
  State state;
  impl_->shaders();
  GPU_depth_test(GPU_DEPTH_NONE);
  GPU_depth_mask(false);
  GPU_face_culling(GPU_CULL_NONE);
  GPU_blend(GPU_BLEND_ALPHA_PREMULT);
  auto *fb = GPU_framebuffer_create("Golemics composite");
  const float clear[4] = {};
  for (size_t c = 0; c < outputs.size(); c++) {
    GPU_texture_clear(texture(outputs[c]), GPU_DATA_FLOAT, clear);
    GPU_framebuffer_texture_attach(fb, texture(outputs[c]), 0, 0);
    GPU_framebuffer_bind(fb);
    GPU_batch_set_shader(impl_->fullscreen, impl_->composite);
    for (size_t layer = 0; layer < layers.size(); layer++) {
      if (!layers[layer].visible) continue;
      auto *tex = texture(channels[layer * outputs.size() + c]);
      GPU_texture_bind(tex, 0);
      GPU_shader_uniform_1f(impl_->composite, "opacity", layers[layer].opacity);
      GPU_batch_draw(impl_->fullscreen);
      GPU_texture_unbind(tex);
    }
    GPU_framebuffer_texture_detach(fb, texture(outputs[c]));
  }
  GPU_framebuffer_free(fb);
}
}  // namespace blender::golemics
