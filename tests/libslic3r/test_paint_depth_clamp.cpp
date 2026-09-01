#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/Model.hpp"
#include "libslic3r/MultiMaterialSegmentation.hpp"
#include "libslic3r/PaintDepth.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/TriangleSelector.hpp"

using namespace Slic3r;

// Paint Depth Stage 1, plan Task 2 item 4 (docs/superpowers/plans/2026-08-31-paint-depth.md,
// docs/superpowers/specs/2026-08-31-paint-depth-design.md): end-to-end tests of the clamp
// wiring landed in MultiMaterialSegmentation.cpp's multi_material_segmentation_by_painting.
// Unlike test_paint_depth.cpp (pure paint_depth_band_mm arithmetic), these instantiate a
// real synthetic mesh, paint facets with TriangleSelector/mmu_segmentation_facets exactly
// as the multi-material-painting gizmo does (see
// tests/libslic3r/test_mixed_filament.cpp's "Mixed filament component edits rebuild
// painted region targets" test for this construction pattern), slice a real PrintObject,
// and inspect the resulting per-region layer geometry - the actual claimed area that
// would print - which is the "gold standard" end-to-end test the plan asks for, not a
// hand-walk.
//
// Note: PrintObject::slice() already calls multi_material_segmentation_by_painting
// internally (PrintObjectSlice.cpp's slice_volumes()) and then folds its result into the
// per-region LayerRegion::slices via apply_mm_segmentation, which SPLITS the plain
// per-volume region slices into the base region plus one auto-created PrintRegion per
// painted extruder (config().wall_filament resolves to that extruder). Calling
// multi_material_segmentation_by_painting a second time afterwards would feed it that
// already-split, no-longer-representative geometry - not undefined behavior on paper,
// but empirically flaky (occasional SIGSEGV on the second, later Voronoi/graph pass),
// so these tests read the already-applied result off the sliced layers exactly once
// instead of invoking the segmentation function directly.
namespace {

// its_make_cube(x,y,z) (src/libslic3r/TriangleMesh.cpp) lays the box from (0,0,0) to
// (x,y,z) with these facet indices:
//   0,1   = bottom cap (z=0)   2,3   = top cap (z=z)
//   4,5   = +X side (x=x)      6,7   = Y=0 side
//   8,9   = -X side (x=0)      10,11 = Y=y side
// Side facets each span the object's FULL height as a single triangle, which is exactly
// what these tests need: painting facets {4,5} paints one entire side wall top-to-bottom
// (Tests 1-2, exercising the Voronoi/cut_segmented_layers path at every layer); painting
// all eight side facets paints the entire boundary at every layer, which is precisely
// the has_layer_only_one_color whole-layer short-circuit (:2149-2151) the plan calls out
// as the "brown-ring-claims-whole-layer" failure (Tests 3-4).
const std::vector<int> PLUS_X_FACE   = {4, 5};
const std::vector<int> ALL_SIDE_FACE = {4, 5, 6, 7, 8, 9, 10, 11};
// Top/bottom caps, for the vertical paint-depth alignment tests further down: {2,3} is the
// z=z cap (upward-facing, "top face"), {0,1} is the z=0 cap (downward-facing, "bottom face").
const std::vector<int> TOP_CAP_FACE    = {2, 3};
const std::vector<int> BOTTOM_CAP_FACE = {0, 1};

// paint_infill_override defaults to true (today's behavior, matches the option's own
// PrintConfig.cpp default) so existing callers that only pass mode/walls are unaffected.
DynamicPrintConfig paint_depth_test_config(PaintDepthMode mode, int walls, bool paint_infill_override = true)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    // Print::apply uses filament_diameter.size() as the physical filament count
    // (multi_material_segmentation_by_painting's num_physical_filaments).
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020"};
    // set_num_extruders() alone left nozzle_diameter's new slot effectively unusable for
    // Flow computation ("Flow::spacing() produced negative spacing"), so pin every
    // per-extruder width-driving option explicitly rather than trust the resize.
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    // full_print_config()'s bare option-registry default for outer_wall_line_width is a
    // literal 0 (not "0%"). Most Flow computations special-case 0 as "auto", but
    // MultiMaterialSegmentation.cpp's layer_color_stat lambda (used by
    // segmentation_top_and_bottom_layers, which multi_material_segmentation_by_painting
    // always runs) reads it via config.get_abs_value() directly with no such fallback,
    // so a literal 0 feeds Flow::rounded_rectangle_extrusion_spacing(0, layer_height) and
    // throws "Flow::spacing() produced negative spacing" for ANY MM-painted object sliced
    // against bare defaults - confirmed by bisection to be pre-existing (independent of
    // the Task 2 clamp-wiring change), not something this task introduced. Real
    // printer/filament presets always carry a non-zero outer_wall_line_width, so mirror
    // that here rather than the bare option-registry default.
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value = mode;
    config.option<ConfigOptionInt>("paint_depth_walls")->value               = walls;
    config.option<ConfigOptionBool>("paint_infill_override")->value          = paint_infill_override;
    return config;
}

