#ifndef slic3r_MeshEdit_hpp_
#define slic3r_MeshEdit_hpp_

// Ultra: CAD-like direct mesh editing on an indexed_triangle_set - the geometry
// half of the "Edit" gizmo (docs/superpowers/specs/2026-09-11-cad-mode-research.md).
//
// PHASE 1 SCOPE, and the reason for it: the only mesh operation in here is
// translate_region(), which MOVES VERTEX POSITIONS AND NOTHING ELSE. It never
// touches its.indices, never adds or removes a vertex, and never reorders
// anything. That is the same contract MeshSculpt.hpp states, and it buys the
// same thing: the commit path may go through Sculpt::commit_sculpted_mesh()
// (ModelVolume::set_mesh + changed_mesh) WITHOUT Plater::clear_before_change_mesh(),
// so painted supports, seams, MMU colours and fuzzy skin all survive a push/pull.
//
// Phase 2 (edge bevel/chamfer) and phase 3 (extrude / inset / loop tools) insert
// geometry and therefore renumber facets; they are deliberately NOT here, and
// when they land they must take the other commit path the way Subdivide and
// Simplify do.
//
// Everything in this header is pure: no GL, no wx, no Model. The gizmo owns the
// mouse, the panel and the undo stack; this owns the mesh.

#include <cstdint>
#include <vector>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

namespace MeshEdit {

// ----------------------------------------------------------------------------
// Topology cache
// ----------------------------------------------------------------------------
//
// Everything the selection and the push/pull need, computed once per session:
// face normals, face neighbours, the global edge id of each (facet, side), and
// the reverse map from a global edge id to its (up to two) incident facets.
//
// its_face_edge_ids(its, face_neighbors, ...) already gives the first three in
// one pass (TriangleMesh.hpp:197); the reverse map is built here because the
// chain walk needs to go edge -> facets, which nothing in-tree exposes.
struct MeshTopology
{
    // Per facet: the outward unit normal. Zero for a degenerate facet.
    std::vector<Vec3f>   face_normals;
    // Per facet: the neighbouring facet across each of the three sides, or -1
    // for an open edge.
    std::vector<Vec3i32> face_neighbors;
    // Per facet: the global edge id of each of the three sides. Two facets
    // sharing an edge report the same id here, which is what makes an edge a
    // first-class object rather than a (facet, side) pair.
    std::vector<Vec3i32> face_edge_ids;
    // Number of distinct global edge ids.
    int                  num_edges{0};
    // Per global edge id: the two vertex indices of that edge, in the order the
    // first facet that claimed it saw them.
    std::vector<Vec2i32> edge_vertices;
    // Per global edge id: the incident facets. Exactly two on a closed manifold
    // edge, one on a boundary edge, more than two on a non-manifold one (which
    // every operation in here refuses rather than guesses at).
    std::vector<Vec2i32> edge_faces;   // second is -1 when there is only one
    // Per global edge id: how many facets actually claimed it. > 2 marks a
    // non-manifold edge.
    std::vector<uint8_t> edge_face_count;

    size_t face_count() const { return face_normals.size(); }
    bool   valid() const { return !face_normals.empty(); }
};

// Build the cache. O(F log F) - one its_face_edge_ids pass plus a linear
// scatter into the reverse map.
MeshTopology build_topology(const indexed_triangle_set &its);

// ----------------------------------------------------------------------------
// Feature edges and chains
// ----------------------------------------------------------------------------

// The dihedral angle at a global edge, in degrees: the angle between its two
// incident facet normals, 0 for a flat (coplanar) edge and 90 for a cube's.
// A boundary edge (one incident facet) is reported as 180 - it is as "feature"
// as an edge gets - and a non-manifold edge as 0, since we refuse to chain
// through one anyway.
float edge_dihedral_deg(const MeshTopology &topo, int edge_id);

// Which global edges count as feature edges at `threshold_deg`. A cube at 45 deg
// gives exactly its 12 edges: every edge of a cube has a 90 deg dihedral and
// every coplanar edge (the diagonal splitting each square face into 2 triangles)
// has 0.
std::vector<uint8_t> feature_edge_mask(const MeshTopology &topo, float threshold_deg);

// One selected chain of feature edges: global edge ids, in walk order, plus the
// vertex path they trace. `closed` is true when the walk came back to its start
// (a cube's top face is a 4-edge closed loop).
struct EdgeChain
{
    std::vector<int>      edges;
    // edges.size() + 1 vertices for an open chain, edges.size() for a closed
    // one (the repeat of the first vertex is not stored).
    std::vector<int>      vertices;
    bool                  closed{false};

