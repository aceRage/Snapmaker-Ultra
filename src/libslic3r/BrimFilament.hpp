#ifndef slic3r_BrimFilament_hpp_
#define slic3r_BrimFilament_hpp_
#include "WallSampleIndex.hpp"
#include "ExtrusionEntity.hpp"
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
} // namespace Slic3r
#endif
