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

} // namespace Slic3r

#endif // slic3r_MeshRepair_hpp_
