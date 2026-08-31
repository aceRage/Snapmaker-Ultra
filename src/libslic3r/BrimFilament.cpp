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

    // v2.3 Task 2 (spec C7, root cause 8): LOOP inputs only. `chain`'s last sample is
    // the SAME physical point as its first - for a loop, `working` above has
    // poly.front() appended as an explicit closing point, so build_chain's last output
    // sample is that same coordinate. If the array-index-FIRST and array-index-LAST
    // runs built just above land on the SAME extruder, they are not two separate
    // sectors - they are ONE sector artificially split by where the sample array
    // happens to start/end (the seam). Left unmerged, that one sector is counted TWICE
    // (once at each end of the array), inflating the effective run count into
    // guard_max_runs' cap for no geometric reason (spec root cause 8: "Ring seam
    // double-counts a sector... max_runs=4 trips on 4-sector rings"). Fold the last run
    // into the first (prepend its points - the last run physically precedes the first
    // one when the loop is read circularly, since `chain` wraps from its last sample
    // back to its first) BEFORE absorb_short_runs/guard_max_runs run below, so both
    // length-based passes see the seam sector as the single sector it actually is.
    // The join point (the last run's last point == the first run's first point, both
    // `poly.front()`) is a literal duplicate coordinate, not a gap - it's the SAME
    // loop-closing vertex build_chain already produced, so this preserves the shared-
    // boundary-vertex invariant across the seam exactly like the ordinary inter-run
    // gap-fix loop below does for every OTHER pair of adjacent runs, without needing a
    // separate fixup here. Only merges when there are >= 2 runs (a single-run loop, the
    // whole chain one color, has no seam to speak of - and by construction adjacent
    // array runs never share an extruder, so 2 runs are automatically each other's
    // whole-array front/back and a genuine merge candidate) and only for loops - an
    // OPEN polyline's first/last samples are its two distinct endpoints, not a seam, so
    // merging on a color match there would wrongly join two unrelated ends of an open
    // path. A single merge always suffices (never needs to repeat): the run that
    // becomes the new "last" after popping is the ORIGINAL second-to-last run, which by
    // the array's own adjacency invariant (no two array-adjacent runs ever share an
    // extruder) already differs from the just-removed original last run's extruder -
    // and the just-removed run's extruder is exactly what the merged run now carries.
    // v2.3 final-review M4 fix: gated on p.merge_ring_seam (BrimVoteParams, defaults
    // false) - this function is shared with Part 1's own split_polyline_by_vote
    // (:696/partition_leaf_entity :250), and the merge used to fire unconditionally for
    // ANY loop, violating the plan's "Part 1 brim behavior stays byte-identical" contract
    // for a feature-ON brim loop whose seam happened to land inside one color's sector.
    // Part 1's own brim BrimVoteParams never sets this field (stays false, block
    // unreachable); the support pass (Print.cpp chameleon_assign_support_interfaces) sets
    // it true on its own vote_params before copying it into every per-role BrimVoteParams
    // it builds, so support ring/loop splitting keeps this merge exactly as before.
    bool seam_merged = false;
    if (p.merge_ring_seam && is_loop && runs.size() >= 2 && runs.front().extruder == runs.back().extruder) {
        BrimRun &first = runs.front();
        BrimRun &last  = runs.back();
        first.pts.insert(first.pts.begin(), last.pts.begin(), last.pts.end());
        runs.pop_back();
        seam_merged = true;
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

    // v2.3 final-review I1 fix: the seam merge above (when it fired) throws away the
    // ring's own CIRCULAR wrap-boundary coverage. Before the merge, the array-last run's
    // own last point was `chain`'s closing sample - the SAME coordinate as the array-
    // first run's own first point (a loop's `working` has poly.front() appended as an
    // explicit closing point, so build_chain's last output sample equals its first) - so
    // the wrap segment was already covered with no extra fixup needed, exactly like every
    // other run boundary. Folding the old last run into the front of the new first run
    // (above) buries that shared closing vertex as an INTERIOR point of the merged run,
    // not its boundary anymore: the run that is now runs.back() (the ORIGINAL second-to-
    // last run - guard_max_runs/absorb_short_runs only ever combine ADJACENT runs, so it
    // may have grown but its own tail end is unchanged) ends at a chain sample that is
    // merely ADJACENT to runs.front()'s own first point (~sample_mm apart, not equal) -
    // exactly the ordinary inter-run gap the loop just above closes for every OTHER pair,
    // except this pair (back -> front, wrapping around) is never visited by that loop (k
    // never wraps back to 0). Left alone, every ring whose seam merge fires prints with
    // one ~sample_mm unextruded hole at the wrap boundary (spec/review finding I1).
    // Prepend runs.back()'s own last point to runs.front(), the SAME shared-boundary-
    // vertex fixup the loop above applies to every linear pair, closing the ring.
    // Single-run case (re-review N1): when absorb/guard collapse EVERYTHING into one run
    // after the seam merge fired, that run's endpoints are two merely-ADJACENT chain
    // samples (the seam merge buried the ring's true closing vertex as an interior
    // point), so the ring still has one ~sample_mm wrap hole - close it by appending the
    // run's own first point, the circular analogue of the shared-boundary fixup.
    if (seam_merged) {
        if (runs.size() >= 2)
            runs.front().pts.insert(runs.front().pts.begin(), runs.back().pts.back());
        else if (runs.size() == 1 && runs.front().pts.size() >= 2 &&
                 runs.front().pts.front() != runs.front().pts.back())
            runs.front().pts.push_back(runs.front().pts.front());
    }

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
// v2.3 Task 3 (spec C5): every emitted split path also inherits `entity`'s OWN
// can_reverse() (ExtrusionEntity.hpp ~111/294/347/389/461 - each concrete entity type
// expresses "reversal disabled" its own way: ExtrusionPath via the settable
// m_can_reverse/set_reverse(), ExtrusionPathOriented/ExtrusionLoop by hard-overriding
// can_reverse() to always return false). A branch-wall leaf built with reversal
// disabled (Support/SupportCommon.cpp:660-663/765-767 -
// extrusion_entities_append_paths(..., /*can_reverse=*/false), "always start with the
// anchor, always print CCW") must keep that property on every rebuilt piece: every
// new_path this function creates is a plain ExtrusionPath (never an
// ExtrusionPathOriented), so the only way to carry a false can_reverse() forward is
// set_reverse() - a no-op call for the (common) case where entity.can_reverse() is
// already true, since ExtrusionPath defaults m_can_reverse to true. Losing this would
// let GCode's chain_and_reorder flip a rebuilt piece's direction freely, breaking the
// seam anchor into a visible blob (spec: "seam-anchor blob hazard").
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
    const bool           source_can_reverse = entity.can_reverse(); // v2.3 Task 3 (spec C5)

    for (const BrimRun &run : runs) {
        if (run.pts.empty())
            continue;
        auto *new_path      = new ExtrusionPath(role, mm3_per_mm, width, height);
        new_path->polyline  = Polyline(run.pts);
        if (!source_can_reverse)
            new_path->set_reverse(); // v2.3 Task 3 (spec C5): carry reversal-disable forward
        if (run.extruder == fallback_extruder)
            new_fallback_paths.push_back(new_path);
        else
            out[run.extruder].entities.push_back(new_path);
    }

    return runs.empty() ? 0 : runs.size() - 1;
}

