#include "GcodeArchive.hpp"

#include "DeviceManager.hpp"
#include "GUI_App.hpp"
#include "PartPlate.hpp"
#include "Plater.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/GCode/Thumbnails.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/Utils.hpp"

#include <boost/algorithm/string/predicate.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <openssl/sha.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <future>
#include <mutex>
#include <sstream>

#include <wx/app.h>

namespace Slic3r {
namespace GUI {
namespace GcodeArchive {

namespace fs = boost::filesystem;
using nlohmann::json;

// One archive per PC, but several slicer instances write into it at once. Names carry the pid and a
// counter so two instances never pick the same one, and this mutex only keeps one thread of *this*
// instance in the retention pass at a time.
static std::mutex g_mutex;

// ---------------------------------------------------------------- settings ----

bool enabled()
{
    AppConfig* cfg = wxGetApp().app_config;
    return cfg && cfg->get_bool("ultra_gcode_archive");
}

std::string dir()
{
    AppConfig*        cfg = wxGetApp().app_config;
    const std::string set = cfg ? cfg->get("ultra_gcode_archive_dir") : std::string();
    if (!set.empty())
        return set;
    return (fs::path(data_dir()) / "gcode_archive").string();
}

int max_records()
{
    AppConfig* cfg = wxGetApp().app_config;
    int        n   = 100;
    if (cfg) {
        try {
            const std::string v = cfg->get("ultra_gcode_archive_max");
            if (!v.empty()) n = std::stoi(v);
        } catch (...) { n = 100; }
    }
    return std::max(1, std::min(10000, n));
}

// ----------------------------------------------------------------- helpers ----

// From a worker thread: run fn on the GUI thread and wait for it (bounded). Already there: just run.
static bool on_main(std::function<void()> fn, int timeout_ms = 15000)
{
    if (wxIsMainThread()) {
        try { fn(); } catch (...) { return false; }
        return true;
    }
    auto done = std::make_shared<std::promise<void>>();
    auto fut  = done->get_future();
    wxGetApp().CallAfter([done, fn]() {
        try { fn(); } catch (...) {}
        done->set_value();
    });
    return fut.wait_for(std::chrono::milliseconds(timeout_ms)) == std::future_status::ready;
}

// The archive's own names, which are also the ids the phone API addresses. Deliberately narrow:
// letters, digits, dot, dash and underscore only, so a name is safe as a Windows file name, as a
// URL path segment and against the hub's allow-list pattern - whatever the project was called.
// The name the printer was really given is kept in the sidecar (sent_name), not in the file name.
static std::string sanitize(std::string s, size_t max_len = 64)
{
    std::string out;
    for (char c : s) {
        const bool keep = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-' || c == '_';
        out += keep ? c : '_';
    }
    while (!out.empty() && (out.back() == '.' || out.back() == '_')) out.pop_back();
    if (out.size() > max_len) out.resize(max_len);
    if (out.empty()) out = "x";
    return out;
}

static std::string sha256_of(const fs::path& path)
{
    boost::nowide::ifstream f(path.string().c_str(), std::ios::binary);
    if (!f) return "";
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    std::string buf(64 * 1024, '\0');
    while (f.read(&buf[0], (std::streamsize) buf.size()) || f.gcount() > 0)
        SHA256_Update(&ctx, buf.data(), (size_t) f.gcount());
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    std::string hex;
    hex.reserve(SHA256_DIGEST_LENGTH * 2);
    char two[3];
    for (unsigned char b : digest) {
        std::snprintf(two, sizeof two, "%02x", b);
        hex += two;
    }
    return hex;
}

// Temp file first, then rename: a reader never sees half a sidecar.
static bool write_atomic(const fs::path& path, const std::string& data)
{
    const fs::path tmp = path.string() + ".part";
    {
        boost::nowide::ofstream f(tmp.string().c_str(), std::ios::binary | std::ios::trunc);
        if (!f) return false;
        f.write(data.data(), (std::streamsize) data.size());
        if (!f) return false;
    }
    boost::system::error_code ec;
    fs::remove(path, ec);
    fs::rename(tmp, path, ec);
    if (ec) { fs::remove(tmp, ec); return false; }
    return true;
}

static long long now_seconds()
{
    return (long long) std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

// "20260904-131502" in local time - the name sorts the folder for a person browsing it.
static std::string stamp(long long unix_seconds)
{
    const std::time_t t = (std::time_t) unix_seconds;
    std::tm           tm {};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%d-%H%M%S", &tm);
    return buf;
}

// ------------------------------------------------------------------- meta ----

Meta meta_for_plate(int plate, const std::string& mode)
{
    // Shared, not captured by reference: a call that times out leaves the lambda queued, and it
    // must not write into a Meta this function has already returned.
    auto out    = std::make_shared<Meta>();
    out->mode   = mode.empty() ? "upload" : mode;
    out->plate  = plate;
    on_main([out, plate]() {
        Meta&         m      = *out;
        Plater*       plater = wxGetApp().plater();
        PresetBundle* bundle = wxGetApp().preset_bundle;
        if (!plater || !bundle) return;
        m.project_title = plater->get_project_name().ToUTF8().data();
        m.project_path  = plater->get_project_filename().ToUTF8().data();
        if (auto* model = bundle->printers.get_edited_preset().config.option<ConfigOptionString>("printer_model"))
            m.printer_model = model->value;

        PartPlateList& plates = plater->get_partplate_list();
        const int      index  = (plate >= 0 && plate < plates.get_plate_count()) ? plate : plates.get_curr_plate_index();
        m.plate               = index;
        PartPlate* p          = (index >= 0 && index < plates.get_plate_count()) ? plates.get_plate(index) : nullptr;
        if (!p) return;
        m.plate_name = p->get_plate_name();

        // One local copy: full_config() returns by value, and a temporary must not be read through
        // afterwards (RemoteSend's file_filaments_of does the same).
        const DynamicPrintConfig full = bundle->full_config();
        std::vector<double>      density;
        if (auto* d = full.option<ConfigOptionFloats>("filament_density"))
            density = d->values;
        std::vector<std::string> types;
        if (auto* t = full.option<ConfigOptionStrings>("filament_type"))
            types = t->values;
        std::vector<std::string> colours;
        if (auto* c = bundle->project_config.option<ConfigOptionStrings>("filament_colour"))
            colours = c->values;

        if (p->is_slice_result_valid() && p->get_slice_result()) {
            const auto& st = p->get_slice_result()->print_statistics;
            if (!st.modes.empty())
                m.estimated_time_s = (int) st.modes.front().time;
            for (const auto& kv : st.total_volumes_per_extruder) {
                Filament f;
                f.index        = (int) kv.first;
                f.type         = kv.first < types.size() ? types[kv.first] : "";
                f.colour       = kv.first < colours.size() ? colours[kv.first] : "";
                const double d = kv.first < density.size() ? density[kv.first] : 1.24;
                f.grams        = kv.second / 1000.0 * d;
                m.estimated_weight_g += f.grams;
                m.filaments.push_back(f);
            }
        }
        // The small plate thumbnail, when the plate already has one. Never rendered here: a send
        // must not wait on OpenGL, and a plate that was sliced from the UI has one anyway.
        if (p->thumbnail_data.is_valid()) {
            try {
                auto png = GCodeThumbnails::compress_thumbnail(p->thumbnail_data, GCodeThumbnailsFormat::PNG);
                if (png && png->data && png->size > 0)
                    m.thumbnail_png.assign(static_cast<const char*>(png->data), png->size);
            } catch (...) {}
        }
    });
    return *out;
}

std::string bambu_printer_name(const std::string& dev_id)
{
    auto name = std::make_shared<std::string>(dev_id);
    auto id   = std::make_shared<std::string>(dev_id);
    on_main([name, id]() {
        DeviceManager* dm = wxGetApp().getDeviceManager();
        if (!dm) return;
        std::map<std::string, MachineObject*> all = dm->get_my_machine_list();
        for (const auto& kv : dm->get_local_machine_list()) all.insert(kv);
        for (const auto& kv : all)
            if (kv.second && kv.second->dev_id == *id && !kv.second->dev_name.empty()) { *name = kv.second->dev_name; return; }
    }, 5000);
    return *name;
}

// --------------------------------------------------------------- archiving ----

static json meta_json(const Meta& m)
{
    json j;
    j["printer"] = { { "id", m.printer_id }, { "kind", m.printer_kind }, { "name", m.printer_name }, { "model", m.printer_model } };
    j["plate"]   = m.plate;
    j["plate_name"]    = m.plate_name;
    j["project_title"] = m.project_title;
    j["project_path"]  = m.project_path;
    j["source"]        = m.source;
    j["mode"]          = m.mode;
    j["sent_name"]     = m.file_name; // the name the printer was given, before the archive tidied it
    j["filaments"]     = json::array();
    for (const Filament& f : m.filaments)
        j["filaments"].push_back({ { "index", f.index }, { "type", f.type }, { "colour", f.colour }, { "grams", f.grams } });
    if (m.estimated_time_s > 0)     j["estimated_time_s"] = m.estimated_time_s;
    if (m.estimated_weight_g > 0.0) j["estimated_weight_g"] = m.estimated_weight_g;
    return j;
}

static Record record_from_sidecar(const fs::path& sidecar)
{
    Record r;
    boost::nowide::ifstream f(sidecar.string().c_str(), std::ios::binary);
    if (!f) return r;
    std::stringstream ss;
    ss << f.rdbuf();
    json j;
    try { j = json::parse(ss.str()); } catch (...) { return r; }
    if (!j.is_object() || !j.contains("id")) return r;
    r.id     = j.value("id", std::string());
    r.time   = j.value("time", (long long) 0);
    r.file   = j.value("file", std::string());
    r.size   = j.value("size", (long long) 0);
    r.sha256 = j.value("sha256", std::string());
    r.json   = j;
    r.path   = (sidecar.parent_path() / r.file).string();
    const fs::path thumb = sidecar.parent_path() / (r.id + ".png");
    boost::system::error_code ec;
    r.has_thumbnail = fs::is_regular_file(thumb, ec);
    if (r.has_thumbnail) r.thumbnail_path = thumb.string();
    return r;
}

// Drop the oldest records until only max are left. Called with g_mutex held.
static void enforce_retention(const fs::path& root, int max)
{
    std::vector<std::pair<long long, fs::path>> sidecars;
    boost::system::error_code                   ec;
    for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
        if (it->path().extension() != ".json") continue;
        const Record r = record_from_sidecar(it->path());
        if (r.id.empty()) continue;
        sidecars.emplace_back(r.time, it->path());
    }
    if ((int) sidecars.size() <= max) return;
    std::sort(sidecars.begin(), sidecars.end(),
              [](const std::pair<long long, fs::path>& a, const std::pair<long long, fs::path>& b) {
                  return a.first != b.first ? a.first < b.first : a.second.string() < b.second.string();
              });
    const size_t drop = sidecars.size() - (size_t) max;
    for (size_t i = 0; i < drop; ++i) {
        const Record r = record_from_sidecar(sidecars[i].second);
        boost::system::error_code e;
        if (!r.file.empty()) fs::remove(root / r.file, e);
        fs::remove(root / (r.id + ".png"), e);
        fs::remove(sidecars[i].second, e);
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: retention removed " << r.id;
    }
}

Record archive(const std::string& sent_file_path, const Meta& meta)
{
    Record out;
    try {
        if (!enabled()) return out;
        boost::system::error_code ec;
        const fs::path            source(sent_file_path);
        if (sent_file_path.empty() || !fs::is_regular_file(source, ec)) {
            BOOST_LOG_TRIVIAL(warning) << "GcodeArchive: nothing to archive at " << sent_file_path;
            return out;
        }
        const fs::path root(dir());
        fs::create_directories(root, ec);
        if (!fs::is_directory(root, ec)) {
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: cannot use " << root.string();
            return out;
        }

        std::lock_guard<std::mutex> lock(g_mutex);
        const long long             time = now_seconds();
        // <timestamp>_<printer>_<name>, plus this process's pid and a counter when that is taken:
        // several instances share one folder and must never pick the same id.
        const std::string printer = sanitize(meta.printer_name.empty() ? meta.printer_id : meta.printer_name, 40);
        std::string       name    = meta.file_name.empty() ? source.filename().string() : meta.file_name;
        name                      = sanitize(fs::path(name).filename().string(), 80);
        // The extension stays on the file; the id (and so the sidecar) drops it.
        std::string ext;
        for (const char* e : { ".gcode.3mf", ".gcode.gz", ".gcode", ".3mf", ".bgcode" })
            if (name.size() > std::strlen(e) && boost::algorithm::iends_with(name, e)) { ext = e; break; }
        if (ext.empty()) ext = source.extension().string();
        std::string stem = name.substr(0, name.size() - std::min(name.size(), ext.size()));
        if (stem.empty()) stem = "plate";

        // The name is claimed by the copy itself: fail_if_exists means the file system decides, so
        // two instances writing into one folder in the same second cannot end up on the same id.
        const std::string base = stamp(time) + "_" + printer + "_" + stem;
        std::string       id;
        fs::path          dest;
        for (int n = 0; n < 200; ++n) {
            if (n == 0) {
                id = base;
            } else {
                char suffix[32];
                std::snprintf(suffix, sizeof suffix, "-%d-%d", (int) get_current_pid(), n);
                id = base + suffix;
            }
            if (fs::exists(root / (id + ".json"), ec)) continue;
            dest = root / (id + ext);
            ec.clear();
            fs::copy_file(source, dest, fs::copy_option::fail_if_exists, ec);
            if (!ec) break;
            dest.clear();
        }
        if (dest.empty()) {
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: copying " << source.string() << " into " << root.string()
                                     << " failed: " << ec.message();
            return out;
        }

        bool has_thumb = false;
        if (!meta.thumbnail_png.empty())
            has_thumb = write_atomic(root / (id + ".png"), meta.thumbnail_png);

        json j    = meta_json(meta);
        j["id"]   = id;
        j["time"] = time;
        j["file"] = dest.filename().string();
        j["size"] = (long long) fs::file_size(dest, ec);
        j["sha256"]        = sha256_of(dest);
        j["has_thumbnail"] = has_thumb;
        if (!write_atomic(root / (id + ".json"), j.dump(2))) {
            // No sidecar means no record: do not leave an orphan file behind.
            fs::remove(dest, ec);
            fs::remove(root / (id + ".png"), ec);
            BOOST_LOG_TRIVIAL(error) << "GcodeArchive: writing the sidecar of " << id << " failed";
            return out;
        }

        out.id            = id;
        out.time          = time;
        out.file          = j["file"].get<std::string>();
        out.path          = dest.string();
        out.size          = j["size"].get<long long>();
        out.sha256        = j["sha256"].get<std::string>();
        out.has_thumbnail = has_thumb;
        if (has_thumb) out.thumbnail_path = (root / (id + ".png")).string();
        out.json = j;
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: stored " << id << " (" << out.size << " bytes) for " << meta.printer_name;

        enforce_retention(root, max_records());
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: " << e.what();
        return Record();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: archiving failed";
        return Record();
    }
    return out;
}

std::vector<Record> list(const std::string& printer_id_filter)
{
    std::vector<Record> out;
    try {
        const fs::path            root(dir());
        boost::system::error_code ec;
        if (!fs::is_directory(root, ec)) return out;
        for (fs::directory_iterator it(root, ec), end; it != end && !ec; it.increment(ec)) {
            if (it->path().extension() != ".json") continue;
            Record r = record_from_sidecar(it->path());
            if (r.id.empty()) continue;
            if (!printer_id_filter.empty()) {
                const std::string id = r.json.contains("printer") && r.json["printer"].is_object()
                                           ? r.json["printer"].value("id", std::string())
                                           : std::string();
                if (id != printer_id_filter) continue;
            }
            out.push_back(std::move(r));
        }
        std::sort(out.begin(), out.end(), [](const Record& a, const Record& b) {
            return a.time != b.time ? a.time > b.time : a.id > b.id;
        });
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "GcodeArchive: listing failed: " << e.what();
    } catch (...) {}
    return out;
}

Record find(const std::string& id)
{
    Record out;
    // The id is a file stem this module made (sanitize()); never let a caller's string leave the
    // folder, so it is checked against that same alphabet rather than only for ".." and slashes.
    if (id.empty() || id.size() > 200 || id != sanitize(id, 200))
        return out;
    try {
        const fs::path            sidecar = fs::path(dir()) / (id + ".json");
        boost::system::error_code ec;
        if (!fs::is_regular_file(sidecar, ec)) return out;
        Record r = record_from_sidecar(sidecar);
        if (r.id != id) return Record();
        return r;
    } catch (...) {}
    return out;
}

bool remove(const std::string& id)
{
    const Record r = find(id);
    if (r.id.empty()) return false;
    try {
        const fs::path            root(dir());
        boost::system::error_code ec;
        if (!r.file.empty()) fs::remove(root / r.file, ec);
        fs::remove(root / (r.id + ".png"), ec);
        fs::remove(root / (r.id + ".json"), ec);
        BOOST_LOG_TRIVIAL(info) << "GcodeArchive: removed " << id;
        return true;
    } catch (...) {}
    return false;
}

} // namespace GcodeArchive
} // namespace GUI
} // namespace Slic3r
