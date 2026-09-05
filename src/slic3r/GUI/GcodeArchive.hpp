#pragma once

#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace Slic3r {
namespace GUI {

// Ultra: the G-code archive.
//
// Every file this PC uploads to a printer - from the desktop's own Send / Print dialogs, from the
// Snapmaker preprint page, or from the phone through RemoteSend - is copied into a folder the user
// picks, next to a JSON sidecar saying which printer it went to and what the plate was. The phone's
// "Reprints" tab (stage 2) then has a list it can send again without the project being open.
//
// Rules for callers:
//   * archiving must never fail a send. Everything here swallows its own errors and logs them.
//   * archive() copies the file, so it may be called after the sender has finished with it, but
//     before anything deletes it (the print-host queue removes its temporary source afterwards).
//   * meta_for_plate() reads the plater and the presets and so has to run on the GUI thread; it
//     marshals there itself when called from a worker, so any hook may call it.
namespace GcodeArchive {

struct Filament
{
    int         index { 0 };
    std::string type;
    std::string colour;
    double      grams { 0.0 };
};

// What a send knows about itself. The printer identity and the mode are the caller's; everything
// else meta_for_plate() fills in from the plate.
struct Meta
{
    std::string printer_id;    // the id /api/printers uses: a Bambu dev_id, "host", "connect", "sm:<id>"
    std::string printer_kind;  // bambu | snapmaker | printhost | connect
    std::string printer_name;
    std::string printer_model;
    std::string source { "desktop" }; // desktop | phone
    std::string mode { "upload" };    // upload | print
    std::string file_name;     // the name the printer was given ("plate_1.gcode"); "" = the source's

    int         plate { -1 };  // 0-based
    std::string plate_name;
    std::string project_title, project_path;
    std::vector<Filament> filaments;
    int         estimated_time_s { 0 };
    double      estimated_weight_g { 0.0 };
    std::string thumbnail_png;  // raw PNG bytes of the plate's small thumbnail, "" = none
};

// One archived send, as the sidecar holds it.
struct Record
{
    std::string    id;         // the sidecar's stem, unique in the folder
    long long      time { 0 }; // unix seconds
    std::string    file;       // the archived file's name (not its path)
    std::string    path;       // its full path on this PC - never leaves the instance API
    long long      size { 0 };
    std::string    sha256;
    bool           has_thumbnail { false };
    std::string    thumbnail_path;
    nlohmann::json json;       // the whole sidecar, as read
};

// Is the archive switched on (app_config ultra_gcode_archive)?
bool enabled();
// The folder, from app_config ultra_gcode_archive_dir, defaulting to <datadir>/gcode_archive.
std::string dir();
// ultra_gcode_archive_max, clamped to 1..10000.
int max_records();

// GUI thread or any thread (it marshals). Everything about the plate a sidecar wants.
Meta meta_for_plate(int plate, const std::string& mode);

// Any thread (it marshals). The name the Device tab shows for a Bambu dev_id, or the id itself.
std::string bambu_printer_name(const std::string& dev_id);

// Any thread. Copies `sent_file_path` into the archive and writes its sidecar; then trims the
// folder to max_records(). Returns the record, or a record with an empty id when nothing was
// archived (switched off, no such file, no room on disk...). Never throws.
Record archive(const std::string& sent_file_path, const Meta& meta);

// Any thread. The sidecars, newest first. `printer_id_filter` empty = all of them.
std::vector<Record> list(const std::string& printer_id_filter = "");

// Any thread. One record by id, or a record with an empty id.
Record find(const std::string& id);

// Any thread. Removes the file, its sidecar and its thumbnail. True when the sidecar was there.
bool remove(const std::string& id);

} // namespace GcodeArchive
} // namespace GUI
} // namespace Slic3r
