#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TriangleMesh.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

using namespace Slic3r;

SCENARIO("Reading 3mf file", "[3mf]") {
    GIVEN("umlauts in the path of the file") {
        Model model;
        WHEN("3mf model is read") {
        	std::string path = std::string(TEST_DATA_DIR) + "/test_3mf/Geräte/Büchse.3mf";
        	DynamicPrintConfig config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
            bool ret = load_3mf(path.c_str(), config, ctxt, &model, false);
            THEN("load should succeed") {
                REQUIRE(ret);
            }
        }
    }
}

SCENARIO("Export+Import geometry to/from 3mf file cycle", "[3mf]") {
    GIVEN("world vertices coordinates before save") {
        // load a model from stl file
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();

        // apply generic transformation to the 1st volume
        Geometry::Transformation src_volume_transform;
        src_volume_transform.set_offset({ 10.0, 20.0, 0.0 });
        src_volume_transform.set_rotation({ Geometry::deg2rad(25.0), Geometry::deg2rad(35.0), Geometry::deg2rad(45.0) });
        src_volume_transform.set_scaling_factor({ 1.1, 1.2, 1.3 });
        src_volume_transform.set_mirror({ -1.0, 1.0, -1.0 });
        src_object->volumes.front()->set_transformation(src_volume_transform);

        // apply generic transformation to the 1st instance
        Geometry::Transformation src_instance_transform;
        src_instance_transform.set_offset({ 5.0, 10.0, 0.0 });
        src_instance_transform.set_rotation({ Geometry::deg2rad(12.0), Geometry::deg2rad(13.0), Geometry::deg2rad(14.0) });
        src_instance_transform.set_scaling_factor({ 0.9, 0.8, 0.7 });
        src_instance_transform.set_mirror({ 1.0, -1.0, -1.0 });
        src_object->instances.front()->set_transformation(src_instance_transform);

        WHEN("model is saved+loaded to/from 3mf file") {
            // save the model to 3mf file
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/prusa.3mf";
            store_3mf(test_file.c_str(), &src_model, nullptr, false);

            // load back the model from the 3mf file
            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false);
            }
            boost::filesystem::remove(test_file);

            // compare meshes
            TriangleMesh src_mesh = src_model.mesh();
            TriangleMesh dst_mesh = dst_model.mesh();

            bool res = src_mesh.its.vertices.size() == dst_mesh.its.vertices.size();
            if (res) {
                for (size_t i = 0; i < dst_mesh.its.vertices.size(); ++i) {
                    res &= dst_mesh.its.vertices[i].isApprox(src_mesh.its.vertices[i]);
                }
            }
            THEN("world vertices coordinates after load match") {
                REQUIRE(res);
            }
        }
    }
}

SCENARIO("2D convex hull of sinking object", "[3mf]") {
    GIVEN("model") {
        // load a model
        Model model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &model);
        model.add_default_instances();

        WHEN("model is rotated, scaled and set as sinking") {
            ModelObject* object = model.objects.front();
            object->center_around_origin(false);

            // set instance's attitude so that it is rotated, scaled and sinking
            ModelInstance* instance = object->instances.front();
            instance->set_rotation(X, -M_PI / 4.0);
            instance->set_offset(Vec3d::Zero());
            instance->set_scaling_factor({ 2.0, 2.0, 2.0 });

            // calculate 2D convex hull
            Polygon hull_2d = object->convex_hull_2d(instance->get_transformation().get_matrix());

            // verify result
            // Unlike upstream PrusaSlicer, this fork's convex_hull_2d projects the
            // entire mesh: it does not clip a sinking object at the print bed, so the
            // hull extends to the full rotated extents of the model.
            Points result = {
                { -91501495, -15914144 },
                { 91501495, -15914144 },
                { 91501495, 13792823 },
                { 34846496, 14717717 },
                { -85501495, 13917981 },
                { -91501495, 13792823 }
            };

            // Allow 1um error due to floating point rounding.
            bool res = hull_2d.points.size() == result.size();
            if (res)
                for (size_t i = 0; i < result.size(); ++ i) {
                    const Point &p1 = result[i];
                    const Point &p2 = hull_2d.points[i];
                    if (std::abs(p1.x() - p2.x()) > 1 || std::abs(p1.y() - p2.y()) > 1) {
                        res = false;
                        break;
                    }
                }

            THEN("2D convex hull should match with reference") {
                for (const Point &p : hull_2d.points)
                    UNSCOPED_INFO("actual hull point: { " << p.x() << ", " << p.y() << " }");
                REQUIRE(res);
            }
        }
    }
}


// True if the loader would drop or rename this key on the way in: PrintConfigDef::handle_legacy()
// clears obsolete keys (extruder_type and silent_mode are in that set although print_config_def
// still defines them), and a round trip cannot expect those back.
static bool retired_on_load(const std::string &key)
{
    t_config_option_key legacy_key = key;
    std::string         value;
    PrintConfigDef::handle_legacy(legacy_key, value);
    return legacy_key != key;
}

