#include "SupportSet.hpp"

#include <algorithm>
#include <cctype>
#include <ctime>
#include <set>

#include <boost/algorithm/string/predicate.hpp>
#include <boost/algorithm/string/trim.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <nlohmann/json.hpp>

#include "Config.hpp"
#include "I18N.hpp"
#include "Preset.hpp"
#include "PresetBundle.hpp"
#include "PrintConfig.hpp"
#include "Utils.hpp"
#include "format.hpp"
#include "libslic3r.h"

namespace fs = boost::filesystem;

namespace Slic3r {

// -------------------------------------------------------------------------------------------
// Key sets
// -------------------------------------------------------------------------------------------

const std::vector<std::string>& support_set_excluded_keys()
{
    // Named constant so Stages 1, 2 and 5 agree on it (plan §3.2).
    static const std::vector<std::string> s_excluded = {
        // Filament slot indices are not portable between printers; the interface filament
        // travels as SupportSet::interface_filament_type instead, and the base support filament
        // is deliberately out of scope.
        "support_filament",
        "support_interface_filament",
        // A raft is an object-wide substructure, not a support-interface setting.
        "raft_layers", "raft_contact_distance", "raft_expansion",
        "raft_first_layer_density", "raft_first_layer_expansion",
        // Object layer indices - meaningless on another model.
        "enforce_support_layers",
        // A printer-wide PrintConfig key (not a PrintObjectConfig member today, listed for the
        // day it becomes one).
        "independent_support_layer_height",
        // These carry category "Support" in print_config_def but are brim and bridging
        // settings, not support settings. Applying a support set must not silently rewrite the
        // user's brim or bridge behaviour. Deviation from the plan's mechanical rule, kept here
        // because the plan designates this list as the place such adjustments live.
        "brim_width", "brim_type", "brim_object_gap",
        "brim_ears_max_angle", "brim_ears_detection_length",
        "bridge_no_support", "max_bridge_length",
    };
    return s_excluded;
}

const std::vector<std::string>& support_set_keys()
{
    static const std::vector<std::string> s_keys = []() {
        std::vector<std::string> out;
        const std::vector<std::string> &excluded = support_set_excluded_keys();
        // Built from the def rather than hard-coded, so a future support key joins sets
        // automatically. (a) a PrintObjectConfig member and (b) category "Support".
        PrintObjectConfig proto;
        for (const std::string &key : proto.keys()) {
            const ConfigOptionDef *def = print_config_def.get(key);
            if (def == nullptr || def->category != "Support")
                continue;
            if (std::find(excluded.begin(), excluded.end(), key) != excluded.end())
                continue;
            out.emplace_back(key);
        }
        std::sort(out.begin(), out.end());
        return out;
    }();
    return s_keys;
}

bool support_set_is_allowed_key(const std::string &key)
{
    const std::vector<std::string> &keys = support_set_keys();
    return std::binary_search(keys.begin(), keys.end(), key);
}

// -------------------------------------------------------------------------------------------
// Interface-filament resolution (plan §3.3)
// -------------------------------------------------------------------------------------------

static bool iequals(const std::string &a, const std::string &b)
{
    return boost::iequals(a, b);
}

int resolve_interface_filament(const std::string &type, const DynamicPrintConfig &full_config, std::string *warning)
{
    if (warning != nullptr)
        warning->clear();

    // "same" (or missing) - support_interface_filament == 0 already means "no specific filament,
    // use the current one".
    if (type.empty() || iequals(type, "same"))
        return 0;

    const ConfigOptionStrings *types    = full_config.option<ConfigOptionStrings>("filament_type");
    const ConfigOptionBools   *solubles = full_config.option<ConfigOptionBools>("filament_soluble");
    const size_t               n        = types == nullptr ? 0 : types->values.size();

    auto is_soluble = [solubles](size_t i) {
        return solubles != nullptr && i < solubles->values.size() && solubles->values[i] != 0;
    };

    if (iequals(type, "soluble")) {
        // Ties always break to the lowest slot, so the rule is deterministic.
        for (size_t i = 0; i < n; ++ i)
            if (is_soluble(i))
                return int(i) + 1;
        for (size_t i = 0; i < n; ++ i)
            if (iequals(types->values[i], "PVA") || iequals(types->values[i], "BVOH"))
                return int(i) + 1;
        if (warning != nullptr)
            *warning = _u8L("No soluble filament is loaded; the support interface will use the current filament.");
        return 0;
    }

    // An explicit filament type: exact (case-insensitive) first, then a "<T>-" prefix, so "PLA"
    // matches "PLA-CF" only when no plain PLA is loaded.
    for (size_t i = 0; i < n; ++ i)
        if (iequals(types->values[i], type))
            return int(i) + 1;
    const std::string prefix = type + "-";
    for (size_t i = 0; i < n; ++ i)
        if (types->values[i].size() > prefix.size() && boost::istarts_with(types->values[i], prefix))
            return int(i) + 1;

    if (warning != nullptr)
        *warning = Slic3r::format(_u8L("No %1% filament is loaded; the support interface will use the current filament."), type);
    return 0;
}

std::string support_set_interface_filament_type(const DynamicPrintConfig &full_config)
{
    const ConfigOptionInt *slot_opt = full_config.option<ConfigOptionInt>("support_interface_filament");
    const int              slot     = slot_opt == nullptr ? 0 : slot_opt->value;
    if (slot <= 0)
        return "same";

    const ConfigOptionBools   *solubles = full_config.option<ConfigOptionBools>("filament_soluble");
    const size_t               i        = size_t(slot - 1);
    if (solubles != nullptr && i < solubles->values.size() && solubles->values[i] != 0)
        return "soluble";

    const ConfigOptionStrings *types = full_config.option<ConfigOptionStrings>("filament_type");
    if (types != nullptr && i < types->values.size() && ! types->values[i].empty())
        return types->values[i];

    // The slot is set but we cannot say to what - fall back to "same" rather than inventing a type.
    return "same";
}

// -------------------------------------------------------------------------------------------
// Capture and apply
// -------------------------------------------------------------------------------------------

static std::string iso_utc_now()
{
    // The plan documents the extended form ("2026-09-02T14:05:11Z"); Utils::iso_utc_timestamp()
    // produces the basic form, so format it here. Informational metadata, never parsed back.
    std::time_t t = std::time(nullptr);
    std::tm     tm {};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32] = { 0 };
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tm);
    return std::string(buf);
}

