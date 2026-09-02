#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/ClipperUtils.hpp"
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
//   4,5   = +X side (x=x)      6,7   = -Y side (y=0)
//   8,9   = -X side (x=0)      10,11 = +Y side (y=y)
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

// paint_infill_override and paint_depth_solid_interfaces both default to true (today's
// behavior, matches each option's own PrintConfig.cpp default) so existing callers that
// only pass mode/walls (or mode/walls/paint_infill_override) are unaffected.
DynamicPrintConfig paint_depth_test_config(PaintDepthMode mode, int walls, bool paint_infill_override = true,
                                            bool paint_depth_solid_interfaces = true)
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
    config.option<ConfigOptionBool>("paint_depth_solid_interfaces")->value   = paint_depth_solid_interfaces;
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
// Fix-wave 2 (absorb-tail-fixwave2-review.md I-D): trailing gap_infill_speed, same < 0
// "leave at paint_depth_test_config's own registry default" sentinel every other gap_infill_
// speed parameter in this file uses (slice_bounded_sphere_two_colours, slice_bounded_frustum_
// two_colours), so every existing caller (which omits it) is unaffected.
PrintObject *slice_painted_box(double x, double y, double z, const std::vector<int> &painted_facets,
                                PaintDepthMode mode, int walls, double paint_depth_mm,
                                double layer_height, Print &print,
                                double interlocking_depth = -1.,
                                PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne,
                                double gap_infill_speed = -1.0)
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
    if (gap_infill_speed >= 0.0)
        config.option<ConfigOptionFloat>("gap_infill_speed")->value = gap_infill_speed;

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
// paint_depth_solid_interfaces defaults to true (today's behavior) so existing callers
// that only pass mode/walls/print are unaffected.
// absorb-tail-review.md I2: only_one_wall_top / wall_generator, both defaulted to today's
// behavior (false / Arachne) so every existing caller (which omits both) is unaffected.
// only_one_wall_top engages PerimeterGenerator.cpp sites 3/4 (the has_bounded_paint_depth
// consumers I2 found untested); wall_generator selects WHICH of the two actually runs -
// PerimeterGenerator::process_classic() (site 3, :622, inside split_top_surfaces(), called only
// from process_classic()) or PerimeterGenerator::process_arachne() (site 4, :2273) - so a caller
// needs to pick a generator to exercise a SPECIFIC one of the two sites in isolation.
PrintObject *process_z_interface_cube(PaintDepthMode mode, int walls, Print &print,
                                       bool paint_depth_solid_interfaces = true,
                                       bool only_one_wall_top = false,
                                       PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne)
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

    DynamicPrintConfig config = paint_depth_test_config(mode, walls, /*paint_infill_override=*/true, paint_depth_solid_interfaces);
    config.option<ConfigOptionBool>("only_one_wall_top")->value = only_one_wall_top;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    print.process();
    PrintObject *out_object = print.objects_mutable().front();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// absorb-tail-review.md I2, site 2 isolation: process_z_interface_cube's single flat colour
// boundary cannot separate site 2 (discover_vertical_shells's top_bottom_surfaces_all_regions,
// merged-vs-per-region) from site 1 (detect_surfaces_type's OWN interface_shells, same gate) -
// measured directly: ungating site 2 alone (site 1 left correctly gated) changes NO test
// outcome on that fixture, because site 1 already classifies the base region's boundary layer
// as exposed (stTop) by itself - detect_surfaces_type only ever compares ONE layer up/down, so
// its own per-region-vs-merged reading is identical either way on a fixture with no OTHER
// region resuming further up. Site 2's OWN, multi-layer "is there enough of MY OWN region's
// material within the shell thickness above me" lookahead needs a fixture where that differs
// from site 1's single-layer view: a base cube with a THIN (2-layer) fully Extruder2-painted
// slab sandwiched inside it, base resuming above. Site 1 classifies the lower base's topmost
// layer as stTop either way (nothing of the SAME region immediately above, regardless of merge
// mode - unaffected by this fixture's change). Site 2 (top_shell_layers=4) then asks whether 4
// layers of the base's OWN colour exist within the shell above: PER-REGION sees only 2 (the
// upper base slab starts 2 layers into the window, the sandwich's own 2 layers do not count),
// so it is short and adds solid infill; MERGED sees all 4 (any region's presence counts,
// including the sandwiched paint), so it is already satisfied and adds none.
// I2 (sites 3/4): only_one_wall_top / wall_generator, both defaulted to today's behavior
// (false / Arachne) so the site-2-isolation caller above (which omits both) is unaffected.
// Fix-wave 2 (absorb-tail-fixwave2-review.md I-B): trailing min_width_top_surface_mm, same
// negative-sentinel convention as slice_bounded_frustum_two_colours' gap_infill_speed - "leave
// at paint_depth_test_config's own registry default" (300%) so every existing caller (which
// omits it) is unaffected. Site 3 (PerimeterGenerator.cpp:622, Classic split_top_surfaces) is
// inert at the registry default on this fixture only because upper_polygons_series_clipped is
// grown by min_width_top_surface (300% of 0.45mm = 1.35mm - PrintConfig.cpp:1231-1244) before
// the diff at :650, which swallows the exposed ring whole; lowering it to 0 here removes that
// swallowing so a source mutation of :622 can actually be seen (see the new site-3 test below).
PrintObject *process_z_sandwich_cube(bool paint_depth_solid_interfaces, Print &print,
                                      bool only_one_wall_top = false,
                                      PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne,
                                      double min_width_top_surface_mm = -1.0)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-z-sandwich.stl";
    ModelVolume *lower  = object->add_volume(make_cube(40., 40., 5.0));
    ModelVolume *middle = object->add_volume(make_cube(40., 40., 0.2));
    middle->translate(0., 0., 5.0);
    ModelVolume *upper  = object->add_volume(make_cube(40., 40., 5.0));
    upper->translate(0., 0., 5.2);
    (void) lower;
    (void) upper;

    TriangleSelector selector(middle->mesh());
    for (int facet_idx : ALL_SIDE_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(middle->mmu_segmentation_facets.set(selector));

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(pdmWalls, /*walls=*/3, /*paint_infill_override=*/true, paint_depth_solid_interfaces);
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.;
    config.option<ConfigOptionBool>("only_one_wall_top")->value = only_one_wall_top;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;
    if (min_width_top_surface_mm >= 0.) {
        config.option<ConfigOptionFloatOrPercent>("min_width_top_surface")->value   = min_width_top_surface_mm;
        config.option<ConfigOptionFloatOrPercent>("min_width_top_surface")->percent = false;
    }

    print.set_status_silent();
    print.apply(model, config);
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

// absorb-tail-review.md I2 site 2 test: the counterpart of extruder2_local_region_id() above
// for the BASE region (config().wall_filament != 2) - process_z_interface_cube's fixture only
// ever has these two regions, so "not Extruder2" IS "base" there. Returns -1 if none exists.
int base_local_region_id(const PrintObject &object)
{
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value != 2)
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

// Follow-up (item 1, .superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-report.md
// "still open" / shell-setting-and-gapfill-report.md, user decision 2026-09-01):
// paint_depth_solid_interfaces gates the has_bounded_paint_depth() forcing at all four
// read sites (PrintObject.cpp x2, PerimeterGenerator.cpp x2). With it OFF and
// interface_shells at its own plain default (false), a BOUNDED (walls-mode, i.e.
// has_bounded_paint_depth() would otherwise be true) color Z-interface must fall back to
// exactly the same (no solid skin) result as the interface_shells=false/unbounded case
// above - the option, not the mode, is what is being pinned here.
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces=false falls back to plain interface_shells (no solid skin) at a bounded color Z-interface", "[paintdepth]")
{
    Print        print;
    PrintObject *object = process_z_interface_cube(pdmWalls, 3, print, /*paint_depth_solid_interfaces=*/false);

    const size_t first_painted_layer = first_layer_above_z(*object, 10.0);
    CHECK_FALSE(extruder2_layer_has_solid_skin(*object, first_painted_layer));
}

// absorb-tail-review.md I2: total stInternalSolid area in a region's fill_surfaces, on a given
// layer. Site 2 (PrintObject.cpp discover_vertical_shells, top_bottom_surfaces_all_regions)
// changes fill_surfaces (internal-solid ADDITIONS), never slices.surfaces types - invisible to
// extruder2_layer_has_solid_skin() above (site 1 only), which is why site 2 needed its own
// probe.
double region_internal_solid_area(const PrintObject &object, int local_region_id, size_t layer_idx)
{
    if (local_region_id < 0)
        return 0.;
    const Layer *layer = object.get_layer(int(layer_idx));
    if (local_region_id >= layer->region_count())
        return 0.;
    const LayerRegion *layerm = layer->get_region(local_region_id);
    if (layerm == nullptr)
        return 0.;
    double area = 0.;
    for (const Surface &s : layerm->fill_surfaces.surfaces)
        if (s.surface_type == stInternalSolid)
            area += s.area();
    return area;
}

// absorb-tail-review.md I2, sites 3/4: total stTop area in a region's fill_surfaces on a given
// layer - only_one_wall_top's "single wall, replaced by a top fill" treatment produces a stTop
// fill_surfaces entry (split_top_surfaces() / the Arachne top_expolygons path) when it engages,
// which a plain full-wall-count layer does not.
double region_top_fill_area(const PrintObject &object, int local_region_id, size_t layer_idx)
{
    if (local_region_id < 0)
        return 0.;
    const Layer *layer = object.get_layer(int(layer_idx));
    if (local_region_id >= layer->region_count())
        return 0.;
    const LayerRegion *layerm = layer->get_region(local_region_id);
    if (layerm == nullptr)
        return 0.;
    double area = 0.;
    for (const Surface &s : layerm->fill_surfaces.surfaces)
        if (s.surface_type == stTop)
            area += s.area();
    return area;
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-B): recursive perimeter-length walk.
// ExtrusionEntityCollection::length() THROWS by design (ExtrusionEntityCollection.hpp) - it is
// a container, not a single path - so a real total needs a recursive descent over its
// `entities`, summing every LEAF (ExtrusionPath/ExtrusionLoop/ExtrusionMultiPath)'s own
// length(). Same is_collection()+static_cast recursion idiom this file already uses for
// count_loops_recursive/count_role_recursive further down.
double sum_length_recursive(const ExtrusionEntity *entity)
{
    if (entity->is_collection()) {
        double total = 0.;
        for (const ExtrusionEntity *child : static_cast<const ExtrusionEntityCollection *>(entity)->entities)
            total += sum_length_recursive(child);
        return total;
    }
    return entity->length();
}

// absorb-tail-fixwave2-review.md I-B: the metric that can ACTUALLY isolate sites 3/4
// (PerimeterGenerator.cpp) from sites 1/2 (PrintObject.cpp). layerm->perimeters is written
// ONLY by PerimeterGenerator (Layer.cpp:260, make_perimeters' sole call site) and never
// touched again afterwards - unlike fill_surfaces, which detect_surfaces_type/site 1 REBUILDS
// after PerimeterGenerator has already run (see region_top_fill_area's own site-3/4 tests
// below and their honesty note). A total perimeter-length metric therefore cannot be moved by
// anything but the perimeter generator itself.
double region_perimeter_length(const PrintObject &object, int local_region_id, size_t layer_idx)
{
    if (local_region_id < 0)
        return 0.;
    const Layer *layer = object.get_layer(int(layer_idx));
    if (local_region_id >= layer->region_count())
        return 0.;
    const LayerRegion *layerm = layer->get_region(local_region_id);
    if (layerm == nullptr)
        return 0.;
    double total = 0.;
    for (const ExtrusionEntity *ee : layerm->perimeters.entities)
        total += sum_length_recursive(ee);
    return total;
}

// absorb-tail-review.md I2, site 2 (PrintObject.cpp :1772-1773, discover_vertical_shells's
// top_bottom_surfaces_all_regions - changes fill_surfaces, invisible to the site-1-only RED
// above). Same Z-interface fixture, ensure_vertical_shell_thickness left at its own default
// (evstAll - PrintConfig.cpp), and the probe is the BASE region's LAST layer before the colour
// transition (the one whose upward neighbour, physically continuous solid, is now a DIFFERENT
// region by paint alone) - the layer this site's per-region-vs-merged distinction actually
// governs. Gate ON (paint_depth_solid_interfaces=true, default): top_bottom_surfaces_all_regions
// is forced false, so the base region's own vertical shell thickness is computed WITHOUT
// crediting Extruder2's colour-only continuity above it, so this layer earns its own
// stInternalSolid fill. Gate OFF: regions merge for this purpose, the base region reads
// (wrongly) "already covered" by the painted region continuing above it, and gets none.
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates the base region's OWN fill_surfaces solid-infill addition at a colour Z-interface (Item 2, site 2)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_interface_cube(pdmWalls, 3, print_on, /*paint_depth_solid_interfaces=*/true);
    Print        print_off;
    PrintObject *object_off = process_z_interface_cube(pdmWalls, 3, print_off, /*paint_depth_solid_interfaces=*/false);

    // Summed over every base-region layer at/below the colour transition, rather than one
    // hand-picked layer: discover_vertical_shells's own lookahead can place the additional
    // solid fill several layers below the transition itself, not necessarily on the very last
    // base layer - summing is robust to exactly which of those layers it lands on.
    const size_t first_painted_on  = first_layer_above_z(*object_on, 10.0);
    const size_t first_painted_off = first_layer_above_z(*object_off, 10.0);
    const int    base_region_on    = base_local_region_id(*object_on);
    const int    base_region_off   = base_local_region_id(*object_off);
    double area_on = 0., area_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_painted_on; ++layer_idx)
        area_on += region_internal_solid_area(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_painted_off; ++layer_idx)
        area_off += region_internal_solid_area(*object_off, base_region_off, layer_idx);
    CAPTURE(area_on);
    CAPTURE(area_off);

    CHECK(area_on > area_off);
}

// absorb-tail-review.md I2, site 2 ISOLATED from site 1 - see process_z_sandwich_cube's own
// comment for why the Z-interface fixture above cannot do this alone. Same probe shape as that
// test (BASE region's own last layer before losing its own colour, summed stInternalSolid
// area), different fixture. This is the one that ACTUALLY discriminates site 2 on its own:
// ungating site 2 alone (site 1 left correctly gated) fails this test; ungating site 1 alone
// (site 2 left correctly gated) does NOT (verified by hand, both directions, all four scratch
// mutations - see the fix-wave report).
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates the base region's own vertical-shell lookahead across a thin painted sandwich, isolated from site 1 (Item 2, site 2 isolation)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/true, print_on);
    Print        print_off;
    PrintObject *object_off = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/false, print_off);

    // Summed over every lower-base layer (same robustness reasoning as the Z-interface site 2
    // test above - not assuming the effect lands on exactly one hand-picked layer).
    const size_t first_sandwich_on  = first_layer_above_z(*object_on, 5.0);
    const size_t first_sandwich_off = first_layer_above_z(*object_off, 5.0);
    const int    base_region_on     = base_local_region_id(*object_on);
    const int    base_region_off    = base_local_region_id(*object_off);
    double area_on = 0., area_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_sandwich_on; ++layer_idx)
        area_on += region_internal_solid_area(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_sandwich_off; ++layer_idx)
        area_off += region_internal_solid_area(*object_off, base_region_off, layer_idx);
    CAPTURE(area_on);
    CAPTURE(area_off);

    CHECK(area_on > area_off);
}

