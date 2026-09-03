#include <catch2/catch.hpp>

#include <algorithm>
#include <string>

#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

static inline std::string gltf_path(const char *path)
{
    return std::string(TEST_DATA_DIR) + "/test_gltf/" + path;
}

static inline bool contains(const std::string &haystack, const char *needle)
{
    return haystack.find(needle) != std::string::npos;
}

// Load and require success, so a failure prints the reader's own message instead of "false".
static bool load_ok(const char *fixture, Model &model, GltfInfo &info, std::string &message)
{
    const bool ok = Slic3r::load_gltf(gltf_path(fixture).c_str(), &model, info, message);
    if (!ok)
        WARN("load_gltf(" << fixture << ") failed: " << message);
    return ok;
}

SCENARIO("Reading a glTF/GLB file: geometry", "[gltf]")
{
    GIVEN("a 10 x 20 x 30 box authored in glTF's Y-up frame")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        WHEN("the .glb is read")
        {
            THEN("it becomes one object with one part, Z-up and in millimetres")
            {
                REQUIRE(load_ok("box_10_20_30.glb", model, info, message));
                REQUIRE(model.objects.size() == 1);
                REQUIRE(model.objects.front()->volumes.size() == 1);
                REQUIRE(info.parts.size() == 1);
                // glTF (10 X, 20 Y, 30 Z) -> slicer (10 X, 30 Y, 20 Z): the up-axis rule and the
                // unit rule in one line. Asymmetric on purpose - a wrong sign cannot pass.
                REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 30, 20)));
            }
            THEN("the seam-split vertices were welded, so it is not reported as non-manifold")
            {
                REQUIRE(load_ok("box_10_20_30.glb", model, info, message));
                const TriangleMesh &mesh = model.objects.front()->volumes.front()->mesh();
                REQUIRE(mesh.its.vertices.size() == 8);      // 24 in the file, welded down to 8
                REQUIRE(mesh.its.indices.size() == 12);
                REQUIRE(mesh.stats().open_edges == 0);
                REQUIRE(mesh.stats().number_of_parts == 1);
                // Winding survived: a closed box has a positive signed volume.
                REQUIRE(its_volume(mesh.its) == Approx(10.f * 20.f * 30.f).epsilon(0.001));
            }
            THEN("the object and part are named, and source is filled in for reload-from-disk")
            {
                REQUIRE(load_ok("box_10_20_30.glb", model, info, message));
                REQUIRE(model.objects.front()->name == "box scene");   // the glTF scene's name
                const ModelVolume *v = model.objects.front()->volumes.front();
                REQUIRE(v->name == "box");                             // the node's name
                REQUIRE(v->source.object_idx == 0);
                REQUIRE(v->source.volume_idx == 0);
                REQUIRE(contains(v->source.input_file, "box_10_20_30.glb"));
            }
        }
        WHEN("the same box is read as .gltf with a sidecar .bin")
        {
            THEN("the geometry is identical to the .glb")
            {
                Slic3r::Model glb_model;
                GltfInfo      glb_info;
                REQUIRE(load_ok("box_10_20_30.glb", glb_model, glb_info, message));
                REQUIRE(load_ok("box_10_20_30.gltf", model, info, message));
                const TriangleMesh &a = glb_model.objects.front()->volumes.front()->mesh();
                const TriangleMesh &b = model.objects.front()->volumes.front()->mesh();
                REQUIRE(a.its.vertices.size() == b.its.vertices.size());
                REQUIRE(a.its.indices.size() == b.its.indices.size());
                REQUIRE(is_approx(a.size(), b.size()));
                REQUIRE(its_volume(a.its) == Approx(its_volume(b.its)));
            }
        }
        WHEN("it sits at a path with umlauts and Czech characters")
        {
            THEN("the nowide file read opens it")
            {
                REQUIRE(load_ok("Ger\xc3\xa4te/box-\xc4\x8d\xc5\x99\xc5\xa1\xc5\x99\xc4\x9b\xc3\xa1.glb",
                                model, info, message));
                REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 30, 20)));
            }
        }
    }

    GIVEN("two nodes sharing one mesh (Khronos SimpleMeshes)")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("each node reference becomes its own part, 1 mm apart in X")
        {
            REQUIRE(load_ok("SimpleMeshes.gltf", model, info, message));
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 2);
            const ModelVolume *a = model.objects.front()->volumes[0];
            const ModelVolume *b = model.objects.front()->volumes[1];
            REQUIRE(a->name != b->name);                       // "part" and "part_2"
            REQUIRE(a->get_offset().x() == Approx(0.0).margin(1e-5));
            REQUIRE(b->get_offset().x() == Approx(1.0).margin(1e-5));
            REQUIRE(a->mesh().its.indices.size() == 1);
            REQUIRE(b->mesh().its.indices.size() == 1);
        }
    }

    GIVEN("nested nodes with translation, rotation and scale")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("the composed world matrix is baked, up-axis included")
        {
            REQUIRE(load_ok("nested_trs.glb", model, info, message));
            REQUIRE(model.objects.front()->volumes.size() == 1);
            const ModelVolume *v = model.objects.front()->volumes.front();
            // parent T(5,0,0)*Ry(90) and child T(1,0,0)*S(2) put the 1x2x4 box centre at
            // (5,0,-1) with half extents (4,2,1) in glTF space; (x,y,z) -> (x,-z,y) then gives
            // centre (5,1,0) and size (8,2,4). Every factor of that is load bearing.
            REQUIRE(is_approx(v->get_offset(), Vec3d(5, 1, 0), 1e-4));
            REQUIRE(is_approx(v->mesh().size(), Vec3d(8, 2, 4), 1e-4));
            // The volume itself carries no rotation or scale - the transform went into the mesh.
            REQUIRE(is_approx(v->get_rotation(), Vec3d(0, 0, 0), 1e-6));
            REQUIRE(is_approx(v->get_scaling_factor(), Vec3d(1, 1, 1), 1e-6));
        }
    }

    GIVEN("a TRIANGLE_STRIP and a TRIANGLE_FAN primitive")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("both are de-indexed into triangles with consistent winding")
        {
            REQUIRE(load_ok("strip_and_fan.glb", model, info, message));
            REQUIRE(model.objects.front()->volumes.size() == 2);
            const indexed_triangle_set &strip = model.objects.front()->volumes[0]->mesh().its;
            const indexed_triangle_set &fan   = model.objects.front()->volumes[1]->mesh().its;
            REQUIRE(strip.indices.size() == 2);
            REQUIRE(fan.indices.size() == 3);
            // The strip lies in glTF's XY plane facing +Z, which is -Y after the up-axis rotation.
            // Triangle 1 only agrees with triangle 0 if the odd-triangle vertex swap was applied.
            REQUIRE(is_approx(its_face_normal(strip, 0), Vec3f(0.f, -1.f, 0.f), 1e-5f));
            REQUIRE(is_approx(its_face_normal(strip, 1), Vec3f(0.f, -1.f, 0.f), 1e-5f));
            // The fan lies in the XZ plane facing +Y, which is +Z after the rotation.
            for (int i = 0; i < 3; ++i)
                REQUIRE(is_approx(its_face_normal(fan, i), Vec3f(0.f, 0.f, 1.f), 1e-5f));
            // A mesh with several primitives names its parts after the primitive index.
            REQUIRE(model.objects.front()->volumes[0]->name == "sheet_1");
            REQUIRE(model.objects.front()->volumes[1]->name == "sheet_2");
        }
    }

    GIVEN("a primitive with no index accessor (Khronos TriangleWithoutIndices)")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("the vertices are consumed in order")
        {
            REQUIRE(load_ok("TriangleWithoutIndices.gltf", model, info, message));
            REQUIRE(model.objects.front()->volumes.size() == 1);
            REQUIRE(model.objects.front()->volumes.front()->mesh().its.indices.size() == 1);
        }
    }

    GIVEN("a sparse POSITION accessor")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("the sparse override wins, so the triangle is 5 units tall, not 1")
        {
            REQUIRE(load_ok("sparse_triangle.gltf", model, info, message));
            REQUIRE(model.objects.front()->volumes.size() == 1);
            // base (0,0,0) (1,0,0) (0,1,0) with vertex 2 overridden to (0,5,0); after
            // (x,y,z) -> (x,-z,y) the bounding box is 1 x 0 x 5.
            REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(1, 0, 5), 1e-5));
        }
    }
}

