#include "BrimFilament.hpp"
#include "libslic3r.h"

#include <algorithm>
#include <cmath>
#include <limits>
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

// Shared run-building core for split_polyline_by_vote / split_polyline_by_resolver:
// sample `poly` (closed if is_loop) every sample_mm, call `resolve` per sample,
// group runs, absorb runs shorter than min_run_mm into the previous run, then
// coalesce smallest runs until <= max_runs, sharing the boundary vertex between
// adjacent runs. Result covers the whole polyline in order.
std::vector<BrimRun> split_polyline_core(const Points &poly, bool is_loop,
                                          const std::function<unsigned(const Point &)> &resolve,
                                          const BrimVoteParams &p)
{
    if (poly.size() < 2) {
        BrimRun run;
        run.extruder = resolve(poly.empty() ? Point(0, 0) : poly.front());
        run.pts      = poly;
        return { run };
    }

    Points working = poly;
    if (is_loop)
        working.push_back(poly.front());

    if (path_length_mm(working) <= 0.0) {
        BrimRun run;
        run.extruder = resolve(poly.front());
        run.pts      = poly;
        return { run };
    }

    const Points chain = build_chain(working, p.sample_mm);

    std::vector<unsigned> votes;
    votes.reserve(chain.size());
    for (const Point &pt : chain)
        votes.push_back(resolve(pt));

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

    // Close the connecting gap between runs: as partitioned above (and after
    // absorb/guard merges, which only ever combine adjacent partition segments and
    // so preserve this invariant), run k-1's last point and run k's first point are
    // two DIFFERENT, adjacent chain samples (~sample_mm apart) - the travel between
    // them is not extruded, leaving a small unextruded gap at every boundary. Make
    // each run after the first start at the previous run's last point instead, so
    // consecutive runs share that boundary vertex and the extruded geometry is
    // continuous end to end.
    for (size_t k = 1; k < runs.size(); ++k)
        runs[k].pts.insert(runs[k].pts.begin(), runs[k - 1].pts.back());

    return runs;
}

// Return a representative source path to copy flow attributes (mm3_per_mm,
// width, height) from, mirroring how ExtrusionLoop/ExtrusionMultiPath::role()
// resolve to their first path. Returns nullptr for an empty loop/multipath.
const ExtrusionPath *first_path_of(const ExtrusionEntity &entity)
{
    if (const auto *path = dynamic_cast<const ExtrusionPath *>(&entity))
        return path;
    if (const auto *multipath = dynamic_cast<const ExtrusionMultiPath *>(&entity))
        return multipath->paths.empty() ? nullptr : &multipath->paths.front();
    if (const auto *loop = dynamic_cast<const ExtrusionLoop *>(&entity))
        return loop->paths.empty() ? nullptr : &loop->paths.front();
    return nullptr;
}

// Partition a single non-collection entity (path/multipath/loop). Recursion
// into nested ExtrusionEntityCollections happens in the caller.
void partition_leaf_entity(const ExtrusionEntity &entity, unsigned own_extruder,
                            const WallSampleIndex &idx, const BrimVoteParams &p,
                            ExtrusionEntityCollection &kept,
                            std::map<unsigned, ExtrusionEntityCollection> &out)
{
    const bool     is_loop = entity.is_loop();
    const Polyline as_pl   = entity.as_polyline();
    Points         chain   = as_pl.points;
    if (is_loop && chain.size() > 1 && chain.front() == chain.back())
        chain.pop_back(); // split_polyline_by_vote closes loops itself

    if (chain.empty())
        return; // guard: never call split_polyline_by_vote on an empty polyline

    std::vector<BrimRun> runs = split_polyline_by_vote(chain, is_loop, idx, p);

    // Fast path: every sample voted for our own extruder -> keep the ORIGINAL
    // entity untouched (byte-identical geometry, preserves loop-ness).
    if (runs.size() == 1 && runs.front().extruder == own_extruder) {
        kept.entities.push_back(entity.clone());
        return;
    }

    const ExtrusionPath *source = first_path_of(entity);
    const double mm3_per_mm = source ? source->mm3_per_mm : 0.0;
    const float  width      = source ? source->width      : 0.f;
    const float  height     = source ? source->height     : 0.f;

    for (const BrimRun &run : runs) {
        if (run.pts.empty())
            continue;
        auto *new_path      = new ExtrusionPath(erBrim, mm3_per_mm, width, height);
        new_path->polyline  = Polyline(run.pts);
        if (run.extruder == own_extruder)
            kept.entities.push_back(new_path);
        else
            out[run.extruder].entities.push_back(new_path);
    }
}