// Builds a 40x40x20mm cube, paints the given facets with Extruder2 (state 2), applies
// paint_depth_test_config, and slices the object. The returned PrintObject's layers
// already carry the fully-applied MM segmentation (see the file-level note above).
PrintObject *slice_painted_cube(const std::vector<int> &painted_facets, PaintDepthMode mode, int walls, Print &print,
                                 bool paint_infill_override = true)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-clamp.stl";
    ModelVolume *volume  = object->add_volume(make_cube(40., 40., 20.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    print.set_status_silent();
    print.apply(model, paint_depth_test_config(mode, walls, paint_infill_override));
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// Paint Depth Stage 2 (Task 3 item 2/3): the PrintRegionConfig apply_mm_segmentation built
// for the Extruder2 paint claim (config().wall_filament == 2) - i.e. what filament each
// feature (walls / solid infill / sparse infill) will actually print in, independent of any
// per-layer geometry. paint_infill_override's effect is entirely a region-config decision
// (PrintApply.cpp's generate_print_object_regions / verify_update_print_object_regions), so
// this is the right level to pin it at, unlike extruder2_claim_for_layer() below which is
// about clamped geometry (Task 2 concern).
const PrintRegionConfig &extruder2_region_config(const PrintObject &object)
{
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value == 2)
            return region.config();
    }
    FAIL("no PrintRegion with wall_filament == 2 (Extruder2 paint claim) found");
    static PrintRegionConfig unreachable;
    return unreachable;
}

// Returns the ExPolygons of whichever PrintRegion apply_mm_segmentation carved out for
// the Extruder2 paint claim (config().wall_filament == 2) on the given layer - i.e. the
// area that will actually print in the painted color, after the clamp (if any).
ExPolygons extruder2_claim_for_layer(const PrintObject &object, size_t layer_idx)
{
    ExPolygons   result;
    const Layer *layer = object.get_layer(int(layer_idx));
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value != 2)
            continue;
        const int local_id = region.print_object_region_id();
        if (local_id < 0 || local_id >= layer->region_count())
            continue;
        const LayerRegion *layerm = layer->get_region(local_id);
        if (layerm == nullptr)
            continue;
        for (const Surface &s : layerm->slices.surfaces)
            result.emplace_back(s.expolygon);
    }
    return result;
}

bool any_contains(const ExPolygons &polys, const Point &pt)
{
    for (const ExPolygon &poly : polys)
        if (poly.contains(pt))
            return true;
    return false;
}

// Paint Depth Stage 2, plan Task 3 item 1 (docs/superpowers/plans/2026-08-31-paint-depth.md,
// docs/superpowers/specs/2026-08-31-paint-depth-design.md Stage 2(a)): builds a genuine color
// Z-interface WITHOUT a custom subdivided mesh, by stacking two 40x40x10mm cube ModelVolumes
// (both model parts) in one ModelObject: "lower" (z 0-10, entirely unpainted/base) and "upper"
// (z 10-20, +X face painted Extruder2). The boundary at z=10 along the +X wall is exactly
// bleed path (c) - a color transition in Z with nothing of the painted region below it.
//
// Unlike slice_painted_cube() above, this runs the object through Print::process() rather than
// PrintObject::slice() alone: surface classification (detect_surfaces_type() /
// discover_vertical_shells(), PrintObject.cpp) - what has_bounded_paint_depth() actually
// changes (Print.hpp) - only executes at the private prepare_infill() step (posPrepareInfill),
// which slice() does not reach; process() is the only public entry point that gets there.
PrintObject *process_z_interface_cube(PaintDepthMode mode, int walls, Print &print)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-z-interface.stl";
    ModelVolume *lower    = object->add_volume(make_cube(40., 40., 10.));
    ModelVolume *upper    = object->add_volume(make_cube(40., 40., 10.));
    upper->translate(0., 0., 10.);
    (void) lower;

    TriangleSelector selector(upper->mesh());
    for (int facet_idx : PLUS_X_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(upper->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    print.set_status_silent();
    print.apply(model, paint_depth_test_config(mode, walls));
    REQUIRE(print.objects().size() == 1);

    print.process();
    PrintObject *out_object = print.objects_mutable().front();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// Finds the local (per-object) region id of whichever PrintRegion carries the Extruder2 paint
// claim (config().wall_filament == 2). Returns -1 if none exists.
int extruder2_local_region_id(const PrintObject &object)
{
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value == 2)
            return region.print_object_region_id();
    }
    return -1;
}

// True if the Extruder2 region has ANY non-internal (top/bottom-family) typed surface on the
// given layer - i.e. solid skin, what has_bounded_paint_depth()/interface_shells produce at a
// color Z-interface. detect_surfaces_type() (PrintObject.cpp) is what assigns these types into
// layerm->slices.surfaces (see that function's "save surfaces to layer" section); this must run
// after Print::process(), not after PrintObject::slice() alone.
bool extruder2_layer_has_solid_skin(const PrintObject &object, size_t layer_idx)
{
    const int local_id = extruder2_local_region_id(object);
    if (local_id < 0)
        return false;
    const Layer *layer = object.get_layer(int(layer_idx));
    if (local_id >= layer->region_count())
        return false;
    const LayerRegion *layerm = layer->get_region(local_id);
    if (layerm == nullptr)
        return false;
    for (const Surface &s : layerm->slices.surfaces)
        if (s.surface_type != stInternal)
            return true;
    return false;
}

