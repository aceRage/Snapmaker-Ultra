#ifndef slic3r_SliceCompare_Diff_hpp_
#define slic3r_SliceCompare_Diff_hpp_

#include "Snapshot.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace Slic3r { namespace SliceCompare {

struct ConfigRow { std::string key, a, b; };            // a/b empty = absent
std::vector<ConfigRow> diff_configs(const Snapshot& a, const Snapshot& b);

struct FeatureRow {
    uint8_t role;
    double sec_a = 0, sec_b = 0, mm_a = 0, mm_b = 0, len_a = 0, len_b = 0;
};
std::vector<FeatureRow> diff_features(const Snapshot& a, const Snapshot& b); // sorted by sec_a+sec_b desc

struct LayerMatch {
    int zkey_a = -1, zkey_b = -1;         // -1 = no counterpart (a_only/b_only)
    double d_seconds = 0, d_extrusion = 0;
    double overlap = 1.0;                 // cell Jaccard, matched only
    std::vector<std::string> flags;       // RELOCATED, GEOMETRY-CHANGED, SUPPORT-CHANGED, MATERIAL-ADDED-NEW-REGION
    bool changed = false;
};
struct LayerDiff {
    std::vector<LayerMatch> rows;         // z-ascending; includes unmatched
    int matched = 0, identical = 0, changed = 0, a_only = 0, b_only = 0;
    int biggest_zkey_a = -1;              // matched layer w/ max |d_seconds| (ties: |d_extrusion|)
};
LayerDiff diff_layers(const Snapshot& a, const Snapshot& b);

struct SegDiff { std::vector<Seg> both, a_only, b_only, jitter; };
// Compare one matched layer pair's segments. Endpoints are quantized to 10 um
// and matched direction-insensitively; segments that don't match exactly are
// rescued as `jitter` if a same-length unclaimed counterpart lies within
// rescue_radius (mm) of the midpoint, otherwise they land in a_only/b_only.
SegDiff diff_segments(const LayerRec& a, const LayerRec& b, double rescue_radius = 0.3);

    } // SliceCompare
} // Slic3r

#endif
