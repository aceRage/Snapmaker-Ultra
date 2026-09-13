#ifndef slic3r_CutRecipe_hpp_
#define slic3r_CutRecipe_hpp_

// ---------------------------------------------------------------------------
// RE-EDITABLE CUTS: the recipe.
//
// A cut has always been DESTRUCTIVE. The plane, the curved sheet and the drawn
// stroke were session state in the gizmo; the cut baked them into two new
// meshes and everything that described the cut was gone. Only the connector
// VOLUMES survived, through Metadata/cut_information.xml, and only as baked
// geometry.
//
// A CutRecipe is the description that was missing: everything needed to perform
// the same cut again, plus the mesh it was performed ON. Both halves of a cut
// carry the same recipe (they share its cut_id), so selecting either one is
// enough to reopen the gizmo, move the plane and re-cut.
//
// WHERE IT LIVES. In the Model it hangs off ModelObject (::cut_recipe, an
// optional). In the 3MF it is Metadata/cut_recipe.xml plus one binary mesh blob
// per recipe under Metadata/cut_recipe/<sha256>.stlb - deliberately NOT in the
// main <resources> model mesh list, so an upstream slicer that knows nothing
// about any of this still loads the file and simply sees the two halves. The
// same discipline Metadata/cut_information.xml and Metadata/image_fill/ follow.
//
// SHARING. Both halves reference the same pre-cut mesh, which is stored ONCE,
// content-addressed by the SHA-256 of its serialized bytes. Two halves of one
// cut therefore cost one mesh, not two, and re-cutting the same object over and
// over does not grow the file.
//
// OPT-OUT. The Cut panel's "Keep cut editable" writes recipes; with it off
// nothing here is produced and the cut behaves exactly as it did before this
// feature existed.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>
#include <optional>

#include <libslic3r/Point.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/DrawCut.hpp>
// FlexiJointParams, carried per connector. Neither CurvedCut.hpp nor DrawCut.hpp
// pulls it in, so it has to be named here.
#include <libslic3r/FlexiJoint.hpp>

namespace Slic3r {

struct CutConnector;

// The schema version written into Metadata/cut_recipe.xml. A reader refuses a
// version it does not know rather than guessing at fields: a recipe it cannot
// reproduce exactly is worse than no recipe, because "Edit cut" would silently
// produce a DIFFERENT cut from the one the halves were made with.
//
//  1  initial: flat / curved / drawn, thickness, visibility, connectors, mesh blob.
//  2  the drawn line became a CHAIN of strokes (2026-09-12): CutRecipeStroke grew
//     `stroke_bounds`, the per-stroke ranges the chain's undo works off.
//
// A version 1 recipe STILL LOADS. Its samples ARE the chain - the samples and the
// closed flag were always the whole description of the line - so a version 1 file
// re-cuts to exactly the same halves; what it does not carry is where one stroke
// ended and the next began, and cut_recipe_stroke_to_chain() then treats the whole
// list as one stroke, which is the right answer for a line drawn in one gesture and
// the harmless answer for any other.
static constexpr int CutRecipeVersion = 2;

// The oldest version a reader accepts. Between this and CutRecipeVersion the fields
// a recipe does not carry are left at their defaults.
static constexpr int CutRecipeMinVersion = 1;
inline bool cut_recipe_version_supported(int v) { return v >= CutRecipeMinVersion && v <= CutRecipeVersion; }

// Which surface the cut was made with. Mirrors the gizmo's CutSurfaceMode
// crossed with its CutMode, flattened into the one choice that actually decides
// which Cut::perform_* runs.
enum class CutRecipeKind : int {
    // A flat plane: Cut::perform_with_plane().
    Plane = 0,
    // A height field over the plane: Cut::perform_with_curved_sheet().
    Curved = 1,
    // A ruled strip swept along a painted stroke: Cut::perform_with_draw_stroke().
    Drawn = 2,
    // Tongue and groove: Cut::perform_with_groove().
    Groove = 3,
};

inline bool cut_recipe_kind_valid(int k) { return k >= int(CutRecipeKind::Plane) && k <= int(CutRecipeKind::Groove); }

// The tongue-and-groove parameters, mirrored from Cut::Groove so the recipe does
// not have to include CutUtils.hpp (which includes the Model, which includes
// this). Converted at the call site.
struct CutRecipeGroove
{
    float depth{ 0.f };
    float width{ 0.f };
    float flaps_angle{ 0.f };
    float angle{ 0.f };
    float depth_init{ 0.f };
    float width_init{ 0.f };
    float flaps_angle_init{ 0.f };
    float angle_init{ 0.f };
    float depth_tolerance{ 0.1f };
    float width_tolerance{ 0.1f };

