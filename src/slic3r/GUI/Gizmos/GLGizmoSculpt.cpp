#include "GLGizmoSculpt.hpp"

#include "slic3r/GUI/Camera.hpp"
#include "slic3r/GUI/GLCanvas3D.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "libslic3r/QuadRemesh.hpp"
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
#include "libslic3r/TriangleMesh.hpp"

#include <glad/gl.h>

#include "libslic3r/Utils.hpp"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>

namespace Slic3r::GUI {

// The cursor indicator, shared with nothing - the paint gizmos keep their own.
static std::shared_ptr<GLModel> s_cursor_sphere;

// Throttle for the fallback path: when the vertex buffer cannot be patched in
// place we rebuild the whole GLModel, at most this often.
static constexpr int64_t RenderRebuildIntervalMs = 30;

GLGizmoSculpt::GLGizmoSculpt(GLCanvas3D &parent, const std::string &icon_filename, unsigned int sprite_id)
    : GLGizmoBase(parent, icon_filename, sprite_id)
{}

GLGizmoSculpt::~GLGizmoSculpt()
{
    if (s_cursor_sphere != nullptr)
        s_cursor_sphere.reset();
}

bool GLGizmoSculpt::on_init()
{
    m_shortcut_key = 0;

    const wxString ctrl = _L("Ctrl+");
    const wxString shift = _L("Shift+");

    m_desc["brush"]              = _L("Brush");
    m_desc["grab"]               = _L("Grab");
    m_desc["inflate"]            = _L("Inflate");
    m_desc["deflate"]            = _L("Deflate");
    m_desc["smooth"]             = _L("Smooth");
    m_desc["flatten"]            = _L("Flatten");
    m_desc["crease"]             = _L("Crease");
    m_desc["pinch"]              = _L("Pinch");
    m_desc["nudge"]              = _L("Nudge");
    m_desc["snakehook"]          = _L("Snake Hook");
    m_desc["claystrips"]         = _L("Clay Strips");
    m_desc["mask"]               = _L("Mask");
    m_desc["radius"]             = _L("Brush size");
    m_desc["radius_caption"]     = ctrl + _L("Mouse wheel");
    m_desc["strength"]           = _L("Strength");
    m_desc["falloff"]            = _L("Smooth falloff");
    m_desc["taubin"]             = _L("Preserve volume (Taubin)");
    m_desc["sculpt_caption"]     = _L("Left mouse button");
    m_desc["sculpt"]             = _L("Sculpt");
    m_desc["invert_caption"]     = shift + _L("Left mouse button");
    m_desc["invert"]             = _L("Invert the brush");
    m_desc["adjust_radius"]      = _L("F, then move the mouse: brush size");
    m_desc["adjust_strength"]    = shift + _L("F, then move the mouse: strength");
    m_desc["adjusting_radius"]   = _L("Brush size: move the mouse, click to keep it, Esc to cancel");
    m_desc["adjusting_strength"] = _L("Strength: move the mouse, click to keep it, Esc to cancel");
    m_desc["inverted"]           = _L("Ctrl held: the brush is inverted");
    m_desc["subdivide"]          = _L("Subdivide");
    m_desc["subdivide_hint"]     = _L("The mesh here is too coarse for this brush size.");
    m_desc["subdivide_warning"]  = _L("Subdividing changes the triangles, so painted supports, seams, colours and fuzzy skin on this part are cleared.");
    m_desc["subdivide_fine"]     = _L("Mesh is fine enough for this brush.");
    m_desc["no_part"]            = _L("Select a single part to sculpt it.");
    m_desc["paint_kept"]         = _L("Sculpting keeps painted supports, seams, colours and fuzzy skin.");
    m_desc["protect_bed"]        = _L("Protect bed contact");
    m_desc["protect_bed_none"]   = _L("This part has no flat area resting on the bed, so there is nothing to protect.");
    m_desc["protect_bed_hint"]   = _L("Protects the flat area touching the bed: those vertices never move, and Subdivide keeps them at the same height. A filleted or rounded bottom edge is not flat, so it is not protected.");
    m_desc["protect_sharp"]      = _L("Protect sharp edges");
    m_desc["sharp_angle"]        = _L("Edge angle");
    m_desc["mask_strength"]      = _L("Mask strength");
    m_desc["clay_offset"]        = _L("Clay height");
    m_desc["clear_mask"]         = _L("Clear mask");
    m_desc["mask_hint"]          = _L("Paint over the part to protect it from every brush. Ctrl paints the protection off again. The mask lasts for this gizmo session only.");
    m_desc["brush_keys"]         = _L("Keys 1-9, 0 and - pick a brush while this gizmo is open.");

    return true;
}

std::string GLGizmoSculpt::on_get_name() const { return _u8L("Sculpt"); }

std::string GLGizmoSculpt::get_gizmo_entering_text() const { return _u8L("Entering Sculpt gizmo"); }
std::string GLGizmoSculpt::get_gizmo_leaving_text() const { return _u8L("Leaving Sculpt gizmo"); }

CommonGizmosDataID GLGizmoSculpt::on_get_requirements() const
{
    return CommonGizmosDataID(int(CommonGizmosDataID::SelectionInfo) | int(CommonGizmosDataID::Raycaster));
}

bool GLGizmoSculpt::on_is_activable() const
{
    const Selection &selection = m_parent.get_selection();
    if (m_parent.get_canvas_type() == GLCanvas3D::CanvasAssembleView)
        return false;
    // One part at a time in v1.
    return selection.get_volume_idxs().size() == 1;
}

// ----------------------------------------------------------------------------
// selection / session
// ----------------------------------------------------------------------------

ModelVolume *GLGizmoSculpt::selected_volume(int &object_idx, int &volume_idx, int &mesh_id) const
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

void GLGizmoSculpt::attach_to_selection()
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
    m_session    = std::make_unique<Sculpt::SculptSession>(mv->mesh().its);
    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();
    // The auto masks are pure functions of this mesh, so they are computed here
    // and again after anything that could move the bottom. Session-only: they go
    // when the session does, and so does any hand-painted mask.
    refresh_masks();
}

void GLGizmoSculpt::detach()
{
    m_volume     = nullptr;
    m_object_idx = m_volume_idx = m_mesh_id = -1;
    m_session.reset();
    m_stroke_active  = false;
    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();
    m_hit_valid   = false;
    m_cursor      = Sculpt::CursorState{};
}

void GLGizmoSculpt::data_changed(bool /* is_serializing */)
{
    if (m_state != On)
        return;
    attach_to_selection();
}

void GLGizmoSculpt::on_set_state()
{
    if (m_state == On) {
        attach_to_selection();
    } else {
        if (m_stroke_active)
            end_stroke();
        detach();
    }
}

Transform3d GLGizmoSculpt::volume_trafo() const
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

double GLGizmoSculpt::mesh_scale() const
{
    // The brush radius is a world-space length; the mesh lives in the volume's
    // own space. v1 assumes a roughly uniform scale and uses the mean of the
    // three scaling factors (a strongly non-uniform scale makes the brush
    // elliptical, which is a v2 problem).
    const Vec3d s = Geometry::Transformation(volume_trafo()).get_scaling_factor();
    const double mean = (std::abs(s.x()) + std::abs(s.y()) + std::abs(s.z())) / 3.;
    return mean > EPSILON ? mean : 1.;
}

// ----------------------------------------------------------------------------
// stroke
// ----------------------------------------------------------------------------

