#ifndef slic3r_MeshRound_hpp_
#define slic3r_MeshRound_hpp_

// Ultra: "Round all edges" - the interim, whole-mesh fillet.
//
// WHY THIS EXISTS, and what it is NOT. The Edit gizmo's real bevel (phase 2,
// MeshEdit.hpp) operates on a SELECTED feature-edge chain and inserts exact
// geometry. That is the tool people actually want. This module is the interim
// one-liner that ships first: it rounds EVERY convex and concave edge of a part
// at once, by a morphological round trip through the signed distance field, and
// it rebuilds the whole mesh to do it.
//
// The algorithm is the classic level-set "rolling ball" fillet, in two halves:
//
//   OPEN  (erode by r, then dilate by r) rounds every CONVEX edge. Eroding pulls
//         the surface inward everywhere; a convex edge's inward offset is a
//         rounded corner of radius r, and dilating back by r restores the flat
//         faces while leaving that rounding behind.
//   CLOSE (dilate by r, then erode by r) does the same for every CONCAVE edge -
//         the inside corners of an L, a pocket, a slot.
//
// Doing open then close gives both in one pass, which is what "round all edges"
// means to a user. openvdb::tools::LevelSetFilter::offset() is the erode/dilate:
// it offsets the level set by a world distance and renormalises, which is
// exactly a morphological dilation (negative offset) / erosion (positive one).
//
// The cost of the round trip is the same as Repair/Remesh's: the mesh is
// re-extracted from a lattice, so triangle counts change, indices are renumbered,
// and detail below the voxel size is lost. That is why the GUI clears painted
// data the way Remesh does, and why "Keep the bottom flat" exists at all - it
// reuses MeshRemesh's split/union so the base slab never enters the grid.
//
// Like MeshRemesh.hpp, this header does NOT include OpenVDB. The rounder is
// injected as a callable, so libslic3r builds (and these tests run) whether or
// not the OpenVDB target exists, and a test can substitute an analytic stub.

#include <cstddef>
#include <functional>
#include <string>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// What the "Round all edges" dialog collects. GUI-free, so libslic3r owns the
// defaults and the clamping and the dialog is only a view onto them.
struct RoundOptions
{
    // Fillet radius in mm. This is the radius of the rolling ball, so a 90 deg
    // edge comes back as a quarter-circle of this radius.
    double radius = 1.0;
    // Voxel size in mm. <= 0 means "derive it from the radius" - see
    // round_auto_voxel_size(). Detail below this is lost, and the fillet itself
    // is only as accurate as the lattice, so the two are coupled.
    double voxel_size = 0.;
    // Round the concave edges too (the CLOSE half). On by default: "all edges"
    // should mean all of them. Turning it off leaves inside corners sharp, which
    // is what you want when the concave corners are print-critical.
    bool   round_concave = true;
    // Cut the base off, round only the part above, and union the untouched slab
    // back on - MeshRemesh's option (ii), reused verbatim so the base stays
    // bit-identical instead of being eroded by up to a voxel.
    bool   keep_bottom_flat = true;
    // Height of that slab above z_min, in mm. <= 0 means
    // ROUND_BOTTOM_MARGIN_RADII * radius, which has to clear the fillet itself
    // rather than just a couple of voxels - see the constant.
    double bottom_margin = 0.;
};

// Clamps shared by the dialog and the geometry, so a hand-edited config and a
// direct libslic3r caller land on the same numbers.
static constexpr double ROUND_RADIUS_MIN = 0.05;
static constexpr double ROUND_RADIUS_MAX = 20.0;
static constexpr double ROUND_VOXEL_MIN  = 0.02;
static constexpr double ROUND_VOXEL_MAX  = 5.0;
static constexpr double ROUND_BOTTOM_MARGIN_MAX = 50.0;
// The narrow band has to be wide enough to hold the whole offset, so the voxel
// size is bounded above by the radius over this. Below about 4 voxels per radius
// the fillet is visibly faceted; below 2 the offset walks off the end of the band
// and the result is garbage.
static constexpr double ROUND_VOXELS_PER_RADIUS = 6.0;
// How wide to make the narrow band, in voxels, on top of the offset distance
// itself. openvdb's default is 3; an offset of r voxels needs r + a margin or
// LevelSetFilter::offset() clips.
static constexpr double ROUND_BAND_MARGIN_VOXELS = 4.0;
// The bottom slab must clear the fillet the round would otherwise put on the base
// edge, so the margin is a multiple of the RADIUS, not of the voxel size. 1.5
// leaves half a radius of straight wall above the fillet's top for the union to
// bite on.
static constexpr double ROUND_BOTTOM_MARGIN_RADII = 1.5;

