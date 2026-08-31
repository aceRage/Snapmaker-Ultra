#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "libslic3r/PaintDepth.hpp"
#include "libslic3r/PrintConfig.hpp"

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

// Fix-wave F2 (docs/superpowers/sdd/2026-08-31-paint-depth/final-review.md): a legacy
// mmu_segmented_region_max_width migration that leaves the old key nonzero re-arms
// handle_legacy_composite's `!config.has("paint_depth_mode")` guard the next time a
// diff-serialized preset is loaded, because a diff save only writes keys that differ
// from defaults - if the user reverts paint_depth_mode back to its default (walls) and
// saves, paint_depth_mode drops out of the diff (it now equals the default) but a
// still-nonzero legacy key would not, silently re-triggering the migration back to
// millimeters/<value> on every future load.
//
// This test reproduces that scenario end to end at the config level, using bare
// (sparse) `DynamicPrintConfig` objects throughout - not `full_print_config()` - because
// that is what a real preset actually is: Preset.cpp's own load path
// (PresetCollection::load_preset, `DynamicPrintConfig config;`) starts from an empty
// map and only gains the keys the ini file (or, here, migration/diff logic) actually
// sets. A `full_print_config()`-based config already carries every key with its default
// value from construction, so `!config.has("paint_depth_mode")` would never fire on it
// regardless of this fix - that would test nothing.
TEST_CASE("PrintConfigDef::handle_legacy_composite: reverting to paint_depth_mode=walls and re-saving does not re-migrate on the next load", "[paintdepth]")
{
    // Step 1: legacy preset load - a lone nonzero mmu_segmented_region_max_width, the
    // exact shape a pre-paint-depth-feature preset file has. load_from_ini_string() goes
    // through the real ConfigBase::load() path, which calls handle_legacy_composite()
    // once after every key in the file is deserialized - same as opening an ini/Orca
    // preset file for real.
    DynamicPrintConfig loaded; // bare/sparse, matching Preset.cpp's real preset config
    loaded.load_from_ini_string("mmu_segmented_region_max_width = 0.8\n", ForwardCompatibilitySubstitutionRule::Disable);

    REQUIRE(loaded.has("paint_depth_mode"));
    REQUIRE(loaded.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value == pdmMillimeters);
    REQUIRE(loaded.has("paint_depth_mm"));
    REQUIRE(std::abs(loaded.option<ConfigOptionFloat>("paint_depth_mm")->value - 0.8) < 1e-6);
    // The fix itself: the old key must be neutralized once migrated, or it survives as a
    // nonzero straggler that the rest of this test would otherwise catch re-arming things.
    REQUIRE(loaded.has("mmu_segmented_region_max_width"));
    CHECK(std::abs(loaded.option<ConfigOptionFloat>("mmu_segmented_region_max_width")->value) < 1e-6);

    // Step 2: user explicitly switches back to walls (paint_depth_mode's own default).
    loaded.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value = pdmWalls;

    // Step 3: diff-serialized save - a real preset only ever writes keys that differ from
    // its parent/defaults (Preset.cpp), so reproduce exactly that: the set of keys where
    // `loaded` differs from a fresh default config. DynamicConfig::diff() only compares
    // keys present in BOTH configs ("ignoring options not present in both configs" - its
    // own doc comment, Config.cpp), so this naturally mirrors "only the touched keys are
    // candidates for the diff" without needing `loaded` to carry every schema key.
    DynamicPrintConfig   defaults  = DynamicPrintConfig::full_print_config();
    t_config_option_keys diff_keys = loaded.diff(defaults);
    // paint_depth_mode is walls again (the default), so it must NOT appear in the diff -
    // if it did, the scenario this test targets couldn't happen in the first place.
    CHECK(std::find(diff_keys.begin(), diff_keys.end(), "paint_depth_mode") == diff_keys.end());
    // mmu_segmented_region_max_width must also be out of the diff (it's 0, the default) -
    // this is the F2 fix actually taking effect; pre-fix, this key would still be 0.8 and
    // WOULD appear here (and is exactly what re-arms the guard in step 4 below).
    CHECK(std::find(diff_keys.begin(), diff_keys.end(), "mmu_segmented_region_max_width") == diff_keys.end());

    std::string diff_ini;
    for (const std::string &key : diff_keys)
        diff_ini += key + " = " + loaded.option(key)->serialize() + "\n";

    // Step 4: reload from that diff blob, exactly as re-opening the saved preset would -
    // through the same sparse load_from_ini_string() -> handle_legacy_composite() path as
    // step 1 (Preset.cpp loads a preset's own file into a bare config the same way, then
    // separately merges it onto the parent/system preset's full config - reproduced here
    // by applying the loaded diff onto a fresh full_print_config() baseline).
    DynamicPrintConfig diff_config; // bare/sparse, same reasoning as `loaded` above
    diff_config.load_from_ini_string(diff_ini, ForwardCompatibilitySubstitutionRule::Disable);

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.apply(diff_config);

    // Must land back on walls - NOT be silently reverted to millimeters/0.8 by a
    // re-triggered migration.
    CHECK(reloaded.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value == pdmWalls);
}
