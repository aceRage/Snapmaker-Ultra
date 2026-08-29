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
    void add_polyline(const Points& poly, unsigned extruder, size_t object_key,
                      double spacing_mm = 0.8);
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
