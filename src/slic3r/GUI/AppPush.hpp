#pragma once

#include <nlohmann/json_fwd.hpp>

#include <string>
#include <utility>

namespace Slic3r {
namespace GUI {

// Native app push (Ultra1 phase 1). The third sink on the hub's event seam, beside
// RemoteNotify::deliver (the relays) and WebPush::deliver (the browser).
//
// What it is for. Web Push only works where the page has a secure context, which on the plain
// http://<lan-ip>:13640/ link it never does, and on iOS only inside a Home Screen web app. A
// native companion app has neither limit: APNs and FCM deliver to the *app*, wherever the phone
// is - on the LAN, on the tailnet, or on cellular data with this PC switched off.
//
// What the push services see. Nothing readable. The event is encrypted with WebPush::encrypt -
// the same RFC 8291 aes128gcm code path, to a P-256 key pair the app generated on the device and
// whose private half never leaves its Keychain or Keystore - and only the base64url of that blob
// is put in the APNs or FCM payload. Apple and Google see a device token, a size, a collapse id
// and ciphertext. The visible strings in the APNs payload are a deliberate placeholder ("Printer
// update" / "Tap to open"): Apple requires an alert dictionary for a Notification Service
// Extension to run at all, and that placeholder is exactly what the person sees if the extension
// ever fails to decrypt.
//
// The credentials. An APNs .p8 signing key (with its key id and team id) and an FCM service
// account are the *user's own*, created under their own developer accounts and referenced by
// path. They are read by the hub, masked on every /hub/* response, and appear nowhere on the
// phone plane at all. Whoever holds a team's .p8 can push to every app that team signed, which is
// why no such key is ever in this repository or in a release artifact.
//
// Threading. Same shape as WebPush: deliver() runs on RemoteNotify's worker thread, never on a
// request thread. test() is the one exception and runs on the caller's, because somebody is
// watching the hub page for the answer.
namespace AppPush {

// Load the "apppush" object out of settings.json (or a null value on a data dir that has none)
// and configure the providers from it. Called once from HubServer::start().
void start(const nlohmann::json& saved);

// Nothing to join - the sender borrows RemoteNotify's worker - but sets the flag that lets a push
// waiting out a retry backoff give up, and drops the cached credentials.
void stop();

// What write_hub_json() puts back under settings.json's "apppush" key: the provider settings and
// every device row, in the clear. The only place a stored credential leaves this module.
nlohmann::json settings_json();

// What GET /hub/apppush answers: the same thing with every credential masked to "****"+last-4,
// every device token masked, and each provider's availability spelled out in a sentence.
nlohmann::json masked_json();

// POST /r/<token>/push/device - the app registering its platform push token and the public half
// of the key pair it made. Idempotent on (platform, token): the app re-registers on every cold
// launch, because push tokens rotate and no platform reliably tells us when.
std::pair<int, std::string> register_device(const std::string& body);

// DELETE /r/<token>/push/device - the app unpairing. Answers {"ok":true} whether or not the row
// existed, so it cannot be used to find out whether some token is registered here.
std::pair<int, std::string> forget_device(const std::string& body);

// DELETE /hub/apppush?id=<id> - the hub page removing a device by its short id.
std::pair<int, std::string> remove(const std::string& id);

// POST /hub/apppush/options - enabled, min_severity and the two providers' settings. Credential
// fields follow take_secret's rule: a value beginning "****" means keep the stored one, so a
// credential only ever travels inward.
std::pair<int, std::string> set_options(const std::string& body);

// POST /hub/apppush/test - one synchronous push to every device, with per-device results naming
// the provider, the host it reached and the status.
std::pair<int, std::string> test();

// Called from RemoteNotify's worker for every event that passes the hub's own filter.
void deliver(const nlohmann::json& event);

// True when at least one device is registered and push is on - lets the event path skip the work.
bool has_devices();

// True once, if anything changed that settings.json does not yet know about: a device
// registered, or a dead one was pruned on the sender thread.
bool consume_dirty();

// The capability object an app wants before it decides which transport to offer:
// {"apns":<bool>,"fcm":<bool>,"reason":...}. Safe for the phone plane - it names no credential.
nlohmann::json providers_json();

// POST /hub/apppush/debug (SNORCA_DEBUG_ROUTES=1 only): lets the gate look at the exact envelope
// this module would send, without a mock having to receive one first.
std::pair<int, std::string> debug_op(const std::string& body);

} // namespace AppPush
} // namespace GUI
} // namespace Slic3r