// Partition a single support entity (path/multipath/loop) of any role. `entity`
// is still owned by support_fills at this point; the caller deletes it if
// `replaced` comes back true. Fallback-voted runs are appended to
// `new_fallback_paths` (the caller re-inserts them into support_fills once the
// sweep over the original entities vector is finished, since it must not be
// mutated mid-iteration); non-fallback runs land in `out[extruder]`. Emitted
// split paths copy the source entity's role (first_path_of(entity)->role(),
// falling back to entity.role()) rather than hardcoding one - so a base-role
// entity's split paths stay erSupportMaterial. Returns the number of switch
// boundaries this entity contributed.
size_t partition_support_leaf_entity(const ExtrusionEntity &entity, unsigned fallback_extruder,
                                      const std::function<unsigned(const Point &)> &resolver,
                                      const BrimVoteParams &p,
                                      std::vector<ExtrusionPath *> &new_fallback_paths,
                                      std::map<unsigned, ExtrusionEntityCollection> &out,
                                      bool &replaced)
{
    replaced = false;

    const bool     is_loop = entity.is_loop();
    const Polyline as_pl   = entity.as_polyline();
    Points         chain   = as_pl.points;
    if (is_loop && chain.size() > 1 && chain.front() == chain.back())
        chain.pop_back(); // split_polyline_core closes loops itself

    if (chain.empty())
        return 0; // guard: never call split_polyline_core on an empty polyline; leave entity untouched

    std::vector<BrimRun> runs = split_polyline_core(chain, is_loop, resolver, p);

    // Fast path: every sample voted for the fallback extruder -> leave the
    // ORIGINAL entity in support_fills untouched (byte-identical geometry, no
    // entity churn).
    if (runs.size() == 1 && runs.front().extruder == fallback_extruder)
        return 0;

    replaced = true;

    const ExtrusionPath *source = first_path_of(entity);
    const double        mm3_per_mm = source ? source->mm3_per_mm : 0.0;
    const float         width      = source ? source->width      : 0.f;
    const float         height     = source ? source->height     : 0.f;
    const ExtrusionRole  role      = source ? source->role() : entity.role();

    for (const BrimRun &run : runs) {
        if (run.pts.empty())
            continue;
        auto *new_path      = new ExtrusionPath(role, mm3_per_mm, width, height);
        new_path->polyline  = Polyline(run.pts);
        if (run.extruder == fallback_extruder)
            new_fallback_paths.push_back(new_path);
        else
            out[run.extruder].entities.push_back(new_path);
    }

    return runs.empty() ? 0 : runs.size() - 1;
}

// v2.2 Task 3 (spec C7, "nested collections voted as one unit"): vote a WHOLE
// role-eligible nested collection - never split apart internally. `collection` is
// flattened (ExtrusionEntityCollection::flatten(), full recursion regardless of any
// nested no_sort - flatten() only preserves ordering when explicitly asked to via its
// preserve_ordering argument, which we don't need here since we're only SAMPLING
// points, not rebuilding geometry) so every leaf polyline anywhere inside is visited
// once, no matter how deeply nested. Each leaf is resampled with the SAME
// build_chain(..., p.sample_mm) core split_polyline_core uses for a single path (the
// "0.8mm sampling helper" the header comment/plan refer to - same default cadence,
// same helper, just invoked directly instead of through the run-building wrapper
// since a whole-collection vote has no runs to build), and `resolve` is called once
// per sample. Votes accumulate across the ENTIRE collection (not per-leaf), then the
// majority wins; ties broken to the LOWEST extruder id - std::map<unsigned, size_t>
// iterates in ascending key order, and a candidate only replaces the current best on
// a STRICTLY greater count, so the first (lowest-id) extruder reached at the max
// count is kept automatically, deterministically, with no separate tie-break branch
// needed. An empty collection, or one whose leaves contribute zero samples (e.g. every
// leaf's own polyline is empty), returns fallback_extruder - the caller's "leave it in
// support_fills untouched" path fires naturally on that value, same as the leaf fast
// path above.
unsigned vote_collection_as_unit(const ExtrusionEntityCollection &collection, unsigned fallback_extruder,
                                  const std::function<unsigned(const Point &)> &resolve,
                                  const BrimVoteParams &p)
{
    const ExtrusionEntityCollection flat = collection.flatten();

    std::map<unsigned, size_t> votes;
    for (const ExtrusionEntity *leaf : flat.entities) {
        if (leaf == nullptr || leaf->is_collection())
            continue; // flatten() should never leave a nested collection behind; guard anyway
        const Points chain = build_chain(leaf->as_polyline().points, p.sample_mm);
        for (const Point &pt : chain)
            ++votes[resolve(pt)];
    }

    if (votes.empty())
        return fallback_extruder;

    unsigned best_extruder = 0;
    size_t   best_count    = 0;
    for (const auto &kv : votes) // ascending extruder id order
        if (kv.second > best_count) {
            best_extruder = kv.first;
            best_count    = kv.second;
        }
    return best_extruder;
}

} // namespace

