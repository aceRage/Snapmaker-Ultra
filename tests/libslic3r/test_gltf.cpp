#include <catch2/catch.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "libslic3r/Format/GLTF.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ObjColorUtils.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// Golden serialisations captured from the shared paint helper - see the two [golden] scenarios
// at the end of this file for what they pin and why.
static const char *GOLDEN_VERTEX_COLOR_SERIALISATION = "8|0C88A|0C1C80C893|80C0C6|80C0C2|0C886|0C886|8|1C88A|81C0C1C813|80C0C6|1C0C0CA|";
static const char *GOLDEN_FACE_COLOR_SERIALISATION   = "|8|0C|1C||8|0C|1C||8|0C|1C|";

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

    GIVEN("the same box through KHR_mesh_quantization")
    {
        Slic3r::Model model;
        GltfInfo      info;
        std::string   message;
        THEN("it is de-quantized to exactly the same size, not silently imported at the wrong scale")
        {
            // POSITION is normalized int16 and the real scale sits on the node.
            // cgltf_accessor_unpack_floats de-quantizes and cgltf_node_transform_world picks the
            // node scale up, so nothing extra is needed - but a silent wrong scale is the worst
            // failure this importer could have, so it is asserted rather than assumed.
            REQUIRE(load_ok("box_quantized.glb", model, info, message));
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 1);
            REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(), Vec3d(10, 30, 20), 0.02));
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
            // add_volume centres each mesh on its own bbox, so the offset is that centre - the
            // triangle spans 0..1 in X, and the second node's translation moves it by exactly 1.
            REQUIRE(a->get_offset().x() == Approx(0.5).margin(1e-5));
            REQUIRE(b->get_offset().x() == Approx(1.5).margin(1e-5));
            REQUIRE(b->get_offset().x() - a->get_offset().x() == Approx(1.0).margin(1e-5));
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

// ===========================================================================================
// Stage 2 - colours to filaments
// ===========================================================================================

// A stub for the GUI's ObjColorDialog: records what it was handed and answers with fixed ids.
struct ColorDialogStub
{
    bool                       called{false};
    std::vector<RGBA>          seen;
    bool                       seen_single{false};
    std::vector<unsigned char> answer;      // returned verbatim when non-empty
    unsigned char              first{1};
    // When set, the answer is generated per input colour instead of copied from `answer`.
    std::function<unsigned char(size_t)> per_color;

    ObjImportColorFn fn()
    {
        return [this](std::vector<RGBA> &input_colors, bool is_single_color,
                      std::vector<unsigned char> &filament_ids, unsigned char &first_extruder_id) {
            this->called      = true;
            this->seen        = input_colors;
            this->seen_single = is_single_color;
            if (this->per_color) {
                filament_ids.clear();
                filament_ids.reserve(input_colors.size());
                for (size_t i = 0; i < input_colors.size(); ++i)
                    filament_ids.push_back(this->per_color(i));
            } else {
                filament_ids = this->answer;
            }
            first_extruder_id = this->first;
        };
    }
};

static Slic3r::Model read_gltf_with_colors(const char *fixture, ColorDialogStub &stub, std::string *warning = nullptr)
{
    return Slic3r::Model::read_from_file(gltf_path(fixture), nullptr, nullptr,
                                         LoadStrategy::AddDefaultInstances, nullptr, nullptr, nullptr,
                                         nullptr, nullptr, nullptr, nullptr, 0, stub.fn(), warning);
}

SCENARIO("The colour dialog's k-means copes with a handful of material colours", "[gltf]")
{
    // Experiment 5.3 from the plan. Stage 2 hands QuantKMeans 1-16 colours - one per glTF
    // material - where the OBJ path hands it one per triangle. Nothing here may divide by zero,
    // ask OpenCV for more clusters than it has samples, or come back empty.
    GIVEN("between 1 and 30 well-separated colours")
    {
        auto palette = [](size_t n) {
            std::vector<RGBA> out;
            out.reserve(n);
            for (size_t i = 0; i < n; ++i) {
                // Spread over the cube so the colours stay distinct after the 8-bit and Lab trips.
                const float t = float(i) / float(n == 1 ? 1 : n - 1);
                out.push_back(RGBA{t, 1.f - t, float((i * 7) % 5) / 4.f, 1.f});
            }
            return out;
        };

        THEN("automatic cluster selection returns one label per colour and at least one cluster")
        {
            for (size_t n : {size_t(1), size_t(2), size_t(3), size_t(4), size_t(8), size_t(16), size_t(30)}) {
                const std::vector<RGBA> colors = palette(n);
                std::vector<RGBA>       clusters;
                std::vector<int>        labels;
                QuantKMeans             quant(10);
                quant.apply(colors, clusters, labels, -1);
                INFO("n = " << n << ", clusters = " << clusters.size());
                REQUIRE(labels.size() == n);
                REQUIRE(clusters.size() >= 1);
                REQUIRE(clusters.size() <= n);
                for (int label : labels) {
                    REQUIRE(label >= 0);
                    REQUIRE(label < (int) clusters.size());
                }
            }
        }
        THEN("an explicit cluster count never asks for more clusters than there are colours")
        {
            for (size_t n : {size_t(1), size_t(2), size_t(5), size_t(16)}) {
                const std::vector<RGBA> colors = palette(n);
                for (int k = 1; k <= (int) n + 2; ++k) {
                    std::vector<RGBA> clusters;
                    std::vector<int>  labels;
                    QuantKMeans       quant(10);
                    quant.apply(colors, clusters, labels, k);
                    INFO("n = " << n << ", k = " << k);
                    REQUIRE(labels.size() == n);
                    REQUIRE(clusters.size() >= 1);
                    REQUIRE(clusters.size() <= n);
                }
            }
        }
    }
}