// absorb-tail-review.md I2, sites 3/4 (PerimeterGenerator.cpp): only_one_wall_top's "single
// wall, replaced by a top fill" branch is gated on config->only_one_wall_top (:1450, :2226/
// :2249/:2246) AND, once engaged, on has_bounded_paint_depth && paint_depth_solid_interfaces
// (:622, :2273) deciding whether to read upper_slices_same_region (per-region) or *upper_slices
// (physical, any region). This is the coverage gap I2 identifies literally: with only_one_wall_
// top left at its default (false), these two lines never execute AT ALL under any existing
// fixture, gated or not - the sites 1/2 RED/GREEN tests above never reach them. Forcing
// only_one_wall_top=true on the same sandwich fixture that isolates site 2 makes them execute,
// and pins the resulting fill_surfaces difference (below).
//
// Honesty note (measured, not assumed - matches the process's own "verify and report the TRUE
// number" instruction): a source-level mutation of :622/:2273 alone (object_config->
// interface_shells || (has_bounded_paint_depth && ...) narrowed to just interface_shells, and
// separately hard-forced to `if (false)`) left this test's own numbers byte-identical on BOTH
// this sandwich fixture and the flat Z-interface one - i.e. sites 3/4's own upper_slices_same_
// region-vs-*upper_slices choice, in isolation, does not move the TOTAL stTop fill_surfaces
// area this test sums. Traced why: fill_surfaces already carries a stTop entry for this layer
// from discover_horizontal_shells/prepare_fill_surfaces (upstream of perimeter generation
// entirely, seeded from site 1's own slices.surfaces classification) BEFORE PerimeterGenerator
// ever runs; split_top_surfaces'/process_arachne's own top_fills contribution merges into that
// pre-existing area rather than replacing it, so a total-area metric cannot distinguish "sites
// 3/4 added nothing" from "sites 3/4 added something already covered by site 1's own
// classification." This test is therefore PROVEN coverage (only_one_wall_top's mechanism now
// executes and is asserted on, closing the literal gap I2 names) and a real, passing ON/OFF
// pin at the CONFIG level (matching the review's own suggested recipe) - but NOT proof that
// :622/:2273's own upper_slices_same_region-vs-*upper_slices branch specifically, isolated from
// site 1's upstream effect, is what drives it. See the fix-wave report for the full account.
// Site 3 runs under the CLASSIC generator (PerimeterGenerator::process_classic() -> split_top_
// surfaces(), :622); site 4 under ARACHNE (PerimeterGenerator::process_arachne(), :2273) - two
// separate TEST_CASEs, one per generator, so each at least exercises its own code path
// independently of the other (Classic never reaches process_arachne() or vice versa).
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates only_one_wall_top's per-region check across a thin painted sandwich, isolated from site 1, Classic generator (Item 2, site 3)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/true, print_on,
                                                       /*only_one_wall_top=*/true, PerimeterGeneratorType::Classic);
    Print        print_off;
    PrintObject *object_off = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/false, print_off,
                                                        /*only_one_wall_top=*/true, PerimeterGeneratorType::Classic);

    // Summed over every lower-base layer (same robustness reasoning as the site-2-isolation
    // test above - not assuming the effect lands on exactly one hand-picked layer).
    const size_t first_sandwich_on  = first_layer_above_z(*object_on, 5.0);
    const size_t first_sandwich_off = first_layer_above_z(*object_off, 5.0);
    const int    base_region_on     = base_local_region_id(*object_on);
    const int    base_region_off    = base_local_region_id(*object_off);
    double top_area_on = 0., top_area_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_sandwich_on; ++layer_idx)
        top_area_on += region_top_fill_area(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_sandwich_off; ++layer_idx)
        top_area_off += region_top_fill_area(*object_off, base_region_off, layer_idx);
    CAPTURE(top_area_on);
    CAPTURE(top_area_off);

    CHECK(top_area_on > top_area_off);
}

TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates only_one_wall_top's per-region check across a thin painted sandwich, isolated from site 1, Arachne generator (Item 2, site 4)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/true, print_on,
                                                       /*only_one_wall_top=*/true, PerimeterGeneratorType::Arachne);
    Print        print_off;
    PrintObject *object_off = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/false, print_off,
                                                        /*only_one_wall_top=*/true, PerimeterGeneratorType::Arachne);

    const size_t first_sandwich_on  = first_layer_above_z(*object_on, 5.0);
    const size_t first_sandwich_off = first_layer_above_z(*object_off, 5.0);
    const int    base_region_on     = base_local_region_id(*object_on);
    const int    base_region_off    = base_local_region_id(*object_off);
    double top_area_on = 0., top_area_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_sandwich_on; ++layer_idx)
        top_area_on += region_top_fill_area(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_sandwich_off; ++layer_idx)
        top_area_off += region_top_fill_area(*object_off, base_region_off, layer_idx);
    CAPTURE(top_area_on);
    CAPTURE(top_area_off);

    CHECK(top_area_on > top_area_off);
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-B): the site-4 test above (and its site-3
// sibling) prove only_one_wall_top's mechanism now EXECUTES - not that PerimeterGenerator.cpp
// :2273's own upper_slices_same_region-vs-*upper_slices branch specifically, isolated from
// site 1's upstream fill_surfaces seeding, is what DRIVES the result: region_top_fill_area
// sums fill_surfaces' stTop area, and fill_surfaces already carries a stTop entry for this
// layer from discover_horizontal_shells/site 1 BEFORE PerimeterGenerator ever runs (measured
// by the previous wave via direct source mutation - see that test's own honesty note above).
// region_perimeter_length is the metric that CAN isolate site 4: layerm->perimeters is written
// ONLY by the perimeter generator and never touched again (region_perimeter_length's own header
// comment). Same sandwich/only_one_wall_top=true fixture, ON vs OFF via CONFIG (matching the
// review's own recipe) - `len_on < len_off` is mutation-sensitive: dropping
// `&& paint_depth_solid_interfaces` at :2273 collapses OFF onto ON's own (shorter) output,
// verified by direct source mutation as part of this fix-wave's process (see the fix-wave
// report). Measured direction (absorb-tail-fixwave-review.md's own probe): ON's per-region
// upper_slices_same_region sees LESS coverage from above at this colour boundary than OFF's
// physical *upper_slices (the differently-coloured region above does not count as "my own
// region continuing"), so ON treats MORE of the boundary as "top" - fewer inner wall loops,
// shorter total perimeter length.
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates only_one_wall_top's own perimeter-generator output, Arachne generator, mutation-sensitive metric (Fix-wave 2 I-B, site 4)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/true, print_on,
                                                       /*only_one_wall_top=*/true, PerimeterGeneratorType::Arachne);
    Print        print_off;
    PrintObject *object_off = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/false, print_off,
                                                        /*only_one_wall_top=*/true, PerimeterGeneratorType::Arachne);

    const size_t first_sandwich_on  = first_layer_above_z(*object_on, 5.0);
    const size_t first_sandwich_off = first_layer_above_z(*object_off, 5.0);
    const int    base_region_on     = base_local_region_id(*object_on);
    const int    base_region_off    = base_local_region_id(*object_off);
    double len_on = 0., len_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_sandwich_on; ++layer_idx)
        len_on += region_perimeter_length(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_sandwich_off; ++layer_idx)
        len_off += region_perimeter_length(*object_off, base_region_off, layer_idx);
    CAPTURE(len_on);
    CAPTURE(len_off);

    CHECK(len_on < len_off);
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-B): site 3's Classic counterpart of the site-4
// test above. Site 3 (PerimeterGenerator.cpp:622, split_top_surfaces) is inert on the plain
// sandwich fixture (min_width_top_surface at its registry default, 300% = 1.35mm) because
// upper_polygons_series_clipped is grown by that width before the `diff_ex(delete_bridge,
// upper_polygons_series_clipped)` at :650 - wide enough to swallow the exposed ring whole
// regardless of which upper-slices source fed it, so ON and OFF read the same `top_polygons`
// either way (measured by the previous wave, byte-identical result). Passing
// min_width_top_surface_mm=0 through the fixture's new trailing parameter removes that
// swallowing, so the per-region-vs-physical choice at :622 can actually be seen in the
// perimeter-generator's own output (region_perimeter_length, same reasoning as site 4's test).
TEST_CASE("multi_material_segmentation_by_painting: paint_depth_solid_interfaces gates only_one_wall_top's own perimeter-generator output, Classic generator, min_width_top_surface lowered so it does not swallow the exposed ring (Fix-wave 2 I-B, site 3)", "[paintdepth]")
{
    Print        print_on;
    PrintObject *object_on = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/true, print_on,
                                                       /*only_one_wall_top=*/true, PerimeterGeneratorType::Classic,
                                                       /*min_width_top_surface_mm=*/0.0);
    Print        print_off;
    PrintObject *object_off = process_z_sandwich_cube(/*paint_depth_solid_interfaces=*/false, print_off,
                                                        /*only_one_wall_top=*/true, PerimeterGeneratorType::Classic,
                                                        /*min_width_top_surface_mm=*/0.0);

    const size_t first_sandwich_on  = first_layer_above_z(*object_on, 5.0);
    const size_t first_sandwich_off = first_layer_above_z(*object_off, 5.0);
    const int    base_region_on     = base_local_region_id(*object_on);
    const int    base_region_off    = base_local_region_id(*object_off);
    double len_on = 0., len_off = 0.;
    for (size_t layer_idx = 0; layer_idx < first_sandwich_on; ++layer_idx)
        len_on += region_perimeter_length(*object_on, base_region_on, layer_idx);
    for (size_t layer_idx = 0; layer_idx < first_sandwich_off; ++layer_idx)
        len_off += region_perimeter_length(*object_off, base_region_off, layer_idx);
    CAPTURE(len_on);
    CAPTURE(len_off);

    // Fix-wave 3 (absorb-tail-fixwave2-review.md m1): `!=` passed for any perturbation and was
    // weaker than the data - measured ON 15629589160 < OFF 15640000400 (ON shorter by 10.41mm),
    // the SAME direction and a comparable one-layer magnitude to site 4's own 10.23mm (ON's
    // same-region upper slices see less coverage under paint_depth_solid_interfaces -> more area
    // classified "top" -> fewer inner perimeter loops). Tightened to match.
    CHECK(len_on < len_off);
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

// I1 (.superpowers/sdd/2026-08-31-paint-depth/flat-top-cap-review.md): a flat painted top/bottom
// that is NOT the object's own topmost/bottommost face - a ledge beside a taller riser, the
// common case on any stepped or bossed real part - is what exposed_surface_part()'s POINTWISE
// wall-stack yardstick misclassified pre-fix: the ledge band within one wall stack of the riser
// read as "sloped" even though its own local slope is 0, so it never got capped. Two stacked
// ModelVolumes, same construction as process_z_interface_cube() above (no boolean mesh needed) -
// a 40x40xheight SLAB and a centred 20x20xheight TOWER sharing one face, so the slab's own top (or
// bottom) cap facet is painted in full but the ACTUAL exposed surface the segmenter sees is
// clipped by the tower's footprint automatically, to exactly the 10mm-wide ledge ring around it -
// the same occlusion the two-volume Z-stack already relies on elsewhere in this file.
//   - top_face == true:  the TOWER sits ABOVE the slab (z in [4,8]) - the riser rises past the
//     painted ledge, mirroring every other TOP_CAP_FACE fixture's descent direction (depth
//     increases going DOWN from the ledge's own layer).
//   - top_face == false: the STEM (same 20x20xheight box) sits BELOW a SHELF (z in [4,8]) whose
//     own BOTTOM_CAP_FACE is painted - the riser drops past the painted ledge, mirroring every
//     other BOTTOM_CAP_FACE fixture's descent direction (depth increases going UP).
PrintObject *slice_capped_ledge(bool top_face, Print &print)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-ledge.stl";

    ModelVolume *wide_volume;
    if (top_face) {
        wide_volume         = object->add_volume(make_cube(40., 40., 4.));
        ModelVolume *tower  = object->add_volume(make_cube(20., 20., 4.));
        tower->translate(10., 10., 4.);

        TriangleSelector selector(wide_volume->mesh());
        for (int facet_idx : TOP_CAP_FACE)
            selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
        REQUIRE(wide_volume->mmu_segmentation_facets.set(selector));
    } else {
        ModelVolume *stem = object->add_volume(make_cube(20., 20., 4.));
        stem->translate(10., 10., 0.);
        wide_volume        = object->add_volume(make_cube(40., 40., 4.));
        wide_volume->translate(0., 0., 4.);

        TriangleSelector selector(wide_volume->mesh());
        for (int facet_idx : BOTTOM_CAP_FACE)
            selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
        REQUIRE(wide_volume->mmu_segmentation_facets.set(selector));
    }

    object->add_instance();
    object->ensure_on_bed();

    // Same shell pinning as slice_bounded_frustum()/slice_two_painted_colours(): top_shell_layers
    // = 4 / top_shell_thickness = 0.6 at 0.1mm layers is a 6-layer effective top shell,
    // bottom_shell_layers = 3 / bottom_shell_thickness = 0.0 is a 3-layer effective bottom shell -
    // both far short of the D-driven 15-layer descent at walls = 3, so any claim surviving past
    // them is unambiguously the pre-fix D-driven bug, not the shell.
    DynamicPrintConfig config = paint_depth_test_config(pdmWalls, /*walls=*/3);
    config.option<ConfigOptionFloat>("layer_height")->value               = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value = 0.1;
    config.option<ConfigOptionInt>("top_shell_layers")->value             = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value        = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value          = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value     = 0.0;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// I1: a point offset_mm from the ledge fixture's own XY centre along +X, at the given (wide,
