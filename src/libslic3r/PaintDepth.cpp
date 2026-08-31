#include "PaintDepth.hpp"

#include <algorithm>

namespace Slic3r {

float paint_depth_band_mm(PaintDepthMode mode, int walls, double mm,
                           float ext_perimeter_width, float perimeter_spacing)
{
    switch (mode) {
    case pdmUnlimited:
        return 0.f;
    case pdmMillimeters:
        return float(mm);
    case pdmWalls:
    default: {
        int clamped_walls = std::max(walls, 1);
        return ext_perimeter_width + float(clamped_walls - 1) * perimeter_spacing;
    }
    }
}

} // namespace Slic3r