    bool empty() const { return edges.empty(); }
};

// Grow the chain containing `seed_edge`.
//
// The walk: from each end of the current chain, look at the feature edges
// incident to the end vertex. Extend only when there is EXACTLY ONE other
// feature edge there and the turn it makes is under `continuation_deg`.
//
// The "exactly one" rule is what makes a cube behave: at a cube corner three
// feature edges meet, so the walk stops there, and a chain seeded on a top edge
// closes into the 4-edge top loop instead of leaking down a side. Vertices with
// three or more incident feature edges are corners, and corners terminate.
//
// Returns an empty chain when `seed_edge` is not itself a feature edge.
EdgeChain grow_edge_chain(const indexed_triangle_set &its,
                          const MeshTopology         &topo,
                          const std::vector<uint8_t> &is_feature,
                          int                         seed_edge,
                          float                       continuation_deg = 35.f);

// Partition every feature edge into chains. The order of the chains is the order
// their lowest edge id appears, so the result is deterministic. Used by the
// tests (a cube at 45 deg = 12 edges in 12 chains: every cube vertex has three
// incident feature edges, so every one is a corner and no edge ever extends)
// and by the gizmo when it wants to pre-highlight all of them.
std::vector<EdgeChain> all_edge_chains(const indexed_triangle_set &its,
                                       const MeshTopology         &topo,
                                       float                       threshold_deg,
                                       float                       continuation_deg = 35.f);

// The global edge of facet `facet` nearest to `point` (which is expected to lie
// on or near that facet), plus the distance to it. Returns -1 when the facet is
// out of range. The gizmo uses this to turn a raycast hit into an edge pick;
// whether that pick beats a face pick is the gizmo's snap-radius decision, the
// way Measure::facet_snap_extent caps it.
int  nearest_edge_of_facet(const indexed_triangle_set &its,
                           const MeshTopology         &topo,
                           size_t                      facet,
                           const Vec3f                &point,
                           float                      *out_distance = nullptr);

// ----------------------------------------------------------------------------
// Planar face regions
// ----------------------------------------------------------------------------

// How a click grows into a region.
enum class RegionMode : unsigned char {
    // Strictly coplanar with the SEED facet, within `angle_tol_deg`. This is
    // what a CAD "face" is: a cube's +Z face is exactly its 2 triangles, and a
    // tessellated cylinder's side wall is NOT one face.
    Planar,
    // The spec's reusable "smooth region": admit a neighbour when it is within
    // `step_angle_deg` of the facet we came from AND within `angle_tol_deg` of
    // the seed. This is Measure.cpp's grow_curve_patch, re-expressed here with
    // both thresholds exposed rather than hard-coded at 8 and 20 deg, so it
    // tolerates a gently curved face. Reused, not copied: the shape of the DFS
    // is the same, the tuning is now the caller's.
    Smooth
};

struct RegionParams
{
    RegionMode mode{RegionMode::Planar};
    // Planar: the total deviation from the SEED normal a facet may have.
    // Smooth: the same, as the cap that stops a full cylinder being swallowed.
    float      angle_tol_deg{1.f};
    // Smooth only: the per-step deviation between adjacent facets.
    float      step_angle_deg{8.f};
    // Hard cap, so a hover on a million-triangle scan cannot stall the UI.
    size_t     max_facets{200000};
};

// A selected face region.
struct FaceRegion
{
    // Facet indices, SORTED ascending, no duplicates. Sorted so two grows of the
    // same region compare equal and so the highlight model is deterministic.
    std::vector<int> facets;
    // Area-weighted mean unit normal over `facets`. For a planar region this is
    // exactly the face normal; for a smooth one it is the direction a push moves
    // along.
    Vec3f            normal{Vec3f::UnitZ()};
    // Summed area of `facets`, mm^2 for a part at scale 1. This is the number
    // the volume check in the tests multiplies by the push distance.
    float            area{0.f};
    // Centroid of the region, area-weighted: where the gizmo puts its arrow.
    Vec3f            center{Vec3f::Zero()};

