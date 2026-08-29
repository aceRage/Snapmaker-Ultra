#ifndef slic3r_SliceCompare_Snapshot_hpp_
#define slic3r_SliceCompare_Snapshot_hpp_

#include <string>
#include <map>
#include <set>
#include <vector>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <utility>
#include <optional>

namespace Slic3r {
    struct GCodeProcessorResult;  // forward declaration (matches struct GCodeProcessorResult in GCode/GCodeProcessor.hpp)
    namespace SliceCompare {

struct Seg { float x0, y0, x1, y1; uint8_t role; };

struct LayerRec {
    double z = 0.0;                                   // representative z (mm)
    double extrusion_mm = 0.0;
    std::map<uint8_t, double> feature_seconds;        // key = ExtrusionRole
    std::map<uint8_t, double> feature_extrusion_mm;   // key = ExtrusionRole
    std::set<std::pair<int16_t, int16_t>> cells;      // 10 mm grid fingerprint
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool  has_bbox = false;
    std::vector<Seg> segs;
    double seconds() const;                           // sum of feature_seconds
};

struct Snapshot {
    std::string label;
    std::string source;                               // "session" or file path
    std::map<std::string, std::string> config;
    double est_seconds = 0, filament_mm = 0, filament_g = 0, max_speed = 0;
    int    layer_count = 0;
    std::map<int, LayerRec> layers;                   // key = lround(z*100)
};

// config: already-serialized key->value map (caller-provided; see Task 7/6)
Snapshot build_snapshot(const GCodeProcessorResult& result,
                        std::map<std::string, std::string> config,
                        const std::string& label,
                        const std::string& source);

// Parse "; key = value" lines between CONFIG_BLOCK_START/END (Orca/Bambu),
// falling back to scanning "; key = value" lines in the first/last 400 lines
// of the file (PrusaSlicer-style config headers/footers) when no block is found.
std::map<std::string, std::string> parse_gcode_config(const std::string& gcode_path);

// .gcode: GCodeProcessor::process_file -> build_snapshot.
// .3mf / .gcode.3mf: extract Metadata/plate_*.gcode (miniz) to a temp file first,
// then recurse; label/source are taken from the ORIGINAL 3mf path.
// Returns nullopt + error message on failure.
struct FileLoadResult {
    std::optional<Snapshot> snapshot;
    std::string error;
};
FileLoadResult load_snapshot_from_file(const std::string& path);

class SnapshotStore {
public:
    static SnapshotStore& instance();
    // returns id of the stored snapshot; evicts oldest beyond capacity (8)
    int  add(Snapshot snap);
    std::shared_ptr<const Snapshot> get(int id) const;             // null if evicted
    std::vector<std::pair<int, std::string>> list() const;         // (id, label), newest first
    void clear();
private:
    mutable std::mutex m_mutex;
    int m_next_id = 1;
    std::deque<std::pair<int, std::shared_ptr<Snapshot>>> m_items; // capacity 8
};

    } // SliceCompare
} // Slic3r

#endif
