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

// Fix-wave F2/F3/F4: same construction as slice_painted_cube() above, but with the box's XY
// footprint as a parameter (so a cross-section THINNER than the clamp band can be built - the
// 40x40 cube cannot express that at any plausible band) and with paint_depth_mm, layer height
// and the INNER wall width pinned explicitly. paint_depth_test_config() only pins the outer
// wall width, but the band formula (paint_depth_band_mm) is driven by the frPerimeter spacing
// too, so pinning both makes the band arithmetic in these tests exact rather than dependent on
// whatever inner_wall_line_width resolves to from the bare option registry.
//
// WAVE A: two extra parameters, both DEFAULTED so every pre-existing caller is byte-identical.
//   - interlocking_depth < 0 means "leave mmu_segmented_region_interlocking_depth at its config
//     default" (the I-3 / I-2 tests below need to raise it by hand, which is exactly the user
//     situation I-3 is about: the cap only ever bites on a hand-raised notch).
//   - wall_generator defaults to Arachne, which is also what full_print_config() resolves to
//     (PrintConfig.cpp), so passing it explicitly changes nothing for existing callers; the
//     classic-generator tests below pass Classic.
PrintObject *slice_painted_box(double x, double y, double z, const std::vector<int> &painted_facets,
                                PaintDepthMode mode, int walls, double paint_depth_mm,
                                double layer_height, Print &print,
                                double interlocking_depth = -1.,
                                PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-box.stl";
    ModelVolume *volume  = object->add_volume(make_cube(x, y, z));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(mode, walls);
    config.option<ConfigOptionFloat>("paint_depth_mm")->value              = paint_depth_mm;
    config.option<ConfigOptionFloat>("layer_height")->value                = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value  = layer_height;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    if (interlocking_depth >= 0.)
        config.option<ConfigOptionFloat>("mmu_segmented_region_interlocking_depth")->value = interlocking_depth;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;

    print.set_status_silent();
    print.apply(model, config);
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
    float min_perimeter_spacing = 0.f;
    for (size_t region_idx = 0; region_idx < object->num_printing_regions(); ++region_idx) {
        const PrintRegion &region                 = object->printing_region(region_idx);
        const float         ext_perimeter_width   = region.flow(*object, frExternalPerimeter, object->config().layer_height).width();
        const float         ext_perimeter_spacing = region.flow(*object, frExternalPerimeter, object->config().layer_height).spacing();
        const float         perimeter_spacing     = region.flow(*object, frPerimeter, object->config().layer_height).spacing();
        band_mm = std::max(band_mm, paint_depth_band_mm(pdmWalls, 3, 0.0, ext_perimeter_width, ext_perimeter_spacing, perimeter_spacing));
        if (perimeter_spacing > 0.f)
            min_perimeter_spacing = min_perimeter_spacing > 0.f ? std::min(min_perimeter_spacing, perimeter_spacing) : perimeter_spacing;
    }
    REQUIRE(band_mm > 0.f);

    // Fix-wave F4: the notch the segmentation actually applies is the CLAMPED one, not the
    // raw config value, so read it the same way production does.
    const double interlock_mm = paint_depth_interlocking_depth_mm(pdmWalls, object->config().mmu_segmented_region_interlocking_depth.value,
                                                                  min_perimeter_spacing);
    // Test assumption (true of today's defaults - mmu_segmented_region_interlocking_depth
    // default 0.1, walls=3 band well over 1mm): fails loudly, not silently, if that ever
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

// Taper bound (.superpowers/sdd/2026-08-31-paint-depth/taper-bound-report.md): a point
// `inset_mm` inside the +X edge of the given layer's own cross-section, at mid-Y. The
// descent loop's inward erosion is measured from the layer silhouette, so "how far in from
// the edge" - not "how far from the center" - is the coordinate every taper assertion below
// actually cares about, and it is the only one that stays meaningful on a fixture whose
// cross-section changes with height (the frustum further down).
Point layer_edge_probe(const PrintObject &object, size_t layer_idx, double inset_mm)
{
    const BoundingBox bb = get_extents(object.get_layer(int(layer_idx))->lslices);
    REQUIRE(bb.defined);
    return Point(coord_t(bb.max.x() - scale_(inset_mm)), (bb.min.y() + bb.max.y()) / 2);
}

// Taper bound: same construction as slice_capped_slab() above, but with the prism's XY
// footprint and height as parameters so a SMALL painted feature can be built - one whose
// entire cross-section is narrower than the erosion the descent loop accumulates
// (extrusion_spacing + extrusion_width ~= 0.8785mm per layer of descent at a 0.45mm outer
// wall and 0.1mm layers). slice_capped_slab()'s fixed 40x40 footprint cannot express that:
// at a 20mm half-width the accumulated erosion never reaches its center probe, which is
// exactly why every pre-existing vertical-depth test was blind to the taper. Deliberately a
// separate function rather than a new parameter on slice_capped_slab(), so every existing
// vertical-depth fixture stays byte-identical.
PrintObject *slice_capped_prism(const std::vector<int> &painted_cap_facets, double xy_size, double height,
                                 double layer_height,
                                 int top_shell_layers, double top_shell_thickness,
                                 int bottom_shell_layers, double bottom_shell_thickness,
                                 Print &print)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-small-feature.stl";
    ModelVolume *volume  = object->add_volume(make_cube(xy_size, xy_size, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_cap_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = layer_height;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = top_shell_layers;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = top_shell_thickness;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = bottom_shell_layers;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = bottom_shell_thickness;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// Taper bound: a square frustum (truncated pyramid), `bottom` x `bottom` at z=0 tapering to
// `top` x `top` at z=`height`, centered on XY. Its four side walls are genuinely SLOPED
// (not vertical), which matters because TriangleMeshSlicer.cpp classifies slab facets by the
// SIGN of the XY-projected cross product: an exactly-vertical facet is "Vertical" and
// produces no slab at all, so a plain cube's painted side face cannot exercise the
// projection path this fixture needs to probe. Every side facet here has a positive normal-Z
// (hand-verified winding, see the facet table below), so all four walls are Up-facing and
// project into top_raw exactly like a shallow painted top face would - just as a thin
// staircase band hugging each layer's own contour instead of a wide patch.
//
// Facet indices (needed for painting; there is no its_make_* helper for a frustum, and
// its_make_pyramid()'s base facets are wound normal-UP, i.e. inverted):
//   0,1   = bottom cap (z=0, normal -Z)      2,3   = top cap (z=height, normal +Z)
//   4,5   = -Y sloped wall                    6,7   = +X sloped wall
//   8,9   = +Y sloped wall                   10,11  = -X sloped wall
TriangleMesh make_square_frustum(double bottom, double top, double height)
{
    const float b = float(bottom * 0.5), t = float(top * 0.5), h = float(height);
    std::vector<stl_vertex> vertices = {
        {-b, -b, 0.f}, { b, -b, 0.f}, { b,  b, 0.f}, {-b,  b, 0.f},
        {-t, -t, h  }, { t, -t, h  }, { t,  t, h  }, {-t,  t, h  },
    };
    std::vector<stl_triangle_vertex_indices> indices = {
        {0, 2, 1}, {0, 3, 2},
        {4, 5, 6}, {4, 6, 7},
        {0, 1, 5}, {0, 5, 4},
        {1, 2, 6}, {1, 6, 5},
        {2, 3, 7}, {2, 7, 6},
        {3, 0, 4}, {3, 4, 7},
    };
    return TriangleMesh(indexed_triangle_set(indices, vertices));
}

// The four sloped walls of make_square_frustum() (see its facet table above).
const std::vector<int> FRUSTUM_SLOPED_WALLS = {4, 5, 6, 7, 8, 9, 10, 11};

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

// ===========================================================================================
// Fix-wave re-review (.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fixwave-
// rereview.md): N1 (Critical), N2 (Minor), N3 (Minor) - findings against 530e2f52d2, the
// commit that landed the C1/I1/I2/I3 fixes above.
// ===========================================================================================

// N1 (Critical): the I2 guard `if (region->slices.empty()) continue;` sat above BOTH the
// shell-depth max (out.top_shell_layers / out.bottom_shell_layers) AND the per-colour
// extrusion-stat block (extrusion_width / extrusion_spacing / small_region_threshold /
// num_regions), not just the former as I2 intended. For any painted colour, the ONLY region
// with `wall_filament == color_idx` is the auto-created painted region (PrintApply.cpp:
// 1088-1090) - and that region's slices stay EMPTY at layer_color_stat time on EVERY layer,
// because apply_mm_segmentation (PrintObjectSlice.cpp:5275, which actually populates them)
// runs AFTER segmentation (:5267). So the per-colour block was skipped unconditionally for
// every painted colour, on every layer: extrusion_width/spacing/small_region_threshold all
// stayed 0.f and num_regions stayed 0 (re-arming assert(out.num_regions > 0), masked in
// Release by /DNDEBUG). Two consequences, both invisible to every other test in this file
// because they all probe dead-center on a uniform-cross-section slab:
//   - The lateral inward taper the descent loop applies per depth step
//     (offset -= extrusion_spacing + extrusion_width, :1542/:1562) goes to zero, so a
//     painted shell claim stops narrowing as it descends and instead stays the full
//     surface-layer silhouette all the way down - a full-width prism instead of the
//     intended truncated pyramid.
//   - small_region_threshold == 0 also disables the opening_ex() thin-projection filter
//     that exists to fix #7104.
// N1's original discriminating fixture was a FLAT 40x40 painted top cap probed 2mm in from
// the silhouette at the deepest claimed layer. That fixture can no longer express the taper:
// the user-approved taper bound (.superpowers/sdd/2026-08-31-paint-depth/taper-bound-report.md)
// deliberately gives a genuinely near-horizontal painted face its FULL width through the whole
// solid-shell depth, so a flat cap is claimed edge-to-edge at every shell layer by design.
// The taper (and therefore N1's non-zero-extrusion-stat pin) now lives exactly where the
// erosion's verified purpose lives - on a STEEP painted surface - so N1's pin is carried by
// the anti-smear test immediately below, which fails in the identical way if extrusion_width /
// extrusion_spacing ever go back to 0.f (offset stays 0 => the steep bands propagate at full
// width => the probe is wrongly claimed). See that test's comment for the arithmetic.

// TAPER BOUND, anti-smear guard (.superpowers/sdd/2026-08-31-paint-depth/taper-bound-report.md).
// THE proof that the erosion's real job is preserved. Verified purpose of the erosion
// (MultiMaterialSegmentation.cpp descent loops, `offset -= extrusion_spacing +
// extrusion_width` applied to the LAYER OUTLINE, BBS comment "offset width should be
// 2*spacing to avoid too narrow area which has overlap of wall line"): it is a perimeter
// safety margin on INFERRED (propagated) claims. A painted patch projected onto a layer where
// the paint is not actually on the surface must stay clear of that layer's perimeter loops,
// or it splits a wall line and leaves a sub-wall-width base sliver - the exterior-dimple class
// of upstream #7104 / #7235. Its most important consequence is that a STEEP painted surface,
// whose slab projection is by construction a thin staircase band hugging its layer's own
// contour, is annihilated at the first descent step (and the descent then breaks), instead of
// being smeared `top_shell_layers` deep as a full-width prism.
//
// The fixture is built to regress loudly if the erosion were simply deleted. A 40->22mm square
// frustum over 6mm (make_square_frustum above) at 0.3mm layers: the four painted walls slope
// 9mm horizontally over 6mm of height, so each layer's painted band is
// 0.3 * 9/6 = 0.45mm wide, measured inward from that layer's own contour. That width is
// deliberately in the danger window: comfortably ABOVE the opening_ex() thin-projection filter
// (small_region_threshold = 0.5*0.45mm outer wall, halved => 0.1125mm radius, i.e. anything
// narrower than 0.225mm is erased before the descent even starts) and comfortably BELOW one
// erosion step (extrusion_spacing + extrusion_width = (0.45 - 0.3*(1-pi/4)) + 0.45 ~= 0.836mm).
// So the band survives to the descent loop and the descent must then kill it:
//   - WITH the erosion (and with the taper bound's guard): step 1 intersects the band with the
//     layer outline eroded by ~0.836mm. The band lies within 0.45mm of that outline, so the
//     result is empty and the loop breaks. Layer 10's Extruder2 area is its OWN surface band
//     ([0, ~0.45mm] in from its contour, from that layer's OWN sloped-facet cross-section,
//     appended unconditionally - independent of any other layer's descent) plus the Stage-1
//     lateral band (cut_segmented_layers) - nothing else.
//   - WITHOUT the erosion (offset stays 0, whether by deleting the term or by regressing N1's
//     extrusion stats to 0.f): band(L) propagates to L-1, L-2, L-3 at full width, so layer 10
//     collects bands 11, 12 and 13 - the annulus from 0.45mm to 1.8mm in from its contour.
// The negative probe therefore sits 1.0mm in from layer 10's own contour: outside the surface
// band and the lateral band by more than a full band width, and squarely inside band(12)'s
// [0.9mm, 1.35mm] slot. It is claimed if and only if the deep full-width smear happened.
// (Hand-verified as genuinely discriminating, and confirmed empirically by a scratch build
// with the erosion term deleted - see the report.)
//
// taper-bound-review.md IMPORTANT 2: the positive probe's original placement (0.2mm in,
// against a pdmWalls/1-wall fixture) sat inside BOTH the ~0.45mm surface band above AND the
// Stage-1 lateral band, which for pdmWalls/1 is ALSO ext_perimeter_width = 0.45mm
// (PaintDepth.cpp's pdmWalls case, walls clamped to >= 1) - the two coincide exactly at that
// probe. Any regression that kills the vertical projection path alone (slice_mesh_slabs
// re-classifying the sloped facets as Vertical, the max_top_layers gate closing, top_raw being
// filtered away, stat.top_shell_layers misfiring) would leave the positive CHECK green via the
// lateral band alone, proving nothing about the path this test exists to guard. Fix: switch to
// pdmMillimeters/0.15mm, which shrinks the lateral band to [0, 0.15mm] - well clear of the
// vertical surface band's [0, 0.45mm] - and move the positive probe to 0.30mm in: outside the
// shrunk lateral band, inside the surface band, so it can now be satisfied ONLY by the vertical
// path. interlocking_cut_width = max(0.15 - 0.3, 0) = 0 (mmu_segmented_region_interlocking_depth
// defaults to 0.3, MultiMaterialSegmentation.cpp:1164), so region_cut_width stays 0.15mm on
// both layer parities (:1169) - no even/odd surprise. The negative probe (1.0mm) and every
// number in the derivation above are unaffected: paint_depth_mode only ever touches the
// Stage-1 lateral clamp, never the vertical projection/descent path (vertical-depth-
// investigation.md section 2).
TEST_CASE("multi_material_segmentation_by_painting: a steep painted surface gains no deep full-width claim (anti-smear guard)", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-steep-frustum.stl";
    ModelVolume *volume  = object->add_volume(make_square_frustum(40., 22., 6.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : FRUSTUM_SLOPED_WALLS)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    // pdmMillimeters/0.15mm (unlike every other vertical-depth fixture, which uses
    // pdmUnlimited): the painted walls DO cross every layer's contour, so the Stage-1 lateral
    // path claims the whole layer for Extruder2 via the has_layer_only_one_color
    // short-circuit. Clamping it to a 0.15mm band keeps that claim well clear of the 1.0mm
    // negative probe, so the negative side still reads the vertical projection alone - and,
    // per Important 2 above, keeps it clear of the 0.30mm positive probe too, so the positive
    // side does as well.
    DynamicPrintConfig config = paint_depth_test_config(pdmMillimeters, 1);
    config.option<ConfigOptionFloat>("paint_depth_mm")->value             = 0.15;
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.3;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.3;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.0;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = 0.0;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    // 6mm / 0.3mm = 20 layers; the probe layer needs 3 surface layers above it to descend from.
    REQUIRE(out_object->layer_count() >= 16);

    const size_t probe_layer = 10;
    // Two-sided on purpose. Extruder2 IS present on this layer - its own surface band, and the
    // Stage-1 lateral band, both hug the contour - so the CHECK_FALSE below is about HOW FAR IN
    // the claim reaches, never about the paint being absent or the fixture failing to slice.
    // Important 2: 0.30mm, not 0.2mm - outside the shrunk [0, 0.15mm] lateral band, inside the
    // [0, ~0.45mm] vertical surface band, so only the vertical projection path can satisfy it.
    CHECK(any_contains(extruder2_claim_for_layer(*out_object, probe_layer),
                       layer_edge_probe(*out_object, probe_layer, 0.30)));
    const Point probe = layer_edge_probe(*out_object, probe_layer, 1.0);
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, probe_layer), probe));
}

// N2 (Minor) - C1 residual: the surface-layer `append` at :1536 is not gated on
// `stat.top_shell_layers > 0`. In a mixed-region object, `max_top_layers` (the OBJECT-WIDE
// gate at :1311-1326 that decides whether top_raw gets populated at all) can be > 0 purely
// because of ANOTHER region elsewhere on the object, even on a layer where the only region
// actually present (non-empty slices) has top_shell_layers == 0 - i.e. layer_color_stat's
// per-LAYER stat.top_shell_layers legitimately comes back 0 there. C1's contract (see the
// "top_shell_layers=0..." TEST_CASE above) is that a zero-shell region claims nothing, not
// even its own surface layer (LayerRegion.cpp:1025-1036 demotes that surface to
// stInternal/stInternalVoid). Pre-fix, the surface layer still gets claimed anyway, because
// the append that puts it in triangles_by_color_top only checks that top_raw has geometry
// there, never stat.top_shell_layers itself.
//
// Two Z-stacked model-part volumes, same construction as the I2 test above: "lower" (z
// 0-4mm) keeps the stock top_shell_layers/top_shell_thickness defaults (4 / 0.6mm) purely so
// the object-wide max_top_layers gate is nonzero and top_raw gets populated at all; "upper"
// (z 4-20mm) explicitly sets top_shell_layers=0 with a NONZERO top_shell_thickness=0.6 (so
// this also discriminates the helper's own zero-count early return - a thickness-driven
// resurrection there would independently unmask this bug too, N3's "Site A") and has its top
// cap painted. "lower" has no geometry anywhere near the object's top layer, so per-layer
// stat.top_shell_layers there comes back 0 - purely from "upper", the only region present.
//
// Bottom-direction counterpart deliberately not added here: :1556 has the identical residual
// gate gap, but it is out of this fix-wave's cited anchor (:1536 only) and is left as a noted,
// deferred asymmetry rather than folded in unrequested - see the fix-wave report.
TEST_CASE("multi_material_segmentation_by_painting: a zero-shell region's painted top is not claimed even when another region makes the object-wide gate nonzero", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-n2-zero-shell-mixed.stl";
    ModelVolume *lower    = object->add_volume(make_cube(40., 40., 4.));
    lower->name           = "lower";
    // Stock top_shell_layers/top_shell_thickness defaults (4 / 0.6mm) left untouched: this
    // volume's only job is to keep the object-wide max_top_layers gate (:1311-1326) open so
    // top_raw is populated at all, exactly like the I2 test above.

    ModelVolume *upper = object->add_volume(make_cube(40., 40., 16.));
    upper->name         = "upper";
    upper->translate(0., 0., 4.);
    upper->config.set_key_value("top_shell_layers", new ConfigOptionInt(0));
    upper->config.set_key_value("top_shell_thickness", new ConfigOptionFloat(0.6));

    TriangleSelector selector(upper->mesh());
    for (int facet_idx : TOP_CAP_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(upper->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 1.0;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 1.0;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);

    const Point  probe     = slab_center_point(*out_object);
    const size_t top_index = out_object->layer_count() - 1;
    REQUIRE(top_index >= 4);

    // "upper"'s own top_shell_layers=0: the C1 contract says NO claim at all for this
    // region's colour, not even the immediately-painted surface layer itself.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, top_index), probe));
}

// N3 (Minor): the C1 inverted test above ("top_shell_layers=0 with nonzero
// top_shell_thickness claims NO painted depth...") pins only the top_layers_eff gate
// (:1319-1322); there is no bottom-direction (bottom_shell_layers=0) counterpart, even
// though :1321-1322 gates bottom identically to top and LayerRegion.cpp:1025-1036 demotes
// stBottom the same way it demotes stTop. Exact bottom mirror of that test: bottom cap
// painted, bottom_shell_layers=0, bottom_shell_thickness=0.6 (nonzero, so a reversion of the
// bottom_layers_eff gate alone would resurrect a claim here) - must claim NOTHING, not even
// the immediately-painted bottom surface facet. Already-correct behavior (Site B is already
// top/bottom symmetric); this is coverage, not a fix.
TEST_CASE("multi_material_segmentation_by_painting: bottom_shell_layers=0 with nonzero bottom_shell_thickness claims NO painted depth (bottom-direction mirror of the top C1 test above)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_slab(BOTTOM_CAP_FACE, /*layer_height=*/0.2,
                                             /*top_shell_layers=*/3, /*top_shell_thickness=*/0.0,
                                             /*bottom_shell_layers=*/0, /*bottom_shell_thickness=*/0.6,
                                             print);

    const Point probe = slab_center_point(*object);
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, 0), probe));
}

