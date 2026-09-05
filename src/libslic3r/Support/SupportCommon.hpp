#ifndef slic3r_SupportCommon_hpp_
#define slic3r_SupportCommon_hpp_

#include "../Layer.hpp"
#include "../Polygon.hpp"
#include "../Print.hpp"
#include "SupportLayer.hpp"
#include "SupportParameters.hpp"

namespace Slic3r {

class PrintObject;
class SupportLayer;

// Remove bridges from support contact areas.
// To be called if PrintObjectConfig::dont_support_bridges.
void remove_bridges_from_contacts(
    const PrintConfig   &print_config, 
    const Layer         &lower_layer,
    const LayerRegion   &layerm,
    float                fw, 
    Polygons            &contact_polygons);

// Turn some of the base layers into base interface layers.
// For soluble interfaces with non-soluble bases, print maximum two first interface layers with the base
// extruder to improve adhesion of the soluble filament to the base.
// For Organic supports, merge top_interface_layers & top_base_interface_layers with the interfaces
// produced by this function.
std::pair<SupportGeneratorLayersPtr, SupportGeneratorLayersPtr> generate_interface_layers(
    const PrintObjectConfig           &config,
    const SupportParameters           &support_params,
    const SupportGeneratorLayersPtr   &bottom_contacts,
    const SupportGeneratorLayersPtr   &top_contacts,
    // Input / output, will be merged with output
    SupportGeneratorLayersPtr         &top_interface_layers,
    SupportGeneratorLayersPtr         &top_base_interface_layers,
    // Input, will be trimmed with the newly created interface layers.
    SupportGeneratorLayersPtr         &intermediate_layers,
    SupportGeneratorLayerStorage      &layer_storage);

// Generate raft layers, also expand the 1st support layer
// in case there is no raft layer to improve support adhesion.
SupportGeneratorLayersPtr generate_raft_base(
	const PrintObject				&object,
	const SupportParameters			&support_params,
	const SlicingParameters			&slicing_params,
	const SupportGeneratorLayersPtr &top_contacts,
	const SupportGeneratorLayersPtr &interface_layers,
	const SupportGeneratorLayersPtr &base_interface_layers,
	const SupportGeneratorLayersPtr &base_layers,
	SupportGeneratorLayerStorage    &layer_storage);

void tree_supports_generate_paths(ExtrusionEntitiesPtr &dst, const Polygons &polygons, const Flow &flow, const SupportParameters &support_params);

void fill_expolygons_with_sheath_generate_paths(
    ExtrusionEntitiesPtr &dst, const Polygons &polygons, Fill *filler, float density, ExtrusionRole role, const Flow &flow, const SupportParameters& support_params, bool with_sheath, bool no_sort);

// returns sorted layers
SupportGeneratorLayersPtr generate_support_layers(
	PrintObject							&object,
    const SupportGeneratorLayersPtr     &raft_layers,
    const SupportGeneratorLayersPtr     &bottom_contacts,
    const SupportGeneratorLayersPtr     &top_contacts,
    const SupportGeneratorLayersPtr     &intermediate_layers,
    const SupportGeneratorLayersPtr     &interface_layers,
    const SupportGeneratorLayersPtr     &base_interface_layers);

// Ultra (support groups, plan 2026-09-02 Stage 3 3.6): per-group interface parameters plus the
// per-object-layer claim that decides which piece of a SHARED contact layer belongs to the group.
// Passing nullptr for `groups` - the default - is exactly today's single-config behaviour, so
// every call site that does not pass one is unchanged.
struct SupportGroupToolpaths {
    // The group's resolved PrintObjectConfig: the object's, with this group's part-level support
    // overrides applied (PrintObject::support_groups()).
    const PrintObjectConfig     *config { nullptr };
    // SupportParameters built from that config.
    const SupportParameters     *params { nullptr };
    // Per OBJECT layer. For groups >= 1 this is the group's footprint, expanded by the interface
    // margin and cut against every lower group, and a shared polygon is INTERSECTED with it. For
    // group 0 it is the union of all the others, and a shared polygon is what is LEFT after
    // subtracting it - so the K pieces partition the polygon exactly, with no overlap and no gap.
    const std::vector<Polygons> *claim  { nullptr };
    // 1-based support interface filament slot; 0 = "same as the object". When it is non-zero and
    // differs from the object's, this group's interface goes into SupportLayer::interface_by_extruder
    // rather than support_fills, which is what schedules the tool change (ToolOrdering.cpp).
    int                          interface_filament { 0 };
};

// Ultra (support groups): the object layer a SHARED contact layer sits against. The contact
// generators do set idx_object_layer_above / _below, but merge_contact_layers keeps only the
// surviving layer's index, so fall back to the nearest object layer print_z. Returns size_t(-1)
// only when there are no object layers at all.
size_t support_group_object_layer_index(const SupportGeneratorLayer &layer, bool use_above,
                                        const std::vector<coordf_t> &object_layer_zs);

// Ultra (support groups): cut one polygon set down to the piece group `g` owns. Groups >= 1 take
// the intersection with their own claim; group 0 takes the remainder - everything the others did
// not claim - so the K pieces partition `src` exactly, with no overlap and no gap. A missing claim
// gives group 0 everything, which is the conservative direction: geometry is never dropped.
Polygons support_group_piece(const Polygons &src, const std::vector<Polygons> *claim,
                             size_t idx_object_layer, size_t g);

// Produce the support G-code.
// Used by both classic and tree supports.
void generate_support_toolpaths(
	SupportLayerPtrs    				&support_layers,
	const PrintObjectConfig 			&config,
	const SupportParameters 			&support_params,
	const SlicingParameters 			&slicing_params,
    const SupportGeneratorLayersPtr 	&raft_layers,
    const SupportGeneratorLayersPtr   	&bottom_contacts,
    const SupportGeneratorLayersPtr   	&top_contacts,
    const SupportGeneratorLayersPtr   	&intermediate_layers,
	const SupportGeneratorLayersPtr   	&interface_layers,
    const SupportGeneratorLayersPtr   	&base_interface_layers,
    // Ultra (support groups): nullptr / empty / a single entry all mean "one config", i.e. exactly
    // the code this function has always run.
    const std::vector<SupportGroupToolpaths> *groups = nullptr,
    const std::vector<coordf_t>              *object_layer_zs = nullptr);

// FN_HIGHER_EQUAL: the provided object pointer has a Z value >= of an internal threshold.
// Find the first item with Z value >= of an internal threshold of fn_higher_equal.
// If no vec item with Z value >= of an internal threshold of fn_higher_equal is found, return vec.size()
// If the initial idx is size_t(-1), then use binary search.
// Otherwise search linearly upwards.
template<typename IteratorType, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(IteratorType begin, IteratorType end, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = 0;
    } else if (idx == IndexType(-1)) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_higher_equal(begin[idx_mid]))
                idx_high = idx_mid;
            else
                idx_low  = idx_mid;
        }
        idx =  fn_higher_equal(begin[idx_low])  ? idx_low  :
              (fn_higher_equal(begin[idx_high]) ? idx_high : size);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (int(idx) < size && ! fn_higher_equal(begin[idx]))
            ++ idx;
    }
    return idx;
}
template<typename T, typename IndexType, typename FN_HIGHER_EQUAL>
IndexType idx_higher_or_equal(const std::vector<T>& vec, IndexType idx, FN_HIGHER_EQUAL fn_higher_equal)
{
    return idx_higher_or_equal(vec.begin(), vec.end(), idx, fn_higher_equal);
}

