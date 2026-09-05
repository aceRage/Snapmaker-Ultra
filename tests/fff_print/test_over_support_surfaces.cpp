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
#include <string>
#include <vector>

#include "libslic3r/ExtrusionEntity.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/Print.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Surface.hpp"

#include "test_data.hpp"

using namespace Slic3r;
using namespace Slic3r::Test;

namespace {

const std::string kOverSupportType = ";TYPE:Bottom surface over support";
const std::string kBridgeType      = ";TYPE:Bridge";

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
        // Keep the G-code readable and the numbers comparable across the cases below.
        { "outer_wall_speed",                "40" },
        { "bridge_speed",                    "17" },
        { "gcode_comments",                  "0" },
    });
    return config;
}

std::string slice_leg_and_slab(const DynamicPrintConfig &config)
{
    Slic3r::Print print;
    Slic3r::Model model;
    ModelObject  *object = model.add_object();
    object->name = "leg_and_slab";
    object->add_volume(leg_and_slab());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
    print.apply(model, config);
    return Slic3r::Test::gcode(print);
}

// Every ";TYPE:<name>" that appears in the G-code, in first-seen order.
std::vector<std::string> feature_types(const std::string &gcode)
{
    std::vector<std::string> out;
    size_t                   pos = 0;
    while ((pos = gcode.find(";TYPE:", pos)) != std::string::npos) {
        size_t      end  = gcode.find('\n', pos);
        std::string type = gcode.substr(pos, (end == std::string::npos ? gcode.size() : end) - pos);
        while (! type.empty() && (type.back() == '\r' || type.back() == ' '))
            type.pop_back();
        if (std::find(out.begin(), out.end(), type) == out.end())
            out.emplace_back(type);
        pos = (end == std::string::npos) ? gcode.size() : end;
    }
    return out;
}

struct BlockStats
{
    double   e_total   = 0.;   // extruded filament, mm
    double   xy_total  = 0.;   // travelled distance while extruding, mm
    unsigned segments  = 0;
    std::vector<double> feedrates;   // every F seen inside the blocks, mm/min
};

// Walk the G-code and accumulate the extruding moves that belong to the blocks introduced by
// `type_tag`, i.e. from that ";TYPE:" line until the next ";TYPE:" line. Absolute E only, which is
// what the test config produces.
BlockStats stats_for_type(const std::string &gcode, const std::string &type_tag)
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

        if (line.rfind(";TYPE:", 0) == 0) {
            inside = (line == type_tag);
            continue;
        }
        if (line.rfind("G1 ", 0) != 0 && line.rfind("G0 ", 0) != 0)
            continue;

        // Parse the words we care about. Missing word = unchanged.
        double nx = x, ny = y, ne = e, nf = feed;
        bool   has_x = false, has_y = false, has_e = false, has_f = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            char c = line[i];
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

        if (inside && has_f)
            stats.feedrates.push_back(nf);

        if (inside && have_pos && has_e && ne > e && (has_x || has_y)) {
            const double d = std::hypot(nx - x, ny - y);
            if (d > 1e-6) {
                stats.e_total += ne - e;
                stats.xy_total += d;
                ++stats.segments;
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
    ModelObject  *object = model.add_object();
    object->name = "leg_and_slab";
    object->add_volume(leg_and_slab());
    object->add_instance();
    object->ensure_on_bed();
    print.auto_assign_extruders(object);
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

    const std::string        gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    // The slab's underside is over support and is still called a bridge - today's behaviour.
    CHECK(std::find(types.begin(), types.end(), kBridgeType) != types.end());
    // And the new role never appears, not as a feature type and not anywhere else.
    CHECK(gcode.find("Bottom surface over support") == std::string::npos);
    for (const std::string &type : types)
        CHECK(type != kOverSupportType);

    // Nothing produced the new surface type either.
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: the switch is off by default", "[OverSupport]")
{
    PrintObjectConfig defaults;
    CHECK_FALSE(defaults.over_support_surfaces.value);
    CHECK(defaults.over_support_flow.value == Approx(1.));
    CHECK(defaults.over_support_speed.value == Approx(0.));
}

// -------------------------------------------------------------------------------- ON mode

TEST_CASE("over_support: with the switch on the supported bottoms carry the new role", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" } });

    const std::string              gcode = slice_leg_and_slab(config);
    const std::vector<std::string> types = feature_types(gcode);

    REQUIRE(std::find(types.begin(), types.end(), kOverSupportType) != types.end());
    CHECK(object_has_surface_type(config, stBottomOverSupport));

    // The role has real extrusion behind it, not a stray marker.
    const BlockStats stats = stats_for_type(gcode, kOverSupportType);
    CHECK(stats.segments > 20);
    CHECK(stats.xy_total > 50.);
}

TEST_CASE("over_support: speed 0 follows the outer wall speed, a set value is used as is", "[OverSupport]")
{
    // 0 = match the walls around the surface.
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "0" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportType);
        REQUIRE(! stats.feedrates.empty());
        // outer_wall_speed 40 mm/s -> 2400 mm/min. Slow-down-for-first-layers does not reach the
        // slab, which starts 10 mm up.
        for (double f : stats.feedrates)
            CHECK(f == Approx(40. * 60.).margin(1.));
    }
    // A set value is used as it is - and it is emphatically not bridge_speed (17 mm/s here).
    {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "over_support_speed", "23" } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportType);
        REQUIRE(! stats.feedrates.empty());
        for (double f : stats.feedrates)
            CHECK(f == Approx(23. * 60.).margin(1.));
    }
}

