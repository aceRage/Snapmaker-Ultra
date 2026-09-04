#include <catch2/catch.hpp>

#include <array>
#include <stdexcept>

#include "libslic3r/Exception.hpp"
#include "libslic3r/Format/ImportedTexture.hpp"
#include "libslic3r/Format/bbs_3mf.hpp"
#include "libslic3r/GCode/ToolOrdering.hpp"
#include "libslic3r/ImageMapRawFilamentOffsetAtlas.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelTextureDataRemap.hpp"
#include "libslic3r/PaintDepth.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Semver.hpp"
#include "libslic3r/TextureMapping.hpp"
#include "libslic3r/TextureMappingContoning.hpp"
#include "libslic3r/TextureMappingOffset.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include <boost/filesystem.hpp>

using namespace Slic3r;

TEST_CASE("texture_mapping PrintConfig keys register with ImageMap defaults", "[texturemapping]")
{
    const DynamicPrintConfig config = DynamicPrintConfig::full_print_config();

    REQUIRE(config.has("texture_mapping_definitions"));
    REQUIRE(config.has("texture_mapping_global_settings"));
    REQUIRE(config.has("texture_mapping_background_color"));
    REQUIRE(config.has("texture_mapping_outer_wall_gradient_global_strength"));
    REQUIRE(config.has("texture_mapping_outer_wall_gradient_max_line_width"));
    REQUIRE(config.has("texture_mapping_outer_wall_gradient_min_line_width"));

    CHECK(config.option<ConfigOptionString>("texture_mapping_definitions")->value.empty());
    CHECK(config.option<ConfigOptionString>("texture_mapping_global_settings")->value.empty());
    CHECK(config.option<ConfigOptionString>("texture_mapping_background_color")->value == "#FFFFFFFF");
    CHECK(config.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_global_strength")->value == 100.0);
    CHECK(config.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_max_line_width")->value == 0.95);
    CHECK(config.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_min_line_width")->value == 0.32);

    // paint_depth_* must still be present and untouched by this PR.
    REQUIRE(config.has("paint_depth_mode"));
    REQUIRE(config.has("paint_depth_walls"));
    REQUIRE(config.has("paint_depth_mm"));
}

TEST_CASE("TextureMappingZone modulation enums match ImageMap values", "[texturemapping]")
{
    CHECK(int(TextureMappingZone::ModulationLineWidth) == 0);
    CHECK(int(TextureMappingZone::ModulationPerimeterPath) == 1);
    CHECK(int(TextureMappingZone::ModulationPerimeterPathV2) == 2);
}

TEST_CASE("TextureMappingManager construct / serialize round-trip", "[texturemapping]")
{
    const std::vector<std::string> colours = {"#FF0000", "#00FF00", "#0000FF"};
    TextureMappingManager mgr;
    REQUIRE(mgr.zones().empty());

    TextureMappingZone *zone = mgr.add_zone(colours.size(), colours, int(TextureMappingZone::ImageTexture));
    REQUIRE(zone != nullptr);
    REQUIRE(mgr.zones().size() == 1);
    CHECK(zone->modulation_mode == int(TextureMappingZone::ModulationLineWidth));

    const std::string serialized = mgr.serialize_entries();
    REQUIRE_FALSE(serialized.empty());

    TextureMappingManager reloaded;
    reloaded.load_entries(serialized, colours);
    REQUIRE(reloaded.zones().size() == 1);
    CHECK(reloaded.zones().front().stable_id == zone->stable_id);
}

TEST_CASE("texture mapping offset helpers smoke", "[texturemapping]")
{
    TextureMappingZone zone;
    zone.component_ids = "123";

    const std::vector<unsigned int> ids = decode_texture_mapping_offset_component_ids(zone, 3);
    REQUIRE(ids.size() == 3);
    CHECK(ids[0] == 1);

    CHECK(normalize_texture_mapping_offset_angle_deg(0.f) == 0.f);
    CHECK(texture_mapping_offset_fade_factor(int(TextureMappingZone::OffsetFadeNone), 0.5f) == 1.f);

    ImageMapRawFilamentOffsetAtlas atlas;
    CHECK_FALSE(atlas.valid());
}

TEST_CASE("TextureMappingContoningSolver constructs from a zone", "[texturemapping][contoning]")
{
    PrintConfig config;
    config.filament_colour.values = {"#FF0000", "#00FF00", "#0000FF"};
    TextureMappingZone zone;
    zone.component_ids = "12";
    const TextureMappingContoningSolver solver(zone, config, {1, 2}, 0.2f);
    CHECK(solver.component_ids().size() == 2);
}

TEST_CASE("ModelTextureDataRemap snapshot on empty volume", "[texturemapping][remap]")
{
    Model model;
    ModelObject *object = model.add_object();
    TriangleMesh mesh;
    ModelVolume *volume = object->add_volume(mesh);
    REQUIRE(volume != nullptr);
    CHECK(volume->texture_mapping_color_facets.empty());

    const SimplifyTextureDataSnapshot snapshot = snapshot_simplify_texture_data(*volume);
    CHECK(snapshot.source == SimplifyColorSource::None);
    CHECK_FALSE(model_volume_region_painting_needs_remap(*volume));
}

TEST_CASE("ImportedTexture helpers construct", "[texturemapping][importedtexture]")
{
    size_t buffer_size = 0;
    REQUIRE(checked_rgba_buffer_size(2, 2, buffer_size));
    CHECK(buffer_size == 16);
    CHECK_FALSE(is_supported_image_texture_path("model.gltf"));
    CHECK(is_supported_image_texture_path("atlas.png"));
}


TEST_CASE("TextureMappingManager empty is a no-op for zone resolve", "[texturemapping][pr2]")
{
    TextureMappingManager mgr;
    CHECK(mgr.zones().empty());
    CHECK_FALSE(mgr.is_texture_mapping_zone_id(1));
    CHECK_FALSE(mgr.is_texture_mapping_zone_id(4));
    CHECK(mgr.total_filaments(3) == 3);
    CHECK(mgr.zone_from_id(1) == nullptr);
}

TEST_CASE("TextureMappingManager resolves virtual zone IDs to physical components", "[texturemapping][pr2]")
{
    const std::vector<std::string> colours = {"#FF0000", "#00FF00", "#0000FF"};
    TextureMappingManager mgr;
    TextureMappingZone *zone = mgr.add_zone(colours.size(), colours, int(TextureMappingZone::ImageTexture));
    REQUIRE(zone != nullptr);
    REQUIRE(zone->zone_id > colours.size());
    REQUIRE(mgr.is_texture_mapping_zone_id(zone->zone_id));
    CHECK(mgr.total_filaments(colours.size()) >= colours.size() + 1);

    const unsigned int resolved = mgr.resolve_zone_component(zone->zone_id, colours.size(), 0);
    CHECK(resolved >= 1);
    CHECK(resolved <= colours.size());
}

TEST_CASE("TextureMappingContoningSolver remains callable for Fill schedule driver", "[texturemapping][contoning][pr2]")
{
    PrintConfig config;
    config.filament_colour.values = {"#FF0000", "#00FF00", "#0000FF"};
    TextureMappingZone zone;
    zone.component_ids = "12";
    zone.surface_pattern = int(TextureMappingZone::ImageTexture);
    const TextureMappingContoningSolver solver(zone, config, {1, 2}, 0.2f);
    REQUIRE(solver.valid());
    REQUIRE(solver.component_ids().size() == 2);
    const TextureMappingContoningStack stack = solver.solve({0.5f, 0.5f, 0.5f}, 4);
    REQUIRE_FALSE(stack.bottom_to_top.empty());
    const unsigned int component = solver.component_for_depth({0.5f, 0.5f, 0.5f}, 4, 0);
    CHECK(component >= 1);
}

TEST_CASE("LayerTools resolve_filament_id is a no-op without a TextureMapping manager", "[texturemapping][pr2]")
{
    LayerTools tools(0.);
    CHECK(tools.texture_mapping_manager == nullptr);
    CHECK(tools.resolve_filament_id(0) == 0);
    CHECK(tools.resolve_filament_id(4) == 4);
}

TEST_CASE("C5: legacy mmu_segmented_region_max_width still migrates to paint_depth_*", "[texturemapping][pr3][paintdepth]")
{
    DynamicPrintConfig loaded;
    loaded.load_from_ini_string("mmu_segmented_region_max_width = 0.8\n", ForwardCompatibilitySubstitutionRule::Disable);

    REQUIRE(loaded.has("paint_depth_mode"));
    REQUIRE(loaded.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value == pdmMillimeters);
    REQUIRE(loaded.has("paint_depth_mm"));
    CHECK(std::abs(loaded.option<ConfigOptionFloat>("paint_depth_mm")->value - 0.8) < 1e-6);
}

TEST_CASE("C6: bbs_3mf round-trips texture_mapping_* and paint_depth_* together", "[texturemapping][pr3]")
{
    DynamicPrintConfig src = DynamicPrintConfig::full_print_config();
    src.set_key_value("texture_mapping_definitions", new ConfigOptionString("zones-v1"));
    src.set_key_value("texture_mapping_global_settings", new ConfigOptionString("{\"enabled\":true}"));
    src.set_key_value("texture_mapping_background_color", new ConfigOptionString("#AABBCCDD"));
    src.set_key_value("texture_mapping_outer_wall_gradient_global_strength", new ConfigOptionFloat(42.0));
    src.set_key_value("texture_mapping_outer_wall_gradient_max_line_width", new ConfigOptionFloat(0.88));
    src.set_key_value("texture_mapping_outer_wall_gradient_min_line_width", new ConfigOptionFloat(0.22));
    src.set_key_value("paint_depth_mode", new ConfigOptionEnum<PaintDepthMode>(pdmMillimeters));
    src.set_key_value("paint_depth_mm", new ConfigOptionFloat(1.25));
    src.set_key_value("paint_depth_walls", new ConfigOptionInt(4));

    Model model;
    ModelObject *object = model.add_object();
    REQUIRE(object != nullptr);
    object->add_volume(make_cube(10., 10., 10.));
    model.add_default_instances();

    const boost::filesystem::path tmp_path = boost::filesystem::temp_directory_path() / "imagemap_full_pr3_c6.3mf";
    const std::string tmp = tmp_path.string();
    StoreParams store;
    store.path = tmp.c_str();
    store.model = &model;
    store.config = &src;
    store.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence;
    REQUIRE(store_bbs_3mf(store));

    DynamicPrintConfig dst;
    Model dst_model;
    ConfigSubstitutionContext ctx{ForwardCompatibilitySubstitutionRule::Disable};
    PlateDataPtrs plates;
    std::vector<Preset *> presets;
    bool is_bbl = false;
    Semver file_version;
    const bool loaded = load_bbs_3mf(tmp.c_str(), &dst, &ctx, &dst_model, &plates, &presets, &is_bbl, &file_version, nullptr,
                                     LoadStrategy::LoadModel | LoadStrategy::LoadConfig);
    boost::filesystem::remove(tmp_path);
    release_PlateData_list(plates);

    REQUIRE(loaded);
    REQUIRE(dst.has("texture_mapping_definitions"));
    CHECK(dst.option<ConfigOptionString>("texture_mapping_definitions")->value == "zones-v1");
    CHECK(dst.option<ConfigOptionString>("texture_mapping_global_settings")->value == "{\"enabled\":true}");
    CHECK(dst.option<ConfigOptionString>("texture_mapping_background_color")->value == "#AABBCCDD");
    CHECK(dst.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_global_strength")->value == 42.0);
    CHECK(dst.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_max_line_width")->value == 0.88);
    CHECK(dst.option<ConfigOptionFloat>("texture_mapping_outer_wall_gradient_min_line_width")->value == 0.22);
    REQUIRE(dst.has("paint_depth_mode"));
    CHECK(dst.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value == pdmMillimeters);
    CHECK(std::abs(dst.option<ConfigOptionFloat>("paint_depth_mm")->value - 1.25) < 1e-6);
    CHECK(dst.option<ConfigOptionInt>("paint_depth_walls")->value == 4);
}

TEST_CASE("C7: Remap snapshot/apply is wired for ImageTexture and region paint", "[texturemapping][pr3][remap]")
{
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 10.));
    REQUIRE(volume != nullptr);

    const size_t tri_count = volume->mesh().its.indices.size();
    REQUIRE(tri_count > 0);
    volume->imported_texture_width = 2;
    volume->imported_texture_height = 2;
    volume->imported_texture_rgba.assign(16, uint8_t(255));
    volume->imported_texture_uv_valid.assign(tri_count, uint8_t(1));
    volume->imported_texture_uvs_per_face.assign(tri_count * 6, 0.5f);

    const SimplifyTextureDataSnapshot snapshot = snapshot_simplify_texture_data(*volume);
    CHECK(snapshot.source == SimplifyColorSource::ImageTexture);

    SimplifyTextureDataResult result = remap_simplify_texture_data(snapshot, volume->mesh().its);
    apply_simplify_texture_data_result(*volume, std::move(result));
    CHECK(volume->imported_texture_uv_valid.size() == tri_count);
    CHECK(volume->imported_texture_uvs_per_face.size() >= tri_count * 6);

    volume->mmu_segmentation_facets.reserve(int(tri_count));
    volume->mmu_segmentation_facets.set_triangle_from_string(0, "4");
    const SimplifyTextureDataSnapshot region_snapshot = snapshot_simplify_texture_data(*volume);
    SimplifyTextureDataResult region_result = remap_simplify_texture_data(region_snapshot, volume->mesh().its);
    apply_simplify_texture_data_result(*volume, std::move(region_result));
}

TEST_CASE("load_gltf unsupported path does not crash", "[texturemapping][pr3]")
{
    CHECK_THROWS_AS(Model::read_from_file("missing.gltf"), Slic3r::RuntimeError);
    CHECK_THROWS_AS(Model::read_from_file("missing.glb"), Slic3r::RuntimeError);
    try {
        Model::read_from_file("missing.glb");
        FAIL("GLB import should throw");
    } catch (const std::runtime_error &err) {
        const std::string msg = err.what();
        CHECK(msg.find("GLTF") != std::string::npos);
    }
}

TEST_CASE("PR5 conservative preview defaults keep heaviest halftone paths off", "[texturemapping][pr5]")
{
    CHECK_FALSE(TextureMappingZone::DefaultDitheringEnabled);
    CHECK_FALSE(TextureMappingZone::DefaultPreviewSimulateColors);
    CHECK(TextureMappingZone::DefaultPreviewLimitResolution);
    CHECK(TextureMappingZone::DefaultHighSpeedImageTextureSampling);
    CHECK(TextureMappingZone::DefaultCompactOffsetMode);

    TextureMappingZone zone;
    CHECK_FALSE(zone.dithering_enabled);
    TextureMappingGlobalSettings globals;
    CHECK_FALSE(globals.preview_simulate_colors);
    CHECK(globals.preview_limit_resolution);
}

TEST_CASE("C3 best-effort: same-object paint-depth facets + texture mapping does not crash", "[texturemapping][pr5][c3]")
{
    // Same-object paint-depth + TM is unsupported / best-effort (not a product claim).
    // This case only asserts: no crash, paint-depth helpers still compute a band,
    // Remap + TM manager remain callable. See docs/imagemap-full-known-gaps.md.

    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    REQUIRE(config.has("paint_depth_mode"));
    REQUIRE(config.has("paint_depth_mm"));
    REQUIRE(config.has("texture_mapping_definitions"));

    const float band = paint_depth_band_mm(pdmWalls, 2, 0.0, 0.45f, 0.4f, 0.45f);
    CHECK(band >= 0.f);

    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(make_cube(20., 20., 10.));
    REQUIRE(volume != nullptr);

    const size_t tri_count = volume->mesh().its.indices.size();
    REQUIRE(tri_count > 0);

    volume->mmu_segmentation_facets.reserve(int(tri_count));
    volume->mmu_segmentation_facets.set_triangle_from_string(0, "4");

    volume->imported_texture_width = 2;
    volume->imported_texture_height = 2;
    volume->imported_texture_rgba.assign(16, uint8_t(128));
    volume->imported_texture_uv_valid.assign(tri_count, uint8_t(1));
    volume->imported_texture_uvs_per_face.assign(tri_count * 6, 0.5f);

    const std::vector<std::string> colours = {"#FF0000", "#00FF00", "#0000FF"};
    TextureMappingManager mgr;
    TextureMappingZone *zone = mgr.add_zone(colours.size(), colours, int(TextureMappingZone::ImageTexture));
    REQUIRE(zone != nullptr);
    CHECK(mgr.resolve_zone_component(zone->zone_id, colours.size(), 0) >= 1);

    const SimplifyTextureDataSnapshot snapshot = snapshot_simplify_texture_data(*volume);
    CHECK(snapshot.source == SimplifyColorSource::ImageTexture);
    SimplifyTextureDataResult result = remap_simplify_texture_data(snapshot, volume->mesh().its);
    REQUIRE_NOTHROW(apply_simplify_texture_data_result(*volume, std::move(result)));

    PrintConfig print_config;
    print_config.filament_colour.values = colours;
    zone->component_ids = "12";
    const TextureMappingContoningSolver solver(*zone, print_config, {1, 2}, 0.2f);
    REQUIRE(solver.valid());

    LayerTools tools(0.);
    CHECK(tools.texture_mapping_extruders.empty());
    CHECK(tools.texture_mapping_component_extruders.empty());
}
