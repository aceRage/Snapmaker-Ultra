#ifndef slic3r_GUI_PresetMirror_hpp_
#define slic3r_GUI_PresetMirror_hpp_

#include <string>

namespace Slic3r { namespace GUI {

// One-way mirror of the logged-in Bambu Studio user's CUSTOM print + filament presets into the
// fork's user\default\ directory so they are usable/visible in this slicer too. Bambu Studio stays
// authoritative; this never writes back to BS. Behaviour:
//  - copies only when the fork copy is missing OR older than the BS preset (updated_time / mtime);
//  - manifest-tracked (user\default\.bs_mirror_manifest.json): it only ever touches files it owns,
//    so fork-native presets are never overwritten (name collisions: fork-native wins at load);
//  - respects a user deletion of a mirrored preset (won't re-pull it unless BS edited it since);
//  - blanks sync_info in the copied .info so the mirrored presets are inert to the fork's cloud
//    delete/upload paths; carries the base\ inheritance cache so inherits resolve;
//  - excludes machine (printer) presets.
// Sources: %APPDATA%\BambuStudio\user\<uid>\ preferred, %APPDATA%\BambuStudioBeta\user\<uid>\ fallback.
// Call BEFORE preset_bundle->load_presets() so the copies are on disk when it loads. Gated by the
// AppConfig "sync_bambu_user_presets" toggle (default on). logged_in_uid may be empty (then the
// newest numeric uid dir is used). Returns the number of preset files copied/updated this run
// (0 = nothing new — caller can skip a preset reload).
int mirror_bambu_user_presets(const std::string& logged_in_uid);

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_PresetMirror_hpp_
