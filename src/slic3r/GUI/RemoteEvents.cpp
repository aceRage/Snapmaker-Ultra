#include "RemoteEvents.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "RemoteControl.hpp"
#include "RemoteHub.hpp"
#include "SnapmakerLan.hpp"

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteEvents {

using nlohmann::json;

// How often the watcher really looks. The heartbeat ticks every second; a Bambu MachineObject is
// pushed to at a few Hz and a Moonraker printer is polled, so anything faster than this buys
// nothing and costs the GUI thread and the printers' web servers.
static const long long POLL_MS = 5000;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

static std::string hex8(int code)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", (unsigned) code);
    return buf;
}

// ------------------------------------------------------------ the rule ----

json Event::to_json(long instance_pid) const
{
    json j;
    j["instance"] = (long long) instance_pid;
    j["printer"]  = { { "id", printer_id }, { "name", printer_name }, { "kind", printer_kind } };
    j["kind"]     = kind;
    j["severity"] = severity;
    j["title"]    = title;
    j["text"]     = text;
    if (!code.empty()) j["code"] = code;
    if (!job.empty()) j["job"] = job;
    return j;
}

// The word the phone shows for a printer that is doing something.
static std::string job_phrase(const PrinterState& p)
{
    return p.job.empty() ? std::string() : (" \xE2\x80\xA2 " + p.job); // " • <job>"
}

static bool busy_state(const std::string& s) { return s == "printing" || s == "paused" || s == "preparing"; }

static Event make_event(const PrinterState& p, const char* kind, const char* severity, const std::string& title,
                        const std::string& text)
{
    Event e;
    e.printer_id   = p.id;
    e.printer_name = p.name;
    e.printer_kind = p.kind;
    e.kind         = kind;
    e.severity     = severity;
    e.title        = title;
    e.text         = text;
    e.job          = p.job;
    return e;
}

// The cooldown key: one printer, one kind, and the thing that makes this occurrence different from
// the last one - the error code where there is one, the job otherwise. So two starts of the same
// file in a minute are one event (a reconnect, a flap), while starting a second file is its own.
static std::string cooldown_key(const Event& e)
{
    return e.printer_id + "|" + e.kind + "|" + (e.code.empty() ? e.job : e.code);
}