SCENARIO("Reading a glTF/GLB file: materials and colours", "[gltf]")
{
    GIVEN("one mesh with two primitives and two materials")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("each primitive is its own part carrying its own material index")
        {
            REQUIRE(load_ok("two_parts_two_materials.glb", model, info, message));
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 2);
            REQUIRE(info.parts.size() == 2);
            REQUIRE(info.material_colors.size() == 2);
            REQUIRE(info.is_single_material == false);
            REQUIRE(info.parts[0].material_index == 0);
            REQUIRE(info.parts[1].material_index == 1);
            REQUIRE(info.material_colors[0][0] == Approx(1.0f));   // red
            REQUIRE(info.material_colors[0][2] == Approx(0.0f));
            REQUIRE(info.material_colors[1][2] == Approx(1.0f));   // blue
            REQUIRE(info.had_textures == false);
        }
    }

    GIVEN("a material whose linear baseColorFactor is 0.2158605")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("it is reported in sRGB, i.e. about 0.5, not still linear")
        {
            REQUIRE(load_ok("nested_trs.glb", model, info, message));
            REQUIRE(info.material_colors.size() == 1);
            REQUIRE(info.is_single_material == true);
            REQUIRE(info.material_colors[0][0] == Approx(0.5f).margin(0.005f));
            REQUIRE(info.material_colors[0][3] == Approx(1.0f));   // alpha stays linear
        }
    }

    GIVEN("a mesh with COLOR_0 (Khronos BoxVertexColors)")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("one colour per surviving vertex is parsed, ready for Stage 2")
        {
            REQUIRE(load_ok("BoxVertexColors.glb", model, info, message));
            REQUIRE(model.objects.front()->volumes.size() == 1);
            const size_t vertices = model.objects.front()->volumes.front()->mesh().its.vertices.size();
            REQUIRE(vertices > 0);
            REQUIRE(info.vertex_colors.size() == vertices);
            // Stage 1 parses colours but must not paint anything.
            REQUIRE(model.objects.front()->volumes.front()->is_mm_painted() == false);
        }
    }
}

