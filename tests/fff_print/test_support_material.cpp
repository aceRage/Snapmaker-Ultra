#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>
#include <vector>

#include "libslic3r/BoundingBox.hpp"
#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include "libslic3r/GCodeReader.hpp"
#include "libslic3r/Layer.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"

#include "test_data.hpp" // get access to init_print, etc

using namespace Slic3r::Test;
using namespace Slic3r;

TEST_CASE("SupportMaterial: Three raft layers created", "[SupportMaterial]")
{
	Slic3r::Print print;
	Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
		{ "support_material", 1 },
		{ "raft_layers",      3 }
		});
    REQUIRE(print.objects().front()->support_layers().size() == 3);
}

// v2.5a Task 1 (spec: "residual pin", ToolOrdering.cpp's is_support_overriddable):
// a mode-active object (support_filament_matching == true) must never
// be overriddable by WipingExtrusions, for ANY support role, regardless of the
// scalar support_filament/support_interface_filament configs that would otherwise
// make it overriddable - this is what stops flush_into_support's mark_wiping_
// extrusions from claiming and repainting that object's residual support_fills on a
// toolchange with an arbitrary purge-target color (root cause A of the reported
// "khaki mixed into strictly white/teal support" symptom). Off-mode (manual, the
// default) must stay byte-identical to the pre-v2.5a role-based logic.
//
// This predicate is the SAME one mark_wiping_extrusions' own support branch now
// calls (ToolOrdering.cpp, refactored by this same task instead of re-checking the
// scalar configs inline a second time), so this test doubles as the "mode-active
// support_map/support_intf_map stay empty" guarantee from the task's own spec:
// mark_wiping_extrusions only ever calls set_support_extruder_override/
// set_support_interface_extruder_override - the two functions that populate those
// maps - when is_support_overriddable(erSupportMaterial/erSupportMaterialInterface,
// object) returns true; both false here for a mode-active object means neither can
// ever fire for it, by construction. A full wipe-tower G-code export to observe
// support_map directly is not expressible as a fast unit test (needs a GUI-class
// multi-filament fixture - see spike/verify_chameleon.sh's own notes on that CLI
// limitation), so this predicate-level test is the practical, "where expressible"
// substitute the task's own process called for.
//
// A PrintObject is needed for object.config() to resolve, so this lives in
// fff_print (via init_and_process_print) rather than the lighter libslic3r
// [chameleon] suite - support geometry itself is irrelevant to is_support_
// overriddable (it only reads object.config()), so a plain cube with no support
// material enabled is enough; no actual support needs to be generated.
TEST_CASE("WipingExtrusions::is_support_overriddable: mode-active object is never overriddable (v2.5a residual pin); off-mode unchanged", "[SupportMaterial][chameleon]")
{
    WipingExtrusions we;

    SECTION("mode ACTIVE (support_filament_matching = true): false for every support role, even though the scalar configs below would otherwise make it overriddable")
    {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
            { "flush_into_support",                true },
            { "support_filament",                   0 },
            { "support_interface_filament",         0 },
            { "support_filament_matching",           true },
        });
        const PrintObject &object = *print.objects().front();
        CHECK_FALSE(we.is_support_overriddable(erSupportMaterial, object));
        CHECK_FALSE(we.is_support_overriddable(erSupportMaterialInterface, object));
        CHECK_FALSE(we.is_support_overriddable(erMixed, object));
    }

    SECTION("mode OFF (unchecked, the default): byte-identical to the pre-v2.5a role-based logic - true whenever the scalar filament is 0 (\"Default\")")
    {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
            { "flush_into_support",                true },
            { "support_filament",                   0 },
            { "support_interface_filament",         0 },
            { "support_filament_matching",           false },
        });
        const PrintObject &object = *print.objects().front();
        CHECK(we.is_support_overriddable(erSupportMaterial, object));
        CHECK(we.is_support_overriddable(erSupportMaterialInterface, object));
        CHECK(we.is_support_overriddable(erMixed, object));
    }

    SECTION("mode OFF, non-zero scalar filaments: still false - proves the v2.5a pin didn't loosen the pre-existing scalar gate")
    {
        Slic3r::Print print;
        Slic3r::Test::init_and_process_print({ TestMesh::cube_20x20x20 }, print, {
            { "flush_into_support",                true },
            { "support_filament",                   1 },
            { "support_interface_filament",         1 },
            { "support_filament_matching",           false },
        });
        const PrintObject &object = *print.objects().front();
        CHECK_FALSE(we.is_support_overriddable(erSupportMaterial, object));
        CHECK_FALSE(we.is_support_overriddable(erSupportMaterialInterface, object));
    }
}

