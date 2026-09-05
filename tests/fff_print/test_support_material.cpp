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


// ============================================================================================
// Ultra (support groups) - regression for the claim-reach bug found on hardware.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2c.
//
// A group on an object with EXACTLY ONE part has to act like the same values set object-wide.
// The first Stage 3 build claimed only the part's own footprint plus the XY gap and one interface
// extrusion width, while fill_contact_layer() stretches every contact onto a grid cell of
// support_base_pattern_spacing + flow spacing (5.4 mm on stock settings). Most of each contact
// therefore landed outside the claim and was filled by the DEFAULT group, with the object's own
// settings - and on a single-part object there is no neighbouring part whose interface would look
// different, so nothing in the output showed it. That is how it reached a user.
// ============================================================================================

namespace {

// The two-part fixture's leg and one of its floating cubes, merged into ONE MODEL_PART volume: the
// object has a single part, so the default group ends up owning no parts at all.
void make_floating_one_part_print(Slic3r::Print                            &print,
                                  Slic3r::Model                            &model,
                                  const Slic3r::DynamicPrintConfig         &config,
                                  const std::function<void(ModelObject &)> &tweak)
{
    ModelObject *object = model.add_object();
    object->name = "floating_one_part";
    // A leg on the bed carrying a long, NARROW bar 10 mm above it. Narrow on purpose: the bar is
    // 3 mm wide where the support grid cell is 5.42 mm, so the contact SupportGridPattern produces
    // is a band of grid cells much wider than the bar itself. That is the shape a claim of
    // "footprint + 0.6 mm" cannot cover - a wide flat overhang hides the bug, because its fringe is
    // a small fraction of a large contact.
    TriangleMesh mesh = Slic3r::make_cube(2., 2., 10.);      // the leg, on the bed
    TriangleMesh top  = Slic3r::make_cube(3., 30., 6.);      // the bar, floating 10 mm up
    top.translate(0.f, 0.f, 10.f);
    mesh.merge(top);
    object->add_volume(mesh);
    object->add_instance();
    if (tweak)
        tweak(*object);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    print.set_status_silent();
    print.process();
}

// erSupportMaterialInterface path length over every support layer, with no left / right split:
// this object has one part, so there is nothing to split.
double total_interface_length(const PrintObject &object)
{
    double all = 0., unused = 0.;
    for (const SupportLayer *support_layer : object.support_layers())
        sum_interface_length(support_layer->support_fills, std::numeric_limits<double>::max(), all, unused);
    return all;
}

} // namespace

TEST_CASE("SupportMaterial: a group on a single-part object acts like the object's own settings",
          "[SupportMaterial][support_groups]")
{
    auto interface_of = [](const DynamicPrintConfig                  &config,
                           const std::function<void(ModelObject &)>  &tweak,
                           size_t                                     expect_groups) {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_one_part_print(print, model, config, tweak);
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == expect_groups);
        REQUIRE(! object.support_layers().empty());
        return total_interface_length(object);
    };

    // Control: the object's own two interface layers, no group anywhere.
    const double control = interface_of(group_fixture_config(), nullptr, 1);
    REQUIRE(control > 1.);

    // The yardstick: the same five dense interface layers, set OBJECT-WIDE. Whatever a group asks
    // for, it has to deliver about as much of it as the object asking for it itself does.
    DynamicPrintConfig global_config = group_fixture_config();
    global_config.set_deserialize_strict({
        { "support_interface_top_layers", "5" },
        { "support_interface_spacing",    "0" },
    });
    const double global = interface_of(global_config, nullptr, 1);
    // Or the yardstick means nothing.
    REQUIRE(global > control * 1.30);

    // The same five layers, asked for by a GROUP on the object's only part.
    Slic3r::Print print;
    Slic3r::Model model;
    make_floating_one_part_print(print, model, group_fixture_config(), [](ModelObject &object) {
        ModelVolume *part = object.volumes.front();
        part->config.set_key_value("support_group", new ConfigOptionString("B"));
        part->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
        part->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    });
    REQUIRE(! print.objects().empty());
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.support_groups().size() == 2);
    REQUIRE(! object.support_layers().empty());
    // The single-part shape: the group holds the object's only part and the default group is empty
    // of parts. It still exists, because downstream code addresses group 0 unconditionally.
    REQUIRE(object.support_groups()[1].volumes.size() == 1);
    CHECK(object.support_groups()[0].volumes.empty());
    const double grouped = total_interface_length(object);

    // The group really acts - this is what the bug broke: `grouped` used to sit near `control`.
    CHECK(grouped > control * 1.30);
    // And it delivers essentially all of what the object-wide setting delivers. Not equality: the
    // default group still takes whatever falls outside the claim, e.g. grid cells more than one
    // cell out from the part.
    CHECK(grouped > global * 0.90);
}


