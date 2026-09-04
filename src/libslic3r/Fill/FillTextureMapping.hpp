#ifndef slic3r_FillTextureMapping_hpp_
#define slic3r_FillTextureMapping_hpp_

#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>

#include "libslic3r.h"
#include "TextureMappingContoning.hpp"

namespace Slic3r {

// Schedule / cache handle for top-surface Contoning stack plans.
// Geometry-heavy ImageMap Fill Contoning is not boiled into Ultra Fill.cpp.
// The PR1 TextureMappingContoningSolver remains the callable solver.
struct TopSurfaceImageContoningStackPlanCache
{
    struct Key
    {
        unsigned int zone_id = 0;
        int          layer_id = -1;
        int          stack_depth = -1;

        bool operator==(const Key &rhs) const
        {
            return zone_id == rhs.zone_id && layer_id == rhs.layer_id && stack_depth == rhs.stack_depth;
        }
    };

    struct KeyHash
    {
        size_t operator()(const Key &key) const
        {
            return (size_t(key.zone_id) * 1315423911u) ^ (size_t(key.layer_id) * 2654435761u) ^ size_t(key.stack_depth);
        }
    };

    struct Entry
    {
        unsigned int zone_id = 0;
        int          stack_layers = 0;
        TextureMappingContoningStack sample_stack;
    };

    mutable std::mutex mutex;
    std::unordered_map<Key, Entry, KeyHash> plans;
};

std::shared_ptr<TopSurfaceImageContoningStackPlanCache> make_top_surface_image_contoning_stack_plan_cache();

} // namespace Slic3r

#endif
