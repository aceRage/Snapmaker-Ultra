#include "Diff.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace Slic3r { namespace SliceCompare {

namespace {

bool is_volatile_key(const std::string& key)
{
    static const std::set<std::string> exact = {
        "filename", "preset_name", "preset_names", "printer_settings_id",
        "print_settings_id", "filament_settings_id"
    };
    if (exact.count(key))
        return true;
    if (key.rfind("print_host", 0) == 0)
        return true;
    if (key.rfind("printhost_", 0) == 0)
        return true;
    return false;
}

struct RoleAccum { double sec = 0.0, mm = 0.0, len = 0.0; };

// Accumulate per-ExtrusionRole seconds/extrusion-mm/path-length for one snapshot.
//
// `feature_seconds` is already keyed by role, so seconds accumulate directly.
// `segs` carry a role per segment, so path length (sum of hypot per seg) is
// exact per role too. `LayerRec::extrusion_mm`, however, is only tracked as a
// single per-layer total (see Snapshot.cpp) — there is no per-role extrusion
// breakdown in the data model. We approximate a role's share of a layer's
// extrusion by that role's fraction of the layer's total path length, which
// matches the constant-cross-section assumption Snapshot.cpp already makes
// when converting filament_mm to filament_g.
std::map<uint8_t, RoleAccum> accumulate_features(const Snapshot& snap)
{
    std::map<uint8_t, RoleAccum> out;
    for (const auto& lkv : snap.layers) {
        const LayerRec& layer = lkv.second;

        for (const auto& fs : layer.feature_seconds)
            out[fs.first].sec += fs.second;

        std::map<uint8_t, double> role_len;
        double layer_len = 0.0;
        for (const auto& seg : layer.segs) {
            const double len = std::hypot((double)seg.x1 - seg.x0, (double)seg.y1 - seg.y0);
            role_len[seg.role] += len;
            layer_len += len;
        }
        for (const auto& rl : role_len)
            out[rl.first].len += rl.second;
        if (layer_len > 0.0)
            for (const auto& rl : role_len)
                out[rl.first].mm += layer.extrusion_mm * (rl.second / layer_len);
    }
    return out;
}

} // anonymous namespace

std::vector<ConfigRow> diff_configs(const Snapshot& a, const Snapshot& b)
{
    std::set<std::string> keys;
    for (const auto& kv : a.config) keys.insert(kv.first);
    for (const auto& kv : b.config) keys.insert(kv.first);

    std::vector<ConfigRow> rows;
    for (const auto& key : keys) {
        if (is_volatile_key(key))
            continue;
        auto ia = a.config.find(key);
        auto ib = b.config.find(key);
        const bool present_a = ia != a.config.end();
        const bool present_b = ib != b.config.end();
        const std::string va = present_a ? ia->second : std::string();
        const std::string vb = present_b ? ib->second : std::string();
        if (!present_a || !present_b || va != vb)
            rows.push_back({key, va, vb});
    }
    return rows;
}

std::vector<FeatureRow> diff_features(const Snapshot& a, const Snapshot& b)
{
    const std::map<uint8_t, RoleAccum> acc_a = accumulate_features(a);
    const std::map<uint8_t, RoleAccum> acc_b = accumulate_features(b);

    std::set<uint8_t> roles;
    for (const auto& kv : acc_a) roles.insert(kv.first);
    for (const auto& kv : acc_b) roles.insert(kv.first);

    std::vector<FeatureRow> rows;
    rows.reserve(roles.size());
    for (uint8_t role : roles) {
        FeatureRow row;
        row.role = role;
        auto ia = acc_a.find(role);
        if (ia != acc_a.end()) {
            row.sec_a = ia->second.sec;
            row.mm_a  = ia->second.mm;
            row.len_a = ia->second.len;
        }
        auto ib = acc_b.find(role);
        if (ib != acc_b.end()) {
            row.sec_b = ib->second.sec;
            row.mm_b  = ib->second.mm;
            row.len_b = ib->second.len;
        }
        rows.push_back(row);
    }

    std::sort(rows.begin(), rows.end(), [](const FeatureRow& x, const FeatureRow& y) {
        return (x.sec_a + x.sec_b) > (y.sec_a + y.sec_b);
    });
    return rows;
}

}} // namespaces
