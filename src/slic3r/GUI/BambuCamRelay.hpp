#pragma once

#include <atomic>
#include <thread>

namespace Slic3r {
namespace GUI {

// Local HTTP relay that re-serves Bambu Lab chamber cameras as MJPEG.
//
// Bambu printers (P1/A1 family and newer firmwares) stream their camera as raw
// JPEG frames over a TLS socket on port 6000, authenticated with the printer's
// LAN access code - a transport no browser can play. This relay accepts
//   GET /bambu?ip=<printer-ip>&code=<access-code>
// on 127.0.0.1:<port()>, speaks the printer protocol upstream, and forwards the
// frames as multipart/x-mixed-replace (MJPEG), which renders in a plain <img>.
class BambuCamRelay
{
public:
    static BambuCamRelay& get();

    // Starts the listener on first call; returns the bound local port (0 on failure).
    int port();

private:
    BambuCamRelay() = default;
    void ensure_started();
    void accept_loop();

    std::atomic<int>  m_port { 0 };
    std::atomic<bool> m_started { false };
    void*             m_acceptor { nullptr };
};

} // namespace GUI
} // namespace Slic3r
