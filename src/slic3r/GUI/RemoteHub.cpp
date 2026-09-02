// The hub process (see RemoteHub.hpp). Deliberately wx-free: it runs from CLI::run before
// any wxApp exists, so only Boost.Asio, libcurl (Http), nlohmann::json and the OS are used.
#include "RemoteHub.hpp"

#include "BambuCamRelay.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>
#include <boost/dll/runtime_symbol_info.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <ctime>
#include <memory>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

#ifdef _WIN32
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <winsock2.h>
#  include <ws2tcpip.h>
#  include <iphlpapi.h>
#  include <netioapi.h>
#  include <windows.h>
#  pragma comment(lib, "iphlpapi.lib")
#else
#  include <signal.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace Slic3r {
namespace GUI {
namespace RemoteHub {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;
using json     = nlohmann::json;

static const int         HUB_PORT           = 13640;
static const int         GO2RTC_API_PORT    = 21984;
static const char* const GO2RTC_PASSTHROUGH[] = { "/stream.html", "/video-stream.js", "/video-rtc.js", "/api/ws" };
static const size_t      MAX_API_BODY       = 64 * 1024;
static const uint64_t    MAX_UPLOAD         = 2ull * 1024 * 1024 * 1024;
static const int         IDLE_EXIT_SECONDS  = 60;

// ------------------------------------------------------------------ paths ----

std::string hub_dir()       { return (fs::path(data_dir()) / "hub").string(); }
std::string instances_dir() { return (fs::path(hub_dir()) / "instances").string(); }
std::string uploads_dir()   { return (fs::path(hub_dir()) / "uploads").string(); }
std::string saves_dir()     { return (fs::path(hub_dir()) / "saves").string(); }
static std::string hub_json_path()     { return (fs::path(hub_dir()) / "hub.json").string(); }
static std::string streams_json_path() { return (fs::path(hub_dir()) / "streams.json").string(); }

static void ensure_dirs()
{
    boost::system::error_code ec;
    for (const std::string& d : { hub_dir(), instances_dir(), uploads_dir(), saves_dir() })
        fs::create_directories(d, ec);
}

static std::string read_file(const std::string& path)
{
    boost::nowide::ifstream f(path, std::ios::binary);
    if (!f) return "";
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static bool write_file(const std::string& path, const std::string& data)
{
    const std::string tmp = path + ".tmp";
    {
        boost::nowide::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
    }
    boost::system::error_code ec;
    fs::rename(tmp, path, ec);
    if (ec) { // rename over an open file can fail on Windows: write in place instead
        boost::nowide::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f << data;
        fs::remove(tmp, ec);
    }
    return true;
}

// ---------------------------------------------------------------- helpers ----

static bool is_private_v4(const asio::ip::address& a)
{
    if (!a.is_v4()) return false;
    const uint32_t v = a.to_v4().to_uint();
    return (v >> 24) == 10 || (v >> 24) == 127 || (v >> 20) == 0xAC1 || (v >> 16) == 0xC0A8 || (v >> 16) == 0xA9FE;
}

static std::string percent_encode(const std::string& s)
{
    static const char* hex = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hex[c >> 4]; out += hex[c & 15]; }
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
        } else if (s[i] == '+') out += ' ';
        else out += s[i];
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
        if (eq != std::string::npos && kv.substr(0, eq) == key) return percent_decode(kv.substr(eq + 1));
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
        if (eq != std::string::npos && kv.substr(0, eq) == name) return kv.substr(eq + 1);
        pos = semi + 1;
    }
    return "";
}

static bool valid_token(const std::string& t)
{
    return t.size() >= 10 && t.size() <= 32 && t.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789") == std::string::npos;
}

static std::string random_token()
{
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789"; // no 0/o/1/l look-alikes
    std::random_device rd;
    std::mt19937_64    gen(((uint64_t) rd() << 32) ^ rd());
    std::uniform_int_distribution<int> d(0, (int) sizeof(alphabet) - 2);
    std::string t;
    for (int i = 0; i < 14; ++i) t += alphabet[d(gen)];
    return t;
}

static long long now_unix() { return (long long) std::time(nullptr); }

static std::string timestamp_compact()
{
    std::time_t t = std::time(nullptr);
    std::tm tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d-%H%M%S", &tm);
    return buf;
}

static std::string lower(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return (char) std::tolower(c); });
    return s;
}

static std::string json_error(const std::string& msg)
{
    json j;
    j["error"] = msg;
    return j.dump();
}

static const char* status_text(int status)
{
    switch (status) {
    case 200: return "200 OK";                   case 202: return "202 Accepted";
    case 302: return "302 Found";                case 400: return "400 Bad Request";
    case 404: return "404 Not Found";            case 409: return "409 Conflict";
    case 413: return "413 Payload Too Large";    case 502: return "502 Bad Gateway";
    case 503: return "503 Service Unavailable";  default:  return "500 Internal Server Error";
    }
}

