#ifndef slic3r_ElegooCanvas_hpp_
#define slic3r_ElegooCanvas_hpp_

#include <string>
#include <vector>

namespace Slic3r {

// CANVAS is Elegoo's multi-material unit for the Centauri Carbon / Centauri 2
// family. A printer reports its units over the same WebSocket ElegooLink already
// speaks (port 3030, path /websocket); command 324 answers with a "canvas_list",
// one entry per unit, each carrying a "tray_list" of slots.
//
// Ported from upstream OrcaSlicer PR #14885 ("Add CANVAS filament detection for
// Elegoo Centauri Carbon"), which is still open at the time of writing. Upstream
// hangs this off a PrinterAgent plugin interface (NetworkAgentFactory,
// ElegooPrinterAgent) and feeds the result into its DevFilaSystem device layer,
// neither of which exists in this fork. The parts that do not depend on either -
// the query, the tray parsing, and the type/colour normalisation - live here as
// plain functions so the send path can call them directly.

// One slot of one CANVAS unit, normalised away from the wire format.
struct CanvasTray
{
    int         slot_index   = 0;     // 0-based index across all units
    bool        has_filament = false;
    std::string tray_type;            // normalised material, e.g. "PLA", "PETG"
    std::string tray_color;           // RRGGBBAA, uppercase
    std::string tray_info_idx;        // OrcaFilamentLibrary filament id
    int         nozzle_temp  = 0;     // max nozzle temperature, 0 when unknown
};

struct CanvasInfo
{
    int                     unit_count = 0;  // number of CANVAS units reported
    std::vector<CanvasTray> trays;           // every slot of every connected unit

    bool empty() const { return trays.empty(); }
};

// Colour as the printer reports it ("#RRGGBB", "0xRRGGBB", "RRGGBB", "RRGGBBAA")
// to the uppercase RRGGBBAA this tree stores on AmsTray. Invalid input yields
// "00000000" rather than throwing, because a bad colour must not cost the slot.
std::string canvas_normalize_color(const std::string& color);

// Collapse a printer-reported material name onto the coarse family the filament
// presets are keyed by: "PLA Silk" and "PLA-CF" both answer "PLA".
std::string canvas_normalize_filament_type(const std::string& filament_type);

// Map a normalised material to the OrcaFilamentLibrary generic filament id, so a
// detected slot can preselect a real preset. Returns an empty string when the
// material has no generic counterpart - the caller then leaves the slot unmapped
// rather than guessing.
std::string canvas_generic_filament_id(const std::string& filament_type);

// Parse the payload of a command-324 reply. Split out from the query so it can be
// exercised without a printer. Returns false and sets `error` when the payload
// carries no usable canvas_list.
bool canvas_parse_payload(const std::string& payload, CanvasInfo& info, std::string& error);

// Ask the printer at `host` for its CANVAS state. Returns false and sets `error`
// on a connection, send, receive or parse failure. Blocking; call it off the GUI
// thread.
bool canvas_query(const std::string& host, CanvasInfo& info, std::string& error);

} // namespace Slic3r

#endif // slic3r_ElegooCanvas_hpp_