// v2.3 Task 3 (spec C5): result of voting a whole nested collection as one unit -
// extends the pre-v2.3 single winner-extruder answer (`.winner`, unchanged tie-break
// semantics - see vote_collection_as_unit's own comment below) with the raw
// per-extruder sample histogram and the longest contiguous run (mm) of samples that
// voted some extruder OTHER than `.winner`, in COLLECTION ORDER (the same leaf-then-
// sample walk the histogram itself uses - see below). This is what
// partition_support_entities' collection branch uses to decide "uniform/dominant" (stay
// whole, exactly the pre-v2.3 behavior) vs. "genuinely mixed with a real arc" (descend,
// spec C5). `minority_run_mm` is a SAMPLE-COUNT approximation
// (samples_in_the_run * p.sample_mm), NOT exact point-to-point geometry - consistent
// with the "sampled at sample_mm" contract this whole vote already runs under
// (build_chain resamples at that cadence), and deliberately avoids measuring a "run"
// across a LEAF BOUNDARY as if the two leaves' endpoints were geometrically adjacent -
// they are only adjacent in SAMPLE-SEQUENCE order, not necessarily in space (e.g. an
// inner-loop leaf immediately followed by an outer-loop leaf in a double-wall branch
// collection - SupportCommon.cpp:647-773's `eec` - are two concentric rings, not two
// ends of one continuous line). v2.3 final-review M1 fix: this paragraph describes the
// INTENDED contract; the run-scan below now actually enforces it (a run resets at every
// leaf boundary, tracked via `leaf_starts`/`is_leaf_start` built alongside `ordered_votes`
// - see the scan's own comment) - pre-fix, `ordered_votes` carried no leaf-boundary
// markers at all, so two sub-threshold same-extruder tails from UNRELATED leaves that
// happened to land back-to-back in collection order could silently concatenate into one
// run that clears the descend threshold, a spurious DESCEND this paragraph's own wording
// already claimed could not happen. `histogram` is empty (and `minority_run_mm` is 0.0) for
// the same "no samples anywhere" case that returns `.winner == fallback_extruder`
// (empty/all-empty-leaf collection).
struct CollectionVoteResult {
    unsigned                   winner = 0;
    std::map<unsigned, size_t> histogram;
    double                     minority_run_mm = 0.0;
};