#ifdef _WIN32
// The address of the interface that owns the 0.0.0.0/0 route with the best metric (VPNs
// usually route through 0.0.0.0/1 + 128.0.0.0/1, so this stays the real LAN adapter).
static std::string default_route_ipv4_win()
{
    std::string           out;
    PMIB_IPFORWARD_TABLE2 routes = nullptr;
    if (GetIpForwardTable2(AF_INET, &routes) != NO_ERROR || !routes) return out;
    ULONG best_if = 0, best_metric = ~0UL;
    for (ULONG i = 0; i < routes->NumEntries; ++i) {
        const MIB_IPFORWARD_ROW2& r = routes->Table[i];
        if (r.DestinationPrefix.PrefixLength != 0) continue;
        MIB_IPINTERFACE_ROW iface = {};
        iface.Family              = AF_INET;
        iface.InterfaceIndex      = r.InterfaceIndex;
        ULONG metric = r.Metric + (GetIpInterfaceEntry(&iface) == NO_ERROR ? iface.Metric : 0);
        if (metric < best_metric) { best_metric = metric; best_if = r.InterfaceIndex; }
    }
    FreeMibTable(routes);
    if (best_if == 0) return out;
    PMIB_UNICASTIPADDRESS_TABLE addrs = nullptr;
    if (GetUnicastIpAddressTable(AF_INET, &addrs) != NO_ERROR || !addrs) return out;
    for (ULONG i = 0; i < addrs->NumEntries; ++i) {
        const MIB_UNICASTIPADDRESS_ROW& a = addrs->Table[i];
        if (a.InterfaceIndex == best_if && a.DadState == IpDadStatePreferred) {
            char buf[INET_ADDRSTRLEN] = {};
            if (inet_ntop(AF_INET, (void*) &a.Address.Ipv4.sin_addr, buf, sizeof(buf))) out = buf;
            break;
        }
    }
    FreeMibTable(addrs);
    return out;
}
#endif

static std::vector<std::string> lan_ips()
{
    std::vector<std::string> out;
#ifdef _WIN32
    {
        const std::string a = default_route_ipv4_win();
        if (!a.empty()) out.push_back(a);
    }
#endif
    try {
        asio::io_context      ioc;
        asio::ip::udp::socket s(ioc);
        s.open(asio::ip::udp::v4());
        s.connect(asio::ip::udp::endpoint(asio::ip::make_address_v4("8.8.8.8"), 53)); // sends nothing
        const std::string a = s.local_endpoint().address().to_string();
        if (a != "0.0.0.0" && std::find(out.begin(), out.end(), a) == out.end()) out.push_back(a);
    } catch (...) {}
    try {
        asio::io_context ioc;
        tcp::resolver    r(ioc);
        for (const auto& e : r.resolve(asio::ip::host_name(), "")) {
            const auto a = e.endpoint().address();
            if (a.is_v4() && !a.is_loopback()) {
                const std::string s = a.to_string();
                if (std::find(out.begin(), out.end(), s) == out.end()) out.push_back(s);
            }
        }
    } catch (...) {}
    return out;
}

// ------------------------------------------------------------ processes ----

static bool pid_alive(long pid)
{
    if (pid <= 0) return false;
#ifdef _WIN32
    HANDLE h = ::OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, (DWORD) pid);
    if (!h) return false;
    DWORD code = 0;
    const bool alive = ::GetExitCodeProcess(h, &code) && code == STILL_ACTIVE;
    ::CloseHandle(h);
    return alive;
#else
    return ::kill((pid_t) pid, 0) == 0;
#endif
}

static long current_pid()
{
#ifdef _WIN32
    return (long) ::GetCurrentProcessId();
#else
    return (long) ::getpid();
#endif
}

static std::string current_exe()
{
#ifdef __linux__
    // Inside an AppImage, start the AppImage, not the unpacked binary.
    if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) return appimage;
#endif
    boost::system::error_code ec;
    return boost::dll::program_location(ec).string();
}

#ifdef _WIN32
static std::wstring widen(const std::string& s)
{
    if (s.empty()) return L"";
    int n = ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, s.c_str(), (int) s.size(), &w[0], n);
    return w;
}

static std::wstring quote_arg(const std::wstring& a)
{
    if (!a.empty() && a.find_first_of(L" \t\"") == std::wstring::npos) return a;
    std::wstring out = L"\"";
    size_t       bs  = 0;
    for (wchar_t c : a) {
        if (c == L'\\') { ++bs; continue; }
        if (c == L'"') { out.append(bs * 2 + 1, L'\\'); out += c; bs = 0; continue; }
        out.append(bs, L'\\'); bs = 0;
        out += c;
    }
    out.append(bs * 2, L'\\');
    return out + L"\"";
}
#endif

