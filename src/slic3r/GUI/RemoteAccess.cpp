#include "RemoteAccess.hpp"

#include "BambuCamRelay.hpp"
#include "DeviceManager.hpp"
#include "GLCanvas3D.hpp"
#include "GLToolbar.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "PresetComboBoxes.hpp"
#include "Tab.hpp"
#include "libslic3r/FilamentColorLibrary.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
#include <future>
#include <memory>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <netioapi.h>
#  pragma comment(lib, "iphlpapi.lib")
#endif

namespace Slic3r {
namespace GUI {

namespace asio = boost::asio;
using tcp      = asio::ip::tcp;

static const int         REMOTE_PORT       = 13640;
static const char* const GO2RTC_PASSTHROUGH[] = { "/stream.html", "/video-stream.js", "/video-rtc.js", "/api/ws" };

// ---------------------------------------------------------------- helpers ----

static bool is_private_v4(const asio::ip::address& a)
{
    if (!a.is_v4())
        return false;
    const uint32_t v = a.to_v4().to_uint();
    return (v >> 24) == 10 || (v >> 24) == 127 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE;
}

static std::string percent_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~')
            out += (char) c;
        else {
            out += '%';
            out += hex[c >> 4];
            out += hex[c & 15];
        }
    }
    return out;
}

static std::string percent_decode(const std::string& s)
{
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size() && std::isxdigit((unsigned char) s[i + 1]) && std::isxdigit((unsigned char) s[i + 2])) {
            out += (char) std::stoi(s.substr(i + 1, 2), nullptr, 16);
            i += 2;
        } else if (s[i] == '+') {
            out += ' ';
        } else {
            out += s[i];
        }
    }
    return out;
}

static std::string query_param(const std::string& query, const std::string& key)
{
    size_t pos = 0;
    while (pos <= query.size()) {
        size_t amp = query.find('&', pos);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == key)
            return percent_decode(kv.substr(eq + 1));
        pos = amp + 1;
    }
    return "";
}

static std::string cookie_value(const std::string& cookie_header, const std::string& name)
{
    size_t pos = 0;
    while (pos < cookie_header.size()) {
        size_t semi = cookie_header.find(';', pos);
        if (semi == std::string::npos) semi = cookie_header.size();
        std::string kv = cookie_header.substr(pos, semi - pos);
        size_t b = kv.find_first_not_of(' ');
        if (b != std::string::npos) kv = kv.substr(b);
        size_t eq = kv.find('=');
        if (eq != std::string::npos && kv.substr(0, eq) == name)
            return kv.substr(eq + 1);
        pos = semi + 1;
    }
    return "";
}

static std::string random_token()
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789"; // no 0/o/1/l look-alikes
    std::random_device rd;
    std::mt19937_64    gen(((uint64_t) rd() << 32) ^ rd());
    std::uniform_int_distribution<int> d(0, (int) sizeof(alphabet) - 2);
    std::string t;
    for (int i = 0; i < 14; ++i)
        t += alphabet[d(gen)];
    return t;
}

#ifdef _WIN32
// The address of the interface that owns the 0.0.0.0/0 route with the best metric. VPN
// clients usually route through 0.0.0.0/1 + 128.0.0.0/1 instead, so this stays the real
// LAN adapter even while a VPN is up.
static std::string default_route_ipv4_win()
{
    std::string          out;
    PMIB_IPFORWARD_TABLE2 routes = nullptr;
    if (GetIpForwardTable2(AF_INET, &routes) != NO_ERROR || !routes)
        return out;
    ULONG best_if = 0, best_metric = ~0UL;
    for (ULONG i = 0; i < routes->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = routes->Table[i];
        if (r.DestinationPrefix.PrefixLength != 0)
            continue;
        MIB_IPINTERFACE_ROW iface = {};
        iface.Family              = AF_INET;
        iface.InterfaceIndex      = r.InterfaceIndex;
        ULONG metric = r.Metric + (GetIpInterfaceEntry(&iface) == NO_ERROR ? iface.Metric : 0);
        if (metric < best_metric) {
            best_metric = metric;
            best_if     = r.InterfaceIndex;
        }
    }
    FreeMibTable(routes);
    if (best_if == 0)
        return out;
    PMIB_UNICASTIPADDRESS_TABLE addrs = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &addrs) != NO_ERROR || !addrs)
        return out;
    for (ULONG i = 0; i < addrs->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& a = addrs->Table[i];
        if (a.InterfaceIndex == best_if && a.DadState == IpDadStatePreferred) {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, (void*) &a.Address.Ipv4.sin_addr, buf, sizeof(buf)))
                out = buf;
            break;
        }
    }
    FreeMibTable(addrs);
    return out;
}
#endif

