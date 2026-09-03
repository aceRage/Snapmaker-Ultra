// The hub process (see RemoteHub.hpp). The server itself is wx-free (Boost.Asio, libcurl via
// Http, nlohmann::json, the OS); only the tray icon at the bottom of this file uses wx, with its
// own minimal wxApp (never GUI_App) created by run_server().
#include "RemoteHub.hpp"

#include "BambuCamRelay.hpp"
#include "libslic3r/Utils.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/asio.hpp>
#include <boost/beast/core/detail/base64.hpp>
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

#include <wx/app.h>
#include <wx/icon.h>
#include <wx/image.h>
#include <wx/init.h>
#include <wx/menu.h>
#include <wx/taskbar.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteHub {

namespace asio = boost::asio;
namespace fs   = boost::filesystem;
using tcp      = asio::ip::tcp;
using json     = nlohmann::json;

static const int         HUB_PORT           = 13640;
// The stream player is served by the hub itself (resources/web/orca/player.html plus verbatim
// copies of go2rtc's two scripts); only its WebSocket goes on to go2rtc, with the hub's
// credentials attached. go2rtc's own API is never reachable from a browser.
static const char* const PLAYER_PAGE = "/stream.html";
static const char* const PLAYER_JS[] = { "/video-stream.js", "/video-rtc.js" };
static const char* const GO2RTC_WS   = "/api/ws";
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
    case 403: return "403 Forbidden";            case 415: return "415 Unsupported Media Type";
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
    std::string host, secret, sec_fetch_site, content_type; // Host, X-Hub-Secret, Sec-Fetch-Site, Content-Type (lower-cased where compared)
    std::string ts_login, fwd_proto; // Tailscale-User-Login, X-Forwarded-Proto: set by Tailscale Serve, trusted from loopback only
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
            else if (key == "host") r.host = lower(val);
            else if (key == "x-hub-secret") r.secret = val;
            else if (key == "sec-fetch-site") r.sec_fetch_site = lower(val);
            else if (key == "content-type") r.content_type = lower(val);
            else if (key == "tailscale-user-login") r.ts_login = val;
            else if (key == "x-forwarded-proto") r.fwd_proto = lower(val);
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
// Run a command to completion and capture what it prints (the tailscale CLI). Windows only for now.
static bool run_capture(const std::vector<std::string>& args, std::string& out, int& exit_code, int timeout_ms)
{
    out.clear();
    exit_code = -1;
#ifdef _WIN32
    std::wstring cmd;
    for (const std::string& a : args) {
        if (!cmd.empty()) cmd += L' ';
        cmd += quote_arg(widen(a));
    }
    SECURITY_ATTRIBUTES sa = { sizeof(sa), nullptr, TRUE };
    HANDLE rd = nullptr, wr = nullptr;
    if (!::CreatePipe(&rd, &wr, &sa, 0)) return false;
    ::SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si = {};
    si.cb          = sizeof(si);
    si.dwFlags     = STARTF_USESTDHANDLES;
    si.hStdOutput  = wr;
    si.hStdError   = wr;
    PROCESS_INFORMATION  pi = {};
    std::vector<wchar_t> buf(cmd.begin(), cmd.end());
    buf.push_back(L'\0');
    const BOOL ok = ::CreateProcessW(nullptr, buf.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi);
    ::CloseHandle(wr);
    if (!ok) { ::CloseHandle(rd); return false; }
    std::thread reader([&]() {
        char  b[4096];
        DWORD n = 0;
        while (::ReadFile(rd, b, sizeof(b), &n, nullptr) && n > 0) out.append(b, n);
    });
    const DWORD w = ::WaitForSingleObject(pi.hProcess, (DWORD) timeout_ms);
    if (w != WAIT_OBJECT_0) ::TerminateProcess(pi.hProcess, 1);
    reader.join();
    DWORD code = 1;
    ::GetExitCodeProcess(pi.hProcess, &code);
    exit_code = (int) code;
    ::CloseHandle(pi.hThread);
    ::CloseHandle(pi.hProcess);
    ::CloseHandle(rd);
    return w == WAIT_OBJECT_0;
#else
    (void) args; (void) timeout_ms;
    return false;
#endif
}

// ---- Tailscale (remote access): the user's own Tailscale install publishes the hub inside their
// tailnet with `tailscale serve`; the phone runs the Tailscale app signed in to the same account.
static std::string tailscale_exe()
{
#ifdef _WIN32
    const char*       pf = std::getenv("ProgramFiles");
    const std::string p  = std::string(pf ? pf : "C:\\Program Files") + "\\Tailscale\\tailscale.exe";
    if (fs::exists(p)) return p;
#endif
    return "tailscale"; // PATH
}

struct TailscaleState
{
    bool        installed { false };
    std::string backend;       // Running, NeedsLogin, Stopped, ...
    std::string dns_name;      // <machine>.<tailnet>.ts.net
    std::string login;         // the node owner's login
    bool        https { false }; // the tailnet issues certificates (admin console > DNS > HTTPS)
    bool        serving { false };
    int         serving_port { 0 }; // the loopback port the Serve proxy points at
    std::string error;
    long long   checked_at { 0 };
};

static TailscaleState tailscale_query()
{
    TailscaleState t;
    std::string    out;
    int            code = 0;
    t.checked_at = (long long) std::time(nullptr);
    if (!run_capture({ tailscale_exe(), "status", "--json" }, out, code, 15000)) {
        t.error = "Tailscale is not installed on this PC";
        return t;
    }
    t.installed = true;
    try {
        json j    = json::parse(out);
        t.backend = j.value("BackendState", "");
        json self = j.value("Self", json::object());
        t.dns_name = self.value("DNSName", "");
        while (!t.dns_name.empty() && t.dns_name.back() == '.') t.dns_name.pop_back();
        if (self.contains("UserID")) {
            const std::string uid   = std::to_string(self["UserID"].get<long long>());
            json              users = j.value("User", json::object());
            if (users.contains(uid)) t.login = lower(users[uid].value("LoginName", ""));
        }
        t.https = !j.value("CertDomains", json::array()).empty();
    } catch (...) {
        t.error = code == 0 ? "unexpected output from tailscale status" : out.substr(0, 200);
        return t;
    }
    if (t.backend != "Running") {
        t.error = t.backend == "NeedsLogin" ? "Tailscale is installed but not signed in on this PC" : "Tailscale is not running (" + t.backend + ")";
        return t;
    }
    if (run_capture({ tailscale_exe(), "serve", "status", "--json" }, out, code, 15000) && code == 0) {
        try {
            const json s   = json::parse(out);
            const json web_all = s.value("Web", json::object());
            for (const auto& web : web_all) {
                const json handlers = web.value("Handlers", json::object());
                for (const auto& [path, h] : handlers.items()) {
                    const std::string proxy = h.value("Proxy", ""), want = "http://127.0.0.1:";
                    if (path == "/" && proxy.compare(0, want.size(), want) == 0) {
                        t.serving      = true;
                        t.serving_port = std::atoi(proxy.c_str() + want.size());
                    }
                }
            }
        } catch (...) {}
    }
    return t;
}

// Per-run secrets (std::random_device is the OS CSPRNG on every platform we build).
static std::string random_hex(int bytes)
{
    static const char* hx = "0123456789abcdef";
    std::random_device rd;
    std::string        s;
    for (int i = 0; i < bytes; ++i) { const unsigned v = rd() & 0xffu; s += hx[v >> 4]; s += hx[v & 15]; }
    return s;
}

static std::string basic_auth(const std::string& user, const std::string& pass)
{
    const std::string raw = user + ":" + pass;
    std::string       out(boost::beast::detail::base64::encoded_size(raw.size()), '\0');
    out.resize(boost::beast::detail::base64::encode(out.data(), raw.data(), raw.size()));
    return "Basic " + out;
}

// A free loopback port for go2rtc (bound and released; the race with another process is
// theoretical on a PC and go2rtc simply fails to start, which the log shows).
static int free_loopback_port()
{
    try {
        asio::io_context ioc;
        tcp::acceptor    a(ioc);
        a.open(tcp::v4());
        a.bind(tcp::endpoint(asio::ip::make_address_v4("127.0.0.1"), 0));
        return (int) a.local_endpoint().port();
    } catch (...) { return 0; }
}