// 40x40-footprint) layer's mid-Y - i.e. how far out from the riser (tower half-width 10mm) or in
// from the slab's own edge (half-width 20mm) the probe sits. The ledge itself spans offset_mm in
// (10, 20). reference_layer_idx must be a layer whose OWN lslices is the full 40x40 slab/shelf
// footprint (never the 20x20 tower/stem's) - both slice_capped_ledge() call sites below compute
// this via first_layer_above_z() before calling this helper.
Point ledge_offset_probe(const PrintObject &object, size_t reference_layer_idx, double offset_mm)
{
    const BoundingBox bb = get_extents(object.get_layer(int(reference_layer_idx))->lslices);
    REQUIRE(bb.defined);
    const coord_t cy = (bb.min.y() + bb.max.y()) / 2;
    return Point(coord_t(((bb.min.x() + bb.max.x()) / 2) + scale_(offset_mm)), cy);
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
// Flat-top cap fix wave: two trailing parameters, both DEFAULTED so every pre-existing caller is
// byte-identical.
//   - gap_infill_speed < 0 means "leave at paint_depth_test_config's own registry default" - the
//     same sentinel convention slice_painted_box/slice_bounded_frustum_two_colours/
//     slice_bounded_sphere_two_colours already use.
//   - top_shell_layers_override < 0 means "leave top_shell_layers/top_shell_thickness at 4/0.6"
//     (today's behaviour); >= 0 instead sets top_shell_layers to that exact count AND
//     top_shell_thickness to 0 (purely count-driven), so out.top_shell_layers becomes EXACTLY
//     that count. Passing 15 (== M, the D-driven descent depth at walls=3/0.1mm layers) makes
//     top_cap_active's "descent_layers > shell_layers" test false everywhere - the cap is
//     provably INACTIVE - while top_descent_layers itself (max(top_shell_layers, M)) stays 15,
//     UNCHANGED from the default 4/0.6 config (max(6, 15) == max(15, 15) == 15 either way). Used
//     to build a "same descent depth, cap disabled" reference fixture without hand-deriving the
//     exact reach at every layer near the object's own apex (I2/Minor 1 regression pins below).
PrintObject *slice_bounded_frustum(double bottom, double top, double height,
                                    const std::vector<int> &painted_facets,
                                    PaintDepthMode mode, int walls, double layer_height, Print &print,
                                    PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne,
                                    double gap_infill_speed = -1.0,
                                    int top_shell_layers_override = -1)
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
    config.option<ConfigOptionInt>("top_shell_layers")->value                   = top_shell_layers_override >= 0 ? top_shell_layers_override : 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value              = top_shell_layers_override >= 0 ? 0.0 : 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value                = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value           = 0.0;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;
    if (gap_infill_speed >= 0.0)
        config.option<ConfigOptionFloat>("gap_infill_speed")->value = gap_infill_speed;

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

// ===========================================================================================
// WAVE B REVIEW FIX WAVE (.superpowers/sdd/2026-08-31-paint-depth/wave-b-review.md). Important
// 1, Important 2. Important 3 is a documentation-only correction (design/report cost claims) -
// no test. The Wave A fix wave above landed FIRST and shifted this review's own line numbers;
// every anchor below was re-read against the current file, not trusted from the review text.
// ===========================================================================================

// Important 2 fixture: same construction as slice_painted_box() above, but with a THIRD
// physical extruder and TWO painted facet groups, each its own colour - the painted-cap-over-
// painted-stripe interaction Wave B left unmentioned and untested. A new helper rather than
// extending slice_painted_box() / paint_depth_test_config(), which stay untouched: every other
// fixture in this file paints exactly one colour, so generalizing either of those shared helpers
// for this one two-colour case would be a bigger, riskier change than adding beside them.
// Flat-top cap fix wave (Minor 4): trailing gap_infill_speed, same < 0 "leave at
// paint_depth_test_config's own registry default" sentinel every other gap_infill_speed
// parameter in this file uses - every existing caller (which omits it) is unaffected.
PrintObject *slice_two_painted_colours(double x, double y, double z,
                                        const std::vector<int> &cap_facets, const std::vector<int> &side_facets,
                                        PaintDepthMode mode, int walls, double layer_height, Print &print,
                                        double gap_infill_speed = -1.0)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-two-colour.stl";
    ModelVolume *volume  = object->add_volume(make_cube(x, y, z));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : cap_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    for (int facet_idx : side_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder3);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    // Base config from paint_depth_test_config(), then widened from 2 to 3 physical filaments -
    // see that function's own comment on why every per-extruder width-driving option needs
    // pinning explicitly rather than trusting the resize.
    DynamicPrintConfig config = paint_depth_test_config(mode, walls);
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020", "#2040A0"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4};
    config.option<ConfigOptionFloat>("layer_height")->value                     = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = layer_height;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    // Matches slice_bounded_frustum()'s pinning: top_shell_layers = 4 / top_shell_thickness =
    // 0.6 at 0.1mm layers is a 6-layer effective legacy shell, so the review's own "6 -> 15
    // layers at stock defaults" numbers apply verbatim.
    config.option<ConfigOptionInt>("top_shell_layers")->value    = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value = 0.6;
    if (gap_infill_speed >= 0.0)
        config.option<ConfigOptionFloat>("gap_infill_speed")->value = gap_infill_speed;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// Generalization of extruder2_claim_for_layer() (above) to an arbitrary physical filament id -
// needed once a fixture paints more than one colour. Identical body, parameterized.
ExPolygons claim_for_layer(const PrintObject &object, size_t layer_idx, int extruder_id)
{
    ExPolygons   result;
    const Layer *layer = object.get_layer(int(layer_idx));
    for (size_t region_idx = 0; region_idx < object.num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object.printing_region(region_idx);
        if (region.config().wall_filament.value != extruder_id)
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

// Important 1: the review's own worked scenario, re-derived against the CURRENT code. Wave A's
// own I-1 fix (immediately above - paint_depth_classic_notch_cap_mm, landed AFTER this review
// was written) turns out to already close this finding, not merely narrow it. Re-derivation:
//
//   effective_even_layer_band = max_width - interlocking_depth'
//                              = max_width - min(interlocking_depth, max(0, max_width - max_wall_stack))
//                              = max(max_width - interlocking_depth, max_wall_stack)
//
// (algebra: let a = interlocking_depth, s = max(0, max_width - max_wall_stack). The per-region
// classic floor guarantees max_width >= max_wall_stack term-by-term - see
// paint_depth_band_classic_floor_mm - so max() of the larger sequence is >= max() of the smaller
// one, i.e. s = max_width - max_wall_stack exactly, never clamped to 0. interlocking_depth' =
// min(a, s), so max_width - interlocking_depth' = max_width - min(a, s) = max(max_width - a,
// max_width - s) = max(max_width - a, max_wall_stack).) The right-hand max() means the
// even-layer band can never fall below max_wall_stack, for ANY D on the classic generator - not
// only at the walls = 1 boundary the review's own numbers use. And the D >= wall_stack gate
// (out.normal_shell, MultiMaterialSegmentation.cpp) is itself only ever open on the classic
// generator BECAUSE that same per-region floor guarantees max_width (== D ==
// paint_depth_normal_mm) >= every region's own wall_stack. So wherever the gate is open on the
// classic generator, max_wall_stack >= wall_stack, and the "ring" (wall_stack -
// effective_even_layer_band) is <= 0: Wave A's I-1 fix closes Important 1 as a side effect of
// closing its own, differently-framed finding - the same notch narrowing the same band below the
// same wall_stack, observed from the classic-floor side rather than the descent-gate side. This
// case pins it as a REGRESSION, not a fresh RED: classic, walls = 1 (the review's own
// configuration, where the floor gives ZERO slack and the notch is capped to exactly 0), probing
// the review's own disputed inset window [0.7785, 0.8785]mm on BOTH parities. Reused fixture
// from the "D >= wall_stack gate" case above.
//
// Stock flow at 0.1mm layers / 0.45mm lines: ext_w = 0.45, ext_s = 0.428540, wall_stack =
// 0.878540.
TEST_CASE("multi_material_segmentation_by_painting: the classic floor's notch cap already closes the D >= wall_stack gate's sandwiched ring, on BOTH parities, at walls = 1 (Important 1)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40.392, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                 pdmWalls, /*walls=*/1, /*layer_height=*/0.1, print,
                                                 PerimeterGeneratorType::Classic);
    REQUIRE(object->layer_count() >= 27);

    // wall_stack - 0.05 = 0.828540mm: inside the review's disputed [0.7785, 0.8785]mm window,
    // matching the I-1 test's own `wall_stack +/- 0.05mm` probing convention. Without the notch
    // cap this would be claimed on the odd layer (band reaches 0.878540mm untouched) but NOT on
    // the even layer (band narrowed to 0.778540mm by the uncapped 0.1mm notch) - the review's own
    // "sandwiched ring" defect. Both parities pass here because the notch is capped to 0 at
    // walls = 1 (the floored band has zero slack above wall_stack), so the even-layer band
    // reaches wall_stack too, exactly like the odd one - a true regression pin, not a fresh RED.
    const double probe_inset = 0.828540;
    for (size_t probe_layer : {size_t(12), size_t(13)}) {
        CAPTURE(probe_layer);
        const ExPolygons claim = extruder2_claim_for_layer(*object, probe_layer);
        CHECK(any_contains(claim, layer_edge_probe(*object, probe_layer, probe_inset)));
    }
}

// Important 2: merge_segmented_layers trims EVERY extruder's lateral claim by EVERY extruder's
// top/bottom claim, keyed purely on extruder identity - not on whether the trimmer is the base
// colour or another painted one. segmentation_top_and_bottom_layers's own "PAINTED COLOURS ONLY"
// comment is careful to explain why deepening is safe against the BASE colour (color_idx 0 never
// deepens) and silent about painted-vs-painted. Before this fix, a painted cap's DEEPENED
// top/bottom claim (up to 15 layers at stock defaults) ate just as deep into a NEIGHBOURING
// PAINTED colour's lateral band as it does into the base colour's - 2.5x deeper than the
// pre-Wave-B legacy shell (6 layers), unmentioned and untested in Wave B.
//
// New helpers below (slice_two_painted_colours / claim_for_layer) rather than extending
// slice_painted_box() / paint_depth_test_config() / extruder2_claim_for_layer(), which stay
// untouched - this is the only fixture in the file with two independently painted colours.
//
// 40x40x6mm box: TOP_CAP_FACE painted Extruder2 (colour A, the cap), the full-height +X wall
// painted Extruder3 (colour B, the side stripe, painted right up to the shared top edge) -
// exactly the review's failure scenario ("colour B painted on a side wall right up to the top
// edge, colour A painted on the flat top"). Stock-shaped defaults at 0.1mm layers:
// top_shell_layers = 4 / top_shell_thickness = 0.6 -> 6-layer legacy shell; walls = 3 -> D =
// 1.435675mm -> M = ceil(1.435675/0.1) = 15 -> 15-layer deepened descent. Probe inset 1.1mm is
// inside colour B's own lateral band (>= 1.335675mm on this fixture at either parity) AND past
// colour A's F1 wall_stack clearance (0.878540mm) from the +X contour, so it is exactly the
// overlap zone the excess-vs-other-painted-laterals clip is about: depth 3 is within colour A's
// LEGACY shell (colour A legitimately owns it, unchanged by this fix - a non-regression sanity
// check); depth 10 is within colour A's DEEPENED reach only (colour B must keep it, exactly as
// it did before Wave B - the regression this case pins).
TEST_CASE("multi_material_segmentation_by_painting: a painted cap does not eat a neighbouring painted colour's lateral band deeper than its legacy shell (Important 2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_two_painted_colours(/*x=*/40., /*y=*/40., /*z=*/6.,
                                                      TOP_CAP_FACE, PLUS_X_FACE,
                                                      pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 20);

    // Depth 3: within colour A's legacy shell (<= 6 layers) - both the pre- and post-fix claim
    // agree here, so this is a sanity check that the fixture behaves as designed, not the
    // regression itself.
    {
        const size_t probe_layer = top_index - 3;
        const Point  probe       = layer_edge_probe(*object, probe_layer, 1.1);
        CHECK(any_contains(claim_for_layer(*object, probe_layer, /*Extruder2, colour A*/ 2), probe));
        CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder3, colour B*/ 3), probe));
    }

    // Depth 10: within colour A's DEEPENED reach (15 layers) but past its LEGACY shell (6
    // layers). RED pre-fix: colour A's cap claims this too (2.5x deeper than its legacy shell).
    // GREEN post-fix: colour B's stripe keeps it, exactly as it did before Wave B - and colour A
    // does NOT also claim it, which the CHECK_FALSE below pins directly (bounding the trim alone,
    // without also clipping colour A's own excess against colour B's lateral claim, would leave
    // this a genuine geometric overlap rather than a clean hand-back).
    {
        const size_t probe_layer = top_index - 10;
        const Point  probe       = layer_edge_probe(*object, probe_layer, 1.1);
        CHECK(any_contains(claim_for_layer(*object, probe_layer, /*Extruder3, colour B*/ 3), probe));
        CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder2, colour A*/ 2), probe));
    }
}

// ===========================================================================================
// ITEM 1 / ITEM 2 (.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-report.md,
// interclaim-sliver-investigation.md): the interior inter-claim absorb (a third stage in
// merge_segmented_layers) and the band-level opening (segmentation_top_and_bottom_layers).
// ===========================================================================================

// Pure-geometry helper: an axis-aligned CCW rectangle. Coordinates are plain scaled-unit
// integers with no mm meaning - used only by the interclaim_absorb_winner unit test below,
// which is deliberately independent of any mesh/Clipper-driven slicing geometry so its tie-break
// case is provable by construction (exact mirror symmetry) rather than hoped for from a slicer
// run.
ExPolygon absorb_test_rect(coord_t x0, coord_t y0, coord_t x1, coord_t y1)
{
    return ExPolygon({Point(x0, y0), Point(x1, y0), Point(x1, y1), Point(x0, y1)});
}

TEST_CASE("interclaim_absorb_winner: largest shared area wins, and a genuine tie is broken by the lowest colour index", "[paintdepth]")
{
    const ExPolygons island{absorb_test_rect(0, 0, 1000, 1000)};
    const float       eps = 10.f;

    SECTION("clear winner: colour 2's overlap is far larger than colour 1's") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(-50, 400, 50, 600)};     // thin strip: small overlap
        claims[2] = {absorb_test_rect(500, -500, 1500, 1500)}; // whole right half: large overlap
        // claims[3] left empty - must be skipped cleanly, not crash.
        CHECK(interclaim_absorb_winner(island, claims, eps) == 2);
    }

    SECTION("genuine tie: mirror-symmetric overlaps -> the LOWEST colour index (1) wins over 3") {
        std::vector<ExPolygons> claims(4);
        // claims[1] and claims[3] are EXACT mirror images of each other about x=500 (the
        // island's own midline: island spans x in [0,1000]), so intersection_ex(dilated_island,
        // claims[1]) and intersection_ex(dilated_island, claims[3]) are exact mirror images too
        // - their areas are therefore bit-for-bit equal, a genuine tie by construction, not by
        // luck. claims[2] is deliberately given zero overlap, to pin that a colour with NO
        // overlap can never "win" a tie merely by being scanned - only genuine candidates
        // (non-empty intersection) participate.
        claims[1] = {absorb_test_rect(-500, -500, 500, 1500)}; // left half, generous overlap
        claims[2] = {};                                        // empty: zero overlap, must not win
        claims[3] = {absorb_test_rect(500, -500, 1500, 1500)}; // mirror of claims[1] about x=500
        CHECK(interclaim_absorb_winner(island, claims, eps) == 1);
    }

    SECTION("no painted claim touches the island -> 0, never orphan it") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(5000, 5000, 6000, 6000)}; // far away: no overlap at all
        CHECK(interclaim_absorb_winner(island, claims, eps) == 0);
    }
}

// Fix-wave (absorb-tail-review.md Minor 4 / M4): interclaim_absorb_effective_claim_width's own
// unit test, same hand-built-rectangle style as interclaim_absorb_winner's above and for the
// same reason - provable by construction, independent of mesh/Clipper-driven slicing geometry.
// M4's defect was that the absorb's gap-fill-off widening used to be a SINGLE object-wide max,
// so a gap-fill-disabled region ANYWHERE on the object widened the kill width for EVERY island,
// including ones whose actual neighbours all have gap fill on. Both directions of the fix are
// pinned in the two SECTIONs below: an island must NOT be widened by a colour that merely
// EXISTS with gap fill off elsewhere on the object but does not border it (does-not-over-absorb
// direction - M4's own regression), and MUST still be widened by a colour that genuinely DOES
// border it (the widening must still fire where it is actually needed - Item 2's own
// regression, guarded against separately).
TEST_CASE("interclaim_absorb_effective_claim_width: widening applies only where a gap-fill-off colour actually borders the island", "[paintdepth]")
{
    const ExPolygons island{absorb_test_rect(0, 0, 1000, 1000)};
    const float       eps             = 10.f;
    const float       min_claim_width = 0.45f;

    SECTION("mixed object, non-bordering gap-fill-off colour -> stays at min_claim_width (does not over-absorb)") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(-500, -500, 0, 1500)};   // touches the island's left edge (x=0)
        claims[2] = {absorb_test_rect(1000, -500, 1500, 1500)}; // touches the island's right edge (x=1000)
        // Fix-wave 2 (absorb-tail-fixwave2-review.md m3): colour 3 is PRESENT (non-empty) but
        // far from the island (x in [3000,4000] - the eps=10-dilated island only reaches x in
        // [-10,1010]), so this genuinely exercises the intersection_ex(dilated, ...) adjacency
        // test's negative path. Leaving claims[3] EMPTY (as before) would instead exit through
        // the painted_claims[color_idx].empty() early skip without ever reaching that geometry
        // test at all - the adjacency test was only ever exercised in the positive direction.
        claims[3] = {absorb_test_rect(3000, 0, 4000, 1000)};
        std::vector<float> claim_width_gapfill_off_by_color = {0.f, 0.f, 0.f, 0.75f}; // ONLY colour 3 has gap fill off
        const float width = interclaim_absorb_effective_claim_width(island, claims, claim_width_gapfill_off_by_color, min_claim_width, eps);
        CHECK_THAT(width, Catch::Matchers::WithinAbs(min_claim_width, 1e-6));
    }

    SECTION("a colour that actually borders the island has gap fill off -> widened to its own value") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(-500, -500, 0, 1500)};    // touches the island's left edge
        claims[2] = {absorb_test_rect(1000, -500, 1500, 1500)}; // touches the island's right edge
        std::vector<float> claim_width_gapfill_off_by_color = {0.f, 0.f, 0.75f, 0.f}; // colour 2 (a real neighbour) has gap fill off
        const float width = interclaim_absorb_effective_claim_width(island, claims, claim_width_gapfill_off_by_color, min_claim_width, eps);
        CHECK_THAT(width, Catch::Matchers::WithinAbs(0.75f, 1e-6));
    }

    SECTION("two bordering colours both have gap fill off, different widths -> the WIDER one wins") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(-500, -500, 0, 1500)};
        claims[2] = {absorb_test_rect(1000, -500, 1500, 1500)};
        std::vector<float> claim_width_gapfill_off_by_color = {0.f, 0.60f, 0.75f, 0.f};
        const float width = interclaim_absorb_effective_claim_width(island, claims, claim_width_gapfill_off_by_color, min_claim_width, eps);
        CHECK_THAT(width, Catch::Matchers::WithinAbs(0.75f, 1e-6));
    }

    SECTION("empty gapfill-off array (fuzzy skin caller's value) -> always min_claim_width") {
        std::vector<ExPolygons> claims(4);
        claims[1] = {absorb_test_rect(-500, -500, 0, 1500)};
        claims[2] = {absorb_test_rect(1000, -500, 1500, 1500)};
        const float width = interclaim_absorb_effective_claim_width(island, claims, {}, min_claim_width, eps);
        CHECK_THAT(width, Catch::Matchers::WithinAbs(min_claim_width, 1e-6));
    }
}

// ITEM 1 fixture: same construction as slice_bounded_frustum() above, but with a THIRD physical
// extruder and the sloped walls split into TWO painted colours on ADJACENT walls
// (colour_a_facets / colour_b_facets) instead of one colour on all four - the two-colour
// analogue of slice_two_painted_colours(), needed because slice_bounded_frustum() /
// paint_depth_test_config() only ever paint a single colour. New helper rather than extending
// either of those (which stay untouched), matching this file's established precedent - see
// slice_two_painted_colours's own comment above.
// ITEM 2 (loose end 3, shell-setting-and-gapfill-report.md): trailing gap_infill_speed
// parameter, same < 0 "leave at the registry default" sentinel slice_painted_box already uses
// for interlocking_depth, so every existing caller (which omits it) is unaffected.
PrintObject *slice_bounded_frustum_two_colours(double bottom, double top, double height,
                                                const std::vector<int> &colour_a_facets, const std::vector<int> &colour_b_facets,
                                                PaintDepthMode mode, int walls, double layer_height, Print &print,
                                                PerimeterGeneratorType wall_generator = PerimeterGeneratorType::Arachne,
                                                double gap_infill_speed = -1.0)
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-two-colour-frustum.stl";
    ModelVolume *volume  = object->add_volume(make_square_frustum(bottom, top, height));
    object->add_instance();
    object->ensure_on_bed();

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : colour_a_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    for (int facet_idx : colour_b_facets)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder3);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    DynamicPrintConfig config = paint_depth_test_config(mode, walls);
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020", "#2040A0"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4};
    config.option<ConfigOptionFloat>("layer_height")->value                     = layer_height;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = layer_height;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionInt>("top_shell_layers")->value                   = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value              = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value                = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value           = 0.0;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = wall_generator;
    if (gap_infill_speed >= 0.0)
        config.option<ConfigOptionFloat>("gap_infill_speed")->value = gap_infill_speed;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// The -Y and +X sloped walls of make_square_frustum() (see its facet table comment above) -
// ADJACENT walls, sharing the corner at X=+half, Y=-half.
const std::vector<int> FRUSTUM_WALL_NEG_Y = {4, 5};
const std::vector<int> FRUSTUM_WALL_POS_X = {6, 7};
const std::vector<int> FRUSTUM_WALL_POS_Y = {8, 9};

