// original author: sentientstardust
// ImageMap FULL PR3 G-code texture / Contoning / seam-hiding helpers.
// Extracted from OrcaSlicer-ImageMap @ 92548381056.

#include "GCodeTextureMapping.hpp"

#include "BoundingBox.hpp"
#include "Color.hpp"
#include "GCode.hpp"
#include "Geometry.hpp"
#include "I18N.hpp"
#include "ImageMapRawFilamentOffsetAtlas.hpp"
#include "Layer.hpp"
#include "Model.hpp"
#include "Print.hpp"
#include "TextureMapping.hpp"
#include "TextureMappingOffset.hpp"
#include "TriangleSelector.hpp"
#include "Utils.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <iomanip>
#include <limits>
#include <optional>
#include <sstream>
#include <unordered_map>

#include <boost/log/trivial.hpp>

namespace Slic3r {

static float normalize_angle_deg_for_gcode(float angle)
{
    float normalized = std::fmod(angle, 360.f);
    if (normalized < 0.f)
        normalized += 360.f;
    return normalized;
}

static float angular_distance_deg_for_gcode(float a, float b)
{
    const float d = std::abs(normalize_angle_deg_for_gcode(a) - normalize_angle_deg_for_gcode(b));
    return std::min(d, 360.f - d);
}

static float angular_distance_cw_deg_for_gcode(float from_deg, float to_deg)
{
    float d = normalize_angle_deg_for_gcode(to_deg) - normalize_angle_deg_for_gcode(from_deg);
    if (d < 0.f)
        d += 360.f;
    return d;
}

static float clamp01f_for_gcode(float v)
{
    if (!std::isfinite(v))
        return 0.f;
    return std::clamp(v, 0.f, 1.f);
}

static float extrusion_area_for_width_height_for_gcode(float width_mm, float height_mm)
{
    if (!std::isfinite(width_mm) || !std::isfinite(height_mm))
        return 1e-6f;

    const float safe_width = std::max(0.01f, width_mm);
    const float safe_height = std::max(0.01f, height_mm);
    const float area = safe_height * (safe_width - safe_height * (1.f - float(0.25 * PI)));
    return std::max(1e-6f, area);
}

static double flow_scale_for_target_width_for_gcode(float base_width_mm, float target_width_mm, float height_mm)
{
    if (!std::isfinite(base_width_mm) || !std::isfinite(target_width_mm) || !std::isfinite(height_mm))
        return 1.0;

    const float base_area = extrusion_area_for_width_height_for_gcode(base_width_mm, height_mm);
    const float target_area = extrusion_area_for_width_height_for_gcode(target_width_mm, height_mm);
    if (!std::isfinite(base_area) || base_area <= 0.f || !std::isfinite(target_area))
        return 1.0;

    return std::clamp(double(target_area / base_area), 0.01, 10.0);
}

static double bbox_distance_sq_to_point_for_gcode(const BoundingBox &bbox, const Point &point)
{
    if (!bbox.defined)
        return 0.0;

    double dx = 0.0;
    if (point.x() < bbox.min.x())
        dx = double(bbox.min.x() - point.x());
    else if (point.x() > bbox.max.x())
        dx = double(point.x() - bbox.max.x());

    double dy = 0.0;
    if (point.y() < bbox.min.y())
        dy = double(bbox.min.y() - point.y());
    else if (point.y() > bbox.max.y())
        dy = double(point.y() - bbox.max.y());

    return dx * dx + dy * dy;
}

static bool find_nearest_layer_slice_boundary_point_for_gcode(const Layer *layer, const Point &query_point, Point &nearest_point)
{
    if (layer == nullptr || layer->lslices.empty())
        return false;

    const bool has_slice_bboxes = layer->lslices_bboxes.size() == layer->lslices.size();
    double best_distance_sq = std::numeric_limits<double>::max();
    bool found = false;

    for (size_t slice_idx = 0; slice_idx < layer->lslices.size(); ++slice_idx) {
        const ExPolygon &slice = layer->lslices[slice_idx];
        if (slice.empty())
            continue;

        if (has_slice_bboxes && layer->lslices_bboxes[slice_idx].defined) {
            const double bbox_distance_sq = bbox_distance_sq_to_point_for_gcode(layer->lslices_bboxes[slice_idx], query_point);
            if (bbox_distance_sq > best_distance_sq)
                continue;
        }

        const Point projected = slice.point_projection(query_point);
        const double projected_distance_sq = (projected - query_point).cast<double>().squaredNorm();
        if (projected_distance_sq < best_distance_sq) {
            best_distance_sq = projected_distance_sq;
            nearest_point = projected;
            found = true;
        }
    }

    return found;
}

struct NormalAwareLayerSliceBoundaryPointForGCode {
    Point  point;
    double outward_x { 0.0 };
    double outward_y { 0.0 };
    float  normal_delta_mm { 0.f };
    float  tangent_delta_mm { 0.f };
};

static std::optional<NormalAwareLayerSliceBoundaryPointForGCode> find_normal_aware_layer_slice_boundary_point_for_gcode(
    const Layer *layer,
    const Point &query_point,
    double       reference_outward_x,
    double       reference_outward_y,
    double       reference_tangent_x,
    double       reference_tangent_y,
    float        max_normal_delta_mm,
    float        max_tangent_delta_mm)
{
    if (layer == nullptr ||
        layer->lslices.empty() ||
        !std::isfinite(reference_outward_x) ||
        !std::isfinite(reference_outward_y) ||
        !std::isfinite(reference_tangent_x) ||
        !std::isfinite(reference_tangent_y))
        return std::nullopt;

    const double max_search_scaled = scale_(std::max(max_normal_delta_mm, max_tangent_delta_mm));
    const double max_search_sq = max_search_scaled * max_search_scaled;
    const bool has_slice_bboxes = layer->lslices_bboxes.size() == layer->lslices.size();
    double best_score = std::numeric_limits<double>::max();
    std::optional<NormalAwareLayerSliceBoundaryPointForGCode> best;

    auto consider_polygon = [&](const Polygon &poly) {
        const Points &points = poly.points;
        if (points.size() < 2)
            return;

        for (size_t idx = 0; idx < points.size(); ++idx) {
            const Point &a = points[idx];
            const Point &b = points[next_idx_modulo(idx, points.size())];
            const double ax = double(a.x());
            const double ay = double(a.y());
            const double bx = double(b.x());
            const double by = double(b.y());
            const double dx = bx - ax;
            const double dy = by - ay;
            const double len_sq = dx * dx + dy * dy;
            if (len_sq <= EPSILON)
                continue;

            const double qx = double(query_point.x());
            const double qy = double(query_point.y());
            const double t = std::clamp(((qx - ax) * dx + (qy - ay) * dy) / len_sq, 0.0, 1.0);
            const double px = ax + t * dx;
            const double py = ay + t * dy;
            const double delta_x = px - qx;
            const double delta_y = py - qy;
            const double dist_sq = delta_x * delta_x + delta_y * delta_y;
            if (dist_sq > max_search_sq)
                continue;

            const double len = std::sqrt(len_sq);
            const double tangent_x = dx / len;
            const double tangent_y = dy / len;
            const double outward_x = dy / len;
            const double outward_y = -dx / len;
            const double normal_alignment = outward_x * reference_outward_x + outward_y * reference_outward_y;
            if (normal_alignment < 0.15)
                continue;

            const float normal_delta_mm = unscale<float>(delta_x * reference_outward_x + delta_y * reference_outward_y);
            const float tangent_delta_mm = std::abs(unscale<float>(delta_x * reference_tangent_x + delta_y * reference_tangent_y));
            if (!std::isfinite(normal_delta_mm) ||
                !std::isfinite(tangent_delta_mm) ||
                std::abs(normal_delta_mm) > max_normal_delta_mm ||
                tangent_delta_mm > max_tangent_delta_mm)
                continue;

            const double tangent_alignment = std::abs(tangent_x * reference_tangent_x + tangent_y * reference_tangent_y);
            if (tangent_alignment < 0.2 && tangent_delta_mm > 0.25f * max_tangent_delta_mm)
                continue;

            const double dist_mm = unscale<double>(std::sqrt(dist_sq));
            const double score = dist_mm + 1.5 * double(tangent_delta_mm) + (1.0 - normal_alignment) * double(max_normal_delta_mm);
            if (score < best_score) {
                best_score = score;
                best = NormalAwareLayerSliceBoundaryPointForGCode {
                    Point(coord_t(std::llround(px)), coord_t(std::llround(py))),
                    outward_x,
                    outward_y,
                    normal_delta_mm,
                    tangent_delta_mm
                };
            }
        }
    };

    for (size_t slice_idx = 0; slice_idx < layer->lslices.size(); ++slice_idx) {
        const ExPolygon &slice = layer->lslices[slice_idx];
        if (slice.empty())
            continue;

        if (has_slice_bboxes && layer->lslices_bboxes[slice_idx].defined &&
            bbox_distance_sq_to_point_for_gcode(layer->lslices_bboxes[slice_idx], query_point) > max_search_sq)
            continue;

        consider_polygon(slice.contour);
        for (const Polygon &hole : slice.holes)
            consider_polygon(hole);
    }

    return best;
}

static void choose_segment_outward_normal_from_reference_for_gcode(double reference_x,
                                                                   double reference_y,
                                                                   double n0x,
                                                                   double n0y,
                                                                   double n1x,
                                                                   double n1y,
                                                                   double &outward_x,
                                                                   double &outward_y)
{
    const double dot0 = n0x * reference_x + n0y * reference_y;
    const double dot1 = n1x * reference_x + n1y * reference_y;
    if (dot1 > dot0) {
        outward_x = n1x;
        outward_y = n1y;
    } else {
        outward_x = n0x;
        outward_y = n0y;
    }
}

static void resolve_segment_shift_outward_normal_for_gcode(const Layer *layer,
                                                           const Point &mid_point,
                                                           double       dx,
                                                           double       dy,
                                                           double       len,
                                                           double       fallback_reference_x,
                                                           double       fallback_reference_y,
                                                           double      &outward_x,
                                                           double      &outward_y)
{
    const double n0x = dy / len;
    const double n0y = -dx / len;
    const double n1x = -n0x;
    const double n1y = -n0y;

    Point nearest_boundary_point;
    if (find_nearest_layer_slice_boundary_point_for_gcode(layer, mid_point, nearest_boundary_point)) {
        const double boundary_x = double(nearest_boundary_point.x()) - double(mid_point.x());
        const double boundary_y = double(nearest_boundary_point.y()) - double(mid_point.y());
        const double boundary_len2 = boundary_x * boundary_x + boundary_y * boundary_y;
        if (boundary_len2 > 1e-6) {
            const double boundary_len = std::sqrt(boundary_len2);
            const double normal_alignment = std::abs(n0x * boundary_x + n0y * boundary_y) / std::max(boundary_len, 1e-9);
            if (normal_alignment >= 0.25) {
                choose_segment_outward_normal_from_reference_for_gcode(boundary_x, boundary_y, n0x, n0y, n1x, n1y, outward_x, outward_y);
                return;
            }
        }
    }

    const double fallback_len2 = fallback_reference_x * fallback_reference_x + fallback_reference_y * fallback_reference_y;
    if (fallback_len2 > 1e-6) {
        choose_segment_outward_normal_from_reference_for_gcode(fallback_reference_x, fallback_reference_y, n0x, n0y, n1x, n1y, outward_x, outward_y);
        return;
    }

    outward_x = n0x;
    outward_y = n0y;
}

static bool clamped_shift_coord_for_gcode(double direction_component, double shift_scaled, coord_t max_abs_shift, coord_t &out)
{
    if (!std::isfinite(direction_component) || !std::isfinite(shift_scaled))
        return false;

    const double raw = direction_component * shift_scaled;
    if (!std::isfinite(raw))
        return false;

    const double max_shift = std::max(0.0, double(max_abs_shift));
    double clamped = std::clamp(raw, -max_shift, max_shift);
    clamped = std::clamp(clamped, double(std::numeric_limits<coord_t>::lowest()), double(std::numeric_limits<coord_t>::max()));

    out = coord_t(std::llround(clamped));
    return true;
}

static bool is_reasonable_quantized_gcode_point_for_gcode(const Vec2d &p)
{
    constexpr double k_abs_coord_limit_mm = 10000.0;
    return std::isfinite(p(0)) && std::isfinite(p(1)) &&
           std::abs(p(0)) <= k_abs_coord_limit_mm &&
           std::abs(p(1)) <= k_abs_coord_limit_mm;
}

static float repeated_rotation_progress_for_gcode(float progress01, float repeats, bool reverse_repeats)
{
    const float p = clamp01f_for_gcode(progress01);
    const float r = std::max(1.f, repeats);
    if (r <= 1.f + EPSILON)
        return p;

    float repeated_pos = p * r;
    int segment_idx = int(std::floor(repeated_pos));
    float local = repeated_pos - float(segment_idx);

    if (p >= 1.f - EPSILON) {
        segment_idx = std::max(0, int(std::ceil(r)) - 1);
        local = 1.f;
    }

    if (reverse_repeats && (segment_idx % 2 == 1))
        local = 1.f - local;
    return clamp01f_for_gcode(local);
}

static bool has_explicit_offset_gradient_profile_for_gcode(const TextureMappingZone &zone)
{
    return zone.has_custom_offset_settings();
}

static float overhang_filament_strength_factor_for_gcode(const TextureMappingZone &zone, unsigned int physical_filament_id)
{
    if (physical_filament_id == 0)
        return 1.f;

    const size_t idx = size_t(physical_filament_id - 1);
    if (idx >= zone.filament_strengths_pct.size())
        return 1.f;

    const float strength_pct = zone.filament_strengths_pct[idx];
    if (!std::isfinite(strength_pct))
        return 1.f;

    return std::clamp(strength_pct / 100.f, 0.f, 1.f);
}

static float overhang_filament_minimum_offset_factor_for_gcode(const TextureMappingZone &zone, unsigned int physical_filament_id)
{
    if (physical_filament_id == 0)
        return 0.f;

    const size_t idx = size_t(physical_filament_id - 1);
    if (idx >= zone.filament_minimum_offsets_pct.size())
        return 0.f;

    const float minimum_offset_pct = zone.filament_minimum_offsets_pct[idx];
    if (!std::isfinite(minimum_offset_pct))
        return 0.f;

    return std::clamp(minimum_offset_pct / 100.f, 0.f, 1.f);
}

static std::vector<float> overhang_component_strength_factors_for_gcode(const TextureMappingZone &zone,
                                                                        const std::vector<unsigned int> &component_ids)
{
    std::vector<float> factors;
    factors.reserve(component_ids.size());
    for (const unsigned int id : component_ids)
        factors.emplace_back(overhang_filament_strength_factor_for_gcode(zone, id));
    return factors;
}

static std::vector<float> overhang_component_minimum_offset_factors_for_gcode(const TextureMappingZone &zone,
                                                                              const std::vector<unsigned int> &component_ids)
{
    std::vector<float> factors;
    factors.reserve(component_ids.size());
    for (const unsigned int id : component_ids)
        factors.emplace_back(overhang_filament_minimum_offset_factor_for_gcode(zone, id));
    return factors;
}

struct TransmissionDistanceCalibrationContextForGCode {
    bool               enabled { false };
    int                mode { int(TextureMappingZone::TDCalibrationNone) };
    std::vector<float> own_width_factors;
    std::vector<float> neighbor_opacity_ratios;
};

static bool texture_mapping_component_is_black_for_gcode(size_t                                      component_idx,
                                                         int                                         filament_color_mode,
                                                         const std::vector<std::array<float, 3>>    &component_colors)
{
    switch (std::clamp(filament_color_mode, int(TextureMappingZone::FilamentColorAny), int(TextureMappingZone::FilamentColorRGBKW))) {
    case int(TextureMappingZone::FilamentColorCMYK):
    case int(TextureMappingZone::FilamentColorRGBK):
    case int(TextureMappingZone::FilamentColorCMYKW):
    case int(TextureMappingZone::FilamentColorRGBKW):
        if (component_idx == 3)
            return true;
        break;
    case int(TextureMappingZone::FilamentColorBW):
        if (component_idx == 0)
            return true;
        break;
    default:
        break;
    }

    if (component_idx >= component_colors.size())
        return false;

    const std::array<float, 3> &c = component_colors[component_idx];
    const float max_channel = std::max({ c[0], c[1], c[2] });
    const float luminance = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    return max_channel <= 0.18f && luminance <= 0.12f;
}

static bool overhang_filament_explicit_transmission_distance_for_gcode(const TextureMappingZone &zone,
                                                                       unsigned int              physical_filament_id,
                                                                       float                    &td_mm)
{
    if (physical_filament_id == 0)
        return false;

    const size_t idx = size_t(physical_filament_id - 1);
    if (idx >= zone.filament_transmission_distances_mm.size())
        return false;

    const float value = zone.filament_transmission_distances_mm[idx];
    if (!std::isfinite(value) || value <= 0.f)
        return false;

    td_mm = std::clamp(value, 0.01f, 50.f);
    return true;
}

static float transmission_distance_reference_for_gcode(bool is_black)
{
    return is_black ? 0.1f : 3.f;
}

static float transmission_distance_opacity_for_gcode(float td_mm, float path_extension_mm)
{
    constexpr float surface_scatter = 0.50f;
    constexpr float surface_depth_mm = 0.32f;
    const float safe_td = std::clamp(td_mm, 0.01f, 50.f);
    const float path_mm = std::max(0.f, surface_depth_mm + std::max(0.f, path_extension_mm));
    const float opacity = surface_scatter + (1.f - surface_scatter) * (1.f - std::exp(-path_mm / safe_td));
    return std::clamp(opacity, 1e-4f, 1.f);
}

static TransmissionDistanceCalibrationContextForGCode transmission_distance_calibration_context_for_gcode(
    const TextureMappingZone                         &zone,
    const std::vector<unsigned int>                  &component_ids,
    const std::vector<std::array<float, 3>>          &component_colors,
    int                                               filament_color_mode)
{
    TransmissionDistanceCalibrationContextForGCode context;
    context.mode = std::clamp(zone.transmission_distance_calibration_mode,
                              int(TextureMappingZone::TDCalibrationNone),
                              int(TextureMappingZone::TDCalibrationCalibratedNearestMeasuredSample));
    if (context.mode == int(TextureMappingZone::TDCalibrationNone) ||
        context.mode == int(TextureMappingZone::TDCalibrationCalibratedNearestMeasuredSample) ||
        component_ids.empty())
        return context;

    std::vector<float> explicit_tds(component_ids.size(), 0.f);
    bool has_active_explicit_td = false;
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        float td_mm = 0.f;
        if (overhang_filament_explicit_transmission_distance_for_gcode(zone, component_ids[idx], td_mm)) {
            explicit_tds[idx] = td_mm;
            has_active_explicit_td = true;
        }
    }
    if (!has_active_explicit_td)
        return context;

    const bool neighbor_mode = context.mode == int(TextureMappingZone::TDCalibrationNeighbor);
    const float path_extension_mm = neighbor_mode ? 0.16f : 0.12f;
    const float own_power = neighbor_mode ? 0.25f : 0.35f;
    context.own_width_factors.assign(component_ids.size(), 1.f);
    context.neighbor_opacity_ratios.assign(component_ids.size(), 1.f);
    context.enabled = true;

    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        const bool is_black = texture_mapping_component_is_black_for_gcode(idx, filament_color_mode, component_colors);
        const float reference_td_mm = transmission_distance_reference_for_gcode(is_black);
        const float actual_td_mm = explicit_tds[idx] > 0.f ? explicit_tds[idx] : reference_td_mm;
        const float actual_opacity = transmission_distance_opacity_for_gcode(actual_td_mm, path_extension_mm);
        const float reference_opacity = transmission_distance_opacity_for_gcode(reference_td_mm, path_extension_mm);
        context.own_width_factors[idx] =
            std::clamp(std::pow(reference_opacity / std::max(actual_opacity, 1e-4f), own_power), 0.25f, 2.f);
        context.neighbor_opacity_ratios[idx] =
            std::clamp(actual_opacity / std::max(reference_opacity, 1e-4f), 0.25f, 4.f);
    }

    return context;
}

static float transmission_distance_width_factor_for_gcode(const TransmissionDistanceCalibrationContextForGCode &context,
                                                          size_t                                                active_component_idx,
                                                          size_t                                                previous_component_idx)
{
    if (!context.enabled || active_component_idx >= context.own_width_factors.size())
        return 1.f;

    float factor = context.own_width_factors[active_component_idx];
    if (context.mode == int(TextureMappingZone::TDCalibrationNeighbor) &&
        previous_component_idx < context.neighbor_opacity_ratios.size())
        factor *= std::pow(context.neighbor_opacity_ratios[previous_component_idx], 0.20f);

    return std::clamp(factor, 0.25f, 2.f);
}

static float variable_width_delta_for_overhang_range_for_gcode(float inset_strength,
                                                               float max_width_delta_limit_mm,
                                                               float minimum_offset_factor,
                                                               float strength_factor,
                                                               float transmission_distance_width_factor)
{
    if (!std::isfinite(max_width_delta_limit_mm) || max_width_delta_limit_mm <= 0.f)
        return 0.f;

    const float desired_width_factor =
        std::clamp((1.f - std::clamp(inset_strength, 0.f, 1.f)) *
                       std::clamp(transmission_distance_width_factor, 0.f, 2.f),
                   0.f,
                   1.f);
    const float min_width_factor = std::clamp(minimum_offset_factor, 0.f, 1.f);
    const float adjusted_width_factor =
        min_width_factor + desired_width_factor * std::clamp(strength_factor, 0.f, 1.f) * (1.f - min_width_factor);

    return std::clamp(max_width_delta_limit_mm * (1.f - adjusted_width_factor), 0.f, max_width_delta_limit_mm);
}

static float nonlinear_visibility_width_factor_for_gcode(float desired_width_factor,
                                                         float layer_height_mm,
                                                         float stair_step_mm,
                                                         float max_width_delta_limit_mm)
{
    const float r = clamp01f_for_gcode(desired_width_factor);
    if (!std::isfinite(layer_height_mm) ||
        !std::isfinite(stair_step_mm) ||
        !std::isfinite(max_width_delta_limit_mm) ||
        layer_height_mm <= EPSILON ||
        max_width_delta_limit_mm <= EPSILON)
        return r;

    if (r <= EPSILON || r >= 1.f - EPSILON)
        return r;
    if (std::abs(r - 0.5f) <= 1e-5f)
        return 0.5f;

    const float h = std::max(0.01f, layer_height_mm);
    const float d = std::max(0.f, stair_step_mm);
    const float diag = std::hypot(h, d);
    if (!std::isfinite(diag) || diag <= EPSILON)
        return r;

    const float symmetric_r = std::min(r, 1.f - r);
    const float direction = r >= 0.5f ? 1.f : -1.f;
    const float sin_n = std::clamp(d / diag, 0.f, 1.f);
    const float cos_n = std::clamp(h / diag, 1e-4f, 1.f);
    const float sin_cos = sin_n * cos_n;
    float offset_mm = 0.f;

    if (sin_cos > 1e-5f) {
        offset_mm = (0.5f - symmetric_r) * h / sin_cos;
        if (2.f * std::abs(offset_mm) <= d + EPSILON)
            return std::clamp(0.5f + direction * offset_mm / max_width_delta_limit_mm, 0.f, 1.f);
    }

    const float cx = std::clamp(1.f - std::sqrt(2.f) / 2.f, 0.f, 0.95f);
    const float c = (1.f - cx) * (1.f - cx);
    const float safe_cos = std::max(cos_n, 1e-4f);
    const float tan_n = sin_n / safe_cos;
    const float a = -0.5f * c * (1.f + sin_n) / std::max(h * diag, 1e-6f);
    const float b = 0.5f * (c * tan_n * (1.f + sin_n) + 2.f * cos_n * (cx - 1.f)) / std::max(diag, 1e-6f);
    const float q = c * 0.25f * tan_n * (1.f + sin_n) - cx * cos_n;
    const float cc = 0.5f - 0.5f * cos_n * q - symmetric_r;
    const float det = std::max(0.f, b * b - 4.f * a * cc);
    if (std::abs(a) > 1e-8f) {
        offset_mm = (-b - std::sqrt(det)) / (2.f * a);
        if (!std::isfinite(offset_mm) || offset_mm < 0.f)
            offset_mm = (-b + std::sqrt(det)) / (2.f * a);
    }
    if (!std::isfinite(offset_mm) || offset_mm < 0.f) {
        if (sin_cos > 1e-5f)
            offset_mm = (0.5f - symmetric_r) * h / sin_cos;
        else
            offset_mm = max_width_delta_limit_mm;
    }

    return std::clamp(0.5f + direction * offset_mm / max_width_delta_limit_mm, 0.f, 1.f);
}

static float variable_width_delta_for_visibility_range_for_gcode(float inset_strength,
                                                                 float max_width_delta_limit_mm,
                                                                 float minimum_offset_factor,
                                                                 float strength_factor,
                                                                 float transmission_distance_width_factor,
                                                                 bool  nonlinear_offset_adjustment,
                                                                 float layer_height_mm,
                                                                 float stair_step_mm)
{
    if (!std::isfinite(max_width_delta_limit_mm) || max_width_delta_limit_mm <= 0.f)
        return 0.f;

    float desired_width_factor =
        std::clamp((1.f - std::clamp(inset_strength, 0.f, 1.f)) *
                       std::clamp(transmission_distance_width_factor, 0.f, 2.f),
                   0.f,
                   1.f);
    if (nonlinear_offset_adjustment)
        desired_width_factor = nonlinear_visibility_width_factor_for_gcode(desired_width_factor,
                                                                           layer_height_mm,
                                                                           stair_step_mm,
                                                                           max_width_delta_limit_mm);

    const float min_width_factor = std::clamp(minimum_offset_factor, 0.f, 1.f);
    const float adjusted_width_factor =
        min_width_factor + desired_width_factor * std::clamp(strength_factor, 0.f, 1.f) * (1.f - min_width_factor);

    return std::clamp(max_width_delta_limit_mm * (1.f - adjusted_width_factor), 0.f, max_width_delta_limit_mm);
}