SupportSet support_set_from_config(const DynamicPrintConfig &cfg, const std::string &name)
{
    SupportSet set;
    set.name        = name;
    set.created     = iso_utc_now();
    set.app_version = SLIC3R_VERSION;
    for (const std::string &key : support_set_keys())
        if (cfg.has(key))
            set.values[key] = cfg.opt_serialize(key);
    set.interface_filament_type = support_set_interface_filament_type(cfg);
    return set;
}

void support_set_apply_to(const SupportSet         &set,
                          DynamicPrintConfig       &out,
                          const DynamicPrintConfig &full_config,
                          std::string              *warning)
{
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::EnableSilent);
    for (const auto &kv : set.values) {
        if (! support_set_is_allowed_key(kv.first)) {
            BOOST_LOG_TRIVIAL(warning) << "support set \"" << set.name << "\": ignoring key " << kv.first;
            continue;
        }
        try {
            out.set_deserialize(kv.first, kv.second, substitutions);
        } catch (const std::exception &ex) {
            BOOST_LOG_TRIVIAL(warning) << "support set \"" << set.name << "\": cannot apply " << kv.first
                                       << " = \"" << kv.second << "\": " << ex.what();
        }
    }
    // The interface filament travels as a type and is resolved against the filaments actually
    // loaded here, which is what makes a set portable between printers.
    const int slot = resolve_interface_filament(set.interface_filament_type, full_config, warning);
    out.set_key_value("support_interface_filament", new ConfigOptionInt(slot));
}

// -------------------------------------------------------------------------------------------
// JSON
// -------------------------------------------------------------------------------------------

std::string sanitize_support_set_filename(const std::string &name)
{
    std::string out;
    out.reserve(name.size());
    for (unsigned char c : name) {
        const bool keep = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                          c == ' ' || c == '_' || c == '.' || c == '-';
        out.push_back(keep ? char(c) : '_');
    }
    boost::trim(out);
    // A name of nothing but separators (or non-ASCII) collapses to underscores; keep something
    // openable rather than producing ".json".
    if (out.empty() || out.find_first_not_of('.') == std::string::npos)
        out = "support_set";
    return out;
}

nlohmann::json support_set_to_json(const SupportSet &set)
{
    nlohmann::json j;
    j["version"]                 = SUPPORT_SET_VERSION;
    j["type"]                    = SUPPORT_SET_TYPE;
    j["name"]                    = set.name;
    j["description"]             = set.description;
    j["created"]                 = set.created;
    j["app_version"]             = set.app_version;
    j["interface_filament_type"] = set.interface_filament_type;
    nlohmann::json values = nlohmann::json::object();
    for (const auto &kv : set.values)
        values[kv.first] = kv.second;
    j["values"] = values;
    return j;
}

