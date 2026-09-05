#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <nlohmann/json.hpp>

#include "GcodeArchive.hpp"
#include "SnapmakerLan.hpp"
#include "slic3r/Utils/PrintHost.hpp"
#include "slic3r/Utils/bambu_networking.hpp"

namespace Slic3r {
class MachineObject;
namespace GUI {

// Sending a sliced plate to a printer for the phone / agent API (RemoteAccess), without any of the
// desktop's dialogs. It composes exactly what SelectMachineDialog + PrintJob (upload and print),
// SendToPrinterDialog + SendJob (upload only) and the print-host / Snapmaker branch of
// Plater::send_gcode_legacy would send, so a phone send looks like a desktop send to the printer.
namespace RemoteSend {

struct Request
{
    int         plate { -1 };
    std::string printer;           // a Bambu dev_id, "host" (the printer preset's print host) or
                                   // "connect" (the Snapmaker connected on the PC's Device tab)
    std::string mode;              // "upload" | "print"
    bool        confirm { false }; // mode=print needs it: the phone asks the person first
    bool        force { false };   // send although the printer's model differs from the sliced profile
    bool        dry_run { false }; // everything but the upload / print command (SNORCA_SEND_DRYRUN=1 forces it)
    // Bambu options; -1 = the desktop's remembered choice (AppConfig section "print").
    int         bed_leveling { -1 }, flow_cali { -1 }, vibration_cali { -1 }, timelapse { -1 }, use_ams { -1 };
    std::string name;              // print host: the file name on the printer (default: the export name)
    // Snapmaker over the LAN: which toolhead prints which of the file's filaments,
    // "<filament>:<toolhead>,..." (0-based). Empty = the auto-match the printer's own app makes.
    std::string mapping;
};

// Everything prepare() worked out on the GUI thread; run() only performs the transfer.
struct Prepared
{
    std::string kind;              // bambu | printhost | connect
    std::string mode;              // upload | print
    int         plate { -1 };
    bool        dry_run { false };
    std::string printer_id, printer_name;
    // Bambu (the network plugin)
    BBL::PrintParams params {};
    std::string      call;                          // the NetworkAgent function the desktop would call
    bool             verify_access_code { false };  // PrintJob's tiny upload that proves IP + access code first
    bool             lan_fallback_to_cloud { false };
    int              print_error_before { 0 };      // the printer's error code before the send
    // Print host (Moonraker / OctoPrint / … and the connected Snapmaker)
    std::shared_ptr<PrintHost> host;
    PrintHostUpload  upload {};
    bool             two_step { false };            // upload with print=false, then printer.print.start over MQTT
    // Snapmaker over the LAN (Moonraker HTTP): no host object, just the printer and the mapping.
    SnapmakerLan::Device                    lan {};
    std::string                             lan_filename;
    std::vector<SnapmakerLan::FileFilament> file_filaments;
    std::vector<int>                        mapping;      // toolhead per file filament, -1 = unused
    std::vector<SnapmakerLan::Toolhead>     toolheads;
    // Ultra: what the G-code archive records about this send, gathered on the GUI thread by
    // prepare() so run() only has to copy the file it uploaded.
    GcodeArchive::Meta archive_meta;
};

struct Sink
{
    std::function<void(int percent, const std::string& text)>                              progress;
    std::function<void(bool ok, const std::string& error, const nlohmann::json& result)>   done;
};

// GUI thread, first. A Bambu printer other than the selected one is selected (and so connected)
// like picking it in the dialog's combo box; `wait` then says the caller should give its status
// (SD card, connection) a moment to arrive before prepare(): the GUI thread must not block for it.
// Anything else returns {200, ""} with wait = false. Errors as prepare().
std::pair<int, std::string> preselect(const Request& req, bool& wait);
// GUI thread. True once the printer's connection and status have arrived (or it is not a Bambu one).
bool printer_ready(const std::string& printer);

// GUI thread. Validates the request against the plater and the printer, exports the plate file the
// way the desktop does and composes the parameters. {200, ""} with `out` filled, or an HTTP status
// and an error text for the phone.
std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out);

// Worker thread. The upload / print start (or the dry run); reports through the sink and always
// ends with sink.done.
void run(std::shared_ptr<Prepared> p, Sink sink);

// GUI thread. The file name the desktop's own export gives a plate ("<project>_plate_2.gcode"),
// for any plate, without making it the current one on the PC. `plate` out of range = the current
// plate; an empty extension gives the bare name.
std::string export_name_for(int plate, const std::string& extension);

// GUI thread. What /api/printers adds for the send UI: a Bambu machine's send capabilities and
// option defaults, and the print-host / connected-Snapmaker entries. `plate` says which plate the
// `upload_name` defaults are for (-1 = the current one).
void describe_bambu(MachineObject* m, nlohmann::json& p);
void list_hosts(nlohmann::json& printers, int plate = -1);

} // namespace RemoteSend
} // namespace GUI
} // namespace Slic3r