// Native separators: this path is shown to the user to paste into the firewall dialog.
static std::string go2rtc_exe_path()
{
    return fs::path(resources_dir() + "/tools/go2rtc/go2rtc.exe").make_preferred().string();
}

// ---- WebRTC (Phase 2): go2rtc's media port ------------------------------------------------
// Everything else the hub runs is loopback-only, but WebRTC media goes straight from go2rtc to
// the phone, so this one port has to be reachable on the LAN and on the tailnet. A predictable
// port keeps a Windows Firewall rule the user allows once valid across restarts; 8555 is
// go2rtc's own documented default, and if something else on the PC already holds it (another
// go2rtc, say) we take the next free one and say which on the hub page.
static const int WEBRTC_PORT_FIRST = 8555;
static const int WEBRTC_PORT_LAST  = 8574;

// Free for both protocols on every interface. A dual-stack listener elsewhere on the PC makes
// the v4 bind fail too, which is what we want: go2rtc would not get the port either.
static bool port_free_any(int port)
{
    try {
        asio::io_context ioc;
        tcp::acceptor    a(ioc);
        a.open(tcp::v4());
        a.bind(tcp::endpoint(tcp::v4(), (unsigned short) port));
        asio::ip::udp::socket u(ioc);
        u.open(asio::ip::udp::v4());
        u.bind(asio::ip::udp::endpoint(asio::ip::udp::v4(), (unsigned short) port));
        return true;
    } catch (...) { return false; }
}

static int free_webrtc_port()
{
    for (int p = WEBRTC_PORT_FIRST; p <= WEBRTC_PORT_LAST; ++p)
        if (port_free_any(p)) return p;
    return 0;
}

// What Windows Firewall thinks of go2rtc.exe. WebRTC media arrives inbound on the port above, so
// without an allow rule for the profile the phone's network is on, the peer connection never
// completes and the page silently stays on MSE through the hub. We only *look*: adding a rule
// needs administrator rights and doing it silently would be wrong, so the answer is shown to the
// user as a note with the two things they can allow.
struct FirewallState
{
    std::string state { "unknown" }; // allowed | partial | missing | unknown
    std::string note;                // one sentence, shown on the hub page (and on the phone)
    std::string networks;            // profiles the PC's live networks are in ("Public, Private")
    long long   checked_at { 0 };
};

// Joined for a sentence, each value once (two rules for the same profile, two networks in the
// same profile - the user only wants to read "Private" once).
static std::string join_words(const std::vector<std::string>& v, const char* sep)
{
    std::string              out;
    std::vector<std::string> seen;
    for (const std::string& s : v) {
        if (std::find(seen.begin(), seen.end(), s) != seen.end()) continue;
        seen.push_back(s);
        if (!out.empty()) out += sep;
        out += s;
    }
    return out;
}

// Windows Firewall through PowerShell rather than netsh: Get-NetFirewallRule answers with
// property values (Allow/Inbound/Private) that are the same in every Windows display language,
// while netsh's verbose output is localised and would have to be parsed by label.
static FirewallState firewall_query(const std::string& exe, int port)
{
    FirewallState fw;
    fw.checked_at = (long long) std::time(nullptr);
#ifdef _WIN32
    std::string quoted = exe; // '' escapes a quote inside a PowerShell single-quoted string
    for (size_t i = 0; i < quoted.size(); ++i)
        if (quoted[i] == '\'') quoted.insert(i++, 1, '\'');
    const std::string script =
        "$p='" + quoted + "';$f=[IO.Path]::GetFullPath($p);"
        "$r=@(Get-NetFirewallApplicationFilter -ErrorAction SilentlyContinue |"
        " Where-Object { try { [IO.Path]::GetFullPath($_.Program) -ieq $f } catch { $false } } |"
        " Get-NetFirewallRule -ErrorAction SilentlyContinue | Where-Object { $_.Enabled -eq 'True' -and"
        " $_.Direction -eq 'Inbound' -and $_.Action -eq 'Allow' });"
        "foreach ($x in $r) { 'RULE=' + $x.Profile };"
        "foreach ($n in @(Get-NetConnectionProfile -ErrorAction SilentlyContinue)) { 'NET=' + $n.NetworkCategory };"
        "'DONE'";
    std::string out;
    int         code = 0;
    if (!run_capture({ "powershell", "-NoProfile", "-NonInteractive", "-ExecutionPolicy", "Bypass", "-Command", script }, out, code, 30000) ||
        out.find("DONE") == std::string::npos) {
        fw.note = "Windows Firewall could not be checked. If remote video does not start, allow "
                  "go2rtc.exe (inbound, UDP and TCP port " + std::to_string(port) + ").";
        return fw;
    }
    std::vector<std::string> rules, nets;
    std::istringstream       is(out);
    std::string              line;
    while (std::getline(is, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.compare(0, 5, "RULE=") == 0) rules.push_back(line.substr(5));
        else if (line.compare(0, 4, "NET=") == 0) nets.push_back(line.substr(4) == "DomainAuthenticated" ? "Domain" : line.substr(4));
    }
    // Only the profiles the PC's live networks are in matter: a rule that covers Public does
    // nothing for a phone on a network Windows filed as Private.
    std::vector<std::string> uncovered;
    for (const std::string& n : nets) {
        bool covered = false;
        for (const std::string& r : rules)
            if (r == "Any" || lower(r).find(lower(n)) != std::string::npos) { covered = true; break; }
        if (!covered && std::find(uncovered.begin(), uncovered.end(), n) == uncovered.end()) uncovered.push_back(n);
    }
    fw.networks = join_words(nets, ", ");
    const std::string allow = "In Windows Defender Firewall allow go2rtc.exe (inbound, UDP and TCP), or open port " +
                              std::to_string(port) + " for UDP and TCP.";
    if (rules.empty()) {
        fw.state = "missing";
        fw.note  = "Windows Firewall has no inbound rule for go2rtc.exe, so direct video will not reach the phone; "
                   "it falls back to relayed video through this hub. " + allow;
    } else if (!uncovered.empty()) {
        fw.state = "partial";
        fw.note  = "Windows Firewall allows go2rtc.exe on " + join_words(rules, " / ") + " networks, but this PC is on a " +
                   join_words(uncovered, " and ") + " network. " + allow;
    } else {
        fw.state = "allowed";
        fw.note  = "";
    }
#else
    (void) exe; (void) port;
    fw.note = "Direct video needs inbound UDP and TCP port " + std::to_string(port) + " open for go2rtc.";
#endif
    return fw;
}

// The Host header must name this PC's loopback (a DNS-rebound name is not accepted).
static bool loopback_host(const std::string& host)
{
    if (host.empty()) return false;
    std::string h = host;
    if (h[0] == '[') { const size_t e = h.find(']'); if (e != std::string::npos) h = h.substr(0, e + 1); }
    else { const size_t c = h.find(':'); if (c != std::string::npos) h = h.substr(0, c); }
    return h == "127.0.0.1" || h == "localhost" || h == "[::1]";
}

// Rewrite a request head for go2rtc: drop the client's Cookie and Authorization, drop the hub's
// own `lt` query parameter, attach the hub's go2rtc credentials.
static std::string go2rtc_head(const std::string& head, const std::string& auth)
{
    std::string out;
    size_t      pos   = 0;
    bool        first = true;
    while (pos < head.size()) {
        size_t nl = head.find("\r\n", pos);
        if (nl == std::string::npos) nl = head.size();
        const std::string line = head.substr(pos, nl - pos);
        pos = nl + 2;
        if (first) {
            first = false;
            std::istringstream is(line);
            std::string        m, t, v;
            is >> m >> t >> v;
            const size_t q = t.find('?');
            if (q != std::string::npos) {
                const std::string  path = t.substr(0, q);
                std::istringstream qs(t.substr(q + 1));
                std::string        kv, kept;
                while (std::getline(qs, kv, '&')) {
                    if (kv.compare(0, 3, "lt=") == 0) continue;
                    if (!kept.empty()) kept += '&';
                    kept += kv;
                }
                t = kept.empty() ? path : path + "?" + kept;
            }
            out += m + " " + t + " " + v + "\r\n";
            continue;
        }
        if (line.empty()) break;
        const std::string key = lower(line.substr(0, line.find(':')));
        if (key == "cookie" || key == "authorization") continue;
        out += line + "\r\n";
    }
    out += "Authorization: " + auth + "\r\n\r\n";
    return out;
}