std::vector<Event> step(Memory& mem, const Snapshot& now, long long cooldown_ms)
{
    std::vector<Event> out;
    for (const auto& kv : now.printers) {
        const PrinterState& cur = kv.second;
        auto                prev_it = mem.last.printers.find(kv.first);
        // A printer nobody can see the state of says nothing. Same for one that has only just
        // appeared, or that was offline / unwatched last time: the first watched snapshot seeds the
        // memory and nothing more, so starting the slicer next to a printer that is already halfway
        // through a job does not announce a start, and a reconnect does not replay one.
        if (!cur.watched || !cur.online || prev_it == mem.last.printers.end()) continue;
        const PrinterState& prev = prev_it->second;
        if (!prev.watched || !prev.online) continue;

        const std::string name = cur.name.empty() ? cur.id : cur.name;

        // A printer error, whatever the print state is doing: a new code, or a code where there was
        // none. Bambu's HMS text and Klipper's own message both arrive here as error_text.
        if (!cur.error_code.empty() && cur.error_code != prev.error_code) {
            Event e   = make_event(cur, "error", "error", name + " reported an error",
                                   cur.error_text.empty() ? (name + " reported error " + cur.error_code + ".")
                                                          : (name + ": " + cur.error_text));
            e.code    = cur.error_code;
            out.push_back(e);
        }

        if (cur.state != prev.state) {
            if (cur.state == "printing" && prev.state == "paused") {
                out.push_back(make_event(cur, "resumed", "info", name + " resumed", name + " picked the print up again" + job_phrase(cur) + "."));
            } else if (cur.state == "printing") {
                out.push_back(make_event(cur, "started", "info", name + " started printing",
                                         name + " started a print" + job_phrase(cur) + "."));
            } else if (cur.state == "paused") {
                // Stage 6 is the printer's own "Paused due to filament runout"; it is the one pause
                // worth waking somebody for, so it gets its own kind.
                const bool runout = cur.stage_curr == 6;
                Event      e      = make_event(cur, runout ? "runout" : "paused", "warning",
                                               runout ? (name + " ran out of filament") : (name + " paused"),
                                               runout ? (name + " paused because it ran out of filament" + job_phrase(cur) + ".")
                                                      : (name + " paused" + (cur.stage.empty() ? std::string() : " (" + cur.stage + ")") + job_phrase(cur) + "."));
                out.push_back(e);
            } else if (cur.state == "finished" && busy_state(prev.state)) {
                out.push_back(make_event(cur, "finished", "info", name + " finished", name + " finished the print" + job_phrase(prev) + "."));
            } else if (cur.state == "failed") {
                Event e = make_event(cur, "failed", "error", name + " failed",
                                     name + " stopped with a failure" + job_phrase(prev) +
                                         (cur.error_text.empty() ? "." : ": " + cur.error_text));
                e.code  = cur.error_code;
                out.push_back(e);
            } else if (cur.state == "cancelled" && busy_state(prev.state)) {
                out.push_back(make_event(cur, "cancelled", "warning", name + " was stopped", "The print on " + name + " was cancelled" + job_phrase(prev) + "."));
            }
        } else if (cur.state == "printing" && !cur.job.empty() && cur.job != prev.job) {
            // Straight from one job into the next without passing through an idle state.
            out.push_back(make_event(cur, "started", "info", name + " started printing",
                                     name + " started a print" + job_phrase(cur) + "."));
        }
    }

    // A printer that fails usually sets its error code in the same breath, and the failure event
    // already carries that code and its text. One thing that happened is one notification, so the
    // bare `error` is dropped where a `failed` for the same printer came out of the same poll.
    std::vector<Event> merged;
    for (const Event& e : out) {
        if (e.kind == "error") {
            bool failed_too = false;
            for (const Event& f : out)
                if (f.kind == "failed" && f.printer_id == e.printer_id) failed_too = true;
            if (failed_too) continue;
        }
        merged.push_back(e);
    }
    out.swap(merged);

    // The cooldown, last: an event that is really the same thing as one just sent is dropped, and
    // its key is not refreshed, so a genuinely long-running condition reappears once the window
    // has passed rather than never.
    std::vector<Event> kept;
    for (const Event& e : out) {
        const std::string key = cooldown_key(e);
        auto              it  = mem.last_emit.find(key);
        if (it != mem.last_emit.end() && now.at - it->second < cooldown_ms) continue;
        mem.last_emit[key] = now.at;
        kept.push_back(e);
    }
    // Keys of printers that are gone would otherwise accumulate for the life of the process.
    if (mem.last_emit.size() > 256) mem.last_emit.clear();
    mem.last = now;
    return kept;
}

// ------------------------------------------------------- snapshot: JSON ----

// Both directions of PrinterState <-> JSON, so the debug route can drive `step` with snapshots a
// test wrote by hand and read back what came out.
static PrinterState state_of_json(const json& j)
{
    PrinterState p;
    p.id         = j.value("id", std::string());
    p.name       = j.value("name", std::string());
    p.kind       = j.value("kind", std::string("bambu"));
    p.watched    = j.value("watched", true);
    p.online     = j.value("online", true);
    p.state      = j.value("state", std::string());
    p.raw_state  = j.value("raw_state", std::string());
    p.job        = j.value("job", std::string());
    p.stage      = j.value("stage", std::string());
    p.stage_curr = j.value("stage_curr", -1);
    p.error_code = j.value("error_code", std::string());
    p.error_text = j.value("error_text", std::string());
    return p;
}

json replay(const json& in)
{
    Memory          mem;
    const long long cooldown = in.value("cooldown_ms", (long long) 180000);
    json            out;
    out["steps"] = json::array();
    if (!in.contains("snapshots") || !in["snapshots"].is_array()) {
        out["error"] = "snapshots must be an array";
        return out;
    }
    for (const json& s : in["snapshots"]) {
        Snapshot snap;
        snap.at = s.value("at", (long long) 0);
        for (const json& p : s.value("printers", json::array())) {
            PrinterState ps = state_of_json(p);
            if (!ps.id.empty()) snap.printers[ps.id] = ps;
        }
        json step_out;
        step_out["at"]     = snap.at;
        step_out["events"] = json::array();
        for (const Event& e : step(mem, snap, cooldown)) step_out["events"].push_back(e.to_json(0));
        out["steps"].push_back(step_out);
    }
    return out;
}

// ------------------------------------------------- snapshot: the printers ----

// Bambu's print_status, in the words the rule uses.
static std::string bambu_state(const std::string& s)
{
    if (s == "RUNNING") return "printing";
    if (s == "PAUSE") return "paused";
    if (s == "FINISH") return "finished";
    if (s == "FAILED") return "failed";
    if (s == "SLICING" || s == "PREPARE") return "preparing";
    if (s.empty()) return "";
    return "idle"; // IDLE, INIT and anything the printer invents later
}

