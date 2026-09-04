#include "FillTextureMapping.hpp"

#include "Layer.hpp"
#include "Print.hpp"
#include "TextureMapping.hpp"

#include <algorithm>
#include <array>

namespace Slic3r {

std::shared_ptr<TopSurfaceImageContoningStackPlanCache> make_top_surface_image_contoning_stack_plan_cache()
{
    return std::make_shared<TopSurfaceImageContoningStackPlanCache>();
}

void Layer::prebuild_contoning_stack_plan_cache(std::function<void()> throw_if_canceled,
                                                TopSurfaceImageContoningStackPlanCache *contoning_stack_plan_cache) const
{
    if (contoning_stack_plan_cache == nullptr || this->object() == nullptr || this->object()->print() == nullptr)
        return;

    const Print &print = *this->object()->print();
    const TextureMappingManager &mgr = print.texture_mapping_manager();
    if (mgr.zones().empty())
        return;

    const size_t num_physical = print.config().filament_colour.size();
    for (const LayerRegion *layerm : this->regions()) {
        if (throw_if_canceled)
            throw_if_canceled();
        if (layerm == nullptr)
            continue;
        const unsigned int filament_id = unsigned(std::max(0, layerm->region().config().solid_infill_filament.value));
        if (!mgr.is_texture_mapping_zone_id(filament_id))
            continue;
        const TextureMappingZone *zone = mgr.zone_from_id(filament_id);
        if (zone == nullptr || !zone->top_surface_contoning_active())
            continue;

        TopSurfaceImageContoningStackPlanCache::Key key;
        key.zone_id = filament_id;
        key.layer_id = int(this->id());
        key.stack_depth = std::clamp(zone->top_surface_contoning_stack_layers,
                                     TextureMappingZone::MinTopSurfaceContoningStackLayers,
                                     TextureMappingZone::MaxTopSurfaceContoningStackLayers);

        {
            std::lock_guard<std::mutex> lock(contoning_stack_plan_cache->mutex);
            if (contoning_stack_plan_cache->plans.find(key) != contoning_stack_plan_cache->plans.end())
                continue;
        }

        const std::vector<unsigned int> component_ids =
            TextureMappingManager::effective_texture_component_ids(*zone, num_physical, print.config().filament_colour.values);
        TextureMappingContoningSolver solver(*zone, print.config(), component_ids, float(this->height));
        TopSurfaceImageContoningStackPlanCache::Entry entry;
        entry.zone_id = filament_id;
        entry.stack_layers = key.stack_depth;
        if (solver.valid())
            entry.sample_stack = solver.solve({0.5f, 0.5f, 0.5f}, std::max(1, key.stack_depth));

        std::lock_guard<std::mutex> lock(contoning_stack_plan_cache->mutex);
        contoning_stack_plan_cache->plans.emplace(key, std::move(entry));
    }
}

} // namespace Slic3r
