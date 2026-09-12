#include <catch2/catch.hpp>

#include "slic3r/Utils/ElegooCanvas.hpp"

#include <string>

using namespace Slic3r;

// CANVAS is Elegoo's multi-material unit for the Centauri Carbon / Centauri 2.
// src/slic3r/Utils/ElegooCanvas.cpp parses what command 324 reports and maps it
// onto the filament families this tree knows; ElegooCanvasQuery.cpp does the
// WebSocket round trip and is deliberately not linked here, so every case below
// runs without a printer.
//
// Ported from upstream OrcaSlicer PR #14885, which is still open and ships no
// tests of its own.

SCENARIO("CANVAS colours are normalised to RRGGBBAA", "[ElegooCanvas]")
{
    GIVEN("the shapes a printer may report a colour in")
    {
        // Alpha is appended when the printer omits it.
        CHECK(canvas_normalize_color("#FF8800")   == "FF8800FF");
        CHECK(canvas_normalize_color("0xFF8800")  == "FF8800FF");
        CHECK(canvas_normalize_color("0XFF8800")  == "FF8800FF");
        CHECK(canvas_normalize_color("FF8800")    == "FF8800FF");
        // An explicit alpha is kept as given.
        CHECK(canvas_normalize_color("FF880080")  == "FF880080");
        // Case and surrounding whitespace do not matter.
        CHECK(canvas_normalize_color("  ff8800 ") == "FF8800FF");
    }

    GIVEN("input that is not a colour")
    {
        THEN("the slot gets a transparent placeholder rather than an exception")
        {
            CHECK(canvas_normalize_color("")        == "00000000");
            CHECK(canvas_normalize_color("nope")    == "00000000");
            CHECK(canvas_normalize_color("FF88")    == "00000000");
            CHECK(canvas_normalize_color("FF8800A") == "00000000");
        }
    }
}

SCENARIO("CANVAS material names collapse onto a filament family", "[ElegooCanvas]")
{
    GIVEN("the variants Elegoo sells")
    {
        CHECK(canvas_normalize_filament_type("PLA")      == "PLA");
        CHECK(canvas_normalize_filament_type("PLA Silk") == "PLA");
        CHECK(canvas_normalize_filament_type("PLA-CF")   == "PLA");
        CHECK(canvas_normalize_filament_type("PLA Matte")== "PLA");
        CHECK(canvas_normalize_filament_type("PETG")     == "PETG");
        CHECK(canvas_normalize_filament_type("PETG-CF")  == "PETG");
        CHECK(canvas_normalize_filament_type("ABS")      == "ABS");
        CHECK(canvas_normalize_filament_type("ASA-CF")   == "ASA");
        CHECK(canvas_normalize_filament_type("TPU 95A")  == "TPU");
    }

    GIVEN("names where one family's letters contain another's")
    {
        THEN("the longer name wins")
        {
            // "PET-CF" must not stop at PET and become a different family, and
            // "PC-FR" must not be read as a nylon on the strength of its P.
            CHECK(canvas_normalize_filament_type("PET-CF")  == "PETG");
            CHECK(canvas_normalize_filament_type("PC")      == "PC");
            CHECK(canvas_normalize_filament_type("PC-FR")   == "PC");
            CHECK(canvas_normalize_filament_type("PAHT-CF") == "PA");
            CHECK(canvas_normalize_filament_type("Nylon")   == "PA");
        }
    }

    GIVEN("a material this tree has no family for")
    {
        THEN("the name is passed through upper-cased rather than guessed at")
        {
            CHECK(canvas_normalize_filament_type("pps-cf") == "PPS-CF");
        }
    }
}

SCENARIO("A CANVAS family maps to an OrcaFilamentLibrary preset id", "[ElegooCanvas]")
{
    GIVEN("the families the normaliser produces")
    {
        // These ids exist in resources/profiles/OrcaFilamentLibrary.
        CHECK(canvas_generic_filament_id("PLA")  == "OGFL99");
        CHECK(canvas_generic_filament_id("PETG") == "OGFG99");
        CHECK(canvas_generic_filament_id("ABS")  == "OGFB99");
        CHECK(canvas_generic_filament_id("ASA")  == "OGFB98");
        CHECK(canvas_generic_filament_id("TPU")  == "OGFU99");
    }

    GIVEN("a family with no generic counterpart")
    {
        THEN("the slot is left unmapped instead of pointing at the wrong preset")
        {
            CHECK(canvas_generic_filament_id("PPS-CF").empty());
            CHECK(canvas_generic_filament_id("").empty());
        }
    }
}