// Klipper's print_stats.state, likewise.
static std::string klipper_state(const std::string& s)
{
    if (s == "printing") return "printing";
    if (s == "paused") return "paused";
    if (s == "complete") return "finished";
    if (s == "error") return "failed";
    if (s == "cancelled") return "cancelled";
    if (s == "standby") return "idle";
    if (s.empty()) return "";
    return "idle";
}

static std::string print_error_message(int code)
{
    wxString msg;
    if (HMSQuery* q = wxGetApp().get_hms_query(); q && q->query_print_error_msg(code, msg)) return msg.ToUTF8().data();
    return std::string();
}

// GUI thread: reading a MachineObject is field access, no network and no locks - which is why the
// Bambu half of the snapshot is taken on the heartbeat itself.
static void snapshot_bambu(Snapshot& s)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return;
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all) {
        MachineObject* m = kv.second;
        if (!m) continue;
        PrinterState p;
        p.id   = m->dev_id;
        p.name = m->dev_name;
        p.kind = "bambu";
        // In LAN mode exactly one printer is connected at a time (DeviceManager::set_selected_machine
        // disconnects the previous one). The others are a discovery entry and nothing else: their
        // print_status is stale or empty, so they are listed here but never watched.
        p.watched   = m->is_connected();
        p.online    = m->is_online();
        p.raw_state = m->print_status;
        p.state     = bambu_state(m->print_status);
        p.job       = m->subtask_name;
        p.stage_curr = m->stage_curr;
        try {
            p.stage = m->get_curr_stage().ToUTF8().data();
        } catch (...) {}
        if (m->print_error != 0) {
            p.error_code = hex8(m->print_error);
            p.error_text = print_error_message(m->print_error);
        } else {
            // No print error: the worst thing HMS is reporting, if it is serious enough to be worth
            // a notification. HMS_COMMON and HMS_INFO are the printer's chatter and stay off.
            for (HMSItem& item : m->hms_list) {
                if (item.msg_level != HMS_FATAL && item.msg_level != HMS_SERIOUS) continue;
                p.error_code = item.get_long_error_code();
                if (HMSQuery* q = wxGetApp().get_hms_query())
                    p.error_text = q->query_hms_msg(p.error_code).ToUTF8().data();
                break;
            }
        }
        s.printers[p.id] = p;
    }
}

// Worker thread: the LAN Snapmakers, over the Moonraker HTTP API they serve themselves. This is
// the same cached probe /api/printers uses (four-second TTL), so a phone polling the Devices tab
// and the watcher share the answer instead of asking twice.
static void snapshot_snapmaker(Snapshot& s)
{
    for (const SnapmakerLan::Device& d : SnapmakerLan::devices()) {
        SnapmakerLan::Status st;
        try {
            st = SnapmakerLan::status(d);
        } catch (...) {
            continue;
        }
        PrinterState p;
        p.id        = "sm:" + d.id;
        p.name      = d.name.empty() ? d.ip : d.name;
        p.kind      = "snapmaker";
        p.online    = st.online;
        // A printer that wants a login answers nothing useful; do not pretend to watch it.
        p.watched   = st.online && !st.login_required;
        p.raw_state = st.state;
        p.state     = klipper_state(st.state);
        p.job       = st.filename;
        if (st.state == "error") {
            p.error_code = "error";
            p.error_text = st.message;
        }
        s.printers[p.id] = p;
    }
}

