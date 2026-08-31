#include <catch2/catch.hpp>
#include "libslic3r/PaintDepth.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

TEST_CASE("paint_depth_band_mm: unlimited mode is always 0 (disables the clamp)", "[paintdepth]")
{
    CHECK(paint_depth_band_mm(pdmUnlimited, 3, 1.5, 0.45f, 0.42f) == 0.f);
    CHECK(paint_depth_band_mm(pdmUnlimited, 1, 0.0, 0.f, 0.f) == 0.f);
}

TEST_CASE("paint_depth_band_mm: millimeters mode returns mm verbatim", "[paintdepth]")
{
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 3, 1.5, 0.45f, 0.42f), WithinAbs(1.5, 1e-6));
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 99, 0.25, 10.f, 10.f), WithinAbs(0.25, 1e-6));
    // Millimeters mode ignores wall count / flow widths entirely.
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 1, 2.0, 0.f, 0.f), WithinAbs(2.0, 1e-6));
}

TEST_CASE("paint_depth_band_mm: walls mode = ext_perimeter_width + (walls-1)*perimeter_spacing", "[paintdepth]")
{
    // fuzzy-skin precedent: first wall is the external perimeter width, every
    // additional wall adds one perimeter_spacing (MultiMaterialSegmentation.cpp:2237-2253).
    CHECK_THAT(paint_depth_band_mm(pdmWalls, 3, 999.0, 0.45f, 0.42f), WithinAbs(0.45 + 2 * 0.42, 1e-5));
    CHECK_THAT(paint_depth_band_mm(pdmWalls, 1, 999.0, 0.45f, 0.42f), WithinAbs(0.45, 1e-5));
}

TEST_CASE("paint_depth_band_mm: walls mode edge cases", "[paintdepth]")
{
    SECTION("walls clamped to >= 1 for a zero/negative input") {
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 0, 0.0, 0.45f, 0.42f), WithinAbs(0.45, 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, -5, 0.0, 0.45f, 0.42f), WithinAbs(0.45, 1e-5));
    }
    SECTION("zero flow widths collapse the band to 0") {
        CHECK(paint_depth_band_mm(pdmWalls, 3, 0.0, 0.f, 0.f) == 0.f);
    }
}
