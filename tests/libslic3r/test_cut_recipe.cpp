// ---------------------------------------------------------------------------
// RE-EDITABLE CUTS: the recipe's round trip, and its ability to reproduce a cut.
//
// A cut used to be destructive - the plane, the sheet and the stroke were
// session state in the gizmo and nothing described how the two halves had been
// made. A CutRecipe is that description, plus the pre-cut mesh, carried by both
// halves and written to Metadata/cut_recipe.xml (with the mesh beside it as
// Metadata/cut_recipe/<sha256>.bin).
//
// What is asserted here:
//   1. The mesh blob round trips EXACTLY - same vertices, same indexing - which
//      is what lets a re-cut reproduce the same halves rather than merely
//      similar ones.
//   2. The whole recipe survives a save/load of a real project 3MF, for each of
//      the three surface kinds, field for field.
//   3. Re-cutting from the LOADED recipe produces the same halves as the
//      original cut did, compared by volume and bounding box.
//   4. The pre-cut mesh is stored ONCE even though both halves reference it.
//   5. With no recipe on the objects, nothing is written - the opt-out path
//      behaves exactly as the tool did before this feature.
// ---------------------------------------------------------------------------

#include <catch2/catch.hpp>

#include <libslic3r/CutRecipe.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/DrawCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/PresetBundle.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/Utils.hpp>

#include <libslic3r/miniz_extension.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

using namespace Slic3r;

// The test article: a 40 mm cube centred on the origin, so a cut plane at the
// frame's z == 0 runs through its middle. The same article the curved- and
// draw-cut suites use.
static const double RCUBE = 40.0;

static indexed_triangle_set recipe_centred_cube(double s = RCUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f &v : its.vertices)
        v -= Vec3f(float(s) * 0.5f, float(s) * 0.5f, float(s) * 0.5f);
    return its;
}

// Volume of a closed indexed triangle set, via the divergence theorem. Signed,
// so a consistently wound mesh gives a positive number.
static double recipe_mesh_volume(const indexed_triangle_set &its)
{
    double v = 0.0;
    for (const Vec3i &f : its.indices) {
        const Vec3d a = its.vertices[size_t(f[0])].cast<double>();
        const Vec3d b = its.vertices[size_t(f[1])].cast<double>();
        const Vec3d c = its.vertices[size_t(f[2])].cast<double>();
        v += a.dot(b.cross(c));
    }
    return std::abs(v) / 6.0;
}

static double recipe_object_volume(const ModelObject *mo)
{
    double v = 0.0;
    for (const ModelVolume *vol : mo->volumes)
        if (vol->type() == ModelVolumeType::MODEL_PART) {
            TriangleMesh m = vol->mesh();
            m.transform(vol->get_matrix());
            v += recipe_mesh_volume(m.its);
        }
    return v;
}

static BoundingBoxf3 recipe_object_bbox(const ModelObject *mo)
{
    BoundingBoxf3 bb;
    for (const ModelVolume *vol : mo->volumes)
        if (vol->type() == ModelVolumeType::MODEL_PART) {
            TriangleMesh m = vol->mesh();
            m.transform(vol->get_matrix());
            bb.merge(m.bounding_box());
        }
    return bb;
}

// A curved sheet with one control point pulled up, so the surface really is a
// height field rather than a plane in disguise.
static CutRecipeSheet recipe_bent_sheet()
{
    CurvedCutSheet s;
    s.reset(5, 5);
    s.set_half_size(30.0, 30.0);
    s.at(2, 2) = 6.0;

    CutRecipeSheet out;
    out.nx          = s.nx();
    out.ny          = s.ny();
    out.half_size_u = s.half_size_u();
    out.half_size_v = s.half_size_v();
    out.values      = s.values();
    return out;
}

// A straight stroke across the top face of the cube, captured the way the gizmo
// captures one: raw samples in the cut plane's frame with a surface normal.
static CutRecipeStroke recipe_straight_stroke()
{
    CutRecipeStroke st;
    st.closed    = false;
    st.smoothing = 0.2;
    for (int i = 0; i <= 40; ++i) {
        DrawCutSample s;
        const double t = -20.0 + double(i);
        s.pos    = Vec3d(t, 0.0, 20.0);
        s.normal = Vec3d::UnitZ();
        s.facet  = size_t(i);
        st.samples.push_back(s);
    }
    return st;
}