static float local_surface_stair_step_distance_for_gcode(const Layer *layer,
                                                         const Point &mid_point,
                                                         double       outward_x,
                                                         double       outward_y,
                                                         float        base_outer_width_mm,
                                                         float        max_allowed_distance_mm)
{
    if (layer == nullptr || !std::isfinite(outward_x) || !std::isfinite(outward_y))
        return std::numeric_limits<float>::quiet_NaN();

    const double half_width_scaled = scale_(0.5 * double(std::max(0.01f, base_outer_width_mm)));
    const Point current_base_edge(
        coord_t(std::llround(double(mid_point.x()) + outward_x * half_width_scaled)),
        coord_t(std::llround(double(mid_point.y()) + outward_y * half_width_scaled)));
    const float max_local_edge_tangent_delta_mm = std::max(1.0f, base_outer_width_mm * 2.f);
    const float max_local_edge_normal_delta_mm =
        std::max(2.0f, base_outer_width_mm * 4.f + 2.f * std::max(0.f, max_allowed_distance_mm));
    float best_distance_mm = std::numeric_limits<float>::quiet_NaN();

    auto consider_adjacent_layer = [&](const Layer *adjacent_layer) {
        if (adjacent_layer == nullptr)
            return;

        Point adjacent_base_edge;
        if (!find_nearest_layer_slice_boundary_point_for_gcode(adjacent_layer, current_base_edge, adjacent_base_edge))
            return;

        const double edge_delta_x = double(adjacent_base_edge.x()) - double(current_base_edge.x());
        const double edge_delta_y = double(adjacent_base_edge.y()) - double(current_base_edge.y());
        const double edge_distance_scaled = std::hypot(edge_delta_x, edge_delta_y);
        const double edge_normal_delta_scaled = edge_delta_x * outward_x + edge_delta_y * outward_y;
        const double edge_tangent_delta_scaled_sq =
            std::max(0.0, edge_distance_scaled * edge_distance_scaled - edge_normal_delta_scaled * edge_normal_delta_scaled);
        const float edge_tangent_delta_mm = unscale<float>(std::sqrt(edge_tangent_delta_scaled_sq));
        if (!std::isfinite(edge_tangent_delta_mm) || edge_tangent_delta_mm > max_local_edge_tangent_delta_mm)
            return;

        const float edge_normal_delta_mm = std::abs(unscale<float>(edge_normal_delta_scaled));
        if (!std::isfinite(edge_normal_delta_mm) || edge_normal_delta_mm > max_local_edge_normal_delta_mm)
            return;

        if (!std::isfinite(best_distance_mm) || edge_normal_delta_mm < best_distance_mm)
            best_distance_mm = edge_normal_delta_mm;
    };

    consider_adjacent_layer(layer->upper_layer);
    consider_adjacent_layer(layer->lower_layer);
    return best_distance_mm;
}

static bool is_horizontal_overhang_gradient_row_for_gcode(const TextureMappingZone &zone)
{
    return zone.enabled && !zone.deleted && !zone.uses_perimeter_path_modulation() && (zone.is_surface_gradient() || zone.is_image_texture());
}

static bool is_vertex_color_match_overhang_row_for_gcode(const TextureMappingZone &zone)
{
    return zone.enabled && !zone.deleted && !zone.uses_perimeter_path_modulation() && zone.is_image_texture();
}

static bool is_2d_offset_gradient_row_for_gcode(const TextureMappingZone &zone)
{
    return zone.enabled && !zone.deleted && !zone.uses_perimeter_path_modulation() && zone.is_2d_gradient();
}

static bool is_surface_offset_gradient_row_for_gcode(const TextureMappingZone &zone)
{
    return zone.enabled && !zone.deleted && !zone.uses_perimeter_path_modulation() && zone.is_surface_gradient();
}

static std::array<float, 4> unpack_rgba_u32(uint32_t packed_rgba)
{
    const float r = float((packed_rgba >> 24) & 0xFFu) / 255.f;
    const float g = float((packed_rgba >> 16) & 0xFFu) / 255.f;
    const float b = float((packed_rgba >> 8) & 0xFFu) / 255.f;
    const float a = float(packed_rgba & 0xFFu) / 255.f;
    return { clamp01f_for_gcode(r), clamp01f_for_gcode(g), clamp01f_for_gcode(b), clamp01f_for_gcode(a) };
}

static constexpr const char *TEXTURE_MAPPING_BACKGROUND_COLOR_CONFIG_KEY = "texture_mapping_background_color";

static int texture_mapping_color_hex_digit_for_gcode(char ch)
{
    return ch >= '0' && ch <= '9' ? ch - '0' :
           ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
           ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : -1;
}

static std::optional<std::array<float, 4>> parse_texture_mapping_color_hex_for_gcode(const std::string &text)
{
    if (text.empty())
        return std::nullopt;

    const size_t hash_pos = text.find('#');
    const size_t start = hash_pos == std::string::npos ? 0 : hash_pos + 1;
    if (start + 6 > text.size())
        return std::nullopt;

    uint32_t packed = 0;
    for (size_t idx = 0; idx < 6; ++idx) {
        const int value = texture_mapping_color_hex_digit_for_gcode(text[start + idx]);
        if (value < 0)
            return std::nullopt;
        packed = (packed << 4) | uint32_t(value);
    }

    uint32_t alpha = 255;
    if (start + 8 <= text.size()) {
        alpha = 0;
        for (size_t idx = 6; idx < 8; ++idx) {
            const int value = texture_mapping_color_hex_digit_for_gcode(text[start + idx]);
            if (value < 0)
                return std::nullopt;
            alpha = (alpha << 4) | uint32_t(value);
        }
    }

    return std::array<float, 4> {
        clamp01f_for_gcode(float((packed >> 16) & 0xFFu) / 255.f),
        clamp01f_for_gcode(float((packed >> 8) & 0xFFu) / 255.f),
        clamp01f_for_gcode(float(packed & 0xFFu) / 255.f),
        clamp01f_for_gcode(float(alpha & 0xFFu) / 255.f)
    };
}

static std::optional<std::array<float, 4>> texture_mapping_background_color_from_config_for_gcode(const ModelConfigObject &config)
{
    if (!config.has(TEXTURE_MAPPING_BACKGROUND_COLOR_CONFIG_KEY))
        return std::nullopt;

    const ConfigOptionString *opt = dynamic_cast<const ConfigOptionString *>(config.option(TEXTURE_MAPPING_BACKGROUND_COLOR_CONFIG_KEY));
    if (opt == nullptr)
        return std::nullopt;

    std::optional<std::array<float, 4>> color = parse_texture_mapping_color_hex_for_gcode(opt->value);
    if (color)
        (*color)[3] = 1.f;
    return color;
}

static std::optional<std::array<float, 4>> texture_mapping_background_color_from_metadata_for_gcode(const ColorFacetsAnnotation &annotation)
{
    const std::string &metadata = annotation.metadata_json();
    const std::string key = "\"background_color\":\"#";
    const size_t start = metadata.find(key);
    if (start == std::string::npos || start + key.size() + 8 > metadata.size())
        return std::nullopt;

    std::optional<std::array<float, 4>> color =
        parse_texture_mapping_color_hex_for_gcode(metadata.substr(start + key.size() - 1, 9));
    if (color)
        (*color)[3] = 1.f;
    return color;
}

static std::array<float, 4> texture_mapping_background_color_for_gcode(const ModelVolume &volume)
{
    if (std::optional<std::array<float, 4>> color = texture_mapping_background_color_from_config_for_gcode(volume.config))
        return *color;
    if (volume.get_object() != nullptr) {
        if (std::optional<std::array<float, 4>> color = texture_mapping_background_color_from_config_for_gcode(volume.get_object()->config))
            return *color;
    }
    if (std::optional<std::array<float, 4>> color = texture_mapping_background_color_from_metadata_for_gcode(volume.texture_mapping_color_facets))
        return *color;
    return { 1.f, 1.f, 1.f, 1.f };
}

static std::array<float, 4> composite_rgba_over_background_for_gcode(const std::array<float, 4> &rgba,
                                                                     const std::array<float, 4> &background)
{
    const float alpha = clamp01f_for_gcode(rgba[3]);
    return {
        clamp01f_for_gcode(rgba[0] * alpha + background[0] * (1.f - alpha)),
        clamp01f_for_gcode(rgba[1] * alpha + background[1] * (1.f - alpha)),
        clamp01f_for_gcode(rgba[2] * alpha + background[2] * (1.f - alpha)),
        1.f
    };
}

static float srgb_to_linear_component_for_gcode(float value)
{
    const float x = clamp01f_for_gcode(value);
    return x <= 0.04045f ? x / 12.92f : std::pow((x + 0.055f) / 1.055f, 2.4f);
}

static std::array<float, 3> oklab_from_srgb_for_gcode(const std::array<float, 3> &rgb)
{
    const float r = srgb_to_linear_component_for_gcode(rgb[0]);
    const float g = srgb_to_linear_component_for_gcode(rgb[1]);
    const float b = srgb_to_linear_component_for_gcode(rgb[2]);

    const float l = std::cbrt(0.4122214708f * r + 0.5363325363f * g + 0.0514459929f * b);
    const float m = std::cbrt(0.2119034982f * r + 0.6806995451f * g + 0.1073969566f * b);
    const float s = std::cbrt(0.0883024619f * r + 0.2817188376f * g + 0.6299787005f * b);

    return {
        0.2104542553f * l + 0.7936177850f * m - 0.0040720468f * s,
        1.9779984951f * l - 2.4285922050f * m + 0.4505937099f * s,
        0.0259040371f * l + 0.7827717662f * m - 0.8086757660f * s
    };
}

static float generic_solver_oklab_chroma_factor_for_gcode(const std::array<float, 3> &target_oklab)
{
    const float chroma = std::hypot(target_oklab[1], target_oklab[2]);
    return std::clamp((chroma - 0.015f) / 0.13f, 0.f, 1.f);
}

static std::array<float, 3> generic_solver_oklab_axis_weights_for_gcode(const std::array<float, 3> &target_oklab)
{
    const float chroma_factor = generic_solver_oklab_chroma_factor_for_gcode(target_oklab);
    return {
        1.f + (0.25f - 1.f) * chroma_factor,
        1.25f + (8.f - 1.25f) * chroma_factor,
        1.25f + (8.f - 1.25f) * chroma_factor
    };
}

static std::array<float, 3> generic_solver_perceptual_axis_weights_for_gcode(const std::array<float, 3> &target_oklab,
                                                                             int                         generic_solver_mode)
{
    const int effective_solver_mode = TextureMappingZone::effective_generic_solver_mode(generic_solver_mode);
    std::array<float, 3> weights = generic_solver_oklab_axis_weights_for_gcode(target_oklab);
    if (effective_solver_mode == int(TextureMappingZone::GenericSolverOklabSoftCap4Dark4)) {
        weights[0] = std::max(weights[0], 1.f);
        weights[1] = std::min(weights[1], 4.f);
        weights[2] = std::min(weights[2], 4.f);
    }
    return weights;
}

static bool generic_solver_mode_is_perceptual_for_gcode(int generic_solver_mode)
{
    return TextureMappingZone::effective_generic_solver_mode(generic_solver_mode) != int(TextureMappingZone::GenericSolverRGB);
}

static std::string generic_mix_candidate_cache_key_for_gcode(const std::vector<std::array<float, 3>> &component_colors)
{
    std::ostringstream key;
    key << component_colors.size();
    for (const std::array<float, 3> &color : component_colors) {
        key << '|'
            << int(std::lround(clamp01f_for_gcode(color[0]) * 65535.f)) << ','
            << int(std::lround(clamp01f_for_gcode(color[1]) * 65535.f)) << ','
            << int(std::lround(clamp01f_for_gcode(color[2]) * 65535.f));
    }
    return key.str();
}

static int generic_mix_total_units_for_component_count_for_gcode(size_t component_count)
{
    return component_count <= 4 ? 40 : (component_count == 5 ? 24 : (component_count == 6 ? 20 : 12));
}

static size_t generic_mix_candidate_count_for_gcode(size_t component_count, int total_units)
{
    if (component_count == 0)
        return 0;

    const size_t n = size_t(total_units) + component_count - 1;
    size_t k = component_count - 1;
    k = std::min(k, n - k);

    size_t result = 1;
    for (size_t idx = 1; idx <= k; ++idx)
        result = (result * (n - k + idx)) / idx;
    return result;
}

static int build_generic_mix_candidate_kd_tree_for_gcode(
    const std::vector<float>                         &coords,
    std::vector<GCodeGenericMixCandidateSet::KdNode> &nodes,
    std::vector<uint32_t>                            &indices,
    size_t                                            begin,
    size_t                                            end,
    uint8_t                                           axis)
{
    if (begin >= end)
        return -1;

    const size_t mid = begin + (end - begin) / 2;
    auto axis_value = [&coords, axis](uint32_t candidate_idx) {
        return coords[size_t(candidate_idx) * 3 + size_t(axis)];
    };
    std::nth_element(indices.begin() + begin,
                     indices.begin() + mid,
                     indices.begin() + end,
                     [&axis_value](uint32_t lhs, uint32_t rhs) {
                         return axis_value(lhs) < axis_value(rhs);
                     });

    const int node_idx = int(nodes.size());
    GCodeGenericMixCandidateSet::KdNode node;
    node.candidate_idx = indices[mid];
    node.axis = axis;
    nodes.emplace_back(node);

    const uint8_t next_axis = uint8_t((axis + 1) % 3);
    const int left = build_generic_mix_candidate_kd_tree_for_gcode(coords, nodes, indices, begin, mid, next_axis);
    const int right = build_generic_mix_candidate_kd_tree_for_gcode(coords, nodes, indices, mid + 1, end, next_axis);
    nodes[size_t(node_idx)].left = left;
    nodes[size_t(node_idx)].right = right;
    return node_idx;
}

static int build_generic_mix_candidate_kd_tree_for_gcode(
    const std::vector<float>                         &coords,
    std::vector<GCodeGenericMixCandidateSet::KdNode> &nodes)
{
    const size_t candidate_count = coords.size() / 3;
    nodes.clear();
    if (candidate_count == 0)
        return -1;

    std::vector<uint32_t> indices(candidate_count, 0);
    for (size_t idx = 0; idx < candidate_count; ++idx)
        indices[idx] = uint32_t(idx);

    nodes.reserve(candidate_count);
    return build_generic_mix_candidate_kd_tree_for_gcode(coords,
                                                         nodes,
                                                         indices,
                                                         0,
                                                         candidate_count,
                                                         uint8_t(0));
}

static void build_generic_mix_candidate_kd_tree_for_gcode(GCodeGenericMixCandidateSet &candidates)
{
    candidates.kd_root = build_generic_mix_candidate_kd_tree_for_gcode(candidates.rgbs, candidates.kd_nodes);
    if (candidates.perceptual_coords.size() == candidates.rgbs.size()) {
        candidates.perceptual_kd_root =
            build_generic_mix_candidate_kd_tree_for_gcode(candidates.perceptual_coords, candidates.perceptual_kd_nodes);
    } else {
        candidates.perceptual_kd_nodes.clear();
        candidates.perceptual_kd_root = -1;
    }
}

static GCodeGenericMixCandidateSet build_generic_mix_candidates_for_gcode(
    const std::vector<std::array<float, 3>> &component_colors,
    int                                      generic_solver_mix_model)
{
    return build_color_solver_candidates(component_colors, color_solver_mix_model_from_index(generic_solver_mix_model));
}

static const GCodeGenericMixCandidateSet &generic_mix_candidates_for_gcode(
    std::map<std::string, GCodeGenericMixCandidateSet> &cache,
    const std::vector<std::array<float, 3>>             &component_colors,
    int                                                  generic_solver_mix_model)
{
    const std::string key =
        color_solver_candidate_cache_key(component_colors, color_solver_mix_model_from_index(generic_solver_mix_model));
    auto it = cache.find(key);
    if (it != cache.end())
        return it->second;
    return cache.emplace(key, build_generic_mix_candidates_for_gcode(component_colors, generic_solver_mix_model)).first->second;
}

struct GCodeGenericMixNearestResult {
    size_t best_idx { size_t(-1) };
    size_t second_idx { size_t(-1) };
    float  best_error { std::numeric_limits<float>::max() };
    float  second_error { std::numeric_limits<float>::max() };
};

static void update_generic_mix_nearest_result_for_gcode(GCodeGenericMixNearestResult &result,
                                                        size_t                        candidate_idx,
                                                        float                         error)
{
    if (candidate_idx == result.best_idx || candidate_idx == result.second_idx)
        return;

    if (error < result.best_error) {
        result.second_error = result.best_error;
        result.second_idx = result.best_idx;
        result.best_error = error;
        result.best_idx = candidate_idx;
    } else if (error < result.second_error) {
        result.second_error = error;
        result.second_idx = candidate_idx;
    }
}

static float generic_mix_candidate_error_for_gcode(const GCodeGenericMixCandidateSet &candidates,
                                                   size_t                             candidate_idx,
                                                   const std::array<float, 3>        &target_rgb)
{
    const size_t rgb_idx = candidate_idx * 3;
    const float dr = candidates.rgbs[rgb_idx + 0] - target_rgb[0];
    const float dg = candidates.rgbs[rgb_idx + 1] - target_rgb[1];
    const float db = candidates.rgbs[rgb_idx + 2] - target_rgb[2];
    return dr * dr + dg * dg + db * db;
}

static GCodeGenericMixNearestResult nearest_generic_mix_candidates_linear_for_gcode(
    const GCodeGenericMixCandidateSet &candidates,
    const std::array<float, 3>        &target_rgb)
{
    GCodeGenericMixNearestResult result;
    const size_t candidate_count = candidates.rgbs.size() / 3;
    for (size_t candidate_idx = 0; candidate_idx < candidate_count; ++candidate_idx) {
        update_generic_mix_nearest_result_for_gcode(result,
                                                    candidate_idx,
                                                    generic_mix_candidate_error_for_gcode(candidates,
                                                                                         candidate_idx,
                                                                                         target_rgb));
    }
    return result;
}

static void query_generic_mix_candidate_kd_tree_for_gcode(const GCodeGenericMixCandidateSet &candidates,
                                                          const std::array<float, 3>        &target_rgb,
                                                          int                                node_idx,
                                                          GCodeGenericMixNearestResult      &result)
{
    if (node_idx < 0 || size_t(node_idx) >= candidates.kd_nodes.size())
        return;

    const size_t candidate_count = candidates.rgbs.size() / 3;
    const GCodeGenericMixCandidateSet::KdNode &node = candidates.kd_nodes[size_t(node_idx)];
    if (size_t(node.candidate_idx) >= candidate_count) {
        query_generic_mix_candidate_kd_tree_for_gcode(candidates, target_rgb, node.left, result);
        query_generic_mix_candidate_kd_tree_for_gcode(candidates, target_rgb, node.right, result);
        return;
    }

    update_generic_mix_nearest_result_for_gcode(result,
                                                size_t(node.candidate_idx),
                                                generic_mix_candidate_error_for_gcode(candidates,
                                                                                     size_t(node.candidate_idx),
                                                                                     target_rgb));

    const size_t rgb_idx = size_t(node.candidate_idx) * 3;
    const size_t axis = std::min<size_t>(node.axis, 2);
    const float split_delta = target_rgb[axis] - candidates.rgbs[rgb_idx + axis];
    const int near_node = split_delta <= 0.f ? node.left : node.right;
    const int far_node = split_delta <= 0.f ? node.right : node.left;

    query_generic_mix_candidate_kd_tree_for_gcode(candidates, target_rgb, near_node, result);
    if (split_delta * split_delta <= result.second_error)
        query_generic_mix_candidate_kd_tree_for_gcode(candidates, target_rgb, far_node, result);
}

static GCodeGenericMixNearestResult nearest_generic_mix_candidates_for_gcode(
    const GCodeGenericMixCandidateSet &candidates,
    const std::array<float, 3>        &target_rgb)
{
    GCodeGenericMixNearestResult result;
    const size_t candidate_count = candidates.rgbs.size() / 3;
    if (candidates.kd_root >= 0 && !candidates.kd_nodes.empty())
        query_generic_mix_candidate_kd_tree_for_gcode(candidates, target_rgb, candidates.kd_root, result);
    if (result.best_idx >= candidate_count)
        result = nearest_generic_mix_candidates_linear_for_gcode(candidates, target_rgb);
    return result;
}

static float generic_mix_candidate_perceptual_error_for_gcode(const GCodeGenericMixCandidateSet &candidates,
                                                              size_t                             candidate_idx,
                                                              const std::array<float, 3>        &target_oklab,
                                                              const std::array<float, 3>        &axis_weights,
                                                              int                                generic_solver_mode)
{
    const size_t coord_idx = candidate_idx * 3;
    const float dl = candidates.perceptual_coords[coord_idx + 0] - target_oklab[0];
    const float da = candidates.perceptual_coords[coord_idx + 1] - target_oklab[1];
    const float db = candidates.perceptual_coords[coord_idx + 2] - target_oklab[2];
    float error = axis_weights[0] * dl * dl + axis_weights[1] * da * da + axis_weights[2] * db * db;
    if (TextureMappingZone::effective_generic_solver_mode(generic_solver_mode) == int(TextureMappingZone::GenericSolverOklabSoftCap4Dark4)) {
        const float under_l = std::max(0.f, target_oklab[0] - candidates.perceptual_coords[coord_idx + 0] - 0.04f);
        error += 4.f * generic_solver_oklab_chroma_factor_for_gcode(target_oklab) * under_l * under_l;
    }
    return error;
}

static GCodeGenericMixNearestResult nearest_generic_mix_candidates_perceptual_linear_for_gcode(
    const GCodeGenericMixCandidateSet &candidates,
    const std::array<float, 3>        &target_oklab,
    const std::array<float, 3>        &axis_weights,
    int                                generic_solver_mode)
{
    GCodeGenericMixNearestResult result;
    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    for (size_t candidate_idx = 0; candidate_idx < candidate_count; ++candidate_idx)
        update_generic_mix_nearest_result_for_gcode(
            result,
            candidate_idx,
            generic_mix_candidate_perceptual_error_for_gcode(candidates, candidate_idx, target_oklab, axis_weights, generic_solver_mode));
    return result;
}

static void query_generic_mix_candidate_perceptual_kd_tree_for_gcode(const GCodeGenericMixCandidateSet &candidates,
                                                                     const std::array<float, 3>        &target_oklab,
                                                                     const std::array<float, 3>        &axis_weights,
                                                                     int                                generic_solver_mode,
                                                                     int                                node_idx,
                                                                     GCodeGenericMixNearestResult      &result)
{
    if (node_idx < 0 || size_t(node_idx) >= candidates.perceptual_kd_nodes.size())
        return;

    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    const GCodeGenericMixCandidateSet::KdNode &node = candidates.perceptual_kd_nodes[size_t(node_idx)];
    if (size_t(node.candidate_idx) >= candidate_count) {
        query_generic_mix_candidate_perceptual_kd_tree_for_gcode(candidates, target_oklab, axis_weights, generic_solver_mode, node.left, result);
        query_generic_mix_candidate_perceptual_kd_tree_for_gcode(candidates, target_oklab, axis_weights, generic_solver_mode, node.right, result);
        return;
    }

    update_generic_mix_nearest_result_for_gcode(
        result,
        size_t(node.candidate_idx),
        generic_mix_candidate_perceptual_error_for_gcode(candidates, size_t(node.candidate_idx), target_oklab, axis_weights, generic_solver_mode));

    const size_t coord_idx = size_t(node.candidate_idx) * 3;
    const size_t axis = std::min<size_t>(node.axis, 2);
    const float split_delta = target_oklab[axis] - candidates.perceptual_coords[coord_idx + axis];
    const int near_node = split_delta <= 0.f ? node.left : node.right;
    const int far_node = split_delta <= 0.f ? node.right : node.left;

    query_generic_mix_candidate_perceptual_kd_tree_for_gcode(candidates, target_oklab, axis_weights, generic_solver_mode, near_node, result);
    if (axis_weights[axis] * split_delta * split_delta <= result.second_error)
        query_generic_mix_candidate_perceptual_kd_tree_for_gcode(candidates, target_oklab, axis_weights, generic_solver_mode, far_node, result);
}

static GCodeGenericMixNearestResult nearest_generic_mix_candidates_perceptual_for_gcode(
    const GCodeGenericMixCandidateSet &candidates,
    const std::array<float, 3>        &target_rgb,
    int                                generic_solver_mode)
{
    GCodeGenericMixNearestResult result;
    const size_t candidate_count = candidates.perceptual_coords.size() / 3;
    if (candidate_count == 0 || candidates.perceptual_coords.size() != candidates.rgbs.size())
        return result;

    const std::array<float, 3> target_oklab = oklab_from_srgb_for_gcode(target_rgb);
    const std::array<float, 3> axis_weights = generic_solver_perceptual_axis_weights_for_gcode(target_oklab, generic_solver_mode);
    if (candidates.perceptual_kd_root >= 0 && !candidates.perceptual_kd_nodes.empty())
        query_generic_mix_candidate_perceptual_kd_tree_for_gcode(
            candidates, target_oklab, axis_weights, generic_solver_mode, candidates.perceptual_kd_root, result);
    if (result.best_idx >= candidate_count)
        result = nearest_generic_mix_candidates_perceptual_linear_for_gcode(candidates, target_oklab, axis_weights, generic_solver_mode);
    return result;
}

static std::vector<float> best_component_mix_weights_for_target_for_gcode(const GCodeGenericMixCandidateSet &candidates,
                                                                          const std::array<float, 3>        &target_rgb,
                                                                          int                                generic_solver_lookup_mode,
                                                                          int                                generic_solver_mode)
{
    if (candidates.empty())
        return {};

    const size_t candidate_count = candidates.rgbs.size() / 3;
    const int clamped_solver_mode = TextureMappingZone::effective_generic_solver_mode(generic_solver_mode);
    GCodeGenericMixNearestResult nearest =
        generic_solver_mode_is_perceptual_for_gcode(clamped_solver_mode) ?
            nearest_generic_mix_candidates_perceptual_for_gcode(candidates, target_rgb, clamped_solver_mode) :
            nearest_generic_mix_candidates_for_gcode(candidates, target_rgb);
    if (nearest.best_idx >= candidate_count && generic_solver_mode_is_perceptual_for_gcode(clamped_solver_mode))
        nearest = nearest_generic_mix_candidates_for_gcode(candidates, target_rgb);
    if (nearest.best_idx >= candidate_count)
        return {};

    const int clamped_mode = std::clamp(generic_solver_lookup_mode,
                                        int(TextureMappingZone::GenericSolverClosestMix),
                                        int(TextureMappingZone::GenericSolverBlendClosestTwo));
    std::vector<float> weights(candidates.component_count, 0.f);
    const size_t best_weight_idx = nearest.best_idx * candidates.component_count;
    if (clamped_mode == int(TextureMappingZone::GenericSolverClosestMix) ||
        nearest.second_idx >= candidate_count ||
        nearest.best_error <= 1e-12f) {
        for (size_t idx = 0; idx < candidates.component_count; ++idx)
            weights[idx] = candidates.weights[best_weight_idx + idx];
        return weights;
    }

    const size_t second_weight_idx = nearest.second_idx * candidates.component_count;
    const float best_inv = 1.f / std::max(nearest.best_error, 1e-12f);
    const float second_inv = 1.f / std::max(nearest.second_error, 1e-12f);
    const float inv_sum = std::max(best_inv + second_inv, 1e-12f);
    for (size_t idx = 0; idx < candidates.component_count; ++idx)
        weights[idx] = clamp01f_for_gcode((candidates.weights[best_weight_idx + idx] * best_inv +
                                           candidates.weights[second_weight_idx + idx] * second_inv) / inv_sum);
    return weights;
}

static float apply_texture_tone_gamma_for_gcode(float channel, float tone_gamma)
{
    const float safe_channel = clamp01f_for_gcode(channel);
    const float safe_gamma =
        (!std::isfinite(tone_gamma) || tone_gamma <= 0.f) ? 1.f : std::clamp(tone_gamma, 0.5f, 3.f);
    if (std::abs(safe_gamma - 1.f) <= 1e-5f)
        return safe_channel;
    return clamp01f_for_gcode(std::pow(safe_channel, 1.f / safe_gamma));
}

static void apply_filament_overhang_contrast_to_mapped_components_for_gcode(std::vector<float> &component_weights,
                                                                  float               contrast_factor,
                                                                  size_t              mapped_component_count)
{
    const size_t count = std::min(mapped_component_count, component_weights.size());
    if (count == 0)
        return;

    float mean_weight = 0.f;
    for (size_t idx = 0; idx < count; ++idx)
        mean_weight += clamp01f_for_gcode(component_weights[idx]);
    mean_weight /= float(count);

    for (size_t idx = 0; idx < count; ++idx) {
        const float safe_weight = clamp01f_for_gcode(component_weights[idx]);
        component_weights[idx] = clamp01f_for_gcode(mean_weight + (safe_weight - mean_weight) * contrast_factor);
    }
}