// Index of the first layer whose print_z is above target_z (with a half-layer-height epsilon
// so the layer immediately following a volume boundary is picked, not the one straddling it).
size_t first_layer_above_z(const PrintObject &object, double target_z)
{
    for (size_t idx = 0; idx < object.layer_count(); ++idx)
        if (object.get_layer(int(idx))->print_z > target_z + EPSILON)
            return idx;
    FAIL("no layer found above z=" << target_z);
    return 0;
}

} // namespace

TEST_CASE("multi_material_segmentation_by_painting: walls mode clamps a full-height painted face to the wall band", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmWalls, 3, print);

    const size_t      mid_layer = object->layer_count() / 2;
    const BoundingBox bb        = get_extents(object->get_layer(int(mid_layer))->lslices);
    REQUIRE(bb.defined);

    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    // 0.3mm in from the painted (+X) face: well inside any plausible wall band.
    const Point near_face(coord_t(bb.max.x() - scale_(0.3)), mid_y);
    // 10mm in from the painted (+X) face, on the object's horizontal midline: far
    // outside any plausible wall band, AND (for the unlimited-mode counterpart test)
    // still unambiguously on the "nearer to the painted face than to any other face"
    // side of the unclamped Voronoi split for a single-face paint - unlike the exact
    // geometric center of a square cross-section, which sits exactly on the medial-axis
    // skeleton where all four edges are equidistant and is therefore not a reliable
    // "still claimed" probe even with the clamp fully disabled.
    const Point deep_but_still_nearest_to_face(coord_t(bb.max.x() - scale_(10.0)), mid_y);

    const ExPolygons extruder2_claim = extruder2_claim_for_layer(*object, mid_layer);
    CHECK(any_contains(extruder2_claim, near_face));
    CHECK_FALSE(any_contains(extruder2_claim, deep_but_still_nearest_to_face));
}

TEST_CASE("multi_material_segmentation_by_painting: unlimited mode leaves the same painted face unbounded (legacy parity)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmUnlimited, 3, print);

    const size_t      mid_layer = object->layer_count() / 2;
    const BoundingBox bb        = get_extents(object->get_layer(int(mid_layer))->lslices);
    REQUIRE(bb.defined);

    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    // 10mm in from the painted (+X) face: far outside any plausible wall band, and
    // (unlike the exact geometric center of a square cross-section, which sits exactly
    // on the medial-axis skeleton where all four edges are equidistant) unambiguously
    // still on the "nearer to the painted face than to any other face" side of the
    // unclamped Voronoi split.
    const Point deep_but_still_nearest_to_face(coord_t(bb.max.x() - scale_(10.0)), mid_y);

    const ExPolygons extruder2_claim = extruder2_claim_for_layer(*object, mid_layer);
    // pdmUnlimited must reproduce the pre-existing (unclamped) Voronoi behavior exactly:
    // a single painted face's claim reaches well past any plausible wall band. This also
    // pins that mmu_segmented_region_interlocking_depth's new 0.3mm default (Task 1) does
    // NOT leak a clamp into unlimited mode (Task 2 item 3's interlocking gate).
    CHECK(any_contains(extruder2_claim, deep_but_still_nearest_to_face));
}

