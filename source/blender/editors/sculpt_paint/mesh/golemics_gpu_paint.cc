/* SPDX-License-Identifier: GPL-2.0-or-later */
#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <vector>

#include "gpu_paint_blender.hh"
#include "golemics_paint_override.hh"
#include "BKE_attribute.hh"
#include "BKE_context.hh"
#include "BKE_image.hh"
#include "BKE_image_gpu.hh"
#include "BKE_lib_id.hh"
#include "BKE_material.hh"
#include "BKE_mesh.hh"
#include "BKE_node.hh"
#include "BKE_node_legacy_types.hh"
#include "BKE_node_tree_update.hh"
#include "BKE_object.hh"
#include "BKE_object_types.hh"
#include "BKE_report.hh"
#include "BLI_math_vector.hh"
#include "DEG_depsgraph.hh"
#include "DEG_depsgraph_query.hh"
#include "DNA_image_types.h"
#include "DNA_material_types.h"
#include "DNA_node_types.h"
#include "DNA_object_types.h"
#include "DNA_view3d_types.h"
#include "ED_screen.hh"
#include "IMB_imbuf_types.hh"
#include "GPU_texture.hh"
#include "RNA_access.hh"
#include "RNA_define.hh"
#include "WM_api.hh"
#include "WM_types.hh"