// A fully populated recipe of the given kind, so the round trip has something to
// lose in every field rather than only in the ones a default happens to differ
// on.
static CutRecipe recipe_make(CutRecipeKind kind)
{
    CutRecipe r;
    r.version = CutRecipeVersion;
    r.kind    = kind;

    r.plane_center = Vec3d(1.5, -2.25, 3.125);
    {
        Transform3d rot = Transform3d::Identity();
        rot.rotate(Eigen::AngleAxisd(0.3, Vec3d(0.0, 1.0, 0.0)));
        r.rotation_m = rot;
    }

    r.thickness        = 0.8;
    r.thickness_offset = CutThicknessOffset::Above;

    r.keep_upper         = true;
    r.keep_lower         = true;
    r.keep_as_parts      = false;
    r.place_on_cut_upper = true;
    r.place_on_cut_lower = true;
    r.rotate_upper       = false;
    r.rotate_lower       = true;
    r.upper_visibility   = 1;
    r.lower_visibility   = 2;

    if (kind == CutRecipeKind::Curved)
        r.sheet = recipe_bent_sheet();
    if (kind == CutRecipeKind::Drawn) {
        r.stroke           = recipe_straight_stroke();
        r.draw_direction   = int(DrawCutDirection::SurfaceNormal);
        r.draw_view_dir    = Vec3d(0.1, 0.2, -0.9);
        r.draw_extension   = 7.5;
        r.draw_angle_deg   = 4.25;
        r.draw_through_all = false;
        r.draw_depth       = 12.75;
    }
    if (kind == CutRecipeKind::Groove) {
        r.groove.depth            = 3.5f;
        r.groove.width            = 5.5f;
        r.groove.flaps_angle      = 0.25f;
        r.groove.angle            = 0.125f;
        r.groove.depth_init       = 3.0f;
        r.groove.width_init       = 5.0f;
        r.groove.flaps_angle_init = 0.2f;
        r.groove.angle_init       = 0.1f;
        r.groove.depth_tolerance  = 0.15f;
        r.groove.width_tolerance  = 0.2f;
    }

    // One connector of each kind the panel can place, so the connector list is
    // not trivially empty and the enum clamping on the way back in is exercised.
    {
        CutRecipeConnector c;
        c.pos              = Vec3d(4.0, -3.0, 0.0);
        c.rotation_m       = Transform3d::Identity();
        c.radius           = 2.5f;
        c.height           = 6.0f;
        c.radius_tolerance = 0.05f;
        c.height_tolerance = 0.15f;
        c.z_angle          = 0.75f;
        c.type             = int(CutConnectorType::Plug);
        c.style            = int(CutConnectorStyle::Frustum);
        c.shape            = int(CutConnectorShape::Hexagon);
        r.connectors.push_back(c);

        CutRecipeConnector d = c;
        d.pos   = Vec3d(-4.0, 3.0, 0.0);
        d.type  = int(CutConnectorType::Dowel);
        d.shape = int(CutConnectorShape::Circle);
        r.connectors.push_back(d);
    }

    r.mesh      = TriangleMesh(recipe_centred_cube());
    r.mesh_hash = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(r.mesh));
    return r;
}

// Save a model as a project 3MF and read it back, the way the curved-cut suite's
// connector round trip does: Metadata/ parts only reach a file through the
// PROJECT writer, not the generic 3MF one.
static void recipe_round_trip(Model &src, Model &dst, const std::string &file)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams        store_params;
    store_params.path     = file.c_str();
    store_params.model    = &src;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset *>     project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(file.c_str(), &dst_config, &ctxt, &dst, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                         LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);
}

static std::string recipe_tmp_file(const std::string &name)
{
    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    return (tmp_root / name).string();
}

// How many entries under Metadata/cut_recipe/ a written 3MF actually holds. The
// point of the content-addressed store is that two halves of one cut share one
// blob, and only the file itself can prove that.
static int recipe_count_mesh_blobs(const std::string &file)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, file))
        return -1;
    int n = 0;
    const mz_uint num = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < num; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        const std::string name = stat.m_filename;
        if (name.rfind("Metadata/cut_recipe/", 0) == 0)
            ++n;
    }
    close_zip_reader(&archive);
    return n;
}

static bool recipe_has_entry(const std::string &file, const std::string &path)
{
    mz_zip_archive archive;
    mz_zip_zero_struct(&archive);
    if (!open_zip_reader(&archive, file))
        return false;
    bool found = false;
    const mz_uint num = mz_zip_reader_get_num_files(&archive);
    for (mz_uint i = 0; i < num && !found; ++i) {
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&archive, i, &stat))
            continue;
        found = (path == stat.m_filename);
    }
    close_zip_reader(&archive);
    return found;
}