SCENARIO("glTF material colours become per-part filaments", "[gltf]")
{
    GIVEN("a two-material GLB and a dialog that answers filaments 2 and 3")
    {
        ColorDialogStub stub;
        stub.answer = {2, 3};
        stub.first  = 2;
        Slic3r::Model model = read_gltf_with_colors("two_parts_two_materials.glb", stub);

        THEN("the dialog saw one colour per material, not one per triangle")
        {
            REQUIRE(stub.called);
            REQUIRE(stub.seen.size() == 2);
            REQUIRE(stub.seen_single == false);
        }
        THEN("each part lands on its own filament, with no MMU painting at all")
        {
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 2);
            const ModelObject *obj = model.objects.front();
            REQUIRE(obj->config.extruder() == 2);
            REQUIRE(obj->volumes[0]->config.extruder() == 2);
            REQUIRE(obj->volumes[1]->config.extruder() == 3);
            // The whole point of per-part assignment: parts stay editable, nothing is painted.
            REQUIRE(obj->volumes[0]->is_mm_painted() == false);
            REQUIRE(obj->volumes[1]->is_mm_painted() == false);
        }
    }

    GIVEN("a three-material GLB and a dialog that answers 2, 3 and 4")
    {
        ColorDialogStub stub;
        stub.answer = {2, 3, 4};
        stub.first  = 2;
        Slic3r::Model model = read_gltf_with_colors("three_materials.glb", stub);
        THEN("the dialog is opened once and the three parts take the three filaments")
        {
            REQUIRE(stub.called);
            REQUIRE(stub.seen.size() == 3);
            const ModelObject *obj = model.objects.front();
            REQUIRE(obj->volumes.size() == 3);
            REQUIRE(obj->volumes[0]->config.extruder() == 2);
            REQUIRE(obj->volumes[1]->config.extruder() == 3);
            REQUIRE(obj->volumes[2]->config.extruder() == 4);
            for (const ModelVolume *v : obj->volumes)
                REQUIRE(v->is_mm_painted() == false);
        }
    }

    GIVEN("a three-material GLB and a dialog the user cancelled")
    {
        // What a hidden instance sees: the modal hook answers the dialog without showing it, and
        // Plater clears filament_ids on anything but OK. The import must still succeed, in colour
        // or not - it must never fail or hang.
        ColorDialogStub stub;   // answer left empty, i.e. cancelled
        stub.first = 1;
        Slic3r::Model model = read_gltf_with_colors("three_materials.glb", stub);
        THEN("the geometry still imports, simply without colour")
        {
            REQUIRE(stub.called);
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.size() == 3);
            for (const ModelVolume *v : model.objects.front()->volumes) {
                REQUIRE(v->is_mm_painted() == false);
                REQUIRE(v->config.has("extruder") == false);
            }
        }
    }

    GIVEN("a single-material GLB")
    {
        ColorDialogStub stub;
        stub.answer = {2};
        Slic3r::Model model = read_gltf_with_colors("box_10_20_30.glb", stub);
        THEN("no dialog is opened - there is nothing to choose")
        {
            REQUIRE(stub.called == false);
            REQUIRE(model.objects.size() == 1);
            REQUIRE(model.objects.front()->volumes.front()->is_mm_painted() == false);
        }
    }

    GIVEN("a two-material GLB whose first material paints from a texture")
    {
        ColorDialogStub stub;
        stub.answer = {2, 3};
        std::string   warning;
        Slic3r::Model model = read_gltf_with_colors("textured_two_materials.glb", stub, &warning);
        THEN("no dialog is opened, and the dropped texture is reported instead")
        {
            // Without the had_textures guard this file WOULD open the dialog - it has two
            // materials - so this really tests the guard.
            REQUIRE(stub.called == false);
            REQUIRE_FALSE(warning.empty());
            REQUIRE(contains(warning, "texture"));
            REQUIRE(model.objects.front()->volumes.size() == 2);
        }
    }

    GIVEN("a part with no material at all")
    {
        // strip_and_fan.glb has two primitives and no materials, so material_colors is empty and
        // the dialog must stay shut.
        ColorDialogStub stub;
        stub.answer = {2, 3};
        Slic3r::Model model = read_gltf_with_colors("strip_and_fan.glb", stub);
        THEN("no dialog, no filament assignment")
        {
            REQUIRE(stub.called == false);
            REQUIRE(model.objects.front()->volumes.size() == 2);
        }
    }
}