// v2.2 Task 3 (spec C7, "nested collections voted as one unit"): vote a WHOLE
// role-eligible nested collection - never split apart internally BY THIS FUNCTION (v2.3
// Task 3's DESCEND path, driven by this function's richer CollectionVoteResult, is what
// may go on to split it - see partition_support_entities). `collection` is flattened
// (ExtrusionEntityCollection::flatten(), full recursion regardless of any nested
// no_sort - flatten() only preserves ordering when explicitly asked to via its
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
// leaf's own polyline is empty), returns fallback_extruder as `.winner` (empty
// histogram) - the caller's "leave it in support_fills untouched" path fires naturally
// on that value, same as the leaf fast path above.
CollectionVoteResult vote_collection_as_unit(const ExtrusionEntityCollection &collection, unsigned fallback_extruder,
                                  const std::function<unsigned(const Point &)> &resolve,
                                  const BrimVoteParams &p)
{
    const ExtrusionEntityCollection flat = collection.flatten();

    // v2.3 Task 3 (spec C5): the same per-sample votes the histogram below accumulates,
    // ALSO kept in COLLECTION ORDER (leaf by leaf, sample by sample within each leaf -
    // the same walk order, just not collapsed into counts) so the minority-run scan
    // after the winner is decided can find contiguous same-extruder stretches.
    // v2.3 final-review M1 fix: `leaf_starts` records the ordered_votes index each leaf's
    // OWN samples begin at, so the run scan below can refuse to extend a run across a
    // leaf boundary - see the run-scan's own comment for why crossing one is wrong.
    std::vector<unsigned> ordered_votes;
    std::vector<size_t>   leaf_starts;
    std::map<unsigned, size_t> votes;
    for (const ExtrusionEntity *leaf : flat.entities) {
        if (leaf == nullptr || leaf->is_collection())
            continue; // flatten() should never leave a nested collection behind; guard anyway
        const Points chain = build_chain(leaf->as_polyline().points, p.sample_mm);
        if (chain.empty())
            continue;
        leaf_starts.push_back(ordered_votes.size());
        for (const Point &pt : chain) {
            const unsigned v = resolve(pt);
            ordered_votes.push_back(v);
            ++votes[v];
        }
    }

    if (votes.empty())
        return { fallback_extruder, {}, 0.0 };

    size_t best_count = 0;
    for (const auto &kv : votes)
        best_count = std::max(best_count, kv.second);

    // v2.3 Task 1 (spec C3): every extruder tied at best_count, ascending id order
    // (votes is a std::map, so this loop already visits keys ascending). Before this
    // task there was no explicit tie branch here at all - the strict `>` scan above
    // (now replaced) silently kept the lowest id among ties as a side effect of never
    // replacing on an equal count. That lowest-id fallback is preserved below
    // (`tied.front()`); the only new behavior is trying prev_kept FIRST when the tie
    // has more than one candidate.
    std::vector<unsigned> tied;
    for (const auto &kv : votes)
        if (kv.second == best_count)
            tied.push_back(kv.first);

    unsigned winner = tied.front(); // lowest id among the tied (or the sole max when there's no tie)
    if (tied.size() > 1) {
        // Same hysteresis preference as brim_vote's own tie path (spec C3): if exactly
        // ONE tied candidate is in the previous support layer's committed set, it wins
        // outright. p.prev_kept defaults empty, so this is a no-op whenever the caller
        // hasn't wired hysteresis through (Part 1 brim path never calls this function
        // at all; every pre-v2.3 support caller left prev_kept empty).
        unsigned prev_kept_member = 0;
        size_t   prev_kept_hits   = 0;
        for (unsigned ext : tied)
            if (p.prev_kept.count(ext) != 0) {
                prev_kept_member = ext;
                ++prev_kept_hits;
            }
        if (prev_kept_hits == 1)
            winner = prev_kept_member;
    }

    // v2.3 Task 3 (spec C5): longest contiguous run of the SAME non-winner extruder, in
    // collection order (ordered_votes, built above in the same leaf-then-sample walk the
    // majority histogram itself uses) - see CollectionVoteResult's own comment for why
    // this is a sample-count approximation of mm length, not exact point-to-point
    // geometry. v2.3 final-review M1 fix: two samples adjacent in ordered_votes are only
    // adjacent in SAMPLE-SEQUENCE order, not necessarily in space, whenever they straddle
    // a LEAF boundary (e.g. an inner-loop leaf immediately followed by an outer-loop leaf
    // in a double-wall branch collection - SupportCommon.cpp:647-773's `eec` - are two
    // concentric rings, not two ends of one continuous line) - so a run must never extend
    // past one. `is_leaf_start[k]` marks every index in ordered_votes where a leaf's own
    // samples begin (leaf_starts above); the inner while loop's own extend condition below
    // now also requires the NEXT sample not be one of those indices, resetting the run
    // exactly at each leaf boundary instead of silently concatenating two leaves' matching
    // tail/head runs into one that was never geometrically contiguous - the bug this
    // function's own header comment already claimed was handled (CollectionVoteResult's
    // comment above) but, pre-fix, was not.
    std::vector<bool> is_leaf_start(ordered_votes.size(), false);
    for (size_t start : leaf_starts)
        is_leaf_start[start] = true;

    double best_run_mm = 0.0;
    {
        size_t i = 0;
        while (i < ordered_votes.size()) {
            size_t j = i;
            while (j + 1 < ordered_votes.size() && ordered_votes[j + 1] == ordered_votes[i] && !is_leaf_start[j + 1])
                ++j;
            if (ordered_votes[i] != winner)
                best_run_mm = std::max(best_run_mm, double(j - i + 1) * p.sample_mm);
            i = j + 1;
        }
    }

    return { winner, std::move(votes), best_run_mm };
}

