#include "PrusaLinkStatus.hpp"

#include "Http.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>

namespace Slic3r {
namespace PrusaLinkStatus {

using nlohmann::json;

bool speaks_prusalink(const std::string& key)
{
    // "prusalink" and "prusaconnect" are the two host_type enum keys (PrintConfig.cpp:82-83) whose
    // PrintHost class is PrusaLink or its PrusaConnect subclass, i.e. the ones whose printer answers
    // /api/v1/status. "sl1" is deliberately left out: an SL1 is an SLA machine whose PrusaLink build
    // answers a different shape, and this fork never sends it FFF plates anyway.
    return key == "prusalink" || key == "prusaconnect";
}

std::string to_klipper_state(const std::string& raw)
{
    std::string s;
    s.reserve(raw.size());
    for (char c : raw)
        s.push_back((char) std::toupper((unsigned char) c));
    if (s == "PRINTING")               return "printing";
    if (s == "PAUSED")                 return "paused";
    if (s == "FINISHED")               return "complete";
    if (s == "STOPPED")                return "cancelled";
    if (s == "IDLE" || s == "READY")   return "standby";
    if (s == "ERROR" || s == "ATTENTION") return "error";
    if (s == "BUSY")                   return "busy";
    // An unknown word is still worth showing, lower-cased so it reads like the rest.
    for (char& c : s)
        c = (char) std::tolower((unsigned char) c);
    return s;
}

std::string base_url(const std::string& host)
{
    std::string h = host;
    while (!h.empty() && (h.back() == '/' || h.back() == ' '))
        h.pop_back();
    if (h.empty())
        return h;
    if (h.compare(0, 7, "http://") == 0 || h.compare(0, 8, "https://") == 0)
        return h;
    return "http://" + h;
}

// ---------------------------------------------------------------- parsing ----

static bool num_at(const json& j, const char* key, double& out)
{
    if (!j.is_object())
        return false;
    auto it = j.find(key);
    if (it == j.end() || !it->is_number())
        return false;
    out = it->get<double>();
    return true;
}

static std::string str_at(const json& j, const char* key)
{
    if (!j.is_object())
        return std::string();
    auto it = j.find(key);
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string();
}

static const json obj_at(const json& j, const char* key)
{
    if (!j.is_object())
        return json::object();
    auto it = j.find(key);
    return it != j.end() && it->is_object() ? *it : json::object();
}

// The job half, shared by /api/v1/status's embedded `job` and the standalone /api/v1/job answer.
// Only fills what the object actually carries, so a status-embedded job (which has no file name)
// does not wipe a name the /api/v1/job answer already gave.
static void fill_job(const json& job, Status& st)
{
    if (!job.is_object() || job.empty())
        return;
    double d = 0;
    if (num_at(job, "progress", d)) {
        st.has_progress = true;
        // PrusaLink reports 0..100. Clamp rather than trust: a mid-job firmware once sent 100.0001.
        st.progress = std::max(0.0, std::min(100.0, d));
    }
    if (num_at(job, "time_remaining", d) && d >= 0) {
        st.has_time_remaining = true;
        st.time_remaining     = (long long) d;
    }
    if (num_at(job, "time_printing", d) && d >= 0) {
        st.has_time_printing = true;
        st.time_printing     = (long long) d;
    }
    if (num_at(job, "id", d))
        st.job_id = (long long) d;
    // /api/v1/job names the file under `file`; its `display_name` is what PrusaSlicer shows, and it
    // falls back to `name` (the 8.3-ish on-disk name) when the firmware did not send one.
    const json file = obj_at(job, "file");
    if (!file.empty()) {
        const std::string display = str_at(file, "display_name");
        const std::string name    = str_at(file, "name");
        if (!display.empty())
            st.filename = display;
        else if (!name.empty())
            st.filename = name;
    }
    // A standalone /api/v1/job also carries the state, and it is the authoritative one while a job
    // is loaded. Only used when the status answer did not already give one.
    if (st.raw_state.empty()) {
        const std::string s = str_at(job, "state");
        if (!s.empty()) {
            st.raw_state = s;
            st.state     = to_klipper_state(s);
        }
    }
}

Status parse_status(const std::string& status_json, const std::string& job_json)
{
    Status st;
    json   j;
    try {
        j = json::parse(status_json);
    } catch (...) {
        j = json::object();
    }
    if (!j.is_object() || j.empty()) {
        st.error = "the answer was not a PrusaLink status object";
        return st;
    }
    const json printer = obj_at(j, "printer");
    if (printer.empty()) {
        // Not a /api/v1/status answer at all (an OctoPrint box, a captive portal, a 404 page).
        st.error = "the answer was not a PrusaLink status object";
        return st;
    }
    st.answered  = true;
    st.raw_state = str_at(printer, "state");
    if (!st.raw_state.empty())
        st.state = to_klipper_state(st.raw_state);

    double t = 0, target = 0;
    if (num_at(printer, "temp_bed", t)) {
        st.has_bed   = true;
        st.bed_temp  = t;
        st.bed_target = num_at(printer, "target_bed", target) ? target : 0.0;
    }
    if (num_at(printer, "temp_nozzle", t)) {
        st.has_nozzle     = true;
        st.nozzle_temp    = t;
        st.nozzle_target  = num_at(printer, "target_nozzle", target) ? target : 0.0;
    }
    // The status answer embeds a slim job object while a print is loaded: progress and the two
    // times, but no file name. Newer firmwares also put progress straight on `printer`.
    fill_job(obj_at(j, "job"), st);
    if (!st.has_progress && num_at(printer, "progress", t)) {
        st.has_progress = true;
        st.progress     = std::max(0.0, std::min(100.0, t));
    }

    if (!job_json.empty()) {
        json jb;
        try {
            jb = json::parse(job_json);
        } catch (...) {
            jb = json::object();
        }
        // /api/v1/job answers the job object at the top level. It is asked second, so what it says
        // wins for the fields it carries - notably the file name, which the status answer lacks.
        fill_job(jb, st);
    }
    return st;
}

// ----------------------------------------------------------------- the probe ----

static bool get(const std::string& url, const Auth& auth, std::string& body, std::string& error,
                unsigned& status_code, int timeout_s)
{
    bool ok = false;
    auto http = Http::get(url);
    // Exactly PrusaLink::set_auth (OctoPrint.cpp:597): a key in the header, or HTTP Digest.
    if (auth.auth_type == "user")
        http.auth_digest(auth.user, auth.password);
    else
        http.header("X-Api-Key", auth.apikey);
    http.header("Accept", "application/json")
        .timeout_connect(timeout_s)
        .timeout_max(timeout_s)
        .size_limit(256 * 1024)
        .on_error([&](std::string reply, std::string err, unsigned code) {
            status_code = code;
            error       = err.empty() ? ("HTTP " + std::to_string(code)) : err;
            if (!reply.empty())
                body = reply;
        })
        .on_complete([&](std::string reply, unsigned code) {
            status_code = code;
            body        = reply;
            ok          = true;
        })
        .perform_sync();
    if (!ok && error.empty())
        error = "no answer";
    return ok;
}

Status probe(const std::string& host, const Auth& auth, int timeout_s)
{
    Status            st;
    const std::string base = base_url(host);
    if (base.empty()) {
        st.error = "this device has no address";
        return st;
    }
    std::string body, error;
    unsigned    code = 0;
    if (!get(base + "/api/v1/status", auth, body, error, code, timeout_s)) {
        st.error = error;
        // 401/403 is worth saying out loud: the address is right and the credentials are not, which
        // is the single most common way a PrusaLink preset is mis-set up.
        if (code == 401 || code == 403)
            st.authorized = false;
        return st;
    }
    Status parsed = parse_status(body);
    if (!parsed.answered) {
        parsed.error = parsed.error.empty() ? "this printer did not answer as a PrusaLink printer" : parsed.error;
        return parsed;
    }
    // The file name lives only in /api/v1/job, and only a printer that has a job loaded has one.
    // One extra call, and only while it would say something: a poll of an idle printer stays at one
    // request.
    if (parsed.state == "printing" || parsed.state == "paused") {
        std::string job_body, job_error;
        unsigned    job_code = 0;
        if (get(base + "/api/v1/job", auth, job_body, job_error, job_code, timeout_s))
            parsed = parse_status(body, job_body);
    }
    return parsed;
}

} // namespace PrusaLinkStatus
} // namespace Slic3r
