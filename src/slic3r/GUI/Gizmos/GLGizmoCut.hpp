#ifndef slic3r_GLGizmoCut_hpp_
#define slic3r_GLGizmoCut_hpp_

#include "GLGizmoBase.hpp"
#include "slic3r/GUI/GLSelectionRectangle.hpp"
#include "slic3r/GUI/GLModel.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "FacetPicker.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/CutUtils.hpp"
#include "libslic3r/CurvedCut.hpp"
#include "imgui/imgui.h"

namespace Slic3r {

enum class CutConnectorType : int;
class ModelVolume;
class GLShaderProgram;
struct CutConnectorAttributes;

namespace GUI {
class Selection;

enum class SLAGizmoEventType : unsigned char;

namespace CommonGizmosDataObjects { class ObjectClipper; }

class GLGizmoCut3D : public GLGizmoBase
{
    enum GrabberID {
        X = 0,
        Y,
        Z,
        CutPlane,
        CutPlaneZRotation,
        CutPlaneXMove,
        CutPlaneYMove,
        Count,
    };

    Transform3d                 m_rotation_m{ Transform3d::Identity() };
    double                      m_snap_step{ 1.0 };
    int                         m_connectors_group_id;

    // archived values 
    Vec3d m_ar_plane_center { Vec3d::Zero() };
    Transform3d m_start_dragging_m{ Transform3d::Identity() };

    Vec3d m_plane_center{ Vec3d::Zero() };
    // data to check position of the cut palne center on gizmo activation
    Vec3d m_min_pos{ Vec3d::Zero() };
    Vec3d m_max_pos{ Vec3d::Zero() };
    Vec3d m_bb_center{ Vec3d::Zero() };
    Vec3d m_center_offset{ Vec3d::Zero() };

    BoundingBoxf3 m_bounding_box;
    BoundingBoxf3 m_transformed_bounding_box;

    // Pick-face mode: click a triangular facet of the model to set the cut plane flush
    // with it (ported from BambuStudio #12048, re-targeted onto this Orca gizmo).
    FacetPicker m_facet_picker;

    // values from RotationGizmo
    double m_radius{ 0.0 };
    double m_grabber_radius{ 0.0 };
    double m_grabber_connection_len{ 0.0 };
    Vec3d  m_cut_plane_start_move_pos {Vec3d::Zero()};

    double m_snap_coarse_in_radius{ 0.0 };
    double m_snap_coarse_out_radius{ 0.0 };
    double m_snap_fine_in_radius{ 0.0 };
    double m_snap_fine_out_radius{ 0.0 };

    // dragging angel in hovered axes
    double m_angle{ 0.0 };

    TriangleMesh    m_connector_mesh;
    // workaround for using of the clipping plane normal
    Vec3d           m_clp_normal{ Vec3d::Ones() };

    Vec3d           m_line_beg{ Vec3d::Zero() };
    Vec3d           m_line_end{ Vec3d::Zero() };

    Vec2d           m_ldown_mouse_position{ Vec2d::Zero() };

    GLModel m_grabber_connection;
    GLModel m_cut_line;

    PickingModel m_plane;
    PickingModel m_sphere;
    PickingModel m_cone;
    PickingModel m_cube;
    std::map<CutConnectorAttributes, PickingModel> m_shapes;
    std::vector<std::shared_ptr<SceneRaycasterItem>> m_raycasters;

    GLModel m_circle;
    GLModel m_scale;
    GLModel m_snap_radii;
    GLModel m_reference_radius;
    GLModel m_angle_arc;

    Vec3d   m_old_center;
    Vec3d   m_cut_normal;

    struct InvalidConnectorsStatistics
    {
        unsigned int    outside_cut_contour;
        unsigned int    outside_bb;
        bool            is_overlap;

        void invalidate() {
            outside_cut_contour = 0;
            outside_bb = 0;
            is_overlap = false;
        } 
    } m_info_stats;

    bool m_keep_upper{ true };
    bool m_keep_lower{ true };
    bool m_keep_as_parts{ false };
    bool m_place_on_cut_upper{ true };
    bool m_place_on_cut_lower{ false };
    bool m_rotate_upper{ false };
    bool m_rotate_lower{ false };

    // Input params for cut with tongue and groove
    Cut::Groove m_groove;
    bool m_groove_editing { false };

    bool m_is_slider_editing_done { false };