// The IPv4 the default route leaves through, then any other non-loopback IPv4 this host
// resolves to (the page shows the extras as alternatives).
static std::vector<std::string> lan_ips()
{
    std::vector<std::string> out;
#ifdef _WIN32
    {
        const std::string a = default_route_ipv4_win();
        if (!a.empty())
            out.push_back(a);
    }
#endif
    try {
        asio::io_context      ioc;
        asio::ip::udp::socket s(ioc);
        s.open(asio::ip::udp::v4());
        s.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53)); // sends nothing
        const std::string a = s.local_endpoint().address().to_string();
        if (a != "0.0.0.0" && std::find(out.begin(), out.end(), a) == out.end())
            out.push_back(a);
    } catch (...) {}
    try {
        asio::io_context ioc;
        tcp::resolver    r(ioc);
        for (const auto& e : r.resolve(asio::ip::host_name(), "")) {
            const auto a = e.endpoint().address();
            if (a.is_v4() && !a.is_loopback()) {
                const std::string s = a.to_string();
                if (std::find(out.begin(), out.end(), s) == out.end())
                    out.push_back(s);
            }
        }
    } catch (...) {}
    return out;
}

static void write_all(tcp::socket& s, const std::string& data)
{
    asio::write(s, asio::buffer(data));
}

static void respond(tcp::socket& s, const char* status, const std::string& type, const std::string& body,
                    const std::string& extra_headers = "")
{
    std::ostringstream o;
    o << "HTTP/1.1 " << status << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << extra_headers
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
}

// Make the upstream close after this response (unless it is a WebSocket upgrade), so the
// browser cannot reuse the spliced connection for a request that belongs to us.
static std::string force_close(const std::string& head)
{
    std::string out;
    bool        upgrade = false;
    size_t      pos     = 0;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) nl = head.size();
        std::string line = head.substr(pos, nl - pos);
        std::string key  = line.substr(0, std::min<size_t>(11, line.size()));
        std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
        if (key.compare(0, 8, "upgrade:") == 0)
            upgrade = true;
        if (key.compare(0, 11, "connection:") != 0 && !line.empty())
            out += line + "\r\n";
        pos = nl + 2;
    }
    if (upgrade)
        return head;
    return out + "Connection: close\r\n\r\n";
}

static void pump(tcp::socket& from, tcp::socket& to)
{
    char                      buf[16384];
    boost::system::error_code ec;
    for (;;) {
        size_t n = from.read_some(asio::buffer(buf), ec);
        if (ec)
            break;
        asio::write(to, asio::buffer(buf, n), ec);
        if (ec)
            break;
    }
    boost::system::error_code ig;
    to.shutdown(tcp::socket::shutdown_both, ig);
    from.shutdown(tcp::socket::shutdown_both, ig);
}

// Splice the client onto 127.0.0.1:<port>, replaying the (rewritten) request head first.
// Works for plain responses and WebSocket upgrades alike.
static void tunnel(tcp::socket& client, int port, const std::string& head, const std::string& pending)
{
    asio::io_context ioc;
    tcp::socket      up(ioc);
    up.connect(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), (unsigned short) port));
    up.set_option(tcp::no_delay(true));
    write_all(up, head);
    if (!pending.empty())
        write_all(up, pending);
    std::thread t([&]() { pump(client, up); });
    pump(up, client);
    t.join();
}

// Run fn on the GUI thread and wait for it (Plater, plates and devices are GUI-thread only).
// Returns false on timeout — e.g. a modal dialog is blocking the app — and fn may still run
// later, so callers only capture shared state.
static bool run_on_main(std::function<void()> fn, int timeout_ms = 15000)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        try { fn(); } catch (...) {}
        done->set_value();
    });
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

static std::string json_error(const std::string& msg)
{
    nlohmann::json j;
    j["error"] = msg;
    return j.dump();
}

std::string RemoteAccess::ff_camera_url_from_detail(const std::string& body)
{
    try {
        nlohmann::json j = nlohmann::json::parse(body);
        if (j.contains("detail") && j["detail"].is_object() && j["detail"].contains("cameraStreamUrl"))
            return j["detail"]["cameraStreamUrl"].get<std::string>();
        if (j.contains("cameraStreamUrl"))
            return j["cameraStreamUrl"].get<std::string>();
    } catch (...) {}
    return "";
}

// ------------------------------------------------------------ RemoteAccess ----

std::string RemoteAccess::Info::json() const
{
    nlohmann::json j;
    j["on"]    = on;
    j["port"]  = port;
    j["token"] = token;
    j["ips"]   = ips;
    j["url"]   = (on && !ips.empty()) ? "http://" + ips.front() + ":" + std::to_string(port) + "/r/" + token + "/" : "";
    return j.dump();
}

RemoteAccess& RemoteAccess::get()
{
    static RemoteAccess instance;
    return instance;
}

RemoteAccess::Info RemoteAccess::info()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    Info i;
    i.on    = m_on;
    i.port  = m_port;
    i.token = m_token;
    if (m_on)
        i.ips = lan_ips();
    return i;
}

