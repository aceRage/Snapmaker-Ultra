#ifndef slic3r_MeshRemesh_hpp_
#define slic3r_MeshRemesh_hpp_

// Ultra: the geometry half of "Repair by remeshing".
//
// remesh_by_voxels() (MeshRepair.hpp, implemented in OpenVDBUtils.cpp) rebuilds a
// mesh as the zero level-set of its signed distance field. That is robust, but it
// is also lossy in two ways the owner cares about:
//
//   * a flat base comes back rippled and its outline eroded by up to a voxel,
//     because volumeToMesh extracts the zero crossing on a fixed lattice, and
//   * a sharp dihedral edge comes back filleted, because the 3-voxel narrow band
//     averages both incident faces together within about one voxel of the edge.
//
// The two post-passes here address each of those. Both are pure geometry on an
// indexed_triangle_set so they can be unit-tested without a GUI or a plater:
//
//   remesh_keep_flat_bottom() - cut the ORIGINAL mesh at z_min + delta, voxel-remesh
//       only the part above, and boolean-union the result back onto the untouched
//       bottom slab. The slab never passes through OpenVDB, so the base is
//       bit-identical to the input.
//   snap_to_sharp_edges() - pull remeshed vertices that sit within one voxel of an
//       original sharp dihedral edge back onto that edge's line.
//
// This header deliberately does NOT include OpenVDB: the voxel remesher is passed
// in as a callable, so libslic3r builds (and the tests run) whether or not the
// OpenVDB target exists, and a test can substitute a stub remesher.

#include <cstdint>
#include <functional>
#include <vector>

#include "Point.hpp"
#include "TriangleMesh.hpp"