namespace blender::ed::sculpt_paint {
namespace {
struct PaintSession {
  Object *object = nullptr;
  Material *previous_material = nullptr;
  Material *material = nullptr;
  short slot = 1;
  int resolution = 1024;
  std::array<Image *, 7> images{};
  std::vector<dcc::PaintVertex> vertices;
  std::vector<uint32_t> indices;
  golemics::PaintGPU gpu;
  std::unique_ptr<dcc::GpuPaintSession> backend;
  bool stroke = false;
  float2 last{};
  uint64_t revision = 1;
  std::vector<std::array<dcc::PaintTextureId, 7>> undo;
  std::vector<std::array<dcc::PaintTextureId, 7>> redo;
};
/* A modal operator owns this pointer. It is cleared before destruction, including cancellation. */
PaintSession *live = nullptr;


static eevee::GolemicsPaintOverride paint_override(const Object *object)
{
  eevee::GolemicsPaintOverride result;
  if (!live || DEG_get_original(object) != live->object) return result;
  auto ids = live->backend->channel_textures();
  for (int i = 0; i < 7; i++) result.channels[i] = live->gpu.texture(ids[i]);
  result.images = live->images;
  result.base_color = result.channels[0];
  result.revision = live->revision;
  return result;
}
static void link(bNodeTree &tree, bNode &from, const char *output, bNode &to, const char *input)
{
  bke::node_add_link(tree, from, *bke::node_find_socket(from, SOCK_OUT, UString(output)),
                     to, *bke::node_find_socket(to, SOCK_IN, UString(input)));
}
static void material_create(bContext *C, PaintSession &s)
{
  Main *bmain = CTX_data_main(C);
  s.slot = std::max<short>(1, s.object->actcol);
  s.previous_material = BKE_object_material_get(s.object, s.slot);
  s.material = BKE_material_add(bmain, "Golemics PBR Paint");
  s.material->nodetree = bke::node_tree_add_tree_embedded(bmain, &s.material->id,
                                                       "Golemics PBR", "ShaderNodeTree");
  bNodeTree &tree = *s.material->nodetree;
  bNode &output = *bke::node_add_static_node(C, tree, SH_NODE_OUTPUT_MATERIAL);
  bNode &principled = *bke::node_add_static_node(C, tree, SH_NODE_BSDF_PRINCIPLED);
  link(tree, principled, "BSDF", output, "Surface");
  static const char *names[7] = {"Base Color", "Roughness", "Metallic", "Normal", "Height", "Ambient Occlusion", "Emission"};
  static const float defaults[7][4] = {{0.8f,0.8f,0.8f,1}, {0.5f,0.5f,0.5f,1}, {0,0,0,1},
                                     {0.5f,0.5f,1,1}, {0.5f,0.5f,0.5f,1}, {1,1,1,1}, {0,0,0,1}};
  std::array<bNode *, 7> textures{};
  for (int c = 0; c < 7; c++) {
    s.images[c] = BKE_image_add_generated(bmain, s.resolution, s.resolution, names[c], 32, false,
                                         IMA_GENTYPE_BLANK, defaults[c], false, true, false);
    bNode &tex = *bke::node_add_static_node(C, tree, SH_NODE_TEX_IMAGE);
    tex.id = &s.images[c]->id;
    id_us_plus(tex.id);
    textures[c] = &tex;
    std::vector<uint8_t> pixels(size_t(s.resolution) * s.resolution * 4);
    for (size_t p = 0; p < pixels.size(); p++) pixels[p] = uint8_t(defaults[c][p % 4] * 255.0f);
    s.gpu.upload(s.backend->layer_texture(0,c), {0,0,uint32_t(s.resolution),uint32_t(s.resolution)}, pixels);
  }
  link(tree, *textures[1], "Color", principled, "Roughness");
  link(tree, *textures[2], "Color", principled, "Metallic");
  link(tree, *textures[6], "Color", principled, "Emission Color");
  auto *strength = bke::node_find_socket(principled, SOCK_IN, "Emission Strength"_ustr);
  static_cast<bNodeSocketValueFloat *>(strength->default_value)->value = 1.0f;
  bNode &normal = *bke::node_add_static_node(C, tree, SH_NODE_NORMAL_MAP);
  bNode &bump = *bke::node_add_static_node(C, tree, SH_NODE_BUMP);
  link(tree, *textures[3], "Color", normal, "Color");
  link(tree, normal, "Normal", bump, "Normal");
  link(tree, *textures[4], "Color", bump, "Height");
  link(tree, bump, "Normal", principled, "Normal");
  /* AO multiplies base color using the native material node, preserving UV and shader behavior. */
  bNode &ao = *bke::node_add_static_node(C, tree, SH_NODE_MIX_RGB_LEGACY);
  ao.custom1 = MA_RAMP_MULT;
  static_cast<bNodeSocketValueFloat *>(bke::node_find_socket(ao, SOCK_IN, "Fac"_ustr)->default_value)->value = 1.0f;
  link(tree, *textures[0], "Color", ao, "Color1");
  link(tree, *textures[5], "Color", ao, "Color2");
  link(tree, ao, "Color", principled, "Base Color");
  BKE_ntree_update_tag_all(&tree);
  BKE_object_material_assign(bmain, s.object, s.material, s.slot, BKE_MAT_ASSIGN_OBDATA);
  DEG_id_tag_update(&s.object->id, ID_RECALC_SHADING);
  DEG_relations_tag_update(bmain);
  s.backend->composite();
}
static bool poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  return !live && ob && ob->type == OB_MESH && ob->mode == OB_MODE_OBJECT && CTX_wm_region_view3d(C);
}
static void finish(bContext *C, wmOperator *op, bool commit)
{
  auto *s = static_cast<PaintSession *>(op->customdata);
  if (!s) return;
  if (s->stroke) s->backend->end_stroke();
  if (commit) {
    for (int c = 0; c < 7; c++) {
      void *lock = nullptr;
      ImBuf *ibuf = BKE_image_acquire_ibuf(s->images[c], nullptr, &lock);
      if (ibuf) {
        s->gpu.readback(s->backend->channel_textures()[c],
                        {0,0,uint32_t(s->resolution),uint32_t(s->resolution)},
                        {ibuf->byte_data_for_write(), size_t(s->resolution) * s->resolution * 4});
        ibuf->userflags |= IB_BITMAPDIRTY;
      }
      BKE_image_release_ibuf(s->images[c], ibuf, lock);
      BKE_image_memorypack(s->images[c]);
      BKE_image_free_gpu_texture_caches(s->images[c]);
    }
  }
  else {
    BKE_object_material_assign(CTX_data_main(C), s->object, s->previous_material, s->slot,
                               BKE_MAT_ASSIGN_OBDATA);
    BKE_id_delete(CTX_data_main(C), &s->material->id);
    for (Image *image : s->images) BKE_id_delete(CTX_data_main(C), &image->id);
  }
  live = nullptr;
  DEG_id_tag_update(&s->object->id, ID_RECALC_SHADING);
  delete s;
  op->customdata = nullptr;
  ED_region_tag_redraw(CTX_wm_region(C));
}
static void dab(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &s = *static_cast<PaintSession *>(op->customdata);
  RegionView3D *rv3d = CTX_wm_region_view3d(C);
  ARegion *region = CTX_wm_region(C);
  dcc::PaintView view;
  std::memcpy(view.model.data(), s.object->object_to_world().ptr(), sizeof(float)*16);
  std::memcpy(view.view_projection.data(), rv3d->persmat, sizeof(float)*16);
  view.viewport_width = region->winx;
  view.viewport_height = region->winy;
  s.backend->set_view(view);
  const float2 current(float(event->mval[0]), float(event->mval[1]));
  const float radius = RNA_float_get(op->ptr, "radius");
  const int steps = std::max(1, int(math::distance(current, s.last) / std::max(1.0f, radius * 0.15f)));
  std::vector<dcc::PaintDab> dabs;
  for (int i = 1; i <= steps; i++) {
    float2 p = math::interpolate(s.last, current, float(i)/steps);
    dabs.push_back({p.x, p.y, radius, 1.0f});
  }
  (void)s.backend->submit({dabs});
  s.backend->composite();
  s.last = current;
  s.revision++;
  ED_region_tag_redraw(region);
}
static wmOperatorStatus invoke(bContext *C, wmOperator *op, const wmEvent *)
{
  auto s = std::make_unique<PaintSession>();
  s->object = CTX_data_active_object(C);
  Mesh &mesh = *id_cast<Mesh *>(s->object->data);
  const bke::AttributeAccessor attributes = mesh.attributes();
  const VArraySpan uv = *attributes.lookup<float2>(mesh.active_uv_map_name(), bke::AttrDomain::Corner);
  if (uv.is_empty()) {
    BKE_report(op->reports, RPT_ERROR, "Golemics GPU paint requires an active UV map");
    return OPERATOR_CANCELLED;
  }
  s->resolution = RNA_int_get(op->ptr, "resolution");
  for (const int3 &tri : mesh.corner_tris()) for (int k = 0; k < 3; k++) {
    const int corner = tri[k];
    const float3 p = mesh.vert_positions()[mesh.corner_verts()[corner]];
    s->indices.push_back(s->vertices.size());
    s->vertices.push_back({{p.x,p.y,p.z}, uv[corner].x, uv[corner].y});
  }
  s->backend = std::make_unique<dcc::GpuPaintSession>(s->gpu, s->gpu,
      dcc::PaintGeometry{s->vertices,s->indices,1}, s->resolution);
  dcc::PaintBrushState brush;
  float color[3];
  RNA_float_get_array(op->ptr, "color", color);
  for (int i = 0; i < 3; i++) brush.channel_values[i] = color[i];
  brush.channel_values[3] = 1.0f;
  brush.channel_values[4] = brush.channel_values[5] = brush.channel_values[6] = RNA_float_get(op->ptr,"roughness");
  brush.channel_values[7] = 1.0f;
  brush.channel_values[8] = brush.channel_values[9] = brush.channel_values[10] = RNA_float_get(op->ptr,"metallic");
  brush.channel_values[11] = 1.0f;
  brush.opacity = RNA_float_get(op->ptr,"strength");
  s->backend->set_brush(brush);
  material_create(C, *s);
  live = s.get();
  eevee::golemics_paint_set_provider(paint_override);
  op->customdata = s.release();
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}
static void history_clear(PaintSession &s, std::vector<std::array<dcc::PaintTextureId, 7>> &history)
{
  for (const auto &checkpoint : history) for (auto id : checkpoint) s.gpu.destroy_texture(id);
  history.clear();
}
static void checkpoint(PaintSession &s, std::vector<std::array<dcc::PaintTextureId, 7>> &history)
{
  std::array<dcc::PaintTextureId, 7> ids{};
  for (int c = 0; c < 7; c++) {
    ids[c] = s.gpu.create_texture({uint32_t(s.resolution), uint32_t(s.resolution)});
    GPU_texture_copy(s.gpu.texture(ids[c]), s.gpu.texture(s.backend->layer_texture(0,c)));
  }
  history.push_back(ids);
  if (history.size() > 8) {
    for (auto id : history.front()) s.gpu.destroy_texture(id);
    history.erase(history.begin());
  }
}
static void history_step(PaintSession &s, bool redo)
{
  auto &source = redo ? s.redo : s.undo;
  auto &destination = redo ? s.undo : s.redo;
  if (source.empty()) return;
  checkpoint(s, destination);
  for (int c = 0; c < 7; c++) {
    GPU_texture_copy(s.gpu.texture(s.backend->layer_texture(0,c)), s.gpu.texture(source.back()[c]));
    s.gpu.destroy_texture(source.back()[c]);
  }
  source.pop_back();
  s.backend->composite();
  s.revision++;
}
static wmOperatorStatus modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  auto &s = *static_cast<PaintSession *>(op->customdata);
  if (!s.stroke && event->type == EVT_ZKEY && event->val == KM_PRESS && (event->modifier & KM_CTRL)) {
    history_step(s, event->modifier & KM_SHIFT);
    ED_region_tag_redraw(CTX_wm_region(C));
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == EVT_ESCKEY) { finish(C,op,false); return OPERATOR_CANCELLED; }
  if (ELEM(event->type, EVT_RETKEY, EVT_PADENTER) && event->val == KM_PRESS) {
    finish(C,op,true); return OPERATOR_FINISHED;
  }
  if (event->type == LEFTMOUSE) {
    if (event->val == KM_PRESS && !s.stroke) {
      history_clear(s, s.redo);
      checkpoint(s, s.undo);
      s.backend->begin_stroke(); s.stroke = true;
      s.last = float2(event->mval[0],event->mval[1]); dab(C,op,event);
    }
    else if (event->val == KM_RELEASE && s.stroke) { s.backend->end_stroke(); s.stroke = false; }
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == MOUSEMOVE && s.stroke) { dab(C,op,event); return OPERATOR_RUNNING_MODAL; }
  if (!s.stroke && ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE, WHEELDOWNMOUSE, MOUSEPAN, MOUSEROTATE, MOUSEZOOM)) return OPERATOR_PASS_THROUGH;
  return OPERATOR_RUNNING_MODAL;
}
static void cancel(bContext *C, wmOperator *op) { finish(C,op,false); }
}  // namespace
}  // namespace blender::ed::sculpt_paint

