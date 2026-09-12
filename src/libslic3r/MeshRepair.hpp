#ifndef slic3r_MeshRepair_hpp_
#define slic3r_MeshRepair_hpp_

#include <admesh/stl.h>
#include "TriangleMesh.hpp"

namespace Slic3r {

// Ultra: rebuild the mesh as the zero level-set of its signed distance field
// (OpenVDB). The output is guaranteed watertight and manifold regardless of how
// broken the input is; detail smaller than voxel_size (in mm) is lost.
// Returns an empty set on failure. Implemented in OpenVDBUtils.cpp; this header
// stays free of OpenVDB includes so GUI code can use it cheaply.
indexed_triangle_set remesh_by_voxels(const indexed_triangle_set &mesh, double voxel_size);

// Ultra: "Round all edges" - a whole-mesh fillet of radius `radius` (mm) by the
// morphological round trip on the same distance field: OPEN (erode r, dilate r)
// rounds every convex edge, and when `round_concave` is set CLOSE (dilate r,
// erode r) rounds every concave one as well. See MeshRound.hpp for the why; this
// is only the OpenVDB half, kept here so the header stays OpenVDB-free.
//
// Returns an empty set on failure. Like remesh_by_voxels() it re-extracts the
// surface from a lattice, so indices are renumbered and detail below voxel_size
// is lost.
indexed_triangle_set round_by_voxels(const indexed_triangle_set &mesh,
                                     double                      radius,
                                     double                      voxel_size,
                                     bool                        round_concave);

// True when this build has the OpenVDB target, i.e. when remesh_by_voxels() and
// round_by_voxels() do anything at all. The menu hides the "Round all edges"
// entry when it is false rather than offering something that can only ever fail.
bool voxel_ops_available();

} // namespace Slic3r

#endif // slic3r_MeshRepair_hpp_
