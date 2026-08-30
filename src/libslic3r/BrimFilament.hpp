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
// select_layers_in_band/select_contact_layers return). Finds the first (lowest) layer
// that covers `p` (bbox-gated when the relevant bboxes are usable) - EITHER its raw
// lslices, OR (v2.2 Task 2, spec C5) its expanded_lslices, the margin ring around them.
// A raw hit is unchanged from v2.1: within it, the region whose slice polys contain p,
// preferring one whose bottom polys ALSO contain p when more than one region's slices
// contain p (first-contains-p wins otherwise, ties broken by ascending region index).
//
// v2.2 Task 2 (spec C5): when no region's raw slice polys contain p - either because the
// hit was only via expanded_lslices (a genuine margin-ring sample), or the pre-existing
// degenerate case (a layer whose lslices cover p but whose per-region data doesn't,
// previously treated as a miss for the whole layer) - the region whose raw slice polys
// are NEAREST to p is picked instead, over ALL of this layer's regions (not just the
// island that produced the hit). No separate distance threshold is enforced at this
// step: the layer-level hit test above already established p is within the margin of
// SOME geometry on this layer, so "nearest region" is simply "which region does that
// nearby geometry belong to". Two-stage search, per region: (1) a cheap bbox-distance
// lower bound (point-to-AABB, via get_extents on each of the region's raw polys) prunes
// any region whose lower bound already can't beat the current best, without touching its
// polygon; (2) only surviving regions get the exact-ish "distance to polygon" test
// Slic3r already uses elsewhere (MultiPoint::distance_to - nearest-VERTEX distance over
// contour + holes, not true nearest-edge distance; a documented, precedented
// simplification, not a new one). This is a correct branch-and-bound nearest search (the
// bbox distance is a real lower bound), not an approximation of "nearest" itself.
// Determinism: regions are scanned in ascending index order and only a STRICTLY smaller
// exact distance replaces the current best, so an exact tie is won by the lower region
// index, deterministically, regardless of visitation-order edge cases. A region that
// contributes no raw slice polys at all is never a candidate.
//
// Returns false (out_layer/out_region left unwritten) when no band layer's raw or
// expanded lslices cover p, or when a covering layer offers no region with any raw slice
// geometry at all.
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
// - Entities whose role() != role_filter (and any nested collection) are never
//   touched. Matched originals are deleted.
// Returns switch-boundary count added (for the per-object cap accounting).
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
} // namespace Slic3r
#endif
