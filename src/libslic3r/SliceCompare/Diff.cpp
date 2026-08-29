#include "Diff.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace Slic3r { namespace SliceCompare {

namespace {

constexpr int    Z_TOL_KEYS       = 5;     // 0.05 mm on the 10 um (lround(z*100)) key
constexpr double MATCH_TOL        = 0.5;
constexpr double IDENTICAL_OVERLAP = 0.85;

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
// `feature_seconds` and `feature_extrusion_mm` are already keyed by role, so
// seconds and extrusion mm accumulate directly and exactly. `segs` carry a
// role per segment, so path length (sum of hypot per seg) is exact per role
// too.
std::map<uint8_t, RoleAccum> accumulate_features(const Snapshot& snap)
{
    std::map<uint8_t, RoleAccum> out;
    for (const auto& lkv : snap.layers) {
        const LayerRec& layer = lkv.second;

        for (const auto& fs : layer.feature_seconds)
            out[fs.first].sec += fs.second;
        for (const auto& fm : layer.feature_extrusion_mm)
            out[fm.first].mm += fm.second;
        for (const auto& seg : layer.segs)
            out[seg.role].len += std::hypot((double)seg.x1 - seg.x0, (double)seg.y1 - seg.y0);
    }
    return out;
}

// Sum of feature_seconds for both support roles (erSupportMaterial +
// erSupportMaterialInterface); used for the SUPPORT-CHANGED flag.
double support_seconds(const LayerRec& l)
{
    double s = 0.0;
    auto it = l.feature_seconds.find((uint8_t)erSupportMaterial);
    if (it != l.feature_seconds.end())
        s += it->second;
    it = l.feature_seconds.find((uint8_t)erSupportMaterialInterface);
    if (it != l.feature_seconds.end())
        s += it->second;
    return s;
}

// Jaccard overlap of two cell fingerprints: |intersection| / |union|.
// 1.0 if either set is empty (nothing to disagree about).
double cell_overlap(const LayerRec& la, const LayerRec& lb)
{
    if (la.cells.empty() || lb.cells.empty())
        return 1.0;
    size_t inter = 0;
    for (const auto& c : la.cells)
        if (lb.cells.count(c))
            ++inter;
    const size_t uni = la.cells.size() + lb.cells.size() - inter;
    return uni > 0 ? (double)inter / (double)uni : 1.0;
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

LayerDiff diff_layers(const Snapshot& a, const Snapshot& b)
{
    LayerDiff out;

    auto push_matched = [&](const LayerRec& la, const LayerRec& lb, int ka, int kb) {
        LayerMatch m;
        m.zkey_a = ka;
        m.zkey_b = kb;
        m.d_seconds   = lb.seconds() - la.seconds();
        m.d_extrusion = lb.extrusion_mm - la.extrusion_mm;
        m.overlap     = cell_overlap(la, lb);

        const bool identical_like = std::abs(m.d_extrusion) <= MATCH_TOL &&
                                     std::abs(m.d_seconds)   <= MATCH_TOL &&
                                     m.overlap > IDENTICAL_OVERLAP;
        m.changed = !identical_like;

        if (m.overlap < 0.5 && std::abs(m.d_extrusion) < MATCH_TOL)
            m.flags.push_back("RELOCATED");

        if (la.has_bbox && lb.has_bbox) {
            const double wa = la.bx1 - la.bx0, ha = la.by1 - la.by0;
            const double wb = lb.bx1 - lb.bx0, hb = lb.by1 - lb.by0;
            if (std::abs(wa - wb) > 5.0 || std::abs(ha - hb) > 5.0)
                m.flags.push_back("GEOMETRY-CHANGED");
        }

        if (std::abs(support_seconds(lb) - support_seconds(la)) > MATCH_TOL)
            m.flags.push_back("SUPPORT-CHANGED");

        if (m.overlap < 0.5 && m.d_extrusion > MATCH_TOL)
            m.flags.push_back("MATERIAL-ADDED-NEW-REGION");

        out.rows.push_back(std::move(m));
        ++out.matched;
        if (out.rows.back().changed)
            ++out.changed;
        else
            ++out.identical;
    };

    auto ia = a.layers.begin();
    auto ib = b.layers.begin();
    while (ia != a.layers.end() && ib != b.layers.end()) {
        const int ka = ia->first, kb = ib->first;
        if (std::abs(ka - kb) <= Z_TOL_KEYS) {
            push_matched(ia->second, ib->second, ka, kb);
            ++ia; ++ib;
        } else if (ka < kb) {
            LayerMatch m; m.zkey_a = ka; m.zkey_b = -1;
            out.rows.push_back(std::move(m));
            ++out.a_only;
            ++ia;
        } else {
            LayerMatch m; m.zkey_a = -1; m.zkey_b = kb;
            out.rows.push_back(std::move(m));
            ++out.b_only;
            ++ib;
        }
    }
    for (; ia != a.layers.end(); ++ia) {
        LayerMatch m; m.zkey_a = ia->first; m.zkey_b = -1;
        out.rows.push_back(std::move(m));
        ++out.a_only;
    }
    for (; ib != b.layers.end(); ++ib) {
        LayerMatch m; m.zkey_a = -1; m.zkey_b = ib->first;
        out.rows.push_back(std::move(m));
        ++out.b_only;
    }

    // biggest_zkey_a: matched+changed row maximizing |d_seconds|, ties by |d_extrusion|.
    double best_d_seconds = -1.0, best_d_extrusion = -1.0;
    for (const auto& row : out.rows) {
        if (row.zkey_a < 0 || row.zkey_b < 0 || !row.changed)
            continue;
        const double ads = std::abs(row.d_seconds);
        const double ade = std::abs(row.d_extrusion);
        if (ads > best_d_seconds || (ads == best_d_seconds && ade > best_d_extrusion)) {
            best_d_seconds   = ads;
            best_d_extrusion = ade;
            out.biggest_zkey_a = row.zkey_a;
        }
    }

    return out;
}

}} // namespaces
