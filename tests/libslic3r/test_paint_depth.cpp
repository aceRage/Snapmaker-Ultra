#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <string>

#include "libslic3r/Flow.hpp"
#include "libslic3r/PaintDepth.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

namespace {

// The stock flow the wall-count investigation's arithmetic is quoted against: a 0.45mm line
// at a 0.1mm layer height. Derived through Flow's own spacing definition (Flow.cpp:182-184,
// spacing = width - height * (1 - pi/4)) rather than a hardcoded literal, so these pins track
// the production definition instead of drifting from it.
constexpr float STOCK_LINE_WIDTH  = 0.45f;
constexpr float STOCK_LAYER_HEIGHT = 0.1f;

float stock_spacing() { return Flow::rounded_rectangle_extrusion_spacing(STOCK_LINE_WIDTH, STOCK_LAYER_HEIGHT); }

// The band the fix-wave F3 formula is specified to produce - written out here as the SPEC,
// independent of the production helper, so the pins below actually discriminate.
double expected_band(int walls, float ext_w, float ext_s, float s)
{
    return double(walls) * double(s) + 2.0 * (double(ext_w) - double(ext_s)) + 0.25 * double(s);
}

} // namespace

TEST_CASE("paint_depth_band_mm: unlimited mode is always 0 (disables the clamp)", "[paintdepth]")
{
    CHECK(paint_depth_band_mm(pdmUnlimited, 3, 1.5, 0.45f, 0.43f, 0.42f) == 0.f);
    CHECK(paint_depth_band_mm(pdmUnlimited, 1, 0.0, 0.f, 0.f, 0.f) == 0.f);
}

TEST_CASE("paint_depth_band_mm: millimeters mode returns mm verbatim", "[paintdepth]")
{
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 3, 1.5, 0.45f, 0.43f, 0.42f), WithinAbs(1.5, 1e-6));
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 99, 0.25, 10.f, 10.f, 10.f), WithinAbs(0.25, 1e-6));
    // Millimeters mode ignores wall count / flow widths entirely.
    CHECK_THAT(paint_depth_band_mm(pdmMillimeters, 1, 2.0, 0.f, 0.f, 0.f), WithinAbs(2.0, 1e-6));
}

// Fix-wave F3 (.superpowers/sdd/2026-08-31-paint-depth/wall-count-investigation.md section 5):
// the band is now sized in whole bead PITCHES plus Arachne's precise_outer_wall pre-inset plus
// a quarter-spacing count-window margin, replacing `ext_w + (N-1)*s`. See
// paint_depth_band_mm's header comment for the derivation of each term.
TEST_CASE("paint_depth_band_mm: walls mode = N*spacing + 2*(ext_w - ext_s) + 0.25*spacing", "[paintdepth]")
{
    const float s     = stock_spacing();
    const float ext_s = s; // outer and inner line widths are equal at stock settings
    const float ext_w = STOCK_LINE_WIDTH;

    SECTION("matches the specified formula for N = 1, 3, 6") {
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 1, 999.0, ext_w, ext_s, s), WithinAbs(expected_band(1, ext_w, ext_s, s), 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 3, 999.0, ext_w, ext_s, s), WithinAbs(expected_band(3, ext_w, ext_s, s), 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 6, 999.0, ext_w, ext_s, s), WithinAbs(expected_band(6, ext_w, ext_s, s), 1e-5));
    }

    SECTION("the investigation's quoted absolute values at 0.45mm lines / 0.1mm layers") {
        // Pinned as literals as well as symbolically: these are the numbers the fix wave was
        // signed off against (old band(3) was 1.307080, which left only 0.083mm of downward
        // margin before Arachne dropped from 3 beads to 2).
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 1, 0.0, ext_w, ext_s, s), WithinAbs(0.578595, 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 3, 0.0, ext_w, ext_s, s), WithinAbs(1.435675, 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 6, 0.0, ext_w, ext_s, s), WithinAbs(2.721294, 1e-5));
    }

    SECTION("every additional wall adds exactly one bead pitch") {
        const float band_2 = paint_depth_band_mm(pdmWalls, 2, 0.0, ext_w, ext_s, s);
        const float band_3 = paint_depth_band_mm(pdmWalls, 3, 0.0, ext_w, ext_s, s);
        const float band_4 = paint_depth_band_mm(pdmWalls, 4, 0.0, ext_w, ext_s, s);
        CHECK_THAT(band_3 - band_2, WithinAbs(s, 1e-5));
        CHECK_THAT(band_4 - band_3, WithinAbs(s, 1e-5));
    }
}

