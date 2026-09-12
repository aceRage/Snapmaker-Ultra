// The pure half of the CANVAS support declared in ElegooCanvas.hpp: parsing a
// command-324 payload and normalising what it carries. No networking lives here,
// so the unit tests can link this translation unit on its own - the WebSocket
// query sits in ElegooCanvasQuery.cpp.

#include "ElegooCanvas.hpp"

#include <nlohmann/json.hpp>
#include <boost/algorithm/string.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r {

namespace {

std::string trim_and_upper(const std::string& input)
{
    std::string result = input;
    boost::trim(result);
    std::transform(result.begin(), result.end(), result.begin(),
                   [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return result;
}

// The payload may arrive bare, wrapped once in "Data", or wrapped twice -
// which of the three depends on the firmware. Unwrap to whichever object
// actually carries canvas_list.
const nlohmann::json* find_canvas_object(const nlohmann::json& j)
{
    if (j.contains("canvas_list"))
        return &j;
    if (j.contains("Data") && j["Data"].is_object()) {
        const nlohmann::json& d1 = j["Data"];
        if (d1.contains("canvas_list"))
            return &d1;
        if (d1.contains("Data") && d1["Data"].is_object() && d1["Data"].contains("canvas_list"))
            return &d1["Data"];
    }
    return nullptr;
}

} // namespace

std::string canvas_normalize_color(const std::string& color)
{
    std::string value = color;
    boost::trim(value);

    if (value.size() >= 2 && (value.rfind("0x", 0) == 0 || value.rfind("0X", 0) == 0))
        value = value.substr(2);
    if (!value.empty() && value[0] == '#')
        value = value.substr(1);

    std::string normalized;
    for (char c : value) {
        if (std::isxdigit(static_cast<unsigned char>(c)))
            normalized.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
    }

    // RRGGBB is the common case; this tree stores colours with an alpha byte.
    if (normalized.size() == 6)
        normalized += "FF";

    if (normalized.size() != 8)
        return "00000000";

    return normalized;
}

std::string canvas_normalize_filament_type(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // Order matters where one name contains another: "PETG" must be tested
    // before "PET", and "PC" before "PA" so a "PC-FR" is not read as a nylon.
    // Everything else (PLA Silk, PLA-CF, ASA-CF, TPU 95A, PAHT-CF, PPA-CF...)
    // collapses onto its family by substring.
    if (upper.find("PLA") != std::string::npos)   return "PLA";
    if (upper.find("PETG") != std::string::npos)  return "PETG";
    if (upper.find("PET") != std::string::npos)   return "PETG";
    if (upper.find("ABS") != std::string::npos)   return "ABS";
    if (upper.find("ASA") != std::string::npos)   return "ASA";
    if (upper.find("TPU") != std::string::npos)   return "TPU";
    if (upper.find("PVA") != std::string::npos)   return "PVA";
    if (upper.find("HIPS") != std::string::npos)  return "HIPS";
    if (upper.find("PC") != std::string::npos)    return "PC";
    if (upper.find("PA") != std::string::npos || upper.find("NYLON") != std::string::npos)
        return "PA";

    return upper;
}

std::string canvas_generic_filament_id(const std::string& filament_type)
{
    const std::string upper = trim_and_upper(filament_type);

    // OrcaFilamentLibrary generic ids. Only the families the CANVAS normaliser
    // can actually produce are listed; anything else stays unmapped.
    if (upper == "PLA")   return "OGFL99";
    if (upper == "PETG")  return "OGFG99";
    if (upper == "ABS")   return "OGFB99";
    if (upper == "ASA")   return "OGFB98";
    if (upper == "TPU")   return "OGFU99";
    if (upper == "PA")    return "OGFN99";
    if (upper == "PC")    return "OGFC99";
    if (upper == "PVA")   return "OGFS99";
    if (upper == "HIPS")  return "OGFS98";

    return std::string();
}

bool canvas_parse_payload(const std::string& payload, CanvasInfo& info, std::string& error)
{
    info = CanvasInfo();

    nlohmann::json j = nlohmann::json::parse(payload, nullptr, false, true);
    if (j.is_discarded()) {
        error = "CANVAS reply is not valid JSON";
        return false;
    }

    const nlohmann::json* obj = find_canvas_object(j);
    if (obj == nullptr) {
        error = "CANVAS reply carries no canvas_list";
        return false;
    }

    const nlohmann::json& canvas_list = (*obj)["canvas_list"];
    if (!canvas_list.is_array() || canvas_list.empty()) {
        error = "canvas_list is empty or not an array";
        return false;
    }

    info.unit_count = static_cast<int>(canvas_list.size());

    // Slots are numbered across units, so a disconnected unit still consumes its
    // share of the numbering - otherwise unplugging unit 1 would silently
    // renumber every slot behind it.
    int slot_offset = 0;
    for (const auto& canvas : canvas_list) {
        if (!canvas.contains("tray_list") || !canvas["tray_list"].is_array())
            continue;

        const auto& tray_list = canvas["tray_list"];
        if (canvas.value("connected", 0) == 0) {
            slot_offset += static_cast<int>(tray_list.size());
            continue;
        }

        for (const auto& tray : tray_list) {
            CanvasTray t;
            t.slot_index = slot_offset + tray.value("tray_id", 0);

            const std::string filament_type = tray.value("filament_type", std::string());
            const int         status        = tray.value("status", -1);

            // status 0 means the slot is empty; a slot that reports no material
            // name is unusable even when the status says otherwise.
            t.has_filament = (status != 0 && !filament_type.empty());

            if (t.has_filament) {
                t.tray_type     = canvas_normalize_filament_type(filament_type);
                t.tray_color    = canvas_normalize_color(tray.value("filament_color", std::string()));
                t.nozzle_temp   = tray.value("max_nozzle_temp", 0);
                t.tray_info_idx = canvas_generic_filament_id(t.tray_type);
            }

            info.trays.emplace_back(std::move(t));
        }

        slot_offset += static_cast<int>(tray_list.size());
    }

    return true;
}

} // namespace Slic3r
