#ifndef slic3r_QuadRemesh_hpp_
#define slic3r_QuadRemesh_hpp_

// Ultra: Phase 2 of the remesh spec - an optional QUAD remesher, on top of Phase 1's
// voxel path (MeshRemesh.hpp).
//
// The two remeshers solve different problems and neither replaces the other:
//
//   * remesh_with_options() (MeshRemesh.hpp) rebuilds a mesh from its signed distance
//     field. It REPAIRS: whatever soup goes in, a watertight manifold comes out. It
//     does not care about the shape of the triangles it emits.
//   * quad_remesh() (here) rebuilds a mesh as a field-aligned grid of quads. It does
//     NOT repair - it needs a manifold input and refuses otherwise - but the faces it
//     emits are evenly sized and follow the surface's curvature, which is what the
//     Sculpt panel's Subdivide workflow wants and what arbitrary triangles are bad at.
//
// Backed by QuadriFlow (hjwdzh/QuadriFlow, BSD-3-Clause; see
// deps/QuadriFlow/QuadriFlow.cmake). The whole header is behind SLIC3R_QUAD_REMESH:
// when the dependency is absent the feature compiles out, and the GUI hides the menu
// entry rather than offering something that cannot work.
//
// Design notes worth knowing before calling this:
//
//   * DETERMINISM. QuadriFlow seeds a PRNG and calls srand(). The same input, target
//     and seed must give a bit-identical mesh, or the slice determinism gates
//     (docs/superpowers/specs/2026-09-07-slice-determinism.md) break the moment
//     anyone quad-remeshes before slicing. quad_remesh() therefore takes an explicit
//     seed, defaults it to a fixed constant rather than anything clock- or
//     address-derived, and serialises calls - see the .cpp for why that matters.
//   * THE VOLUME STORES TRIANGLES. QuadMesh is returned for the one consumer that
//     wants topology (Sculpt/Subdivide); every existing consumer - slicing, convex
//     hull, 3MF - keeps getting an indexed_triangle_set, so nothing downstream has to
//     learn about quads. The GUI stores only the triangulated form.

#include <cstdint>
#include <string>
#include <vector>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// A quad mesh, deliberately minimal: QuadriFlow's output is exactly this and nothing
// in this tree needs more. Kept out of TriangleMesh.hpp so no existing consumer grows
// an opinion about quads.
struct QuadMesh
{
    std::vector<Vec3f> vertices;
    // Each face is four vertex indices, wound consistently with the input's normals.
    // Vec4i32 rather than Vec4i because Point.hpp only defines the sized alias.
    std::vector<Vec4i32> quads;

    bool empty() const { return quads.empty(); }

    // Split every quad into two triangles along its 0-2 diagonal. This is what the
    // volume actually stores: slicing, the convex hull and every file format keep
    // seeing triangles. Vertices are shared, so a watertight quad mesh triangulates
    // to a watertight triangle mesh.
    indexed_triangle_set triangulate() const;
};

// Clamps shared by the dialog and the geometry, so a hand-edited config and a direct
// libslic3r caller land on the same numbers. The upper bound is not a QuadriFlow
// limit; it is where a single remesh stops being interactive on a desktop machine.
static constexpr int      QUAD_REMESH_TARGET_MIN   = 20;
static constexpr int      QUAD_REMESH_TARGET_MAX   = 2000000;
// Fixed, not derived from a clock or an address: see the determinism note above.
static constexpr uint32_t QUAD_REMESH_DEFAULT_SEED = 0;

// What the Quad remesh dialog collects. GUI-free, so libslic3r owns the defaults and
// the clamping and the dialog is only a view onto them - same split as RemeshOptions.
struct QuadRemeshOptions
{
    // Target QUAD count. <= 0 means "use the default", which is the input's triangle
    // count / 2 (see quad_remesh_default_target) - roughly face-count-preserving,
    // since two triangles make a quad.
    int  target_faces = 0;
    // QuadriFlow's -sharp: detect sharp dihedral edges in the input and align the
    // quad field to them, so a cube's twelve edges survive instead of being smoothed.
    bool preserve_sharp = true;
    // Fixed by default on purpose - see the determinism note above. Exposed so a
    // caller who wants a different-but-reproducible tessellation can ask for one.
    uint32_t seed = QUAD_REMESH_DEFAULT_SEED;
};

// target_faces = triangles / 2, clamped. The dialog prefills with this.
int quad_remesh_default_target(const indexed_triangle_set &mesh);

// Why a remesh refused, for the log and the notification. Distinguishing the reasons
// matters: "your mesh has holes" and "QuadriFlow failed" need different answers from
// the user.
enum class QuadRemeshStatus
{
    Ok,
    // The input was empty, or the target clamped to nothing.
    EmptyInput,
    // The input is not a closed manifold: open edges, or more than one shell.
    // QuadriFlow's dedge/hierarchy stage assumes a manifold and produces garbage or
    // asserts otherwise, so this is a refusal rather than an attempt.
    NotManifold,
    // QuadriFlow ran but produced nothing usable.
    Failed,
    // Built without SLIC3R_QUAD_REMESH.
    Unavailable,
};

struct QuadRemeshReport
{
    QuadRemeshStatus status = QuadRemeshStatus::Unavailable;
    size_t triangles_before = 0;
    size_t quads_after      = 0;
    size_t triangles_after  = 0;
    // Filled for every status except Ok; already localised-agnostic English, the GUI
    // wraps it in _L().
    std::string note;
};

// Is `mesh` something quad_remesh() will accept? Closed (no open edges) and a single
// shell. Exposed so the dialog can grey out its OK button with a reason instead of
// letting the user press it and get an error.
bool quad_remesh_accepts(const indexed_triangle_set &mesh, std::string *why = nullptr);

// The remesh. Returns an empty QuadMesh on any refusal, with `report` saying which.
//
// Deterministic: the same (mesh, options) gives a bit-identical result, every run and
// every process.
QuadMesh quad_remesh(const indexed_triangle_set &mesh,
                     const QuadRemeshOptions    &opts,
                     QuadRemeshReport           *report = nullptr);

// Convenience for the GUI, which stores triangles: quad_remesh() then triangulate().
// Returns an empty set on refusal.
indexed_triangle_set quad_remesh_triangulated(const indexed_triangle_set &mesh,
                                              const QuadRemeshOptions    &opts,
                                              QuadRemeshReport           *report = nullptr);

// Compiled with the QuadriFlow dependency? The GUI hides "Quad remesh..." when false.
// A function rather than a bare #ifdef at each call site so GUI code stays readable
// and the false branch still type-checks.
bool quad_remesh_available();

} // namespace Slic3r

#endif // slic3r_QuadRemesh_hpp_