// Start a process that outlives us. `env` entries are added to the child's environment;
// `job` (Windows Job Object handle) ties the child to OUR lifetime instead.
static long spawn_process(const std::vector<std::string>& args, const std::vector<std::pair<std::string, std::string>>& env,
                          bool hide_console, void* job)
{
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quote_arg(widen(a));
    }
    std::vector<std::pair<std::wstring, std::wstring>> saved;
    for (const auto& kv : env) {
        wchar_t old[4096] = {};
        DWORD   n         = ::GetEnvironmentVariableW(widen(kv.first).c_str(), old, 4096);
        saved.emplace_back(widen(kv.first), n > 0 && n < 4096 ? std::wstring(old) : std::wstring());
        ::SetEnvironmentVariableW(widen(kv.first).c_str(), widen(kv.second).c_str());
    }
    STARTUPINFOW        si = {};
    PROCESS_INFORMATION pi = {};
    si.cb                  = sizeof(si);
    DWORD flags            = CREATE_UNICODE_ENVIRONMENT | (hide_console ? CREATE_NO_WINDOW : 0);
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    BOOL ok = FALSE;
    if (!job) {
        // Leave any job we are in, so the child is not killed together with us.
        std::vector<wchar_t> buf2 = buf;
        ok = ::CreateProcessW(nullptr, buf2.data(), nullptr, nullptr, FALSE, flags | CREATE_BREAKAWAY_FROM_JOB, nullptr, nullptr, &si, &pi);
    }
    if (!ok)
        ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, FALSE, flags, nullptr, nullptr, &si, &pi);
    for (const auto& kv : saved)
        ::SetEnvironmentVariableW(kv.first.c_str(), kv.second.empty() ? nullptr : kv.second.c_str());
    if (!ok) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: CreateProcess failed (" << ::GetLastError() << ") for " << args.front();
        return 0;
    }
    if (job) ::AssignProcessToJobObject((HANDLE) job, pi.hProcess);
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    return (long) pi.dwProcessId;
#else
    (void) hide_console; (void) job;
    // Double fork: the grandchild is re-parented to init, so nobody has to reap it.
    int pipefd[2];
    if (::pipe(pipefd) != 0) return 0;
    pid_t child = ::fork();
    if (child < 0) { ::close(pipefd[0]); ::close(pipefd[1]); return 0; }
    if (child == 0) {
        ::close(pipefd[0]);
        ::setsid();
        pid_t grand = ::fork();
        if (grand == 0) {
            ::close(pipefd[1]);
            for (const auto& kv : env) ::setenv(kv.first.c_str(), kv.second.c_str(), 1);
            std::vector<char*> argv;
            for (const std::string& a : args) argv.push_back(const_cast<char*>(a.c_str()));
            argv.push_back(nullptr);
            ::execv(args.front().c_str(), argv.data());
            ::_exit(127);
        }
        long gp = (long) grand;
        (void) !::write(pipefd[1], &gp, sizeof(gp));
        ::_exit(0);
    }
    ::close(pipefd[1]);
    long gp = 0;
    (void) !::read(pipefd[0], &gp, sizeof(gp));
    ::close(pipefd[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    return gp;
#endif
}

// ------------------------------------------------------------ HTTP bits ----

struct Request
{
    std::string method, target, path, query, head, pending, cookies, file_name;
    size_t      content_length { 0 };
};

static bool read_request(tcp::socket& client, Request& r)
{
    asio::streambuf req;
    boost::system::error_code ec;
    asio::read_until(client, req, "\r\n\r\n", ec);
    if (ec) return false;
    std::string head(asio::buffers_begin(req.data()), asio::buffers_end(req.data()));
    const size_t head_end = head.find("\r\n\r\n") + 4;
    r.pending             = head.substr(head_end);
    head.resize(head_end);
    r.head = head;
    std::istringstream first(head.substr(0, head.find("\r\n")));
    std::string        version;
    first >> r.method >> r.target >> version;
    size_t pos = head.find("\r\n") + 2;
    while (pos < head_end - 2) {
        size_t      nl   = head.find("\r\n", pos);
        std::string line = head.substr(pos, nl - pos);
        size_t      colon = line.find(':');
        if (colon != std::string::npos) {
            std::string key = lower(line.substr(0, colon));
            std::string val = line.substr(colon + 1);
            size_t b = val.find_first_not_of(' ');
            val = b == std::string::npos ? "" : val.substr(b);
            if (key == "cookie") r.cookies = val;
            else if (key == "content-length") r.content_length = (size_t) std::max(0LL, std::atoll(val.c_str()));
            else if (key == "x-file-name") r.file_name = val;
        }
        pos = nl + 2;
    }
    const size_t q = r.target.find('?');
    r.path         = q == std::string::npos ? r.target : r.target.substr(0, q);
    r.query        = q == std::string::npos ? "" : r.target.substr(q + 1);
    return !r.method.empty();
}

static void write_all(tcp::socket& s, const std::string& data)
{
    boost::system::error_code ec;
    asio::write(s, asio::buffer(data), ec);
}

static void respond(tcp::socket& s, int status, const std::string& type, const std::string& body, const std::string& extra_headers = "")
{
    std::ostringstream o;
    o << "HTTP/1.1 " << status_text(status) << "\r\n"
      << "Content-Type: " << type << "\r\n"
      << "Content-Length: " << body.size() << "\r\n"
      << "Cache-Control: no-store\r\n"
      << extra_headers
      << "Connection: close\r\n\r\n"
      << body;
    write_all(s, o.str());
}

static void respond_json(tcp::socket& s, int status, const std::string& body) { respond(s, status, "application/json", body); }

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
        std::string key  = lower(line.substr(0, std::min<size_t>(11, line.size())));
        if (key.compare(0, 8, "upgrade:") == 0) upgrade = true;
        if (key.compare(0, 11, "connection:") != 0 && !line.empty()) out += line + "\r\n";
        pos = nl + 2;
    }
    if (upgrade) return head;
    return out + "Connection: close\r\n\r\n";
}

static void pump(tcp::socket& from, tcp::socket& to)
{
    char                      buf[16384];
    boost::system::error_code ec;
    for (;;) {
        size_t n = from.read_some(asio::buffer(buf), ec);
        if (ec) break;
        asio::write(to, asio::buffer(buf, n), ec);
        if (ec) break;
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
    if (!pending.empty()) write_all(up, pending);
    std::thread t([&]() { pump(client, up); });
    pump(up, client);
    t.join();
}

// Read a small request body (form parameters, JSON).
static bool read_small_body(tcp::socket& client, Request& r, std::string& body, size_t limit)
{
    if (r.content_length > limit) return false;
    body = r.pending;
    boost::system::error_code ec;
    while (body.size() < r.content_length) {
        char   buf[8192];
        size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), r.content_length - body.size())), ec);
        if (ec) return false;
        body.append(buf, n);
    }
    return true;
}

static std::string ff_camera_url_from_detail(const std::string& body)
{
    try {
        json j = json::parse(body);
        if (j.contains("detail") && j["detail"].is_object() && j["detail"].contains("cameraStreamUrl"))
            return j["detail"]["cameraStreamUrl"].get<std::string>();
        if (j.contains("cameraStreamUrl")) return j["cameraStreamUrl"].get<std::string>();
    } catch (...) {}
    return "";
}