// ---------------------------------------------------------------------------
// 1. The mesh blob.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: the pre-cut mesh blob round trips exactly", "[CutRecipe]")
{
    const TriangleMesh mesh(recipe_centred_cube());
    const std::vector<uint8_t> blob = cut_recipe_mesh_to_blob(mesh);
    REQUIRE_FALSE(blob.empty());

    TriangleMesh back;
    REQUIRE(cut_recipe_mesh_from_blob(blob, back));

    // Not "close": IDENTICAL. The vertices come back bit for bit and the facet
    // indexing is unchanged, which is what lets the re-cut reproduce the same
    // halves rather than halves that merely look the same. An STL round trip
    // would fail this - it re-welds, and loses the indexing.
    REQUIRE(back.its.vertices.size() == mesh.its.vertices.size());
    REQUIRE(back.its.indices.size() == mesh.its.indices.size());
    for (size_t i = 0; i < mesh.its.vertices.size(); ++i)
        REQUIRE(back.its.vertices[i] == mesh.its.vertices[i]);
    for (size_t i = 0; i < mesh.its.indices.size(); ++i)
        REQUIRE(back.its.indices[i] == mesh.its.indices[i]);

    // And the hash is a function of the content, so both halves of a cut name
    // the same blob.
    REQUIRE(cut_recipe_mesh_hash(blob) == cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(back)));
    REQUIRE(cut_recipe_mesh_hash(blob).size() == 64);
}

