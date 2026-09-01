#include "PaintDepth.hpp"

#include <algorithm>

namespace Slic3r {

float paint_depth_band_mm(PaintDepthMode mode, int walls, double mm,
                           float ext_perimeter_width, float ext_perimeter_spacing,
                           float perimeter_spacing)
{
    switch (mode) {
    case pdmUnlimited:
        return 0.f;
    case pdmMillimeters:
        return float(mm);
    case pdmWalls:
    default: {
        // Fix-wave F3 - see the header comment for the derivation of each of the three terms.
        const int clamped_walls = std::max(walls, 1);
        const float band = float(clamped_walls) * perimeter_spacing                    // N bead pitches
                         + 2.f * (ext_perimeter_width - ext_perimeter_spacing)          // Arachne's pre-inset
                         + 0.25f * perimeter_spacing;                                   // count-window margin
        // A degenerate flow (all-zero widths, or a spacing that somehow exceeds its own
        // width) must collapse to "disabled", never to a negative band that would make
        // offset_ex() grow the keep-core instead of shrinking it.
        return std::max(0.f, band);
    }
    }
}

float paint_depth_band_classic_floor_mm(float band, float ext_perimeter_width, float ext_perimeter_spacing)
{
    // Wave A / item 8 - see the header comment. "Disabled" (unlimited mode, or an explicit zero
    // millimetre depth) must stay disabled; a degenerate flow has no floor to offer.
    if (band <= 0.f)
        return band;
    const float wall_stack = ext_perimeter_width + ext_perimeter_spacing;
    return wall_stack > 0.f ? std::max(band, wall_stack) : band;
}

float paint_depth_interlocking_depth_mm(PaintDepthMode mode, double configured_depth, float perimeter_spacing)
{
    // Fix-wave F4 - see the header comment. Nothing to clamp against without a real spacing,
    // and 0 stays 0 (the option's "disabled" convention).
    if (configured_depth <= 0. || perimeter_spacing <= 0.f)
        return float(configured_depth);
    // Wave A / I-3: the cap protects the WALLS-mode bead-count window and nothing else. In
    // millimetres mode the band is the user's literal depth and carries no count contract, so
    // their configured notch is honoured verbatim.
    if (mode != pdmWalls)
        return float(configured_depth);
    return std::min(float(configured_depth), 0.25f * perimeter_spacing);
}

} // namespace Slic3r
