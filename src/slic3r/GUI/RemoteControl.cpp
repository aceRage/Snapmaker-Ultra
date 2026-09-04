#include "RemoteControl.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "HMS.hpp"
#include "SnapmakerLan.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "slic3r/Utils/Http.hpp"

#include <boost/log/trivial.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#include <wx/utils.h>

namespace Slic3r {
namespace GUI {
namespace RemoteControl {

using nlohmann::json;

// ---------------------------------------------------------------- helpers ----

// From the worker thread: run fn on the GUI thread and wait for it (bounded), as RemoteSend does.
static bool on_main(std::function<void()> fn, int timeout_ms = 10000)
{
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        try { fn(); } catch (...) {}
        done->set_value();
    });
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

static bool env_flag(const char* name)
{
    wxString v;
    return wxGetEnv(name, &v) && v == "1";
}

static MachineObject* find_machine(DeviceManager* dm, const std::string& id)
{
    std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
    for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
    for (const auto& kv : all)
        if (kv.second && kv.second->dev_id == id) return kv.second;
    return nullptr;
}

static std::string error_code_text(int code)
{
    char buf[16];
    std::snprintf(buf, sizeof buf, "%08X", (unsigned) code);
    return buf;
}

// The printer's own words for a print error, when the HMS table has them (StatusPanel shows the same).
static std::string print_error_message(int code)
{
    wxString msg;
    if (HMSQuery* q = wxGetApp().get_hms_query(); q && q->query_print_error_msg(code, msg)) return msg.ToUTF8().data();
    return std::string();
}

// A print host address turned into the base URL of its Moonraker HTTP API. This is
// Moonraker::make_url's own rule (MoonRaker.cpp), which is protected: an address without a scheme
// becomes http://, and the MQTT ports the Device tab stores (1884 plain, 8883 TLS) are dropped so
// the printer's HTTP API on port 80 is addressed instead.
static std::string moonraker_base(const std::string& host)
{
    std::string h = host;
    while (!h.empty() && (h.back() == '/' || h.back() == ' ')) h.pop_back();
    if (h.empty()) return h;
    if (h.compare(0, 7, "http://") == 0 || h.compare(0, 8, "https://") == 0) return h;
    const size_t mqtt_plain = h.find(":1884"), mqtt_tls = h.find(":8883");
    if (mqtt_plain != std::string::npos) h = h.substr(0, mqtt_plain);
    else if (mqtt_tls != std::string::npos) h = h.substr(0, mqtt_tls);
    return "http://" + h;
}

// One Moonraker HTTP call. Short timeouts: /api/printers probes with this on every poll and a
// printer that is off must not hold the answer up. Any thread but the GUI one.
static bool moonraker_http(const std::string& url, bool post, std::string& body, std::string& error, int timeout_s)
{
    bool ok = false;
    auto http = post ? Http::post(url) : Http::get(url);
    // An empty JSON object rather than no body at all: CURLOPT_POST without post fields would fall
    // through to the wrapper's file-upload read callback. Moonraker ignores the body of these three.
    if (post) http.header("Content-Type", "application/json").set_post_body(std::string("{}"));
    http.timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .size_limit(256 * 1024)
        .on_error([&](std::string reply, std::string err, unsigned status) {
            error = err.empty() ? ("HTTP " + std::to_string(status)) : err;
            if (!reply.empty()) body = reply;
        })
        .on_complete([&](std::string reply, unsigned) {
            body = reply;
            ok   = true;
        })
        .perform_sync();
    // A 3xx answers through neither callback (Http::priv::http_perform); say something either way.
    if (!ok && error.empty()) error = "no answer";
    return ok;
}

static json parse_or_raw(const std::string& body)
{
    try {
        return json::parse(body);
    } catch (...) {
        return json(body.substr(0, 400));
    }
}

// What an address answered the last time it was asked. prepare() runs on the GUI thread and must
// not touch the network, so it reads what the last /api/printers probe (describe_hosts, every 5 s
// while the phone's Devices tab is open) found there: whether it is a Moonraker printer at all, and
// what it said it was doing - the print host's equivalent of a Bambu printer's can_* predicates.
struct Probe
{
    bool        moonraker { false };
    std::string state; // Klipper print_stats.state: standby | printing | paused | complete | ...
    long long   when { 0 };
};
static std::mutex                   s_probe_mutex;
static std::map<std::string, Probe> s_probes;

static long long now_ms()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

static void remember_probe(const std::string& base, bool moonraker, const std::string& state)
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    s_probes[base] = Probe { moonraker, state, now_ms() };
}

// An address that did not answer is not asked again for half a minute: /api/printers is polled
// every 5 s and must not wait on a printer that is off, or on a print host that is not a Klipper
// one at all (an OctoPrint address never will be).
static bool ask_again(const std::string& base)
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    auto it = s_probes.find(base);
    return it == s_probes.end() || it->second.moonraker || now_ms() - it->second.when > 30000;
}

