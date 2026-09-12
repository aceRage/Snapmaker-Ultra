#include "GLGizmoEdit.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/Gizmos/GLGizmosCommon.hpp"
#include "slic3r/GUI/ImGuiWrapper.hpp"
#include "slic3r/GUI/MeshUtils.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/format.hpp"
#include "slic3r/Utils/UndoRedo.hpp"

#include "libslic3r/Geometry.hpp"
#include "libslic3r/Line.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/MeshSculpt.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <cfloat>
#include <cmath>

namespace Slic3r::GUI {

// The same z-fight offset GLGizmoMeasure uses for its plane overlay, so the two
// gizmos' highlights sit at the same height above the part.
static constexpr float EditHighlightOffset = 0.05f;
// A chain is drawn as a thin triangular prism per segment, in the spirit of the
// Measure gizmo's cylinder-per-segment edges, but built through init_plane_data's
// own vertex layout so there is one rendering path for both kinds of highlight.
// The radius is a fraction of the mesh's bounding-box diagonal so a chain is
// visible on a 5 mm part and not a slab on a 300 mm one.
static constexpr float EditChainRadiusFraction = 0.0025f;

GLGizmoEdit::GLGizmoEdit(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoEdit::~GLGizmoEdit() = default;

bool GLGizmoEdit::on_init()
{
    m_shortcut_key = 0;

    const wxString ctrl = _L("Ctrl+");

    m_desc["mode"]              = _L("Select");
    m_desc["mode_face"]         = _L("Face");
    m_desc["mode_smooth_face"]  = _L("Curved face");
    m_desc["mode_chain"]        = _L("Edge chain");
    m_desc["mode_face_hint"]    = _L("Click a face: the whole flat face is selected.");
    m_desc["mode_smooth_hint"]  = _L("Click a face: a gently curved face is selected whole instead of one triangle at a time.");
    m_desc["mode_chain_hint"]   = _L("Hover an edge: the whole edge chain lights up. Selecting a chain is preparation for bevel and chamfer, which are not in this release.");
    m_desc["feature_angle"]     = _L("Edge angle");
    m_desc["feature_angle_hint"]= _L("An edge counts as an edge when the two faces meeting at it turn by more than this.");
    m_desc["planar_tol"]        = _L("Face tolerance");
    m_desc["smooth_step"]       = _L("Curvature per step");
    m_desc["smooth_cap"]        = _L("Total curvature");
    m_desc["push"]              = _L("Push / pull");
    m_desc["distance"]          = _L("Distance");
    m_desc["snap"]              = _L("Snap");
    m_desc["snap_step"]         = _L("Step");
    m_desc["apply"]             = _L("Apply");
    m_desc["reset_selection"]   = _L("Clear selection");
    m_desc["undo"]              = _L("Undo");
    m_desc["redo"]              = _L("Redo");
    m_desc["undo_caption"]      = ctrl + _L("Z");
    m_desc["redo_caption"]      = ctrl + _L("Y");
    m_desc["no_part"]           = _L("Select a single part to edit it.");
    m_desc["no_selection"]      = _L("Click a face on the part to select it, then drag the arrow or type a distance.");
    m_desc["chain_selected"]    = _L("Edge chain selected. Push and pull work on faces; bevel and chamfer arrive in a later release.");
    m_desc["paint_kept"]        = _L("Pushing a face keeps painted supports, seams, colours and fuzzy skin: it moves points and adds no triangles.");
    m_desc["drag_hint"]         = _L("Drag the arrow to push the face along its normal.");
    m_desc["selected_info"]     = _L("Selected: %1% triangles, %2% mm2");
    m_desc["chain_info"]        = _L("Selected: %1% edges%2%");
    m_desc["chain_closed"]      = _L(" (closed loop)");
    m_desc["err_self"]          = _L("That push would make the part intersect itself, so it was not applied. Try a smaller distance.");
    m_desc["err_bounds"]        = _L("That distance is larger than the part, so it was not applied.");
    m_desc["err_whole"]         = _L("That selection is the whole part. Use the Move gizmo to move a whole part.");
    m_desc["err_empty"]         = _L("Nothing is selected.");

    return true;
}

std::string GLGizmoEdit::on_get_name() const { return _u8L("Edit"); }

std::string GLGizmoEdit::get_gizmo_entering_text() const { return _u8L("Entering Edit gizmo"); }
std::string GLGizmoEdit::get_gizmo_leaving_text() const { return _u8L("Leaving Edit gizmo"); }

CommonGizmosDataID GLGizmoEdit::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

bool GLGizmoEdit::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    // One part at a time, as Sculpt does: the session owns one mesh.
    return selection.get_volume_idxs().size() == 1;
}

// ----------------------------------------------------------------------------
// selection / session
// ----------------------------------------------------------------------------

ModelVolume *GLGizmoEdit::selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const
{
    object_idx = volume_idx = mesh_id = -1;

    const Selection &selection = m_parent.get_selection();
    const Selection::IndicesList &idxs = selection.get_volume_idxs();
    if (idxs.size() != 1)
        return nullptr;
    const GLVolume *gl_volume = selection.get_volume(*idxs.begin());
    if (gl_volume == nullptr)
        return nullptr;

    const GLVolume::CompositeID &cid = gl_volume->composite_id;
    const ModelObjectPtrs &objects = wxGetApp().model().objects;
    if (cid.object_id < 0 || objects.size() <= size_t(cid.object_id))
        return nullptr;
    ModelObject *object = objects[cid.object_id];
    if (cid.volume_id < 0 || object->volumes.size() <= size_t(cid.volume_id))
        return nullptr;
    ModelVolume *mv = object->volumes[cid.volume_id];
    if (!mv->is_model_part())
        return nullptr;

    // The shared Raycaster indexes only the model-part volumes, in order.
    int counter = -1;
    for (int i = 0; i <= cid.volume_id; ++i)
        if (object->volumes[i]->is_model_part())
            ++counter;

    object_idx = cid.object_id;
    volume_idx = cid.volume_id;
    mesh_id    = counter;
    return mv;
}

void GLGizmoEdit::attach_to_selection()
{
    int object_idx = -1, volume_idx = -1, mesh_id = -1;
    ModelVolume *mv = selected_volume(object_idx, volume_idx, mesh_id);
    if (mv == nullptr) {
        detach();
        return;
    }
    if (m_volume == mv && m_session && m_volume_id == mv->id())
        return;

    m_volume     = mv;
    m_volume_id  = mv->id();
    m_object_idx = object_idx;
    m_volume_idx = volume_idx;
    m_mesh_id    = mesh_id;
    m_session    = std::make_unique<MeshEdit::EditSession>(mv->mesh().its);
    clear_selection();
}

void GLGizmoEdit::detach()
{
    m_volume     = nullptr;
    m_object_idx = m_volume_idx = m_mesh_id = -1;
    m_session.reset();
    m_dragging   = false;
    clear_selection();
}

void GLGizmoEdit::data_changed(bool /* is_serializing */)
{
    if (m_state != On)
        return;
    attach_to_selection();
}

void GLGizmoEdit::on_set_state()
{
    if (m_state == On) {
        attach_to_selection();
    } else {
        if (m_dragging)
            cancel_drag();
        detach();
    }
}

Transform3d GLGizmoEdit::volume_trafo() const
{
    const Selection &selection = m_parent.get_selection();
    const ModelObject *mo = m_c->selection_info() ? m_c->selection_info()->model_object() : nullptr;
    if (mo == nullptr || m_volume == nullptr)
        return Transform3d::Identity();
    const int instance_idx = selection.get_instance_idx();
    if (instance_idx < 0 || size_t(instance_idx) >= mo->instances.size())
        return Transform3d::Identity();
    return mo->instances[instance_idx]->get_transformation().get_matrix() * m_volume->get_matrix();
}

double GLGizmoEdit::mesh_scale() const
{
    // The push distance is a world-space length; the mesh lives in the volume's
    // own space. As in Sculpt, a roughly uniform scale is assumed and the mean of
    // the three factors is used.
    const Vec3d s = Geometry::Transformation(volume_trafo()).get_scaling_factor();
    const double mean = (std::abs(s.x()) + std::abs(s.y()) + std::abs(s.z())) / 3.;
    return mean > EPSILON ? mean : 1.;
}

// ----------------------------------------------------------------------------
// picking
// ----------------------------------------------------------------------------

bool GLGizmoEdit::raycast(const Vec2d &mouse_position, Vec3f &hit, Vec3f &normal, size_t &facet) const
{
    if (m_volume == nullptr || m_mesh_id < 0 || m_c == nullptr || m_c->raycaster() == nullptr)
        return false;
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (size_t(m_mesh_id) >= raycasters.size())
        return false;

    const Camera &camera = wxGetApp().plater()->get_camera();
    return raycasters[m_mesh_id]->unproject_on_mesh(mouse_position, volume_trafo(), camera, hit, normal, nullptr, &facet);
}

MeshEdit::RegionParams GLGizmoEdit::region_params() const
{
    MeshEdit::RegionParams p;
    if (m_pick_mode == PickMode::SmoothFace) {
        p.mode           = MeshEdit::RegionMode::Smooth;
        p.step_angle_deg = m_smooth_step;
        p.angle_tol_deg  = m_smooth_cap;
    } else {
        p.mode          = MeshEdit::RegionMode::Planar;
        p.angle_tol_deg = m_planar_tol;
    }
    return p;
}

void GLGizmoEdit::update_hover(const Vec2d &mouse_position)
{
    m_last_mouse = mouse_position;
    if (!m_session || m_volume == nullptr) {
        m_hover_valid = false;
        return;
    }

    Vec3f  hit = Vec3f::Zero(), normal = Vec3f::Zero();
    size_t facet = 0;
    m_hover_valid = raycast(mouse_position, hit, normal, facet);
    if (!m_hover_valid) {
        m_hover_region = MeshEdit::FaceRegion{};
        m_hover_chain  = MeshEdit::EdgeChain{};
        m_hover_region_key = m_hover_chain_key = -1;
        return;
    }
    m_hover_point = hit;
    m_hover_facet = facet;

    if (m_pick_mode == PickMode::EdgeChain) {
        m_hover_region = MeshEdit::FaceRegion{};
        m_hover_region_key = -1;
        const int edge = m_session->nearest_edge(facet, hit);
        if (edge != m_hover_chain_key) {
            m_hover_chain     = m_session->grow_chain(edge, m_feature_angle, m_chain_continuation);
            m_hover_chain_key = m_hover_chain.empty() ? -1 : edge;
            m_hover_chain_model.reset();
        }
    } else {
        m_hover_chain = MeshEdit::EdgeChain{};
        m_hover_chain_key = -1;
        // The cache key is the seed facet, so a hover that stays on the same
        // facet rebuilds nothing. A hover that moves to a different facet of the
        // SAME region does re-grow it - the grow is bounded and cheap, and
        // keying on the region itself would need a facet -> region map that
        // phase 1 does not otherwise want.
        if (int(facet) != m_hover_region_key) {
            m_hover_region     = m_session->grow_region(facet, region_params());
            m_hover_region_key = m_hover_region.empty() ? -1 : int(facet);
            m_hover_region_model.reset();
        }
    }
}

void GLGizmoEdit::commit_hover_to_selection()
{
    if (!m_hover_valid)
        return;
    if (m_pick_mode == PickMode::EdgeChain) {
        if (m_hover_chain.empty())
            return;
        m_selected_chain       = m_hover_chain;
        m_selected_chain_key   = m_hover_chain_key;
        m_selected_region      = MeshEdit::FaceRegion{};
        m_selection_is_chain   = true;
    } else {
        if (m_hover_region.empty())
            return;
        m_selected_region      = m_hover_region;
        m_selected_region_key  = m_hover_region_key;
        m_selected_chain       = MeshEdit::EdgeChain{};
        m_selection_is_chain   = false;
    }
    m_has_selection    = true;
    m_push_distance    = 0.f;
    m_show_last_status = false;
    m_selected_region_model.reset();
    m_selected_chain_model.reset();
}

void GLGizmoEdit::clear_selection()
{
    m_has_selection      = false;
    m_selection_is_chain = false;
    m_selected_region    = MeshEdit::FaceRegion{};
    m_selected_chain     = MeshEdit::EdgeChain{};
    m_hover_region       = MeshEdit::FaceRegion{};
    m_hover_chain        = MeshEdit::EdgeChain{};
    m_push_distance      = 0.f;
    m_show_last_status   = false;
    invalidate_highlight_models();
}

void GLGizmoEdit::invalidate_highlight_models()
{
    m_hover_region_model.reset();
    m_selected_region_model.reset();
    m_hover_chain_model.reset();
    m_selected_chain_model.reset();
    m_hover_region_key = m_selected_region_key = -1;
    m_hover_chain_key  = m_selected_chain_key  = -1;
}

// ----------------------------------------------------------------------------
// push / pull
// ----------------------------------------------------------------------------

// The push axis in WORLD space: the region's normal carried through the volume
// transform. A non-uniform scale skews normals, hence the inverse-transpose.
static Vec3d push_axis_world(const Transform3d &trafo, const Vec3f &normal_mesh)
{
    const Matrix3d nm = trafo.matrix().block(0, 0, 3, 3).inverse().transpose();
    Vec3d axis = nm * normal_mesh.cast<double>();
    const double l = axis.norm();
    return l > EPSILON ? Vec3d(axis / l) : Vec3d::UnitZ();
}

bool GLGizmoEdit::drag_distance(const Vec2d &mouse_position, float &out) const
{
    // Project the mouse ray onto the LINE through the drag anchor along the push
    // axis, and report how far along that line the closest approach is. That is
    // the standard "drag along an axis" solve the Move gizmo uses, and unlike
    // projecting onto a camera-facing plane it stays stable when the axis points
    // nearly at the camera.
    const Linef3 ray = m_parent.mouse_ray(Point(int(mouse_position.x()), int(mouse_position.y())));
    const Vec3d  rd  = ray.b - ray.a;
    const Vec3d &ad  = m_drag_axis_world;

    const double a = ad.dot(ad);
    const double b = ad.dot(rd);
    const double c = rd.dot(rd);
    const Vec3d  w = m_drag_anchor_world - ray.a;
    const double d = ad.dot(w);
    const double e = rd.dot(w);

    const double denom = a * c - b * b;
    if (std::abs(denom) < 1e-12)
        return false;
    const double t = (d * c - b * e) / denom;   // distance along the axis, world mm
    out = float(t);
    return true;
}

bool GLGizmoEdit::begin_drag(const Vec2d &mouse_position)
{
    if (!m_session || !m_has_selection || m_selection_is_chain || m_selected_region.empty())
        return false;

    m_drag_base_mesh = m_session->mesh();
    m_drag_region    = m_selected_region;
    const Transform3d trafo = volume_trafo();
    m_drag_axis_world  = push_axis_world(trafo, m_drag_region.normal);
    m_drag_anchor_world = trafo * m_drag_region.center.cast<double>();

    float t = 0.f;
    if (!drag_distance(mouse_position, t))
        return false;
    m_drag_start_distance = t;
    m_drag_distance       = 0.f;
    m_dragging            = true;
    m_show_last_status    = false;
    return true;
}

void GLGizmoEdit::update_drag(const Vec2d &mouse_position)
{
    if (!m_dragging)
        return;
    float t = 0.f;
    if (!drag_distance(mouse_position, t))
        return;
    float d = t - m_drag_start_distance;
    if (m_push_snap)
        d = MeshEdit::snap_to_step(d, m_push_step);
    if (d == m_drag_distance)
        return;
    m_drag_distance = d;
    m_push_distance = d;
    // Preview only: no self-intersection test (too slow per tick), no undo
    // entry, no commit to the Model. The drag is absolute, so the whole
    // displacement is re-applied to the pre-drag mesh every tick.
    apply_push(d, /* final_apply */ false);
}

void GLGizmoEdit::end_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    const float d = m_drag_distance;
    // Put the working mesh back to where the drag began, then apply once for
    // real: the guarded path takes the undo snapshot, runs the self-intersection
    // test and commits to the volume.
    m_session->set_mesh(indexed_triangle_set(m_drag_base_mesh));
    if (d == 0.f) {
        refresh_render_volume();
        return;
    }
    apply_push(d, /* final_apply */ true);
    m_drag_base_mesh = indexed_triangle_set();
}