bool support_set_from_json(const nlohmann::json     &j,
                           SupportSet               &set,
                           std::string              *err,
                           std::vector<std::string> *dropped)
{
    auto fail = [err](const std::string &msg) {
        if (err != nullptr)
            *err = msg;
        return false;
    };

    if (! j.is_object())
        return fail("not a JSON object");
    if (j.contains("type") && j["type"].is_string() && j["type"].get<std::string>() != SUPPORT_SET_TYPE)
        return fail("not a support set (type is \"" + j["type"].get<std::string>() + "\")");
    if (j.contains("version") && j["version"].is_number_integer() && j["version"].get<int>() > SUPPORT_SET_VERSION)
        return fail("written by a newer version of the application");
    if (! j.contains("name") || ! j["name"].is_string() || j["name"].get<std::string>().empty())
        return fail("no name");

    set = SupportSet();
    set.name = j["name"].get<std::string>();
    if (j.contains("description") && j["description"].is_string())
        set.description = j["description"].get<std::string>();
    if (j.contains("created") && j["created"].is_string())
        set.created = j["created"].get<std::string>();
    if (j.contains("app_version") && j["app_version"].is_string())
        set.app_version = j["app_version"].get<std::string>();
    if (j.contains("interface_filament_type") && j["interface_filament_type"].is_string())
        set.interface_filament_type = j["interface_filament_type"].get<std::string>();
    if (set.interface_filament_type.empty())
        set.interface_filament_type = "same";

    if (j.contains("values") && j["values"].is_object()) {
        for (auto it = j["values"].begin(); it != j["values"].end(); ++ it) {
            if (! it.value().is_string()) {
                if (dropped != nullptr)
                    dropped->emplace_back(it.key());
                continue;
            }
            // A key outside the allowed set is dropped and reported; it does not fail the load.
            if (! support_set_is_allowed_key(it.key())) {
                if (dropped != nullptr)
                    dropped->emplace_back(it.key());
                continue;
            }
            set.values[it.key()] = it.value().get<std::string>();
        }
    }
    return true;
}

std::string support_set_to_json_string(const SupportSet &set)
{
    return support_set_to_json(set).dump(4);
}

bool support_set_from_json_string(const std::string        &text,
                                  SupportSet               &set,
                                  std::string              *err,
                                  std::vector<std::string> *dropped)
{
    try {
        return support_set_from_json(nlohmann::json::parse(text), set, err, dropped);
    } catch (const std::exception &ex) {
        if (err != nullptr)
            *err = ex.what();
        return false;
    }
}

// -------------------------------------------------------------------------------------------
// The store
// -------------------------------------------------------------------------------------------

static std::string s_preset_folder;

SupportSetStore& SupportSetStore::instance()
{
    static SupportSetStore s_store;
    return s_store;
}

void SupportSetStore::set_preset_folder(const std::string &folder) { s_preset_folder = folder; }

std::string SupportSetStore::preset_folder()
{
    return s_preset_folder.empty() ? std::string(DEFAULT_USER_FOLDER_NAME) : s_preset_folder;
}

std::string SupportSetStore::dir()
{
    return (fs::path(data_dir()) / PRESET_USER_DIR / preset_folder() / SUPPORT_SET_SUBDIR).make_preferred().string();
}

std::string SupportSetStore::fallback_dir()
{
    return (fs::path(data_dir()) / PRESET_USER_DIR / DEFAULT_USER_FOLDER_NAME / SUPPORT_SET_SUBDIR).make_preferred().string();
}

bool SupportSetStore::load_file(const std::string &path, SupportSet &set, std::string *err)
{
    try {
        boost::nowide::ifstream ifs(path);
        if (! ifs.good()) {
            if (err != nullptr)
                *err = "cannot open " + path;
            return false;
        }
        std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
        if (! support_set_from_json_string(text, set, err))
            return false;
        set.file = path;
        return true;
    } catch (const std::exception &ex) {
        if (err != nullptr)
            *err = ex.what();
        return false;
    }
}

bool SupportSetStore::save_file(const std::string &path, const SupportSet &set, std::string *err)
{
    try {
        fs::path p(path);
        if (p.has_parent_path() && ! fs::exists(p.parent_path()))
            fs::create_directories(p.parent_path());
        boost::nowide::ofstream ofs(path, std::ios::binary | std::ios::trunc);
        if (! ofs.good()) {
            if (err != nullptr)
                *err = "cannot write " + path;
            return false;
        }
        ofs << support_set_to_json_string(set) << "\n";
        ofs.close();
        return true;
    } catch (const std::exception &ex) {
        if (err != nullptr)
            *err = ex.what();
        return false;
    }
}

void SupportSetStore::load_dir(const std::string &path, bool read_only)
{
    boost::system::error_code ec;
    if (! fs::exists(path, ec) || ec)
        return;
    // Mirrors PresetCollection::load_presets: walk the directory, keep the .json files, skip and
    // log anything that fails to parse.
    for (auto &entry : fs::directory_iterator(fs::path(path))) {
        boost::system::error_code ec2;
        if (! fs::is_regular_file(entry.path(), ec2) || ec2)
            continue;
        const std::string file_name = entry.path().filename().string();
        if (! Slic3r::is_json_file(file_name))
            continue;
        SupportSet  set;
        std::string err;
        if (! load_file(entry.path().string(), set, &err)) {
            BOOST_LOG_TRIVIAL(warning) << "support set: skipping " << entry.path().string() << ": " << err;
            continue;
        }
        // The account's own folder wins over the read-only "default" fallback.
        if (std::any_of(m_sets.begin(), m_sets.end(), [&set](const SupportSet &s) { return s.name == set.name; }))
            continue;
        set.read_only = read_only;
        m_sets.emplace_back(std::move(set));
    }
}