// tri-state: 1 = a Moonraker printer, 0 = it answered something else, -1 = never asked.
static int probed(const std::string& base, std::string* state = nullptr)
{
    std::lock_guard<std::mutex> lock(s_probe_mutex);
    auto it = s_probes.find(base);
    if (it == s_probes.end()) return -1;
    if (state) *state = it->second.state;
    return it->second.moonraker ? 1 : 0;
}

// The same rule fill_from_print_stats renders as can_pause / can_resume / can_stop.
static bool klipper_allows(const std::string& action, const std::string& state)
{
    if (action == "pause")  return state == "printing";
    if (action == "resume") return state == "paused";
    return state == "printing" || state == "paused";
}

// ---------------------------------------------------------------- prepare ----

// The three actions, in every spelling this feature needs: what the phone asks for, the
// MachineObject call the desktop's buttons make, and Moonraker's own endpoint and method name.
struct ActionNames
{
    const char* action;
    const char* bambu_call;
    const char* bambu_command;
    const char* moonraker_path;
    const char* moonraker_method;
};
static const ActionNames k_actions[] = {
    { "pause",  "command_task_pause",  "pause",  "printer/print/pause",  "printer.print.pause" },
    { "resume", "command_task_resume", "resume", "printer/print/resume", "printer.print.resume" },
    // "stop" is the phone's word for what the desktop calls Cancel print (an abort); Moonraker's
    // own name for it is cancel.
    { "stop",   "command_task_abort",  "stop",   "printer/print/cancel", "printer.print.cancel" },
};

static const ActionNames* action_names(const std::string& action)
{
    for (const ActionNames& a : k_actions)
        if (action == a.action) return &a;
    return nullptr;
}

static std::pair<int, std::string> prepare_bambu(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                 std::shared_ptr<Prepared>& out)
{
    DeviceManager* dm = wxGetApp().getDeviceManager();
    if (!dm) return { 503, "no device manager" };
    MachineObject* obj = find_machine(dm, req.printer);
    if (!obj) return { 404, "no such printer: " + req.printer };
    if (!obj->is_online()) return { 409, obj->dev_name + " is offline" };
    // The same preconditions a send has: without the access code (LAN printers) or a live
    // connection the printer never sees the command, and its reported state is stale.
    if (obj->is_lan_mode_printer() && !obj->has_access_right())
        return { 409, obj->dev_name + " needs its access code entered on the PC first" };
    if (!obj->is_connected())
        return { 409, obj->dev_name + " is not connected; open it on the PC's Device tab once, or pick it in a send" };
    // The desktop's own buttons are enabled by exactly these three predicates (StatusPanel).
    const bool allowed = req.action == "pause" ? obj->can_pause() : req.action == "resume" ? obj->can_resume() : obj->can_abort();
    if (!allowed) {
        const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
        return { 409, obj->dev_name + " cannot be " + what + " right now (it reports " +
                          (obj->print_status.empty() ? "no print state" : obj->print_status) + ")" };
    }
    p->kind               = "bambu";
    p->printer_name       = obj->dev_name;
    p->call               = a.bambu_call;
    p->command            = a.bambu_command;
    p->status_before      = obj->print_status;
    p->print_error_before = obj->print_error;
    out                   = p;
    return { 200, "" };
}

