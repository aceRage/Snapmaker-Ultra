#include <catch2/catch.hpp>

#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Preset.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"

using namespace Slic3r;

namespace {

Preset make_printer_preset(const std::string &default_bed_type)
{
    Preset printer(Preset::TYPE_PRINTER, "Test Printer");
    printer.config.set_key_value("default_bed_type", new ConfigOptionString(default_bed_type));
    return printer;
}

Preset &add_inmemory_printer(PresetBundle &bundle, const std::string &name)
{
    DynamicPrintConfig cfg;
    Preset &printer = bundle.printers.load_preset(std::string(), name, cfg, false);
    printer.is_system = true;
    printer.is_visible = true;
    return printer;
}

} // namespace

TEST_CASE("get_default_bed_type parses symbolic and legacy values", "[Preset][BedType]")
{
    SECTION("Symbolic Textured PEI Plate is btPTE, not atoi 0") {
        Preset printer = make_printer_preset("Textured PEI Plate");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPTE);
    }

    SECTION("Symbolic Engineering Plate is btEP") {
        Preset printer = make_printer_preset("Engineering Plate");
        REQUIRE(printer.get_default_bed_type(nullptr) == btEP);
    }

    SECTION("Legacy numeric default_bed_type stays valid") {
        Preset printer = make_printer_preset("4");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPTE);
    }

    SECTION("Invalid default_bed_type falls back to PEI") {
        Preset printer = make_printer_preset("not-a-bed");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPEI);
    }

    SECTION("Empty default_bed_type uses the generic PEI fallback") {
        Preset printer = make_printer_preset("");
        REQUIRE(printer.get_default_bed_type(nullptr) == btPEI);
    }
}

TEST_CASE("Selected printer uses its default or saved bed type", "[Preset][Bundle]")
{
    PresetBundle bundle;
    Preset &printer = add_inmemory_printer(bundle, "Test Printer");
    printer.config.option<ConfigOptionString>("printer_model", true)->value = "TEST-MODEL";
    printer.config.option<ConfigOptionString>("printer_variant", true)->value = "0.4";
    printer.config.option<ConfigOptionString>("default_bed_type", true)->value = "Engineering Plate";

    AppConfig app_config;
    app_config.set("curr_bed_type", std::to_string(static_cast<int>(btPTE)));
    PresetBundle::PresetPreferences preferred_selection;
    BedType expected_bed_type;

    SECTION("New printer uses its symbolic default") {
        expected_bed_type = btEP;
        preferred_selection = {"TEST-MODEL", "0.4"};
    }
    SECTION("Re-enabled printer uses its saved selection") {
        expected_bed_type = btPC;
        preferred_selection = {"TEST-MODEL", "0.4"};
        app_config.set_printer_setting("Test Printer", "curr_bed_type",
                                       std::to_string(static_cast<int>(expected_bed_type)));
    }
    SECTION("Existing printer keeps its saved selection after presets reload") {
        expected_bed_type = btPCT;
        app_config.set("presets", PRESET_PRINTER_NAME, "Test Printer");
        app_config.set_printer_setting("Test Printer", "curr_bed_type",
                                       std::to_string(static_cast<int>(expected_bed_type)));
    }

    bundle.load_selections(app_config, preferred_selection);
    bundle.export_selections(app_config);

    CHECK(bundle.project_config.opt_enum<BedType>("curr_bed_type") == expected_bed_type);
    CHECK(app_config.get_printer_setting("Test Printer", "curr_bed_type") == std::to_string(static_cast<int>(expected_bed_type)));
}
