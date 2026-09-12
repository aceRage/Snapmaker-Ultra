#include <catch2/catch.hpp>

#include "libslic3r/PlaceholderParser.hpp"
#include "libslic3r/PrintConfig.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <regex>
#include <string>

using namespace Slic3r;

// A multi-filament print on the Centauri Carbon 2 changes filament with CANVAS's
// M6211 macro, which the machine preset carries in change_filament_gcode:
//
//   M6211 T[next_extruder] L[flush_length] M{old_filament_e_feedrate}
//         N{new_filament_e_feedrate} Q[old_filament_temp]
//         R[nozzle_temperature_range_high] S[new_filament_temp]
//
// Every one of those arguments has to come out as a number. A placeholder that
// resolves to nothing leaves "M6211 T1 L M..." on the wire, which the firmware
// either rejects or - worse - reads as a different argument, so the tool change
// purges the wrong length. The cases below run the real template from the real
// preset through the PlaceholderParser with the same keys
// ToolChangeGcodeGenerator installs (GCode.cpp, the toolchange dyn_config), and
// insist on numeric arguments.

namespace {

// The change_filament_gcode of resources/profiles/Elegoo/machine/ECC2/
// "Elegoo Centauri Carbon 2 0.4 nozzle.json", read from the shipped profile so
// this test fails if the macro is ever edited into a different shape.
std::string read_cc2_change_filament_gcode()
{
    // tests run from the build tree; walk up to the repository root
    boost::filesystem::path dir = boost::filesystem::current_path();
    boost::filesystem::path preset;
    for (int up = 0; up < 6 && !dir.empty(); ++up) {
        const boost::filesystem::path candidate =
            dir / "resources" / "profiles" / "Elegoo" / "machine" / "ECC2" /
            "Elegoo Centauri Carbon 2 0.4 nozzle.json";
        if (boost::filesystem::exists(candidate)) {
            preset = candidate;
            break;
        }
        dir = dir.parent_path();
    }
    if (preset.empty())
        return std::string();

    boost::nowide::ifstream ifs(preset.string());
    const std::string body((std::istreambuf_iterator<char>(ifs)),
                            std::istreambuf_iterator<char>());

    // pull "change_filament_gcode": "....." and unescape the JSON string
    const std::string key = "\"change_filament_gcode\"";
    size_t k = body.find(key);
    if (k == std::string::npos)
        return std::string();
    size_t q = body.find('"', body.find(':', k + key.size()));
    if (q == std::string::npos)
        return std::string();
    std::string out;
    for (size_t i = q + 1; i < body.size(); ++i) {
        const char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            const char n = body[++i];
            if (n == 'n')       out.push_back('\n');
            else if (n == 't')  out.push_back('\t');
            else if (n == 'r')  out.push_back('\r');
            else                out.push_back(n);
        } else if (c == '"') {
            break;
        } else {
            out.push_back(c);
        }
    }
    return out;
}

// The keys GCode.cpp's toolchange path installs before expanding the template.
PlaceholderParser make_toolchange_parser()
{
    PlaceholderParser parser;

    // a full config so every key the template may touch resolves, with the
    // per-filament array the macro's R argument indexes set explicitly
    auto config = DynamicPrintConfig::full_print_config();
    config.set_deserialize_strict({
        {"nozzle_temperature_range_high", "240,260"},
        {"nozzle_diameter",               "0.4"},
    });
    parser.apply_config(config);

    parser.set("next_extruder",             new ConfigOptionInt(1));
    parser.set("old_filament_temp",         new ConfigOptionInt(220));
    parser.set("new_filament_temp",         new ConfigOptionInt(230));
    parser.set("old_filament_e_feedrate",   new ConfigOptionInt(20));
    parser.set("new_filament_e_feedrate",   new ConfigOptionInt(22));
    parser.set("flush_length",              new ConfigOptionFloat(96.5f));
    parser.set("max_layer_z",               new ConfigOptionFloat(12.4f));
    parser.set("printable_height",          new ConfigOptionFloat(256.f));

    return parser;
}

} // namespace

SCENARIO("The CC2 change_filament_gcode expands to a numeric M6211", "[ElegooCC2][Toolchange]")
{
    const std::string tmpl = read_cc2_change_filament_gcode();

    GIVEN("the M6211 macro the shipped CC2 0.4 preset carries")
    {
        REQUIRE_FALSE(tmpl.empty());
        REQUIRE(tmpl.find("M6211") != std::string::npos);

        WHEN("it is expanded with the keys a tool change provides")
        {
            PlaceholderParser parser = make_toolchange_parser();
            std::string out;
            REQUIRE_NOTHROW(out = parser.process(tmpl, 0));

            THEN("no placeholder survives unexpanded")
            {
                CHECK(out.find('[') == std::string::npos);
                CHECK(out.find('{') == std::string::npos);
            }

            THEN("every M6211 argument is a number")
            {
                // locate the emitted M6211 line
                const size_t at = out.find("M6211");
                REQUIRE(at != std::string::npos);
                size_t eol = out.find('\n', at);
                const std::string line =
                    out.substr(at, eol == std::string::npos ? std::string::npos : eol - at);

                INFO("expanded M6211 line: " << line);

                // T/L/M/N/Q/R/S each followed by a number (L may be fractional)
                const std::regex re(
                    R"(^M6211\s+T\s*(-?\d+)\s+L\s*(-?\d+(?:\.\d+)?)\s+M\s*(-?\d+(?:\.\d+)?)\s+)"
                    R"(N\s*(-?\d+(?:\.\d+)?)\s+Q\s*(-?\d+(?:\.\d+)?)\s+R\s*(-?\d+(?:\.\d+)?)\s+)"
                    R"(S\s*(-?\d+(?:\.\d+)?)\s*$)");
                std::smatch m;
                REQUIRE(std::regex_match(line, m, re));

                // and they carry the values the tool change handed in
                CHECK(m[1].str() == "1");     // T next_extruder
                CHECK(std::stod(m[2].str()) == Approx(96.5)); // L flush_length
                CHECK(std::stod(m[3].str()) == Approx(20));   // M old feedrate
                CHECK(std::stod(m[4].str()) == Approx(22));   // N new feedrate
                CHECK(std::stod(m[5].str()) == Approx(220));  // Q old temp
                // R indexes nozzle_temperature_range_high with no explicit index,
                // so it resolves against the parser's current extruder (0).
                CHECK(std::stod(m[6].str()) == Approx(240));
                CHECK(std::stod(m[7].str()) == Approx(230));  // S new temp
            }

            THEN("the tool change still selects the new extruder")
            {
                CHECK(out.find("T1") != std::string::npos);
            }
        }
    }
}