static std::array<float, 3> apply_filament_overhang_contrast_to_rgb_for_gcode(const std::array<float, 3> &rgb, float contrast_factor)
{
    const float clamped_contrast = std::clamp(contrast_factor, 0.25f, 3.f);
    if (std::abs(clamped_contrast - 1.f) <= 1e-5f)
        return {clamp01f_for_gcode(rgb[0]), clamp01f_for_gcode(rgb[1]), clamp01f_for_gcode(rgb[2])};

    const float mean = (clamp01f_for_gcode(rgb[0]) + clamp01f_for_gcode(rgb[1]) + clamp01f_for_gcode(rgb[2])) / 3.f;
    return {
        clamp01f_for_gcode(mean + (clamp01f_for_gcode(rgb[0]) - mean) * clamped_contrast),
        clamp01f_for_gcode(mean + (clamp01f_for_gcode(rgb[1]) - mean) * clamped_contrast),
        clamp01f_for_gcode(mean + (clamp01f_for_gcode(rgb[2]) - mean) * clamped_contrast)
    };
}

struct GCodeBinaryDitherCandidate {
    uint32_t             mask { 0 };
    std::array<float, 3> rgb { { 1.f, 1.f, 1.f } };
    std::array<float, 3> oklab { { 1.f, 0.f, 0.f } };
};

static const std::array<float, 3> &binary_dither_candidate_color_for_gcode(const GCodeBinaryDitherCandidate &candidate,
                                                                            int                              generic_solver_mode)
{
    return TextureMappingZone::effective_generic_solver_mode(generic_solver_mode) == int(TextureMappingZone::GenericSolverRGB) ?
        candidate.rgb :
        candidate.oklab;
}

static std::array<float, 3> binary_dither_axis_weights_for_gcode(const std::array<float, 3> &target_color,
                                                                 int                         generic_solver_mode)
{
    return TextureMappingZone::effective_generic_solver_mode(generic_solver_mode) == int(TextureMappingZone::GenericSolverRGB) ?
        std::array<float, 3>{ { 1.f, 1.f, 1.f } } :
        generic_solver_perceptual_axis_weights_for_gcode(target_color, generic_solver_mode);
}

static float binary_dither_candidate_error_for_gcode(const GCodeBinaryDitherCandidate &candidate,
                                                     const std::array<float, 3>      &target_color,
                                                     const std::array<float, 3>      &axis_weights,
                                                     int                              generic_solver_mode)
{
    const std::array<float, 3> &candidate_color = binary_dither_candidate_color_for_gcode(candidate, generic_solver_mode);
    const float d0 = candidate_color[0] - target_color[0];
    const float d1 = candidate_color[1] - target_color[1];
    const float d2 = candidate_color[2] - target_color[2];
    float error = axis_weights[0] * d0 * d0 + axis_weights[1] * d1 * d1 + axis_weights[2] * d2 * d2;
    if (TextureMappingZone::effective_generic_solver_mode(generic_solver_mode) == int(TextureMappingZone::GenericSolverOklabSoftCap4Dark4)) {
        const float under_l = std::max(0.f, target_color[0] - candidate_color[0] - 0.04f);
        error += 4.f * generic_solver_oklab_chroma_factor_for_gcode(target_color) * under_l * under_l;
    }
    return error;
}

static std::vector<GCodeBinaryDitherCandidate> binary_dither_candidates_for_gcode(
    const std::vector<std::array<float, 3>> &component_colors,
    const std::vector<float>                &component_strength_factors,
    const std::vector<float>                &component_minimum_offset_factors,
    int                                      generic_solver_mix_model)
{
    std::vector<GCodeBinaryDitherCandidate> candidates;
    const size_t component_count = component_colors.size();
    if (component_count == 0 || component_count > 16)
        return candidates;

    const uint32_t mask_end = uint32_t(1) << component_count;
    candidates.reserve(size_t(mask_end - 1));
    for (uint32_t mask = 1; mask < mask_end; ++mask) {
        std::vector<float> mix_weights(component_count, 0.f);
        float total_weight = 0.f;
        for (size_t component_idx = 0; component_idx < component_count; ++component_idx) {
            const float strength = component_idx < component_strength_factors.size() ?
                std::clamp(component_strength_factors[component_idx], 0.f, 1.f) :
                1.f;
            const float minimum = component_idx < component_minimum_offset_factors.size() ?
                std::clamp(component_minimum_offset_factors[component_idx], 0.f, 1.f) :
                0.f;
            const bool active = (mask & (uint32_t(1) << component_idx)) != 0;
            mix_weights[component_idx] = std::clamp(minimum + (active ? strength * (1.f - minimum) : 0.f), 0.f, 1.f);
            total_weight += mix_weights[component_idx];
        }
        if (total_weight <= EPSILON) {
            for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
                if ((mask & (uint32_t(1) << component_idx)) != 0)
                    mix_weights[component_idx] = 1.f;
        }

        GCodeBinaryDitherCandidate candidate;
        candidate.mask = mask;
        candidate.rgb = mix_color_solver_components(component_colors,
                                                    mix_weights,
                                                    color_solver_mix_model_from_index(generic_solver_mix_model));
        candidate.oklab = oklab_from_srgb_for_gcode(candidate.rgb);
        candidates.emplace_back(candidate);
    }
    return candidates;
}

struct GCodeBinaryDitherNearestResult {
    size_t best_idx { size_t(-1) };
    size_t second_idx { size_t(-1) };
    float  best_error { std::numeric_limits<float>::max() };
    float  second_error { std::numeric_limits<float>::max() };
};

static GCodeBinaryDitherNearestResult nearest_binary_dither_candidates_for_gcode(
    const std::vector<GCodeBinaryDitherCandidate> &candidates,
    const std::array<float, 3>                    &target_color,
    int                                            generic_solver_mode)
{
    GCodeBinaryDitherNearestResult result;
    const int clamped_solver_mode = TextureMappingZone::effective_generic_solver_mode(generic_solver_mode);
    const std::array<float, 3> axis_weights = binary_dither_axis_weights_for_gcode(target_color, clamped_solver_mode);
    for (size_t idx = 0; idx < candidates.size(); ++idx) {
        const float error = binary_dither_candidate_error_for_gcode(candidates[idx],
                                                                    target_color,
                                                                    axis_weights,
                                                                    clamped_solver_mode);
        if (error < result.best_error) {
            result.second_error = result.best_error;
            result.second_idx = result.best_idx;
            result.best_error = error;
            result.best_idx = idx;
        } else if (error < result.second_error) {
            result.second_error = error;
            result.second_idx = idx;
        }
    }
    return result;
}

static size_t nearest_binary_dither_candidate_for_gcode(const std::vector<GCodeBinaryDitherCandidate> &candidates,
                                                        const std::array<float, 3>                    &target_color,
                                                        int                                            generic_solver_mode)
{
    return nearest_binary_dither_candidates_for_gcode(candidates, target_color, generic_solver_mode).best_idx;
}

static float binary_dither_alternate_fraction_for_gcode(const std::vector<GCodeBinaryDitherCandidate> &candidates,
                                                        const std::array<float, 3>                    &target_color,
                                                        size_t                                         base_idx,
                                                        size_t                                         alternate_idx,
                                                        int                                            generic_solver_mode)
{
    if (base_idx >= candidates.size() || alternate_idx >= candidates.size() || base_idx == alternate_idx)
        return 0.f;

    const int clamped_solver_mode = TextureMappingZone::effective_generic_solver_mode(generic_solver_mode);
    const std::array<float, 3> axis_weights = binary_dither_axis_weights_for_gcode(target_color, clamped_solver_mode);
    const std::array<float, 3> &base = binary_dither_candidate_color_for_gcode(candidates[base_idx], clamped_solver_mode);
    const std::array<float, 3> &alternate = binary_dither_candidate_color_for_gcode(candidates[alternate_idx], clamped_solver_mode);
    float numerator = 0.f;
    float denominator = 0.f;
    for (size_t axis = 0; axis < 3; ++axis) {
        const float delta = alternate[axis] - base[axis];
        numerator += axis_weights[axis] * (target_color[axis] - base[axis]) * delta;
        denominator += axis_weights[axis] * delta * delta;
    }
    if (!std::isfinite(numerator) || !std::isfinite(denominator) || denominator <= 1e-12f)
        return 0.f;
    return std::clamp(numerator / denominator, 0.f, 1.f);
}

static size_t thresholded_binary_dither_candidate_for_gcode(const std::vector<GCodeBinaryDitherCandidate> &candidates,
                                                            const std::array<float, 3>                    &target_color,
                                                            float                                          threshold,
                                                            int                                            generic_solver_mode)
{
    const GCodeBinaryDitherNearestResult nearest =
        nearest_binary_dither_candidates_for_gcode(candidates, target_color, generic_solver_mode);
    if (nearest.best_idx >= candidates.size())
        return size_t(-1);
    if (nearest.second_idx >= candidates.size())
        return nearest.best_idx;

    const float alternate_fraction =
        binary_dither_alternate_fraction_for_gcode(candidates, target_color, nearest.best_idx, nearest.second_idx, generic_solver_mode);
    return std::clamp(threshold, 0.f, 1.f) < alternate_fraction ? nearest.second_idx : nearest.best_idx;
}

static float ordered_bayer_threshold_for_gcode(int x, int y)
{
    static constexpr int matrix[8][8] = {
        { 0, 48, 12, 60, 3, 51, 15, 63 },
        { 32, 16, 44, 28, 35, 19, 47, 31 },
        { 8, 56, 4, 52, 11, 59, 7, 55 },
        { 40, 24, 36, 20, 43, 27, 39, 23 },
        { 2, 50, 14, 62, 1, 49, 13, 61 },
        { 34, 18, 46, 30, 33, 17, 45, 29 },
        { 10, 58, 6, 54, 9, 57, 5, 53 },
        { 42, 26, 38, 22, 41, 25, 37, 21 }
    };
    const int bx = ((x % 8) + 8) % 8;
    const int by = ((y % 8) + 8) % 8;
    return (float(matrix[by][bx]) + 0.5f) / 64.f - 0.5f;
}

static bool is_halftone_dithering_method_for_gcode(int method)
{
    const int clamped_method = std::clamp(method,
                                          int(TextureMappingZone::DitheringClosest),
                                          int(TextureMappingZone::DitheringHalftoneV2));
    return clamped_method == int(TextureMappingZone::DitheringHalftone) ||
           clamped_method == int(TextureMappingZone::DitheringHalftoneIncreasedDetail) ||
           clamped_method == int(TextureMappingZone::DitheringHalftoneV2);
}

static float dither_pitch_for_gcode(float base_outer_width_mm,
                                    int   dithering_method,
                                    float dithering_resolution_mm,
                                    float halftone_dot_size_mm)
{
    const float high_res_step_mm = std::clamp(base_outer_width_mm * 0.20f, 0.04f, 0.12f);
    const int clamped_method = std::clamp(dithering_method,
                                          int(TextureMappingZone::DitheringClosest),
                                          int(TextureMappingZone::DitheringHalftoneV2));
    if (is_halftone_dithering_method_for_gcode(clamped_method)) {
        const float dot_sample_step_mm =
            std::clamp(std::clamp(halftone_dot_size_mm,
                                  TextureMappingZone::MinHalftoneDotSizeMm,
                                  TextureMappingZone::MaxHalftoneDotSizeMm) *
                           0.25f,
                       0.04f,
                       0.08f);
        return std::min(high_res_step_mm, dot_sample_step_mm);
    }
    return std::min(high_res_step_mm,
                    std::clamp(dithering_resolution_mm,
                               TextureMappingZone::MinDitheringResolutionMm,
                               TextureMappingZone::MaxDitheringResolutionMm));
}

static float dither_cell_size_for_gcode(float dithering_resolution_mm)
{
    return std::clamp(dithering_resolution_mm,
                      TextureMappingZone::MinDitheringResolutionMm,
                      TextureMappingZone::MaxDitheringResolutionMm);
}

static float wrap_repeat01_for_gcode(float uv)
{
    if (!std::isfinite(uv))
        return 0.f;

    constexpr float k_uv_epsilon = 1e-6f;
    if (uv >= -k_uv_epsilon && uv <= 1.f + k_uv_epsilon)
        return std::clamp(uv, 0.f, 1.f);

    float wrapped = uv - std::floor(uv);
    if (wrapped < 0.f)
        wrapped += 1.f;
    return wrapped;
}

static std::array<float, 4> sample_texture_rgba_bilinear_for_gcode(const std::vector<uint8_t> &rgba,
                                                                    uint32_t                     width,
                                                                    uint32_t                     height,
                                                                    float                        u,
                                                                    float                        v)
{
    if (width == 0 || height == 0 || rgba.size() < size_t(width) * size_t(height) * 4)
        return { 0.f, 0.f, 0.f, 1.f };

    const float uu = wrap_repeat01_for_gcode(u);
    const float vv = wrap_repeat01_for_gcode(v);

    const float x = uu * float(width > 1 ? width - 1 : 0);
    const float y = vv * float(height > 1 ? height - 1 : 0);
    const size_t x0 = std::min<size_t>(size_t(std::floor(x)), size_t(width - 1));
    const size_t y0 = std::min<size_t>(size_t(std::floor(y)), size_t(height - 1));
    const size_t x1 = std::min<size_t>(x0 + 1, size_t(width - 1));
    const size_t y1 = std::min<size_t>(y0 + 1, size_t(height - 1));
    const float tx = x - float(x0);
    const float ty = y - float(y0);

    auto sample_channel = [&rgba, width](size_t sx, size_t sy, size_t channel) {
        const size_t idx = (sy * size_t(width) + sx) * 4 + channel;
        return float(rgba[idx]) / 255.f;
    };

    std::array<float, 4> out{};
    for (size_t c = 0; c < 4; ++c) {
        const float c00 = sample_channel(x0, y0, c);
        const float c10 = sample_channel(x1, y0, c);
        const float c01 = sample_channel(x0, y1, c);
        const float c11 = sample_channel(x1, y1, c);
        const float cx0 = c00 + (c10 - c00) * tx;
        const float cx1 = c01 + (c11 - c01) * tx;
        out[c] = clamp01f_for_gcode(cx0 + (cx1 - cx0) * ty);
    }
    return out;
}

static std::vector<float> sample_texture_raw_offsets_bilinear_for_gcode(const std::vector<uint8_t> &offsets,
                                                                        uint32_t width,
                                                                        uint32_t height,
                                                                        uint32_t channels,
                                                                        float u,
                                                                        float v)
{
    std::vector<float> out;
    if (width == 0 || height == 0 || channels == 0 ||
        offsets.size() < size_t(width) * size_t(height) * size_t(channels))
        return out;

    const float uu = wrap_repeat01_for_gcode(u);
    const float vv = wrap_repeat01_for_gcode(v);

    const float x = uu * float(width > 1 ? width - 1 : 0);
    const float y = vv * float(height > 1 ? height - 1 : 0);
    const size_t x0 = std::min<size_t>(size_t(std::floor(x)), size_t(width - 1));
    const size_t y0 = std::min<size_t>(size_t(std::floor(y)), size_t(height - 1));
    const size_t x1 = std::min<size_t>(x0 + 1, size_t(width - 1));
    const size_t y1 = std::min<size_t>(y0 + 1, size_t(height - 1));
    const float tx = x - float(x0);
    const float ty = y - float(y0);

    auto sample_channel = [&offsets, width, channels](size_t sx, size_t sy, size_t channel) {
        const size_t idx = (sy * size_t(width) + sx) * size_t(channels) + channel;
        return float(offsets[idx]) / 255.f;
    };

    out.assign(size_t(channels), 0.f);
    for (size_t c = 0; c < out.size(); ++c) {
        const float c00 = sample_channel(x0, y0, c);
        const float c10 = sample_channel(x1, y0, c);
        const float c01 = sample_channel(x0, y1, c);
        const float c11 = sample_channel(x1, y1, c);
        const float cx0 = c00 + (c10 - c00) * tx;
        const float cx1 = c01 + (c11 - c01) * tx;
        out[c] = clamp01f_for_gcode(cx0 + (cx1 - cx0) * ty);
    }
    return out;
}

static std::array<float, 4> raw_offset_preview_rgba_for_gcode(const std::vector<float> &offsets)
{
    if (offsets.empty())
        return { 0.f, 0.f, 0.f, 1.f };
    if (offsets.size() == 1)
        return { offsets[0], offsets[0], offsets[0], 1.f };
    return {
        offsets.size() > 0 ? offsets[0] : 0.f,
        offsets.size() > 1 ? offsets[1] : 0.f,
        offsets.size() > 2 ? offsets[2] : 0.f,
        1.f
    };
}

static float raw_filament_color_distance_sq_for_gcode(const std::array<float, 3> &lhs, const std::array<float, 3> &rhs)
{
    const std::array<float, 3> lhs_oklab = oklab_from_srgb_for_gcode(lhs);
    const std::array<float, 3> rhs_oklab = oklab_from_srgb_for_gcode(rhs);
    const float dl = lhs_oklab[0] - rhs_oklab[0];
    const float da = lhs_oklab[1] - rhs_oklab[1];
    const float db = lhs_oklab[2] - rhs_oklab[2];
    return dl * dl + da * da + db * db;
}

static std::array<float, 3> raw_filament_channel_color_for_gcode(const ImageMapRawFilament &filament, size_t channel_idx)
{
    const std::string key = image_map_raw_filament_channel_key(filament, channel_idx);
    if (key == "C")
        return { { 0.f, 1.f, 1.f } };
    if (key == "M")
        return { { 1.f, 0.f, 1.f } };
    if (key == "Y")
        return { { 1.f, 1.f, 0.f } };
    if (key == "K")
        return { { 0.f, 0.f, 0.f } };
    if (key == "W")
        return { { 1.f, 1.f, 1.f } };
    if (key == "R")
        return { { 1.f, 0.f, 0.f } };
    if (key == "G")
        return { { 0.f, 1.f, 0.f } };
    if (key == "B")
        return { { 0.f, 0.f, 1.f } };
    if (!filament.hex.empty()) {
        const std::optional<std::array<float, 4>> parsed = parse_texture_mapping_color_hex_for_gcode(filament.hex);
        if (parsed)
            return { { (*parsed)[0], (*parsed)[1], (*parsed)[2] } };
    }
    return { { 1.f, 1.f, 1.f } };
}

static std::vector<std::string> raw_filament_color_mode_channel_keys_for_gcode(int filament_color_mode, size_t component_count)
{
    std::vector<std::string> keys;
    switch (std::clamp(filament_color_mode, int(TextureMappingZone::FilamentColorAny), int(TextureMappingZone::FilamentColorRGBKW))) {
    case int(TextureMappingZone::FilamentColorRGB):
        keys = { "R", "G", "B" };
        break;
    case int(TextureMappingZone::FilamentColorCMY):
        keys = { "C", "M", "Y" };
        break;
    case int(TextureMappingZone::FilamentColorCMYK):
        keys = { "C", "M", "Y", "K" };
        break;
    case int(TextureMappingZone::FilamentColorCMYW):
        keys = { "C", "M", "Y", "W" };
        break;
    case int(TextureMappingZone::FilamentColorRGBK):
        keys = { "R", "G", "B", "K" };
        break;
    case int(TextureMappingZone::FilamentColorRGBW):
        keys = { "R", "G", "B", "W" };
        break;
    case int(TextureMappingZone::FilamentColorBW):
        keys = { "K", "W" };
        break;
    case int(TextureMappingZone::FilamentColorCMYKW):
        keys = { "C", "M", "Y", "K", "W" };
        break;
    case int(TextureMappingZone::FilamentColorRGBKW):
        keys = { "R", "G", "B", "K", "W" };
        break;
    default:
        break;
    }
    if (keys.size() > component_count)
        keys.resize(component_count);
    return keys;
}

static std::vector<size_t> raw_component_source_channels_for_gcode(const std::string &metadata_json,
                                                                   uint32_t source_channels,
                                                                   int filament_color_mode,
                                                                   size_t component_count,
                                                                   const std::vector<std::array<float, 3>> &component_colors)
{
    if (source_channels == 0 || component_count == 0)
        return {};

    const size_t sentinel = std::numeric_limits<size_t>::max();
    std::vector<size_t> mapping(component_count, sentinel);
    const std::vector<ImageMapRawFilament> filaments =
        image_map_raw_filaments_from_metadata_json(metadata_json, source_channels);
    if (filaments.size() != size_t(source_channels))
        return {};

    std::vector<std::string> source_keys(static_cast<size_t>(source_channels));
    std::vector<std::array<float, 3>> source_colors(static_cast<size_t>(source_channels));
    for (size_t channel = 0; channel < filaments.size(); ++channel) {
        const std::string key = image_map_raw_filament_channel_key(filaments[channel], channel);
        if (key.size() == 1 && image_map_raw_filament_is_standard_color(key))
            source_keys[channel] = key;
        source_colors[channel] = raw_filament_channel_color_for_gcode(filaments[channel], channel);
    }

    const std::vector<std::string> target_keys =
        raw_filament_color_mode_channel_keys_for_gcode(filament_color_mode, component_count);
    if (!target_keys.empty()) {
        std::vector<uint8_t> used(static_cast<size_t>(source_channels), 0);
        for (size_t component_idx = 0; component_idx < target_keys.size(); ++component_idx) {
            for (size_t channel = 0; channel < source_keys.size(); ++channel) {
                if (used[channel] == 0 && source_keys[channel] == target_keys[component_idx]) {
                    mapping[component_idx] = channel;
                    used[channel] = 1;
                    break;
                }
            }
        }

        const float max_match_distance_sq =
            TextureMappingManager::poor_color_match_distance() * TextureMappingManager::poor_color_match_distance();
        for (size_t component_idx = 0; component_idx < target_keys.size(); ++component_idx) {
            if (mapping[component_idx] != sentinel)
                continue;
            const std::array<float, 3> target_color =
                raw_filament_channel_color_for_gcode({ 0, target_keys[component_idx], std::string() }, component_idx);
            size_t best_channel = size_t(source_channels);
            float best_distance_sq = std::numeric_limits<float>::max();
            for (size_t channel = 0; channel < source_colors.size(); ++channel) {
                if (used[channel] != 0)
                    continue;
                const float distance_sq = raw_filament_color_distance_sq_for_gcode(source_colors[channel], target_color);
                if (distance_sq < best_distance_sq) {
                    best_distance_sq = distance_sq;
                    best_channel = channel;
                }
            }
            if (best_channel < source_colors.size() && best_distance_sq <= max_match_distance_sq) {
                mapping[component_idx] = best_channel;
                used[best_channel] = 1;
            }
        }
        return mapping;
    }

    if (component_colors.size() == component_count) {
        struct RawColorMatchCandidate {
            float  distance_sq { 0.f };
            size_t component_idx { 0 };
            size_t source_channel { 0 };
        };

        std::vector<RawColorMatchCandidate> candidates;
        candidates.reserve(component_count * size_t(source_channels));
        for (size_t channel = 0; channel < filaments.size(); ++channel) {
            for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
                candidates.push_back({
                    raw_filament_color_distance_sq_for_gcode(component_colors[component_idx], source_colors[channel]),
                    component_idx,
                    channel
                });
        }

        std::sort(candidates.begin(), candidates.end(), [](const RawColorMatchCandidate &lhs, const RawColorMatchCandidate &rhs) {
            return lhs.distance_sq < rhs.distance_sq;
        });

        std::vector<uint8_t> used_components(component_count, 0);
        std::vector<uint8_t> used_sources(static_cast<size_t>(source_channels), 0);
        for (const RawColorMatchCandidate &candidate : candidates) {
            if (used_components[candidate.component_idx] != 0 || used_sources[candidate.source_channel] != 0)
                continue;
            mapping[candidate.component_idx] = candidate.source_channel;
            used_components[candidate.component_idx] = 1;
            used_sources[candidate.source_channel] = 1;
        }
    }

    const bool has_mapping = std::any_of(mapping.begin(), mapping.end(), [sentinel](size_t value) { return value != sentinel; });
    return has_mapping ? mapping : std::vector<size_t>{};
}

static std::vector<float> map_raw_sample_to_components_for_gcode(const std::vector<float> &raw_sample,
                                                                 const std::vector<size_t> &component_source_channels)
{
    if (component_source_channels.empty())
        return {};
    const size_t sentinel = std::numeric_limits<size_t>::max();
    std::vector<float> mapped(component_source_channels.size(), 0.f);
    for (size_t component_idx = 0; component_idx < component_source_channels.size(); ++component_idx) {
        const size_t source_channel = component_source_channels[component_idx];
        if (source_channel != sentinel && source_channel < raw_sample.size())
            mapped[component_idx] = raw_sample[source_channel];
    }
    return mapped;
}

static bool model_volume_has_raw_offset_texture_data_for_gcode(const ModelVolume *volume)
{
    if (volume == nullptr ||
        volume->imported_texture_width == 0 ||
        volume->imported_texture_height == 0 ||
        volume->imported_texture_raw_channels == 0 ||
        volume->imported_texture_raw_filament_offsets.empty())
        return false;

    return volume->imported_texture_raw_filament_offsets.size() >=
           size_t(volume->imported_texture_width) *
               size_t(volume->imported_texture_height) *
               size_t(volume->imported_texture_raw_channels);
}

static bool model_volume_has_raw_texture_payload_for_gcode(const ModelVolume *volume)
{
    return volume != nullptr &&
           (volume->imported_texture_raw_channels != 0 ||
            !volume->imported_texture_raw_filament_offsets.empty() ||
            !volume->imported_texture_raw_metadata_json.empty() ||
            !volume->imported_texture_raw_top_surface_depths.empty() ||
            !volume->imported_texture_raw_top_surface_filament_slots.empty());
}

bool print_has_raw_offset_texture_zone_without_raw_data_for_gcode(const Print &print)
{
    const TextureMappingManager &texture_mgr = print.texture_mapping_manager();
    for (const PrintObject *print_object : print.objects()) {
        if (print_object == nullptr || print_object->model_object() == nullptr)
            continue;

        for (const ModelVolume *volume : print_object->model_object()->volumes) {
            if (volume == nullptr || !volume->is_model_part())
                continue;

            const unsigned int filament_id = unsigned(std::max(0, volume->extruder_id()));
            const TextureMappingZone *zone = texture_mgr.zone_from_id(filament_id);
            if (zone != nullptr &&
                zone->texture_mapping_mode == int(TextureMappingZone::TextureMappingRawValues) &&
                !model_volume_has_raw_offset_texture_data_for_gcode(volume))
                return true;
        }
    }
    return false;
}

static void append_image_texture_zone_id_for_raw_atlas_warning_for_gcode(const TextureMappingManager &texture_mgr,
                                                                         int                          filament_id,
                                                                         std::vector<unsigned int>   &zone_ids)
{
    if (filament_id <= 0)
        return;

    const unsigned int filament_id_u = unsigned(filament_id);
    const TextureMappingZone *zone = texture_mgr.zone_from_id(filament_id_u);
    if (zone == nullptr || !zone->enabled || zone->deleted || !zone->is_image_texture())
        return;
    if (std::find(zone_ids.begin(), zone_ids.end(), filament_id_u) == zone_ids.end())
        zone_ids.emplace_back(filament_id_u);
}