// Snapmaker U1 (camera-streamer behind nginx): the Stream tab embeds http://<ip>/webcam/webrtc,
// which only a browser on the LAN can reach. The same server offers raw H.264 at
// /webcam/stream.h264, which go2rtc can carry to a phone anywhere - via the relay below.
static std::string u1_h264_url(const json& host)
{
    const std::string rurl = host.value("rurl", "");
    const std::string tail = "/webcam/webrtc";
    if (rurl.compare(0, 7, "http://") != 0) return "";
    std::string u = rurl;
    while (!u.empty() && u.back() == '/') u.pop_back();
    if (u.size() <= tail.size() || u.compare(u.size() - tail.size(), tail.size(), tail) != 0) return "";
    const std::string hostport = u.substr(7, u.size() - 7 - tail.size());
    if (hostport.empty() || hostport.find('/') != std::string::npos) return "";
    return "http://" + hostport + "/webcam/stream.h264";
}

static std::string u1_stream_name(const std::string& id)
{
    std::string n = "u1_";
    for (unsigned char c : id) n += std::isalnum(c) ? (char) c : '_';
    return n;
}

static std::string encode_component(const std::string& s)
{
    static const char* hx = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') out += (char) c;
        else { out += '%'; out += hx[c >> 4]; out += hx[c & 15]; }
    }
    return out;
}

// GET a raw H.264 stream from a camera server and re-serve it from the first SPS onward (a
// stream that starts mid-GOP begins with a slice byte that also reads as an HEVC header, and
// go2rtc then decodes nothing). Chunked transfer is undone; the client gets a plain body.
static void relay_h264(tcp::socket& client, const std::string& url)
{
    std::string rest  = url.substr(7);
    const size_t sl   = rest.find('/');
    std::string hostport = rest.substr(0, sl), path = sl == std::string::npos ? "/" : rest.substr(sl);
    std::string host = hostport, port = "80";
    const size_t colon = hostport.rfind(':');
    if (colon != std::string::npos) { host = hostport.substr(0, colon); port = hostport.substr(colon + 1); }
    asio::io_context          ioc;
    tcp::resolver             resolver(ioc);
    boost::system::error_code ec;
    auto endpoints = resolver.resolve(host, port, ec);
    if (ec) { respond(client, 502, "text/plain", "camera address did not resolve"); return; }
    tcp::socket up(ioc);
    asio::connect(up, endpoints, ec);
    if (ec) { respond(client, 502, "text/plain", "camera did not answer"); return; }
    up.set_option(tcp::no_delay(true));
    write_all(up, "GET " + path + " HTTP/1.1\r\nHost: " + hostport + "\r\nConnection: close\r\n\r\n");
    std::string buf;
    char        tmp[16384];
    while (buf.find("\r\n\r\n") == std::string::npos) {
        const size_t n = up.read_some(asio::buffer(tmp), ec);
        if (ec || buf.size() > 65536) { respond(client, 502, "text/plain", "bad answer from the camera"); return; }
        buf.append(tmp, n);
    }
    const size_t      he   = buf.find("\r\n\r\n") + 4;
    const std::string head = lower(buf.substr(0, he));
    if (head.compare(0, 12, "http/1.1 200") != 0 && head.compare(0, 12, "http/1.0 200") != 0) {
        respond(client, 502, "text/plain", "camera answered " + buf.substr(0, buf.find("\r\n")));
        return;
    }
    const bool chunked = head.find("transfer-encoding: chunked") != std::string::npos;
    write_all(client, "HTTP/1.1 200 OK\r\nContent-Type: video/h264\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n");

    bool        started = false;
    std::string pend; // unforwarded bytes while waiting for the first SPS
    auto emit = [&](const char* p, size_t n) -> bool {
        boost::system::error_code e;
        if (started) { asio::write(client, asio::buffer(p, n), e); return !e; }
        pend.append(p, n);
        size_t at = std::string::npos;
        for (size_t k = 0; k + 3 < pend.size(); ++k)
            if (pend[k] == 0 && pend[k + 1] == 0 && pend[k + 2] == 1 && ((unsigned char) pend[k + 3] & 0x1f) == 7) {
                at = (k > 0 && pend[k - 1] == 0) ? k - 1 : k;
                break;
            }
        if (at == std::string::npos) { if (pend.size() > 4) pend.erase(0, pend.size() - 4); return true; }
        started = true;
        asio::write(client, asio::buffer(pend.data() + at, pend.size() - at), e);
        pend.clear();
        return !e;
    };
    std::string acc = buf.substr(he);
    auto more = [&]() -> bool {
        const size_t n = up.read_some(asio::buffer(tmp), ec);
        if (ec) return false;
        acc.append(tmp, n);
        return true;
    };
    if (!chunked) {
        for (;;) {
            if (!acc.empty() && !emit(acc.data(), acc.size())) break;
            acc.clear();
            if (!more()) break;
        }
    } else {
        size_t need = 0;
        bool   alive = true;
        while (alive) {
            if (need == 0) {
                const size_t nl = acc.find("\r\n");
                if (nl == std::string::npos) { if (!more()) break; continue; }
                std::string line = acc.substr(0, nl);
                acc.erase(0, nl + 2);
                const size_t semi = line.find(';');
                if (semi != std::string::npos) line.resize(semi);
                need = (size_t) std::strtoul(line.c_str(), nullptr, 16);
                if (need == 0) break; // last chunk
                continue;
            }
            if (acc.empty()) { if (!more()) break; continue; }
            const size_t take = std::min(need, acc.size());
            if (!emit(acc.data(), take)) { alive = false; break; }
            acc.erase(0, take);
            need -= take;
            if (need == 0) {
                while (acc.size() < 2 && alive) alive = more();
                if (alive) acc.erase(0, 2); // CRLF after the chunk data
            }
        }
    }
    boost::system::error_code ig;
    up.shutdown(tcp::socket::shutdown_both, ig);
    client.shutdown(tcp::socket::shutdown_both, ig);
}

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
    bool        hidden { false };
    bool        needs_attention { false };
    std::string attention_reason;
    bool        alive { false };
};

class HubServer
{
public:
    HubServer(std::string token, bool phone) : m_token(std::move(token)), m_phone(phone) {}

    bool start();                 // state, go2rtc, relay, listener, hub.json
    void loop(bool idle_exit);    // until request_quit(); with idle_exit also once nobody needs us
    void shutdown();
    void request_quit() { m_quit = true; }

    struct Snapshot
    {
        bool        phone { false };
        int         port { 0 };
        std::string token, url;
        int         instances { 0 };
        int         hidden { 0 };
        int         attention { 0 };
    };
    Snapshot snapshot();
    bool     set_phone(bool on, const std::string& token); // rebinds the listener, persists
    long     spawn_slicer(const std::string& file, bool hidden = false); // "" = an empty window
    // One request to an instance's loopback API (HTTP: never on the tray's GUI thread).
    std::pair<int, std::string> instance_post(long pid, const std::string& sub);
    bool     instance_window(long pid, bool show);
    bool     instance_quit(long pid, bool discard);
    std::vector<Instance> instances(bool probe);

private:
    json  info_json();
    json  instances_json();
    void  write_hub_json();
    bool  bind(bool lan);
    void  accept_loop(std::shared_ptr<tcp::acceptor> acceptor);
    void  serve(std::unique_ptr<tcp::socket> sock);
    void  handle_hub(tcp::socket& client, Request& r);
    void  handle_phone(tcp::socket& client, Request& r, const std::string& rest);
    void  start_go2rtc();
    void  register_streams();
    std::string go2rtc_base_locked() const; // http://user:pass@127.0.0.1:port ("" while go2rtc is down); m_mutex held
    std::pair<int, std::string> onvif_discover(); // GET /api/onvif on go2rtc: {status, body}
    std::string state_for_phone();
    bool  lookup_host(const std::string& id, std::string& ip, std::string& code);
    std::string relay_h264_url(const std::string& id); // the U1 raw stream behind /relay/h264?id=, or ""
    FirewallState  firewall_state(bool refresh);       // cached; the query runs on a detached thread
    TailscaleState remote_state(bool refresh);         // cached ~15 s; runs the tailscale CLI off the lock
    bool  set_remote(bool on, std::string& error);     // tailscale serve on/off for this hub
    void  remote_logins(const std::string& add, const std::string& remove);
    bool  login_allowed(const std::string& login);
    json  remote_json_locked() const;                  // m_mutex held
    static Instance probe_instance(Instance inst);

