#include <catch2/catch.hpp>

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

DynamicPrintConfig paint_depth_test_config(PaintDepthMode mode, int walls)
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
    return config;
}

// Builds a 40x40x20mm cube, paints the given facets with Extruder2 (state 2), applies
// paint_depth_test_config, and slices the object. The returned PrintObject's layers
// already carry the fully-applied MM segmentation (see the file-level note above).
PrintObject *slice_painted_cube(const std::vector<int> &painted_facets, PaintDepthMode mode, int walls, Print &print)
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
    print.apply(model, paint_depth_test_config(mode, walls));
    REQUIRE(print.objects().size() == 1);

    PrintObject *out_object = print.objects_mutable().front();
    out_object->slice();
    REQUIRE(out_object->layer_count() > 0);
    return out_object;
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