void GLGizmoEdit::cancel_drag()
{
    if (!m_dragging)
        return;
    m_dragging = false;
    if (m_session && !m_drag_base_mesh.indices.empty())
        m_session->set_mesh(indexed_triangle_set(m_drag_base_mesh));
    m_drag_base_mesh = indexed_triangle_set();
    m_drag_distance  = 0.f;
    m_push_distance  = 0.f;
    refresh_render_volume();
    m_parent.set_as_dirty();
}

void GLGizmoEdit::apply_push(float distance, bool final_apply)
{
    if (!m_session || m_volume == nullptr)
        return;

    // Distances are world millimetres; the mesh lives in the volume's own space.
    const float d_mesh = float(double(distance) / mesh_scale());

    MeshEdit::TranslateParams tp;
    tp.d = d_mesh;
    tp.check_self_intersection = final_apply;

    if (!final_apply) {
        // Preview: translate the PRE-DRAG mesh and show the result without
        // touching the session's history.
        const MeshEdit::MeshTopology topo = MeshEdit::build_topology(m_drag_base_mesh);
        MeshEdit::TranslateResult res = MeshEdit::translate_region(m_drag_base_mesh, topo, m_drag_region, tp);
        m_last_status = res.status;
        if (!res.ok())
            return;
        if (res.status == MeshEdit::TranslateStatus::Ok)
            m_session->set_mesh(std::move(res.mesh));
        refresh_render_volume();
        m_parent.set_as_dirty();
        return;
    }

    const MeshEdit::FaceRegion &region = m_dragging || !m_drag_region.empty() ? m_drag_region : m_selected_region;
    MeshEdit::TranslateResult res = m_session->apply_translate(region.empty() ? m_selected_region : region, tp);
    m_last_status      = res.status;
    m_show_last_status = res.status != MeshEdit::TranslateStatus::Ok && res.status != MeshEdit::TranslateStatus::NoOp;

    if (res.status != MeshEdit::TranslateStatus::Ok) {
        // A refusal leaves the working mesh exactly as it was; put the on-screen
        // preview back in step with it.
        refresh_render_volume();
        m_parent.set_as_dirty();
        if (res.status == MeshEdit::TranslateStatus::SelfIntersects)
            wxGetApp().plater()->get_notification_manager()->push_notification(
                NotificationType::CustomNotification, NotificationManager::NotificationLevel::WarningNotificationLevel,
                into_u8(m_desc.at("err_self")));
        return;
    }

    commit_to_volume();
    // The selection is still the same facets, but their positions moved, so the
    // region's normal, area and centre are stale - re-grow it against the new
    // mesh so a second push starts from the right place.
    if (!m_selected_region.empty()) {
        const int seed = m_selected_region.facets.front();
        m_selected_region = m_session->grow_region(size_t(seed), region_params());
        m_selected_region_model.reset();
    }
    m_push_distance = 0.f;
    m_drag_distance = 0.f;
}