static std::vector<unsigned int> image_texture_zone_ids_for_raw_atlas_warning_for_gcode(const Print &print, const PrintObject &print_object)
{
    const ModelObject *model_object = print_object.model_object();
    if (model_object == nullptr)
        return {};

    const TextureMappingManager &texture_mgr = print.texture_mapping_manager();
    std::vector<unsigned int> zone_ids;
    for (const ModelVolume *volume : model_object->volumes) {
        if (volume == nullptr)
            continue;

        for (const int filament_id : volume->get_extruders())
            append_image_texture_zone_id_for_raw_atlas_warning_for_gcode(texture_mgr, filament_id, zone_ids);

        if (volume->mmu_segmentation_facets.empty())
            continue;

        const std::vector<bool> &used_states = volume->mmu_segmentation_facets.get_data().used_states;
        for (size_t state_idx = static_cast<size_t>(EnforcerBlockerType::Extruder1); state_idx < used_states.size(); ++state_idx)
            if (used_states[state_idx])
                append_image_texture_zone_id_for_raw_atlas_warning_for_gcode(texture_mgr, int(state_idx), zone_ids);
    }
    return zone_ids;
}

static std::string format_texture_mapping_line_width_mm_for_gcode(double value)
{
    std::ostringstream stream;
    stream << std::fixed << std::setprecision(3) << value;
    std::string formatted = stream.str();
    while (formatted.size() > 1 && formatted.back() == '0')
        formatted.pop_back();
    if (!formatted.empty() && formatted.back() == '.')
        formatted.pop_back();
    return formatted + " mm";
}

static std::string join_texture_mapping_labels_for_gcode(const std::vector<std::string> &labels)
{
    std::string out;
    for (size_t idx = 0; idx < labels.size(); ++idx) {
        if (idx > 0)
            out += ", ";
        out += labels[idx];
    }
    return out;
}

struct RawAtlasChannelWarningInfoForGCode
{
    std::string key;
    std::string label;
    std::array<float, 3> rgb { { 1.f, 1.f, 1.f } };
};

static std::vector<RawAtlasChannelWarningInfoForGCode> raw_atlas_channel_warning_infos_for_gcode(const ModelVolume &volume)
{
    const std::vector<ImageMapRawFilament> filaments =
        image_map_raw_filaments_from_metadata_json(volume.imported_texture_raw_metadata_json, volume.imported_texture_raw_channels);
    std::vector<RawAtlasChannelWarningInfoForGCode> infos;
    infos.reserve(filaments.size());
    for (size_t channel = 0; channel < filaments.size(); ++channel) {
        const ImageMapRawFilament &filament = filaments[channel];
        const std::string key = image_map_raw_filament_channel_key(filament, channel);
        std::string label = key;
        if (key.size() != 1 || !image_map_raw_filament_is_standard_color(key)) {
            const unsigned int slot = filament.slot != 0 ? filament.slot : unsigned(channel + 1);
            label = filament.hex.empty() ? "slot " + std::to_string(slot) : "slot " + std::to_string(slot) + " " + filament.hex;
        }
        infos.push_back({ key, label, raw_filament_channel_color_for_gcode(filament, channel) });
    }
    return infos;
}

std::vector<std::string> collect_raw_atlas_warnings_for_gcode(const Print &print)
{
    const double active_max_line_width_mm =
        std::max(0.05, print.config().texture_mapping_outer_wall_gradient_max_line_width.value);
    const double active_min_line_width_mm =
        std::clamp(print.config().texture_mapping_outer_wall_gradient_min_line_width.value, 0.05, active_max_line_width_mm);
    const float poor_match_distance_sq =
        TextureMappingManager::poor_color_match_distance() * TextureMappingManager::poor_color_match_distance();

    std::vector<std::string> warnings;
    std::set<std::string> seen;
    for (const PrintObject *print_object : print.objects()) {
        if (print_object == nullptr || print_object->model_object() == nullptr)
            continue;

        const std::vector<unsigned int> used_texture_zone_ids =
            image_texture_zone_ids_for_raw_atlas_warning_for_gcode(print, *print_object);
        if (used_texture_zone_ids.empty())
            continue;

        for (const ModelVolume *volume : print_object->model_object()->volumes) {
            if (!model_volume_has_raw_offset_texture_data_for_gcode(volume))
                continue;

            const ImageMapRawExpectedLineWidth expected =
                image_map_raw_expected_line_width_from_metadata_json(volume->imported_texture_raw_metadata_json);
            if (expected.valid && expected.warn_if_differs &&
                (std::abs(active_min_line_width_mm - expected.min_mm) > 0.001 ||
                 std::abs(active_max_line_width_mm - expected.max_mm) > 0.001)) {
                for (const unsigned int zone_id : used_texture_zone_ids) {
                    std::ostringstream key_stream;
                    key_stream << "width|" << print_object->id().id << "|" << volume->id().id << "|" << zone_id << "|" <<
                        std::fixed << std::setprecision(3) << expected.min_mm << "|" << expected.max_mm;
                    if (!seen.insert(key_stream.str()).second)
                        continue;

                    warnings.emplace_back(
                        _(L("Texture mapping zone ")) + std::to_string(zone_id) +
                        _(L(" uses a raw filament offset atlas designed for line widths ")) +
                        format_texture_mapping_line_width_mm_for_gcode(expected.min_mm) + " - " +
                        format_texture_mapping_line_width_mm_for_gcode(expected.max_mm) +
                        _(L(", but current texture mapping settings use ")) +
                        format_texture_mapping_line_width_mm_for_gcode(active_min_line_width_mm) + " - " +
                        format_texture_mapping_line_width_mm_for_gcode(active_max_line_width_mm) +
                        _(L(". Update the texture mapping minimum/maximum outer wall line width or regenerate the raw offset atlas.")));
                }
            }

            const std::vector<RawAtlasChannelWarningInfoForGCode> atlas_channels =
                raw_atlas_channel_warning_infos_for_gcode(*volume);
            if (atlas_channels.empty())
                continue;

            for (const unsigned int zone_id : used_texture_zone_ids) {
                const TextureMappingZone *zone = print.texture_mapping_manager().zone_from_id(zone_id);
                if (zone == nullptr || !zone->enabled || zone->deleted || !zone->is_image_texture())
                    continue;

                const std::vector<unsigned int> component_ids =
                    TextureMappingManager::effective_texture_component_ids(*zone,
                                                                           print.config().filament_colour.size(),
                                                                           print.config().filament_colour.values);
                if (component_ids.empty())
                    continue;

                const int filament_color_mode = std::clamp(zone->filament_color_mode,
                                                           int(TextureMappingZone::FilamentColorAny),
                                                           int(TextureMappingZone::FilamentColorRGBKW));
                std::ostringstream zone_key_stream;
                zone_key_stream << "channels|" << print_object->id().id << "|" << volume->id().id << "|" << zone_id << "|" <<
                    filament_color_mode;
                if (seen.find(zone_key_stream.str()) != seen.end())
                    continue;

                const std::vector<std::string> target_keys =
                    raw_filament_color_mode_channel_keys_for_gcode(filament_color_mode, component_ids.size());
                if (!target_keys.empty()) {
                    std::vector<std::string> atlas_labels;
                    std::vector<std::string> missing;
                    std::vector<std::string> unused;
                    atlas_labels.reserve(atlas_channels.size());
                    for (const RawAtlasChannelWarningInfoForGCode &channel : atlas_channels)
                        atlas_labels.push_back(channel.label);

                    std::vector<size_t> target_to_channel(target_keys.size(), size_t(-1));
                    std::vector<uint8_t> used_channels(atlas_channels.size(), 0);
                    for (size_t target_idx = 0; target_idx < target_keys.size(); ++target_idx) {
                        for (size_t channel_idx = 0; channel_idx < atlas_channels.size(); ++channel_idx) {
                            if (used_channels[channel_idx] == 0 && atlas_channels[channel_idx].key == target_keys[target_idx]) {
                                target_to_channel[target_idx] = channel_idx;
                                used_channels[channel_idx] = 1;
                                break;
                            }
                        }
                    }
                    for (size_t target_idx = 0; target_idx < target_keys.size(); ++target_idx) {
                        if (target_to_channel[target_idx] != size_t(-1))
                            continue;
                        const std::array<float, 3> target_rgb =
                            raw_filament_channel_color_for_gcode({ 0, target_keys[target_idx], std::string() }, target_idx);
                        size_t best_channel = atlas_channels.size();
                        float best_distance_sq = std::numeric_limits<float>::max();
                        for (size_t channel_idx = 0; channel_idx < atlas_channels.size(); ++channel_idx) {
                            if (used_channels[channel_idx] != 0)
                                continue;
                            const float distance_sq =
                                raw_filament_color_distance_sq_for_gcode(target_rgb, atlas_channels[channel_idx].rgb);
                            if (distance_sq < best_distance_sq) {
                                best_distance_sq = distance_sq;
                                best_channel = channel_idx;
                            }
                        }
                        if (best_channel < atlas_channels.size() && best_distance_sq <= poor_match_distance_sq) {
                            target_to_channel[target_idx] = best_channel;
                            used_channels[best_channel] = 1;
                        }
                    }

                    for (size_t target_idx = 0; target_idx < target_keys.size(); ++target_idx)
                        if (target_to_channel[target_idx] == size_t(-1))
                            missing.push_back(target_keys[target_idx]);
                    for (size_t channel_idx = 0; channel_idx < atlas_channels.size(); ++channel_idx)
                        if (used_channels[channel_idx] == 0)
                            unused.push_back(atlas_channels[channel_idx].label);

                    if (missing.empty() && unused.empty())
                        continue;

                    seen.insert(zone_key_stream.str());
                    std::string message =
                        _(L("Object has raw atlas data with channels [")) + join_texture_mapping_labels_for_gcode(atlas_labels) +
                        _(L("], but texture mapping zone ")) + std::to_string(zone_id) + _(L(" uses [")) + join_texture_mapping_labels_for_gcode(target_keys) + "].";
                    if (!missing.empty())
                        message += _(L(" Missing channels will use 0 offset/minimum line width: [")) +
                                   join_texture_mapping_labels_for_gcode(missing) + "].";
                    if (!unused.empty())
                        message += _(L(" Unused atlas channels will be ignored: [")) +
                                   join_texture_mapping_labels_for_gcode(unused) + "].";
                    warnings.emplace_back(std::move(message));
                    continue;
                }

                struct GenericRawAtlasCandidateForGCode {
                    float distance_sq { 0.f };
                    size_t component_idx { 0 };
                    size_t channel_idx { 0 };
                };

                std::vector<std::array<float, 3>> component_colors;
                std::vector<std::string> component_labels;
                component_colors.reserve(component_ids.size());
                component_labels.reserve(component_ids.size());
                for (const unsigned int component_id : component_ids) {
                    if (component_id < 1 || component_id > print.config().filament_colour.size())
                        continue;
                    const std::optional<std::array<float, 4>> parsed =
                        parse_texture_mapping_color_hex_for_gcode(print.config().filament_colour.get_at(size_t(component_id - 1)));
                    if (!parsed)
                        continue;
                    component_colors.push_back({ { (*parsed)[0], (*parsed)[1], (*parsed)[2] } });
                    component_labels.push_back("F" + std::to_string(component_id));
                }
                if (component_colors.empty())
                    continue;

                std::vector<GenericRawAtlasCandidateForGCode> candidates;
                candidates.reserve(component_colors.size() * atlas_channels.size());
                for (size_t component_idx = 0; component_idx < component_colors.size(); ++component_idx)
                    for (size_t channel_idx = 0; channel_idx < atlas_channels.size(); ++channel_idx)
                        candidates.push_back({
                            raw_filament_color_distance_sq_for_gcode(component_colors[component_idx], atlas_channels[channel_idx].rgb),
                            component_idx,
                            channel_idx
                        });
                std::sort(candidates.begin(), candidates.end(), [](const GenericRawAtlasCandidateForGCode &lhs,
                                                                    const GenericRawAtlasCandidateForGCode &rhs) {
                    return lhs.distance_sq < rhs.distance_sq;
                });

                std::vector<size_t> component_to_channel(component_colors.size(), size_t(-1));
                std::vector<uint8_t> used_components(component_colors.size(), 0);
                std::vector<uint8_t> used_channels(atlas_channels.size(), 0);
                for (const GenericRawAtlasCandidateForGCode &candidate : candidates) {
                    if (used_components[candidate.component_idx] != 0 || used_channels[candidate.channel_idx] != 0)
                        continue;
                    component_to_channel[candidate.component_idx] = candidate.channel_idx;
                    used_components[candidate.component_idx] = 1;
                    used_channels[candidate.channel_idx] = 1;
                }

                std::vector<std::string> poor_matches;
                std::vector<std::string> missing_components;
                std::vector<std::string> unused_channels;
                for (size_t component_idx = 0; component_idx < component_to_channel.size(); ++component_idx) {
                    const size_t channel_idx = component_to_channel[component_idx];
                    if (channel_idx == size_t(-1)) {
                        missing_components.push_back(component_labels[component_idx]);
                        continue;
                    }
                    const float distance_sq =
                        raw_filament_color_distance_sq_for_gcode(component_colors[component_idx], atlas_channels[channel_idx].rgb);
                    if (distance_sq > poor_match_distance_sq)
                        poor_matches.push_back(component_labels[component_idx] + " to " + atlas_channels[channel_idx].label);
                }
                for (size_t channel_idx = 0; channel_idx < atlas_channels.size(); ++channel_idx)
                    if (used_channels[channel_idx] == 0)
                        unused_channels.push_back(atlas_channels[channel_idx].label);

                if (poor_matches.empty() && missing_components.empty() && unused_channels.empty())
                    continue;

                seen.insert(zone_key_stream.str());
                std::string message =
                    _(L("Texture mapping zone ")) + std::to_string(zone_id) +
                    _(L("'s filament colors may not match "
                        "the selected filaments in an object's raw offset atlas data."));
                if (!poor_matches.empty())
                    message += _(L(" Poor color matches: ")) + join_texture_mapping_labels_for_gcode(poor_matches) + ".";
                if (!missing_components.empty())
                    message += _(L(" Unmatched filaments will use 0 offset/minimum line width: ")) +
                               join_texture_mapping_labels_for_gcode(missing_components) + ".";
                if (!unused_channels.empty())
                    message += _(L(" Unused atlas channels will be ignored: ")) +
                               join_texture_mapping_labels_for_gcode(unused_channels) + ".";
                warnings.emplace_back(std::move(message));
            }
        }
    }
    return warnings;
}

static std::array<Vec2f, 3> unwrap_triangle_uvs_for_sampling_for_gcode(const Vec2f &uv0,
                                                                        const Vec2f &uv1,
                                                                        const Vec2f &uv2)
{
    std::array<Vec2f, 3> out { uv0, uv1, uv2 };

    auto unwrap_axis = [&out](bool use_u_axis) {
        std::array<float, 3> values = {
            use_u_axis ? out[0].x() : out[0].y(),
            use_u_axis ? out[1].x() : out[1].y(),
            use_u_axis ? out[2].x() : out[2].y()
        };

        if (!std::all_of(values.begin(), values.end(), [](float value) { return std::isfinite(value); }))
            return;

        auto span = [](const std::array<float, 3> &v) {
            return std::max({ v[0], v[1], v[2] }) - std::min({ v[0], v[1], v[2] });
        };

        const bool has_repeat_evidence = std::any_of(values.begin(), values.end(), [](float value) {
            constexpr float eps = 1e-6f;
            return value < -eps || value > 1.f + eps;
        });
        const float original_span = span(values);
        if (!has_repeat_evidence || original_span <= 0.5f)
            return;

        std::array<float, 3> best = values;
        float best_span = original_span;
        for (size_t anchor = 0; anchor < values.size(); ++anchor) {
            std::array<float, 3> candidate = values;
            for (size_t i = 0; i < candidate.size(); ++i) {
                const float delta = values[i] - values[anchor];
                candidate[i] = values[anchor] + delta - std::round(delta);
            }
            const float candidate_span = span(candidate);
            if (candidate_span + 1e-6f < best_span) {
                best = candidate;
                best_span = candidate_span;
            }
        }
        if (best_span >= original_span - 1e-6f)
            return;

        if (use_u_axis) {
            out[0].x() = best[0];
            out[1].x() = best[1];
            out[2].x() = best[2];
        } else {
            out[0].y() = best[0];
            out[1].y() = best[1];
            out[2].y() = best[2];
        }
    };

    unwrap_axis(true);
    unwrap_axis(false);
    return out;
}

static GCodeUVTextureTriangleCache build_uv_texture_triangle_cache_for_gcode(const PrintObject &print_object)
{
    GCodeUVTextureTriangleCache cache;

    const ModelObject *model_object = print_object.model_object();
    if (model_object == nullptr)
        return cache;

    const Transform3d object_trafo = print_object.trafo_centered();
    for (const ModelVolume *volume : model_object->volumes) {
        if (volume == nullptr)
            continue;

        const std::shared_ptr<const TriangleMesh> mesh_ptr = volume->mesh_ptr();
        if (!mesh_ptr)
            continue;

        const indexed_triangle_set &its = mesh_ptr->its;
        const bool has_uv_texture =
            !volume->imported_texture_rgba.empty() &&
            volume->imported_texture_width > 0 &&
            volume->imported_texture_height > 0 &&
            volume->imported_texture_uv_valid.size() == its.indices.size() &&
            volume->imported_texture_uvs_per_face.size() >= its.indices.size() * 6 &&
            volume->imported_texture_rgba.size() >= size_t(volume->imported_texture_width) * size_t(volume->imported_texture_height) * 4;
        if (!has_uv_texture)
            continue;

        GCodeUVTextureVolumeMetadata volume_cache;
        volume_cache.volume = volume;
        volume_cache.triangles.reserve(its.indices.size());
        const Transform3d volume_trafo = object_trafo * volume->get_matrix();
        auto uv_edge_texel_length = [volume](const Vec2f &a, const Vec2f &b) {
            const float du = (a.x() - b.x()) * float(volume->imported_texture_width);
            const float dv = (a.y() - b.y()) * float(volume->imported_texture_height);
            return std::hypot(du, dv);
        };

        float min_z = std::numeric_limits<float>::max();
        float max_z = -std::numeric_limits<float>::max();
        for (size_t tri_idx = 0; tri_idx < its.indices.size(); ++tri_idx) {
            if (volume->imported_texture_uv_valid[tri_idx] == 0)
                continue;

            const auto &tri = its.indices[tri_idx];
            if (tri[0] < 0 || tri[1] < 0 || tri[2] < 0)
                continue;
            if (size_t(tri[0]) >= its.vertices.size() ||
                size_t(tri[1]) >= its.vertices.size() ||
                size_t(tri[2]) >= its.vertices.size())
                continue;

            const Vec3d p0 = volume_trafo * its.vertices[size_t(tri[0])].cast<double>();
            const Vec3d p1 = volume_trafo * its.vertices[size_t(tri[1])].cast<double>();
            const Vec3d p2 = volume_trafo * its.vertices[size_t(tri[2])].cast<double>();
            if (!p0.allFinite() || !p1.allFinite() || !p2.allFinite())
                continue;

            const size_t uv_off = tri_idx * 6;
            const Vec2f uv0(volume->imported_texture_uvs_per_face[uv_off + 0], volume->imported_texture_uvs_per_face[uv_off + 1]);
            const Vec2f uv1(volume->imported_texture_uvs_per_face[uv_off + 2], volume->imported_texture_uvs_per_face[uv_off + 3]);
            const Vec2f uv2(volume->imported_texture_uvs_per_face[uv_off + 4], volume->imported_texture_uvs_per_face[uv_off + 5]);
            if (!uv0.allFinite() || !uv1.allFinite() || !uv2.allFinite())
                continue;

            const std::array<Vec2f, 3> tri_uv = unwrap_triangle_uvs_for_sampling_for_gcode(uv0, uv1, uv2);
            const float tri_min_z = std::min({ float(p0.z()), float(p1.z()), float(p2.z()) });
            const float tri_max_z = std::max({ float(p0.z()), float(p1.z()), float(p2.z()) });
            const float max_uv_edge_texel = std::max({
                uv_edge_texel_length(tri_uv[0], tri_uv[1]),
                uv_edge_texel_length(tri_uv[1], tri_uv[2]),
                uv_edge_texel_length(tri_uv[2], tri_uv[0])
            });
            const float max_world_edge_mm = std::max({
                float((p1 - p0).norm()),
                float((p2 - p1).norm()),
                float((p0 - p2).norm())
            });
            const double tri_area_mm2 = 0.5 * ((p1 - p0).cross(p2 - p0)).norm();
            if (!std::isfinite(tri_min_z) || !std::isfinite(tri_max_z) ||
                !std::isfinite(max_uv_edge_texel) || !std::isfinite(max_world_edge_mm) ||
                !std::isfinite(tri_area_mm2))
                continue;

            min_z = std::min(min_z, tri_min_z);
            max_z = std::max(max_z, tri_max_z);
            volume_cache.triangles.push_back({ volume, p0, p1, p2, tri_uv, tri_min_z, tri_max_z, max_uv_edge_texel, max_world_edge_mm, tri_area_mm2 });
        }

        if (volume_cache.triangles.empty() || !std::isfinite(min_z) || !std::isfinite(max_z))
            continue;

        volume_cache.min_z = min_z;
        volume_cache.max_z = max_z;
        const float z_span = std::max(max_z - min_z, 1e-3f);
        const int z_bin_count = std::clamp(int(std::ceil(z_span / 0.2f)), 1, 2048);
        volume_cache.z_bin_step_mm = std::max(1e-3f, z_span / float(z_bin_count));
        volume_cache.z_bins.assign(size_t(z_bin_count), {});
        for (size_t tri_idx = 0; tri_idx < volume_cache.triangles.size(); ++tri_idx) {
            const GCodeUVTextureTriangleMetadata &tri = volume_cache.triangles[tri_idx];
            const int first_bin = std::clamp(int(std::floor((tri.min_z - min_z) / volume_cache.z_bin_step_mm)) - 1, 0, z_bin_count - 1);
            const int last_bin = std::clamp(int(std::floor((tri.max_z - min_z) / volume_cache.z_bin_step_mm)) + 1, 0, z_bin_count - 1);
            if (first_bin > last_bin || last_bin - first_bin > 256 || (z_bin_count > 16 && last_bin - first_bin > z_bin_count / 4)) {
                volume_cache.fallback_triangle_indices.emplace_back(uint32_t(tri_idx));
                continue;
            }
            for (int bin = first_bin; bin <= last_bin; ++bin)
                volume_cache.z_bins[size_t(bin)].emplace_back(uint32_t(tri_idx));
        }

        cache.volumes.emplace_back(std::move(volume_cache));
    }

    return cache;
}

static const GCodeUVTextureTriangleCache &uv_texture_triangle_cache_for_gcode(
    const PrintObject &print_object,
    std::map<const PrintObject*, GCodeUVTextureTriangleCache> &triangle_cache)
{
    auto it = triangle_cache.find(&print_object);
    if (it == triangle_cache.end())
        it = triangle_cache.emplace(&print_object, build_uv_texture_triangle_cache_for_gcode(print_object)).first;
    return it->second;
}

static float perceptual_color_distance_sq_for_gcode(const std::array<float, 3> &lhs, const std::array<float, 3> &rhs)
{
    const std::array<float, 3> lhs_oklab = oklab_from_srgb_for_gcode(lhs);
    const std::array<float, 3> rhs_oklab = oklab_from_srgb_for_gcode(rhs);
    const float dr = lhs_oklab[0] - rhs_oklab[0];
    const float dg = lhs_oklab[1] - rhs_oklab[1];
    const float db = lhs_oklab[2] - rhs_oklab[2];
    return dr * dr + dg * dg + db * db;
}

static std::vector<size_t> best_matching_component_indices_for_semantic_colors_for_gcode(
    const std::vector<std::array<float, 3>> &component_colors,
    const std::vector<std::array<float, 3>> &semantic_colors)
{
    if (component_colors.empty() || component_colors.size() != semantic_colors.size())
        return {};

    std::vector<size_t> permutation(component_colors.size(), 0);
    std::iota(permutation.begin(), permutation.end(), size_t(0));

    std::vector<size_t> best_permutation = permutation;
    float best_error = std::numeric_limits<float>::max();
    do {
        float error = 0.f;
        for (size_t role_idx = 0; role_idx < semantic_colors.size(); ++role_idx)
            error += perceptual_color_distance_sq_for_gcode(component_colors[permutation[role_idx]], semantic_colors[role_idx]);

        if (error < best_error) {
            best_error = error;
            best_permutation = permutation;
        }
    } while (std::next_permutation(permutation.begin(), permutation.end()));

    return best_permutation;
}

static std::vector<size_t> semantic_component_indices_for_gcode(const std::vector<std::array<float, 3>> &component_colors,
                                                                int                                       filament_color_mode,
                                                                bool                                      force_sequential_filaments)
{
    if (force_sequential_filaments)
        return {};

    std::vector<std::array<float, 3>> semantic_colors;
    switch (filament_color_mode) {
    case int(TextureMappingZone::FilamentColorRGB):
        semantic_colors = { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } } };
        break;
    case int(TextureMappingZone::FilamentColorCMY):
        semantic_colors = { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } } };
        break;
    case int(TextureMappingZone::FilamentColorCMYK):
        semantic_colors = { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } }, { { 0.f, 0.f, 0.f } } };
        break;
    case int(TextureMappingZone::FilamentColorCMYW):
        semantic_colors = { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } }, { { 1.f, 1.f, 1.f } } };
        break;
    case int(TextureMappingZone::FilamentColorRGBK):
        semantic_colors = { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } }, { { 0.f, 0.f, 0.f } } };
        break;
    case int(TextureMappingZone::FilamentColorRGBW):
        semantic_colors = { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } }, { { 1.f, 1.f, 1.f } } };
        break;
    case int(TextureMappingZone::FilamentColorCMYKW):
        semantic_colors = {
            { { 0.f, 1.f, 1.f } },
            { { 1.f, 0.f, 1.f } },
            { { 1.f, 1.f, 0.f } },
            { { 0.f, 0.f, 0.f } },
            { { 1.f, 1.f, 1.f } }
        };
        break;
    case int(TextureMappingZone::FilamentColorRGBKW):
        semantic_colors = {
            { { 1.f, 0.f, 0.f } },
            { { 0.f, 1.f, 0.f } },
            { { 0.f, 0.f, 1.f } },
            { { 0.f, 0.f, 0.f } },
            { { 1.f, 1.f, 1.f } }
        };
        break;
    default:
        return {};
    }

    return best_matching_component_indices_for_semantic_colors_for_gcode(component_colors, semantic_colors);
}