    bool empty() const { return facets.empty(); }
};

// Grow the region containing `seed_facet`. Returns an empty region for an
// out-of-range seed or a degenerate seed facet.
FaceRegion grow_face_region(const indexed_triangle_set &its,
                            const MeshTopology         &topo,
                            size_t                      seed_facet,
                            const RegionParams         &params);

// ----------------------------------------------------------------------------
// Push / pull by translation - the one mesh edit of phase 1
// ----------------------------------------------------------------------------

// What a translate did or refused to do.
enum class TranslateStatus : unsigned char {
    Ok,
    // Nothing to move: an empty region, or a zero-length direction.
    EmptyRegion,
    // d == 0. The mesh is returned bit-identical; this is the documented
    // identity case, not an error.
    NoOp,
    // A cheap pre-check refused it: the move pushes the region past the far side
    // of the mesh's own bounding box, so it cannot be anything but a fold.
    OutOfBounds,
    // The full check refused it: the moved mesh self-intersects.
    SelfIntersects,
    // The region covers the whole mesh, so "push the face" would just move the
    // part - there is no ring to stretch. The Move gizmo is the right tool.
    WholeMesh
};

struct TranslateResult
{
    TranslateStatus status{TranslateStatus::EmptyRegion};
    // The moved mesh. On any status other than Ok it is left empty, EXCEPT
    // NoOp, which returns the input unchanged.
    indexed_triangle_set mesh;
    // Vertices whose position changed, sorted. Empty for NoOp. The gizmo feeds
    // these to the vertex-buffer patch the way a sculpt stroke does.
    std::vector<uint32_t> moved_vertices;
    // Facets whose geometry is now stale: the region's own, plus every facet
    // incident to a moved vertex (the "ring" that stretches to follow).
    std::vector<uint32_t> dirty_facets;