/* Declared in namespace blender by paint_intern.hh, where paint_ops.cc registers it. */
namespace blender {
void PAINT_OT_golemics_gpu_paint(wmOperatorType *ot)
{
  namespace sp = ed::sculpt_paint;
  ot->name = "Golemics GPU PBR Paint";
  ot->idname = "PAINT_OT_golemics_gpu_paint";
  ot->description = "Paint a native GPU PBR material; Enter commits packed images, Escape cancels";
  ot->poll = sp::poll; ot->invoke = sp::invoke; ot->modal = sp::modal; ot->cancel = sp::cancel;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO | OPTYPE_BLOCKING;
  RNA_def_int(ot->srna,"resolution",1024,64,4096,"Resolution","Channel texture resolution",64,4096);
  RNA_def_float(ot->srna,"radius",40,1,1000,"Radius","Brush radius in pixels",1,300);
  RNA_def_float(ot->srna,"strength",0.5f,0,1,"Strength","Dab opacity",0,1);
  const float color[3] = {0.3f,0.1f,0.05f};
  RNA_def_float_color(ot->srna,"color",3,color,0,1,"Color","Linear base color",0,1);
  RNA_def_float(ot->srna,"roughness",0.5f,0,1,"Roughness","Painted roughness",0,1);
  RNA_def_float(ot->srna,"metallic",0,0,1,"Metallic","Painted metallic",0,1);
}
}  // namespace blender
