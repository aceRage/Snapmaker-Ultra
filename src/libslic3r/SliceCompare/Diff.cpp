#include "Diff.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <utility>

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

// Quantize a coordinate (mm) to 10 um integer resolution.
inline int64_t quantize10um(float v)
{
    return static_cast<int64_t>(std::lround((double)v * 100.0));
}

// Direction-insensitive segment key: both endpoints quantized to 10 um, with
// the lesser (x,y) tuple ordered first so a segment and its reverse collide.
struct SegKey {
    int64_t x0, y0, x1, y1;
    bool operator<(const SegKey& o) const
    {
        if (x0 != o.x0) return x0 < o.x0;
        if (y0 != o.y0) return y0 < o.y0;
        if (x1 != o.x1) return x1 < o.x1;
        return y1 < o.y1;
    }
};

SegKey make_seg_key(const Seg& s)
{
    int64_t qx0 = quantize10um(s.x0), qy0 = quantize10um(s.y0);
    int64_t qx1 = quantize10um(s.x1), qy1 = quantize10um(s.y1);
    if (std::make_pair(qx1, qy1) < std::make_pair(qx0, qy0)) {
        std::swap(qx0, qx1);
        std::swap(qy0, qy1);
    }
    return SegKey{qx0, qy0, qx1, qy1};
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

SegDiff diff_segments(const LayerRec& a, const LayerRec& b, double rescue_radius)
{
    SegDiff out;

    // Exact match pass: dedupe each side to one representative Seg per
    // canonical (direction-insensitive, 10 um quantized) key, then intersect.
    std::map<SegKey, Seg> mapA, mapB;
    for (const auto& s : a.segs)
        mapA.emplace(make_seg_key(s), s);
    for (const auto& s : b.segs)
        mapB.emplace(make_seg_key(s), s);

    std::vector<SegKey> a_only_keys, b_only_keys;
    for (const auto& kv : mapA) {
        if (mapB.count(kv.first))
            out.both.push_back(kv.second);
        else
            a_only_keys.push_back(kv.first);
    }
    for (const auto& kv : mapB) {
        if (!mapA.count(kv.first))
            b_only_keys.push_back(kv.first);
    }

    // Rescue pass: index unclaimed B-only segments by midpoint in a 1 mm grid,
    // then for each unclaimed A-only segment search the 3x3 neighborhood of
    // its midpoint cell for a same-length, nearby, unclaimed B partner.
    auto midpoint = [](const Seg& s) {
        return std::make_pair((double)(s.x0 + s.x1) * 0.5, (double)(s.y0 + s.y1) * 0.5);
    };
    auto seg_len = [](const Seg& s) {
        return std::hypot((double)s.x1 - s.x0, (double)s.y1 - s.y0);
    };
    auto cell_of = [](double x, double y) {
        return std::make_pair((int)std::floor(x), (int)std::floor(y)); // 1 mm grid
    };

    std::map<std::pair<int, int>, std::vector<SegKey>> grid;
    for (const SegKey& k : b_only_keys) {
        const auto mid = midpoint(mapB.at(k));
        grid[cell_of(mid.first, mid.second)].push_back(k);
    }

    std::set<SegKey> claimed_b;
    std::vector<SegKey> a_only_remaining;
    for (const SegKey& ka : a_only_keys) {
        const Seg& sa = mapA.at(ka);
        const auto mid_a = midpoint(sa);
        const auto cell = cell_of(mid_a.first, mid_a.second);
        const double len_a = seg_len(sa);

        bool found = false;
        double best_dist = 0.0;
        SegKey best{};
        for (int dx = -1; dx <= 1; ++dx) {
            for (int dy = -1; dy <= 1; ++dy) {
                auto it = grid.find({cell.first + dx, cell.second + dy});
                if (it == grid.end())
                    continue;
                for (const SegKey& kb : it->second) {
                    if (claimed_b.count(kb))
                        continue;
                    const Seg& sb = mapB.at(kb);
                    if (std::abs(len_a - seg_len(sb)) >= 0.2)
                        continue;
                    const auto mid_b = midpoint(sb);
                    const double dist = std::hypot(mid_a.first - mid_b.first, mid_a.second - mid_b.second);
                    if (dist > rescue_radius)
                        continue;
                    if (!found || dist < best_dist) {
                        found = true;
                        best_dist = dist;
                        best = kb;
                    }
                }
            }
        }

        if (found) {
            claimed_b.insert(best);
            out.jitter.push_back(sa);
        } else {
            a_only_remaining.push_back(ka);
        }
    }

    for (const SegKey& ka : a_only_remaining)
        out.a_only.push_back(mapA.at(ka));
    for (const SegKey& kb : b_only_keys)
        if (!claimed_b.count(kb))
            out.b_only.push_back(mapB.at(kb));

    return out;
}

}} // namespaces
