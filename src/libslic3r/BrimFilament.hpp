#ifndef slic3r_BrimFilament_hpp_
#define slic3r_BrimFilament_hpp_
#include "WallSampleIndex.hpp"
#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include <map>
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

// Indices of object layers whose TOP z lies in (support_top_z, support_top_z + gap_mm].
// print_zs = ascending layer TOP z values; layer i spans (print_zs[i-1], print_zs[i]].
// Requires gap_mm > max layer height, else the direct contact layer itself is dropped.
std::vector<size_t> select_contact_layers(const std::vector<double>& print_zs,
                                          double support_top_z, double gap_mm = 2.0);

// Partition the interface-role entities of `support_fills` by vote against `idx`.
// - Entities whose every vote == fallback stay in support_fills untouched (fast path).
// - Otherwise the entity is split; runs voted fallback are appended back into
//   support_fills as new interface paths; other runs go to out[extruder].
// - Non-interface entities are never touched. Matched originals are deleted.
// Returns switch-boundary count added (for the per-object cap accounting).
size_t partition_support_interfaces(ExtrusionEntityCollection& support_fills,
                                    unsigned fallback_extruder,
                                    const WallSampleIndex& idx,
                                    const BrimVoteParams& params,
                                    std::map<unsigned, ExtrusionEntityCollection>& out);
} // namespace Slic3r
#endif