SCENARIO("SupportMaterial: support_layers_z and contact_distance", "[SupportMaterial]")
{
    // Box h = 20mm, hole bottom at 5mm, hole height 10mm (top edge at 15mm).
    TriangleMesh mesh = Slic3r::Test::mesh(Slic3r::Test::TestMesh::cube_with_hole);
    mesh.rotate_x(float(M_PI / 2));
//    mesh.write_binary("d:\\temp\\cube_with_hole.stl");

	auto check = [](Slic3r::Print &print, bool &first_support_layer_height_ok, bool &layer_height_minimum_ok, bool &layer_height_maximum_ok, bool &top_spacing_ok)
	{
        ConstSupportLayerPtrsAdaptor support_layers = print.objects().front()->support_layers();

		first_support_layer_height_ok = support_layers.front()->print_z == print.config().initial_layer_print_height.value;

		layer_height_minimum_ok = true;
		layer_height_maximum_ok = true;
		double min_layer_height = print.config().min_layer_height.values.front();
		double max_layer_height = print.config().nozzle_diameter.values.front();
		if (print.config().max_layer_height.values.front() > EPSILON)
			max_layer_height = std::min(max_layer_height, print.config().max_layer_height.values.front());
		for (size_t i = 1; i < support_layers.size(); ++ i) {
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z < min_layer_height - EPSILON)
				layer_height_minimum_ok = false;
			if (support_layers[i]->print_z - support_layers[i - 1]->print_z > max_layer_height + EPSILON)
				layer_height_maximum_ok = false;
		}

#if 0
		double expected_top_spacing = print.default_object_config().layer_height + print.config().nozzle_diameter.get_at(0);
		bool wrong_top_spacing = 0;
        std::vector<coordf_t> top_z { 1.1 };
		for (coordf_t top_z_el : top_z) {
			// find layer index of this top surface.
			size_t layer_id = -1;
			for (size_t i = 0; i < support_z.size(); ++ i) {
				if (abs(support_z[i] - top_z_el) < EPSILON) {
					layer_id = i;
					i = static_cast<int>(support_z.size());
				}
			}

			// check that first support layer above this top surface (or the next one) is spaced with nozzle diameter
			if (abs(support_z[layer_id + 1] - support_z[layer_id] - expected_top_spacing) > EPSILON && 
				abs(support_z[layer_id + 2] - support_z[layer_id] - expected_top_spacing) > EPSILON) {
				wrong_top_spacing = 1;
			}
		}
		d = ! wrong_top_spacing;
#else
		top_spacing_ok = true;
#endif
	};

    GIVEN("A print object having one modelObject") {
        WHEN("First layer height = 0.4") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.4 },
                { "dont_support_bridges", false },
			});
			bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = 0.2 and, first layer height = 0.3") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
        WHEN("Layer height = nozzle_diameter[0]") {
			Slic3r::Print print;
			Slic3r::Test::init_and_process_print({ mesh }, print, {
				{ "support_material",	1 },
				{ "layer_height",		0.2 },
				{ "first_layer_height", 0.3 },
                { "dont_support_bridges", false },
            });
            bool a, b, c, d;
            check(print, a, b, c, d);
            THEN("First layer height is honored")					{ REQUIRE(a == true); }
            THEN("No null or negative support layers")				{ REQUIRE(b == true); }
            THEN("No layers thicker than nozzle diameter")			{ REQUIRE(c == true); }
//            THEN("Layers above top surfaces are spaced correctly")	{ REQUIRE(d == true); }
        }
    }
}

