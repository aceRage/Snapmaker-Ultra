#include "BrimFilament.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace Slic3r {

namespace {

// Path length (mm) of a polyline given as scaled points.
double path_length_mm(const Points &pts)
{
    double len = 0.0;
    for (size_t i = 0; i + 1 < pts.size(); ++i) {
        const double dx = double(pts[i + 1].x() - pts[i].x());
        const double dy = double(pts[i + 1].y() - pts[i].y());
        len += std::sqrt(dx * dx + dy * dy);
    }
    return unscale<double>(len);
}

// Resample `working` every sample_mm, preserving the exact first/last points
// and every original vertex (mirrors WallSampleIndex::add_polyline).
Points build_chain(const Points &working, double sample_mm)
{
    Points chain;
    if (working.empty())
        return chain;
    if (working.size() == 1) {
        chain.push_back(working.front());
        return chain;
    }

    const double step = scale_(sample_mm);
    for (size_t i = 0; i + 1 < working.size(); ++i) {
        const Point &p0 = working[i];
        const Point &p1 = working[i + 1];
        chain.push_back(p0);

        const double dx  = double(p1.x() - p0.x());
        const double dy  = double(p1.y() - p0.y());
        const double len = std::sqrt(dx * dx + dy * dy);

        if (len > 0 && step > 0) {
            for (double t = step; t < len; t += step) {
                const double frac = t / len;
                chain.emplace_back(double(p0.x()) + dx * frac, double(p0.y()) + dy * frac);
            }
        }
    }
    chain.push_back(working.back());
    return chain;
}

// Merge any run shorter than min_run_mm into its previous run (the first run,
// having no previous, merges forward into the next one instead).
void absorb_short_runs(std::vector<BrimRun> &runs, double min_run_mm)
{
    bool changed = true;
    while (changed && runs.size() > 1) {
        changed = false;
        for (size_t i = 0; i < runs.size(); ++i) {
            if (path_length_mm(runs[i].pts) >= min_run_mm)
                continue;
            if (i == 0) {
                runs[1].pts.insert(runs[1].pts.begin(), runs[0].pts.begin(), runs[0].pts.end());
                runs.erase(runs.begin());
            } else {
                runs[i - 1].pts.insert(runs[i - 1].pts.end(), runs[i].pts.begin(), runs[i].pts.end());
                runs.erase(runs.begin() + i);
            }
            changed = true;
            break; // indices shifted; rescan
        }
    }
}

// While there are more runs than max_runs, merge the shortest run into its
// longer neighbor (keeping the neighbor's extruder), preserving point order.
void guard_max_runs(std::vector<BrimRun> &runs, size_t max_runs)
{
    while (runs.size() > max_runs && runs.size() > 1) {
        size_t shortest_idx = 0;
        double shortest_len = path_length_mm(runs[0].pts);
        for (size_t i = 1; i < runs.size(); ++i) {
            const double len = path_length_mm(runs[i].pts);
            if (len < shortest_len) {
                shortest_len = len;
                shortest_idx = i;
            }
        }

        const bool has_prev = shortest_idx > 0;
        const bool has_next = shortest_idx + 1 < runs.size();

        bool merge_into_next;
        if (has_prev && has_next) {
            const double prev_len = path_length_mm(runs[shortest_idx - 1].pts);
            const double next_len = path_length_mm(runs[shortest_idx + 1].pts);
            merge_into_next = next_len > prev_len; // strictly longer next wins; ties favor prev
        } else {
            merge_into_next = has_next; // only one neighbor exists
        }

        if (merge_into_next) {
            BrimRun &next = runs[shortest_idx + 1];
            next.pts.insert(next.pts.begin(), runs[shortest_idx].pts.begin(), runs[shortest_idx].pts.end());
        } else {
            BrimRun &prev = runs[shortest_idx - 1];
            prev.pts.insert(prev.pts.end(), runs[shortest_idx].pts.begin(), runs[shortest_idx].pts.end());
        }
        runs.erase(runs.begin() + shortest_idx);
    }
}

} // namespace