RemoteAccess::Info RemoteAccess::start(const std::string& token)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_on) {
            const bool valid = token.size() >= 10 && token.size() <= 32 &&
                               token.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
            m_token = valid ? token : random_token();
            try {
                static asio::io_context ioc; // lives for the process
                auto* acceptor = new tcp::acceptor(ioc);
                acceptor->open(tcp::v4());
                acceptor->set_option(tcp::acceptor::reuse_address(true));
                boost::system::error_code ec;
                int port = REMOTE_PORT;
                for (; port < REMOTE_PORT + 20; ++port) {
                    acceptor->bind(tcp::endpoint(tcp::v4(), (unsigned short) port), ec);
                    if (!ec)
                        break;
                }
                if (ec)
                    throw boost::system::system_error(ec);
                acceptor->listen();
                m_acceptor = acceptor;
                m_port     = port;
                m_on       = true;
                std::thread([this]() { accept_loop(); }).detach();
                BOOST_LOG_TRIVIAL(info) << "RemoteAccess: phone access on 0.0.0.0:" << port;
            } catch (const std::exception& e) {
                BOOST_LOG_TRIVIAL(error) << "RemoteAccess: failed to start: " << e.what();
                m_on   = false;
                m_port = 0;
            }
        }
    }
    if (info().on)
        register_streams();
    return info();
}

void RemoteAccess::stop()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_on)
        return;
    m_on = false;
    if (auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor)) {
        boost::system::error_code ig;
        acceptor->close(ig); // unblocks accept_loop, which deletes the acceptor
        m_acceptor = nullptr;
    }
    m_port = 0;
    BOOST_LOG_TRIVIAL(info) << "RemoteAccess: phone access stopped";
}

void RemoteAccess::set_state(const std::string& json)
{
    bool on;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_state = json;
        on      = m_on;
    }
    if (on)
        register_streams();
}

// The phone only sees ids, aliases, the source kind, go2rtc stream names and direct
// printer-page URLs. Addresses, access codes and camera credentials stay here.
std::string RemoteAccess::state_for_phone()
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    nlohmann::json out;
    out["hosts"]  = nlohmann::json::array();
    out["active"] = nlohmann::json::array();
    try {
        nlohmann::json j = nlohmann::json::parse(state);
        for (const auto& h : j.value("hosts", nlohmann::json::array())) {
            nlohmann::json p;
            p["id"]    = h.value("id", "");
            p["alias"] = h.value("alias", "");
            p["rkind"] = h.value("rkind", "");
            p["rname"] = h.value("rname", "");
            p["rurl"]  = h.value("rurl", "");
            out["hosts"].push_back(p);
        }
        out["active"] = j.value("active", nlohmann::json::array());
    } catch (...) {}
    return out.dump();
}

bool RemoteAccess::lookup_host(const std::string& id, std::string& ip, std::string& code)
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(state);
        for (const auto& h : j.value("hosts", nlohmann::json::array())) {
            if (h.value("id", "") == id) {
                ip   = h.value("ip", "");
                code = h.value("code", "");
                return !ip.empty();
            }
        }
    } catch (...) {}
    return false;
}

void RemoteAccess::register_streams()
{
    const int port = Go2RtcLauncher::get().port();
    if (port == 0)
        return;
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        nlohmann::json j = nlohmann::json::parse(state);
        for (const auto& h : j.value("hosts", nlohmann::json::array())) {
            const std::string name = h.value("rname", ""), src = h.value("rsrc", "");
            if (name.empty() || src.empty())
                continue;
            // Http::put() is a file-upload PUT (its read callback dereferences the missing
            // file and crashes); put2() is a plain PUT with no body, which is what go2rtc wants.
            const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/streams?name=" + name + "&src=" + percent_encode(src);
            auto http = Http::put2(url);
            http.timeout_connect(2).timeout_max(5)
                .on_error([url](std::string, std::string, unsigned) {
                    // go2rtc may still be starting: one retry a moment later.
                    std::thread([url]() {
                        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
                        auto again = Http::put2(url);
                        again.timeout_connect(2).timeout_max(5).perform_sync();
                    }).detach();
                })
                .perform();
        }
    } catch (...) {}
}

// ---------------------------------------------------------------- JSON API ----

void RemoteAccess::note_slice_progress(int plate, int percent, const std::string& text)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Job& j : m_jobs)
        if (j.state == "running" && (j.plate == -1 || j.plate == plate)) {
            if (percent >= 0) j.percent = std::max(j.percent, std::min(percent, 99));
            j.text = text;
        }
}

void RemoteAccess::note_slice_done(bool finished_all, bool ok, const std::string& error)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (Job& j : m_jobs)
        if (j.state == "running") {
            if (!ok) {
                j.state = error.empty() ? "cancelled" : "error";
                j.error = error;
            } else if (finished_all || j.plate != -1) {
                j.state   = "done";
                j.percent = 100;
            }
        }
}