    // Input params for cut with snaps
    float m_snap_bulge_proportion{ 0.15f };
    float m_snap_space_proportion{ 0.3f };

    // Input params for the Flexi joint connector family (print-in-place articulated joint).
    FlexiJointParams m_flexi;
    int              m_flexi_kind_id{ int(FlexiJointKind::DoubleRing) };
    bool             m_flexi_auto_size{ true };
    // Hinge: keep the barrel parked on the cut face's edge as the size and rotation change.
    bool             m_flexi_hinge_auto_edge{ true };
    // Set when a cut used a Flexi joint, so the next time the gizmo opens the after-cut
    // state that the flexi path forced (keep-as-parts, both Keep flags, the connector type)
    // is put back to its defaults instead of staying stuck - which used to grey out
    // "Add connectors" for the rest of the session.
    bool             m_flexi_forced_after_cut{ false };
    std::vector<std::string> m_flexi_kinds;

    // --- Curved cut (phase 1) ---------------------------------------------
    // Surface: Flat | Curved. Curved replaces the flat cut plane with a height
    // field z = f(u,v) defined over it by a coarse control grid, upsampled to a
    // dense sheet for the preview and for the cut. Nothing here is persisted:
    // the cut is baked, exactly as a plane cut is, and the grid lives only for
    // the gizmo session.
    bool           m_curved_surface{ false };
    CurvedCutSheet m_curved_sheet;
    // The control grid is nx COLUMNS by ny ROWS, kept apart so a RULED bend
    // (10 x 2: every column one straight line, grabbable from either end) is
    // expressible. m_curved_square locks the two together and is ON by default,
    // so the panel behaves exactly as the single "Control points" slider did.
    int            m_curved_nx{ CurvedCutSheet::DefaultResolution };
    int            m_curved_ny{ CurvedCutSheet::DefaultResolution };
    bool           m_curved_square{ true };
    // Brush radius for the control-point grab, in world mm, and whether the
    // Sculpt gizmo's falloff applies. F / Shift+F adjust it the way Sculpt does.
    float          m_curved_brush_radius{ 0.f }; // 0 = take default_curved_bend_radius() on first use
    float          default_curved_bend_radius() const;
    bool           m_curved_falloff{ true };
    // The dense sheet, rebuilt whenever the grid changes.
    GLModel        m_curved_sheet_model;
    bool           m_curved_sheet_dirty{ true };
    // Drag state: which control point is under the cursor / being dragged.
    int            m_curved_hover_ctl{ -1 };
    int            m_curved_drag_ctl{ -1 };
    Vec3d          m_curved_drag_anchor_world{ Vec3d::Zero() };
    // The control grid as it stood when the drag started: a drag is absolute,
    // so every tick re-applies the whole displacement to this snapshot.
    std::vector<double> m_curved_drag_grid;
    bool           m_curved_drag_took_snapshot{ false };

    // --- Curved cut: gizmo-local undo/redo ---------------------------------
    // The sheet is SESSION state. It lives in the gizmo, not in the Model, so
    // the plater's own undo stack cannot restore it: the TakeSnapshot calls that
    // already bracket the sheet edits roll the MODEL back and leave the control
    // grid exactly where it was, which is why Ctrl+Z did nothing to a handle
    // that had been dragged slightly out of place.
    //
    // So the gizmo keeps its own stack, in the shape GLGizmoSculpt uses for a
    // stroke: one entry per COMPLETED edit, pushed at the start of the gesture
    // (drag, snap) or before an instant edit (Smooth, Reset, resolution change,
    // flip), and consumed by Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z while the Cut gizmo
    // is open and the canvas has focus. Keyboard handling goes through the same
    // first-refusal hook Sculpt uses (GLGizmosManager::on_char), so the two
    // gizmos behave the same way and the canvas's own Ctrl+Z is only reached
    // when this stack has nothing to give.
    //
    // An entry carries the extent and BOTH grid counts as well as the values: a
    // grid change or a re-fit alters those, and restoring values against the
    // wrong grid would be meaningless. Both counts, not one: a 10 x 2 and a
    // 2 x 10 hold the same number of values and nothing else would tell them
    // apart, so restoring one onto the other would transpose the surface.
    struct CurvedSheetState {
        std::vector<double> values;
        int                 nx{ 0 };
        int                 ny{ 0 };
        double              half_size_u{ 0.0 };
        double              half_size_v{ 0.0 };
    };
    std::vector<CurvedSheetState> m_curved_undo;
    std::vector<CurvedSheetState> m_curved_redo;
    // Deep history is not the point - a handful of handle nudges is - and each
    // entry is at most MaxResolution^2 doubles.
    static const size_t           CurvedUndoLimit = 64;

