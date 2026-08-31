#include "WallSampleIndex.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {

namespace {

// Grid cell (floor(x/cell), floor(y/cell)) that a scaled point falls into.
inline std::pair<int32_t, int32_t> cell_of(const Point &pt, double cell)
{
    int32_t cx = int32_t(std::floor(double(pt.x()) / cell));
    int32_t cy = int32_t(std::floor(double(pt.y()) / cell));
    return { cx, cy };
}

// Determinism: squared distance ascending, ties broken by (extruder, object_key).
inline bool sample_less(const std::pair<const WallSample*, double> &a,
                         const std::pair<const WallSample*, double> &b)
{
    if (a.second != b.second)
        return a.second < b.second;
    if (a.first->extruder != b.first->extruder)
        return a.first->extruder < b.first->extruder;
    return a.first->object_key < b.first->object_key;
}

} // namespace

WallSampleIndex::WallSampleIndex(double cell_mm) : m_cell(scale_(cell_mm))
{
}

void WallSampleIndex::add_polyline(const Points &poly, unsigned extruder, size_t object_key,
                                    double spacing_mm, std::map<unsigned, size_t> *sample_count_out)
{
    if (poly.empty())
        return;

    auto insert_sample = [this, extruder, object_key, sample_count_out](const Point &pt) {
        std::pair<int32_t, int32_t> key = cell_of(pt, m_cell);
        m_cells[key].push_back(WallSample{ pt, extruder, object_key });
        ++m_count;
        // CHAMELEON_DEBUG: see this function's own header comment (WallSampleIndex.hpp)
        // - null when the caller isn't in debug mode, so this is one pointer compare.
        if (sample_count_out)
            ++(*sample_count_out)[extruder];
    };

    if (poly.size() == 1) {
        insert_sample(poly.front());
        return;
    }

    const double step = scale_(spacing_mm);

    for (size_t i = 0; i + 1 < poly.size(); ++i) {
        const Point &p0 = poly[i];
        const Point &p1 = poly[i + 1];
        insert_sample(p0); // include segment start

        const double dx  = double(p1.x() - p0.x());
        const double dy  = double(p1.y() - p0.y());
        const double len = std::sqrt(dx * dx + dy * dy);

        if (len > 0 && step > 0) {
            for (double t = step; t < len; t += step) {
                const double frac = t / len;
                Point sample(double(p0.x()) + dx * frac, double(p0.y()) + dy * frac);
                insert_sample(sample);
            }
        }
    }
    insert_sample(poly.back()); // include final endpoint
}

std::vector<std::pair<const WallSample*, double>> WallSampleIndex::knn(const Point &pt, size_t k) const
{
    std::vector<std::pair<const WallSample*, double>> candidates;
    if (k == 0 || m_cells.empty())
        return candidates;

    const std::pair<int32_t, int32_t> center = cell_of(pt, m_cell);
    const double px = double(pt.x());
    const double py = double(pt.y());

    auto squared_dist = [px, py](const Point &p) {
        const double dx = double(p.x()) - px;
        const double dy = double(p.y()) - py;
        return dx * dx + dy * dy;
    };

    auto collect_cell = [this, &candidates, &squared_dist](int32_t cx, int32_t cy) {
        auto it = m_cells.find(std::make_pair(cx, cy));
        if (it == m_cells.end())
            return;
        for (const WallSample &s : it->second)
            candidates.emplace_back(&s, squared_dist(s.pt));
    };

    collect_cell(center.first, center.second);

    int radius = 0;
    while (true) {
        const size_t target = std::min(k, m_count);
        if (candidates.size() >= target) {
            std::sort(candidates.begin(), candidates.end(), sample_less);
            const double kth_d2 = candidates[target - 1].second;
            // Minimum possible distance from pt to any cell outside the square of
            // cells already scanned (chebyshev radius 'radius' around its cell).
            const double x0 = double(center.first - radius) * m_cell;
            const double x1 = double(center.first + radius + 1) * m_cell;
            const double y0 = double(center.second - radius) * m_cell;
            const double y1 = double(center.second + radius + 1) * m_cell;
            const double min_next = std::min({ px - x0, x1 - px, py - y0, y1 - py });
            const bool all_collected = candidates.size() >= m_count;
            if (all_collected || min_next * min_next > kth_d2)
                break;
        }

        ++radius;
        const int32_t cx0 = center.first - radius;
        const int32_t cx1 = center.first + radius;
        const int32_t cy0 = center.second - radius;
        const int32_t cy1 = center.second + radius;
        for (int32_t cx = cx0; cx <= cx1; ++cx) {
            collect_cell(cx, cy0);
            if (cy1 != cy0)
                collect_cell(cx, cy1);
        }
        for (int32_t cy = cy0 + 1; cy <= cy1 - 1; ++cy) {
            collect_cell(cx0, cy);
            if (cx1 != cx0)
                collect_cell(cx1, cy);
        }
    }

    std::sort(candidates.begin(), candidates.end(), sample_less);
    if (candidates.size() > k)
        candidates.resize(k);
    return candidates;
}

size_t WallSampleIndex::size() const
{
    return m_count;
}

bool WallSampleIndex::empty() const
{
    return m_count == 0;
}

} // namespace Slic3r