// ITEM 1 regression pin - the investigation's own suggested assertion (interclaim-sliver-
// investigation.md section 5, Option 1, "Testability"): true if ANY layer of `object` has a
// base-coloured component that is simultaneously (a) narrower than kill_width_mm - i.e. it has
// no printable core of its own under an opening by half that width - and (b) entirely clear of
// the F1 wall-stack band at the contour - i.e. NOT F1's own territory. Such a component can only
// be a base sliver trapped between painted claims: genuine base always either touches the
// contour (F1's territory) or is wide enough to have a printable core, so a THIN, INTERIOR base
// component has nothing but painted neighbours around it.
// kill_width_mm defaults to 0.45 (= min_claim_width at stock flows, the absorb's OWN kill width
// - merge_segmented_layers's "t = min_claim_width / 2" - not 2*small_region_threshold, which is
// a narrower, different quantity; see interclaim-absorb-report.md section 1's threshold
// correction), so every ITEM 1 caller that omits the parameter is unaffected. ITEM 2 (loose end
// 3, shell-setting-and-gapfill-report.md) passes the WIDER gap-fill-disabled kill width (0.75mm
// at stock flows) explicitly, so this probe can actually see the wider sliver population that
// configuration produces - the default 0.45mm probe would find nothing even when the underlying
// defect is present, since a ~0.6-0.75mm component has a printable core under a 0.225mm opening.
bool has_interclaim_sliver(const PrintObject &object, double wall_stack_mm = 0.878540, double kill_width_mm = 0.45)
{
    const float t          = float(scale_(kill_width_mm / 2.0));
    const float wall_stack = float(scale_(wall_stack_mm));
    for (size_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer *layer = object.get_layer(int(layer_idx));
        ExPolygons   painted;
        append(painted, claim_for_layer(object, layer_idx, /*Extruder2*/ 2));
        append(painted, claim_for_layer(object, layer_idx, /*Extruder3*/ 3));
        if (painted.empty())
            continue;
        painted = union_ex(painted);
        const ExPolygons base_area = diff_ex(layer->lslices, painted);
        if (base_area.empty())
            continue;
        const ExPolygons interior = offset_ex(layer->lslices, -wall_stack);
        for (const ExPolygon &island : base_area) {
            const ExPolygons single{island};
            if (! opening_ex(single, t).empty())
                continue;
            if (! diff_ex(single, interior).empty())
                continue;
            return true;
        }
    }
    return false;
}

TEST_CASE("multi_material_segmentation_by_painting: two adjacent painted claims on FLAT facets stay sliver-free on both sides of the fix (Item 1 sanity/no-false-positive)", "[paintdepth]")
{
    // T1's exact 15 deg fixture (well inside the <24 deg band Item 2 is about), split into two
    // colours on adjacent walls instead of one colour on all four. Their shared corner
    // (X=+half, Y=-half) was the FIRST hypothesis for where a sliver would appear, but measured
    // (not asserted) against pre-fix code, it does not: has_interclaim_sliver finds nothing here
    // either before or after the fix. Root cause, worked out by hand-tracing the descent loop
    // (interclaim-absorb-report.md): on a FLAT facet every originating layer's own top_ex has the
    // SAME ring width r, so the per-colour opening_ex is an all-or-nothing gate per colour (either
    // the whole wall survives it, at 15deg here, or none of it does) - never a locally varying
    // one, so there is nothing for a neighbouring colour to inherit. Genuine curvature is required
    // (see the sphere-based fixture further below, which DOES reproduce the defect and is this
    // fix's real RED/GREEN pin). Kept as a deliberate negative control: a flat-faceted, in-band
    // two-colour claim boundary must stay sliver-free on BOTH sides of the fix, i.e. the absorb
    // must not manufacture a false positive out of ordinary flat geometry.
    //
    // Run on both generators (the process explicitly calls for this: Arachne is the worse case -
    // it prints every survivor as a widened bead - but the underlying geometry defect, and this
    // fix, are generator-independent, so the same invariant must hold on Classic too).
    for (PerimeterGeneratorType generator : {PerimeterGeneratorType::Arachne, PerimeterGeneratorType::Classic}) {
        DYNAMIC_SECTION("generator " << (generator == PerimeterGeneratorType::Arachne ? "arachne" : "classic")) {
            Print        print;
            PrintObject *object = slice_bounded_frustum_two_colours(40.392, 18., 3., FRUSTUM_WALL_NEG_Y, FRUSTUM_WALL_POS_X,
                                                                     pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print, generator);
            REQUIRE(object->layer_count() >= 27);
            CHECK_FALSE(has_interclaim_sliver(*object));

            // ...and the absorbed area is not double-claimed: an absorbed island is appended to
            // exactly one winning colour and removed from base, never left claimed by both.
            for (size_t layer_idx = 0; layer_idx < object->layer_count(); ++layer_idx) {
                CAPTURE(layer_idx);
                const ExPolygons claim_a = claim_for_layer(*object, layer_idx, /*Extruder2*/ 2);
                const ExPolygons claim_b = claim_for_layer(*object, layer_idx, /*Extruder3*/ 3);
                CHECK(intersection_ex(claim_a, claim_b).empty());
            }
        }
    }
}

TEST_CASE("multi_material_segmentation_by_painting: a genuine base region wider than the threshold between two painted claims stays base (Item 1 does not over-absorb)", "[paintdepth]")
{
    // OPPOSITE-ish walls painted (not adjacent): -Y and +Y, leaving the +X and -X sides of the
    // frustum as genuine, many-mm-wide unpainted base - nowhere near either colour's claim and
    // nowhere near thin. If the absorb ever ate real base area instead of only thin interior
    // slivers, this is what it would touch.
    Print        print;
    PrintObject *object = slice_bounded_frustum_two_colours(40.392, 18., 3., FRUSTUM_WALL_NEG_Y, FRUSTUM_WALL_POS_Y,
                                                             pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 27);

    const size_t       probe_layer = 12;
    const BoundingBox bb = get_extents(object->get_layer(int(probe_layer))->lslices);
    REQUIRE(bb.defined);
    // Dead centre of the +X side, far (multiple mm) from both painted walls and from the
    // frustum's own contour - unambiguously genuine base on any reasonable claim width.
    const Point centre_probe(coord_t(bb.max.x() - scale_(1.0)), (bb.min.y() + bb.max.y()) / 2);

    CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder2*/ 2), centre_probe));
    CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder3*/ 3), centre_probe));
}

TEST_CASE("multi_material_segmentation_by_painting: the interior inter-claim absorb respects the F1 guard - the interlocking notch tooth is not absorbed (Item 1)", "[paintdepth]")
{
    // Reuses the walls-mode interlock-notch fixture/math from the band-width test earlier in
    // this file. A single painted colour is enough here: the notch is CONNECTED to the wide base
    // core behind it, so it is exactly the "thin tooth attached to a real base component" case
    // the per-connected-component guard exists to protect - a NAIVE implementation that ran
    // diff_ex(base_area, opening_ex(base_area, t)) on the WHOLE base area at once (instead of
    // component-by-component) would strip this tooth right along with genuine slivers, because
    // the tooth reads as "thin" only in isolation, never as part of its own (wide) connected
    // component. Independent of how many colours are painted.
    Print        print;
    PrintObject *object = slice_painted_cube(PLUS_X_FACE, pdmWalls, 3, print);

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
    const double interlock_mm = paint_depth_interlocking_depth_mm(pdmWalls, object->config().mmu_segmented_region_interlocking_depth.value, min_perimeter_spacing);
    REQUIRE(interlock_mm > 0.);
    REQUIRE(interlock_mm < 0.225); // the notch (default 0.1mm) really is narrower than the absorb threshold

    REQUIRE(object->layer_count() >= 10);
    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    REQUIRE(even_layer % 2 == 0);

    // Inside the interlock "tooth" (between band-interlock and band) on the even (notched)
    // layer: still base, not absorbed - even though it is narrower than the absorb threshold and
    // directly adjacent to the painted claim on one side.
    const double interlock_notch_mm = band_mm - interlock_mm / 2.0;
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, even_layer), layer_edge_probe(*object, even_layer, interlock_notch_mm)));
}

TEST_CASE("multi_material_segmentation_by_painting: Item 2's band-level opening does not change unlimited mode (legacy parity)", "[paintdepth]")
{
    // paint_depth_normal_mm - which Item 1's bounded_mode gate (merge_segmented_layers) and
    // Item 2's normal_shell gate (segmentation_top_and_bottom_layers) both trace back to - is
    // forced to exactly 0.f whenever paint_depth_mode == pdmUnlimited, unconditionally
    // (MultiMaterialSegmentation.cpp, multi_material_segmentation_by_painting: "paint_depth_
    // normal_mm = paint_depth_mode != pdmUnlimited ? max_width : 0.f"). That is a config-only
    // gate, not a geometric one, so this pins it the same way the file's other legacy-parity
    // pairs do (e.g. "unlimited mode leaves the same painted face unbounded" above): the SAME
    // two-colour fixture the Item 1 RED/GREEN test above uses is sliced TWICE, independently, in
    // pdmUnlimited, and both runs must agree exactly - neither fix's new code executes anything
    // that could make an unlimited-mode result depend on run ordering (the absorb's own
    // determinism guard, re-affirmed at the level this file can actually observe).
    Print        print1, print2;
    PrintObject *object1 = slice_bounded_frustum_two_colours(40.392, 18., 3., FRUSTUM_WALL_NEG_Y, FRUSTUM_WALL_POS_X,
                                                              pdmUnlimited, /*walls=*/3, /*layer_height=*/0.1, print1);
    PrintObject *object2 = slice_bounded_frustum_two_colours(40.392, 18., 3., FRUSTUM_WALL_NEG_Y, FRUSTUM_WALL_POS_X,
                                                              pdmUnlimited, /*walls=*/3, /*layer_height=*/0.1, print2);
    REQUIRE(object1->layer_count() == object2->layer_count());
    for (size_t layer_idx = 0; layer_idx < object1->layer_count(); ++layer_idx) {
        CAPTURE(layer_idx);
        for (int extruder_id : {2, 3}) {
            CAPTURE(extruder_id);
            const ExPolygons a = claim_for_layer(*object1, layer_idx, extruder_id);
            const ExPolygons b = claim_for_layer(*object2, layer_idx, extruder_id);
            CHECK(diff_ex(a, b).empty());
            CHECK(diff_ex(b, a).empty());
        }
    }
}

TEST_CASE("multi_material_segmentation_by_painting: Item 2 (band-level opening) and the true ceiling on a uniform-slope surface", "[paintdepth]")
{
    // Item 2 (interclaim-absorb-report.md / interclaim-sliver-investigation.md section 5 Option
    // 2, section 6): moves the per-STEP opening_ex (segmentation_top_and_bottom_layers's
    // :2079/:2146 sites) to the ACCUMULATED band (the merge loop consuming shell_triangles_by_
    // color_top/bottom), so overlapping contributions from DIFFERENT originating surface layers
    // landing in the same destination slot are opened together rather than each filtered alone.
    //
    // On a SINGLE UNIFORM-angle frustum wall that mechanism has nothing to bite: every
    // originating layer's OWN top_ex has the SAME ring width r = layer_height/tan(theta) (the
    // staircase tread of a constant slope), so past the ceiling every layer's SURFACE opening
    // (opening_ex(top_ex, small_region_threshold), the :1946 site - which Item 2 explicitly does
    // NOT touch) already empties top_ex before any originating layer descends at all. There is
    // then nothing left for the moved :2079/:2146 opening to have been over-filtering.
    //
    // Measured here, not asserted analytically, per the report's own instruction to verify and
    // report the TRUE number rather than claim one.
    struct SlopeCase { double degrees; double bottom; };
    // bottom = 2*(9 + 3/tan(theta)), same construction as the existing "headline numbers" test.
    const SlopeCase cases[] = {
        {24., 31.4765},
        {25., 30.8670}, // the existing headline test's own value at this angle
        {28., 29.2845},
        {30., 28.3923},
    };
    for (const SlopeCase &c : cases) {
        DYNAMIC_SECTION("slope " << c.degrees << " deg") {
            Print        print;
            PrintObject *object = slice_bounded_frustum(c.bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                         pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
            REQUIRE(object->layer_count() >= 27);

            const double reach = claim_reach_mm(*object, /*layer_idx=*/12);
            CAPTURE(c.degrees);
            CAPTURE(reach);
            // The lateral band alone on this even layer is ~1.34mm (1.435675mm floored by the
            // 0.1mm interlock notch); a deepened normal-thickness descent reaches several mm
            // further (T1's 15deg case reaches 5.6mm). This range distinguishes the two; see the
            // report for the measured value at each angle and the ceiling conclusion.
            CHECK(reach > 1.0);
            CHECK(reach < 2.0);
        }
    }
}

// ITEM 2 (loose end 3, shell-setting-and-gapfill-report.md): same sphere construction as the
// Item 1 curved two-colour fixture below, extracted into a helper so it can be built twice -
// once at the registry's own gap_infill_speed default (today's gap-fill-ON behavior, unchanged)
// and once with it forced to 0 (gap fill disabled - the configuration loose end 3 identified as
// widening the absorb's own sliver population past its kill width). gap_infill_speed < 0 (the
// same sentinel slice_bounded_frustum_two_colours/slice_painted_box already use) means "leave it
// at whatever paint_depth_test_config's own registry default resolves to".
// Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): gapfill_off_modifier_z_min/_max add an
// OPTIONAL PARAMETER_MODIFIER slab confined to [z_min, z_max] with gap_infill_speed=0 - the
// review's own probe C construction (a Z-confined gap-fill-off region, geometrically FAR from
// the sliver population it corrupts). Sentinel -1.0 (default, matching every existing caller's
// omission of these two arguments byte-for-byte) means "no modifier" - trailing, defaulted,
// backward-compatible, same convention slice_bounded_frustum/slice_two_painted_colours already
// use for their own added parameters (fix-wave 2 report, "one genuinely new inline E2E
// construction" precedent for this exact PARAMETER_MODIFIER pattern - reused here on the sphere
// rather than duplicated as a whole new fixture-builder function).
PrintObject *slice_bounded_sphere_two_colours(double radius, PaintDepthMode mode, int walls, Print &print,
                                               double gap_infill_speed = -1.0,
                                               double gapfill_off_modifier_z_min = -1.0,
                                               double gapfill_off_modifier_z_max = -1.0)
{
    constexpr double kPi = 3.14159265358979323846;

    Model        model;
    ModelObject *object = model.add_object();
    object->name        = "paint-depth-sphere-two-colour.stl";
    ModelVolume *volume  = object->add_volume(make_sphere(radius, kPi / 12.));

    // Colour split by each facet's own LOCAL slope-from-horizontal, computed from its face
    // normal (its_face_normal) rather than a hard-coded facet index list, so this does not
    // depend on its_make_sphere's internal vertex/facet ordering. nz = the normal's Z component
    // = sin(beta), beta = polar angle from the equator; slope-from-horizontal = (90-beta) deg.
    //   colour2 (shallow cap):   nz > sin(66deg) = 0.9135  -> slope < 24deg
    //   colour3 (steeper band):  0.3 < nz <= 0.9135         -> 24deg <= slope < 72deg
    //   left unpainted (base):   nz <= 0.3 (near/below the equator, and the whole lower
    //                            hemisphere) - comfortably clear of both painted claims.
    // mesh() returns TriangleMesh BY VALUE - keep a named local so `its` below does not
    // reference a member of an already-destroyed temporary.
    const TriangleMesh sphere_mesh = volume->mesh();
    TriangleSelector    selector(sphere_mesh);
    for (size_t facet_idx = 0; facet_idx < sphere_mesh.its.indices.size(); ++facet_idx) {
        const float nz = its_face_normal(sphere_mesh.its, int(facet_idx)).z();
        if (nz > 0.9135f)
            selector.set_facet(int(facet_idx), EnforcerBlockerType::Extruder2);
        else if (nz > 0.3f)
            selector.set_facet(int(facet_idx), EnforcerBlockerType::Extruder3);
    }
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    // Fix-wave 3 (I-1): optional Z-confined gap-fill-off PARAMETER_MODIFIER, geometrically FAR
    // from any painted claim boundary (the sphere's colour split is entirely a function of
    // facet normal, not Z, so a low modifier at [z_min, z_max] shares no geometry at all with
    // the cap-boundary slivers this fixture is used to probe near the sphere's own apex).
    // Oversized (2.5x the sphere's own diameter) and centred on the sphere's local origin (0,0)
    // - its_make_sphere centres the mesh there, see that function's own vertex construction -
    // so it fully spans the sphere's cross-section at any Z, matching the "oversized... fully
    // spans whatever it overlaps" convention the box+modifier fixture above already uses.
    // z_min/z_max are given in the same PRINT-Z frame slice_bounded_sphere_two_colours' own
    // callers probe layers in (e.g. `first_layer_above_z`) - NOT the sphere mesh's own local
    // frame (which spans [-radius, radius] before ensure_on_bed() below shifts the whole
    // instance up by +radius so the sphere's own bottom lands at print z 0). Placed here, before
    // that shift, at local Z = print Z - radius, so it lands at the intended PRINT Z afterward.
    if (gapfill_off_modifier_z_min >= 0.0 && gapfill_off_modifier_z_max > gapfill_off_modifier_z_min) {
        const double modifier_side = 5.0 * radius;
        ModelVolume *modifier = object->add_volume(make_cube(modifier_side, modifier_side, gapfill_off_modifier_z_max - gapfill_off_modifier_z_min),
                                                    ModelVolumeType::PARAMETER_MODIFIER);
        modifier->translate(-0.5 * modifier_side, -0.5 * modifier_side, gapfill_off_modifier_z_min - radius);
        modifier->config.set_key_value("gap_infill_speed", new ConfigOptionFloat(0.0));
    }

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(mode, walls);
    config.set_num_extruders(3);
    config.set_num_filaments(3);
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020", "#2040A0"};
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = {0.4, 0.4, 0.4};
    config.option<ConfigOptionFloat>("layer_height")->value                     = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = 0.1;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionInt>("top_shell_layers")->value                   = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value              = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value                = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value           = 0.0;
    if (gap_infill_speed >= 0.0)
        config.option<ConfigOptionFloat>("gap_infill_speed")->value = gap_infill_speed;

    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
}

// ITEM 1, second (curved) fixture. The frustum-based test above turns out NOT to reproduce the
// defect: every originating layer's own top_ex has the SAME ring width r on a FLAT facet, so the
// per-colour opening is an all-or-nothing gate per colour (either the whole wall survives it or
// the whole wall does not) - never a locally varying one, so there is nothing for a neighbouring
// colour to inherit. The investigation's own mechanism needs r to vary from originating layer to
// originating layer, so that SOME descent-step contributions survive their own opening_ex while
// adjacent ones (from a nearby, slightly steeper originating layer) do not - which needs genuine
// surface CURVATURE, not a flat facet.
//
// A sphere gives exactly that: local slope-from-horizontal at polar angle beta from the equator
// is (90-beta) degrees, continuously varying, so painting two colours split at beta=66deg (slope
// 24deg, the #7104 ceiling) puts the transition interclaim-sliver-investigation.md analyses right
// at the colour boundary, with genuine curvature on both sides of it - much closer to the
// reported defect (a curved painted face) than any flat-faceted primitive in this file.
TEST_CASE("multi_material_segmentation_by_painting: two adjacent painted claims on a curved (sphere) surface leave NO base-coloured sliver at their shared boundary (Item 1)", "[paintdepth]")
{
    Print        print;
    PrintObject *out_object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print);

    // RED/GREEN, measured on this exact fixture with the absorb toggled off and on
    // (interclaim-absorb-report.md "Diagnosis"): with the absorb OFF, has_interclaim_sliver
    // finds FOUR genuine base annuli - 9.43, 8.79, 3.49 and 2.75 mm2, each ~0.34mm wide - on
    // layers 135-138, exactly at the colour2/colour3 boundary near z = 13.6-13.9mm, plus ~430
    // sub-0.00002 mm2 Clipper fragments along their rims. With it ON, the count is ZERO at any
    // area: every one of those components is thin, fully interior and painted-neighboured, so
    // the absorb hands each to a painted claim (colour 3 for the annuli) and the rim fragments
    // go with them. This assertion is therefore NOT satisfied by an area floor - it holds
    // exactly, which is why none is used.
    CHECK_FALSE(has_interclaim_sliver(*out_object));
}