    CurvedSheetState curved_sheet_state() const;
    void             apply_curved_sheet_state(const CurvedSheetState& st);
    // Push the CURRENT sheet onto the undo stack and drop the redo branch. Call
    // BEFORE changing the sheet, so the entry is the state to come back to.
    void             push_curved_undo();
    bool             curved_undo();
    bool             curved_redo();
    void             clear_curved_undo() { m_curved_undo.clear(); m_curved_redo.clear(); }

    // --- Curved PREVIEW ----------------------------------------------------
    // The height field, uploaded as a DefaultSamples x DefaultSamples single
    // channel float texture, is what makes the shaded upper/lower split follow
    // the sheet instead of the flat plane. See gouraud.fs (curved_sheet_*).
    unsigned int   m_curved_sheet_tex{ 0 };
    bool           m_curved_sheet_tex_dirty{ true };
    // On the GL 2.1 fallback the only float-ish texture is GL_LUMINANCE, which
    // clamps to [0,1], so f is stored there as (f/range + 1)/2 and decoded in
    // the shader. On 3.0+ it is a plain GL_R32F and neither of these is used.
    bool           m_curved_sheet_encoded{ false };
    double         m_curved_sheet_range{ 1.0 };
    // The cut face (the coloured cross-section). The flat path gets it from
    // MeshClipper, which slices at one z and so can only ever produce a flat
    // cap; for a curved sheet the cap is the sheet itself restricted to the
    // object's interior, sampled here on the same grid the sheet is drawn on.
    GLModel        m_curved_cap_model;
    bool           m_curved_cap_dirty{ true };
    // Hash of everything the cap depends on, so a redraw that changed nothing
    // (camera orbit, hover) does not pay for a rebuild.
    size_t         m_curved_cap_key{ 0 };
    // Set while a control point is being dragged: the cap is not rebuilt then,
    // so the drag stays fluid, and the panel says the face is catching up.
    bool           m_curved_cap_stale{ false };

    // --- Phase 2 -----------------------------------------------------------
    // (1) FIT. The sheet is sized to the cut's own cross-section, not to the
    // object's bounding-box diagonal, so the handles land over the material.
    // The fit is DEBOUNCED to the end of a plane drag: it costs a pass over the
    // instance mesh, and a plane being dragged would pay it every frame.
    bool           m_curved_fit_pending{ false };
    // The plane pose the current fit was computed for, so the fit is redone when
    // (and only when) the plane has actually moved or turned.
    Vec3d          m_curved_fit_center{ Vec3d::Zero() };
    Transform3d    m_curved_fit_rotation{ Transform3d::Identity() };
    bool           m_curved_fit_valid{ false };
    void           fit_curved_sheet_to_section(bool force = false);
    void           request_curved_fit() { m_curved_fit_pending = true; }
    // The instance mesh in the CUT PLANE's frame, which both the fit and the
    // snap need. Empty when there is no selection or no raycaster yet.
    bool           curved_instance_mesh_in_plane(indexed_triangle_set& out) const;

    // (2) SIDE VISIBILITY. Per half: solid, ghosted (translucent, no depth
    // write) or hidden (the shader discards it). Both default to Visible and
    // are reset when the gizmo closes.
    enum class SideVisibility : int { Visible = 0, Ghost = 1, Hidden = 2 };
    SideVisibility m_upper_visibility{ SideVisibility::Visible };
    SideVisibility m_lower_visibility{ SideVisibility::Visible };
    static float   side_visibility_alpha(SideVisibility v);
    void           apply_side_visibility();
    void           render_side_visibility_inputs();

    // (3) SNAP. Right-click-drag on a handle pulls it onto the model surface.
    // While the button is held the handle follows the nearest surface hit under
    // it; release commits. Shift carries the neighbours along with the Bend
    // radius falloff, exactly as a left drag does.
    int            m_curved_snap_ctl{ -1 };
    bool           m_curved_snap_falloff{ false };
    // The last hit, in the plane frame, for the crosshair the gesture draws.
    bool           m_curved_snap_hit_valid{ false };
    Vec3d          m_curved_snap_hit{ Vec3d::Zero() };
    // The instance mesh in the plane frame, cached for the duration of one snap
    // gesture: it does not change while the button is down, and re-deriving it
    // per motion event would stall the drag on a heavy model.
    indexed_triangle_set m_curved_snap_mesh;
    bool           curved_snap_apply(int ctl, bool falloff);
    void           render_curved_snap_marker();

