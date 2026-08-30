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
    // object_key -> layer-0 area (for tie-break 1); larger area wins
    std::map<size_t, double> object_area;
    unsigned fallback_extruder = 0;     // used when index empty / no candidates
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

// Indices of object layers whose TOP z lies in (lo_z, hi_z].
// print_zs = ascending layer TOP z values; layer i spans (print_zs[i-1], print_zs[i]].
std::vector<size_t> select_layers_in_band(const std::vector<double>& print_zs,
                                          double lo_z, double hi_z);

// Indices of object layers whose TOP z lies in (support_top_z, support_top_z + gap_mm].
// Thin call onto select_layers_in_band(print_zs, support_top_z, support_top_z + gap_mm).
// Requires gap_mm > max layer height, else the direct contact layer itself is dropped.
std::vector<size_t> select_contact_layers(const std::vector<double>& print_zs,
                                          double support_top_z, double gap_mm = 2.0);

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

// Partition the `role_filter`-role entities of `support_fills`: `resolver` is
// called per sample point (in place of a fixed WallSampleIndex knn vote) via
// split_polyline_by_resolver.
// - Entities whose every vote == fallback stay in support_fills untouched (fast path).
// - Otherwise the entity is split; runs voted fallback are appended back into
//   support_fills as new paths (role copied from first_path_of(entity)->role(),
//   falling back to entity->role() - so a base-role split stays erSupportMaterial,
//   never hardcoded to erSupportMaterialInterface); other runs go to out[extruder].
// - Entities whose collapsed role() != role_filter are never touched (this includes
//   a nested collection whose own contents mix roles, or match some OTHER role
//   entirely - "collapsed" per ExtrusionEntityCollection::role(), which is already
//   fully recursive: erNone for empty, the single common role for a uniform
//   collection at any nesting depth, erMixed otherwise).
// - v2.2 Task 3 (spec C7, "nested collections"): a nested ExtrusionEntityCollection
//   whose collapsed role() DOES equal role_filter (tree double-wall branch
//   collections, layer-0 no_sort sheath collections, etc. - previously invisible to
//   this pass entirely, spec root cause 6) is voted as ONE UNIT, never split apart:
//   every leaf polyline reachable via collection.flatten() is sampled at the same
//   0.8mm-default cadence split_polyline_core's build_chain uses (reused directly),
//   `resolver` is called once per sample across the WHOLE collection, and the
//   majority vote decides the outcome (ties broken to the LOWEST extruder id,
//   deterministic - see vote_collection_as_unit's own comment in the .cpp). A
//   non-fallback majority MOVES the original collection pointer whole into
//   out[extruder] (no clone, no per-child split); a fallback majority (or an empty/
//   sample-less collection) leaves it in support_fills untouched, exactly like the
//   leaf fast path above - this is also what preserves a moved-or-left collection's
//   own `no_sort` flag, since the collection object itself is never touched, only
//   relocated as a whole. Matched originals (leaf OR whole collection) are deleted
//   from support_fills's own top-level entities vector, never from inside a
//   collection that stays behind.
// - v2.3 Task 3 (spec C5, root cause 5, "tree selective descent"): the whole-collection
//   vote above (vote_collection_as_unit) now ALSO reports the raw per-extruder sample
//   histogram and the longest contiguous run (mm) of samples that voted some extruder
//   OTHER than the winner ("the minority's contiguous run"). A collection whose vote is
//   UNIFORM (histogram has only one distinct extruder) or whose minority run is SHORTER
//   than the descend threshold behaves EXACTLY as the C7 paragraph above describes -
//   whole-move or stay, pointer-stable, no change. A collection whose minority run
//   reaches the threshold DESCENDS instead: every LEAF entity reachable inside it
//   (recursing into any further-nested sub-collection the same way) is individually
//   partitioned via the SAME per-leaf logic the top-level leaf case above already uses -
//   a leaf whose own vote is uniformly fallback is left in place untouched; a leaf whose
//   own vote is uniformly one non-fallback extruder moves whole into out[extruder]
//   (rebuilt as a new ExtrusionPath, per-leaf flow attrs from first_path_of(leaf) -
//   ExtrusionLoop/MultiPath-ness is not preserved, same pre-existing tradeoff the
//   top-level leaf case already has); a leaf with a genuinely mixed vote is split into
//   runs, with non-fallback runs going to out[extruder] and fallback runs SPLICED BACK
//   IN PLACE at that leaf's own index inside its IMMEDIATE parent collection (never
//   appended to the end - `no_sort` order is preserved exactly, unlike the top-level
//   leaf case's own new_fallback_paths, which DOES append to the end of support_fills
//   since ordering there doesn't carry the same meaning). Every rebuilt ExtrusionPath
//   (in either case above) inherits the SOURCE LEAF's own can_reverse() - see
//   ExtrusionEntity::can_reverse()/ExtrusionPath::set_reverse() (ExtrusionEntity.hpp) -
//   a branch-wall leaf built with reversal disabled (Support/SupportCommon.cpp:660-663/
//   765-767, "always start with the anchor") must keep that property on its split
//   pieces, or GCode's chain_and_reorder is free to flip a piece's direction and break
//   the seam anchor (spec: "seam-anchor blob hazard"). A (possibly nested) sub-
//   collection left fully empty by this recursion is itself removed from its parent and
//   deleted - never left behind as a dangling empty shell - the SAME rule the top-level
//   collection pointer follows: it stays in support_fills, still the same object,
//   unless the descend emptied it entirely, in which case it is deleted exactly once.
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
// whole collection does not contribute to this count (it isn't split into runs); a
// DESCENDED collection contributes the sum of its own per-leaf switch-boundary counts
// (mirrors the top-level leaf case's own accounting, just summed across every leaf the
// descend touched).
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
// comment) - a bucket built by partition_support_entities is a flat vector of
// ExtrusionPath* today (partition_support_leaf_entity only ever emplaces
// ExtrusionPath), but this recurses into any nested collection too (is_collection())
// so it stays correct if a future nested-collection vote (spec C7) ever lands a whole
// ExtrusionEntityCollection* in a bucket. Each leaf's own length() is in SCALED units
// (see Line::length()/MultiPoint::length()) - unscale<double> converts to mm, the same
// way GCode.cpp/ExtrusionEntity.cpp do at every other length() call site.
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
    // Buckets merged back by the C3 min-benefit gate (total length < min_len_mm).
    size_t buckets_dropped_min_benefit = 0;
    // Buckets merged back by the C1 distinct-extruder trim (survived the gate, but
    // ranked below the top max_extruders by (prev_kept membership, then length)).
    size_t buckets_trimmed_cap = 0;
};