// ============================================================================================
// Ultra (support groups) - Stage 4 gate: TREE supports.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 4 "Gate".
//
// 4a - organic trees. Where an organic tree's interface comes from is NOT where the plan assumed.
// generate_interface_layers() only converts intermediate layers that sit near a contact; the roof
// itself is drawn by the tip placement pass, one island at a time, in sample_overhang_area(), which
// takes the roof layer count as an argument. That argument is what Stage 4a made per group, so the
// magnitude test below - "a group delivers what the same value set object-wide delivers" - is the
// assertion that matters, exactly as it was for the normal generator after the hardware pass.
//
// 4b - classic trees. Their roof GEOMETRY is decided during influence-area propagation
// (draw_circles), object-wide, and Stage 4b deliberately does not touch it: only the FILL follows
// the group. So the classic-tree cases assert the opposite pair - the fill really changed, and the
// number of roof layers did NOT - plus the notice that says so.
// ============================================================================================

namespace {

DynamicPrintConfig tree_fixture_config(const char *style)
{
    DynamicPrintConfig config = group_fixture_config();
    config.set_deserialize_strict({
        { "support_type",  "tree(auto)" },
        { "support_style", style },
    });
    return config;
}

// Interface extrusion length under part A (left) and part B (right), counting the interface a group
// routed to its own filament as well - support_fills is not the only place it can land.
void sum_interface_length_all(const SupportLayer &support_layer, double split_x, double &left, double &right)
{
    sum_interface_length(support_layer.support_fills, split_x, left, right);
    for (const auto &kv : support_layer.interface_by_extruder)
        sum_interface_length(kv.second, split_x, left, right);
}

std::pair<double, double> interface_lengths_all(const PrintObject &object)
{
    const double split_x = split_x_between_parts(object);
    double left = 0., right = 0.;
    for (const SupportLayer *support_layer : object.support_layers())
        sum_interface_length_all(*support_layer, split_x, left, right);
    return std::make_pair(left, right);
}

// How many support layers carry interface on each side. This is the ROOF DEPTH - the quantity a
// classic tree decides object-wide and an organic tree now decides per group.
std::pair<size_t, size_t> interface_layer_counts(const PrintObject &object)
{
    const double split_x = split_x_between_parts(object);
    size_t left = 0, right = 0;
    for (const SupportLayer *support_layer : object.support_layers()) {
        double l = 0., r = 0.;
        sum_interface_length_all(*support_layer, split_x, l, r);
        if (l > 0.)
            ++ left;
        if (r > 0.)
            ++ right;
    }
    return std::make_pair(left, right);
}

double total_interface_length_all(const PrintObject &object)
{
    double all = 0., unused = 0.;
    for (const SupportLayer *support_layer : object.support_layers())
        sum_interface_length_all(*support_layer, std::numeric_limits<double>::max(), all, unused);
    return all;
}

bool has_warning_containing(const PrintObject &object, const std::string &needle)
{
    for (const auto &warning : object.step_state_with_warnings(posSupportMaterial).warnings)
        if (warning.message.find(needle) != std::string::npos)
            return true;
    return false;
}

} // namespace

