#include "TextureMappingPlaterHooks.hpp"

#include "GUI_App.hpp"
#include "Plater.hpp"

#include "libslic3r/PresetBundle.hpp"

#include "nlohmann/json.hpp"

#include <algorithm>
#include <limits>

using json = nlohmann::json;

namespace Slic3r {
namespace GUI {
namespace TextureMappingPlaterHooks {

namespace {

const std::array<const char *, 6> &texture_mapping_filament_config_keys()
{
    static const std::array<const char *, 6> filament_keys = {
        "extruder",
        "wall_filament",
        "sparse_infill_filament",
        "solid_infill_filament",
        "support_filament",
        "support_interface_filament"
    };
    return filament_keys;
}

bool texture_mapping_config_uses_zone_id(const ConfigOptionResolver &config, unsigned int zone_id)
{
    const int target = int(zone_id);
    for (const char *key : texture_mapping_filament_config_keys()) {
        const ConfigOptionInt *opt = dynamic_cast<const ConfigOptionInt *>(config.option(key));
        if (opt != nullptr && opt->getInt() == target)
            return true;
    }
    return false;
}

int texture_mapping_config_int_or(const ConfigOptionResolver &config, const char *key, int fallback)
{
    const ConfigOptionInt *opt = dynamic_cast<const ConfigOptionInt *>(config.option(key));
    return opt != nullptr ? opt->getInt() : fallback;
}

bool texture_mapping_object_has_raw_top_surface_layers(const ModelObject &object)
{
    for (const ModelVolume *volume : object.volumes) {
        if (volume == nullptr ||
            volume->imported_texture_width == 0 ||
            volume->imported_texture_height == 0 ||
            volume->imported_texture_raw_top_surface_depths.empty())
            continue;
        const size_t pixel_count =
            size_t(volume->imported_texture_width) * size_t(volume->imported_texture_height);
        if (pixel_count > 0 &&
            volume->imported_texture_raw_top_surface_filament_slots.size() >=
                pixel_count * volume->imported_texture_raw_top_surface_depths.size() &&
            std::any_of(volume->imported_texture_raw_top_surface_depths.begin(),
                        volume->imported_texture_raw_top_surface_depths.end(),
                        [](int depth) { return depth >= 0; }))
            return true;
    }
    return false;
}

bool texture_mapping_effective_config_uses_zone_id(const ConfigOptionResolver  &config,
                                                   const ConfigOptionResolver *fallback_config,
                                                   const ConfigOptionResolver *secondary_fallback_config,
                                                   unsigned int                zone_id)
{
    const int target = int(zone_id);
    const std::array<const ConfigOptionResolver *, 3> configs { &config, fallback_config, secondary_fallback_config };
    for (const char *key : texture_mapping_filament_config_keys()) {
        for (const ConfigOptionResolver *resolver : configs) {
            if (resolver == nullptr)
                continue;
            const ConfigOptionInt *opt = dynamic_cast<const ConfigOptionInt *>(resolver->option(key));
            if (opt == nullptr)
                continue;
            if (opt->getInt() == target)
                return true;
            break;
        }
    }
    return false;
}

std::map<uint64_t, unsigned int> active_texture_zone_ids_by_stable_id_for_import(const TextureMappingManager &manager)
{
    std::map<uint64_t, unsigned int> ids;
    for (const TextureMappingZone &zone : manager.zones())
        if (zone.enabled && !zone.deleted && zone.stable_id != 0 && zone.zone_id != 0)
            ids[zone.stable_id] = zone.zone_id;
    return ids;
}

bool remap_texture_mapping_model_config(ModelConfig &config, const std::set<unsigned int> &source_zone_ids, unsigned int target_zone_id)
{
    bool changed = false;
    for (const char *key : texture_mapping_filament_config_keys()) {
        const ConfigOptionInt *opt = dynamic_cast<const ConfigOptionInt *>(config.option(key));
        if (opt == nullptr || source_zone_ids.find(unsigned(opt->getInt())) == source_zone_ids.end())
            continue;
        if (opt->getInt() != int(target_zone_id)) {
            config.set(key, int(target_zone_id));
            changed = true;
        }
    }
    return changed;
}

unsigned int ensure_texture_mapping_import_target_zone(PresetBundle &bundle, bool &changed)
{
    DynamicPrintConfig &project_config = bundle.project_config;
    const ConfigOptionStrings *color_opt = project_config.option<ConfigOptionStrings>("filament_colour", false);
    if (color_opt == nullptr || color_opt->values.size() < 2)
        return 0;

    project_config.option<ConfigOptionString>("texture_mapping_definitions", true);
    bundle.texture_mapping_zones.load_entries(project_config.opt_string("texture_mapping_definitions"), color_opt->values);
    for (const unsigned int zone_id : bundle.texture_mapping_zones.zone_ids_by_index())
        if (zone_id != 0)
            return zone_id;

    const unsigned int zone_id = bundle.texture_mapping_zones.ensure_image_texture_zone(color_opt->values.size(), color_opt->values);
    if (zone_id != 0)
        changed |= store_texture_mapping_definitions(bundle);
    return zone_id;
}

std::vector<std::string> wipe_tower_filament_colours(const DynamicPrintConfig *config)
{
    auto normalize_count = [](std::vector<std::string> colours) {
        const int filaments_count = std::max(wxGetApp().filaments_cnt(), 0);
        if (filaments_count > 0)
            colours.resize(size_t(filaments_count), "#FFFFFF");
        return colours;
    };

    if (config != nullptr) {
        if (const ConfigOptionStrings *opt = dynamic_cast<const ConfigOptionStrings *>(config->option("filament_colour"));
            opt != nullptr && !opt->values.empty())
            return normalize_count(opt->values);
    }

    if (wxGetApp().preset_bundle != nullptr) {
        if (const ConfigOptionStrings *opt =
                dynamic_cast<const ConfigOptionStrings *>(wxGetApp().preset_bundle->project_config.option("filament_colour"));
            opt != nullptr && !opt->values.empty())
            return normalize_count(opt->values);
    }

    const int filaments_count = std::max(wxGetApp().filaments_cnt(), 0);
    return std::vector<std::string>(size_t(filaments_count), "#FFFFFF");
}

struct DisplayColorCache {
    std::string serialized;
    std::vector<std::string> filament_colors;
    std::vector<std::string> display_colors;
    std::vector<ColorRGBA>   rgba_colors;
};

DisplayColorCache &display_color_cache()
{
    static DisplayColorCache cache;
    return cache;
}

ColorRGBA parse_hex_color(const std::string &hex)
{
    ColorRGBA out = ColorRGBA::WHITE();
    if (hex.size() >= 7 && hex[0] == '#') {
        auto hex_digit = [](char ch) -> int {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
            if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
            return 0;
        };
        out = ColorRGBA(float(hex_digit(hex[1]) * 16 + hex_digit(hex[2])) / 255.f,
                        float(hex_digit(hex[3]) * 16 + hex_digit(hex[4])) / 255.f,
                        float(hex_digit(hex[5]) * 16 + hex_digit(hex[6])) / 255.f,
                        1.f);
    }
    return out;
}

void refresh_display_color_cache(PresetBundle *bundle, const DynamicPrintConfig *config, const std::vector<std::string> &filament_colors)
{
    DisplayColorCache &cache = display_color_cache();
    const std::string serialized = (bundle != nullptr) ? bundle->texture_mapping_zones.serialize_entries() :
        (config != nullptr ? config->opt_string("texture_mapping_definitions") : std::string());
    if (cache.serialized == serialized && cache.filament_colors == filament_colors)
        return;

    cache.serialized = serialized;
    cache.filament_colors = filament_colors;
    cache.display_colors.clear();
    cache.rgba_colors.clear();

    TextureMappingManager *mgr = bundle != nullptr ? &bundle->texture_mapping_zones : nullptr;
    TextureMappingManager local;
    if (mgr == nullptr) {
        local.load_entries(serialized, filament_colors);
        mgr = &local;
    } else if (mgr->zones().empty() && !serialized.empty()) {
        mgr->load_entries(serialized, filament_colors);
    }

    cache.display_colors = mgr->display_colors(filament_colors.size());
    cache.rgba_colors.reserve(cache.display_colors.size());
    for (const std::string &hex : cache.display_colors)
        cache.rgba_colors.push_back(parse_hex_color(hex));
}

} // namespace

bool model_volume_has_imported_texture_mapping_data(const ModelVolume *volume)
{
    return volume != nullptr &&
           (!volume->imported_vertex_colors_rgba.empty() ||
            (!volume->imported_texture_rgba.empty() &&
             volume->imported_texture_width > 0 &&
             volume->imported_texture_height > 0));
}

bool model_object_has_imported_texture_mapping_data(const ModelObject *object)
{
    return object != nullptr && std::any_of(object->volumes.begin(), object->volumes.end(), [](const ModelVolume *volume) {
        return model_volume_has_imported_texture_mapping_data(volume);
    });
}

bool assign_imported_texture_mapping_zone(Model &model)
{
    bool has_imported_data = false;
    for (const ModelObject *object : model.objects) {
        if (model_object_has_imported_texture_mapping_data(object)) {
            has_imported_data = true;
            break;
        }
    }
    if (!has_imported_data)
        return false;

    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;

    DynamicPrintConfig &project_config = bundle->project_config;
    const ConfigOptionStrings *color_opt = project_config.option<ConfigOptionStrings>("filament_colour", false);
    if (color_opt == nullptr || color_opt->values.size() < 2)
        return false;

    project_config.option<ConfigOptionString>("texture_mapping_definitions", true);
    project_config.option<ConfigOptionString>("texture_mapping_global_settings", true);
    bundle->texture_mapping_zones.load_entries(project_config.opt_string("texture_mapping_definitions"), color_opt->values);
    bundle->texture_mapping_global_settings.load(project_config.opt_string("texture_mapping_global_settings"));
    const unsigned int zone_id = bundle->texture_mapping_zones.ensure_image_texture_zone(color_opt->values.size(), color_opt->values);
    if (zone_id == 0)
        return false;

    const std::string serialized = bundle->texture_mapping_zones.serialize_entries();
    set_texture_mapping_definitions(*bundle, serialized);
    const std::string global_serialized = bundle->texture_mapping_global_settings.serialize();
    set_texture_mapping_config_string(project_config, "texture_mapping_global_settings", global_serialized);
    DynamicPrintConfig &print_config = bundle->prints.get_edited_preset().config;
    set_texture_mapping_config_string(print_config, "texture_mapping_global_settings", global_serialized);
    sync_model_texture_mapping_definitions(model, serialized);

    for (ModelObject *object : model.objects) {
        if (!model_object_has_imported_texture_mapping_data(object))
            continue;
        object->config.set("extruder", int(zone_id));
        for (ModelVolume *volume : object->volumes)
            if (model_volume_has_imported_texture_mapping_data(volume))
                volume->config.set("extruder", int(zone_id));
    }
    invalidate_texture_mapping_display_color_cache();
    return true;
}

std::set<unsigned int> texture_mapping_zone_ids_from_import_config(const DynamicPrintConfig &config)
{
    const ConfigOptionString *defs = config.option<ConfigOptionString>("texture_mapping_definitions");
    if (defs == nullptr || defs->value.empty())
        return {};

    std::set<unsigned int> ids;
    try {
        const json root = json::parse(defs->value);
        if (!root.is_array())
            return {};
        for (const json &entry : root) {
            if (!entry.is_object() || !entry.value("enabled", true))
                continue;
            const unsigned int zone_id = entry.value("zone_id", entry.value("filament_id", 0u));
            if (zone_id != 0)
                ids.insert(zone_id);
        }
    } catch (...) {
        ids.clear();
    }
    return ids;
}

bool store_texture_mapping_definitions(PresetBundle &bundle)
{
    set_texture_mapping_definitions(bundle, bundle.texture_mapping_zones.serialize_entries());
    return true;
}

void set_texture_mapping_definitions(PresetBundle &bundle, const std::string &serialized)
{
    set_texture_mapping_config_string(bundle.project_config, "texture_mapping_definitions", serialized);
    set_texture_mapping_config_string(bundle.prints.get_edited_preset().config, "texture_mapping_definitions", serialized);
}

void load_texture_mapping_definitions(PresetBundle &bundle, const std::string &serialized)
{
    set_texture_mapping_definitions(bundle, serialized);
    const ConfigOptionStrings *color_opt = bundle.project_config.option<ConfigOptionStrings>("filament_colour", false);
    bundle.texture_mapping_zones.load_entries(serialized, color_opt != nullptr ? color_opt->values : std::vector<std::string>());
    set_texture_mapping_definitions(bundle, bundle.texture_mapping_zones.serialize_entries());
    invalidate_texture_mapping_display_color_cache();
}

void sync_model_texture_mapping_definitions(Model &model, const std::string &serialized)
{
    model.texture_mapping_definitions = serialized;
    model.texture_mapping_definitions_valid = true;
}

void sync_current_model_texture_mapping_definitions(const std::string &serialized)
{
    if (Plater *plater = wxGetApp().plater(); plater != nullptr)
        sync_model_texture_mapping_definitions(plater->model(), serialized);
}

std::string serialize_texture_mapping_manager(TextureMappingManager *manager)
{
    return manager != nullptr ? manager->serialize_entries() : std::string();
}

std::string texture_mapping_config_string(const DynamicPrintConfig  &project_config,
                                          const DynamicPrintConfig *print_config,
                                          const std::string        &key)
{
    if (project_config.has(key)) {
        const std::string value = project_config.opt_string(key);
        if (!value.empty())
            return value;
    }
    if (print_config != nullptr && print_config->has(key))
        return print_config->opt_string(key);
    return std::string();
}

bool set_texture_mapping_config_string(DynamicPrintConfig &config, const std::string &key, const std::string &value)
{
    if (ConfigOptionString *opt = config.option<ConfigOptionString>(key)) {
        if (opt->value == value)
            return false;
        opt->value = value;
        return true;
    }
    if (value.empty())
        return false;
    config.set_key_value(key, new ConfigOptionString(value));
    return true;
}

bool canonicalize_texture_mapping_config(PresetBundle &bundle, bool sync_model)
{
    DynamicPrintConfig &project_config = bundle.project_config;
    DynamicPrintConfig &print_config = bundle.prints.get_edited_preset().config;
    const ConfigOptionStrings *color_opt = project_config.option<ConfigOptionStrings>("filament_colour", false);
    if (color_opt == nullptr)
        return false;

    bool changed = false;

    const std::string serialized = texture_mapping_config_string(project_config, &print_config, "texture_mapping_definitions");
    bundle.texture_mapping_zones.load_entries(serialized, color_opt->values);
    const std::string canonical = bundle.texture_mapping_zones.serialize_entries();
    changed |= set_texture_mapping_config_string(project_config, "texture_mapping_definitions", canonical);
    changed |= set_texture_mapping_config_string(print_config, "texture_mapping_definitions", canonical);
    if (sync_model)
        sync_current_model_texture_mapping_definitions(canonical);

    const std::string global_serialized = texture_mapping_config_string(project_config, &print_config, "texture_mapping_global_settings");
    bundle.texture_mapping_global_settings.load(global_serialized);
    const std::string global_canonical = bundle.texture_mapping_global_settings.serialize();
    changed |= set_texture_mapping_config_string(project_config, "texture_mapping_global_settings", global_canonical);
    changed |= set_texture_mapping_config_string(print_config, "texture_mapping_global_settings", global_canonical);
    if (changed)
        invalidate_texture_mapping_display_color_cache();
    return changed;
}

bool model_uses_texture_mapping_zone_id(const Model &model, const ConfigOptionResolver *print_config, unsigned int zone_id)
{
    if (zone_id == 0)
        return false;
    if (print_config != nullptr && texture_mapping_config_uses_zone_id(*print_config, zone_id))
        return true;
    for (const ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        if (texture_mapping_config_uses_zone_id(object->config.get(), zone_id))
            return true;
        for (const auto &layer_range : object->layer_config_ranges)
            if (texture_mapping_config_uses_zone_id(layer_range.second.get(), zone_id))
                return true;
        for (const ModelVolume *volume : object->volumes) {
            if (volume == nullptr)
                continue;
            if (texture_mapping_config_uses_zone_id(volume->config.get(), zone_id))
                return true;
            const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
            if (size_t(zone_id) < used_states.size() && used_states[zone_id])
                return true;
        }
    }
    return false;
}

TextureMappingZoneShellUsageSummary texture_mapping_zone_shell_usage_summary(const Model              &model,
                                                                            const DynamicPrintConfig &print_config,
                                                                            unsigned int              zone_id)
{
    TextureMappingZoneShellUsageSummary out;
    if (zone_id == 0)
        return out;

    const int global_top_shell_layers = texture_mapping_config_int_or(print_config, "top_shell_layers", 0);
    const int global_bottom_shell_layers = texture_mapping_config_int_or(print_config, "bottom_shell_layers", 0);
    int min_top_shell_layers = std::numeric_limits<int>::max();
    int min_bottom_shell_layers = std::numeric_limits<int>::max();
    int raw_top_surface_min_top_shell_layers = std::numeric_limits<int>::max();

    for (const ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;

        const ConfigOptionResolver &object_config = object->config.get();
        const int object_top_shell_layers =
            texture_mapping_config_int_or(object_config, "top_shell_layers", global_top_shell_layers);
        const int object_bottom_shell_layers =
            texture_mapping_config_int_or(object_config, "bottom_shell_layers", global_bottom_shell_layers);
        int object_min_top_shell_layers = object_top_shell_layers;
        int object_min_bottom_shell_layers = object_bottom_shell_layers;
        bool object_uses_zone = false;

        auto note_usage = [&](int top_shell_layers, int bottom_shell_layers) {
            object_uses_zone = true;
            object_min_top_shell_layers = std::min(object_min_top_shell_layers, top_shell_layers);
            object_min_bottom_shell_layers = std::min(object_min_bottom_shell_layers, bottom_shell_layers);
        };

        if (texture_mapping_effective_config_uses_zone_id(object_config, &print_config, nullptr, zone_id))
            note_usage(object_top_shell_layers, object_bottom_shell_layers);

        for (const auto &layer_range : object->layer_config_ranges) {
            const ConfigOptionResolver &layer_config = layer_range.second.get();
            if (!texture_mapping_effective_config_uses_zone_id(layer_config, &object_config, &print_config, zone_id))
                continue;
            note_usage(texture_mapping_config_int_or(layer_config, "top_shell_layers", object_top_shell_layers),
                       texture_mapping_config_int_or(layer_config, "bottom_shell_layers", object_bottom_shell_layers));
        }

        for (const ModelVolume *volume : object->volumes) {
            if (volume == nullptr)
                continue;
            const ConfigOptionResolver &volume_config = volume->config.get();
            const int volume_top_shell_layers =
                texture_mapping_config_int_or(volume_config, "top_shell_layers", object_top_shell_layers);
            const int volume_bottom_shell_layers =
                texture_mapping_config_int_or(volume_config, "bottom_shell_layers", object_bottom_shell_layers);
            if (texture_mapping_effective_config_uses_zone_id(volume_config, &object_config, &print_config, zone_id))
                note_usage(volume_top_shell_layers, volume_bottom_shell_layers);
            const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
            if (size_t(zone_id) < used_states.size() && used_states[zone_id])
                note_usage(volume_top_shell_layers, volume_bottom_shell_layers);
        }

        if (!object_uses_zone)
            continue;
        ++out.object_count;
        min_top_shell_layers = std::min(min_top_shell_layers, object_min_top_shell_layers);
        min_bottom_shell_layers = std::min(min_bottom_shell_layers, object_min_bottom_shell_layers);
        if (texture_mapping_object_has_raw_top_surface_layers(*object)) {
            ++out.raw_top_surface_object_count;
            raw_top_surface_min_top_shell_layers =
                std::min(raw_top_surface_min_top_shell_layers, object_min_top_shell_layers);
        }
    }

    if (out.object_count > 0) {
        out.min_top_shell_layers = min_top_shell_layers == std::numeric_limits<int>::max() ? 0 : min_top_shell_layers;
        out.min_bottom_shell_layers = min_bottom_shell_layers == std::numeric_limits<int>::max() ? 0 : min_bottom_shell_layers;
    }
    if (out.raw_top_surface_object_count > 0)
        out.raw_top_surface_min_top_shell_layers =
            raw_top_surface_min_top_shell_layers == std::numeric_limits<int>::max() ?
                0 :
                raw_top_surface_min_top_shell_layers;
    return out;
}

bool assign_imported_3mf_texture_mapping_zones(Model &model, const std::set<unsigned int> &source_zone_ids)
{
    if (source_zone_ids.empty())
        return false;

    std::set<unsigned int> used_zone_ids;
    for (const unsigned int zone_id : source_zone_ids)
        if (model_uses_texture_mapping_zone_id(model, nullptr, zone_id))
            used_zone_ids.insert(zone_id);
    if (used_zone_ids.empty())
        return false;

    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;

    bool changed = false;
    const unsigned int target_zone_id = ensure_texture_mapping_import_target_zone(*bundle, changed);
    if (target_zone_id == 0)
        return changed;

    for (ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        changed |= remap_texture_mapping_model_config(object->config, used_zone_ids, target_zone_id);
        for (auto &layer_range : object->layer_config_ranges)
            changed |= remap_texture_mapping_model_config(layer_range.second, used_zone_ids, target_zone_id);
        for (ModelVolume *volume : object->volumes)
            if (volume != nullptr)
                changed |= remap_texture_mapping_model_config(volume->config, used_zone_ids, target_zone_id);
    }

    std::map<unsigned int, unsigned int> state_map;
    for (const unsigned int zone_id : used_zone_ids)
        if (zone_id != target_zone_id)
            state_map[zone_id] = target_zone_id;
    changed |= remap_mmu_segmentation_filaments(model, state_map);
    return changed;
}

bool remap_mmu_segmentation_filaments(Model &model, const std::map<unsigned int, unsigned int> &filament_id_map)
{
    if (filament_id_map.empty())
        return false;

    EnforcerBlockerStateMap state_map;
    for (size_t i = 0; i < state_map.size(); ++i)
        state_map[i] = EnforcerBlockerType(i);

    unsigned int max_id = 0;
    for (const auto &[from, to] : filament_id_map) {
        if (from < state_map.size())
            state_map[from] = EnforcerBlockerType(to);
        max_id = std::max(max_id, std::max(from, to));
    }

    bool changed = false;
    for (ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        for (ModelVolume *volume : object->volumes) {
            if (volume == nullptr || volume->mmu_segmentation_facets.empty())
                continue;
            volume->mmu_segmentation_facets.remap_enforcer_block_types(*volume, EnforcerBlockerType(std::max(max_id, 1u)), state_map);
            changed = true;
        }
    }
    return changed;
}

std::vector<unsigned int> collect_missing_mmu_segmentation_filaments(const Model                 &model,
                                                                     size_t                       physical_count,
                                                                     const TextureMappingManager &manager)
{
    std::set<unsigned int> missing;
    for (const ModelObject *object : model.objects) {
        if (object == nullptr)
            continue;
        for (const ModelVolume *volume : object->volumes) {
            if (volume == nullptr || volume->mmu_segmentation_facets.empty())
                continue;
            const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
            for (size_t state_id = 1; state_id < used_states.size(); ++state_id) {
                if (!used_states[state_id])
                    continue;
                if (state_id <= physical_count)
                    continue;
                if (manager.is_texture_mapping_zone_id(unsigned(state_id)))
                    continue;
                missing.insert(unsigned(state_id));
            }
        }
    }
    return std::vector<unsigned int>(missing.begin(), missing.end());
}

bool auto_add_missing_mmu_segmentation_filaments_to_current_project(Model &model)
{
    PresetBundle *bundle = wxGetApp().preset_bundle;
    if (bundle == nullptr)
        return false;

    constexpr size_t max_physical_filament_id = MAXIMUM_EXTRUDER_NUMBER < 99 ? MAXIMUM_EXTRUDER_NUMBER : 98;
    const size_t physical_count = bundle->filament_presets.size();
    if (physical_count == 0 || physical_count >= max_physical_filament_id)
        return false;

    const std::map<uint64_t, unsigned int> old_zone_ids =
        active_texture_zone_ids_by_stable_id_for_import(bundle->texture_mapping_zones);
    std::vector<unsigned int> missing_states =
        collect_missing_mmu_segmentation_filaments(model, physical_count, bundle->texture_mapping_zones);
    if (missing_states.empty())
        return false;

    std::vector<std::string> new_colors;
    std::map<unsigned int, unsigned int> filament_id_map;
    unsigned int next_filament_id = unsigned(physical_count + 1);
    for (const unsigned int state_id : missing_states) {
        if (next_filament_id > unsigned(max_physical_filament_id))
            break;
        if (state_id != next_filament_id)
            filament_id_map[state_id] = next_filament_id;
        if (Plater *plater = wxGetApp().plater()) {
            wxColour new_col = Plater::get_next_color_for_filament();
            new_colors.emplace_back(new_col.GetAsString(wxC2S_HTML_SYNTAX).ToStdString());
        } else {
            new_colors.emplace_back("#26A69A");
        }
        ++next_filament_id;
    }

    if (new_colors.empty())
        return false;

    const size_t target_count = physical_count + new_colors.size();
    bundle->set_num_filaments(unsigned(target_count), new_colors);

    const std::map<uint64_t, unsigned int> new_zone_ids =
        active_texture_zone_ids_by_stable_id_for_import(bundle->texture_mapping_zones);
    for (const auto &[stable_id, old_zone_id] : old_zone_ids) {
        auto new_it = new_zone_ids.find(stable_id);
        if (new_it != new_zone_ids.end() && new_it->second != old_zone_id)
            filament_id_map[old_zone_id] = new_it->second;
    }

    remap_mmu_segmentation_filaments(model, filament_id_map);
    if (Plater *plater = wxGetApp().plater())
        plater->on_filaments_change(target_count);
    return true;
}

const std::vector<std::string> &texture_mapping_display_colors(PresetBundle               *bundle,
                                                               const DynamicPrintConfig   *config,
                                                               const std::vector<std::string> &filament_colors)
{
    refresh_display_color_cache(bundle, config, filament_colors);
    return display_color_cache().display_colors;
}

const std::vector<ColorRGBA> &texture_mapping_rgba_colors(PresetBundle               *bundle,
                                                          const DynamicPrintConfig   *config,
                                                          const std::vector<std::string> &filament_colors)
{
    refresh_display_color_cache(bundle, config, filament_colors);
    return display_color_cache().rgba_colors;
}

void invalidate_texture_mapping_display_color_cache()
{
    display_color_cache() = {};
}

std::vector<int> expand_wipe_tower_extruders_for_texture_mapping(const std::vector<int>  &extruders,
                                                                 const DynamicPrintConfig *config)
{
    if (extruders.empty())
        return extruders;

    const std::vector<std::string> filament_colours = wipe_tower_filament_colours(config);
    const size_t num_physical = filament_colours.size();
    if (num_physical == 0)
        return extruders;

    TextureMappingManager texture_mgr;
    const std::string texture_mapping_definitions = config != nullptr && config->has("texture_mapping_definitions") ?
        config->opt_string("texture_mapping_definitions") :
        (wxGetApp().preset_bundle != nullptr && wxGetApp().preset_bundle->project_config.has("texture_mapping_definitions") ?
             wxGetApp().preset_bundle->project_config.opt_string("texture_mapping_definitions") :
             std::string());
    texture_mgr.load_entries(texture_mapping_definitions, filament_colours);

    std::vector<int> expanded;
    expanded.reserve(extruders.size());
    for (const int extruder : extruders) {
        if (extruder <= 0)
            continue;
        const unsigned int filament_id = static_cast<unsigned int>(extruder);
        if (filament_id <= num_physical) {
            expanded.emplace_back(extruder);
            continue;
        }
        const TextureMappingZone *zone = texture_mgr.zone_from_id(filament_id);
        if (zone == nullptr) {
            expanded.emplace_back(extruder);
            continue;
        }
        std::vector<unsigned int> component_ids = zone->is_image_texture() ?
            TextureMappingManager::effective_texture_component_ids(*zone, num_physical, filament_colours) :
            TextureMappingManager::selected_component_ids(*zone, num_physical);
        if (component_ids.empty())
            component_ids = TextureMappingManager::selected_component_ids(*zone, num_physical);
        bool added = false;
        for (const unsigned int component_id : component_ids) {
            if (component_id >= 1 && component_id <= num_physical) {
                expanded.emplace_back(int(component_id));
                added = true;
            }
        }
        if (!added)
            expanded.emplace_back(1);
    }

    std::sort(expanded.begin(), expanded.end());
    expanded.erase(std::unique(expanded.begin(), expanded.end()), expanded.end());
    return expanded;
}

size_t estimate_wipe_tower_filaments_count_for_texture_mapping(const std::vector<int>  &extruders,
                                                               const DynamicPrintConfig *config)
{
    if (extruders.empty())
        return 0;

    const std::vector<std::string> filament_colours = wipe_tower_filament_colours(config);
    const size_t                   num_physical     = filament_colours.size();
    if (num_physical == 0)
        return 0;

    TextureMappingManager texture_mgr;
    const std::string texture_mapping_definitions = config != nullptr && config->has("texture_mapping_definitions") ?
        config->opt_string("texture_mapping_definitions") :
        (wxGetApp().preset_bundle != nullptr && wxGetApp().preset_bundle->project_config.has("texture_mapping_definitions") ?
             wxGetApp().preset_bundle->project_config.opt_string("texture_mapping_definitions") :
             std::string());
    texture_mgr.load_entries(texture_mapping_definitions, filament_colours);

    struct TextureEstimatePattern {
        std::vector<unsigned int> component_ids;
        std::string component_weights;
        bool operator==(const TextureEstimatePattern &rhs) const
        {
            return component_ids == rhs.component_ids && component_weights == rhs.component_weights;
        }
    };

    auto append_physical_id = [num_physical](std::vector<unsigned int> &ids, unsigned int id) {
        if (id >= 1 && id <= num_physical)
            ids.emplace_back(id);
    };
    auto append_physical_ids = [&append_physical_id](std::vector<unsigned int> &ids, const std::vector<unsigned int> &to_add) {
        for (const unsigned int id : to_add)
            append_physical_id(ids, id);
    };
    auto sort_unique = [](std::vector<unsigned int> &ids) {
        std::sort(ids.begin(), ids.end());
        ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    };
    auto contains_id = [](const std::vector<unsigned int> &ids, unsigned int id) {
        return std::find(ids.begin(), ids.end(), id) != ids.end();
    };

    std::vector<unsigned int> physical_ids;
    std::vector<unsigned int> all_texture_component_ids;
    std::vector<unsigned int> every_layer_texture_ids;
    std::vector<std::pair<TextureEstimatePattern, std::vector<unsigned int>>> sequential_texture_patterns;

    for (const int extruder : extruders) {
        if (extruder <= 0)
            continue;
        const unsigned int filament_id = static_cast<unsigned int>(extruder);
        if (filament_id <= num_physical) {
            physical_ids.emplace_back(filament_id);
            continue;
        }
        const TextureMappingZone *zone = texture_mgr.zone_from_id(filament_id);
        if (zone == nullptr || !zone->enabled || zone->deleted)
            continue;

        std::vector<unsigned int> component_ids = zone->is_image_texture() ?
            TextureMappingManager::effective_texture_component_ids(*zone, num_physical, filament_colours) :
            TextureMappingManager::selected_component_ids(*zone, num_physical);
        std::vector<unsigned int> filtered;
        for (const unsigned int id : component_ids)
            if (id >= 1 && id <= num_physical && !contains_id(filtered, id))
                filtered.emplace_back(id);
        component_ids = std::move(filtered);
        if (component_ids.empty()) {
            const unsigned int resolved = texture_mgr.resolve_zone_component(filament_id, num_physical, 0);
            if (resolved >= 1 && resolved <= num_physical)
                component_ids.emplace_back(resolved);
        }
        if (component_ids.empty())
            continue;

        append_physical_ids(all_texture_component_ids, component_ids);
        const bool same_layer_components =
            zone->top_surface_image_printing_enabled ||
            zone->recolor_small_perimeter_loops ||
            zone->recolor_top_visible_perimeter_sections;
        if (same_layer_components || component_ids.size() == 1)
            append_physical_ids(every_layer_texture_ids, component_ids);
        else
            sequential_texture_patterns.emplace_back(TextureEstimatePattern{component_ids, zone->component_weights}, component_ids);
    }

    sort_unique(physical_ids);
    sort_unique(all_texture_component_ids);
    sort_unique(every_layer_texture_ids);

    bool have_sequential_texture_slot = false;
    if (!sequential_texture_patterns.empty()) {
        std::vector<TextureEstimatePattern> patterns;
        std::vector<unsigned int> sequential_component_ids;
        for (const auto &entry : sequential_texture_patterns) {
            if (std::find(patterns.begin(), patterns.end(), entry.first) == patterns.end())
                patterns.emplace_back(entry.first);
            append_physical_ids(sequential_component_ids, entry.second);
        }
        if (patterns.size() == 1)
            have_sequential_texture_slot = true;
        else {
            append_physical_ids(every_layer_texture_ids, sequential_component_ids);
            sort_unique(every_layer_texture_ids);
        }
    }

    std::vector<unsigned int> all_used_ids = physical_ids;
    append_physical_ids(all_used_ids, all_texture_component_ids);
    sort_unique(all_used_ids);

    size_t count = every_layer_texture_ids.size();
    for (const unsigned int physical_id : physical_ids)
        if (!contains_id(every_layer_texture_ids, physical_id))
            ++count;
    if (have_sequential_texture_slot)
        count += 2;
    if (!all_used_ids.empty())
        count = std::min(count, all_used_ids.size());
    return count;
}

} // namespace TextureMappingPlaterHooks
} // namespace GUI
} // namespace Slic3r
