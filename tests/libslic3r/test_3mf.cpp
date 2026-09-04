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
