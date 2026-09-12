/* SPDX-License-Identifier: GPL-2.0-or-later */

/** \file
 * \ingroup edsculpt
 *
 * Native modal editing and mesh ownership for the embedded Golemics backend.
 */

#include <cfloat>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "dcc/voxel_sculpt_c_api.h"

#include "BLI_math_matrix.hh"
#include "BLI_string.hh"
#include "BLI_math_vector.hh"
#include "BKE_context.hh"
#include "BKE_idtype.hh"
#include "BKE_lib_id.hh"
#include "BKE_library.hh"
#include "BKE_mesh.hh"
#include "BKE_mesh_runtime.hh"
#include "BKE_modifier.hh"
#include "BKE_paint.hh"
#include "BKE_object.hh"
#include "BKE_report.hh"

#include "DEG_depsgraph.hh"

#include "ED_screen.hh"
#include "ED_sculpt.hh"
#include "ED_view3d.hh"

#include "RNA_access.hh"
#include "RNA_define.hh"

#include "WM_api.hh"
#include "WM_types.hh"

#include "DNA_object_types.h"
#include "BKE_object_types.hh"

#include "../paint_intern.hh"

namespace blender::ed::sculpt_paint {

/* This map is deliberately process-local: a session is an editing resource,
 * not scene data.  The source mesh is owned by the entry until the session is
 * committed or cancelled. */
struct VoxelSession {
  Mesh *source = nullptr;
  bContext *C = nullptr;
  Object *object = nullptr;
  bool stroke_active = false;
  dcc_voxel_bridge *backend = nullptr;
  bool backend_stroke = false;
};

static std::unordered_map<Object *, VoxelSession> sessions;

static void backend_replace_preview(void *userdata, const dcc_voxel_mesh *view)
{
  auto *session = static_cast<VoxelSession *>(userdata);
  if (!session || !session->source || !view || view->index_count % 3 != 0) return;
  Mesh *mesh = id_cast<Mesh *>(session->object->data);
  Mesh *preview = BKE_mesh_new_nomain(int(view->vertex_count), 0,
                                      int(view->index_count / 3), int(view->index_count));
  MutableSpan<float3> positions = preview->vert_positions_for_write();
  for (int i = 0; i < positions.size(); ++i) {
    positions[i] = {view->positions[i * 3], view->positions[i * 3 + 1], view->positions[i * 3 + 2]};
  }
  MutableSpan<int> offsets = preview->face_offsets_for_write();
  for (int i = 0; i <= preview->faces_num; ++i) offsets[i] = i * 3;
  MutableSpan<int> corners = preview->corner_verts_for_write();
  for (int i = 0; i < corners.size(); ++i) corners[i] = int(view->indices[i]);
  bke::mesh_calc_edges(*preview, false, false);
  BKE_mesh_nomain_to_mesh(preview, mesh, session->object, false);
  if (session->object->mode == OB_MODE_SCULPT) {
    BKE_sculptsession_free_pbvh(*session->object);
  }
  BKE_mesh_batch_cache_dirty_tag(mesh, BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&session->object->id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(session->C, NC_GEOM | ND_DATA, session->object->data);
}


static void backend_redraw(void *userdata)
{
  auto *session = static_cast<VoxelSession *>(userdata);
  if (session) ED_region_tag_redraw(CTX_wm_region(session->C));
}

static bool voxel_sculpt_poll(bContext *C)
{
  Object *ob = CTX_data_active_object(C);
  return ob && ob->type == OB_MESH && ob->data && ID_IS_EDITABLE(ob) &&
         ID_IS_EDITABLE(ob->data) && !ID_IS_OVERRIDE_LIBRARY(ob->data) &&
         !BKE_object_is_in_editmode(ob) && !BKE_modifiers_uses_multires(ob) &&
         !(ob->mode == OB_MODE_SCULPT && ob->runtime->sculpt_session->bm) &&
         !id_cast<Mesh *>(ob->data)->key;
}

static void tag_voxel_mesh(bContext *C, Object &ob)
{
  if (ob.mode == OB_MODE_SCULPT) BKE_sculptsession_free_pbvh(ob);
  BKE_mesh_batch_cache_dirty_tag(id_cast<Mesh *>(ob.data), BKE_MESH_BATCH_DIRTY_ALL);
  DEG_id_tag_update(&ob.id, ID_RECALC_GEOMETRY);
  WM_event_add_notifier(C, NC_GEOM | ND_DATA, ob.data);
}

static bool voxel_sculpt_begin(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  if (!ob || ob->type != OB_MESH) {
    BKE_report(op->reports, RPT_ERROR, "A mesh object is required");
    return false;
  }
  if (sessions.contains(ob)) {
    BKE_report(op->reports, RPT_WARNING, "Golemics voxel sculpt is already active");
    return false;
  }

  Mesh *mesh = id_cast<Mesh *>(ob->data);
  const float voxel_size = RNA_float_get(op->ptr, "voxel_size");
  Mesh *source = BKE_mesh_copy_for_eval(*mesh);

  /* Keep the original mesh alive while the voxel session owns the data ID. */
  sessions.emplace(ob, VoxelSession{source, C, ob});
  VoxelSession &session = sessions.at(ob);
  const Span<float3> positions = mesh->vert_positions();
  const Span<int3> triangles = mesh->corner_tris();
  const Span<int> corner_verts = mesh->corner_verts();
  std::vector<float> source_positions;
  source_positions.reserve(positions.size() * 3);
  for (const float3 &p : positions) source_positions.insert(source_positions.end(), {p.x, p.y, p.z});
  std::vector<uint32_t> source_indices;
  source_indices.reserve(triangles.size() * 3);
  for (const int3 &triangle : triangles) {
    source_indices.insert(source_indices.end(), {uint32_t(corner_verts[triangle.x]), uint32_t(corner_verts[triangle.y]),
                                                    uint32_t(corner_verts[triangle.z])});
  }
  const dcc_voxel_source_mesh source_view{source_positions.data(), size_t(positions.size()),
                                          source_indices.data(), source_indices.size()};
  const dcc_voxel_sculpt_callbacks callbacks{&session, backend_replace_preview, nullptr, nullptr,
                                              backend_redraw};
  session.backend = dcc_voxel_bridge_create(source_view, voxel_size, callbacks);
  if (!session.backend || !dcc_voxel_bridge_enter(session.backend)) {
    if (session.backend) dcc_voxel_bridge_destroy(session.backend);
    sessions.erase(ob);
    BKE_id_free(nullptr, source);
    BKE_report(op->reports, RPT_ERROR, "Golemics voxel backend initialization failed");
    return false;
  }
  tag_voxel_mesh(C, *ob);
  return true;
}

/* Rays and brush radii use the voxel field's object-space coordinates. */
static void voxel_sculpt_apply_dab(bContext *C,
                                   const wmEvent *event,
                                   wmOperator *op,
                                   VoxelSession &session)
{
  const float mouse[2] = {float(event->mval[0]), float(event->mval[1])};
  float3 origin, direction;
  ED_view3d_win_to_ray(CTX_wm_region(C), mouse, origin, direction);
  const float4x4 &inverse = session.object->world_to_object();
  origin = math::transform_point(inverse, origin);
  direction = math::normalize(math::transform_direction(inverse, direction));
  if (!session.backend_stroke) {
    const int mode = (event->modifier & KM_CTRL) ? 1 :
                     (event->modifier & KM_SHIFT) ? 2 : RNA_enum_get(op->ptr, "brush");
    session.backend_stroke = dcc_voxel_bridge_begin_stroke(
        session.backend, mode, RNA_float_get(op->ptr, "radius"),
        RNA_float_get(op->ptr, "strength"), 0.2F) != 0;
  }
  if (session.backend_stroke) {
    dcc_voxel_bridge_dab(session.backend, origin, direction,
                         WM_event_tablet_data(event, nullptr, nullptr));
  }
}

static void voxel_sculpt_cancel_session(bContext *C, Object &ob, VoxelSession &session)
{
  if (session.backend) {
    if (session.backend_stroke) dcc_voxel_bridge_end_stroke(session.backend);
    dcc_voxel_bridge_leave(session.backend, 0);
    dcc_voxel_bridge_destroy(session.backend);
    session.backend = nullptr;
  }
  Mesh *mesh = id_cast<Mesh *>(ob.data);
  if (session.source) {
    Mesh *source = session.source;
    session.source = nullptr;
    BKE_mesh_nomain_to_mesh(source, mesh, &ob);
  }
  tag_voxel_mesh(C, ob);
}

static void voxel_sculpt_status(bContext *C, wmOperator *op)
{
  static const char *brush_names[] = {
      "Add", "Subtract", "Smooth", "Flatten", "Inflate", "Crease", "Clay", "Clay Strips"};
  char status[512];
  BLI_snprintf(status, sizeof(status),
               "Voxel Sculpt: %s | Radius %.4g | Strength %.2f | "
               "LMB: Paint, Ctrl: Subtract, Shift: Smooth | "
               "Ctrl Wheel: Radius, Shift Wheel: Strength, B: Brush | "
               "Ctrl Z / Ctrl Shift Z: Undo / Redo | Enter: Commit, Esc: Cancel",
               brush_names[RNA_enum_get(op->ptr, "brush")],
               RNA_float_get(op->ptr, "radius"), RNA_float_get(op->ptr, "strength"));
  ED_area_status_text(CTX_wm_area(C), status);
}

static void voxel_sculpt_finish(bContext *C, Object &ob, const bool commit)
{
  auto it = sessions.find(&ob);
  if (it == sessions.end()) return;
  VoxelSession &session = it->second;
  session.C = C;
  if (commit) {
    dcc_voxel_bridge_leave(session.backend, 1);
    dcc_voxel_bridge_destroy(session.backend);
    if (ob.mode == OB_MODE_SCULPT) {
      Mesh *mesh = id_cast<Mesh *>(ob.data);
      Mesh *result = BKE_mesh_copy_for_eval(*mesh);
      BKE_mesh_nomain_to_mesh(session.source, mesh, &ob);
      session.source = nullptr;
      undo::geometry_begin_ex(*CTX_data_scene(C), ob, "Golemics Voxel Sculpt");
      BKE_mesh_nomain_to_mesh(result, mesh, &ob);
      undo::geometry_end(ob);
    }
    if (session.source) BKE_id_free(nullptr, session.source);
    tag_voxel_mesh(C, ob);
  }
  else {
    voxel_sculpt_cancel_session(C, ob, session);
  }
  sessions.erase(it);
  ED_area_status_text(CTX_wm_area(C), nullptr);
}

static void voxel_sculpt_cancel(bContext *C, wmOperator *op)
{
  if (auto *ob = static_cast<Object *>(op->customdata)) {
    voxel_sculpt_finish(C, *ob, false);
    op->customdata = nullptr;
  }
}

static wmOperatorStatus voxel_sculpt_modal(bContext *C, wmOperator *op, const wmEvent *event)
{
  Object *ob = static_cast<Object *>(op->customdata);
  auto it = sessions.find(ob);
  if (it == sessions.end()) {
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  VoxelSession &session = it->second;
  session.C = C;
  if (CTX_data_active_object(C) != ob) {
    voxel_sculpt_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  if (event->val == KM_PRESS && event->type == EVT_ESCKEY) {
    voxel_sculpt_cancel(C, op);
    return OPERATOR_CANCELLED;
  }
  if (event->val == KM_PRESS && ELEM(event->type, EVT_RETKEY, EVT_PADENTER)) {
    voxel_sculpt_finish(C, *ob, true);
    op->customdata = nullptr;
    return OPERATOR_FINISHED;
  }
  if (event->val == KM_PRESS && event->type == EVT_ZKEY &&
      (event->modifier & KM_CTRL)) {
    if (!session.stroke_active) {
      if (event->modifier & KM_SHIFT) dcc_voxel_bridge_redo(session.backend);
      else dcc_voxel_bridge_undo(session.backend);
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (!session.stroke_active && event->val == KM_PRESS) {
    if (ELEM(event->type, WHEELUPMOUSE, WHEELDOWNMOUSE)) {
      const bool increase = event->type == WHEELUPMOUSE;
      if (event->modifier & KM_CTRL) {
        RNA_float_set(op->ptr, "radius", RNA_float_get(op->ptr, "radius") *
                                          (increase ? 1.1F : 1.0F / 1.1F));
        voxel_sculpt_status(C, op);
        return OPERATOR_RUNNING_MODAL;
      }
      if (event->modifier & KM_SHIFT) {
        RNA_float_set(op->ptr, "strength", RNA_float_get(op->ptr, "strength") +
                                            (increase ? 0.05F : -0.05F));
        voxel_sculpt_status(C, op);
        return OPERATOR_RUNNING_MODAL;
      }
    }
    if (event->type == EVT_BKEY) {
      RNA_enum_set(op->ptr, "brush", (RNA_enum_get(op->ptr, "brush") + 1) % 8);
      voxel_sculpt_status(C, op);
      return OPERATOR_RUNNING_MODAL;
    }
  }
  if (event->type == LEFTMOUSE) {
    if (event->val == KM_PRESS) {
      session.stroke_active = true;
      voxel_sculpt_apply_dab(C, event, op, session);
    }
    else if (event->val == KM_RELEASE) {
      session.stroke_active = false;
      if (session.backend_stroke) {
        dcc_voxel_bridge_end_stroke(session.backend);
        session.backend_stroke = false;
      }
    }
    return OPERATOR_RUNNING_MODAL;
  }
  if (event->type == MOUSEMOVE && session.stroke_active) {
    voxel_sculpt_apply_dab(C, event, op, session);
    return OPERATOR_RUNNING_MODAL;
  }
  if (!session.stroke_active && ELEM(event->type, MIDDLEMOUSE, WHEELUPMOUSE,
                                     WHEELDOWNMOUSE, MOUSEPAN, MOUSEZOOM, MOUSEROTATE)) {
    return OPERATOR_PASS_THROUGH;
  }
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus voxel_sculpt_invoke(bContext *C, wmOperator *op, const wmEvent * /*event*/)
{
  if (!CTX_wm_region_view3d(C)) {
    BKE_report(op->reports, RPT_ERROR, "A 3D View is required");
    return OPERATOR_CANCELLED;
  }
  if (!voxel_sculpt_begin(C, op)) {
    return OPERATOR_CANCELLED;
  }
  op->customdata = CTX_data_active_object(C);
  voxel_sculpt_status(C, op);
  WM_event_add_modal_handler(C, op);
  return OPERATOR_RUNNING_MODAL;
}

static wmOperatorStatus voxel_sculpt_exec(bContext *C, wmOperator *op)
{
  if (!voxel_sculpt_begin(C, op)) return OPERATOR_CANCELLED;
  Object *ob = CTX_data_active_object(C);
  VoxelSession &session = sessions.at(ob);
  if (RNA_boolean_get(op->ptr, "use_dab")) {
    float3 origin, direction;
    RNA_float_get_array(op->ptr, "ray_origin", origin);
    RNA_float_get_array(op->ptr, "ray_direction", direction);
    origin = math::transform_point(ob->world_to_object(), origin);
    direction = math::normalize(math::transform_direction(ob->world_to_object(), direction));
    if (dcc_voxel_bridge_begin_stroke(session.backend, RNA_enum_get(op->ptr, "brush"),
                                       RNA_float_get(op->ptr, "radius"),
                                       RNA_float_get(op->ptr, "strength"), 0.2F)) {
      dcc_voxel_bridge_dab(session.backend, origin, direction, 1.0F);
      dcc_voxel_bridge_end_stroke(session.backend);
    }
  }
  voxel_sculpt_finish(C, *ob, RNA_boolean_get(op->ptr, "commit"));
  return OPERATOR_FINISHED;
}

static wmOperatorStatus voxel_sculpt_end_exec(bContext *C, wmOperator *op)
{
  Object *ob = CTX_data_active_object(C);
  auto it = ob ? sessions.find(ob) : sessions.end();
  if (it == sessions.end()) {
    BKE_report(op->reports, RPT_ERROR, "No Golemics voxel sculpt session is active");
    return OPERATOR_CANCELLED;
  }

  voxel_sculpt_finish(C, *ob, RNA_boolean_get(op->ptr, "commit"));
  return OPERATOR_FINISHED;
}

}  // namespace blender::ed::sculpt_paint

/* Declared in namespace blender by paint_intern.hh, where paint_ops.cc registers them. */
namespace blender {

void SCULPT_OT_golemics_voxel_sculpt(wmOperatorType *ot)
{
  namespace sp = ed::sculpt_paint;
  ot->name = "Start Golemics Voxel Sculpt";
  ot->description = "Start a persistent voxel sculpt session on the active mesh";
  ot->idname = "SCULPT_OT_golemics_voxel_sculpt";
  ot->poll = sp::voxel_sculpt_poll;
  ot->invoke = sp::voxel_sculpt_invoke;
  ot->modal = sp::voxel_sculpt_modal;
  ot->cancel = sp::voxel_sculpt_cancel;
  ot->exec = sp::voxel_sculpt_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_float(ot->srna, "voxel_size", 0.05f, 0.001f, 10.0f, "Voxel Size", "Voxel side length", 0.001f, 1.0f);
  static const EnumPropertyItem brushes[] = {
      {0, "ADD", 0, "Add", "Add material"},
      {1, "SUBTRACT", 0, "Subtract", "Remove material"},
      {2, "SMOOTH", 0, "Smooth", "Smooth the surface"},
      {3, "FLATTEN", 0, "Flatten", "Flatten the surface"},
      {4, "INFLATE", 0, "Inflate", "Inflate the surface"},
      {5, "CREASE", 0, "Crease", "Carve a crease"},
      {6, "CLAY_ADD", 0, "Clay", "Build clay"},
      {7, "CLAY_STRIPS", 0, "Clay Strips", "Build clay strips"},
      {0, nullptr, 0, nullptr, nullptr},
  };
  RNA_def_enum(ot->srna, "brush", brushes, 0, "Brush", "Voxel sculpt brush");
  RNA_def_boolean(ot->srna, "commit", true, "Commit", "Keep the result of non-interactive execution");
  RNA_def_boolean(ot->srna, "use_dab", false, "Apply Dab", "Apply a ray-guided dab in non-interactive execution");
  const float ray_origin[3] = {0, 0, 3};
  const float ray_direction[3] = {0, 0, -1};
  RNA_def_float_vector(ot->srna, "ray_origin", 3, ray_origin, -FLT_MAX, FLT_MAX,
                       "Ray Origin", "World-space ray origin for scripted dabs", -100, 100);
  RNA_def_float_vector(ot->srna, "ray_direction", 3, ray_direction, -FLT_MAX, FLT_MAX,
                       "Ray Direction", "World-space ray direction for scripted dabs", -1, 1);
  RNA_def_float(ot->srna, "radius", 0.1f, 0.0001f, 100.0f, "Radius", "Voxel brush radius", 0.0001f, 10.0f);
  RNA_def_float(ot->srna, "strength", 0.25f, 0.0f, 1.0f, "Strength", "Voxel brush strength", 0.0f, 1.0f);
}

void SCULPT_OT_golemics_voxel_sculpt_end(wmOperatorType *ot)
{
  ot->name = "End Golemics Voxel Sculpt";
  ot->description = "Commit or cancel the active Golemics voxel sculpt session";
  ot->idname = "SCULPT_OT_golemics_voxel_sculpt_end";
  ot->poll = ed::sculpt_paint::voxel_sculpt_poll;
  ot->exec = ed::sculpt_paint::voxel_sculpt_end_exec;
  ot->flag = OPTYPE_REGISTER | OPTYPE_UNDO;
  RNA_def_boolean(ot->srna, "commit", true, "Commit", "Keep the voxel mesh");
}

}  // namespace blender
