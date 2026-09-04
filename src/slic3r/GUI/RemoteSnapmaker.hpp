#pragma once

#include <string>
#include <utility>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

// Connecting a paired Snapmaker printer (Moonraker over MQTT) from the phone / agent API, without
// the PC's Device page. The Device page's "My Devices" pick is a Flutter page driving SSWCP
// (sw_create_mqtt_client -> sw_mqtt_connect -> sw_mqtt_subscribe -> sw_mqtt_set_engine); those
// entry points are keyed by the page's wxWebView and answer into it, so they cannot be called from
// a request. This module performs the same steps with the credentials AppConfig already holds for
// a device that was paired on the PC, and leaves the PC in the state a manual pick leaves it in
// (the device cards, the printer combo boxes and the Device tab's page).
//
// Pairing itself (the PIN / server.request_key exchange) is not reproduced: a device that was
// never paired on this PC has no stored credentials and is reported as such.
namespace RemoteSnapmaker {

// Any thread. Called when the PC's own Device page connects a device: the printer answers only a
// client it has issued a certificate to, and this fork keeps that certificate in the web page's
// storage alone (AppConfig blanks it, SSWCP.cpp:6716-6718), so a phone connect has nothing to log
// in with. With app_config app/snapmaker_remember_keys set to "1" - off by default, because it
// puts a private key on disk - this keeps what the PC's connect used under
// <datadir>/hub/snapmaker_keys.json, and connect() below can then reconnect that printer without
// anyone at the PC. Nothing kept here is ever sent to the phone.
void remember_credentials(const std::string& sn, const nlohmann::json& connect_params);

// GUI thread. The paired devices as the Device page lists them (no credentials), which one is the
// connected host, and what the current printer preset is.
void list(nlohmann::json& out);

// Request thread (never the GUI thread): blocks on the MQTT connect and the printer's first
// answer, a few seconds normally and up to ~30 s for a printer that is off. {200, ""} or an HTTP
// status with a message for the phone.
std::pair<int, std::string> connect(const std::string& dev_id);
std::pair<int, std::string> disconnect();

} // namespace RemoteSnapmaker
} // namespace GUI
} // namespace Slic3r