SCENARIO("Reading a glTF/GLB file: refusals are specific", "[gltf]")
{
    Slic3r::Model model;
    GltfInfo      info;
    std::string   message;

    GIVEN("a file with only POINTS primitives")
    {
        THEN("it is refused by name, and the model is untouched")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("points_only.glb").c_str(), &model, info, message));
            REQUIRE(contains(message, "points or lines"));
            REQUIRE(model.objects.empty());
            REQUIRE(info.dropped_primitives == 1);
        }
    }
    GIVEN("a Draco-compressed file")
    {
        THEN("the message names Draco rather than blaming the file")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("box_draco.glb").c_str(), &model, info, message));
            REQUIRE(contains(message, "Draco"));
            REQUIRE(model.objects.empty());
        }
    }
    GIVEN("a file requiring an extension we do not implement")
    {
        THEN("the extension is named verbatim")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("unknown_extension.glb").c_str(), &model, info, message));
            REQUIRE(contains(message, "KHR_texture_basisu"));
            REQUIRE(model.objects.empty());
        }
    }
    GIVEN("a truncated file")
    {
        THEN("it is refused with the damaged-file message and does not crash")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("truncated.glb").c_str(), &model, info, message));
            REQUIRE_FALSE(message.empty());
            REQUIRE(contains(message, "damaged"));
            REQUIRE(model.objects.empty());
        }
    }
    GIVEN("a .gltf whose buffer URI climbs out of its own folder")
    {
        THEN("it is refused before anything is opened")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("escaping_buffer.gltf").c_str(), &model, info, message));
            REQUIRE_FALSE(message.empty());
            REQUIRE(model.objects.empty());
        }
    }
    GIVEN("a file that does not exist")
    {
        THEN("the failure still carries a message, never an empty one")
        {
            REQUIRE_FALSE(Slic3r::load_gltf(gltf_path("no_such_file.glb").c_str(), &model, info, message));
            REQUIRE_FALSE(message.empty());
        }
    }
    GIVEN("a file that is not glTF at all")
    {
        THEN("it says so instead of reporting damage")
        {
            const std::string not_gltf = std::string(TEST_DATA_DIR) + "/test_gltf/SOURCES.md";
            REQUIRE_FALSE(Slic3r::load_gltf(not_gltf.c_str(), &model, info, message));
            REQUIRE_FALSE(message.empty());
        }
    }
}

SCENARIO("Importing a glTF/GLB file through Model::read_from_file", "[gltf]")
{
    GIVEN("the dispatch in Model::read_from_file")
    {
        THEN(".glb is routed to the glTF reader")
        {
            Slic3r::Model model = Slic3r::Model::read_from_file(gltf_path("box_10_20_30.glb"));
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 1);
            REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 30, 20)));
            // read_from_file stamps input_file on every object it produced.
            REQUIRE(contains(model.objects.front()->input_file, "box_10_20_30.glb"));
        }
        THEN(".gltf is routed to the glTF reader")
        {
            Slic3r::Model model = Slic3r::Model::read_from_file(gltf_path("box_10_20_30.gltf"));
            REQUIRE(model.objects.size() == 1);
        }
        THEN("a failing glTF import surfaces the reader's own message, not the generic one")
        {
            REQUIRE_THROWS_WITH(Slic3r::Model::read_from_file(gltf_path("box_draco.glb")),
                                Catch::Matchers::Contains("Draco"));
        }
    }

    GIVEN("a metre-authored box (0.01 x 0.02 x 0.03 units)")
    {
        THEN("the existing 'too small, scale to millimetres?' rescue fires, so no new unit code is needed")
        {
            Slic3r::Model model = Slic3r::Model::read_from_file(gltf_path("box_meters.glb"));
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.looks_like_saved_in_meters());
            model.convert_from_meters(true);
            REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 30, 20), 1e-3));
        }
    }
}
