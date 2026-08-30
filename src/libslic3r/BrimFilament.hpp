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
};

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
// lowest first, this time testing expanded_lslices (the margin ring) instead of raw
// lslices. The first layer whose ring covers p is picked, resolved via the nearest-
// region search (below) over ALL of that layer's regions. Reaching PASS 2 at all already
// means no band layer anywhere raw-contains p, so there is no raw-containment region
// scan to redo here - a region's own raw slice polys are always a subset of its layer's
// raw lslices, which PASS 1 has already established as a global miss.
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
// Returns switch-boundary count added (for the per-object cap accounting) - a moved
// whole collection does not contribute to this count (it isn't split into runs).
size_t partition_support_entities(ExtrusionEntityCollection& support_fills,
                                  ExtrusionRole role_filter,
                                  unsigned fallback_extruder,
                                  const std::function<unsigned(const Point&)>& resolver,
                                  const BrimVoteParams& p,
                                  std::map<unsigned, ExtrusionEntityCollection>& out);

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
BucketCapResult apply_bucket_caps(std::map<unsigned, ExtrusionEntityCollection>& map,
                                  const std::set<unsigned>& prev_kept,
                                  size_t max_extruders,
                                  double min_len_mm,
                                  ExtrusionEntityCollection& merge_back_target);

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
