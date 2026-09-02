#pragma once

#include <mutex>
#include <string>
#include <vector>

namespace Slic3r {
namespace GUI {

// "Phone access" for the Stream tab.
//
// A token-gated HTTP listener on all interfaces (off by default, toggled from the Stream
// tab) that lets a phone on the same LAN open the camera wall:
//   GET /r/<token>/            the stream page in remote mode (also sets the rt cookie)
//   GET /r/<token>/state       the camera list, stripped of addresses and credentials
//   GET /r/<token>/bambu?id=   MJPEG tunnel to the local BambuCamRelay for a P1/A1 host
//   GET /r/<token>/ff?id=      Flashforge camera-URL probe (the phone cannot POST to it)
//   /stream.html, /video-*.js, /api/ws   go2rtc player + MSE websocket, tunneled to the
//                              local go2rtc when the request carries cookie rt=<token>
// Only private/loopback peers are answered. The full Stream-tab state (with credentials)
// is pushed here by the PC page and never leaves this machine; streams are registered in
// go2rtc from here so the phone only ever sees opaque stream names.
//
// Later phases (printer status/control, plates, slice, send, agent drivability) are meant
// to hang off the same token-gated listener as JSON routes under /r/<token>/api/...
class RemoteAccess
{
public:
    struct Info
    {
        bool                     on { false };
        int                      port { 0 };
        std::string              token;
        std::vector<std::string> ips; // LAN IPv4 addresses, default-route one first
        std::string              json() const;
    };

    static RemoteAccess& get();

    // Idempotent; call from the main thread (may spawn go2rtc). A non-empty token is reused so a
    // remembered link keeps working across restarts; otherwise a fresh one is generated.
    Info start(const std::string& token = "");
    void stop();
    Info info();

    // Full Stream-tab state as JSON ({hosts:[...], active:[...]}), pushed by the PC page.
    void set_state(const std::string& json);

    // Flashforge new-gen LAN API: pull cameraStreamUrl out of the /detail JSON ("" if absent).
    static std::string ff_camera_url_from_detail(const std::string& body);

private:
    RemoteAccess() = default;
    void accept_loop();
    void serve(void* socket); // boost::asio tcp::socket*, owned by the call
    void register_streams();  // PUT every go2rtc-kind host into the local go2rtc
    std::string state_for_phone();
    bool        lookup_host(const std::string& id, std::string& ip, std::string& code);

    std::mutex  m_mutex;
    bool        m_on { false };
    int         m_port { 0 };
    std::string m_token;
    std::string m_state; // JSON from the PC page
    void*       m_acceptor { nullptr };
};

} // namespace GUI
} // namespace Slic3r