void SupportSetStore::reload()
{
    m_sets.clear();
    const std::string own      = dir();
    const std::string fallback = fallback_dir();
    load_dir(own, false);
    // §5 item 5: sets saved before a login (or by another account) stay usable, read-only.
    if (fallback != own)
        load_dir(fallback, true);
    std::sort(m_sets.begin(), m_sets.end(), [](const SupportSet &a, const SupportSet &b) {
        return boost::ilexicographical_compare(a.name, b.name);
    });
}

const SupportSet* SupportSetStore::find(const std::string &name) const
{
    auto it = std::find_if(m_sets.begin(), m_sets.end(), [&name](const SupportSet &s) { return s.name == name; });
    return it == m_sets.end() ? nullptr : &*it;
}

// Pick a file path in `directory` for `name`, avoiding files that belong to a different set.
// On a collision append " (2)", " (3)", ... to the *file* name only.
static std::string support_set_file_path(const std::string &directory, const std::string &name, const std::string &reuse)
{
    if (! reuse.empty())
        return reuse;
    const std::string stem = sanitize_support_set_filename(name);
    for (int i = 1; i < 1000; ++ i) {
        const std::string candidate =
            (fs::path(directory) / (i == 1 ? stem + ".json" : stem + " (" + std::to_string(i) + ").json")).make_preferred().string();
        boost::system::error_code ec;
        if (! fs::exists(candidate, ec) || ec)
            return candidate;
    }
    return (fs::path(directory) / (stem + ".json")).make_preferred().string();
}

bool SupportSetStore::save(const SupportSet &set, std::string *err)
{
    if (set.name.empty()) {
        if (err != nullptr)
            *err = _u8L("A support set needs a name.");
        return false;
    }
    const std::string directory = dir();
    // Overwrite in place only when the existing set lives in this account's own folder; a set
    // read from the shared "default" folder is shadowed by a new file instead of being edited.
    std::string reuse;
    if (const SupportSet *existing = this->find(set.name); existing != nullptr && ! existing->read_only)
        reuse = existing->file;

    SupportSet to_write = set;
    if (to_write.created.empty())
        to_write.created = iso_utc_now();
    if (to_write.app_version.empty())
        to_write.app_version = SLIC3R_VERSION;

    const std::string path = support_set_file_path(directory, set.name, reuse);
    if (! save_file(path, to_write, err))
        return false;
    this->reload();
    return true;
}

bool SupportSetStore::rename(const std::string &from, const std::string &to, std::string *err)
{
    const SupportSet *existing = this->find(from);
    if (existing == nullptr) {
        if (err != nullptr)
            *err = Slic3r::format(_u8L("There is no support set named \"%1%\"."), from);
        return false;
    }
    if (existing->read_only) {
        if (err != nullptr)
            *err = _u8L("This support set is shared with every account on this computer and cannot be renamed here.");
        return false;
    }
    if (to.empty()) {
        if (err != nullptr)
            *err = _u8L("A support set needs a name.");
        return false;
    }
    if (to == from)
        return true;
    if (this->find(to) != nullptr) {
        if (err != nullptr)
            *err = Slic3r::format(_u8L("A support set named \"%1%\" already exists."), to);
        return false;
    }

    SupportSet       renamed  = *existing;
    const std::string old_file = existing->file;
    renamed.name = to;
    renamed.file.clear();
    const std::string path = support_set_file_path(dir(), to, std::string());
    if (! save_file(path, renamed, err))
        return false;
    boost::system::error_code ec;
    if (! old_file.empty() && old_file != path)
        fs::remove(old_file, ec);
    this->reload();
    return true;
}

bool SupportSetStore::remove(const std::string &name, std::string *err)
{
    const SupportSet *existing = this->find(name);
    if (existing == nullptr) {
        if (err != nullptr)
            *err = Slic3r::format(_u8L("There is no support set named \"%1%\"."), name);
        return false;
    }
    if (existing->read_only) {
        if (err != nullptr)
            *err = _u8L("This support set is shared with every account on this computer and cannot be deleted here.");
        return false;
    }
    boost::system::error_code ec;
    fs::remove(existing->file, ec);
    if (ec) {
        if (err != nullptr)
            *err = ec.message();
        return false;
    }
    this->reload();
    return true;
}

} // namespace Slic3r
