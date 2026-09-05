// Ultra (over-support surfaces) - the gate for docs/superpowers/specs/2026-09-05-over-support-surfaces.md.
//
// Two halves, and the first one is the important one:
//
//  * OFF mode is the hard gate. With over_support_surfaces off, the classifier must not run at
//    all: the same object must still emit only bridge roles for the faces that land on support,
//    and the string "Bottom surface over support" must not appear anywhere in the G-code.
//  * ON mode: the faces that land on support carry the new role, their feedrate follows
//    over_support_speed (0 = the outer wall speed of the layer), their extrusion per millimetre
//    scales with over_support_flow, and a face that the generator will NOT support - blocked, or
//    under a support type that refuses to support bridges - stays a true bridge.

#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include <boost/filesystem.hpp>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;
// tests/CLAUDE.md: floating point comparisons go through the matchers, never through Approx.
using Catch::Matchers::WithinAbs;
using Catch::Matchers::WithinRel;

namespace {

const std::string kOverSupportRole = "Bottom surface over support";
const std::string kBridgeRole      = "Bridge";

// The feature-type marker is ";TYPE:<role>" on a generic printer and "; FEATURE: <role>" on a BBL
// one (GCodeProcessor::Reserved_Tags vs Reserved_Tags_compatible, picked by s_IsBBLPrinter). Return
// the role name for either, and an empty string for anything else.
std::string role_marker(const std::string &line)
{
    static const std::string generic = ";TYPE:";
    static const std::string bbl     = "; FEATURE: ";
    std::string              name;
    if (line.rfind(bbl, 0) == 0)
        name = line.substr(bbl.size());
    else if (line.rfind(generic, 0) == 0)
        name = line.substr(generic.size());
    else
        return std::string();
    while (! name.empty() && (name.back() == '\r' || name.back() == ' '))
        name.pop_back();
    return name;
}

// A 2x2x10 leg on the bed carrying a 20x20x4 slab 10 mm above it, one volume. The slab's whole
// underside hangs over air, so with automatic supports on the generator fills that gap - which
// makes the underside the exact surface this feature is about. A wide flat slab on purpose: the
// area has to be big enough to survive the classifier's opening and produce measurable extrusion.
TriangleMesh leg_and_slab()
{
    TriangleMesh mesh = Slic3r::make_cube(2., 2., 10.);
    TriangleMesh slab = Slic3r::make_cube(20., 20., 4.);
    slab.translate(0.f, 0.f, 10.f);
    mesh.merge(slab);
    return mesh;
}

DynamicPrintConfig base_config()
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        { "enable_support",                  "1" },
        { "support_type",                    "normal(auto)" },
        { "support_style",                   "grid" },
        { "support_top_z_distance",          "0.2" },
        { "support_interface_top_layers",    "2" },
        { "support_interface_bottom_layers", "2" },
        { "support_interface_spacing",       "0.5" },
        { "support_on_build_plate_only",     "0" },
        // The generator must be willing to put support under the slab, or there would be nothing
        // here to reclassify and every ON-mode case below would pass for the wrong reason.
        { "bridge_no_support",               "0" },
        // Keep the numbers comparable across the cases below. The cooling slowdown scales every
        // feedrate on a small, fast layer - the slab is exactly that - so it has to be off before
        // any F in the G-code can be compared against a setting.
        { "outer_wall_speed",                "40" },
        { "bridge_speed",                    "17" },
        { "slow_down_layer_time",            "0" },
        { "slow_down_layers",                "0" },
        { "reduce_fan_stop_start_freq",      "0" },
        // GCode::_extrude caps every speed at filament_max_volumetric_speed / mm3_per_mm. The
        // default is low enough to cap 40 mm/s on this line width, which would make the F values
        // say something about the filament rather than about the setting under test.
        { "filament_max_volumetric_speed",   "200" },
    });
    return config;
}

void add_leg_and_slab(Slic3r::Model &model)
{
    ModelObject *object = model.add_object();
    object->name = "leg_and_slab";
    object->add_volume(leg_and_slab());
    object->add_instance();
    object->ensure_on_bed();
}