TEST_CASE("SupportMaterial: per-group interface, organic tree", "[SupportMaterial][support_groups][tree]")
{
    // Control: no part carries an override, so support_groups() collapses to a single group and the
    // organic generator takes exactly today's path.
    double control_a = 0., control_b = 0.;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, tree_fixture_config("organic"), nullptr);
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == 1);
        REQUIRE(! object.support_layers().empty());
        std::tie(control_a, control_b) = interface_lengths_all(object);
        REQUIRE(control_a > 1.);
        REQUIRE(control_b > 1.);
    }

    double grouped_a = 0., grouped_b = 0.;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, tree_fixture_config("organic"), [](ModelObject &object) {
            ModelVolume *part_b = object.volumes.back();
            part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
            part_b->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
            part_b->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
        });
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == 2);
        REQUIRE(object.support_groups()[1].name == "B");
        std::tie(grouped_a, grouped_b) = interface_lengths_all(object);
    }

    // B's roof got denser and deeper.
    CHECK(grouped_b > control_b * 1.30);
    // A's did not. Not the normal generator's 1 %: an organic tree's branches are drawn from
    // influence areas that propagate down the whole object, so raising the roof count over B moves
    // a little material under A too. 10 % is comfortably below the change a per-group roof makes,
    // and the two-part fixture's parts are 20 mm apart so their branch systems are separate.
    CHECK(std::abs(grouped_a - control_a) <= control_a * 0.10);
}

TEST_CASE("SupportMaterial: a group on a single-part organic tree acts like the object's own settings",
          "[SupportMaterial][support_groups][tree]")
{
    auto interface_of = [](const DynamicPrintConfig                 &config,
                           const std::function<void(ModelObject &)> &tweak,
                           size_t                                    expect_groups) {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_one_part_print(print, model, config, tweak);
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == expect_groups);
        REQUIRE(! object.support_layers().empty());
        return total_interface_length_all(object);
    };

    const double control = interface_of(tree_fixture_config("organic"), nullptr, 1);
    REQUIRE(control > 1.);

    // The yardstick: the same five dense roof layers, set OBJECT-WIDE.
    DynamicPrintConfig global_config = tree_fixture_config("organic");
    global_config.set_deserialize_strict({
        { "support_interface_top_layers", "5" },
        { "support_interface_spacing",    "0" },
    });
    const double global = interface_of(global_config, nullptr, 1);
    REQUIRE(global > control * 1.30);

    // The same five layers, asked for by a GROUP on the object's only part. This is the assertion
    // that fails when the roof count is left object-wide: the tips over this part would still carry
    // the object's two layers and the group would deliver barely more than the control.
    const double grouped = interface_of(tree_fixture_config("organic"), [](ModelObject &object) {
        ModelVolume *part = object.volumes.front();
        part->config.set_key_value("support_group", new ConfigOptionString("B"));
        part->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
        part->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
    }, 2);

    CHECK(grouped > control * 1.30);
    CHECK(grouped > global * 0.90);
}

TEST_CASE("SupportMaterial: per-group roof fill, classic tree", "[SupportMaterial][support_groups][tree]")
{
    // A classic tree's roof geometry is object-wide, so a group that changes only the interface
    // SPACING must change how much material is laid into its own roof and nothing else at all -
    // not the other part's roof, and not the number of roof layers on either side.
    double control_a = 0., control_b = 0.;
    std::pair<size_t, size_t> control_layers;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, tree_fixture_config("tree_slim"), nullptr);
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == 1);
        REQUIRE(! object.support_layers().empty());
        std::tie(control_a, control_b) = interface_lengths_all(object);
        control_layers = interface_layer_counts(object);
        REQUIRE(control_a > 1.);
        REQUIRE(control_b > 1.);
        REQUIRE(control_layers.first > 0);
        REQUIRE(control_layers.second > 0);
    }

    double grouped_a = 0., grouped_b = 0.;
    std::pair<size_t, size_t> grouped_layers;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, tree_fixture_config("tree_slim"), [](ModelObject &object) {
            ModelVolume *part_b = object.volumes.back();
            part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
            part_b->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.));
        });
        REQUIRE(! print.objects().empty());
        const PrintObject &object = *print.objects().front();
        REQUIRE(object.support_groups().size() == 2);
        // A group that changes no interface LAYER COUNT raises no classic-tree notice.
        CHECK(! object.has_support_group_interface_layer_override());
        std::tie(grouped_a, grouped_b) = interface_lengths_all(object);
        grouped_layers = interface_layer_counts(object);
    }

    // B's roof is solid where the object's is spaced.
    CHECK(grouped_b > control_b * 1.20);
    // A's roof did not move at all: the geometry is shared and untouched, and A is still filled with
    // the object's own interface parameters.
    CHECK(std::abs(grouped_a - control_a) <= control_a * 0.01);
    // And the roof DEPTH is identical on both sides - the documented 4b limit, asserted rather than
    // asserted-about.
    CHECK(grouped_layers.first  == control_layers.first);
    CHECK(grouped_layers.second == control_layers.second);
}