void GLGizmoEdit::apply_push_from_field()
{
    if (!m_session || !m_has_selection || m_selection_is_chain || m_selected_region.empty())
        return;
    float d = m_push_distance;
    if (m_push_snap)
        d = MeshEdit::snap_to_step(d, m_push_step);
    if (d == 0.f)
        return;
    m_drag_region = m_selected_region;
    apply_push(d, /* final_apply */ true);
    m_drag_region = MeshEdit::FaceRegion{};
}

void GLGizmoEdit::commit_to_volume()
{
    if (!m_session || m_volume == nullptr)
        return;

    // One undo step per APPLIED operation, taken before the volume is touched -
    // the discipline GLGizmoSculpt::end_stroke() follows.
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Edit"), UndoRedo::SnapshotType::GizmoAction);

    indexed_triangle_set edited = m_session->mesh();
    // Deliberately NO plater->clear_before_change_mesh(): a translate changes no
    // indices, so every painted annotation on this volume stays valid. That is
    // the whole reason phase 1 is translation-only.
    // commit_sculpted_mesh() refuses outright unless indices_match(), so this
    // can never silently misalign existing paint.
    const bool committed = Sculpt::commit_sculpted_mesh(*m_volume, std::move(edited), /* ensure_on_bed */ true);
    assert(committed);
    if (!committed) {
        // Belt and braces: if the contract were ever broken, do nothing rather
        // than commit a mesh whose paint no longer lines up.
        return;
    }

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    // set_new_unique_id() bumped the volume id; keep the cache in step instead of
    // throwing the session away.
    m_volume_id = m_volume->id();
    invalidate_highlight_models();
}