// A phone upload: stream the body to <uploads>/<stamp>_<name>. Only model files, only
// a bare file name (X-File-Name, percent-encoded UTF-8), Content-Length required.
static bool spool_upload(tcp::socket& client, Request& r, std::string& out_path, std::string& error)
{
    std::string name = percent_decode(r.file_name);
    const size_t slash = name.find_last_of("/\\");
    if (slash != std::string::npos) name = name.substr(slash + 1);
    std::string clean;
    for (unsigned char c : name)
        if (c >= 32 && std::string("<>:\"|?*").find((char) c) == std::string::npos) clean += (char) c;
    while (!clean.empty() && (clean.back() == ' ' || clean.back() == '.')) clean.pop_back();
    if (clean.size() > 100) clean = clean.substr(clean.size() - 100);
    if (clean.empty()) { error = "X-File-Name header with the file name is required"; return false; }
    const std::string ext = lower(fs::path(clean).extension().string());
    if (ext != ".3mf" && ext != ".stl" && ext != ".obj" && ext != ".step" && ext != ".stp") {
        error = "only .3mf, .stl, .obj and .step files can be opened";
        return false;
    }
    if (r.content_length == 0) { error = "empty upload (Content-Length required)"; return false; }
    if (r.content_length > MAX_UPLOAD) { error = "file too large"; return false; }
    ensure_dirs();
    // One folder per upload so the file keeps its own name (it becomes the object / project name).
    const fs::path folder = fs::path(uploads_dir()) / timestamp_compact();
    boost::system::error_code ec;
    fs::create_directories(folder, ec);
    out_path = (folder / clean).string();
    boost::nowide::ofstream f(out_path, std::ios::binary | std::ios::trunc);
    if (!f) { error = "cannot write to " + uploads_dir(); return false; }
    size_t received = std::min(r.pending.size(), r.content_length);
    f.write(r.pending.data(), (std::streamsize) received);
    char buf[65536];
    while (received < r.content_length) {
        size_t n = client.read_some(asio::buffer(buf, std::min(sizeof(buf), r.content_length - received)), ec);
        if (ec) { error = "upload interrupted"; f.close(); fs::remove(out_path, ec); return false; }
        f.write(buf, (std::streamsize) n);
        received += n;
    }
    f.close();
    return true;
}

// --------------------------------------------------------------- server ----

struct Instance
{
    long        pid { 0 };
    int         port { 0 };
    long long   started { 0 };
    std::string title, path;
    bool        slicing { false };
    bool        alive { false };
};

class HubServer
{
public:
    HubServer(std::string token, bool phone) : m_token(std::move(token)), m_phone(phone) {}
    int run();

private:
    json  info_json();
    void  write_hub_json();
    bool  bind(bool lan);
    void  accept_loop(std::shared_ptr<tcp::acceptor> acceptor);
    void  serve(std::unique_ptr<tcp::socket> sock);
    void  handle_hub(tcp::socket& client, Request& r);
    void  handle_phone(tcp::socket& client, Request& r, const std::string& rest);
    void  start_go2rtc();
    void  register_streams();
    std::string state_for_phone();
    bool  lookup_host(const std::string& id, std::string& ip, std::string& code);
    std::vector<Instance> instances(bool probe);
    static Instance probe_instance(Instance inst);

    std::mutex                     m_mutex;
    std::string                    m_token;
    bool                           m_phone { false };
    bool                           m_lan { false };
    int                            m_port { 0 };
    asio::io_context               m_ioc;
    std::shared_ptr<tcp::acceptor> m_acceptor;
    std::string                    m_state; // full Stream-tab state JSON (with credentials)
    int                            m_go2rtc_port { 0 };
    long                           m_go2rtc_pid { 0 };
    void*                          m_job { nullptr };
    std::atomic<bool>              m_quit { false };
};

json HubServer::info_json()
{
    json j;
    std::lock_guard<std::mutex> lock(m_mutex);
    j["alive"]       = true;
    j["pid"]         = current_pid();
    j["port"]        = m_port;
    j["phone"]       = m_phone;
    j["token"]       = m_token;
    j["go2rtc_port"] = m_go2rtc_port;
    j["relay_port"]  = BambuCamRelay::get().port();
    j["version"]     = std::string(SLIC3R_VERSION);
    j["ips"]         = json::array();
    j["url"]         = "";
    if (m_phone) {
        const std::vector<std::string> ips = lan_ips();
        j["ips"] = ips;
        if (!ips.empty()) j["url"] = "http://" + ips.front() + ":" + std::to_string(m_port) + "/r/" + m_token + "/";
    }
    return j;
}

void HubServer::write_hub_json()
{
    json j;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        j["pid"]         = current_pid();
        j["port"]        = m_port;
        j["phone"]       = m_phone;
        j["token"]       = m_token;
        j["go2rtc_port"] = m_go2rtc_port;
        j["version"]     = std::string(SLIC3R_VERSION);
    }
    write_file(hub_json_path(), j.dump(2));
}

bool HubServer::bind(bool lan)
{
    auto acceptor = std::make_shared<tcp::acceptor>(m_ioc);
    boost::system::error_code ec;
    acceptor->open(tcp::v4(), ec);
    if (ec) return false;
#ifndef _WIN32
    acceptor->set_option(tcp::acceptor::reuse_address(true), ec); // on Windows this would let two hubs share the port
#endif
    int port = HUB_PORT;
    for (; port < HUB_PORT + 20; ++port) {
        acceptor->bind(tcp::endpoint(lan ? asio::ip::address_v4::any() : asio::ip::address_v4::loopback(), (unsigned short) port), ec);
        if (!ec) break;
    }
    if (ec) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: no free port from " << HUB_PORT << ": " << ec.message();
        return false;
    }
    acceptor->listen(64, ec);
    if (ec) return false;
    std::shared_ptr<tcp::acceptor> old;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        old        = m_acceptor;
        m_acceptor = acceptor;
        m_lan      = lan;
        m_port     = port;
    }
    if (old) { boost::system::error_code ig; old->close(ig); } // its accept loop exits
    std::thread([this, acceptor]() { accept_loop(acceptor); }).detach();
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: listening on " << (lan ? "0.0.0.0" : "127.0.0.1") << ":" << port;
    return true;
}