RemoteAccess::ApiResponse RemoteAccess::api_plates()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        Plater*        plater = wxGetApp().plater();
        PresetBundle*  bundle = wxGetApp().preset_bundle;
        PartPlateList& plates = plater->get_partplate_list();
        nlohmann::json& j     = *out;
        j["project"]       = plater->get_project_filename().ToUTF8().data();
        j["printer"]       = bundle->printers.get_selected_preset_name();
        j["current_plate"] = plates.get_curr_plate_index();
        j["slicing"]       = plater->is_background_process_slicing();
        j["filaments"]     = nlohmann::json::array();
        std::vector<double> density;
        if (auto* d = bundle->full_config().option<ConfigOptionFloats>("filament_density"))
            density = d->values;
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["name"]  = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        j["plates"] = nlohmann::json::array();
        for (int i = 0; i < plates.get_plate_count(); ++i) {
            PartPlate*     p = plates.get_plate(i);
            nlohmann::json jp;
            jp["index"]   = i;
            jp["name"]    = p->get_plate_name();
            jp["objects"] = nlohmann::json::array();
            for (const ModelObject* o : p->get_objects_on_this_plate())
                jp["objects"].push_back(o->name);
            jp["printable"]       = p->has_printable_instances();
            jp["locked"]          = p->is_locked();
            jp["sliced"]          = p->is_slice_result_valid();
            jp["ready_for_print"] = p->is_slice_result_ready_for_print();
            jp["slicing_percent"] = p->get_slicing_percent();
            if (p->is_slice_result_valid() && p->get_slice_result()) {
                const auto& st = p->get_slice_result()->print_statistics;
                if (!st.modes.empty())
                    jp["time_s"] = st.modes.front().time;
                double mm3 = 0, grams = 0;
                for (const auto& kv : st.total_volumes_per_extruder) {
                    mm3 += kv.second;
                    const double dens = kv.first < density.size() ? density[kv.first] : 1.24;
                    grams += kv.second / 1000.0 * dens;
                }
                jp["filament_mm3"] = mm3;
                jp["filament_g"]   = grams;
            }
            j["plates"].push_back(jp);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_plate_thumbnail(int plate)
{
    auto data = std::make_shared<ThumbnailData>();
    bool ok   = run_on_main([data, plate]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plate < 0 || plate >= plates.get_plate_count())
            return;
        plater->update_all_plate_thumbnails(false);
        *data = plates.get_plate(plate)->thumbnail_data;
    }, 30000);
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (!data->is_valid()) { r.status = 404; r.body = json_error("no thumbnail for this plate"); return r; }
    auto png = GCodeThumbnails::compress_thumbnail(*data, GCodeThumbnailsFormat::PNG);
    r.type   = "image/png";
    r.body.assign(static_cast<const char*>(png->data), png->size);
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_printers()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j = *out;
        j["printers"]     = nlohmann::json::array();
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm)
            return;
        MachineObject* selected = dm->get_selected_machine();
        std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
        for (const auto& kv : dm->get_local_machine_list())
            all.insert(kv);
        for (const auto& kv : all) {
            MachineObject* m = kv.second;
            if (!m) continue;
            nlohmann::json p;
            p["id"]           = m->dev_id;
            p["name"]         = m->dev_name;
            p["model"]        = m->printer_type;
            p["online"]       = m->is_online();
            p["connected"]    = m->is_connected();
            p["status"]       = m->print_status;
            p["printing"]     = m->is_in_printing();
            p["percent"]      = m->mc_print_percent;
            p["left_time_s"]  = m->mc_left_time;
            p["layer"]        = m->curr_layer;
            p["total_layers"] = m->total_layers;
            p["task"]         = m->subtask_name;
            p["bed_temp"]     = m->bed_temp;
            p["bed_target"]   = m->bed_temp_target;
            p["nozzles"]      = nlohmann::json::array();
            for (const Extder& e : m->m_extder_data.extders) {
                nlohmann::json n;
                n["temp"]   = e.temp;
                n["target"] = e.target_temp;
                p["nozzles"].push_back(n);
            }
            p["selected"] = (selected == m);
            j["printers"].push_back(p);
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_slice(int plate, bool all)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, plate, all]() {
        Plater*        plater = wxGetApp().plater();
        PartPlateList& plates = plater->get_partplate_list();
        if (plater->is_background_process_slicing()) { *result = { 409, "already slicing" }; return; }
        if (!all && (plate < 0 || plate >= plates.get_plate_count())) { *result = { 404, "no such plate" }; return; }
        if (all && !plater->has_sliceable_plate_for_slice_all()) { *result = { 409, "nothing to slice" }; return; }
        if (!all) {
            if (!plates.get_plate(plate)->has_printable_instances()) { *result = { 409, "plate is empty" }; return; }
            plater->select_plate(plate);
        }
        plater->exit_gizmo();
        plater->update(true, true);
        wxPostEvent(plater, SimpleEvent(all ? EVT_GLTOOLBAR_SLICE_ALL : EVT_GLTOOLBAR_SLICE_PLATE));
        *result = { 200, "" };
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    Job job;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        job.id    = m_next_job++;
        job.plate = all ? -1 : plate;
        job.state = "running";
        m_jobs.push_back(job);
        if (m_jobs.size() > 50)
            m_jobs.erase(m_jobs.begin());
    }
    nlohmann::json j;
    j["job"]   = job.id;
    j["plate"] = job.plate;
    r.body     = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_jobs(int id)
{
    ApiResponse r;
    std::lock_guard<std::mutex> lock(m_mutex);
    auto to_json = [](const Job& j) {
        nlohmann::json o;
        o["id"] = j.id; o["plate"] = j.plate; o["state"] = j.state; o["percent"] = j.percent;
        o["text"] = j.text; o["error"] = j.error;
        return o;
    };
    if (id < 0) {
        nlohmann::json j;
        j["jobs"] = nlohmann::json::array();
        for (const Job& job : m_jobs) j["jobs"].push_back(to_json(job));
        r.body = j.dump();
        return r;
    }
    for (const Job& job : m_jobs)
        if (job.id == id) { r.body = to_json(job).dump(); return r; }
    r.status = 404;
    r.body   = json_error("no such job");
    return r;
}