void partition_brim_by_wall(const ExtrusionEntityCollection &brim, unsigned own_extruder,
                             const WallSampleIndex &idx, const BrimVoteParams &p,
                             ExtrusionEntityCollection &kept,
                             std::map<unsigned, ExtrusionEntityCollection> &out)
{
    for (const ExtrusionEntity *entity : brim.entities) {
        if (entity == nullptr)
            continue;
        if (entity->is_collection()) {
            const auto *sub = static_cast<const ExtrusionEntityCollection *>(entity);
            partition_brim_by_wall(*sub, own_extruder, idx, p, kept, out);
            continue;
        }
        partition_leaf_entity(*entity, own_extruder, idx, p, kept, out);
    }
}

unsigned brim_vote(const WallSampleIndex &idx, const Point &pt, const BrimVoteParams &p)
{
    std::vector<std::pair<const WallSample *, double>> knn_result = idx.knn(pt, p.k);
    if (knn_result.empty())
        return p.fallback_extruder;

    // max_dist_mm == 0 means uncapped (Part 1 brim path; behavior identical to
    // pre-v2.1). Otherwise (v2.1 final-review I1 fix): the cap must bound the whole
    // electorate, not just the nearest sample - filtering only knn_result.front() left
    // every farther-but-still-in-k sample free to vote (and win, via the 1/d^2 weighted
    // score, or via the tie path's min-extruder-id fallback), so a wall beyond the cap
    // could still be chosen even though the spec's "nearest wall <= 1.0mm" rule requires
    // it to lose to a closer, cap-compliant wall or to fallback_extruder. Erase every
    // candidate farther than the cap BEFORE any scoring/tie logic runs; if none remain
    // (including the case where even the nearest sample was beyond the cap), fall back.
    // The tie path below then only ever chooses among <=-cap candidates.
    if (p.max_dist_mm > 0.0) {
        const double cap_scaled = scale_(p.max_dist_mm);
        const double cap2       = cap_scaled * cap_scaled;
        knn_result.erase(std::remove_if(knn_result.begin(), knn_result.end(),
                              [cap2](const std::pair<const WallSample *, double> &cand) {
                                  return cand.second > cap2;
                              }),
                          knn_result.end());
        if (knn_result.empty())
            return p.fallback_extruder;
    }

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
    return split_polyline_core(poly, is_loop,
        [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); }, p);
}

std::vector<BrimRun> split_polyline_by_resolver(const Points &poly, bool is_loop,
                                                 const std::function<unsigned(const Point &)> &resolver,
                                                 const BrimVoteParams &p)
{
    return split_polyline_core(poly, is_loop, resolver, p);
}

std::vector<size_t> select_layers_in_band(const std::vector<double> &print_zs,
                                           double lo_z, double hi_z)
{
    // Linear scan over ascending print_zs (print_zs is sorted, so this could be
    // a binary search, but the band is only ever a few layers deep). A layer is
    // selected by comparing its own TOP z (print_zs[i], the z its extruded
    // material actually reaches) against the band's bounds: strictly above
    // lo_z and at/below hi_z. This intentionally excludes a layer whose top
    // overshoots hi_z even when its bottom still dips into the band (variable
    // layer height: a tall layer straddling the far edge of the band is not
    // "in" it).
    std::vector<size_t> result;
    for (size_t i = 0; i < print_zs.size(); ++i) {
        const double layer_top = print_zs[i];
        if (layer_top > lo_z + EPSILON && layer_top <= hi_z + EPSILON)
            result.push_back(i);
    }
    return result;
}