void GLGizmoEdit::refresh_render_volume()
{
    if (m_object_idx < 0 || m_volume_idx < 0 || !m_session)
        return;
    GLVolumeCollection &volumes = m_parent.get_volumes();
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        v->model.reset();
        v->model.init_from(m_session->mesh());
    }
    invalidate_highlight_models();
}

// ----------------------------------------------------------------------------
// undo
// ----------------------------------------------------------------------------

bool GLGizmoEdit::do_undo()
{
    if (!m_session || !m_session->can_undo())
        return false;
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Undo edit"), UndoRedo::SnapshotType::GizmoAction);
    if (!m_session->undo())
        return false;
    indexed_triangle_set restored = m_session->mesh();
    if (!Sculpt::commit_sculpted_mesh(*m_volume, std::move(restored), /* ensure_on_bed */ true))
        return false;
    plater->changed_mesh(m_object_idx);
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_volume_id = m_volume->id();
    clear_selection();
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoEdit::do_redo()
{
    if (!m_session || !m_session->can_redo())
        return false;
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Redo edit"), UndoRedo::SnapshotType::GizmoAction);
    if (!m_session->redo())
        return false;
    indexed_triangle_set restored = m_session->mesh();
    if (!Sculpt::commit_sculpted_mesh(*m_volume, std::move(restored), /* ensure_on_bed */ true))
        return false;
    plater->changed_mesh(m_object_idx);
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_volume_id = m_volume->id();
    clear_selection();
    m_parent.set_as_dirty();
    return true;
}

bool GLGizmoEdit::on_edit_char(int key_code, bool shift_down, bool ctrl_down)
{
    if (m_state != On || !m_session)
        return false;

    // Esc drops the selection rather than closing the gizmo, the way the Cut
    // gizmo's Draw mode claims Esc to clear its line. Only when there IS a
    // selection to drop - otherwise Esc keeps its usual "close the gizmo"
    // meaning.
    if (key_code == WXK_ESCAPE && !ctrl_down) {
        if (m_dragging) {
            cancel_drag();
            return true;
        }
        if (m_has_selection) {
            clear_selection();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (!ctrl_down)
        return false;

    // wx delivers Ctrl+letter as the control character (1-26) in on_char, so
    // both spellings are accepted.
    const bool is_z = key_code == 'z' || key_code == 'Z' || key_code == 26;
    const bool is_y = key_code == 'y' || key_code == 'Y' || key_code == 25;

    if (is_z && !shift_down)
        return do_undo();
    if (is_y || (is_z && shift_down))
        return do_redo();
    return false;
}

// ----------------------------------------------------------------------------
// mouse
// ----------------------------------------------------------------------------

bool GLGizmoEdit::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));

    if (m_volume == nullptr || !m_session)
        return false;

    if (mouse_event.Moving()) {
        update_hover(mouse_pos);
        m_parent.set_as_dirty();
        // Hovering must not swallow the event: the canvas still wants it for its
        // own tooltip and hover handling.
        return false;
    }

    if (mouse_event.LeftDown()) {
        update_hover(mouse_pos);
        // A click on the push arrow starts a drag; a click on the part picks a
        // new selection. The arrow is tested first so it can be grabbed even
        // when it stands over the part.
        if (m_has_selection && !m_selection_is_chain && m_hover_valid &&
            m_hover_region_key == m_selected_region_key && begin_drag(mouse_pos)) {
            m_parent.set_as_dirty();
            return true;
        }
        if (m_hover_valid) {
            commit_hover_to_selection();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (mouse_event.Dragging()) {
        if (m_dragging) {
            update_drag(mouse_pos);
            return true;
        }
        return false;
    }

    if (mouse_event.LeftUp()) {
        if (m_dragging) {
            end_drag();
            m_parent.set_as_dirty();
            return true;
        }
        return false;
    }

    if (mouse_event.RightDown() && m_dragging) {
        cancel_drag();
        return true;
    }

    if (mouse_event.Leaving()) {
        m_hover_valid = false;
        m_parent.set_as_dirty();
        return false;
    }

    return false;
}

// ----------------------------------------------------------------------------
// rendering
// ----------------------------------------------------------------------------

void GLGizmoEdit::rebuild_region_model(const MeshEdit::FaceRegion &region, GLModel &model, int &cached_key, int key) const
{
    if (region.empty() || !m_session)
        return;
    if (model.is_initialized() && cached_key == key)
        return;
    model.reset();
    // The same builder the Measure gizmo's plane overlay uses, with the same
    // z-fight offset: one rendering path for every facet-list highlight in the
    // application.
    model.init_from(init_plane_data(m_session->mesh(), region.facets, EditHighlightOffset));
    cached_key = key;
}

void GLGizmoEdit::rebuild_chain_model(const MeshEdit::EdgeChain &chain, GLModel &model, int &cached_key, int key) const
{
    if (chain.empty() || !m_session)
        return;
    if (model.is_initialized() && cached_key == key)
        return;
    model.reset();

    const indexed_triangle_set &its  = m_session->mesh();
    const MeshEdit::MeshTopology &topo = m_session->topology();

    // Radius as a fraction of the part, so a chain reads the same on any size.
    Vec3f lo = its.vertices.empty() ? Vec3f::Zero() : its.vertices.front();
    Vec3f hi = lo;
    for (const Vec3f &v : its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    const float radius = std::max(1e-4f, (hi - lo).norm() * EditChainRadiusFraction);

    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    unsigned int base = 0;
    constexpr int Sides = 6;

    for (int e : chain.edges) {
        if (e < 0 || e >= topo.num_edges)
            continue;
        const Vec2i32 &ev = topo.edge_vertices[size_t(e)];
        if (ev(0) < 0 || ev(1) < 0)
            continue;
        const Vec3f a = its.vertices[size_t(ev(0))];
        const Vec3f b = its.vertices[size_t(ev(1))];
        Vec3f axis = b - a;
        const float len = axis.norm();
        if (len < 1e-9f)
            continue;
        axis /= len;
        // Any two vectors perpendicular to the edge; the choice is arbitrary
        // because the tube is round.
        Vec3f u = std::abs(axis.z()) < 0.9f ? axis.cross(Vec3f::UnitZ()) : axis.cross(Vec3f::UnitX());
        u.normalize();
        const Vec3f v = axis.cross(u);

        for (int k = 0; k < Sides; ++k) {
            const float ang = 2.f * float(M_PI) * float(k) / float(Sides);
            const Vec3f n   = u * std::cos(ang) + v * std::sin(ang);
            data.add_vertex(Vec3f(a + n * radius), n);
            data.add_vertex(Vec3f(b + n * radius), n);
        }
        for (int k = 0; k < Sides; ++k) {
            const unsigned int a0 = base + unsigned(2 * k);
            const unsigned int b0 = base + unsigned(2 * k + 1);
            const unsigned int a1 = base + unsigned(2 * ((k + 1) % Sides));
            const unsigned int b1 = base + unsigned(2 * ((k + 1) % Sides) + 1);
            data.add_triangle(a0, b0, b1);
            data.add_triangle(a0, b1, a1);
        }
        base += unsigned(2 * Sides);
    }

    if (data.is_empty())
        return;
    model.init_from(std::move(data));
    cached_key = key;
}

void GLGizmoEdit::render_highlights()
{
    if (!m_session || m_volume == nullptr)
        return;

    // Build whatever is missing. Not const, so it cannot go in on_render's const
    // path - hence on_render() is not const either, matching GLGizmoBase.
    if (m_pick_mode == PickMode::EdgeChain) {
        rebuild_chain_model(m_hover_chain, m_hover_chain_model, m_hover_chain_key, m_hover_chain_key);
        rebuild_chain_model(m_selected_chain, m_selected_chain_model, m_selected_chain_key, m_selected_chain_key);
    } else {
        rebuild_region_model(m_hover_region, m_hover_region_model, m_hover_region_key, m_hover_region_key);
    }
    if (!m_selection_is_chain)
        rebuild_region_model(m_selected_region, m_selected_region_model, m_selected_region_key, m_selected_region_key);

    GLShaderProgram *shader = wxGetApp().get_shader("gouraud_light");
    if (shader == nullptr)
        return;
    shader->start_using();

    const Camera     &camera = wxGetApp().plater()->get_camera();
    const Transform3d trafo  = volume_trafo();
    const Transform3d view   = camera.get_view_matrix();
    shader->set_uniform("view_model_matrix", view * trafo);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix",
                        (Matrix3d) (view.matrix().block(0, 0, 3, 3) * trafo.matrix().block(0, 0, 3, 3).inverse().transpose()));

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));

    // Hover is the paler of the two, selection the solid one - the same
    // convention GLGizmoMeasure's render_glmodel(..., hover) uses.
    const ColorRGBA hover_color(0.30f, 0.70f, 0.95f, 0.35f);
    const ColorRGBA sel_color  (1.00f, 0.60f, 0.10f, 0.60f);

    auto draw = [&](GLModel &m, const ColorRGBA &c) {
        if (!m.is_initialized())
            return;
        m.set_color(c);
        m.render();
    };

    // Selection under hover, so hovering a different face of the same part still
    // reads clearly.
    draw(m_selected_region_model, sel_color);
    draw(m_selected_chain_model, sel_color);
    if (!(m_has_selection && m_hover_region_key == m_selected_region_key))
        draw(m_hover_region_model, hover_color);
    if (!(m_has_selection && m_hover_chain_key == m_selected_chain_key))
        draw(m_hover_chain_model, hover_color);

    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoEdit::render_push_axis() const
{
    if (!m_has_selection || m_selection_is_chain || m_selected_region.empty() || !m_session)
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const indexed_triangle_set &its = m_session->mesh();
    Vec3f lo = its.vertices.empty() ? Vec3f::Zero() : its.vertices.front();
    Vec3f hi = lo;
    for (const Vec3f &v : its.vertices) { lo = lo.cwiseMin(v); hi = hi.cwiseMax(v); }
    const double length = std::max(1.0, double((hi - lo).norm()) * 0.25);
    const double radius = length * 0.03;

    static std::shared_ptr<GLModel> s_axis;
    if (s_axis == nullptr) {
        s_axis = std::make_shared<GLModel>();
        // A unit-length, unit-radius cylinder; the transform below gives it its
        // real proportions.
        s_axis->init_from(its_make_cylinder(1.0, 1.0));
    }

    const Transform3d trafo = volume_trafo();
    // Point the cylinder's +Z along the region normal.
    const Vec3d n = m_selected_region.normal.cast<double>();
    const Vec3d z = Vec3d::UnitZ();
    Transform3d rot = Transform3d::Identity();
    const Vec3d axis = z.cross(n);
    const double s = axis.norm(), c = z.dot(n);
    if (s > EPSILON)
        rot.rotate(Eigen::AngleAxisd(std::atan2(s, c), axis.normalized()));
    else if (c < 0.)
        rot.rotate(Eigen::AngleAxisd(M_PI, Vec3d::UnitX()));

    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d model = trafo *
                              Geometry::assemble_transform(m_selected_region.center.cast<double>()) * rot *
                              Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), Vec3d(radius, radius, length));

    shader->start_using();
    shader->set_uniform("view_model_matrix", camera.get_view_matrix() * model);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    s_axis->set_color(ColorRGBA(1.0f, 0.75f, 0.2f, 0.85f));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    s_axis->render();
    glsafe(::glDisable(GL_BLEND));
    shader->stop_using();
}