TEST_CASE("paint_depth_band_mm: walls mode edge cases", "[paintdepth]")
{
    const float s     = stock_spacing();
    const float ext_w = STOCK_LINE_WIDTH;

    SECTION("walls clamped to >= 1 for a zero/negative input") {
        CHECK_THAT(paint_depth_band_mm(pdmWalls, 0, 0.0, ext_w, s, s), WithinAbs(expected_band(1, ext_w, s, s), 1e-5));
        CHECK_THAT(paint_depth_band_mm(pdmWalls, -5, 0.0, ext_w, s, s), WithinAbs(expected_band(1, ext_w, s, s), 1e-5));
    }
    SECTION("zero flow widths collapse the band to 0") {
        CHECK(paint_depth_band_mm(pdmWalls, 3, 0.0, 0.f, 0.f, 0.f) == 0.f);
    }
    SECTION("a degenerate flow whose spacing exceeds its width never produces a negative band") {
        CHECK(paint_depth_band_mm(pdmWalls, 1, 0.0, 0.1f, 0.5f, 0.f) >= 0.f);
    }
}

// Wave A / item 8 (classic-generator-investigation.md sections 2b/2c/3/6): the classic wall
// generator's floor for the band. See paint_depth_band_classic_floor_mm's header comment for the
// three defects it closes at paint_depth_walls = 1 and for why it must stay classic-only.
TEST_CASE("paint_depth_band_classic_floor_mm: the classic band is floored at one wall stack", "[paintdepth]")
{
    const float s     = stock_spacing(); // 0.428540 at 0.45mm lines / 0.1mm layers
    const float ext_w = STOCK_LINE_WIDTH;
    const float ext_s = s;
    const float wall_stack = ext_w + ext_s; // 0.878540

    SECTION("N=1 is the band the floor exists for") {
        const float band = paint_depth_band_mm(pdmWalls, 1, 0.0, ext_w, ext_s, s);
        CHECK_THAT(double(band), WithinAbs(0.578595, 1e-5));
        CHECK_THAT(double(paint_depth_band_classic_floor_mm(band, ext_w, ext_s)), WithinAbs(double(wall_stack), 1e-5));
        // The floor equals F1's own top/bottom inset exactly, which is the point: the lateral band
        // and the projected top/bottom claim then meet, instead of leaving a 0.299945mm base ring
        // that classic prints as nothing.
        CHECK_THAT(double(wall_stack), WithinAbs(0.878540, 1e-5));
    }
    SECTION("every band at N >= 2 is already above the floor and passes through untouched") {
        for (int walls = 2; walls <= 6; ++walls) {
            const float band = paint_depth_band_mm(pdmWalls, walls, 0.0, ext_w, ext_s, s);
            CHECK(band > wall_stack);
            CHECK_THAT(double(paint_depth_band_classic_floor_mm(band, ext_w, ext_s)), WithinAbs(double(band), 1e-6));
        }
    }
    SECTION("a millimetres-mode band is floored the same way (classic's limitation is geometric, not modal)") {
        CHECK_THAT(double(paint_depth_band_classic_floor_mm(0.4f, ext_w, ext_s)), WithinAbs(double(wall_stack), 1e-5));
        CHECK_THAT(double(paint_depth_band_classic_floor_mm(2.0f, ext_w, ext_s)), WithinAbs(2.0, 1e-6));
    }
    SECTION("'disabled' stays disabled - flooring a zero band would switch the clamp back on") {
        CHECK(paint_depth_band_classic_floor_mm(0.f, ext_w, ext_s) == 0.f);
        CHECK(paint_depth_band_classic_floor_mm(paint_depth_band_mm(pdmUnlimited, 3, 1.5, ext_w, ext_s, s), ext_w, ext_s) == 0.f);
    }
    SECTION("a degenerate (all-zero) flow offers no floor") {
        CHECK_THAT(double(paint_depth_band_classic_floor_mm(1.5f, 0.f, 0.f)), WithinAbs(1.5, 1e-6));
    }
}