bool GLGizmoSculpt::raycast(const Vec2d &mouse_position, Vec3f &hit) const
{
    if (m_volume == nullptr || m_mesh_id < 0 || m_c == nullptr || m_c->raycaster() == nullptr)
        return false;
    const std::vector<const MeshRaycaster *> raycasters = m_c->raycaster()->raycasters();
    if (size_t(m_mesh_id) >= raycasters.size())
        return false;

    const Camera &camera = wxGetApp().plater()->get_camera();
    Vec3f normal = Vec3f::Zero();
    size_t facet = 0;
    // No clipping plane: the Sculpt gizmo does not request the ObjectClipper resource.
    return raycasters[m_mesh_id]->unproject_on_mesh(mouse_position, volume_trafo(), camera, hit, normal, nullptr, &facet);
}

bool GLGizmoSculpt::project_on_drag_plane(const Vec2d &mouse_position, Vec3d &out) const
{
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Linef3 ray = m_parent.mouse_ray(Point(int(mouse_position.x()), int(mouse_position.y())));
    const Vec3d dir = ray.b - ray.a;
    const Vec3d n = camera.get_dir_forward();
    const double denom = n.dot(dir);
    if (std::abs(denom) < EPSILON)
        return false;
    const double t = n.dot(m_stroke_anchor_world - ray.a) / denom;
    out = ray.a + t * dir;
    return true;
}

// Ctrl inverts Inflate/Deflate, Flatten and Crease. Grab and Smooth have no
// meaningful opposite, so Ctrl does nothing for them (Sculpt::
// brush_inverts_with_ctrl is the single place that decision lives).
bool GLGizmoSculpt::brush_inverted(bool ctrl_down) const
{
    if (! ctrl_down)
        return false;
    switch (m_brush) {
    case Brush::Inflate:
    case Brush::Deflate: return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::Inflate);
    case Brush::Flatten: return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::Flatten);
    case Brush::Crease:  return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::Crease);
    case Brush::Pinch:      return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::Pinch);
    case Brush::ClayStrips: return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::ClayStrips);
    case Brush::Mask:       return Sculpt::brush_inverts_with_ctrl(Sculpt::BrushType::Mask);
    case Brush::Grab:
    case Brush::Smooth:
    case Brush::Nudge:
    case Brush::SnakeHook:
    default:             return false;
    }
}

Sculpt::BrushParams GLGizmoSculpt::make_brush(const Vec3f &center_mesh, const Vec3f &displacement_mesh, bool shift_down, bool ctrl_down) const
{
    Sculpt::BrushParams p;
    p.center   = center_mesh;
    p.radius   = float(double(m_cursor_radius) / mesh_scale());
    p.strength = m_strength;
    p.falloff  = m_falloff;

    // Shift and Ctrl both invert; holding both is a double negative and cancels,
    // which is the least surprising reading of "each of them flips the brush".
    const bool invert = shift_down != brush_inverted(ctrl_down);

    switch (m_brush) {
    case Brush::Grab:
        p.type         = Sculpt::BrushType::Grab;
        p.displacement = displacement_mesh;
        break;
    case Brush::Inflate:
    case Brush::Deflate:
        p.type    = Sculpt::BrushType::Inflate;
        // A tick moves the surface by a fraction of the brush radius, so the
        // brush feels the same at any size.
        p.amount  = 0.06f * p.radius;
        p.deflate = (m_brush == Brush::Deflate);
        // Shift inverts the brush, the way the paint gizmos use it to erase.
        if (invert)
            p.deflate = !p.deflate;
        break;
    case Brush::Smooth:
        p.type       = Sculpt::BrushType::Smooth;
        p.iterations = 1;
        p.taubin     = m_taubin;
        break;
    case Brush::Flatten:
        p.type      = Sculpt::BrushType::Flatten;
        // The plane direction is pinned at stroke start, so a long stroke levels
        // one plane instead of chasing the surface it has just flattened.
        p.plane_normal = m_stroke_plane_normal;
        // Inverted, Flatten becomes Blender's "Fill": only the vertices below
        // the plane come up, so a dent is filled and the bumps are left alone.
        p.fill_only    = invert ? ! m_flatten_fill_only : m_flatten_fill_only;
        break;
    case Brush::Crease:
        p.type      = Sculpt::BrushType::Crease;
        p.plane_normal = m_stroke_plane_normal;
        // Inverted, the valley becomes a ridge.
        p.ridge     = invert;
        p.crease_normal_ratio = 1.f;
        break;
    case Brush::Pinch:
        p.type         = Sculpt::BrushType::Pinch;
        // Same pinned-axis discipline as Crease: refitting every tick would let
        // the axis wander as the surface it is pinching moves.
        p.plane_normal = m_stroke_plane_normal;
        // Inverted, the pinch pushes out instead of in - Blender's Magnify.
        p.magnify      = invert;
        break;
    case Brush::Nudge:
        p.type         = Sculpt::BrushType::Nudge;
        // Nudge rides Grab's displacement field; the kernel flattens it into
        // each vertex's own tangent plane, so the surface slides rather than
        // being lifted.
        p.displacement = displacement_mesh;
        break;
    case Brush::SnakeHook:
        p.type         = Sculpt::BrushType::SnakeHook;
        p.displacement = displacement_mesh;
        break;
    case Brush::ClayStrips:
        p.type         = Sculpt::BrushType::ClayStrips;
        p.plane_normal = m_stroke_plane_normal;
        // Both halves of the plane are pinned for the stroke, not just the
        // direction - see BrushParams::plane_origin.
        p.plane_origin = m_stroke_plane_origin;
        // A fraction of the brush radius, so it feels the same at any size.
        p.clay_offset  = (invert ? -1.f : 1.f) * m_clay_offset_ratio * p.radius;
        break;
    case Brush::Mask:
        p.type        = Sculpt::BrushType::Mask;
        p.strength    = m_mask_strength;
        // Ctrl erases the mask instead of painting it.
        p.mask_amount = invert ? -1.f : 1.f;
        break;
    }
    return p;
}

bool GLGizmoSculpt::start_stroke(const Vec2d &mouse_position, bool shift_down, bool ctrl_down)
{
    if (m_volume == nullptr || !m_session)
        return false;

    Vec3f hit = Vec3f::Zero();
    if (!raycast(mouse_position, hit))
        return false;

    m_stroke_active       = true;
    m_stroke_center_mesh  = hit;
    m_stroke_anchor_world = volume_trafo() * hit.cast<double>();
    m_drag_prev_world     = m_stroke_anchor_world;
    m_stroke_dirty_triangles.clear();
    m_last_render_refresh = 0;

    // Flatten and Crease both work against a plane. Fit it once, here, and hold
    // it for the whole stroke: refitting every tick makes Flatten chase the
    // surface it has already levelled and never converge.
    m_stroke_plane_normal = Vec3f::Zero();
    m_stroke_plane_origin = Vec3f::Zero();
    if (m_brush == Brush::Flatten || m_brush == Brush::Crease ||
        m_brush == Brush::Pinch   || m_brush == Brush::ClayStrips) {
        const float radius_mesh = float(double(m_cursor_radius) / mesh_scale());
        std::vector<uint32_t> verts;
        m_session->collect_vertices_in_radius(hit, radius_mesh, verts);
        Vec3f origin = Vec3f::Zero(), normal = Vec3f::Zero();
        if (Sculpt::fit_plane(m_session->mesh(), m_session->vertex_normals(), verts, hit, radius_mesh, m_falloff, origin, normal)) {
            m_stroke_plane_normal = normal;
            m_stroke_plane_origin = origin;
        }
    }

    // Grab and its drag-driven relatives (Nudge, Snake Hook) need a drag before
    // they do anything - their displacement is the mouse delta, which is zero on
    // the click. Every other brush acts on the click itself.
    if (! brush_is_drag_driven(m_brush))
        continue_stroke(mouse_position, shift_down, ctrl_down);
    return true;
}

