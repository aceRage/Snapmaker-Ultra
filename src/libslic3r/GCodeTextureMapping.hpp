// original author: sentientstardust
// ImageMap FULL PR3: texture / Contoning / seam-hiding G-code helpers extracted from
// OrcaSlicer-ImageMap @ 92548381056. Call sites stay in GCode.cpp.

#ifndef slic3r_GCodeTextureMapping_hpp_
#define slic3r_GCodeTextureMapping_hpp_

#include "ColorSolver.hpp"
#include "Point.hpp"
#include "PrintConfig.hpp"

#include <array>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <tuple>
#include <vector>

namespace Slic3r {

class ModelVolume;
class Print;

using GCodeGenericMixCandidateSet = ColorSolverCandidateSet;

struct VertexColorOverhangWeightField {
    float min_x_mm { 0.f };
    float min_y_mm { 0.f };
    float bucket_width_mm { 1.f };
    float bucket_height_mm { 1.f };
    int bucket_width { 0 };
    int bucket_height { 0 };
    size_t component_count { 0 };
    std::vector<float> sample_x_mm;
    std::vector<float> sample_y_mm;
    std::vector<float> sample_weight;
    std::vector<float> sample_component_weights;
    std::vector<std::vector<uint32_t>> buckets;
    std::vector<float> fallback_weights;
    bool raw_component_weights_from_texture { false };
    bool binary_dithered { false };

    bool empty() const
    {
        return bucket_width <= 0 ||
               bucket_height <= 0 ||
               component_count == 0 ||
               sample_x_mm.empty() ||
               sample_y_mm.size() != sample_x_mm.size() ||
               sample_weight.size() != sample_x_mm.size() ||
               sample_component_weights.size() != sample_x_mm.size() * component_count;
    }
};

struct GCodeUVTextureTriangleMetadata {
    const ModelVolume *volume { nullptr };
    Vec3d p0;
    Vec3d p1;
    Vec3d p2;
    std::array<Vec2f, 3> uv;
    float min_z { 0.f };
    float max_z { 0.f };
    float max_uv_edge_texel { 0.f };
    float max_world_edge_mm { 0.f };
    double area_mm2 { 0.0 };
};

struct GCodeUVTextureVolumeMetadata {
    const ModelVolume *volume { nullptr };
    std::vector<GCodeUVTextureTriangleMetadata> triangles;
    std::vector<std::vector<uint32_t>> z_bins;
    std::vector<uint32_t> fallback_triangle_indices;
    float min_z { 0.f };
    float max_z { 0.f };
    float z_bin_step_mm { 1.f };
};

struct GCodeUVTextureTriangleCache {
    std::vector<GCodeUVTextureVolumeMetadata> volumes;
};

bool print_has_raw_offset_texture_zone_without_raw_data_for_gcode(const Print &print);
std::vector<std::string> collect_raw_atlas_warnings_for_gcode(const Print &print);

} // namespace Slic3r

#endif
