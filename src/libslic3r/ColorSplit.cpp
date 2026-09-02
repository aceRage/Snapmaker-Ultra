#include "ColorSplit.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

namespace Slic3r {

ColorPatches extract_color_patches(const indexed_triangle_set &mesh_in, const TriangleSelector::TriangleSplittingData &paint)
{
    // Weld exact duplicates so index adjacency (what TriangleSelector uses for T-joint resolution) sees the
    // real topology. its_merge_vertices keeps face order, so the paint's facet indexing stays valid.
    indexed_triangle_set mesh = mesh_in;
    its_merge_vertices(mesh);
    if (its_num_open_edges(mesh) != 0)
        throw ColorSplitError("The part is not watertight; repair it before splitting by colour.");

    TriangleMesh tm(mesh);
    TriangleSelector sel(tm);
    sel.deserialize(paint, /*needs_reset=*/false);

    ColorPatches out;
    std::vector<int> states;
    for (size_t s = 1; s < paint.used_states.size(); ++s)
        if (paint.used_states[s] && sel.has_facets(EnforcerBlockerType(s)))
            states.push_back(int(s));
    states.push_back(0); // unpainted remainder, processed last

    // All get_facets_strict calls from ONE selector share the same vertex pool (TriangleSelector.cpp:1478-1502),
    // so indices from different states refer to the same coordinates and can simply be concatenated.
    bool first = true;
    for (int s : states) {
        indexed_triangle_set part = sel.get_facets_strict(EnforcerBlockerType(s));
        if (first) { out.surface.vertices = part.vertices; first = false; }
        if (part.vertices.size() != out.surface.vertices.size())
            throw ColorSplitError("Paint data does not share one vertex pool (internal error).");
        for (const Vec3i32 &t : part.indices) {
            out.surface.indices.push_back(t);
            out.facet_state.push_back(s);
        }
    }
    its_compactify_vertices(out.surface, /*shrink_to_fit=*/true);
    if (its_num_open_edges(out.surface) != 0)
        throw ColorSplitError("Paint data does not cover the surface consistently (internal error).");
    states.pop_back();
    out.states = std::move(states);
    return out;
}

static int effective_shell_layers(int n_layers, double thickness, double h)
{
    // Mirrors effective_shell_layers_by_thickness (MultiMaterialSegmentation.cpp:1381-1420) for uniform layers:
    // a zero count means no shell at all; otherwise the larger of the count and the layers spanning the thickness.
    if (n_layers <= 0) return 0;
    int by_thickness = thickness > 0. ? int(std::ceil(thickness / h - EPSILON)) : 0;
    return std::max(n_layers, by_thickness);
}

ColorSplitDepths color_split_depths(const DynamicPrintConfig &cfg, const std::vector<int> &filaments)
{
    if (filaments.empty())
        throw ColorSplitError("No filaments to derive the split depth from.");
    ColorSplitDepths out;
    const double h = cfg.opt_float("layer_height");
    out.layer_height = h;
    const PaintDepthMode mode  = cfg.opt_enum<PaintDepthMode>("paint_depth_mode");
    const int            walls = cfg.opt_int("paint_depth_walls");
    const double         mm    = cfg.opt_float("paint_depth_mm");
    const bool classic = cfg.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Classic;
    ConfigOptionFloatOrPercent ext_w = *cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    ConfigOptionFloatOrPercent per_w = *cfg.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
    if (ext_w.value == 0) ext_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");   // PrintRegion.cpp:95-96
    if (per_w.value == 0) per_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");
    const auto &nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values;
    if (nozzles.empty())
        throw ColorSplitError("Printer profile has no nozzle diameters.");
    out.unlimited = mode == pdmUnlimited;
    for (int f : filaments) {
        const float nozzle = float(nozzles[std::min<size_t>(std::max(f, 1) - 1, nozzles.size() - 1)]);
        Flow ext = Flow::new_from_config_width(frExternalPerimeter, ext_w, nozzle, float(h));
        Flow per = Flow::new_from_config_width(frPerimeter,         per_w, nozzle, float(h));
        float band = paint_depth_band_mm(mode, walls, mm, ext.width(), ext.spacing(), per.spacing());
        if (classic) band = paint_depth_band_classic_floor_mm(band, ext.width(), ext.spacing());
        out.D  = std::max(out.D,  double(band));
        out.ws = std::max(out.ws, double(ext.width() + ext.spacing()));
    }
    int n_top = effective_shell_layers(cfg.opt_int("top_shell_layers"),    cfg.opt_float("top_shell_thickness"),    h);
    int n_bot = effective_shell_layers(cfg.opt_int("bottom_shell_layers"), cfg.opt_float("bottom_shell_thickness"), h);
    out.cap_top    = std::max(h, n_top * h);
    out.cap_bottom = std::max(h, n_bot * h);
    return out;
}

std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface)
{
    // Spec 3.2: angle-weighted vertex normals - triangulation-independent, so a cube edge gets the exact
    // 45 degree bisector - always computed on the FULL surface F. NormalUtils::Normals is already
    // std::vector<Vec3f> (NormalUtils.hpp:17), so no copy is needed - but create_normals_angle_weighted
    // (NormalUtils.cpp:89-92) only divides by the summed angle weight, it does not renormalize to unit
    // length, so that step is still required here.
    std::vector<Vec3f> normals = NormalUtils::create_normals(surface, NormalUtils::VertexNormalType::AngleWeighted);
    for (Vec3f &n : normals) n.normalize();
    return normals;
}

std::vector<float> compute_vertex_depths(const ColorPatches &p, const std::vector<Vec3f> &normals, double D)
{
    AABBMesh aabb(p.surface);
    const double eps = 1e-3;
    std::vector<float> d(p.surface.vertices.size(), float(std::isfinite(D) ? D : std::numeric_limits<float>::max()));
    for (size_t v = 0; v < p.surface.vertices.size(); ++v) {
        const Vec3d n   = normals[v].cast<double>();
        const Vec3d src = p.surface.vertices[v].cast<double>() - eps * n;
        double t = std::numeric_limits<double>::infinity();
        // AABBMesh::query_ray_hits sorts hits by distance (AABBMesh.cpp:187-189), so the first hit past the
        // self-intersection radius is already the nearest one.
        for (const AABBMesh::hit_result &hit : aabb.query_ray_hits(src, -n))
            if (hit.is_hit() && hit.distance() > 5. * eps) { t = hit.distance() + eps; break; }
        d[v] = float(std::min(double(d[v]), t / 2. - 0.002));   // delta keeps the bottom strictly short of the mid-surface
    }
    return d;
}

} // namespace Slic3r