void GLGizmoSculpt::continue_stroke(const Vec2d &mouse_position, bool shift_down, bool ctrl_down)
{
    if (!m_stroke_active || !m_session || m_volume == nullptr)
        return;

    Vec3f center_mesh = m_stroke_center_mesh;
    Vec3f displacement_mesh = Vec3f::Zero();

    if (brush_is_drag_driven(m_brush)) {
        Vec3d world = Vec3d::Zero();
        if (!project_on_drag_plane(mouse_position, world))
            return;
        const Vec3d delta_world = world - m_drag_prev_world;
        m_drag_prev_world = world;
        if (delta_world.squaredNorm() < 1e-12)
            return;
        // World delta -> volume space.
        const Transform3d trafo = volume_trafo();
        displacement_mesh = (trafo.linear().inverse() * delta_world).cast<float>();
        // Grab and Nudge hold the anchor they started on and translate one
        // patch; Snake Hook's whole point is that its anchor rides the cursor,
        // so the touched set travels with the drag and pulls a horn out behind
        // it. Both feed SculptSession::apply() whatever centre they want this
        // tick, so the kernel needs no idea which is which.
        center_mesh = m_stroke_center_mesh;
        m_stroke_center_mesh += displacement_mesh;
        if (m_brush == Brush::SnakeHook)
            center_mesh = m_stroke_center_mesh;
    } else {
        // Inflate / Smooth / Flatten / Crease / Pinch / Clay / Mask re-pick the
        // surface under the cursor each tick.
        Vec3f hit = Vec3f::Zero();
        if (raycast(mouse_position, hit))
            center_mesh = hit;
        m_stroke_center_mesh = center_mesh;
    }

    m_session->apply(make_brush(center_mesh, displacement_mesh, shift_down, ctrl_down), m_stroke_step);
    if (m_stroke_step.empty())
        return;

    // The Mask brush writes the per-vertex mask and moves nothing, so there is
    // no mesh edit to commit and no vertex buffer to patch - only a redraw, so
    // the tinted overlay follows the cursor.
    if (m_brush == Brush::Mask) {
        m_parent.set_as_dirty();
        return;
    }

    m_pending_commit = true;
    refresh_render_volumes(m_stroke_step.dirty_triangles, false);
}

void GLGizmoSculpt::end_stroke()
{
    m_stroke_active = false;
    if (!m_pending_commit || !m_session || m_volume == nullptr) {
        m_pending_commit = false;
        return;
    }

    // One undo step per stroke: the snapshot is taken here, before the volume is
    // touched, so it records the pre-stroke mesh. This is the same discipline the
    // paint gizmos use (snapshot on button-up, around update_model_object()).
    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Sculpt"), UndoRedo::SnapshotType::GizmoAction);

    indexed_triangle_set sculpted = m_session->mesh();
    // Deliberately no plater->clear_before_change_mesh(): the indices are
    // untouched, so every painted annotation stays valid.
    const bool committed = Sculpt::commit_sculpted_mesh(*m_volume, std::move(sculpted), /* ensure_on_bed */ true);
    assert(committed);
    (void) committed;

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    m_pending_commit = false;
    m_stroke_dirty_triangles.clear();

    // The AABB tree and the shared raycaster are both stale now.
    m_session->rebuild_tree();
    // A stroke can have moved the part's lowest vertices, and ensure_on_bed()
    // above may have shifted the whole thing - so z_min and the bed-contact set
    // are both potentially stale. Vertex ids are unchanged (that is the whole
    // point of the commit path), so a hand-painted mask stays valid across this.
    refresh_masks();
    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    // set_new_unique_id() bumped the volume id; keep the cache in step instead of
    // throwing the (just rebuilt) session away.
    m_volume_id = m_volume->id();
}

void GLGizmoSculpt::cancel_stroke()
{
    m_stroke_active  = false;
    m_pending_commit = false;
    m_stroke_plane_normal = Vec3f::Zero();
    if (m_volume != nullptr)
        m_session = std::make_unique<Sculpt::SculptSession>(m_volume->mesh().its);
}

// The cursor sphere is repositioned here, from BOTH the hover path and every
// drag tick. Doing it only on wxMouseEvent::Moving() (which wx fires only while
// no button is held) was the v1 bug: once a drag started, wx sent Dragging()
// instead and the sphere stayed frozen at the click point.
void GLGizmoSculpt::update_cursor(const Vec2d &mouse_position)
{
    m_last_mouse = mouse_position;

    Vec3f hit = Vec3f::Zero();
    m_hit_valid = raycast(mouse_position, hit);
    if (m_hit_valid)
        m_hit = hit;

    Sculpt::CursorInput in;
    in.hit_valid = m_hit_valid;
    in.hit       = hit;
    if (m_stroke_active)
        in.mode = brush_is_drag_driven(m_brush) ? Sculpt::CursorMode::StrokeGrab : Sculpt::CursorMode::StrokeHit;
    // For Grab the stroke centre has already been advanced by the drag, so it is
    // exactly "the anchor moved by the accumulated drag" the cursor should ride.
    in.grab_anchor = m_stroke_center_mesh;

    m_cursor = Sculpt::next_cursor_state(m_cursor, in);
}