    bool ok() const { return status == TranslateStatus::Ok || status == TranslateStatus::NoOp; }
};

struct TranslateParams
{
    // Signed distance along `direction`, in mesh units.
    float d{0.f};
    // Unit vector. A zero vector means "use the region's own normal", which is
    // what the gizmo's push/pull arrow does.
    Vec3f direction{Vec3f::Zero()};
    // Run MeshBoolean::cgal::does_self_intersect on the result and refuse a
    // positive. Off makes translate_region() allocation-cheap enough for a live
    // drag preview; the gizmo runs it once, on release, before it commits.
    bool  check_self_intersection{true};
    // Multiplier on the mesh's bounding-box diagonal past which the cheap
    // pre-check refuses outright, without paying for the CGAL test.
    float max_travel_bbox_factor{1.f};
};

// Move every vertex used by `region`'s facets by d * direction.
//
// The vertices of the region's border are moved too, and every facet outside the
// region that touches one of them therefore STRETCHES to follow: a cube's top
// face pushed up 5 mm makes the four side walls 5 mm taller, which is the whole
// point. No facet is added, removed or renumbered, so the painted-data contract
// above holds.
//
// Caveat, recorded here rather than rediscovered: where a side wall is not
// parallel to `direction` (a tapered or chamfered face), stretching SHEARS the
// wall rather than lengthening it along its own plane. Moving border vertices
// along their adjacent-face planes instead is a phase-3 refinement.
TranslateResult translate_region(const indexed_triangle_set &its,
                                 const MeshTopology         &topo,
                                 const FaceRegion           &region,
                                 const TranslateParams      &params);

// ----------------------------------------------------------------------------
// Edge bevel / chamfer - PHASE 2, the real algorithm
// ----------------------------------------------------------------------------
//
// Takes a set of feature edges (phase 1's EdgeChain, or several of them, or every
// edge of the mesh) and replaces each one with a strip of new geometry: a single
// flat facet band for a CHAMFER, or an N-segment approximation of a circular arc
// for a BEVEL.
//
// THE CONTRACT IS DIFFERENT FROM PHASE 1's, and deliberately so. This INSERTS
// GEOMETRY: vertices are added, facets are added and every index is renumbered.
// So the commit must take the OTHER path - Plater::clear_before_change_mesh()
// first, the way Subdivide and Simplify do - and painted supports, seams, MMU
// colours and fuzzy skin are all CLEARED. The header says it once here so nobody
// has to rediscover it from a bug report.
//
// ---------------------------------------------------------------------------
// The algorithm, and why it is shaped this way
// ---------------------------------------------------------------------------
//
// The single most important design decision, taken straight from the research
// spec's reading of Blender's bmesh_bevel: SOLVE EVERY WIDTH GLOBALLY BEFORE
// BUILDING ANY GEOMETRY. Blender's first bevel solved widths greedily, one edge
// at a time, depth-first - and was scrapped because it was ORDER-DEPENDENT: two
// topologically identical parts came out with different widths depending on
// which edge the walk happened to start from. This repo has determinism gates,
// so that failure mode is not acceptable here either.
//
// Concretely the pass order is:
//
//   1. VALIDATE. Every requested edge must be manifold (exactly two incident
//      facets) and non-degenerate. A non-manifold edge is refused outright
//      rather than guessed at.
//   2. SOLVE WIDTHS. Compute one offset distance per BEVELLED EDGE, as the
//      minimum of the requested d and every geometric limit that edge is subject
//      to - the length of the incident facets' other edges, the room left by
//      other bevelled edges meeting it at a shared vertex, and the chain's own
//      shortest edge. The solve is a pure function of the mesh and the edge SET,
//      never of the order the edges are visited in, so the same input always
//      gives the same widths.
//   3. BUILD. Only now is any geometry emitted: offset points, then edge strips,
//      then corner patches, then the re-cut interiors of the incident facets.
//   4. CLEAN. its_merge_vertices / its_remove_degenerate_faces /
//      its_compactify_vertices, then assert closedness.
//
// Steps 3 and 4 cannot change a width, so no amount of reordering them can
// change the result.

// Which cross-section to put on the edge.
enum class BevelProfile : unsigned char {
    // One flat band: the straight chord between the two offset points. This is
    // exactly the N = 1 case of Round, and the tests pin that equivalence.
    Chamfer,
    // N segments interpolated along the circular arc tangent to both incident
    // faces. N = 1 degenerates to the chamfer, which is the degeneracy test.
    Round
};

struct BevelParams
{
    // The offset distance from the edge, measured along each incident face, in
    // mesh units. This is the "width" the global solve clamps.
    float        width{1.f};
    // Segments in the cross-section. 1 is a chamfer whatever the profile says;
    // higher values only matter for Round. Clamped to [1, BevelMaxSegments].
    int          segments{1};
    BevelProfile profile{BevelProfile::Chamfer};
    // Below this dihedral an edge is too flat to be worth bevelling, and
    // bevelling it would only add facets for nothing. Such edges are dropped from
    // the request (and counted in the result) rather than refused.
    float        min_dihedral_deg{5.f};
    // The fraction of a limiting length a width may take up. 0.5 means "never
    // take more than half of the shortest edge you are competing for", which is
    // what stops two bevels meeting in the middle and inverting.
    float        clamp_fraction{0.5f};
    // Run the self-intersection guard on the result. Off for a live preview.
    bool         check_self_intersection{false};
};

static constexpr int BevelMaxSegments = 32;

// Why a bevel did or did not happen.
enum class BevelStatus : unsigned char {
    Ok,
    // No edges were asked for, or every one of them was dropped as too flat.
    EmptyChain,
    // At least one requested edge is non-manifold. Refused outright: the strip
    // has no well-defined two sides, so there is nothing honest to build.
    NonManifold,
    // width <= 0, or segments <= 0. The mesh comes back unchanged.
    NoOp,
    // The global solve clamped every width to zero - the requested width does not
    // fit anywhere on this mesh at all.
    WidthTooSmall,
    // The build produced an open or self-intersecting mesh. This is the "should
    // not happen" bucket; the input mesh is returned unchanged rather than a
    // broken one being handed on.
    Failed
};

struct BevelResult
{
    BevelStatus          status{BevelStatus::EmptyChain};
    // The bevelled mesh. Empty on any status but Ok and NoOp; NoOp returns the
    // input unchanged.
    indexed_triangle_set mesh;