// Fix-wave F4 (.superpowers/sdd/2026-08-31-paint-depth/wall-count-investigation.md section 3):
// the even-layer interlocking notch must fit INSIDE the band's own count-window margin, or it
// costs the painted region a wall loop on every even layer (the reported 3/2/3/2 alternation).
TEST_CASE("paint_depth_interlocking_depth_mm: the notch is capped at a quarter of one perimeter spacing", "[paintdepth]")
{
    const float s = stock_spacing(); // 0.428540 at 0.45mm lines / 0.1mm layers
    const float cap = 0.25f * s;     // 0.107135

    SECTION("the old 0.3mm default is clamped down to the cap") {
        // 0.3mm is ~0.70 * spacing - 3.6x the 0.083mm of margin the pre-F3 band had.
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmWalls, 0.3, s), WithinAbs(cap, 1e-6));
    }
    SECTION("the new 0.1mm default is already under the cap and passes through untouched") {
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmWalls, 0.1, s), WithinAbs(0.1, 1e-6));
    }
    SECTION("zero stays zero (the option's own 'disabled' convention)") {
        CHECK(paint_depth_interlocking_depth_mm(pdmWalls, 0.0, s) == 0.f);
    }
    SECTION("a degenerate (non-positive) spacing carries no information to clamp against") {
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmWalls, 0.3, 0.f), WithinAbs(0.3, 1e-6));
    }
    // Wave A / I-3: the cap's entire justification is the WALLS-mode bead-count window. In
    // millimetres mode the band is the user's literal depth, no count contract exists, and a
    // hand-set mechanical key must not be silently cut to a quarter spacing (0.5 -> 0.107mm,
    // 4.7x less, for a reason that does not apply). Pinned as the arithmetic twin of the
    // end-to-end RED in test_paint_depth_clamp.cpp.
    SECTION("millimetres mode is not capped - there is no bead-count window to protect") {
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmMillimeters, 0.5, s), WithinAbs(0.5, 1e-6));
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmMillimeters, 0.3, s), WithinAbs(0.3, 1e-6));
        // ...and the sub-cap default still passes through, identically in both modes.
        CHECK_THAT(paint_depth_interlocking_depth_mm(pdmMillimeters, 0.1, s), WithinAbs(0.1, 1e-6));
        CHECK(paint_depth_interlocking_depth_mm(pdmMillimeters, 0.0, s) == 0.f);
    }
}

// The F3 + F4 contract stated as one arithmetic invariant, for every wall count the option
// allows in practice: after the even-layer notch is subtracted, the band STILL covers the exact
// N-bead optimum thickness `N*spacing + 2*(ext_w - ext_s)`. That is what makes both layer
// parities land in the same Arachne bead-count window - "N walls" delivers N loops on every
// layer, not N on odd and N-1 on even.
//
// I-4 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md): "N beads fit
// on both parities" is a TWO-sided condition. Arachne gives exactly N beads iff
// `x = T - 2*s_ext` lies in `[(N-3+thr(N-3))*s, (N-2+thr(N-2))*s)`
// (RedistributeBeadingStrategy.cpp:42-49, DistributedBeadingStrategy.cpp:92-98, thresholds at
// WallToolPaths.cpp:510-511). F3 puts `x` at `(N-1.75)*s`, whose margins are
//
//     odd  N (N=3):  0.4944*s = 0.2119mm down,  0.2611*s = 0.1119mm UP
//     even N (N=4):  0.7389*s = 0.3167mm down,  0.5056*s = 0.2167mm UP
//
// so the SMALLEST number anywhere in the design is the 0.1119mm upward margin at odd N - the
// direction the original assertion did not bound at all. A future change that widened the band
// by 0.12mm (a different flow model, a min_bead_width profile change, an extra safety term)
// would silently turn "3 walls" into 4 with the whole suite still green. The upper bounds below
// close that: the band is pinned to EXACTLY one quarter-spacing above the N-bead optimum, and
// the notch is pinned to only ever eat INTO that quarter-spacing, never to widen past it.
TEST_CASE("paint depth band: the even-layer interlocking notch never eats into the N-bead budget", "[paintdepth]")
{
    const float s     = stock_spacing();
    const float ext_w = STOCK_LINE_WIDTH;
    const float ext_s = s;
    // The shipped default AND the pre-fix default, so this holds however the user got here.
    const double configured_depths[] = {0.1, 0.3, 1.0};

    for (int walls = 1; walls <= 6; ++walls) {
        for (double configured : configured_depths) {
            DYNAMIC_SECTION("walls = " << walls << ", configured interlock = " << configured) {
                const float band       = paint_depth_band_mm(pdmWalls, walls, 0.0, ext_w, ext_s, s);
                const float interlock  = paint_depth_interlocking_depth_mm(pdmWalls, configured, s);
                const double n_bead_optimum = double(walls) * double(s) + 2.0 * (double(ext_w) - double(ext_s));
                const double margin         = 0.25 * double(s);
                // Lower bound (unchanged): the notch never eats into the N-bead budget itself.
                CHECK(double(band) - double(interlock) >= n_bead_optimum - 1e-6);
                // I-4 upper bound A: the band's count-window margin is EXACTLY 0.25*spacing -
                // not "at least". This is what a widened band trips.
                CHECK_THAT(double(band) - n_bead_optimum, WithinAbs(margin, 1e-5));
                // I-4 upper bound B: the notch is subtractive only, so the claimed depth on
                // EITHER parity stays inside the same count window on the high side too.
                CHECK(double(band) - double(interlock) <= n_bead_optimum + margin + 1e-6);
            }
        }
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
