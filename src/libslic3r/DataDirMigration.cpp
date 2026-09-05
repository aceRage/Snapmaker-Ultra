#include "DataDirMigration.hpp"

#include "common_func/common_func.hpp"

#include <boost/algorithm/hex.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>
#include <boost/uuid/detail/md5.hpp>

#include <cstdlib>
#include <ctime>
#include <sstream>
#include <utility>
#include <vector>

namespace fs = boost::filesystem;

namespace Slic3r {

const char* legacy_data_dir_name() { return SLIC3R_LEGACY_APP_KEY; }

namespace {

// Relative paths under the old root that are not worth carrying over. Everything else
// comes, including the parts we do not understand - a data directory nobody has a full
// map of is exactly the case for copying it whole.
bool skip_entry(const std::string& rel, bool include_archive)
{
    if (rel == "log" || rel.rfind("log/", 0) == 0)
        return true; // history of the *old* install; a fresh log dir is made on start
    if (rel == "hub/hub.json")
        return true; // the per-run secret and port, deleted on a clean shutdown anyway
    if (!include_archive && (rel.rfind("hub/saves", 0) == 0 || rel.rfind("hub/uploads", 0) == 0))
        return true;
    return false;
}

std::string generic_rel(const fs::path& root, const fs::path& p)
{
    return fs::relative(p, root).generic_string();
}

// Replace every occurrence of `from` with `to`, counting them.
size_t replace_all(std::string& s, const std::string& from, const std::string& to)
{
    if (from.empty() || from == to)
        return 0;
    size_t n = 0;
    for (size_t i = s.find(from); i != std::string::npos; i = s.find(from, i + to.size())) {
        s.replace(i, from.size(), to);
        ++n;
    }
    return n;
}

// The .conf is JSON, so a Windows path inside it is stored with its backslashes doubled.
// Rewrite the literal form, the JSON-escaped form and the forward-slash form, so a path
// written by any of the three code paths that produce them is caught.
size_t rewrite_conf_paths(const fs::path& conf, const fs::path& old_dir, const fs::path& new_dir)
{
    boost::nowide::ifstream in(conf.string().c_str(), std::ios::binary);
    if (!in)
        return 0;
    std::ostringstream buf;
    buf << in.rdbuf();
    in.close();
    std::string s = buf.str();

    auto escaped = [](std::string p) {
        std::string out;
        for (char c : p) {
            if (c == '\\')
                out += "\\\\";
            else
                out += c;
        }
        return out;
    };

    const std::string old_native = fs::path(old_dir).make_preferred().string();
    const std::string new_native = fs::path(new_dir).make_preferred().string();
    size_t n = 0;
    n += replace_all(s, escaped(old_native), escaped(new_native)); // the JSON form: do it first
    n += replace_all(s, old_native, new_native);                   // anything stored raw
    n += replace_all(s, old_dir.generic_string(), new_dir.generic_string());
    if (n == 0)
        return 0;

    // The .conf is the JSON document followed by "# MD5 checksum <hex>" over exactly that
    // document. Rewriting the paths invalidates it, and a wrong checksum is the first thing
    // anyone reads as file corruption - so recompute it.
    //
    // The checksum is over the LF form, not what is on disk: AppConfig writes the file through
    // a text-mode stream (so the bytes are CRLF on Windows) but hashes the in-memory JSON dump,
    // and reads it back through a text-mode stream too. We do our own I/O in binary to keep
    // every other byte exactly as it was, so the normalisation has to be explicit here.
    if (const size_t last_brace = s.find_last_of('}'); last_brace != std::string::npos) {
        const std::string body = s.substr(0, last_brace + 1);
        std::string       lf   = body;
        for (size_t i = lf.find("\r\n"); i != std::string::npos; i = lf.find("\r\n", i + 1))
            lf.erase(i, 1);
        using boost::uuids::detail::md5;
        md5              hash;
        md5::digest_type digest{};
        std::string      hex;
        hash.process_bytes(lf.data(), lf.size());
        hash.get_digest(digest);
        boost::algorithm::hex(digest, digest + std::size(digest), std::back_inserter(hex));
        const std::string eol = body.find("\r\n") == std::string::npos ? "\n" : "\r\n";
        s = body + eol + "# MD5 checksum " + hex + eol;
    }

    boost::nowide::ofstream out(conf.string().c_str(), std::ios::binary | std::ios::trunc);
    if (!out)
        return 0;
    out << s;
    return n;
}

} // namespace

DataDirMigrationResult migrate_data_dir(const std::string& parent,
                                        const std::string& new_dir_str,
                                        bool               include_archive)
{
    DataDirMigrationResult res;
    const fs::path new_dir = fs::path(new_dir_str);
    // The old directory is normally the new one's sibling. Under Flatpak it is not: the app id
    // changed, so the sandbox home changed with it and the old data sits under the *previous*
    // app id's config dir. The launcher points us at it with this variable; nothing else sets it.
    const char*    legacy_parent = std::getenv("ULTRAONE_LEGACY_DATA_PARENT");
    const fs::path old_dir = fs::path(legacy_parent && *legacy_parent ? std::string(legacy_parent) : parent)
                             / SLIC3R_LEGACY_APP_KEY;
    res.old_dir = old_dir.string();
    res.new_dir = new_dir.string();

    boost::system::error_code ec;
    if (fs::exists(new_dir, ec)) {
        res.skipped_new_exists = true;
        return res; // already here, whether we made it or the user did
    }
    if (!fs::is_directory(old_dir, ec)) {
        res.skipped_no_old = true;
        return res; // a clean install; nothing to carry over
    }
    if (fs::equivalent(old_dir, new_dir, ec)) {
        res.skipped_new_exists = true;
        return res;
    }

    // Build the copy beside the target and rename it into place at the end, so a crash or
    // a full disk halfway through leaves no half-built directory pretending to be migrated.
    const fs::path staging = fs::path(parent) / (new_dir.filename().string() + ".migrating");
    fs::remove_all(staging, ec);
    fs::create_directories(staging, ec);
    if (ec) {
        res.error = "could not create " + staging.string() + ": " + ec.message();
        return res;
    }

    try {
        for (fs::recursive_directory_iterator it(old_dir), end; it != end; ++it) {
            const fs::path src = it->path();
            const std::string rel = generic_rel(old_dir, src);
            if (skip_entry(rel, include_archive)) {
                if (fs::is_directory(src, ec))
                    it.disable_recursion_pending();
                continue;
            }
            const fs::path dst = staging / fs::path(rel);
            if (fs::is_directory(src, ec)) {
                fs::create_directories(dst, ec);
            } else if (fs::is_regular_file(src, ec)) {
                fs::create_directories(dst.parent_path(), ec);
                // copy_file, not a read/write loop: hub/settings.json carries the VAPID key
                // pair and must arrive byte for byte.
                boost::system::error_code cec;
                fs::copy_file(src, dst, fs::copy_option::overwrite_if_exists, cec);
                if (cec)
                    continue; // a file we cannot read (locked, permissions) is not fatal
                res.files_copied++;
                res.bytes_copied += static_cast<unsigned long long>(fs::file_size(dst, cec));
            }
            // symlinks and anything else are skipped on purpose
        }
    } catch (const std::exception& e) {
        res.error = std::string("copy failed: ") + e.what();
        fs::remove_all(staging, ec);
        return res;
    }

    // The config file is named after the app key, so it is renamed with it - along with any
    // sibling the old build left behind (.conf.bak and friends).
    const std::string old_stem = std::string(SLIC3R_LEGACY_APP_KEY) + ".";
    const std::string new_stem = std::string(SLIC3R_APP_KEY) + ".";
    std::vector<std::pair<fs::path, fs::path>> renames;
    for (fs::directory_iterator it(staging, ec), end; it != end; ++it) {
        const std::string name = it->path().filename().string();
        if (name.rfind(old_stem, 0) == 0)
            renames.emplace_back(it->path(), staging / (new_stem + name.substr(old_stem.size())));
    }
    for (auto& r : renames)
        fs::rename(r.first, r.second, ec);

    // The recent-project list, last_backup_path, settings_folder and the hub upload paths are
    // absolute and point into the old directory. Left alone they would still resolve - the old
    // directory is still there - but every later write would land in the copy the user thinks
    // they stopped using.
    const fs::path conf = staging / (std::string(SLIC3R_APP_KEY) + ".conf");
    if (fs::exists(conf, ec))
        res.conf_paths_rewritten = rewrite_conf_paths(conf, old_dir, new_dir);

    // A note for whoever has to work out later where this data came from. Written into the
    // NEW directory only: the old one is left exactly as it was found.
    {
        boost::nowide::ofstream marker(
            (staging / (std::string(".migrated-from-") + SLIC3R_LEGACY_APP_KEY)).string().c_str(),
            std::ios::binary | std::ios::trunc);
        if (marker) {
            const std::time_t now = std::time(nullptr);
            marker << "source=" << old_dir.string() << "\n"
                   << "app=" << SLIC3R_APP_NAME << "\n"
                   << "epoch=" << static_cast<long long>(now) << "\n"
                   << "files=" << res.files_copied << "\n"
                   << "archive=" << (include_archive ? 1 : 0) << "\n"
                   << "note=the source directory was copied, not moved, and was not modified\n";
        }
    }

    fs::rename(staging, new_dir, ec);
    if (ec) {
        res.error = "could not publish " + staging.string() + " as " + new_dir.string() + ": " + ec.message();
        fs::remove_all(staging, ec);
        return res;
    }

    res.ran = true;
    BOOST_LOG_TRIVIAL(warning) << "data dir migration: copied " << res.files_copied << " files ("
                               << res.bytes_copied << " bytes) from " << res.old_dir << " to "
                               << res.new_dir << ", rewrote " << res.conf_paths_rewritten
                               << " absolute paths in the config; the old directory was left alone";
    return res;
}

} // namespace Slic3r