// absorb-tail-review.md I1: the "painted twin" of has_interclaim_sliver above. That helper
// scans the BASE colour's residue for a component the interclaim absorb should have caught;
// this one scans each PAINTED colour's own FINAL claim for a component the #7104
// thin-projection filter (opening_ex(., small_region_threshold) - MultiMaterialSegmentation.cpp
// :1960/:2111/:2131/:2180, moved to the band level at :2268-2269 by Item 2) should already have
// removed and never did. small_region_threshold_mm defaults to 0.1125mm = the SAME quantity
// stat.small_region_threshold resolves to at stock flows (outer_wall_line_width 0.45mm, gap
// fill on: 0.5 * 0.5 * 0.45 = 0.1125 - MultiMaterialSegmentation.cpp :1855-1860), i.e. this is
// the filter's own kill-width delta, deliberately narrower than has_interclaim_sliver's
// interclaim-absorb kill width (0.225mm) - I1 is about a filter that should already have run
// once, upstream of and independent from the interclaim absorb entirely. wall_stack_mm defaults
// to the same F1 clearance has_interclaim_sliver uses, for the same reason: a component this
// thin that also touches the contour is F1's own legitimate territory, not a leaked fragment.
//
// Fix-wave 2 (absorb-tail-fixwave2-review.md I-A): NO area floor - matches has_interclaim_
// sliver's own discipline just above (a WIDTH test via opening_ex, not an area test). The
// previous wave's min_area_mm2 default (1.0mm2) was, by the review's own hand-execution, TUNED
// to the residual it was meant to catch: the pin failed at the geometrically-derivable floor
// (3 x sqr(scale_(0.1f)) = 0.03mm2, the same "not real geometry" constant segmentation_by_
// painting's own preprocessing uses), so the floor was raised past every post-fix fragment
// (0.315, 0.133, 0.128, 0.079mm2) instead of the fragments being eliminated. This wave's I1
// follow-up (see the merge_segmented_layers fix-wave comment) additionally opens the legacy
// shadow at its own build site with the SAME small_region_threshold `full`'s own band-level
// opening already uses, which stops it from shaping the final claim with un-opened sub-
// threshold fingers at all - so this check is now EXACT (extruder_id parameter, caller's
// choice, no floor of any kind) for Extruder 2, the colour this mechanism actually touches.
//
// Extruder 3 carries a SEPARATE, LARGER (3.5-4.0mm2) thin+interior ring population at layers
// 146-148 that the previous wave excluded via the same (tuned) floor rather than explaining.
// Fix-wave 2 attributed it to "legacy multi-colour lateral-vs-top/bottom trimming... genuinely
// out of scope for a paint-DEPTH fix" - MEASURED and CORRECTED by fix-wave 3 (absorb-tail-
// fixwave2-review.md I-2): it is this feature's OWN Wave-B cross-colour clip revealing colour
// 3's OWN Stage-1-clamped lateral claim, not legacy trimming and not independent of anything
// this feature has touched. See the TEST_CASEs below (after the Important-1 / Extruder-2 test
// just below) for the corrected mechanism, the "is it a visible defect" assessment, and the
// decision to pin it as a known-benign artefact rather than eliminate it.
bool has_painted_unopened_fragment(const PrintObject &object, int extruder_id, double wall_stack_mm = 0.878540,
                                    double small_region_threshold_mm = 0.1125)
{
    const float  t          = float(scale_(small_region_threshold_mm));
    const float  wall_stack = float(scale_(wall_stack_mm));
    for (size_t layer_idx = 0; layer_idx < object.layer_count(); ++layer_idx) {
        const Layer     *layer    = object.get_layer(int(layer_idx));
        const ExPolygons interior = offset_ex(layer->lslices, -wall_stack);
        ExPolygons claim = claim_for_layer(object, layer_idx, extruder_id);
        if (claim.empty())
            continue;
        claim = union_ex(claim);
        for (const ExPolygon &island : claim) {
            const ExPolygons single{island};
            if (! opening_ex(single, t).empty())
                continue; // has a core under the filter's OWN kill width -> not a leaked fragment
            if (! diff_ex(single, interior).empty())
                continue; // touches/extends into the F1 wall-stack band -> F1's own territory
            return true;
        }
    }
    return false;
}

TEST_CASE("multi_material_segmentation_by_painting: a painted colour's final claim never carries an un-opened raw fragment the #7104 filter should have removed (Important 1)", "[paintdepth]")
{
    // absorb-tail-review.md I1: merge_segmented_layers's cross-colour clip (:2732- , the
    // wave-b-review.md Important 2 fix) used to rebuild a painted colour's DEEPENED claim as
    // `legacy U (excess \ other_painted_laterals)`, referencing the legacy shadow VERBATIM.
    // That shadow (segmentation_top_and_bottom_layers's legacy_top_and_bottom_layers_out,
    // :2216-2228) is fed the RAW `last` from the descent loop whenever normal_shell is true
    // (:2110/:2179 skip their per-step opening_ex exactly then, deferring to the band-level
    // opening at :2268-2269 instead) and is itself never opened - so on any layer where this
    // colour's excess (its deepened, beyond-legacy-shell reach) overlaps ANOTHER painted
    // colour's own lateral claim (other_painted_laterals non-empty - true at any two-colour
    // boundary), the old formula reintroduced whatever raw, un-opened, sub-threshold ring
    // fragments the legacy shadow happened to carry, bypassing the filter for exactly that
    // portion of the claim. Same curved sphere fixture as the Item 1 sliver-free test above -
    // two adjacent painted colours give a two-colour boundary at every layer near it, and the
    // fixture's genuine surface curvature is what makes per-step ring widths vary in the first
    // place (see that test's own comment on why a flat facet cannot reproduce this class of
    // defect at all). RED pre-fix (this fixture reproduces the shadow-bypass on HEAD), GREEN
    // post-fix (merge_segmented_layers now only ever subtracts from the already band-opened
    // `full`, so it can never reintroduce a fragment that opening removed).
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print);

    CHECK_FALSE(has_painted_unopened_fragment(*object, /*Extruder2*/ 2));
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-A) explained (did not merely exclude) the
// Extruder-3 thin ring population at layers 146-148 the previous wave found and left out of its
// pin - but that explanation does not survive hand-execution. CORRECTED here by fix-wave 3
// (absorb-tail-fixwave2-review.md I-2).
//
// REFUTED (fix-wave 2's claim): "a pre-existing characteristic of legacy multi-colour
// lateral-vs-top/bottom trimming (merge_segmented_layers's UNCONDITIONAL per-extruder trim
// loop)... genuinely out of scope for a paint-DEPTH fix", reasoned from the ring persisting in
// pdmUnlimited mode where the absorb-tail-specific machinery (Item 1/2, fix-wave 2's own I1 fix)
// is provably inert. That reasoning proves too little: pdmUnlimited ALSO disables Stage 1's own
// lateral clamp (segmentation_max_width == 0 there) AND makes the cross-colour CLIP a no-op for
// an unrelated reason (normal_shell is false everywhere there, so `full` and `legacy` are built
// identically and `excess` - full \ legacy - is empty on every layer; the clip site's own
// `if (excess.empty()) continue;` fires throughout). So the ring's absence in pdmUnlimited is
// consistent with EITHER "this is legacy code" OR "this is paint-depth code that also happens to
// go inert in unlimited mode, for a reason unrelated to the clamp" - it does not distinguish
// between them, and the previous wave's "yet the SAME ring persists... proof that the ring
// predates and is independent of every line this feature has ever touched" overclaimed what a
// single non-discriminating negative result can show.
//
// CORRECTED (measured, MultiMaterialSegmentation.cpp on this HEAD): the ring's INNER edge equals
// `r_slice - region_cut_width` (:1309, Stage 1's own lateral clamp - the notched/un-notched band
// on alternating interlock parities) to three decimals on both parities; its OUTER edge is the
// boundary between colour 2's LEGACY top/bottom contributions (:2343, `m < top_shell_layers`,
// exempt from the cross-colour clip) and its EXCESS ones (clipped wherever they overlap another
// painted colour's own lateral claim - the clip itself, :3144, `full = diff_ex(full,
// intersection_ex(excess, other_painted_laterals))`, Wave B's own Important-2 fix - THIS
// feature's own paint-depth code, not legacy, and the very line this fix-wave's own I-1 change
// re-comments). So the ring is colour 3's OWN (Stage-1-clamped) lateral claim, appearing exactly
// where the clip now correctly refuses to let colour 2's excess top/bottom claim override it:
// two of THIS feature's OWN mechanisms interacting (the clamp, the clip), not one unrelated
// legacy mechanism acting alone. Without the clip, colour 2 would own the whole disk solid and
// colour 3's remnant would be its full ~0.9mm-wide clamped band, not a thin ring; without the
// clamp, colour 3's remnant would reach the centre. Both are required, and one of them (the
// clip) is paint-depth's own code.
//
// IS IT A VISIBLE DEFECT? No (measured, not assumed): the ring is colour 3, not base; it sits
// >= 0.9mm inside the contour under the cap's own shell; colour 3 already prints its outer band
// on the same layer, so no toolchange is added by it. Arachne widens the 0.2mm ring to a
// min-width bead of the "wrong" colour hidden inside colour 2; Classic emits nothing for it (a
// hairline void). Cost is quality noise on an invisible interior seam, not a fidelity or
// structural defect.
//
// DECISION (fix-wave 3): ENSHRINE, not eliminate. A clean elimination means the clip must stop
// treating "does colour 2's excess overlap ANY other painted colour's lateral claim" as one
// merged test (`other_painted_laterals` appends every OTHER colour's claim together, so the clip
// site has already lost track of which specific neighbour owns which part of the overlap by the
// time it runs) and instead clip PER NEIGHBOUR COLOUR, no further than leaving that neighbour's
// OWN remaining claim at least its small_region_threshold wide - i.e. a genuinely new
// painted-vs-painted absorb, keyed on each neighbour's own per-layer threshold (now correctly
// resolved - see compute_layer_color_stat, fix-wave 3 I-1), not a one-line tweak to the existing
// subtract. That is real new surface area on the exact clip this feature has already hand-tuned
// twice (Wave B, then this fix-wave's own I-A), for a defect that is - measured, not assumed -
// already invisible and toolchange-free. Not taken this wave: the risk/reward does not clear the
// bar a core segmentation clip site should have to clear. The two TEST_CASEs below therefore
// KNOWINGLY PIN this artefact as expected behaviour rather than fix it - stated here honestly,
// not left for a future reader to rediscover the hard way.
TEST_CASE("multi_material_segmentation_by_painting: the Extruder-3 thin ring at layers 146-148 is the Wave-B cross-colour clip's remnant of colour 3's Stage-1-clamped lateral claim - present at pdmWalls, pinned as a known-benign artefact, not eliminated (Fix-wave 3 I-2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print);

    CHECK(has_painted_unopened_fragment(*object, /*Extruder3*/ 3));
}

TEST_CASE("multi_material_segmentation_by_painting: the Extruder-3 thin ring does NOT appear in unlimited mode - consistent with, but not proof of, the Stage-1-clamp dependency alone (the cross-colour clip is ALSO inert there, for an unrelated reason) (Fix-wave 3 I-2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmUnlimited, 3, print);

    // Measured, not assumed - kept as the honest record of what this negative result actually
    // shows, not silently rewritten to claim more. It does NOT discriminate between "caused by
    // the clamp alone" and "caused by the clamp+clip interaction" (see the corrected header
    // comment above): both are inert in pdmUnlimited mode simultaneously (the clamp because
    // segmentation_max_width == 0 there; the clip because normal_shell == false makes `excess`
    // empty on every layer, an unrelated reason to the clamp being off).
    CHECK_FALSE(has_painted_unopened_fragment(*object, /*Extruder3*/ 3));
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md Minor / m1): the I1 fix's own correctness argument
// is SUBTRACT-ONLY (`full := diff_ex(full, ...)` can never ADD area to an already-trimmed claim
// - see the corrected comment at the fix site, MultiMaterialSegmentation.cpp), which is weaker
// than but implies the cheapest strong invariant this loop can offer: two DIFFERENT painted
// colours' final claims never physically overlap. Pinned directly on the same curved sphere
// fixture the Item 1 tests use, where painted-vs-painted overlap is geometrically possible at
// all (the colour2/colour3 boundary) - every OTHER fixture in this file paints only one colour.
//
// Floor: MEASURED (not assumed) at 0.0000000052mm2, a single-fragment artefact at one layer -
// four to five orders of magnitude below has_painted_unopened_fragment's own documented Clipper-
// rounding ceiling (sub-0.0001mm2, itself citing has_interclaim_sliver's independently-measured
// "~430 sub-0.00002mm2 Clipper fragments"). A raw claim2/claim3 comparison runs each colour's own
// independent chain of unions/diffs/offsets (including the per-colour "Remove dimples (#7235)"
// offset2_ex close in merge_segmented_layers) separately, so a hairline, sub-nanometer-scale
// mismatch at a shared boundary is expected arc-approximation noise, not a geometric defect - the
// SAME class of artefact this file already documents and tolerates elsewhere, not a NEW floor
// invented for this residual (this measured value is over 10000x smaller than the existing
// documented ceiling, i.e. nowhere near "tuned to the residual").
constexpr double kClipperRoundingCeilingMm2 = 0.0001;
TEST_CASE("multi_material_segmentation_by_painting: two different painted colours' final claims never overlap beyond Clipper rounding noise, on any layer (Fix-wave 2 Minor / m1, 2x3 disjointness)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print);

    for (size_t layer_idx = 0; layer_idx < object->layer_count(); ++layer_idx) {
        const ExPolygons claim2 = claim_for_layer(*object, layer_idx, /*Extruder2*/ 2);
        const ExPolygons claim3 = claim_for_layer(*object, layer_idx, /*Extruder3*/ 3);
        if (claim2.empty() || claim3.empty())
            continue;
        const ExPolygons overlap = intersection_ex(claim2, claim3);
        if (overlap.empty())
            continue;
        double overlap_area_mm2 = 0.;
        for (const ExPolygon &e : overlap)
            overlap_area_mm2 += e.area();
        overlap_area_mm2 *= sqr(unscale<double>(1.0)); // scaled-unit^2 -> mm^2
        CAPTURE(layer_idx);
        CAPTURE(overlap_area_mm2);
        CAPTURE(overlap.size());
        CHECK(overlap_area_mm2 < kClipperRoundingCeilingMm2);
    }
}