bool GLGizmoSculpt::on_mouse(const wxMouseEvent &mouse_event)
{
    const Vec2d mouse_pos(double(mouse_event.GetX()), double(mouse_event.GetY()));
    const bool  control_down = mouse_event.CmdDown();

    // The F / Shift+F modal owns the mouse outright while it runs: moving sizes
    // the brush, left click keeps the new value, right click puts it back. No
    // stroke may start and the canvas must not see the click.
    if (m_adjust.active()) {
        if (mouse_event.Moving() || mouse_event.Dragging()) {
            update_adjust(mouse_pos);
            m_parent.set_as_dirty();
            return true;
        }
        if (mouse_event.LeftDown() || mouse_event.LeftUp()) {
            if (mouse_event.LeftDown()) {
                end_adjust(/* confirm */ true);
                // The modal is over as of THIS event, so the matching LeftUp a
                // moment from now would find m_adjust inactive, fall through the
                // whole gizmo, and reach GLCanvas3D's LeftUp handler - which
                // calls deselect_all() when the click landed on no volume. A
                // brush-sizing drag almost always ends off the part, so the
                // confirming click was deselecting the object the user was
                // sculpting and they had to re-pick it. Owe ourselves that up.
                m_swallow_left_up = true;
            }
            m_parent.set_as_dirty();
            return true;
        }
        if (mouse_event.RightDown() || mouse_event.RightUp()) {
            if (mouse_event.RightDown()) {
                end_adjust(/* confirm */ false);
                // Same for the cancelling right click: a RightUp that reaches
                // the canvas opens the context menu over the plate.
                m_swallow_right_up = true;
            }
            m_parent.set_as_dirty();
            return true;
        }
        return true;
    }

    // The other half of the click that ended the modal. Consumed here, before
    // any stroke or selection handling, and before hover/selection state is
    // touched at all - so it changes nothing and starts nothing.
    if (m_swallow_left_up && (mouse_event.LeftUp() || mouse_event.LeftDClick())) {
        m_swallow_left_up = false;
        m_parent.set_as_dirty();
        return true;
    }
    if (m_swallow_right_up && (mouse_event.RightUp() || mouse_event.RightDClick())) {
        m_swallow_right_up = false;
        m_parent.set_as_dirty();
        return true;
    }
    // A fresh press, or the pointer leaving the canvas, means the owed up is
    // never coming (wx dropped it, focus moved, ...): drop the debt rather than
    // swallowing an unrelated click much later. A plain Moving() does NOT clear
    // it - wx interleaves motion between the down and the up of one click.
    if (mouse_event.LeftDown() || mouse_event.RightDown() || mouse_event.MiddleDown() || mouse_event.Leaving()) {
        m_swallow_left_up  = false;
        m_swallow_right_up = false;
    }

    if (mouse_event.Moving()) {
        // Ctrl on hover already recolours the cursor, so the inverted state is
        // visible before the stroke starts rather than only during it.
        m_ctrl_inverted = brush_inverted(control_down);
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return false;
    }

    if (mouse_event.LeftDown()) {
        if (get_hover_id() != -1)
            return false;
        m_ctrl_inverted = brush_inverted(control_down);
        // Ctrl is the brush-invert modifier for Inflate/Flatten/Crease/Pinch/
        // Clay/Mask, so it must NOT fall through to the canvas (which reads
        // Ctrl+click as an additive selection) for those brushes. For Grab,
        // Smooth, Nudge and Snake Hook, where Ctrl means nothing to the brush,
        // the old behaviour stands and the canvas keeps the click.
        //
        // Note, same path as the adjust-mode swallow above: a Ctrl-held click on
        // an INVERTING brush is fully ours - this LeftDown returns true and the
        // matching LeftUp is taken by the m_stroke_active branch below, so the
        // canvas sees neither half and the selection is untouched. For a
        // NON-inverting brush the click is deliberately handed to the canvas
        // whole (both halves), which is what makes Ctrl+click still add to the
        // selection while Grab is the active brush. The two cases are exclusive;
        // there is no path where only one half of a Ctrl click escapes.
        if (control_down && ! m_ctrl_inverted)
            return false;
        if (start_stroke(mouse_pos, mouse_event.ShiftDown(), control_down)) {
            update_cursor(mouse_pos);
            m_parent.set_as_dirty();
            return true;
        }
        update_cursor(mouse_pos);
        return false;
    }
    if (mouse_event.Dragging()) {
        if (!m_stroke_active)
            return false;
        m_ctrl_inverted = brush_inverted(control_down);
        // Ctrl mid-drag used to end the stroke, matching the paint gizmos. Now
        // that Ctrl is the invert modifier it may only do that for the brushes
        // Ctrl does not invert.
        if (control_down && ! m_ctrl_inverted) {
            end_stroke();
            return false;
        }
        continue_stroke(mouse_pos, mouse_event.ShiftDown(), control_down);
        // After the stroke has advanced, so a Grab cursor rides the moved anchor.
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.LeftUp()) {
        if (!m_stroke_active)
            return false;
        end_stroke();
        // The tree was just rebuilt, so this raycast lands on the sculpted
        // surface and the cursor settles onto what the stroke actually made.
        update_cursor(mouse_pos);
        m_parent.set_as_dirty();
        return true;
    }
    if (mouse_event.Leaving()) {
        m_hit_valid = false;
        m_cursor.visible = false;
    }
    return false;
}

// ----------------------------------------------------------------------------
// modal brush adjust (F / Shift+F)
// ----------------------------------------------------------------------------

void GLGizmoSculpt::begin_adjust(Sculpt::AdjustTarget target)
{
    if (m_volume == nullptr || m_stroke_active)
        return;
    const float current = (target == Sculpt::AdjustTarget::Radius) ? m_cursor_radius : m_strength;
    m_adjust = Sculpt::adjust_begin(target, current, m_last_mouse.x());
    m_parent.set_as_dirty();
}

void GLGizmoSculpt::update_adjust(const Vec2d &mouse_position)
{
    if (! m_adjust.active())
        return;
    m_last_mouse = mouse_position;
    if (m_adjust.target == Sculpt::AdjustTarget::Radius) {
        m_adjust = Sculpt::adjust_move(m_adjust, mouse_position.x(), CursorRadiusMin, CursorRadiusMax);
        // Live, so the panel input and the on-screen circle both show it as it
        // changes - that is the whole point of the modal.
        m_cursor_radius = m_adjust.value;
    } else {
        m_adjust = Sculpt::adjust_move(m_adjust, mouse_position.x(), StrengthMin, 1.f);
        m_strength = m_adjust.value;
    }
}

void GLGizmoSculpt::end_adjust(bool confirm)
{
    if (! m_adjust.active())
        return;
    const float value = confirm ? Sculpt::adjust_confirm(m_adjust) : Sculpt::adjust_cancel(m_adjust);
    if (m_adjust.target == Sculpt::AdjustTarget::Radius)
        m_cursor_radius = std::clamp(value, CursorRadiusMin, CursorRadiusMax);
    else
        m_strength = std::clamp(value, StrengthMin, 1.f);
    m_adjust = Sculpt::AdjustState{};
    m_parent.set_as_dirty();
}

// Called from GLGizmosManager::on_char while Sculpt is the current gizmo, before
// its handle_shortcut() fallthrough - otherwise a bare F would open the "place
// face on bed" gizmo instead of sizing the brush. Returns true when the key was
// consumed. ImGui gets first refusal upstream, so a typed F in the numeric input
// boxes never reaches here.
bool GLGizmoSculpt::on_sculpt_char(int key_code, bool shift_down, bool ctrl_down)
{
    if (m_state != On || m_volume == nullptr)
        return false;

    if (m_adjust.active()) {
        if (key_code == WXK_RETURN || key_code == WXK_NUMPAD_ENTER) {
            end_adjust(/* confirm */ true);
            return true;
        }
        if (key_code == WXK_ESCAPE) {
            end_adjust(/* confirm */ false);
            return true;
        }
        // Anything else is swallowed: the modal owns the keyboard until it ends.
        return true;
    }

    if (key_code == 'f' || key_code == 'F') {
        begin_adjust(shift_down ? Sculpt::AdjustTarget::Strength : Sculpt::AdjustTarget::Radius);
        return true;
    }

    // Number keys pick a brush, in the dropdown's own order. Bare 1-7 are the
    // canvas's camera-view shortcuts (GLCanvas3D::on_char), so taking them here
    // overrides those while this gizmo is open - the same deliberate override
    // bare F already makes against "place face on bed", and the same one Blender
    // makes (a number in sculpt mode is a brush, not a view). Ctrl+1..7 keeps
    // the camera views, so nothing is lost; only the unmodified key is claimed,
    // and only while Sculpt is the current gizmo.
    if (! shift_down && ! ctrl_down) {
        int index = -1;
        if (key_code >= '1' && key_code <= '9')
            index = key_code - '1';           // 1..9 -> brushes 0..8
        else if (key_code == '0')
            index = 9;                        // 0 -> the tenth
        else if (key_code == '-')
            index = 10;                       // - -> the eleventh (Mask)
        if (index >= 0 && index < BrushCount) {
            m_brush = Brush(index);
            m_parent.set_as_dirty();
            return true;
        }
    }
    return false;
}

bool GLGizmoSculpt::gizmo_event(SLAGizmoEventType action, const Vec2d & /* mouse_position */, bool /* shift_down */, bool /* alt_down */, bool control_down)
{
    if (action == SLAGizmoEventType::MouseWheelUp || action == SLAGizmoEventType::MouseWheelDown) {
        if (!control_down)
            return false;
        m_cursor_radius = (action == SLAGizmoEventType::MouseWheelDown)
                              ? std::max(m_cursor_radius - CursorRadiusStep, CursorRadiusMin)
                              : std::min(m_cursor_radius + CursorRadiusStep, CursorRadiusMax);
        // The panel reads m_cursor_radius straight out of the member every frame,
        // so the slider and its input box follow the wheel with no extra plumbing;
        // this redraw is what makes both the panel and the sphere show it at once.
        m_parent.set_as_dirty();
        return true;
    }
    if (action == SLAGizmoEventType::Escape) {
        if (m_adjust.active()) {
            end_adjust(/* confirm */ false);
            return true;
        }
        if (m_stroke_active) {
            cancel_stroke();
            m_parent.set_as_dirty();
            return true;
        }
    }
    return false;
}