#if 0
// Test 8.
TEST_CASE("SupportMaterial: forced support is generated", "[SupportMaterial]")
{
    // Create a mesh & modelObject.
    TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

    Model model = Model();
    ModelObject *object = model.add_object();
    object->add_volume(mesh);
    model.add_default_instances();
    model.align_instances_to_origin();

    Print print = Print();

    std::vector<coordf_t> contact_z = {1.9};
    std::vector<coordf_t> top_z = {1.1};
    print.default_object_config.support_material_enforce_layers = 100;
    print.default_object_config.support_material = 0;
    print.default_object_config.layer_height = 0.2;
    print.default_object_config.set_deserialize("first_layer_height", "0.3");

    print.add_model_object(model.objects[0]);
    print.objects.front()->_slice();

    SupportMaterial *support = print.objects.front()->_support_material();
    auto support_z = support->support_layers_z(contact_z, top_z, print.default_object_config.layer_height);

    bool check = true;
    for (size_t i = 1; i < support_z.size(); i++) {
        if (support_z[i] - support_z[i - 1] <= 0)
            check = false;
    }

    REQUIRE(check == true);
}

// TODO
bool test_6_checks(Print& print)
{
	bool has_bridge_speed = true;

	// Pre-Processing.
	PrintObject* print_object = print.objects.front();
	print_object->infill();
	SupportMaterial* support_material = print.objects.front()->_support_material();
	support_material->generate(print_object);
	// TODO but not needed in test 6 (make brims and make skirts).

	// Exporting gcode.
	// TODO validation found in Simple.pm


	return has_bridge_speed;
}

// Test 6.
SCENARIO("SupportMaterial: Checking bridge speed", "[SupportMaterial]")
{
    GIVEN("Print object") {
        // Create a mesh & modelObject.
        TriangleMesh mesh = TriangleMesh::make_cube(20, 20, 20);

        Model model = Model();
        ModelObject *object = model.add_object();
        object->add_volume(mesh);
        model.add_default_instances();
        model.align_instances_to_origin();

        Print print = Print();
        print.config.brim_width = 0;
        print.config.skirts = 0;
        print.config.skirts = 0;
        print.default_object_config.support_material = 1;
        print.default_region_config.top_solid_layers = 0; // so that we don't have the internal bridge over infill.
        print.default_region_config.bridge_speed = 99;
        print.config.cooling = 0;
        print.config.set_deserialize("first_layer_speed", "100%");

        WHEN("support_material_contact_distance = 0.2") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0") {
            print.default_object_config.support_material_contact_distance = 0;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is not used.
        }

        WHEN("support_material_contact_distance = 0.2 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0.2;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);
            REQUIRE(check == true); // bridge speed is used.
        }

        WHEN("support_material_contact_distance = 0 & raft_layers = 5") {
            print.default_object_config.support_material_contact_distance = 0;
            print.default_object_config.raft_layers = 5;
            print.add_model_object(model.objects[0]);

            bool check = test_6_checks(print);

            REQUIRE(check == true); // bridge speed is not used.
        }
    }
}

#endif


// ============================================================================================
// Ultra (support groups) - Stage 3 gate, items 2 and 3.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 3 "Gate".
//
// The first tests in this suite that assert support GEOMETRY rather than layer Zs or a predicate:
// they measure the length of the erSupportMaterialInterface extrusions landing under each part of a
// two-part object, and require that a support group changes its own part's interface and leaves the
// other part's exactly where it was.
// ============================================================================================