TEST_CASE("SupportMaterial: classic tree says its interface layer count is object-wide",
          "[SupportMaterial][support_groups][tree]")
{
    // The plan's 4b limit, made visible. A group asking for its own interface layer count on a
    // classic tree gets the object's count - the roof layers were decided in draw_circles() before
    // anything knew about groups - and the user is told, non-fatally, instead of being left to
    // wonder why nothing changed.
    std::pair<size_t, size_t> control_layers;
    {
        Slic3r::Print print;
        Slic3r::Model model;
        make_floating_two_part_print(print, model, tree_fixture_config("tree_slim"), nullptr);
        REQUIRE(! print.objects().empty());
        control_layers = interface_layer_counts(*print.objects().front());
        REQUIRE(control_layers.second > 0);
    }

    Slic3r::Print print;
    Slic3r::Model model;
    make_floating_two_part_print(print, model, tree_fixture_config("tree_slim"), [](ModelObject &object) {
        ModelVolume *part_b = object.volumes.back();
        part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
        part_b->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
    });
    REQUIRE(! print.objects().empty());
    const PrintObject &object = *print.objects().front();
    REQUIRE(object.support_groups().size() == 2);
    CHECK(object.has_support_group_interface_layer_override());
    CHECK(has_warning_containing(object, "object-wide for classic tree supports"));
    // The count really is unchanged - this is the limit, not a bug report.
    const std::pair<size_t, size_t> grouped_layers = interface_layer_counts(object);
    CHECK(grouped_layers.second == control_layers.second);

    // The same override on an ORGANIC tree raises no notice, because organic trees do honour it.
    Slic3r::Print organic_print;
    Slic3r::Model organic_model;
    make_floating_two_part_print(organic_print, organic_model, tree_fixture_config("organic"), [](ModelObject &object) {
        ModelVolume *part_b = object.volumes.back();
        part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
        part_b->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
    });
    REQUIRE(! organic_print.objects().empty());
    CHECK(! has_warning_containing(*organic_print.objects().front(), "object-wide for classic tree supports"));
}

TEST_CASE("SupportMaterial: per-group interface filament, organic tree", "[SupportMaterial][support_groups][tree]")
{
    DynamicPrintConfig config = tree_fixture_config("organic");
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
            CHECK(ee->role() == erSupportMaterialInterface);
        }
        for (const auto &kv : support_layer->interface_by_extruder)
            CHECK(kv.first == 1u);
    }
    CHECK(layers_with_second_extruder > 0);
    CHECK(moved_length > 1.);
}

TEST_CASE("SupportMaterial: per-group interface filament, classic tree", "[SupportMaterial][support_groups][tree]")
{
    DynamicPrintConfig config = tree_fixture_config("tree_slim");
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
    CHECK(object.has_support_group_interface_filament());

    // The classic tree writes the group's roof straight into interface_by_extruder: it does no
    // height modulation, so there is nothing to do afterwards.
    const double split_x = split_x_between_parts(object);
    size_t layers_with_second_extruder = 0;
    double own_part = 0., other_part = 0.;
    for (const SupportLayer *support_layer : object.support_layers()) {
        auto it = support_layer->interface_by_extruder.find(1);
        if (it == support_layer->interface_by_extruder.end() || it->second.entities.empty())
            continue;
        ++ layers_with_second_extruder;
        sum_interface_length(it->second, split_x, other_part, own_part);
        for (const ExtrusionEntity *ee : it->second.entities)
            CHECK(ee->role() == erSupportMaterialInterface);
        for (const auto &kv : support_layer->interface_by_extruder)
            CHECK(kv.first == 1u);
    }
    CHECK(layers_with_second_extruder > 0);
    CHECK(own_part > 1.);
    // It stays on its own part: the corpus gate's expect_tool_part criterion, in miniature.
    CHECK(other_part <= own_part * 0.05);
}
