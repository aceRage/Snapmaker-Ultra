// Ultra (support groups) - Stage 2 / T4 gate.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 2 "Gate", items 1-6.
//
// These exercise PrintObject::support_groups() and the soluble rule of §3.6. Nothing in the
// generator consumes a group yet, so the point of every case below is the resolution itself -
// above all that a project which carries no group data, or whose overrides happen to match the
// object, still yields exactly ONE group, which is what keeps the slicer on today's code path.

#include <catch2/catch.hpp>

#include <algorithm>

#include "libslic3r/Model.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::WithinAbs;

namespace {

// Two 20 mm cubes side by side as two MODEL_PART volumes of ONE object - the multi-part shape
// support groups are about. `tweak` runs after the volumes exist and before print.apply(), which
// is where a test sets per-volume config.
void make_two_part_print(Slic3r::Print                                        &print,
                         Slic3r::Model                                        &model,
                         std::initializer_list<ConfigBase::SetDeserializeItem> config_items,
                         const std::function<void(ModelObject &)>             &tweak)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict(config_items);

    ModelObject *object = model.add_object();
    object->name = "two_part";
    object->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20));
    object->add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20, Vec3d(25., 0., 0.)));
    object->add_instance();
    if (tweak)
        tweak(*object);
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
}

const PrintObject& first_object(const Slic3r::Print &print)
{
    REQUIRE(! print.objects().empty());
    return *print.objects().front();
}

} // namespace

TEST_CASE("support_groups: two parts with no overrides resolve to one group", "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model, {{"enable_support", "1"}}, nullptr);

    auto groups = first_object(print).support_groups();
    // The off-mode pin: no group data anywhere, so K == 1 and every caller stays on today's path.
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].name.empty());
    CHECK(groups[0].volumes.size() == 2);
    CHECK(groups[0].config.diff(first_object(print).config()).empty());
}

TEST_CASE("support_groups: one part overriding an interface key makes a second group", "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_interface_top_layers", "2"}},
                        [](ModelObject &object) {
                            object.volumes[1]->config.set_key_value("support_interface_top_layers",
                                                                    new ConfigOptionInt(5));
                            object.volumes[1]->config.set_key_value("support_group",
                                                                    new ConfigOptionString("B"));
                        });

    auto groups = first_object(print).support_groups();
    REQUIRE(groups.size() == 2);

    // Group 0 is always the default group and comes first.
    CHECK(groups[0].name.empty());
    REQUIRE(groups[0].volumes.size() == 1);
    CHECK(groups[0].volumes[0] == model.objects.front()->volumes[0]);
    CHECK(groups[0].config.opt_int("support_interface_top_layers") == 2);

    // The override group carries the right volume and the resolved value.
    CHECK(groups[1].name == "B");
    REQUIRE(groups[1].volumes.size() == 1);
    CHECK(groups[1].volumes[0] == model.objects.front()->volumes[1]);
    CHECK(groups[1].config.opt_int("support_interface_top_layers") == 5);
}

TEST_CASE("support_groups: an override equal to the object's value collapses into the default group",
          "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_interface_top_layers", "3"}},
                        [](ModelObject &object) {
                            // Same value the object already has, plus a name.
                            object.volumes[1]->config.set_key_value("support_interface_top_layers",
                                                                    new ConfigOptionInt(3));
                            object.volumes[1]->config.set_key_value("support_group",
                                                                    new ConfigOptionString("B"));
                        });

    auto groups = first_object(print).support_groups();
    // Grouping is by RESOLVED config, never by name, so a no-op override keeps K == 1. This is
    // the case that makes "a group the user created but did not change" free.
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].volumes.size() == 2);
}

TEST_CASE("support_groups: two differently named parts with identical values collapse to one group",
          "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_interface_top_layers", "2"}},
                        [](ModelObject &object) {
                            for (int i = 0; i < 2; ++ i)
                                object.volumes[i]->config.set_key_value("support_interface_top_layers",
                                                                        new ConfigOptionInt(6));
                            object.volumes[0]->config.set_key_value("support_group",
                                                                    new ConfigOptionString("first"));
                            object.volumes[1]->config.set_key_value("support_group",
                                                                    new ConfigOptionString("second"));
                        });

    auto groups = first_object(print).support_groups();
    // §3.4 step 3: identical values collapse regardless of the labels, and the survivor takes the
    // first non-empty name. Group 0 is still present (and now empty of parts) per step 4, so the
    // count is 2 rather than the 1 the plan's gate line quotes - see the Stage 2 status section.
    REQUIRE(groups.size() == 2);
    CHECK(groups[0].volumes.empty());
    CHECK(groups[1].name == "first");
    CHECK(groups[1].volumes.size() == 2);
}