// absorb-tail-review.md Minor 3 / M3: the interior inter-claim absorb's F1 guard was meant to
// spare the F2 degradation ladder's own deliberately-preserved base residue
// (paint_depth_clamp_keep_core, cut_segmented_layers) - but F1's own clearance is only
// wall_stack (0.878540mm at stock flows) from the contour, while keep_core's residue on a
// two-face-painted wall sits `band` (1.435675mm at walls=3) from EACH face - deeper than F1
// reaches, so F1 never exempts it and the absorb hands it to the only painted neighbour
// touching it from both sides.
//
// A 3.1mm-thick, 40mm-long, 6mm-tall box, ALL FOUR side faces (ALL_SIDE_FACE) painted the SAME
// colour (Extruder2) - a genuine two-face-painted (in the binding X dimension) thin wall,
// unambiguous which colour would absorb the centre residue if the exemption fails. ALL four
// sides, not just the two X ones: with only the X faces painted, the UNPAINTED Y=0/Y=y end caps
// leave their own genuine base material at the wall's two short ends, connected (same 2D
// component, per layer) to the thin centre residue strip - and that end-cap base, being close
// to two-dimensionally-adjacent contour corners, has its own locally wider "printable core"
// there. opening_ex is a WHOLE-COMPONENT test (deliberately, so it doesn't split the
// interlocking notch tooth off its own wide base - see the absorb's own comment), so that one
// wide corner exempts the ENTIRE connected island, including the actual thin centre strip -
// case (c) genuine base, not case (a)/keep-core, defeats the very mechanism this test means to
// exercise (measured: disabling the keep-core exemption made no observable difference with only
// the X faces painted). Painting all four sides removes the unpainted end caps entirely, so the
// residue is one long (~37mm), UNIFORMLY thin (~0.23mm) strip with no wide corner anywhere -
// band erodes it back from every side, X and Y alike, and Y (40mm) is nowhere near thin enough
// to matter; only X (3.1mm) binds. 3.1mm falls inside M3's own window (2*band, 2*band +
// min_claim_width] = (2.871350, 3.321350]mm at these stock settings (walls=3, 0.45mm
// outer_wall_line_width, 0.1mm layers - the same flow numbers the Important 2 fixture above
// derives by hand). interlocking_depth is forced to 0 so the interlock notch cannot also narrow
// the window on even layers (M3's own noted, orthogonal complication) - this test is about the
// base clamp width alone, not the notch.
TEST_CASE("multi_material_segmentation_by_painting: the interior inter-claim absorb never eats the F2 degradation ladder's own keep-core (Minor 3)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(3.1, 40., 6., ALL_SIDE_FACE, pdmWalls, /*walls=*/3, /*paint_depth_mm=*/0.,
                                             /*layer_height=*/0.1, print, /*interlocking_depth=*/0.);
    REQUIRE(object->layer_count() >= 40);

    const size_t       probe_layer = object->layer_count() / 2; // clear of top/bottom shell layers
    const BoundingBox bb = get_extents(object->get_layer(int(probe_layer))->lslices);
    REQUIRE(bb.defined);
    const Point centre_probe((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);

    // RED pre-fix: the only painted claim touching the residue from both faces (Extruder2)
    // absorbs it, so the centre reads as painted. GREEN post-fix: the residue is F2's own
    // keep-core, exempted, so the centre stays base.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, probe_layer), centre_probe));
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-D): the keep-core re-test (MultiMaterialSegmentation
// .cpp, merge_segmented_layers) used min_claim_width/2 (kill 0.45mm) instead of the SAME widened
// `t` (effective_claim_width/2) the OUTER absorb test already computed for this exact island - so
// with gap fill OFF (effective_claim_width widened to ~0.75mm at stock flows, min_claim_width_
// gapfill_off_by_color's own per-colour value), a keep-core residue between 0.45 and 0.75mm wide
// is a genuine "printable core" under the outer test's own 0.375mm delta (never even reaching
// interclaim_absorb_winner on a healthy, single-colour-painted object) but reads as "not the thin
// residue" under the STALE 0.225mm delta the re-test used - so the keep-core exemption never
// fires, and interclaim_absorb_winner (which finds the one painted neighbour touching it) absorbs
// it. The default 0.1mm interlock notch alternately widens/narrows the keep-core's own erosion
// per layer parity, so wherever that alternation straddles the 0.75mm line, ONE parity gets
// wrongly absorbed and the other does not - the same region churn Minor 3 originally flagged, now
// reappearing whenever gap fill is off. Same box fixture as Minor 3 above, width chosen inside
// the review's own measured churn window (3.42, 3.62]mm, interlocking_depth left at its registry
// default (unlike Minor 3's own interlock=0 probe, which isolates the base mechanism instead of
// the alternation) so the notch is actually in play.
TEST_CASE("multi_material_segmentation_by_painting: the interior inter-claim absorb's keep-core exemption tracks the widened gap-fill-off threshold on both layer parities (Fix-wave 2 I-D)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_painted_box(3.5, 40., 6., ALL_SIDE_FACE, pdmWalls, /*walls=*/3, /*paint_depth_mm=*/0.,
                                             /*layer_height=*/0.1, print, /*interlocking_depth=*/-1.,
                                             PerimeterGeneratorType::Arachne, /*gap_infill_speed=*/0.0);
    REQUIRE(object->layer_count() >= 40);

    const size_t even_layer = (object->layer_count() / 2) - (object->layer_count() / 2) % 2;
    const size_t odd_layer  = even_layer + 1;
    REQUIRE(even_layer % 2 == 0);
    REQUIRE(odd_layer % 2 == 1);

    const BoundingBox bb = get_extents(object->get_layer(int(even_layer))->lslices);
    REQUIRE(bb.defined);
    const Point centre_probe((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);

    // RED pre-fix (measured, absorb-tail-fixwave2-review.md I-D): one of the two parities
    // absorbs the centre while the other keeps it base - churn. GREEN post-fix: the keep-core
    // exemption now uses the SAME widened threshold the outer test already computed for this
    // island on BOTH parities, so the centre stays base on both, with no alternation.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, even_layer), centre_probe));
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, odd_layer), centre_probe));
}

// ITEM 2 (loose end 3, .superpowers/sdd/2026-08-31-paint-depth/interclaim-sliver-
// investigation.md section 2 "Config sensitivity" / interclaim-absorb-report.md "still open" /
// shell-setting-and-gapfill-report.md): with gap_infill_speed == 0, layer_color_stat's
// small_region_threshold (MultiMaterialSegmentation.cpp) takes its "gap fill disabled" arm -
// ext_perimeter_width + 0.7 * that region's extrusion spacing - instead of half the width,
// which widens the #7104 thin-projection filter's kill width from 0.225mm to ~0.75mm at stock
// flows. That is PAST the absorb's own (unfixed) kill width, min_claim_width = 0.45mm, so an
// inter-claim sliver up to ~0.75mm wide survives the absorb and prints body-coloured - on the
// SAME curved sphere fixture Item 1 already fixed for the gap-fill-ON case. This is item 2's
// regression pin: it must FAIL on HEAD before the fix (the absorb's threshold does not yet
// track the wider gap-off population) and pass once it does.
TEST_CASE("multi_material_segmentation_by_painting: gap_infill_speed=0 no longer leaves a base-coloured sliver at a curved colour boundary (Item 2)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print, /*gap_infill_speed=*/0.0);

    // The WIDER kill width gap_infill_speed=0 actually produces, computed from the object's
    // own real per-region flow via the EXACT SAME formula layer_color_stat's "gap fill
    // disabled" arm uses (MultiMaterialSegmentation.cpp) - tracks the production formula
    // instead of hardcoding a value that could silently drift from it.
    float ext_perimeter_width = 0.f;
    for (size_t region_idx = 0; region_idx < object->num_printing_regions(); ++region_idx) {
        const PrintRegion &region = object->printing_region(region_idx);
        ext_perimeter_width = std::max(ext_perimeter_width,
            region.flow(*object, frExternalPerimeter, object->config().layer_height).width());
    }
    REQUIRE(ext_perimeter_width > 0.f);
    const float kill_width_mm = ext_perimeter_width +
        0.7f * Flow::rounded_rectangle_extrusion_spacing(ext_perimeter_width, float(object->config().layer_height.value));
    CAPTURE(kill_width_mm);

    CHECK_FALSE(has_interclaim_sliver(*object, /*wall_stack_mm=*/0.878540, double(kill_width_mm)));
}

TEST_CASE("multi_material_segmentation_by_painting: gap_infill_speed=0 leaves the gap-fill-on sphere fixture's own result unchanged (Item 2)", "[paintdepth]")
{
    // Same fixture and probe as the Item 1 sphere test above, sliced with the registry's own
    // (nonzero) gap_infill_speed default - i.e. gap fill ON, today's behavior, explicitly
    // pinned unaffected by item 2's fix (which is conditional on gap fill being OFF for at
    // least one region; an object with gap fill on everywhere must see byte-for-byte the same
    // absorb threshold as before this fix).
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print);

    CHECK_FALSE(has_interclaim_sliver(*object));
}

TEST_CASE("multi_material_segmentation_by_painting: a genuine base region wider than the gap-fill-off threshold between two painted claims stays base (Item 2 does not over-absorb)", "[paintdepth]")
{
    // Same OPPOSITE-walls fixture as the Item 1 "does not over-absorb" test above, but with
    // gap_infill_speed=0 so item 2's WIDER (gap-fill-disabled) kill width is the one actually
    // active for this object. The genuine base region here is many mm wide - nowhere close to
    // either the 0.45mm (gap-on) or ~0.75mm (gap-off) kill width - so if item 2's fix ever
    // widened the absorb's threshold UNCONDITIONALLY rather than tracking the real gap-fill-off
    // quantity, or widened it too far, this is exactly the kind of region it would put at risk.
    // It must stay base.
    Print        print;
    PrintObject *object = slice_bounded_frustum_two_colours(40.392, 18., 3., FRUSTUM_WALL_NEG_Y, FRUSTUM_WALL_POS_Y,
                                                             pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print,
                                                             PerimeterGeneratorType::Arachne, /*gap_infill_speed=*/0.0);
    REQUIRE(object->layer_count() >= 27);

    const size_t       probe_layer = 12;
    const BoundingBox bb = get_extents(object->get_layer(int(probe_layer))->lslices);
    REQUIRE(bb.defined);
    // Dead centre of the +X side, far (multiple mm) from both painted walls and from the
    // frustum's own contour - unambiguously genuine base at any plausible claim width.
    const Point centre_probe(coord_t(bb.max.x() - scale_(1.0)), (bb.min.y() + bb.max.y()) / 2);

    CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder2*/ 2), centre_probe));
    CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder3*/ 3), centre_probe));
}

// Fix-wave 2 (absorb-tail-fixwave2-review.md I-C): the absorb's gap-fill-off widening was
// resolved from claim_width_gapfill_off_by_color, a SINGLE OBJECT-WIDE MAX over every printing
// region of a colour (multi_material_segmentation_by_painting's own per-region loop, `for
// (region_idx : num_printing_regions())`) regardless of which LAYER that region actually has
// geometry on - so a PARAMETER_MODIFIER confined to a small Z range with gap_infill_speed=0
// widened the absorb's kill width for EVERY layer of that colour, including layers nowhere near
// the modifier where every region actually PRESENT on that layer has gap fill ON. Reproduces the
// review's own probe E: a 3.47 x 40 x 10mm box, all four sides painted Extruder2 (keep-core =
// width - 2*band = 3.47 - 2*1.435675 = 0.59865mm at walls=3, stock flows, interlock 0 - inside
// the M3 thin-residue window: a genuine "printable core" under the default gap-fill-ON 0.225mm
// opening, but NOT under the widened 0.375mm one), plus a PARAMETER_MODIFIER slab spanning the
// whole footprint (oversized in X/Y, so it fully covers both the base and painted claims
// wherever it overlaps) at z in [9,10] with gap_infill_speed=0 - touching nothing at the low
// probe layer (z ~ 2.5mm), ten times its own thickness away.
TEST_CASE("multi_material_segmentation_by_painting: the interior inter-claim absorb's gap-fill-off widening is resolved per layer from the regions actually bordering it, not object-wide (Fix-wave 2 I-C)", "[paintdepth]")
{
    Model        model;
    ModelObject *object = model.add_object();
    object->name         = "paint-depth-gapfill-modifier.stl";
    ModelVolume *volume  = object->add_volume(make_cube(3.47, 40., 10.));

    TriangleSelector selector(volume->mesh());
    for (int facet_idx : ALL_SIDE_FACE)
        selector.set_facet(facet_idx, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));

    // Oversized (20 x 60 x 1mm) and centred over the base object's own X/Y footprint, so it
    // fully spans whatever region (base or painted) it overlaps in Z - confined to z in [9,10].
    ModelVolume *modifier = object->add_volume(make_cube(20., 60., 1.), ModelVolumeType::PARAMETER_MODIFIER);
    modifier->translate(-8.265, -10., 9.);
    modifier->config.set_key_value("gap_infill_speed", new ConfigOptionFloat(0.0));

    object->add_instance();
    object->ensure_on_bed();

    DynamicPrintConfig config = paint_depth_test_config(pdmWalls, /*walls=*/3);
    config.option<ConfigOptionFloat>("layer_height")->value                     = 0.1;
    config.option<ConfigOptionFloat>("initial_layer_print_height")->value       = 0.1;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionFloat>("mmu_segmented_region_interlocking_depth")->value = 0.;

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);

    // z ~ 2.5-2.6mm at 0.1mm layers - well clear of the modifier (z 9-10) and of any top/bottom
    // shell effect near either cap.
    const size_t probe_layer = 25;
    REQUIRE(out_object->layer_count() > probe_layer);
    const BoundingBox bb = get_extents(out_object->get_layer(int(probe_layer))->lslices);
    REQUIRE(bb.defined);
    const Point centre_probe((bb.min.x() + bb.max.x()) / 2, (bb.min.y() + bb.max.y()) / 2);

    // RED pre-fix (probe E, absorb-tail-fixwave2-review.md I-C): the modifier's gap_infill_
    // speed=0 at z 9-10 widens the OBJECT-WIDE kill width for every layer of Extruder2,
    // including this one far away, so the 0.599mm keep-core here gets wrongly absorbed. GREEN
    // post-fix: this layer's own resolved gap-fill-off width sees no gap-fill-off region
    // bordering it (every region actually present here has gap fill ON), so the centre stays
    // base, matching probe E0 (gap fill on everywhere) exactly.
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, probe_layer), centre_probe));

    // The OTHER direction (I-C: "test the modifier-at-different-z case both ways") CANNOT be
    // demonstrated on THIS SAME probe: a symmetric, all-sides-painted wall's centre residue is
    // ALWAYS keep_core (cut_segmented_layers's own preserved "printable core" - see Minor 3's
    // fixture above, same construction) and I-D's own fix (t_keep_core now tracking the SAME
    // widened threshold this island's outer test uses) means it is CORRECTLY exempted from the
    // absorb regardless of gap-fill state, at EVERY z including inside the modifier - proven by
    // hand-walk, not merely assumed: at z ~ 9.5mm the widened t = 0.375mm (kill 0.75mm) makes
    // BOTH the outer test AND the keep-core re-test agree the 0.599mm residue has no printable
    // core, so the SAME exemption that protects it at z ~ 2.6mm protects it here too - by
    // design, a genuine keep-core component must never depend on gap-fill state. Measured
    // directly (probe_layer_in_modifier below): this residue stays base at z ~ 9.5mm exactly
    // like at z ~ 2.6mm, for a DIFFERENT reason (I-D's keep-core protection, not I-C's per-layer
    // narrowing) - confirming the two fixes compose correctly rather than fighting each other.
    // The "does widen a GENUINE (non-keep-core) sliver when gap fill is actually off" direction
    // is exactly what the existing "Item 2" gap_infill_speed=0 sphere test above already proves
    // (a curved two-colour boundary has no keep_core component at all to protect it) - this
    // fix-wave's own per-layer narrowing reuses that same code path unchanged (segmented_
    // regions[layer_idx][color_idx] there is non-empty at every layer near the boundary, so the
    // per-layer signal degrades to the same object-wide answer the pre-existing test already
    // exercises).
    const size_t probe_layer_in_modifier = first_layer_above_z(*out_object, 9.4);
    REQUIRE(out_object->layer_count() > probe_layer_in_modifier);
    const BoundingBox bb_in_modifier = get_extents(out_object->get_layer(int(probe_layer_in_modifier))->lslices);
    REQUIRE(bb_in_modifier.defined);
    const Point centre_probe_in_modifier((bb_in_modifier.min.x() + bb_in_modifier.max.x()) / 2,
                                          (bb_in_modifier.min.y() + bb_in_modifier.max.y()) / 2);
    CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, probe_layer_in_modifier), centre_probe_in_modifier));
}