// Fix-wave F1/F8 (docs/superpowers/sdd/2026-08-31-paint-depth/final-review.md): the review
// flagged that no committed test pinned the walls-mode band WIDTH or the interlocking
// sub-band's per-layer alternation, so the pre-fix bug (cut_segmented_layers used the raw
// interlocking_depth as a REPLACEMENT band on even layers, clamping them to a ~0.3mm sliver
// instead of the full wall band) was invisible to the suite. This test pins both: every
// layer's claim reaches (at least) band - interlock deep regardless of parity - i.e. no
// layer is ever reduced to a bare sliver - and the interlock "tooth" (the sub-band between
// band-interlock and band) is claimed on odd layers but carved away on even layers, per
// Prusa-style semantics (see cut_segmented_layers's fix-wave F1 comment,
// MultiMaterialSegmentation.cpp).
TEST_CASE("multi_material_segmentation_by_painting: walls-mode band width is pinned and the interlock sub-band alternates only at the inner edge", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmWalls, 3, print);

    // Expected band, computed with the exact formula/inputs multi_material_segmentation_by_
    // painting itself uses (paint_depth_band_mm, max across the object's real printing
    // regions) - read off the already-sliced object so this test tracks the production
    // formula rather than hardcoding a value that would silently drift from it.
    float band_mm = 0.f;
    for (size_t region_idx = 0; region_idx < object->num_printing_regions(); ++region_idx) {
        const PrintRegion &region               = object->printing_region(region_idx);
        const float         ext_perimeter_width = region.flow(*object, frExternalPerimeter, object->config().layer_height).width();
        const float         perimeter_spacing   = region.flow(*object, frPerimeter, object->config().layer_height).spacing();
        band_mm = std::max(band_mm, paint_depth_band_mm(pdmWalls, 3, 0.0, ext_perimeter_width, perimeter_spacing));
    }
    REQUIRE(band_mm > 0.f);

    const double interlock_mm = object->config().mmu_segmented_region_interlocking_depth.value;
    // Test assumption (true of today's defaults - mmu_segmented_region_interlocking_depth
    // default 0.3, walls=3 band well over 1mm): fails loudly, not silently, if that ever
    // stops holding.
    REQUIRE(interlock_mm > 0.);
    REQUIRE(interlock_mm < band_mm);

    // Two adjacent layers of known parity, away from the top/bottom shell-layer projection
    // merge (mandatory check #2: top/bottom projections merge in AFTER the cut, un-clamped,
    // so a probe too close to either cap could see that unclamped merge instead of the cut
    // this test targets).
    REQUIRE(object->layer_count() >= 10);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
    REQUIRE(bb.defined);
    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;

    auto probe_at_depth = [&](size_t layer_idx, double depth_mm) -> bool {
        const Point pt(coord_t(bb.max.x() - scale_(depth_mm)), mid_y);
        return any_contains(extruder2_claim_for_layer(*object, layer_idx), pt);
    };

    // Well inside BOTH the full band and the interlock-shrunk band: must be claimed on
    // EVERY layer regardless of parity - this is exactly what pre-fix F1 broke (even
    // layers' claim depth was interlocking_depth alone, ~0.3mm, not the band).
    const double well_inside_both_mm = band_mm - interlock_mm - 0.05;
    REQUIRE(well_inside_both_mm > 0.05);
    CHECK(probe_at_depth(even_layer, well_inside_both_mm));
    CHECK(probe_at_depth(odd_layer, well_inside_both_mm));

    // Inside the interlock "tooth" itself (between band-interlock and band): claimed on
    // the odd (full-band) layer, NOT claimed on the even (interlock-notched) layer. This
    // is the alternating sub-band Prusa semantics F1 restores - carved at the INNER
    // boundary of the claim, not a wholesale replacement of the band.
    const double interlock_notch_mm = band_mm - interlock_mm / 2.0;
    REQUIRE(interlock_notch_mm > well_inside_both_mm);
    REQUIRE(interlock_notch_mm < band_mm);
    CHECK(probe_at_depth(odd_layer, interlock_notch_mm));
    CHECK_FALSE(probe_at_depth(even_layer, interlock_notch_mm));

    // Past the full band on both parities: unclaimed everywhere.
    const double past_band_mm = band_mm + 0.2;
    CHECK_FALSE(probe_at_depth(even_layer, past_band_mm));
    CHECK_FALSE(probe_at_depth(odd_layer, past_band_mm));
}

TEST_CASE("multi_material_segmentation_by_painting: a fully-painted boundary still clamps to the band (whole-layer short-circuit)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(ALL_SIDE_FACE, pdmWalls, 3, print);

    const size_t      mid_layer = object->layer_count() / 2;
    const BoundingBox bb        = get_extents(object->get_layer(int(mid_layer))->lslices);
    REQUIRE(bb.defined);

    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    const Point   near_edge(coord_t(bb.max.x() - scale_(0.3)), mid_y);
    const Point   object_center((bb.min.x() + bb.max.x()) / 2, mid_y);

    const ExPolygons extruder2_claim = extruder2_claim_for_layer(*object, mid_layer);
    CHECK(any_contains(extruder2_claim, near_edge));
    // This is the exact bug the design doc calls out at :2149-2151: with every side
    // facet at this layer painted the same color, has_layer_only_one_color hands the
    // ENTIRE cross-section - including dead center - to that one color unless the clamp
    // still applies. It must.
    CHECK_FALSE(any_contains(extruder2_claim, object_center));
}

TEST_CASE("multi_material_segmentation_by_painting: unlimited mode reproduces the whole-layer-claims-interior bug on a fully-painted ring", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(ALL_SIDE_FACE, pdmUnlimited, 3, print);

    const size_t      mid_layer = object->layer_count() / 2;
    const BoundingBox bb        = get_extents(object->get_layer(int(mid_layer))->lslices);
    REQUIRE(bb.defined);

    const Point object_center((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);

    const ExPolygons extruder2_claim = extruder2_claim_for_layer(*object, mid_layer);
    // Documents the pre-existing behavior pdmUnlimited deliberately preserves: without
    // any clamp, the whole-layer short-circuit really does hand over the object center.
    CHECK(any_contains(extruder2_claim, object_center));
}

// Paint Depth Stage 2, plan Task 3 item 2 (docs/superpowers/plans/2026-08-31-paint-depth.md,
// docs/superpowers/specs/2026-08-31-paint-depth-design.md Stage 2(b)): paint_infill_override.
// These pin the region-config decision at the PrintApply.cpp region-override site
// (generate_print_object_regions / verify_update_print_object_regions), not clamped geometry -
// walls mode is used only to have a realistic bounded claim; the override's effect does not
// depend on the clamp band's width.
TEST_CASE("multi_material_segmentation_by_painting: paint_infill_override=false keeps base-color sparse infill while walls/solid stay painted", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmWalls, 3, print, /*paint_infill_override=*/false);

    const PrintRegionConfig &painted_cfg = extruder2_region_config(*object);
    CHECK(painted_cfg.wall_filament.value == 2);
    CHECK(painted_cfg.solid_infill_filament.value == 2);
    // The base (unpainted) filament in paint_depth_test_config's filament_colour list is
    // extruder 1 ("#FFFFFF") - see that config's comment. sparse_infill_filament must stay
    // there instead of following the painted claim when the override is off.
    CHECK(painted_cfg.sparse_infill_filament.value == 1);
}