// ===========================================================================================
// TAPER BOUND - user decision: "Painted top/bottom claims keep FULL WIDTH for the solid-shell
// depth; taper only below that." Report:
// .superpowers/sdd/2026-08-31-paint-depth/taper-bound-report.md
//
// The descent loops erode the LAYER OUTLINE by (extrusion_spacing + extrusion_width) per layer
// of descent before intersecting it with the painted projection, so a painted claim that
// reaches the object silhouette retreats inward as it descends - a truncated pyramid, not a
// prism. Because the claim depth IS the solid-shell depth
// (effective_shell_layers_by_thickness, landed in the vertical-depth fix wave), that erosion
// leaves base-coloured SOLID SHELL under the painted skin all round its rim, and a painted
// feature narrower than 2 * 0.8785 * depth (~10mm at a 6-layer shell / 0.1mm layers) loses its
// deeper layers entirely. That is the user-reported bug.
//
// The bound: full width for genuinely near-horizontal painted faces, erosion retained where
// the projected patch belongs to a steep surface (proved by the anti-smear test above).
// ===========================================================================================

// The user's actual bug, stated as a test. An 8mm painted feature (an eye, a cheek) on a top
// face must keep its footprint - at DEPTH - at every layer of the solid shell it caps. 8x8x4mm
// prism, whole top cap painted, 0.1mm layers, top_shell_layers=4 / top_shell_thickness=0.6mm
// => a 6-layer effective shell (same arithmetic as the "thin layers..." test above).
// Pre-taper-bound the erosion accumulated 0.8785mm per descent step on a 4mm half-width, so:
//   - depths 1..4 claimed only 8 - 2*k*0.8785 mm, retreating inward layer by layer;
//   - depth 5 needed 8.785mm of an 8mm cross-section, so the claim was empty and the descent
//     broke - even the CENTER of the feature was unpainted there.
// The taper bound fixed that: the footprint no longer retreats with depth.
//
// Fix-wave F1 amends what "full footprint" means on a feature whose paint REACHES ITS OWN
// SILHOUETTE, which this one does (the whole 8x8 cap is painted, so the claim runs out to the
// prism's own contour). The sub-surface claim is now held one wall stack (extrusion_width +
// extrusion_spacing = 0.87854mm here) clear of each layer's contour, because otherwise the
// painted colour owns the EXTERIOR perimeter of every shell layer below the cap - a 0.7-1.0mm
// ring of the wrong colour on the visible side wall of every painted flat-topped object, which
// is what the user reported (outward-bleed-investigation.md section 1.2). So:
//   - depth 0 (the painted surface layer itself) still claims the full 8mm out to the contour -
//     that IS what the user painted, and it is appended with zero margin;
//   - depths 1..5 claim the footprint inset by exactly one wall stack - constant with depth,
//     not growing like the pre-taper-bound k*wall_stack erosion, and never empty;
//   - the feature's CENTER stays claimed at every one of the six shell layers, which is the
//     taper bound's actual win and is what this test has always been about.
TEST_CASE("multi_material_segmentation_by_painting: a small painted top feature keeps its footprint at depth, held one wall stack clear of the contour", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_prism(TOP_CAP_FACE, /*xy_size=*/8., /*height=*/4.,
                                              /*layer_height=*/0.1,
                                              /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                              /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                              print);

    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 7);

    // The painted surface layer itself: full width, right out to the silhouette.
    CHECK(any_contains(extruder2_claim_for_layer(*object, top_index),
                       layer_edge_probe(*object, top_index, 0.3)));

    for (size_t depth = 0; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        // Deep inside the wall-stack inset (2.5mm from centre on a 4mm half-width): claimed at
        // every shell layer. Pre-taper-bound this failed from depth 2 on.
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                           layer_edge_probe(*object, layer_idx, 1.5)));
        // ...and the feature's centre, which the pre-taper-bound erosion erased outright at
        // depth 5.
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx), slab_center_point(*object)));
    }
    // Fix-wave F1: no painted material within one wall stack of the contour on any SUB-SURFACE
    // layer. RED pre-F1 (the claim reached the contour exactly on every one of these layers).
    for (size_t depth = 1; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                                 layer_edge_probe(*object, layer_idx, 0.3)));
    }
    // No over-claim past the shell: depth 6 is outside the 6-layer effective shell.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - 6), slab_center_point(*object)));
}

// Bottom-direction mirror of the case above. bottom_shell_layers=3 /
// bottom_shell_thickness=0.6mm at 0.1mm layers: the thickness walk over real bottom_z values
// reaches layer 6 (bottom_z 0.6 >= 0.6 - EPSILON) => a 6-layer effective shell, layers 0..5,
// with layer 6 outside it. The bottom descent applies the identical per-step erosion
// (offset -= extrusion_spacing + extrusion_width) and the identical fix-wave F1 inset, so this
// pins exactly the same three things at the same depths as the top case.
TEST_CASE("multi_material_segmentation_by_painting: a small painted bottom feature keeps its footprint at depth, held one wall stack clear of the contour", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_prism(BOTTOM_CAP_FACE, /*xy_size=*/8., /*height=*/4.,
                                              /*layer_height=*/0.1,
                                              /*top_shell_layers=*/3, /*top_shell_thickness=*/0.0,
                                              /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.6,
                                              print);

    REQUIRE(object->layer_count() >= 8);

    CHECK(any_contains(extruder2_claim_for_layer(*object, 0), layer_edge_probe(*object, 0, 0.3)));

    for (size_t layer_idx = 0; layer_idx <= 5; ++layer_idx) {
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                           layer_edge_probe(*object, layer_idx, 1.5)));
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx), slab_center_point(*object)));
    }
    for (size_t layer_idx = 1; layer_idx <= 5; ++layer_idx)
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                                 layer_edge_probe(*object, layer_idx, 0.3)));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, 6), slab_center_point(*object)));
}

// No over-claim regression on a WIDE painted face. Same 40x40 slab and 6-layer effective shell
// as the "thin layers..." test above. Two things are pinned:
//   - the claim now reaches the silhouette at EVERY shell layer (the taper bound's intended
//     behaviour change; pre-change the 2mm-from-edge probe is lost from depth 3 on, where the
//     accumulated erosion 3 * 0.8785 = 2.64mm passes it), and
//   - the claim still stops dead at the shell boundary: depth 6 is unclaimed at the center AND
//     at the edge, so widening the claim did not also deepen it. That second half is the
//     regression guard the taper bound most needs, since removing a per-layer `break` source
//     is exactly the kind of change that could let a descent run past its bound.
TEST_CASE("multi_material_segmentation_by_painting: a wide painted top face claims full width within the shell and nothing past it", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_slab(TOP_CAP_FACE, /*layer_height=*/0.1,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                             print);

    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 7);

    for (size_t depth = 0; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                           layer_edge_probe(*object, layer_idx, 2.0)));
    }
    const size_t past_shell = top_index - 6;
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, past_shell), slab_center_point(*object)));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, past_shell),
                             layer_edge_probe(*object, past_shell, 2.0)));
}

