#include "Spoolman.hpp"

#include "Http.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include "slic3r/GUI/GUI_App.hpp"

namespace Slic3r {

using json = nlohmann::json;

static AppConfig* spoolman_app_config() { return GUI::wxGetApp().app_config; }

std::string Spoolman::base_url()
{
    AppConfig* cfg = spoolman_app_config();
    if (cfg == nullptr || cfg->get("spoolman_enabled") != "true")
        return {};
    std::string url = cfg->get("spoolman_url");
    boost::trim(url);
    if (url.empty())
        return {};
    if (!boost::istarts_with(url, "http"))
        url = "http://" + url;
    while (!url.empty() && url.back() == '/')
        url.pop_back();
    return url;
}

bool Spoolman::enabled() { return !base_url().empty(); }

static std::string json_string(const json& j, const char* key)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_string()) ? it->get<std::string>() : std::string();
}

static double json_number(const json& j, const char* key, double def)
{
    auto it = j.find(key);
    return (it != j.end() && it->is_number()) ? it->get<double>() : def;
}

bool Spoolman::get_spools(std::vector<SpoolmanSpool>& spools, std::string& error)
{
    const std::string base = base_url();
    if (base.empty()) {
        error = "Spoolman is not configured";
        return false;
    }
    bool ok = false;
    auto http = Http::get(base + "/api/v1/spool");
    http.timeout_connect(4)
        .timeout_max(10)
        .on_error([&](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
        })
        .on_complete([&](std::string body, unsigned) {
            try {
                json arr = json::parse(body);
                for (const json& js : arr) {
                    SpoolmanSpool s;
                    s.id               = int(json_number(js, "id", 0));
                    s.remaining_weight = json_number(js, "remaining_weight", -1.);
                    s.location         = json_string(js, "location");
                    s.archived         = js.value("archived", false);
                    if (auto fil = js.find("filament"); fil != js.end() && fil->is_object()) {
                        s.name      = json_string(*fil, "name");
                        s.material  = json_string(*fil, "material");
                        s.color_hex = json_string(*fil, "color_hex");
                        s.diameter  = json_number(*fil, "diameter", 1.75);
                        s.density   = json_number(*fil, "density", 1.24);
                        s.ext_temp  = int(json_number(*fil, "settings_extruder_temp", 0.));
                        s.bed_temp  = int(json_number(*fil, "settings_bed_temp", 0.));
                        if (auto vend = fil->find("vendor"); vend != fil->end() && vend->is_object())
                            s.vendor = json_string(*vend, "name");
                    }
                    if (!s.archived)
                        spools.push_back(std::move(s));
                }
                ok = true;
            } catch (const std::exception& e) {
                error = std::string("Invalid response: ") + e.what();
            }
        })
        .perform_sync();
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "Spoolman::get_spools failed: " << error;
    return ok;
}

bool Spoolman::use_weight(int spool_id, double grams, std::string& error)
{
    const std::string base = base_url();
    if (base.empty()) {
        error = "Spoolman is not configured";
        return false;
    }
    bool ok = false;
    json body;
    body["use_weight"] = grams;
    auto http = Http::put(base + "/api/v1/spool/" + std::to_string(spool_id) + "/use");
    http.timeout_connect(4)
        .timeout_max(10)
        .header("Content-Type", "application/json")
        .set_post_body(body.dump())
        .on_error([&](std::string, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
        })
        .on_complete([&](std::string, unsigned) { ok = true; })
        .perform_sync();
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "Spoolman::use_weight failed: " << error;
    return ok;
}

} // namespace Slic3r