std::vector<size_t> select_contact_layers(const std::vector<double> &print_zs,
                                           double support_top_z, double gap_mm)
{
    return select_layers_in_band(print_zs, support_top_z, support_top_z + gap_mm);
}

std::vector<size_t> select_layers_overlapping_span(const std::vector<double> &print_zs,
                                                     double lo_z, double hi_z,
                                                     double first_bottom_z)
{
    // Two half-open intervals (a, b] and (c, d] overlap iff a < d && c < b. Here layer
    // i's own interval is (bottom, print_zs[i]] with bottom carried forward from the
    // previous iteration (first_bottom_z for i == 0 - callers with a raft must pass the
    // first layer's true bottom, else raft-level bands falsely overlap layer 0's span,
    // re-review finding N2), and the band is (lo_z, hi_z] - so keep layer i when
    // bottom < hi_z && lo_z < print_zs[i], EPSILON-padded the same way
    // select_layers_in_band pads its own top-z comparison.
    std::vector<size_t> result;
    double bottom = first_bottom_z;
    for (size_t i = 0; i < print_zs.size(); ++i) {
        const double top = print_zs[i];
        if (top > lo_z + EPSILON && bottom < hi_z - EPSILON)
            result.push_back(i);
        bottom = top;
    }
    return result;
}