static std::pair<int, std::string> prepare_host(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                std::shared_ptr<Prepared>& out)
{
    PresetBundle*              bundle = wxGetApp().preset_bundle;
    std::shared_ptr<PrintHost> host;
    std::string                address;
    if (req.printer == "connect") {
        wxGetApp().get_connect_host(host);
        if (!host) return { 409, "no Snapmaker printer is connected on the PC's Device tab" };
        p->kind         = "connect";
        p->printer_name = "Snapmaker " + host->get_host();
        address         = host->get_host();
        p->host         = host; // the live MQTT socket, in case the printer's HTTP API refuses
    } else {
        if (!bundle) return { 503, "no preset bundle" };
        DynamicPrintConfig& cfg = bundle->printers.get_edited_preset().config;
        if (bundle->use_bbl_network()) return { 409, "the current printer preset sends through the Bambu network; pick that printer by its id" };
        address = cfg.opt_string("print_host");
        if (address.empty()) return { 409, "the printer preset has no print host address" };
        std::unique_ptr<PrintHost> h(PrintHost::get_print_host(&cfg, false));
        p->kind         = "printhost";
        p->printer_name = (h ? std::string(h->get_name()) + " " : std::string()) + address;
    }
    const std::string base = moonraker_base(address);
    if (base.empty()) return { 409, "this printer has no address" };
    // These three controls are Moonraker's; the printer itself decides whether it speaks it. The
    // last status probe (describe_hosts, from every /api/printers) is the answer when there is one
    // - the GUI thread cannot ask now. Never asked yet: let the command find out. A printer the PC
    // holds an MQTT socket to is never refused for the first reason: that socket is run()'s fallback.
    std::string state;
    const int   seen = probed(base, &state);
    if (!p->host && seen == 0)
        return { 409, p->printer_name + " does not answer as a Klipper / Moonraker printer, so it cannot be paused, resumed or stopped from here" };
    if (seen == 1 && !state.empty() && !klipper_allows(req.action, state)) {
        const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
        return { 409, p->printer_name + " cannot be " + what + " right now (it reports " + state + ")" };
    }
    p->url              = base + "/" + a.moonraker_path;
    p->moonraker_method = a.moonraker_method;
    out                 = p;
    return { 200, "" };
}

// A Snapmaker the LAN list knows (printer sm:<id>): Moonraker over the printer's own HTTP API,
// gated by what the list's last probe saw. That probe runs off the GUI thread whenever the phone
// lists printers, so this only reads it; a printer never probed is asked nothing here and the
// command itself finds out. No MQTT socket to fall back on: the LAN path never opens one.
static std::pair<int, std::string> prepare_snapmaker_lan(const Request& req, const ActionNames& a, std::shared_ptr<Prepared> p,
                                                         std::shared_ptr<Prepared>& out)
{
    SnapmakerLan::Device d;
    if (!SnapmakerLan::find(req.printer.substr(3), d)) return { 404, "no such printer: " + req.printer };
    p->kind         = "snapmaker";
    p->printer_name = d.name.empty() ? d.ip : d.name;
    SnapmakerLan::Status s;
    if (SnapmakerLan::cached_status(d, s)) {
        if (!s.online) return { 409, p->printer_name + " is offline" };
        if (s.login_required) return { 409, p->printer_name + " requires a login for its LAN API, so it cannot be controlled from here" };
        if (!s.state.empty() && !klipper_allows(req.action, s.state)) {
            const std::string what = req.action == "pause" ? "paused" : req.action == "resume" ? "resumed" : "stopped";
            return { 409, p->printer_name + " cannot be " + what + " right now (it reports " + s.state + ")" };
        }
    }
    p->url              = SnapmakerLan::base_url(d) + "/" + a.moonraker_path;
    p->moonraker_method = a.moonraker_method;
    out                 = p;
    return { 200, "" };
}

std::pair<int, std::string> prepare(const Request& req, std::shared_ptr<Prepared>& out)
{
    const ActionNames* a = action_names(req.action);
    if (!a) return { 400, "action must be pause, resume or stop" };
    // Stopping a print throws the print away; pause and resume are reversible and the desktop does
    // not confirm them either (StatusPanel::on_subtask_pause_resume).
    if (req.action == "stop" && !req.confirm) return { 400, "stopping a print needs confirm=1" };
    if (req.printer.empty()) return { 400, "printer is required" };

    auto p     = std::make_shared<Prepared>();
    p->action  = req.action;
    p->dry_run = req.dry_run || env_flag("SNORCA_SEND_DRYRUN");
    p->printer_id = req.printer;
    if (req.printer == "host" || req.printer == "connect") return prepare_host(req, *a, p, out);
    if (req.printer.compare(0, 3, "sm:") == 0) return prepare_snapmaker_lan(req, *a, p, out);
    return prepare_bambu(req, *a, p, out);
}