static std::vector<std::array<float, 3>> fixed_color_generic_solver_component_colors_for_gcode(int filament_color_mode)
{
    switch (std::clamp(filament_color_mode,
                       int(TextureMappingZone::FilamentColorAny),
                       int(TextureMappingZone::FilamentColorRGBKW))) {
    case int(TextureMappingZone::FilamentColorRGB):
        return { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } } };
    case int(TextureMappingZone::FilamentColorCMY):
        return { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } } };
    case int(TextureMappingZone::FilamentColorCMYK):
        return { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } }, { { 0.f, 0.f, 0.f } } };
    case int(TextureMappingZone::FilamentColorCMYW):
        return { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } }, { { 1.f, 1.f, 1.f } } };
    case int(TextureMappingZone::FilamentColorRGBK):
        return { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } }, { { 0.f, 0.f, 0.f } } };
    case int(TextureMappingZone::FilamentColorRGBW):
        return { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } }, { { 1.f, 1.f, 1.f } } };
    case int(TextureMappingZone::FilamentColorCMYKW):
        return { { { 0.f, 1.f, 1.f } }, { { 1.f, 0.f, 1.f } }, { { 1.f, 1.f, 0.f } }, { { 0.f, 0.f, 0.f } }, { { 1.f, 1.f, 1.f } } };
    case int(TextureMappingZone::FilamentColorRGBKW):
        return { { { 1.f, 0.f, 0.f } }, { { 0.f, 1.f, 0.f } }, { { 0.f, 0.f, 1.f } }, { { 0.f, 0.f, 0.f } }, { { 1.f, 1.f, 1.f } } };
    default:
        return {};
    }
}

static std::vector<float> optimized_primary_component_weights_for_target_for_gcode(const std::array<float, 3> &target_rgb,
                                                                                   size_t                      component_count,
                                                                                   int                         filament_color_mode,
                                                                                   const std::vector<std::array<float, 3>> &component_colors,
                                                                                   bool                        force_sequential_filaments)
{
    const int clamped_mode = std::clamp(filament_color_mode,
                                        int(TextureMappingZone::FilamentColorAny),
                                        int(TextureMappingZone::FilamentColorRGBKW));
    if (clamped_mode == int(TextureMappingZone::FilamentColorAny))
        return {};

    auto print_visibility_strength = [](float value) {
        return clamp01f_for_gcode(std::pow(std::max(0.f, value), 0.85f));
    };

    const float r = clamp01f_for_gcode(target_rgb[0]);
    const float g = clamp01f_for_gcode(target_rgb[1]);
    const float b = clamp01f_for_gcode(target_rgb[2]);
    const float whiteness = std::min({ r, g, b });
    const float darkness = 1.f - std::max({ r, g, b });

    auto safe_div = [](float numerator, float denominator) {
        if (denominator <= EPSILON)
            return 0.f;
        return clamp01f_for_gcode(numerator / denominator);
    };

    const std::vector<size_t> semantic_component_indices =
        semantic_component_indices_for_gcode(component_colors, clamped_mode, force_sequential_filaments);
    const auto component_index_for_role = [&semantic_component_indices](size_t role_idx) {
        if (role_idx < semantic_component_indices.size())
            return semantic_component_indices[role_idx];
        return role_idx;
    };

    std::vector<float> weights(component_count, 0.f);
    if (clamped_mode == int(TextureMappingZone::FilamentColorRGB)) {
        if (component_count != 3)
            return {};
        weights[component_index_for_role(0)] = print_visibility_strength(target_rgb[0]);
        weights[component_index_for_role(1)] = print_visibility_strength(target_rgb[1]);
        weights[component_index_for_role(2)] = print_visibility_strength(target_rgb[2]);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorCMY)) {
        if (component_count != 3)
            return {};
        weights[component_index_for_role(0)] = print_visibility_strength(1.f - r);
        weights[component_index_for_role(1)] = print_visibility_strength(1.f - g);
        weights[component_index_for_role(2)] = print_visibility_strength(1.f - b);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorBW)) {
        if (component_count != 2)
            return {};

        const float gray = clamp01f_for_gcode(0.2126f * r + 0.7152f * g + 0.0722f * b);
        const float black_strength = gray >= 0.5f ? (2.f * (1.f - gray)) : 1.f;
        const float white_strength = gray <= 0.5f ? (2.f * gray) : 1.f;
        size_t black_component_idx = 0;
        size_t white_component_idx = 1;
        if (!force_sequential_filaments && component_colors.size() >= 2) {
            const float lum0 = 0.2126f * component_colors[0][0] + 0.7152f * component_colors[0][1] + 0.0722f * component_colors[0][2];
            const float lum1 = 0.2126f * component_colors[1][0] + 0.7152f * component_colors[1][1] + 0.0722f * component_colors[1][2];
            if (lum0 > lum1) {
                black_component_idx = 1;
                white_component_idx = 0;
            }
        }

        weights[black_component_idx] = print_visibility_strength(black_strength);
        weights[white_component_idx] = print_visibility_strength(white_strength);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorCMYKW)) {
        if (component_count != 5)
            return {};
        const float chroma = std::max(0.f, 1.f - darkness - whiteness);
        const float c = chroma <= EPSILON ? 0.f : 1.f - safe_div(r - whiteness, chroma);
        const float m = chroma <= EPSILON ? 0.f : 1.f - safe_div(g - whiteness, chroma);
        const float y = chroma <= EPSILON ? 0.f : 1.f - safe_div(b - whiteness, chroma);
        weights[component_index_for_role(0)] = print_visibility_strength(c);
        weights[component_index_for_role(1)] = print_visibility_strength(m);
        weights[component_index_for_role(2)] = print_visibility_strength(y);
        weights[component_index_for_role(3)] = print_visibility_strength(darkness);
        weights[component_index_for_role(4)] = print_visibility_strength(whiteness);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorRGBKW)) {
        if (component_count != 5)
            return {};
        const float chroma = std::max(0.f, 1.f - darkness - whiteness);
        weights[component_index_for_role(0)] = print_visibility_strength(safe_div(r - whiteness, chroma));
        weights[component_index_for_role(1)] = print_visibility_strength(safe_div(g - whiteness, chroma));
        weights[component_index_for_role(2)] = print_visibility_strength(safe_div(b - whiteness, chroma));
        weights[component_index_for_role(3)] = print_visibility_strength(darkness);
        weights[component_index_for_role(4)] = print_visibility_strength(whiteness);
        return weights;
    }

    if (component_count != 4)
        return {};

    if (clamped_mode == int(TextureMappingZone::FilamentColorCMYK)) {
        const float k = clamp01f_for_gcode(darkness);
        const float inv = 1.f - k;
        weights[component_index_for_role(0)] = print_visibility_strength(safe_div(1.f - r - k, inv));
        weights[component_index_for_role(1)] = print_visibility_strength(safe_div(1.f - g - k, inv));
        weights[component_index_for_role(2)] = print_visibility_strength(safe_div(1.f - b - k, inv));
        weights[component_index_for_role(3)] = print_visibility_strength(k);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorCMYW)) {
        const float inv = 1.f - whiteness;
        const float r_no_w = safe_div(r - whiteness, inv);
        const float g_no_w = safe_div(g - whiteness, inv);
        const float b_no_w = safe_div(b - whiteness, inv);
        weights[component_index_for_role(0)] = print_visibility_strength(clamp01f_for_gcode((1.f - r_no_w) * inv));
        weights[component_index_for_role(1)] = print_visibility_strength(clamp01f_for_gcode((1.f - g_no_w) * inv));
        weights[component_index_for_role(2)] = print_visibility_strength(clamp01f_for_gcode((1.f - b_no_w) * inv));
        weights[component_index_for_role(3)] = clamp01f_for_gcode(std::pow(whiteness, 1.35f));
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorRGBK)) {
        const float k = clamp01f_for_gcode(darkness);
        const float inv = 1.f - k;
        weights[component_index_for_role(0)] = print_visibility_strength(safe_div(r - k, inv));
        weights[component_index_for_role(1)] = print_visibility_strength(safe_div(g - k, inv));
        weights[component_index_for_role(2)] = print_visibility_strength(safe_div(b - k, inv));
        weights[component_index_for_role(3)] = print_visibility_strength(k);
        return weights;
    }

    if (clamped_mode == int(TextureMappingZone::FilamentColorRGBW)) {
        const float inv = 1.f - whiteness;
        weights[component_index_for_role(0)] = print_visibility_strength(safe_div(r - whiteness, inv));
        weights[component_index_for_role(1)] = print_visibility_strength(safe_div(g - whiteness, inv));
        weights[component_index_for_role(2)] = print_visibility_strength(safe_div(b - whiteness, inv));
        weights[component_index_for_role(3)] = print_visibility_strength(whiteness);
        return weights;
    }

    return {};
}