std::string slice_leg_and_slab(const DynamicPrintConfig &config)
{
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_slab(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    // Not Slic3r::Test::gcode(): that helper exports to a bare filename, and this fork's
    // GCode::_do_export creates the output's parent directory when it is missing - with a bare
    // filename the parent is "", and create_directory("") throws. It also passes a null
    // GCodeProcessorResult, which Print::export_gcode dereferences unconditionally on its last
    // line. Both are pre-existing; test_printgcode fails on the first of them on this tree.
    const boost::filesystem::path out =
        boost::filesystem::temp_directory_path() /
        boost::filesystem::unique_path("over_support_%%%%%%%%.gcode");
    GCodeProcessorResult result;
    print.export_gcode(out.string(), &result, nullptr);
    std::ifstream in(out.string());
    std::string   text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    in.close();
    boost::filesystem::remove(out);
    return text;
}

// Every feature type name that appears in the G-code, in first-seen order.
std::vector<std::string> feature_types(const std::string &gcode)
{
    std::vector<std::string> out;
    std::size_t              line_begin = 0;
    while (line_begin <= gcode.size()) {
        std::size_t line_end = gcode.find('\n', line_begin);
        if (line_end == std::string::npos)
            line_end = gcode.size();
        const std::string name = role_marker(gcode.substr(line_begin, line_end - line_begin));
        line_begin = line_end + 1;
        if (! name.empty() && std::find(out.begin(), out.end(), name) == out.end())
            out.emplace_back(name);
    }
    return out;
}

struct BlockStats
{
    double              e_total  = 0.;   // extruded filament, mm
    double              xy_total = 0.;   // travelled distance while extruding, mm
    unsigned            segments = 0;
    std::vector<double> feedrates;       // every F seen inside the blocks, mm/min
};

// Walk the G-code and accumulate the extruding moves that belong to the blocks introduced by
// `role_name`, i.e. from that feature-type marker until the next one. Absolute E only, which is
// what the test config produces.
BlockStats stats_for_type(const std::string &gcode, const std::string &role_name)
{
    BlockStats stats;
    bool       inside = false;
    double     x = 0., y = 0., e = 0.;
    bool       have_pos = false;
    double     feed = 0.;

    std::size_t line_begin = 0;
    while (line_begin <= gcode.size()) {
        std::size_t line_end = gcode.find('\n', line_begin);
        if (line_end == std::string::npos)
            line_end = gcode.size();
        std::string line = gcode.substr(line_begin, line_end - line_begin);
        line_begin       = line_end + 1;
        if (! line.empty() && line.back() == '\r')
            line.pop_back();

        if (const std::string marker = role_marker(line); ! marker.empty()) {
            inside = (marker == role_name);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;

        // Parse the words this test cares about. A missing word means unchanged.
        double nx = x, ny = y, ne = e, nf = feed;
        bool   has_x = false, has_y = false, has_e = false, has_f = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c != 'X' && c != 'Y' && c != 'E' && c != 'F')
                continue;
            double v = 0.;
            try {
                v = std::stod(line.substr(i + 1));
            } catch (...) {
                continue;
            }
            switch (c) {
            case 'X': nx = v; has_x = true; break;
            case 'Y': ny = v; has_y = true; break;
            case 'E': ne = v; has_e = true; break;
            case 'F': nf = v; has_f = true; break;
            default: break;
            }
        }

        if (inside && have_pos && has_e && ne > e && (has_x || has_y)) {
            const double d = std::hypot(nx - x, ny - y);
            if (d > 1e-6) {
                stats.e_total += ne - e;
                stats.xy_total += d;
                ++stats.segments;
                // Only the feedrate of an EXTRUDING move. A block also carries travels (F7200)
                // and the retract/wipe (F1800), which say nothing about the print speed. nf
                // carries the last F forward, so a move that sets no F of its own still counts.
                stats.feedrates.push_back(nf);
            }
        }
        x = nx; y = ny; feed = nf;
        if (has_e)
            e = ne;
        if (has_x || has_y)
            have_pos = true;
    }
    return stats;
}

// The surface types the object's fill surfaces carry, straight out of the slicing pipeline.
bool object_has_surface_type(const DynamicPrintConfig &config, SurfaceType type)
{
    Slic3r::Print print;
    Slic3r::Model model;
    add_leg_and_slab(model);
    print.auto_assign_extruders(model.objects.front());
    print.apply(model, config);
    print.set_status_silent();
    print.process();

    REQUIRE(! print.objects().empty());
    for (const Layer *layer : print.objects().front()->layers())
        for (const LayerRegion *region : layer->regions())
            for (const Surface &surface : region->fill_surfaces.surfaces)
                if (surface.surface_type == type)
                    return true;
    return false;
}

} // namespace

// ------------------------------------------------------------------ OFF mode: the hard gate