    // --- Phase 3 -----------------------------------------------------------
    // (1) THE FIT COVERS THE WHOLE PART. Phase 2 fitted the sheet to the plane's
    // cross-section; outside the sheet's domain the slab extrudes the RIM height
    // outwards, so a part wider above or below the plane got cut by that rim and
    // a strongly bent sheet could leave one side empty. The extent is now the
    // whole instance mesh projected onto the plane's axes, and the control-grid
    // resolution follows the extent so the handles keep a workable pitch.
    // curved_cut_default_resolution() picks it; the user's own choice wins once
    // they have moved the slider.
    bool           m_curved_res_user_set{ false };

    // (2) THE EMPTY-SIDE WARNING. A sign test of the instance mesh's vertices
    // against the sheet, refreshed with the fit and after every edit, so the
    // panel can say "that side would be empty" before the user commits.
    bool           m_curved_upper_empty{ false };
    bool           m_curved_lower_empty{ false };
    void           update_curved_empty_sides();

    // --- PHASE 4: connectors on the sheet ----------------------------------
    // The sheet as a WORLD-frame triangle mesh, plus a raycaster over it, so a
    // click can be intersected with the surface the user drew rather than with
    // the flat clipping plane. Rebuilt lazily from the same dense sample grid
    // the preview uses (DefaultSamples), so what the user clicks IS what they
    // see. Held by pointer because MeshRaycaster is not default-constructible.
    std::unique_ptr<MeshRaycaster> m_curved_pick_raycaster;
    TriangleMesh   m_curved_pick_mesh;
    bool           m_curved_pick_dirty{ true };
    // Warning counts for the connector panel, refreshed with the connector
    // state. Advisory only: nothing here refuses a cut.
    int            m_curved_tilted_connectors{ 0 };
    int            m_curved_unflat_connectors{ 0 };
    // The flat cut has the same failure mode (a plane clear of the part), and
    // has_valid_contour() already covers it, so this is only computed in Curved.

    // (3) CUT THICKNESS ("kerf"), for BOTH Flat and Curved: a band of material
    // centred on the cut surface is removed, so the two halves come apart with a
    // real gap between them. Session state like everything else in this gizmo -
    // the cut is baked and a plane cut persists nothing today, so there is
    // nothing to write to the 3MF.
    float          m_cut_thickness{ 0.f };
    int            m_cut_thickness_offset{ int(CutThicknessOffset::Centred) };
    CutThicknessOffset cut_thickness_offset() const { return CutThicknessOffset(m_cut_thickness_offset); }
    // The two face offsets the current thickness produces, in mm along the cut
    // normal: lo <= 0 <= hi, hi - lo == thickness.
    void           cut_thickness_faces(double& lo, double& hi) const;
    void           render_cut_thickness_input();

    bool m_hide_cut_plane{ false };
    bool m_connectors_editing{ false };
    bool m_cut_plane_as_circle{ false };

    float m_connector_depth_ratio{ 3.f };
    float m_connector_size{ 2.5f };
    float m_connector_angle{ 0.f };

    float m_connector_depth_ratio_tolerance{ 0.1f };
    float m_connector_size_tolerance{ 0.f };

    float m_label_width{ 0.f };
    float m_control_width{ 200.f };
    double m_editing_window_width;
    bool  m_imperial_units{ false };

    float m_contour_width{ 0.4f };
    float m_cut_plane_radius_koef{ 1.5f };

    mutable std::vector<bool> m_selected; // which pins are currently selected
    int  m_selected_count{ 0 };

    GLSelectionRectangle m_selection_rectangle;

    std::vector<size_t> m_invalid_connectors_idxs;
    bool m_was_cut_plane_dragged { false };
    bool m_was_contour_selected { false };

    // Vertices of the groove used to detection if groove is valid
    std::vector<Vec3d> m_groove_vertices;

    class PartSelection {
    public:
        PartSelection() = default;
        PartSelection(const ModelObject* mo, const Transform3d& cut_matrix, int instance_idx, const Vec3d& center, const Vec3d& normal, const CommonGizmosDataObjects::ObjectClipper& oc);
        PartSelection(const ModelObject* mo, int instance_idx_in);
        ~PartSelection() { m_model.clear_objects(); }