static VertexColorOverhangWeightField build_vertex_color_weight_field_for_gcode(const PrintObject                        &print_object,
                                                                                const std::vector<std::array<float, 3>> &component_colors,
                                                                                bool                                      raw_values_mode,
                                                                                int                                       filament_color_mode,
                                                                                bool                                      force_sequential_filaments,
                                                                                int                                       generic_solver_lookup_mode,
                                                                                int                                       generic_solver_mode,
                                                                                int                                       generic_solver_mix_model,
                                                                                bool                                      use_legacy_fixed_color_mode,
                                                                                bool                                      dithering_enabled,
                                                                                int                                       dithering_method,
                                                                                float                                     dither_pitch_mm,
                                                                                float                                     dither_cell_size_mm,
                                                                                float                                     halftone_dot_size_mm,
                                                                                const std::vector<float>                 &component_strength_factors,
                                                                                const std::vector<float>                 &component_minimum_offset_factors,
                                                                                const GCodeGenericMixCandidateSet        *calibrated_side_candidates,
                                                                                std::map<std::string, GCodeGenericMixCandidateSet> *generic_mix_candidate_cache,
                                                                                std::map<const PrintObject*, GCodeUVTextureTriangleCache> *uv_texture_triangle_cache,
                                                                                float                                     texture_filament_overhang_contrast_pct,
                                                                                float                                     texture_tone_gamma,
                                                                                bool                                      layer_aware_weighting,
                                                                                float                                     layer_z_mm,
                                                                                float                                     layer_z_falloff_mm,
                                                                                bool                                      high_resolution_texture_sampling,
                                                                                bool                                      high_speed_image_texture_sampling)
{
    VertexColorOverhangWeightField weight_field;
    (void) halftone_dot_size_mm;
    if (component_colors.empty())
        return weight_field;
    const int effective_solver_mode = TextureMappingZone::effective_generic_solver_mode(generic_solver_mode);
    const size_t component_count = component_colors.size();

    const ModelObject *model_object = print_object.model_object();
    if (model_object == nullptr)
        return weight_field;

    const BoundingBox object_bbox = print_object.bounding_box();
    const float min_x_mm = unscale<float>(object_bbox.min.x());
    const float min_y_mm = unscale<float>(object_bbox.min.y());
    const float max_x_mm = unscale<float>(object_bbox.max.x());
    const float max_y_mm = unscale<float>(object_bbox.max.y());
    const float span_x_mm = std::max(max_x_mm - min_x_mm, 1e-3f);
    const float span_y_mm = std::max(max_y_mm - min_y_mm, 1e-3f);
    if (!std::isfinite(min_x_mm) || !std::isfinite(min_y_mm) ||
        !std::isfinite(max_x_mm) || !std::isfinite(max_y_mm) ||
        !std::isfinite(span_x_mm) || !std::isfinite(span_y_mm))
        return VertexColorOverhangWeightField{};

    const bool use_layer_weighting = layer_aware_weighting && std::isfinite(layer_z_mm);
    const float safe_layer_z_falloff_mm = std::max(layer_z_falloff_mm, 1e-3f);
    const float contrast_factor = std::clamp(texture_filament_overhang_contrast_pct, 25.f, 300.f) / 100.f;
    const float tone_gamma =
        (!std::isfinite(texture_tone_gamma) || texture_tone_gamma <= 0.f) ? 1.f : std::clamp(texture_tone_gamma, 0.5f, 3.f);
    const float physical_sample_pitch_mm =
        dithering_enabled && !raw_values_mode && std::isfinite(dither_pitch_mm) && dither_pitch_mm > EPSILON ?
            dither_pitch_mm :
            (high_resolution_texture_sampling ? 0.08f : 0.16f);

    struct TextureSampleData {
        std::array<float, 4> rgba { { 0.f, 0.f, 0.f, 1.f } };
        std::vector<float>   raw_component_weights;
        bool                 raw_component_weights_from_texture { false };
    };

    struct WeightedTextureSample {
        float               x_mm { 0.f };
        float               y_mm { 0.f };
        std::array<float, 4> rgba { { 0.f, 0.f, 0.f, 1.f } };
        std::vector<float>   raw_component_weights;
        bool                 raw_component_weights_from_texture { false };
        float               weight { 0.f };
    };
    std::vector<WeightedTextureSample> samples;
    samples.reserve(8192);

    auto accumulate_sample = [&samples, component_count](float x_mm,
                                                        float y_mm,
                                                        const std::array<float, 4> &rgba,
                                                        float sample_weight,
                                                        std::vector<float> raw_component_weights = {},
                                                        bool raw_component_weights_from_texture = false) {
        if (!std::isfinite(x_mm) || !std::isfinite(y_mm) || sample_weight <= EPSILON)
            return;
        if (!std::isfinite(sample_weight) ||
            !std::isfinite(rgba[0]) ||
            !std::isfinite(rgba[1]) ||
            !std::isfinite(rgba[2]) ||
            !std::isfinite(rgba[3]))
            return;
        if (raw_component_weights.size() != component_count)
            raw_component_weights_from_texture = false;

        samples.push_back({ x_mm, y_mm, rgba, std::move(raw_component_weights), raw_component_weights_from_texture, sample_weight });
    };

    struct LayerPlaneSamplePoint {
        Vec3d p;
        Vec3f barycentric;
    };

    auto accumulate_layer_plane_triangle_samples = [&](const Vec3d &p0,
                                                       const Vec3d &p1,
                                                       const Vec3d &p2,
                                                       const auto  &sample_data_for_barycentric) {
        if (!use_layer_weighting)
            return false;

        const float z0 = float(p0.z());
        const float z1 = float(p1.z());
        const float z2 = float(p2.z());
        if (!std::isfinite(z0) || !std::isfinite(z1) || !std::isfinite(z2))
            return false;

        const float min_z = std::min({ z0, z1, z2 });
        const float max_z = std::max({ z0, z1, z2 });
        const float z_eps = std::max(1e-5f, safe_layer_z_falloff_mm * 1e-4f);
        if (layer_z_mm < min_z - z_eps || layer_z_mm > max_z + z_eps || max_z - min_z <= z_eps)
            return false;

        const std::array<Vec3d, 3> vertices = { p0, p1, p2 };
        const std::array<Vec3f, 3> barycentrics = {
            Vec3f(1.f, 0.f, 0.f),
            Vec3f(0.f, 1.f, 0.f),
            Vec3f(0.f, 0.f, 1.f)
        };
        const std::array<float, 3> zs = { z0, z1, z2 };
        std::vector<LayerPlaneSamplePoint> layer_points;
        layer_points.reserve(3);

        auto add_layer_point = [&layer_points](const Vec3d &p, const Vec3f &barycentric) {
            if (!p.allFinite() || !barycentric.allFinite())
                return;
            for (const LayerPlaneSamplePoint &existing : layer_points)
                if ((existing.p - p).squaredNorm() <= 1e-10)
                    return;
            layer_points.push_back({ p, barycentric });
        };

        const std::array<std::pair<size_t, size_t>, 3> edges = {
            std::make_pair(size_t(0), size_t(1)),
            std::make_pair(size_t(1), size_t(2)),
            std::make_pair(size_t(2), size_t(0))
        };
        for (const auto &edge : edges) {
            const size_t a = edge.first;
            const size_t b = edge.second;
            const float da = zs[a] - layer_z_mm;
            const float db = zs[b] - layer_z_mm;
            const bool a_on_layer = std::abs(da) <= z_eps;
            const bool b_on_layer = std::abs(db) <= z_eps;

            if (a_on_layer)
                add_layer_point(vertices[a], barycentrics[a]);
            if (b_on_layer)
                add_layer_point(vertices[b], barycentrics[b]);
            if (a_on_layer || b_on_layer)
                continue;
            if ((da < 0.f && db > 0.f) || (da > 0.f && db < 0.f)) {
                const float t = (layer_z_mm - zs[a]) / (zs[b] - zs[a]);
                if (!std::isfinite(t) || t < -1e-4f || t > 1.f + 1e-4f)
                    continue;
                const float clamped_t = std::clamp(t, 0.f, 1.f);
                add_layer_point(vertices[a] * double(1.f - clamped_t) + vertices[b] * double(clamped_t),
                                barycentrics[a] * (1.f - clamped_t) + barycentrics[b] * clamped_t);
            }
        }

        if (layer_points.size() < 2)
            return false;

        size_t best_a = 0;
        size_t best_b = 1;
        double best_length_sq = 0.0;
        for (size_t i = 0; i + 1 < layer_points.size(); ++i) {
            for (size_t j = i + 1; j < layer_points.size(); ++j) {
                const double length_sq = (layer_points[i].p - layer_points[j].p).squaredNorm();
                if (length_sq > best_length_sq) {
                    best_a = i;
                    best_b = j;
                    best_length_sq = length_sq;
                }
            }
        }

        const double segment_length_mm = std::sqrt(best_length_sq);
        if (!std::isfinite(segment_length_mm) || segment_length_mm <= EPSILON)
            return false;

        const float sample_pitch_mm = physical_sample_pitch_mm;
        const int sample_count = std::clamp(int(std::ceil(segment_length_mm / std::max(float(EPSILON), sample_pitch_mm))), 1, 2000);
        const float sample_weight = std::max(0.05f, float(segment_length_mm) / float(sample_count));
        for (int sample_idx = 0; sample_idx < sample_count; ++sample_idx) {
            const float t = (float(sample_idx) + 0.5f) / float(sample_count);
            Vec3f barycentric = layer_points[best_a].barycentric * (1.f - t) + layer_points[best_b].barycentric * t;
            barycentric.x() = std::max(0.f, barycentric.x());
            barycentric.y() = std::max(0.f, barycentric.y());
            barycentric.z() = std::max(0.f, barycentric.z());
            const float barycentric_sum = barycentric.x() + barycentric.y() + barycentric.z();
            if (!std::isfinite(barycentric_sum) || barycentric_sum <= EPSILON)
                continue;
            barycentric /= barycentric_sum;

            const Vec3d world_pos = p0 * double(barycentric.x()) + p1 * double(barycentric.y()) + p2 * double(barycentric.z());
            TextureSampleData sample_data = sample_data_for_barycentric(barycentric);
            accumulate_sample(float(world_pos.x()),
                              float(world_pos.y()),
                              sample_data.rgba,
                              sample_weight,
                              std::move(sample_data.raw_component_weights),
                              sample_data.raw_component_weights_from_texture);
        }

        return true;
    };

    auto accumulate_constant_surface_triangle_samples = [&](const Vec3d &p0,
                                                            const Vec3d &p1,
                                                            const Vec3d &p2,
                                                            const std::array<float, 4> &rgba) {
        if (accumulate_layer_plane_triangle_samples(p0, p1, p2, [&rgba](const Vec3f &) { return TextureSampleData{ rgba, {}, false }; }))
            return;

        const float max_world_edge_mm = std::max({
            float((p1 - p0).norm()),
            float((p2 - p1).norm()),
            float((p0 - p2).norm())
        });
        if (!std::isfinite(max_world_edge_mm))
            return;

        const double tri_area_mm2 = 0.5 * ((p1 - p0).cross(p2 - p0)).norm();
        if (!std::isfinite(tri_area_mm2))
            return;

        const float world_sample_pitch_mm = physical_sample_pitch_mm;
        const int max_bary_steps =
            dithering_enabled && !raw_values_mode ? 160 : (high_resolution_texture_sampling ? 80 : 40);
        const int bary_steps = std::clamp(int(std::ceil(max_world_edge_mm / world_sample_pitch_mm)), 1, max_bary_steps);
        const int sample_count = bary_steps * (bary_steps + 1) / 2;
        if (sample_count <= 0)
            return;

        const float area_weight = std::max(0.05f, float(tri_area_mm2)) / float(sample_count);
        if (!std::isfinite(area_weight))
            return;

        const float inv_steps = 1.f / float(bary_steps);
        for (int i = 0; i < bary_steps; ++i) {
            for (int j = 0; j < (bary_steps - i); ++j) {
                const float b1 = (float(i) + 0.33333334f) * inv_steps;
                const float b2 = (float(j) + 0.33333334f) * inv_steps;
                const float b0 = 1.f - b1 - b2;
                if (b0 < 0.f)
                    continue;

                const Vec3d world_pos = p0 * double(b0) + p1 * double(b1) + p2 * double(b2);
                float sample_weight = area_weight;
                if (use_layer_weighting) {
                    const float dz = std::abs(float(world_pos.z()) - layer_z_mm);
                    const float z_norm = dz / safe_layer_z_falloff_mm;
                    const float z_weight = std::exp(-0.5f * z_norm * z_norm);
                    if (!std::isfinite(z_weight))
                        continue;
                    sample_weight *= z_weight;
                }
                if (sample_weight <= EPSILON)
                    continue;

                accumulate_sample(float(world_pos.x()), float(world_pos.y()), rgba, sample_weight);
            }
        }
    };

    const Transform3d object_trafo = print_object.trafo_centered();
    GCodeUVTextureTriangleCache local_uv_texture_cache;
    const GCodeUVTextureTriangleCache *uv_texture_cache = nullptr;
    if (uv_texture_triangle_cache != nullptr)
        uv_texture_cache = &uv_texture_triangle_cache_for_gcode(print_object, *uv_texture_triangle_cache);
    else {
        local_uv_texture_cache = build_uv_texture_triangle_cache_for_gcode(print_object);
        uv_texture_cache = &local_uv_texture_cache;
    }
    for (const ModelVolume *volume : model_object->volumes) {
        if (volume == nullptr)
            continue;

        const std::shared_ptr<const TriangleMesh> mesh_ptr = volume->mesh_ptr();
        if (!mesh_ptr)
            continue;

        const indexed_triangle_set &its = mesh_ptr->its;
        const Transform3d volume_trafo = object_trafo * volume->get_matrix();
        const std::array<float, 4> background_color = texture_mapping_background_color_for_gcode(*volume);

        if (!volume->texture_mapping_color_facets.empty()) {
            std::vector<ColorFacetTriangle> color_facets;
            volume->texture_mapping_color_facets.get_facet_triangles(*volume, color_facets);
            std::vector<uint8_t> rgba_source_triangles(its.indices.size(), 0);
            for (const ColorFacetTriangle &facet : color_facets) {
                if (facet.source_triangle >= 0 && size_t(facet.source_triangle) < rgba_source_triangles.size())
                    rgba_source_triangles[size_t(facet.source_triangle)] = 1;

                const Vec3d p0 = volume_trafo * facet.vertices[0].cast<double>();
                const Vec3d p1 = volume_trafo * facet.vertices[1].cast<double>();
                const Vec3d p2 = volume_trafo * facet.vertices[2].cast<double>();
                if (!p0.allFinite() || !p1.allFinite() || !p2.allFinite())
                    continue;

                std::array<float, 4> rgba = composite_rgba_over_background_for_gcode(unpack_rgba_u32(facet.rgba), background_color);

                accumulate_constant_surface_triangle_samples(p0, p1, p2, rgba);
            }
            for (size_t tri_idx = 0; tri_idx < its.indices.size(); ++tri_idx) {
                if (tri_idx < rgba_source_triangles.size() && rgba_source_triangles[tri_idx] != 0)
                    continue;

                const stl_triangle_vertex_indices &tri = its.indices[tri_idx];
                if (tri[0] < 0 || tri[1] < 0 || tri[2] < 0)
                    continue;
                if (size_t(tri[0]) >= its.vertices.size() ||
                    size_t(tri[1]) >= its.vertices.size() ||
                    size_t(tri[2]) >= its.vertices.size())
                    continue;

                const Vec3d p0 = volume_trafo * its.vertices[size_t(tri[0])].cast<double>();
                const Vec3d p1 = volume_trafo * its.vertices[size_t(tri[1])].cast<double>();
                const Vec3d p2 = volume_trafo * its.vertices[size_t(tri[2])].cast<double>();
                if (!p0.allFinite() || !p1.allFinite() || !p2.allFinite())
                    continue;

                accumulate_constant_surface_triangle_samples(p0, p1, p2, background_color);
            }
            continue;
        }

        bool sampled_from_uv_texture = false;
        const GCodeUVTextureVolumeMetadata *volume_uv_cache = nullptr;
        if (uv_texture_cache != nullptr) {
            for (const GCodeUVTextureVolumeMetadata &candidate : uv_texture_cache->volumes) {
                if (candidate.volume == volume) {
                    volume_uv_cache = &candidate;
                    break;
                }
            }
        }

        const bool raw_texture_payload = model_volume_has_raw_texture_payload_for_gcode(volume);
        if (volume_uv_cache != nullptr && !volume_uv_cache->triangles.empty()) {
            const std::vector<size_t> raw_component_source_channels =
                raw_component_source_channels_for_gcode(volume->imported_texture_raw_metadata_json,
                                                        volume->imported_texture_raw_channels,
                                                        filament_color_mode,
                                                        component_count,
                                                        component_colors);
            const bool use_raw_uv_texture =
                raw_component_source_channels.size() == component_count &&
                volume->imported_texture_raw_filament_offsets.size() >=
                    size_t(volume->imported_texture_width) *
                        size_t(volume->imported_texture_height) *
                        size_t(volume->imported_texture_raw_channels);
            if (raw_texture_payload && !use_raw_uv_texture)
                continue;

            auto sample_data_for_uv = [&](const Vec2f &uv) {
                std::array<float, 4> rgba = sample_texture_rgba_bilinear_for_gcode(volume->imported_texture_rgba,
                                                                                    volume->imported_texture_width,
                                                                                    volume->imported_texture_height,
                                                                                    uv.x(),
                                                                                    uv.y());
                std::vector<float> raw_component_weights;
                if (use_raw_uv_texture) {
                    const std::vector<float> raw_sample =
                        sample_texture_raw_offsets_bilinear_for_gcode(volume->imported_texture_raw_filament_offsets,
                                                                       volume->imported_texture_width,
                                                                       volume->imported_texture_height,
                                                                       volume->imported_texture_raw_channels,
                                                                       uv.x(),
                                                                       uv.y());
                    raw_component_weights = map_raw_sample_to_components_for_gcode(raw_sample, raw_component_source_channels);
                    if (raw_component_weights.size() == component_count)
                        rgba = raw_offset_preview_rgba_for_gcode(raw_component_weights);
                }
                if (raw_component_weights.size() != component_count)
                    rgba = composite_rgba_over_background_for_gcode(rgba, background_color);
                return TextureSampleData{ rgba, std::move(raw_component_weights), use_raw_uv_texture };
            };

            auto accumulate_uv_texture_triangle_samples = [&](const GCodeUVTextureTriangleMetadata &tri) {
                auto sample_data_for_barycentric = [&tri, &sample_data_for_uv](const Vec3f &barycentric) {
                    const Vec2f uv = tri.uv[0] * barycentric.x() + tri.uv[1] * barycentric.y() + tri.uv[2] * barycentric.z();
                    return sample_data_for_uv(uv);
                };
                if (accumulate_layer_plane_triangle_samples(tri.p0, tri.p1, tri.p2, sample_data_for_barycentric))
                    return true;
                if (!std::isfinite(tri.max_uv_edge_texel) || !std::isfinite(tri.max_world_edge_mm) || !std::isfinite(tri.area_mm2))
                    return false;

                const float uv_texels_per_step = high_resolution_texture_sampling ? 8.f : 18.f;
                const float world_sample_pitch_mm = physical_sample_pitch_mm;
                const int max_bary_steps =
                    dithering_enabled && !raw_values_mode ? 160 : (high_resolution_texture_sampling ? 80 : 40);
                const int uv_steps = std::clamp(int(std::ceil(tri.max_uv_edge_texel / uv_texels_per_step)), 1, max_bary_steps);
                const int world_steps = std::clamp(int(std::ceil(tri.max_world_edge_mm / world_sample_pitch_mm)), 1, max_bary_steps);
                const int bary_steps = std::max(uv_steps, world_steps);
                const int sample_count = bary_steps * (bary_steps + 1) / 2;
                if (sample_count <= 0)
                    return false;

                const float area_weight = std::max(0.05f, float(tri.area_mm2)) / float(sample_count);
                if (!std::isfinite(area_weight))
                    return false;
                const float inv_steps = 1.f / float(bary_steps);

                bool sampled = false;
                for (int i = 0; i < bary_steps; ++i) {
                    for (int j = 0; j < (bary_steps - i); ++j) {
                        const float b1 = (float(i) + 0.33333334f) * inv_steps;
                        const float b2 = (float(j) + 0.33333334f) * inv_steps;
                        const float b0 = 1.f - b1 - b2;
                        if (b0 < 0.f)
                            continue;

                        const Vec3d world_pos = tri.p0 * double(b0) + tri.p1 * double(b1) + tri.p2 * double(b2);
                        const Vec2f uv = tri.uv[0] * b0 + tri.uv[1] * b1 + tri.uv[2] * b2;
                        TextureSampleData sample_data = sample_data_for_uv(uv);

                        float sample_weight = area_weight;
                        if (use_layer_weighting) {
                            const float dz = std::abs(float(world_pos.z()) - layer_z_mm);
                            const float z_norm = dz / safe_layer_z_falloff_mm;
                            const float z_weight = std::exp(-0.5f * z_norm * z_norm);
                            if (!std::isfinite(z_weight))
                                continue;
                            sample_weight *= z_weight;
                        }
                        if (sample_weight <= EPSILON)
                            continue;

                        accumulate_sample(float(world_pos.x()),
                                          float(world_pos.y()),
                                          sample_data.rgba,
                                          sample_weight,
                                          std::move(sample_data.raw_component_weights),
                                          sample_data.raw_component_weights_from_texture);
                        sampled = true;
                    }
                }

                return sampled;
            };

            auto visit_triangle = [&](uint32_t tri_idx, std::vector<uint8_t> *visited) {
                if (size_t(tri_idx) >= volume_uv_cache->triangles.size())
                    return;
                if (visited != nullptr) {
                    if ((*visited)[size_t(tri_idx)] != 0)
                        return;
                    (*visited)[size_t(tri_idx)] = 1;
                }
                if (accumulate_uv_texture_triangle_samples(volume_uv_cache->triangles[size_t(tri_idx)]))
                    sampled_from_uv_texture = true;
            };

            const bool use_fast_layer_lookup =
                high_speed_image_texture_sampling && use_layer_weighting && !volume_uv_cache->z_bins.empty();
            if (use_fast_layer_lookup) {
                std::vector<uint8_t> visited(volume_uv_cache->triangles.size(), 0);
                const float z_margin = std::max(1e-4f, safe_layer_z_falloff_mm * 8.f + float(EPSILON));
                const float query_min_z = layer_z_mm - z_margin;
                const float query_max_z = layer_z_mm + z_margin;
                const bool valid_query =
                    std::isfinite(query_min_z) &&
                    std::isfinite(query_max_z) &&
                    std::isfinite(volume_uv_cache->min_z) &&
                    std::isfinite(volume_uv_cache->max_z) &&
                    std::isfinite(volume_uv_cache->z_bin_step_mm) &&
                    volume_uv_cache->z_bin_step_mm > 0.f;
                const bool query_overlaps_volume =
                    valid_query && query_max_z >= volume_uv_cache->min_z && query_min_z <= volume_uv_cache->max_z;
                if (!valid_query || query_overlaps_volume) {
                    for (const uint32_t tri_idx : volume_uv_cache->fallback_triangle_indices)
                        visit_triangle(tri_idx, &visited);
                }
                if (query_overlaps_volume) {
                    const int bin_count = int(volume_uv_cache->z_bins.size());
                    const int first_bin = std::clamp(
                        int(std::floor((query_min_z - volume_uv_cache->min_z) / volume_uv_cache->z_bin_step_mm)) - 1,
                        0,
                        bin_count - 1);
                    const int last_bin = std::clamp(
                        int(std::floor((query_max_z - volume_uv_cache->min_z) / volume_uv_cache->z_bin_step_mm)) + 1,
                        0,
                        bin_count - 1);
                    if (first_bin <= last_bin) {
                        for (int bin = first_bin; bin <= last_bin; ++bin)
                            for (const uint32_t tri_idx : volume_uv_cache->z_bins[size_t(bin)])
                                visit_triangle(tri_idx, &visited);
                    } else {
                        for (uint32_t tri_idx = 0; tri_idx < volume_uv_cache->triangles.size(); ++tri_idx)
                            visit_triangle(tri_idx, &visited);
                    }
                } else if (!valid_query) {
                    for (uint32_t tri_idx = 0; tri_idx < volume_uv_cache->triangles.size(); ++tri_idx)
                        visit_triangle(tri_idx, &visited);
                }
            } else {
                for (uint32_t tri_idx = 0; tri_idx < volume_uv_cache->triangles.size(); ++tri_idx)
                    visit_triangle(tri_idx, nullptr);
            }
        }

        if (sampled_from_uv_texture)
            continue;

        if (raw_texture_payload)
            continue;

        if (volume->imported_vertex_colors_rgba.empty())
            continue;
        if (its.vertices.size() != volume->imported_vertex_colors_rgba.size())
            continue;

        for (size_t i = 0; i < its.vertices.size(); ++i) {
            const Vec3d world_pos = volume_trafo * its.vertices[i].cast<double>();
            std::array<float, 4> rgba = composite_rgba_over_background_for_gcode(unpack_rgba_u32(volume->imported_vertex_colors_rgba[i]), background_color);
            float sample_weight = 1.f;
            if (use_layer_weighting) {
                const float dz = std::abs(float(world_pos.z()) - layer_z_mm);
                const float z_norm = dz / safe_layer_z_falloff_mm;
                const float z_weight = std::exp(-0.5f * z_norm * z_norm);
                if (!std::isfinite(z_weight))
                    continue;
                sample_weight *= z_weight;
            }
            if (sample_weight <= EPSILON)
                continue;

            accumulate_sample(float(world_pos.x()), float(world_pos.y()), rgba, sample_weight);
        }
    }

    if (samples.empty())
        return VertexColorOverhangWeightField{};

    const int clamped_binary_dither_method =
        std::clamp(dithering_method,
                   int(TextureMappingZone::DitheringClosest),
                   int(TextureMappingZone::DitheringHalftoneV2));
    std::vector<uint32_t> binary_dither_masks;
    const bool has_calibrated_side_candidates =
        calibrated_side_candidates != nullptr && !calibrated_side_candidates->empty();
    const bool can_binary_dither =
        dithering_enabled &&
        !has_calibrated_side_candidates &&
        !raw_values_mode &&
        !is_halftone_dithering_method_for_gcode(clamped_binary_dither_method) &&
        component_count > 0 &&
        std::isfinite(dither_cell_size_mm) &&
        dither_cell_size_mm > EPSILON;
    if (can_binary_dither) {
        bool has_raw_samples = false;
        for (const WeightedTextureSample &sample : samples) {
            if (sample.raw_component_weights_from_texture || sample.raw_component_weights.size() == component_count) {
                has_raw_samples = true;
                break;
            }
        }

        const std::vector<GCodeBinaryDitherCandidate> binary_candidates =
            has_raw_samples ?
                std::vector<GCodeBinaryDitherCandidate>{} :
                binary_dither_candidates_for_gcode(component_colors,
                                                   component_strength_factors,
                                                   component_minimum_offset_factors,
                                                   generic_solver_mix_model);
        if (!binary_candidates.empty()) {
            struct BinaryDitherCell {
                int                  x { 0 };
                int                  y { 0 };
                float                weight { 0.f };
                std::array<float, 3> target_color { { 0.f, 0.f, 0.f } };
                std::vector<size_t>  sample_indices;
            };

            auto sample_target_color = [tone_gamma, contrast_factor, effective_solver_mode](const WeightedTextureSample &sample) {
                std::array<float, 3> target = {
                    clamp01f_for_gcode(sample.rgba[0]),
                    clamp01f_for_gcode(sample.rgba[1]),
                    clamp01f_for_gcode(sample.rgba[2])
                };
                if (std::abs(tone_gamma - 1.f) > 1e-5f) {
                    target[0] = apply_texture_tone_gamma_for_gcode(target[0], tone_gamma);
                    target[1] = apply_texture_tone_gamma_for_gcode(target[1], tone_gamma);
                    target[2] = apply_texture_tone_gamma_for_gcode(target[2], tone_gamma);
                }
                target = apply_filament_overhang_contrast_to_rgb_for_gcode(target, contrast_factor);
                return effective_solver_mode == int(TextureMappingZone::GenericSolverRGB) ?
                    target :
                    oklab_from_srgb_for_gcode(target);
            };

            std::vector<BinaryDitherCell> cells;
            std::map<std::pair<int, int>, size_t> cell_index_by_coord;
            for (size_t sample_idx = 0; sample_idx < samples.size(); ++sample_idx) {
                const WeightedTextureSample &sample = samples[sample_idx];
                if (sample.weight <= EPSILON)
                    continue;

                const int cell_x = int(std::floor((sample.x_mm - min_x_mm) / dither_cell_size_mm));
                const int cell_y = int(std::floor((sample.y_mm - min_y_mm) / dither_cell_size_mm));
                const std::pair<int, int> key(cell_x, cell_y);
                auto cell_it = cell_index_by_coord.find(key);
                if (cell_it == cell_index_by_coord.end()) {
                    cell_it = cell_index_by_coord.emplace(key, cells.size()).first;
                    BinaryDitherCell cell;
                    cell.x = cell_x;
                    cell.y = cell_y;
                    cells.emplace_back(std::move(cell));
                }

                BinaryDitherCell &cell = cells[cell_it->second];
                const std::array<float, 3> target_color = sample_target_color(sample);
                const float sample_weight = std::max(sample.weight, 0.f);
                for (size_t axis = 0; axis < 3; ++axis)
                    cell.target_color[axis] += target_color[axis] * sample_weight;
                cell.weight += sample_weight;
                cell.sample_indices.emplace_back(sample_idx);
            }

            if (!cells.empty()) {
                for (BinaryDitherCell &cell : cells)
                    if (cell.weight > EPSILON)
                        for (float &value : cell.target_color)
                            value /= cell.weight;

                std::vector<size_t> order(cells.size(), 0);
                std::iota(order.begin(), order.end(), size_t(0));
                std::sort(order.begin(), order.end(), [&cells](size_t lhs, size_t rhs) {
                    if (cells[lhs].y != cells[rhs].y)
                        return cells[lhs].y < cells[rhs].y;
                    return cells[lhs].x < cells[rhs].x;
                });

                binary_dither_masks.assign(samples.size(), 0);
                std::map<std::pair<int, int>, std::array<float, 3>> floyd_error;
                for (const size_t cell_idx : order) {
                    BinaryDitherCell &cell = cells[cell_idx];
                    std::array<float, 3> target_color = cell.target_color;
                    if (clamped_binary_dither_method == int(TextureMappingZone::DitheringFloydSteinberg)) {
                        const auto error_it = floyd_error.find({cell.x, cell.y});
                        if (error_it != floyd_error.end())
                            for (size_t axis = 0; axis < 3; ++axis)
                                target_color[axis] += error_it->second[axis];
                    }

                    bool thresholded_dither = false;
                    float threshold = 0.f;
                    if (clamped_binary_dither_method == int(TextureMappingZone::DitheringOrderedBayer)) {
                        thresholded_dither = true;
                        threshold = ordered_bayer_threshold_for_gcode(cell.x, cell.y) + 0.5f;
                    }

                    const size_t candidate_idx =
                        thresholded_dither ?
                            thresholded_binary_dither_candidate_for_gcode(binary_candidates, target_color, threshold, effective_solver_mode) :
                            nearest_binary_dither_candidate_for_gcode(binary_candidates, target_color, effective_solver_mode);
                    if (candidate_idx >= binary_candidates.size())
                        continue;

                    const GCodeBinaryDitherCandidate &candidate = binary_candidates[candidate_idx];
                    for (const size_t sample_idx : cell.sample_indices)
                        if (sample_idx < binary_dither_masks.size())
                            binary_dither_masks[sample_idx] = candidate.mask;

                    if (clamped_binary_dither_method == int(TextureMappingZone::DitheringFloydSteinberg)) {
                        const std::array<float, 3> &candidate_color =
                            binary_dither_candidate_color_for_gcode(candidate, effective_solver_mode);
                        std::array<float, 3> error = {
                            target_color[0] - candidate_color[0],
                            target_color[1] - candidate_color[1],
                            target_color[2] - candidate_color[2]
                        };
                        auto add_error = [&floyd_error, &cell_index_by_coord, &error](int x, int y, float factor) {
                            if (cell_index_by_coord.find({x, y}) == cell_index_by_coord.end())
                                return;
                            std::array<float, 3> &dst = floyd_error[{x, y}];
                            for (size_t axis = 0; axis < 3; ++axis)
                                dst[axis] += error[axis] * factor;
                        };
                        add_error(cell.x + 1, cell.y, 7.f / 16.f);
                        add_error(cell.x - 1, cell.y + 1, 3.f / 16.f);
                        add_error(cell.x, cell.y + 1, 5.f / 16.f);
                        add_error(cell.x + 1, cell.y + 1, 1.f / 16.f);
                    }
                }
            }
        }
    }

    const std::vector<std::array<float, 3>> fixed_color_solver_component_colors =
        fixed_color_generic_solver_component_colors_for_gcode(filament_color_mode);
    const bool use_fixed_color_generic_solver =
        !raw_values_mode &&
        !use_legacy_fixed_color_mode &&
        fixed_color_solver_component_colors.size() == component_count;
    const std::vector<std::array<float, 3>> &generic_solver_component_colors =
        use_fixed_color_generic_solver ? fixed_color_solver_component_colors : component_colors;
    const size_t sample_count = samples.size();
    GCodeGenericMixCandidateSet local_generic_mix_candidates;
    const GCodeGenericMixCandidateSet *generic_mix_candidates = nullptr;
    if (!raw_values_mode && !has_calibrated_side_candidates) {
        std::vector<float> optimized_probe;
        if (!use_fixed_color_generic_solver) {
            const std::array<float, 3> probe_target { 0.f, 0.f, 0.f };
            optimized_probe =
                optimized_primary_component_weights_for_target_for_gcode(probe_target,
                                                                         component_count,
                                                                         filament_color_mode,
                                                                         component_colors,
                                                                         force_sequential_filaments);
        }
        if (optimized_probe.size() != component_count) {
            if (generic_mix_candidate_cache != nullptr)
                generic_mix_candidates =
                    &generic_mix_candidates_for_gcode(*generic_mix_candidate_cache,
                                                      generic_solver_component_colors,
                                                      generic_solver_mix_model);
            else {
                local_generic_mix_candidates =
                    build_generic_mix_candidates_for_gcode(generic_solver_component_colors, generic_solver_mix_model);
                generic_mix_candidates = &local_generic_mix_candidates;
            }
        }
    }

    weight_field.component_count = component_count;
    weight_field.sample_x_mm.resize(sample_count);
    weight_field.sample_y_mm.resize(sample_count);
    weight_field.sample_weight.resize(sample_count);
    weight_field.sample_component_weights.assign(sample_count * component_count, 0.f);
    weight_field.raw_component_weights_from_texture = false;
    weight_field.binary_dithered = !binary_dither_masks.empty();

    std::vector<float> fallback_acc(component_count, 0.f);
    float fallback_weight = 0.f;
    for (size_t sample_idx = 0; sample_idx < sample_count; ++sample_idx) {
        const WeightedTextureSample &sample = samples[sample_idx];
        if (sample.weight <= EPSILON)
            continue;
        if (sample.raw_component_weights_from_texture)
            weight_field.raw_component_weights_from_texture = true;

        weight_field.sample_x_mm[sample_idx] = sample.x_mm;
        weight_field.sample_y_mm[sample_idx] = sample.y_mm;
        weight_field.sample_weight[sample_idx] = sample.weight;

        std::vector<float> desired(component_count, 0.f);
        size_t mapped_component_count = component_count;
        const bool has_raw_component_weights = sample.raw_component_weights.size() == component_count;
        const bool has_binary_dither = sample_idx < binary_dither_masks.size() && binary_dither_masks[sample_idx] != 0;
        if (has_binary_dither) {
            const uint32_t mask = binary_dither_masks[sample_idx];
            for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
                desired[component_idx] = (mask & (uint32_t(1) << component_idx)) != 0 ? 1.f : 0.f;
        } else if (has_raw_component_weights) {
            float raw_activity = 0.f;
            for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
                desired[component_idx] = clamp01f_for_gcode(sample.raw_component_weights[component_idx]);
            for (const float value : desired)
                raw_activity = std::max(raw_activity, value);
            if (raw_activity <= EPSILON && !sample.raw_component_weights_from_texture)
                std::fill(desired.begin(), desired.end(), 1.f);
        } else {
            std::array<float, 3> target = {
                clamp01f_for_gcode(sample.rgba[0]),
                clamp01f_for_gcode(sample.rgba[1]),
                clamp01f_for_gcode(sample.rgba[2])
            };
            if (std::abs(tone_gamma - 1.f) > 1e-5f) {
                target[0] = apply_texture_tone_gamma_for_gcode(target[0], tone_gamma);
                target[1] = apply_texture_tone_gamma_for_gcode(target[1], tone_gamma);
                target[2] = apply_texture_tone_gamma_for_gcode(target[2], tone_gamma);
            }

            if (raw_values_mode) {
                const float channels[3] = { target[0], target[1], target[2] };
                const size_t channel_count = std::min(component_count, size_t(3));
                for (size_t channel_idx = 0; channel_idx < channel_count; ++channel_idx)
                    desired[channel_idx] = clamp01f_for_gcode(channels[channel_idx]);
                mapped_component_count = channel_count;
            } else if (has_calibrated_side_candidates) {
                std::vector<float> calibrated =
                    best_component_mix_weights_for_target_for_gcode(*calibrated_side_candidates,
                                                                    target,
                                                                    int(TextureMappingZone::GenericSolverClosestMix),
                                                                    effective_solver_mode);
                if (calibrated.size() == component_count)
                    desired = std::move(calibrated);
            } else {
                std::vector<float> optimized;
                if (!use_fixed_color_generic_solver)
                    optimized = optimized_primary_component_weights_for_target_for_gcode(target,
                                                                                        component_count,
                                                                                        filament_color_mode,
                                                                                        component_colors,
                                                                                        force_sequential_filaments);
                if (optimized.size() == component_count)
                    desired = std::move(optimized);
                else {
                    std::vector<float> best = generic_mix_candidates != nullptr ?
                        best_component_mix_weights_for_target_for_gcode(*generic_mix_candidates,
                                                                        target,
                                                                        generic_solver_lookup_mode,
                                                                        effective_solver_mode) :
                        std::vector<float>{};
                    if (best.size() == component_count)
                        desired = std::move(best);
                }
            }
        }

        if (!has_binary_dither && !has_raw_component_weights && !has_calibrated_side_candidates && std::abs(contrast_factor - 1.f) > 1e-5f)
            apply_filament_overhang_contrast_to_mapped_components_for_gcode(desired, contrast_factor, mapped_component_count);

        for (size_t component_idx = 0; component_idx < component_count; ++component_idx) {
            const float v = clamp01f_for_gcode(desired[component_idx]);
            weight_field.sample_component_weights[sample_idx * component_count + component_idx] = v;
            fallback_acc[component_idx] += v * sample.weight;
        }
        fallback_weight += sample.weight;
    }

    weight_field.fallback_weights.assign(component_count, 1.f / float(component_count));
    if (fallback_weight > EPSILON) {
        for (size_t component_idx = 0; component_idx < component_count; ++component_idx)
            weight_field.fallback_weights[component_idx] =
                clamp01f_for_gcode(fallback_acc[component_idx] / fallback_weight);
    }

    const float k_target_bucket_mm = high_resolution_texture_sampling ? 0.12f : 0.22f;
    constexpr int k_min_bucket_dim = 16;
    constexpr int k_max_bucket_dim = 320;
    constexpr int k_max_buckets = 72000;
    int bucket_width = std::clamp(int(std::ceil(span_x_mm / k_target_bucket_mm)) + 1, k_min_bucket_dim, k_max_bucket_dim);
    int bucket_height = std::clamp(int(std::ceil(span_y_mm / k_target_bucket_mm)) + 1, k_min_bucket_dim, k_max_bucket_dim);
    const int initial_buckets = bucket_width * bucket_height;
    if (initial_buckets > k_max_buckets) {
        const float scale_factor = std::sqrt(float(initial_buckets) / float(k_max_buckets));
        bucket_width = std::max(k_min_bucket_dim, int(std::ceil(float(bucket_width) / scale_factor)));
        bucket_height = std::max(k_min_bucket_dim, int(std::ceil(float(bucket_height) / scale_factor)));
    }

    weight_field.min_x_mm = min_x_mm;
    weight_field.min_y_mm = min_y_mm;
    weight_field.bucket_width = bucket_width;
    weight_field.bucket_height = bucket_height;
    weight_field.bucket_width_mm = std::max(1e-3f, span_x_mm / std::max(1, bucket_width - 1));
    weight_field.bucket_height_mm = std::max(1e-3f, span_y_mm / std::max(1, bucket_height - 1));
    weight_field.buckets.assign(size_t(bucket_width) * size_t(bucket_height), {});

    for (size_t sample_idx = 0; sample_idx < sample_count; ++sample_idx) {
        const float gx_unclamped = (weight_field.sample_x_mm[sample_idx] - min_x_mm) / weight_field.bucket_width_mm;
        const float gy_unclamped = (weight_field.sample_y_mm[sample_idx] - min_y_mm) / weight_field.bucket_height_mm;
        const int bx = std::clamp(int(std::floor(gx_unclamped)), 0, bucket_width - 1);
        const int by = std::clamp(int(std::floor(gy_unclamped)), 0, bucket_height - 1);
        const size_t bidx = size_t(by) * size_t(bucket_width) + size_t(bx);
        weight_field.buckets[bidx].push_back(uint32_t(sample_idx));
    }

    return weight_field;
}