    bool operator==(const CutRecipeGroove& o) const;
    bool operator!=(const CutRecipeGroove& o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive& ar) {
        ar(depth, width, flaps_angle, angle, depth_init, width_init, flaps_angle_init, angle_init,
           depth_tolerance, width_tolerance);
    }
};

// One connector, as the recipe remembers it. This is the DEFINITION the gizmo
// works with (ModelObject::cut_connectors), not the baked volume: re-cutting
// regenerates the volumes from these.
//
// Deliberately a struct of its own rather than a CutConnector: CutConnector
// lives in Model.hpp, which includes this header, so the recipe cannot name it.
// The two convert in CutRecipe.cpp.
struct CutRecipeConnector
{
    Vec3d       pos{ Vec3d::Zero() };
    Transform3d rotation_m{ Transform3d::Identity() };
    float       radius{ 5.f };
    float       height{ 10.f };
    float       radius_tolerance{ 0.f };
    float       height_tolerance{ 0.1f };
    float       z_angle{ 0.f };
    int         type{ 0 };   // CutConnectorType
    int         style{ 0 };  // CutConnectorStyle
    int         shape{ 0 };  // CutConnectorShape
    // Only meaningful for CutConnectorType::FlexiJoint.
    FlexiJointParams flexi;

    bool operator==(const CutRecipeConnector& o) const;
    bool operator!=(const CutRecipeConnector& o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive& ar) {
        ar(pos, rotation_m, radius, height, radius_tolerance, height_tolerance, z_angle,
           type, style, shape, flexi);
    }
};

// The stroke, flattened for storage. DrawCutStroke keeps raw samples, a
// resampled path and derived binormals; only the RAW samples plus the open /
// closed decision are stored, because finish() regenerates the rest
// deterministically from them and the parameters - which is exactly the
// contract DrawCutStroke::finish() documents ("always starts from the RAW
// samples"). Storing the path too would let the two disagree.
struct CutRecipeStroke
{
    std::vector<DrawCutSample> samples;
    bool                       closed{ false };
    // The panel's 0..1 smoothing, which is an INPUT to finish(), so it belongs
    // with the samples rather than with the sweep parameters.
    double                     smoothing{ 0.2 };
    // VERSION 2 (2026-09-12). The line is a CHAIN of strokes, and the chain's undo
    // works off the range each appended stroke occupies in `samples`. Storing the
    // ranges is what lets a reopened cut's Ctrl+Z take back one stroke rather than
    // the whole line.
    //
    // Empty for a version 1 recipe. cut_recipe_stroke_to_chain() then treats the
    // whole sample list as one stroke.
    std::vector<std::pair<uint32_t, uint32_t>> stroke_bounds;
    // VERSION 2. The user's explicit "this line is finished and is not a loop" - the
    // panel's "Cut along the line". Distinct from `closed == false`, which on its own
    // means only "not a loop", and which for a chain still being drawn means "not
    // finished". See DrawCutChain::finish_open().
    //
    // A version 1 recipe was always one of the two: its line was cut with, so it was
    // finished. cut_recipe_stroke_to_chain() therefore treats an unclosed version 1
    // stroke as finished-open, which is what it was.
    bool                       finished_open{ false };

    bool operator==(const CutRecipeStroke& o) const;
    bool operator!=(const CutRecipeStroke& o) const { return !(*this == o); }