    // --- what the solve decided, for the panel and the tests ---
    // How many edges were actually bevelled.
    size_t bevelled_edges{0};
    // How many were dropped for being flatter than min_dihedral_deg.
    size_t dropped_flat{0};
    // How many were dropped for being CONCAVE. A known limitation, spelled out at
    // the drop site in MeshEdit.cpp: the corner split removes material, which is
    // right for an outside edge and wrong for an inside one. "Round all edges"
    // (MeshRound.hpp) rounds concave edges and is the tool for them meanwhile.
    size_t dropped_concave{0};
    // How many corner patches were emitted (one per vertex where two or more
    // bevelled edges meet).
    size_t corner_patches{0};
    // The narrowest and widest width the solve settled on. When `clamped` is set,
    // min_width is below the requested one and the panel should say so.
    float  min_width{0.f};
    float  max_width{0.f};
    bool   clamped{false};

    bool ok() const { return status == BevelStatus::Ok || status == BevelStatus::NoOp; }
};

// Bevel `edges` (global edge ids, in any order - the result does not depend on
// it) on `its`.
//
// Corners: where two or more bevelled edges share a vertex, their strips would
// overlap, so the shared vertex is replaced by a CORNER PATCH - the polygon
// whose boundary is the arriving end-rings of each strip, triangulated as a fan
// from its own centroid. On a cube with all 12 edges bevelled this produces the
// classic 8 three-sided corner patches, which is the thing the tests check for.
//
// Returns Ok with the new mesh, or a status saying why not. The mesh is always
// either watertight and manifold or not returned at all.
BevelResult bevel_edges(const indexed_triangle_set &its,
                        const MeshTopology         &topo,
                        const std::vector<int>     &edges,
                        const BevelParams          &params);

// Convenience: bevel one chain. Exactly bevel_edges() over chain.edges.
BevelResult bevel_chain(const indexed_triangle_set &its,
                        const MeshTopology         &topo,
                        const EdgeChain            &chain,
                        const BevelParams          &params);

// The width solve, exposed so the tests can pin order-independence directly
// rather than inferring it from the output mesh, and so the gizmo's panel can
// show the clamped width before the user presses Apply.
//
// Returns one width per entry of `edges`, in the SAME ORDER as `edges`. An entry
// is 0 for an edge that was dropped (too flat) or could not be bevelled.
std::vector<float> solve_bevel_widths(const indexed_triangle_set &its,
                                      const MeshTopology         &topo,
                                      const std::vector<int>     &edges,
                                      const BevelParams          &params);

// The volume a box of `size` loses when `r` is chamfered off each of its twelve
// edges - the yardstick the cube tests measure against. A single edge of length
// L loses the triangular prism 0.5 * w^2 * L where w is the offset; twelve of
// them share eight corners, which the corner patches cut back.
//
// Exposed because both the tests and the gizmo's panel want the same number.
double chamfered_box_volume_loss(const Vec3d &size, double w);

// True when every edge of `its` is shared by exactly two facets: the closedness
// assertion the bevel finishes with, and what the tests mean by "watertight and
// manifold".
bool is_closed_manifold(const indexed_triangle_set &its);

// True when `its` intersects itself. Thin wrapper over
// MeshBoolean::cgal::does_self_intersect so the tests and the gizmo have one
// name for the guard and MeshEdit.cpp owns the only include of it.
bool self_intersects(const indexed_triangle_set &its);

// Snap `value` to the nearest multiple of `step` (a step <= 0 is the identity).
// The numeric field and the drag both go through this, so typing 5.0 and
// dragging to 5.0 give bit-identical meshes.
float snap_to_step(float value, float step);

// ----------------------------------------------------------------------------
// Session
// ----------------------------------------------------------------------------
//
// Per-part state for the duration of an Edit session: the working mesh, its
// topology cache, and the gizmo-local undo stack.
//
// The undo stack is the shape GLGizmoSculpt and the Cut gizmo's curved sheet
// both use - one entry per COMPLETED operation, pushed before the mesh is
// touched, consumed by Ctrl+Z / Ctrl+Y while the gizmo owns the keyboard. It
// stores whole meshes rather than deltas: phase 1's edits are vertex moves, a
// mesh of a few hundred thousand vertices is a few megabytes, and the limit
// keeps that bounded. Simple beats clever for a stack that is never hot.
class EditSession
{
public:
    explicit EditSession(const indexed_triangle_set &its);

