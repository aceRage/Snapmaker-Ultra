// Image Row Phase 4, step 2: the 3D preview's colour swatch.
//
// The question this file answers, and pins: does the G-code preview colour each image-row RUN by
// the filament that run actually prints with, or by the region's nominal (un-split) filament?
//
// The answer, established here by measurement rather than by argument (phase 3's spec asserted it
// but never tested it): the preview is correct in BOTH colour-relevant views, because the split
// reaches the preview through the G-code's own T<n> commands rather than through any
// ExtrusionEntity attribute.
//
// The chain, in the two places it matters (src/slic3r/GUI/GCodeViewer.cpp:3267-3299,
// `GCodeViewer::refresh_render_paths()`'s `extrusion_color` lambda):
//
//     case EViewType::Tool:       color = m_tools.m_tool_colors[path.extruder_id];
//     case EViewType::ColorPrint: color = m_tools.m_tool_colors[path.cp_color_id];
//
// `Path::extruder_id` / `Path::cp_color_id` are copied verbatim from the MoveVertex
// (GCodeViewer.cpp:228), and a MoveVertex takes them from the processor's live state at
// store_move_vertex() (GCodeProcessor.cpp:4806-4807):
//
//     m_extruder_id,       // <- set ONLY by process_T(), i.e. by a real T<n> in the G-code
//     m_cp_color.current,  // <- set to m_extruder_colors[id] at the same tool change
//
// and absent any M600/colour change, m_extruder_colors[i] == i (GCodeProcessor.cpp:977-979), so
// cp_color_id == extruder_id and the two views agree.
//
// So the ONE thing that has to be true for the swatch to be right is that each run's own physical
// filament reaches the G-code as a T<n> - which is what phase 3's ToolOrdering/GCode.cpp wiring
// does (ExtrusionEntityCollection::image_row_extruder_1based). This test asserts exactly that, at
// the preview's own data structure (GCodeProcessorResult::moves) rather than at the text of the
// G-code, so a regression anywhere between the split and the preview fails here:
//
//   1. more than one extruder_id appears among the top layer's erTopSolidInfill moves - i.e. the
//      preview does NOT paint the whole image-row surface one flat nominal colour;
//   2. extruder_id CHANGES several times within the top layer, at run boundaries, rather than
//      once (which is what a per-layer mixed-filament cycle - the un-split fallback - would give);
//   3. cp_color_id == extruder_id on every one of those moves, so the ColorPrint view shows the
//      same colours as the Tool view rather than GRAY() or a stale colour-change slot;
//   4. every extruder_id used is a PHYSICAL filament index (< the physical filament count) and one
//      of the row's own allowed filaments - never the row's VIRTUAL id, which would index into the
//      mixed-filament display-colour tail of m_tool_colors (Plater.cpp:24311) and paint the run
//      with the row's average colour instead of the run's own.
//
// The fixture is phase 3's own Bar B plaque (a 50 x 50 x 3 mm cube, ramp_kw.png over three
// filaments, Planar/Z), built through the same model-level API calls Plater::apply_image_fill()
// makes - the same arrangement tests/libslic3r/test_image_row_dialog.cpp's control case uses.
#include <catch2/catch.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/cstdio.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/ImageFill.hpp"
#include "libslic3r/MixedFilament.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TriangleMesh.hpp"

#include "test_data.hpp"

using namespace Slic3r;

namespace {

const std::vector<std::string> kColours = {"#000000", "#808080", "#FFFFFF"};

std::vector<uint8_t> read_ramp()
{
    // The fff_print tests' own TEST_DATA_DIR points at tests/data, the same tree
    // tests/libslic3r/test_image_row_dialog.cpp reads this fixture from.
    const std::string path = std::string(TEST_DATA_DIR) + "/image_fill/ramp_kw.png";
    boost::nowide::ifstream in(path, std::ios::binary);
    REQUIRE(in.good());
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}

// Same base config as tests/libslic3r/test_image_row_dialog.cpp's, plus what a G-code export needs
// that a bare process() does not: a printer that actually emits tool changes.
DynamicPrintConfig preview_test_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(unsigned(kColours.size()));
    config.set_num_filaments(unsigned(kColours.size()));
    config.option<ConfigOptionFloats>("filament_diameter")->values = std::vector<double>(kColours.size(), 1.75);
    config.option<ConfigOptionStrings>("filament_colour")->values  = kColours;
    config.option<ConfigOptionFloats>("nozzle_diameter")->values   = std::vector<double>(kColours.size(), 0.4);
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value    = 0.45;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent  = false;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("top_surface_line_width")->percent = false;
    // A single-extruder machine's GCodeWriter::toolchange() emits nothing at all
    // (GCodeWriter.cpp:462 requires multiple_extruders); this is a 3-filament tool changer, so the
    // T<n> commands the preview depends on are actually written.
    config.option<ConfigOptionBool>("single_extruder_multi_material")->value = true;
    // No wipe tower: it inserts its own extrusions between tool changes, which would make the
    // "moves between two top-solid runs" bookkeeping below measure the tower rather than the runs.
    config.option<ConfigOptionBool>("enable_prime_tower")->value = false;
    return config;
}

ImageFillParams ramp_params(Model &model)
{
    ImageFillParams p;
    p.asset      = model.image_assets.add(read_ramp());
    p.projection = ImageFillProjection::Planar;
    p.axis       = ImageFillAxis::Z;
    p.allowed    = {1, 2, 3};
    return p;
}