// v2.3 Task 3 (spec C5): descend ONE role-eligible collection whose whole-unit vote came
// back genuinely mixed (see partition_support_entities' collection branch below for the
// threshold decision that leads here). Every LEAF entity reachable inside `collection`
// is individually voted/split via the SAME per-leaf logic partition_support_leaf_entity
// already uses for a top-level (non-nested) leaf entity - this recurses into any
// FURTHER-nested sub-collection the same way (in practice, tree branch double-wall
// collections - the only known nested-collection producer at this role,
// SupportCommon.cpp:647-773 - are exactly one level deep, but this makes no assumption
// of that):
//   - a leaf whose entire vote is fallback_extruder is left in place, AT ITS OWN INDEX,
//     inside its IMMEDIATE parent collection - untouched, no churn (mirrors
//     partition_support_leaf_entity's own uniform-fallback fast path).
//   - any other leaf (uniform non-fallback, or genuinely mixed) is split via
//     partition_support_leaf_entity: fallback-voted runs become new ExtrusionPaths
//     SPLICED IN at the original leaf's index inside its immediate parent - this is why
//     the rebuild below appends to a fresh per-collection vector IN ITERATION ORDER
//     rather than accumulating fallback runs globally the way the top-level leaf case's
//     own new_fallback_paths does (no_sort order matters here, since these collections
//     model the tree branch generator's own explicit anchor/sheath ordering); non-
//     fallback runs go to out[extruder] (role/flow/can_reverse carried, same function).
// A nested sub-collection left FULLY EMPTY by this recursion (every one of its own
// direct entities matched/moved out, nothing fell back into it) is itself removed from
// its parent and deleted - the SAME "emptied shell, single delete" rule the top-level
// caller applies to `collection` itself, just recursive, so no empty shell is ever left
// dangling at any nesting depth. Returns the summed switch-boundary count from every
// leaf this touched (partition_support_leaf_entity's own per-leaf return value, summed).
size_t descend_collection_in_place(ExtrusionEntityCollection &collection, unsigned fallback_extruder,
                                    const std::function<unsigned(const Point &)> &resolver,
                                    const BrimVoteParams &p,
                                    std::map<unsigned, ExtrusionEntityCollection> &out)
{
    size_t switch_boundaries = 0;
    ExtrusionEntitiesPtr rebuilt;
    rebuilt.reserve(collection.entities.size());

    for (ExtrusionEntity *child : collection.entities) {
        if (child == nullptr)
            continue;

        if (child->is_collection()) {
            auto *sub = static_cast<ExtrusionEntityCollection *>(child);
            switch_boundaries += descend_collection_in_place(*sub, fallback_extruder, resolver, p, out);
            if (sub->entities.empty())
                delete sub; // emptied shell -> single delete, never left behind
            else
                rebuilt.push_back(sub); // stays at this same relative position (no_sort preserved)
            continue;
        }

        std::vector<ExtrusionPath *> fallback_runs;
        bool replaced = false;
        switch_boundaries += partition_support_leaf_entity(*child, fallback_extruder, resolver, p,
                                                             fallback_runs, out, replaced);
        if (!replaced) {
            rebuilt.push_back(child); // uniform-fallback fast path: original stays, at this index
            continue;
        }
        // Splice fallback runs in AT THIS LEAF'S POSITION (no_sort order preserved),
        // then drop the now-superseded original.
        for (ExtrusionPath *fb : fallback_runs)
            rebuilt.push_back(fb);
        delete child;
    }

    collection.entities.swap(rebuilt);
    return switch_boundaries;
}

} // namespace

Point chameleon_quantize_point(const Point &p, double quantize_mm)
{
    const double cell = scale_(quantize_mm);
    return Point(coord_t(std::floor(double(p.x()) / cell)), coord_t(std::floor(double(p.y()) / cell)));
}