    const indexed_triangle_set &mesh() const { return m_its; }
    const MeshTopology         &topology() const { return m_topo; }

    size_t vertices_count() const { return m_its.vertices.size(); }
    size_t triangles_count() const { return m_its.indices.size(); }

    // Selection helpers, so the gizmo does not re-thread the topology cache
    // through every call.
    FaceRegion grow_region(size_t seed_facet, const RegionParams &params) const;
    EdgeChain  grow_chain(int seed_edge, float threshold_deg, float continuation_deg) const;
    int        nearest_edge(size_t facet, const Vec3f &point, float *out_distance = nullptr) const;

    // Apply a translate to the working mesh. On Ok the mesh is replaced and an
    // undo entry is pushed first; on anything else nothing changes and the
    // status says why. NoOp changes nothing and pushes nothing.
    TranslateResult apply_translate(const FaceRegion &region, const TranslateParams &params);

    // Apply a bevel to the working mesh. Same contract as apply_translate: on Ok
    // the mesh is replaced and an undo entry is pushed FIRST; on anything else
    // nothing changes and the status says why.
    //
    // Unlike a translate this renumbers everything, so the caller must commit it
    // through the clear_before_change_mesh() path - see BevelResult's header
    // comment. The session itself does not care: it rebuilds its topology either
    // way, and the undo stack stores whole meshes.
    BevelResult apply_bevel(const std::vector<int> &edges, const BevelParams &params);
    BevelResult apply_bevel(const EdgeChain &chain, const BevelParams &params);

    // Preview a bevel WITHOUT touching the working mesh or the undo stack, so the
    // gizmo can show the result mesh while the width slider moves.
    BevelResult preview_bevel(const std::vector<int> &edges, const BevelParams &params) const;

    // Replace the working mesh outright (the drag preview reverting to the
    // pre-drag mesh between ticks). Does NOT push undo - a preview is not an
    // operation.
    void set_mesh(indexed_triangle_set &&its);

    // --- gizmo-local undo -----------------------------------------------
    bool can_undo() const { return !m_undo.empty(); }
    bool can_redo() const { return !m_redo.empty(); }
    bool undo();
    bool redo();
    void clear_history();
    size_t undo_depth() const { return m_undo.size(); }
    size_t redo_depth() const { return m_redo.size(); }

    static constexpr size_t UndoLimit = 32;

private:
    void push_undo();
    void rebuild_topology();

    indexed_triangle_set              m_its;
    MeshTopology                      m_topo;
    std::vector<indexed_triangle_set> m_undo;
    std::vector<indexed_triangle_set> m_redo;
};

} // namespace MeshEdit
} // namespace Slic3r

#endif // slic3r_MeshEdit_hpp_