// FN_LOWER_EQUAL: the provided object pointer has a Z value <= of an internal threshold.
// Find the first item with Z value <= of an internal threshold of fn_lower_equal.
// If no vec item with Z value <= of an internal threshold of fn_lower_equal is found, return -1.
// If the initial idx is < -1, then use binary search.
// Otherwise search linearly downwards.
template<typename IT, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(IT begin, IT end, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    auto size = int(end - begin);
    if (size == 0) {
        idx = -1;
    } else if (idx < -1) {
        // First of the batch of layers per thread pool invocation. Use binary search.
        int idx_low  = 0;
        int idx_high = std::max(0, size - 1);
        while (idx_low + 1 < idx_high) {
            int idx_mid  = (idx_low + idx_high) / 2;
            if (fn_lower_equal(begin[idx_mid]))
                idx_low  = idx_mid;
            else
                idx_high = idx_mid;
        }
        idx =  fn_lower_equal(begin[idx_high]) ? idx_high :
              (fn_lower_equal(begin[idx_low ]) ? idx_low  : -1);
    } else {
        // For the other layers of this batch of layers, search incrementally, which is cheaper than the binary search.
        while (idx >= 0 && ! fn_lower_equal(begin[idx]))
            -- idx;
    }
    return idx;
}
template<typename T, typename FN_LOWER_EQUAL>
int idx_lower_or_equal(const std::vector<T*> &vec, int idx, FN_LOWER_EQUAL fn_lower_equal)
{
    return idx_lower_or_equal(vec.begin(), vec.end(), idx, fn_lower_equal);
}

} // namespace Slic3r

#endif /* slic3r_SupportCommon_hpp_ */
