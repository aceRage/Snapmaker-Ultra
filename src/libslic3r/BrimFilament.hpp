#ifndef slic3r_BrimFilament_hpp_
#define slic3r_BrimFilament_hpp_
#include "WallSampleIndex.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ExPolygon.hpp"
#include "BoundingBox.hpp"
#include <functional>
#include <map>
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

// v2.1 Task 2 (projection resolver): pure geometric core of Print.cpp's
// chameleon_projection_extruder. Layer/LayerRegion can't be built standalone outside a
// full slice (private/protected ctors, PrintObject-owned storage), so the point-in-band-
// layer / region-preference selection is factored out here as a plain-data view over
// ExPolygon POINTERS (no ownership, no copying - a LayerRegion's true storage is
// SurfaceCollection, not a bare ExPolygons, so the caller collects `&surface.expolygon`
// pointers per call; this stays cheap even though the helper below is invoked once per
// 0.8mm sample point).
struct ProjectionLayerView {
    // This band layer's lslices (required - a null/empty pointer is a miss) and, in
    // parallel (index i matches lslices[i]), each island's precomputed bbox for a cheap
    // AABB gate before the exact point-in-polygon test. lslices_bboxes may be left
    // null or size-mismatched to skip the gate (falls straight through to the exact
    // test) - Layer::lslices_bboxes is populated at slicing time (PrintObjectSlice.cpp),
    // well before this pass runs, so the common case is a free (already-computed) gate.
    const ExPolygons*              lslices = nullptr;
    const std::vector<BoundingBox>* lslices_bboxes = nullptr;
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
// whose lslices cover `p` (bbox-gated when lslices_bboxes is usable); within it, the
// region whose slice polys contain p, preferring one whose bottom polys ALSO contain p
// when more than one region's slices contain p (first-contains-p wins otherwise, ties
// broken by ascending region index). If a layer's lslices cover p but no region's own
// slice polys do (degenerate/inconsistent data), that layer is skipped, not treated as
// a hit - the scan continues to the next band layer. Returns false (out_layer/out_region
// left unwritten) when no band layer's lslices cover p.
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
} // namespace Slic3r
#endif