// Fix-wave 3 (absorb-tail-fixwave2-review.md I-1): the fix-wave 2 test just above is NON-
// DISCRIMINATING - its z ~ 2.6mm probe IS the keep-core component cut_segmented_layers already
// protects (review probe E: base width 0.5987mm = 3.47 - 2*1.435675), so I-D's own
// t_keep_core = t fix alone turns it green regardless of whether the I-C per-layer narrowing
// above is correct or not; deleting the narrowing entirely would still leave that test green.
// This fixture instead reuses slice_bounded_sphere_two_colours - the SAME curved two-colour
// geometry the "Item 1"/"Important 1" sliver-free tests above already use, which has NO
// keep-core component anywhere (a sphere has no interlocking notch to protect) - plus a
// gap-fill-off PARAMETER_MODIFIER confined to print z [2,3], geometrically FAR from the
// colour2/colour3 cap boundary near the sphere's own apex (layers 149/150 at this radius/layer
// height, per the review's own probe C).
//
// Mechanism (see compute_layer_color_stat's own I-1 comment for the fix). The modifier gives
// every painted colour a SECOND PrintRegion variant (gap_infill_speed=0) that PrintObjectSlice.
// cpp:5199-5208 hands a LayerRegion on EVERY layer regardless of Z. Before this fix,
// layer_color_stat's per-colour block resolved small_region_threshold by iterating
// layer.regions() and overwriting it UNCONDITIONALLY on every match ("last-region-wins"),
// un-gated on slices - so whichever variant a colour's PrintRegion happened to be created LAST
// (the gap-off one, PrintApply.cpp:1082-1123's own creation order) won on EVERY layer, not only
// the ones the modifier's own geometry covers. The top/bottom descent's own erosion
// (segmentation_top_and_bottom_layers) then eroded EVERY layer's claim at the WIDE (gap-off,
// ~0.75mm at stock flows) width - including layers 149/150 at the sphere's own cap boundary,
// more than ten mm away from the z 2-3 modifier - manufacturing a genuine (if, pre-fix-wave-2,
// still absorbed) base sliver population there. Fix-wave 2's own I-C change correctly narrowed
// the ABSORB's kill width to per-layer (so it no longer widens on layers 149/150, which have no
// gap-fill-off region of their own) - but left the GENERATOR's over-erosion bug in place, so the
// two now disagree: the generator manufactures a wider sliver than the (correctly) narrower
// absorb is willing to catch. RED on HEAD before this fix-wave, measured directly on THIS
// fixture (not merely cited from the review): 8.123mm2 at layer 149 / 6.002mm2 at layer 150 -
// matching the review's own probe C (8.14 / 6.01mm2) closely, see the fix-wave 3 report for the
// full RED run. GREEN once the generator itself resolves the applicable variant per layer (this
// fix-wave), because the erosion at layers 149/150 no longer widens in the first place - there
// is no sliver left for the absorb to catch or miss (measured: zero sliver components found on
// this fixture post-fix).
TEST_CASE("multi_material_segmentation_by_painting: a gap-fill-off PARAMETER_MODIFIER confined to one Z range does not widen the descent's own erosion on unrelated layers, leaving no inter-claim sliver at the cap boundary (Fix-wave 3 I-1)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print,
                                                             /*gap_infill_speed=*/-1.0,
                                                             /*gapfill_off_modifier_z_min=*/2.0,
                                                             /*gapfill_off_modifier_z_max=*/3.0);

    // kill_width_mm = 0.75mm: the production gap-fill-off formula at stock flows
    // (ext_perimeter_width + 0.7*spacing) - the SAME width the descent's own erosion wrongly
    // used on every layer, pre-fix, once ANY gap-fill-off region existed anywhere on the object.
    // wall_stack_mm is the default (unchanged from every other has_interclaim_sliver call here).
    CHECK_FALSE(has_interclaim_sliver(*object, /*wall_stack_mm=*/0.878540, /*kill_width_mm=*/0.75));
}

// ===========================================================================================
// FLAT-TOP CAP (user decision 2026-09-01, .superpowers/sdd/2026-08-31-paint-depth/
// flat-top-cap-report.md): on a FLAT painted top the normal-thickness shell (Wave B / Option N)
// claims D deep (15 layers at stock defaults / 0.1mm layers, walls = 3), but only the solid top
// shell (6 layers at stock defaults: top_shell_layers = 4, top_shell_thickness = 0.6) is ever
// VISIBLE - the rest is hidden sparse infill of the SAME colour either way
// (paint_infill_override). Capping the flat portion of the claim at the shell depth removes 9
// tool changes and ~2.5cm3 of purge per painted flat cap at stock defaults, with NO visible
// change. SLOPES AND WALLS KEEP THE FULL D BOUND - every Wave B test above (T1/T2/T3, the
// break-placement pin, the normal-thickness-across-slopes table, the D >= wall_stack gate, the
// base-filament-not-deepened pin, Important 1/2) is untouched by this section and must stay
// green, unmodified.
// ===========================================================================================

TEST_CASE("multi_material_segmentation_by_painting: a flat painted top is capped at the solid-shell depth, not the full normal thickness (flat-top cap)", "[paintdepth]")
{
    // slice_bounded_frustum(bottom, top, ...) with bottom == top degenerates the frustum to a
    // plain vertical-walled prism (see make_square_frustum's own facet-table comment) - reused
    // rather than adding a new fixture, and it already pins top_shell_layers = 4 /
    // top_shell_thickness = 0.6 (a 6-layer effective shell at 0.1mm layers) and walls = 3
    // (D = 1.435675mm, M = ceil(D/0.1) = 15).
    Print        print;
    PrintObject *object = slice_bounded_frustum(40., 40., 4., TOP_CAP_FACE,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 16);

    const Point probe = slab_center_point(*object);

    // Surface facet + shell depths 1-5 (6 layers total, matching the effective 6-layer solid
    // shell) stay claimed exactly as Wave B already delivered - the cap never touches anything
    // within the shell.
    for (size_t depth = 0; depth <= 5; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe));
    }
    // RED on HEAD: without the cap a flat top's claim continues to the full D-driven depth (15
    // layers total), so depths 6-14 are STILL claimed today. A 40x40mm cap is far wider than the
    // ~0.88mm/layer erosion accumulates by depth 14 (~12.3mm from each edge, leaving >15mm still
    // solid at the centre), so this is not a fixture artefact - the centre really is claimed
    // pre-fix. The cap must stop it dead at the shell boundary instead.
    for (size_t depth = 6; depth <= 14; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe));
    }
}

// Pins the exact MEASURED numbers from wave-b-report.md section 1 (also reproduced, with a
// looser [1.36, 1.58] window, by "normal thickness across slopes" above) - this cap must leave
// every sloped-origin descent byte-identical, so these figures must not move by more than
// claim_reach_mm()'s own 0.05mm scan step (scaled by sin(theta)).
TEST_CASE("multi_material_segmentation_by_painting: the flat-top cap leaves the measured 10/15/20 degree normal thickness untouched (regression pin)", "[paintdepth]")
{
    constexpr double kPi = 3.14159265358979323846;
    struct SlopeCase { double degrees; double bottom; double normal_mm_expected; };
    // Same four-frustum family as "normal thickness across slopes" above (18mm top over 3mm),
    // walls = 3, 0.1mm layers, probe layer 12. Expected values from wave-b-report.md section 1's
    // "measured now" column (M = 15, so the realised thickness is M*h*cos(theta), the design's
    // D*cos(theta) rounded UP by the one-layer quantisation of M, never down).
    const SlopeCase cases[] = {
        {10., 52.0276, 1.476},
        {15., 40.3920, 1.436},
        {20., 34.4848, 1.402},
    };

    for (const SlopeCase &c : cases) {
        DYNAMIC_SECTION("slope " << c.degrees << " deg") {
            Print        print;
            PrintObject *object = slice_bounded_frustum(c.bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                         pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
            REQUIRE(object->layer_count() >= 27);

            const double theta     = c.degrees * kPi / 180.;
            const double reach     = claim_reach_mm(*object, /*layer_idx=*/12);
            const double normal_mm = reach * std::sin(theta);
            CAPTURE(c.degrees);
            CAPTURE(reach);
            CAPTURE(normal_mm);

            CHECK_THAT(normal_mm, Catch::Matchers::WithinAbs(c.normal_mm_expected, 0.03));
        }
    }
}

TEST_CASE("multi_material_segmentation_by_painting: a flat crown with sloped flanks is capped at the crown, not at the flanks (dome/frustum mix)", "[paintdepth]")
{
    // Same 15-degree frustum as T1 (40.392 -> 18mm top over 3mm, 30 layers at 0.1mm), but paint
    // BOTH the flat top cap (the "crown") AND the sloped walls (the "flanks") together - a
    // dome's flat-crown/sloped-flank shape without needing a new mesh helper.
    std::vector<int> crown_and_flanks = TOP_CAP_FACE;
    crown_and_flanks.insert(crown_and_flanks.end(), FRUSTUM_SLOPED_WALLS.begin(), FRUSTUM_SLOPED_WALLS.end());

    Print        print;
    PrintObject *object = slice_bounded_frustum(40.392, 18., 3., crown_and_flanks,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 27);
    const size_t top_index = object->layer_count() - 1;

    // The CROWN. The flat cap's own descent, even uncapped, reaches at most M = 15 layers from
    // its own origin (layer top_index = 29 on this 30-layer fixture) - down to layer 15, short
    // of probe layer 12 by 3 layers regardless of this change. The object's XY centre is
    // likewise unreachable by the FLANKS at any depth (their own deepest reach is ~5.6mm from
    // the CONTOUR, per T1, nowhere near the centre of an 18x18mm cap) - so probing the centre at
    // the crown's own depths isolates its behaviour cleanly from the flanks. Same numbers as the
    // plain flat-cap test above: depths 0-5 claimed, 6-14 capped away.
    const Point centre = slab_center_point(*object);
    for (size_t depth = 0; depth <= 5; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, top_index - depth), centre));
    }
    for (size_t depth = 6; depth <= 14; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, top_index - depth), centre));
    }

    // The FLANKS. Layer 12's claim is built entirely from nearby sloped-wall origins (layers
    // 12..26, per T1's own comment) - the crown at layer 29 is too deep to reach it (as above),
    // so this is an untouched reproduction of T1 itself, now with the crown painted too. Must be
    // byte-identical to T1: full D depth, not the lateral band alone.
    const ExPolygons claim = extruder2_claim_for_layer(*object, 12);
    CHECK(any_contains(claim, layer_edge_probe(*object, 12, 3.0)));
    CHECK(any_contains(claim, layer_edge_probe(*object, 12, 5.0)));
    CHECK_FALSE(any_contains(claim, layer_edge_probe(*object, 12, 6.0)));
}

TEST_CASE("multi_material_segmentation_by_painting: a flat painted bottom is capped at the solid-shell depth (flat-top cap, bottom mirror)", "[paintdepth]")
{
    // Mirror of the flat-cap test above. slice_bounded_frustum() pins bottom_shell_layers = 3 /
    // bottom_shell_thickness = 0.0 (thickness disabled, so the effective bottom shell is the
    // plain layer count, 3 - a different number from the top's 6, which is fine: the point being
    // pinned is "capped at the shell depth, not the D-driven depth", and 3 vs 15 demonstrates
    // that exactly as clearly as 6 vs 15 does).
    Print        print;
    PrintObject *object = slice_bounded_frustum(40., 40., 4., BOTTOM_CAP_FACE,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    REQUIRE(object->layer_count() >= 16);

    const Point probe = slab_center_point(*object);

    for (size_t depth = 0; depth <= 2; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, depth), probe));
    }
    // RED on HEAD: depths 3-14 are still claimed today (the D-driven 15-layer descent, unbounded
    // by the effective 3-layer bottom shell); capped, they must not be.
    for (size_t depth = 3; depth <= 14; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, depth), probe));
    }
}

TEST_CASE("multi_material_segmentation_by_painting: the interior absorb does not annex a flat cap's capped floor into a neighbouring painted colour", "[paintdepth]")
{
    // Same construction as the "Important 2" cross-colour-clip test above: TOP_CAP_FACE painted
    // Extruder2 (colour A, the flat cap), the full-height +X wall painted Extruder3 (colour B,
    // right up to the shared top edge) - a genuinely ACTIVE painted neighbour at every layer,
    // which is exactly the precondition the interior inter-claim absorb needs to have a "winner"
    // candidate at all.
    //
    // Flat-top cap fix wave (Minor 4): looped over gap_infill_speed default (gap fill ON) and 0
    // (OFF, the WIDENED kill width) - cheap insurance per the review, since the fix means no
    // capped gap is ever sliver-sized any more (either the whole rim is capped WITH the floor, or
    // the whole component stays at D), so the widened kill width has nothing new to misjudge
    // either.
    const double gap_fill_cases[] = {-1.0 /*default, gap fill ON*/, 0.0 /*OFF, widened kill width*/};
    for (double gap_infill_speed : gap_fill_cases) {
        DYNAMIC_SECTION("gap_infill_speed=" << gap_infill_speed) {
            Print        print;
            PrintObject *object = slice_two_painted_colours(/*x=*/40., /*y=*/40., /*z=*/6.,
                                                              TOP_CAP_FACE, PLUS_X_FACE,
                                                              pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print,
                                                              gap_infill_speed);
            const size_t top_index = object->layer_count() - 1;
            REQUIRE(top_index >= 20);

            // Depth 8: past colour A's capped floor (shell depth 6) and far past colour B's own
            // lateral reach (~1.44mm from the +X wall) - the box centre is 20mm from every edge.
            // Pre-fix, colour A's (uncapped) claim reaches the centre here (RED, same mechanism as
            // the plain flat-cap test above). The genuine risk this pins is the SECOND check: if
            // the now-empty capped floor were ever mistaken for an absorbable inter-claim sliver,
            // colour B - the only active painted neighbour at this layer - is exactly who it would
            // be annexed into. It must stay base instead.
            const size_t probe_layer = top_index - 8;
            const Point  centre      = slab_center_point(*object);
            CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder2, colour A, the capped flat top*/ 2), centre));
            CHECK_FALSE(any_contains(claim_for_layer(*object, probe_layer, /*Extruder3, colour B, the side stripe*/ 3), centre));
        }
    }
}

// Cost evidence (the whole point of this change - spec's "Cost note",
// docs/superpowers/specs/2026-08-31-paint-depth-design.md, and wave-b-report.md section 6):
// counts how many layers of the SAME flat-cap fixture as the RED test above actually carry
// painted material - each one costs one tool change and its purge (~280mm3 stock flush default,
// PrintConfig.cpp) on a real multi-material printer. Run against the pre-fix binary and it
// reports 15 (the D-driven depth); post-fix it must report 6 (the effective solid-shell depth) -
// 9 fewer tool changes / painted layers per painted flat cap, ~2.5cm3 of purge recovered, with NO
// visible change (everything past the solid shell was hidden sparse infill either way).
TEST_CASE("multi_material_segmentation_by_painting: cost evidence - painted-layer count on the flat-cap fixture drops from the D-driven depth to the shell depth", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_bounded_frustum(40., 40., 4., TOP_CAP_FACE,
                                                 pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print);
    const size_t top_index = object->layer_count() - 1;
    REQUIRE(top_index >= 16);

    const Point probe          = slab_center_point(*object);
    size_t      painted_layers = 0;
    for (size_t depth = 0; depth <= 20 && depth <= top_index; ++depth) {
        if (! any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe))
            break; // contiguous from the surface on this fixture (a wide flat cap).
        ++painted_layers;
    }

    INFO("painted layers on the flat-cap fixture (== tool changes saved versus the pre-cap "
         "D-driven depth of 15): " << painted_layers);
    CHECK(painted_layers == 6);
}

// ===========================================================================================
// FLAT-TOP CAP FIX WAVE (user decision 2026-09-01, .superpowers/sdd/2026-08-31-paint-depth/
// flat-top-cap-review.md I1/I2, folding in Minors 1/3/4): the cap above classified a flat top or
// bottom PER POINT, using exposed_surface_part()'s wall-stack yardstick measured against the
// NEXT layer's own contour - which is not the patch's local slope. Two consequences, one fix
// (flat_cap_component_ex(), MultiMaterialSegmentation.cpp): a flat top/bottom beside a taller
// riser kept a wall-stack-wide painted ring at the full D depth forever (I1, below); a slope only
// a little above the classic ~6.49deg cliff got PARTIALLY capped into a striped bullseye that the
// absorb then silently undid, or not, depending on gap_infill_speed (I2, further below).
// ===========================================================================================