// -------------------------------------------------------------------- run ----

// What the printer reports after a control command, watched for a few seconds so the phone learns
// whether it took (an H2-series printer without LAN-only mode + Developer Mode refuses third-party
// commands with "command verification failed", exactly as it does for a print).
static void watch_bambu(std::shared_ptr<Prepared> p, json& result)
{
    struct Watch { std::mutex m; std::string state { "unknown" }, status, err_text; int err { 0 }; };
    auto w = std::make_shared<Watch>(); // shared: a timed-out GUI call may still run after this loop
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        on_main([w, p]() {
            DeviceManager* dm = wxGetApp().getDeviceManager();
            if (!dm) return;
            MachineObject* obj = find_machine(dm, p->printer_id);
            if (!obj) return;
            std::lock_guard<std::mutex> lock(w->m);
            if (w->state != "unknown") return;
            w->status = obj->print_status;
            if (obj->print_error != 0 && obj->print_error != p->print_error_before) {
                w->err      = obj->print_error;
                w->state    = "error";
                w->err_text = print_error_message(w->err);
            } else if (p->action == "pause" && obj->print_status == "PAUSE") {
                w->state = "paused";
            } else if (p->action == "resume" && obj->print_status == "RUNNING") {
                w->state = "printing";
            } else if (p->action == "stop" && !MachineObject::is_in_printing_status(obj->print_status)) {
                w->state = "stopped";
            }
        }, 3000);
        std::lock_guard<std::mutex> lock(w->m);
        if (w->state != "unknown") break;
    }
    std::lock_guard<std::mutex> lock(w->m);
    result["printer_state"]  = w->state;
    result["status_after"]   = w->status;
    if (w->state == "error")
        result["printer_error"] = { { "code", error_code_text(w->err) }, { "message", w->err_text } };
}

static void run_bambu(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]          = "bambu";
    result["action"]        = p->action;
    result["printer"]       = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["call"]          = p->call;
    result["command"]       = p->command;
    result["status_before"] = p->status_before;
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    // The command itself is one MQTT publish; it is sent from the GUI thread because the
    // MachineObject lives there, exactly as the desktop's own buttons do.
    auto rc = std::make_shared<int>(-1);
    const bool ran = on_main([rc, p]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm) return;
        MachineObject* obj = find_machine(dm, p->printer_id);
        if (!obj) return;
        if (p->action == "pause")       *rc = obj->command_task_pause();
        else if (p->action == "resume") *rc = obj->command_task_resume();
        else                            *rc = obj->command_task_abort();
    }, 10000);
    result["result_code"] = *rc;
    if (!ran) { sink.done(false, "the PC did not send the command in time", result); return; }
    if (*rc != 0) { sink.done(false, p->printer_name + " did not accept the command (code " + std::to_string(*rc) + ")", result); return; }

    sink.progress(60, p->action == "pause" ? "waiting for the printer to pause" : p->action == "resume" ? "waiting for the printer to resume" : "waiting for the printer to stop");
    watch_bambu(p, result);
    if (result["printer_state"] == "error") {
        const std::string code = result["printer_error"]["code"];
        const std::string msg  = result["printer_error"]["message"];
        sink.done(false, "the printer refused the command (error " + code + (msg.empty() ? "" : ": " + msg) + ")", result);
        return;
    }
    sink.done(true, "", result);
}