// The sidebar's own combo boxes are the source of truth for "what can be picked" and selecting
// through them runs the exact code path a click in the sidebar does (Plater::priv::on_select_preset).
static bool is_label_item(PresetComboBox* combo, unsigned int i)
{
    const size_t marker = reinterpret_cast<size_t>(combo->GetClientData(i));
    return marker >= PresetComboBox::LABEL_ITEM_MARKER && marker < PresetComboBox::LABEL_ITEM_MAX;
}

static std::string combo_item_value(PresetComboBox* combo, unsigned int i, Preset::Type type)
{
    const std::string display = combo->GetString(i).ToUTF8().data();
    return wxGetApp().preset_bundle->get_preset_name_by_alias(type, Preset::remove_suffix_modified(display));
}

// The process preset has no sidebar combo in this fork; its list lives on the Process tab.
static PresetComboBox* process_combo()
{
    Tab* tab = wxGetApp().get_tab(Preset::TYPE_PRINT);
    return tab ? tab->get_combo_box() : nullptr;
}

static nlohmann::json combo_items(PresetComboBox* combo, Preset::Type type)
{
    nlohmann::json items = nlohmann::json::array();
    if (!combo)
        return items;
    const int sel = combo->GetSelection();
    for (unsigned int i = 0; i < combo->GetCount(); ++i) {
        if (is_label_item(combo, i))
            continue;
        nlohmann::json it;
        it["name"]     = combo->GetString(i).ToUTF8().data();
        it["value"]    = combo_item_value(combo, i, type);
        it["selected"] = ((int) i == sel);
        items.push_back(it);
    }
    return items;
}

RemoteAccess::ApiResponse RemoteAccess::api_presets()
{
    auto out = std::make_shared<nlohmann::json>();
    bool ok  = run_on_main([out]() {
        nlohmann::json& j      = *out;
        Sidebar&        sb     = wxGetApp().sidebar();
        PresetBundle*   bundle = wxGetApp().preset_bundle;
        j["printer"]      = combo_items(sb.combo_printer(), Preset::TYPE_PRINTER);
        j["process"]      = combo_items(process_combo(), Preset::TYPE_PRINT);
        j["printer_name"] = bundle->printers.get_selected_preset_name();
        j["process_name"] = bundle->prints.get_selected_preset_name();
        auto& combos = sb.combos_filament();
        j["filament_choices"] = combos.empty() ? nlohmann::json::array() : combo_items(combos.front(), Preset::TYPE_FILAMENT);
        j["filaments"]        = nlohmann::json::array();
        const ConfigOptionStrings* colors = bundle->project_config.option<ConfigOptionStrings>("filament_colour");
        for (size_t i = 0; i < bundle->filament_presets.size(); ++i) {
            nlohmann::json f;
            f["index"] = i;
            f["value"] = bundle->filament_presets[i];
            f["color"] = (colors && i < colors->values.size()) ? colors->values[i] : "";
            j["filaments"].push_back(f);
        }
        j["dirty"]["printer"]  = bundle->printers.current_is_dirty();
        j["dirty"]["process"]  = bundle->prints.current_is_dirty();
        j["dirty"]["filament"] = bundle->filaments.current_is_dirty();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); }
    else       r.body = out->dump();
    return r;
}

// Unsaved modifications of a preset collection, ready to be re-applied to the newly selected
// preset (what the transfer/discard dialog's "Transfer" button does).
static DynamicPrintConfig capture_dirty(PresetCollection& presets)
{
    DynamicPrintConfig dirty;
    if (!presets.current_is_dirty())
        return dirty;
    const Preset& edited = presets.get_edited_preset();
    for (const std::string& opt : presets.current_dirty_options())
        if (const ConfigOption* o = edited.config.option(opt))
            dirty.set_key_value(opt, o->clone());
    return dirty;
}