        struct Part {
            GLModel glmodel;
            MeshRaycaster raycaster;
            bool selected;
            bool is_modifier;
        };

        void render(const Vec3d* normal, GLModel& sphere_model);
        void toggle_selection(const Vec2d& mouse_pos);
        void turn_over_selection();
        ModelObject* model_object() { return m_model.objects.front(); }
        bool valid() const { return m_valid; }
        bool is_one_object() const;
        const std::vector<Part>& parts() const { return m_parts; }
        const std::vector<size_t>* get_ignored_contours_ptr() const { return (valid() ? &m_ignored_contours : nullptr); }

        std::vector<Cut::Part> get_cut_parts();

    private:
        Model m_model;
        int m_instance_idx;
        std::vector<Part> m_parts;
        bool m_valid = false;
        std::vector<std::pair<std::vector<size_t>, std::vector<size_t>>> m_contour_to_parts; // for each contour, there is a vector of parts above and a vector of parts below
        std::vector<size_t> m_ignored_contours; // contour that should not be rendered (the parts on both sides will both be parts of the same object)

        std::vector<Vec3d> m_contour_points;         // Debugging
        std::vector<std::vector<Vec3d>> m_debug_pts; // Debugging

        void add_object(const ModelObject* object);
    };

    PartSelection m_part_selection;

    std::vector<std::pair<wxString, wxString>> m_shortcuts_cut;
    std::vector<std::pair<wxString, wxString>> m_shortcuts_connector;

    enum class CutMode {
        cutPlanar
        , cutTongueAndGroove
        //, cutGrig
        //,cutRadial
        //,cutModular
    };

    enum class CutConnectorMode {
        Auto
        , Manual
    };

    std::vector<std::string> m_modes;
    size_t m_mode{ size_t(CutMode::cutPlanar) };

    std::vector<std::string> m_connector_modes;
    CutConnectorMode m_connector_mode{ CutConnectorMode::Manual };

    std::vector<std::string> m_connector_types;
    CutConnectorType m_connector_type;

    std::vector<std::string> m_connector_styles;
    int m_connector_style;

    std::vector<std::string> m_connector_shapes;
    int m_connector_shape_id;

    std::vector<std::string> m_axis_names;

    std::map<std::string, wxString> m_part_orientation_names;

    std::map<std::string, std::string> m_labels_map;

public:
    GLGizmoCut3D(GLCanvas3D& parent, const std::string& icon_filename, unsigned int sprite_id);

    std::string get_tooltip() const override;
    bool unproject_on_cut_plane(const Vec2d& mouse_pos, Vec3d& pos, Vec3d& pos_world, bool respect_contours = true);
    bool gizmo_event(SLAGizmoEventType action, const Vec2d& mouse_position, bool shift_down, bool alt_down, bool control_down);

    // --- bounded cut-surface hit test --------------------------------------
    // Is the mouse ray actually ON the drawn cut surface?
    //
    // The picking mesh registered for CutPlane is the flat plane's frustum at
    // m_cut_plane_radius_koef * m_radius - 1.5x the object's bounding-box half
    // diagonal - so it reaches well past the part and, in Curved mode, has
    // nothing to do with the sheet that is actually drawn. A click on empty
    // canvas that happens to land on that oversized quad started a plane drag
    // and moved the cut by accident while the user was only navigating.
    //
    // This bounds the hit to what is RENDERED: the drawn quad in Flat mode, and
    // the sheet's own rectangular domain (half_size_u/v, at the sheet's actual
    // height) in Curved mode. Both the hover highlight and the drag go through
    // it, so the highlight never promises a grab that the click will not honour.
    //
    // Connectors, control handles and the rotation grabbers are picked by their
    // own raycasters and are untouched by this.
    bool mouse_on_cut_surface(const Vec2d& mouse_position) const;
    // Last mouse-move answer from mouse_on_cut_surface(), so the HIGHLIGHT can
    // follow the same bound as the click without re-raycasting every frame.
    // Only meaningful while m_hover_id == CutPlane.
    bool m_cut_surface_hovered{ false };
    // First-refusal keyboard hook, in the shape GLGizmoSculpt::on_sculpt_char
    // has: GLGizmosManager::on_char offers the key here BEFORE the canvas's own
    // Ctrl+Z / Ctrl+Y get it, so an undo while the Cut gizmo is open rolls back
    // a sheet edit if there is one to roll back, and otherwise falls through to
    // the plater's undo untouched. Returns true when the key was consumed.
    bool on_cut_char(int key_code, bool shift_down, bool ctrl_down);