namespace Slic3r {

// What the Remesh dialog collects. GUI-free so libslic3r owns the defaults and the
// clamping, and the dialog is only a view onto them.
struct RemeshOptions
{
    // Voxel size in mm. <= 0 means "use the auto value" - see auto_voxel_size().
    double voxel_size = 0.;
    // Option (ii): cut, remesh the top, union the untouched bottom slab back on.
    bool   keep_bottom_flat = true;
    // Height of that slab above the mesh's z_min, in mm. <= 0 means
    // BOTTOM_MARGIN_VOXELS * voxel_size.
    double bottom_margin = 0.;
    // Post-pass that snaps remeshed vertices back onto the original sharp edges.
    bool   preserve_sharp_edges = true;
    // Dihedral angle (degrees) above which an edge counts as sharp.
    double sharp_feature_angle = 45.;
};

// Clamps used by both the dialog and the geometry, so a hand-edited config and a
// direct libslic3r caller land on the same numbers.
static constexpr double REMESH_VOXEL_MIN         = 0.02;
static constexpr double REMESH_VOXEL_MAX         = 5.0;
static constexpr double REMESH_ANGLE_MIN         = 5.0;
static constexpr double REMESH_ANGLE_MAX         = 175.0;
static constexpr double REMESH_BOTTOM_MARGIN_MAX = 50.0;
// Default bottom slab height, in voxels. The spec's option (ii) recommends 2-3x the
// voxel size so the cut plane clears the fillet the narrow band would otherwise
// bake into the base.
static constexpr double REMESH_BOTTOM_MARGIN_VOXELS = 2.5;
// A vertex counts as "on the base" when it is within this fraction of a voxel of
// z_min. Used by the flat-bottom verification and by the tests.
static constexpr double REMESH_BASE_EPS_VOXELS = 0.05;

// Today's auto voxel size, unchanged: the bounding box diagonal over 300, clamped
// to [0.05, 0.3] mm. Kept here (rather than inline in the GUI) so the dialog can
// show the same number the remesh would have used on its own.
double remesh_auto_voxel_size(const indexed_triangle_set &mesh);

// The voxel remesher, injected. In the application this is always
// remesh_by_voxels() from MeshRepair.hpp; the tests pass a stub so the geometry can
// be exercised without OpenVDB.
using VoxelRemesher = std::function<indexed_triangle_set(const indexed_triangle_set &, double)>;

// What a remesh did, for the log and the completion notification.
struct RemeshReport
{
    size_t triangles_before = 0;
    size_t triangles_after  = 0;
    // The flat-bottom path was asked for AND taken.
    bool   kept_bottom_flat = false;
    // The flat-bottom path was asked for but declined or failed; `note` says why and
    // the result is the plain whole-mesh remesh.
    bool   fell_back        = false;
    // How many vertices the sharp-edge post-pass moved.
    size_t sharp_snapped    = 0;
    std::string note;
};

// The whole Phase 1 pipeline for one volume's mesh, in the mesh's OWN frame. `bed_z`
// is the direction of the print bed expressed in that frame (see remesh_volume() in
// GUI_ObjectList.cpp): the "bottom" is the side the bed is on, which is only the
// mesh's -Z when the volume's world transform has no rotation. Pass Vec3d(0,0,-1)
// for the unrotated case; the tests use that.
//
// Returns an empty set on failure (same contract as remesh_by_voxels).
indexed_triangle_set remesh_with_options(const indexed_triangle_set &mesh,
                                         const RemeshOptions        &opts,
                                         const VoxelRemesher        &remesher,
                                         const Vec3d                &bed_dir,
                                         RemeshReport               *report = nullptr);

// ---------------------------------------------------------------------------
// The two post-passes, exposed for the unit tests.
// ---------------------------------------------------------------------------

// Option (ii). Cuts `mesh` at z_min + margin along -bed_dir, remeshes only the upper
// part, and unions it back onto the untouched lower slab (MeshBoolean::mfd first,
// mcut as fallback).
//
// Returns an empty set and sets `*why` when the split is not worth taking or the
// union failed - the caller then falls back to a whole-mesh remesh. Declines when:
//   * the mesh has no flat bottom at all (nothing to protect),
//   * the bottom is only a handful of triangles (the slab would be degenerate),
//   * the cut leaves an empty upper or lower part.
indexed_triangle_set remesh_keep_flat_bottom(const indexed_triangle_set &mesh,
                                             double                      voxel_size,
                                             double                      margin,
                                             const VoxelRemesher        &remesher,
                                             const Vec3d                &bed_dir,
                                             std::string                *why = nullptr);

// One sharp edge of the original mesh, as a segment. `snap_to_sharp_edges` pulls
// nearby remeshed vertices onto the infinite line through a and b, clamped to the
// segment, so a vertex near a corner lands on the corner rather than past it.
struct SharpEdge
{
    Vec3f a{Vec3f::Zero()};
    Vec3f b{Vec3f::Zero()};
};

// The dihedral detection from SculptSession::detect_sharp_edges, factored out as a
// free function on edges rather than vertices: for every manifold edge shared by two
// faces, the edge is sharp when the two face normals differ by more than
// `dihedral_deg`. A boundary edge (one incident face) is always sharp - a hole rim
// is a feature line too.
std::vector<SharpEdge> detect_sharp_edges(const indexed_triangle_set &mesh, double dihedral_deg);

// Per-vertex version, matching SculptSession::detect_sharp_edges()'s contract: true
// for a vertex with two incident faces differing by more than the threshold. Shared
// so Sculpt and Remesh agree on what "sharp" means.
std::vector<bool> sharp_vertex_mask(const indexed_triangle_set &mesh, double dihedral_deg);

// Move every vertex of `its` that lies within `radius` of one of `edges` onto the
// nearest point of that edge. Bounded on purpose: `radius` is one voxel, so a vertex
// the remesh moved further than that is left where it is instead of being dragged
// across the surface. Returns how many vertices moved.
size_t snap_to_sharp_edges(indexed_triangle_set       &its,
                           const std::vector<SharpEdge> &edges,
                           double                        radius);

// ---------------------------------------------------------------------------
// Small helpers the tests share.
// ---------------------------------------------------------------------------

// Signed "height above the bed": -bed_dir . v, so the base is the minimum. bed_dir
// need not be normalised.
double remesh_height(const Vec3d &bed_dir, const Vec3f &v);

// Area of the mesh's base facets (those whose normal faces the bed and which lie
// within eps of z_min), projected onto the bed plane. 0 when there is no flat
// bottom. This is what "has a flat bottom" means throughout this module.
double remesh_bottom_area(const indexed_triangle_set &mesh,
                          const Vec3d                &bed_dir,
                          double                      eps,
                          size_t                     *facet_count = nullptr);

} // namespace Slic3r

#endif // slic3r_MeshRemesh_hpp_
