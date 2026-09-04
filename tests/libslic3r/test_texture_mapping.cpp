#include <catch2/catch.hpp>

#include "libslic3r/Format/ImportedTexture.hpp"
#include "libslic3r/ImageMapRawFilamentOffsetAtlas.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/ModelTextureDataRemap.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TextureMapping.hpp"
#include "libslic3r/TextureMappingContoning.hpp"
#include "libslic3r/TextureMappingOffset.hpp"

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
    const PrintConfig config;
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
