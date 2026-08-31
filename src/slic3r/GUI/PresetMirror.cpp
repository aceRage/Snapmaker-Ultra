#include "PresetMirror.hpp"

#include "libslic3r/libslic3r.h"   // Slic3r::data_dir()
#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <sstream>
#include <vector>
#include <ctime>

namespace bfs = boost::filesystem;
using json = nlohmann::json;

namespace Slic3r { namespace GUI {

// ---- helpers ----------------------------------------------------------------

static long long read_updated_time(const bfs::path& info_path)
{
    // .info is a simple "key = value" text file; return updated_time or 0.
    boost::system::error_code ec;
    if (!bfs::exists(info_path, ec)) return 0;
    std::ifstream in(info_path.string());
    std::string line;
    while (std::getline(in, line)) {
        auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        std::string key = line.substr(0, pos);
        // trim
        key.erase(0, key.find_first_not_of(" \t"));
        key.erase(key.find_last_not_of(" \t") + 1);
        if (key == "updated_time") {
            std::string val = line.substr(pos + 1);
            try { return std::stoll(val); } catch (...) { return 0; }
        }
    }
    return 0;
}

// Copy an .info, forcing sync_info blank so the mirrored preset is inert to the fork's cloud
// delete/upload gates (keeps user_id + setting_id + base_id).
static void copy_info_inert(const bfs::path& src_info, const bfs::path& dst_info)
{
    boost::system::error_code ec;
    if (!bfs::exists(src_info, ec)) return;
    std::ifstream in(src_info.string());
    std::vector<std::string> lines;
    std::string line;
    bool had_sync = false;
    while (std::getline(in, line)) {
        std::string trimmed = line;
        trimmed.erase(0, trimmed.find_first_not_of(" \t"));
        if (trimmed.rfind("sync_info", 0) == 0) { lines.push_back("sync_info = "); had_sync = true; }
        else lines.push_back(line);
    }
    if (!had_sync) lines.push_back("sync_info = ");
    std::ofstream out(dst_info.string(), std::ios::binary | std::ios::trunc);
    for (auto& l : lines) out << l << "\n";
}

// Find the Bambu Studio user\<uid> dir. Prefer the logged-in uid; prefer stable over Beta; else
// the most-recently-modified numeric dir. Returns empty path if none.
static bfs::path find_bambu_user_dir(const std::string& logged_in_uid)
{
    // data_dir() is %APPDATA%\Snapmaker_Orca; BambuStudio is a sibling under %APPDATA%.
    bfs::path appdata = bfs::path(Slic3r::data_dir()).parent_path();
    const char* roots[] = { "BambuStudio", "BambuStudioBeta" };
    bfs::path newest; std::time_t newest_t = 0;
    for (const char* r : roots) {
        bfs::path user = appdata / r / "user";
        boost::system::error_code ec;
        if (!bfs::is_directory(user, ec)) continue;
        // exact uid match wins immediately (stable checked before Beta)
        if (!logged_in_uid.empty()) {
            bfs::path cand = user / logged_in_uid;
            if (bfs::is_directory(cand, ec)) return cand;
        }
        // else track newest numeric dir
        for (bfs::directory_iterator it(user, ec), end; it != end && !ec; it.increment(ec)) {
            if (!bfs::is_directory(it->status())) continue;
            std::string name = it->path().filename().string();
            if (name.empty() || name == "default") continue;
            if (name.find_first_not_of("0123456789") != std::string::npos) continue;
            std::time_t t = bfs::last_write_time(it->path(), ec);
            if (t > newest_t) { newest_t = t; newest = it->path(); }
        }
    }
    return newest;
}

// ---- manifest ---------------------------------------------------------------

struct Manifest {
    bfs::path path;
    json j = json::object();
    void load(const bfs::path& p) {
        path = p;
        boost::system::error_code ec;
        if (bfs::exists(p, ec)) {
            try { std::ifstream in(p.string()); in >> j; } catch (...) { j = json::object(); }
        }
        if (!j.is_object()) j = json::object();
        if (!j.contains("files")) {
            // Migrate the flat spike manifest { "<rel>": {"t": N}, ... } into { "files": {...} }, so
            // presets copied by the Stage-0 spike are adopted (tracked/updatable) rather than treated
            // as fork-native.
            json files = json::object();
            for (auto it = j.begin(); it != j.end(); ++it)
                if (it.value().is_object() && it.value().contains("t"))
                    files[it.key()] = json{{"t", it.value()["t"]}, {"deleted", false}};
            j = json::object();
            j["files"] = files;
        }
        if (!j["files"].is_object()) j["files"] = json::object();
    }
    bool has(const std::string& rel) const { return j["files"].contains(rel); }
    long long time_of(const std::string& rel) const {
        try { return j["files"].at(rel).at("t").get<long long>(); } catch (...) { return 0; }
    }
    bool deleted(const std::string& rel) const {
        try { return j["files"].at(rel).value("deleted", false); } catch (...) { return false; }
    }
    void set(const std::string& rel, long long t, bool del) {
        j["files"][rel] = json{{"t", t}, {"deleted", del}};
    }
    void save() {
        try { std::ofstream out(path.string(), std::ios::binary | std::ios::trunc); out << j.dump(1); } catch (...) {}
    }
};

// ---- core -------------------------------------------------------------------

// Mirror one file (json/base) applying the older-or-missing + fork-native-protect + deletion rules.
// rel is the manifest key (relative to the fork user\default dir). Returns action for stats.
static const char* mirror_one(const bfs::path& src, const bfs::path& dst, const std::string& rel,
                              long long src_t, Manifest& man, bool is_user_preset)
{
    boost::system::error_code ec;
    bool dst_exists = bfs::exists(dst, ec);
    bool in_man = man.has(rel);

    if (dst_exists && !in_man)
        return "protect";                     // fork-native file we don't own
    if (dst_exists && in_man) {
        if (src_t <= man.time_of(rel)) return "uptodate";
    } else { // dst missing
        if (in_man && src_t <= man.time_of(rel)) {  // user deleted it, BS not newer -> respect deletion
            man.set(rel, man.time_of(rel), true);
            return "respect_delete";
        }
        // in_man && src_t newer -> BS edited since deletion: re-pull. not in_man -> new. both copy.
    }

    bfs::create_directories(dst.parent_path(), ec);
    bfs::copy_file(src, dst, bfs::copy_option::overwrite_if_exists, ec);
    if (ec) { BOOST_LOG_TRIVIAL(warning) << "[preset-mirror] copy failed " << src.string() << ": " << ec.message(); return "error"; }
    if (is_user_preset) {
        bfs::path si = src; si.replace_extension(".info");
        bfs::path di = dst; di.replace_extension(".info");
        copy_info_inert(si, di);
    }
    man.set(rel, src_t, false);
    return "copied";
}

int mirror_bambu_user_presets(const std::string& logged_in_uid)
{
    try {
        bfs::path src_uid = find_bambu_user_dir(logged_in_uid);
        if (src_uid.empty()) { BOOST_LOG_TRIVIAL(info) << "[preset-mirror] no Bambu Studio user dir found; skipping"; return 0; }

        bfs::path dst_root = bfs::path(Slic3r::data_dir()) / "user" / "default";
        boost::system::error_code ec;
        bfs::create_directories(dst_root, ec);

        Manifest man; man.load(dst_root / ".bs_mirror_manifest.json");

        int copied = 0, uptodate = 0, native_protected = 0, respected = 0, errors = 0;
        auto tally = [&](const char* a) {
            std::string s = a;
            if (s == "copied") ++copied; else if (s == "uptodate") ++uptodate;
            else if (s == "protect") ++native_protected; else if (s == "respect_delete") ++respected;
            else if (s == "error") ++errors;
        };

        // print + filament + machine (Stage 2). base\ first so inherits resolve; a machine preset
        // whose inherits/base is a printer this fork lacks is dropped non-fatally by the loader.
        const char* types[] = { "filament", "process", "machine" };
        for (const char* typ : types) {
            bfs::path sdir = src_uid / typ;
            if (!bfs::is_directory(sdir, ec)) continue;
            bfs::path ddir = dst_root / typ;

            // 1) base\ cache (full inheritance copies; no .info, use mtime)
            bfs::path sbase = sdir / "base";
            if (bfs::is_directory(sbase, ec)) {
                for (bfs::directory_iterator it(sbase, ec), end; it != end && !ec; it.increment(ec)) {
                    if (it->path().extension() != ".json") continue;
                    std::string rel = std::string(typ) + "/base/" + it->path().filename().string();
                    long long t = (long long) bfs::last_write_time(it->path(), ec);
                    tally(mirror_one(it->path(), ddir / "base" / it->path().filename(), rel, t, man, false));
                }
            }
            // 2) top-level user presets (sparse override diffs; use .info updated_time)
            for (bfs::directory_iterator it(sdir, ec), end; it != end && !ec; it.increment(ec)) {
                if (!bfs::is_regular_file(it->status())) continue;
                if (it->path().extension() != ".json") continue;
                std::string rel = std::string(typ) + "/" + it->path().filename().string();
                bfs::path info = it->path(); info.replace_extension(".info");
                long long t = read_updated_time(info);
                if (t == 0) t = (long long) bfs::last_write_time(it->path(), ec);
                tally(mirror_one(it->path(), ddir / it->path().filename(), rel, t, man, true));
            }
        }

        man.save();
        BOOST_LOG_TRIVIAL(info) << "[preset-mirror] from " << src_uid.string()
            << " -> copied=" << copied << " uptodate=" << uptodate
            << " fork_native_protected=" << native_protected << " user_deletions_respected=" << respected
            << " errors=" << errors;
        return copied;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "[preset-mirror] failed: " << e.what();
    } catch (...) {
        BOOST_LOG_TRIVIAL(error) << "[preset-mirror] failed (unknown)";
    }
    return 0;
}

}} // namespace Slic3r::GUI