// v2.2 Task 1 (spec C1-C3, replaces the old per-layer ">3 switch-boundaries" whole-
// layer revert and the per-object ">20 cumulative switches" escalation - both deleted
// entirely, C2). `map` holds one support layer's matched geometry, keyed by extruder,
// straight out of one or more partition_support_entities calls sharing the same out
// map. Applied in order:
//   a. C3 min-benefit gate: any bucket whose total_path_length_mm() < min_len_mm is
//      merged into `merge_back_target` and erased from `map` - a matched sliver isn't
//      worth a toolchange/purge, and must never survive the trim below by being one of
//      the "top N longest" among other slivers.
//   b. C1 distinct-extruder trim: if more than `max_extruders` buckets survive the
//      gate, rank them by (extruder present in `prev_kept` DESC, total path length
//      DESC, extruder id ASC as the final deterministic tie-break) and merge every
//      bucket past the top `max_extruders` into `merge_back_target`. This measures the
//      TRUE per-layer cost - the number of DISTINCT matched extruders (support fills
//      emit one path-group per extruder; run-boundary/switch counts are a red herring,
//      support fills are one ExtrusionPath per LINE so boundary-crossing runs trip
//      that counter constantly without costing an extra toolchange). Hysteresis:
//      prev_kept membership outranks length outright (a short bucket this object kept
//      last layer beats a longer bucket that's new this layer) - stability up the
//      column is what stops the alternating-stripe artifact a whole-layer revert
//      caused; length only breaks ties among buckets with the same prev_kept status.
// The merge-back itself is the same ownership-transferring append(ExtrusionEntitiesPtr
// &&) the old whole-layer revert used (role-preserving - every path already carries
// its true source role from partition_support_entities' role-copy, never hardcoded),
// just applied per-bucket instead of per-layer, so a partial-degradation layer keeps
// its winning bucket(s) matched instead of falling back to fallback entirely. `map` is
// left holding only the committed buckets - the caller moves it straight into
// SupportLayer::interface_by_extruder.
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
// within EPSILON of `query_z` (a support layer's own print_z) - COINCIDENCE, not
// nearest-neighbor: a query z with no matching entry (no object anywhere on the plate
// has a layer at that height) returns an empty set - "not free" - rather than falling
// back to the closest entry, since a support layer between two object layers isn't
// actually sharing a toolchange with either one. `table` must already be ascending by
// z (build_layer_filament_table's own contract) - this does a binary search (multiple
// adjacent entries can each fall within EPSILON of query_z at a merge boundary, so the
// search widens outward from the found index in both directions rather than trusting a
// single lower_bound hit).
std::set<unsigned> chameleon_layer_free_extruders(const LayerFilamentTable& table, double query_z);

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
BucketCapResult apply_bucket_caps(std::map<unsigned, ExtrusionEntityCollection>& map,
                                  const std::set<unsigned>& prev_kept,
                                  size_t max_extruders,
                                  double min_len_mm,
                                  ExtrusionEntityCollection& merge_back_target,
                                  const std::set<unsigned>& free_extruders = {},
                                  double min_len_free_mm = 0.0);

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