TEST_CASE("multi_material_segmentation_by_painting: paint_infill_override=true (default) paints sparse infill too (today's behavior)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmWalls, 3, print, /*paint_infill_override=*/true);

    const PrintRegionConfig &painted_cfg = extruder2_region_config(*object);
    CHECK(painted_cfg.wall_filament.value == 2);
    CHECK(painted_cfg.solid_infill_filament.value == 2);
    CHECK(painted_cfg.sparse_infill_filament.value == 2);
}

TEST_CASE("multi_material_segmentation_by_painting: paint_infill_override is a no-op in unlimited mode (matches the UI greying)", "[paintdepth]")
{
    // ConfigManipulation.cpp greys the paint_infill_override control out whenever
    // paint_depth_mode == unlimited; the underlying behavior must match that presentation -
    // ANDing the override's gate on bounded mode (Print::apply's paint_sparse_infill local)
    // means sparse infill keeps following the painted claim even with override=false here.
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmUnlimited, 3, print, /*paint_infill_override=*/false);

    const PrintRegionConfig &painted_cfg = extruder2_region_config(*object);
    CHECK(painted_cfg.sparse_infill_filament.value == 2);
}

// Paint Depth Stage 2, plan Task 3 item 1 (docs/superpowers/plans/2026-08-31-paint-depth.md,
// docs/superpowers/specs/2026-08-31-paint-depth-design.md Stage 2(a)): bleed path (c), color
// Z-interfaces. Uses process_z_interface_cube() (stacked base/painted volumes, see its comment)
// so the boundary at z=10 is a genuine paint color transition, and Print::process() so surface
// classification (detect_surfaces_type()) actually runs.
TEST_CASE("multi_material_segmentation_by_painting: a bounded color Z-interface gets solid skin", "[paintdepth]")
{
    Print        print;
    PrintObject *object = process_z_interface_cube(pdmWalls, 3, print);

    // is_mm_painted() is true (upper volume is painted) and paint_depth_mode is bounded
    // (walls), so PrintObject::has_bounded_paint_depth() should be active for this object -
    // the first layer above the base/painted boundary must carry solid skin instead of an
    // all-internal (bleeding) claim.
    const size_t first_painted_layer = first_layer_above_z(*object, 10.0);
    CHECK(extruder2_layer_has_solid_skin(*object, first_painted_layer));
}

TEST_CASE("multi_material_segmentation_by_painting: an unbounded (unlimited mode) color Z-interface has no solid skin (legacy parity)", "[paintdepth]")
{
    // Documents the pre-existing bleed-path-(c) bug that pdmUnlimited deliberately
    // preserves: has_bounded_paint_depth() requires paint_depth_mode != unlimited, so this
    // object's color Z-interface stays plain stInternal, exactly like before Stage 2.
    Print        print;
    PrintObject *object = process_z_interface_cube(pdmUnlimited, 3, print);

    const size_t first_painted_layer = first_layer_above_z(*object, 10.0);
    CHECK_FALSE(extruder2_layer_has_solid_skin(*object, first_painted_layer));
}