std::vector<size_t> union_layer_indices(const std::vector<size_t> &a, const std::vector<size_t> &b)
{
    // v2.2 Task 4 (spec C8): see this function's own header comment in BrimFilament.hpp
    // - no ordering/duplicate-free assumption on either input, sort+unique the
    // concatenation. Layer index counts here are always small (a handful of band
    // layers), so this is deliberately the simplest correct implementation, not a
    // merge-of-two-sorted-ranges optimization.
    std::vector<size_t> result;
    result.reserve(a.size() + b.size());
    result.insert(result.end(), a.begin(), a.end());
    result.insert(result.end(), b.begin(), b.end());
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

namespace {

// v2.2 Task 2 (spec C5): bbox-gated "does any island in `polys` contain p" test,
// shared by the raw-lslices and expanded_lslices (margin-ring) layer-hit checks in
// chameleon_pick_projection_region below - they differ only in which ExPolygons/bboxes
// pair they're testing. `bboxes` may be null or size-mismatched to skip the gate
// (falls straight through to the exact test) - the same defensive contract the
// original raw-lslices-only code documented before this was factored out.
bool any_polygon_contains(const ExPolygons &polys, const std::vector<BoundingBox> *bboxes, const Point &p)
{
    if (polys.empty())
        return false;
    if (bboxes != nullptr && bboxes->size() == polys.size()) {
        bool bbox_hit = false;
        for (size_t k = 0; k < bboxes->size() && !bbox_hit; ++k)
            if ((*bboxes)[k].contains(p))
                bbox_hit = true;
        if (!bbox_hit)
            return false;
    }
    for (const ExPolygon &expoly : polys)
        if (expoly.contains(p))
            return true;
    return false;
}

// v2.2 Task 2 (spec C5): point-to-AABB clamped distance - a cheap LOWER BOUND on the
// true distance from p to anything inside bb (zero when p is inside bb). Standard
// clamped-rectangle formula. Used only to prune candidate regions in
// nearest_region_to_point below before the more expensive exact-ish polygon distance
// test - never itself treated as the final answer.
double bbox_point_distance(const BoundingBox &bb, const Point &p)
{
    const double dx = std::max({ double(bb.min.x() - p.x()), 0.0, double(p.x() - bb.max.x()) });
    const double dy = std::max({ double(bb.min.y() - p.y()), 0.0, double(p.y() - bb.max.y()) });
    return std::sqrt(dx * dx + dy * dy);
}

// v2.2 Task 2 (spec C5): "distance from p to this ExPolygon" - the same definition
// Slic3r already uses elsewhere for polygon-to-point distance (MultiPoint::distance_to,
// MultiPoint.hpp: "the minimum distance of all points to that point"), applied to the
// contour and every hole, taking the overall minimum. This is nearest-VERTEX distance,
// not true nearest-EDGE distance - a documented, precedented simplification (the same
// one MultiPoint::distance_to itself already documents), cheap because it needs no
// per-edge projection math.
double expoly_point_distance(const ExPolygon &poly, const Point &p)
{
    double d = poly.contour.distance_to(p);
    for (const Polygon &hole : poly.holes)
        d = std::min(d, hole.distance_to(p));
    return d;
}

// v2.2 Task 2 (spec C5): among `lv`'s regions, the one whose raw slice polys are
// nearest to p - see chameleon_pick_projection_region's own header comment for the
// two-stage branch-and-bound algorithm (cheap bbox lower bound prunes the expensive
// exact test) and its determinism (ties broken by ascending region index: only a
// STRICTLY smaller exact distance replaces the current best). Returns
// lv.region_slice_polys.size() (the same "none found" sentinel the caller already
// uses) when no region on this layer contributes any raw slice geometry at all.
size_t nearest_region_to_point(const ProjectionLayerView &lv, const Point &p)
{
    size_t nearest_region = lv.region_slice_polys.size();
    double nearest_dist    = std::numeric_limits<double>::max();

    for (size_t r = 0; r < lv.region_slice_polys.size(); ++r) {
        if (lv.region_slice_polys[r].empty())
            continue;

        // Stage A: cheap bbox lower bound across this region's raw polys. If even the
        // lower bound can't beat the current best, the exact distance certainly can't
        // either - skip it without ever computing expoly_point_distance for this region.
        double region_bbox_dist = std::numeric_limits<double>::max();
        for (const ExPolygon *poly : lv.region_slice_polys[r])
            if (poly != nullptr)
                region_bbox_dist = std::min(region_bbox_dist, bbox_point_distance(get_extents(*poly), p));
        if (region_bbox_dist >= nearest_dist)
            continue;

        // Stage B: exact-ish polygon distance, only for a region whose cheap bound
        // didn't already rule it out.
        double region_dist = std::numeric_limits<double>::max();
        for (const ExPolygon *poly : lv.region_slice_polys[r])
            if (poly != nullptr)
                region_dist = std::min(region_dist, expoly_point_distance(*poly, p));

        if (region_dist < nearest_dist) { // strict: ties keep the earlier (lower-index) region
            nearest_dist   = region_dist;
            nearest_region = r;
        }
    }
    return nearest_region;
}

} // namespace

bool chameleon_pick_projection_region(const std::vector<ProjectionLayerView> &layers,
                                       const Point &p,
                                       size_t &out_layer, size_t &out_region)
{
    // PASS 1 (v2.2 final-review C1 fix): raw containment ONLY, lowest band layer first.
    // This must run to completion over every layer before the margin ring (PASS 2,
    // below) is ever consulted - a ring hit on a lower layer must never pre-empt genuine
    // raw containment on a higher layer. See this function's own header comment for why
    // that ordering matters (the first contact-band layer is typically wall-only).
    for (size_t li = 0; li < layers.size(); ++li) {
        const ProjectionLayerView &lv = layers[li];
        if (lv.lslices == nullptr || lv.lslices->empty())
            continue; // no raw geometry on this layer at all
        if (!any_polygon_contains(*lv.lslices, lv.lslices_bboxes, p))
            continue; // raw miss on this layer; try the next one (still within PASS 1)

        // Within the raw-hit layer: the region whose own RAW slices contain p,
        // preferring one with a bottom/bottom-bridge fill surface covering p when more
        // than one region's slices contain p. First-contains-p wins the non-preferred
        // case, so the outcome is a deterministic function of region order, not point
        // order. Unchanged from v2.1.
        size_t chosen = lv.region_slice_polys.size(); // sentinel: none yet
        for (size_t r = 0; r < lv.region_slice_polys.size(); ++r) {
            bool slice_hit = false;
            for (const ExPolygon *poly : lv.region_slice_polys[r])
                if (poly != nullptr && poly->contains(p)) { slice_hit = true; break; }
            if (!slice_hit)
                continue;

            if (chosen == lv.region_slice_polys.size())
                chosen = r; // first region containing p; kept unless a bottom-hit region beats it

            bool bottom_hit = false;
            if (r < lv.region_bottom_polys.size())
                for (const ExPolygon *poly : lv.region_bottom_polys[r])
                    if (poly != nullptr && poly->contains(p)) { bottom_hit = true; break; }
            if (bottom_hit) { chosen = r; break; } // preferred tie-break wins outright
        }

        // Pre-existing degenerate case: this layer's lslices cover p, but no per-region
        // slice data agrees. Resolve to the nearest region ON THIS SAME LAYER rather
        // than treating the layer as a miss and scanning further - this layer already
        // won PASS 1 on raw containment, so it keeps resolution, it does not fall
        // through to a higher layer (documented, tested v2.1 behavior, unchanged here).
        if (chosen == lv.region_slice_polys.size())
            chosen = nearest_region_to_point(lv, p);

        if (chosen == lv.region_slice_polys.size())
            continue; // this raw-hit layer offers no region with any raw geometry at all; try the next one

        out_layer  = li;
        out_region = chosen;
        return true;
    }

    // PASS 2 (spec C5 margin ring): reached only when NO band layer's raw lslices
    // contain p anywhere. Scan again, lowest first, this time testing the margin ring
    // (expanded_lslices) - a rescue for samples the grown contact polygon pushed outside
    // all raw geometry, never a way to out-rank a higher layer's real containment (PASS
    // 1 already ruled that out globally). No raw-containment region scan is repeated
    // here: a region's own raw slice polys are always a subset of its layer's raw
    // lslices, which PASS 1 has already established as a miss on every layer.
    for (size_t li = 0; li < layers.size(); ++li) {
        const ProjectionLayerView &lv = layers[li];
        if (lv.expanded_lslices.empty())
            continue; // no margin-ring geometry on this layer at all
        if (!any_polygon_contains(lv.expanded_lslices, &lv.expanded_lslices_bboxes, p))
            continue; // ring miss on this layer; try the next one

        size_t chosen = nearest_region_to_point(lv, p);
        if (chosen == lv.region_slice_polys.size())
            continue; // ring covers p but this layer offers no region with any raw geometry; try the next one

        out_layer  = li;
        out_region = chosen;
        return true;
    }

    return false;
}

size_t partition_support_entities(ExtrusionEntityCollection &support_fills, ExtrusionRole role_filter,
                                   unsigned fallback_extruder, const std::function<unsigned(const Point &)> &resolver,
                                   const BrimVoteParams &p, std::map<unsigned, ExtrusionEntityCollection> &out)
{
    size_t switch_boundaries = 0;
    std::vector<ExtrusionPath *> new_fallback_paths;

    // Rebuilt in place: entities whose role() != role_filter and untouched
    // (fast-path) role_filter entities keep their original pointer and
    // position; matched role_filter entities are deleted here and their
    // fallback-voted runs are appended (as new entities, role copied from the
    // source) below, once the sweep is done.
    ExtrusionEntitiesPtr kept;
    kept.reserve(support_fills.entities.size());

    for (ExtrusionEntity *entity : support_fills.entities) {
        if (entity == nullptr)
            continue;

        if (entity->is_collection()) {
            // v2.2 Task 3 (spec C7): a nested collection is never split apart
            // internally (as_polyline()/is_loop() can't handle one anyway) - it is
            // either role-INELIGIBLE (its own collapsed role() isn't role_filter -
            // mixed-role or some other single role entirely) and left untouched
            // exactly like before C7, or role-ELIGIBLE and voted as ONE unit below.
            auto *collection = static_cast<ExtrusionEntityCollection *>(entity);
            if (collection->role() != role_filter) {
                kept.push_back(entity);
                continue;
            }

            const unsigned voted = vote_collection_as_unit(*collection, fallback_extruder, resolver, p);
            if (voted == fallback_extruder) {
                // Fallback majority (or no samples at all): leave the ORIGINAL
                // collection pointer exactly where it is - byte-identical geometry,
                // no entity churn, and its own no_sort flag is untouched because the
                // collection object itself is never touched, only re-examined.
                kept.push_back(entity);
            } else {
                // Non-fallback majority: move the WHOLE collection pointer into
                // out[voted] - true ownership transfer (not append(), which clones),
                // same "pointer moves, no clone" contract the leaf fast path already
                // gives its own untouched entities. The collection's no_sort flag
                // travels with it unchanged (still the same object).
                out[voted].entities.push_back(entity);
            }
            continue;
        }

        // Only role_filter leaf entities (path/multipath/loop) reach here.
        if (entity->role() != role_filter) {
            kept.push_back(entity);
            continue;
        }

        bool replaced = false;
        switch_boundaries += partition_support_leaf_entity(*entity, fallback_extruder, resolver, p,
                                                             new_fallback_paths, out, replaced);
        if (replaced)
            delete entity; // matched original removed; its runs were recorded above
        else
            kept.push_back(entity); // uniform-fallback fast path: original stays untouched
    }

    for (ExtrusionPath *path : new_fallback_paths)
        kept.push_back(path);

    support_fills.entities.swap(kept);
    return switch_boundaries;
}

size_t partition_support_interfaces(ExtrusionEntityCollection &support_fills, unsigned fallback_extruder,
                                     const WallSampleIndex &idx, const BrimVoteParams &p,
                                     std::map<unsigned, ExtrusionEntityCollection> &out)
{
    return partition_support_entities(support_fills, erSupportMaterialInterface, fallback_extruder,
        [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); }, p, out);
}

