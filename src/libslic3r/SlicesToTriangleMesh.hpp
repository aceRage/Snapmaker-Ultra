#ifndef SLICESTOTRIANGLEMESH_HPP
#define SLICESTOTRIANGLEMESH_HPP

#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/ExPolygon.hpp"

namespace Slic3r {

// The loft the two overloads below are both built on: `grid[i]` is the TOP of slice i's band,
// and `zmin` is the bottom of slice 0's. It was already the implementation; it is declared here
// so a caller whose layer heights are not constant (slice baking, adaptive or variable layer
// height, a layer subset that does not start at the bed) can hand over the real per-layer Z
// values instead of having a uniform grid derived for it.
indexed_triangle_set slices_to_mesh(const std::vector<ExPolygons> &slices,
                                    double                         zmin,
                                    const std::vector<float>      &grid);

void slices_to_mesh(indexed_triangle_set &         mesh,
                    const std::vector<ExPolygons> &slices,
                    double                         zmin,
                    double                         lh,
                    double                         ilh);

inline indexed_triangle_set slices_to_mesh(
    const std::vector<ExPolygons> &slices, double zmin, double lh, double ilh)
{
    indexed_triangle_set out;
    slices_to_mesh(out, slices, zmin, lh, ilh);

    return out;
}

} // namespace Slic3r

#endif // SLICESTOTRIANGLEMESH_HPP
