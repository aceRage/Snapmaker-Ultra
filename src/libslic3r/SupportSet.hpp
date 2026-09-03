#ifndef slic3r_SupportSet_hpp_
#define slic3r_SupportSet_hpp_

// Ultra (support sets): a support set is a reusable, named snippet of the Support-category
// process settings, saved as JSON next to the user's presets. It is deliberately NOT a preset:
// there is no Preset::Type, no PresetCollection, no compatibility condition and no inheritance.
// Applying a set copies its values into the current process settings, which then show as
// modified exactly like a hand edit.
//
// Sets are portable across projects and printers because the support-interface filament travels
// as a *type* ("same" / "soluble" / an explicit filament_type string) and is resolved to a
// filament slot at apply time - see resolve_interface_filament().
//
// The plan (docs/superpowers/plans/2026-09-02-support-sets-and-groups.md §1.1) put this file in
// src/slic3r/GUI. It lives in libslic3r instead because every helper here is wx-free and the
// unit tests link only libslic3r; the one thing that genuinely needs the GUI - which per-account
// preset folder is active - is handed in by SupportSetStore::set_preset_folder().

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json_fwd.hpp>

namespace Slic3r {

class DynamicPrintConfig;

// Bumped only when the on-disk shape changes incompatibly. Readers accept anything <= this.
static constexpr int SUPPORT_SET_VERSION = 1;
// Value of the "type" field, so a stray JSON in the folder is recognised as not-a-support-set.
#define SUPPORT_SET_TYPE      "support_set"
// Subdirectory of <datadir>/user/<preset_folder>/ holding the *.json files. A sibling of
// process/ filament/ machine/, which PresetCollection::load_presets never walks into.
#define SUPPORT_SET_SUBDIR    "support_set"

struct SupportSet
{
    // Authoritative identity: this is what a support group references, not the file name.
    std::string name;
    std::string description;
    // ISO-8601 UTC, informational only ("2026-09-02T14:05:11Z").
    std::string created;
    std::string app_version;
    // Absolute path of the file this set was read from. Empty for a set built in memory.
    std::string file;
    // Set when the file was read from the read-only "default" fallback folder rather than from
    // the account's own folder. Such a set can be applied but not renamed or deleted.
    bool        read_only = false;

    // "same" | "soluble" | an exact filament_type enum string ("PVA", "PLA", "PETG", ...).
    std::string interface_filament_type = "same";

    // ConfigBase::opt_serialize strings keyed by print_config_def key, exactly like a preset json.
    std::map<std::string, std::string> values;
};

// ---------------------------------------------------------------------------------------------
// Pure helpers - no data_dir(), no AppConfig, no wx. These are what the unit tests exercise.
// ---------------------------------------------------------------------------------------------

// The keys a support set may carry: every PrintObjectConfig member whose print_config_def
// category is "Support", minus support_set_excluded_keys(). Built once from the def, so a future
// support key joins sets automatically. Sorted.
const std::vector<std::string>& support_set_keys();

// Keys that carry the "Support" category but must never travel in a set. Filament slot indices
// are not portable (they travel as SupportSet::interface_filament_type instead); raft geometry,
// enforced layer counts and the brim/bridge keys are not support-interface settings at all.
const std::vector<std::string>& support_set_excluded_keys();

bool support_set_is_allowed_key(const std::string &key);

// Capture the Support-category values of `cfg` (usually the edited process preset) into a set.
SupportSet support_set_from_config(const DynamicPrintConfig &cfg, const std::string &name);

// Write the set's values into `out` as a delta ready for Tab::load_config. `full_config` supplies
// filament_type / filament_soluble for the interface-filament resolution; `warning` receives a
// user-facing message when the requested filament type is not loaded (never fatal).
void support_set_apply_to(const SupportSet          &set,
                          DynamicPrintConfig        &out,
                          const DynamicPrintConfig  &full_config,
                          std::string               *warning = nullptr);

// Resolve an interface_filament_type to a 1-based support_interface_filament slot, or 0 for
// "Default / use the current filament". Ties always break to the lowest slot, so the answer is
// deterministic and printer-independent. See the plan's §3.3 table.
int resolve_interface_filament(const std::string        &type,
                               const DynamicPrintConfig &full_config,
                               std::string              *warning = nullptr);

// The reverse map, used by "Save current as...": slot 0 -> "same", a soluble slot -> "soluble",
// otherwise that slot's filament_type.
std::string support_set_interface_filament_type(const DynamicPrintConfig &full_config);

// Replace every character outside [A-Za-z0-9 _.-] with '_' and trim. Never returns an empty
// string. The result is a file *stem*, without the .json suffix.
std::string sanitize_support_set_filename(const std::string &name);

// JSON round trip. `dropped` collects keys that were skipped because they are not in
// support_set_keys() - a file carrying them still loads.
nlohmann::json support_set_to_json(const SupportSet &set);
bool           support_set_from_json(const nlohmann::json     &j,
                                     SupportSet               &set,
                                     std::string              *err     = nullptr,
                                     std::vector<std::string> *dropped = nullptr);

std::string support_set_to_json_string(const SupportSet &set);
bool        support_set_from_json_string(const std::string        &text,
                                         SupportSet               &set,
                                         std::string              *err     = nullptr,
                                         std::vector<std::string> *dropped = nullptr);

// ---------------------------------------------------------------------------------------------
// The on-disk store.
// ---------------------------------------------------------------------------------------------

class SupportSetStore
{
public:
    static SupportSetStore& instance();

    // libslic3r cannot reach the running AppConfig, so the GUI hands over
    // AppConfig::get("preset_folder") (empty means the "default" folder).
    static void        set_preset_folder(const std::string &folder);
    static std::string preset_folder();

    // <datadir>/user/<preset_folder>/support_set
    static std::string dir();
    // <datadir>/user/default/support_set - read-only, and equal to dir() for the default account.
    static std::string fallback_dir();

    // Enumerate both directories, the account folder winning on a name clash. Skips and logs
    // anything that fails to parse. Sorted by name, case-insensitively.
    void reload();

    const std::vector<SupportSet>& list() const { return m_sets; }
    const SupportSet*              find(const std::string &name) const;

    // Create or overwrite by name. Always writes into dir(), so saving over a fallback set
    // shadows it rather than editing the shared copy.
    bool save(const SupportSet &set, std::string *err = nullptr);
    bool rename(const std::string &from, const std::string &to, std::string *err = nullptr);
    bool remove(const std::string &name, std::string *err = nullptr);

    // Testing seam: read/write a single file without touching data_dir().
    static bool load_file(const std::string &path, SupportSet &set, std::string *err = nullptr);
    static bool save_file(const std::string &path, const SupportSet &set, std::string *err = nullptr);

private:
    SupportSetStore() = default;

    void load_dir(const std::string &path, bool read_only);

    std::vector<SupportSet> m_sets;
};

} // namespace Slic3r

#endif // slic3r_SupportSet_hpp_
