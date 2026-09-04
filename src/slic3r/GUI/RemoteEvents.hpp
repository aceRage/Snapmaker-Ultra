#pragma once

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

// The printer event watcher (P4 of docs/superpowers/specs/2026-09-03-phone-mobile-capabilities-research.md).
//
// The slicer instance is the only place where printer state already lives: a Bambu printer's
// MachineObject is fed by the networking plugin's MQTT push, a Snapmaker on the LAN answers
// Moonraker over plain HTTP, and a print host is probed by the same code /api/printers uses. So the
// watcher runs here, on the instance's own one-second GUI heartbeat, and is edge-triggered: it
// remembers what each printer looked like last time and emits an event only where something
// actually changed.
//
// Each event is posted to the hub (POST /hub/event on its loopback control plane), which owns
// delivery: the tray balloon, the ring the phone reads, and - once P5 lands - the relays. Posting
// is fire-and-forget off the GUI thread; with no hub running the event is simply dropped.
namespace RemoteEvents {

// What one printer looked like at one moment, in one vocabulary. Bambu's print_status and
// Klipper's print_stats.state are both normalised into `state` so the transition rule below has
// only one set of words to know about.
struct PrinterState
{
    std::string id, name, kind; // kind: bambu | snapmaker | printhost | connect
    // False when this instance cannot actually see this printer's print state. In LAN mode only
    // the connected Bambu printer reports one; the rest are known by discovery alone and their
    // "idle" is an absence of information, not a fact - no event may ever be invented for them.
    bool        watched { false };
    bool        online { false };
    std::string state; // idle | preparing | printing | paused | finished | failed | cancelled | ""
    std::string raw_state; // what the printer itself said (FINISH, complete, ...), for the text
    std::string job;       // the file / task it is on
    std::string stage;     // the printer's own words for what it is doing (Bambu get_curr_stage())
    int         stage_curr { -1 }; // Bambu stage index; 6 is "Paused due to filament runout"
    std::string error_code, error_text;
};

struct Snapshot
{
    long long                           at { 0 }; // unix ms; the only clock the rule sees
    std::map<std::string, PrinterState> printers;
};

// One event, without the id and time the hub assigns.
struct Event
{
    std::string printer_id, printer_name, printer_kind;
    std::string kind;     // started | finished | failed | cancelled | paused | resumed | runout | error
    std::string severity; // info | warning | error
    std::string title, text, code, job;
    nlohmann::json to_json(long instance_pid) const;
};

// Everything the watcher carries from one poll to the next.
struct Memory
{
    Snapshot                         last;
    std::map<std::string, long long> last_emit; // "<printer>|<kind>|<code or job>" -> snapshot time
};

// The transition rule, and the only place an event is decided: the previous memory plus the
// snapshot just taken give the events to send, with `mem` advanced to the new state. It reads no
// clock, touches no network and holds no globals - `now.at` is the only time it knows - so a test
// can drive a whole print through it without a printer. `cooldown_ms` suppresses a repeat of the
// same printer + kind + code (a flapping error, a reconnect that re-announces a start).
std::vector<Event> step(Memory& mem, const Snapshot& now, long long cooldown_ms = 180000);

// ---- the live watcher ----
// Called from RemoteAccess's one-second GUI heartbeat; polls at its own slower rate.
void heartbeat();
void stop();
// This instance's own recent events (the last 50), for GET /api/events?since=.
nlohmann::json recent(int since);

// Test hook (the SNORCA_DEBUG_ROUTES back door): run `step` over snapshots handed in as JSON and
// report what each one produced. This is how the transition rule is covered without hardware.
nlohmann::json replay(const nlohmann::json& in);

} // namespace RemoteEvents
} // namespace GUI
} // namespace Slic3r