// Vertical paint-depth alignment fix (.superpowers/sdd/2026-08-31-paint-depth/
// vertical-depth-investigation.md, shell-coverage-investigation.md): segmentation_top_and_
// bottom_layers (MultiMaterialSegmentation.cpp) claimed exactly top_shell_layers /
// bottom_shell_layers layers deep and, pre-fix, never consulted top_shell_thickness /
// bottom_shell_thickness - while the solid shell itself (discover_vertical_shells /
// discover_horizontal_shells, PrintObject.cpp) is built to whichever is DEEPER of "N layers"
// or "T millimeters" (max(layers, mm), PrintObject.cpp:1954-1967/:4141-4147). At thin layer
// heights the solid shell ends up deeper than the painted claim, so base-colored solid shell
// layers show through under a painted top/bottom face.
//
// Builds a 40x40x4mm slab, paints one whole cap (top or bottom) with Extruder2, sets
// layer_height / top_shell_layers / top_shell_thickness / bottom_shell_layers /
// bottom_shell_thickness explicitly, and slices via PrintObject::slice() (the same "already
// fully applied" pattern as slice_painted_cube() above - see that function's file-level
// note). paint_depth_mode is pinned to pdmUnlimited: the vertical projection this fixture
// targets is a data path independent of the Stage 1 lateral clamp
// (vertical-depth-investigation.md section 2 proves the two never touch each other's
// inputs), and nothing here paints a SIDE facet for the lateral clamp to act on anyway, so
// pdmUnlimited isolates the vertical mechanism from Stage 1 entirely. 4mm tall is
// comfortably more than any shell depth exercised below (at most ~7 layers from either cap),
// so the top and bottom shells never reach each other or the opposite (unpainted) cap.
PrintObject *slice_capped_slab(const std::vector<int> &painted_cap_facets, double layer_height,
                                int top_shell_layers, double top_shell_thickness,
                                int bottom_shell_layers, double bottom_shell_thickness,
                                Print &print,
                                // I3 (.superpowers/sdd/2026-08-31-paint-depth/
                                // vertical-depth-fix-review.md): 0. (the default) means "unset,
                                // use layer_height" - the uniform-layers pin every existing
                                // caller below relies on for exact hand-derived expected claim
                                // depths. Pass an explicit different value to instead build
                                // genuine non-uniform-layer-height coverage (see the I3 test
                                // further down, the one caller that does this).
                                double initial_layer_print_height = 0.)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-vertical.stl";
    ModelVolume *volume  = object->add_volume(make_cube(40., 40., 4.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_cap_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value           = layer_height;
    // initial_layer_print_height defaults to 0.2mm independent of layer_height
    // (PrintConfig.cpp). Most callers want a uniform, known layer_height throughout (so their
    // hand-derived expected claim depths are exact), which they get by leaving
    // initial_layer_print_height unset (real heights are always > 0, so <= 0 means "unset"
    // here); the I3 test below instead passes an explicit value to deliberately keep the first
    // layer a different height.
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value =
        initial_layer_print_height > 0. ? initial_layer_print_height : layer_height;
    config.option<ConfigOptionInt>("top_shell_layers")->value         = top_shell_layers;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value    = top_shell_thickness;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value      = bottom_shell_layers;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value = bottom_shell_thickness;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// A point at the slab's XY center, safely away from the per-layer inward taper the descent
// loop applies near the silhouette (MultiMaterialSegmentation.cpp:1403-1407 - untouched by
// this fix; see PLUS_X_FACE's file-level comment above for the same consideration on the
// side-face tests). The slab's cross-section is the same 40x40 square at every layer, so any
// layer's lslices gives the same bounding box.
Point slab_center_point(const PrintObject &object)
{
    const BoundingBox bb = get_extents(object.get_layer(0)->lslices);
    REQUIRE(bb.defined);
    return Point((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);
}

TEST_CASE("multi_material_segmentation_by_painting: thin layers make the painted top claim reach the full (thickness-driven) shell depth", "[paintdepth]")
{
    // Defaults: top_shell_layers=4, top_shell_thickness=0.6mm (PrintConfig.cpp). At 0.1mm
    // layers the thickness bound needs 6 layers (0.6/0.1), deeper than the 4-layer count
    // bound - discover_vertical_shells already builds the solid shell 6 layers deep via
    // max(layers, mm); pre-fix, the painted claim stopped at 4, leaving layers 5-6
    // base-colored solid under the painted skin (the user-reported symptom).
    Print        print;
    PrintObject *object = slice_capped_slab(TOP_CAP_FACE, /*layer_height=*/0.1,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                             print);

    const Point  probe     = slab_center_point(*object);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 6);

    // The surface layer itself, always claimed regardless of shell depth.
    CHECK(any_contains(extruder2_claim_for_layer(*object, top_index), probe));
    // Depths 1-5 counting down from the surface (6 layers total, the surface included):
    // must ALL be claimed now that the claim mirrors the thickness-driven 6-layer shell.
    // This is exactly what FAILS pre-fix (the old code stops after depth 3, i.e. 4 layers
    // total, matching only top_shell_layers).
    for (size_t depth = 1; depth <= 5; ++depth)
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe));
    // Depth 6 (the 7th layer down) is past the 6-layer effective shell: unclaimed, pinning
    // that the fix computes max(layers, mm) and does not over-claim an unbounded depth.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - 6), probe));
}

TEST_CASE("multi_material_segmentation_by_painting: top_shell_layers=0 with nonzero top_shell_thickness claims NO painted depth (thickness is dead config when the layer count is 0)", "[paintdepth]")
{
    // C1 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md): the version of
    // this test committed at 41394ce2b4 asserted the claim was present here, on the premise
    // that "a real solid shell was still being built underneath". That premise is false - three
    // independent places gate the ENTIRE top shell on a nonzero LAYER count, never consulting
    // thickness at all when that count is 0:
    //   - PrintObject.cpp:1965 - discover_vertical_shells's `if (n_top_layers > 0)` wraps its
    //     whole top-gather block.
    //   - PrintObject.cpp:4123-4125 - discover_horizontal_shells's
    //     `if (num_solid_layers == 0) continue;` skips the thickness term entirely.
    //   - LayerRegion.cpp:1025-1036 - prepare_fill_surfaces() demotes every stTop surface to
    //     stInternal/stInternalVoid when top_shell_layers == 0: there is no top surface left to
    //     paint a color onto, let alone a shell beneath it.
    // PrintConfig.cpp:6376-6381 states the intended semantics outright: thickness only ever
    // RAISES an existing layer-count-driven shell; "0 means that this setting is disabled." So
    // this configuration must claim NOTHING - not even the immediately-painted surface facet,
    // because there is no solid top skin there for the paint to land on; it prints as ordinary
    // infill either way, exactly like pre-41394ce2b4 (correct) behavior.
    Print        print;
    PrintObject *object = slice_capped_slab(TOP_CAP_FACE, /*layer_height=*/0.2,
                                             /*top_shell_layers=*/0, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                             print);

    const Point  probe     = slab_center_point(*object);
    const size_t top_index = object->layer_count() - 1;
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index), probe));
}