// Taper bound, required engineering item 3: the vertical-depth fix wave gated the TOP
// surface-layer claim on `stat.top_shell_layers > 0` (N2) but deliberately left the symmetric
// BOTTOM site ungated, recording it as a known residual. This is that residual's RED test -
// the exact bottom mirror of N2's "a zero-shell region's painted top is not claimed even when
// another region makes the object-wide gate nonzero" above.
//
// Two Z-stacked model-part volumes. "lower" (z 0-4mm) sets bottom_shell_layers=0 with a
// NONZERO bottom_shell_thickness=0.6 (so the helper's zero-count early return is exercised
// too) and has its BOTTOM cap painted; "upper" (z 4-20mm) keeps the stock bottom_shell_layers
// default purely so the object-wide max_bottom_layers gate stays open and bottom_raw is
// populated at all. At layer 0 the only region with geometry is "lower"'s, so the per-layer
// stat.bottom_shell_layers is 0 - and C1's contract is that a zero shell count claims nothing,
// not even the immediately-painted bottom surface facet (LayerRegion.cpp demotes stBottom to
// stInternal/stInternalVoid there, so there is no solid skin for the colour to land on).
TEST_CASE("multi_material_segmentation_by_painting: a zero-shell region's painted bottom is not claimed even when another region makes the object-wide gate nonzero", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-zero-shell-mixed-bottom.stl";
    ModelVolume *lower    = object->add_volume(make_cube(40., 40., 4.));
    lower->name           = "lower";
    lower->config.set_key_value("bottom_shell_layers", new ConfigOptionInt(0));
    lower->config.set_key_value("bottom_shell_thickness", new ConfigOptionFloat(0.6));

    ModelVolume *upper = object->add_volume(make_cube(40., 40., 16.));
    upper->name         = "upper";
    upper->translate(0., 0., 4.);
    // Stock bottom_shell_layers default (3) left untouched: this volume's only job is to keep
    // the object-wide max_bottom_layers gate open so bottom_raw is populated at all.

    TriangleSelector selector(lower->mesh());
    for (int facet_idx : BOTTOM_CAP_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(lower->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 1.0;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 1.0;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 4);

    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, 0), slab_center_point(*out_object)));
}

// ===========================================================================================
// taper-bound-review.md, IMPORTANT 1: exposed_surface_part()'s early return
// (MultiMaterialSegmentation.cpp:1331-1333) fires whenever `reference_layer_idx >= num_layers`
// - i.e. for EVERY painted flat top face, since layer_idx+1 == num_layers there - and hands
// back the whole projected patch with NO clearance test at all. The surrounding comment's
// claimed invariant ("the base material left at that layer's perimeter is either nothing at
// all or at least one wall stack wide - never a sliver") is enforced by the diff_ex/offset_ex
// call on every OTHER path, but not on this one. The anti-smear test above cannot see this: it
// paints STEEP side walls whose own slope keeps the test's `run` equal to the taper below it
// (a locally monotone slope), which is exactly the case the early-return does not take. This
// fixture instead paints a FLAT cap sitting on top of a taper - the report's own conflated-run
// case - so the gap is governed entirely by the (here, unconstrained pre-fix) taper below.
// ===========================================================================================

// FIX-WAVE F1 UPDATE. The version of this test committed at cfe7fae1df asserted the OPPOSITE
// of the assertions below: that the sub-wall-stack base ring was ABSORBED INTO the painted
// claim (the "I1 absorb", MultiMaterialSegmentation.cpp:1673-1678 / :1725-1730), so the claim
// reached to within 0.1mm of each layer's own contour. That satisfied the no-sliver invariant
// by painting the sliver - and outward-bleed-investigation.md section 1.2 then showed what
// that costs in the GUI: on any chamfer/fillet/draft/organic taper below a painted flat cap the
// absorb fires on EVERY descent layer, pushing the painted colour onto the exterior perimeter
// for the whole shell depth. That is the user's reported exterior paint bleed, and the absorb
// is what made it fire on real (non-prismatic) models.
//
// F1 enforces the same invariant from the other side: the full-width term is intersected with
// offset_ex(input_expolygons[last_idx], -wall_stack), so the claim is held one wall stack clear
// of the descent layer's own contour and the base ring is at least one wall stack wide BY
// CONSTRUCTION. The I1 absorb is removed as part of the same change (with F1 in place its
// base_rest is one wall stack wide by construction, so opening_ex leaves it intact and the
// absorb's diff is empty - it is dead code that could only ever misfire on the Clipper
// arc-approximation knife-edge it now sits exactly on). Same invariant, one mechanism, and the
// exterior stays base-coloured. This test therefore keeps its fixture and its subject and flips
// the direction of its probes.
//
// make_square_frustum(40., 22., 6.) painted on its TOP CAP (facets {2,3}, the flat 22x22 face)
// instead of its sloped walls - the fixture the anti-smear test's own comment calls out as
// missing ("the only 'paint reaches the silhouette' fixture is the 40x40 slab, whose vertical
// walls make the gap exactly zero"). The cap is flat (paint reaches the silhouette with zero
// clearance, same as a plain box's top), but the object WIDENS below it: half-width grows from
// 11mm at the cap to 20mm at the base, 1.5mm of horizontal run per 1mm of height - the same
// shallow-taper shape a 45-degree chamfer gives at a steeper 1mm-run-per-mm-height, just
// reusing an already-existing fixture instead of a new mesh.
//
// top_ex at the top layer is the cap's own exact flat-facet footprint: half-width 11.000mm,
// fixed, independent of layer height (the "small painted top feature" test above already
// establishes that a fully-painted flat cap's depth-0 claim is exactly its own facet
// footprint). reference_layer_idx = layer_idx+1 = num_layers at the very top layer, so
// exposed_surface_part() takes the FIRST early-return branch and hands back top_ex unclamped.
// Because the frustum only widens going down, the running containment intersection
// (layer_slices_trimmed) never shrinks below the TOP layer's own outline (half-width
// ~11.075mm at 0.1mm layers - the cross-section a hair below the z=6 tip, since
// Layer::slice_z sits at the layer's z-midpoint, not its top) - so the "exposed" full-width
// term stays pinned at the cap's fixed 11.000mm footprint at EVERY descent depth, while each
// layer's own true contour keeps growing underneath it:
//   depth k contour half-width  = 11.075 + 0.15*k
//   cap footprint (top_exposed) = 11.000 (fixed, every k)
//   post-F1 claim edge          = min(11.000, 11.075 + 0.15*k - 0.87854) = 10.19646 + 0.15*k
// i.e. the claim sits exactly one wall stack (extrusion_width + extrusion_spacing = 0.45 +
// 0.42854 = 0.87854mm at 0.1mm layers / 0.45mm walls) inside this layer's own contour at every
// descent depth - never a sliver of base material at the perimeter (the invariant
// exposed_surface_part()'s comment states), and never painted colour on the exterior wall
// (the user's reported bleed).
TEST_CASE("multi_material_segmentation_by_painting: a chamfered/tapered painted top leaves at least one wall stack of base material at the contour of every layer below the cap (F1)", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-taper-bound-cap.stl";
    ModelVolume *volume  = object->add_volume(make_square_frustum(40., 22., 6.));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : TOP_CAP_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    // pdmUnlimited (not pdmWalls, unlike the anti-smear test): only the flat top CAP is
    // painted, never a side facet, so there is no painted boundary anywhere in any layer's own
    // cross-section for the Stage-1 lateral clamp to act on - the entire claim below the
    // surface layer comes from the vertical projection/descent path this fixture targets. See
    // slice_capped_slab()'s file comment above for the identical reasoning.
    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = 0.0;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);

    // 6mm / 0.1mm = 60 layers; top_shell_layers=4 / thickness=0.6mm at 0.1mm layers is the same
    // "6-layer effective shell" arithmetic as the "thin layers..." and "small painted top
    // feature" tests above (0.6 / 0.1 = 6 >= the 4-layer count bound).
    const size_t top_index = out_object->layer_count() - 1;
    REQUIRE(top_index >= 6);

    // Positive control: comfortably inside the cap's own 11mm half-width footprint at the
    // surface layer itself (1mm in from that layer's own ~11.075mm contour is still well
    // inside the 11.000mm cap edge), claimed both before and after the fix - proves the
    // fixture actually sliced and painted, so the CHECKs below are about how far IN the claim
    // reaches, never about the paint being absent.
    CHECK(any_contains(extruder2_claim_for_layer(*out_object, top_index),
                       layer_edge_probe(*out_object, top_index, 1.0)));

    for (size_t depth = 1; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        // 0.1mm inside THIS layer's own true contour: base material, on every sub-surface
        // layer. RED pre-F1 - the I1 absorb swallowed exactly this ring into the painted claim
        // at every one of these depths, which is the exterior bleed.
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, layer_idx),
                                 layer_edge_probe(*out_object, layer_idx, 0.1)));
        // I-5 (bleed-and-walls-fixwave-review.md): and the base ring is at least one WALL STACK
        // wide, not merely non-zero. The 0.1/1.5 pair alone only pinned the ring to the open
        // interval (0.1, 1.5)mm, so a 0.3mm sub-wall-stack sliver - the exact class the review
        // that demanded the I1 absorb cited - passed unchanged. The whole "the absorb is
        // redundant because the F1 inset guarantees >= one wall stack" argument rests on this
        // number, and on TAPERED geometry it was argued rather than enforced (the tight
        // magnitude probe existed only on a prism, where input_expolygons[last_idx] ==
        // input_expolygons[layer_idx] and the taper is not exercised at all).
        // Post-F1 the claim edge sits at exactly extrusion_width + extrusion_spacing =
        // 0.87854mm from each descent layer's own contour here, so 0.8 is a valid negative
        // probe with 0.078mm of clearance.
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, layer_idx),
                                 layer_edge_probe(*out_object, layer_idx, 0.8)));
        // ...and the claim is nonetheless real and deep: 1.5mm in from the same contour is
        // still painted at every one of those depths (claim edge is 0.87854mm in), so this is
        // a boundary that moved, not a claim that vanished.
        CHECK(any_contains(extruder2_claim_for_layer(*out_object, layer_idx),
                           layer_edge_probe(*out_object, layer_idx, 1.5)));
    }

    // No over-claim regression: depth 6 is past the 6-layer effective shell (structurally
    // outside the descent loop's own bound regardless of this fix), so it must stay unclaimed
    // even 0.1mm in from its own contour.
    const size_t past_shell = top_index - 6;
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, past_shell),
                             layer_edge_probe(*out_object, past_shell, 0.1)));
}

// ===========================================================================================
// FIX WAVE: exterior paint bleed (F1), lateral-clamp self-disabling on thin geometry (F2),
// band arithmetic (F3) and the interlocking notch's wall-loop cost (F4).
// .superpowers/sdd/2026-08-31-paint-depth/outward-bleed-investigation.md
// .superpowers/sdd/2026-08-31-paint-depth/wall-count-investigation.md
// ===========================================================================================

// F1, the plain flat-cap case (outward-bleed-investigation.md section 1.2's worked example).
// A 40x40x4mm slab with its whole top cap painted: nothing sits above the cap, so
// exposed_surface_part()'s early return (MultiMaterialSegmentation.cpp:1331-1332) hands the
// patch back with NO clearance test, and pre-F1 the full-width term
// `intersection_ex(top_exposed_ex, layer_slices_trimmed)` was bounded only by the layer
// OUTLINE. On a prism the outline does not change with depth, so the claim reached the
// silhouette EXACTLY on all six shell layers - i.e. the painted colour owned the exterior
// perimeter of a 0.7mm-tall ring of side wall the user never painted. That was structurally
// impossible before 65d17c964f (the legacy descent was inset by k * wall_stack
// unconditionally; 3448111acd:1570-1578 == f1e9f78696:1388-1396), so it is a regression this
// branch introduced, and it is what the user validated negatively in the GUI.
TEST_CASE("multi_material_segmentation_by_painting: a painted flat cap leaves the exterior wall base-coloured on every sub-surface shell layer (F1)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_slab(TOP_CAP_FACE, /*layer_height=*/0.1,
                                             /*top_shell_layers=*/4, /*top_shell_thickness=*/0.6,
                                             /*bottom_shell_layers=*/3, /*bottom_shell_thickness=*/0.0,
                                             print);

    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 7);

    // The painted SURFACE layer keeps its zero-margin claim out to the silhouette - that is
    // literally the facet the user painted, appended separately at :1634 and untouched by F1.
    // Also the fixture's positive control: if this failed, the CHECK_FALSEs below would prove
    // nothing.
    CHECK(any_contains(extruder2_claim_for_layer(*object, top_index),
                       layer_edge_probe(*object, top_index, 0.2)));

    for (size_t depth = 1; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        // One wall stack here is extrusion_width + extrusion_spacing = 0.45 + 0.42854 =
        // 0.87854mm (0.45mm outer wall, 0.1mm layers). Both probes sit inside it, so both must
        // be BASE material: no painted filament anywhere in the perimeter band of a
        // sub-surface layer. RED pre-F1 at both depths - the claim reached the contour exactly.
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                                 layer_edge_probe(*object, layer_idx, 0.2)));
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                                 layer_edge_probe(*object, layer_idx, 0.8)));
        // Immediately past the wall stack the claim is still full width, at every shell depth -
        // F1 holds a CONSTANT one-wall-stack inset, not the legacy k * wall_stack taper (which
        // would already be 4.39mm in by depth 5).
        CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                           layer_edge_probe(*object, layer_idx, 1.0)));
    }
}

