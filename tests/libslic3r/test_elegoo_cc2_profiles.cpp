#include <catch2/catch.hpp>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Preset.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <map>
#include <string>

using namespace Slic3r;

// The Elegoo Centauri Carbon 2 / Centauri 2 machine presets imported from
// upstream carry a handful of keys no PrintConfig in either tree defines:
// support_multi_filament, support_wan_network, auto_toolchange_command and
// bed_texture_area (upstream's CC2 contribution was explicitly "NOT TESTED"),
// plus overhang_speed_classic and tree_support_branch_diameter_double_wall in
// the process bases.
//
// Those keys must not cost the preset the rest of its content. The guard that
// makes that true is the tail of PrintConfigDef::handle_legacy(): a key that is
// not in print_config_def has its opt_key cleared, so ConfigBase::load_from_json
// records it in unrecogized_keys and moves on, instead of letting
// set_deserialize_raw throw UnknownOptionException - which load_from_json's
// catch(std::exception&) would turn into a -1 return, abandoning every key in
// the file.
//
// These cases pin that behaviour down for both the string-valued and the
// array-valued shapes, because the two take different paths through
// load_from_json (set_deserialize vs the is_array branch).

namespace {

struct TempTree
{
    boost::filesystem::path root;

    explicit TempTree(const std::string &tag)
        : root(boost::filesystem::temp_directory_path() / "snorca_tests" /
               boost::filesystem::unique_path(tag + "_%%%%%%%%"))
    {
        boost::filesystem::create_directories(root);
    }
    ~TempTree()
    {
        boost::system::error_code ec;
        boost::filesystem::remove_all(root, ec);
    }

    boost::filesystem::path write(const std::string &rel, const std::string &body) const
    {
        const boost::filesystem::path path = root / rel;
        boost::filesystem::create_directories(path.parent_path());
        boost::nowide::ofstream ofs(path.string());
        ofs << body;
        ofs.close();
        return path;
    }
};

int load(DynamicPrintConfig &config, const boost::filesystem::path &path,
         ConfigSubstitutionContext &ctxt)
{
    std::map<std::string, std::string> key_values;
    std::string reason;
    return config.load_from_json(path.string(), ctxt, true, key_values, reason);
}

} // namespace

SCENARIO("A machine preset survives the CC2 keys PrintConfig does not define",
         "[Config][ElegooCC2]")
{
    GIVEN("a machine preset carrying the undefined CC2 keys next to real ones")
    {
        TempTree tree("elegoo_cc2");
        // The shape of machine/ECC2/Elegoo Centauri Carbon 2 0.4 nozzle.json:
        // undefined keys interleaved with the settings that must survive them.
        const auto preset = tree.write("machine/cc2.json", R"({
    "type": "machine",
    "name": "Elegoo Centauri Carbon 2 0.4 nozzle",
    "instantiation": "true",
    "nozzle_diameter": ["0.4"],
    "support_multi_filament": "1",
    "printable_height": "256",
    "support_wan_network": "1",
    "auto_toolchange_command": "0",
    "bed_texture_area": ["0x-10", "256x-10", "256x256", "0x256"],
    "printer_variant": "0.4",
    "extruder_offset": ["0x1.5"],
    "retract_restart_extra_toolchange": ["0.5"]
})");

        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::Enable);
        const int ret = load(config, preset, ctxt);

        THEN("the load succeeds rather than aborting the whole file")
        {
            REQUIRE(ret == 0);
        }
        THEN("every defined key around them still arrives")
        {
            REQUIRE(config.has("nozzle_diameter"));
            CHECK(config.opt_serialize("nozzle_diameter") == "0.4");
            REQUIRE(config.has("printable_height"));
            CHECK(config.opt_serialize("printable_height") == "256");
            REQUIRE(config.has("printer_variant"));
            CHECK(config.opt_serialize("printer_variant") == "0.4");
            REQUIRE(config.has("extruder_offset"));
            CHECK(config.opt_serialize("extruder_offset") == "0x1.5");
            // one of the keys the brief expected to be missing, but which this
            // tree already defines - it must round-trip, not be dropped
            REQUIRE(config.has("retract_restart_extra_toolchange"));
            CHECK(config.opt_serialize("retract_restart_extra_toolchange") == "0.5");
        }
        THEN("the undefined keys are dropped, not silently invented")
        {
            CHECK(! config.has("support_multi_filament"));
            CHECK(! config.has("support_wan_network"));
            CHECK(! config.has("auto_toolchange_command"));
            CHECK(! config.has("bed_texture_area"));
        }
        THEN("each undefined key is reported as unrecognised exactly once")
        {
            const auto &u = ctxt.unrecogized_keys;
            for (const char *k : {"support_multi_filament", "support_wan_network",
                                  "auto_toolchange_command", "bed_texture_area"}) {
                CHECK(std::count(u.begin(), u.end(), std::string(k)) == 1);
            }
        }
    }

    GIVEN("a process preset carrying the undefined CC2 process keys")
    {
        TempTree tree("elegoo_cc2_process");
        const auto preset = tree.write("process/cc2.json", R"({
    "type": "process",
    "name": "0.20mm Standard @Elegoo CC2 0.4 nozzle",
    "instantiation": "true",
    "layer_height": "0.2",
    "overhang_speed_classic": "1",
    "tree_support_branch_diameter_double_wall": "10",
    "internal_bridge_support_thickness": "0.8",
    "sparse_infill_density": "15%"
})");

        DynamicPrintConfig config;
        ConfigSubstitutionContext ctxt(ForwardCompatibilitySubstitutionRule::Enable);
        const int ret = load(config, preset, ctxt);

        THEN("the load succeeds and the real settings survive")
        {
            REQUIRE(ret == 0);
            REQUIRE(config.has("layer_height"));
            CHECK(config.opt_serialize("layer_height") == "0.2");
            REQUIRE(config.has("sparse_infill_density"));
            CHECK(config.opt_serialize("sparse_infill_density") == "15%");
        }
        THEN("the retired and the merely-undefined keys are both dropped")
        {
            // internal_bridge_support_thickness is in handle_legacy's explicit
            // ignore set; the other two only hit the print_config_def.has() tail.
            CHECK(! config.has("internal_bridge_support_thickness"));
            CHECK(! config.has("overhang_speed_classic"));
            CHECK(! config.has("tree_support_branch_diameter_double_wall"));
        }
    }
}