TEST_CASE("over_support: with the switch off the roles are exactly today's", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    // The default, spelled out: this case must behave like a build that never heard of the feature.
    config.set_deserialize_strict({ { "over_support_surfaces", "0" } });

    const std::string              gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    // The slab's underside is over support and is still called a bridge - today's behaviour.
    CHECK(std::find(types.begin(), types.end(), kBridgeRole) != types.end());
    // And the new role never appears, not as a feature type and not anywhere else.
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK(std::find(types.begin(), types.end(), kOverSupportRole) == types.end());

    // Nothing produced the new surface type either.
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: the switch is off by default", "[OverSupport]")
{
    PrintObjectConfig defaults;
    CHECK_FALSE(defaults.over_support_surfaces.value);
    CHECK_THAT(defaults.over_support_flow.value, WithinAbs(1., 1e-9));
    CHECK_THAT(defaults.over_support_speed.value, WithinAbs(0., 1e-9));
}

// -------------------------------------------------------------------------------- ON mode

TEST_CASE("over_support: with the switch on the supported bottoms carry the new role", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" } });

    const std::string              gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    REQUIRE(std::find(types.begin(), types.end(), kOverSupportRole) != types.end());
    CHECK(object_has_surface_type(config, stBottomOverSupport));

    // The role has real extrusion behind it, not a stray marker.
    const BlockStats stats = stats_for_type(gcode, kOverSupportRole);
    CHECK(stats.segments > 20);
    CHECK(stats.xy_total > 50.);
}

TEST_CASE("over_support: speed 0 follows the outer wall speed, a set value is used as is", "[OverSupport]")
{
    // 0 = match the walls around the surface.
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "0" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(! stats.feedrates.empty());
        // outer_wall_speed 40 mm/s -> 2400 mm/min. The slab starts 10 mm up, so neither the first
        // layer nor the slow-down-for-first-layers ramp reaches it.
        for (double f : stats.feedrates)
            CHECK_THAT(f, WithinAbs(40. * 60., 1.));
    }
    // A set value is used as it is - and it is emphatically not bridge_speed (17 mm/s here).
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "23" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(! stats.feedrates.empty());
        for (double f : stats.feedrates)
            CHECK_THAT(f, WithinAbs(23. * 60., 1.));
    }
}

TEST_CASE("over_support: the flow ratio scales the extrusion per millimetre", "[OverSupport]")
{
    auto e_per_mm = [](const char *flow) {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" },
                                        { "over_support_speed", "20" },
                                        { "over_support_flow", flow } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportRole);
        REQUIRE(stats.xy_total > 50.);
        return stats.e_total / stats.xy_total;
    };

    const double full = e_per_mm("1");
    const double half = e_per_mm("0.5");
    REQUIRE(full > 0.);
    // The line geometry is identical either way, so the ratio is the ratio of the two settings.
    CHECK_THAT(half / full, WithinRel(0.5, 0.02));
}

// ------------------------------------------------------------- what stays a true bridge

TEST_CASE("over_support: nothing is reclassified when the generator will not support the bridge", "[OverSupport]")
{
    // bridge_no_support tells the normal generator to leave bridges unsupported, so the slab's
    // underside really is a bridge and has to keep the bridge settings.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "bridge_no_support", "1" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: with supports off every bottom stays a bridge", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "enable_support", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: a zero top Z distance is left to the soluble path", "[OverSupport]")
{
    // At a zero gap the bottom is already stBottom (the soluble-interface rule), so there is
    // nothing for this feature to reclassify and it must stay out of the way.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "support_top_z_distance", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find(kOverSupportRole) == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

// -------------------------------------------------------------------- keys and plumbing

TEST_CASE("over_support: the three keys are support-set and part eligible", "[OverSupport]")
{
    const std::vector<std::string> &part = Slic3r::part_support_keys();
    for (const char *key : { "over_support_surfaces", "over_support_flow", "over_support_speed" })
        CHECK(std::find(part.begin(), part.end(), std::string(key)) != part.end());
}

TEST_CASE("over_support: the role has a name and survives the round trip", "[OverSupport]")
{
    const std::string name = ExtrusionEntity::role_to_string(erBottomSurfaceOverSupport);
    CHECK(name == kOverSupportRole);
    CHECK(ExtrusionEntity::string_to_role(name) == erBottomSurfaceOverSupport);
    // The plain bottom surface must not be swallowed by the longer name.
    CHECK(ExtrusionEntity::string_to_role("Bottom surface") == erBottomSurface);
    // It is solid infill, and it is emphatically not a bridge.
    CHECK(is_solid_infill(erBottomSurfaceOverSupport));
    CHECK_FALSE(is_bridge(erBottomSurfaceOverSupport));
}
