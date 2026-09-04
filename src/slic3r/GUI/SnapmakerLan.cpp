#include "SnapmakerLan.hpp"

#include "GUI_App.hpp"
#include "RemoteHub.hpp" // hub_dir
#include "libslic3r/AppConfig.hpp"
#include "slic3r/Utils/Bonjour.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <future>
#include <map>
#include <mutex>

namespace Slic3r {
namespace GUI {
namespace SnapmakerLan {

using nlohmann::json;
namespace fs = boost::filesystem;

// ---------------------------------------------------------------- helpers ----

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::string base_url(const Device& d)
{
    return "http://" + d.ip + (d.port > 0 && d.port != 80 ? ":" + std::to_string(d.port) : "");
}

// One short GET. Moonraker answers {"result": ...} or {"error": ...}; the parsed body comes back.
static bool get_json(const std::string& url, json& out, std::string& error, int timeout_s = 4)
{
    bool ok = false;
    Http::get(url)
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .on_complete([&](std::string body, unsigned) {
            try {
                out = json::parse(body);
                ok  = true;
            } catch (const std::exception& e) {
                error = std::string("unreadable answer: ") + e.what();
            }
        })
        .on_error([&](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try { // Moonraker puts its own message in the body of a 4xx
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error += ": " + j["error"]["message"].get<std::string>();
            } catch (...) {}
        })
        .perform_sync();
    return ok;
}

static std::string str_of(const json& j, const char* key, const std::string& def = "")
{
    return j.contains(key) && j[key].is_string() ? j[key].get<std::string>() : def;
}

static double num_of(const json& j, const char* key, double def = 0)
{
    return j.contains(key) && j[key].is_number() ? j[key].get<double>() : def;
}

// ------------------------------------------------------------- the store ----

static std::mutex s_store_mutex;

static std::string store_path() { return (fs::path(RemoteHub::hub_dir()) / "snapmaker_lan.json").string(); }

static json load_store()
{
    try {
        boost::nowide::ifstream in(store_path());
        if (!in.good())
            return json::object();
        json j;
        in >> j;
        return j.is_object() ? j : json::object();
    } catch (...) {
        return json::object();
    }
}

// Written through a temporary file: another instance reading the list never sees half of it.
static void save_store(const json& j)
{
    try {
        boost::system::error_code ec;
        fs::create_directories(RemoteHub::hub_dir(), ec);
        const fs::path final(store_path());
        const fs::path temp = final.parent_path() / (final.filename().string() + ".tmp");
        {
            boost::nowide::ofstream out(temp.string(), std::ios::binary | std::ios::trunc);
            out << j.dump(1);
        }
        fs::remove(final, ec);
        fs::rename(temp, final, ec);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "[SnapmakerLan] could not write the device list: " << e.what();
    }
}

static Device device_of(const json& j)
{
    Device d;
    d.id       = str_of(j, "id");
    d.name     = str_of(j, "name");
    d.model    = str_of(j, "model");
    d.ip       = str_of(j, "ip");
    d.port     = j.contains("port") && j["port"].is_number_integer() ? j["port"].get<int>() : 80;
    d.added_by = str_of(j, "added_by", "manual");
    return d;
}

static json json_of(const Device& d)
{
    json j;
    j["id"]       = d.id;
    j["name"]     = d.name;
    j["model"]    = d.model;
    j["ip"]       = d.ip;
    j["port"]     = d.port;
    j["added_by"] = d.added_by;
    return j;
}

std::vector<Device> devices()
{
    std::lock_guard<std::mutex> lock(s_store_mutex);
    std::vector<Device>         out;
    const json                  store = load_store();
    if (store.contains("devices") && store["devices"].is_array())
        for (const json& d : store["devices"])
            if (d.is_object() && !str_of(d, "ip").empty())
                out.push_back(device_of(d));
    return out;
}

bool find(const std::string& id, Device& out)
{
    for (const Device& d : devices())
        if (d.id == id || d.ip == id) {
            out = d;
            return true;
        }
    return false;
}

// Add or update one entry. Matching is by serial number first, then by address, so a printer that
// comes back on another address (DHCP) moves rather than doubling up.
static void store_device(const Device& d)
{
    if (d.ip.empty())
        return;
    std::lock_guard<std::mutex> lock(s_store_mutex);
    json                        store = load_store();
    if (!store.contains("devices") || !store["devices"].is_array())
        store["devices"] = json::array();
    for (json& e : store["devices"]) {
        const bool same_id = !d.id.empty() && str_of(e, "id") == d.id;
        const bool same_ip = str_of(e, "ip") == d.ip;
        if (same_id || same_ip) {
            Device merged = device_of(e);
            merged.ip     = d.ip;
            if (d.port > 0) merged.port = d.port;
            if (!d.id.empty()) merged.id = d.id;
            if (!d.name.empty()) merged.name = d.name;
            if (!d.model.empty()) merged.model = d.model;
            // How it first arrived is how it stays: a printer someone typed in is theirs to remove.
            e = json_of(merged);
            save_store(store);
            return;
        }
    }
    store["devices"].push_back(json_of(d));
    save_store(store);
}

bool remove(const std::string& id)
{
    std::lock_guard<std::mutex> lock(s_store_mutex);
    json                        store = load_store();
    if (!store.contains("devices") || !store["devices"].is_array())
        return false;
    json kept  = json::array();
    bool found = false;
    for (const json& e : store["devices"]) {
        if (str_of(e, "id") == id || str_of(e, "ip") == id) {
            found = true;
            continue;
        }
        kept.push_back(e);
    }
    if (!found)
        return false;
    store["devices"] = kept;
    save_store(store);
    return true;
}

// ----------------------------------------------------------- identifying ----

// Ask a printer who it is. /machine/system_info carries the model, the serial number and the name
// the person gave it; /printer/info is the fallback for a Moonraker that has neither.
static bool identify(const std::string& ip, int port, Device& out, std::string& error)
{
    Device d;
    d.ip   = ip;
    d.port = port > 0 ? port : 80;
    json j;
    if (!get_json(base_url(d) + "/machine/system_info", j, error, 5) || !j.contains("result")) {
        json        info;
        std::string ignore;
        if (!get_json(base_url(d) + "/printer/info", info, ignore, 5) || !info.contains("result"))
            return false; // `error` says why
        d.name  = str_of(info["result"], "hostname", ip);
        d.model = "Snapmaker";
        d.id    = ip;
    } else {
        const json& sys = j["result"].contains("system_info") ? j["result"]["system_info"] : j["result"];
        const json& p   = sys.contains("product_info") ? sys["product_info"] : sys;
        d.model         = str_of(p, "machine_type", "Snapmaker");
        d.id            = str_of(p, "serial_number");
        d.name          = str_of(p, "device_name");
        if (d.name.empty()) d.name = str_of(p, "hostname", ip);
        if (d.id.empty()) d.id = ip;
    }
    if (d.name.empty()) d.name = ip;
    out = d;
    return true;
}

bool identify_at(const std::string& ip, int port, Device& out, std::string& error) { return identify(ip, port, out, error); }

bool add(const std::string& ip_in, int port, Device& out, std::string& error)
{
    std::string ip = ip_in;
    boost::trim(ip);
    if (ip.empty()) {
        error = "an address is required";
        return false;
    }
    // "10.0.0.5:7125" is accepted as well as a plain address.
    const size_t colon = ip.rfind(':');
    if (colon != std::string::npos && ip.find(':') == colon) {
        try {
            port = std::stoi(ip.substr(colon + 1));
            ip   = ip.substr(0, colon);
        } catch (...) {}
    }
    if (ip.find('/') != std::string::npos || ip.find(' ') != std::string::npos) {
        error = "that does not look like an address: " + ip_in;
        return false;
    }
    if (!identify(ip, port, out, error)) {
        const std::string why = error;
        error = "no Snapmaker answered at " + ip + (why.empty() ? "" : " (" + why + ")");
        return false;
    }
    out.added_by = "manual";
    store_device(out);
    return true;
}

void merge_app_devices()
{
    if (!wxGetApp().app_config)
        return;
    for (const DeviceInfo& d : wxGetApp().app_config->get_devices()) {
        if (d.ip.empty())
            continue;
        Device dev;
        dev.ip       = d.ip;
        dev.port     = 80; // the Device tab's port is the MQTT one; Moonraker's HTTP is on 80
        dev.id       = d.sn.empty() ? (d.dev_id.empty() ? d.ip : d.dev_id) : d.sn;
        dev.name     = d.dev_name.empty() ? d.ip : d.dev_name;
        dev.model    = d.model_name;
        dev.added_by = "device_tab";
        store_device(dev);
    }
}

// ------------------------------------------------------------- discovery ----

static std::mutex   s_discovery_mutex;
static long long    s_last_discovery = 0;
static Bonjour::Ptr s_lookup; // kept alive while it runs

void start_discovery()
{
    std::lock_guard<std::mutex> lock(s_discovery_mutex);
    if (s_last_discovery != 0 && now_ms() - s_last_discovery < 60000)
        return;
    s_last_discovery = now_ms();
    // The service and the keys the Device page's own search uses (SSWCP.cpp:1790).
    Bonjour::TxtKeys keys = { "sn", "version", "machine_type", "link_mode", "device_name", "ip" };
    s_lookup              = Bonjour("snapmaker")
                   .set_txt_keys(std::move(keys))
                   .set_retries(2)
                   .set_timeout(6)
                   .on_reply([](BonjourReply&& reply) {
                       if (!reply.ip.is_v4())
                           return;
                       Device d;
                       d.ip     = reply.ip.to_string();
                       d.port   = 80; // the SRV port is the printer's own app port, not Moonraker's
                       auto txt = [&reply](const char* k) {
                           auto it = reply.txt_data.find(k);
                           return it == reply.txt_data.end() ? std::string() : it->second;
                       };
                       d.id    = txt("sn");
                       d.model = txt("machine_type");
                       d.name  = txt("device_name");
                       if (d.name.empty()) {
                           d.name         = reply.hostname;
                           const size_t p = d.name.find(".local");
                           if (p != std::string::npos)
                               d.name = d.name.substr(0, p);
                       }
                       if (d.name.empty()) d.name = reply.service_name;
                       if (d.id.empty()) d.id = d.ip;
                       d.added_by = "discovery";
                       BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] discovered " << d.name << " at " << d.ip;
                       store_device(d);
                   })
                   .on_complete([]() { BOOST_LOG_TRIVIAL(debug) << "[SnapmakerLan] discovery pass done"; })
                   .lookup();
}

// The Stream tab's camera wall is the fourth source: the hub keeps it in
// <datadir>/hub/streams.json and a U1's camera URL points at the printer itself, so a host that
// answers as a Snapmaker is one. Bambu cameras (an access code, an rtsps source) are left alone -
// those printers arrive through the device manager.
static std::mutex s_streams_mutex;
static long long  s_streams_seen = 0; // last write time of streams.json we looked at

void merge_stream_devices()
{
    std::string path = (fs::path(RemoteHub::hub_dir()) / "streams.json").string();
    boost::system::error_code ec;
    const std::time_t         when = fs::last_write_time(path, ec);
    if (ec)
        return;
    {
        std::lock_guard<std::mutex> lock(s_streams_mutex);
        if ((long long) when == s_streams_seen)
            return; // nothing new since the last look
        s_streams_seen = (long long) when;
    }
    json state;
    try {
        boost::nowide::ifstream in(path);
        if (!in.good())
            return;
        in >> state;
    } catch (...) {
        return;
    }
    if (!state.contains("hosts") || !state["hosts"].is_array())
        return;
    const std::vector<Device> known = devices();
    int probes = 0;
    for (const json& h : state["hosts"]) {
        if (!h.is_object() || ++probes > 12) // a camera wall is short; do not walk a long one twice
            continue;
        const std::string ip    = str_of(h, "ip");
        const std::string kind  = str_of(h, "kind");
        const std::string alias = str_of(h, "alias");
        if (ip.empty() || !h.value("code", std::string()).empty() || kind == "rtsps")
            continue; // a Bambu camera: that printer is the device manager's
        if (std::any_of(known.begin(), known.end(), [&ip](const Device& d) { return d.ip == ip; }))
            continue;
        Device      d;
        std::string error;
        if (!identify(ip, 80, d, error) || !boost::icontains(d.model, "Snapmaker"))
            continue;
        if (!alias.empty() && (d.name.empty() || d.name == ip))
            d.name = alias;
        d.added_by = "streams";
        BOOST_LOG_TRIVIAL(info) << "[SnapmakerLan] the camera at " << ip << " is a " << d.model;
        store_device(d);
    }
}

// ------------------------------------------------------------ live state ----

struct Cached
{
    Status                st;
    std::vector<Toolhead> heads;
    long long             when { 0 };
};
static std::mutex                    s_status_mutex;
static std::map<std::string, Cached> s_status;
static const long long               STATUS_TTL_MS = 4000;

// "FEE5A5FF" (RRGGBBAA) or an ARGB integer -> "#RRGGBB".
static std::string color_of(const json& cfg, size_t i)
{
    if (cfg.contains("filament_color_rgba") && cfg["filament_color_rgba"].is_array() && i < cfg["filament_color_rgba"].size() &&
        cfg["filament_color_rgba"][i].is_string()) {
        const std::string rgba = cfg["filament_color_rgba"][i].get<std::string>();
        if (rgba.size() >= 6)
            return "#" + rgba.substr(0, 6);
    }
    if (cfg.contains("filament_color") && cfg["filament_color"].is_array() && i < cfg["filament_color"].size() &&
        cfg["filament_color"][i].is_number()) {
        const unsigned argb = (unsigned) cfg["filament_color"][i].get<long long>();
        char            buf[8];
        std::snprintf(buf, sizeof buf, "#%06X", argb & 0xFFFFFFu);
        return buf;
    }
    return "";
}

static std::string arr_str(const json& cfg, const char* key, size_t i)
{
    return cfg.contains(key) && cfg[key].is_array() && i < cfg[key].size() && cfg[key][i].is_string()
               ? cfg[key][i].get<std::string>()
               : std::string();
}

static bool arr_bool(const json& cfg, const char* key, size_t i)
{
    return cfg.contains(key) && cfg[key].is_array() && i < cfg[key].size() && cfg[key][i].is_boolean() &&
           cfg[key][i].get<bool>();
}

// print_task_config's per-toolhead arrays are what the desktop's own update_filament_info reads
// (SSWCP.cpp:1485); the nozzle comes from extruder / extruder1 / ... next to them.
static std::vector<Toolhead> toolheads_of(const json& status_obj)
{
    std::vector<Toolhead> out;
    if (!status_obj.contains("print_task_config") || !status_obj["print_task_config"].is_object())
        return out;
    const json& cfg   = status_obj["print_task_config"];
    size_t      count = 0;
    for (const char* key : { "filament_type", "filament_color_rgba", "filament_exist" })
        if (cfg.contains(key) && cfg[key].is_array())
            count = std::max(count, cfg[key].size());
    for (size_t i = 0; i < count; ++i) {
        Toolhead t;
        t.index    = (int) i;
        t.type     = arr_str(cfg, "filament_type", i);
        t.sub_type = arr_str(cfg, "filament_sub_type", i);
        t.vendor   = arr_str(cfg, "filament_vendor", i);
        t.color    = color_of(cfg, i);
        t.loaded   = arr_bool(cfg, "filament_exist", i);
        t.official = arr_bool(cfg, "filament_official", i);
        const std::string ex = i == 0 ? "extruder" : ("extruder" + std::to_string(i));
        if (status_obj.contains(ex) && status_obj[ex].is_object())
            t.nozzle = num_of(status_obj[ex], "nozzle_diameter");
        out.push_back(t);
    }
    return out;
}

static Status probe(const Device& d, std::vector<Toolhead>* heads = nullptr)
{
    Status      s;
    std::string error;
    json        info;
    if (!get_json(base_url(d) + "/server/info", info, error, 3) || !info.contains("result"))
        return s; // offline
    s.online = true;
    s.klippy = str_of(info["result"], "klippy_state");
    json access;
    if (get_json(base_url(d) + "/access/info", access, error, 3) && access.contains("result"))
        s.login_required = access["result"].contains("login_required") && access["result"]["login_required"].is_boolean() &&
                           access["result"]["login_required"].get<bool>();
    json q;
    if (get_json(base_url(d) +
                     "/printer/objects/query?print_stats&display_status&heater_bed&extruder&extruder1&extruder2&extruder3&"
                     "print_task_config",
                 q, error, 5) &&
        q.contains("result") && q["result"].contains("status")) {
        const json& st = q["result"]["status"];
        if (heads)
            *heads = toolheads_of(st);
        if (st.contains("print_stats")) {
            const json& ps   = st["print_stats"];
            s.state          = str_of(ps, "state");
            s.filename       = str_of(ps, "filename");
            s.message        = str_of(ps, "message");
            s.print_duration = num_of(ps, "print_duration");
            s.total_duration = num_of(ps, "total_duration");
            if (ps.contains("info") && ps["info"].is_object()) {
                s.layer        = (int) num_of(ps["info"], "current_layer");
                s.total_layers = (int) num_of(ps["info"], "total_layer");
            }
        }
        if (st.contains("display_status"))
            s.progress = num_of(st["display_status"], "progress");
        if (st.contains("heater_bed")) {
            s.bed_temp   = num_of(st["heater_bed"], "temperature");
            s.bed_target = num_of(st["heater_bed"], "target");
        }
        if (st.contains("extruder")) {
            s.nozzle_temp   = num_of(st["extruder"], "temperature");
            s.nozzle_target = num_of(st["extruder"], "target");
        }
    }
    return s;
}

static Cached probe_cached(const Device& d, bool fresh = false)
{
    if (!fresh) {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        auto                        it = s_status.find(d.id);
        if (it != s_status.end() && now_ms() - it->second.when < STATUS_TTL_MS)
            return it->second;
    }
    Cached c;
    c.st   = probe(d, &c.heads);
    c.when = now_ms();
    {
        std::lock_guard<std::mutex> lock(s_status_mutex);
        s_status[d.id] = c;
    }
    return c;
}

Status status(const Device& d) { return probe_cached(d).st; }

Status status_now(const Device& d) { return probe_cached(d, true).st; }

bool cached_status(const Device& d, Status& out)
{
    std::lock_guard<std::mutex> lock(s_status_mutex);
    auto                        it = s_status.find(d.id);
    if (it == s_status.end()) return false;
    out = it->second.st;
    return true;
}

std::vector<Toolhead> toolheads(const Device& d) { return probe_cached(d).heads; }

static json toolheads_json(const std::vector<Toolhead>& heads)
{
    json out = json::array();
    for (const Toolhead& t : heads) {
        json j;
        j["index"]    = t.index;
        j["type"]     = t.type;
        j["sub_type"] = t.sub_type;
        j["vendor"]   = t.vendor;
        j["color"]    = t.color;
        j["loaded"]   = t.loaded;
        j["official"] = t.official;
        j["nozzle"]   = t.nozzle;
        out.push_back(j);
    }
    return out;
}

static json status_json(const Device& d, const Status& s)
{
    json j;
    j["id"]             = d.id;
    j["name"]           = d.name;
    j["model"]          = d.model;
    j["ip"]             = d.ip;
    j["port"]           = d.port;
    j["added_by"]       = d.added_by;
    j["online"]         = s.online;
    j["login_required"] = s.login_required;
    j["state"]          = s.state;
    j["printing"]       = s.printing();
    j["task"]           = s.filename;
    j["message"]        = s.message;
    j["percent"]        = (int) (s.progress * 100 + 0.5);
    j["layer"]          = s.layer;
    j["total_layers"]   = s.total_layers;
    j["bed_temp"]       = s.bed_temp;
    j["bed_target"]     = s.bed_target;
    j["nozzle_temp"]    = s.nozzle_temp;
    j["nozzle_target"]  = s.nozzle_target;
    // The printer reports elapsed time and progress; the desktop's "time left" is the rest.
    j["left_time_s"] = (s.progress > 0.01 && s.print_duration > 0) ? (int) (s.print_duration / s.progress - s.print_duration) : 0;
    return j;
}

void list_json(json& out)
{
    const std::vector<Device> list = devices();
    // A printer that is off must not hold up the ones that are: probe them side by side.
    std::vector<std::future<Cached>> pending;
    pending.reserve(list.size());
    for (const Device& d : list)
        pending.push_back(std::async(std::launch::async, [d]() { return probe_cached(d); }));
    out["devices"] = json::array();
    for (size_t i = 0; i < list.size(); ++i) {
        Cached c;
        try {
            c = pending[i].get();
        } catch (...) {}
        json j         = status_json(list[i], c.st);
        j["toolheads"] = toolheads_json(c.heads);
        out["devices"].push_back(j);
    }
}

void list_printers(json& printers)
{
    for (const Device& d : devices()) {
        const Cached c = probe_cached(d);
        const Status s = c.st;
        json         p;
        p["id"]             = "sm:" + d.id;
        p["kind"]           = "snapmaker";
        p["name"]           = d.name.empty() ? d.ip : d.name;
        p["model"]          = d.model;
        p["url"]            = base_url(d);
        p["online"]         = s.online;
        p["printing"]       = s.printing();
        p["status"]         = s.state;
        p["percent"]        = (int) (s.progress * 100 + 0.5);
        p["task"]           = s.filename;
        p["login_required"] = s.login_required;
        p["can_upload"]     = s.online && !s.login_required;
        p["can_print"]      = s.online && !s.login_required;
        p["bed_temp"]       = s.bed_temp;
        p["bed_target"]     = s.bed_target;
        p["nozzles"]        = json::array({ json { { "temp", s.nozzle_temp }, { "target", s.nozzle_target } } });
        p["toolheads"]      = toolheads_json(c.heads);
        // The predicates RemoteControl derives for any Moonraker printer, so the phone's Pause /
        // Resume / Stop buttons work on a printer found over the LAN too. `task` already names the
        // job, so no `stage`; the printer's message is an error only when its state says so.
        p["print_status"]   = s.state;
        p["can_pause"]      = s.online && s.state == "printing";
        p["can_resume"]     = s.online && s.state == "paused";
        p["can_stop"]       = s.online && (s.state == "printing" || s.state == "paused");
        if (s.online && s.state == "error")
            p["print_error"] = json { { "code", "error" }, { "message", s.message } };
        else
            p["print_error"] = nullptr;
        printers.push_back(p);
    }
}

// --------------------------------------------------------------- sending ----

bool upload(const Device& d, const std::string& source_path, const std::string& filename, std::function<void(int)> progress,
            std::string& error)
{
    bool ok = false;
    // print=false always: a print that will not start must still leave a file the person can use
    // from the printer's own screen.
    Http::post(base_url(d) + "/server/files/upload")
        .timeout_connect(10)
        .form_add("print", "false")
        .form_add("root", "gcodes")
        .form_add_file("file", fs::path(source_path), filename)
        .on_progress([&progress](Http::Progress prog, bool& cancel) {
            cancel = false;
            if (progress && prog.ultotal > 0)
                progress((int) std::min<size_t>(100, prog.ulnow * 100 / prog.ultotal));
        })
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error += ": " + j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

bool metadata(const Device& d, const std::string& filename, long long& size, std::string& error)
{
    json j;
    if (!get_json(base_url(d) + "/server/files/metadata?filename=" + Http::url_encode(filename), j, error, 8))
        return false;
    if (!j.contains("result")) {
        error = "the printer does not list that file";
        return false;
    }
    size = (long long) num_of(j["result"], "size");
    return true;
}

// --------------------------------------------- the toolhead mapping ----
//
// How a print with more than one filament is started. The U1 takes the mapping as Klipper macros
// over plain HTTP, exactly as its own touchscreen and u1hub do it:
//
//   SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=<the file's filament> MAP_EXTRUDER=<toolhead>
//   SET_PRINT_USED_EXTRUDERS EXTRUDERS=<every toolhead used, once each>
//   SET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=0
//
// followed by the ordinary start (printer.print.start, i.e. POST /printer/print/start?filename=).
// That is what the fork's own Device page sends - the shipped Flutter bundle builds exactly these
// macros and then printer.print.start; its server.files.start_local_print path has the routing but
// no request builder - and what u1hub sends (it ends with SDCARD_PRINT_FILE, which is what
// /printer/print/start runs). server.files.start_local_print is in any case registered on the
// printer's websocket and MQTT transports only: over HTTP it answers "Method not found".
static bool gcode_script(const Device& d, const std::string& script, std::string& error)
{
    bool ok = false;
    Http::post(base_url(d) + "/printer/gcode/script?script=" + Http::url_encode(script))
        .timeout_connect(5)
        .timeout_max(60) // the printer answers a script only once it has run it
        // Everything is in the query string, but the body has to be set: without it the Http
        // wrapper leaves CURLOPT_POSTFIELDS alone and curl waits for a body nobody will write.
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}"))
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error = j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

// "#RRGGBB" -> 0..255 triple. False when it is not a colour.
static bool rgb_of(const std::string& hex, int rgb[3])
{
    std::string h = hex;
    if (!h.empty() && h[0] == '#')
        h = h.substr(1);
    if (h.size() < 6)
        return false;
    for (int i = 0; i < 3; ++i) {
        try {
            rgb[i] = std::stoi(h.substr(i * 2, 2), nullptr, 16);
        } catch (...) {
            return false;
        }
    }
    return true;
}

// "Redmean" colour distance - the cheap approximation of how different two colours look, and the
// one u1hub matches with, so the phone's suggestion agrees with the tools people already use.
static double color_distance(const std::string& a, const std::string& b)
{
    int ca[3], cb[3];
    if (!rgb_of(a, ca) || !rgb_of(b, cb))
        return 1e9;
    const double rm = (ca[0] + cb[0]) / 2.0;
    const double dr = ca[0] - cb[0], dg = ca[1] - cb[1], db = ca[2] - cb[2];
    return std::sqrt((2 + rm / 256) * dr * dr + 4 * dg * dg + (2 + (255 - rm) / 256) * db * db);
}

static std::string upper_hex(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return (char) std::toupper(c); });
    return out;
}

std::vector<int> auto_match(const std::vector<FileFilament>& filaments, const std::vector<Toolhead>& heads)
{
    std::vector<int>  out(filaments.size(), -1);
    std::vector<bool> taken(heads.size(), false);
    // 1. every used filament takes the nearest free loaded toolhead.
    for (size_t i = 0; i < filaments.size(); ++i) {
        if (!filaments[i].used)
            continue;
        double best = 1e9;
        int    pick = -1;
        for (size_t h = 0; h < heads.size(); ++h) {
            if (!heads[h].loaded || taken[h])
                continue;
            const double d = color_distance(filaments[i].color, heads[h].color);
            if (d < best) {
                best = d;
                pick = (int) h;
            }
        }
        if (pick >= 0) {
            out[i]      = pick;
            taken[pick] = true;
        }
    }
    // 2. a filament of exactly the same colour shares the toolhead its twin got.
    for (size_t i = 0; i < filaments.size(); ++i) {
        if (out[i] >= 0 || !filaments[i].used || filaments[i].color.empty())
            continue;
        for (size_t j = 0; j < filaments.size(); ++j)
            if (j != i && out[j] >= 0 && upper_hex(filaments[j].color) == upper_hex(filaments[i].color)) {
                out[i] = out[j];
                break;
            }
    }
    // 3. anything still unplaced goes on the first loaded toolhead, as a suggestion the person can
    //    change - never a refusal.
    int first_loaded = -1;
    for (size_t h = 0; h < heads.size(); ++h)
        if (heads[h].loaded) {
            first_loaded = (int) h;
            break;
        }
    for (size_t i = 0; i < filaments.size(); ++i)
        if (out[i] < 0 && filaments[i].used)
            out[i] = first_loaded >= 0 ? first_loaded : 0;
    return out;
}

// The macros for one mapping, without the start: mapping[i] is the toolhead that prints the file's
// filament i, -1 for a filament the file does not use.
std::string mapping_script(const std::vector<int>& mapping)
{
    std::string      script;
    std::vector<int> used; // every toolhead once, in the order the file first uses it
    for (size_t i = 0; i < mapping.size(); ++i) {
        if (mapping[i] < 0)
            continue;
        script += "SET_PRINT_EXTRUDER_MAP CONFIG_EXTRUDER=" + std::to_string(i) +
                  " MAP_EXTRUDER=" + std::to_string(mapping[i]) + "\n";
        if (std::find(used.begin(), used.end(), mapping[i]) == used.end())
            used.push_back(mapping[i]);
    }
    if (script.empty())
        return script;
    script += "SET_PRINT_USED_EXTRUDERS EXTRUDERS=";
    for (size_t i = 0; i < used.size(); ++i)
        script += (i ? "," : "") + std::to_string(used[i]);
    script += "\nSET_PRINT_PREFERENCES BED_LEVEL=0 FLOW_CALIBRATE=0 TIME_LAPSE_CAMERA=0";
    return script;
}

bool start_print_mapped(const Device& d, const std::string& filename, const std::vector<int>& mapping, json& sent,
                        std::string& error)
{
    const std::string script = mapping_script(mapping);
    sent["mapping_script"]   = script;
    if (!script.empty() && !gcode_script(d, script, error)) {
        error = "the printer refused the toolhead mapping: " + error;
        return false;
    }
    // The start itself is the page's own printer.print.start, which is this over HTTP.
    sent["start"] = "/printer/print/start?filename=" + filename;
    if (!start_print(d, filename, error)) {
        error = "the printer refused the print start: " + error;
        return false;
    }
    return true;
}

bool start_print(const Device& d, const std::string& filename, std::string& error)
{
    bool ok = false;
    Http::post(base_url(d) + "/printer/print/start?filename=" + Http::url_encode(filename))
        .timeout_connect(5)
        .timeout_max(30)
        .header("Content-Type", "application/json")
        .set_post_body(std::string("{}")) // as above: a POST needs a body for curl to send it
        .on_complete([&ok](std::string, unsigned) { ok = true; })
        .on_error([&error](std::string body, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            try {
                const json j = json::parse(body);
                if (j.contains("error") && j["error"].contains("message"))
                    error = j["error"]["message"].get<std::string>();
            } catch (...) {
                if (!body.empty() && body.size() < 300)
                    error += ": " + body;
            }
        })
        .perform_sync();
    return ok;
}

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