    // DrawCutSample has no serializer of its own (it is a plain capture record in
    // DrawCut.hpp, which knows nothing about cereal), so its fields go through
    // one by one rather than pulling cereal into that header.
    // Cereal is for the UNDO STACK and the project backup, never across versions -
    // a blob is written and read by the same binary - so both sides always carry
    // stroke_bounds. The 3MF path is the one that has to read a version 1 stream,
    // and it has its own explicit schema in bbs_3mf.cpp.
    template<class Archive> void save(Archive& ar) const {
        ar(finished_open);
        ar(closed, smoothing);
        ar(uint64_t(samples.size()));
        for (const DrawCutSample& s : samples)
            ar(s.pos, s.normal, uint64_t(s.facet));
        ar(uint64_t(stroke_bounds.size()));
        for (const std::pair<uint32_t, uint32_t>& b : stroke_bounds)
            ar(b.first, b.second);
    }
    template<class Archive> void load(Archive& ar) {
        uint64_t n = 0, nb = 0;
        ar(finished_open);
        ar(closed, smoothing);
        ar(n);
        samples.clear();
        samples.resize(size_t(n));
        for (DrawCutSample& s : samples) {
            uint64_t facet = 0;
            ar(s.pos, s.normal, facet);
            s.facet = size_t(facet);
        }
        ar(nb);
        stroke_bounds.clear();
        stroke_bounds.reserve(size_t(nb));
        for (uint64_t i = 0; i < nb; ++ i) {
            uint32_t a = 0, b = 0;
            ar(a, b);
            stroke_bounds.emplace_back(a, b);
        }
    }
};

// The sheet, flattened for storage: the control grid and the extent it is
// defined over. CurvedCutSheet derives everything else from these.
struct CutRecipeSheet
{
    int                 nx{ 0 };
    int                 ny{ 0 };
    double              half_size_u{ 0.0 };
    double              half_size_v{ 0.0 };
    std::vector<double> values;

    bool valid() const { return nx >= 2 && ny >= 2 && values.size() == size_t(nx) * size_t(ny); }
    bool operator==(const CutRecipeSheet& o) const;
    bool operator!=(const CutRecipeSheet& o) const { return !(*this == o); }

    template<class Archive> void serialize(Archive& ar) {
        ar(nx, ny, half_size_u, half_size_v, values);
    }
};

// ---------------------------------------------------------------------------
// The recipe.
// ---------------------------------------------------------------------------
struct CutRecipe
{
    int           version{ CutRecipeVersion };
    CutRecipeKind kind{ CutRecipeKind::Plane };

    // --- the cut frame ----------------------------------------------------
    // The cut plane, in the OBJECT's coordinate system (the frame
    // ModelObject::volumes live in), not the world: the halves can be moved,
    // rotated and re-laid-out on the bed after the cut, and a world-frame plane
    // would then describe a cut through empty space. The gizmo converts to and
    // from the world with the instance transform it is editing.
    //
    // Together these are the gizmo's m_plane_center and m_rotation_m, taken into
    // the object frame.
    Vec3d       plane_center{ Vec3d::Zero() };
    Transform3d rotation_m{ Transform3d::Identity() };

    // --- the surface ------------------------------------------------------
    CutRecipeSheet  sheet;   // kind == Curved
    CutRecipeStroke stroke;  // kind == Drawn
    CutRecipeGroove groove;  // kind == Groove

    // Drawn-cut sweep parameters. Held as the scalar fields rather than as a
    // DrawCutParams so the stored form is explicit about what is persisted;
    // draw_params() composes one.
    int    draw_direction{ int(DrawCutDirection::SurfaceNormal) };
    Vec3d  draw_view_dir{ -Vec3d::UnitZ() };
    double draw_extension{ 5.0 };
    double draw_angle_deg{ 0.0 };
    bool   draw_through_all{ true };
    double draw_depth{ 10.0 };

    // --- shared parameters ------------------------------------------------
    // Kerf. Applies to Plane, Curved and Drawn alike.
    double             thickness{ 0.0 };
    CutThicknessOffset thickness_offset{ CutThicknessOffset::Centred };

    // The after-cut attributes, exactly as the gizmo's checkboxes set them.
    bool keep_upper{ true };
    bool keep_lower{ true };
    bool keep_as_parts{ false };
    bool place_on_cut_upper{ true };
    bool place_on_cut_lower{ false };
    bool rotate_upper{ false };
    bool rotate_lower{ false };

    // Preview-only side visibility (0 Visible, 1 Ghost, 2 Hidden). Not part of
    // the geometry, but the spec asks for it: reopening a cut should look the
    // way the user left it.
    int upper_visibility{ 0 };
    int lower_visibility{ 0 };

    // --- connectors -------------------------------------------------------
    std::vector<CutRecipeConnector> connectors;

