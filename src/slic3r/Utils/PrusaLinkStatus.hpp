#ifndef slic3r_PrusaLinkStatus_hpp_
#define slic3r_PrusaLinkStatus_hpp_

#include <string>
#include <vector>

// Asking a PrusaLink / PrusaConnect printer what it is doing.
//
// PrusaLink (the Buddy-board web API on an MK4/MK4S/MINI/CORE One, and the PrusaLink daemon in
// front of an MK3S) answers a small REST API that is nothing like Moonraker's:
//
//   GET /api/version    { "api": "2.0.0", "server": "2.1.2", "text": "PrusaLink", "hostname": ...,
//                         "capabilities": { "upload-by-put": true } }
//   GET /api/v1/status  { "printer": { "state": "PRINTING", "temp_nozzle": 215.0,
//                                      "target_nozzle": 215.0, "temp_bed": 60.0, "target_bed": 60.0,
//                                      "axis_z": 4.2, "flow": 100, "speed": 100, "fan_hotend": 6000,
//                                      "fan_print": 4000 },
//                         "job": { "id": 34, "progress": 12.0, "time_remaining": 3120,
//                                  "time_printing": 420 },
//                         "storage": { ... }, "camera": { ... } }
//   GET /api/v1/job     { "id": 34, "state": "PRINTING", "progress": 12.0, "time_remaining": 3120,
//                         "time_printing": 420,
//                         "file": { "refs": {...}, "name": "cube.bgcode",
//                                   "display_name": "cube.bgcode", "path": "/usb", "size": 91234,
//                                   "m_timestamp": 1699999999 } }
//
// `/api/v1/status` alone carries the state, the temperatures and the progress, so it is the one
// call this fork makes on every poll. `/api/v1/job` is asked only while the printer says it is
// printing (or paused), because that is the only answer that names the file.
//
// The state strings are upper-case and are PrusaLink's own vocabulary, not Klipper's:
//   IDLE, READY, BUSY, PRINTING, PAUSED, FINISHED, STOPPED, ERROR, ATTENTION, PRINTING (and
//   older firmwares also say "PRINTING"/"PAUSED" from /api/printer's `state.text`).
// `to_klipper_state()` maps them onto the strings the rest of this API already speaks
// (standby / printing / paused / complete / cancelled / error), so the phone's cards, the event
// watcher and the control buttons need no PrusaLink-specific code at all.
//
// Auth is exactly what the PrusaLink PrintHost class uses (OctoPrint.cpp:597): an `X-Api-Key`
// header when printhost_authorization_type is atKeyPassword, HTTP Digest with the user and
// password when it is atUserPassword. A Buddy board answers 401 to everything without it.
//
// Cameras are deliberately not here: a Buddy board has no /api/v1/cameras endpoint.
//
// Blocking. Never call any of it from the GUI thread.

namespace Slic3r {
namespace PrusaLinkStatus {

// Everything one poll of a PrusaLink printer can say. All of it optional: a field that the printer
// did not report keeps its `has_*` flag false, and the caller then leaves it off the card rather
// than rendering a zero or a NaN.
struct Status
{
    bool        answered { false };   // the printer replied to /api/v1/status at all
    bool        authorized { true };  // false when it answered 401/403: the key or password is wrong
    std::string error;                // why it did not answer

    std::string raw_state;            // PrusaLink's own word: "PRINTING", "IDLE", ...
    std::string state;                // the same thing in this API's vocabulary, see to_klipper_state

    bool        has_progress { false };
    double      progress { 0.0 };     // 0..100, as PrusaLink reports it

    bool        has_time_remaining { false };
    long long   time_remaining { 0 }; // seconds
    bool        has_time_printing { false };
    long long   time_printing { 0 };  // seconds

    bool        has_bed { false };
    double      bed_temp { 0.0 }, bed_target { 0.0 };
    bool        has_nozzle { false };
    double      nozzle_temp { 0.0 }, nozzle_target { 0.0 };

    std::string filename;             // the job's display name, "" = not printing / not asked
    long long   job_id { 0 };
};

// Which host_type enum keys this client can speak.
bool speaks_prusalink(const std::string& host_type_key);

// PrusaLink's state word -> the vocabulary the hub's cards, buttons and events already use:
//   IDLE / READY / STOPPED            -> "standby"
//   PRINTING                          -> "printing"
//   PAUSED                            -> "paused"
//   FINISHED                          -> "complete"
//   ERROR / ATTENTION                 -> "error"
//   BUSY                              -> "busy"
// Anything unrecognised comes back lower-cased, so a firmware that invents a word still shows it.
std::string to_klipper_state(const std::string& prusalink_state);

// The network-free half: the JSON text of a /api/v1/status answer (and optionally of a
// /api/v1/job one) turned into a Status. Exposed so the Catch2 suite exercises the mapping
// without a printer on somebody's desk.
Status parse_status(const std::string& status_json, const std::string& job_json = std::string());

// How this fork addresses a PrusaLink box: the same normalisation OctoPrint::make_url does
// (a bare host[:port] becomes http://, a trailing slash is dropped), so a preset that works for an
// upload works here.
std::string base_url(const std::string& host);

// What to send with each request. Mirrors PrusaLink::set_auth.
struct Auth
{
    std::string auth_type; // "key" | "user"
    std::string apikey;
    std::string user, password;
};

// One poll, read-only, never a command. `timeout_s` is per HTTP call; /api/v1/job is only asked
// when the status said the printer is printing or paused.
Status probe(const std::string& host, const Auth& auth, int timeout_s = 2);

} // namespace PrusaLinkStatus
} // namespace Slic3r

#endif // slic3r_PrusaLinkStatus_hpp_