SCENARIO("glTF COLOR_0 becomes MMU painting", "[gltf]")
{
    GIVEN("BoxVertexColors.glb and a dialog that alternates two filaments")
    {
        ColorDialogStub stub;
        stub.first     = 2;
        stub.per_color = [](size_t i) { return (unsigned char) (2 + (i % 2)); };
        Slic3r::Model model = read_gltf_with_colors("BoxVertexColors.glb", stub);

        THEN("the dialog saw one colour per vertex and the volume ends up painted")
        {
            REQUIRE(stub.called);
            REQUIRE(model.objects.size() == 1);
            const ModelVolume *v = model.objects.front()->volumes.front();
            REQUIRE(stub.seen.size() == v->mesh().its.vertices.size());
            REQUIRE(v->is_mm_painted());
            REQUIRE(v->config.extruder() == 2);
        }
    }

    GIVEN("a vertex-colour array of the wrong total length")
    {
        Slic3r::Model    model;
        GltfInfo         info;
        std::string      message;
        REQUIRE(load_ok("two_parts_two_materials.glb", model, info, message));
        REQUIRE(model.objects.front()->volumes.size() == 2);
        size_t total = 0;
        for (const ModelVolume *v : model.objects.front()->volumes)
            total += v->mesh().its.vertices.size();

        THEN("it is refused and nothing at all is painted")
        {
            std::vector<unsigned char> ids(total - 1, 2);
            REQUIRE_FALSE(Slic3r::Model::import_multi_volume_vertex_color_deal(ids, 2, &model));
            for (const ModelVolume *v : model.objects.front()->volumes)
                REQUIRE(v->is_mm_painted() == false);
        }
        THEN("the right length is accepted and paints every volume")
        {
            std::vector<unsigned char> ids;
            ids.reserve(total);
            for (size_t i = 0; i < total; ++i)
                ids.push_back((unsigned char) (2 + (i % 2)));
            REQUIRE(Slic3r::Model::import_multi_volume_vertex_color_deal(ids, 2, &model));
            for (const ModelVolume *v : model.objects.front()->volumes)
                REQUIRE(v->is_mm_painted());
        }
    }
}

// The golden regression for the refactor in change 2.2. paint_volume_from_vertex_colors is the
// per-triangle body lifted verbatim out of Model::obj_import_vertex_color_deal; these strings are
// the exact 3MF/AMF serialisation it produces, so any future edit to the shared helper that
// changes OBJ behaviour fails here rather than silently shipping.
SCENARIO("obj_import_vertex_color_deal serialisation is unchanged by the shared helper", "[gltf][golden]")
{
    GIVEN("a cube whose eight corners span all three vertex-colour cases")
    {
        Slic3r::Model model;
        ModelObject  *object = model.add_object();
        object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART);
        ModelVolume *volume = object->volumes.front();
        REQUIRE(volume->mesh().its.vertices.size() == 8);
        REQUIRE(volume->mesh().its.indices.size() == 12);

        // Deliberately uneven so the cube's twelve faces hit _3_SAME_COLOR, _3_DIFF_COLOR and
        // _2_SAME_1_DIFF_COLOR between them.
        const std::vector<unsigned char> ids = {2, 2, 2, 3, 3, 4, 2, 3};

        THEN("every painted triangle serialises to its recorded golden string")
        {
            REQUIRE(Slic3r::Model::obj_import_vertex_color_deal(ids, 2, &model));
            REQUIRE(object->config.extruder() == 2);
            REQUIRE(volume->config.extruder() == 2);
            REQUIRE(volume->is_mm_painted());
            std::string serialised;
            for (int i = 0; i < (int) volume->mesh().its.indices.size(); ++i)
                serialised += volume->mmu_segmentation_facets.get_triangle_as_string(i) + "|";
            REQUIRE(serialised == GOLDEN_VERTEX_COLOR_SERIALISATION);
        }
    }
}

SCENARIO("obj_import_face_color_deal serialisation is unchanged", "[gltf][golden]")
{
    GIVEN("a cube with a per-face filament array")
    {
        Slic3r::Model model;
        ModelObject  *object = model.add_object();
        object->add_volume(TriangleMesh(its_make_cube(10., 10., 10.)), ModelVolumeType::MODEL_PART);
        ModelVolume *volume = object->volumes.front();

        std::vector<unsigned char> ids;
        for (size_t i = 0; i < volume->mesh().its.indices.size(); ++i)
            ids.push_back((unsigned char) (1 + (i % 4)));   // includes 1, which must be skipped

        THEN("every painted triangle serialises to its recorded golden string")
        {
            REQUIRE(Slic3r::Model::obj_import_face_color_deal(ids, 2, &model));
            std::string serialised;
            for (int i = 0; i < (int) volume->mesh().its.indices.size(); ++i)
                serialised += volume->mmu_segmentation_facets.get_triangle_as_string(i) + "|";
            REQUIRE(serialised == GOLDEN_FACE_COLOR_SERIALISATION);
        }
    }
}