void HubServer::accept_loop(std::shared_ptr<tcp::acceptor> acceptor)
{
    for (;;) {
        auto sock = std::make_unique<tcp::socket>(m_ioc);
        boost::system::error_code ec;
        acceptor->accept(*sock, ec);
        if (ec) break; // closed by a rebind or at shutdown
        tcp::socket* raw = sock.release();
        std::thread([this, raw]() { serve(std::unique_ptr<tcp::socket>(raw)); }).detach();
    }
}

void HubServer::start_go2rtc()
{
#ifdef _WIN32
    const std::string exe = resources_dir() + "/tools/go2rtc/go2rtc.exe";
    if (!fs::exists(exe)) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: missing " << exe;
        return;
    }
    // An orphan from a crashed hub may still own the port: reuse it rather than fail.
    bool alive = false;
    Http::get("http://127.0.0.1:" + std::to_string(GO2RTC_API_PORT) + "/api")
        .timeout_connect(1).timeout_max(2)
        .on_complete([&alive](std::string, unsigned status) { alive = status < 500; })
        .perform_sync();
    if (alive) {
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: go2rtc already answering on " << GO2RTC_API_PORT;
        m_go2rtc_port = GO2RTC_API_PORT;
        return;
    }
    const std::string cfg_path = (fs::path(hub_dir()) / "go2rtc.yaml").string();
    {
        boost::nowide::ofstream cfg(cfg_path);
        cfg << "api:\n  listen: \"127.0.0.1:" << GO2RTC_API_PORT << "\"\n  origin: \"*\"\n"
            << "rtsp:\n  listen: \"\"\n"
            << "webrtc:\n  listen: \"\"\n"
            << "srtp:\n  listen: \"\"\n";
    }
    if (!m_job) {
        HANDLE job = ::CreateJobObjectW(nullptr, nullptr);
        if (job) {
            JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
            jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE; // go2rtc dies with the hub
            ::SetInformationJobObject(job, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
            m_job = job;
        }
    }
    m_go2rtc_pid = spawn_process({ exe, "-config", cfg_path }, {}, true, m_job);
    if (m_go2rtc_pid <= 0) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: failed to start go2rtc";
        return;
    }
    m_go2rtc_port = GO2RTC_API_PORT;
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: go2rtc pid " << m_go2rtc_pid << " on 127.0.0.1:" << GO2RTC_API_PORT;
#endif
}

void HubServer::register_streams()
{
    std::string state;
    int         port;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
        port  = m_go2rtc_port;
    }
    if (port == 0 || state.empty()) return;
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            const std::string name = h.value("rname", ""), src = h.value("rsrc", "");
            if (name.empty() || src.empty()) continue;
            // Http::put() is a file-upload PUT (its read callback dereferences the missing
            // file and crashes); put2() is a plain PUT with no body, which is what go2rtc wants.
            const std::string url = "http://127.0.0.1:" + std::to_string(port) + "/api/streams?name=" + name + "&src=" + percent_encode(src);
            std::thread([url]() {
                for (int attempt = 0; attempt < 3; ++attempt) {
                    bool ok = false;
                    Http::put2(url).timeout_connect(2).timeout_max(5).on_complete([&ok](std::string, unsigned) { ok = true; }).perform_sync();
                    if (ok) return;
                    std::this_thread::sleep_for(std::chrono::milliseconds(1500)); // go2rtc may still be starting
                }
            }).detach();
        }
    } catch (...) {}
}

// The phone only sees ids, aliases, the source kind, go2rtc stream names and direct
// printer-page URLs. Addresses, access codes and camera credentials stay here.
std::string HubServer::state_for_phone()
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    json out;
    out["hosts"]  = json::array();
    out["active"] = json::array();
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            json p;
            p["id"]    = h.value("id", "");
            p["alias"] = h.value("alias", "");
            p["rkind"] = h.value("rkind", "");
            p["rname"] = h.value("rname", "");
            p["rurl"]  = h.value("rurl", "");
            out["hosts"].push_back(p);
        }
        out["active"] = j.value("active", json::array());
    } catch (...) {}
    return out.dump();
}

bool HubServer::lookup_host(const std::string& id, std::string& ip, std::string& code)
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array()))
            if (h.value("id", "") == id) {
                ip   = h.value("ip", "");
                code = h.value("code", "");
                return !ip.empty();
            }
    } catch (...) {}
    return false;
}

Instance HubServer::probe_instance(Instance inst)
{
    Http::get("http://127.0.0.1:" + std::to_string(inst.port) + "/api/info")
        .timeout_connect(1).timeout_max(3)
        .on_complete([&inst](std::string body, unsigned status) {
            if (status != 200) return;
            try {
                json j        = json::parse(body);
                inst.alive    = j.value("pid", 0L) == inst.pid;
                inst.title    = j.value("title", "");
                inst.path     = j.value("path", "");
                inst.slicing  = j.value("slicing", false);
            } catch (...) {}
        })
        .perform_sync();
    return inst;
}

// Every slicer instance drops <instances>/<pid>.json when its loopback API starts; a dead
// pid means a crash and the file is dropped here.
std::vector<Instance> HubServer::instances(bool probe)
{
    std::vector<Instance> out;
    boost::system::error_code ec;
    if (!fs::is_directory(instances_dir(), ec)) return out;
    for (fs::directory_iterator it(instances_dir(), ec), end; !ec && it != end; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        Instance inst;
        try {
            json j       = json::parse(read_file(it->path().string()));
            inst.pid     = j.value("pid", 0L);
            inst.port    = j.value("port", 0);
            inst.started = j.value("started", 0LL);
        } catch (...) {}
        if (inst.pid <= 0 || inst.port <= 0 || !pid_alive(inst.pid)) {
            boost::system::error_code ig;
            fs::remove(it->path(), ig);
            continue;
        }
        inst.alive = true;
        out.push_back(inst);
    }
    std::sort(out.begin(), out.end(), [](const Instance& a, const Instance& b) { return a.started != b.started ? a.started < b.started : a.pid < b.pid; });
    if (probe) {
        std::vector<std::thread> threads;
        for (Instance& inst : out) threads.emplace_back([&inst]() { inst = probe_instance(inst); });
        for (std::thread& t : threads) t.join();
    }
    return out;
}