// A headless project save. store_bbs_3mf serializes the whole project config with
// ConfigBase::save_to_json (_add_project_config_file_to_archive); a config built from the static
// defaults rather than from a PresetBundle used to crash there on its first coEnums member, whose
// keys map was never set. The application never hit it because its project config comes from a
// PresetBundle, but anything headless that stores a project did.
SCENARIO("A project built from the static default config survives a project 3MF round trip", "[3mf][Config]") {
    GIVEN("a one-part model and DynamicPrintConfig::full_print_config()") {
        Model src_model;
        ModelObject *src_object = src_model.add_object();
        src_object->name = "cube";
        src_object->add_volume(make_cube(10., 10., 10.))->name = "cube";
        src_object->add_instance();
        src_object->ensure_on_bed();
        DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

        WHEN("the project is stored and loaded again") {
            // The exporter writes its scratch config through Model::get_backup_path(), which is rooted at
            // temporary_dir(). The application sets that at startup; a test that exercises the project
            // writer has to as well, or the path resolves to the root of the current drive.
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());
            const std::string test_file = (tmp_root / "full_print_config_project.3mf").string();

            StoreParams store_params;
            store_params.path     = test_file.c_str();
            store_params.model    = &src_model;
            store_params.config   = &store_config;
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            const bool stored = store_bbs_3mf(store_params);

            Model                     dst_model;
            DynamicPrintConfig        dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs             plate_data;
            std::vector<Preset*>      project_presets;
            bool                      is_bbl_3mf = false;
            Semver                    file_version;
            const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                                       &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                                       nullptr,
                                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                       LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
            release_PlateData_list(plate_data);
            boost::filesystem::remove(test_file);

            THEN("store and load both succeed") {
                REQUIRE(stored);
                REQUIRE(loaded);
            }

            THEN("the coEnums options come back by name, with the values they were stored with") {
                REQUIRE(loaded);
                for (const std::string &key : store_config.keys()) {
                    const ConfigOption *opt = store_config.option(key);
                    if (opt->type() != coEnums || retired_on_load(key))
                        continue;
                    INFO("option " << key);
                    REQUIRE(dst_config.has(key));
                    CHECK(*dst_config.option(key) == *opt);
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------------------------
// Ultra (support groups) - Stage 2 / T5: per-part support data through a project 3MF.
//
// The project format is bbs_3mf, NOT the legacy writer the scenarios above use: store_3mf()
// emits a plain PrusaSlicer 3MF and never writes a volume's ModelConfig, so a round trip through
// it could not say anything about group data. store_bbs_3mf()/load_bbs_3mf() is the pair the
// application uses when the user saves a project and opens it again.
//
// Two things an earlier attempt tripped over, fixed here rather than worked around:
//   * StoreParams::config is dereferenced unconditionally (_add_model_config_file_to_archive
//     takes a const DynamicPrintConfig&), so it has to be a real config and not nullptr;
//   * without SaveStrategy::Silence the exporter writes an origin.txt under the model's backup
//     path, which a bare Model has not got.
//
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 2 "Gate" item 1.
// ---------------------------------------------------------------------------------------------
SCENARIO("Support group data survives a project 3MF round trip", "[3mf][SupportGroups]") {
    GIVEN("a two-part object whose parts carry a support group and part-level support values") {
        // A pillar with a plate floating above it: an overhang, so the corpus case built from
        // this same fixture really does generate support.
        Model src_model;
        ModelObject* src_object = src_model.add_object();
        src_object->name = "two_part_groups";
        src_object->add_volume(make_cube(10., 10., 20.))->name = "pillar";
        src_object->add_volume(make_cube(30., 10.,  2.))->name = "plate";
        src_object->volumes[1]->set_offset({ -10., 0., 20. });
        src_object->add_instance();
        src_object->ensure_on_bed();

        // Part A stays in the default group but carries one override, part B is group "B" with
        // the four values a support set writes. Both tiers are represented: A keys
        // (interface geometry and filament) and a B key (support_top_z_distance).
        ModelVolume* a = src_object->volumes[0];
        ModelVolume* b = src_object->volumes[1];
        a->config.set_key_value("support_interface_bottom_layers", new ConfigOptionInt(1));
        b->config.set_key_value("support_group",                new ConfigOptionString("B"));
        b->config.set_key_value("support_interface_top_layers",  new ConfigOptionInt(5));
        b->config.set_key_value("support_interface_spacing",     new ConfigOptionFloat(0.15));
        b->config.set_key_value("support_interface_filament",    new ConfigOptionInt(2));
        b->config.set_key_value("support_top_z_distance",        new ConfigOptionFloat(0.));

        WHEN("the project is stored and loaded again") {
            const std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/support_groups.3mf";
            DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();

            // The exporter writes its scratch config through Model::get_backup_path(), which is
            // rooted at temporary_dir(). The application sets that at startup; a test that
            // exercises the project writer has to as well, or the path resolves to the root of
            // the current drive.
            const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
            boost::filesystem::create_directories(tmp_root);
            Slic3r::set_temporary_dir(tmp_root.string());

            // A libslic3r bug this test must not trip over, and worth recording:
            // ConfigOptionEnumsGenericTempl::set() copies only the values, never keys_map, so a
            // coEnums member of a STATIC config class - FullPrintConfig, i.e. what
            // full_print_config() is built from - keeps keys_map == nullptr, and
            // serialize_single_value() dereferences it with no check. The exporter serialises the
            // whole config to JSON (_add_project_config_file_to_archive), so it crashes there. A
            // project config that came from a PresetBundle has the maps, which is why the
            // application never hits this.
            // Rebuild those options straight from print_config_def instead: DynamicConfig's
            // create path clones the def's default value, and THAT clone carries keys_map.
            // Erasing them is not enough - a project config with no nozzle_volume_type crashes
            // the CLI on load.
            for (const std::string& key : store_config.keys())
                if (const ConfigOption* opt = store_config.option(key); opt != nullptr && opt->type() == coEnums) {
                    store_config.erase(key);
                    store_config.option(key, true);
                }

            StoreParams store_params;
            store_params.path     = test_file.c_str();
            store_params.model    = &src_model;
            store_params.config   = &store_config;
            store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
            const bool stored = store_bbs_3mf(store_params);

            Model                 dst_model;
            DynamicPrintConfig    dst_config;
            ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
            PlateDataPtrs         plate_data;
            std::vector<Preset*>  project_presets;
            bool                  is_bbl_3mf = false;
            Semver                file_version;
            const bool loaded = stored && load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &dst_model,
                                                       &plate_data, &project_presets, &is_bbl_3mf, &file_version,
                                                       nullptr,
                                                       LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                                                       LoadStrategy::AddDefaultInstances | LoadStrategy::Silence);
            release_PlateData_list(plate_data);
            boost::filesystem::remove(test_file);

            THEN("store and load both succeed") {
                REQUIRE(stored);
                REQUIRE(loaded);
            }

            THEN("both volumes come back with byte-identical configs") {
                REQUIRE(dst_model.objects.size() == 1);
                REQUIRE(dst_model.objects.front()->volumes.size() == src_object->volumes.size());
                for (size_t i = 0; i < src_object->volumes.size(); ++ i) {
                    const DynamicPrintConfig& want = src_object->volumes[i]->config.get();
                    const DynamicPrintConfig& have = dst_model.objects.front()->volumes[i]->config.get();
                    INFO("volume " << i);
                    REQUIRE(have.keys() == want.keys());
                    for (const std::string& key : want.keys()) {
                        INFO("key " << key);
                        REQUIRE(*have.option(key) == *want.option(key));
                    }
                }
            }

            THEN("support_group and the tier values are intact, and nothing leaked onto part A") {
                REQUIRE(dst_model.objects.size() == 1);
                const ModelObject* dst_object = dst_model.objects.front();
                REQUIRE(dst_object->volumes.size() == 2);
                const DynamicPrintConfig& da = dst_object->volumes[0]->config.get();
                const DynamicPrintConfig& db = dst_object->volumes[1]->config.get();

                REQUIRE(! da.has("support_group"));
                REQUIRE(da.opt_int("support_interface_bottom_layers") == 1);

                REQUIRE(db.has("support_group"));
                REQUIRE(db.opt_string("support_group") == "B");
                REQUIRE(db.opt_int("support_interface_top_layers") == 5);
                REQUIRE(db.opt_float("support_interface_spacing") == Approx(0.15));
                REQUIRE(db.opt_int("support_interface_filament") == 2);
                REQUIRE(db.opt_float("support_top_z_distance") == Approx(0.));
            }

            THEN("the geometry is unchanged") {
                // The other half of the stock-Orca degradation claim: dropping the key costs the
                // reader nothing but the key.
                REQUIRE(dst_model.mesh().its.vertices.size() == src_model.mesh().its.vertices.size());
                REQUIRE(dst_model.mesh().its.indices.size() == src_model.mesh().its.indices.size());
            }
        }
    }
}

// A build that does not know support_group - stock Orca or Bambu Studio - drops it silently on
// load rather than throwing UnknownOptionException: PrintConfigDef::handle_legacy() clears an
// opt_key it does not recognise and returns, and ConfigBase::set_deserialize() then skips it.
// This is the mechanism behind "a project with groups still opens everywhere", asserted here
// against a key this build genuinely does not have. No stock build was run.
SCENARIO("An unknown support key degrades gracefully the way stock Orca would", "[3mf][SupportGroups]") {
    GIVEN("a key this build does not define") {
        t_config_option_key key   = "support_group_from_a_newer_build";
        std::string         value = "B";
        WHEN("handle_legacy sees it") {
            PrintConfigDef::handle_legacy(key, value);
            THEN("the key is cleared, which is how the loader drops it without failing") {
                REQUIRE(key.empty());
            }
        }
    }
    GIVEN("support_group itself, which THIS build does define") {
        t_config_option_key key   = "support_group";
        std::string         value = "B";
        WHEN("handle_legacy sees it") {
            PrintConfigDef::handle_legacy(key, value);
            THEN("it survives untouched") {
                REQUIRE(key == "support_group");
                REQUIRE(value == "B");
                REQUIRE(print_config_def.has("support_group"));
            }
        }
    }
}
