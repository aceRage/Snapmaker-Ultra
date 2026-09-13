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
    // Keep the part sitting flat on the plate instead of letting the round lift
    // its outline off the bed. TWO different mechanisms, chosen by bottom_margin:
    // see that field.
    bool   keep_bottom_flat = true;
    // Height of the unrounded slab kept at the base, in mm. This is the field the
    // owner's 2026-09-12 feedback is about, so the two modes are spelled out:
    //
    //   0 (the DEFAULT) - the MIRROR mode. No slab at all: the mesh is reflected
    //       across its own base plane before the voxel round trip, so the bottom
    //       face sits in the INTERIOR of the solid the rounder sees and no fillet
    //       can form on it, while the vertical edges round straight through the
    //       plane. The result is cut back at z_min and capped, which leaves a
    //       perfectly flat base with a sharp 90 deg perimeter and a footprint whose
    //       XY corners are rounded by the radius - the fillet running all the way
    //       down, which is what "round all edges" should look like on a part that
    //       still has to sit on a bed.
    //   > 0 - the SLAB mode, today's behaviour: the base band of that height is cut
    //       off, never enters the grid, and is unioned back on afterwards. It leaves
    //       a straight, UNrounded lip of that height at the bottom (the "pedestal"
    //       in the screenshot). Kept because a lip is occasionally what someone
    //       wants, but it is no longer the default.
    //
    // Note the polarity change: <= 0 used to mean "derive a default slab from the
    // radius" and now means "no slab, mirror instead". ROUND_BOTTOM_MARGIN_RADII
    // survives as the value the dialog offers when someone turns a slab back on.
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
// A bottom slab, when someone asks for one, must clear the fillet the round would
// otherwise put on the base edge, so its height is a multiple of the RADIUS, not of
// the voxel size. 1.5 leaves half a radius of straight wall above the fillet's top
// for the union to bite on. This is no longer a DEFAULT (the default slab height is
// 0 - see RoundOptions::bottom_margin); it is the height the dialog suggests when a
// slab is switched back on.
static constexpr double ROUND_BOTTOM_MARGIN_RADII = 1.5;
// Mirror mode: how far below the base plane the reflected copy has to reach for the
// base face to be safely interior to the grid. The fillet on a mirrored convex edge
// is a radius tall, so ONE radius only makes the base plane tangent to the top of
// that fillet - exactly the case where a voxel of error either way shows. Two radii
// puts a clear radius of straight wall between the fillet and the cut, which is the
// same reasoning (and the same margin) the slab mode's ROUND_BOTTOM_MARGIN_RADII
// uses for its cut plane. The voxel term covers a tiny radius, where the narrow
// band rather than the fillet is what smears across the plane; the depth used is the
// larger of the two.
static constexpr double ROUND_MIRROR_DEPTH_RADII  = 2.0;
static constexpr double ROUND_MIRROR_DEPTH_VOXELS = 6.0;
// After the mirrored result is cut at the base plane, floating-point noise can leave
// a cap vertex a hair below it. Anything within this many voxels of the plane is
// snapped exactly onto it, so "no vertex below z_min" is true by construction rather
// than by luck; anything further down is a real failure and is reported as one.
static constexpr double ROUND_BASE_SNAP_VOXELS = 0.75;

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
    // Which mechanism did it: true for the mirror (bottom_margin == 0), false for
    // the slab. Only meaningful when kept_bottom_flat is set.
    bool   mirrored_base    = false;
    // The slab height actually used, 0 in mirror mode.
    double bottom_margin_used = 0.;
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

// The zero-slab (mirror) path, exposed for the unit tests.
//
// Reflects `mesh` across its own base plane (the plane through its lowest point,
// normal -bed_dir), merges the reflection back in so the rounder sees one solid
// whose base face is interior, rounds that, then cuts the result at the base plane
// and caps it. The reflected copy only has to reach `depth` below the plane for the
// base to be interior, so it is itself cut off there rather than running the whole
// height of the part - a 200 mm tall part does not pay for a 400 mm tall grid.
//
// Returns an empty set and sets `*why` when the path cannot be taken (no flat
// bottom, degenerate height, the rounder failed, or the cut left a vertex below the
// plane) - the caller then falls back to the plain whole-mesh round.
indexed_triangle_set round_mirror_flat_bottom(const indexed_triangle_set &mesh,
                                              double                      radius,
                                              double                      voxel_size,
                                              bool                        round_concave,
                                              const VoxelRounder         &rounder,
                                              const Vec3d                &bed_dir,
                                              std::string                *why = nullptr);

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

// Area of the facets of `mesh` that lie in the plane `height` above the bed (within
// `eps`) and face the bed, projected onto the bed plane. This is the base cap's area
// - the footprint of the first layer - and is what the zero-slab tests measure to
// show the XY corners came back rounded by the radius rather than square or eroded.
double round_base_cap_area(const indexed_triangle_set &mesh,
                           const Vec3d                &bed_dir,
                           double                      eps);

// The largest dihedral angle, in degrees, over the edges of `mesh` that lie in the
// base plane (within `eps`) - the bottom PERIMETER. A sharp 90 deg base reads as
// 90 here: every facet touching the plane is either the cap itself (normal along
// bed_dir) or a wall rising from it (normal perpendicular to bed_dir), and nothing
// in between, so the worst angle across such an edge is exactly 90. A fillet on the
// bottom edge would break it into a run of shallow steps and pull this WELL below
// 90, which is the asymmetry the test exploits: it asserts the angle is 90, not
// that it is small.
//
// `max_off_axis_deg` out-parameter: the worst deviation of any base-touching facet's
// normal from either pure -bed_dir or pure horizontal, in degrees. 0 for a sharp
// base; a filleted one has facets at every intermediate angle and this goes large.
double round_base_perimeter_dihedral_deg(const indexed_triangle_set &mesh,
                                         const Vec3d                &bed_dir,
                                         double                      eps,
                                         double                     *max_off_axis_deg = nullptr);

// The cross-section of `mesh` at `height` above the bed, as its area and its
// bounding box in the bed plane. Used to compare the footprint at the base with the
// section at mid-height: the zero-slab round has to give the SAME corner-rounded
// outline at both, which is what "the vertical edges' rounding continues all the way
// down" means numerically. Returns false when the plane misses the mesh.
bool round_section_at_height(const indexed_triangle_set &mesh,
                             const Vec3d                &bed_dir,
                             double                      height,
                             double                     *area,
                             double                     *bbox_size_u = nullptr,
                             double                     *bbox_size_v = nullptr);

// True when no two facets of `mesh` intersect each other away from a shared vertex
// or edge. Deliberately the honest O(n^2)-with-a-grid test rather than a library
// call, because the thing the zero-slab path can plausibly get wrong is exactly a
// self-intersection at the cut plane.
bool has_no_self_intersections(const indexed_triangle_set &mesh);

} // namespace Slic3r

#endif // slic3r_MeshRound_hpp_