static std::vector<float> sample_vertex_color_weight_field_components_for_gcode(const VertexColorOverhangWeightField &weight_field,
                                                                                float                                  x_mm,
                                                                                float                                  y_mm,
                                                                                bool                                   high_resolution_texture_sampling,
                                                                                float                                  smoothing_radius_mm = 0.f)
{
    std::vector<float> fallback = weight_field.fallback_weights;
    if (fallback.size() < weight_field.component_count)
        fallback.resize(weight_field.component_count, 0.f);
    if (weight_field.empty())
        return fallback;
    if (!std::isfinite(x_mm) || !std::isfinite(y_mm))
        return fallback;

    const float gx_unclamped = (x_mm - weight_field.min_x_mm) / std::max(weight_field.bucket_width_mm, 1e-6f);
    const float gy_unclamped = (y_mm - weight_field.min_y_mm) / std::max(weight_field.bucket_height_mm, 1e-6f);
    const int cx = std::clamp(int(std::floor(gx_unclamped)), 0, weight_field.bucket_width - 1);
    const int cy = std::clamp(int(std::floor(gy_unclamped)), 0, weight_field.bucket_height - 1);

    if (weight_field.binary_dithered) {
        float nearest_d2 = std::numeric_limits<float>::max();
        size_t nearest_sample_idx = size_t(-1);
        const int nearest_ring_limit = std::min(16, std::max(weight_field.bucket_width, weight_field.bucket_height));
        for (int ring = 0; ring <= nearest_ring_limit; ++ring) {
            const int min_x = std::max(0, cx - ring);
            const int max_x = std::min(weight_field.bucket_width - 1, cx + ring);
            const int min_y = std::max(0, cy - ring);
            const int max_y = std::min(weight_field.bucket_height - 1, cy + ring);

            auto visit_bucket = [&weight_field, x_mm, y_mm, &nearest_d2, &nearest_sample_idx](int bx, int by) {
                if (bx < 0 || by < 0 || bx >= weight_field.bucket_width || by >= weight_field.bucket_height)
                    return;
                const size_t bucket_idx = size_t(by) * size_t(weight_field.bucket_width) + size_t(bx);
                if (bucket_idx >= weight_field.buckets.size())
                    return;
                for (const uint32_t sample_idx_u32 : weight_field.buckets[bucket_idx]) {
                    const size_t sample_idx = size_t(sample_idx_u32);
                    if (sample_idx >= weight_field.sample_x_mm.size() || sample_idx >= weight_field.sample_y_mm.size())
                        continue;
                    const float dx = x_mm - weight_field.sample_x_mm[sample_idx];
                    const float dy = y_mm - weight_field.sample_y_mm[sample_idx];
                    const float d2 = dx * dx + dy * dy;
                    if (d2 >= nearest_d2)
                        continue;
                    const size_t value_idx = sample_idx * weight_field.component_count;
                    if (value_idx + weight_field.component_count > weight_field.sample_component_weights.size())
                        continue;
                    nearest_d2 = d2;
                    nearest_sample_idx = sample_idx;
                }
            };

            if (ring == 0) {
                visit_bucket(cx, cy);
            } else {
                for (int x = min_x; x <= max_x; ++x) {
                    visit_bucket(x, min_y);
                    if (max_y != min_y)
                        visit_bucket(x, max_y);
                }
                for (int y = min_y + 1; y <= max_y - 1; ++y) {
                    visit_bucket(min_x, y);
                    if (max_x != min_x)
                        visit_bucket(max_x, y);
                }
            }

            if (nearest_sample_idx != size_t(-1))
                break;
        }

        if (nearest_sample_idx != size_t(-1)) {
            std::vector<float> values(weight_field.component_count, 0.f);
            const size_t value_idx = nearest_sample_idx * weight_field.component_count;
            for (size_t component_idx = 0; component_idx < weight_field.component_count; ++component_idx)
                values[component_idx] = clamp01f_for_gcode(weight_field.sample_component_weights[value_idx + component_idx]);
            return values;
        }
        return fallback;
    }

    const float sigma_scale = high_resolution_texture_sampling ? 0.45f : 0.7f;
    const float min_sigma_mm = high_resolution_texture_sampling ? 0.04f : 0.06f;
    const float safe_smoothing_radius_mm =
        std::isfinite(smoothing_radius_mm) ? std::max(0.f, smoothing_radius_mm) : 0.f;
    const float sigma_floor_mm = safe_smoothing_radius_mm > EPSILON ?
        std::max(min_sigma_mm, safe_smoothing_radius_mm * 0.5f) :
        min_sigma_mm;
    const float sigma_x_mm = std::max(sigma_floor_mm, weight_field.bucket_width_mm * sigma_scale);
    const float sigma_y_mm = std::max(sigma_floor_mm, weight_field.bucket_height_mm * sigma_scale);
    const float inv_two_sigma_x2 = 1.f / std::max(2.f * sigma_x_mm * sigma_x_mm, 1e-8f);
    const float inv_two_sigma_y2 = 1.f / std::max(2.f * sigma_y_mm * sigma_y_mm, 1e-8f);

    const float min_radius_mm = high_resolution_texture_sampling ? 0.16f : 0.30f;
    const float radius_scale = high_resolution_texture_sampling ? 1.75f : 3.f;
    const float max_radius_mm = std::max(std::max(min_radius_mm, safe_smoothing_radius_mm),
                                         std::max(weight_field.bucket_width_mm, weight_field.bucket_height_mm) * radius_scale);
    const float max_radius2 = max_radius_mm * max_radius_mm;
    const float min_bucket_span_mm = std::max(1e-3f, std::min(weight_field.bucket_width_mm, weight_field.bucket_height_mm));
    const int max_ring = std::max(1, int(std::ceil(max_radius_mm / min_bucket_span_mm)));

    std::vector<float> weighted_sum(weight_field.component_count, 0.f);
    float total_weight = 0.f;
    size_t contributing_samples = 0;

    auto process_bucket = [&weight_field,
                           x_mm,
                           y_mm,
                           max_radius2,
                           inv_two_sigma_x2,
                           inv_two_sigma_y2,
                           &weighted_sum,
                           &total_weight,
                           &contributing_samples](int bx, int by) {
        if (bx < 0 || by < 0 || bx >= weight_field.bucket_width || by >= weight_field.bucket_height)
            return;

        const size_t bucket_idx = size_t(by) * size_t(weight_field.bucket_width) + size_t(bx);
        if (bucket_idx >= weight_field.buckets.size())
            return;

        for (const uint32_t sample_idx_u32 : weight_field.buckets[bucket_idx]) {
            const size_t sample_idx = size_t(sample_idx_u32);
            if (sample_idx >= weight_field.sample_x_mm.size() ||
                sample_idx >= weight_field.sample_y_mm.size() ||
                sample_idx >= weight_field.sample_weight.size())
                continue;

            const float dx = x_mm - weight_field.sample_x_mm[sample_idx];
            const float dy = y_mm - weight_field.sample_y_mm[sample_idx];
            const float d2 = dx * dx + dy * dy;
            if (d2 > max_radius2)
                continue;

            const float kernel = std::exp(-(dx * dx) * inv_two_sigma_x2 - (dy * dy) * inv_two_sigma_y2);
            const float sample_w = weight_field.sample_weight[sample_idx] * kernel;
            if (!std::isfinite(sample_w) || sample_w <= EPSILON)
                continue;

            const size_t value_idx = sample_idx * weight_field.component_count;
            if (value_idx + weight_field.component_count > weight_field.sample_component_weights.size())
                continue;

            for (size_t component_idx = 0; component_idx < weight_field.component_count; ++component_idx)
                weighted_sum[component_idx] += weight_field.sample_component_weights[value_idx + component_idx] * sample_w;
            total_weight += sample_w;
            ++contributing_samples;
        }
    };

    for (int ring = 0; ring <= max_ring; ++ring) {
        const int min_x = std::max(0, cx - ring);
        const int max_x = std::min(weight_field.bucket_width - 1, cx + ring);
        const int min_y = std::max(0, cy - ring);
        const int max_y = std::min(weight_field.bucket_height - 1, cy + ring);

        if (ring == 0) {
            process_bucket(cx, cy);
        } else {
            for (int x = min_x; x <= max_x; ++x) {
                process_bucket(x, min_y);
                if (max_y != min_y)
                    process_bucket(x, max_y);
            }
            for (int y = min_y + 1; y <= max_y - 1; ++y) {
                process_bucket(min_x, y);
                if (max_x != min_x)
                    process_bucket(max_x, y);
            }
        }

        if (total_weight > EPSILON && contributing_samples >= 12)
            break;
    }

    if (total_weight > EPSILON) {
        std::vector<float> values(weight_field.component_count, 0.f);
        for (size_t component_idx = 0; component_idx < weight_field.component_count; ++component_idx)
            values[component_idx] = clamp01f_for_gcode(weighted_sum[component_idx] / total_weight);
        return values;
    }

    float nearest_d2 = std::numeric_limits<float>::max();
    size_t nearest_sample_idx = size_t(-1);
    const int nearest_ring_limit = std::min(std::max(max_ring + 2, 4), std::max(weight_field.bucket_width, weight_field.bucket_height));

    for (int ring = 0; ring <= nearest_ring_limit; ++ring) {
        const int min_x = std::max(0, cx - ring);
        const int max_x = std::min(weight_field.bucket_width - 1, cx + ring);
        const int min_y = std::max(0, cy - ring);
        const int max_y = std::min(weight_field.bucket_height - 1, cy + ring);

        auto visit_bucket = [&weight_field, x_mm, y_mm, &nearest_d2, &nearest_sample_idx](int bx, int by) {
            if (bx < 0 || by < 0 || bx >= weight_field.bucket_width || by >= weight_field.bucket_height)
                return;

            const size_t bucket_idx = size_t(by) * size_t(weight_field.bucket_width) + size_t(bx);
            if (bucket_idx >= weight_field.buckets.size())
                return;

            for (const uint32_t sample_idx_u32 : weight_field.buckets[bucket_idx]) {
                const size_t sample_idx = size_t(sample_idx_u32);
                if (sample_idx >= weight_field.sample_x_mm.size() || sample_idx >= weight_field.sample_y_mm.size())
                    continue;

                const float dx = x_mm - weight_field.sample_x_mm[sample_idx];
                const float dy = y_mm - weight_field.sample_y_mm[sample_idx];
                const float d2 = dx * dx + dy * dy;
                if (d2 >= nearest_d2)
                    continue;

                const size_t value_idx = sample_idx * weight_field.component_count;
                if (value_idx + weight_field.component_count > weight_field.sample_component_weights.size())
                    continue;

                nearest_d2 = d2;
                nearest_sample_idx = sample_idx;
            }
        };

        if (ring == 0) {
            visit_bucket(cx, cy);
        } else {
            for (int x = min_x; x <= max_x; ++x) {
                visit_bucket(x, min_y);
                if (max_y != min_y)
                    visit_bucket(x, max_y);
            }
            for (int y = min_y + 1; y <= max_y - 1; ++y) {
                visit_bucket(min_x, y);
                if (max_x != min_x)
                    visit_bucket(max_x, y);
            }
        }

        if (nearest_d2 < std::numeric_limits<float>::max() && ring >= 2)
            break;
    }

    if (nearest_sample_idx != size_t(-1)) {
        std::vector<float> values(weight_field.component_count, 0.f);
        const size_t value_idx = nearest_sample_idx * weight_field.component_count;
        for (size_t component_idx = 0; component_idx < weight_field.component_count; ++component_idx)
            values[component_idx] = clamp01f_for_gcode(weight_field.sample_component_weights[value_idx + component_idx]);
        return values;
    }

    return fallback;
}

static float sample_vertex_color_weight_field_for_gcode(const VertexColorOverhangWeightField &weight_field,
                                                        float                                  x_mm,
                                                        float                                  y_mm,
                                                        size_t                                 component_idx,
                                                        bool                                   high_resolution_texture_sampling,
                                                        bool                                   compact_offset_mode = false,
                                                        float                                  smoothing_radius_mm = 0.f)
{
    const float fallback = component_idx < weight_field.fallback_weights.size() ?
        weight_field.fallback_weights[component_idx] : 0.f;
    if (compact_offset_mode && !weight_field.raw_component_weights_from_texture && !weight_field.empty() &&
        component_idx < weight_field.component_count) {
        std::vector<float> values = sample_vertex_color_weight_field_components_for_gcode(weight_field,
                                                                                          x_mm,
                                                                                          y_mm,
                                                                                          high_resolution_texture_sampling,
                                                                                          smoothing_radius_mm);
        float max_value = 0.f;
        for (size_t idx = 0; idx < weight_field.component_count && idx < values.size(); ++idx)
            max_value = std::max(max_value, clamp01f_for_gcode(values[idx]));
        if (max_value > EPSILON)
            return clamp01f_for_gcode(values[component_idx] / max_value);
    }

    const std::vector<float> values = sample_vertex_color_weight_field_components_for_gcode(weight_field,
                                                                                            x_mm,
                                                                                            y_mm,
                                                                                            high_resolution_texture_sampling,
                                                                                            smoothing_radius_mm);
    return component_idx < values.size() ? clamp01f_for_gcode(values[component_idx]) : fallback;
}

static float component_angular_influence_for_gcode(unsigned int                     active_component_id,
                                                   float                            theta_deg,
                                                   const std::vector<unsigned int> &component_ids,
                                                   const std::vector<float>        &component_angles_deg)
{
    if (component_ids.empty() || component_ids.size() != component_angles_deg.size())
        return 0.f;

    const auto active_it = std::find(component_ids.begin(), component_ids.end(), active_component_id);
    if (active_it == component_ids.end())
        return 0.f;

    if (component_ids.size() == 1)
        return 1.f;

    struct SortedComponentAngle {
        float  angle_deg { 0.f };
        size_t component_idx { 0 };
    };

    std::vector<SortedComponentAngle> sorted_angles;
    sorted_angles.reserve(component_ids.size());
    for (size_t i = 0; i < component_ids.size(); ++i)
        sorted_angles.push_back({ normalize_angle_deg_for_gcode(component_angles_deg[i]), i });

    std::sort(sorted_angles.begin(), sorted_angles.end(), [](const SortedComponentAngle &lhs, const SortedComponentAngle &rhs) {
        return lhs.angle_deg < rhs.angle_deg;
    });

    const size_t active_component_idx = size_t(active_it - component_ids.begin());
    const auto sorted_active_it = std::find_if(sorted_angles.begin(), sorted_angles.end(),
                                               [active_component_idx](const SortedComponentAngle &entry) {
                                                   return entry.component_idx == active_component_idx;
                                               });
    if (sorted_active_it == sorted_angles.end())
        return 0.f;

    const size_t sorted_pos = size_t(sorted_active_it - sorted_angles.begin());
    const size_t count = sorted_angles.size();
    const float prev_angle = sorted_angles[(sorted_pos + count - 1) % count].angle_deg;
    const float self_angle = sorted_angles[sorted_pos].angle_deg;
    const float next_angle = sorted_angles[(sorted_pos + 1) % count].angle_deg;
    const float prev_to_self_deg = angular_distance_cw_deg_for_gcode(prev_angle, self_angle);
    const float self_to_next_deg = angular_distance_cw_deg_for_gcode(self_angle, next_angle);

    if (prev_to_self_deg <= 1e-3f || self_to_next_deg <= 1e-3f) {
        float total_weight = 0.f;
        float active_weight = 0.f;
        for (size_t i = 0; i < component_ids.size(); ++i) {
            const float dist = angular_distance_deg_for_gcode(theta_deg, component_angles_deg[i]);
            const float weight = std::max(0.f, 1.f - dist / 180.f);
            total_weight += weight;
            if (component_ids[i] == active_component_id)
                active_weight += weight;
        }

        if (total_weight <= EPSILON)
            return 0.f;
        return std::clamp(active_weight / total_weight, 0.f, 1.f);
    }

    const float theta_norm = normalize_angle_deg_for_gcode(theta_deg);
    const float prev_to_theta_deg = angular_distance_cw_deg_for_gcode(prev_angle, theta_norm);
    if (prev_to_theta_deg <= prev_to_self_deg + 1e-4f)
        return std::clamp(prev_to_theta_deg / prev_to_self_deg, 0.f, 1.f);

    const float self_to_theta_deg = angular_distance_cw_deg_for_gcode(self_angle, theta_norm);
    if (self_to_theta_deg <= self_to_next_deg + 1e-4f)
        return std::clamp(1.f - self_to_theta_deg / self_to_next_deg, 0.f, 1.f);

    return 0.f;
}

