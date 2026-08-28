#include "BambuCamRelay.hpp"

#include <boost/asio.hpp>
#include <boost/asio/ssl.hpp>
#include <boost/log/trivial.hpp>

#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

namespace asio = boost::asio;
using tcp = asio::ip::tcp;

static std::map<std::string, std::string> parse_query(const std::string& target)
{
    std::map<std::string, std::string> out;
    size_t q = target.find('?');
    if (q == std::string::npos)
        return out;
    std::string qs = target.substr(q + 1);
    size_t pos = 0;
    while (pos < qs.size()) {
        size_t amp = qs.find('&', pos);
        if (amp == std::string::npos) amp = qs.size();
        std::string kv = qs.substr(pos, amp - pos);
        size_t eq = kv.find('=');
        if (eq != std::string::npos) {
            std::string key = kv.substr(0, eq);
            std::string val = kv.substr(eq + 1);
            // minimal %XX decoding for the access code
            std::string dec;
            for (size_t i = 0; i < val.size(); ++i) {
                if (val[i] == '%' && i + 2 < val.size()) {
                    dec += (char) std::stoi(val.substr(i + 1, 2), nullptr, 16);
                    i += 2;
                } else if (val[i] == '+') {
                    dec += ' ';
                } else {
                    dec += val[i];
                }
            }
            out[key] = dec;
        }
        pos = amp + 1;
    }
    return out;
}

static void write_all(tcp::socket& s, const char* data, size_t len)
{
    asio::write(s, asio::buffer(data, len));
}

static void serve_client(tcp::socket client)
{
    try {
        // Read the request head.
        asio::streambuf req;
        asio::read_until(client, req, "\r\n\r\n");
        std::istream is(&req);
        std::string method, target;
        is >> method >> target;
        if (method != "GET" || target.find("/bambu") != 0) {
            static const char nf[] = "HTTP/1.1 404 Not Found\r\nConnection: close\r\n\r\n";
            write_all(client, nf, sizeof(nf) - 1);
            return;
        }
        auto        params = parse_query(target);
        std::string ip     = params["ip"];
        std::string code   = params["code"];
        if (ip.empty() || code.empty() || code.size() > 32 ||
            ip.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.:-_") != std::string::npos) {
            static const char br[] = "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
            write_all(client, br, sizeof(br) - 1);
            return;
        }

        // Connect to the printer's camera port (TLS, self-signed certificate).
        asio::io_context ioc;
        asio::ssl::context ssl_ctx(asio::ssl::context::tls_client);
        ssl_ctx.set_verify_mode(asio::ssl::verify_none);
        asio::ssl::stream<tcp::socket> upstream(ioc, ssl_ctx);
        tcp::resolver resolver(ioc);
        asio::connect(upstream.next_layer(), resolver.resolve(ip, "6000"));
        upstream.next_layer().set_option(tcp::no_delay(true));
        upstream.handshake(asio::ssl::stream_base::client);

        // 80-byte auth packet: u32le 0x40, u32le 0x3000, u32 0, u32 0,
        // then username and access code, each null-padded to 32 bytes.
        unsigned char auth[80] = { 0 };
        auth[0] = 0x40;
        auth[4] = 0x00; auth[5] = 0x30; // 0x3000 little-endian
        const char user[] = "bblp";
        std::memcpy(auth + 16, user, sizeof(user) - 1);
        std::memcpy(auth + 48, code.data(), code.size());
        asio::write(upstream, asio::buffer(auth, sizeof(auth)));

        // MJPEG response head.
        static const char head[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: multipart/x-mixed-replace; boundary=bambuframe\r\n"
            "Cache-Control: no-cache\r\n"
            "Access-Control-Allow-Origin: *\r\n"
            "Connection: close\r\n\r\n";
        write_all(client, head, sizeof(head) - 1);

        // Frames are delimited by JPEG markers (FF D8 ... FF D9); scan the byte
        // stream rather than trusting the 16-byte frame headers.
        std::vector<unsigned char> buf;
        buf.reserve(256 * 1024);
        unsigned char chunk[8192];
        for (;;) {
            size_t n = upstream.read_some(asio::buffer(chunk, sizeof(chunk)));
            buf.insert(buf.end(), chunk, chunk + n);
            for (;;) {
                // find start marker
                size_t start = std::string::npos;
                for (size_t i = 0; i + 1 < buf.size(); ++i)
                    if (buf[i] == 0xFF && buf[i + 1] == 0xD8) { start = i; break; }
                if (start == std::string::npos) {
                    if (buf.size() > 2 * 1024 * 1024) buf.clear(); // garbage guard
                    break;
                }
                size_t end = std::string::npos;
                for (size_t i = start + 2; i + 1 < buf.size(); ++i)
                    if (buf[i] == 0xFF && buf[i + 1] == 0xD9) { end = i + 2; break; }
                if (end == std::string::npos)
                    break; // frame incomplete, read more
                char part[128];
                int  hl = std::snprintf(part, sizeof(part),
                                        "--bambuframe\r\nContent-Type: image/jpeg\r\nContent-Length: %zu\r\n\r\n",
                                        end - start);
                write_all(client, part, (size_t) hl);
                write_all(client, (const char*) buf.data() + start, end - start);
                write_all(client, "\r\n", 2);
                buf.erase(buf.begin(), buf.begin() + end);
            }
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(info) << "BambuCamRelay: session ended: " << e.what();
        try {
            static const char bg[] = "HTTP/1.1 502 Bad Gateway\r\nConnection: close\r\n\r\n";
            write_all(client, bg, sizeof(bg) - 1);
        } catch (...) {}
    }
}

BambuCamRelay& BambuCamRelay::get()
{
    static BambuCamRelay instance;
    return instance;
}

int BambuCamRelay::port()
{
    ensure_started();
    return m_port.load();
}

void BambuCamRelay::ensure_started()
{
    bool expected = false;
    if (!m_started.compare_exchange_strong(expected, true))
        return;
    try {
        static asio::io_context ioc; // lives for the process
        auto* acceptor = new tcp::acceptor(ioc, tcp::endpoint(asio::ip::make_address("127.0.0.1"), 0));
        m_acceptor = acceptor;
        m_port     = acceptor->local_endpoint().port();
        BOOST_LOG_TRIVIAL(info) << "BambuCamRelay: listening on 127.0.0.1:" << m_port.load();
        std::thread([this]() { accept_loop(); }).detach();
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "BambuCamRelay: failed to start: " << e.what();
        m_port = 0;
    }
}

void BambuCamRelay::accept_loop()
{
    auto* acceptor = static_cast<tcp::acceptor*>(m_acceptor);
    for (;;) {
        try {
            tcp::socket client(acceptor->get_executor());
            acceptor->accept(client);
            std::thread(serve_client, std::move(client)).detach();
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(error) << "BambuCamRelay: accept failed: " << e.what();
            return;
        }
    }
}

} // namespace GUI
} // namespace Slic3r
