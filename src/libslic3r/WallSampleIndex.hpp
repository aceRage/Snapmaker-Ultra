#ifndef slic3r_WallSampleIndex_hpp_
#define slic3r_WallSampleIndex_hpp_
#include "Point.hpp"
#include <cstdint>
#include <map>
#include <vector>
namespace Slic3r {

struct WallSample { Point pt; unsigned extruder; size_t object_key; };

class WallSampleIndex {
public:
    // cell_mm: grid cell size (default 2.0). Samples added via add_polyline.
    explicit WallSampleIndex(double cell_mm = 2.0);
    // Sample the polyline every spacing_mm (default 0.8) and insert points.
    // CHAMELEON_DEBUG (v2.5c diagnostic instrumentation): `sample_count_out`, when
    // non-null, gets this call's own inserted-sample count added under `extruder`'s key
    // (every sample this one call inserts shares the same extruder - the caller's own
    // per-call parameter - so one running total per call is all this needs, no per-
    // sample bucketing). Default nullptr: every pre-v2.5c call site (this class's own
    // existing unit tests included) is unaffected - the only added cost when null is one
    // pointer compare per inserted sample, alongside work (a map insert + a distance
    // computation) already far more expensive than that check.
    void add_polyline(const Points& poly, unsigned extruder, size_t object_key,
                      double spacing_mm = 0.8,
                      std::map<unsigned, size_t>* sample_count_out = nullptr);
    // k nearest samples to pt (by expanding ring search over grid cells).
    // Returns up to k samples sorted by squared distance ascending, ties by
    // (extruder, object_key) for determinism.
    std::vector<std::pair<const WallSample*, double>> knn(const Point& pt, size_t k) const;
    size_t size() const;
    bool   empty() const;
private:
    double m_cell; // scaled units
    std::map<std::pair<int32_t,int32_t>, std::vector<WallSample>> m_cells;
    size_t m_count = 0;
};
} // namespace Slic3r
#endif