// Phone selections never raise the transfer/discard dialog: modifications are carried over to
// the new preset (they stay "modified" there, so Revert on the PC still discards them).
RemoteAccess::ApiResponse RemoteAccess::api_select_preset(const std::string& type, const std::string& name_in, int index)
{
    auto result = std::make_shared<std::pair<int, std::string>>(500, "");
    bool ok     = run_on_main([result, type, name_in, index]() {
        const std::string& name = name_in;
        Sidebar&      sb     = wxGetApp().sidebar();
        Plater*       plater = wxGetApp().plater();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (plater->is_background_process_slicing()) { *result = { 409, "slicing in progress" }; return; }
        auto reapply = [](Preset::Type t, const DynamicPrintConfig& dirty) {
            if (!dirty.empty())
                wxGetApp().get_tab(t)->load_config(dirty);
        };
        if (type == "printer") {
            std::string name = name_in;
            if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->printers.find_preset(name)) {
                // The sidebar lists printer models ("Bambu Lab H2D") as well as presets; resolve a
                // model to its preset the way Plater::priv::on_select_preset does.
                Preset* similar = bundle->get_similar_printer_preset(name, {});
                if (!similar) { *result = { 404, "preset not in the list: " + name }; return; }
                similar->is_visible = true;
                name = similar->name;
                if (name == bundle->printers.get_selected_preset_name()) { *result = { 200, "" }; return; }
            }
            const DynamicPrintConfig printer_dirty = capture_dirty(bundle->printers);
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->physical_printers.unselect_printer();
            // force_select: no dialogs; incompatible process/filament presets are remapped silently.
            wxGetApp().get_tab(Preset::TYPE_PRINTER)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINTER, printer_dirty);
            reapply(Preset::TYPE_PRINT, process_dirty);
            reapply(Preset::TYPE_FILAMENT, filament_dirty);
            plater->on_config_change(bundle->full_config());
            wxGetApp().app_config->set("preferred_printer", bundle->printers.get_selected_preset_name());
            *result = { 200, "" };
        } else if (type == "process") {
            if (name == bundle->prints.get_selected_preset_name()) { *result = { 200, "" }; return; }
            if (!bundle->prints.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            const DynamicPrintConfig process_dirty = capture_dirty(bundle->prints);
            wxGetApp().get_tab(Preset::TYPE_PRINT)->select_preset(name, false, "", true);
            reapply(Preset::TYPE_PRINT, process_dirty);
            plater->on_config_change(bundle->full_config());
            plater->record_preferred_print_profile();
            *result = { 200, "" };
        } else if (type == "filament") {
            auto& combos = sb.combos_filament();
            if (index < 0 || index >= (int) combos.size()) { *result = { 404, "no such filament slot" }; return; }
            if (index < (int) bundle->filament_presets.size() && bundle->filament_presets[index] == name) { *result = { 200, "" }; return; }
            if (!bundle->filaments.find_preset(name)) { *result = { 404, "preset not in the list: " + name }; return; }
            // Same steps as Plater::priv::on_select_preset for TYPE_FILAMENT, with force on the tab.
            const DynamicPrintConfig filament_dirty = capture_dirty(bundle->filaments);
            bundle->set_filament_preset(index, name);
            plater->update_project_dirty_from_presets();
            bundle->export_selections(*wxGetApp().app_config);
            sb.update_dynamic_filament_list();
            sb.update_color_mix_panel();
            if (sb.is_multifilament())
                combos[index]->update();
            else {
                wxGetApp().get_tab(Preset::TYPE_FILAMENT)->select_preset(name, false, "", true);
                reapply(Preset::TYPE_FILAMENT, filament_dirty);
            }
            plater->on_config_change(bundle->full_config());
            *result = { 200, "" };
        } else {
            *result = { 404, "type must be printer, process or filament" };
        }
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (result->first != 200) { r.status = result->first; r.body = json_error(result->second); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_color(int index, const std::string& color)
{
    ApiResponse r;
    if (color.size() != 7 || color[0] != '#' || color.find_first_not_of("0123456789abcdefABCDEF", 1) != std::string::npos) {
        r.status = 404; r.body = json_error("color must be #RRGGBB"); return r;
    }
    auto result = std::make_shared<int>(500);
    bool ok     = run_on_main([result, index, color]() {
        auto& combos = wxGetApp().sidebar().combos_filament();
        if (index < 0 || index >= (int) combos.size()) { *result = 404; return; }
        combos[index]->ApplyFilamentColor(FilamentColor::FromColors({ color }, FilamentColorMode::Segment));
        *result = 200;
    });
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    if (*result != 200) { r.status = *result; r.body = json_error("no such filament slot"); return r; }
    r.body = "{\"ok\":true}";
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::api_filament_add()
{
    auto count = std::make_shared<size_t>(0);
    bool ok    = run_on_main([count]() {
        wxGetApp().sidebar().add_filament();
        *count = wxGetApp().preset_bundle->filament_presets.size();
    });
    ApiResponse r;
    if (!ok) { r.status = 503; r.body = json_error("the slicer is busy"); return r; }
    nlohmann::json j;
    j["filaments"] = *count;
    r.body = j.dump();
    return r;
}

RemoteAccess::ApiResponse RemoteAccess::handle_api(const std::string& method, const std::string& path, const std::string& query, const std::string& body)
{
    ApiResponse r;
    auto num = [](const std::string& s, int def) { try { return s.empty() ? def : std::stoi(s); } catch (...) { return def; } };
    if (path.empty() || path == "/") {
        nlohmann::json j;
        j["name"]    = "Snapmaker-Ultra remote API";
        j["version"] = 1;
        j["routes"]  = nlohmann::json::array({
            { {"method", "GET"},  {"path", "/api"},                        {"description", "this manifest"} },
            { {"method", "GET"},  {"path", "/api/plates"},                 {"description", "project, printer preset, filaments and every plate with objects, slice state, time and filament estimates"} },
            { {"method", "GET"},  {"path", "/api/plates/{index}/thumbnail.png"}, {"description", "rendered plate preview"} },
            { {"method", "GET"},  {"path", "/api/printers"},               {"description", "known printers with live status"} },
            { {"method", "POST"}, {"path", "/api/slice?plate={index}|all"}, {"description", "start slicing one plate (selects it) or all; returns a job id; 409 while slicing"} },
            { {"method", "GET"},  {"path", "/api/jobs"},                   {"description", "recent jobs"} },
            { {"method", "GET"},  {"path", "/api/jobs/{id}"},              {"description", "job state: running | done | error | cancelled, percent, text"} },
            { {"method", "GET"},  {"path", "/api/presets"},                {"description", "printer / process choices as the sidebar shows them, filament choices and the current filament slots with colours"} },
            { {"method", "POST"}, {"path", "/api/presets/select?type=printer|process|filament&name={value}[&index={slot}]"}, {"description", "select a preset the way the sidebar does; 409 when that preset has unsaved changes on the PC"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_color?index={slot}&color=%23RRGGBB"}, {"description", "set a filament slot colour"} },
            { {"method", "POST"}, {"path", "/api/presets/filament_add"},   {"description", "add a filament slot"} },
            { {"method", "GET"},  {"path", "/state"},                      {"description", "camera list for the stream wall (see /r/<token>/)"} }
        });
        r.body = j.dump();
        return r;
    }
    if (path == "/presets" && method == "GET")
        return api_presets();
    if (path == "/presets/select" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_select_preset(get("type"), get("name"), num(get("index"), -1));
    }
    if (path == "/presets/filament_color" && method == "POST") {
        auto get = [&](const char* k) { std::string v = query_param(query, k); return v.empty() ? query_param(body, k) : v; };
        return api_filament_color(num(get("index"), -1), get("color"));
    }
    if (path == "/presets/filament_add" && method == "POST")
        return api_filament_add();
    if (path == "/plates" && method == "GET")
        return api_plates();
    if (path.compare(0, 8, "/plates/") == 0 && method == "GET") {
        const std::string rest = path.substr(8);
        const size_t      slash = rest.find('/');
        if (slash != std::string::npos && rest.substr(slash) == "/thumbnail.png")
            return api_plate_thumbnail(num(rest.substr(0, slash), -1));
    }
    if (path == "/printers" && method == "GET")
        return api_printers();
    if (path == "/slice" && method == "POST") {
        std::string plate = query_param(query, "plate");
        if (plate.empty()) plate = query_param(body, "plate");
        return api_slice(num(plate, -1), plate == "all");
    }
    if (path == "/jobs" && method == "GET")
        return api_jobs(-1);
    if (path.compare(0, 6, "/jobs/") == 0 && method == "GET")
        return api_jobs(num(path.substr(6), -1));
    r.status = 404;
    r.body   = json_error("no such route; see /api");
    return r;
}

void RemoteAccess::accept_loop()
{
    auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor);
    for (;;) {
        boost::system::error_code ec;
        auto* sock = new tcp::socket(acceptor->get_executor());
        acceptor->accept(*sock, ec);
        if (ec) {
            delete sock;
            break; // closed by stop()
        }
        std::thread([this, sock]() { serve(sock); }).detach();
    }
    delete acceptor;
}

void RemoteAccess::serve(void* socket_ptr)
{
    std::unique_ptr<tcp::socket> owner(static_cast<tcp::socket*>(socket_ptr));
    tcp::socket&                 client = *owner;
    try {
        boost::system::error_code ec;
        if (!is_private_v4(client.remote_endpoint(ec).address()) || ec)
            return;
        client.set_option(tcp::no_delay(true));

        asio::streambuf req;
        asio::read_until(client, req, "\r\n\r\n");
        std::string head(asio::buffers_begin(req.data()), asio::buffers_end(req.data()));
        const size_t head_end = head.find("\r\n\r\n") + 4;
        std::string  pending  = head.substr(head_end); // any body bytes already read
        head.resize(head_end);

        std::istringstream first(head.substr(0, head.find("\r\n")));
        std::string        method, target, version;
        first >> method >> target >> version;
        std::string cookies;
        size_t      content_length = 0;
        {
            size_t pos = head.find("\r\n") + 2;
            while (pos < head_end - 2) {
                size_t nl = head.find("\r\n", pos);
                std::string line = head.substr(pos, nl - pos);
                std::string key  = line.substr(0, std::min<size_t>(15, line.size()));
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
                if (key.compare(0, 7, "cookie:") == 0)
                    cookies = line.substr(7);
                else if (key == "content-length:")
                    content_length = (size_t) std::max(0, std::atoi(line.c_str() + 15));
                pos = nl + 2;
            }
        }

        std::string token, port_go2rtc_unused;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            token = m_token;
        }
        const size_t q     = target.find('?');
        std::string  path  = q == std::string::npos ? target : target.substr(0, q);
        std::string  query = q == std::string::npos ? "" : target.substr(q + 1);

        // go2rtc player + websocket at the root, gated by the rt cookie.
        for (const char* p : GO2RTC_PASSTHROUGH) {
            if (path == p) {
                if (token.empty() || cookie_value(cookies, "rt") != token) {
                    respond(client, "404 Not Found", "text/plain", "not found");
                    return;
                }
                const int port = Go2RtcLauncher::get().port();
                if (port == 0) {
                    respond(client, "503 Service Unavailable", "text/plain", "stream relay is not running");
                    return;
                }
                tunnel(client, port, force_close(head), pending);
                return;
            }
        }

        const std::string prefix = "/r/" + token;
        if (token.empty() || path.compare(0, prefix.size(), prefix) != 0 ||
            (path.size() > prefix.size() && path[prefix.size()] != '/')) {
            respond(client, "404 Not Found", "text/plain", "not found");
            return;
        }
        std::string rest = path.substr(prefix.size());
        if (rest.empty()) { // make relative URLs on the page resolve under the token
            respond(client, "302 Found", "text/plain", "", "Location: " + prefix + "/\r\n");
            return;
        }
        if (rest == "/" || rest == "/index.html") {
            boost::nowide::ifstream f(resources_dir() + "/web/orca/stream_center.html", std::ios::binary);
            std::stringstream ss;
            ss << f.rdbuf();
            respond(client, "200 OK", "text/html; charset=utf-8", ss.str(),
                    "Set-Cookie: rt=" + token + "; Path=/; SameSite=Lax\r\n");
        } else if (rest == "/state") {
            respond(client, "200 OK", "application/json", state_for_phone());
        } else if (rest.compare(0, 4, "/api") == 0 && (rest.size() == 4 || rest[4] == '/')) {
            // Small request bodies only (form-encoded parameters).
            std::string body = pending;
            if (content_length > 64 * 1024) {
                respond(client, "413 Payload Too Large", "application/json", json_error("body too large"));
                return;
            }
            while (body.size() < content_length) {
                char   buf[4096];
                size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), content_length - body.size())));
                body.append(buf, n);
            }
            ApiResponse ar = handle_api(method, rest.substr(4), query, body);
            const char* status = ar.status == 200 ? "200 OK" : ar.status == 404 ? "404 Not Found" : ar.status == 409 ? "409 Conflict"
                               : ar.status == 413 ? "413 Payload Too Large" : ar.status == 503 ? "503 Service Unavailable" : "500 Internal Server Error";
            respond(client, status, ar.type, ar.body);
        } else if (rest == "/bambu") {
            std::string ip, code;
            if (!lookup_host(query_param(query, "id"), ip, code) || code.empty()) {
                respond(client, "404 Not Found", "text/plain", "unknown camera");
                return;
            }
            const int relay = BambuCamRelay::get().port();
            if (relay == 0) {
                respond(client, "503 Service Unavailable", "text/plain", "camera relay is not running");
                return;
            }
            const std::string new_head = "GET /bambu?ip=" + percent_encode(ip) + "&code=" + percent_encode(code) + " HTTP/1.1\r\n" +
                                         head.substr(head.find("\r\n") + 2);
            tunnel(client, relay, force_close(new_head), "");
        } else if (rest == "/ff") {
            std::string ip, code;
            if (!lookup_host(query_param(query, "id"), ip, code) || ip.find_first_of("\"'\\<>") != std::string::npos) {
                respond(client, "404 Not Found", "text/plain", "unknown printer");
                return;
            }
            std::string url;
            auto http = Http::post("http://" + ip + ":8898/detail");
            http.timeout_connect(4)
                .timeout_max(8)
                .header("Content-Type", "application/json")
                .set_post_body(std::string("{\"serialNumber\":\"\",\"checkCode\":\"\"}"))
                .on_complete([&url](std::string body, unsigned) { url = ff_camera_url_from_detail(body); })
                .perform_sync();
            nlohmann::json j;
            j["url"] = url;
            respond(client, "200 OK", "application/json", j.dump());
        } else {
            respond(client, "404 Not Found", "text/plain", "not found");
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "RemoteAccess: session ended: " << e.what();
    }
}

} // namespace GUI
} // namespace Slic3r
