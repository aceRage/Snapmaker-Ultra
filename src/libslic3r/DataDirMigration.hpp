#ifndef slic3r_DataDirMigration_hpp_
#define slic3r_DataDirMigration_hpp_

#include <string>

namespace Slic3r {

// First start after the rename: the data directory moved with the app key, from
// %APPDATA%\Snapmaker_Orca to %APPDATA%\UltraOne (and the matching paths on macOS and
// Linux). Everything the user cares about lives in there - the presets, and inside hub/
// the phone token, the VAPID key pair every Web Push subscription is bound to, the relay
// destinations and the tailnet allow-list - so it has to come across, once, without
// being asked about.
//
// The copy is a copy and never a move. The old directory is left byte-for-byte as it
// was, so the previous install keeps working and a user who dislikes the new build has a
// real rollback rather than a promise of one.
//
// The old directory's name comes from SLIC3R_LEGACY_APP_KEY in common_func.hpp and from
// nowhere else, so a later release drops all of this by deleting one constant, one call
// and this file.
struct DataDirMigrationResult
{
    bool        ran               = false; // a copy actually happened on this call
    bool        skipped_new_exists = false;
    bool        skipped_no_old     = false;
    std::string old_dir;
    std::string new_dir;
    size_t      files_copied         = 0;
    unsigned long long bytes_copied  = 0;
    size_t      conf_paths_rewritten = 0;
    std::string error; // empty on success; non-empty means nothing was published
};

// `parent` is the directory both data dirs are siblings in (%APPDATA%,
// ~/Library/Application Support, $XDG_CONFIG_HOME). `new_dir` is where we live now.
// Does nothing at all if `new_dir` already exists, or if the old one does not.
// `include_archive` false leaves hub/saves and hub/uploads behind - the G-code archive,
// which is the bulk of the bytes and the only part that is genuinely optional.
DataDirMigrationResult migrate_data_dir(const std::string& parent,
                                        const std::string& new_dir,
                                        bool               include_archive = true);

// The name of the directory we used to live in, for callers that need to say it.
const char* legacy_data_dir_name();

} // namespace Slic3r

#endif // slic3r_DataDirMigration_hpp_
