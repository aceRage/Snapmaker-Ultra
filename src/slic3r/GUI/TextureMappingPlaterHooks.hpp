#ifndef slic3r_TextureMappingPlaterHooks_hpp_
#define slic3r_TextureMappingPlaterHooks_hpp_

#include "libslic3r/Color.hpp"
#include "libslic3r/Model.hpp"
#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/TextureMapping.hpp"

#include <map>
#include <set>
#include <string>
#include <vector>

namespace Slic3r {

class PresetBundle;

namespace GUI {
namespace TextureMappingPlaterHooks {

struct TextureMappingZoneShellUsageSummary
{
    int object_count { 0 };
    int min_top_shell_layers { 0 };
    int min_bottom_shell_layers { 0 };
    int raw_top_surface_object_count { 0 };
    int raw_top_surface_min_top_shell_layers { 0 };
};

bool model_volume_has_imported_texture_mapping_data(const ModelVolume *volume);
bool model_object_has_imported_texture_mapping_data(const ModelObject *object);

bool assign_imported_texture_mapping_zone(Model &model);
bool assign_imported_3mf_texture_mapping_zones(Model &model, const std::set<unsigned int> &source_zone_ids);
std::set<unsigned int> texture_mapping_zone_ids_from_import_config(const DynamicPrintConfig &config);
bool auto_add_missing_mmu_segmentation_filaments_to_current_project(Model &model);

bool canonicalize_texture_mapping_config(PresetBundle &bundle, bool sync_model);
void load_texture_mapping_definitions(PresetBundle &bundle, const std::string &serialized);
void sync_model_texture_mapping_definitions(Model &model, const std::string &serialized);
void sync_current_model_texture_mapping_definitions(const std::string &serialized);
bool store_texture_mapping_definitions(PresetBundle &bundle);
void set_texture_mapping_definitions(PresetBundle &bundle, const std::string &serialized);
std::string serialize_texture_mapping_manager(TextureMappingManager *manager);
bool set_texture_mapping_config_string(DynamicPrintConfig &config, const std::string &key, const std::string &value);
std::string texture_mapping_config_string(const DynamicPrintConfig  &project_config,
                                          const DynamicPrintConfig *print_config,
                                          const std::string        &key);

bool model_uses_texture_mapping_zone_id(const Model &model, const ConfigOptionResolver *print_config, unsigned int zone_id);
TextureMappingZoneShellUsageSummary texture_mapping_zone_shell_usage_summary(const Model              &model,
                                                                            const DynamicPrintConfig &print_config,
                                                                            unsigned int              zone_id);

const std::vector<std::string> &texture_mapping_display_colors(PresetBundle               *bundle,
                                                               const DynamicPrintConfig   *config,
                                                               const std::vector<std::string> &filament_colors);
const std::vector<ColorRGBA>   &texture_mapping_rgba_colors(PresetBundle               *bundle,
                                                            const DynamicPrintConfig   *config,
                                                            const std::vector<std::string> &filament_colors);
void invalidate_texture_mapping_display_color_cache();

// Wipe-tower filament COUNT from texture_mapping_zones. Does not use prime-tower images.
// PartPlate.cpp is intentionally untouched; call this from Plater / ArrangeJob.
size_t estimate_wipe_tower_filaments_count_for_texture_mapping(const std::vector<int>  &extruders,
                                                               const DynamicPrintConfig *config);
std::vector<int> expand_wipe_tower_extruders_for_texture_mapping(const std::vector<int>  &extruders,
                                                                 const DynamicPrintConfig *config);

bool remap_mmu_segmentation_filaments(Model &model, const std::map<unsigned int, unsigned int> &filament_id_map);
std::vector<unsigned int> collect_missing_mmu_segmentation_filaments(const Model                 &model,
                                                                     size_t                       physical_count,
                                                                     const TextureMappingManager &manager);

} // namespace TextureMappingPlaterHooks
} // namespace GUI
} // namespace Slic3r

#endif