    std::mutex                     m_mutex;
    std::string                    m_token;
    bool                           m_phone { false };
    bool                           m_lan { false };
    int                            m_port { 0 };
    asio::io_context               m_ioc;
    std::shared_ptr<tcp::acceptor> m_acceptor;
    std::string                    m_state; // full Stream-tab state JSON (with credentials)
    std::string                    m_secret;      // per run; in hub.json and the hub page, required as X-Hub-Secret on /hub/*
    std::string                    m_go2rtc_user, m_go2rtc_pass, m_go2rtc_auth; // go2rtc credentials, this process only
    bool                           m_remote_on { false };        // publish through Tailscale Serve (persisted)
    std::vector<std::string>       m_allowed_logins;             // tailnet logins that may connect (lower-case, persisted)
    TailscaleState                 m_ts;                         // last tailscale_query()
    std::string                    m_last_login;                 // most recent remote visitor
    long long                      m_last_login_at { 0 };
    int                            m_go2rtc_port { 0 };
    int                            m_webrtc_port { 0 };          // go2rtc's WebRTC media port (0 = WebRTC off)
    FirewallState                  m_fw;                         // last firewall_query()
    std::atomic<bool>              m_fw_busy { false };
    long                           m_go2rtc_pid { 0 };
    void*                          m_job { nullptr };
    std::atomic<bool>              m_quit { false };
};

json HubServer::info_json()
{
    const FirewallState fw = firewall_state(false); // takes m_mutex itself: before the lock below
    json j;
    std::lock_guard<std::mutex> lock(m_mutex);
    json v;
    v["webrtc_port"]  = m_webrtc_port;
    v["firewall"]     = m_webrtc_port ? fw.state : std::string("off");
    v["note"]         = m_webrtc_port ? fw.note : std::string("No free port for WebRTC video; the phone uses relayed video.");
    v["networks"]     = fw.networks;
    v["go2rtc_exe"]   = go2rtc_exe_path();
    j["video"]       = v;
    j["alive"]       = true;
    j["pid"]         = current_pid();
    j["port"]        = m_port;
    j["phone"]       = m_phone;
    j["token"]       = m_token;
    j["go2rtc_port"] = m_go2rtc_port;
    j["relay_port"]  = BambuCamRelay::get().port();
    j["version"]     = std::string(SLIC3R_VERSION);
    j["remote"]      = remote_json_locked();
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
        j["secret"]      = m_secret;
        j["go2rtc_port"] = m_go2rtc_port;
        j["webrtc_port"] = m_webrtc_port;
        j["version"]     = std::string(SLIC3R_VERSION);
        j["remote_on"]      = m_remote_on;
        j["allowed_logins"] = m_allowed_logins;
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
    const std::string exe = go2rtc_exe_path();
    if (!fs::exists(exe)) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: missing " << exe;
        return;
    }
    // A random loopback port and per-run credentials: nothing on this PC reaches go2rtc's API
    // except through the hub. local_auth makes go2rtc check them for loopback peers too, and
    // allow_paths leaves only the three routes the hub uses registered (go2rtc 1.9.14 then also
    // drops its built-in player pages, which the hub serves itself), so even a leaked credential
    // cannot reach /api/config, /api/restart or /api/exit. An orphan go2rtc cannot exist: it is
    // in a kill-on-close job object.
    const int port = free_loopback_port();
    if (port == 0) {
        BOOST_LOG_TRIVIAL(error) << "RemoteHub: no free loopback port for go2rtc";
        return;
    }
    m_go2rtc_user = random_hex(8);
    m_go2rtc_pass = random_hex(16);
    m_go2rtc_auth = basic_auth(m_go2rtc_user, m_go2rtc_pass);
    // WebRTC media goes straight from go2rtc to the phone (Phase 2). The signalling still rides
    // the hub's /api/ws tunnel - go2rtc 1.9.14 answers webrtc/offer on the WebSocket, so
    // allow_paths does not need go2rtc's /api/webrtc (WHEP) route and stays as it is. The public
    // STUN server only matters off the tailnet: it lets go2rtc learn its own public address so a
    // phone on mobile data can try a direct path. On the tailnet and on the LAN the host
    // candidates (100.x, 192.168.x/10.x) are what actually connect.
    const int webrtc_port = free_webrtc_port();
    if (webrtc_port == 0) BOOST_LOG_TRIVIAL(warning) << "RemoteHub: no free WebRTC port; video stays on MSE";
    const std::string cfg_path = (fs::path(hub_dir()) / "go2rtc.yaml").string();
    {
        boost::nowide::ofstream cfg(cfg_path);
        cfg << "api:\n  listen: \"127.0.0.1:" << port << "\"\n"
            << "  username: \"" << m_go2rtc_user << "\"\n  password: \"" << m_go2rtc_pass << "\"\n"
            << "  local_auth: true\n"
            << "  allow_paths: [\"/api/ws\", \"/api/streams\", \"/api/onvif\"]\n"
            << "rtsp:\n  listen: \"\"\n";
        if (webrtc_port > 0)
            cfg << "webrtc:\n  listen: \":" << webrtc_port << "\"\n"
                << "  ice_servers:\n    - urls: [\"stun:stun.cloudflare.com:3478\"]\n";
        else
            cfg << "webrtc:\n  listen: \"\"\n";
        cfg << "srtp:\n  listen: \"\"\n";
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
    m_go2rtc_port = port;
    m_webrtc_port = webrtc_port;
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: go2rtc pid " << m_go2rtc_pid << " on 127.0.0.1:" << port
                            << " (credential-only), WebRTC media on " << (webrtc_port ? std::to_string(webrtc_port) : std::string("off"));
    if (webrtc_port > 0) firewall_state(true); // one PowerShell run on a detached thread; result cached
#endif
}

// Cached; a refresh runs the (slow) PowerShell query on a detached thread and never blocks a
// request, so the hub page's 3 s poll always gets the last answer straight away.
FirewallState HubServer::firewall_state(bool refresh)
{
    int port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        port = m_webrtc_port;
        // Re-checked every few minutes so the hub page notices by itself once the user has
        // allowed go2rtc in the firewall (or removed the rule again).
        if (!refresh && m_fw.checked_at && (long long) std::time(nullptr) - m_fw.checked_at < 300) return m_fw;
    }
    if (port > 0 && !m_fw_busy.exchange(true)) {
        std::thread([this, port]() {
            FirewallState fw = firewall_query(go2rtc_exe_path(), port);
            {
                std::lock_guard<std::mutex> lock(m_mutex);
                m_fw = fw;
            }
            if (fw.state != "allowed")
                BOOST_LOG_TRIVIAL(info) << "RemoteHub: Windows Firewall for go2rtc.exe: " << fw.state << " (" << fw.note << ")";
            m_fw_busy = false;
        }).detach();
    }
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_fw;
}