TEST_CASE("multi_material_segmentation_by_painting: layer-count-driven shell depth is unchanged when it already exceeds the thickness bound", "[paintdepth]")
{
    // Regression guard (no over-claiming): at a normal 0.2mm layer height the stock defaults
    // (top_shell_layers=4, top_shell_thickness=0.6mm) already have the layer count win
    // (4 layers = 0.8mm >= 0.6mm), exactly as pre-fix - see
    // vertical-depth-investigation.md section 3's table. The claim must stay at 4 layers,
    // not deepen.
    Print        print;
    PrintObject *object = slice_capped_slab(TOP_CAP_FACE, /*layer_height=*/0.2,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                             print);

    const Point  probe     = slab_center_point(*object);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 4);

    for (size_t depth = 0; depth <= 3; ++depth)
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - 4), probe));
}

TEST_CASE("multi_material_segmentation_by_painting: thin layers make the painted bottom claim reach the full (thickness-driven) shell depth", "[paintdepth]")
{
    // Bottom-face mirror of the top-face depth test above, using bottom_shell_layers /
    // bottom_shell_thickness and BOTTOM_CAP_FACE. bottom_shell_thickness defaults to 0.0mm
    // (inert - investigation table), so this pins a nonzero value explicitly to exercise the
    // mirrored bottom_z()-based walk.
    Print        print;
    PrintObject *object = slice_capped_slab(BOTTOM_CAP_FACE, /*layer_height=*/0.1,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.6,
                                             print);

    const Point probe = slab_center_point(*object);

    CHECK(any_contains(extruder2_claim_for_layer(*object, 0), probe));
    for (size_t depth = 1; depth <= 5; ++depth)
        CHECK(any_contains(extruder2_claim_for_layer(*object, depth), probe));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, 6), probe));
}

// Fix-wave I1 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md):
// effective_shell_layers_by_thickness's `++m` happens before the break test, so `m` only equals
// the correct TOTAL depth (surface layer included) when the walk actually breaks. When it
// instead runs off the end of the object (every remaining layer is within `thickness`, so the
// break condition never fires) it undercounts by exactly one layer - the surface layer itself
// is never added to `m`. bottom_shell_thickness here (4mm) is deliberately larger than this
// slab could ever satisfy (max real gap across a 4mm/0.5mm-layer slab is 3.5mm), so the walk is
// GUARANTEED to exhaust rather than break, matching PrintObject.cpp's own generator loop
// (:2000-2001), which in that situation walks every layer down to (and including) the object's
// very last one. layer_height is deliberately coarse (0.5mm, only 8 layers total) rather than
// reusing the thin 0.1mm layers the other vertical-depth tests use: the descent loop's own
// per-layer inward taper (offset -= extrusion_spacing + extrusion_width,
// slab_center_point()'s file comment above) accumulates with depth and would otherwise erode
// the claim away from the dead-center probe by roughly the object's own half-width well before
// ~40 thin layers are reached - a confound independent of the I1 bug this test targets. At 8
// layers the cumulative erosion (~6mm) stays well inside the 20mm half-width margin.
//
// (No top-direction counterpart: the review traced that the pre-existing strict `>` plus
// `std::max(..., 0)` bound at MultiMaterialSegmentation.cpp:1495 already makes layer 0
// unreachable through the top descent for any effective count once effective >= the surface
// layer's own index, so the missing `+1` has no observable effect there - a top exhaustion test
// would pass identically with or without this fix, i.e. it would not discriminate.)
TEST_CASE("multi_material_segmentation_by_painting: painted bottom claim reaches the object's very last layer when the thickness walk exhausts (no off-by-one)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_slab(BOTTOM_CAP_FACE, /*layer_height=*/0.5,
                                             /*top_shell_layers=*/0, /*top_shell_thickness=*/0.0,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/4.0,
                                             print);

    const Point  probe      = slab_center_point(*object);
    const size_t last_index = object->layer_count() - 1;
    REQUIRE(last_index >= 6);

    // Every layer of the object, including the very last (topmost) one, must be claimed: the
    // configured thickness (4mm) exceeds anything this 4mm-tall slab can present (max real gap
    // 3.5mm), so per PrintObject.cpp's generator the bottom shell - and therefore the painted
    // claim mirroring it - covers the whole object. Pre-fix this FAILS at last_index only (one
    // short), leaving a single base-colored layer at the top of an otherwise fully
    // bottom-shelled object.
    for (size_t idx = 0; idx <= last_index; ++idx)
        CHECK(any_contains(extruder2_claim_for_layer(*object, idx), probe));
}