TEST_CASE("over_support: the flow ratio scales the extrusion per millimetre", "[OverSupport]")
{
    auto e_per_mm = [](const char *flow) {
        DynamicPrintConfig config = base_config();
        config.set_deserialize_strict({ { "over_support_surfaces", "1" },
                                        { "over_support_speed", "20" },
                                        { "over_support_flow", flow } });
        const BlockStats stats = stats_for_type(slice_leg_and_slab(config), kOverSupportType);
        REQUIRE(stats.xy_total > 50.);
        return stats.e_total / stats.xy_total;
    };

    const double full = e_per_mm("1");
    const double half = e_per_mm("0.5");
    REQUIRE(full > 0.);
    // The line geometry is identical either way, so the ratio is the ratio of the two settings.
    CHECK(half / full == Approx(0.5).epsilon(0.02));
}

// ------------------------------------------------------------- what stays a true bridge

TEST_CASE("over_support: nothing is reclassified when the generator will not support the bridge", "[OverSupport]")
{
    // bridge_no_support tells the normal generator to leave bridges unsupported, so the slab's
    // underside really is a bridge and has to keep the bridge settings.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "bridge_no_support", "1" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find("Bottom surface over support") == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: with supports off every bottom stays a bridge", "[OverSupport]")
{
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "enable_support", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find("Bottom surface over support") == std::string::npos);
    CHECK_FALSE(object_has_surface_type(config, stBottomOverSupport));
}

TEST_CASE("over_support: a zero top Z distance is left to the soluble path", "[OverSupport]")
{
    // At a zero gap the bottom is already stBottom (the soluble-interface rule), so there is
    // nothing for this feature to reclassify and it must stay out of the way.
    DynamicPrintConfig config = base_config();
    config.set_deserialize_strict({ { "over_support_surfaces", "1" }, { "support_top_z_distance", "0" } });

    const std::string gcode = slice_leg_and_slab(config);
    CHECK(gcode.find("Bottom surface over support") == std::string::npos);
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
    CHECK(name == "Bottom surface over support");
    CHECK(ExtrusionEntity::string_to_role(name) == erBottomSurfaceOverSupport);
    // The plain bottom surface must not be swallowed by the longer name.
    CHECK(ExtrusionEntity::string_to_role("Bottom surface") == erBottomSurface);
    // It is solid infill, and it is emphatically not a bridge.
    CHECK(is_solid_infill(erBottomSurfaceOverSupport));
    CHECK_FALSE(is_bridge(erBottomSurfaceOverSupport));
}
