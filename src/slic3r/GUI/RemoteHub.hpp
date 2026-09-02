#pragma once

#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace GUI {

// The "hub": one long-lived helper process per PC (this same executable started with
// --hub) that owns everything a phone or an agent talks to, so it keeps working after the
// slicer window is closed and serves every open slicer instance at once:
//
//   * the token-gated LAN listener on :13640 (phone page, camera list, JSON API),
//   * the bundled go2rtc relay and the Bambu MJPEG relay (camera streams),
//   * the list of running slicer instances (each instance runs a loopback-only JSON API
//     and drops a <pid>.json under <datadir>/hub/instances; the hub proxies
//     /r/<token>/i/<pid>/api/... to it),
//   * uploads from the phone (<datadir>/hub/uploads), opened in an existing instance or
//     in a freshly spawned one.
//
// The first slicer instance that needs it spawns the hub detached; the hub exits on its
// own once phone access is off and no instance has been alive for a minute. State the
// hub needs across restarts (token, phone on/off, the camera list) lives in
// <datadir>/hub/hub.json and streams.json.
namespace RemoteHub {

struct Info
{
    bool                     alive { false };
    long                     pid { 0 };
    int                      port { 0 };      // the listener port (LAN when phone is on, loopback otherwise)
    bool                     phone { false }; // LAN listener + /r/<token>/ routes enabled
    std::string              token;
    std::string              secret;          // hub.json's per-run secret for /hub/* (client side only)
    int                      go2rtc_port { 0 };
    int                      relay_port { 0 };
    std::string              version;
    std::vector<std::string> ips; // LAN IPv4 addresses, default-route one first (phone on only)
    std::string              remote_url; // https://<machine>.<tailnet>.ts.net/r/<token>/ while Tailscale remote access is on
    std::string url() const;      // http://<ip>:<port>/r/<token>/ or ""
    std::string json() const;     // what the Stream tab's phone modal shows: {on, port, token, ips, url}
};

// Process mode: serve until asked to quit or idle. Called from CLI::run for `--hub`.
int run_server(const std::string& token_hint, bool phone_on);

// ---- client side (a slicer instance) ----
Info query();                                                       // is a hub running? (~1 s worst case)
std::pair<int, std::string> onvif_discover();                       // ONVIF WS-Discovery via the hub's go2rtc: {http status, body}
Info ensure_running(const std::string& token_hint, bool phone_on); // spawn one if needed; waits for it
Info set_phone(bool on, const std::string& token = ""); // a valid token keeps a remembered link
bool post_state(const std::string& json); // full Stream-tab state; remembered for a hub started later
void quit();

std::string hub_dir();
std::string instances_dir();
std::string uploads_dir();
std::string saves_dir();

} // namespace RemoteHub
} // namespace GUI
} // namespace Slic3r