SCENARIO("A command-324 payload becomes a slot list", "[ElegooCanvas]")
{
    GIVEN("one connected unit with a loaded and an empty slot")
    {
        const std::string payload = R"({
            "canvas_list": [
                {
                    "connected": 1,
                    "tray_list": [
                        {"tray_id": 0, "status": 1, "filament_type": "PLA Silk",
                         "filament_color": "#FF8800", "max_nozzle_temp": 240},
                        {"tray_id": 1, "status": 0, "filament_type": ""}
                    ]
                }
            ]
        })";

        CanvasInfo info;
        std::string error;
        REQUIRE(canvas_parse_payload(payload, info, error));

        THEN("both slots are reported, only one carrying filament")
        {
            CHECK(info.unit_count == 1);
            REQUIRE(info.trays.size() == 2);

            CHECK(info.trays[0].slot_index == 0);
            CHECK(info.trays[0].has_filament);
            CHECK(info.trays[0].tray_type == "PLA");
            CHECK(info.trays[0].tray_color == "FF8800FF");
            CHECK(info.trays[0].tray_info_idx == "OGFL99");
            CHECK(info.trays[0].nozzle_temp == 240);

            CHECK(info.trays[1].slot_index == 1);
            CHECK(! info.trays[1].has_filament);
            CHECK(info.trays[1].tray_type.empty());
        }
    }

    GIVEN("a disconnected unit in front of a connected one")
    {
        const std::string payload = R"({
            "canvas_list": [
                {"connected": 0, "tray_list": [
                    {"tray_id": 0}, {"tray_id": 1}, {"tray_id": 2}, {"tray_id": 3}]},
                {"connected": 1, "tray_list": [
                    {"tray_id": 0, "status": 1, "filament_type": "PETG",
                     "filament_color": "00FF00"}]}
            ]
        })";

        CanvasInfo info;
        std::string error;
        REQUIRE(canvas_parse_payload(payload, info, error));

        THEN("the absent unit still consumes its slot numbers")
        {
            // Otherwise unplugging the first unit would renumber every slot
            // behind it and remap the whole print.
            CHECK(info.unit_count == 2);
            REQUIRE(info.trays.size() == 1);
            CHECK(info.trays[0].slot_index == 4);
            CHECK(info.trays[0].tray_type == "PETG");
            CHECK(info.trays[0].tray_info_idx == "OGFG99");
        }
    }

    GIVEN("the payload wrapped the way the firmware nests it")
    {
        const std::string once = R"({"Data": {"canvas_list": [
            {"connected": 1, "tray_list": [
                {"tray_id": 0, "status": 1, "filament_type": "ABS"}]}]}})";
        const std::string twice = R"({"Data": {"Data": {"canvas_list": [
            {"connected": 1, "tray_list": [
                {"tray_id": 0, "status": 1, "filament_type": "ABS"}]}]}}})";

        THEN("either nesting is unwrapped")
        {
            for (const std::string &p : {once, twice}) {
                CanvasInfo info;
                std::string error;
                REQUIRE(canvas_parse_payload(p, info, error));
                REQUIRE(info.trays.size() == 1);
                CHECK(info.trays[0].tray_type == "ABS");
            }
        }
    }

    GIVEN("replies that carry no usable slot list")
    {
        THEN("the parse fails with a reason instead of inventing slots")
        {
            CanvasInfo info;
            std::string error;

            CHECK(! canvas_parse_payload("not json", info, error));
            CHECK(! error.empty());

            // the ACK frame the printer sends before the payload
            CHECK(! canvas_parse_payload(R"({"Status": "ok"})", info, error));
            CHECK(! error.empty());

            CHECK(! canvas_parse_payload(R"({"canvas_list": []})", info, error));
            CHECK(! error.empty());
        }
    }
}