// Worker thread: the printer preset's print host and the Snapmaker connected on the PC's Device
// tab, through the probe /api/printers already runs against them (RemoteControl::describe_hosts,
// which caches and backs off for half a minute on an address that is not a Moonraker printer).
static void snapshot_hosts(Snapshot& s, const std::vector<RemoteControl::HostTarget>& targets_in)
{
    if (targets_in.empty()) return;
    // The Snapmaker the PC's Device tab is connected to is usually the same machine as one of the
    // LAN cards above (merge_app_devices puts it in that list). One machine must produce one event,
    // so an address the LAN half already covered is dropped here rather than watched twice.
    std::vector<RemoteControl::HostTarget> targets;
    std::vector<std::string>               lan;
    try {
        for (const SnapmakerLan::Device& d : SnapmakerLan::devices()) lan.push_back(SnapmakerLan::base_url(d));
    } catch (...) {}
    for (const RemoteControl::HostTarget& t : targets_in)
        if (std::find(lan.begin(), lan.end(), t.base) == lan.end()) targets.push_back(t);
    if (targets.empty()) return;
    json printers = json::array();
    for (const RemoteControl::HostTarget& t : targets) printers.push_back(json { { "id", t.id } });
    try {
        RemoteControl::describe_hosts(targets, printers);
    } catch (...) {
        return;
    }
    for (size_t i = 0; i < targets.size() && i < printers.size(); ++i) {
        const json&  e = printers[i];
        PrinterState p;
        p.id   = targets[i].id;
        p.name = targets[i].base.empty() ? targets[i].id : targets[i].base;
        p.kind = targets[i].id == "connect" ? "connect" : "printhost";
        p.raw_state = e.value("print_status", std::string());
        p.state     = klipper_state(p.raw_state);
        p.job       = e.value("stage", std::string()); // fill_from_print_stats puts the file name there
        // describe_hosts leaves print_status off entirely for an address that did not answer as a
        // Moonraker printer, and that is exactly the case where nothing may be inferred.
        p.online    = e.contains("print_status");
        p.watched   = p.online;
        if (e.contains("print_error") && e["print_error"].is_object()) {
            p.error_code = e["print_error"].value("code", std::string());
            p.error_text = e["print_error"].value("message", std::string());
        }
        s.printers[p.id] = p;
    }
}

// --------------------------------------------------------- the watcher ----

static std::mutex       s_mutex;
static Memory           s_memory;          // worker thread only, guarded for the debug path
static std::deque<json> s_recent;          // this instance's own ring, for GET /api/events
static int              s_next_local = 1;
static std::atomic<bool> s_busy { false }; // one poll in flight at a time
static std::atomic<bool> s_stop { false };
static long long        s_last_poll = 0;

static void remember(const json& e)
{
    std::lock_guard<std::mutex> lock(s_mutex);
    s_recent.push_back(e);
    while (s_recent.size() > 50) s_recent.pop_front();
}

json recent(int since)
{
    json out;
    out["events"] = json::array();
    int last      = 0;
    std::lock_guard<std::mutex> lock(s_mutex);
    for (const json& e : s_recent) {
        const int id = e.value("local_id", 0);
        last         = std::max(last, id);
        if (id > since) out["events"].push_back(e);
    }
    out["last_id"] = last;
    return out;
}

void stop() { s_stop = true; }

void heartbeat()
{
    if (s_stop || s_busy.load()) return;
    const long long at = now_ms();
    if (s_last_poll != 0 && at - s_last_poll < POLL_MS) return;
    s_last_poll = at;

    // The Bambu half here and now: this is the GUI thread, where MachineObject lives. The preset's
    // print host is read here too - the preset bundle is GUI-thread state as well.
    auto snap    = std::make_shared<Snapshot>();
    auto targets = std::make_shared<std::vector<RemoteControl::HostTarget>>();
    snap->at     = at;
    try {
        snapshot_bambu(*snap);
        RemoteControl::list_host_targets(*targets);
    } catch (...) {}

    // Everything that needs the network, the diff and the POST to the hub go to a worker: none of
    // it may run on the GUI thread, and the whole point of the watcher is that nobody waits on it.
    s_busy = true;
    std::thread([snap, targets]() {
        try {
            snapshot_snapmaker(*snap);
            snapshot_hosts(*snap, *targets);
            std::vector<Event> events;
            {
                std::lock_guard<std::mutex> lock(s_mutex);
                events = step(s_memory, *snap);
            }
            const long pid = (long) wxGetProcessId();
            for (const Event& e : events) {
                // What goes to the hub is the event without id and time: the hub assigns both, so
                // ids stay in one increasing sequence however many instances are running.
                const std::string body = e.to_json(pid).dump();
                json              mine = e.to_json(pid);
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    mine["local_id"] = s_next_local++;
                }
                mine["time"] = snap->at;
                remember(mine);
                BOOST_LOG_TRIVIAL(info) << "RemoteEvents: " << e.kind << " on " << e.printer_id << ": " << e.title;
                // Fire and forget: with no hub running this is a failed connect to a closed port
                // and the event stays in this instance's own ring. Nothing waits on delivery.
                RemoteHub::post_event(body);
            }
        } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(debug) << "RemoteEvents: poll failed: " << ex.what();
        } catch (...) {}
        s_busy = false;
    }).detach();
}

} // namespace RemoteEvents
} // namespace GUI
} // namespace Slic3r