namespace {

// A leg on the bed plus two cubes floating 10 mm above it, as THREE MODEL_PART volumes of ONE
// object. The cubes have a full flat underside, so normal supports build a large, easily measured
// interface under each of them, and they sit 20 mm apart in X so the two interfaces can be told
// apart by coordinate. The leg is what stops ensure_on_bed() from simply dropping the cubes onto
// the bed; it carries no overrides, so it lands in the default group along with cube A.
void make_floating_two_part_print(Slic3r::Print                                        &print,
                                  Slic3r::Model                                        &model,
                                  const Slic3r::DynamicPrintConfig                     &config,
                                  const std::function<void(ModelObject &)>             &tweak)
{
    ModelObject *object = model.add_object();
    object->name = "floating_two_part";
    object->add_volume(Slic3r::make_cube(2., 2., 10.));                 // the leg, on the bed
    TriangleMesh a = Slic3r::make_cube(20., 20., 10.);
    a.translate(0.f, 0.f, 10.f);
    object->add_volume(a);                                              // part A, floating
    TriangleMesh b = Slic3r::make_cube(20., 20., 10.);
    b.translate(40.f, 0.f, 10.f);
    object->add_volume(b);                                              // part B, floating
    object->add_instance();
    if (tweak)
        tweak(*object);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// The object config every case below starts from. Built fresh each time rather than kept in a
// namespace-scope initializer_list, whose backing array is easy to get wrong.
DynamicPrintConfig group_fixture_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                  "1" },
        { "support_type",                    "normal(auto)" },
        { "support_style",                   "grid" },
        { "support_interface_top_layers",    "2" },
        { "support_interface_bottom_layers", "2" },
        { "support_interface_spacing",       "0.5" },
        { "support_on_build_plate_only",     "0" },
    });
    return config;
}

// The X coordinate half way between the two floating cubes, in the frame the support fills live
// in. Read off the object's own slices rather than assumed, because PrintObject slices through
// trafo_centered() and a test should not have to know where that puts the origin.
double split_x_between_parts(const PrintObject &object)
{
    for (const Layer *layer : object.layers()) {
        if (layer->print_z < 12. || layer->lslices.size() < 2)
            continue;
        std::vector<BoundingBox> bboxes;
        for (const ExPolygon &island : layer->lslices)
            bboxes.emplace_back(get_extents(island));
        std::sort(bboxes.begin(), bboxes.end(),
                  [](const BoundingBox &l, const BoundingBox &r) { return l.min.x() < r.min.x(); });
        return 0.5 * (unscale<double>(bboxes.front().max.x()) + unscale<double>(bboxes.back().min.x()));
    }
    return 0.;
}

void sum_interface_length(const ExtrusionEntityCollection &collection, double split_x,
                          double &left, double &right)
{
    for (const ExtrusionEntity *ee : collection.entities) {
        if (const auto *eec = dynamic_cast<const ExtrusionEntityCollection*>(ee)) {
            sum_interface_length(*eec, split_x, left, right);
            continue;
        }
        if (ee->role() != erSupportMaterialInterface)
            continue;
        (unscale<double>(ee->first_point().x()) < split_x ? left : right) += unscale<double>(ee->length());
    }
}

// Interface extrusion length under part A (left) and part B (right), over every support layer.
std::pair<double, double> interface_lengths(const PrintObject &object)
{
    const double split_x = split_x_between_parts(object);
    double left = 0., right = 0.;
    for (const SupportLayer *support_layer : object.support_layers())
        sum_interface_length(support_layer->support_fills, split_x, left, right);
    return std::make_pair(left, right);
}

} // namespace