// ----------------------------------------------------------------------------
// rendering
// ----------------------------------------------------------------------------

void GLGizmoSculpt::refresh_render_volumes(const std::vector<uint32_t> &dirty_triangles, bool force)
{
    if (m_object_idx < 0 || m_volume_idx < 0 || !m_session)
        return;

    // Accumulate for the fallback path, which redraws everything it was told
    // about since the last rebuild.
    m_stroke_dirty_triangles.insert(m_stroke_dirty_triangles.end(), dirty_triangles.begin(), dirty_triangles.end());

    GLVolumeCollection &volumes = m_parent.get_volumes();
    bool partial_ok = true;
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        if (!v->model.update_triangles_in_place(m_session->mesh(), dirty_triangles))
            partial_ok = false;
    }
    m_partial_gpu_update = partial_ok;

    if (partial_ok) {
        m_stroke_dirty_triangles.clear();
        return;
    }

    // Fallback: rebuild the whole GLModel, throttled.
    const int64_t now = m_parent.timestamp_now();
    if (!force && now < m_last_render_refresh + RenderRebuildIntervalMs)
        return;
    m_last_render_refresh = now;
    for (GLVolume *v : volumes.volumes) {
        if (v == nullptr || v->composite_id.object_id != m_object_idx || v->composite_id.volume_id != m_volume_idx)
            continue;
        v->model.reset();
        v->model.init_from(m_session->mesh());
    }
    m_stroke_dirty_triangles.clear();
}

void GLGizmoSculpt::render_cursor_sphere() const
{
    // While the F modal runs the mouse may well be off the part, but the whole
    // point of the gesture is watching the circle resize - so keep it on screen
    // at wherever it last was.
    if ((!m_cursor.visible && !m_adjust.active()) || m_volume == nullptr)
        return;

    if (s_cursor_sphere == nullptr) {
        s_cursor_sphere = std::make_shared<GLModel>();
        s_cursor_sphere->init_from(its_make_sphere(1.0, double(PI) / 12.0));
    }

    GLShaderProgram *shader = wxGetApp().get_shader("flat");
    if (shader == nullptr)
        return;

    const Transform3d trafo = volume_trafo();
    const Transform3d scaling_inverse = Geometry::Transformation(trafo).get_scaling_factor_matrix().inverse();

    // The paint gizmos' cursor colours (GLGizmoPainterBase::get_cursor_hover_color
    // and get_cursor_sphere_left_button_color), so the brush reads the same here.
    // Inverted (Ctrl held on a brush Ctrl inverts) it goes red, which is the only
    // on-screen indication that the next stroke will run the other way. While the
    // F modal is sizing the brush the circle goes amber, so the sphere the user is
    // watching grow is visibly the thing being changed.
    ColorRGBA color = m_stroke_active ? ColorRGBA(0.0f, 0.0f, 1.0f, 0.25f) : ColorRGBA(0.0f, 0.0f, 0.0f, 0.25f);
    if (m_adjust.active())
        color = ColorRGBA(1.0f, 0.6f, 0.0f, 0.25f);
    else if (m_ctrl_inverted)
        color = ColorRGBA(0.9f, 0.1f, 0.1f, 0.25f);

    shader->start_using();
    const Camera &camera = wxGetApp().plater()->get_camera();
    const Transform3d view_model_matrix = camera.get_view_matrix() * trafo *
                                          Geometry::assemble_transform(m_cursor.position.cast<double>()) * scaling_inverse *
                                          Geometry::assemble_transform(Vec3d::Zero(), Vec3d::Zero(), m_cursor_radius * Vec3d::Ones());
    shader->set_uniform("view_model_matrix", view_model_matrix);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());

    const bool is_left_handed = Geometry::Transformation(view_model_matrix).is_left_handed();
    if (is_left_handed)
        glsafe(::glFrontFace(GL_CW));

    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    s_cursor_sphere->set_color(color);
    s_cursor_sphere->render();
    glsafe(::glDisable(GL_BLEND));

    if (is_left_handed)
        glsafe(::glFrontFace(GL_CCW));

    shader->stop_using();
}

// The mask, tinted onto the mesh the way the paint gizmos tint enforcers and
// blockers: a GLModel of just the masked triangles, offset a hair along their
// normals so it does not z-fight with the part underneath, drawn with the same
// "gouraud" shader the paint overlay uses.
//
// The mask is per-VERTEX, so "a masked triangle" is one all three of whose
// vertices are masked. That draws the interior of a masked region and leaves its
// boundary triangles untinted, which reads as a soft edge rather than a hard
// facet-aligned one - honest, since the mask itself is a continuous weight.
void GLGizmoSculpt::render_mask_overlay() const
{
    if (!m_session || m_volume == nullptr || !m_session->any_mask())
        return;

    // Rebuilt whenever the mask or the mesh may have changed. A stroke can move
    // vertices under the mask, so this is not cacheable across ticks without a
    // dirty flag; the masked set is small (a bed face, a few sharp rings) so
    // rebuilding it is cheaper than the bookkeeping would be.
    const indexed_triangle_set &its = m_session->mesh();
    GLModel::Geometry data;
    data.format = {GLModel::Geometry::EPrimitiveType::Triangles, GLModel::Geometry::EVertexLayout::P3N3};
    static const float offset = 0.02f;
    unsigned int cnt = 0;
    for (const Vec3i32 &f : its.indices) {
        const uint32_t a = uint32_t(f(0)), b = uint32_t(f(1)), c = uint32_t(f(2));
        if (m_session->effective_mask(a) > 0.f || m_session->effective_mask(b) > 0.f || m_session->effective_mask(c) > 0.f)
            continue;
        const Vec3f &v0 = its.vertices[a];
        const Vec3f &v1 = its.vertices[b];
        const Vec3f &v2 = its.vertices[c];
        const Vec3f  n  = (v1 - v0).cross(v2 - v1).normalized();
        const Vec3f  on = offset * n;
        data.add_vertex(Vec3f(v0 + on), n);
        data.add_vertex(Vec3f(v1 + on), n);
        data.add_vertex(Vec3f(v2 + on), n);
        data.add_triangle(cnt, cnt + 1, cnt + 2);
        cnt += 3;
    }
    if (data.is_empty())
        return;

    GLShaderProgram *shader = wxGetApp().get_shader("gouraud");
    if (shader == nullptr)
        return;
    shader->start_using();

    const Camera      &camera = wxGetApp().plater()->get_camera();
    const Transform3d  trafo  = volume_trafo();
    const Transform3d  view   = camera.get_view_matrix();
    shader->set_uniform("view_model_matrix", view * trafo);
    shader->set_uniform("projection_matrix", camera.get_projection_matrix());
    shader->set_uniform("view_normal_matrix",
                        (Matrix3d) (view.matrix().block(0, 0, 3, 3) * trafo.matrix().block(0, 0, 3, 3).inverse().transpose()));
    // The paint gizmos' clipping-plane uniforms still have to be fed even though
    // Sculpt asks for no ObjectClipper: the shader reads them unconditionally.
    shader->set_uniform("clipping_plane", std::array<float, 4>{0.f, 0.f, 0.f, 0.f});
    shader->set_uniform("z_range", std::array<float, 2>{-FLT_MAX, FLT_MAX});
    shader->set_uniform("slope.actived", false);
    shader->set_uniform("volume_world_matrix", trafo);
    shader->set_uniform("volume_mirrored", false);

    GLModel model;
    model.init_from(std::move(data));
    // A cool blue-grey, distinct from the paint gizmos' orange/blue and from the
    // brush cursor's own blue, so "protected" never reads as "painted support".
    model.set_color(ColorRGBA(0.35f, 0.55f, 0.80f, 0.55f));
    glsafe(::glEnable(GL_BLEND));
    glsafe(::glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA));
    glsafe(::glDisable(GL_CULL_FACE));
    model.render();
    glsafe(::glEnable(GL_CULL_FACE));
    glsafe(::glDisable(GL_BLEND));

    shader->stop_using();
}

