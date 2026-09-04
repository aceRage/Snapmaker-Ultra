#ifndef slic3r_LayerRegionTextureMapping_hpp_
#define slic3r_LayerRegionTextureMapping_hpp_

#include "libslic3r.h"
#include <vector>
#include "ExPolygon.hpp"
#include "SurfaceCollection.hpp"

namespace Slic3r {

class LayerRegion;
class PrintRegionConfig;
using LayerRegionPtrs = std::vector<LayerRegion*>;

// Returns true when a TextureMapping zone handled perimeter generation for this region.
// Ultra paint-depth PerimeterGenerator flags are still applied inside the TM path.
bool try_make_texture_mapping_perimeters(LayerRegion                 &layer_region,
                                         const SurfaceCollection     &slices,
                                         const LayerRegionPtrs       &compatible_regions,
                                         SurfaceCollection           *fill_surfaces,
                                         ExPolygons                  *fill_no_overlap,
                                         const ExPolygons            *contoning_one_wall_shell_infill,
                                         const PrintRegionConfig     &region_config);

} // namespace Slic3r

#endif
