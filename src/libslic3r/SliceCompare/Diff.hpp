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

    } // SliceCompare
} // Slic3r

#endif