void GLGizmoSculpt::on_render()
{
    if (m_volume == nullptr)
        return;
    glsafe(::glEnable(GL_DEPTH_TEST));
    render_mask_overlay();
    render_cursor_sphere();
}

// ----------------------------------------------------------------------------
// subdivision
// ----------------------------------------------------------------------------

size_t GLGizmoSculpt::subdivided_triangle_count() const
{
    return m_session ? 4 * m_session->triangles_count() : 0;
}

// Whether the mesh under the brush is too coarse for it.
//
// v1 asked this of the patch under the cursor, so the answer - and with it the
// whole Subdivide row - appeared and vanished as the mouse crossed the part, and
// the panel resized under the pointer so the button could never be clicked. It
// is now asked of the WHOLE mesh (the mean edge length over everything) whenever
// the cursor is off the part, so the row's presence never depends on hover; only
// the number it reports does.
bool GLGizmoSculpt::needs_subdivision()
{
    if (!m_session)
        return false;
    const float radius_mesh = float(double(m_cursor_radius) / mesh_scale());
    const float edge = m_hit_valid ? m_session->local_edge_length(m_hit, radius_mesh)
                                   : its_average_edge_length(m_session->mesh());
    if (edge <= 0.f)
        return false;
    return edge > radius_mesh * m_subdivide_ratio;
}

void GLGizmoSculpt::do_subdivide()
{
    if (!m_session || m_volume == nullptr)
        return;
    if (subdivided_triangle_count() > MaxTrianglesAfterSubdivision)
        return;

    Plater *plater = wxGetApp().plater();
    Plater::TakeSnapshot snapshot(plater, _u8L("Subdivide for sculpting"), UndoRedo::SnapshotType::GizmoAction);
    // A subdivision renumbers every facet, so the painted annotations have to go,
    // exactly the way the Simplify gizmo and "Repair by remeshing" handle it.
    plater->clear_before_change_mesh(m_object_idx);

    // Pin the bed face through the subdivision: every new vertex that is the
    // midpoint of two bed-contact vertices is snapped back to z_min exactly, so
    // the flat bottom stays flat and stays at the same height. Existing vertices
    // are never moved by midpoint subdivision, so they need nothing. A boundary
    // midpoint (one bed endpoint, one not) is on the old edge by construction,
    // so the footprint outline gains a collinear vertex and its area and
    // perimeter are unchanged - see the note in MeshSculpt.hpp.
    const bool pin_bed = m_protect_bed && m_session->bed_contact_count() > 0;
    indexed_triangle_set subdivided =
        pin_bed ? Sculpt::its_subdivide_midpoint(m_session->mesh(), m_session->bed_contact_vertices(), m_session->bed_z_min())
                : Sculpt::its_subdivide_midpoint(m_session->mesh());
    m_volume->set_mesh(std::move(subdivided));
    m_volume->calculate_convex_hull();
    m_volume->invalidate_convex_hull_2d();
    m_volume->set_new_unique_id();
    m_volume->get_object()->invalidate_bounding_box();
    m_volume->get_object()->ensure_on_bed();

    plater->changed_mesh(m_object_idx);
    wxGetApp().obj_list()->update_item_error_icon(m_object_idx, -1);

    if (m_c != nullptr)
        m_c->update(on_get_requirements());
    m_session.reset();
    attach_to_selection();
}

// ----------------------------------------------------------------------------
// v3: brush icons and the dropdown
// ----------------------------------------------------------------------------

// The dropdown's order, which is also the 1..N key order and the Brush enum's.
static const std::array<std::pair<int, const char *>, GLGizmoSculpt::BrushCount> s_brush_rows = {
    std::make_pair(0,  "grab"),
    std::make_pair(1,  "inflate"),
    std::make_pair(2,  "deflate"),
    std::make_pair(3,  "smooth"),
    std::make_pair(4,  "flatten"),
    std::make_pair(5,  "crease"),
    std::make_pair(6,  "pinch"),
    std::make_pair(7,  "nudge"),
    std::make_pair(8,  "snakehook"),
    std::make_pair(9,  "claystrips"),
    std::make_pair(10, "mask")};

void GLGizmoSculpt::ensure_brush_icons()
{
    // Once per gizmo, on the first panel frame - by then a GL context exists,
    // which IMTexture::load_from_svg_file needs and on_init() does not have.
    if (m_brush_icons_tried)
        return;
    m_brush_icons_tried = true;

    // 48 px for a ~24 px slot: the panel is drawn at the user's DPI scale, and a
    // texture rasterised at the nominal size goes soft on a HiDPI display.
    const unsigned px = 48;
    const std::string dir = Slic3r::resources_dir() + "/images/sculpt_brush_";
    for (const auto &row : s_brush_rows) {
        ImTextureID id = nullptr;
        if (IMTexture::load_from_svg_file(dir + row.second + ".svg", px, px, id))
            m_brush_icons[row.first] = id;
        ImTextureID id_dark = nullptr;
        if (IMTexture::load_from_svg_file(dir + row.second + "_dark.svg", px, px, id_dark))
            m_brush_icons_dark[row.first] = id_dark;
    }
}

ImTextureID GLGizmoSculpt::brush_icon(Brush brush) const
{
    // Follow the app theme the way the align icons do: a separate file per
    // theme, since the SVGs are rasterised once and cannot be recoloured after.
    const auto &map = wxGetApp().dark_mode() ? m_brush_icons_dark : m_brush_icons;
    const auto  it  = map.find(int(brush));
    return it == map.end() ? nullptr : it->second;
}

const wxString &GLGizmoSculpt::brush_label(Brush brush) const
{
    const int i = int(brush);
    return m_desc.at(s_brush_rows[size_t(i >= 0 && i < BrushCount ? i : 0)].second);
}