void HubServer::handle_hub(tcp::socket& client, Request& r)
{
    if (r.path == "/hub/info" && r.method == "GET") {
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/state" && r.method == "POST") {
        std::string body;
        if (!read_small_body(client, r, body, 4 * 1024 * 1024)) { respond_json(client, 413, json_error("state too large")); return; }
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_state = body;
        }
        write_file(streams_json_path(), body);
        register_streams();
        respond_json(client, 200, "{\"ok\":true}");
    } else if (r.path == "/hub/phone" && r.method == "POST") {
        const bool        on    = query_param(r.query, "on") == "1";
        const std::string token = query_param(r.query, "token"); // a remembered link to keep
        bool              changed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            changed = on != m_phone;
            m_phone = on;
        }
        if (changed) {
            // A new link every time it is turned on (as the PC page promises), unless the
            // caller brings the one it remembered.
            if (on) { std::lock_guard<std::mutex> lock(m_mutex); m_token = valid_token(token) ? token : random_token(); }
            bind(on);
            write_hub_json();
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: phone access " << (on ? "on" : "off");
        }
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/quit" && r.method == "POST") {
        respond_json(client, 200, "{\"ok\":true}");
        m_quit = true;
    } else {
        respond_json(client, 404, json_error("no such hub route"));
    }
}

void HubServer::handle_phone(tcp::socket& client, Request& r, const std::string& rest)
{
    // ---- hub-level API (instances, uploads) ----
    if (rest == "/api" || rest == "/api/") {
        json j;
        j["name"]    = "Snapmaker-Ultra hub API";
        j["version"] = 2;
        j["routes"]  = json::array({
            { {"method", "GET"},  {"path", "/api/instances"},       {"description", "running slicer instances: id (pid), index, title, project path, slicing"} },
            { {"method", "POST"}, {"path", "/api/instances/open"},  {"description", "body = a .3mf/.stl/.obj/.step file, header X-File-Name = its name; starts a new slicer instance with it"} },
            { {"method", "POST"}, {"path", "/i/{id}/open?mode=load|import"}, {"description", "same upload, opened in instance {id}: load = save the current project, then open this project (default for .3mf); import = add the model to the current plate (default otherwise)"} },
            { {"method", "*"},    {"path", "/i/{id}/api/..."},      {"description", "the instance's own API (see GET /i/{id}/api)"} },
            { {"method", "GET"},  {"path", "/state"},               {"description", "camera list for the stream wall"} }
        });
        respond_json(client, 200, j.dump());
        return;
    }
    if (rest == "/api/instances" && r.method == "GET") {
        json j;
        j["instances"] = json::array();
        int index      = 0;
        for (const Instance& inst : instances(true)) {
            if (!inst.alive) continue;
            json ji;
            ji["id"]      = inst.pid;
            ji["index"]   = ++index;
            ji["pid"]     = inst.pid;
            ji["title"]   = inst.title;
            ji["path"]    = inst.path;
            ji["slicing"] = inst.slicing;
            j["instances"].push_back(ji);
        }
        respond_json(client, 200, j.dump());
        return;
    }
    if (rest == "/api/instances/open" && r.method == "POST") {
        std::string path, error;
        if (!spool_upload(client, r, path, error)) { respond_json(client, 400, json_error(error)); return; }
        const long pid = spawn_process({ current_exe(), path }, { { "SNORCA_NEW_INSTANCE", "1" } }, false, nullptr);
        json j;
        j["ok"]      = pid > 0;
        j["file"]    = path;
        j["spawned"] = pid;
        respond_json(client, pid > 0 ? 200 : 500, pid > 0 ? j.dump() : json_error("could not start a new slicer"));
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: new instance pid " << pid << " for " << path;
        return;
    }
    // ---- per-instance: /i/<pid>/open, /i/<pid>/api/... ----
    if (rest.compare(0, 3, "/i/") == 0) {
        const size_t slash = rest.find('/', 3);
        const long   pid   = std::atol(rest.substr(3, slash == std::string::npos ? std::string::npos : slash - 3).c_str());
        const std::string sub = slash == std::string::npos ? "" : rest.substr(slash);
        Instance target;
        for (const Instance& inst : instances(false))
            if (inst.pid == pid) target = inst;
        if (!target.alive) { respond_json(client, 404, json_error("no such slicer instance")); return; }
        const std::string base = "http://127.0.0.1:" + std::to_string(target.port);
        if (sub == "/open" && r.method == "POST") {
            std::string path, error;
            if (!spool_upload(client, r, path, error)) { respond_json(client, 400, json_error(error)); return; }
            std::string mode = query_param(r.query, "mode");
            if (mode.empty()) mode = lower(fs::path(path).extension().string()) == ".3mf" ? "load" : "import";
            int         status = 502;
            std::string body   = json_error("the slicer did not answer");
            Http::post(base + "/api/project/open")
                .timeout_connect(2).timeout_max(900)
                .header("Content-Type", "application/x-www-form-urlencoded")
                .set_post_body("path=" + percent_encode(path) + "&mode=" + mode)
                .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
                .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? json_error(err) : b; })
                .perform_sync();
            respond_json(client, status, body);
            return;
        }
        if (sub.compare(0, 4, "/api") == 0 && (sub.size() == 4 || sub[4] == '/')) {
            if (r.content_length > MAX_API_BODY) { respond_json(client, 413, json_error("body too large")); return; }
            // Replay the request against the instance with the prefix stripped; the instance
            // answers with Connection: close, so the splice ends by itself.
            const std::string line = r.method + " " + sub + (r.query.empty() ? "" : "?" + r.query) + " HTTP/1.1\r\n";
            const std::string head = line + r.head.substr(r.head.find("\r\n") + 2);
            try {
                tunnel(client, target.port, force_close(head), r.pending);
            } catch (const std::exception& e) {
                respond_json(client, 502, json_error(std::string("slicer unreachable: ") + e.what()));
            }
            return;
        }
        respond_json(client, 404, json_error("no such route; see /api"));
        return;
    }
    respond_json(client, 404, json_error("no such route; see /api"));
}