Point chameleon_collection_bbox_center(const ExtrusionEntityCollection &collection)
{
    Points pts;
    const ExtrusionEntityCollection flat = collection.flatten();
    for (const ExtrusionEntity *leaf : flat.entities) {
        if (leaf == nullptr || leaf->is_collection())
            continue;
        const Points leaf_pts = leaf->as_polyline().points;
        pts.insert(pts.end(), leaf_pts.begin(), leaf_pts.end());
    }
    if (pts.empty())
        return Point(0, 0);
    return BoundingBox(pts).center();
}

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

    // v2.3 Task 1 (spec C3): hysteresis preference, tried BEFORE object_area/min-id -
    // if exactly ONE of {winner, runner} was in the PREVIOUS support layer's committed
    // set, it wins outright (stability up the column beats the tie-break heuristics
    // below). Both-or-neither falls through unchanged - this never widens the tie
    // window itself, it only changes which of the two already-tied candidates wins.
    // p.prev_kept defaults empty (Part 1 brim path, every pre-v2.3 caller), so both
    // membership tests are false and this is a strict no-op there.
    const bool winner_prev_kept = p.prev_kept.count(winner_ext) != 0;
    const bool runner_prev_kept = p.prev_kept.count(runner_ext) != 0;
    if (winner_prev_kept != runner_prev_kept)
        return winner_prev_kept ? winner_ext : runner_ext;

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

    // PASS 2 (spec C5 margin ring; v2.3 Task 2 spec C4/root cause 4 changes the scan
    // DIRECTION only): reached only when NO band layer's raw lslices contain p anywhere.
    // Scan again, this time HIGHEST band layer FIRST - the OPPOSITE direction from PASS
    // 1 above - testing the margin ring (expanded_lslices) instead of raw lslices. This
    // asymmetry is deliberate, not an oversight, and the two passes are answering
    // different questions: PASS 1 ("nearest surface above") scans lowest-first because
    // the lowest layer whose RAW geometry genuinely contains p already IS "the surface
    // above" by construction - real containment on a low layer is real containment, full
    // stop, so escalating upward only on a miss is correct there. PASS 2 only ever
    // triggers as a RESCUE for samples every layer's raw geometry missed, and a band's
    // FIRST (lowest) layer is typically wall-only (see this function's own header
    // comment: it spans the z-gap between the support top and the overhang bottom, so
    // its raw lslices see only the laterally-adjacent wall, never the overhang body,
    // which only starts one layer higher) - so scanning the RING lowest-first would
    // resolve a rim sample against that lower wall layer's ring even when the actual
    // overhang's own ring, one layer up, ALSO covers it, inverting "surface above wins"
    // for exactly the rim samples this rescue exists to help (spec root cause 4: "the
    // 1.2mm-grown contact rim resolves against the adjacent WALL layer below the
    // overhang layer"). Scanning highest-first here restores that priority: whichever
    // band layer's ring covers p, the HIGHEST one wins. No raw-containment region scan
    // is repeated here: a region's own raw slice polys are always a subset of its
    // layer's raw lslices, which PASS 1 has already established as a miss on every
    // layer, regardless of scan order.
    for (size_t idx = 0; idx < layers.size(); ++idx) {
        const size_t li = layers.size() - 1 - idx; // highest layer first (opposite of PASS 1)
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
                                   const BrimVoteParams &p, std::map<unsigned, ExtrusionEntityCollection> &out,
                                   DescendColumnMap *descended_out)
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

            const CollectionVoteResult vote = vote_collection_as_unit(*collection, fallback_extruder, resolver, p);

            // v2.3 Task 3 (spec C5): a real minority only matters if it is CONTIGUOUS
            // for at least the descend threshold - a handful of scattered stray votes
            // (histogram.size() > 1 but no run ever gets long) is noise, not a genuine
            // sector-straddling arc, so it stays on the pre-v2.3 whole-move/stay path
            // exactly like a uniform collection does. The threshold is this object's own
            // min_run_mm (support-pass override, spec C6: 1.6mm), HALVED for a column
            // that already descended LAST layer (descend hysteresis) - see
            // DescendColumnMap's own comment (BrimFilament.hpp) for the quantized-
            // bbox-center key and why this is only computed once we know a minority
            // exists at all (a uniform collection's minority_run_mm is always 0.0, and
            // 0.0 can never clear a strictly positive threshold, so skipping the lookup
            // entirely for that common case is both cheaper and harmless).
            const bool has_minority = vote.histogram.size() > 1;
            Point      column_key;
            bool       genuinely_mixed = false;
            if (has_minority) {
                column_key = chameleon_quantize_point(chameleon_collection_bbox_center(*collection));
                const auto it = p.descended_last_layer.find(column_key);
                const bool descended_last = it != p.descended_last_layer.end() && it->second;
                const double threshold = descended_last ? p.min_run_mm * 0.5 : p.min_run_mm;
                genuinely_mixed = vote.minority_run_mm >= threshold;
            }

            if (!genuinely_mixed) {
                // (a) Uniform, or a real-but-too-short minority: EXACT pre-v2.3
                // whole-move/stay behavior, pointer-stable.
                if (vote.winner == fallback_extruder) {
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
                    out[vote.winner].entities.push_back(entity);
                }
                continue;
            }

            // (b) DESCEND: partition every leaf inside `collection` individually, in
            // place - see descend_collection_in_place's own comment for the full
            // per-leaf splice/delete contract.
            switch_boundaries += descend_collection_in_place(*collection, fallback_extruder, resolver, p, out);
            if (collection->entities.empty())
                delete collection; // emptied shell -> single delete, never left in support_fills
            else
                kept.push_back(entity); // pointer stays - same object, rebuilt contents
            if (descended_out != nullptr)
                (*descended_out)[column_key] = true; // never records false - see DescendColumnMap's comment
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