    // --- the pre-cut mesh -------------------------------------------------
    // The object as it stood BEFORE the cut, in the object frame, as ONE mesh
    // (the volumes' meshes merged, each already transformed into the object
    // frame - which is what Cut operates on anyway). Content-addressed: the
    // hash names the blob in the 3MF, and is what lets the two halves share one
    // copy.
    //
    // Empty when the recipe came from a 3MF whose blob was missing or corrupt;
    // has_mesh() is then false and "Edit cut" refuses with a clear reason
    // rather than re-cutting the wrong thing.
    TriangleMesh mesh;
    std::string  mesh_hash;

    bool has_mesh() const { return !mesh.empty(); }
    // A recipe that can actually be re-cut.
    bool valid() const;

    // The sweep parameters as DrawCut wants them.
    DrawCutParams  draw_params() const;
    // The sheet as CurvedCut wants it.
    CurvedCutSheet curved_sheet() const;

    // Field-by-field equality, EXCLUDING the mesh (compared by hash instead).
    // This is what the round-trip test asserts.
    bool operator==(const CutRecipe& o) const;
    bool operator!=(const CutRecipe& o) const { return !(*this == o); }

    // Cereal, for the undo/redo stack and the project backup - NOT for the 3MF,
    // which has its own explicit schema in bbs_3mf.cpp. The mesh rides along:
    // dropping it would make an undo step silently lose the ability to re-edit,
    // which is exactly the kind of half-state this feature exists to avoid.
    template<class Archive> void serialize(Archive& ar) {
        int kind_i = int(kind);
        int toff_i = int(thickness_offset);
        ar(version, kind_i, toff_i);
        kind             = cut_recipe_kind_valid(kind_i) ? CutRecipeKind(kind_i) : CutRecipeKind::Plane;
        thickness_offset = CutThicknessOffset(toff_i);
        ar(plane_center, rotation_m);
        ar(sheet, stroke, groove);
        ar(draw_direction, draw_view_dir, draw_extension, draw_angle_deg, draw_through_all, draw_depth);
        ar(thickness);
        ar(keep_upper, keep_lower, keep_as_parts, place_on_cut_upper, place_on_cut_lower,
           rotate_upper, rotate_lower);
        ar(upper_visibility, lower_visibility);
        ar(connectors);
        ar(mesh, mesh_hash);
    }
};

// Serialize a mesh to the recipe's own compact binary form, and back. Not STL:
// this keeps full double-free float precision and the exact vertex indexing, so
// a round trip is bit-identical rather than merely close - which is what lets
// the re-cut reproduce the same halves.
//
// Layout, all little-endian, no padding:
//   magic "ESCUTMSH", uint32 version(1), uint32 n_vertices, uint32 n_facets,
//   then n_vertices * 3 float32, then n_facets * 3 uint32.
std::vector<uint8_t> cut_recipe_mesh_to_blob(const TriangleMesh& mesh);
bool                 cut_recipe_mesh_from_blob(const std::vector<uint8_t>& blob, TriangleMesh& out);

// SHA-256 of the blob, lowercase hex. Names the file in the 3MF.
std::string cut_recipe_mesh_hash(const std::vector<uint8_t>& blob);

// Convert between the recipe's connector form and the Model's. Defined in
// CutRecipe.cpp, which may include Model.hpp.
void cut_recipe_connectors_from_model(const std::vector<CutConnector>& in, std::vector<CutRecipeConnector>& out);
void cut_recipe_connectors_to_model(const std::vector<CutRecipeConnector>& in, std::vector<CutConnector>& out);

// Convert between the recipe's stored stroke and the DrawCutChain the gizmo edits.
//
// A recipe whose `stroke_bounds` is empty - every version 1 recipe, and any version 2
// one written from a line drawn in a single gesture - becomes a chain of ONE stroke
// covering every sample. That is right twice over: it is what a single-gesture line
// was, and a reopened cut's first Ctrl+Z should take back "the line", not unpick
// strokes from a session the user does not remember.
//
// Bounds that do not tile [0, samples.size()) exactly are DISCARDED rather than
// half-applied, and the one-stroke fallback is used: a chain whose ranges do not
// match its samples would corrupt undo in a way the user cannot see coming.
void            cut_recipe_stroke_to_chain(const CutRecipeStroke& in, DrawCutChain& out);
CutRecipeStroke cut_recipe_stroke_from_chain(const DrawCutChain& chain, double smoothing);

} // namespace Slic3r

#endif // slic3r_CutRecipe_hpp_
