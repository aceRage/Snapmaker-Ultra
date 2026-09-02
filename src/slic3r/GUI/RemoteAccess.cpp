#include "RemoteAccess.hpp"

#include "BambuCamRelay.hpp"
#include "slic3r/Utils/Http.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/asio.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <chrono>
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
        {
            size_t pos = head.find("\r\n") + 2;
            while (pos < head_end - 2) {
                size_t nl = head.find("\r\n", pos);
                std::string line = head.substr(pos, nl - pos);
                std::string key  = line.substr(0, std::min<size_t>(7, line.size()));
                std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char) std::tolower(c); });
                if (key == "cookie:")
                    cookies = line.substr(7);
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