LayerFilamentTable build_layer_filament_table(std::vector<std::pair<double, unsigned>> raw)
{
    // Sort by z first so every sample that belongs together (within EPSILON) is
    // adjacent - turns the merge below into a single linear pass instead of an O(n^2)
    // scan for each sample's z-neighborhood.
    std::sort(raw.begin(), raw.end(),
        [](const std::pair<double, unsigned> &a, const std::pair<double, unsigned> &b) { return a.first < b.first; });

    LayerFilamentTable table;
    for (const auto &sample : raw) {
        // A run of samples can drift by more than EPSILON from its OWN run's first
        // z if compared one-hop-at-a-time (many small sub-EPSILON steps accumulating) -
        // compare against the table's last COMMITTED entry z (not the previous raw
        // sample) so the merge window is anchored, matching select_layers_in_band's own
        // single-anchor EPSILON comparisons elsewhere in this file.
        if (!table.empty() && sample.first <= table.back().first + EPSILON)
            table.back().second.insert(sample.second);
        else
            table.push_back({ sample.first, std::set<unsigned>{ sample.second } });
    }
    return table;
}

std::set<unsigned> chameleon_layer_free_extruders(const LayerFilamentTable &table, double query_z,
                                                   double down_mm, double up_mm)
{
    // v2.4 Task B (spec B): windowed coincidence - [query_z - down_mm - EPSILON,
    // query_z + up_mm + EPSILON] - generalizing the old exact-z query (down_mm=up_mm=0
    // collapses this to the same [query_z - EPSILON, query_z + EPSILON] window the
    // pre-v2.4 version used). Binary search (table is ascending by z,
    // build_layer_filament_table's own contract) for the first entry whose z is >=
    // query_z - down_mm - EPSILON - by definition of lower_bound, every entry before it
    // has z < that bound and so is wholly out of range, meaning that first qualifying
    // entry is also the LEFTMOST one the window could possibly include (no separate
    // backward scan needed). Widen forward from there while entries remain <= query_z +
    // up_mm + EPSILON - more than one table entry can qualify even with a zero-width
    // window, when a merge boundary in the table leaves two adjacent entries each
    // individually within EPSILON of a query z that sits between them, even though the
    // two entries themselves are more than EPSILON apart from EACH OTHER (EPSILON-
    // merging is anchored per-run, not a transitive equivalence).
    std::set<unsigned> result;
    auto it = std::lower_bound(table.begin(), table.end(), query_z - down_mm - EPSILON,
        [](const std::pair<double, std::set<unsigned>> &entry, double z) { return entry.first < z; });
    for (; it != table.end() && it->first <= query_z + up_mm + EPSILON; ++it)
        result.insert(it->second.begin(), it->second.end());
    return result;
}

PrevKeptState chameleon_update_prev_kept(const PrevKeptState &state,
                                          const std::set<unsigned> &committed,
                                          bool had_buckets_pre_gate)
{
    if (!committed.empty())
        return { committed, false }; // (a) real commit: fresh grace for next time

    if (had_buckets_pre_gate && !state.retained_last_layer)
        return { state.prev_kept, true }; // (b) spend the one-layer retention grace

    return { {}, false }; // (c) grace already spent, or nothing existed pre-gate at all
}

namespace {
// v2.5a (spec item 2): one bucket's geometry, pulled out of `map` by the gate or
// trim step below, held here instead of being appended anywhere yet - the redirect
// phase decides its final destination only after BOTH steps have finished and the
// kept set is final. See apply_bucket_caps' own header comment (BrimFilament.hpp)
// for the full phase breakdown.
struct DroppedBucket {
    unsigned                   extruder;
    ExtrusionEntityCollection  geometry;
};
} // namespace