    // m_hover_id == CutPlane AND the ray is within the drawn extent.
    bool cut_surface_hovered(const Vec2d& mouse_position) const
        { return m_hover_id == CutPlane && mouse_on_cut_surface(mouse_position); }

    bool is_in_editing_mode() const override { return m_connectors_editing; }
    bool is_selection_rectangle_dragging() const override { return m_selection_rectangle.is_dragging(); }
    bool is_looking_forward() const;

    /// <summary>
    /// Drag of plane
    /// </summary>
    /// <param name="mouse_event">Keep information about mouse click</param>
    /// <returns>Return True when use the information otherwise False.</returns>
    bool on_mouse(const wxMouseEvent &mouse_event) override;

    void shift_cut(double delta);
    void rotate_vec3d_around_plane_center(Vec3d&vec);
    void put_connectors_on_cut_plane(const Vec3d& cp_normal, double cp_offset);
    void update_clipper();
    void invalidate_cut_plane();
    // pick-face mode: apply the currently picked facet as the cut plane
    bool apply_picked_facet();

    BoundingBoxf3   bounding_box() const;
    BoundingBoxf3   transformed_bounding_box(const Vec3d& plane_center, const Transform3d& rotation_m = Transform3d::Identity()) const;

protected:
    bool               on_init() override;
    void               on_load(cereal::BinaryInputArchive&ar) override;
    void               on_save(cereal::BinaryOutputArchive&ar) const override;
    std::string        on_get_name() const override;
    void               on_set_state() override;
    CommonGizmosDataID on_get_requirements() const override;
    void               on_set_hover_id() override;
    bool               on_is_activable() const override;
    bool               on_is_selectable() const override;
    Vec3d              mouse_position_in_local_plane(GrabberID axis, const Linef3&mouse_ray) const;
    void               dragging_grabber_move(const GLGizmoBase::UpdateData &data);
    void               dragging_grabber_rotation(const GLGizmoBase::UpdateData &data);
    void               dragging_connector(const GLGizmoBase::UpdateData &data);
    void               on_dragging(const UpdateData&data) override;
    void               on_start_dragging() override;
    void               on_stop_dragging() override;
    void               on_render() override;

    void render_debug_input_window(float x);
    void unselect_all_connectors();
    void select_all_connectors();
    void apply_selected_connectors(std::function<void(size_t idx)> apply_fn);
    void render_connectors_input_window(CutConnectors &connectors, float x, float y, float bottom_limit);
    void render_build_size();
    void reset_cut_plane();
    void set_connectors_editing(bool connectors_editing);
    void flip_cut_plane();
    void process_contours();
    void reset_cut_by_contours();
    void render_flip_plane_button(bool disable_pred = false);
    void add_vertical_scaled_interval(float interval);
    void add_horizontal_scaled_interval(float interval);
    void add_horizontal_shift(float shift);
    void render_color_marker(float size, const ImU32& color);
    void render_groove_float_input(const std::string &label, float &in_val, const float &init_val, float &in_tolerance);
    void render_groove_angle_input(const std::string &label, float &in_val, const float &init_val, float min_val, float max_val);
    bool render_angle_input(const std::string& label, float& in_val, const float& init_val, float min_val, float max_val);
    void render_snap_specific_input(const std::string& label, const wxString& tooltip, float& in_val, const float& init_val, const float min_val, const float max_val);
    void render_connectors_window_footer(float x, float y);
    // Flexi joint
    bool   is_flexi_joint_type() const { return m_connector_type == CutConnectorType::FlexiJoint; }
    bool   render_flexi_float_input(const std::string& label, float& in_val, float min_val, float max_val, const wxString& tooltip);
    Vec3d  flexi_hinge_axis_world() const;
    Vec3d  flexi_twist_axis_world() const;
    float  flexi_hinge_auto_edge_offset() const;
    // Rotation about the cut normal, in DEGREES (FlexiJointParams::rotation), 0-180.
    bool   render_flexi_rotation_input(const std::string& label, float& in_val, const wxString& tooltip);
    void   render_flexi_joint_inputs(CutConnectors& connectors);
    void   sync_flexi_params(CutConnectors& connectors, bool resize_from_section);
    double flexi_section_inscribed_radius() const;
    float  flexi_nozzle_diameter() const;
    double flexi_slice_closing_radius() const;
    void render_cut_plane_input_window(CutConnectors &connectors, float x, float y, float bottom_limit);
    void init_input_window_data(CutConnectors &connectors);
    void render_input_window_warning() const;
    bool add_connector(CutConnectors&connectors, const Vec2d&mouse_position);
    bool delete_selected_connectors(CutConnectors&connectors);
    void select_connector(int idx, bool select);
    bool is_selection_changed(bool alt_down, bool shift_down);
    void process_selection_rectangle(CutConnectors &connectors);