unsigned brim_vote(const WallSampleIndex &idx, const Point &pt, const BrimVoteParams &p)
{
    std::vector<std::pair<const WallSample *, double>> knn_result = idx.knn(pt, p.k);
    if (knn_result.empty())
        return p.fallback_extruder;

    const double eps = double(scale_(0.01)) * double(scale_(0.01));

    std::map<unsigned, double> score;
    std::map<unsigned, double> nearest_d2;
    std::map<unsigned, size_t> nearest_object_key;

    for (const auto &cand : knn_result) {
        const WallSample *s  = cand.first;
        const double      d2 = cand.second;
        score[s->extruder] += 1.0 / std::max(d2, eps);

        auto it = nearest_d2.find(s->extruder);
        if (it == nearest_d2.end() || d2 < it->second) {
            nearest_d2[s->extruder]          = d2;
            nearest_object_key[s->extruder]  = s->object_key;
        }
    }

    // Winner = max score; ties favor the lower extruder id because std::map
    // iterates keys ascending and we only replace on strict improvement.
    auto winner_it = score.begin();
    for (auto it = score.begin(); it != score.end(); ++it)
        if (it->second > winner_it->second)
            winner_it = it;
    const unsigned winner_ext = winner_it->first;

    if (score.size() == 1)
        return winner_ext;

    // Runner-up = highest score among the rest.
    auto runner_it = score.end();
    for (auto it = score.begin(); it != score.end(); ++it) {
        if (it == winner_it)
            continue;
        if (runner_it == score.end() || it->second > runner_it->second)
            runner_it = it;
    }
    const unsigned runner_ext = runner_it->first;

    const double winner_score = winner_it->second;
    const double runner_score = runner_it->second;
    const double d_winner     = std::sqrt(nearest_d2[winner_ext]);
    const double d_runner     = std::sqrt(nearest_d2[runner_ext]);

    const bool tie = runner_score >= winner_score * (1.0 - p.tie_score_ratio)
                   || std::abs(d_winner - d_runner) < scale_(p.tie_dist_mm);

    if (!tie)
        return winner_ext;

    auto object_area_of = [&p](size_t object_key) -> double {
        auto it = p.object_area.find(object_key);
        return it == p.object_area.end() ? 0.0 : it->second;
    };

    const double area_winner = object_area_of(nearest_object_key[winner_ext]);
    const double area_runner = object_area_of(nearest_object_key[runner_ext]);

    if (area_winner > area_runner)
        return winner_ext;
    if (area_runner > area_winner)
        return runner_ext;
    return std::min(winner_ext, runner_ext);
}

std::vector<BrimRun> split_polyline_by_vote(const Points &poly, bool is_loop,
                                             const WallSampleIndex &idx,
                                             const BrimVoteParams &p)
{
    if (poly.size() < 2) {
        BrimRun run;
        run.extruder = brim_vote(idx, poly.empty() ? Point(0, 0) : poly.front(), p);
        run.pts      = poly;
        return { run };
    }

    Points working = poly;
    if (is_loop)
        working.push_back(poly.front());

    if (path_length_mm(working) <= 0.0) {
        BrimRun run;
        run.extruder = brim_vote(idx, poly.front(), p);
        run.pts      = poly;
        return { run };
    }

    const Points chain = build_chain(working, p.sample_mm);

    std::vector<unsigned> votes;
    votes.reserve(chain.size());
    for (const Point &pt : chain)
        votes.push_back(brim_vote(idx, pt, p));

    std::vector<BrimRun> runs;
    size_t i = 0;
    while (i < chain.size()) {
        size_t j = i;
        while (j + 1 < chain.size() && votes[j + 1] == votes[i])
            ++j;
        BrimRun run;
        run.extruder = votes[i];
        run.pts.assign(chain.begin() + i, chain.begin() + j + 1);
        runs.push_back(std::move(run));
        i = j + 1;
    }

    absorb_short_runs(runs, p.min_run_mm);
    guard_max_runs(runs, p.max_runs);

    return runs;
}

} // namespace Slic3r