BucketCapResult apply_bucket_caps(std::map<unsigned, ExtrusionEntityCollection> &map,
                                   const std::set<unsigned> &prev_kept,
                                   size_t max_extruders,
                                   double min_len_mm,
                                   ExtrusionEntityCollection &merge_back_target,
                                   const std::set<unsigned> &free_extruders,
                                   double min_len_free_mm,
                                   const std::set<unsigned> &free_extruders_exempt,
                                   std::vector<ChameleonBucketDebugEntry> *debug_out)
{
    BucketCapResult result;
    // v2.5a: gate/trim drops land here (moved out of `map`, NOT appended anywhere
    // yet) instead of going straight to merge_back_target - see the redirect phase
    // at the end of this function.
    std::vector<DroppedBucket> gate_dropped;
    std::vector<DroppedBucket> trim_dropped;

    // CHAMELEON_DEBUG (v2.5c): snapshot every bucket's pre-gate length and default it
    // to "kept" - every write below only ever NARROWS a bucket's fate to gated/trimmed/
    // kept_exempt or records a redirect target; a bucket nothing below touches stayed
    // in `map` through both the gate and the trim, i.e. really was kept normally. Keyed
    // by extruder id (std::map, so the final `debug_out` copy comes out pre-sorted
    // ascending - no separate sort needed). Every write past this point is behind
    // `if (debug_out)`, so a caller passing nullptr (every pre-v2.5c call site) pays one
    // pointer compare per decision point and nothing else - `dbg` itself stays a single
    // empty map, no allocation, when debug_out is null (the snapshot loop below is
    // itself gated).
    std::map<unsigned, ChameleonBucketDebugEntry> dbg;
    if (debug_out)
        for (const auto &kv : map)
            dbg[kv.first] = ChameleonBucketDebugEntry{ kv.first, total_path_length_mm(kv.second), 0.0, "kept", -1 };

    // (a) C3 min-benefit gate, v2.3 Task 1 (spec C1-C2) two-tier rework: each bucket's
    // own eff_min starts at min_len_free_mm if its extruder is in `free_extruders`
    // (already printing wall/solid/sparse geometry at this z elsewhere on the plate -
    // its toolchange costs nothing extra) or min_len_mm otherwise, THEN is halved (spec
    // C2) if its extruder is also in `prev_kept` (the previous support layer's
    // committed set) - the two preferences stack rather than one overriding the other.
    // Default call (free_extruders empty) makes every bucket take the min_len_mm/
    // prev_kept-halved path only - byte-identical to the pre-v2.3 flat-threshold gate
    // this replaces, just re-expressed per-bucket instead of once for the whole call.
    // gate_dropped accumulates in `map`'s own ascending-extruder iteration order.
    for (auto it = map.begin(); it != map.end(); ) {
        const bool is_free = free_extruders.count(it->first) != 0;
        double     eff_min = is_free ? min_len_free_mm : min_len_mm;
        if (prev_kept.count(it->first) != 0)
            eff_min *= 0.5;
        if (total_path_length_mm(it->second) < eff_min) {
            if (debug_out)
                dbg[it->first].outcome = "gated";
            gate_dropped.push_back({ it->first, std::move(it->second) });
            it = map.erase(it);
            ++result.buckets_dropped_min_benefit;
            // v2.4 Task C (spec C): `is_free` is already computed above for this same
            // bucket - free-tier subset split, see BucketCapResult's own comment.
            if (is_free)
                ++result.buckets_dropped_min_benefit_free;
        } else {
            ++it;
        }
    }

    // (b) C1 distinct-extruder trim: rank the gate's survivors by (previously-kept
    // DESC, total length DESC, extruder id ASC as the final deterministic tie-break)
    // and keep only the top max_extruders. Hysteresis outranks length outright - see
    // this function's header comment for why (stability up the column vs. the
    // alternating-stripe artifact the old whole-layer revert caused). trim_dropped
    // accumulates in RANK order (worst-of-the-discarded last) - the legacy fallback
    // path below replays that order verbatim; the redirect path re-sorts its own
    // copy to ascending extruder instead (see there for why the two need different
    // orders).
    //
    // v2.5b (spec: "free-extruder trim exemption"): a bucket whose extruder is in
    // `free_extruders_exempt` is pulled out BEFORE ranking - it is never a trim
    // candidate at all, so it neither competes for nor consumes one of the
    // max_extruders slots (see this function's own header comment in the .hpp for
    // the toolchange-pricing rationale and the strict-vs-windowed distinction
    // between this set and the gate's `free_extruders`). The trim budget below then
    // applies only to `ranked` (the non-exempt survivors) - if map.size() overall
    // still exceeds max_extruders only because of exempt buckets, nothing is
    // trimmed at all.
    if (map.size() > max_extruders) {
        std::vector<std::pair<unsigned, double>> ranked;
        ranked.reserve(map.size());
        for (auto &kv : map) {
            if (free_extruders_exempt.count(kv.first) != 0) {
                ++result.buckets_exempt_kept;
                if (debug_out)
                    dbg[kv.first].outcome = "kept_exempt";
                continue; // exempt: never ranked, never trimmed, no slot consumed
            }
            ranked.emplace_back(kv.first, total_path_length_mm(kv.second));
        }

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
            if (debug_out)
                dbg[it->first].outcome = "trimmed";
            trim_dropped.push_back({ it->first, std::move(it->second) });
            map.erase(it);
            ++result.buckets_trimmed_cap;
        }
    }

    // `kept` is exactly `map`'s remaining keys right now - final, and never changed
    // by the redirect phase below (that phase only ever APPENDS into an existing
    // kept bucket's geometry, never adds/removes a key).
    for (const auto &kv : map)
        result.kept.insert(kv.first);

    // v2.5a redirect phase (spec item 2, residual-paint fix) - see this function's
    // header comment (BrimFilament.hpp) for the full algorithm this implements.
    if (map.empty()) {
        // Phase 1 (no survivor anywhere): legacy fallback, byte-identical append
        // order to every pre-v2.5a call - gate_dropped in its own ascending-extruder
        // encounter order, then trim_dropped in its own rank order. In practice
        // trim_dropped is only ever non-empty here when max_extruders == 0 (an
        // empty `map` after the gate can never satisfy `map.size() > max_extruders`
        // for any max_extruders >= 1, so the trim block above never even runs) -
        // handled anyway so the "byte-identical to legacy" claim holds unconditionally,
        // not just for today's real call site (which always passes max_extruders=2).
        for (DroppedBucket &d : gate_dropped)
            merge_back_target.append(std::move(d.geometry.entities));
        for (DroppedBucket &d : trim_dropped)
            merge_back_target.append(std::move(d.geometry.entities));
        // CHAMELEON_DEBUG: no survivor at all this layer - every dbg entry keeps
        // whatever outcome the gate/trim loops above already assigned it (gated or
        // trimmed; "kept"/"kept_exempt" cannot occur here since `map` is empty), and
        // `redirected_into` stays -1 for all of them (legacy fallback, not a redirect).
        if (debug_out) {
            debug_out->clear();
            debug_out->reserve(dbg.size());
            for (auto &kv : dbg)
                debug_out->push_back(std::move(kv.second));
        }
        return result;
    }

    // Phase 2: snapshot every surviving bucket's bbox center ONCE, before ANY
    // redirect append below runs - this is what makes the outcome independent of
    // processing order (see header comment's worked example).
    std::map<unsigned, Point> survivor_centroids;
    for (const auto &kv : map)
        survivor_centroids.emplace(kv.first, chameleon_collection_bbox_center(kv.second));

    // Phase 3 processing order: gate_dropped first (already ascending extruder),
    // then trim_dropped - re-sorted to ascending extruder for THIS phase only (its
    // rank order, used by phase 1 above, has no meaning here; the redirect target
    // is decided purely by centroid distance/tie-break, not by trim rank).
    std::sort(trim_dropped.begin(), trim_dropped.end(),
        [](const DroppedBucket &a, const DroppedBucket &b) { return a.extruder < b.extruder; });

    auto redirect_one = [&](DroppedBucket &dropped) {
        const Point centroid = chameleon_collection_bbox_center(dropped.geometry);
        unsigned best_extruder = 0;
        int64_t  best_dist2    = 0;
        bool     have_best     = false;
        for (const auto &sv : survivor_centroids) {
            // Scaled-int squared distance - no float comparison anywhere in the
            // redirect target decision.
            const int64_t dist2 = (centroid - sv.second).cast<int64_t>().squaredNorm();
            bool candidate_wins;
            if (!have_best) {
                candidate_wins = true;
            } else if (dist2 != best_dist2) {
                candidate_wins = dist2 < best_dist2;
            } else {
                // Exact tie: prev_kept DESC, then extruder id ASC.
                const bool cand_prev = prev_kept.count(sv.first) != 0;
                const bool best_prev = prev_kept.count(best_extruder) != 0;
                candidate_wins = (cand_prev != best_prev) ? cand_prev : (sv.first < best_extruder);
            }
            if (candidate_wins) {
                best_extruder = sv.first;
                best_dist2    = dist2;
                have_best     = true;
            }
        }
        map.at(best_extruder).append(std::move(dropped.geometry.entities));
        ++result.buckets_redirected;
        if (debug_out)
            dbg[dropped.extruder].redirected_into = int(best_extruder);
    };

    for (DroppedBucket &d : gate_dropped)
        redirect_one(d);
    for (DroppedBucket &d : trim_dropped)
        redirect_one(d);

    // CHAMELEON_DEBUG: final pass over the surviving buckets - a kept bucket may have
    // GROWN via the redirect loop just above, so its "after" length can only be read
    // once every redirect_one call has finished appending into `map`.
    if (debug_out) {
        for (const auto &kv : map)
            dbg[kv.first].length_after_mm = total_path_length_mm(kv.second);
        debug_out->clear();
        debug_out->reserve(dbg.size());
        for (auto &kv : dbg)
            debug_out->push_back(std::move(kv.second));
    }

    return result;
}

int chameleon_dominant_matched_extruder(const std::map<unsigned, ExtrusionEntityCollection> &buckets)
{
    int    dominant_extruder = -1;
    double dominant_len      = -1.0;
    for (const auto &kv : buckets) {
        if (kv.second.entities.empty())
            continue;
        const double len = total_path_length_mm(kv.second);
        if (dominant_extruder < 0 || len > dominant_len) {
            dominant_extruder = int(kv.first);
            dominant_len      = len;
        }
        // Tie (len == dominant_len): keep the earlier pick - `buckets` is key-
        // ordered ascending and only a STRICTLY greater length replaces it, so the
        // lowest extruder id among exact-tied buckets wins, deterministically.
    }
    return dominant_extruder;
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