// Fix-wave I2 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md):
// layer_color_stat (MultiMaterialSegmentation.cpp:1436) maxes top_shell_layers /
// bottom_shell_layers over every LayerRegion layer.regions() returns for a layer - but
// PrintObjectSlice.cpp:5199-5208 gives EVERY layer a LayerRegion for EVERY PrintRegion on the
// object, whether or not that region has any geometry there. So a region confined to one part
// of the object (a modifier, or here, a second Z-stacked volume) can inflate the painted
// claim's depth on layers it never touches.
//
// Two Z-stacked model-part volumes, same construction as process_z_interface_cube() above:
// "lower" (z 0-4mm) carries a per-volume top_shell_layers override wildly larger than anything
// this test probes; "upper" (z 4-20mm, top cap painted) keeps the stock top_shell_layers
// default (4 - see the "thin layers..." top-depth TEST_CASE's comment above). A layer well
// inside "upper", nowhere near "lower", must see ONLY "upper"'s shell setting - "lower"'s
// LayerRegion exists there (PrintObjectSlice.cpp:5199-5208) but with empty slices (no
// lower-volume geometry reaches that high up), and must not contribute to the max.
TEST_CASE("multi_material_segmentation_by_painting: a region with no geometry on a layer cannot inflate that layer's painted claim depth", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-empty-region.stl";
    ModelVolume *lower    = object->add_volume(make_cube(40., 40., 4.));
    lower->name           = "lower";
    // Wildly larger than the correct (4-layer) shell and than anything this test probes below -
    // if an empty LayerRegion for "lower" ever leaked into "upper"'s layers, this value is what
    // would leak through.
    lower->config.set_key_value("top_shell_layers", new ConfigOptionInt(30));
    lower->config.set_key_value("top_shell_thickness", new ConfigOptionFloat(0.0));

    ModelVolume *upper = object->add_volume(make_cube(40., 40., 16.));
    upper->name         = "upper";
    upper->translate(0., 0., 4.);

    TriangleSelector selector(upper->mesh());
    for (int facet_idx : TOP_CAP_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(upper->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 1.0;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 1.0;
    // top_shell_layers / top_shell_thickness left at their stock defaults (4 / 0.6mm) for the
    // "upper"/base region - at this 1mm layer height the thickness term needs only 1 layer
    // (well under 4), so the count wins and the correct claim depth is exactly 4.

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);

    const Point  probe     = slab_center_point(*out_object);
    const size_t top_index = out_object->layer_count() - 1;
    REQUIRE(top_index >= 12);

    // Within the correct 4-layer shell: claimed on every one of the 4 topmost layers.
    for (size_t depth = 0; depth <= 3; ++depth)
        CHECK(any_contains(extruder2_claim_for_layer(*out_object, top_index - depth), probe));
    // Depth 10 is well past the correct 4-layer shell, but still squarely inside "upper" - far
    // from "lower"'s 4 layers at the very bottom of the object. Pre-fix this is WRONGLY claimed
    // because "lower"'s top_shell_layers=30 leaks into the max even though "lower" has no
    // geometry anywhere near this high up.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, top_index - 10), probe));
}

// Fix-wave I3 (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md): every
// case above pins initial_layer_print_height == layer_height (slice_capped_slab's default), so
// all of them are exactly reproducible by a naive ceil(thickness / layer_height) - none of them
// actually exercises "walk the real per-layer print_z/bottom_z, no uniform-layer-height
// assumption", the property the fix's own comments lean on hardest. This is the one case in the
// suite where the first layer's height genuinely differs from every other layer, chosen so a
// uniform-height assumption gives the WRONG count: with layer_height=0.1 but
// initial_layer_print_height=0.2, real bottom_z = 0, 0.2, 0.3, 0.4, 0.5, 0.6 (not 0, 0.1, 0.2,
// ...) - gaps 0.2/0.3/0.4/0.5/0.6 against bottom_shell_thickness=0.6 break at m=5, so the
// correct claim is layers 0-4 (5 layers), NOT the 6 a uniform-0.1 assumption would compute
// (ceil(0.6/0.1)=6). Confirmed by hand (temporarily swapping the real walk for
// ceil(thickness/layer_height) in effective_shell_layers_by_thickness and rebuilding): this is
// the only one of the five vertical-depth cases in this file that fails under that swap - see
// the fix-wave report for the captured RED.
TEST_CASE("multi_material_segmentation_by_painting: bottom claim depth follows the real (non-uniform) first-layer height, not a uniform-layer-height assumption", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_slab(BOTTOM_CAP_FACE, /*layer_height=*/0.1,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.6,
                                             print, /*initial_layer_print_height=*/0.2);

    const Point probe = slab_center_point(*object);
    REQUIRE(object->layer_count() >= 6);

    CHECK(any_contains(extruder2_claim_for_layer(*object, 0), probe));
    for (size_t depth = 1; depth <= 4; ++depth)
        CHECK(any_contains(extruder2_claim_for_layer(*object, depth), probe));
    // A plain ceil(0.6 / 0.1) = 6 (uniform-height assumption) would claim this layer too; the
    // real walk against actual bottom_z (first gap 0.2, not 0.1) stops one layer short of that.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, 5), probe));
}