TEST_CASE("multi_material_segmentation_by_painting: a flat painted top beside a riser is capped WITH the rest of the flat component, rim included (I1, ledge)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_ledge(/*top_face=*/true, print);

    // The slab's OWN top layer (z=4, the boundary with the tower) - first_layer_above_z(4.0)
    // lands on the tower's FIRST layer (z in [4.0,4.1], 20x20 footprint); the slab's own last
    // layer (z in [3.9,4.0], full 40x40 footprint) is one below that, and is the ledge's own
    // origin.
    const size_t surface = first_layer_above_z(*object, 4.0) - 1;
    REQUIRE(surface >= 16);

    // mid-ledge: 15mm out from centre - 5mm clear of the 10mm tower edge and 5mm clear of the
    // 20mm slab edge, comfortably inside the ledge either way.
    const Point mid_ledge  = ledge_offset_probe(*object, surface, 15.0);
    // near-riser: 10.4mm out from centre - 0.4mm past the tower's own 10mm edge, deep inside the
    // ~0.8785mm wall-stack band the pointwise yardstick used to exclude (review's own measured
    // [10.0, 10.85] window).
    const Point near_riser = ledge_offset_probe(*object, surface, 10.4);

    // Surface facet + shell depths 1-5 (6 layers total, matching the effective 6-layer solid
    // shell) stay claimed everywhere on the ledge, exactly as the plain flat-cap test already
    // pins for a top with no riser at all.
    for (size_t depth = 0; depth <= 5; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, surface - depth), mid_ledge));
        CHECK(any_contains(extruder2_claim_for_layer(*object, surface - depth), near_riser));
    }
    // RED on HEAD: mid_ledge (already more than one wall stack from the riser) was already
    // correctly capped, but near_riser - within one wall stack of the tower's own contour - kept
    // a painted ring all the way to the old D-driven depth (15 layers total): depths 6-14 were
    // STILL claimed there, saving zero tool changes on exactly the geometry the cap exists for.
    // Per-component classification must cap the WHOLE ledge, rim included.
    for (size_t depth = 6; depth <= 14; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, surface - depth), mid_ledge));
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, surface - depth), near_riser));
    }
}

TEST_CASE("multi_material_segmentation_by_painting: a flat painted bottom beside a riser is capped WITH the rest of the flat component, rim included (I1, ledge, bottom mirror)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_ledge(/*top_face=*/false, print);

    // The shelf's OWN bottom layer (z in [4.0,4.1], the boundary with the stem, full 40x40
    // footprint) - the ledge's own origin for a bottom claim.
    const size_t surface = first_layer_above_z(*object, 4.0);
    REQUIRE(object->layer_count() - surface >= 16);

    const Point mid_ledge  = ledge_offset_probe(*object, surface, 15.0);
    const Point near_riser = ledge_offset_probe(*object, surface, 10.4);

    // Effective bottom shell = 3 (bottom_shell_thickness disabled), same as every other bottom-
    // mirror test in this file - depths 0-2 claimed everywhere on the ledge.
    for (size_t depth = 0; depth <= 2; ++depth) {
        CAPTURE(depth);
        CHECK(any_contains(extruder2_claim_for_layer(*object, surface + depth), mid_ledge));
        CHECK(any_contains(extruder2_claim_for_layer(*object, surface + depth), near_riser));
    }
    // RED on HEAD: same bug, mirrored - near_riser stayed claimed to the D-driven depth (15
    // layers) while mid_ledge was already correctly capped.
    for (size_t depth = 3; depth <= 14; ++depth) {
        CAPTURE(depth);
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, surface + depth), mid_ledge));
        CHECK_FALSE(any_contains(extruder2_claim_for_layer(*object, surface + depth), near_riser));
    }
}

// Minor 3: the existing cost-evidence test above counts painted layers via a CENTRE probe, which
// on this ledge fixture would report 6 (the centre sits mid-ledge, already correctly capped even
// pre-fix) while the near-riser rim above proves 15 layers actually carried paint - the proxy
// passes precisely where the saving used to be lost. Count over the WHOLE cap footprint instead:
// a layer counts as painted if its claim is non-empty ANYWHERE, not merely at one point.
TEST_CASE("multi_material_segmentation_by_painting: cost evidence on the ledge fixture - painted layers are counted over the WHOLE cap footprint, not a centre probe (Minor 3)", "[paintdepth]")
{
    Print        print;
    PrintObject *object = slice_capped_ledge(/*top_face=*/true, print);
    const size_t surface = first_layer_above_z(*object, 4.0) - 1;
    REQUIRE(surface >= 20);

    size_t painted_layers = 0;
    for (size_t depth = 0; depth <= 20 && depth <= surface; ++depth) {
        if (extruder2_claim_for_layer(*object, surface - depth).empty())
            break; // contiguous from the surface on this fixture (a wide flat ledge).
        ++painted_layers;
    }

    INFO("painted layers over the WHOLE ledge footprint, incl. the rim beside the riser (== tool "
         "changes saved versus the pre-fix D-driven depth of 15): " << painted_layers);
    CHECK(painted_layers == 6);
}

// Important 2: a slope whose per-layer staircase run r is only a little above one wall stack used
// to get the INNER wall-stack band capped and the OUTER remainder kept (the wrong way round),
// producing alternating capped/uncapped rings that the absorb then silently re-annexed, or not,
// depending on gap_infill_speed. Per-component classification means every origin along a UNIFORM
// slope makes the SAME whole-ring decision, so there is nothing left to stripe: a component-level
// "flat enough" ring is capped whole, and one that is not stays wholly at D - reproducing this
// slope's PRE-CAP-FEATURE descent exactly. Verified directly against a same-depth, cap-disabled
// reference build (slice_bounded_frustum's top_shell_layers_override) rather than a hand-derived
// reach formula, so there is no magic number to get subtly wrong near the object's own edges.
TEST_CASE("multi_material_segmentation_by_painting: near-flat slopes (3/4/5 deg) get one whole-component decision, never per-layer stripes (I2)", "[paintdepth]")
{
    constexpr double kPi = 3.14159265358979323846;
    const double      degrees_cases[]  = {3., 4., 5.};
    const double      gap_fill_cases[] = {-1.0 /*default, gap fill ON*/, 0.0 /*OFF, widened kill width*/};

    for (double gap_infill_speed : gap_fill_cases) {
        for (double degrees : degrees_cases) {
            DYNAMIC_SECTION(degrees << " deg, gap_infill_speed=" << gap_infill_speed) {
                // Same "18mm top over 3mm" family as the 10/15/20deg pin above: tan(theta) =
                // height / half_diff, so half_diff = height / tan(theta) and bottom = top +
                // 2*half_diff.
                const double theta  = degrees * kPi / 180.;
                const double bottom = 18. + 2. * 3. / std::tan(theta);

                Print        print_capped;
                PrintObject *with_cap = slice_bounded_frustum(bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                               pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_capped,
                                                               PerimeterGeneratorType::Arachne, gap_infill_speed);
                REQUIRE(with_cap->layer_count() >= 27);

                Print        print_reference;
                PrintObject *cap_disabled = slice_bounded_frustum(bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                                    pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_reference,
                                                                    PerimeterGeneratorType::Arachne, gap_infill_speed,
                                                                    /*top_shell_layers_override=*/15);
                REQUIRE(cap_disabled->layer_count() >= 27);

                const ExPolygons claim_with_cap     = extruder2_claim_for_layer(*with_cap, 12);
                const ExPolygons claim_cap_disabled = extruder2_claim_for_layer(*cap_disabled, 12);
                CAPTURE(degrees);
                CAPTURE(gap_infill_speed);
                CAPTURE(claim_with_cap.size());
                CAPTURE(claim_cap_disabled.size());

                // No disjoint-polygon striping (the I2 bug produced ~10 separate capped/uncapped
                // rings at 3/4deg): the claim keeps the SAME small polygon count as the
                // cap-disabled reference - never inflated by a bullseye of alternating bands.
                CHECK(claim_with_cap.size() == claim_cap_disabled.size());
                CHECK(claim_with_cap.size() <= 2);

                // No base annuli inside the claim, absorb inert either way: the claim reaches
                // EXACTLY as far, contiguously from the contour, as the same-depth cap-disabled
                // reference - a striped claim would break claim_reach_mm's contiguous scan at the
                // first capped gap and fall far short (measured pre-fix: ~11.4/8.55/8.50mm at
                // 3/4/5deg vs a full un-capped reach of 28.62/21.45/17.15mm = 15 * r).
                const double reach_with_cap     = claim_reach_mm(*with_cap, 12, 32.0);
                const double reach_cap_disabled = claim_reach_mm(*cap_disabled, 12, 32.0);
                CAPTURE(reach_with_cap);
                CAPTURE(reach_cap_disabled);
                CHECK_THAT(reach_with_cap, Catch::Matchers::WithinAbs(reach_cap_disabled, 0.0001));
            }
        }
    }
}

// Fix-wave 3 (flat-top-cap-fixwave-review.md Minor 1, folded in by the coordinator while this
// fix-wave was already in flight on the same file): just BELOW the classic cliff (2.17 deg),
// flat_cap_component_ex()'s per-component test used TWO DIFFERENT thresholds for what should be
// one whole-slope decision. A full ring's own origin has a real reference layer, so
// exposed_surface_part() erodes it before the opening test sees it, and it survives (is capped)
// iff its run r > 3*wall_stack_width (ws = 0.878540mm at stock flows -> 2.6356mm). The TOPMOST
// origin has NO reference layer at all (exposed_surface_part()'s own early return hands back the
// whole, un-eroded patch), and that patch is a HALF-ring (width r/2, one slab's worth) - opening
// it at the SAME wall_stack_width the (already-eroded) full-ring case uses only lets it survive
// past r > 4*ws = 3.5142mm, a full wall-stack STRICTER than its neighbours. Between those two
// thresholds (3ws, 4ws] - degrees 1.63-2.17 at 0.1mm layers - every full ring below the surface
// is capped but the apex origin's own half-ring is not, leaving an isolated, still-full-depth
// painted "fin" ring sitting inside an otherwise-capped, printable base annulus: TWO disjoint
// components (the capped surface band, the uncapped apex fin) where a working per-COMPONENT
// classifier should produce one.
//
// 1.5deg (r = 3.819mm) sits ABOVE even the stricter 4ws apex threshold, so both rules already
// agreed pre-fix - a CONTROL, expected to stay a single ring both before and after. 2.0deg
// (r = 2.864mm) sits inside the broken window - full rings capped (r > 3ws), apex not
// (r/2 = 1.432mm < 2ws = 1.757mm) - RED pre-fix.
TEST_CASE("multi_material_segmentation_by_painting: just-below-the-cliff slopes cap the apex origin's own half-ring the same as every full ring below it, leaving no isolated base annulus (cap-fix review Minor 1)", "[paintdepth]")
{
    constexpr double kPi = 3.14159265358979323846;
    struct SlopeCase { double degrees; };
    // Same "18mm top over 3mm" frustum family as the near-flat-slopes (I2) test above.
    const SlopeCase cases[] = { {1.5}, {2.0} };

    for (const SlopeCase &c : cases) {
        DYNAMIC_SECTION(c.degrees << " deg") {
            const double theta  = c.degrees * kPi / 180.;
            const double bottom = 18. + 2. * 3. / std::tan(theta);

            Print        print_capped;
            PrintObject *with_cap = slice_bounded_frustum(bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                           pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_capped);
            REQUIRE(with_cap->layer_count() >= 27);

            // Same-depth cap-disabled reference (top_shell_layers_override=15 forces top_cap_
            // active false while leaving top_descent_layers, hence the raw un-capped claim,
            // unchanged) - the SAME technique the I2/Minor-1 tests above use, so the RED evidence
            // below is a measured area, not a hand-derived magic number.
            Print        print_reference;
            PrintObject *cap_disabled = slice_bounded_frustum(bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                                pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_reference,
                                                                PerimeterGeneratorType::Arachne, /*gap_infill_speed=*/-1.0,
                                                                /*top_shell_layers_override=*/15);
            REQUIRE(cap_disabled->layer_count() == with_cap->layer_count());

            // Layers 15-22 (m = 7..14 descent steps below the object's own top layer 29) - the
            // review's own measured window for this fixture family.
            for (size_t layer_idx = 15; layer_idx <= 22; ++layer_idx) {
                CAPTURE(c.degrees);
                CAPTURE(layer_idx);
                const ExPolygons claim = extruder2_claim_for_layer(*with_cap, layer_idx);
                REQUIRE(! claim.empty());

                // Measured RED evidence (ref-cap symmetric difference against the cap-disabled
                // reference), captured BEFORE the assertions below so a failure shows it: not
                // hard-pinned to an exact figure (an exact-area pin would be a magic number
                // re-derived from scratch rather than compared against a reference build) - a
                // failing run's own CAPTURE output reports the real area, matching the review's
                // own measured 271.8 / 248.5 mm2 at 2.0 / 2.15deg.
                const ExPolygons cap_disabled_claim = extruder2_claim_for_layer(*cap_disabled, layer_idx);
                const ExPolygons ref_minus_cap       = diff_ex(cap_disabled_claim, claim);
                double            ref_minus_cap_area_mm2 = 0.;
                for (const ExPolygon &e : ref_minus_cap)
                    ref_minus_cap_area_mm2 += e.area();
                ref_minus_cap_area_mm2 *= sqr(unscale<double>(1.0));
                CAPTURE(ref_minus_cap_area_mm2);

                // The review's own discriminating signal: a correctly-capped slope has ONE
                // component (component count is the headline "2 polys" vs "1 poly" signal the
                // review measured) at every layer in this window - the apex origin's own
                // half-ring capped exactly like the full rings below it, so there is nothing left
                // to split off into a separate island. RED pre-fix at 2.0deg: TWO components (the
                // capped surface band plus the isolated, still-full-depth apex fin).
                CHECK(claim.size() == 1);
                size_t total_holes = 0;
                for (const ExPolygon &e : claim)
                    total_holes += e.holes.size();
                CAPTURE(total_holes);
                CHECK(total_holes == 1);
            }
        }
    }
}

// Minor 1 (apex half-ring wrongly capped): exposed_surface_part() returns the WHOLE patch when
// the reference layer does not exist, so the TOPMOST origin layer of any painted slope was always
// "flat" by that pointwise test alone - a genuine bug at 10deg (its half-ring, 0.284mm, survives
// the pre-existing small_region_threshold opening and therefore reached flat_cap_component_ex()'s
// own opening/dilate test) though invisible at 15/20deg (the smaller 0.19/0.14mm half-ring is
// erased by that SAME pre-existing opening before this feature ever sees it). The "Slope
// regression pin" test above only samples ONE layer (12), 17 layers below the apex - too deep to
// see it (measured pre-fix: layer 20's reach was 5.10mm instead of the correct 5.39mm).
//
// Rather than hand-deriving the exact reach at every layer near the object's own apex (not a
// closed form worth inlining - it depends on how many origins are actually available there), pin
// the INVARIANT directly: compare this fixture's claim against an otherwise-identical build where
// top_shell_layers_override makes the cap provably INACTIVE while leaving the descent depth
// itself (top_descent_layers, hence the raw claim) UNCHANGED at every origin that can reach any
// of the probed layers - see slice_bounded_frustum()'s own comment. Any difference measured is
// then caused ONLY by the cap actually firing somewhere it should not, at ANY layer across the
// whole reachable descent - apex included.
TEST_CASE("multi_material_segmentation_by_painting: the flat-top cap leaves 10/15/20 degree slopes byte-identical across the WHOLE descent, apex included (Minor 1)", "[paintdepth]")
{
    struct SlopeCase { double degrees; double bottom; };
    const SlopeCase cases[] = {
        {10., 52.0276},
        {15., 40.3920},
        {20., 34.4848},
    };

    for (const SlopeCase &c : cases) {
        DYNAMIC_SECTION("slope " << c.degrees << " deg") {
            Print        print_capped;
            PrintObject *with_cap = slice_bounded_frustum(c.bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                           pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_capped);
            REQUIRE(with_cap->layer_count() >= 27);
            const size_t top_index = with_cap->layer_count() - 1;

            Print        print_reference;
            PrintObject *cap_disabled = slice_bounded_frustum(c.bottom, 18., 3., FRUSTUM_SLOPED_WALLS,
                                                                pdmWalls, /*walls=*/3, /*layer_height=*/0.1, print_reference,
                                                                PerimeterGeneratorType::Arachne, /*gap_infill_speed=*/-1.0,
                                                                /*top_shell_layers_override=*/15);
            REQUIRE(cap_disabled->layer_count() == with_cap->layer_count());

            // The review's own measured window (5.10 vs 5.39mm at layer 20, 9 layers below the
            // apex at layer 29) is layers 15-23; scan a few layers either side of it too, spanning
            // the whole region the apex origin's own descent can reach (M=15 layers below it,
            // i.e. down to layer top_index-15=14).
            for (size_t layer_idx = 14; layer_idx <= 26; ++layer_idx) {
                CAPTURE(c.degrees);
                CAPTURE(layer_idx);
                CAPTURE(top_index);
                const double reach_with_cap     = claim_reach_mm(*with_cap, layer_idx, 12.0);
                const double reach_cap_disabled = claim_reach_mm(*cap_disabled, layer_idx, 12.0);
                CHECK_THAT(reach_with_cap, Catch::Matchers::WithinAbs(reach_cap_disabled, 0.0001));
            }
        }
    }
}