// F1's other half: the approved intent must survive. outward-bleed-investigation.md section 4
// Option A's central claim is that measuring the inset from the LAYER CONTOUR (not from the
// painted patch) leaves an interior painted feature completely unclipped - "a raised painted
// boss well inside the silhouette keeps its ENTIRE footprint at every shell layer, which was
// the whole point of 65d17c964f". This is that claim as a test, and it must stay GREEN across
// the fix (it is a guard, not a RED item).
//
// Two stacked model-part volumes, the same construction as process_z_interface_cube() above: a
// 40x40x3.8mm slab with a centred 8x8x0.4mm boss on top, the boss's top cap painted. At 0.1mm
// layers the boss is only 4 layers tall, so the 6-layer effective shell (top_shell_layers=4 /
// top_shell_thickness=0.6mm) descends past it into the slab - where the painted claim is 16mm
// clear of the layer's own silhouette and F1's inset therefore cannot touch it.
TEST_CASE("multi_material_segmentation_by_painting: an interior painted top feature keeps its FULL footprint through the shell (F1 preserves the approved intent)", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-interior-boss.stl";
    ModelVolume *slab     = object->add_volume(make_cube(40., 40., 3.8));
    slab->name            = "slab";
    ModelVolume *boss     = object->add_volume(make_cube(8., 8., 0.4));
    boss->name            = "boss";
    boss->translate(16., 16., 3.8); // centred on the 40x40 slab, sitting on top of it

    TriangleSelector selector(boss->mesh());
    for (int facet_idx : TOP_CAP_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(boss->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    // pdmUnlimited: only a horizontal cap is painted, so no layer's own cross-section carries a
    // painted boundary for the Stage-1 lateral clamp to act on - the whole claim comes from the
    // vertical projection/descent path this test targets (same reasoning as
    // slice_capped_slab()'s file comment above).
    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = 0.0;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);

    const size_t top_index  = out_object->layer_count() - 1;
    const size_t boss_first = first_layer_above_z(*out_object, 3.8);
    REQUIRE(top_index >= 7);
    // The geometry this test depends on, asserted rather than assumed: depths 4 and 5 of the
    // shell descent land in the SLAB, where the painted footprint is deep inside the silhouette.
    REQUIRE(top_index - 4 < boss_first);
    REQUIRE(top_index - 5 < boss_first);

    // Probes in ABSOLUTE coordinates, taken from the boss's own cross-section, so the same
    // points can be re-probed on the (much wider) slab layers below it.
    const BoundingBox boss_bb = get_extents(out_object->get_layer(int(boss_first))->lslices);
    REQUIRE(boss_bb.defined);
    const coord_t boss_mid_y = (boss_bb.min.y() + boss_bb.max.y()) / 2;
    const Point   boss_edge_probe(coord_t(boss_bb.max.x() - scale_(0.2)), boss_mid_y);
    const Point   boss_centre((boss_bb.min.x() + boss_bb.max.x()) / 2, boss_mid_y);

    for (size_t depth = 4; depth <= 5; ++depth) {
        const size_t layer_idx = top_index - depth;
        const ExPolygons claim = extruder2_claim_for_layer(*out_object, layer_idx);
        // FULL width: 0.2mm inside the painted feature's own footprint edge - well inside the
        // 0.87854mm wall stack, and claimed anyway, because the inset is measured from the
        // LAYER's contour (16mm away), not from the patch.
        CHECK(any_contains(claim, boss_edge_probe));
        CHECK(any_contains(claim, boss_centre));
        // ...and it still does not bleed to the slab's own exterior wall.
        CHECK_FALSE(any_contains(claim, layer_edge_probe(*out_object, layer_idx, 0.3)));
    }
    // No over-claim past the 6-layer effective shell.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, top_index - 6), boss_centre));
}

// F2 (outward-bleed-investigation.md section 2.3): where the local cross-section's half-thickness
// is less than cut_width, `offset_ex(input_expolygons[layer_idx], -region_cut_width)` comes back
// EMPTY, diff_ex(claim, {}) == claim, and the lateral clamp is a COMPLETE no-op on that layer -
// the painted colour keeps the entire local cross-section. At paint_depth_mm = 4-6mm that
// condition holds across most of a typical organic model, which is why the clamp appeared to
// stop working (and the paint to "spread outward") exactly in the range the user reported.
//
// Fixture: a 1.2mm-thin, 40mm-long wall with EVERY side facet painted, so
// has_layer_only_one_color hands the whole cross-section to the painted colour and the lateral
// clamp is the only thing that can take any of it back. paint_depth_mm = 2mm is more than three
// times the 0.6mm local half-thickness, so pre-F2 the clamp does nothing whatsoever.
//
// WAVE A / C-1 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md):
// this fixture's EXPECTED RESULT is now the no-op, and that is the fix, not a regression.
// F2's ladder starts at `b = band/4` and halves; with no lower bound the widest claim it can
// ever produce is band/4, and on this fixture it landed on step 1, b = 0.25mm. A 0.25mm-wide
// painted strip is a separate PrintRegion whose perimeters are generated on the strip alone
// (Layer.cpp:184, :257-260): Arachne sees T = 0.25 - 2h(1-pi/4) = 0.207mm, above
// min_feature_size and below min_bead_width, so WideningBeadingStrategy widens it to a 0.34mm
// bead in a 0.207mm gap - ~64% local over-extrusion on both faces of every thin wall, on every
// painted layer (one ladder step further down it produces no toolpath at all while the base
// region has already been cut back by it). The ladder is now floored at one external extrusion
// (ext_perimeter_width, 0.45mm here): geometry that cannot carry a printable painted skin keeps
// its whole cross-section, exactly as it did before F2, instead of getting an unprintable one.
//
// Hand-walk of this fixture at the floor: offset_ex(L, -2.0) is empty so `thin` = L; step 0's
// b = 0.5 >= 0.45 runs but its membership test opening_ex(L, 2b = 1.0) is empty on a 1.2mm wall
// (half-thickness 0.6); step 1's b = 0.25 < 0.45 stops the ladder. Result: the keep-core is
// empty, diff_ex(claim, {}) == claim, and the whole cross-section stays painted.
//
// The ladder is NOT dead - the "degrades to a printable band" test below exercises it on
// geometry thick enough to carry one.
TEST_CASE("multi_material_segmentation_by_painting: a fin too thin to carry one extrusion of paint keeps its whole cross-section rather than a sub-extrusion skin (C-1)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/1.2, /*y=*/40., /*z=*/20., ALL_SIDE_FACE,
                                             pdmMillimeters, /*walls=*/3, /*paint_depth_mm=*/2.0,
                                             /*layer_height=*/0.2, print);

    REQUIRE(object->layer_count() >= 10);

    // Both parities: the interlocking notch narrows the band on even layers, and the floor has
    // to hold on both.
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    for (size_t layer_idx : {even_layer, odd_layer}) {
        CAPTURE(layer_idx);
        const BoundingBox bb = get_extents(object->get_layer(int(layer_idx))->lslices);
        REQUIRE(bb.defined);
        const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
        const Point   near_face(coord_t(bb.max.x() - scale_(0.1)), mid_y);
        const Point   centre((bb.min.x() + bb.max.x()) / 2, mid_y);

        const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);
        // The painted face's own perimeter band is painted (it always was).
        CHECK(any_contains(claim, near_face));
        // C-1: and so is the centre - the clamp is a clean no-op here, NOT a 0.25mm strip on
        // each face with a 0.7mm base core between them. RED pre-C-1 on both parities.
        CHECK(any_contains(claim, centre));
    }
}

// C-1's other half, and F2's: on geometry that CAN carry a printable painted skin the ladder
// still degrades the band instead of swallowing the cross-section whole.
//
// Fixture: a 5.8mm-wide fin (local half-thickness t = 2.9mm) at paint_depth_mm = 6.0, every
// side facet painted. offset_ex(L, -6.0) is empty so the full-band inset is a no-op and the
// ladder runs. Step 0 (b = 1.5, membership at 2b = 3.0 > t = 2.9) misses; step 1 (b = 0.75,
// membership at 1.5 <= 2.9) takes it, and 0.75mm is comfortably above the 0.45mm external
// extrusion floor. So each face gets a 0.75mm painted skin and the middle 4.3mm stays base -
// the `t/4 < b <= t/2` proportionality F2 exists to deliver.
//
// It is also the I-2 pin. The ladder's STEP is now chosen from the un-notched band on every
// layer, not from the notched one: with the interlocking notch raised to 0.4mm the even-layer
// band would be 5.6, whose step 0 (2b = 2.8 <= 2.9) DOES fit, so pre-I-2 this fin got a 1.4mm
// skin on even layers and a 0.75mm skin on odd ones - the claim halving and doubling on
// alternating layers, a smaller cousin of the 3/2/3/2 wall alternation F4 removed. Probing at
// 1.0mm catches exactly that: base on both parities post-I-2, painted on the even ones before.
TEST_CASE("multi_material_segmentation_by_painting: the lateral clamp degrades to a printable painted skin, identical on both parities, on geometry thinner than the band (F2 + I-2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/5.8, /*y=*/40., /*z=*/6., ALL_SIDE_FACE,
                                             pdmMillimeters, /*walls=*/3, /*paint_depth_mm=*/6.0,
                                             /*layer_height=*/0.2, print, /*interlocking_depth=*/0.4);

    REQUIRE(object->layer_count() >= 10);

    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    for (size_t layer_idx : {even_layer, odd_layer}) {
        CAPTURE(layer_idx);
        const BoundingBox bb = get_extents(object->get_layer(int(layer_idx))->lslices);
        REQUIRE(bb.defined);
        const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
        const Point   centre((bb.min.x() + bb.max.x()) / 2, mid_y);

        const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);
        // The skin is real and at least one external extrusion (0.45mm) wide - the C-1 floor is
        // a floor, not an off switch.
        CHECK(any_contains(claim, Point(coord_t(bb.max.x() - scale_(0.5)), mid_y)));
        // ...and it stops at 0.75mm, the same on both parities (I-2).
        CHECK_FALSE(any_contains(claim, Point(coord_t(bb.max.x() - scale_(1.0)), mid_y)));
        // ...leaving the middle of the fin base-coloured (F2's whole point).
        CHECK_FALSE(any_contains(claim, centre));
    }
}

// I-3 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md): F4's
// quarter-spacing cap on the interlocking notch exists to keep the notch inside the
// count-window margin `paint_depth_band_mm` builds into the WALLS-mode band, so Arachne still
// delivers N loops on both parities. In MILLIMETRES mode there is no N: the band is the user's
// literal paint_depth_mm (PaintDepth.cpp), it is not sized to a bead count, and no wall-count
// contract is being protected - the cap there just silently shrinks a notch the user asked for
// (0.4mm -> 0.107mm at stock flows, 3.7x less) for a reason that does not apply to them. The
// tooltip already justifies the cap by "Paint depth walls" alone.
//
// Fixture: a 40x40 box (half-thickness 20mm, far thicker than the 6mm band, so F2's ladder
// never runs and the band is the plain clamp) with paint_depth_mm = 6.0 and the notch raised to
// 0.4mm by hand. The even-layer band is band - notch; the odd-layer band is the band itself.
TEST_CASE("multi_material_segmentation_by_painting: millimetres mode honours the configured interlocking notch verbatim (I-3)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/6., PLUS_X_FACE,
                                             pdmMillimeters, /*walls=*/3, /*paint_depth_mm=*/6.0,
                                             /*layer_height=*/0.2, print, /*interlocking_depth=*/0.4);

    REQUIRE(object->layer_count() >= 10);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
    REQUIRE(bb.defined);
    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    auto probe = [&](double inset_mm) { return Point(coord_t(bb.max.x() - scale_(inset_mm)), mid_y); };

    const ExPolygons claim_even = extruder2_claim_for_layer(*object, even_layer);
    const ExPolygons claim_odd  = extruder2_claim_for_layer(*object, odd_layer);

    // Odd layers: the band is paint_depth_mm verbatim, 6.0mm. (Pins millimetres mode itself.)
    CHECK(any_contains(claim_odd, probe(5.9)));
    CHECK_FALSE(any_contains(claim_odd, probe(6.1)));
    // Even layers: 6.0 - 0.4 = 5.6mm. RED pre-I-3, where the notch was capped at 0.25*spacing
    // (0.1018mm at 0.45mm lines / 0.2mm layers) and the even band was therefore 5.898mm, so
    // 5.7mm in was still painted.
    CHECK(any_contains(claim_even, probe(5.4)));
    CHECK_FALSE(any_contains(claim_even, probe(5.7)));
}

// F3 + F4 as one end-to-end contract: "N walls" must deliver the same wall-loop capacity on
// BOTH layer parities. Bead counting is not reachable from this harness (these fixtures stop at
// PrintObject::slice(), which never runs perimeter generation; and the painted region's bead
// count is decided inside Arachne from the strip thickness), so the assertion is made on the
// quantity that decides the bead count: the band width actually claimed, measured against the
// exact N-bead optimum thickness `N * perimeter_spacing + 2 * (ext_w - ext_s)` that
// paint_depth_band_mm is built around (see its header comment, and wall-count-investigation.md
// sections 2 and 5 for the Arachne count rule this number comes from).
//
// Pre-fix both parities fail: the old band `ext_w + (N-1)*s` is 1.264mm at these settings
// against a 1.307mm optimum, and the old 0.3mm notch took the even layers down to 0.964mm.
TEST_CASE("multi_material_segmentation_by_painting: a walls-mode band delivers the full N-bead budget on BOTH layer parities (F3+F4)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/20., PLUS_X_FACE,
                                             pdmWalls, /*walls=*/3, /*paint_depth_mm=*/1.5,
                                             /*layer_height=*/0.2, print);

    // Read the real flows off the sliced object rather than hardcoding, exactly as the
    // segmentation itself does.
    float ext_w = 0.f, ext_s = 0.f, s = 0.f;
    for (size_t region_idx = 0; region_idx < object->num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object->printing_region(region_idx);
        ext_w = std::max(ext_w, region.flow(*object, frExternalPerimeter, object->config().layer_height).width());
        ext_s = std::max(ext_s, region.flow(*object, frExternalPerimeter, object->config().layer_height).spacing());
        s     = std::max(s,     region.flow(*object, frPerimeter,         object->config().layer_height).spacing());
    }
    REQUIRE(s > 0.f);

    // The arithmetic half of F4, pinned against this object's own flow: the notch the
    // segmentation applies is at most a quarter of one perimeter spacing, which is exactly the
    // count-window margin the band builds in - so it cannot move the strip across a bead-count
    // boundary. RED pre-F4 (the shipped default was 0.3mm, ~0.70 * spacing).
    const double interlock_mm = paint_depth_interlocking_depth_mm(pdmWalls, object->config().mmu_segmented_region_interlocking_depth.value, s);
    CHECK(interlock_mm <= 0.25 * double(s) + 1e-6);

    // The geometric half: the N-bead optimum depth is claimed on both parities.
    const double n_bead_optimum = 3.0 * double(s) + 2.0 * (double(ext_w) - double(ext_s));
    const double probe_mm       = n_bead_optimum - 0.02;
    REQUIRE(probe_mm > 0.);
    // I-4: ...and NOT MORE than the spec band. "N beads fit" is a two-sided condition (see the
    // margin table in test_paint_depth.cpp's F3+F4 invariant test); the tightest number in the
    // whole design is a 0.1119mm UPWARD margin at odd N, and nothing used to bound that side at
    // all - a band 0.12mm wider would silently turn "3 walls" into 4 with every assertion here
    // still green. The over-probe is measured against the SPEC band (optimum + one quarter
    // spacing), deliberately not against paint_depth_band_mm's own output, so it stays
    // discriminating if the production formula drifts.
    const double band_spec      = n_bead_optimum + 0.25 * double(s);
    const double over_probe_mm  = band_spec + 0.05;

    REQUIRE(object->layer_count() >= 10);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
    REQUIRE(bb.defined);
    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    const Point   probe(coord_t(bb.max.x() - scale_(probe_mm)), mid_y);
    const Point   over_probe(coord_t(bb.max.x() - scale_(over_probe_mm)), mid_y);

    CHECK(any_contains(extruder2_claim_for_layer(*object, even_layer), probe));
    CHECK(any_contains(extruder2_claim_for_layer(*object, odd_layer), probe));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, even_layer), over_probe));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, odd_layer), over_probe));
}