void GLGizmoSculpt::draw_brush_combo(float wrap_width)
{
    ensure_brush_icons();

    const float icon = ImGui::GetFrameHeight();
    const float gap  = ImGui::GetStyle().ItemInnerSpacing.x;

    ImGui::PushItemWidth(wrap_width);
    // BeginCombo's preview is text only, so the icon is drawn over the closed
    // combo afterwards and the label is padded across to leave room for it -
    // ImGui has no "combo with an image in the preview" of its own.
    const wxString  current = brush_label(m_brush);
    const ImTextureID cur_id = brush_icon(m_brush);
    std::string preview = into_u8(current);
    if (cur_id != nullptr)
        // Enough leading spaces to clear the icon; measured, not guessed, so it
        // holds at every DPI scale and font size.
        preview = std::string(size_t(std::ceil(double(icon + gap) / double(std::max(1.f, ImGui::CalcTextSize(" ").x)))), ' ') + preview;

    const ImVec2 combo_pos = ImGui::GetCursorScreenPos();
    if (ImGui::BBLBeginCombo("##sculpt_brush", preview.c_str(), 0)) {
        for (const auto &row : s_brush_rows) {
            const Brush       b  = Brush(row.first);
            const ImTextureID id = brush_icon(b);
            ImGui::PushID(row.first);
            const ImVec2 row_pos = ImGui::GetCursorScreenPos();
            std::string  label   = into_u8(m_desc.at(row.second));
            if (id != nullptr)
                label = std::string(size_t(std::ceil(double(icon + gap) / double(std::max(1.f, ImGui::CalcTextSize(" ").x)))), ' ') + label;
            if (ImGui::Selectable(label.c_str(), m_brush == b))
                m_brush = b;
            // A brush whose texture failed to load simply has no icon and its
            // text sits where every other row's does - the panel degrades, it
            // does not break.
            if (id != nullptr)
                ImGui::GetWindowDrawList()->AddImage(id, ImVec2(row_pos.x, row_pos.y),
                                                     ImVec2(row_pos.x + icon, row_pos.y + icon));
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::PopItemWidth();

    if (cur_id != nullptr) {
        const float pad = ImGui::GetStyle().FramePadding.x;
        ImGui::GetWindowDrawList()->AddImage(cur_id, ImVec2(combo_pos.x + pad, combo_pos.y),
                                             ImVec2(combo_pos.x + pad + icon, combo_pos.y + icon));
    }
}

// ----------------------------------------------------------------------------
// v3: the mask
// ----------------------------------------------------------------------------

void GLGizmoSculpt::refresh_masks()
{
    if (!m_session)
        return;
    // Both detections are pure functions of the current mesh, so re-running them
    // whenever the mesh may have changed is both correct and cheap - there is no
    // staleness to reason about. A hand-painted mask is NOT recomputed and is
    // deliberately kept across a commit (the vertex numbering is unchanged); a
    // Subdivide throws the session away and takes the painted mask with it,
    // which is the honest thing to do since the vertex ids are gone.
    m_session->detect_bed_contact();
    if (m_protect_sharp)
        m_session->detect_sharp_edges(m_sharp_dihedral);
    m_session->set_bed_contact_enabled(m_protect_bed && m_session->bed_contact_count() > 0);
    m_session->set_sharp_edge_enabled(m_protect_sharp);
}

// ----------------------------------------------------------------------------
// the panel
// ----------------------------------------------------------------------------

void GLGizmoSculpt::on_render_input_window(float x, float y, float bottom_limit)
{
    if (!m_c->selection_info() || !m_c->selection_info()->model_object())
        return;

    // v1 let the window auto-size, so the panel's width and height changed with
    // its contents - and since the Subdivide row only existed while the cursor
    // was over the part, the panel resized out from under the pointer the moment
    // the user moved toward the button. A fixed width, and a Subdivide row that
    // is always present, are the two halves of the fix. The width is the paint
    // gizmos' order of magnitude, expressed in the same scaled units so it
    // survives DPI scaling.
    const float window_width  = m_imgui->scaled(16.0f);
    const float approx_height = m_imgui->scaled(20.f);
    y = std::min(y, bottom_limit - approx_height);

    // The toolbar hands us the x of the gizmo's own icon, and Sculpt is the LAST
    // icon on the bar, so a panel drawn rightwards from there hangs off the end
    // of the canvas. GizmoImguiSetNextWIndowPos() already knows how to pull a
    // window back inside - but it clamps against last_input_window_width, i.e.
    // the width of the PREVIOUS frame, which under AlwaysAutoResize was whatever
    // the last frame's contents happened to need. Passing the real width (which
    // is now fixed) makes that clamp exact: the panel opens toward the centre of
    // the bar, its right edge flush with the canvas edge, and it never hangs off.
#if BBS_TOOLBAR_ON_TOP
    GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 0.0f, 0.0f);
#else
    GizmoImguiSetNextWIndowPos(x, y, window_width, 0.f, ImGuiCond_Always, 1.0f, 0.0f);
#endif
    ImGui::SetNextWindowSize(ImVec2(window_width, 0.f), ImGuiCond_Always);

    ImGuiWrapper::push_toolbar_style(m_parent.get_scale());
    // No AlwaysAutoResize: the width is pinned above, and only the height is left
    // to the contents.
    GizmoImguiBegin(get_name(), ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoTitleBar);

    // Everything the panel wraps to. Inside the window, so the padding is known.
    const float wrap_width = ImGui::GetContentRegionAvail().x;

    if (m_volume == nullptr || !m_session) {
        m_imgui->text_wrapped(m_desc.at("no_part"), wrap_width);
        GizmoImguiEnd();
        ImGuiWrapper::pop_toolbar_style();
        return;
    }

    // Lay the two sliders out the way the paint gizmos do: the label column is as
    // wide as the widest label plus a gap, the slider sits at a fixed offset, and
    // a small drag-input box for typing an exact value goes at the end of the row.
    // scaled(1.5f) after the text is the gap that keeps the labels off the track -
    // without it the panel reads as cramped, which is what the v1 panel did.
    const float space_size     = m_imgui->get_style_scaling() * 8;
    const float radius_label   = m_imgui->calc_text_size(m_desc.at("radius")).x + m_imgui->scaled(1.5f);
    const float strength_label = m_imgui->calc_text_size(m_desc.at("strength")).x + m_imgui->scaled(1.5f);
    const float sliders_left   = std::max(radius_label, strength_label);
    const float slider_icon_width = m_imgui->get_slider_icon_size().x;
    // The window is fixed-width now, so the slider takes what is left after the
    // label column and the numeric box rather than a hardcoded width - otherwise
    // the row would overflow or leave a gap depending on the translation.
    const float sliders_width  = std::max(m_imgui->scaled(3.0f),
                                          wrap_width - sliders_left - 1.5f * slider_icon_width - space_size);
    const float drag_left      = ImGui::GetStyle().WindowPadding.x + sliders_left + sliders_width - space_size;

    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("brush"));

    // v1.1's wrapping radio row held six brushes and only just fitted the
    // fixed-width panel; v3 has eleven, which would take four lines and push the
    // sliders off the bottom. A combo takes one line whatever the count, and its
    // entries carry an icon each - a sphere with the deformation drawn on it, so
    // the shape of the brush is readable before its name is.
    draw_brush_combo(wrap_width);

    ImGui::Separator();

    // Brush size, in world millimetres - the slider prints the unit, the box at
    // the end takes an exact figure.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("radius"));
    ImGui::SameLine(sliders_left);
    ImGui::PushItemWidth(sliders_width);
    m_imgui->bbl_slider_float_style("##sculpt_radius", &m_cursor_radius, CursorRadiusMin, CursorRadiusMax, "%.2f mm", 1.0f, true);
    ImGui::SameLine(drag_left);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    ImGui::BBLDragFloat("##sculpt_radius_input", &m_cursor_radius, 0.05f, 0.0f, 0.0f, "%.2f");
    // BBLDragFloat does not clamp, so a typed figure has to be brought back into
    // the range the brush and the Ctrl+wheel step both honour.
    m_cursor_radius = std::clamp(m_cursor_radius, CursorRadiusMin, CursorRadiusMax);

    // Strength is the dimensionless 0.05-1 multiplier from BrushParams, shown as
    // a percentage because that is what it reads as to a user.
    ImGui::AlignTextToFramePadding();
    m_imgui->text(m_desc.at("strength"));
    ImGui::SameLine(sliders_left);
    ImGui::PushItemWidth(sliders_width);
    float strength_pct = m_strength * 100.f;
    if (m_imgui->bbl_slider_float_style("##sculpt_strength", &strength_pct, StrengthMin * 100.f, 100.f, "%.0f%%", 1.0f, true))
        m_strength = strength_pct / 100.f;
    ImGui::SameLine(drag_left);
    ImGui::PushItemWidth(1.5f * slider_icon_width);
    if (ImGui::BBLDragFloat("##sculpt_strength_input", &strength_pct, 1.0f, 0.0f, 0.0f, "%.0f"))
        m_strength = strength_pct / 100.f;
    m_strength = std::clamp(m_strength, StrengthMin, 1.f);

    m_imgui->bbl_checkbox(m_desc.at("falloff"), m_falloff);
    if (m_brush == Brush::Smooth)
        m_imgui->bbl_checkbox(m_desc.at("taubin"), m_taubin);

    // Per-brush extras, on the same slider metrics as the two above so the rows
    // line up whichever brush is selected.
    if (m_brush == Brush::ClayStrips) {
        // As a fraction of the brush radius, so a Clay stroke builds the same
        // relative amount of material at any brush size - Inflate's `amount`
        // trick, exposed because the plateau height is the brush's whole point.
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("clay_offset"));
        ImGui::SameLine(sliders_left);
        ImGui::PushItemWidth(sliders_width);
        float clay_pct = m_clay_offset_ratio * 100.f;
        if (m_imgui->bbl_slider_float_style("##sculpt_clay", &clay_pct, 1.f, 50.f, "%.0f%%", 1.0f, true))
            m_clay_offset_ratio = clay_pct / 100.f;
        m_clay_offset_ratio = std::clamp(m_clay_offset_ratio, 0.01f, 0.5f);
    }
    if (m_brush == Brush::Mask) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("mask_strength"));
        ImGui::SameLine(sliders_left);
        ImGui::PushItemWidth(sliders_width);
        float mask_pct = m_mask_strength * 100.f;
        if (m_imgui->bbl_slider_float_style("##sculpt_mask_strength", &mask_pct, 5.f, 100.f, "%.0f%%", 1.0f, true))
            m_mask_strength = mask_pct / 100.f;
        m_mask_strength = std::clamp(m_mask_strength, 0.05f, 1.f);
        m_imgui->text_wrapped(m_desc.at("mask_hint"), wrap_width);
    }

    ImGui::Separator();

    // --- the mask (v3, phase 3b) ---
    //
    // Session-only by design: the two auto sources are recomputed from the mesh
    // every time the gizmo attaches, so they can never go stale, and a
    // hand-painted mask deliberately does not survive the gizmo closing -
    // persisting it would mean a new FacetsAnnotation-style store on
    // ModelVolume, a 3MF schema change and a migration, which is real scope.
    const bool has_bed = m_session->bed_contact_count() > 0;
    m_imgui->disabled_begin(! has_bed);
    bool protect_bed = m_protect_bed;
    if (m_imgui->bbl_checkbox(m_desc.at("protect_bed"), protect_bed)) {
        m_protect_bed = protect_bed;
        refresh_masks();
    }
    m_imgui->disabled_end();
    // The copy says "the flat area touching the bed", not "the footprint": on a
    // filleted or chamfered bottom edge the sloped ring of triangles is not
    // bed-normal, so it is not protected, and those two are different things.
    m_imgui->text_wrapped(has_bed ? m_desc.at("protect_bed_hint") : m_desc.at("protect_bed_none"), wrap_width);

    bool protect_sharp = m_protect_sharp;
    if (m_imgui->bbl_checkbox(m_desc.at("protect_sharp"), protect_sharp)) {
        m_protect_sharp = protect_sharp;
        refresh_masks();
    }
    if (m_protect_sharp) {
        ImGui::AlignTextToFramePadding();
        m_imgui->text(m_desc.at("sharp_angle"));
        ImGui::SameLine(sliders_left);
        ImGui::PushItemWidth(sliders_width);
        float dihedral = m_sharp_dihedral;
        if (m_imgui->bbl_slider_float_style("##sculpt_sharp", &dihedral, 10.f, 170.f, "%.0f deg", 1.0f, true)) {
            m_sharp_dihedral = std::clamp(dihedral, 10.f, 170.f);
            refresh_masks();
        }
    }

    if (m_session->has_painted_mask() && m_imgui->button(m_desc.at("clear_mask"))) {
        m_session->clear_painted_mask();
        m_parent.set_as_dirty();
    }

    ImGui::Separator();

    // One line, always: the count may change as the cursor moves, the layout
    // must not.
    m_imgui->text(GUI::format_wxstr(_L("%1% triangles"), m_session->triangles_count()));

    // While a modal is running, say so where the user is already looking.
    if (m_adjust.active())
        m_imgui->text_wrapped(m_adjust.target == Sculpt::AdjustTarget::Radius ? m_desc.at("adjusting_radius")
                                                                             : m_desc.at("adjusting_strength"),
                              wrap_width);
    else if (m_ctrl_inverted)
        m_imgui->text_wrapped(m_desc.at("inverted"), wrap_width);
    else
        m_imgui->text_wrapped(m_desc.at("paint_kept"), wrap_width);

    ImGui::Separator();

    // The Subdivide row is ALWAYS here - the note above it changes, the row does
    // not appear and disappear. v1 drew it only while the cursor was over the
    // part, so the panel resized the instant the pointer left and the button was
    // unreachable.
    const bool   needs   = needs_subdivision();
    const size_t after   = subdivided_triangle_count();
    const bool   too_big = after > MaxTrianglesAfterSubdivision;

    if (needs && ! too_big)
        m_imgui->text_wrapped(m_desc.at("subdivide_hint") + " " + m_desc.at("subdivide_warning"), wrap_width);
    else if (needs && too_big)
        m_imgui->text_wrapped(GUI::format_wxstr(_L("Subdividing would exceed %1% triangles."), MaxTrianglesAfterSubdivision), wrap_width);
    else
        m_imgui->text_wrapped(m_desc.at("subdivide_fine"), wrap_width);

    // Enabled whenever a subdivision is actually possible, which is the honest
    // condition: a mesh that is already fine enough can still be subdivided, and
    // the user may want to. Only the triangle cap disables it.
    m_imgui->disabled_begin(too_big);
    if (m_imgui->button(GUI::format_wxstr(_L("Subdivide to %1% triangles"), after)) && ! too_big)
        wxGetApp().CallAfter([this]() { do_subdivide(); });
    m_imgui->disabled_end();

    // Ultra: Phase 2 - the second entry point into the quad remesher, as the spec
    // asks. It belongs next to Subdivide because that is what it is FOR: Subdivide
    // splits whatever triangles the part happens to have, so an uneven mesh stays
    // uneven and the brush bites differently in different places. Quad remeshing
    // first gives it an even grid to work on.
    //
    // It hands over to the object-list action rather than remeshing in place: the mesh
    // swap invalidates every mask and cache the open session holds, and
    // ObjectList::quad_remesh already does the snapshot, the painted-data clearing and
    // the notification properly.
    //
    // Sculpt has to be closed first, since quad_remesh() declines while a gizmo is
    // open - and closing it is the manager's job. A gizmo .cpp in this tree
    // deliberately does not include GLGizmosManager.hpp (only the manager itself and
    // GizmoObjectManipulation do), so the close happens on the other side, inside
    // quad_remesh(), which already holds a reference to the manager. Deferred, so it
    // does not run inside this ImGui frame.
    if (quad_remesh_available()) {
        ImGui::Separator();
        m_imgui->text_wrapped(_L("Rebuild the part as an even grid of quads first, so the brush and "
                                 "Subdivide behave the same everywhere. Closes Sculpt."), wrap_width);
        if (m_imgui->button(_L("Quad remesh...")))
            wxGetApp().CallAfter([]() { wxGetApp().obj_list()->quad_remesh(/*close_gizmos*/ true); });
    }

    GizmoImguiEnd();
    ImGuiWrapper::pop_toolbar_style();
}

} // namespace Slic3r::GUI
