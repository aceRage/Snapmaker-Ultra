// Ultra: the no-OpenVDB half of MeshRepair.hpp.
//
// OpenVDBUtils.cpp is only compiled when the OpenVDB target exists (see
// src/libslic3r/CMakeLists.txt). Before "Round all edges" that left
// remesh_by_voxels() simply unresolved in an OpenVDB-less build, which meant the
// GUI could not link at all - fine in practice, because every shipping
// configuration has OpenVDB, but it also meant there was no way for a caller to
// ASK whether the voxel operations exist.
//
// This file is the other side of that switch: when OpenVDB is absent it supplies
// the same symbols, each returning "nothing happened", and voxel_ops_available()
// answers false. The menu uses that answer to hide "Round all edges" rather than
// offering an entry that can only ever report a failure.
//
// It is never compiled at the same time as OpenVDBUtils.cpp.

#include "MeshRepair.hpp"

namespace Slic3r {

indexed_triangle_set remesh_by_voxels(const indexed_triangle_set &, double) { return {}; }

indexed_triangle_set round_by_voxels(const indexed_triangle_set &, double, double, bool) { return {}; }

bool voxel_ops_available() { return false; }

} // namespace Slic3r