// A Snapmaker over Moonraker. The HTTP API the printer serves is the path both a connected printer
// and one the PC has never connected to can use, so it is tried first; a printer reached only over
// the Device tab's MQTT socket falls back to that socket's own printer.print.* method.
static void run_host(std::shared_ptr<Prepared> p, Sink& sink)
{
    json result;
    result["kind"]      = p->kind;
    result["action"]    = p->action;
    result["printer"]   = { { "id", p->printer_id }, { "name", p->printer_name } };
    result["url"]       = p->url;
    result["method"]    = p->moonraker_method;
    result["transport"] = "http";
    if (p->dry_run) {
        result["dry_run"] = true;
        sink.progress(99, "dry run: nothing was sent");
        sink.done(true, "", result);
        return;
    }
    sink.progress(40, "sending " + p->action + " to the printer");
    std::string body, error;
    if (moonraker_http(p->url, true, body, error, 15)) {
        result["reply"] = parse_or_raw(body);
        sink.done(true, "", result);
        return;
    }
    result["http_error"] = error;
    if (!body.empty()) result["reply"] = parse_or_raw(body);
    if (!p->host) { sink.done(false, p->printer_name + " refused the command: " + error, result); return; }

    // The MQTT fallback: the socket the PC's Device tab opened, the way its own page pauses.
    sink.progress(70, "the printer's web API refused; trying the connection the PC holds");
    result["transport"] = "mqtt";
    auto reply = std::make_shared<std::promise<json>>();
    auto once  = std::make_shared<std::atomic<bool>>(false);
    auto fut   = reply->get_future();
    auto cb    = [reply, once](const json& r) { if (!once->exchange(true)) reply->set_value(r); };
    if (p->action == "pause")       p->host->async_pause_print_job(cb);
    else if (p->action == "resume") p->host->async_resume_print_job(cb);
    else                            p->host->async_cancel_print_job(cb);
    if (fut.wait_for(std::chrono::seconds(15)) != std::future_status::ready) {
        sink.done(false, p->printer_name + " did not answer the " + p->action + " (its web API said: " + error + ")", result);
        return;
    }
    const json r    = fut.get();
    result["reply"] = r;
    if (r.is_null() || (r.is_object() && r.contains("error"))) {
        sink.done(false, "the printer refused the " + p->action + ": " + (r.is_null() ? std::string("no reply") : r["error"].dump()), result);
        return;
    }
    sink.done(true, "", result);
}

void run(std::shared_ptr<Prepared> p, Sink sink)
{
    try {
        if (p->kind == "bambu") run_bambu(p, sink);
        else                    run_host(p, sink);
    } catch (const std::exception& e) {
        sink.done(false, std::string("the command failed: ") + e.what(), json::object());
    } catch (...) {
        sink.done(false, "the command failed", json::object());
    }
}

// ----------------------------------------------------------- /api/printers ----

void describe_bambu(MachineObject* m, json& p)
{
    // The desktop's own three predicates, so the phone's buttons light up exactly as its do.
    p["can_pause"]  = m->can_pause();
    p["can_resume"] = m->can_resume();
    p["can_stop"]   = m->can_abort();
    // print_status under its own name: "status" has carried the same string since the first
    // version of this API, and the control UI reads the field the desktop's code names.
    p["print_status"] = m->print_status;
    p["stage"]        = std::string(m->get_curr_stage().ToUTF8().data());
    if (m->print_error != 0)
        p["print_error"] = { { "code", error_code_text(m->print_error) }, { "message", print_error_message(m->print_error) } };
    else
        p["print_error"] = nullptr;
    // The HMS summary: how many the printer is reporting and what the first one says, so a card can
    // show "why" without a second request.
    json hms = json::object();
    hms["count"] = (int) m->hms_list.size();
    if (!m->hms_list.empty()) {
        HMSItem&          first = m->hms_list.front();
        const std::string code  = first.get_long_error_code();
        hms["code"]             = code;
        if (HMSQuery* q = wxGetApp().get_hms_query())
            hms["message"] = std::string(q->query_hms_msg(code).ToUTF8().data());
    }
    p["hms"] = hms;
}

void list_host_targets(std::vector<HostTarget>& out)
{
    // Any print host address, not only one the app calls a Moonraker: the fork has no host_type
    // string for Moonraker (only the Device tab's MQTT connect sets that enum), so what a printer
    // speaks is decided by what it answers, not by the preset.
    PresetBundle* bundle = wxGetApp().preset_bundle;
    if (bundle && !bundle->use_bbl_network()) {
        const std::string url = bundle->printers.get_edited_preset().config.opt_string("print_host");
        if (!url.empty()) out.push_back({ "host", moonraker_base(url) });
    }
    std::shared_ptr<PrintHost> connected;
    wxGetApp().get_connect_host(connected);
    if (connected) out.push_back({ "connect", moonraker_base(connected->get_host()) });
}

