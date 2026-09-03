#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/3mf.hpp"
#include "libslic3r/Format/STL.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem/operations.hpp>

using namespace Slic3r;
using Catch::Matchers::WithinAbs;

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


// Ultra (support groups) - Stage 2 / T5 gate.
// A part's support-group data must survive a 3MF round trip: 3MF export writes every
// volume->config key with no allow-list, and import feeds every unmatched metadata key back to
// volume->config.set_deserialize. Nothing else in the format had to change for groups to work.
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md, Stage 2 "Gate".
SCENARIO("Support group data survives a 3mf round trip", "[3mf][SupportGroups]") {
    GIVEN("a two-volume object whose second part carries a group and two support overrides") {
        Model src_model;
        std::string src_file = std::string(TEST_DATA_DIR) + "/test_3mf/Prusa.stl";
        load_stl(src_file.c_str(), &src_model);
        src_model.add_default_instances();

        ModelObject* src_object = src_model.objects.front();
        // A second MODEL_PART volume, so the object is genuinely multi-part.
        TriangleMesh second = src_object->volumes.front()->mesh();
        ModelVolume* src_part_b = src_object->add_volume(std::move(second));
        src_part_b->set_type(ModelVolumeType::MODEL_PART);

        src_part_b->config.set_key_value("support_group", new ConfigOptionString("B"));
        src_part_b->config.set_key_value("support_interface_top_layers", new ConfigOptionInt(5));
        src_part_b->config.set_key_value("support_interface_spacing", new ConfigOptionFloat(0.0));

        WHEN("the model is saved to and loaded back from a 3mf file") {
            std::string test_file = std::string(TEST_DATA_DIR) + "/test_3mf/support_group.3mf";
            REQUIRE(store_3mf(test_file.c_str(), &src_model, nullptr, false));

            Model dst_model;
            DynamicPrintConfig dst_config;
            {
                ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::Disable };
                REQUIRE(load_3mf(test_file.c_str(), dst_config, ctxt, &dst_model, false));
            }
            boost::filesystem::remove(test_file);

            THEN("both volumes come back with their configs intact") {
                REQUIRE(dst_model.objects.size() == 1);
                ModelObject* dst_object = dst_model.objects.front();
                REQUIRE(dst_object->volumes.size() == 2);

                // The first part carried nothing and must still carry nothing.
                CHECK(! dst_object->volumes[0]->config.has("support_group"));
                CHECK(! dst_object->volumes[0]->config.has("support_interface_top_layers"));

                const ModelConfig& b = dst_object->volumes[1]->config;
                REQUIRE(b.has("support_group"));
                CHECK(b.opt_serialize("support_group") == "B");
                REQUIRE(b.has("support_interface_top_layers"));
                CHECK(b.opt_int("support_interface_top_layers") == 5);
                REQUIRE(b.has("support_interface_spacing"));
                CHECK_THAT(b.opt_float("support_interface_spacing"), WithinAbs(0.0, 1e-9));
            }
        }
    }
}