TEST_CASE("support_groups: a part asking for a soluble interface makes the whole object soluble",
          "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_top_z_distance", "0.2"}},
                        [](ModelObject &object) {
                            object.volumes[1]->config.set_key_value("support_top_z_distance",
                                                                    new ConfigOptionFloat(0.));
                        });

    // §3.6: the strictest group wins, object-wide, because support_top_z_distance also drives
    // SlicingParameters::soluble_interface and bottom-surface classification.
    CHECK_THAT(first_object(print).config().support_top_z_distance.value, WithinAbs(0.0, 1e-9));
    CHECK(PrintObject::support_groups_want_soluble(*model.objects.front()));
}

TEST_CASE("support_groups: the object is untouched when no part asks for a soluble interface",
          "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_top_z_distance", "0.2"}}, nullptr);

    CHECK_THAT(first_object(print).config().support_top_z_distance.value, WithinAbs(0.2, 1e-9));
    CHECK(! PrintObject::support_groups_want_soluble(*model.objects.front()));
}

TEST_CASE("support_groups: group data on a modifier or an enforcer is ignored", "[SupportGroups]")
{
    Slic3r::Print print;
    Slic3r::Model model;
    make_two_part_print(print, model,
                        {{"enable_support", "1"}, {"support_interface_top_layers", "2"},
                         {"support_top_z_distance", "0.2"}},
                        [](ModelObject &object) {
                            ModelVolume *modifier =
                                object.add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20,
                                                                     Vec3d(0., 25., 0.)));
                            modifier->set_type(ModelVolumeType::PARAMETER_MODIFIER);
                            modifier->config.set_key_value("support_interface_top_layers",
                                                           new ConfigOptionInt(9));
                            modifier->config.set_key_value("support_top_z_distance",
                                                           new ConfigOptionFloat(0.));
                            modifier->config.set_key_value("support_group",
                                                           new ConfigOptionString("ignore me"));

                            ModelVolume *enforcer =
                                object.add_volume(Slic3r::Test::mesh(TestMesh::cube_20x20x20,
                                                                     Vec3d(0., -25., 0.)));
                            enforcer->set_type(ModelVolumeType::SUPPORT_ENFORCER);
                            enforcer->config.set_key_value("support_group",
                                                           new ConfigOptionString("nor me"));
                        });

    auto groups = first_object(print).support_groups();
    // Only is_model_part() volumes take part, so neither the modifier nor the enforcer splits a
    // group - and neither can trip the soluble rule.
    REQUIRE(groups.size() == 1);
    CHECK(groups[0].volumes.size() == 2);
    CHECK(groups[0].name.empty());
    CHECK(! PrintObject::support_groups_want_soluble(*model.objects.front()));
    CHECK_THAT(first_object(print).config().support_top_z_distance.value, WithinAbs(0.2, 1e-9));
}

TEST_CASE("support_group is defined but belongs to no static config class", "[SupportGroups]")
{
    // §3.1: the key exists in print_config_def, so a ModelVolume can carry it and 3MF export
    // writes it - but it is a member of NO static config class and is not a print option, which
    // is what keeps it out of the process preset, the project config and the G-code CONFIG_BLOCK.
    REQUIRE(print_config_def.has("support_group"));

    PrintObjectConfig object_config;
    const t_config_option_keys object_keys = object_config.keys();
    CHECK(std::find(object_keys.begin(), object_keys.end(), "support_group") == object_keys.end());

    PrintRegionConfig region_config;
    const t_config_option_keys region_keys = region_config.keys();
    CHECK(std::find(region_keys.begin(), region_keys.end(), "support_group") == region_keys.end());

    const std::vector<std::string> &print_options = Preset::print_options();
    CHECK(std::find(print_options.begin(), print_options.end(), "support_group") == print_options.end());

    // Graceful degradation in a stock Orca / Bambu Studio: handle_legacy() blanks any key the
    // build does not know, so a project carrying support_group opens there with the key silently
    // dropped rather than raising UnknownOptionException. Mirrored here on a key no build has.
    std::string opt_key = "a_key_no_build_knows";
    std::string value   = "x";
    PrintConfigDef::handle_legacy(opt_key, value);
    CHECK(opt_key.empty());
}