    virtual void on_register_raycasters_for_picking() override;
    virtual void on_unregister_raycasters_for_picking() override;
    void update_raycasters_for_picking();
    void set_volumes_picking_state(bool state);
    void update_raycasters_for_picking_transform();

    void update_plane_model();

    void on_render_input_window(float x, float y, float bottom_limit) override;
    void show_tooltip_information(float x, float y);

    bool wants_enter_leave_snapshots() const override       { return true; }
    std::string get_gizmo_entering_text() const override    { return _u8L("Entering Cut gizmo"); }
    std::string get_gizmo_leaving_text() const override     { return _u8L("Leaving Cut gizmo"); }
    std::string get_action_snapshot_name() const override   { return _u8L("Cut gizmo editing"); }

    void data_changed(bool is_serializing) override; 
    Transform3d get_cut_matrix(const Selection& selection);

private:
    void set_center(const Vec3d&center, bool update_tbb = false);
    void switch_to_mode(size_t new_mode);
    bool render_cut_mode_combo();
    bool render_double_input(const std::string& label, double& value_in);
    bool render_slider_double_input(const std::string& label, float& value_in, float& tolerance_in, float min_val = -0.1f, float max_tolerance = -0.1f);
    void render_move_center_input(int axis);
    void render_connect_mode_radio_button(CutConnectorMode mode);
    bool render_reset_button(const std::string& label_id, const std::string& tooltip) const;
    bool render_connect_type_radio_button(CutConnectorType type);
    bool is_outside_of_cut_contour(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos);
    bool is_conflict_for_connector(size_t idx, const CutConnectors& connectors, const Vec3d cur_pos);
    void render_connectors();

    // --- Curved cut (phase 1) ---------------------------------------------
    bool   is_curved_surface() const { return m_curved_surface && CutMode(m_mode) == CutMode::cutPlanar; }
    // Half extent the sheet needs so it covers the object under the cut plane.
    // The FALLBACK extent, used before the first successful cross-section fit
    // (and when the plane misses the object): the bounding-box based size phase
    // 1 always used. fit_curved_sheet_to_section() replaces it as soon as it has
    // a cross-section to fit to.
    double curved_sheet_half_size() const;
    void   update_curved_sheet_model();
    // PHASE 4: the connector PICK mesh is invalidated with the sheet too - a
    // click has to hit the surface as it stands, not as it was before the last
    // handle drag.
    void   invalidate_curved_sheet() { m_curved_sheet_dirty = true; m_curved_sheet_tex_dirty = true; m_curved_cap_dirty = true; m_curved_pick_dirty = true; }
    void   render_curved_sheet();
    void   render_curved_control_points();
    // Upload / drop the height-field texture and point the volume shader at it.
    void   update_curved_sheet_texture();
    void   apply_curved_color_clip();
    void   release_curved_sheet_texture();
    // Rebuild the curved cut face from the sheet and the instance mesh.
    void   update_curved_cap_model();
    void   render_curved_cap();
    // World position of control point (i,j), i.e. through the base plane's
    // own rotate/translate, so the sheet follows the plane.
    Vec3d  curved_control_world(int i, int j) const;
    // Screen-space pick of the nearest control point; -1 when none is close.
    int    curved_pick_control(const Vec2d& mouse_position) const;
    // Project the mouse onto the camera-facing plane through the drag anchor,
    // the way GLGizmoSculpt does, and return the displacement along the cut
    // plane normal only - f(u,v) is a height, not a free 3D position.
    bool   curved_drag_delta(const Vec2d& mouse_position, double& delta) const;
    bool   curved_on_mouse(const wxMouseEvent& mouse_event);
    void   render_curved_surface_inputs();