// The voxel size a radius implies: r / ROUND_VOXELS_PER_RADIUS, clamped. Exposed
// so the dialog can show the number the round would have used on its own.
double round_auto_voxel_size(double radius);

// The rounder, injected. In the application this is always round_by_voxels()
// from MeshRepair.hpp; the tests pass an analytic stub.
//
// Arguments: (mesh, radius, voxel_size, round_concave).
using VoxelRounder = std::function<indexed_triangle_set(const indexed_triangle_set &, double, double, bool)>;

// What a round did, for the log and the completion notification.
struct RoundReport
{
    size_t triangles_before = 0;
    size_t triangles_after  = 0;
    // The radius and voxel size actually used, after clamping and after the
    // auto-voxel derivation.
    double radius_used = 0.;
    double voxel_used  = 0.;
    // The flat-bottom path was asked for AND taken.
    bool   kept_bottom_flat = false;
    // Asked for but declined or failed; `note` says why and the result is the
    // plain whole-mesh round.
    bool   fell_back = false;
    std::string note;
};

// The whole pipeline for one volume's mesh, in the mesh's OWN frame. `bed_dir` is
// the direction of the print bed expressed in that frame - the same convention
// remesh_with_options() uses, so volume_bed_direction() in GUI_ObjectList.cpp
// feeds both. Pass Vec3d(0,0,-1) for the unrotated case; the tests use that.
//
// Returns an empty set on failure (same contract as remesh_by_voxels).
indexed_triangle_set round_with_options(const indexed_triangle_set &mesh,
                                        const RoundOptions         &opts,
                                        const VoxelRounder         &rounder,
                                        const Vec3d                &bed_dir,
                                        RoundReport                *report = nullptr);

// ---------------------------------------------------------------------------
// Measurement helpers - used by the tests, and by the report.
// ---------------------------------------------------------------------------

// The volume a box of `size` loses when every one of its edges and corners is
// rounded at radius r by a rolling ball. Derived by the usual decomposition of
// the rounded box (Minkowski sum of a smaller box with a ball of radius r):
//
//   V(rounded) = (a-2r)(b-2r)(c-2r)                      the core
//              + r * 2[(a-2r)(b-2r) + ...]               the six slabs
//              + (pi r^2 / 4) * 4[(a-2r) + (b-2r) + (c-2r)]   the twelve quarter-
//                                                        cylinders along the edges
//              + (4/3) pi r^3                            the eight corner octants
//
// so the loss against a*b*c is what is left over. Returns 0 when 2r does not fit.
//
// This is the yardstick the "a 20 mm cube at r = 2" test measures against; it is
// here rather than in the test file because the GUI's estimate wants it too.
double rounded_box_volume(const Vec3d &size, double r);
double rounded_box_volume_loss(const Vec3d &size, double r);

// The largest dihedral angle in `mesh`, in degrees, over its manifold edges: 0
// for a perfectly smooth mesh, 90 for a cube. A boundary edge is skipped (a hole
// rim says nothing about how well the round worked). Used by the "no face remains
// sharper than the feature angle" assertion.
double max_dihedral_deg(const indexed_triangle_set &mesh);

// True when every edge of `mesh` is shared by exactly two facets and the mesh has
// no unreferenced or degenerate facet - "watertight and manifold" as the tests
// mean it.
bool is_watertight_manifold(const indexed_triangle_set &mesh);

} // namespace Slic3r

#endif // slic3r_MeshRound_hpp_