void GLGizmoEdit::on_render()
{
    if (m_volume == nullptr || !m_session)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_highlights();
    render_push_axis();
}

// ----------------------------------------------------------------------------
// panel
// ----------------------------------------------------------------------------

void GLGizmoEdit::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    // A fixed width, as Sculpt learnt to use: an auto-sizing panel resizes out
    // from under the pointer whenever a conditional row appears.
    const float window_width  = m_imgui->scaled(18.0f);
    const float approx_height = m_imgui->scaled(20.f);
    y = std::min(y, bottom_limit - approx_height);

#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    ImGui::SetNextWindowSize(ImVec2(window_width, 0.f), ImGuiCond_Always);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    const float wrap_width = ImGui::GetContentRegionAvail().x;

    if (m_volume == nullptr || !m_session) {
        m_imgui->text_wrapped(m_desc.at("no_part"), wrap_width);
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    const float space_size        = m_imgui->get_style_scaling() * 8;
    const float label_col         = m_imgui->calc_text_size(m_desc.at("feature_angle")).x + m_imgui->scaled(1.5f);
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    const float sliders_width     = std::max(m_imgui->scaled(3.0f),
                                             wrap_width - label_col - 1.5f * slider_icon_width - space_size);
    const float drag_left         = ImGui::GetStyle().WindowPadding.x + label_col + sliders_width - space_size;

    // --- pick mode ---
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("mode"));
    {
        int mode = int(m_pick_mode);
        const bool changed =
            ImGui::RadioButton(into_u8(m_desc.at("mode_face")).c_str(), &mode, int(PickMode::Face)) ||
            (ImGui::SameLine(), ImGui::RadioButton(into_u8(m_desc.at("mode_smooth_face")).c_str(), &mode, int(PickMode::SmoothFace))) ||
            (ImGui::SameLine(), ImGui::RadioButton(into_u8(m_desc.at("mode_chain")).c_str(), &mode, int(PickMode::EdgeChain)));
        if (changed && mode != int(m_pick_mode)) {
            m_pick_mode = PickMode(mode);
            clear_selection();
            update_hover(m_last_mouse);
        }
    }

    switch (m_pick_mode) {
    case PickMode::Face:       m_imgui->text_wrapped(m_desc.at("mode_face_hint"), wrap_width); break;
    case PickMode::SmoothFace: m_imgui->text_wrapped(m_desc.at("mode_smooth_hint"), wrap_width); break;
    case PickMode::EdgeChain:  m_imgui->text_wrapped(m_desc.at("mode_chain_hint"), wrap_width); break;
    }

    ImGui::Separator();

    // --- selection thresholds, whichever mode is live ---
    auto slider_row = [&](const wxString &label, const char *id, float *value, float lo, float hi, const char *fmt) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(label);
        ImGui::SameLine(label_col);
        ImGui::PushItemWidth(sliders_width);
        const bool changed = m_imgui->bbl_slider_float_style(id, value, lo, hi, fmt, 1.0f, true);
        ImGui::SameLine(drag_left);
        ImGui::PushItemWidth(1.5f * slider_icon_width);
        const bool typed = ImGui::BBLDragFloat((std::string(id) + "_input").c_str(), value, 0.1f, 0.0f, 0.0f, "%.1f");
        *value = std::clamp(*value, lo, hi);
        return changed || typed;
    };

    bool selection_params_changed = false;
    if (m_pick_mode == PickMode::EdgeChain) {
        selection_params_changed |= slider_row(m_desc.at("feature_angle"), "##edit_feature_angle", &m_feature_angle, 1.f, 179.f, "%.0f deg");
        m_imgui->text_wrapped(m_desc.at("feature_angle_hint"), wrap_width);
    } else if (m_pick_mode == PickMode::SmoothFace) {
        selection_params_changed |= slider_row(m_desc.at("smooth_step"), "##edit_smooth_step", &m_smooth_step, 0.5f, 45.f, "%.1f deg");
        selection_params_changed |= slider_row(m_desc.at("smooth_cap"), "##edit_smooth_cap", &m_smooth_cap, 1.f, 90.f, "%.0f deg");
    } else {
        selection_params_changed |= slider_row(m_desc.at("planar_tol"), "##edit_planar_tol", &m_planar_tol, 0.05f, 20.f, "%.2f deg");
    }
    if (selection_params_changed) {
        // A threshold change invalidates every cached grow, hover and selection
        // alike - the selection was grown with the OLD numbers.
        m_hover_region_key = m_hover_chain_key = -1;
        invalidate_highlight_models();
        update_hover(m_last_mouse);
        m_parent.set_as_dirty();
    }

    ImGui::Separator();

    // --- what is selected ---
    if (!m_has_selection) {
        m_imgui->text_wrapped(m_desc.at("no_selection"), wrap_width);
    } else if (m_selection_is_chain) {
        m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("chain_info"),
                                                m_selected_chain.edges.size(),
                                                m_selected_chain.closed ? m_desc.at("chain_closed") : wxString()),
                              wrap_width);
        m_imgui->text_wrapped(m_desc.at("chain_selected"), wrap_width);
    } else {
        m_imgui->text_wrapped(GUI::format_wxstr(m_desc.at("selected_info"),
                                                m_selected_region.facets.size(),
                                                wxString::Format("%.2f", double(m_selected_region.area))),
                              wrap_width);
    }

    // --- push/pull ---
    if (m_has_selection && !m_selection_is_chain) {
        ImGui::Separator();
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("push"));
        m_imgui->text_wrapped(m_desc.at("drag_hint"), wrap_width);

        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("distance"));
        ImGui::SameLine(label_col);
        ImGui::PushItemWidth(sliders_width);
        ImGui::BBLDragFloat("##edit_distance", &m_push_distance, 0.1f, 0.0f, 0.0f, "%.2f mm");

        m_imgui->bbl_checkbox(m_desc.at("snap"), m_push_snap);
        if (m_push_snap) {
            ImGui::AlignTextToFramePadding();
            m_imgui->text(m_desc.at("snap_step"));
            ImGui::SameLine(label_col);
            ImGui::PushItemWidth(sliders_width);
            ImGui::BBLDragFloat("##edit_snap_step", &m_push_step, 0.05f, 0.0f, 0.0f, "%.2f mm");
            m_push_step = std::clamp(m_push_step, PushStepMin, PushStepMax);
        }

        if (m_imgui->button(m_desc.at("apply")))
            apply_push_from_field();
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("reset_selection")))
            clear_selection();
    }

    // --- undo / redo ---
    ImGui::Separator();
    {
        // This imgui build has no BeginDisabled/EndDisabled, so the greying
        // goes through ImGuiWrapper::button(label, size, enable), which is what
        // the rest of the application uses for a conditionally-live button.
        const bool can_undo = m_session->can_undo();
        const bool can_redo = m_session->can_redo();
        if (m_imgui->button(m_desc.at("undo"), ImVec2(0.f, 0.f), can_undo))
            do_undo();
        ImGui::SameLine();
        if (m_imgui->button(m_desc.at("redo"), ImVec2(0.f, 0.f), can_redo))
            do_redo();
        ImGui::SameLine();
        m_imgui->text(m_desc.at("undo_caption") + " / " + m_desc.at("redo_caption"));
    }

    // --- status ---
    if (m_show_last_status) {
        const wxString *msg = nullptr;
        switch (m_last_status) {
        case MeshEdit::TranslateStatus::SelfIntersects: msg = &m_desc.at("err_self"); break;
        case MeshEdit::TranslateStatus::OutOfBounds:    msg = &m_desc.at("err_bounds"); break;
        case MeshEdit::TranslateStatus::WholeMesh:      msg = &m_desc.at("err_whole"); break;
        case MeshEdit::TranslateStatus::EmptyRegion:    msg = &m_desc.at("err_empty"); break;
        default: break;
        }
        if (msg != nullptr) {
            ImGui::Separator();
            m_imgui->warning_text(*msg);
        }
    }

    ImGui::Separator();
    m_imgui->text_wrapped(m_desc.at("paint_kept"), wrap_width);

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