TEST_CASE("SupportMaterial: per-group interface", "[SupportMaterial][support_groups]")
{
    // Control: no part carries an override, so PrintObject::support_groups() collapses to a single
    // group and the generator takes exactly today's path.
    double control_a = 0., control_b = 0.;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, group_fixture_config(), nullptr);
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == 1);
        REQUIRE(! object.support_layers().empty());
        std::tie(control_a, control_b) = interface_lengths(object);
        // The fixture has to produce a measurable interface under BOTH parts, or nothing below
        // means anything.
        REQUIRE(control_a > 1.);
        REQUIRE(control_b > 1.);
    }

    // Part B asks for five dense interface layers where the object asks for two sparse ones.
    double grouped_a = 0., grouped_b = 0.;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, group_fixture_config(), [](ModelObject &object) {
            ModelVolume *part_b = object.volumes.back();
            part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
            part_b->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
            part_b->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
        });
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        // The default group (the leg and part A) plus B's group.
        REQUIRE(object.support_groups().size() == 2);
        REQUIRE(object.support_groups()[1].name == "B");
        REQUIRE(object.support_groups()[1].volumes.size() == 1);
        std::tie(grouped_a, grouped_b) = interface_lengths(object);
    }

    // B's interface got denser and deeper: a big, unmistakable increase.
    CHECK(grouped_b > control_b * 1.30);
    // A's interface did not move. The shared pipeline - contacts, bases, columns - ran once and
    // unchanged, and A is still filled with the object's own interface parameters.
    CHECK(std::abs(grouped_a - control_a) <= control_a * 0.01);
}

TEST_CASE("SupportMaterial: per-group interface filament", "[SupportMaterial][support_groups]")
{
    DynamicPrintConfig config = group_fixture_config();
    // A second filament has to exist for a group to be able to pick it. filament_diameter is what
    // ToolOrdering counts physical extruders from, so it has to grow with nozzle_diameter.
    config.set_deserialize_strict({
        { "nozzle_diameter",   "0.4,0.4" },
        { "filament_diameter", "1.75,1.75" },
        { "filament_type",     "PLA;PLA" },
        { "filament_soluble",  "0,0" },
    });

    Slic3r::Print print;
    Slic3r::Model model;
    make_floating_two_part_print(print, model, config, [](ModelObject &object) {
        ModelVolume *part_b = object.volumes.back();
        part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
        part_b->config.set_key_value("support_interface_filament", new ConfigOptionInt(2));
    });
    REQUIRE(! print.objects().empty());
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.support_groups().size() == 2);
    // The predicate both the Chameleon pass and WipingExtrusions stand down on (Stage 3 3.7, R3.5).
    CHECK(object.has_support_group_interface_filament());

    size_t layers_with_second_extruder = 0;
    double moved_length = 0.;
    for (const SupportLayer *support_layer : object.support_layers()) {
        auto it = support_layer->interface_by_extruder.find(1); // 0-based key: filament 2
        if (it == support_layer->interface_by_extruder.end() || it->second.entities.empty())
            continue;
        ++ layers_with_second_extruder;
        for (const ExtrusionEntity *ee : it->second.entities) {
            moved_length += unscale<double>(ee->length());
            // Only B's INTERFACE may be routed to the second extruder, never a base extrusion.
            CHECK(ee->role() == erSupportMaterialInterface);
        }
        // Nothing but that one extruder is ever registered here.
        for (const auto &kv : support_layer->interface_by_extruder)
            CHECK(kv.first == 1u);
    }
    // The interface under B left support_fills and went to its own extruder.
    CHECK(layers_with_second_extruder > 0);
    CHECK(moved_length > 1.);

    // The other half of the plan's gate item 3 - "ToolOrdering lists that extruder on those
    // layers" - is NOT asserted here, and deliberately so. ToolOrdering's layer table is built
    // from a real printer profile's filament vectors and filament map; on the synthetic two-entry
    // config this fixture can put together it does not even keep one LayerTools per layer, so an
    // assertion over it would be testing the fixture rather than the feature. The claim is instead
    // proven end to end, on a real profile, by the corpus gate: scripts/support_group_identity.py
    // --gate groups requires the group_parts case's expect_tool ("T1") to appear in the candidate's
    // G-code and NOT in the baseline's - i.e. the tool change is actually scheduled and emitted.
    // Nothing in ToolOrdering was changed by this stage; interface_by_extruder is storage it
    // already read for Chameleon (ToolOrdering.cpp collect_extruders).
}
