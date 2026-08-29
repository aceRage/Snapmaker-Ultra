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

    } // SliceCompare
} // Slic3r

#endif