double total_path_length_mm(const ExtrusionEntityCollection &collection)
{
    double len = 0.0;
    for (const ExtrusionEntity *entity : collection.entities) {
        if (entity == nullptr)
            continue;
        // ExtrusionEntityCollection::length() throws - recurse instead of calling it
        // (see this function's own header comment: today's buckets are flat
        // ExtrusionPath* only, but this stays correct if that ever changes).
        if (entity->is_collection())
            len += total_path_length_mm(*static_cast<const ExtrusionEntityCollection *>(entity));
        else
            len += unscale<double>(entity->length()); // length() is in SCALED units
    }
    return len;
}

BucketCapResult apply_bucket_caps(std::map<unsigned, ExtrusionEntityCollection> &map,
                                   const std::set<unsigned> &prev_kept,
                                   size_t max_extruders,
                                   double min_len_mm,
                                   ExtrusionEntityCollection &merge_back_target)
{
    BucketCapResult result;

    // (a) C3 min-benefit gate: drop any bucket under min_len_mm BEFORE the trim below
    // ever ranks it - a sliver must never survive by being one of the "top N longest"
    // among other slivers.
    for (auto it = map.begin(); it != map.end(); ) {
        if (total_path_length_mm(it->second) < min_len_mm) {
            merge_back_target.append(std::move(it->second.entities));
            it = map.erase(it);
            ++result.buckets_dropped_min_benefit;
        } else {
            ++it;
        }
    }

    // (b) C1 distinct-extruder trim: rank the gate's survivors by (previously-kept
    // DESC, total length DESC, extruder id ASC as the final deterministic tie-break)
    // and keep only the top max_extruders. Hysteresis outranks length outright - see
    // this function's header comment for why (stability up the column vs. the
    // alternating-stripe artifact the old whole-layer revert caused).
    if (map.size() > max_extruders) {
        std::vector<std::pair<unsigned, double>> ranked;
        ranked.reserve(map.size());
        for (auto &kv : map)
            ranked.emplace_back(kv.first, total_path_length_mm(kv.second));

        std::sort(ranked.begin(), ranked.end(),
            [&prev_kept](const std::pair<unsigned, double> &a, const std::pair<unsigned, double> &b) {
                const bool a_prev = prev_kept.count(a.first) != 0;
                const bool b_prev = prev_kept.count(b.first) != 0;
                if (a_prev != b_prev)
                    return a_prev; // previously-kept extruder ranks first, regardless of length
                if (a.second != b.second)
                    return a.second > b.second; // then longer total path first
                return a.first < b.first; // deterministic final tie-break
            });

        for (size_t i = max_extruders; i < ranked.size(); ++i) {
            auto it = map.find(ranked[i].first);
            merge_back_target.append(std::move(it->second.entities));
            map.erase(it);
            ++result.buckets_trimmed_cap;
        }
    }

    for (const auto &kv : map)
        result.kept.insert(kv.first);

    return result;
}

double gap_aware_lateral_cap_mm(double support_object_xy_distance_mm,
                                double outer_wall_width_mm,
                                double support_line_width_mm)
{
    // v2.2 Task 2 (spec C4): see this function's own header comment in BrimFilament.hpp
    // for the physical reasoning - this body is deliberately just the sum it describes,
    // no branching, so a caller passing 0 for either width (a defensive fallback, not
    // an error) still gets a sane, non-throwing result.
    return support_object_xy_distance_mm + 0.5 * outer_wall_width_mm + 0.5 * support_line_width_mm + 0.35;
}

bool support_role_needs_interface_extruder(ExtrusionRole role)
{
    // See this function's own header comment in BrimFilament.hpp for the full
    // investigation finding (matched-out layers can leave PURE-ironing support_fills)
    // - erIroning is the v2.2 Task 3 addition; erMixed/erSupportMaterialInterface are
    // unchanged from pre-v2.2.
    return role == erMixed || role == erSupportMaterialInterface || role == erIroning;
}

} // namespace Slic3r