void HubServer::register_streams()
{
    std::string state, base, secret;
    int         port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state  = m_state;
        base   = go2rtc_base_locked();
        secret = m_secret;
        port   = m_port;
    }
    if (base.empty() || state.empty()) return;
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            std::string name = h.value("rname", ""), src = h.value("rsrc", "");
            if (!u1_h264_url(h).empty() && port > 0) {
                // The U1 raw stream, fetched by go2rtc from this hub (SPS-first), never from the printer directly.
                name = u1_stream_name(h.value("id", ""));
                src  = "http://127.0.0.1:" + std::to_string(port) + "/relay/h264?id=" + encode_component(h.value("id", "")) + "&lt=" + secret;
            }
            if (name.empty() || src.empty()) continue;
            // Http::put() is a file-upload PUT (its read callback dereferences the missing
            // file and crashes); put2() is a plain PUT with no body, which is what go2rtc wants.
            const std::string url = base + "/api/streams?name=" + name + "&src=" + percent_encode(src);
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

std::string HubServer::go2rtc_base_locked() const
{
    if (m_go2rtc_port == 0) return "";
    return "http://" + m_go2rtc_user + ":" + m_go2rtc_pass + "@127.0.0.1:" + std::to_string(m_go2rtc_port);
}

// ONVIF WS-Discovery, run by go2rtc for the PC's Stream tab (through /hub/onvif). go2rtc answers
// 404 "no sources" when nothing is found; that is passed through as it is.
std::pair<int, std::string> HubServer::onvif_discover()
{
    std::string base;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        base = go2rtc_base_locked();
    }
    if (base.empty()) return { 503, json_error("stream relay is not running") };
    int         status = 0;
    std::string body;
    Http::get(base + "/api/onvif")
        .timeout_connect(2).timeout_max(15)
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? err : b; })
        .perform_sync();
    return { status == 0 ? 502 : status, body };
}

// The phone only sees ids, aliases, the source kind, go2rtc stream names and direct
// printer-page URLs. Addresses, access codes and camera credentials stay here.
std::string HubServer::state_for_phone()
{
    const FirewallState fw = firewall_state(false); // takes m_mutex itself
    std::string state;
    int         webrtc_port = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state       = m_state;
        webrtc_port = m_webrtc_port;
    }
    json out;
    out["hosts"]  = json::array();
    out["active"] = json::array();
    // Whether the phone should try WebRTC at all, and - when the PC's firewall would very likely
    // block it - one sentence the viewer can act on. The player falls back to MSE either way.
    out["webrtc"]     = webrtc_port > 0;
    out["video_note"] = (webrtc_port > 0 && fw.state != "allowed") ? fw.note : "";
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array())) {
            json p;
            p["id"]    = h.value("id", "");
            p["alias"] = h.value("alias", "");
            p["rkind"] = h.value("rkind", "");
            p["rname"] = h.value("rname", "");
            p["rurl"]  = h.value("rurl", "");
            if (!u1_h264_url(h).empty()) {
                p["rname"] = u1_stream_name(h.value("id", "")); // the go2rtc stream fed by /relay/h264
                p["relay"] = true;                               // rurl still works on the LAN
            }
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

std::string HubServer::relay_h264_url(const std::string& id)
{
    std::string state;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        state = m_state;
    }
    try {
        json j = json::parse(state);
        for (const auto& h : j.value("hosts", json::array()))
            if (h.value("id", "") == id) return u1_h264_url(h);
    } catch (...) {}
    return "";
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
                inst.hidden   = j.value("hidden", inst.hidden);
                inst.needs_attention  = j.value("needs_attention", false);
                inst.attention_reason = j.value("attention_reason", "");
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
            inst.hidden  = j.value("hidden", false);
            inst.needs_attention  = j.value("needs_attention", false);
            inst.attention_reason = j.value("attention_reason", "");
            inst.title   = j.value("title", "");
            inst.path    = j.value("path", "");
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

json HubServer::instances_json()
{
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
        ji["hidden"]  = inst.hidden;
        ji["needs_attention"]  = inst.needs_attention;
        ji["attention_reason"] = inst.attention_reason;
        j["instances"].push_back(ji);
    }
    return j;
}

HubServer::Snapshot HubServer::snapshot()
{
    Snapshot s;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        s.phone = m_phone;
        s.port  = m_port;
        s.token = m_token;
    }
    if (s.phone) {
        const std::vector<std::string> ips = lan_ips();
        if (!ips.empty()) s.url = "http://" + ips.front() + ":" + std::to_string(s.port) + "/r/" + s.token + "/";
    }
    for (const Instance& inst : instances(false)) {
        ++s.instances;
        if (inst.hidden) ++s.hidden;
        if (inst.needs_attention) ++s.attention;
    }
    return s;
}

bool HubServer::set_phone(bool on, const std::string& token)
{
    bool changed;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        changed = on != m_phone;
        m_phone = on;
        // A new link every time it is turned on (as the PC page promises), unless the caller
        // brings the one it remembered.
        if (changed && on) m_token = valid_token(token) ? token : random_token();
    }
    if (!changed) return true;
    const bool ok = bind(on);
    write_hub_json();
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: phone access " << (on ? "on" : "off");
    return ok;
}

TailscaleState HubServer::remote_state(bool refresh)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!refresh && m_ts.checked_at && (long long) std::time(nullptr) - m_ts.checked_at < 15) return m_ts;
    }
    TailscaleState t = tailscale_query(); // slow-ish (two CLI runs): never under the lock
    std::lock_guard<std::mutex> lock(m_mutex);
    m_ts = t;
    return m_ts;
}

json HubServer::remote_json_locked() const
{
    json r;
    r["installed"]      = m_ts.installed;
    r["state"]          = m_ts.backend;
    r["dns_name"]       = m_ts.dns_name;
    r["login"]          = m_ts.login;
    r["https"]          = m_ts.https;
    r["serving"]        = m_ts.serving;
    r["on"]             = m_remote_on;
    r["url"]            = (m_remote_on && m_ts.serving && !m_ts.dns_name.empty()) ? "https://" + m_ts.dns_name + "/r/" + m_token + "/" : "";
    r["allowed_logins"] = m_allowed_logins;
    r["last_login"]     = m_last_login;
    r["last_login_at"]  = m_last_login_at;
    std::string err = m_ts.error;
    if (err.empty() && m_remote_on && m_ts.installed && !m_ts.serving) err = "Tailscale is no longer serving this hub; turn remote access off and on again";
    r["error"] = err;
    return r;
}

bool HubServer::set_remote(bool on, std::string& error)
{
    int port;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        port = m_port;
    }
    std::string out;
    int         code = 0;
    if (on) {
        TailscaleState t = remote_state(true);
        if (!t.installed || t.backend != "Running") { error = t.error.empty() ? "Tailscale is not ready" : t.error; return false; }
        if (!t.https) { error = "HTTPS certificates are not enabled for your tailnet: Tailscale admin console > DNS > HTTPS Certificates > Enable, then try again"; return false; }
        // The first run also fetches the certificate, which can take half a minute.
        if (!run_capture({ tailscale_exe(), "serve", "--bg", "--https=443", "http://127.0.0.1:" + std::to_string(port) }, out, code, 90000) || code != 0) {
            error = out.empty() ? "tailscale serve failed" : out.substr(0, 300);
            return false;
        }
        t = remote_state(true);
        if (!t.serving) { error = "tailscale serve did not take: " + out.substr(0, 200); return false; }
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remote_on = true;
        if (m_allowed_logins.empty() && !t.login.empty()) m_allowed_logins.push_back(t.login);
    } else {
        run_capture({ tailscale_exe(), "serve", "--https=443", "off" }, out, code, 30000);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_remote_on = false;
    }
    write_hub_json();
    if (!on) remote_state(true);
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: remote access (Tailscale) " << (on ? "on" : "off");
    return true;
}

void HubServer::remote_logins(const std::string& add, const std::string& remove)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    const std::string a = lower(add), r = lower(remove);
    if (!a.empty() && a.size() < 200 && a.find_first_of(" \t\r\n\"<>") == std::string::npos &&
        std::find(m_allowed_logins.begin(), m_allowed_logins.end(), a) == m_allowed_logins.end())
        m_allowed_logins.push_back(a);
    if (!r.empty()) m_allowed_logins.erase(std::remove(m_allowed_logins.begin(), m_allowed_logins.end(), r), m_allowed_logins.end());
}