    // --- Curved cut (phase 4): connectors on the sheet ---------------------
    // A connector on a curved cut stands on the SHEET, not on the plane: its
    // position is the point of the sheet under the mouse and its frame is the
    // sheet's frame there. Everything below is that, and it is all a pure
    // function of the connector's own position, so no connector carries any new
    // state - which is what keeps the 3MF round trip working (see
    // apply_connectors_in_model()).

    // Local (x,y) in the cut plane's frame of a connector position given in the
    // OBJECT's frame (which is what CutConnector::pos holds).
    Vec2d  connector_plane_xy(const Vec3d& pos_object) const;
    // Drop a world point straight down the plane normal onto the cut plane. The
    // clipper's contour test is a 2D test in the plane's frame, so a footprint
    // sample taken in the sheet's TILTED tangent plane has to come back here
    // before it can be tested. Identity in effect for a point already on the
    // plane, so the flat path is unchanged.
    Vec3d  project_onto_cut_plane(const Vec3d& pos_world) const;
    // The connector's frame: the sheet's local frame at its (x,y) in Curved
    // mode, m_rotation_m in Flat mode. This is the ONE place the two differ, and
    // on a flat sheet the sheet frame IS m_rotation_m, so a curved-but-flat cut
    // renders and cuts exactly like a flat one.
    Transform3d connector_rotation_m(const Vec3d& pos_object) const;
    // Same, for a connector already in the list (uses its own z_angle for the
    // in-plane direction, the way the flat path applies z_angle separately).
    Transform3d connector_rotation_m(const CutConnector& connector) const;
    // Raise `pos_object` onto the sheet: keep its (x,y) in the plane frame and
    // take its height from f(x,y). A no-op in Flat mode.
    Vec3d  connector_pos_on_sheet(const Vec3d& pos_object) const;
    // Intersect the mouse ray with the SHEET (the dense sample mesh) rather than
    // with the flat clipping plane, and return the hit in the object's frame.
    // Falls back to the flat unproject when the ray misses the sheet, so a click
    // just off the bend still lands somewhere sensible.
    bool   unproject_on_curved_sheet(const Vec2d& mouse_position, Vec3d& pos, Vec3d& pos_world, bool respect_contours = true);
    // The sheet as a world-frame triangle mesh + its raycaster, rebuilt lazily.
    void   update_curved_sheet_raycaster();
    void   invalidate_curved_sheet_raycaster() { m_curved_pick_dirty = true; }
    // The connector's largest in-plane half extent, in mm - what the flat-patch
    // test measures the local curvature radius against.
    double connector_extent(const CutConnector& connector) const;
    // Warnings gathered for the connector panel: how many connectors stand more
    // than CurvedConnectorTiltWarnDeg off the plane normal, and how many are a
    // straight-featured Flexi kind (Hinge / Thread) on too tight a patch.
    void   update_curved_connector_warnings();

    bool can_perform_cut() const;
    bool has_valid_groove() const;
    bool has_valid_contour() const;
    void apply_connectors_in_model(ModelObject* mo, int &dowels_count);
    bool cut_line_processing() const;
    void discard_cut_line_processing();

    void apply_color_clip_plane_colors();
    void render_cut_plane();
    static void render_model(GLModel& model, const ColorRGBA& color, Transform3d view_model_matrix);
    void render_line(GLModel& line_model, const ColorRGBA& color, Transform3d view_model_matrix, float width);
    void render_rotation_snapping(GrabberID axis, const ColorRGBA& color);
    void render_grabber_connection(const ColorRGBA& color, Transform3d view_matrix, double line_len_koef = 1.0);
    void render_cut_plane_grabbers();
    void render_cut_line();
    void perform_cut(const Selection&selection);
    void set_center_pos(const Vec3d&center_pos, bool update_tbb = false);
    void update_bb();
    void init_picking_models();
    void init_rendering_items();
    void render_clipper_cut();
    void clear_selection();
    void reset_connectors();
    void init_connector_shapes();
    void update_connector_shape();
    void validate_connector_settings();
    bool process_cut_line(SLAGizmoEventType action, const Vec2d& mouse_position);
    void check_and_update_connectors_state();

    void toggle_model_objects_visibility();

    indexed_triangle_set its_make_groove_plane();

    indexed_triangle_set get_connector_mesh(CutConnectorAttributes connector_attributes);
    void apply_cut_connectors(ModelObject* mo, const std::string& connector_name);
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GLGizmoCut_hpp_