void HubServer::serve(std::unique_ptr<tcp::socket> owner)
{
    tcp::socket& client = *owner;
    try {
        boost::system::error_code ec;
        const auto peer = client.remote_endpoint(ec).address();
        if (ec || !is_private_v4(peer)) return;
        client.set_option(tcp::no_delay(true));
        Request r;
        if (!read_request(client, r)) return;

        std::string token;
        int         go2rtc_port;
        bool        phone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            token       = m_token;
            go2rtc_port = m_go2rtc_port;
            phone       = m_phone;
        }

        // go2rtc player + websocket at the root, gated by the rt cookie.
        for (const char* p : GO2RTC_PASSTHROUGH) {
            if (r.path == p) {
                if (token.empty() || cookie_value(r.cookies, "rt") != token) { respond(client, 404, "text/plain", "not found"); return; }
                if (go2rtc_port == 0) { respond(client, 503, "text/plain", "stream relay is not running"); return; }
                tunnel(client, go2rtc_port, force_close(r.head), r.pending);
                return;
            }
        }
        // Control routes for the slicer instances on this PC.
        if (r.path.compare(0, 5, "/hub/") == 0) {
            if (!peer.is_loopback()) { respond(client, 404, "text/plain", "not found"); return; }
            handle_hub(client, r);
            return;
        }
        const std::string prefix = "/r/" + token;
        if (token.empty() || r.path.compare(0, prefix.size(), prefix) != 0 ||
            (r.path.size() > prefix.size() && r.path[prefix.size()] != '/') ||
            (!phone && !peer.is_loopback())) {
            respond(client, 404, "text/plain", "not found");
            return;
        }
        const std::string rest = r.path.substr(prefix.size());
        if (rest.empty()) { respond(client, 302, "text/plain", "", "Location: " + prefix + "/\r\n"); return; }
        if (rest == "/" || rest == "/index.html") {
            respond(client, 200, "text/html; charset=utf-8", read_file(resources_dir() + "/web/orca/stream_center.html"),
                    "Set-Cookie: rt=" + token + "; Path=/; SameSite=Lax\r\n");
        } else if (rest == "/state") {
            respond_json(client, 200, state_for_phone());
        } else if (rest == "/bambu") {
            std::string ip, code;
            if (!lookup_host(query_param(r.query, "id"), ip, code) || code.empty()) { respond(client, 404, "text/plain", "unknown camera"); return; }
            const int relay = BambuCamRelay::get().port();
            if (relay == 0) { respond(client, 503, "text/plain", "camera relay is not running"); return; }
            const std::string new_head = "GET /bambu?ip=" + percent_encode(ip) + "&code=" + percent_encode(code) + " HTTP/1.1\r\n" +
                                         r.head.substr(r.head.find("\r\n") + 2);
            tunnel(client, relay, force_close(new_head), "");
        } else if (rest == "/ff") {
            std::string ip, code;
            if (!lookup_host(query_param(r.query, "id"), ip, code) || ip.find_first_of("\"'\\<>") != std::string::npos) {
                respond(client, 404, "text/plain", "unknown printer");
                return;
            }
            std::string url;
            Http::post("http://" + ip + ":8898/detail")
                .timeout_connect(4).timeout_max(8)
                .header("Content-Type", "application/json")
                .set_post_body(std::string("{\"serialNumber\":\"\",\"checkCode\":\"\"}"))
                .on_complete([&url](std::string body, unsigned) { url = ff_camera_url_from_detail(body); })
                .perform_sync();
            json j;
            j["url"] = url;
            respond_json(client, 200, j.dump());
        } else {
            handle_phone(client, r, rest);
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(debug) << "RemoteHub: session ended: " << e.what();
    }
}

int HubServer::run()
{
    ensure_dirs();
    // Settings from the last run, unless the caller decided them.
    try {
        json j = json::parse(read_file(hub_json_path()));
        if (!valid_token(m_token)) m_token = j.value("token", "");
        m_phone = m_phone || j.value("phone", false);
    } catch (...) {}
    if (!valid_token(m_token)) m_token = random_token();
    m_state = read_file(streams_json_path());

    start_go2rtc();
    BambuCamRelay::get().port();
    if (!bind(m_phone)) return 1;
    write_hub_json();
    register_streams();

    auto idle_since = std::chrono::steady_clock::now();
    while (!m_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        flush_logs(); // the file sink buffers; keep hub.log readable while we run
        bool phone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            phone = m_phone;
        }
        const bool busy = phone || !instances(false).empty();
        const auto now  = std::chrono::steady_clock::now();
        if (busy) idle_since = now;
        else if (now - idle_since > std::chrono::seconds(IDLE_EXIT_SECONDS)) {
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: idle (phone access off, no slicer running), exiting";
            break;
        }
    }
    {
        boost::system::error_code ig;
        fs::remove(hub_json_path(), ig);
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_acceptor) m_acceptor->close(ig);
    }