bool HubServer::login_allowed(const std::string& login)
{
    const std::string l = lower(login);
    std::lock_guard<std::mutex> lock(m_mutex);
    const bool ok = std::find(m_allowed_logins.begin(), m_allowed_logins.end(), l) != m_allowed_logins.end();
    if (ok && (m_last_login != l || (long long) std::time(nullptr) - m_last_login_at > 600)) {
        m_last_login    = l;
        m_last_login_at = (long long) std::time(nullptr);
        BOOST_LOG_TRIVIAL(info) << "RemoteHub: remote visit by " << l;
    }
    return ok;
}

long HubServer::spawn_slicer(const std::string& file, bool hidden)
{
    std::vector<std::string> args = { current_exe() };
    if (!file.empty()) args.push_back(file);
    std::vector<std::pair<std::string, std::string>> env { { "SNORCA_NEW_INSTANCE", "1" } };
    env.emplace_back("SNORCA_HIDDEN", hidden ? "1" : "0"); // explicit either way
    const long pid = spawn_process(args, env, false, nullptr);
    BOOST_LOG_TRIVIAL(info) << "RemoteHub: new " << (hidden ? "hidden" : "visible") << " instance pid " << pid
                            << (file.empty() ? std::string() : " for " + file);
    return pid;
}

std::pair<int, std::string> HubServer::instance_post(long pid, const std::string& sub)
{
    Instance target;
    for (const Instance& inst : instances(false))
        if (inst.pid == pid) target = inst;
    if (!target.alive) return { 404, json_error("no such slicer instance") };
#ifdef _WIN32
    ::AllowSetForegroundWindow((DWORD) pid); // let the instance raise its own window
#endif
    int         status = 502;
    std::string body   = json_error("the slicer did not answer");
    // Http::post only sets the POST fields when a body is present; without one libcurl falls
    // back to its default read callback (stdin) and the hub faults. Always send a body.
    Http::post("http://127.0.0.1:" + std::to_string(target.port) + sub)
        .timeout_connect(2).timeout_max(120)
        .header("Content-Type", "application/x-www-form-urlencoded")
        .set_post_body(std::string("via=hub"))
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? json_error(err) : b; })
        .perform_sync();
    return { status, body };
}

bool HubServer::instance_window(long pid, bool show) { return instance_post(pid, std::string("/api/window?show=") + (show ? "1" : "0")).first == 200; }
bool HubServer::instance_quit(long pid, bool discard) { return instance_post(pid, std::string("/api/quit") + (discard ? "?discard=1" : "")).first == 200; }