// ===========================================================================================
// WAVE A: the F1 inset's reference layer (I-6), the classic wall generator's band floor and
// its gap-fill filament (classic-generator-investigation.md).
// .superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md
// .superpowers/sdd/2026-08-31-paint-depth/classic-generator-investigation.md
// ===========================================================================================

// Builds and slices a square frustum with the given cap facets painted. pdmUnlimited for the
// same reason slice_capped_slab() uses it: only a horizontal cap is painted, so no layer's own
// cross-section carries a painted boundary and the entire claim comes from the vertical
// projection/descent path these tests target.
PrintObject *slice_painted_frustum(double bottom, double top, double height,
                                    const std::vector<int> &painted_cap_facets,
                                    int top_shell_layers, int bottom_shell_layers, Print &print)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-frustum-taper.stl";
    ModelVolume *volume  = object->add_volume(make_square_frustum(bottom, top, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_cap_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = top_shell_layers;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.0;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = bottom_shell_layers;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = 0.0;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// I-6 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md): F1's central
// design choice is that its one-wall-stack inset is taken at `input_expolygons[last_idx]` - the
// layer the claim is DEPOSITED on - not at `input_expolygons[layer_idx]`, the painted surface
// layer. The outward-bleed investigation rejected the layer_idx variant specifically because it
// measures clearance on the wrong cross-section for objects that NARROW AWAY from the painted
// face (an undercut, a waist, an overhang below a painted top).
//
// Every fixture in this suite was either a prism (where the two are literally the same
// ExPolygons) or make_square_frustum(40, 22, 6) painted on its TOP cap - which WIDENS downward,
// where an inset taken at layer_idx is strictly MORE conservative than one taken at last_idx,
// so the frustum test passes under both. Nothing anywhere distinguished them, in either
// direction: someone "simplifying" last_idx to layer_idx would read as a cleanup and bring the
// exterior bleed back on every undercut with a green suite.
//
// Both directions are covered here, and both narrow in the direction of descent:
//   TOP    make_square_frustum(22, 40, 6) painted on its top cap - going DOWN from the cap the
//          cross-section shrinks 40 -> 22.
//   BOTTOM make_square_frustum(40, 22, 6) painted on its bottom cap - going UP from the cap the
//          cross-section shrinks 40 -> 22. (No new mesh; it is the existing F1 fixture flipped.)
//
// Arithmetic, identical for both (0.1mm layers, 0.15mm of taper per layer, wall stack =
// extrusion_width + extrusion_spacing = 0.87854mm). Writing H_k for the contour half-width at
// descent depth k and H_0 for the painted surface layer's own:
//   correct (last_idx): claim edge = H_k - 0.87854          => always 0.87854mm in
//   wrong   (layer_idx): claim edge = min(H_k, H_0 - 0.87854) => only 0.87854 - 0.15k in
// so a probe 0.3mm inside the layer's own contour is base under the correct inset at every
// depth, and painted under the wrong one from depth 4 on (0.87854 - 0.15*4 = 0.279 < 0.3). At
// depth 5 the margin is 0.17mm on the RED side and 0.58mm on the green side.
TEST_CASE("multi_material_segmentation_by_painting: on geometry that narrows away from the painted cap the F1 inset is measured on the DEPOSIT layer, not the painted one (I-6)", "[paintdepth]")
{
    SECTION("top cap, cross-section narrowing downward") {
        Print        print;
        // 22mm at z=0 widening to 40mm at z=6: descending from the painted top cap, every layer
        // is narrower than the one above it.
        PrintObject *object = slice_painted_frustum(22., 40., 6., TOP_CAP_FACE,
                                                     /*top_shell_layers=*/8, /*bottom_shell_layers=*/3, print);

        const size_t top_index = object->layer_count() - 1;
        REQUIRE(top_index >= 8);

        // Positive control: the painted surface layer itself is claimed out to its silhouette.
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index),
                           layer_edge_probe(*object, top_index, 0.2)));

        for (size_t depth = 4; depth <= 5; ++depth) {
            const size_t layer_idx = top_index - depth;
            CAPTURE(depth);
            // RED with the inset taken at layer_idx: the claim reaches to within
            // 0.87854 - 0.15*depth mm of this layer's own contour, i.e. 0.28/0.13mm.
            CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                                     layer_edge_probe(*object, layer_idx, 0.3)));
            // ...and the claim is still real and deep at the same depths.
            CHECK(any_contains(extruder2_claim_for_layer(*object, layer_idx),
                               layer_edge_probe(*object, layer_idx, 1.5)));
        }
    }

    SECTION("bottom cap, cross-section narrowing upward") {
        Print        print;
        // The existing F1 fixture, painted on its BOTTOM cap instead: 40mm at z=0 tapering to
        // 22mm at z=6, so ascending from the painted cap every layer is narrower than the one
        // below it. Exercises the bottom descent loop's own copy of the F1 inset.
        PrintObject *object = slice_painted_frustum(40., 22., 6., BOTTOM_CAP_FACE,
                                                     /*top_shell_layers=*/3, /*bottom_shell_layers=*/8, print);

        REQUIRE(object->layer_count() >= 8);

        // Positive control on the painted surface layer (layer 0).
        CHECK(any_contains(extruder2_claim_for_layer(*object, 0), layer_edge_probe(*object, 0, 0.2)));

        for (size_t depth = 4; depth <= 5; ++depth) {
            CAPTURE(depth);
            CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, depth),
                                     layer_edge_probe(*object, depth, 0.3)));
            CHECK(any_contains(extruder2_claim_for_layer(*object, depth),
                               layer_edge_probe(*object, depth, 1.5)));
        }
    }
}

// Item 8, classic-generator-investigation.md sections 2b/2c/3/6: the classic wall generator
// cannot render a painted band narrower than TWO properly-spaced lines. offset_ex() on a strip
// always returns both of its boundaries, so a band of width W emits an even number of
// external-width loops or nothing at all, and the narrowest honest classic band is
// ext_perimeter_width + ext_perimeter_spacing = 0.85708mm at 0.45mm lines / 0.2mm layers -
// which is exactly one `wall_stack`, the same quantity F1 insets its top/bottom claim by.
//
// At paint_depth_walls = 1 the Arachne-shaped band is 0.59469mm, and on classic that produces
// three separate real defects:
//   - the two depth-0 loops sit 0.14mm apart centre-to-centre while each is 0.45mm wide:
//     +48% over-extrusion along the whole painted boundary, knowingly uncompensated
//     (PerimeterGenerator.cpp:1419 "FIXME evaluate the overlaps");
//   - on any profile with outer_wall_line_width > 1.25*s + 2h(1-pi/4) the band drops below
//     ext_perimeter_width and the N=1 painted region produces ZERO extrusions on every layer;
//   - worse, F1's top/bottom claim is inset by one wall_stack while the lateral band is not
//     band-clamped against it, so whenever band < wall_stack the union leaves the BASE region
//     holding a closed ring of width wall_stack - band (0.26mm here) on every sub-surface shell
//     layer - and classic prints that ring as NOTHING (offset_ex returns empty at i = 0, last is
//     cleared, gaps are only collected from i >= 1). A genuine void ring under every painted cap.
// Flooring the band at wall_stack closes all three, and closes the last one BY CONSTRUCTION:
// band(1) and the F1 inset become the same number, so the lateral band and the top/bottom claim
// meet exactly.
//
// It must NOT be unconditional. Arachne's 1 -> 2 bead boundary is T > (1 + split_thr)*ext_s =
// 0.6476mm (RedistributeBeadingStrategy.cpp:42-48); today's band(1) gives T = 0.5357 -> 1 bead,
// while a floored band(1) would give T = 0.8356 -> 2 beads, breaking the "1 wall means 1 loop"
// contract F3 established. The Arachne half of this test is that pin.
TEST_CASE("multi_material_segmentation_by_painting: the walls-mode band is floored at one wall stack on the classic generator only (classic floor)", "[paintdepth]")
{
    // 0.45mm lines / 0.2mm layers => spacing 0.40708, band(1) = 0.59469,
    // floor = ext_w + ext_s = 0.85708, notch (walls mode) = 0.25*spacing = 0.10177.
    //   classic: 0.85708 odd / 0.75531 even      arachne: 0.59469 odd / 0.49292 even
    SECTION("classic: the band is at least one wall stack deep on both parities") {
        Print        print;
        PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/6., PLUS_X_FACE,
                                                 pdmWalls, /*walls=*/1, /*paint_depth_mm=*/1.5,
                                                 /*layer_height=*/0.2, print, /*interlocking_depth=*/-1.,
                                                 PerimeterGeneratorType::Classic);
        REQUIRE(object->layer_count() >= 10);
        const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
        const size_t odd_layer  = even_layer + 1;
        REQUIRE(even_layer % 2 == 0);

        const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
        REQUIRE(bb.defined);
        const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
        auto probe = [&](double inset_mm) { return Point(coord_t(bb.max.x() - scale_(inset_mm)), mid_y); };

        for (size_t layer_idx : {even_layer, odd_layer}) {
            CAPTURE(layer_idx);
            const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);
            // RED pre-fix on both parities: the unfloored band is 0.595 / 0.493mm.
            CHECK(any_contains(claim, probe(0.65)));
            // ...and the floor is a floor, not "as deep as you like".
            CHECK_FALSE(any_contains(claim, probe(0.95)));
        }
    }

    SECTION("arachne: the same band is NOT floored - one wall still means one bead") {
        Print        print;
        PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/6., PLUS_X_FACE,
                                                 pdmWalls, /*walls=*/1, /*paint_depth_mm=*/1.5,
                                                 /*layer_height=*/0.2, print, /*interlocking_depth=*/-1.,
                                                 PerimeterGeneratorType::Arachne);
        REQUIRE(object->layer_count() >= 10);
        const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
        const size_t odd_layer  = even_layer + 1;
        REQUIRE(even_layer % 2 == 0);

        const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
        REQUIRE(bb.defined);
        const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
        auto probe = [&](double inset_mm) { return Point(coord_t(bb.max.x() - scale_(inset_mm)), mid_y); };

        for (size_t layer_idx : {even_layer, odd_layer}) {
            CAPTURE(layer_idx);
            const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);
            CHECK(any_contains(claim, probe(0.40)));
            // Would FAIL if the floor were applied unconditionally (0.65 < 0.85708).
            CHECK_FALSE(any_contains(claim, probe(0.65)));
        }
    }
}

// Recursive extrusion counters. The perimeter/fill collections are trees of collections, and
// the quantities the classic-generator investigation derives (2k loops plus one gap-fill line
// across a painted band) are counts of leaf extrusions, not of top-level entries.
size_t count_loops_recursive(const ExtrusionEntity *entity)
{
    if (entity->is_collection()) {
        size_t n = 0;
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            n += count_loops_recursive(child);
        return n;
    }
    return entity->is_loop() ? 1 : 0;
}

size_t count_role_recursive(const ExtrusionEntity *entity, ExtrusionRole role)
{
    if (entity->is_collection()) {
        size_t n = 0;
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            n += count_role_recursive(child, role);
        return n;
    }
    return entity->role() == role ? 1 : 0;
}

// The LayerRegion apply_mm_segmentation carved out for the Extruder2 paint claim, or nullptr.
const LayerRegion *extruder2_layer_region(const PrintObject &object, size_t layer_idx)
{
    const int local_id = extruder2_local_region_id(object);
    if (local_id < 0)
        return nullptr;
    const Layer *layer = object.get_layer(int(layer_idx));
    if (local_id >= layer->region_count())
        return nullptr;
    return layer->get_region(local_id);
}

