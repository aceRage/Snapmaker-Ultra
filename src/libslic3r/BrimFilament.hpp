#ifndef slic3r_BrimFilament_hpp_
#define slic3r_BrimFilament_hpp_
#include "WallSampleIndex.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExPolygon.hpp"
#include "BoundingBox.hpp"
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>
namespace Slic3r {

// v2.3 Task 3 (spec C5): per-object descend-hysteresis state - see
// BrimVoteParams::descended_last_layer and partition_support_entities' collection
// branch (the DESCEND decision) in the .cpp. Keyed by a QUANTIZED collection bbox
// center (chameleon_quantize_point below, ~2mm grid - "the same physical column" across
// consecutive support layers, approximately) rather than any stable topological id -
// tree support branches have no such id available to this pass, and the spec explicitly
// asked to "keep simple + documented" rather than track true branch topology. The bool
// value is always true when present (a column is only ever recorded here the layer it
// actually descended - see partition_support_entities, which never writes a `false`
// entry); a column simply absent from the map is "did not descend last layer", the same
// answer an explicit `false` would give, so callers only ever need to check membership.
using DescendColumnMap = std::map<Point, bool>;

struct BrimVoteParams {
    size_t k = 3;
    double tie_score_ratio = 0.30;      // top-two scores differ < 30% => tie path
    double tie_dist_mm     = 0.3;       // nearest distances differ < 0.3mm => tie path
    double sample_mm       = 0.8;
    double min_run_mm      = 2.0;       // shorter runs absorbed
    size_t max_runs        = 4;         // guard cap per object brim
    // v2.3 final-review M4 fix: gates split_polyline_core's ring-seam merge (spec C7,
    // BrimFilament.cpp ~196) - default FALSE so every pre-v2.3-final-review caller (most
    // notably Part 1's own brim BrimVoteParams, built independently in Print::process's
    // brim call site, which never sets this) is unaffected: the plan's "Part 1 brim
    // behavior stays byte-identical" contract was violated by C7 firing unconditionally
    // for ANY loop, including feature-ON Part 1 brim loops sharing split_polyline_core
    // via partition_leaf_entity -> split_polyline_by_vote (:250/:696) - see the v2.3
    // final review, finding M4. The support pass (Print.cpp's
    // chameleon_assign_support_interfaces) explicitly sets this true on its own
    // vote_params before copying it into every per-role BrimVoteParams instance it
    // builds, so support ring/loop splitting keeps the C7 merge (and its I1 wrap-closure
    // fix) exactly as before; brim's own vote_params never touches this field, so it
    // stays false and the merge block is unreachable for brim, restoring the byte-
    // identical claim as actually true rather than aspirational.
    bool merge_ring_seam   = false;
    // object_key -> layer-0 area (for tie-break 1); larger area wins
    std::map<size_t, double> object_area;
    unsigned fallback_extruder = 0;     // used when index empty / no candidates
    // WARNING (v2.5c+): partition_support_leaf_entity and vote_collection_as_unit now
    // interpret a resolver returning fallback_extruder as a real color match,
    // indistinguishably from a genuine nearest-wall match. With max_dist_mm=0 (uncapped,
    // Part 1 brim) this is unreachable-safe (brim_vote with non-empty index always
    // returns a real nearest extruder). A future capped resolver (max_dist_mm > 0
    // returning fallback beyond the cap) would silently bucket AND ToolOrdering-register
    // unmatched geometry as the fallback COLOR, not residual support_fills.
    // 0 = uncapped (Part 1 brim path; behavior identical to pre-v2.1). > 0: if
    // the nearest knn sample is farther than this, brim_vote returns
    // fallback_extruder before scoring (v2.1 lateral-proximity rule).
    double max_dist_mm     = 0.0;
    // v2.3 Task 1 (spec C3): the extruder set the PREVIOUS support layer committed
    // (same value threaded into apply_bucket_caps' own prev_kept argument - see
    // Print.cpp's chameleon_assign_support_interfaces, which keeps both in sync each
    // layer). Default EMPTY, which makes every new branch that reads this a strict
    // no-op: brim_vote's tie path and vote_collection_as_unit's majority-tie path both
    // only special-case a NON-empty membership hit, so Part 1's brim call sites (which
    // never set this) and every pre-v2.3 support caller see byte-identical behavior.
    std::set<unsigned> prev_kept;
    // v2.3 Task 3 (spec C5): the set of columns (quantized bbox-center keys, see
    // DescendColumnMap above) whose whole-collection vote DESCENDED (was split leaf-by-
    // leaf rather than moved/left whole) on the PREVIOUS support layer that reached the
    // engine calls - threaded through Print.cpp's per-object loop the same way prev_kept
    // is (see chameleon_assign_support_interfaces). A column present here gets HALF the
    // normal min_run_mm descend threshold this layer (partition_support_entities'
    // collection branch) - a real minority arc that's been descending consistently
    // doesn't need to re-clear the FULL bar every single layer, the same "stability up
    // the column" rationale prev_kept/C2 already established for the gate/trim caps,
    // just with a simpler one-shot rule here (no multi-layer grace counter like C2's -
    // see Print.cpp's own comment on why). Default EMPTY, a strict no-op for any caller
    // that never wires this up (every pre-v2.3 caller, and Part 1's brim path, which
    // never calls vote_collection_as_unit's C5 descend path at all).
    DescendColumnMap descended_last_layer;
};

// v2.3 Task 3 (spec C5): quantizes a point to a coarse (~quantize_mm) grid cell,
// identifying "the same physical column" across consecutive support layers for
// BrimVoteParams::descended_last_layer above - floor-based bucketing (not rounding), so
// a value sitting exactly on a cell boundary is deterministic and doesn't depend on
// which side of .5 it lands. This is a DELIBERATE APPROXIMATION, not true column
// tracking: a tapering branch whose XY center drifts by more than quantize_mm between
// consecutive support layers, or a column that splits/merges, will not reliably map to
// the same key every layer - true topology tracking would need to follow the tree
// support generator's own branch graph, well outside this pass's scope (spec: "keep
// simple + documented"). quantize_mm defaults to 2.0mm (spec: "quantize ~2mm").
Point chameleon_quantize_point(const Point &p, double quantize_mm = 2.0);

// v2.3 Task 3 (spec C5): the bbox CENTER (not a true area centroid) of every point
// across every leaf reachable inside `collection` (collection.flatten(), the same full
// recursion vote_collection_as_unit's own histogram walk uses) - the "where is this
// collection" input to chameleon_quantize_point above. Returns Point(0, 0) for an
// empty/all-empty-leaf collection (no geometry to center on); partition_support_entities
// only ever calls this after vote_collection_as_unit has already found at least two
// distinct votes, so that degenerate case does not arise in practice.
Point chameleon_collection_bbox_center(const ExtrusionEntityCollection &collection);

// Vote for one point. Deterministic. Returns extruder id.
unsigned brim_vote(const WallSampleIndex& idx, const Point& pt, const BrimVoteParams& p);

// One contiguous same-extruder piece of a source polyline.
struct BrimRun { unsigned extruder; Points pts; };

// Sample `poly` (closed if is_loop), vote per sample, group runs, absorb runs
// shorter than min_run_mm into the previous run, then coalesce smallest runs
// until <= max_runs. Result covers the whole polyline in order.
std::vector<BrimRun> split_polyline_by_vote(const Points& poly, bool is_loop,
                                            const WallSampleIndex& idx,
                                            const BrimVoteParams& p);

// Same sampling/absorb/max_runs/shared-boundary-vertex semantics as
// split_polyline_by_vote, but voting per sample is delegated to `resolver`
// instead of a fixed WallSampleIndex knn vote. split_polyline_by_vote is the
// knn-vote instance of this (they share one run-building core).
std::vector<BrimRun> split_polyline_by_resolver(const Points& poly, bool is_loop,
                                                const std::function<unsigned(const Point&)>& resolver,
                                                const BrimVoteParams& p);

// Partition `brim` (one object's collection, plate coords): entities whose
// dominant vote == own_extruder stay in `kept`; others land in out[extruder].
// Loop/path entities are split via split_polyline_by_vote; runs become
// ExtrusionPaths (erBrim) copying the source entity's flow attributes.
void partition_brim_by_wall(const ExtrusionEntityCollection& brim,
                            unsigned own_extruder,
                            const WallSampleIndex& idx,
                            const BrimVoteParams& p,
                            ExtrusionEntityCollection& kept,
                            std::map<unsigned, ExtrusionEntityCollection>& out);

// v2.4 Task B (spec B): the contact-band width, hoisted out of a hardcoded literal so
// select_contact_layers' own default AND chameleon_layer_free_extruders' windowed query
// (its up_mm side, see below) share exactly one number - the claw fix (spec root cause:
// free-eligibility used strict z-coincidence, so a wall that only exists in the contact
// band above its support column was never "free" there) depends on the free-extruder
// window covering PRECISELY the same band select_contact_layers already selects for
// voting; two independently-tuned constants could silently drift apart.
constexpr double kContactBandMm = 2.0;

// Indices of object layers whose TOP z lies in (lo_z, hi_z].
// print_zs = ascending layer TOP z values; layer i spans (print_zs[i-1], print_zs[i]].
std::vector<size_t> select_layers_in_band(const std::vector<double>& print_zs,
                                          double lo_z, double hi_z);

// Indices of object layers whose TOP z lies in (support_top_z, support_top_z + gap_mm].
// Thin call onto select_layers_in_band(print_zs, support_top_z, support_top_z + gap_mm).
// Requires gap_mm > max layer height, else the direct contact layer itself is dropped.
std::vector<size_t> select_contact_layers(const std::vector<double>& print_zs,
                                          double support_top_z, double gap_mm = kContactBandMm);

// v2.1 final-review I2 fix: select_layers_in_band's sibling for bands that are only
// ONE layer tall (e.g. the coplanar lateral band, sized to a support layer's own
// height). select_layers_in_band only tests a layer's TOP z, so a layer whose top
// overshoots hi_z but whose BOTTOM still dips into (lo_z, hi_z] - its walls flank the
// band at this z even though its own top lies above it (unsynced support/object layer
// grids, variable layer height) - is missed. This selects by z-INTERVAL overlap
// instead: layer i spans (bottom_i, print_zs[i]], where bottom_i is the previous
// layer's top; for i == 0 it is `first_bottom_z` (default 0.0 = plate). Raft prints
// elevate the first layer, so raft-aware callers pass its true bottom
// (first print_z - first height), else raft-level bands falsely overlap layer 0's span
// (re-review N2). Layer i is selected when that interval overlaps (lo_z, hi_z].
std::vector<size_t> select_layers_overlapping_span(const std::vector<double>& print_zs,
                                                    double lo_z, double hi_z,
                                                    double first_bottom_z = 0.0);

// v2.2 Task 4 (spec C8, "nearest_wall" comparison mode): dedupe-union of two layer-
// index lists - nearest_wall builds its single WallSampleIndex over the UNION of the
// contact-band layers (select_contact_layers) and the coplanar-span layers
// (select_layers_overlapping_span), so a layer index selected by both bands only
// contributes its walls once (WallSampleIndex::add_polyline called twice on the same
// layer would double-weight it in brim_vote's 1/d^2 scoring). Pure/unit-testable
// without a PrintObject scaffold, same free-function pattern as select_contact_layers/
// select_layers_overlapping_span above. Neither input needs to already be sorted or
// duplicate-free (each caller-side selector already returns ascending, duplicate-free
// indices in practice, but this makes no assumption of its own); the result is always
// ascending and duplicate-free.
std::vector<size_t> union_layer_indices(const std::vector<size_t>& a, const std::vector<size_t>& b);

// v2.1 Task 2 (projection resolver): pure geometric core of Print.cpp's
// chameleon_projection_extruder_from_view (fed by chameleon_build_projection_views;
// v2.1 final-review M1 fix hoisted that construction to once per support layer instead
// of once per sample point - see those two functions' own comments). Layer/LayerRegion
// can't be built standalone outside a full slice (private/protected ctors, PrintObject-
// owned storage), so the point-in-band-layer / region-preference selection is factored
// out here as a plain-data view over ExPolygon POINTERS (no ownership, no copying - a
// LayerRegion's true storage is SurfaceCollection, not a bare ExPolygons, so the caller
// collects `&surface.expolygon` pointers per call).
struct ProjectionLayerView {
    // This band layer's lslices (required - a null/empty pointer is a miss) and, in
    // parallel (index i matches lslices[i]), each island's precomputed bbox for a cheap
    // AABB gate before the exact point-in-polygon test. lslices_bboxes may be left
    // null or size-mismatched to skip the gate (falls straight through to the exact
    // test) - Layer::lslices_bboxes is populated at slicing time (PrintObjectSlice.cpp),
    // well before this pass runs, so the common case is a free (already-computed) gate.
    const ExPolygons*              lslices = nullptr;
    const std::vector<BoundingBox>* lslices_bboxes = nullptr;
    // v2.2 Task 2 (spec C5, root cause 4): this band layer's lslices, offset OUTWARD by
    // SUPPORT_MATERIAL_MARGIN + this object's support_expansion - the same margin the
    // support generator itself grows interface contact polygons by beyond the overhang
    // (root cause 4: contact polygons are grown by that margin, so samples over small
    // features can land outside raw lslices even though the generator's own contact
    // polygon covered them). See Print.cpp's chameleon_build_projection_views for the
    // exact offset call and the margin constant's citation/mirror. Unlike lslices/
    // region_slice_polys/region_bottom_polys above (which only ever alias a Layer's own
    // storage), this is OWNED storage: offset_ex produces brand-new geometry that
    // doesn't exist anywhere else to point into. Populated once per band layer at view
    // build (same M1 hoisting rationale as the rest of this struct). Left empty (there
    // is no pointer to leave null) for a layer that contributes nothing.
    ExPolygons                      expanded_lslices;
    // Per-island bbox of expanded_lslices (index i matches expanded_lslices[i]),
    // computed FRESH from the offset result - not a naive "take lslices_bboxes and
    // inflate by the margin", because offset_ex can merge or split islands (two lslices
    // islands within ~2x the margin of each other coalesce into one), so a 1:1 mapping
    // from lslices_bboxes to expanded_lslices doesn't generally exist. Same cheap-AABB-
    // gate role as lslices_bboxes above; always index-parallel to expanded_lslices (no
    // null/size-mismatch case here - the caller that builds one builds the other).
    std::vector<BoundingBox>        expanded_lslices_bboxes;
    // Per LayerRegion on this layer (same indexing the caller will map back to an
    // extruder): region_slice_polys[r] = pointers into that region's `slices` surfaces
    // (any type - "this region's own shape"); region_bottom_polys[r] = pointers into
    // its `fill_surfaces` surfaces that are stBottom/stBottomBridge only (the tie-break
    // hint). Both may be empty for a region that contributes nothing.
    std::vector<std::vector<const ExPolygon*>> region_slice_polys;
    std::vector<std::vector<const ExPolygon*>> region_bottom_polys;
};

// `layers` must already be ordered lowest band layer first (the same order
// select_layers_in_band/select_contact_layers return). "SURFACE ABOVE WINS" (the
// user-approved priority - see Print.cpp's chameleon_assign_support_interfaces) means
// the LOWEST band layer whose RAW geometry genuinely contains p must win, full stop,
// no matter what any OTHER layer's margin ring also happens to cover. v2.2 Task 2 (spec
// C5) added the margin ring (expanded_lslices) as a rescue for samples the grown contact
// polygon pushed outside all raw geometry (see PASS 2 below) - it is a fallback for when
// NOTHING raw covers p anywhere in the band, never a way to jump the queue ahead of a
// higher layer's real containment. This matters in practice: with the standard one-layer
// top gap, a band's FIRST (lowest) layer is typically wall-only - it spans the z-gap
// between the support top and the overhang bottom, so its raw lslices contain only the
// laterally-adjacent wall, and the overhang body itself only starts one layer higher.
// A wall-edge sample commonly lands just inside that first layer's 1.2mm+ ring (support
// lines run close to walls) while also sitting squarely inside the overhang's raw slices
// one layer up - if the ring were allowed to resolve on the lower layer, every such
// sample would mis-color to the wall's extruder instead of the overhang's, inverting
// "surface above wins" along every wall (v2.2 final-review C1). To make that impossible,
// this function runs as two SEPARATE passes over all of `layers`, never interleaved:
//
// PASS 1 (raw containment; wins over PASS 2 unconditionally): scan every layer, lowest
// first; the first layer whose raw lslices contain p (bbox-gated when the relevant
// bboxes are usable) is picked and returned from - a higher layer's raw containment is
// never even reached once a lower layer's raw hit resolves. Within that layer: the
// region whose slice polys contain p, preferring one whose bottom polys ALSO contain p
// when more than one region's slices contain p (first-contains-p wins otherwise, ties
// broken by ascending region index) - unchanged from v2.1. If no region's raw slice
// polys agree with the layer-level raw hit (a pre-existing degenerate case - the layer's
// lslices cover p but its per-region data doesn't), the region whose raw slice polys are
// NEAREST to p on that SAME layer is picked instead (see below) - this layer already won
// PASS 1 on raw containment, so resolution stays on this layer, it does not fall through
// to a higher one.
//
// PASS 2 (margin ring; only reached when PASS 1 found NOTHING): scan every layer again,
// this time HIGHEST band layer FIRST (v2.3 Task 2, spec C4/root cause 4 - the OPPOSITE
// direction from PASS 1), testing expanded_lslices (the margin ring) instead of raw
// lslices. The first layer (in this reversed order) whose ring covers p is picked,
// resolved via the nearest-region search (below) over ALL of that layer's regions.
// Reaching PASS 2 at all already means no band layer anywhere raw-contains p, so there
// is no raw-containment region scan to redo here - a region's own raw slice polys are
// always a subset of its layer's raw lslices, which PASS 1 has already established as a
// global miss.
//
// This scan-direction asymmetry between PASS 1 (lowest-first) and PASS 2 (highest-
// first) is deliberate, not an oversight - the two passes answer different questions.
// PASS 1 = "nearest surface above": the lowest band layer whose RAW geometry genuinely
// contains p already IS the nearest one, by definition of "lowest that hits first", so
// scanning upward on a miss is correct. PASS 2 = "prefer the overhang ring over the
// wall-gap ring": a band's first (lowest) layer is typically wall-only (see the "SURFACE
// ABOVE WINS" discussion above - it spans the z-gap between the support top and the
// overhang bottom, so its raw lslices see only the laterally-adjacent wall, never the
// overhang body one layer up), so a rim sample whose 1.2mm-grown margin ring is hit by
// BOTH the lower wall layer and the higher overhang layer must resolve to the OVERHANG
// layer - scanning lowest-first here would instead resolve it against the wall layer
// below, inverting "surface above wins" for exactly the rim samples this rescue pass
// exists to help.
//
// Nearest-region search (used both for PASS 1's same-layer degenerate case and for every
// PASS 2 resolution): among the layer's regions, the one whose raw slice polys are
// NEAREST to p, over ALL of this layer's regions (not just the island that produced the
// hit). No separate distance threshold is enforced at this step: the layer-level hit
// test already established p is within the margin of SOME geometry on this layer, so
// "nearest region" is simply "which region does that nearby geometry belong to".
// Two-stage search, per region: (1) a cheap bbox-distance lower bound (point-to-AABB,
// via get_extents on each of the region's raw polys) prunes any region whose lower bound
// already can't beat the current best, without touching its polygon; (2) only surviving
// regions get the exact-ish "distance to polygon" test Slic3r already uses elsewhere
// (MultiPoint::distance_to - nearest-VERTEX distance over contour + holes, not true
// nearest-edge distance; a documented, precedented simplification, not a new one). This
// is a correct branch-and-bound nearest search (the bbox distance is a real lower
// bound), not an approximation of "nearest" itself. Determinism: regions are scanned in
// ascending index order and only a STRICTLY smaller exact distance replaces the current
// best, so an exact tie is won by the lower region index, deterministically, regardless
// of visitation-order edge cases. A region that contributes no raw slice polys at all is
// never a candidate.
//
// Returns false (out_layer/out_region left unwritten) when no band layer's raw or
// expanded lslices cover p, or when every covering layer (raw in PASS 1, ring in PASS 2)
// offers no region with any raw slice geometry at all.
bool chameleon_pick_projection_region(const std::vector<ProjectionLayerView>& layers,
                                      const Point& p,
                                      size_t& out_layer, size_t& out_region);

// v2.5c (root cause fix, diagnostic-log-proven): CHAMELEON_DEBUG logging on the
// user's own GUI slice showed white wall samples plentiful (194-593/layer in the
// claw band), fallback_extruder resolving to 0 (white, the layer-0 min external-
// perimeter color), and ZERO e0 buckets across all 560 layers even though every
// gate/trim/redirect mechanism kept every OTHER bucket (559 e2, 128 e3; every
// bucket that formed survived). Root cause: this function (and its leaf/collection
// handling below) treated a run/entity/collection whose vote == fallback_extruder
// as definitionally UNMATCHED - a fast path kept the original untouched (or
// spliced/appended it back into support_fills), regardless of whether that vote
// was a genuine "no candidate wall nearby" fallback or a REAL painted wall color
// that simply happens to share an id with fallback_extruder (fallback is always
// SOME real extruder - typically a layer-0 model color - so this collision is the
// NORM on painted models, not an edge case). The v2.5a residual pin then painted
// that never-bucketed geometry the dominant SURVIVING bucket's color instead (the
// "teal claw wraps, khaki-heavy tree" symptom). Fixed by removing the fallback
// special-case everywhere in this pass: a vote of fallback_extruder now buckets
// exactly like any other vote - see each bullet below for where that changed.
//
// Partition the `role_filter`-role entities of `support_fills`: `resolver` is
// called per sample point (in place of a fixed WallSampleIndex knn vote) via
// split_polyline_by_resolver.
// - v2.5c: a leaf entity whose vote is UNIFORM (every sample the same extruder,
//   fallback included) moves the ORIGINAL pointer whole into out[extruder] - no
//   clone, no rebuild, loop-ness/can_reverse/every other property preserved
//   automatically (SupportLeafOutcome::MovedWhole in the .cpp; this SUBSUMES the
//   old "uniform fallback stays untouched" fast path - "uniform anything moves
//   whole into its bucket" now, whatever that winner is). A leaf whose vote is
//   genuinely MIXED is split into runs; EVERY run - fallback included - lands in
//   out[run.extruder] (role copied from first_path_of(entity)->role(), falling
//   back to entity->role() - so a base-role split stays erSupportMaterial, never
//   hardcoded to erSupportMaterialInterface). Nothing mixed-vote related returns to
//   support_fills anymore.
// - Entities whose collapsed role() != role_filter are never touched (this includes
//   a nested collection whose own contents mix roles, or match some OTHER role
//   entirely - "collapsed" per ExtrusionEntityCollection::role(), which is already
//   fully recursive: erNone for empty, the single common role for a uniform
//   collection at any nesting depth, erMixed otherwise). The only OTHER way an
//   role_filter-matching entity now stays in place is the degenerate/empty-chain
//   guard (SupportLeafOutcome::Unchanged) - an entity whose polyline collapses to
//   nothing to sample at all.
// - v2.2 Task 3 (spec C7, "nested collections"): a nested ExtrusionEntityCollection
//   whose collapsed role() DOES equal role_filter (tree double-wall branch
//   collections, layer-0 no_sort sheath collections, etc. - previously invisible to
//   this pass entirely, spec root cause 6) is voted as ONE UNIT, never split apart:
//   every leaf polyline reachable via collection.flatten() is sampled at the same
//   0.8mm-default cadence split_polyline_core's build_chain uses (reused directly),
//   `resolver` is called once per sample across the WHOLE collection, and the
//   majority vote decides the outcome (ties broken to the LOWEST extruder id,
//   deterministic - see vote_collection_as_unit's own comment in the .cpp). The
//   winning majority MOVES the original collection pointer whole into out[extruder]
//   (no clone, no per-child split) - v2.5c: this now applies uniformly whether that
//   winner is fallback_extruder or not (pre-v2.5c a fallback majority, or an empty/
//   sample-less collection, instead left the pointer in support_fills untouched -
//   the same collision this whole fix removes). This is also what preserves a
//   moved collection's own `no_sort` flag, since the collection object itself is
//   never touched, only relocated as a whole. Matched originals (leaf OR whole
//   collection) are deleted from support_fills's own top-level entities vector,
//   never from inside a collection that stays behind.
// - v2.3 Task 3 (spec C5, root cause 5, "tree selective descent"): the whole-collection
//   vote above (vote_collection_as_unit) now ALSO reports the raw per-extruder sample
//   histogram and the longest contiguous run (mm) of samples that voted some extruder
//   OTHER than the winner ("the minority's contiguous run"). A collection whose vote is
//   UNIFORM (histogram has only one distinct extruder) or whose minority run is SHORTER
//   than the descend threshold behaves EXACTLY as the C7 paragraph above describes -
//   whole-move, pointer-stable, no change (this threshold decision itself is untouched
//   by v2.5c - only the fallback-winner OUTCOME changed, per the C7 paragraph's own
//   v2.5c note). A collection whose minority run reaches the threshold DESCENDS
//   instead: every LEAF entity reachable inside it (recursing into any further-nested
//   sub-collection the same way) is individually partitioned via the SAME per-leaf
//   logic the top-level leaf case above already uses - v2.5c: a leaf whose own vote is
//   UNIFORM (fallback included) moves whole into out[extruder], pointer-stable, exactly
//   like the top-level MovedWhole case (pre-v2.5c only a uniform non-fallback vote
//   moved, and even then it was REBUILT as a new ExtrusionPath rather than moved -
//   ExtrusionLoop/MultiPath-ness was lost; a uniform fallback vote stayed in place
//   untouched. Both of those are gone: the move is now pointer-stable for ANY uniform
//   winner, no rebuild, no loop-ness loss). A leaf with a genuinely mixed vote is split
//   into runs, and EVERY run - fallback included, v2.5c - goes to out[extruder]; no run
//   is ever spliced back into the leaf's immediate parent collection anymore (pre-v2.5c
//   fallback runs were SPLICED BACK IN PLACE at the leaf's own index instead). Every
//   rebuilt ExtrusionPath (the genuinely-mixed split case only - a MovedWhole leaf needs
//   no such handling, being the same object) inherits the SOURCE LEAF's own can_reverse()
//   - see ExtrusionEntity::can_reverse()/ExtrusionPath::set_reverse() (ExtrusionEntity.hpp)
//   - a branch-wall leaf built with reversal disabled (Support/SupportCommon.cpp:660-663/
//   765-767, "always start with the anchor") must keep that property on its split
//   pieces, or GCode's chain_and_reorder is free to flip a piece's direction and break
//   the seam anchor (spec: "seam-anchor blob hazard"). A (possibly nested) sub-
//   collection left fully empty by this recursion is itself removed from its parent and
//   deleted - never left behind as a dangling empty shell - the SAME rule the top-level
//   collection pointer follows: it stays in support_fills, still the same object,
//   unless the descend emptied it entirely, in which case it is deleted exactly once
//   (v2.5c: since a descended collection's leaves now ALL re-bucket, including any
//   uniformly-fallback ones, a descend is more likely than before to empty the
//   collection out entirely - this is expected, not a bug: the geometry did not
//   disappear, it simply landed in out[fallback] instead of staying behind).
//   The descend threshold is `p.min_run_mm` (support-pass override, spec C6: 1.6mm),
//   HALVED for a column found in `p.descended_last_layer` (see DescendColumnMap's own
//   comment above) - if `descended_out` is non-null, every collection this call decides
//   to DESCEND records its quantized column key into it as true (never a `false` entry -
//   see DescendColumnMap's own comment for why absence already means false), so the
//   caller can carry that forward as next layer's `descended_last_layer`. Passing
//   `descended_out = nullptr` (the default) makes the halving lookup still work off
//   `p.descended_last_layer` but skips recording - every pre-v2.3 call site (this
//   function's own existing unit tests included) compiles and behaves unchanged, since
//   `p.descended_last_layer` also defaults empty (C5 is then a strict no-op: the
//   threshold is always the full `p.min_run_mm`, same as before this task whenever a
//   collection's minority run happens to reach it - which, pre-C5, cannot happen at all,
//   since vote_collection_as_unit had no minority-run concept until this task).
// Returns switch-boundary count added (for the per-object cap accounting) - a moved
// whole collection (or leaf) does not contribute to this count (it isn't split into
// runs); a genuinely split entity/DESCENDED collection contributes `runs.size() - 1`
// per leaf touched, summed (mirrors the top-level leaf case's own accounting).
//
// Knock-on effects of this fix, verified elsewhere and NOT requiring their own code
// changes (both already generic over `map`'s/`out`'s extruder keys, with no
// fallback_extruder special-casing to remove):
// - apply_bucket_caps (below): the fallback-color bucket now competes in the gate/
//   trim/redirect tiers like any other bucket - no special-casing added, deliberately
//   (the fallback extruder is usually, but not guaranteed, strict-free at every z; the
//   existing free/prev_kept tiers decide, same as for any other bucket).
// - ToolOrdering.cpp's collect_extruders (~734-736): registers every non-empty
//   out[]-derived bucket's extruder on its layer unconditionally, so out[fallback]
//   surviving the caps now may add a toolchange on a layer where the fallback
//   extruder wasn't otherwise present that layer - this is the intended price of
//   printing the color the user actually painted, not a regression to guard against.
size_t partition_support_entities(ExtrusionEntityCollection& support_fills,
                                  ExtrusionRole role_filter,
                                  unsigned fallback_extruder,
                                  const std::function<unsigned(const Point&)>& resolver,
                                  const BrimVoteParams& p,
                                  std::map<unsigned, ExtrusionEntityCollection>& out,
                                  DescendColumnMap* descended_out = nullptr);

// Thin wrapper: partition_support_entities with role_filter = erSupportMaterialInterface
// and a knn-vote resolver over `idx`. Kept for existing call sites / unit tests.
size_t partition_support_interfaces(ExtrusionEntityCollection& support_fills,
                                    unsigned fallback_extruder,
                                    const WallSampleIndex& idx,
                                    const BrimVoteParams& params,
                                    std::map<unsigned, ExtrusionEntityCollection>& out);

// v2.2 Task 1 (spec C1-C3, "cap semantics rework"): total path length (mm) of
// `collection`'s entities. ExtrusionEntityCollection::length() throws (see its own
// comment) - a bucket built by partition_support_entities can hold four kinds of
// leaves: ExtrusionPath*, original ExtrusionLoop/ExtrusionMultiPath (MovedWhole v2.5c+),
// and whole ExtrusionEntityCollection (C7). This function recurses into any nested
// collection (is_collection()) so it handles all types correctly. Each leaf's own
// length() is in SCALED units (see Line::length()/MultiPoint::length()) -
// unscale<double> converts to mm, the same way GCode.cpp/ExtrusionEntity.cpp do at
// every other length() call site.
double total_path_length_mm(const ExtrusionEntityCollection& collection);

// v2.2 Task 1 (spec C1-C3): result of one apply_bucket_caps call.
struct BucketCapResult {
    // The extruders whose buckets survived both caps this layer - i.e. `map`'s
    // remaining keys. Empty when nothing survived (including the trivial case where
    // `map` was empty going in). Feed this straight into the NEXT support layer's
    // `prev_kept` argument for hysteresis (spec: "prev_kept updates to the committed
    // set each layer" - a layer that reaches apply_bucket_caps at all always updates
    // it, even to empty; only a layer whose caller skips calling apply_bucket_caps
    // entirely - no engine call ran - leaves the caller's prev_kept unchanged).
    std::set<unsigned> kept;
    // Buckets merged back by the C3 min-benefit gate (total length < its tier floor -
    // min_len_mm for a normal bucket, min_len_free_mm for one in free_extruders).
    size_t buckets_dropped_min_benefit = 0;
    // v2.4 Task C (spec C): free-tier SUBSET of buckets_dropped_min_benefit above - how
    // many of the total drops were a bucket whose extruder was already in
    // free_extruders (so it was gated against min_len_free_mm, not min_len_mm). Cheap
    // to track since the gate loop already computes `is_free` per bucket; lets a triage
    // reader tell apart "the claw fix's free-tier window still isn't rescuing enough
    // length" from "12mm normal-tier churn dominates" without re-deriving it from raw
    // per-layer free_extruders sets. Always <= buckets_dropped_min_benefit; the normal-
    // tier count is the difference, not tracked as its own field.
    size_t buckets_dropped_min_benefit_free = 0;
    // Buckets merged back by the C1 distinct-extruder trim (survived the gate, but
    // ranked below the top max_extruders by (prev_kept membership, then length)).
    size_t buckets_trimmed_cap = 0;
    // v2.5a (spec item 2, residual-paint fix): how many gated/trimmed buckets were
    // REDIRECTED into a surviving bucket instead of being merged back to
    // `merge_back_target` - see apply_bucket_caps' own doc comment below for the
    // full redirect algorithm. A bucket only ever counts here when `kept` ended
    // non-empty (a survivor existed to redirect into); the legacy fallback path
    // (no survivors at all) never increments this. Always <=
    // buckets_dropped_min_benefit + buckets_trimmed_cap; the two counters that feed
    // it are unchanged by this task (same buckets are gated/trimmed as before -
    // this field only reports where their geometry ENDED UP, not a new drop
    // reason).
    size_t buckets_redirected = 0;
    // v2.5b (spec: "free-extruder trim exemption"): how many buckets skipped the C1
    // trim entirely because their extruder was in `free_extruders_exempt` - see
    // apply_bucket_caps' own doc comment below for why an exempt bucket neither
    // competes for nor consumes one of the `max_extruders` slots. Only counted when
    // the trim step actually ran its partition (map.size() > max_extruders going
    // in) - an exempt bucket that would have survived the trim anyway (map already
    // <= max_extruders) is still "kept", just never counted here, since the trim
    // never touched anything that layer. Cheap to track (the trim's own partition
    // loop already tests free_extruders_exempt membership per bucket).
    size_t buckets_exempt_kept = 0;
};

// CHAMELEON_DEBUG (v2.5c diagnostic instrumentation, docs/superpowers/sdd/2026-08-29-
// support-interface-match/progress.md v2.5a section): one bucket's before/after/outcome
// accounting for a single apply_bucket_caps call, populated only when that call's own
// trailing `debug_out` parameter is non-null (see apply_bucket_caps' own doc comment
// below for exactly which decision point writes which field). Print.cpp's
// chameleon_assign_support_interfaces threads a CHAMELEON_DEBUG-gated pointer to this
// from its once-per-pass env check; every pre-existing call site (this file's own
// existing unit tests included) passes nullptr (the default) and is unaffected -
// nothing here is read or written when the caller doesn't ask for it. Diagnostic-only:
// no production code path reads this type.
struct ChameleonBucketDebugEntry {
    unsigned    extruder = 0;
    // total_path_length_mm() of this bucket's geometry as it stood at apply_bucket_caps'
    // own entry (before the gate/trim run at all).
    double      length_before_mm = 0.0;
    // total_path_length_mm() of this SAME extruder's bucket in `map` after the call
    // returns - only meaningful when `outcome` is "kept"/"kept_exempt" (a bucket that
    // survived may have GROWN if another dropped bucket's geometry was redirected into
    // it, v2.5a); left at 0.0 for a gated/trimmed bucket, whose own geometry no longer
    // lives under this extruder's key at all (see `redirected_into` for where it went).
    double      length_after_mm = 0.0;
    // One of "kept" (survived both the gate and the trim, ranked normally), "kept_exempt"
    // (survived the trim without ever being ranked - v2.5b free_extruders_exempt),
    // "gated" (dropped by the C3 min-benefit gate), or "trimmed" (survived the gate but
    // ranked below max_extruders in the C1 distinct-extruder trim).
    std::string outcome;
    // Extruder id this bucket's geometry was redirected into (v2.5a phase 2/3), or -1
    // when no redirect happened - either because this bucket was kept (nothing to
    // redirect) or because it hit the legacy no-survivor fallback (map was empty going
    // into the redirect phase - see apply_bucket_caps' own "Phase 1" comment) and merged
    // into merge_back_target instead.
    int         redirected_into = -1;
};

// v2.2 Task 1 (spec C1-C3, replaces the old per-layer ">3 switch-boundaries" whole-
// layer revert and the per-object ">20 cumulative switches" escalation - both deleted
// entirely, C2). `map` holds one support layer's matched geometry, keyed by extruder,
// straight out of one or more partition_support_entities calls sharing the same out
// map. Applied in order:
//   a. C3 min-benefit gate: any bucket whose total_path_length_mm() < min_len_mm is
//      DROPPED (moved out of `map`, NOT yet appended anywhere - see the v2.5a
//      redirect phase below for where it actually lands) - a matched sliver isn't
//      worth a toolchange/purge, and must never survive the trim below by being one of
//      the "top N longest" among other slivers.
//   b. C1 distinct-extruder trim: if more than `max_extruders` buckets survive the
//      gate, rank them by (extruder present in `prev_kept` DESC, total path length
//      DESC, extruder id ASC as the final deterministic tie-break) and DROP every
//      bucket past the top `max_extruders` (same "moved out, not yet appended" as the
//      gate above). This measures the TRUE per-layer cost - the number of DISTINCT
//      matched extruders (support fills emit one path-group per extruder; run-
//      boundary/switch counts are a red herring, support fills are one ExtrusionPath
//      per LINE so boundary-crossing runs trip that counter constantly without costing
//      an extra toolchange). Hysteresis: prev_kept membership outranks length outright
//      (a short bucket this object kept last layer beats a longer bucket that's new
//      this layer) - stability up the column is what stops the alternating-stripe
//      artifact a whole-layer revert caused; length only breaks ties among buckets
//      with the same prev_kept status.
//      v2.5b (spec: "free-extruder trim exemption"): BEFORE ranking, any bucket whose
//      extruder is in `free_extruders_exempt` is pulled out and kept unconditionally -
//      it never enters the ranked list, never occupies one of the `max_extruders`
//      slots, and can never be trimmed. `max_extruders` prices DISTINCT toolchanges;
//      a free extruder's toolchange already happens for model geometry at this exact
//      layer (see this function's own trailing doc comment below for the strict-vs-
//      windowed distinction and the ToolOrdering.cpp citation for why that's true),
//      so trimming it buys nothing - the toolchange it "saves" doesn't exist - and
//      only costs fidelity (a small matched bucket like a claw's own wall color
//      loses to two larger, unrelated buckets and gets redirected away from its own
//      geometry). The trim's budget then applies only to the remaining NON-exempt
//      buckets, exactly the pre-v2.5b algorithm otherwise unchanged.
// `map` is left holding only the committed (kept) buckets after (a)/(b) - the caller
// moves it straight into SupportLayer::interface_by_extruder. Steps (a) and (b) above
// are themselves UNCHANGED by v2.5a: the same buckets are gated/trimmed, in the same
// order, by the same thresholds - `kept`/max_extruders/prev_kept/retention semantics
// are untouched, and no new toolchange is ever introduced (a toolchange is driven by
// the DISTINCT-EXTRUDER SET in `kept`, which this task never alters).
//
// v2.5a (spec item 2, residual-paint fix root cause A): what happens to a dropped
// bucket's geometry is what CHANGES. Pre-v2.5a, every drop from (a) and (b) was
// appended straight into `merge_back_target` (the layer's residual support_fills) -
// meaning a matched-but-gated/trimmed bucket fell all the way back to the UNMATCHED
// fallback path, where flush_into_support's mark_wiping_extrusions (ToolOrdering.cpp)
// could repaint the WHOLE residual with whatever extruder is purging that layer,
// including a color with no nearby geometry at all (the "khaki mixed into strictly
// white/teal support" symptom). v2.5a instead REDIRECTS a dropped bucket into the
// nearest SURVIVING bucket whenever one exists, so the geometry stays inside a
// MATCHED bucket (never touched by WipingExtrusions - see is_support_overriddable's
// own v2.5a residual pin) instead of degrading to residual. Phases, run strictly
// after (a) and (b) above have already finished (so `map`'s remaining keys - `kept` -
// are final and never change again):
//   1. If `map` is EMPTY (no survivor at all - every bucket was gated/trimmed away),
//      there is nothing to redirect into: fall back to the LEGACY behavior exactly -
//      append every dropped bucket's geometry into `merge_back_target`, in the same
//      order the old code would have (this can only happen via the gate step (a)
//      alone - see the "empty map" note in the .cpp for why the trim step (b) can
//      never be the one to empty `map` in practice - so "the same order" is simply
//      the gate's own ascending-extruder std::map iteration order). Byte-identical to
//      every pre-v2.5a call site's output for this case; the residual pin (item 1)
//      is what makes an all-gated layer safe now, not this function.
//   2. Otherwise, snapshot every surviving bucket's bbox center (chameleon_
//      collection_bbox_center, reused as-is - both a survivor and a dropped bucket
//      are plain ExtrusionEntityCollection) ONCE, before ANY redirect append runs -
//      this is what makes the outcome independent of processing order: redirecting
//      an earlier dropped bucket into a survivor changes that survivor's LIVE bbox,
//      but every decision is made against the frozen snapshot, never the live value.
//   3. Process dropped buckets in a fixed, deterministic order - every gate-drop
//      (ascending extruder, their natural encounter order) THEN every trim-drop
//      (also re-sorted ascending extruder for this phase, even though the trim's OWN
//      append order in step 1's legacy path is its rank order, not extruder order) -
//      and for each, compute ITS OWN bbox center and pick the snapshot survivor with
//      the smallest SQUARED centroid distance, computed in SCALED-INTEGER coordinates
//      (Point::cast<int64_t>().squaredNorm() - no float comparison). Ties (an exact
//      equal squared distance to two or more survivors) break to whichever survivor
//      is in `prev_kept` (DESC - a returning color wins over a merely-closer new one),
//      then to the lower extruder id (ASC) if that still doesn't resolve it. The
//      dropped bucket's entities are appended (same ownership-transferring
//      append(ExtrusionEntitiesPtr&&) the legacy path always used) into the chosen
//      survivor's LIVE bucket in `map` - so later redirects see that bucket grow, but
//      per step 2 above, never re-evaluate its centroid because of it.
// `result.buckets_redirected` counts every bucket that took path 2/3 above (never
// incremented by the legacy path 1). `map`'s KEYS (the committed extruder set) are
// identical to pre-v2.5a for the same inputs either way - only which bucket a given
// dropped geometry's entities end up living inside changes.
// v2.3 Task 1 (spec C1): the "free extruder" table this task adds - one entry per
// DISTINCT object-layer z across every object on the plate (EPSILON-merged: several
// objects/layers that land on the "same" z, within float slicing noise, contribute to
// ONE entry, their filament sets unioned rather than kept as separate near-duplicate
// entries), ascending z, each entry holding the union of every wall/solid/sparse-
// infill filament id (0-based) any object actually prints AT that z. A support layer
// whose OWN z coincides with an entry gets that entry's set as its "free" extruders -
// extruders already paying for a toolchange at that height anyway, so a support bucket
// switching to one of them costs nothing extra (spec root cause 1's free-extruder
// refinement). See chameleon_layer_free_extruders below for the per-support-layer
// query side, and Print.cpp's chameleon_collect_layer_filaments for the once-per-pass
// object/layer walk that builds the raw (z, extruder) samples this table is built
// from - kept separate from that walk so the merge logic itself is a pure function,
// testable without a Print/PrintObject scaffold.
using LayerFilamentTable = std::vector<std::pair<double, std::set<unsigned>>>;

// Pure merge step behind LayerFilamentTable: `raw` is unsorted and may repeat the same
// z many times (one sample per wall/solid/sparse region per object-layer in practice -
// the caller does no de-duplication of its own). Groups samples by z - EPSILON-
// tolerant, the same float-noise tolerance select_layers_in_band/
// select_layers_overlapping_span already use for their own top-z comparisons, since
// independently sliced objects can land on the "same" nominal z with tiny float
// differences - into ascending, duplicate-z-free entries whose set is the union of
// every raw sample's extruder recorded at that z. `raw` is consumed by value (moved
// from) since the caller's own copy is never needed again once this returns.
LayerFilamentTable build_layer_filament_table(std::vector<std::pair<double, unsigned>> raw);

// Pure query side of LayerFilamentTable: the union of every entry's set whose z lies
// within [query_z - down_mm - EPSILON, query_z + up_mm + EPSILON] - a WINDOWED
// coincidence query, not nearest-neighbor: a query whose window contains no entry (no
// object anywhere on the plate has a layer in that z range) returns an empty set - "not
// free" - rather than falling back to the closest entry, since a support layer between
// two object layers isn't actually sharing a toolchange with either one.
//
// v2.4 Task B (spec B, root cause): defaults (down_mm=0, up_mm=0) collapse the window
// back to [query_z - EPSILON, query_z + EPSILON] - EXACT coincidence, byte-identical to
// the pre-v2.4 unwindowed query - so every pre-v2.4 call site (this function's own
// existing unit tests included) compiles and behaves unchanged. The claw fix (spec root
// cause: free-eligibility used strict z-COINCIDENCE, so a white claw wall that exists
// only in the contact band ABOVE its support column was never "free" at the support
// layer's own z, even though the vote itself samples that same band) is entirely the
// caller passing non-zero down_mm/up_mm - see Print.cpp's chameleon_assign_support_
// interfaces call site (down_mm = the support layer's own height, an interval-overlap
// correction for unsynced support/object layer grids; up_mm = kContactBandMm, the SAME
// constant select_contact_layers' band uses, so free-eligibility covers precisely the
// band the vote samples - "a color the sampler can vote is a color the gate must not
// floor at the normal-tier length").
//
// `table` must already be ascending by z (build_layer_filament_table's own contract) -
// this does a binary search (multiple adjacent entries can each fall within the window
// at a merge boundary, so the scan widens outward from the found index rather than
// trusting a single lower_bound hit).
std::set<unsigned> chameleon_layer_free_extruders(const LayerFilamentTable& table, double query_z,
                                                   double down_mm = 0.0, double up_mm = 0.0);

// v2.3 Task 1 (spec C2): per-object hysteresis state threaded through
// chameleon_assign_support_interfaces' per-support-layer loop (Print.cpp) - `prev_kept`
// is the committed extruder set (same value fed into both apply_bucket_caps' own
// prev_kept argument and BrimVoteParams::prev_kept each layer), `retained_last_layer`
// tracks whether the ONE-layer retention grace (see chameleon_update_prev_kept below)
// was already spent on the immediately preceding layer that reached the engine calls -
// without this, an all-gated column could retain stale hysteresis indefinitely instead
// of decaying after a single layer (spec: "counted decay - not indefinite").
struct PrevKeptState {
    std::set<unsigned> prev_kept;
    bool                retained_last_layer = false;
};

// v2.3 Task 1 (spec C2): pure prev_kept update-rule decision, extracted out of
// chameleon_assign_support_interfaces so it's directly unit-testable without a Print/
// PrintObject scaffold. The caller only invokes this for a support layer that actually
// reached apply_bucket_caps this pass - a layer that `continue`s BEFORE the engine
// calls (plate guard, already-visited, zero-sample) must leave `state` completely
// untouched by simply never calling this, exactly today's (pre-v2.3) behavior; that
// case is not modeled here at all, it's the caller skipping the call entirely.
// `committed` is this layer's apply_bucket_caps result (BucketCapResult::kept, possibly
// empty). `had_buckets_pre_gate` is whether ANY bucket existed in the per-layer
// partitioned map BEFORE apply_bucket_caps ran (the caller must capture this itself,
// before the call, since apply_bucket_caps erases gated/trimmed buckets from that same
// map in place). Three outcomes:
//   a. `committed` non-empty -> that becomes the new prev_kept outright, and the
//      retention grace resets (a real commit is not a "gated away" event, so a LATER
//      unlucky layer gets its own fresh one-layer grace).
//   b. `committed` empty, but buckets existed pre-gate (real matches were found, then
//      every one of them was gated/trimmed away by apply_bucket_caps' caps), AND the
//      grace hasn't already been spent on the immediately preceding layer: retain the
//      OLD prev_kept for exactly one more layer (spend the grace now) - stops a single
//      unlucky all-gated layer from erasing column memory outright (spec root cause 2),
//      while still decaying rather than holding on indefinitely.
//   c. Otherwise (grace already spent last layer on a second consecutive all-gated
//      layer, OR no buckets existed pre-gate at all - the uniform-fallback fast path,
//      every sample voted fallback with nothing to gate) - clear to empty, same as
//      today's unconditional overwrite.
PrevKeptState chameleon_update_prev_kept(const PrevKeptState& state,
                                         const std::set<unsigned>& committed,
                                         bool had_buckets_pre_gate);

// v2.3 Task 1 (spec C1-C2): signature grows two trailing, DEFAULTED parameters -
// `free_extruders`/`min_len_free_mm` - so every pre-v2.3 call site (this function's own
// existing unit tests included) compiles and behaves byte-identically unchanged: an
// empty `free_extruders` (the default) means no bucket is ever "free", so step (a)
// below always falls through to `min_len_mm` for every bucket, exactly as before this
// task. `min_len_mm` is now the NORMAL-tier floor (spec C1: 12mm at the call site,
// replacing the old flat 40mm) and `min_len_free_mm` is the FREE-tier floor (3mm at the
// call site) for a bucket whose extruder is in `free_extruders` - the once-per-pass z-
// table lookup (chameleon_layer_free_extruders) the caller resolves per support layer
// before calling this. Independently (spec C2), a bucket whose extruder is in
// `prev_kept` (this function's EXISTING argument, unchanged) passes the gate at HALF
// its tier's floor (0.5x eff_min, where eff_min is whichever of min_len_mm/
// min_len_free_mm the free-set membership above selected) - hysteresis stacks with,
// never replaces, the free-tier selection.
//
// v2.5b (spec: "free-extruder trim exemption"): signature grows ONE more trailing,
// DEFAULTED parameter - `free_extruders_exempt` - so every pre-v2.5b call site (this
// function's own existing unit tests included) compiles and behaves byte-identically
// unchanged: an empty `free_extruders_exempt` (the default) exempts nothing, so step
// (b) always ranks/trims every surviving bucket exactly as before this task. When
// non-empty, see step (b)'s own bullet above for the exemption algorithm.
//
// This is DELIBERATELY a SEPARATE, NARROWER set from `free_extruders` above, not the
// same set reused - the two gates price different things. `free_extruders` (the min-
// benefit gate's tier selector) may use the WINDOWED query (chameleon_layer_free_
// extruders' up_mm = kContactBandMm) - a bucket whose wall exists only in the contact
// band ABOVE this exact z is still worth a reduced 3mm floor there, since the vote
// itself samples that same band. `free_extruders_exempt` MUST use the STRICT-
// coincidence query instead (up_mm = 0.0; down_mm = the support layer's own height is
// still correct - that term only corrects for the layer's own z-thickness / unsynced
// object-layer grid, it doesn't reach into the future) - because the claim this
// exemption rests on is "keeping this bucket costs no NEW toolchange", and that is
// only true when the extruder is ALREADY going to be registered in THIS support
// layer's own LayerTools from model geometry. Per-support-layer registration
// (ToolOrdering.cpp's collect_extruders, ~734-736: `for (const auto& kv :
// support_layer->interface_by_extruder) ... layer_tools.extruders.push_back(kv.first +
// 1)`) runs unconditionally for every kept bucket; that push is a genuine no-op only
// because sort_remove_duplicates/remove_duplicates_preserve_order (~870-874) later
// collapses it against an entry model geometry ALREADY placed in that same layer's
// `extruders` vector at THIS print_z (collect_extruders is called once per object,
// same shared LayerTools keyed by z, so any object's wall/solid/sparse-infill
// extruder at this z lands there too). An extruder that is "free" only via the up-
// window prints on a HIGHER object layer that has not contributed to THIS print_z's
// LayerTools at all - registering its bucket here would push a genuinely NEW entry
// that survives the dedup, i.e. a real extra toolchange, not a free one. Exempting it
// from the trim on the strength of the windowed set would therefore be wrong; the
// gate's 3mm floor tier can still afford to be generous there (worst case: a short
// bucket kept an extra layer or two), but the trim's exemption cannot.
//
// CHAMELEON_DEBUG (v2.5c): signature grows one more trailing, DEFAULTED parameter -
// `debug_out` - so every pre-v2.5c call site (this function's own existing unit tests
// included) compiles and behaves byte-identically unchanged: nullptr (the default)
// means every `if (debug_out)` check below this function's own doc comment in the .cpp
// short-circuits, so passing nothing costs one pointer compare per decision point, not
// per sample - see ChameleonBucketDebugEntry's own doc comment above for what gets
// recorded when a caller does pass a non-null vector.
BucketCapResult apply_bucket_caps(std::map<unsigned, ExtrusionEntityCollection>& map,
                                  const std::set<unsigned>& prev_kept,
                                  size_t max_extruders,
                                  double min_len_mm,
                                  ExtrusionEntityCollection& merge_back_target,
                                  const std::set<unsigned>& free_extruders = {},
                                  double min_len_free_mm = 0.0,
                                  const std::set<unsigned>& free_extruders_exempt = {},
                                  std::vector<ChameleonBucketDebugEntry>* debug_out = nullptr);

// v2.5a Task 2b (spec: "mechanism B pin"): the extruder of whichever bucket in
// `buckets` has the largest total_path_length_mm ("dominant"); ties (an exact equal
// length) go to the LOWEST extruder id - `buckets` is key-ordered ascending and only
// a STRICTLY greater length replaces the current pick, so this falls out of the scan
// itself rather than needing a separate tie-break step. A bucket with empty entities
// is never a candidate (mirrors every other bucket-length comparison in this file -
// total_path_length_mm(empty) is 0.0, which a genuinely non-empty bucket of any
// positive length would already beat, but an ALL-empty `buckets` must still resolve
// to "no candidate", not extruder 0). Returns -1 when `buckets` is empty or every
// bucket in it is empty - GCode.cpp's own residual "don't care" pin (~5379+) treats
// that as "nothing to pin to" and falls through to its pre-v2.5a first/active-
// extruder rule unchanged. Pulled out as its own pure function (rather than left
// inline at the one GCode.cpp call site) specifically so this decision is unit-
// testable without a Print/PrintObject/GCode scaffold - the call site itself
// (a support layer's own SupportLayer::interface_by_extruder) is not otherwise
// exercisable outside a full GUI-class multi-filament slice.
int chameleon_dominant_matched_extruder(const std::map<unsigned, ExtrusionEntityCollection>& buckets);

// v2.2 Task 2 (spec C4, root cause 1): gap-aware lateral cap arithmetic, pure so it's
// unit-testable without a full PrintObject/Print scaffold (the three mm inputs need
// PrintObject/PrintRegion/PrintConfig access to resolve - see Print.cpp's
// chameleon_assign_support_interfaces for that glue, which mirrors the support
// generator's own width resolution: Support/SupportParameters.hpp:70-76 for the outer
// wall width, :165-168 for the support line width's 0=auto fallback - then calls this).
// The cap is CENTERLINE-to-centerline (both wall samples and support line paths are
// centerlines), so the physical minimum distance it must admit between a wall and an
// adjacent support skin is the surface-to-surface gap the generator holds
// (support_object_xy_distance_mm) plus half the outer wall's own width (its centerline
// sits that far inside the wall's true outer surface) plus half the support line's own
// width (same, on the support side), plus a fixed 0.35mm slack for centerline
// sampling/voronoi-snap error (spec C4). Replaces the old flat 1.0mm max_dist_mm, which
// was frequently LESS than gap_xy + half-widths alone at common profiles - "nearly
// unsatisfiable" (spec root cause 1).
double gap_aware_lateral_cap_mm(double support_object_xy_distance_mm,
                                double outer_wall_width_mm,
                                double support_line_width_mm);

// v2.2 Task 3 (spec C6, third anchor): shared predicate behind the two independently
// duplicated "has_interface" role classifications that decide whether a support
// layer's residual (unmatched/fallback) support_fills still needs the interface
// extruder registered/bucketed for it - ToolOrdering.cpp's per-support-layer
// extruder-collection pass (~701-703) and GCode.cpp's own support-bucket mirror of
// that same classification (~5339-5341, which picks the ObjectByExtruder key a
// layer's fallback support_fills gets bucketed under). Both call sites collapse
// support_fills.role() (single ExtrusionRole - erNone/uniform/erMixed) and, pre-v2.2,
// treated only erMixed or erSupportMaterialInterface as "needs the interface
// extruder". Investigation finding behind this addition: once C6's third
// partition_support_entities call (role_filter = erIroning) and C7's whole-collection
// moves can remove EVERY erSupportMaterial/erSupportMaterialInterface entity from a
// layer's support_fills while leaving some erIroning behind on the fallback path,
// support_fills.role() collapses to a PURE erIroning - which neither predicate above
// recognized, so the interface extruder never got registered/bucketed for that layer
// and the residual ironing silently never printed. Extracted here (rather than left
// duplicated inline) so the two call sites cannot drift out of sync with each other,
// and so this classification is unit-testable without a full Print/ToolOrdering
// scaffold.
bool support_role_needs_interface_extruder(ExtrusionRole role);
} // namespace Slic3r
#endif
