#pragma once

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "slic3r/Utils/PrintHost.hpp"

namespace Slic3r {
class MachineObject;
namespace GUI {

// Pausing, resuming and stopping a running print for the phone / agent API (RemoteAccess), without
// any of the desktop's dialogs. It sends what StatusPanel's own Pause / Resume / Stop buttons send:
// MachineObject::command_task_pause / _resume / _abort for a Bambu printer, gated by the same
// can_pause() / can_resume() / can_abort() predicates, and Moonraker's print controls for a
// Snapmaker. Nothing here can start a print - that stays in RemoteSend, behind its own confirm.
namespace RemoteControl {

struct Request
{
    std::string printer;           // a Bambu dev_id, "host" (the printer preset's print host) or
                                   // "connect" (the Snapmaker connected on the PC's Device tab)
    std::string action;            // "pause" | "resume" | "stop"
    bool        confirm { false }; // stop needs it: the phone asks the person first
    bool        dry_run { false }; // work everything out but send nothing (SNORCA_SEND_DRYRUN=1 forces it)
};

// Everything prepare() worked out on the GUI thread; run() only sends the command.
struct Prepared
{
    std::string kind;       // bambu | printhost | connect
    std::string action;     // pause | resume | stop
    bool        dry_run { false };
    std::string printer_id, printer_name;
    // Bambu: the MachineObject command the desktop's own buttons call.
    std::string command;            // pause | resume | stop (what goes into the MQTT payload)
    std::string call;               // command_task_pause | command_task_resume | command_task_abort
    std::string status_before;      // print_status when the command was composed
    int         print_error_before { 0 };
    // Moonraker (a Snapmaker over the LAN, or any Moonraker print host): one POST.
    std::string url;                // http://<printer>/printer/print/{pause,resume,cancel}
    std::string moonraker_method;   // printer.print.pause | .resume | .cancel (the MQTT name)
    std::shared_ptr<PrintHost> host; // only for "connect": the live MQTT host, used if the POST fails
};

struct Sink
{
    std::function<void(int percent, const std::string& text)>                            progress;
    std::function<void(bool ok, const std::string& error, const nlohmann::json& result)> done;
};

// GUI thread. Validates the request against the printer's own state and composes the command.
// {200, ""} with `out` filled, or an HTTP status and an error text for the phone.
std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out);

// Worker thread. Sends the command (or, on a dry run, does not) and watches what the printer does;
// reports through the sink and always ends with sink.done.
void run(std::shared_ptr<Prepared> p, Sink sink);

// GUI thread. What GET /api/printers adds for the control buttons of one Bambu printer:
// can_pause / can_resume / can_stop, print_status, the current stage and the print error.
void describe_bambu(MachineObject* m, nlohmann::json& p);

// A print-host entry of /api/printers that speaks Moonraker, and where to reach it. Collected on
// the GUI thread (the preset and the connected host live there), probed off it.
struct HostTarget
{
    std::string id;   // "host" | "connect"
    std::string base; // http://<address>, ready for /printer/...
};
void list_host_targets(std::vector<HostTarget>& out);

// Request thread, never the GUI one: ask each Moonraker printer what it is doing and fill the same
// control fields into its entry of the printers array. A printer that does not answer keeps its
// buttons off. Safe to call with an empty list.
void describe_hosts(const std::vector<HostTarget>& targets, nlohmann::json& printers);

} // namespace RemoteControl
} // namespace GUI
} // namespace Slic3r