// Loopback-only: the slicer instances and the hub's own page (GET /hub/) talk to these.
void HubServer::handle_hub(tcp::socket& client, Request& r)
{
    if (r.path == "/hub/info" && r.method == "GET") {
        remote_state(false);
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/remote" && r.method == "POST") {
        // ?on=1|0 turns Tailscale Serve for this hub on or off; ?add= / ?remove= edit the allow-list.
        const std::string on = query_param(r.query, "on");
        remote_logins(percent_decode(query_param(r.query, "add")), percent_decode(query_param(r.query, "remove")));
        std::string error;
        if (on == "1" || on == "0") set_remote(on == "1", error);
        else write_hub_json();
        remote_state(true);
        json j = info_json();
        if (!error.empty()) j["remote"]["error"] = error;
        respond_json(client, error.empty() ? 200 : 409, j.dump());
    } else if ((r.path == "/hub/" || r.path == "/hub/index.html") && r.method == "GET") {
        // The page gets the per-run secret and sends it back as X-Hub-Secret on every call.
        std::string page = read_file(resources_dir() + "/web/orca/hub.html"), secret;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            secret = m_secret;
        }
        const std::string ph = "__HUB_SECRET__";
        for (size_t at = page.find(ph); at != std::string::npos; at = page.find(ph, at + secret.size())) page.replace(at, ph.size(), secret);
        respond(client, 200, "text/html; charset=utf-8", page, "X-Frame-Options: DENY\r\n");
    } else if (r.path == "/hub/onvif" && r.method == "GET") {
        const auto res = onvif_discover();
        respond(client, res.first, res.first == 200 ? "application/json" : "text/plain; charset=utf-8", res.second);
    } else if (r.path == "/hub/qrcode.js" && r.method == "GET") {
        respond(client, 200, "application/javascript", read_file(resources_dir() + "/web/orca/qrcode.js"));
    } else if (r.path == "/hub/instances" && r.method == "GET") {
        respond_json(client, 200, instances_json().dump());
    } else if (r.path == "/hub/new" && r.method == "POST") {
        const long pid = spawn_slicer("", query_param(r.query, "hidden") == "1");
        json j;
        j["ok"]  = pid > 0;
        j["pid"] = pid;
        respond_json(client, pid > 0 ? 200 : 500, pid > 0 ? j.dump() : json_error("could not start a slicer"));
    } else if (r.path == "/hub/state" && r.method == "POST") {
        if (r.content_type.compare(0, 16, "application/json") != 0) { respond_json(client, 415, json_error("Content-Type must be application/json")); return; }
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
        set_phone(query_param(r.query, "on") == "1", query_param(r.query, "token") /* a remembered link to keep */);
        respond_json(client, 200, info_json().dump());
    } else if (r.path == "/hub/quit" && r.method == "POST") {
        respond_json(client, 200, "{\"ok\":true}");
        m_quit = true;
    } else if (r.path.compare(0, 15, "/hub/instances/") == 0 && r.method == "POST") {
        // /hub/instances/<pid>/window?show=1|0, /hub/instances/<pid>/quit[?discard=1] and
        // /hub/instances/<pid>/attention/clear (the hub page's Dismiss)
        const std::string rest  = r.path.substr(15);
        const size_t      slash = rest.find('/');
        const long        pid   = std::atol(rest.substr(0, slash).c_str());
        const std::string sub   = slash == std::string::npos ? "" : rest.substr(slash);
        if (sub == "/window") {
            auto res = instance_post(pid, std::string("/api/window?show=") + (query_param(r.query, "show") == "0" ? "0" : "1"));
            respond_json(client, res.first, res.second);
        } else if (sub == "/attention/clear") {
            auto res = instance_post(pid, "/api/attention/clear");
            respond_json(client, res.first, res.second);
        } else if (sub == "/quit") {
            auto res = instance_post(pid, std::string("/api/quit") + (query_param(r.query, "discard") == "1" ? "?discard=1" : ""));
            respond_json(client, res.first, res.second);
        } else {
            respond_json(client, 404, json_error("no such hub route"));
        }
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
            { {"method", "POST"}, {"path", "/api/instances/open"},  {"description", "body = a .3mf/.stl/.obj/.step file, header X-File-Name = its name; starts a new (hidden) slicer instance with it; ?visible=1 opens a window"} },
            { {"method", "POST"}, {"path", "/i/{id}/open?mode=load|import"}, {"description", "same upload, opened in instance {id}: load = save the current project, then open this project (default for .3mf); import = add the model to the current plate (default otherwise)"} },
            { {"method", "*"},    {"path", "/i/{id}/api/..."},      {"description", "the instance's own API (see GET /i/{id}/api)"} },
            { {"method", "GET"},  {"path", "/state"},               {"description", "camera list for the stream wall"} }
        });
        respond_json(client, 200, j.dump());
        return;
    }
    if (rest == "/api/instances" && r.method == "GET") {
        respond_json(client, 200, instances_json().dump());
        return;
    }
    if (rest == "/api/instances/open" && r.method == "POST") {
        std::string path, error;
        if (!spool_upload(client, r, path, error)) { respond_json(client, 400, json_error(error)); return; }
        const long pid = spawn_slicer(path, query_param(r.query, "visible") != "1");
        json j;
        j["ok"]      = pid > 0;
        j["file"]    = path;
        j["spawned"] = pid;
        respond_json(client, pid > 0 ? 200 : 500, pid > 0 ? j.dump() : json_error("could not start a new slicer"));
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

        std::string token, secret, auth;
        int         go2rtc_port;
        bool        phone;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            token       = m_token;
            secret      = m_secret;
            auth        = m_go2rtc_auth;
            go2rtc_port = m_go2rtc_port;
            phone       = m_phone;
        }

        // Through Tailscale Serve (a loopback peer carrying Tailscale-User-Login, which Serve sets
        // and strips from clients) only allow-listed tailnet logins get anything at all.
        const bool via_serve = peer.is_loopback() && !r.ts_login.empty();
        if (via_serve && !login_allowed(r.ts_login)) {
            BOOST_LOG_TRIVIAL(warning) << "RemoteHub: tailnet login not allowed: " << r.ts_login;
            respond(client, 403, "text/plain; charset=utf-8", "This printer hub is not shared with " + r.ts_login + ". Ask its owner to add you on the hub page.");
            return;
        }
        const std::string cookie_flags = (via_serve && r.fwd_proto == "https") ? "; Secure" : "";

        // The stream player lives at the root so its relative "api/ws" resolves here. Gate: the
        // phone's rt cookie, or - loopback only - the hub secret as `lt`, which is how the PC's own
        // Stream tab embeds it. The two scripts are public (verbatim go2rtc files).
        const bool player_ok = (!token.empty() && cookie_value(r.cookies, "rt") == token) ||
                               (peer.is_loopback() && !secret.empty() && query_param(r.query, "lt") == secret);
        if (r.path == PLAYER_PAGE) {
            if (!player_ok) { respond(client, 404, "text/plain", "not found"); return; }
            respond(client, 200, "text/html; charset=utf-8", read_file(resources_dir() + "/web/orca/player.html"));
            return;
        }
        for (const char* p : PLAYER_JS) {
            if (r.path == p) { respond(client, 200, "application/javascript", read_file(resources_dir() + "/web/orca" + std::string(p))); return; }
        }
        if (r.path == GO2RTC_WS) {
            if (!player_ok) { respond(client, 404, "text/plain", "not found"); return; }
            if (go2rtc_port == 0) { respond(client, 503, "text/plain", "stream relay is not running"); return; }
            tunnel(client, go2rtc_port, force_close(go2rtc_head(r.head, auth)), r.pending);
            return;
        }
        // go2rtc pulls a printer's raw H.264 through here (loopback + hub secret; see relay_h264).
        if (r.path == "/relay/h264") {
            if (!(peer.is_loopback() && !secret.empty() && query_param(r.query, "lt") == secret)) { respond(client, 404, "text/plain", "not found"); return; }
            const std::string url = relay_h264_url(query_param(r.query, "id"));
            if (url.empty()) { respond(client, 404, "text/plain", "unknown camera"); return; }
            relay_h264(client, url);
            return;
        }
        // Control routes for the slicer instances and the hub page on this PC: loopback peer,
        // loopback Host (no DNS rebinding), never cross-site, and the per-run secret as a custom
        // header - a cross-origin page cannot send one without a preflight we never answer. The
        // page and its script are the only unauthenticated GETs; the page carries the secret.
        if (r.path.compare(0, 5, "/hub/") == 0) {
            if (!peer.is_loopback() || !loopback_host(r.host) || r.sec_fetch_site == "cross-site") { respond(client, 404, "text/plain", "not found"); return; }
            const bool page = r.method == "GET" && (r.path == "/hub/" || r.path == "/hub/index.html" || r.path == "/hub/qrcode.js");
            if (!page && (secret.empty() || r.secret != secret)) { respond_json(client, 403, json_error("missing or wrong X-Hub-Secret")); return; }
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
                    "Set-Cookie: rt=" + token + "; Path=/; SameSite=Lax" + cookie_flags + "\r\n");
        } else if (rest == "/state") {
            // Through Tailscale Serve the phone learns who it is signed in as (shown in its top bar).
            std::string st = state_for_phone();
            if (via_serve) {
                try {
                    json j            = json::parse(st);
                    j["remote_login"] = r.ts_login;
                    st                = j.dump();
                } catch (...) {}
            }
            respond_json(client, 200, st);
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

bool HubServer::start()
{
    ensure_dirs();
    // Settings from the last run, unless the caller decided them.
    try {
        json j = json::parse(read_file(hub_json_path()));
        if (!valid_token(m_token)) m_token = j.value("token", "");
        m_phone = m_phone || j.value("phone", false);
        m_remote_on = j.value("remote_on", false);
        for (const auto& l : j.value("allowed_logins", json::array())) m_allowed_logins.push_back(lower(l.get<std::string>()));
    } catch (...) {}
    if (!valid_token(m_token)) m_token = random_token();
    m_secret = random_hex(16);
    m_state  = read_file(streams_json_path());

    start_go2rtc();
    BambuCamRelay::get().port();
    if (!bind(m_phone)) return false;
    write_hub_json();
    register_streams();
    if (m_remote_on) {
        // Serve's config outlives us (it is Tailscale's); make sure it still points at our port.
        TailscaleState t = remote_state(true);
        if (t.installed && t.backend == "Running" && (!t.serving || t.serving_port != m_port)) {
            std::string err;
            set_remote(true, err);
            if (!err.empty()) BOOST_LOG_TRIVIAL(warning) << "RemoteHub: remote access could not be restored: " << err;
        }
    }
    return true;
}

void HubServer::loop(bool idle_exit)
{
    auto idle_since = std::chrono::steady_clock::now();
    while (!m_quit) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
        flush_logs(); // the file sink buffers; keep hub.log readable while we run
        if (!idle_exit) continue;
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
}

void HubServer::shutdown()
{
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
    flush_logs();
}

// ------------------------------------------------------------ tray icon ----

// The hub's face on the desktop: phone access toggle, the hub page (QR code, link, open
// slicers), a new slicer window, quit. Double-click opens the page.
class HubTaskBarIcon : public wxTaskBarIcon
{
public:
    enum { ID_STATUS = wxID_HIGHEST + 100, ID_PHONE, ID_PAGE, ID_NEW, ID_NEW_HIDDEN, ID_QUIT,
           ID_INST_FIRST = wxID_HIGHEST + 200, ID_INST_PER = 3, ID_INST_MAX = 32,
           ID_INST_LAST = ID_INST_FIRST + ID_INST_PER * ID_INST_MAX };

    HubTaskBarIcon(HubServer& server, std::function<void()> on_quit) : m_server(server), m_on_quit(std::move(on_quit))
    {
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.set_phone(!m_server.snapshot().phone, ""); refresh(); }, ID_PHONE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { open_page(); }, ID_PAGE);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.spawn_slicer(""); }, ID_NEW);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_server.spawn_slicer("", true); }, ID_NEW_HIDDEN);
        Bind(wxEVT_MENU, [this](wxCommandEvent& e) {
            const int n = (e.GetId() - ID_INST_FIRST) / ID_INST_PER, what = (e.GetId() - ID_INST_FIRST) % ID_INST_PER;
            if (n < 0 || n >= (int) m_menu_pids.size()) return;
            const long pid = m_menu_pids[n];
            HubServer* s   = &m_server;
            // HTTP: never on the tray's (GUI) thread.
            std::thread([s, pid, what]() {
                if (what == 2) s->instance_quit(pid, false);
                else           s->instance_window(pid, what == 0);
            }).detach();
        }, ID_INST_FIRST, ID_INST_LAST);
        Bind(wxEVT_MENU, [this](wxCommandEvent&) { m_on_quit(); }, ID_QUIT);
        Bind(wxEVT_TASKBAR_LEFT_DCLICK, [this](wxTaskBarIconEvent&) { open_page(); });
    }

    void set_icon(const wxIcon& icon)
    {
        m_icon = icon;
        refresh();
    }

    void refresh()
    {
        const HubServer::Snapshot st = m_server.snapshot();
        wxString tip = "Snapmaker-Ultra Hub";
        tip += st.phone ? "\nPhone access on" : "\nPhone access off";
        tip += wxString::Format("\n%d slicer window%s open", st.instances, st.instances == 1 ? "" : "s");
        if (st.hidden > 0) tip += wxString::Format(" (%d hidden)", st.hidden);
        if (st.attention > 0) tip += wxString::Format("\n%d need%s attention on the PC", st.attention, st.attention == 1 ? "s" : "");
        if (m_icon.IsOk()) SetIcon(m_icon, tip);
    }

    wxMenu* CreatePopupMenu() override
    {
        const HubServer::Snapshot st = m_server.snapshot();
        auto* menu = new wxMenu;
        menu->Append(ID_STATUS, wxString::Format("Snapmaker-Ultra Hub: %d slicer window%s open", st.instances, st.instances == 1 ? "" : "s"))->Enable(false);
        menu->AppendSeparator();
        menu->AppendCheckItem(ID_PHONE, "Phone access")->Check(st.phone);
        menu->Append(ID_PAGE, st.phone ? "Open hub page (QR code, link, slicers)" : "Open hub page");
        menu->Append(ID_NEW, "Open a new slicer window");
        menu->Append(ID_NEW_HIDDEN, "Open a new hidden slicer");
        // Cheap: pid / title / hidden all come from <datadir>/hub/instances/<pid>.json.
        m_menu_pids.clear();
        auto* subs = new wxMenu;
        int   n    = 0;
        for (const Instance& inst : m_server.instances(false)) {
            if (n >= ID_INST_MAX) break;
            const int base = ID_INST_FIRST + n * ID_INST_PER;
            auto*     one  = new wxMenu;
            one->Append(base + 0, "Show window")->Enable(inst.hidden);
            one->Append(base + 1, "Hide window")->Enable(!inst.hidden);
            one->AppendSeparator();
            one->Append(base + 2, "Quit this window");
            subs->AppendSubMenu(one, wxString::Format("%d \xC2\xB7 %s%s%s", n + 1,
                inst.title.empty() ? wxString("Untitled") : wxString::FromUTF8(inst.title), inst.hidden ? wxString("  (hidden)") : wxString(),
                inst.needs_attention ? wxString("  (needs attention)") : wxString()));
            m_menu_pids.push_back(inst.pid);
            ++n;
        }
        if (n == 0) subs->Append(wxID_ANY, "No slicer is running")->Enable(false);
        menu->AppendSubMenu(subs, "Slicer windows");
        menu->AppendSeparator();
        menu->Append(ID_QUIT, "Quit hub (stops phone access and camera relays)");
        return menu;
    }