#ifndef _WIN32
    if (m_go2rtc_pid > 0) ::kill((pid_t) m_go2rtc_pid, SIGTERM);
#endif
    // On Windows the kill-on-close job object takes go2rtc down with us.
    return 0;
}

int run_server(const std::string& token_hint, bool phone_on)
{
    ensure_dirs();
    try {
        set_log_path_and_level("hub.log", 3); // <datadir>/log/hub.log.<n>
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: starting, version " << SLIC3R_VERSION << ", data dir " << data_dir();
#ifndef _WIN32
        ::signal(SIGPIPE, SIG_IGN);
#endif
        // Only one hub per data dir.
        Info existing = query();
        if (existing.alive) {
            BOOST_LOG_TRIVIAL(info) << "RemoteHub: another hub (pid " << existing.pid << ") is already running, exiting";
            return 0;
        }
        HubServer server(valid_token(token_hint) ? token_hint : std::string(), phone_on);
        return server.run();
    } catch (const std::exception& e) {
        // No console to see it on: leave a note next to the state files.
        boost::nowide::ofstream f((fs::path(hub_dir()) / "hub_error.txt").string(), std::ios::trunc);
        f << "RemoteHub died: " << e.what() << "\n";
        BOOST_LOG_TRIVIAL(error) << "RemoteHub died: " << e.what();
        return 1;
    }
}

// --------------------------------------------------------------- client ----

static std::mutex  s_state_mutex;
static std::string s_last_state;

std::string Info::url() const
{
    return (alive && phone && !ips.empty()) ? "http://" + ips.front() + ":" + std::to_string(port) + "/r/" + token + "/" : "";
}

std::string Info::json() const
{
    nlohmann::json j;
    j["on"]    = alive && phone;
    j["port"]  = port;
    j["token"] = token;
    j["ips"]   = ips;
    j["url"]   = url();
    return j.dump();
}

static Info parse_info(const std::string& body)
{
    Info i;
    try {
        json j        = json::parse(body);
        i.alive       = j.value("alive", false);
        i.pid         = j.value("pid", 0L);
        i.port        = j.value("port", 0);
        i.phone       = j.value("phone", false);
        i.token       = j.value("token", "");
        i.go2rtc_port = j.value("go2rtc_port", 0);
        i.relay_port  = j.value("relay_port", 0);
        i.version     = j.value("version", "");
        for (const auto& ip : j.value("ips", json::array())) i.ips.push_back(ip.get<std::string>());
    } catch (...) {}
    return i;
}

static int hub_port_from_file()
{
    try {
        json j = json::parse(read_file(hub_json_path()));
        if (!pid_alive(j.value("pid", 0L))) return 0;
        return j.value("port", 0);
    } catch (...) {}
    return 0;
}

static Info hub_call(const std::string& method, const std::string& path, const std::string& body, long timeout)
{
    const int port = hub_port_from_file();
    if (port == 0) return Info();
    Info        out;
    std::string url = "http://127.0.0.1:" + std::to_string(port) + path;
    Http        http = method == "POST" ? Http::post(url) : Http::get(url);
    if (method == "POST") http.set_post_body(body).header("Content-Type", body.empty() ? "text/plain" : "application/json");
    http.timeout_connect(1).timeout_max(timeout)
        .on_complete([&out](std::string b, unsigned status) { if (status == 200) out = parse_info(b); })
        .perform_sync();
    return out;
}

Info query() { return hub_call("GET", "/hub/info", "", 3); }

Info set_phone(bool on, const std::string& token)
{
    return hub_call("POST", std::string("/hub/phone?on=") + (on ? "1" : "0") + (valid_token(token) ? "&token=" + token : ""), "", 5);
}

void quit() { hub_call("POST", "/hub/quit", "", 3); }

bool post_state(const std::string& state_json)
{
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        s_last_state = state_json;
    }
    const int port = hub_port_from_file();
    if (port == 0) return false;
    bool ok = false;
    Http::post("http://127.0.0.1:" + std::to_string(port) + "/hub/state")
        .timeout_connect(1).timeout_max(5)
        .header("Content-Type", "application/json")
        .set_post_body(state_json)
        .on_complete([&ok](std::string, unsigned status) { ok = status == 200; })
        .perform_sync();
    return ok;
}

Info ensure_running(const std::string& token_hint, bool phone_on)
{
    Info i = query();
    if (i.alive && i.version != SLIC3R_VERSION) {
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: hub version " << i.version << " != " << SLIC3R_VERSION << ", restarting it";
        const long old_pid = i.pid;
        quit();
        for (int n = 0; n < 30 && pid_alive(old_pid); ++n) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        i = Info();
    }
    if (!i.alive) {
        std::vector<std::string> args = { current_exe(), "--hub", "--datadir", data_dir() };
        if (valid_token(token_hint)) { args.push_back("--hub-token"); args.push_back(token_hint); }
        if (phone_on) args.push_back("--hub-phone");
        const long pid = spawn_process(args, {}, true, nullptr);
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: spawned hub pid " << pid;
        if (pid > 0)
            for (int n = 0; n < 60 && !i.alive; ++n) {
                std::this_thread::sleep_for(std::chrono::milliseconds(200));
                i = query();
            }
        if (!i.alive) {
            BOOST_LOG_TRIVIAL(error) << "RemoteHub: the hub did not come up";
            return i;
        }
    }
    if (phone_on && !i.phone) i = set_phone(true, token_hint);
    std::string state;
    {
        std::lock_guard<std::mutex> lock(s_state_mutex);
        state = s_last_state;
    }
    if (!state.empty()) post_state(state);
    return i;
}

} // namespace RemoteHub
} // namespace GUI
} // namespace Slic3r
