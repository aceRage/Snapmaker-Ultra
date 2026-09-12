#ifndef slic3r_PrintHostDeviceStatus_hpp_
#define slic3r_PrintHostDeviceStatus_hpp_

#include <string>
#include <vector>

#include "PrintHostDevices.hpp"

// Asking one print-host device what it is doing and what is loaded in it.
//
// This is the honest half of the "map the plate's filaments to the printer's slots" story, and the
// honesty is the point: **most print hosts cannot answer the second question at all.**
//
//  * Elegoo Link (the Centauri Carbon) speaks SDCP over its own websocket on port 3030. The fork's
//    ElegooLink only ever sends two commands - `Cmd 0` (status), whose answer it reads exactly one
//    field of (`Status.CurrentStatus`, and only to notice the value 8, "checking the file"), and
//    `Cmd 128` (start print). `Cmd 1` (Attributes), which is where an SDCP device would describe
//    its materials, is declared in the enum and never sent; `PrintInfo` is never read. There is no
//    filament, material, box or slot field anywhere in the Elegoo path, and the upload carries no
//    per-extruder parameter. So for an Elegoo device this returns `probed = false` with a note that
//    says so, and the send goes out exactly as the plate was sliced.
//  * A Moonraker/Klipper box does answer. `/printer/objects/query` gives `print_stats.state`, and a
//    Snapmaker-flavoured Moonraker also carries `print_task_config`, whose parallel arrays are the
//    loaded filaments per toolhead (the same object SnapmakerLan::toolheads_of reads,
//    SnapmakerLan.cpp:456, and the same one SSWCP's machine filament info comes from). Where there
//    is no `print_task_config`, the `extruder`/`extruder1`... objects still say how many tools the
//    machine has, which is a slot list without materials in it.
//  * A PrusaLink / PrusaConnect printer (a Buddy board on an MK4/MK4S/MINI/CORE One, or the
//    PrusaLink daemon in front of an MK3S) answers its own REST API: `/api/v1/status` gives the
//    state and both heaters, `/api/v1/job` the file it is printing. See PrusaLinkStatus.hpp. It
//    names no filaments at all - the API has no filament-identity story - so it comes back online
//    with a state and an empty slot list.
//  * Everything else (Duet, Repetier, SimplyPrint, ...) is not probed: this fork has no client for
//    their status APIs, and guessing would cost a timeout per address.
//
// Blocking. Never call it from the GUI thread.

namespace Slic3r {
namespace PrintHostDevices {

// One place a filament can be, as the printer describes it.
struct Slot
{
    int         index { 0 };    // 0-based, the tool/extruder number
    std::string type;           // "PLA", "" = the printer did not say
    std::string sub_type;
    std::string vendor;
    std::string color;          // "#RRGGBB", "" = the printer did not say
    bool        loaded { false };
    double      nozzle { 0.0 };

    // Something to put in a dropdown.
    std::string label() const;
};

struct Status
{
    bool              probed { false }; // this host type has a status API this fork can speak
    bool              online { false }; // and it answered
    std::string       state;            // print_stats.state ("standby", "printing", ...), "" = unknown
    std::string       error;            // why it did not answer
    std::string       note;             // in words, for the dialog: why there is no slot list
    std::vector<Slot> slots;            // empty = no mapping is possible, see `note`
    // The printer named the materials, not just the tools. Only then is a mapping table worth
    // showing: a bare tool list with nothing in it tells a person nothing they did not know.
    bool              slots_have_filaments { false };
};

// Blocking, off the GUI thread. `timeout_s` is per HTTP call.
Status probe(const Device& d, int timeout_s = 3);

// The half of probe() that has no network in it: a Moonraker `result.status` object turned
// into slots and a state. Exposed so the parsing is exercised by the Catch2 suite instead of
// only by a printer on somebody's desk. `status_json` is the JSON text of that object.
Status parse_moonraker_status(const std::string& status_json, const std::string& host_type_key);

// What can be said before asking: false for every host type this fork has no status client for.
bool   can_probe(const std::string& host_type_key);
// The sentence the dialog shows when `probe()` came back with nothing to map.
std::string no_filament_note(const std::string& host_type_key);

} // namespace PrintHostDevices
} // namespace Slic3r

#endif // slic3r_PrintHostDeviceStatus_hpp_
