#pragma once

#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

// Snapmaker printers over the LAN, through the Moonraker HTTP API they serve on port 80.
//
// Measured on the U1 (firmware 1.5.2): GET /access/info answers
// {"login_required": false, "trusted": true}, and /server/info, /printer/info,
// /machine/system_info, /printer/objects/query, /server/files/* all answer without any
// credential. Uploading is a multipart POST to /server/files/upload and a print is started with
// POST /printer/print/start?filename=. Nothing here needs the MQTT connection the PC's Device tab
// makes, so the phone can list, watch and feed a printer the PC has never connected to.
//
// The device list lives in <datadir>/hub/snapmaker_lan.json, next to the hub's own state, so every
// slicer instance on this PC sees the same printers.
namespace SnapmakerLan {

struct Device
{
    std::string id;       // the printer's serial number when known, else its address
    std::string name;     // its own device name (or the mDNS instance name)
    std::string model;    // "Snapmaker U1"
    std::string ip;
    int         port { 80 };
    std::string added_by; // discovery | manual | device_tab
};

// What one probe of a printer found. Empty/false when it did not answer.
struct Status
{
    bool        online { false };
    bool        login_required { false };
    std::string state;        // print_stats: standby | printing | paused | complete | cancelled | error
    std::string filename;     // the job the printer is on
    std::string message;      // the printer's own error / display message
    double      progress { 0 };   // 0..1
    double      bed_temp { 0 }, bed_target { 0 }, nozzle_temp { 0 }, nozzle_target { 0 };
    int         layer { 0 }, total_layers { 0 };
    double      print_duration { 0 }, total_duration { 0 };
    std::string klippy;       // klippy_state: ready | startup | shutdown | error
    bool        printing() const { return state == "printing" || state == "paused"; }
};

// One of the printer's toolheads and what is loaded in it (print_task_config, per-toolhead arrays).
struct Toolhead
{
    int         index { 0 };
    std::string type;      // PLA, PETG, ...
    std::string sub_type;  // Matte, Silk, ...
    std::string vendor;
    std::string color;     // #RRGGBB, from filament_color_rgba (RRGGBBAA)
    bool        loaded { false };  // filament_exist
    bool        official { false };
    double      nozzle { 0 };      // extruder<N>.nozzle_diameter
};

// ---- the shared list ----
std::vector<Device> devices();                                   // any thread
bool                find(const std::string& id, Device& out);
// Ask an address who it is, without remembering it (blocking, a few seconds).
bool                identify_at(const std::string& ip, int port, Device& out, std::string& error);
// Probe an address and remember it (blocking, a few seconds). error is for the phone.
bool                add(const std::string& ip, int port, Device& out, std::string& error);
bool                remove(const std::string& id);               // only what a person added
// The printers this PC already knows from its Device tab (AppConfig): GUI thread.
void                merge_app_devices();
// One mDNS pass in the background, merging what answers into the list (the fork's Bonjour, the
// "snapmaker" service the Device page's own search uses). Cheap to call: at most one pass a minute.
void                start_discovery();
// The Stream tab's camera list (<datadir>/hub/streams.json) is a fourth source: a camera whose
// address answers as a Snapmaker is that printer, with the camera's alias as its name. Bambu
// cameras are left alone - those printers arrive through the device manager.
void                merge_stream_devices();

// ---- live state ----
// Probes the printer, at most once every few seconds per device.
Status status(const Device& d);
// The same, ignoring that cache: for watching a printer right after telling it to do something.
Status status_now(const Device& d);
// The last probe's answer without asking the printer: for the GUI thread, which must not wait on
// the network. False when this printer has never been probed.
bool cached_status(const Device& d, Status& out);
// What each toolhead holds, from the same cached probe.
std::vector<Toolhead> toolheads(const Device& d);
// The whole list with each printer's state, probed in parallel. Any thread but the GUI one.
void   list_json(nlohmann::json& out);
// What /api/printers needs for the send picker (one entry per device).
void   list_printers(nlohmann::json& printers);

// ---- sending ----
// Multipart upload to /server/files/upload with print=false, so a failed print start still leaves
// a usable file on the printer. `progress` is called with 0..100.
bool upload(const Device& d, const std::string& source_path, const std::string& filename,
            std::function<void(int)> progress, std::string& error);
// GET /server/files/metadata?filename= - proof that the file really landed.
bool metadata(const Device& d, const std::string& filename, long long& size, std::string& error);
// POST /printer/print/start?filename= - the whole file as it was sliced, no mapping.
bool start_print(const Device& d, const std::string& filename, std::string& error);

// One filament of the sliced file, as the plate holds it.
struct FileFilament
{
    int         index { 0 };  // the file's filament / extruder slot, 0-based
    std::string color;        // #RRGGBB
    std::string type;         // PLA, PETG, ...
    double      used_g { 0 };
    bool        used { true };
};

// Which toolhead prints which of the file's filaments, chosen the way the printer's own app and
// u1hub choose it: the nearest loaded colour (redmean distance), one toolhead per colour, a
// filament of the same colour shares that toolhead, and anything left over falls back to the first
// loaded toolhead. Type never decides - it only earns a warning on the phone.
// Returns one toolhead per filament (-1 for a filament the file does not use).
std::vector<int> auto_match(const std::vector<FileFilament>& filaments, const std::vector<Toolhead>& heads);

// The macros for one mapping, without the start (what a dry run reports).
std::string mapping_script(const std::vector<int>& mapping);

// Start `filename` with that mapping: the SET_PRINT_EXTRUDER_MAP / SET_PRINT_USED_EXTRUDERS /
// SET_PRINT_PREFERENCES macros, then SDCARD_PRINT_FILE, each a POST /printer/gcode/script.
// `sent` receives the scripts. mapping[i] = the toolhead for the file's filament i.
bool start_print_mapped(const Device& d, const std::string& filename, const std::vector<int>& mapping,
                        nlohmann::json& sent, std::string& error);

std::string base_url(const Device& d);

} // namespace SnapmakerLan
} // namespace GUI
} // namespace Slic3r