// Same construction as process_z_interface_cube() above (Print::process(), not
// PrintObject::slice(), because perimeter generation and fill only run inside process()), but a
// single painted cube with the wall generator and paint_infill_override as parameters.
PrintObject *process_painted_cube(double xy_size, double height, const std::vector<int> &painted_facets,
                                   PaintDepthMode mode, int walls, bool paint_infill_override,
                                   PerimeterGeneratorType wall_generator, Print &print)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-generator.stl";
    ModelVolume *volume  = object->add_volume(make_cube(xy_size, xy_size, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(mode, walls, paint_infill_override);
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    print.process();
    PrintObject *out_object = print.objects_mutable().front();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// Items 9 + 10, classic-generator-investigation.md section 0 and section 4. Two things nothing
// in this suite covered before: the CLASSIC wall generator (every test here has run Arachne,
// the harness default, and none has ever asserted on extrusions at all), and the filament the
// painted band's GAP FILL prints in.
//
// The band tiling (section 0's table): across a 1.40885mm band at N=3 with wall_loops = 2,
// classic lays one onion iteration - which on an ANNULUS emits two loops, the expolygon's
// contour and its hole, both at depth 0 hence both erExternalPerimeter - plus one gap-fill line
// down the middle whose medial-axis width is 0.50885mm. That middle line is 36% of the painted
// depth. Arachne, on the same band, fits three beads and emits NO gap fill at all
// (process_arachne never calls this->gap_fill), which is exactly why the leak below was never
// observable from an Arachne slice.
//
// The leak: gap fill is stored in LayerRegion::thin_fills and copied into layerm->fills by
// Fill::make_fill(), so GCode::process_layer used to read it out of the INFILL bucket and hand
// it sparse_infill_filament(region). The painted PrintRegion sets wall_filament and
// solid_infill_filament to the painted extruder unconditionally but sparse_infill_filament only
// when paint_sparse_infill is on (PrintApply.cpp), so with "Paint sparse infill" unchecked the
// middle 36% of the painted band - sitting directly behind the single painted outer loop -
// silently flipped to the BASE filament, while the option's own tooltip promises that "walls
// and solid infill still print in the painted filament". Deciding from the ROLE instead
// (is_infill(erGapFill) is false) routes it to wall_filament, which is what LayerTools::extruder
// (ToolOrdering.cpp) has always said this collection needs - the two used to disagree.
TEST_CASE("multi_material_segmentation_by_painting: on the classic generator the painted band's gap fill follows the painted filament while sparse infill stays base", "[paintdepth]")
{
    Print        print;
    PrintObject *object = process_painted_cube(/*xy_size=*/20., /*height=*/4., ALL_SIDE_FACE,
                                                pdmWalls, /*walls=*/3, /*paint_infill_override=*/false,
                                                PerimeterGeneratorType::Classic, print);
    REQUIRE(object->layer_count() >= 12);

    const PrintRegionConfig &cfg = extruder2_region_config(*object);
    // The precondition the leak needs: walls painted, sparse infill deliberately left base.
    REQUIRE(cfg.wall_filament.value == 2);
    REQUIRE(cfg.solid_infill_filament.value == 2);
    REQUIRE(cfg.sparse_infill_filament.value == 1);

    // The fix, at the rule GCode::process_layer now shares with PrintRegion.cpp. RED pre-fix:
    // gap fill resolved to SparseInfill, i.e. filament 1 - the base colour.
    CHECK(int(fill_filament_source(cfg, erGapFill))       == int(FillFilamentSource::Wall));
    // ...and the option still does what it says for real infill.
    CHECK(int(fill_filament_source(cfg, erInternalInfill)) == int(FillFilamentSource::SparseInfill));
    CHECK(int(fill_filament_source(cfg, erSolidInfill))    == int(FillFilamentSource::SolidInfill));

    // The leak is reachable, not hypothetical: on classic the painted band really does carry
    // gap fill on a mid (non-shell) layer. 4mm at 0.2mm layers = 20 layers; layer 10 is past the
    // bottom shell and below the top shell.
    const size_t       mid_layer = 10;
    const LayerRegion *painted   = extruder2_layer_region(*object, mid_layer);
    REQUIRE(painted != nullptr);
    const size_t gap_fills = count_role_recursive(&painted->thin_fills, erGapFill);
    const size_t loops     = count_loops_recursive(&painted->perimeters);
    CAPTURE(gap_fills);
    CAPTURE(loops);
    // Band tiling on classic (section 0's table): one onion iteration on an annulus = 2 loops,
    // plus the single gap-fill line down the middle of the band.
    CHECK(loops == 2);
    CHECK(gap_fills >= 1);
}

// The Arachne half of the same statement, and the reason the leak above was invisible: the
// painted band is tiled with three real beads and no gap fill whatsoever, so
// sparse_infill_filament never got a chance to claim any of it.
TEST_CASE("multi_material_segmentation_by_painting: on the arachne generator the same painted band is tiled with beads and emits no gap fill", "[paintdepth]")
{
    Print        print;
    PrintObject *object = process_painted_cube(/*xy_size=*/20., /*height=*/4., ALL_SIDE_FACE,
                                                pdmWalls, /*walls=*/3, /*paint_infill_override=*/false,
                                                PerimeterGeneratorType::Arachne, print);
    REQUIRE(object->layer_count() >= 12);

    const size_t       mid_layer = 10;
    const LayerRegion *painted   = extruder2_layer_region(*object, mid_layer);
    REQUIRE(painted != nullptr);
    const size_t gap_fills = count_role_recursive(&painted->thin_fills, erGapFill);
    const size_t loops     = count_loops_recursive(&painted->perimeters);
    const size_t surfaces  = painted->slices.surfaces.size();
    const size_t ext_loops = count_role_recursive(&painted->perimeters, erExternalPerimeter);
    const size_t int_loops = count_role_recursive(&painted->perimeters, erPerimeter);
    CAPTURE(gap_fills);
    CAPTURE(loops);
    CAPTURE(surfaces);
    CAPTURE(ext_loops);
    CAPTURE(int_loops);
    // The load-bearing difference, and the reason the classic-only filament leak was never
    // observable from an Arachne slice: process_arachne never calls this->gap_fill.
    CHECK(gap_fills == 0);
    // ...and the band is tiled with real, counted beads rather than classic's two loops plus one
    // variable-width gap-fill line. The EXACT count is deliberately not pinned: this fixture is a
    // square annulus, and at each of its four corners the local width across the band is band*sqrt(2)
    // (~2.0mm against 1.41mm along the flats), so Arachne's variable-width beading correctly adds
    // short extra beads there - measured 6 loops plus 4 open paths, 2 of them external. Pinning
    // that would pin Arachne's corner behaviour, not this feature's. The captures above record it.
    CHECK(loops > 2);
}

// ===========================================================================================
// WAVE B - OPTION N: the painted claim is a CONSTANT-THICKNESS SHELL measured NORMAL to the
// painted surface. .superpowers/sdd/2026-08-31-paint-depth/curved-gap-design.md
//
// Before this wave the claim was max(lateral band, layer-count shell), and the vertical half
// was switched off entirely on any slope steeper than atan(layer_height / wall_stack) (6.49 deg
// at 0.1mm layers) by exposed_surface_part(). Between there and ~24 deg the paint was therefore
// the lateral band alone - normal thickness band*sin(theta), i.e. 0.25mm at 10 deg against
// 0.60mm on a flat top: the dead band the user's domed features (eyes, cheeks) live in.
//
// Option N removes that gate and bounds the descent by NORMAL DEPTH D (the same band value,
// re-read as a thickness) instead of by top/bottom_shell_layers. The descent was ALREADY
// slope-correct - a claim deposited m layers below its painted surface layer lands at lateral
// inset [m*r, (m+1)*r] from the deposit layer's own contour, where r = layer_height/tan(theta)
// is the staircase run - so a descent of M = ceil(D/layer_height) layers reaches lateral inset
// M*r, i.e. normal thickness M*r*sin(theta) = M*layer_height*cos(theta) ~= D*cos(theta). The
// lateral band supplies D*sin(theta) on the same geometry, so the union is D*max(cos, sin).
//
// Arithmetic used by every fixture below (0.45mm outer AND inner wall, 0.4 nozzle, 0.1mm
// layers): ext_perimeter_spacing = perimeter_spacing = 0.45 - 0.1*(1 - pi/4) = 0.428540,
// wall_stack = 0.45 + 0.428540 = 0.878540, band(3) = 3*0.428540 + 2*(0.45 - 0.428540) +
// 0.25*0.428540 = 1.435675, band(1) = 0.578595 (Arachne) / 0.878540 (classic, Wave A's floor).
// M = ceil(1.435675 / 0.1) = 15 at walls = 3, ceil(0.878540 / 0.1) = 9 at the classic walls = 1.
//
// HONEST BOUND, recorded here because it is not in the design doc's headline table: the
// staircase ring top_ex is itself passed through opening_ex(top_ex, small_region_threshold)
// (= 0.1125mm radius at a 0.45mm outer wall with gap fill on) before the descent ever starts,
// so a ring narrower than 0.225mm is erased and there is NO vertical claim to deepen. That
// bounds Option N's reach at theta < atan(layer_height / 0.225) = 23.96 deg at 0.1mm layers
// (41.6 deg at 0.2mm) - well above the 6.49 deg cliff it replaces and covering the user's
// shallow features, but short of the 58.5 deg F1 self-suppression the design derives. At every
// practical layer height the opening filter binds strictly before F1 does (F1 needs
// r < 0.627*layer_height, the opening needs r < 0.225, and 0.627*h < 0.225 for h < 0.359mm).
// The steep-slope test below therefore pins the SUPPRESSION, and names which guard delivers it.
// ===========================================================================================

// Same construction as slice_painted_frustum() above, but with the paint depth actually BOUNDED
// (mode/walls), the layer height and the wall generator as parameters, and the shell settings
// pinned so the "max(effective shell, normal-depth layers)" descent bound is exact:
// top_shell_layers = 4 / top_shell_thickness = 0.6 at 0.1mm layers is a 6-layer effective shell,
// so any descent deeper than 6 layers is unambiguously the normal-depth bound and not the shell.
// inner_wall_line_width is pinned for the same reason slice_painted_box() pins it: the band is
// driven by the frPerimeter spacing too, so both widths must be known for the band to be exact.
PrintObject *slice_bounded_frustum(double bottom, double top, double height,
                                    const std::vector<int> &painted_facets,
                                    PaintDepthMode mode, int walls, double layer_height, Print &print,
                                    PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-normal-shell.stl";
    ModelVolume *volume  = object->add_volume(make_square_frustum(bottom, top, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(mode, walls);
    config.option<ConfigOptionFloat>("layer_height")->value                     = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = layer_height;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionInt>("top_shell_layers")->value                   = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value              = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value                = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value           = 0.0;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// T1, the headline. make_square_frustum(40.392, 18, 3): the half-width runs 20.196 -> 9.0 over
// 3mm, so tan(theta) = 3/11.196 and theta = 15.000 deg exactly; at 0.1mm layers the staircase
// run is r = 0.37320mm per layer (comfortably above the 0.225mm opening filter). 30 layers.
//
// With walls = 3 the descent depth is max(6-layer shell, ceil(1.435675/0.1) = 15) = 15, so a
// mid layer collects the surface bands of the 15 layers at and above it and the claim reaches
// lateral inset 15 * 0.37320 = 5.598mm from its own contour - normal thickness
// 5.598 * sin(15) = 1.4489mm (the design's ideal D*cos(15) = 1.3868 rounded up by the one-layer
// quantisation of M). Today it reaches the band alone, 1.435675mm laterally = 0.372mm normal.
//
// The probe layer is 12, not the geometric middle: contributions come from layers 12..26, all
// interior. The topmost layer's slab is only half a layer tall (there is no zs[30] to bound it),
// so its ring is 0.187mm rather than 0.373mm wide and the reach from a layer within 14 of the
// top would be short by that amount - a needless 0.19mm of slack in the negative probe.
TEST_CASE("multi_material_segmentation_by_painting: a shallow painted slope is claimed to the full NORMAL depth, not to the lateral band (Wave B / Option N)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40.392, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 27);

    const size_t     probe_layer = 12;
    const ExPolygons claim       = extruder2_claim_for_layer(*object, probe_layer);

    // RED today: the claim stops at the 1.435675mm lateral band, so 3.0mm in is base.
    CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, 3.0)));
    // ...and it really does reach the full normal depth, not merely "more than the band":
    // 5.0mm is inside the 5.598mm reach (margin 0.60mm).
    CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, 5.0)));
    // The upper bound, which is what makes this a test of a DEPTH rather than of "more paint":
    // 6.0mm is past the 5.598mm reach (margin 0.40mm). Bracketing the reach in [5.0, 6.0]
    // brackets the normal thickness in [1.294, 1.553]mm around D = 1.436 / M*h*cos = 1.449.
    CHECK_FALSE(any_contains(claim, layer_edge_probe(*object, probe_layer, 6.0)));
}

// The break-placement pin the design doc calls out as the difference between Option N and a
// silent no-op (curved-gap-design.md section 6 hazard 1). On a slope the full-width term is
// EMPTY for the near descent steps - the ring deposited m layers down sits at inset
// [m*r, (m+1)*r] and F1 holds it one wall_stack (0.878540mm) clear of the deposit layer's
// contour, so nothing survives until (m+1)*r > 0.878540, i.e. m >= 2 at 15 deg. The legacy
// eroded term is empty there too (it insets by m*wall_stack, and the ring is only 0.373mm
// wide). An `if (last.empty()) break;` at the bottom of the loop therefore fires at m = 1 and
// the whole change evaporates with a green T1... except that T1's 3.0mm probe needs m = 8.
//
// This case makes that explicit and graded: each probe below can only be satisfied by the
// descent step that owns its slot, so the ladder fails at the FIRST step the break truncates
// rather than all at once, and the failure message names the depth.
TEST_CASE("multi_material_segmentation_by_painting: the normal-depth descent is not terminated by its empty near steps (break placement, Wave B)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40.392, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 27);

    const size_t     probe_layer = 12;
    const ExPolygons claim       = extruder2_claim_for_layer(*object, probe_layer);

    // inset -> the descent step that supplies it (slot m spans [m*0.37320, (m+1)*0.37320]):
    //   1.6 -> m=4    2.5 -> m=6    3.5 -> m=9    4.5 -> m=12    5.0 -> m=13
    // All are beyond the 1.435675mm lateral band, so none can be satisfied any other way.
    for (double inset_mm : {1.6, 2.5, 3.5, 4.5, 5.0}) {
        CAPTURE(inset_mm);
        CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, inset_mm)));
    }
}

// T2 - F1's no-exterior-bleed invariant, re-pinned at the NEW depth. GREEN before and after:
// the point is that deleting exposed_surface_part() and descending 15 layers instead of 6 must
// not let a painted CAP own the exterior perimeter of the unpainted sloped wall below it.
//
// Same frustum, cap only. The sloped walls carry no painted boundary on any layer's contour, so
// the entire claim is the cap's descent - there is no lateral band anywhere to mask a regression.
// Cap half-width 9.0; contour half-width at depth m below the cap layer H_m = 9.1866 + 0.37320m;
// F1 holds the claim at min(9.0, H_m - 0.878540). So:
//   negative probe 0.3mm in: claim edge is at least 0.878540mm in at m = 1..2 and 0.933mm or
//     more from m = 3 on - base at every depth, margin >= 0.58mm;
//   positive probe 1.5mm in: inside the claim for m = 1..3 (margins 0.62 / 0.57 / 0.19mm); at
//     m = 4 the layer has grown past the fixed 9.0mm cap footprint and 1.5mm in is legitimately
//     outside it, which is geometry, not a regression - hence the shorter positive range.
TEST_CASE("multi_material_segmentation_by_painting: a painted cap's deepened descent still leaves the unpainted sloped exterior base-coloured (F1 pin, Wave B)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40.392, 18., 3., TOP_CAP_FACE,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 10);

    for (size_t depth = 1; depth <= 5; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - depth),
                                 layer_edge_probe(*object, top_index - depth, 0.3)));
    }
    // ...and the invariant is not satisfied by claiming nothing: the shell under the painted cap
    // IS claimed at the same depths.
    for (size_t depth = 1; depth <= 3; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index - depth),
                           layer_edge_probe(*object, top_index - depth, 1.5)));
    }
}

