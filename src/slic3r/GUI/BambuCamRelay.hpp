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

// Manages a bundled go2rtc process (resources/tools/go2rtc) that relays RTSPS
// LAN Liveview cameras (Bambu X1/H2 series and similar) into browser-playable
// MSE streams on a localhost API port. Started on demand from the Stream tab.
class Go2RtcLauncher
{
public:
    static Go2RtcLauncher& get();

    // Spawns go2rtc if it is not running; returns its API port (0 on failure).
    int  port();
    void stop();

private:
    Go2RtcLauncher() = default;

    std::atomic<bool> m_started { false };
    std::atomic<int>  m_port { 0 };
    long              m_pid { 0 };
    // Windows Job Object (HANDLE as void*) with KILL_ON_JOB_CLOSE: our go2rtc is
    // assigned to it so it dies with THIS app instance even on crash/force-quit — and
    // only ours (each instance has its own job; other instances' go2rtc are untouched).
    void*             m_job { nullptr };
};

} // namespace GUI
} // namespace Slic3r