// Klipper's print_stats.state, in the words the rest of this API uses. Only the control fields are
// filled: a print host's progress is not this feature's business.
static void fill_from_print_stats(const json& stats, json& p)
{
    const std::string state = stats.value("state", std::string());
    p["print_status"] = state;
    p["can_pause"]    = state == "printing";
    p["can_resume"]   = state == "paused";
    p["can_stop"]     = state == "printing" || state == "paused";
    p["stage"]        = stats.value("filename", std::string()); // the job it is on
    const std::string message = stats.value("message", std::string());
    if (state == "error" || !message.empty())
        p["print_error"] = { { "code", state == "error" ? "error" : "" }, { "message", message } };
    else
        p["print_error"] = nullptr;
}

// A number the printer reported under `key`, or nothing. Klipper answers an object it does not
// have (extruder2 on a two-nozzle printer) with an empty one, or leaves it out altogether.
static bool num_of(const json& obj, const char* key, double& out)
{
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_number()) return false;
    out = obj[key].get<double>();
    return true;
}

// The temperatures a Bambu entry carries, from the same answer: the bed and every extruder Klipper
// has (extruder, extruder1, ...). A host that reports none is left without bed_temp / nozzles, and
// the phone then leaves the temperature block off its card instead of rendering NaN.
static void fill_from_heaters(const json& status, json& p)
{
    double temp = 0, target = 0;
    if (status.contains("heater_bed") && num_of(status["heater_bed"], "temperature", temp) &&
        num_of(status["heater_bed"], "target", target)) {
        p["bed_temp"]   = temp;
        p["bed_target"] = target;
    }
    json nozzles = json::array();
    for (int i = 0; i < 4; ++i) {
        const std::string ex = i == 0 ? "extruder" : ("extruder" + std::to_string(i));
        if (!status.contains(ex) || !num_of(status[ex], "temperature", temp) || !num_of(status[ex], "target", target)) continue;
        nozzles.push_back(json { { "temp", temp }, { "target", target } });
    }
    if (!nozzles.empty()) p["nozzles"] = nozzles;
}

void describe_hosts(const std::vector<HostTarget>& targets, json& printers)
{
    if (targets.empty() || !printers.is_array()) return;
    for (const HostTarget& t : targets) {
        json* entry = nullptr;
        for (json& p : printers)
            if (p.is_object() && p.value("id", std::string()) == t.id) { entry = &p; break; }
        if (!entry || t.base.empty()) continue;
        std::string body, error;
        json        stats, status;
        if (ask_again(t.base)) {
            // Read-only: what the printer says it is doing and how warm it is (the objects the LAN
            // list asks a Snapmaker for; extruder1.. answer empty where there is no such nozzle).
            // Never a command.
            if (moonraker_http(t.base + "/printer/objects/query?print_stats&heater_bed&extruder&extruder1&extruder2&extruder3",
                               false, body, error, 2)) {
                const json j = parse_or_raw(body);
                if (j.is_object()) {
                    status = j.value("result", json::object()).value("status", json::object());
                    if (status.is_object()) stats = status.value("print_stats", json::object());
                }
            }
            const bool ok = stats.is_object() && !stats.empty();
            remember_probe(t.base, ok, ok ? stats.value("state", std::string()) : std::string());
        }
        const bool answered = stats.is_object() && !stats.empty();
        if (answered) {
            fill_from_print_stats(stats, *entry);
            fill_from_heaters(status, *entry);
        } else {
            // It is not a Moonraker printer, or it is off: leave every button off rather than guess.
            (*entry)["can_pause"]   = false;
            (*entry)["can_resume"]  = false;
            (*entry)["can_stop"]    = false;
            (*entry)["print_error"] = nullptr;
            if (!error.empty()) (*entry)["status_error"] = error;
        }
    }
}

} // namespace RemoteControl
} // namespace GUI
} // namespace Slic3r