// T3 - steep-slope suppression. make_square_frustum(40, 34, 6): half-width 20 -> 17 over 6mm, so
// tan(theta) = 2 and theta = 63.435 deg, above the design's derived F1 self-suppression angle
// atan(D / wall_stack) = 58.5 deg. r = 0.05mm per 0.1mm layer.
//
// Which guard actually fires, stated honestly (see the section header): at 0.05mm the staircase
// ring is erased by opening_ex(top_ex, 0.1125) before the descent starts, so there is no vertical
// claim at all - the F1 reach test (M*r = 0.75mm < 0.878540mm) would also suppress it, but the
// opening gets there first and does so at every layer height below 0.359mm. Either way the claim
// on a steep painted wall is the lateral band alone, which is the property that matters and the
// one a future widening must not quietly break.
TEST_CASE("multi_material_segmentation_by_painting: a 63-degree painted slope gains no normal-depth descent (steep suppression, Wave B)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40., 34., 6., FRUSTUM_SLOPED_WALLS,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 40);

    const size_t     probe_layer = 20;
    const ExPolygons claim       = extruder2_claim_for_layer(*object, probe_layer);

    // The lateral band (1.335675mm on this even layer after the 0.1mm interlocking notch) is
    // present and real...
    CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, 1.0)));
    // ...and nothing deeper is, which is exactly what the anti-smear guard has always demanded.
    CHECK_FALSE(any_contains(claim, layer_edge_probe(*object, probe_layer, 2.0)));
}

// How far in from the given layer's own contour the Extruder2 claim reaches, scanned outward in
// 0.05mm steps. The claim is contiguous from the contour on every fixture that uses this (the
// lateral band starts at 0 and overlaps the first surviving descent slot), so the last claimed
// step is the outer edge, to within the step.
//
// Deliberately builds its own probe points rather than calling layer_edge_probe() per step: that
// helper carries a REQUIRE, and a couple of hundred of them per scan would make the suite's
// assertion COUNT a function of the measured geometry - a gate baseline that moves whenever the
// claim does. One REQUIRE here, then arithmetic.
double claim_reach_mm(const PrintObject &object, size_t layer_idx, double max_scan_mm = 12.0)
{
    const BoundingBox bb = get_extents(object.get_layer(int(layer_idx))->lslices);
    REQUIRE(bb.defined);
    const coord_t    mid_y        = (bb.min.y() + bb.max.y()) / 2;
    const ExPolygons claim        = extruder2_claim_for_layer(object, layer_idx);
    double           last_claimed = 0.;
    for (double inset = 0.05; inset <= max_scan_mm; inset += 0.05)
        if (any_contains(claim, Point(coord_t(bb.max.x() - scale_(inset)), mid_y)))
            last_claimed = inset;
        else
            break;
    return last_claimed;
}

// The design's headline table, executable. curved-gap-design.md section 3 predicts a normal
// thickness of D*cos(theta) once Option N lands, against band*sin(theta) before it:
//
//   theta      10 deg   15 deg   20 deg   25 deg
//   before      0.249    0.372    0.491    0.607     (= 1.435675 * sin theta)
//   design      1.414    1.387    1.349    1.301     (= 1.435675 * cos theta)
//
// What this build actually delivers, and why it differs at each end:
//   * the descent depth is a whole number of layers, M = ceil(D / layer_height) = 15, so the
//     realised thickness is M*layer_height*cos(theta) = 1.5*cos(theta), i.e. 1.477 / 1.449 /
//     1.410 - the design's figure rounded UP by the layer quantisation, never down;
//   * 25 deg is NOT reached, and this is the one place the design's table is optimistic. The
//     staircase ring is r = layer_height/tan(theta) = 0.2145mm wide there, and top_ex is passed
//     through opening_ex(top_ex, small_region_threshold = 0.1125) before the descent starts, so a
//     ring under 0.225mm is erased and there is no vertical claim left to deepen. That puts a
//     hard ceiling on Option N at theta < atan(layer_height / 0.225) = 23.96 deg at 0.1mm layers
//     (41.6 deg at 0.2mm). Lifting it means lowering the #7104 sliver guard, which the design
//     considered as its Option B and rejected; 25 deg therefore still gets band*sin(theta).
//
// All four frustums taper to an 18mm top over 3mm of height, so the slope is set purely by the
// base width: half-width delta = 3/tan(theta), bottom = 2*(9 + delta).
TEST_CASE("multi_material_segmentation_by_painting: normal thickness across slopes (Wave B / Option N headline numbers)", "[paintdepth]")
{
    constexpr double kPi = 3.14159265358979323846;
    struct SlopeCase { double degrees; double bottom; bool normal_shell; };
    // bottom = 2*(9 + 3/tan(theta)): 10 deg -> 52.0276, 15 -> 40.392, 20 -> 34.4848, 25 -> 30.8670.
    const SlopeCase cases[] = {
        {10., 52.0276, true},
        {15., 40.3920, true},
        {20., 34.4848, true},
        {25., 30.8670, false}, // ring 0.2145mm < 0.225mm: erased by the thin-projection filter.
    };

    for (const SlopeCase &c : cases) {
        DYNAMIC_SECTION("slope " << c.degrees << " deg") {
            Print        print;
            PrintObject *object = slice_bounded_frustum(c.bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                         pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
            REQUIRE(object->layer_count() >= 27);

            const double theta      = c.degrees * kPi / 180.;
            const double reach      = claim_reach_mm(*object, /*layer_idx=*/12);
            const double normal_mm  = reach * std::sin(theta);
            CAPTURE(c.degrees);
            CAPTURE(reach);
            CAPTURE(normal_mm);

            if (c.normal_shell) {
                // D = 1.435675mm; realised M*h*cos(theta) = 1.5*cos(theta). The window admits the
                // layer quantisation above and the 0.05mm scan step below, and excludes both the
                // old band*sin(theta) (0.249 / 0.372 / 0.491) and an unbounded claim.
                CHECK(normal_mm > 1.36);
                CHECK(normal_mm < 1.58);
            } else {
                // The lateral band alone on this even layer: 1.435675 - 0.1 notch = 1.335675mm of
                // reach, 0.5645mm of normal thickness. Bounded well below the 3.217mm reach a
                // normal-thickness descent would give here, so this fails loudly - and tells the
                // next reader why - if the thin-projection filter ever changes.
                CHECK(reach > 1.0);
                CHECK(reach < 2.0);
            }
        }
    }
}

// "Paint depth bounds PAINT." The base filament (color_idx 0) is not a paint claim: its
// top/bottom claim exists only to stop a neighbouring painted colour smearing across the solid
// shell under an UNPAINTED cap, and that contract is written in shell terms. Giving it the same
// normal thickness inverts the feature, because merge_segmented_layers trims every extruder's
// LATERAL claim by every extruder's TOP/BOTTOM claim - so a base claim descending D deep cuts the
// painted band back to one wall stack on every layer under any unpainted top or bottom face.
//
// This is not hypothetical: it is what the first Wave B build did, and the "millimetres mode
// honours the configured interlocking notch verbatim (I-3)" case above caught it (its 6.0mm band
// collapsed to 0.857mm). This case pins the rule by name so the next person to read a failure
// gets the reason rather than a puzzle. 40x40x6mm box, one side painted, 0.2mm layers, band 4.0mm:
// the base's own top shell is 4 layers (26..29) and cannot reach layer 15, while a base claim
// deepened to 4.0mm would span 20 layers (10..29) and would.
TEST_CASE("multi_material_segmentation_by_painting: the base filament's top/bottom claim is NOT given the paint's normal thickness (Wave B)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/6., PLUS_X_FACE,
                                             pdmMillimeters, /*walls=*/3, /*paint_depth_mm=*/4.0,
                                             /*layer_height=*/0.2, print);
    REQUIRE(object->layer_count() >= 20);

    const size_t      probe_layer = 15;
    const BoundingBox bb          = get_extents(object->get_layer(int(probe_layer))->lslices);
    REQUIRE(bb.defined);
    const coord_t    mid_y = (bb.min.y() + bb.max.y()) / 2;
    const ExPolygons claim = extruder2_claim_for_layer(*object, probe_layer);

    // The full 4.0mm band survives on a layer sitting under an unpainted top cap (the even-layer
    // notch is the stock 0.1mm, so the band here is 3.9mm - hence the 3.85mm probe).
    CHECK(any_contains(claim, Point(coord_t(bb.max.x() - scale_(3.85)), mid_y)));
    // ...and it is still the band, not an unbounded claim.
    CHECK_FALSE(any_contains(claim, Point(coord_t(bb.max.x() - scale_(4.1)), mid_y)));
}

// The D >= wall_stack gate (curved-gap-design.md section 5), and its one real interaction with
// Wave A. Below one wall stack the lateral band reaches only D while the F1-inset descent starts
// at wall_stack, so the base region would keep a sandwiched ring of width wall_stack - D on every
// sub-surface layer - a new sliver class. Hence the gate.
//
// At paint_depth_walls = 1 the two generators land on opposite sides of it, and it is Wave A's
// classic floor that puts them there:
//   classic  band(1) = max(0.578595, ext_w + ext_s) = 0.878540 == wall_stack exactly, so the
//            gate opens with the sandwiched ring at ZERO width by construction. M = 9, reach
//            9 * 0.37320 = 3.3588mm.
//   arachne  band(1) = 0.578595 < 0.878540, so the descent stays off and the claim is the band.
// A float ULP must not decide the classic case, which is why the production gate carries a
// SCALED_EPSILON slack; this pair is what would catch it if that slack were dropped.
TEST_CASE("multi_material_segmentation_by_painting: the normal-shell descent is gated on D >= one wall stack, which Wave A's classic floor is what opens at walls = 1", "[paintdepth]")
{
    SECTION("classic: the floored band(1) equals one wall stack, so the descent runs") {
        Print        print;
        PrintObject *object = slice_bounded_frustum(40.392, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                     pdmWalls, /*walls=*/1, /*layer_height=*/0.1, print,
                                                     PerimeterGeneratorType::Classic);
        REQUIRE(object->layer_count() >= 27);

        const size_t     probe_layer = 12;
        const ExPolygons claim       = extruder2_claim_for_layer(*object, probe_layer);
        // Inside the 3.3588mm reach (margin 0.36mm), far outside the 0.878540mm band.
        CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, 3.0)));
        // ...and bounded by it: D is a depth, not a licence.
        CHECK_FALSE(any_contains(claim, layer_edge_probe(*object, probe_layer, 4.0)));
    }

    SECTION("arachne: the unfloored band(1) is below one wall stack, so the descent stays off") {
        Print        print;
        PrintObject *object = slice_bounded_frustum(40.392, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                     pdmWalls, /*walls=*/1, /*layer_height=*/0.1, print,
                                                     PerimeterGeneratorType::Arachne);
        REQUIRE(object->layer_count() >= 27);

        const size_t     probe_layer = 12;
        const ExPolygons claim       = extruder2_claim_for_layer(*object, probe_layer);
        // The band alone: 0.478595mm on this even layer, 0.578595mm on odd ones.
        CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, 0.3)));
        CHECK_FALSE(any_contains(claim, layer_edge_probe(*object, probe_layer, 3.0)));
    }
}

// ===========================================================================================
// WAVE A FIX WAVE (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md). C-1, I-1, I-2.
// I-3's test lives further below, next to the process()-level fixtures it needs.
// ===========================================================================================

// I-1: the classic floor above guarantees the LATERAL band reaches wall_stack on ODD layers
// (region_cut_width == cut_width, the floored value, untouched). On EVEN layers
// cut_segmented_layers additionally subtracts the interlocking notch (region_cut_width =
// cut_width - notch), so the EFFECTIVE even-layer band fell short of wall_stack by exactly the
// notch (0.1mm default) - reopening, on every even layer, the same base-region ring under a
// painted band that the classic floor exists to close. This pins the floor's OWN promise ("meets
// wall_stack by construction") on the parity the classic-floor test above never distinguishes
// (it probes at 0.65mm, comfortably below BOTH 0.778540mm even and 0.878540mm odd at its 0.2mm
// layer height, so it cannot tell the two apart).
//
// Stock flow at 0.1mm layers / 0.45mm lines (matches the review's own worked numbers): s =
// 0.428540, wall_stack = ext_w + ext_s = 0.878540, notch (uncapped at the 0.1mm default, under
// the 0.25*s = 0.107135 cap) = 0.1. RED pre-fix: even-layer region_cut_width = 0.878540 - 0.1 =
// 0.778540, short of wall_stack by exactly the notch - so a probe 0.05mm inside wall_stack is
// claimed on the odd layer but not the even one.
//
// This pins the arithmetic invariant the review's "void ring" defect is built on (the classic
// floor's promised depth is not actually reached on even layers) rather than the downstream
// classic-toolpath consequence ("prints as nothing") directly - that would need per-role
// G-code/perimeter inspection of a base-coloured annulus a fraction of a millimetre wide, which
// this suite has no existing harness for and which the segmentation-level boundary this test
// pins is the precondition for either way: if the boundary itself is wrong on even layers (as it
// is pre-fix), the downstream toolpath question is moot.
TEST_CASE("multi_material_segmentation_by_painting: the classic floor closes the void ring on BOTH layer parities, not odd ones only (I-1)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/40., /*y=*/40., /*z=*/6., PLUS_X_FACE,
                                             pdmWalls, /*walls=*/1, /*paint_depth_mm=*/1.5,
                                             /*layer_height=*/0.1, print, /*interlocking_depth=*/-1.,
                                             PerimeterGeneratorType::Classic);
    REQUIRE(object->layer_count() >= 20);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
    REQUIRE(bb.defined);
    const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
    auto probe = [&](double inset_mm) { return Point(coord_t(bb.max.x() - scale_(inset_mm)), mid_y); };

    for (size_t layer_idx : {even_layer, odd_layer}) {
        CAPTURE(layer_idx);
        const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);
        // Just inside wall_stack (0.878540 - 0.05): claimed on BOTH parities - the floor's
        // promise. RED pre-fix on the even layer only.
        CHECK(any_contains(claim, probe(0.828540)));
        // Just outside wall_stack (+0.05): NOT claimed on either parity - the floor is a floor,
        // not unlimited.
        CHECK_FALSE(any_contains(claim, probe(0.928540)));
    }
}