// The row half of Plater::apply_image_fill()'s Apply handler - identical in behaviour to
// tests/libslic3r/test_image_row_dialog.cpp's apply_image_row_binding().
int apply_image_row_binding(MixedFilamentManager &mgr, ModelVolume &volume, const ImageFillParams &params)
{
    const std::vector<int> ids = {1, 2, 3};
    mgr.add_custom_filament(1, 2, 50, kColours);
    REQUIRE_FALSE(mgr.mixed_filaments().empty());
    MixedFilament &row         = mgr.mixed_filaments().back();
    row.enabled                = true;
    row.distribution_mode      = int(MixedFilament::ImageWeighted);
    row.gradient_component_ids = MixedFilamentManager::encode_gradient_component_ids(
        std::vector<unsigned int>(ids.begin(), ids.end()));
    row.image_fill_ref = MixedFilamentManager::encode_image_fill_ref(params.to_string());
    REQUIRE_FALSE(row.image_fill_ref.empty());
    const unsigned int virtual_id =
        mgr.filament_id_from_mixed_index(mgr.mixed_filaments().size() - 1, kColours.size());
    REQUIRE(virtual_id != 0);
    volume.config.set_key_value("solid_infill_filament", new ConfigOptionInt(int(virtual_id)));
    volume.config.set_key_value("solid_infill_direction", new ConfigOptionFloat(0.));
    volume.config.set_key_value("top_surface_pattern", new ConfigOptionEnum<InfillPattern>(ipMonotonic));
    return int(virtual_id);
}

ModelVolume *make_plaque(Model &model)
{
    ModelObject *object = model.add_object();
    object->name        = "image row plaque (preview swatch)";
    ModelVolume *volume = object->add_volume(make_cube(50., 50., 3.));
    volume->name        = "plaque";
    object->add_instance();
    object->ensure_on_bed();
    return volume;
}

} // namespace

TEST_CASE("Image Row preview: each run's moves carry the run's own filament, not the region's",
          "[imagefill][ImageRow][preview]")
{
    Model                 model;
    ModelVolume          *volume = make_plaque(model);
    const ImageFillParams p      = ramp_params(model);

    MixedFilamentManager mgr;
    const int            virtual_id = apply_image_row_binding(mgr, *volume, p);

    DynamicPrintConfig config = preview_test_config();
    config.set_key_value("mixed_filament_definitions", new ConfigOptionString(mgr.serialize_custom_entries()));

    Print print;
    print.set_status_silent();
    print.apply(model, config);
    REQUIRE(print.objects().size() == 1);
    print.process();

    // Export to G-code AND capture the very GCodeProcessorResult the preview is built from -
    // Print::export_gcode()'s second parameter is the same result GLCanvas3D hands
    // GCodeViewer::refresh().
    const boost::filesystem::path out = Slic3r::Test::scratch_path(".gcode");
    GCodeProcessorResult          result;
    print.export_gcode(out.string(), &result, nullptr);
    boost::nowide::remove(out.string().c_str());

    REQUIRE_FALSE(result.moves.empty());

    // The top layer's Z: the highest Z at which any extruding move happens.
    float top_z = 0.f;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude)
            top_z = std::max(top_z, m.position.z());
    REQUIRE(top_z > 0.f);

    // Every top-solid-infill extruding move on that layer, in emission order.
    std::vector<const GCodeProcessorResult::MoveVertex *> top_moves;
    for (const auto &m : result.moves)
        if (m.type == EMoveType::Extrude && m.extrusion_role == erTopSolidInfill &&
            std::abs(m.position.z() - top_z) < 1e-4f)
            top_moves.push_back(&m);
    REQUIRE(top_moves.size() > 10);

    std::set<unsigned char> extruders_seen;
    int                     transitions = 0;
    for (size_t i = 0; i < top_moves.size(); ++i) {
        extruders_seen.insert(top_moves[i]->extruder_id);
        if (i > 0 && top_moves[i]->extruder_id != top_moves[i - 1]->extruder_id)
            ++transitions;
    }

    std::string seen;
    for (unsigned char e : extruders_seen)
        seen += std::to_string(int(e)) + " ";
    INFO("top-solid moves=" << top_moves.size() << " distinct extruder_id: " << seen
         << " transitions=" << transitions << " virtual_id=" << virtual_id
         << " physical filaments=" << kColours.size());

    // (1) The preview does not paint the whole image-row surface one flat colour: the Tool view's
    //     own field really does take more than one value across this surface.
    CHECK(extruders_seen.size() >= 2);

    // (2) It changes at RUN boundaries, not once per layer. An un-split image row falls back to
    //     MixedFilamentManager::resolve()'s per-layer cycle, which gives exactly ONE filament for
    //     the whole layer and therefore zero transitions within it. Several transitions is the
    //     signature of the split.
    CHECK(transitions >= 2);

    // (3) The ColorPrint view agrees with the Tool view. cp_color_id is m_extruder_colors[id] at
    //     the tool change, and with no colour change in this print that table is the identity - so
    //     any drift here means a colour-change slot leaked in and ColorPrint would show GRAY() or
    //     a wrong swatch where Tool shows the right one.
    for (const auto *m : top_moves)
        REQUIRE(m->cp_color_id == m->extruder_id);

    // (4) Every id is a PHYSICAL filament, and one the row was allowed to use. If the row's own
    //     VIRTUAL id (4, here) ever reached a move, m_tool_colors[4] would be the mixed row's
    //     average display colour (Plater::get_extruder_colors_from_plater_config appends those
    //     after the physical ones) - i.e. exactly the "region's nominal colour" failure this step
    //     is about, and the swatch would show one flat blended tone instead of the dither.
    for (unsigned char e : extruders_seen) {
        CHECK(size_t(e) < kColours.size());
        CHECK(int(e) + 1 != virtual_id);
    }
}