TEST_CASE("Deft: a corrupt mesh blob is refused rather than trusted", "[CutRecipe]")
{
    const TriangleMesh mesh(recipe_centred_cube());
    std::vector<uint8_t> blob = cut_recipe_mesh_to_blob(mesh);
    TriangleMesh out;

    SECTION("truncated") {
        blob.resize(blob.size() / 2);
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
    SECTION("bad magic") {
        blob[0] = 'X';
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
    SECTION("empty") {
        REQUIRE_FALSE(cut_recipe_mesh_from_blob({}, out));
    }
    SECTION("a facet index past the end of the vertex array") {
        // The last facet's first index, set to a vertex that does not exist. A
        // reader that trusted it would read out of bounds downstream.
        const size_t idx_at = blob.size() - 12;
        blob[idx_at + 0] = 0xFF;
        blob[idx_at + 1] = 0xFF;
        blob[idx_at + 2] = 0x00;
        blob[idx_at + 3] = 0x00;
        REQUIRE_FALSE(cut_recipe_mesh_from_blob(blob, out));
    }
}

// ---------------------------------------------------------------------------
// 2. The recipe's 3MF round trip, per surface kind.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a cut recipe survives a 3MF round trip for every surface kind", "[CutRecipe]")
{
    struct Case { CutRecipeKind kind; const char* name; };
    const Case cases[] = {
        { CutRecipeKind::Plane,  "plane"  },
        { CutRecipeKind::Curved, "curved" },
        { CutRecipeKind::Drawn,  "drawn"  },
        { CutRecipeKind::Groove, "groove" },
    };

    for (const Case &c : cases) {
        INFO("surface kind: " << c.name);
        const CutRecipe in = recipe_make(c.kind);
        REQUIRE(in.valid());

        // Two halves of one cut, both carrying the same recipe - which is how a
        // real cut leaves the model, and what makes the mesh sharing meaningful.
        Model src;
        for (int half = 0; half < 2; ++half) {
            ModelObject *mo = src.add_object();
            mo->name        = std::string("half_") + std::to_string(half);
            mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
            mo->cut_id.init();
            mo->cut_recipe = in;
        }
        src.add_default_instances();

        const std::string file = recipe_tmp_file(std::string("edgeslicer_cut_recipe_") + c.name + ".3mf");
        Model             back;
        recipe_round_trip(src, back, file);

        // THE SHARING. One blob, not two, even though both halves reference it.
        REQUIRE(recipe_count_mesh_blobs(file) == 1);
        REQUIRE(recipe_has_entry(file, "Metadata/cut_recipe.xml"));
        REQUIRE(recipe_has_entry(file, "Metadata/cut_recipe/" + in.mesh_hash + ".bin"));

        REQUIRE(back.objects.size() == 2);
        for (const ModelObject *mo : back.objects) {
            REQUIRE(mo->cut_recipe.has_value());
            const CutRecipe &out = *mo->cut_recipe;

            // FIELD FOR FIELD. operator== compares every stored field and the mesh
            // by its hash, so this one line is the whole schema's round trip.
            REQUIRE(out == in);

            // ... and the mesh really did come back, not merely its name.
            REQUIRE(out.has_mesh());
            REQUIRE(out.mesh.its.vertices.size() == in.mesh.its.vertices.size());
            REQUIRE(out.mesh.its.indices.size() == in.mesh.its.indices.size());
            for (size_t i = 0; i < in.mesh.its.vertices.size(); ++i)
                REQUIRE(out.mesh.its.vertices[i] == in.mesh.its.vertices[i]);

            // Spot-check the fields that a sloppy serializer loses first: the
            // rotation matrix, the sheet values, the stroke samples.
            REQUIRE(out.rotation_m.matrix() == in.rotation_m.matrix());
            REQUIRE(out.plane_center == in.plane_center);
            REQUIRE(out.thickness == in.thickness);
            REQUIRE(out.thickness_offset == in.thickness_offset);
            REQUIRE(out.connectors.size() == in.connectors.size());
            if (c.kind == CutRecipeKind::Curved) {
                REQUIRE(out.sheet.values == in.sheet.values);
                REQUIRE(out.sheet.nx == in.sheet.nx);
                REQUIRE(out.sheet.ny == in.sheet.ny);
                REQUIRE(out.sheet.half_size_u == in.sheet.half_size_u);
            }
            if (c.kind == CutRecipeKind::Drawn) {
                REQUIRE(out.stroke.samples.size() == in.stroke.samples.size());
                for (size_t i = 0; i < in.stroke.samples.size(); ++i) {
                    REQUIRE(out.stroke.samples[i].pos == in.stroke.samples[i].pos);
                    REQUIRE(out.stroke.samples[i].normal == in.stroke.samples[i].normal);
                }
                REQUIRE(out.draw_extension == in.draw_extension);
                REQUIRE(out.draw_angle_deg == in.draw_angle_deg);
                REQUIRE(out.draw_through_all == in.draw_through_all);
                REQUIRE(out.draw_depth == in.draw_depth);
            }
            if (c.kind == CutRecipeKind::Groove)
                REQUIRE(out.groove == in.groove);
        }

        if (!std::getenv("SNORCA_RECIPE_KEEP"))
            boost::filesystem::remove(file);
    }
}

// ---------------------------------------------------------------------------
// 3. Re-cutting from the loaded recipe reproduces the same halves.
//
// This is the feature's actual promise. Everything above only shows the bytes
// survive; this shows the bytes are ENOUGH.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a cut re-performed from a loaded recipe reproduces the same halves", "[CutRecipe]")
{
    struct Case { CutRecipeKind kind; const char* name; };
    const Case cases[] = {
        { CutRecipeKind::Plane,  "plane"  },
        { CutRecipeKind::Curved, "curved" },
    };

    for (const Case &c : cases) {
        INFO("surface kind: " << c.name);

        // A recipe with NO connectors and no kerf: this test is about the surface
        // reproducing, and a connector would add its own volumes to both sides
        // and blur what is being compared.
        CutRecipe in = recipe_make(c.kind);
        in.connectors.clear();
        in.thickness          = 0.0;
        in.thickness_offset   = CutThicknessOffset::Centred;
        in.plane_center       = Vec3d::Zero();
        in.rotation_m         = Transform3d::Identity();
        in.place_on_cut_upper = false;
        in.place_on_cut_lower = false;
        in.rotate_upper       = false;
        in.rotate_lower       = false;
        in.mesh_hash          = cut_recipe_mesh_hash(cut_recipe_mesh_to_blob(in.mesh));

        // THE ORIGINAL CUT, performed from the recipe as the gizmo would.
        auto perform = [](const CutRecipe &r) {
            Model        m;
            ModelObject *mo = m.add_object();
            mo->name        = "src";
            mo->add_volume(r.mesh)->name = "part";
            m.add_default_instances();

            const ModelObjectCutAttributes attrs =
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
            const Transform3d cut_matrix = translation_transform(r.plane_center) * r.rotation_m;

            Cut cut(mo, 0, cut_matrix, attrs);
            ModelObjectPtrs res;
            if (r.kind == CutRecipeKind::Curved)
                res = cut.perform_with_curved_sheet(r.curved_sheet(), r.thickness, r.thickness_offset);
            else
                res = cut.perform_with_plane(r.thickness, r.thickness_offset);

            std::vector<std::pair<double, BoundingBoxf3>> out;
            for (const ModelObject *o : res)
                out.emplace_back(recipe_object_volume(o), recipe_object_bbox(o));
            return out;
        };

        const auto before = perform(in);
        REQUIRE(before.size() == 2);
        // Both halves have material, or the comparison below would be vacuous.
        REQUIRE(before[0].first > 1.0);
        REQUIRE(before[1].first > 1.0);

        // ... now put the recipe through a real 3MF and cut again from what came
        // back.
        Model src;
        {
            ModelObject *mo = src.add_object();
            mo->name        = "half";
            mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
            mo->cut_id.init();
            mo->cut_recipe = in;
            src.add_default_instances();
        }
        const std::string file = recipe_tmp_file(std::string("edgeslicer_cut_recut_") + c.name + ".3mf");
        Model             back;
        recipe_round_trip(src, back, file);
        REQUIRE(back.objects.size() == 1);
        REQUIRE(back.objects.front()->cut_recipe.has_value());

        const auto after = perform(*back.objects.front()->cut_recipe);
        REQUIRE(after.size() == before.size());

        // The halves come back the same. Compared by volume and bounding box
        // within a tolerance, because the boolean is floating point and an exact
        // mesh comparison would be testing the arithmetic rather than the recipe.
        for (size_t i = 0; i < before.size(); ++i) {
            INFO("half " << i);
            REQUIRE(after[i].first == Approx(before[i].first).epsilon(1e-6));
            REQUIRE(after[i].second.min.isApprox(before[i].second.min, 1e-6));
            REQUIRE(after[i].second.max.isApprox(before[i].second.max, 1e-6));
        }

        if (!std::getenv("SNORCA_RECIPE_KEEP"))
            boost::filesystem::remove(file);
    }
}

// ---------------------------------------------------------------------------
// 4. The opt-out.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: with no recipe nothing is written, and the file loads as before", "[CutRecipe]")
{
    // Exactly what "Keep cut editable" OFF leaves in the model: cut halves with
    // a cut_id and no recipe. Nothing about the 3MF should change.
    Model src;
    for (int half = 0; half < 2; ++half) {
        ModelObject *mo = src.add_object();
        mo->name        = std::string("half_") + std::to_string(half);
        mo->add_volume(TriangleMesh(recipe_centred_cube(20.0)))->name = "part";
        mo->cut_id.init();
    }
    src.add_default_instances();

    const std::string file = recipe_tmp_file("edgeslicer_cut_recipe_none.3mf");
    Model             back;
    recipe_round_trip(src, back, file);

    REQUIRE_FALSE(recipe_has_entry(file, "Metadata/cut_recipe.xml"));
    REQUIRE(recipe_count_mesh_blobs(file) == 0);

    REQUIRE(back.objects.size() == 2);
    for (const ModelObject *mo : back.objects) {
        REQUIRE_FALSE(mo->cut_recipe.has_value());
        REQUIRE_FALSE(mo->has_cut_recipe());
        // The cut itself is untouched: this is still a cut object, exactly as it
        // was before the feature existed.
        REQUIRE(mo->is_cut());
    }

    if (!std::getenv("SNORCA_RECIPE_KEEP"))
        boost::filesystem::remove(file);
}

// ---------------------------------------------------------------------------
// 5. A recipe whose mesh blob is missing is dropped, not half-applied.
// ---------------------------------------------------------------------------

TEST_CASE("Deft: a recipe with no stored mesh is not offered for editing", "[CutRecipe]")
{
    // valid() is what the menu item and arm_reedit() gate on. A recipe with no
    // mesh cannot reproduce the cut, so it must not claim it can.
    CutRecipe r = recipe_make(CutRecipeKind::Plane);
    REQUIRE(r.valid());

    r.mesh = TriangleMesh();
    REQUIRE_FALSE(r.has_mesh());
    REQUIRE_FALSE(r.valid());

    // Same for a schema this build does not know.
    CutRecipe future = recipe_make(CutRecipeKind::Plane);
    future.version   = CutRecipeVersion + 1;
    REQUIRE_FALSE(future.valid());

    // ... and for a curved recipe whose sheet does not add up, which is what a
    // truncated value list would produce.
    CutRecipe bent = recipe_make(CutRecipeKind::Curved);
    REQUIRE(bent.valid());
    bent.sheet.values.pop_back();
    REQUIRE_FALSE(bent.sheet.valid());
    REQUIRE_FALSE(bent.valid());
}