// I-2: paint_depth_clamp_keep_core's ladder STEP (b) was already computed from the un-notched
// ladder_band (Wave A's own partial fix), but whether a part enters the ladder AT ALL -
// core/thin - was still decided from the (possibly notched) band, so the SET of geometry the
// ladder touches still alternated with parity. A part whose local half-thickness sits between
// the notched and un-notched band (a ~0.1mm-wide window at these flows) got the FULL band on one
// parity (thick enough to skip the ladder under the narrower notched threshold) and the ladder's
// degraded, much narrower claim on the other (not thick enough under the wider un-notched
// threshold) - the exact class of alternation this ladder exists to remove, just moved from
// "every layer" to "this narrow thickness window".
//
// walls = 5 at stock flow (0.45mm lines, 0.1mm layers, s = 0.428540; equal in/out walls so
// paint_depth_band_mm's 2*(ext_w-ext_s) term is zero): band = 5.25*s = 2.249835mm, notch
// (uncapped, 0.1 < 0.25*s = 0.107135) = 0.1, band_even = 2.149835mm. b0 = 0.25*2.249835 =
// 0.562459 >= min_claim_width (0.45mm) - the ladder is armed (reachable whenever
// paint_depth_walls >= 4, per the review's own derivation; 5 is used here for a more comfortable
// margin on that gate than 4 gives).
//
// Fixture: a 4.40mm-thin (half-thickness 2.20mm), 40mm-long, ALL-SIDE-painted bar - half-
// thickness 2.20mm sits inside (band_even, band) = (2.149835, 2.249835), the narrow window this
// finding is about, with ~0.05mm margin on each side.
TEST_CASE("multi_material_segmentation_by_painting: the degradation ladder's membership does not alternate with parity at walls = 5 (I-2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(/*x=*/4.40, /*y=*/40., /*z=*/6., ALL_SIDE_FACE,
                                             pdmWalls, /*walls=*/5, /*paint_depth_mm=*/1.5,
                                             /*layer_height=*/0.1, print, /*interlocking_depth=*/-1.);
    REQUIRE(object->layer_count() >= 20);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    for (size_t layer_idx : {even_layer, odd_layer}) {
        CAPTURE(layer_idx);
        const BoundingBox bb = get_extents(object->get_layer(int(layer_idx))->lslices);
        REQUIRE(bb.defined);
        const coord_t mid_y = (bb.min.y() + bb.max.y()) / 2;
        const ExPolygons claim = extruder2_claim_for_layer(*object, layer_idx);

        // Shallow probe: claimed either way - the degraded skin is real, at least one extrusion.
        CHECK(any_contains(claim, Point(coord_t(bb.max.x() - scale_(0.3)), mid_y)));
        // Deep probe (0.6mm), inside the FULL band but outside the ladder's ~0.562mm degraded
        // step: RED pre-fix on the even layer (pre-fix reach there is the full 2.149835mm band,
        // since this part was never classified as "thin" under the narrower notched threshold);
        // GREEN post-fix on both (the degraded ~0.562mm claim, identical on both parities).
        CHECK_FALSE(any_contains(claim, Point(coord_t(bb.max.x() - scale_(0.6)), mid_y)));
    }
}

// C-1 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md): LayerTools::wall_filament
// omits the grouped-manual-pattern resolution step that sparse_infill_filament (and
// solid_infill_filament) apply, so an UNPAINTED object using a grouped-manual-pattern mixed
// filament as both its wall and sparse-infill filament could get gap fill (which resolves via
// wall_filament, GCode.cpp's FillFilamentSource::Wall arm) on a DIFFERENT PHYSICAL EXTRUDER than
// its walls even though both config ids are equal - the two lookups diverge on a print with no
// painting anywhere.
//
// Smallest honest reproduction: no mesh painting, no gap-fill geometry needed at all - the
// divergence is a pure property of the two LayerTools resolver functions given the SAME
// configured id. Built through the real production path every other fixture in this file uses
// (Print::apply + PrintObject::slice(), so PrintRegion and its config come from the genuine
// pipeline, not a hand-built stub), then the two resolvers are called directly - exactly what
// GCode.cpp's configured_extruder_id lambda does for a gap-fill collection (FillFilamentSource::
// Wall -> wall_filament(region)) versus a real infill collection (-> sparse_infill_filament
// (region)).
//
// manual_pattern "12,21" flattens (the plain/wall_filament path, MixedFilamentManager::resolve)
// to "1221", whose layer-0 token is "1" -> component_a. The grouped path
// (MixedFilamentManager::resolve_perimeter) instead reads the group at index wall_loops - 1 (the
// default wall_loops = 2, so index 1 = "21"), whose layer-0 token is "2" -> component_b.
// component_a and component_b are physical filaments 1 and 2, so the two functions are proven -
// by the actual production resolution code in MixedFilament.cpp, not by construction of the
// fixture - to pick DIFFERENT physical extruders at layer_index = 0.
TEST_CASE("LayerTools::wall_filament applies the same grouped-manual-pattern resolution as sparse_infill_filament (C-1)", "[paintdepth]")
{
    struct AutoGenerateGuard
    {
        explicit AutoGenerateGuard(bool enabled) : previous(MixedFilamentManager::auto_generate_enabled())
        {
            MixedFilamentManager::set_auto_generate_enabled(enabled);
        }
        ~AutoGenerateGuard() { MixedFilamentManager::set_auto_generate_enabled(previous); }
        bool previous;
    };
    // Disabled so Print::apply's auto_generate(colors) does not ALSO create an auto pair for the
    // same (1,2) physical pair my custom row below uses - with only 2 physical filaments there is
    // no pair to pick that avoids that collision, and the collision would push my row to virtual
    // id 4 instead of the 3 this test's wall_filament/sparse_infill_filament config below assumes.
    AutoGenerateGuard auto_generate_guard(false);

    MixedFilamentManager seed_mgr;
    seed_mgr.add_custom_filament(/*component_a=*/1, /*component_b=*/2, /*mix_b_percent=*/50,
                                  /*filament_colours=*/{"#FFFFFF", "#804020"});
    REQUIRE(seed_mgr.mixed_filaments().size() == 1);
    seed_mgr.mixed_filaments().front().manual_pattern = MixedFilamentManager::normalize_manual_pattern("12,21");
    REQUIRE(seed_mgr.mixed_filaments().front().manual_pattern == "12,21");
    const std::string serialized_mixed = seed_mgr.serialize_custom_entries();

    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "unpainted-grouped-pattern.stl";
    object->add_volume(make_cube(20., 20., 4.));
    object->add_instance();
    object->ensure_on_bed();
    // No mmu_segmentation_facets set anywhere - genuinely unpainted.

    DynamicPrintConfig config = paint_depth_test_config(pdmUnlimited, 3);
    config.option<ConfigOptionString>("mixed_filament_definitions")->value = serialized_mixed;
    // The precondition the leak needs: wall_filament and sparse_infill_filament configured to
    // the SAME grouped mixed filament id (3 = num_physical(2) + the one mixed row above, with
    // auto-generation disabled above so that arithmetic actually holds).
    config.option<ConfigOptionInt>("wall_filament")->value          = 3;
    config.option<ConfigOptionInt>("sparse_infill_filament")->value = 3;
    config.option<ConfigOptionInt>("solid_infill_filament")->value  = 3;
    // wall_loops stays at its default (2), which is what grouped_manual_pattern_infill_filament_
    // 1based reads as the innermost perimeter index (wall_loops - 1 = 1 -> pattern group "21").

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    REQUIRE(out_object->num_printing_regions() >= 1);

    const PrintRegion &region = out_object->printing_region(0);
    REQUIRE(region.config().wall_filament.value == 3);
    REQUIRE(region.config().sparse_infill_filament.value == 3);
    REQUIRE(print.mixed_filament_manager().is_mixed(3, 2));

    LayerTools layer_tools(/*z=*/0.2);
    layer_tools.mixed_mgr    = &print.mixed_filament_manager();
    layer_tools.num_physical = 2;
    layer_tools.layer_index  = 0;
    layer_tools.layer_height = 0.2;

    // RED pre-fix: wall_filament resolves via the plain (flattened-pattern) path and returns
    // component_a - 1 = 0 (physical filament 1); sparse_infill_filament resolves via the grouped
    // path and returns component_b - 1 = 1 (physical filament 2). Same configured id, different
    // physical extruder.
    CHECK(layer_tools.wall_filament(region) == layer_tools.sparse_infill_filament(region));
    // ...and pinned to the specific physical extruder the grouped (correct) resolution gives, so
    // a future change that makes them agree on the WRONG extruder does not pass silently.
    CHECK(layer_tools.wall_filament(region) == 1u); // physical filament 2 (component_b), zero-based
}

// Same construction as process_painted_cube() above, but with the box's X and Y footprint as
// separate parameters - needed below to build an elongated, thin bar rather than a square.
PrintObject *process_painted_cuboid(double x, double y, double height, const std::vector<int> &painted_facets,
                                     PaintDepthMode mode, int walls, bool paint_infill_override,
                                     PerimeterGeneratorType wall_generator, Print &print)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-generator-cuboid.stl";
    ModelVolume *volume  = object->add_volume(make_cube(x, y, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : painted_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(mode, walls, paint_infill_override);
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    print.process();
    PrintObject *out_object = print.objects_mutable().front();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// I-3 (.superpowers/sdd/2026-08-31-paint-depth/wave-a-review.md): ToolOrdering::collect_extruders
// still buckets ANY non-solid-infill fill role (including gap fill) as needing
// sparse_infill_filament, even though emission (GCode.cpp's fill_filament_source, PrintRegion.cpp)
// now routes gap fill through wall_filament instead. Where wall_filament != sparse_infill_filament
// and a region's only fill content on a layer is gap fill (routine on thin-walled features where
// the perimeters consume the whole cross-section), the layer's computed extruder SET disagrees
// with what actually gets emitted: a spurious tool change and wipe-tower purge for an extruder
// that prints nothing on that layer.
//
// Fixture: an elongated (20 x 2.4 x 4mm), ALL-SIDE-painted bar. Walls-mode band(3) = 1.435675mm
// is more than half of the 2.4mm short dimension (margin 0.236mm), so the paint claim covers the
// ENTIRE cross-section at every mid-length layer - no base-coloured region exists there at all,
// so nothing legitimate can push the base extruder into that layer's set. Two classic wall loops
// (2*wall_stack = 1.757080mm at 0.45mm lines/0.2mm layers, this fixture's default layer height)
// leave a genuinely thin (~0.64mm) residual down the middle, which classic fills with a gap-fill
// line rather than sparse infill - exactly as the "on the classic generator the painted band's
// gap fill follows..." sibling test forces on an annulus, just reshaped into a straight bar so
// the WHOLE cross-section (not just a band around a separate interior region) is the one painted
// region. paint_infill_override = false keeps sparse_infill_filament on the BASE colour while
// wall/solid stay painted - the precondition I-3's bug needs: if collect_extruders still buckets
// this layer's gap fill as sparse infill, it pushes the BASE extruder even though nothing on the
// layer emits with it.
TEST_CASE("ToolOrdering::collect_extruders does not add the base filament to a layer whose only fill content is gap fill (I-3)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = process_painted_cuboid(/*x=*/20., /*y=*/2.4, /*height=*/4., ALL_SIDE_FACE,
                                                  pdmWalls, /*walls=*/3, /*paint_infill_override=*/false,
                                                  PerimeterGeneratorType::Classic, print);
    REQUIRE(object->layer_count() >= 12);

    const size_t        mid_layer = 10;
    const LayerRegion   *painted  = extruder2_layer_region(*object, mid_layer);
    REQUIRE(painted != nullptr);
    const size_t gap_fills = count_role_recursive(&painted->thin_fills, erGapFill);
    CAPTURE(gap_fills);
    // The fixture precondition: this layer's painted region really does carry gap fill.
    REQUIRE(gap_fills >= 1);

    const Layer *layer = object->get_layer(int(mid_layer));
    // No OTHER region has content on this layer to legitimately need the base extruder either -
    // the whole cross-section belongs to the one painted region.
    for (size_t region_idx = 0; region_idx < object->num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object->printing_region(region_idx);
        if (region.config().wall_filament.value == 2)
            continue; // the painted region itself, already accounted for above
        const int local_id = region.print_object_region_id();
        if (local_id < 0 || local_id >= layer->region_count())
            continue;
        const LayerRegion *layerm = layer->get_region(local_id);
        if (layerm == nullptr)
            continue;
        CAPTURE(region_idx);
        CHECK(layerm->perimeters.entities.empty());
        CHECK(layerm->fills.entities.empty());
    }

    const PrintRegionConfig &cfg = extruder2_region_config(*object);
    REQUIRE(cfg.wall_filament.value == 2);
    REQUIRE(cfg.sparse_infill_filament.value == 1);

    // ToolOrdering::reorder_extruders re-indexes LayerTools::extruders to ZERO-based as its very
    // last step (ToolOrdering.cpp, "Reindex the extruders, so they are zero based, not 1 based"),
    // after resolving any "dontcare" placeholders in 1-based space - matching the field's own
    // header comment ("Zero based extruder IDs"). So physical filament 1 (base) reads back as 0
    // and physical filament 2 (painted) as 1 here.
    const LayerTools &layer_tools = print.tool_ordering().tools_for_layer(layer->print_z);
    CAPTURE(layer_tools.extruders.size());
    const bool has_base_extruder =
        std::find(layer_tools.extruders.begin(), layer_tools.extruders.end(), 0u) != layer_tools.extruders.end();
    const bool has_painted_extruder =
        std::find(layer_tools.extruders.begin(), layer_tools.extruders.end(), 1u) != layer_tools.extruders.end();
    // Positive control: the painted extruder itself is genuinely needed and present (from the
    // perimeters loop registering wall_filament - unaffected by this fix), so a test that failed
    // to build real gap-fill-only geometry would show up here, not as a false pass below.
    REQUIRE(has_painted_extruder);
    // RED pre-fix: collect_extruders buckets gap fill as sparse infill (any role other than
    // erNone/solid-infill fell into has_sparse_infill), pushing the base extruder even though
    // nothing on this layer emits with it.
    CHECK_FALSE(has_base_extruder);
}