private:
    void open_page()
    {
        wxLaunchDefaultBrowser(wxString::Format("http://127.0.0.1:%d/hub/", m_server.snapshot().port));
    }

    HubServer&            m_server;
    std::function<void()> m_on_quit;
    wxIcon                m_icon;
    std::vector<long>     m_menu_pids; // menu order -> pid, rebuilt with every popup
};

// A wxApp with no windows: the tray icon plus the server running on its own thread.
class HubApp : public wxApp
{
public:
    HubApp(std::string token, bool phone) : m_server(std::move(token), phone) {}

    bool OnInit() override
    {
        SetAppName("Snapmaker-Ultra Hub");
        SetExitOnFrameDelete(false); // no frames: the loop runs until the server thread ends
        wxInitAllImageHandlers();
        if (!m_server.start()) {
            BOOST_LOG_TRIVIAL(error) << "RemoteHub: could not start the listener";
            return false;
        }
        m_icon = new HubTaskBarIcon(m_server, [this]() { m_server.request_quit(); });
        wxIcon icon(wxString::FromUTF8(Slic3r::var("Snapmaker_Orca.ico")), wxBITMAP_TYPE_ICO);
        if (!icon.IsOk()) icon = wxIcon(wxString::FromUTF8(Slic3r::var("Snapmaker_Orca_128px.png")), wxBITMAP_TYPE_PNG);
        m_icon->set_icon(icon);
        m_thread = std::thread([this]() {
            m_server.loop(false);
            CallAfter([this]() {
                if (m_icon) m_icon->RemoveIcon();
                ExitMainLoop();
            });
        });
        // Keep the tooltip's instance count fresh.
        m_timer.Bind(wxEVT_TIMER, [this](wxTimerEvent&) { if (m_icon) m_icon->refresh(); });
        m_timer.Start(5000);
        return true;
    }

    int OnExit() override
    {
        m_timer.Stop();
        m_server.request_quit();
        if (m_thread.joinable()) m_thread.join();
        m_server.shutdown();
        delete m_icon;
        m_icon = nullptr;
        return 0;
    }

private:
    HubServer       m_server;
    HubTaskBarIcon* m_icon { nullptr };
    std::thread     m_thread;
    wxTimer         m_timer;
};

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
        // The tray app owns the server from here; wxEntry returns when it quits.
        wxApp::SetInstance(new HubApp(valid_token(token_hint) ? token_hint : std::string(), phone_on));
#ifdef _WIN32
        // wxEntry wants argv in the ANSI code page; hand it just the module path (GUI_Init does the same).
        wchar_t module_path[MAX_PATH + 1] = {};
        ::GetModuleFileNameW(nullptr, module_path, MAX_PATH);
        char module_ansi[MAX_PATH * 2 + 1] = {};
        ::WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, module_path, -1, module_ansi, sizeof(module_ansi), nullptr, nullptr);
        int   argc   = 1;
        char* argv[] = { module_ansi, nullptr };
#else
        int   argc   = 1;
        char  name[] = "snapmaker-orca";
        char* argv[] = { name, nullptr };
#endif
        return wxEntry(argc, argv);
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
    j["remote_url"] = remote_url;
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
        if (j.contains("remote") && j["remote"].is_object()) i.remote_url = j["remote"].value("url", "");
        i.version     = j.value("version", "");
        for (const auto& ip : j.value("ips", json::array())) i.ips.push_back(ip.get<std::string>());
    } catch (...) {}
    return i;
}

struct HubFile { int port { 0 }; std::string secret; };
static HubFile hub_file()
{
    HubFile h;
    try {
        json j = json::parse(read_file(hub_json_path()));
        if (!pid_alive(j.value("pid", 0L))) return h;
        h.port   = j.value("port", 0);
        h.secret = j.value("secret", "");
    } catch (...) {}
    return h;
}

static Info hub_call(const std::string& method, const std::string& path, const std::string& body, long timeout)
{
    const HubFile hf = hub_file();
    if (hf.port == 0) return Info();
    Info        out;
    std::string url = "http://127.0.0.1:" + std::to_string(hf.port) + path;
    Http        http = method == "POST" ? Http::post(url) : Http::get(url);
    if (method == "POST") http.set_post_body(body).header("Content-Type", body.empty() ? "text/plain" : "application/json");
    http.header("X-Hub-Secret", hf.secret);
    http.timeout_connect(1).timeout_max(timeout)
        .on_complete([&out, &hf](std::string b, unsigned status) { if (status == 200) { out = parse_info(b); out.secret = hf.secret; } })
        .perform_sync();
    return out;
}

Info query() { return hub_call("GET", "/hub/info", "", 3); }

std::pair<int, std::string> onvif_discover()
{
    const HubFile hf = hub_file();
    if (hf.port == 0) return { 503, "the hub is not running" };
    int         status = 0;
    std::string body;
    Http::get("http://127.0.0.1:" + std::to_string(hf.port) + "/hub/onvif")
        .header("X-Hub-Secret", hf.secret)
        .timeout_connect(1).timeout_max(20)
        .on_complete([&](std::string b, unsigned s) { status = (int) s; body = b; })
        .on_error([&](std::string b, std::string err, unsigned s) { status = s ? (int) s : 502; body = b.empty() ? err : b; })
        .perform_sync();
    return { status == 0 ? 502 : status, body };
}

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
    const HubFile hf = hub_file();
    if (hf.port == 0) return false;
    bool ok = false;
    Http::post("http://127.0.0.1:" + std::to_string(hf.port) + "/hub/state")
        .timeout_connect(1).timeout_max(5)
        .header("Content-Type", "application/json")
        .header("X-Hub-Secret", hf.secret)
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