std::optional<PreferredSeamPoint> GCode::texture_mapping_seam_hiding_hint(const ExtrusionLoop &loop)
{
    if (m_curr_print == nullptr ||
        m_layer == nullptr ||
        m_writer.filament() == nullptr ||
        loop.paths.empty() ||
        !is_external_perimeter(loop.role()))
        return std::nullopt;

    const size_t num_physical = m_config.filament_colour.values.size();
    const unsigned int texture_zone_id = unsigned(std::max(0, m_config.wall_filament.value));
    const TextureMappingManager &texture_mgr = m_curr_print->texture_mapping_manager();
    if (num_physical == 0 || texture_zone_id == 0 || !texture_mgr.is_texture_mapping_zone_id(texture_zone_id))
        return std::nullopt;

    const TextureMappingZone *zone = texture_mgr.zone_from_id(texture_zone_id);
    if (zone == nullptr || !zone->seam_hiding)
        return std::nullopt;

    const bool vertex_color_match_mode = is_vertex_color_match_overhang_row_for_gcode(*zone);
    const bool offset_gradient_mode = is_2d_offset_gradient_row_for_gcode(*zone);
    if (!vertex_color_match_mode && !offset_gradient_mode)
        return std::nullopt;

    const PrintObject *layer_object = m_layer->object();
    const Layer *upper_layer = m_layer->upper_layer;
    const Layer *lower_layer = m_layer->lower_layer;
    const int object_layer_count = layer_object ? int(layer_object->layer_count()) : 0;
    if (layer_object == nullptr || object_layer_count <= 0 || (upper_layer == nullptr && lower_layer == nullptr))
        return std::nullopt;

    std::vector<unsigned int> component_ids = decode_texture_mapping_offset_component_ids(*zone, num_physical);
    if (vertex_color_match_mode) {
        const std::vector<unsigned int> effective_component_ids =
            TextureMappingManager::effective_texture_component_ids(*zone, num_physical, m_config.filament_colour.values);
        if (!effective_component_ids.empty())
            component_ids = effective_component_ids;
    }
    if (component_ids.empty())
        return std::nullopt;

    const int texture_filament_color_mode = std::clamp(zone->filament_color_mode,
                                                       int(TextureMappingZone::FilamentColorAny),
                                                       int(TextureMappingZone::FilamentColorRGBKW));
    const bool raw_texture_mapping_mode =
        zone->texture_mapping_mode == int(TextureMappingZone::TextureMappingRawValues);
    const bool texture_force_sequential_filaments = zone->force_sequential_filaments;
    const int generic_solver_lookup_mode = std::clamp(zone->generic_solver_lookup_mode,
                                                      int(TextureMappingZone::GenericSolverClosestMix),
                                                      int(TextureMappingZone::GenericSolverBlendClosestTwo));
    const int generic_solver_mode = TextureMappingZone::effective_generic_solver_mode(zone->generic_solver_mode);
    const int generic_solver_mix_model = std::clamp(zone->generic_solver_mix_model,
                                                    int(TextureMappingZone::GenericSolverPigmentPainter),
                                                    int(TextureMappingZone::GenericSolverPrusaFdmMixer));
    const bool dithering_enabled =
        zone->dithering_enabled &&
        zone->texture_mapping_mode != int(TextureMappingZone::TextureMappingRawValues);
    const int dithering_method = std::clamp(zone->dithering_method,
                                            int(TextureMappingZone::DitheringClosest),
                                            int(TextureMappingZone::DitheringHalftoneV2));
    const bool halftone_dithering_enabled =
        dithering_enabled && is_halftone_dithering_method_for_gcode(dithering_method);
    const float seam_texture_base_width_mm =
        std::max(0.05f, float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
    const float dither_pitch_mm =
        dither_pitch_for_gcode(seam_texture_base_width_mm,
                               dithering_method,
                               zone->dithering_resolution_mm,
                               zone->halftone_dot_size_mm);
    const float dither_cell_size_mm = dither_cell_size_for_gcode(zone->dithering_resolution_mm);
    const float halftone_dot_size_mm = std::clamp(zone->halftone_dot_size_mm,
                                                  TextureMappingZone::MinHalftoneDotSizeMm,
                                                  TextureMappingZone::MaxHalftoneDotSizeMm);
    const bool compact_offset_mode = halftone_dithering_enabled ? false : zone->compact_offset_mode || dithering_enabled;
    const bool nonlinear_offset_adjustment = zone->nonlinear_offset_adjustment;
    const bool use_legacy_fixed_color_mode = zone->use_legacy_fixed_color_mode;
    const float texture_tone_gamma =
        (!std::isfinite(zone->tone_gamma) || zone->tone_gamma <= 0.f) ?
            1.f :
            std::clamp(zone->tone_gamma, 0.5f, 3.f);
    const bool high_resolution_texture_sampling = zone->high_resolution_sampling || dithering_enabled;
    const bool high_speed_image_texture_sampling = zone->high_speed_image_texture_sampling;
    const bool calibrated_side_mode =
        vertex_color_match_mode &&
        !raw_texture_mapping_mode &&
        zone->transmission_distance_calibration_mode == int(TextureMappingZone::TDCalibrationCalibratedNearestMeasuredSample);
    const float texture_filament_overhang_contrast_pct =
        calibrated_side_mode ?
            TextureMappingZone::DefaultFilamentOverhangContrastPct :
            std::clamp(zone->filament_overhang_contrast_pct, 25.f, 300.f);
    std::vector<float> component_strength_factors;
    component_strength_factors.reserve(component_ids.size());
    std::vector<float> component_minimum_offset_factors;
    component_minimum_offset_factors.reserve(component_ids.size());
    for (const unsigned int id : component_ids) {
        component_strength_factors.emplace_back(calibrated_side_mode ? 1.f : overhang_filament_strength_factor_for_gcode(*zone, id));
        component_minimum_offset_factors.emplace_back(calibrated_side_mode ? 0.f : overhang_filament_minimum_offset_factor_for_gcode(*zone, id));
    }

    std::vector<std::array<float, 3>> component_colors;
    component_colors.reserve(component_ids.size());
    bool missing_component_color = false;
    for (const unsigned int id : component_ids) {
        if (id < 1 || id > m_config.filament_colour.values.size()) {
            if (raw_texture_mapping_mode)
                component_colors.push_back({ 0.f, 0.f, 0.f });
            else
                missing_component_color = true;
            continue;
        }
        ColorRGB decoded;
        if (!decode_color(m_config.filament_colour.get_at(size_t(id - 1)), decoded)) {
            if (raw_texture_mapping_mode)
                component_colors.push_back({ 0.f, 0.f, 0.f });
            else
                missing_component_color = true;
            continue;
        }
        component_colors.push_back({ decoded.r(), decoded.g(), decoded.b() });
    }
    if (vertex_color_match_mode &&
        (missing_component_color || component_colors.size() != component_ids.size() || component_colors.empty()))
        return std::nullopt;

    std::vector<float> component_transmission_distances_mm;
    component_transmission_distances_mm.reserve(component_ids.size());
    for (const unsigned int id : component_ids) {
        const size_t idx = id > 0 ? size_t(id - 1) : size_t(-1);
        const float value = idx < zone->filament_transmission_distances_mm.size() ?
            zone->filament_transmission_distances_mm[idx] :
            0.f;
        component_transmission_distances_mm.emplace_back(std::isfinite(value) && value > 0.f ? std::clamp(value, 0.01f, 50.f) : 0.f);
    }
    std::optional<GCodeGenericMixCandidateSet> calibrated_side_candidates;
    const GCodeGenericMixCandidateSet *calibrated_side_candidates_ptr = nullptr;
    if (calibrated_side_mode) {
        calibrated_side_candidates =
            texture_mapping_side_surface_color_calibrated_candidates(*zone,
                                                                     component_colors,
                                                                     component_transmission_distances_mm);
        if (calibrated_side_candidates && !calibrated_side_candidates->empty())
            calibrated_side_candidates_ptr = &*calibrated_side_candidates;
    }

    const TransmissionDistanceCalibrationContextForGCode td_calibration_context =
        transmission_distance_calibration_context_for_gcode(*zone,
                                                            component_ids,
                                                            component_colors,
                                                            texture_filament_color_mode);

    std::ostringstream component_key_stream;
    for (size_t idx = 0; idx < component_ids.size(); ++idx) {
        if (idx > 0)
            component_key_stream << '/';
        component_key_stream << component_ids[idx];
    }
    component_key_stream << (raw_texture_mapping_mode ? "|raw" : "|blend");
    component_key_stream << "|fc" << texture_filament_color_mode;
    component_key_stream << "|fs" << (texture_force_sequential_filaments ? 1 : 0);
    component_key_stream << "|gl" << generic_solver_lookup_mode;
    component_key_stream << "|gm" << generic_solver_mode;
    component_key_stream << "|gx" << generic_solver_mix_model;
    component_key_stream << "|de" << (dithering_enabled ? 1 : 0);
    component_key_stream << "|dm" << dithering_method;
    component_key_stream << "|dp" << int(std::lround(dither_pitch_mm * 1000.f));
    if (is_halftone_dithering_method_for_gcode(dithering_method))
        component_key_stream << "|hdt" << int(std::lround(halftone_dot_size_mm * 1000.f));
    else
        component_key_stream << "|hrz" << int(std::lround(dither_cell_size_mm * 1000.f));
    for (const float strength_factor : component_strength_factors)
        component_key_stream << "|st" << int(std::lround(std::clamp(strength_factor, 0.f, 1.f) * 1000.f));
    for (const float minimum_offset_factor : component_minimum_offset_factors)
        component_key_stream << "|mo" << int(std::lround(std::clamp(minimum_offset_factor, 0.f, 1.f) * 1000.f));
    component_key_stream << "|lf" << (use_legacy_fixed_color_mode ? 1 : 0);
    component_key_stream << "|ct" << int(std::lround(texture_filament_overhang_contrast_pct));
    component_key_stream << "|tg" << int(std::lround(texture_tone_gamma * 100.f));
    component_key_stream << "|hr" << (high_resolution_texture_sampling ? 1 : 0);
    component_key_stream << "|hs" << (high_speed_image_texture_sampling ? 1 : 0);
    if (calibrated_side_mode)
        component_key_stream << "|sc" << std::hash<std::string>{}(zone->side_surface_color_calibration_json);
    const std::string component_key_prefix = component_key_stream.str();

    struct SeamLayerTextureState {
        const Layer *layer { nullptr };
        float        layer_height_mm { 0.f };
        int          layer_index { 0 };
        unsigned int active_component_id { 0 };
        size_t       active_component_idx { size_t(-1) };
        const VertexColorOverhangWeightField *weight_field { nullptr };
        float        active_component_strength_factor { 1.f };
        float        active_component_minimum_offset_factor { 0.f };
        float        active_component_td_width_factor { 1.f };
        float        signed_fade_factor { 1.f };
        float        fade_factor { 1.f };
    };

    auto texture_state_for_layer = [&](const Layer *layer,
                                       unsigned int active_component_override) -> std::optional<SeamLayerTextureState> {
        if (layer == nullptr)
            return std::nullopt;

        const int layer_index = int(layer->id());
        const float layer_height_mm = std::max(0.01f, float(layer->height));
        const float z_progress = object_layer_count > 1 ?
            std::clamp(float(layer_index) / float(object_layer_count - 1), 0.f, 1.f) :
            0.f;
        const unsigned int active_component_id = active_component_override != 0 ?
            active_component_override :
            texture_mgr.resolve_zone_component(texture_zone_id, num_physical, layer_index);
        const auto active_component_it = std::find(component_ids.begin(), component_ids.end(), active_component_id);
        if (active_component_it == component_ids.end())
            return std::nullopt;
        const size_t active_component_idx = size_t(active_component_it - component_ids.begin());
        size_t previous_component_idx = size_t(-1);
        if (layer_index > 0) {
            const unsigned int previous_component_id =
                texture_mgr.resolve_zone_component(texture_zone_id, num_physical, layer_index - 1);
            const auto previous_component_it = std::find(component_ids.begin(), component_ids.end(), previous_component_id);
            if (previous_component_it != component_ids.end())
                previous_component_idx = size_t(previous_component_it - component_ids.begin());
        }

        const VertexColorOverhangWeightField *weight_field = nullptr;
        if (vertex_color_match_mode) {
            std::ostringstream layer_key_stream;
            layer_key_stream << component_key_prefix << "|seamL" << layer->id();
            const auto cache_key = std::make_tuple(layer_object, texture_zone_id, layer_key_stream.str());
            auto cache_it = m_vertex_color_overhang_weight_field_cache.find(cache_key);
            if (cache_it == m_vertex_color_overhang_weight_field_cache.end()) {
                const float layer_sample_falloff_mm = high_resolution_texture_sampling ?
                    std::max(0.03f, layer_height_mm * 0.5f) :
                    std::max(0.12f, layer_height_mm * 1.5f);
                cache_it = m_vertex_color_overhang_weight_field_cache
                               .emplace(cache_key,
                                        build_vertex_color_weight_field_for_gcode(*layer_object,
                                                                                  component_colors,
                                                                                  raw_texture_mapping_mode,
                                                                                  texture_filament_color_mode,
                                                                                  texture_force_sequential_filaments,
                                                                                  generic_solver_lookup_mode,
                                                                                  generic_solver_mode,
                                                                                  generic_solver_mix_model,
                                                                                  use_legacy_fixed_color_mode,
                                                                                  dithering_enabled,
                                                                                  dithering_method,
                                                                                  dither_pitch_mm,
                                                                                  dither_cell_size_mm,
                                                                                  halftone_dot_size_mm,
                                                                                  component_strength_factors,
                                                                                  component_minimum_offset_factors,
                                                                                  calibrated_side_candidates_ptr,
                                                                                  &m_generic_solver_mix_candidate_cache,
                                                                                  &m_uv_texture_triangle_cache,
                                                                                  texture_filament_overhang_contrast_pct,
                                                                                  texture_tone_gamma,
                                                                                  true,
                                                                                  float(layer->print_z),
                                                                                  layer_sample_falloff_mm,
                                                                                  high_resolution_texture_sampling,
                                                                                  high_speed_image_texture_sampling))
                               .first;
            }
            if (cache_it->second.empty())
                return std::nullopt;
            weight_field = &cache_it->second;
        }

        const float signed_fade_factor =
            texture_mapping_offset_fade_factor(zone->offset_fade_mode, z_progress);
        const float fade_factor = std::abs(signed_fade_factor);
        if (fade_factor <= EPSILON)
            return std::nullopt;

        return SeamLayerTextureState{
            layer,
            layer_height_mm,
            layer_index,
            active_component_id,
            active_component_idx,
            weight_field,
            calibrated_side_mode ? 1.f : texture_mapping_offset_filament_strength_factor(*zone, active_component_id),
            calibrated_side_mode ? 0.f : texture_mapping_offset_filament_minimum_offset_factor(*zone, active_component_id),
            calibrated_side_mode ? 1.f : transmission_distance_width_factor_for_gcode(td_calibration_context, active_component_idx, previous_component_idx),
            signed_fade_factor,
            fade_factor
        };
    };

    const unsigned int current_component_id = unsigned(m_writer.filament()->id() + 1);
    const std::optional<SeamLayerTextureState> current_state = texture_state_for_layer(m_layer, current_component_id);
    const std::optional<SeamLayerTextureState> upper_state =
        upper_layer != nullptr ? texture_state_for_layer(upper_layer, 0) : std::optional<SeamLayerTextureState>();
    const std::optional<SeamLayerTextureState> lower_state =
        lower_layer != nullptr ? texture_state_for_layer(lower_layer, 0) : std::optional<SeamLayerTextureState>();
    if (!current_state ||
        (upper_layer != nullptr && !upper_state) ||
        (lower_layer != nullptr && !lower_state) ||
        (!upper_state && !lower_state))
        return std::nullopt;
    const bool require_bidirectional_cover = upper_state.has_value() && lower_state.has_value();

    std::vector<float> reference_nozzles;
    reference_nozzles.reserve(component_ids.size() + 2);
    auto append_nozzle = [&reference_nozzles, this](unsigned int component_id) {
        if (component_id == 0)
            return;
        const size_t idx = size_t(component_id - 1);
        if (idx < m_config.nozzle_diameter.values.size())
            reference_nozzles.emplace_back(float(m_config.nozzle_diameter.get_at(idx)));
    };
    for (unsigned int id : component_ids)
        append_nozzle(id);
    append_nozzle(zone->component_a);
    append_nozzle(zone->component_b);

    const float reference_nozzle = reference_nozzles.empty() ?
        float(m_config.nozzle_diameter.values.empty() ? 0.4 : m_config.nozzle_diameter.values.front()) :
        std::accumulate(reference_nozzles.begin(), reference_nozzles.end(), 0.f) / float(reference_nozzles.size());
    const float max_allowed_distance_mm = TextureMappingManager::max_component_surface_offset_mm(reference_nozzle);
    if (max_allowed_distance_mm <= EPSILON)
        return std::nullopt;

    std::vector<float> distances_mm = TextureMappingManager::effective_offset_distances(*zone, component_ids.size(), reference_nozzle);
    std::vector<float> angles_deg = TextureMappingManager::effective_offset_angles(*zone, component_ids.size());
    if (distances_mm.size() != component_ids.size())
        distances_mm.assign(component_ids.size(), 0.f);
    if (angles_deg.size() != component_ids.size())
        angles_deg = TextureMappingManager::default_offset_angles(component_ids.size());
    for (float &a : angles_deg)
        a = normalize_angle_deg_for_gcode(a);

    bool has_nonzero_distance = false;
    if (vertex_color_match_mode) {
        distances_mm.assign(component_ids.size(), max_allowed_distance_mm);
        has_nonzero_distance = max_allowed_distance_mm > EPSILON;
    } else {
        for (float &d : distances_mm) {
            d = std::clamp(d, 0.f, max_allowed_distance_mm);
            has_nonzero_distance = has_nonzero_distance || d > EPSILON;
        }
    }
    if (!has_nonzero_distance)
        return std::nullopt;

    const float global_strength_factor =
        std::clamp(float(m_config.texture_mapping_outer_wall_gradient_global_strength.value) / 100.f, 0.f, 1.f);
    if (global_strength_factor <= EPSILON)
        return std::nullopt;

    struct SeamTextureEnvelope {
        float outer_offset_mm { 0.f };
        float width_delta_mm { 0.f };
    };

    struct SeamAdjacentOcclusion {
        float cover_mm { 0.f };
        float score_mm { 0.f };
    };

    struct SeamHidingCandidate {
        Point  point;
        float  cover_mm { 0.f };
        float  score_mm { 0.f };
        double arc_mm { 0.0 };
        double span_mm { 0.0 };
        double segment_t { 0.0 };
        size_t path_index { 0 };
        size_t segment_index { 0 };
    };

    std::vector<SeamHidingCandidate> candidates;
    double total_length_mm = 0.0;
    float best_score_mm = std::numeric_limits<float>::lowest();
    float min_score_mm = std::numeric_limits<float>::max();
    double weighted_score_mm = 0.0;
    double weighted_length_mm = 0.0;
    SeamHidingCandidate best_candidate;
    const Point object_center = layer_object->bounding_box().center();

    auto texture_envelope_for_state = [&](const SeamLayerTextureState &state,
                                          const ExtrusionPath         &path,
                                          const Point                 &center_point,
                                          double                       outward_x,
                                          double                       outward_y,
                                          float                        reference_nozzle) -> std::optional<SeamTextureEnvelope> {
        const float path_outer_width_mm = std::max(
            0.01f,
            path.width > EPSILON ? path.width : float(m_config.outer_wall_line_width.get_abs_value(reference_nozzle)));
        const float texture_mapping_max_outer_width_mm = std::max(
            0.05f,
            float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
        const float base_outer_width_mm = vertex_color_match_mode ? texture_mapping_max_outer_width_mm : path_outer_width_mm;
        const float flow_reference_width_mm = path_outer_width_mm;
        const float base_centerline_shift_mm = vertex_color_match_mode ? 0.5f * (base_outer_width_mm - flow_reference_width_mm) : 0.f;
        const float layer_height_mm = std::max(
            0.01f,
            path.height > EPSILON ? path.height : state.layer_height_mm);
        const float config_min_gradient_width_mm = std::clamp(
            float(m_config.texture_mapping_outer_wall_gradient_min_line_width.value),
            0.05f,
            base_outer_width_mm);
        const float min_width_for_positive_spacing_mm =
            layer_height_mm * float(1. - 0.25 * PI) + 1e-4f;
        const float safe_min_gradient_width_mm = std::clamp(
            std::max(config_min_gradient_width_mm, min_width_for_positive_spacing_mm),
            0.05f,
            base_outer_width_mm);
        const float max_width_delta_mm = std::max(0.f, base_outer_width_mm - safe_min_gradient_width_mm);
        const float effective_max_width_delta_mm = max_width_delta_mm * global_strength_factor;
        float max_width_delta_limit_mm = std::min(effective_max_width_delta_mm, 2.f * max_allowed_distance_mm);
        if (!std::isfinite(max_width_delta_limit_mm) || max_width_delta_limit_mm <= EPSILON)
            return std::nullopt;

        float inset_strength = 0.f;
        if (vertex_color_match_mode) {
            if (state.weight_field == nullptr || state.weight_field->empty())
                return std::nullopt;
            const float sample_x_mm = unscale<float>(center_point.x());
            const float sample_y_mm = unscale<float>(center_point.y());
            const float desired_strength =
                sample_vertex_color_weight_field_for_gcode(*state.weight_field,
                                                           sample_x_mm,
                                                           sample_y_mm,
                                                           state.active_component_idx,
                                                           high_resolution_texture_sampling,
                                                           compact_offset_mode);
            inset_strength = std::clamp(1.f - desired_strength, 0.f, 1.f);
        } else {
            const float z_progress = object_layer_count > 1 ?
                std::clamp(float(state.layer_index) / float(object_layer_count - 1), 0.f, 1.f) :
                0.f;
            float rotation_deg = 0.f;
            if (zone->offset_rotation_enabled) {
                const float repeated =
                    repeated_rotation_progress_for_gcode(z_progress, std::max(1.f, zone->offset_repeats), zone->offset_reverse_repeats);
                const float direction = zone->offset_clockwise ? -1.f : 1.f;
                rotation_deg = direction * 360.f * zone->offset_rotations * repeated;
            }
            std::vector<float> rotated_angles = angles_deg;
            for (float &a : rotated_angles)
                a = normalize_angle_deg_for_gcode(a + rotation_deg);

            double theta_direction_x = outward_x;
            double theta_direction_y = outward_y;
            if (zone->offset_angle_mode != int(TextureMappingZone::OffsetAngleSurfaceNormal)) {
                const double radial_x = double(center_point.x()) - double(object_center.x());
                const double radial_y = double(center_point.y()) - double(object_center.y());
                const double radial_len = std::hypot(radial_x, radial_y);
                if (radial_len > EPSILON) {
                    theta_direction_x = radial_x / radial_len;
                    theta_direction_y = radial_y / radial_len;
                }
            }

            const float theta_deg =
                normalize_angle_deg_for_gcode(float(Geometry::rad2deg(std::atan2(theta_direction_y, theta_direction_x))));
            const float sample_theta_deg = state.signed_fade_factor < 0.f ?
                normalize_angle_deg_for_gcode(theta_deg + 180.f) :
                theta_deg;
            float raw_inset_mm = 0.f;
            for (size_t i = 0; i < component_ids.size(); ++i) {
                if (i == state.active_component_idx)
                    continue;
                const float influence = component_angular_influence_for_gcode(component_ids[i],
                                                                               sample_theta_deg,
                                                                               component_ids,
                                                                               rotated_angles);
                raw_inset_mm += distances_mm[i] * influence;
            }
            inset_strength = std::clamp(raw_inset_mm / std::max(max_allowed_distance_mm, float(EPSILON)), 0.f, 1.f);
        }
        inset_strength = std::clamp(inset_strength * state.fade_factor, 0.f, 1.f);
        const float stair_step_mm = nonlinear_offset_adjustment ?
            local_surface_stair_step_distance_for_gcode(state.layer,
                                                        center_point,
                                                        outward_x,
                                                        outward_y,
                                                        base_outer_width_mm,
                                                        max_allowed_distance_mm) :
            std::numeric_limits<float>::quiet_NaN();
        const float variable_width_delta_mm =
            variable_width_delta_for_visibility_range_for_gcode(inset_strength,
                                                                max_width_delta_limit_mm,
                                                                state.active_component_minimum_offset_factor,
                                                                state.active_component_strength_factor,
                                                                state.active_component_td_width_factor,
                                                                nonlinear_offset_adjustment,
                                                                layer_height_mm,
                                                                stair_step_mm);
        const float width_delta_mm = std::clamp(variable_width_delta_mm, 0.f, max_width_delta_limit_mm);
        if (!std::isfinite(width_delta_mm))
            return std::nullopt;

        const float target_width_mm = base_outer_width_mm - width_delta_mm;
        if (!std::isfinite(target_width_mm) || target_width_mm <= 0.f)
            return std::nullopt;

        const float centerline_shift_mm = base_centerline_shift_mm + 0.5f * width_delta_mm;
        const float centerline_outward_shift_mm = -centerline_shift_mm;
        const float outer_offset_mm = centerline_outward_shift_mm + 0.5f * target_width_mm;
        if (!std::isfinite(outer_offset_mm))
            return std::nullopt;

        return SeamTextureEnvelope{ outer_offset_mm, width_delta_mm };
    };

    auto scaled_offset_point = [](const Point &point, double dir_x, double dir_y, float distance_mm) {
        const double distance_scaled = scale_(double(distance_mm));
        return Point(coord_t(std::llround(double(point.x()) + dir_x * distance_scaled)),
                     coord_t(std::llround(double(point.y()) + dir_y * distance_scaled)));
    };

    std::vector<const ExtrusionPath *> external_paths;
    external_paths.reserve(loop.paths.size());
    for (const ExtrusionPath &path : loop.paths)
        if (is_external_perimeter(path.role()) && path.polyline.points.size() >= 2)
            external_paths.push_back(&path);

    auto evaluate_sample_for_path = [&](const ExtrusionPath &path,
                                        size_t               path_index,
                                        size_t               segment_index,
                                        double               segment_t,
                                        double               arc_mm,
                                        double               span_mm) -> std::optional<SeamHidingCandidate> {
        const Points &points = path.polyline.points;
        if (segment_index == 0 || segment_index >= points.size())
            return std::nullopt;

        const float path_outer_width_mm = std::max(
            0.01f,
            path.width > EPSILON ? path.width : float(m_config.outer_wall_line_width.get_abs_value(reference_nozzle)));
        const float texture_mapping_max_outer_width_mm = std::max(
            0.05f,
            float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
        const float base_outer_width_mm = vertex_color_match_mode ? texture_mapping_max_outer_width_mm : path_outer_width_mm;
        const float flow_reference_width_mm = path_outer_width_mm;
        const double half_flow_reference_scaled = scale_(0.5 * double(flow_reference_width_mm));
        const float max_local_edge_tangent_delta_mm = std::max(0.75f, base_outer_width_mm * 1.5f);
        const float max_local_edge_normal_delta_mm =
            std::max(1.25f, base_outer_width_mm * 3.f + 2.f * max_allowed_distance_mm);
        const Point &a = points[segment_index - 1];
        const Point &b = points[segment_index];
        const double ax = double(a.x());
        const double ay = double(a.y());
        const double bx = double(b.x());
        const double by = double(b.y());
        const double dx_scaled = bx - ax;
        const double dy_scaled = by - ay;
        const double len_scaled = std::hypot(dx_scaled, dy_scaled);
        if (len_scaled <= EPSILON)
            return std::nullopt;

        const Point sample_point(coord_t(std::llround(ax + segment_t * dx_scaled)),
                                 coord_t(std::llround(ay + segment_t * dy_scaled)));
        double outward_x = 0.0;
        double outward_y = 0.0;
        resolve_segment_shift_outward_normal_for_gcode(m_layer,
                                                       sample_point,
                                                       dx_scaled,
                                                       dy_scaled,
                                                       len_scaled,
                                                       double(sample_point.x()) - double(object_center.x()),
                                                       double(sample_point.y()) - double(object_center.y()),
                                                       outward_x,
                                                       outward_y);

        const double tangent_x = dx_scaled / len_scaled;
        const double tangent_y = dy_scaled / len_scaled;
        const std::optional<SeamTextureEnvelope> current_envelope =
            texture_envelope_for_state(*current_state, path, sample_point, outward_x, outward_y, reference_nozzle);
        if (!current_envelope)
            return std::nullopt;

        const Point current_outer_edge = scaled_offset_point(sample_point,
                                                             outward_x,
                                                             outward_y,
                                                             current_envelope->outer_offset_mm);

        auto evaluate_adjacent_occlusion =
            [&](const std::optional<SeamLayerTextureState> &adjacent_state) -> std::optional<SeamAdjacentOcclusion> {
            if (!adjacent_state)
                return std::nullopt;

            const std::optional<NormalAwareLayerSliceBoundaryPointForGCode> adjacent_boundary =
                find_normal_aware_layer_slice_boundary_point_for_gcode(adjacent_state->layer,
                                                                       current_outer_edge,
                                                                       outward_x,
                                                                       outward_y,
                                                                       tangent_x,
                                                                       tangent_y,
                                                                       max_local_edge_normal_delta_mm,
                                                                       max_local_edge_tangent_delta_mm);
            if (!adjacent_boundary)
                return std::nullopt;

            const Point adjacent_centerline(
                coord_t(std::llround(double(adjacent_boundary->point.x()) - adjacent_boundary->outward_x * half_flow_reference_scaled)),
                coord_t(std::llround(double(adjacent_boundary->point.y()) - adjacent_boundary->outward_y * half_flow_reference_scaled)));
            const std::optional<SeamTextureEnvelope> adjacent_envelope =
                texture_envelope_for_state(*adjacent_state,
                                           path,
                                           adjacent_centerline,
                                           adjacent_boundary->outward_x,
                                           adjacent_boundary->outward_y,
                                           reference_nozzle);
            if (!adjacent_envelope)
                return std::nullopt;

            const Point adjacent_outer_edge = scaled_offset_point(adjacent_centerline,
                                                                  adjacent_boundary->outward_x,
                                                                  adjacent_boundary->outward_y,
                                                                  adjacent_envelope->outer_offset_mm);
            const double cover_scaled =
                (double(adjacent_outer_edge.x()) - double(current_outer_edge.x())) * outward_x +
                (double(adjacent_outer_edge.y()) - double(current_outer_edge.y())) * outward_y;
            const float cover_mm = unscale<float>(cover_scaled);
            if (!std::isfinite(cover_mm))
                return std::nullopt;

            const float texture_cover_bonus_mm = std::max(0.f, current_envelope->width_delta_mm - adjacent_envelope->width_delta_mm);
            const float score_mm = cover_mm > 0.f ?
                std::max(0.f, cover_mm + 0.25f * texture_cover_bonus_mm - 0.2f * adjacent_boundary->tangent_delta_mm) :
                0.f;
            return SeamAdjacentOcclusion{ std::max(0.f, cover_mm), score_mm };
        };

        const std::optional<SeamAdjacentOcclusion> upper_occlusion = evaluate_adjacent_occlusion(upper_state);
        const std::optional<SeamAdjacentOcclusion> lower_occlusion = evaluate_adjacent_occlusion(lower_state);
        float cover_mm = 0.f;
        float score_mm = 0.f;
        if (require_bidirectional_cover) {
            if (!upper_occlusion || !lower_occlusion)
                return std::nullopt;

            cover_mm = std::min(upper_occlusion->cover_mm, lower_occlusion->cover_mm);
            const float weaker_score_mm = std::min(upper_occlusion->score_mm, lower_occlusion->score_mm);
            const float stronger_score_mm = std::max(upper_occlusion->score_mm, lower_occlusion->score_mm);
            if (cover_mm > 0.f && weaker_score_mm > 0.f)
                score_mm = weaker_score_mm + 0.35f * stronger_score_mm;
        } else {
            const std::optional<SeamAdjacentOcclusion> &occlusion = upper_occlusion ? upper_occlusion : lower_occlusion;
            if (!occlusion)
                return std::nullopt;
            cover_mm = occlusion->cover_mm;
            score_mm = occlusion->score_mm;
        }

        return SeamHidingCandidate{
            sample_point,
            cover_mm,
            score_mm,
            arc_mm,
            span_mm,
            segment_t,
            path_index,
            segment_index
        };
    };

    for (size_t external_path_idx = 0; external_path_idx < external_paths.size(); ++external_path_idx) {
        const ExtrusionPath *path_ptr = external_paths[external_path_idx];
        const ExtrusionPath &path = *path_ptr;
        if (!is_external_perimeter(path.role()) || path.polyline.points.size() < 2)
            continue;

        const float path_outer_width_mm = std::max(
            0.01f,
            path.width > EPSILON ? path.width : float(m_config.outer_wall_line_width.get_abs_value(reference_nozzle)));
        const float texture_mapping_max_outer_width_mm = std::max(
            0.05f,
            float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
        const float base_outer_width_mm = vertex_color_match_mode ? texture_mapping_max_outer_width_mm : path_outer_width_mm;
        const float sample_step_mm = std::clamp(0.5f * base_outer_width_mm, 0.15f, 0.5f);
        const Points &points = path.polyline.points;
        const size_t path_index = external_path_idx;
        double path_arc_start_mm = total_length_mm;

        auto evaluate_sample = [&](size_t segment_index,
                                   double segment_t,
                                   double arc_mm,
                                   double span_mm,
                                   bool add_candidate) -> std::optional<SeamHidingCandidate> {
            std::optional<SeamHidingCandidate> candidate =
                evaluate_sample_for_path(path, path_index, segment_index, segment_t, arc_mm, span_mm);
            if (!candidate)
                return std::nullopt;

            if (add_candidate) {
                candidates.push_back(*candidate);
                min_score_mm = std::min(min_score_mm, candidate->score_mm);
                weighted_score_mm += double(candidate->score_mm) * span_mm;
                weighted_length_mm += span_mm;
                if (candidate->score_mm > best_score_mm) {
                    best_score_mm = candidate->score_mm;
                    best_candidate = *candidate;
                }
            }

            return candidate;
        };

        for (size_t point_idx = 1; point_idx < points.size(); ++point_idx) {
            const Point &a = points[point_idx - 1];
            const Point &b = points[point_idx];
            const double len_mm = unscale<double>((b - a).cast<double>().norm());
            if (len_mm <= EPSILON)
                continue;

            const int sample_count = std::max(1, int(std::ceil(len_mm / sample_step_mm)));
            const double span_mm = len_mm / double(sample_count);
            for (int sample_idx = 0; sample_idx <= sample_count; ++sample_idx) {
                if (point_idx > 1 && sample_idx == 0)
                    continue;
                const double t = double(sample_idx) / double(sample_count);
                evaluate_sample(point_idx,
                                t,
                                path_arc_start_mm + t * len_mm,
                                span_mm,
                                true);
            }

            path_arc_start_mm += len_mm;
        }

        total_length_mm = path_arc_start_mm;
    }

    if (candidates.empty() || total_length_mm <= EPSILON || best_score_mm <= 0.f || !std::isfinite(best_score_mm))
        return std::nullopt;

    const ExtrusionPath *best_path = nullptr;
    size_t external_path_idx = 0;
    for (const ExtrusionPath *path_ptr : external_paths) {
        if (external_path_idx == best_candidate.path_index) {
            best_path = path_ptr;
            break;
        }
        ++external_path_idx;
    }
    if (best_path != nullptr &&
        best_candidate.segment_index > 0 &&
        best_candidate.segment_index < best_path->polyline.points.size()) {
        const Point &a = best_path->polyline.points[best_candidate.segment_index - 1];
        const Point &b = best_path->polyline.points[best_candidate.segment_index];
        const double len_mm = unscale<double>((b - a).cast<double>().norm());
        if (len_mm > EPSILON) {
            const float best_path_outer_width_mm = std::max(
                0.01f,
                best_path->width > EPSILON ? best_path->width : float(m_config.outer_wall_line_width.get_abs_value(reference_nozzle)));
            const float best_texture_mapping_max_outer_width_mm = std::max(
                0.05f,
                float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
            const float best_base_outer_width_mm =
                vertex_color_match_mode ? best_texture_mapping_max_outer_width_mm : best_path_outer_width_mm;
            const double delta_t = std::clamp(
                0.25 * std::clamp(0.5f * best_base_outer_width_mm, 0.15f, 0.5f) / len_mm,
                0.02,
                0.35);
            for (double t : {
                     std::clamp(best_candidate.segment_t - 2.0 * delta_t, 0.0, 1.0),
                     std::clamp(best_candidate.segment_t - delta_t, 0.0, 1.0),
                     std::clamp(best_candidate.segment_t + delta_t, 0.0, 1.0),
                     std::clamp(best_candidate.segment_t + 2.0 * delta_t, 0.0, 1.0) }) {
                if (std::abs(t - best_candidate.segment_t) <= 1e-5)
                    continue;
                const std::optional<SeamHidingCandidate> refined =
                    evaluate_sample_for_path(*best_path,
                                             best_candidate.path_index,
                                             best_candidate.segment_index,
                                             t,
                                             best_candidate.arc_mm + (t - best_candidate.segment_t) * len_mm,
                                             0.25 * len_mm);
                if (refined && refined->score_mm > best_score_mm) {
                    best_score_mm = refined->score_mm;
                    best_candidate = *refined;
                }
            }
        }
    }

    if (best_score_mm <= 0.f || !std::isfinite(best_score_mm))
        return std::nullopt;

    const float useful_score_mm =
        std::max(0.015f, std::min(0.08f, 0.05f * float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value)));
    if (best_score_mm < useful_score_mm)
        return std::nullopt;

    const float score_range_mm = best_score_mm - min_score_mm;
    const double mean_score_mm = weighted_length_mm > EPSILON ? weighted_score_mm / weighted_length_mm : 0.0;
    const double seam_gap_mm = m_config.seam_gap.get_abs_value(reference_nozzle);
    const double required_local_length_mm = std::max({ seam_gap_mm, double(reference_nozzle), 0.4 });
    const float strong_score_mm = std::max(useful_score_mm, best_score_mm * 0.55f);
    double local_support_mm = 0.0;
    for (const SeamHidingCandidate &candidate : candidates) {
        double arc_distance_mm = std::abs(candidate.arc_mm - best_candidate.arc_mm);
        arc_distance_mm = std::min(arc_distance_mm, total_length_mm - arc_distance_mm);
        if (arc_distance_mm <= 0.5 * required_local_length_mm && candidate.score_mm >= strong_score_mm)
            local_support_mm += candidate.span_mm;
    }

    const bool has_local_support = local_support_mm >= 0.65 * required_local_length_mm;
    const bool clearly_stronger =
        score_range_mm >= std::max(0.025f, best_score_mm * 0.35f) &&
        best_score_mm >= float(mean_score_mm) + std::max(0.025f, best_score_mm * 0.30f);
    if (!has_local_support && !clearly_stronger)
        return std::nullopt;

    const float cover_confidence = std::clamp(best_candidate.cover_mm / std::max(0.05f, 0.25f * reference_nozzle), 0.f, 1.f);
    const float support_confidence = std::clamp(float(local_support_mm / std::max(required_local_length_mm, 1e-6)), 0.f, 1.f);
    const float contrast_confidence = std::clamp((best_score_mm - float(mean_score_mm)) / std::max(best_score_mm, 1e-6f), 0.f, 1.f);
    const float confidence =
        std::clamp(0.35f + 0.45f * cover_confidence + 0.20f * std::max(support_confidence, contrast_confidence), 0.f, 1.f);

    return PreferredSeamPoint{ best_candidate.point, best_candidate.cover_mm, confidence };
}


std::vector<double> GCode::texture_mapping_path_flow_scales(const ExtrusionPath &path)
{
    std::vector<double> scales;
    if (path.is_force_no_extrusion() || !is_external_perimeter(path.role()) || path.polyline.points.size() < 2)
        return scales;

    if (m_curr_print == nullptr || m_writer.filament() == nullptr)
        return scales;

    const size_t num_physical = m_config.filament_colour.values.size();
    const unsigned int texture_zone_id = unsigned(std::max(0, m_config.wall_filament.value));
    const TextureMappingManager &texture_mgr = m_curr_print->texture_mapping_manager();
    if (num_physical == 0 || texture_zone_id == 0 || !texture_mgr.is_texture_mapping_zone_id(texture_zone_id))
        return scales;

    const TextureMappingZone *zone = texture_mgr.zone_from_id(texture_zone_id);
    if (zone == nullptr)
        return scales;
    if (!is_horizontal_overhang_gradient_row_for_gcode(*zone) &&
        !is_vertex_color_match_overhang_row_for_gcode(*zone) &&
        !is_surface_offset_gradient_row_for_gcode(*zone) &&
        !has_explicit_offset_gradient_profile_for_gcode(*zone))
        return scales;

    const Layer *layer = m_layer;
    if (layer == nullptr || layer->object() == nullptr)
        return scales;

    const PrintObject *object = layer->object();
    const unsigned int active_component_id = unsigned(m_writer.filament()->id() + 1);
    std::optional<TextureMappingOffsetContext> offset_context =
        build_texture_mapping_offset_context_for_layer(*object, *layer, *zone, texture_zone_id, active_component_id);
    if (!offset_context)
        return scales;

    const float base_width_mm = std::max(0.05f, float(path.width));
    const float height_mm = std::max(0.01f, float(path.height));
    const float max_width_mm = std::max(0.05f, float(m_config.texture_mapping_outer_wall_gradient_max_line_width.value));
    const float min_width_mm = std::clamp(float(m_config.texture_mapping_outer_wall_gradient_min_line_width.value), 0.05f, max_width_mm);

    const auto lines = path.polyline.lines();
    scales.reserve(lines.size());
    for (const Line &line : lines) {
        const double ax = double(line.a.x());
        const double ay = double(line.a.y());
        const double bx = double(line.b.x());
        const double by = double(line.b.y());
        const double dx = bx - ax;
        const double dy = by - ay;
        const double len = std::hypot(dx, dy);
        if (len <= EPSILON) {
            scales.emplace_back(1.0);
            continue;
        }
        const Point mid_point(coord_t(std::llround(0.5 * (ax + bx))), coord_t(std::llround(0.5 * (ay + by))));
        double outward_x = 0.0;
        double outward_y = 0.0;
        const double radial_x = 0.5 * (ax + bx);
        const double radial_y = 0.5 * (ay + by);
        resolve_segment_shift_outward_normal_for_gcode(layer, mid_point, dx, dy, len, radial_x, radial_y, outward_x, outward_y);
        const float inset_mm = texture_mapping_offset_surface_inset_mm(*offset_context, mid_point, -outward_x, -outward_y);
        float target_width_mm = std::clamp(base_width_mm - inset_mm, min_width_mm, max_width_mm);
        if (!std::isfinite(target_width_mm))
            target_width_mm = base_width_mm;
        const double flow_scale = flow_scale_for_target_width_for_gcode(base_width_mm, target_width_mm, height_mm);
        scales.emplace_back(std::isfinite(flow_scale) && flow_scale > 0.0 ? flow_scale : 1.0);
    }
    if (std::all_of(scales.begin(), scales.end(), [](double s) { return std::abs(s - 1.0) <= 1e-6; }))
        scales.clear();
    return scales;
}

} // namespace Slic3r
