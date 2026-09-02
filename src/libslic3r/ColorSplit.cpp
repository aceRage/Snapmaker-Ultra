// Split by painted colour: the public API, the depth model and the one-shot pipeline. The two heavy stages
// live next door - ColorSplitShell.cpp builds the shells, ColorSplitPartition.cpp runs the Manifold booleans.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "ColorSplit.hpp"
#include "ColorSplitInternal.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "MeshBoolean.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <string>
#include <vector>

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

namespace ColorSplitDetail {

// The dialog's depth override wins over both the depth AND the unlimited flag. Every entry point has to
// apply it identically - build_color_shells to cut with, color_split_refine_length to size the refinement,
// split_volume_by_paint to report back - so none of them gets to spell the rule out for itself.
ColorSplitDepths effective_depths(const ColorSplitDepths &depths, const ColorSplitParams &params)
{
    ColorSplitDepths out = depths;
    if (params.depth_override_mm > 0.) { out.D = params.depth_override_mm; out.unlimited = false; }
    return out;
}

float half_thickness_along(const AABBMesh &aabb, const Vec3f &v, const Vec3f &dir)
{
    const double eps = 1e-3;
    const Vec3d  n   = dir.cast<double>();
    const Vec3d  src = v.cast<double>() - eps * n;
    double t = std::numeric_limits<double>::infinity();
    // AABBMesh::query_ray_hits sorts hits by distance (AABBMesh.cpp:187-189), so the first hit past the
    // self-intersection radius is already the nearest one.
    for (const AABBMesh::hit_result &hit : aabb.query_ray_hits(src, -n))
        if (hit.is_hit() && hit.distance() > 5. * eps) { t = hit.distance() + eps; break; }
    // std::max(0., ...): on a feature thinner than 2 * delta the clamp t/2 - delta turns negative, which
    // would push the bottom copies OUTSIDE the part. The fold guard cannot catch that - where the offset
    // is parallel (one flat patch, one normal) a negative depth translates the bottom triangle without
    // changing its orientation, so the guard's orientation test still passes.
    return float(std::max(0., t / 2. - 0.002));
}

std::vector<float> vertex_depths(const AABBMesh &aabb, const ColorPatches &p, const std::vector<Vec3f> &normals, double D)
{
    const float cap = float(std::isfinite(D) ? D : std::numeric_limits<float>::max());
    std::vector<float> d(p.surface.vertices.size());
    for (size_t v = 0; v < p.surface.vertices.size(); ++v)
        d[v] = std::min(cap, half_thickness_along(aabb, p.surface.vertices[v], normals[v]));
    return d;
}

} // namespace ColorSplitDetail

std::vector<float> compute_vertex_depths(const ColorPatches &p, const std::vector<Vec3f> &normals, double D)
{
    return ColorSplitDetail::vertex_depths(AABBMesh(p.surface), p, normals, D);
}

double color_split_refine_length(const ColorSplitDepths &depths_in, const ColorSplitParams &params, const BoundingBoxf3 &mesh_bbox)
{
    const ColorSplitDepths depths = ColorSplitDetail::effective_depths(depths_in, params);
    const double D = depths.unlimited ? std::numeric_limits<double>::infinity() : depths.D;
    // Spec 3.1a: fine enough that a feature of depth D gets vertices inside it, never finer than one wall
    // stack (below that the extra triangles buy nothing a nozzle can print), and never coarser than a
    // twentieth of the part - which is what makes the length scale-free on an unlimited-depth split.
    return std::max(depths.ws, std::min(D, mesh_bbox.size().norm() / 20.));
}

ShellCheck check_shell(const indexed_triangle_set &shell)
{
    ShellCheck c;
    c.closed = its_num_open_edges(shell) == 0;
    c.volume = double(its_volume(shell));        // signed: positive for an outward-oriented closed mesh
    c.self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(shell));
    return c;
}

ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress)
{
    if (progress && !progress(0)) throw ColorSplitCancelled();
    ColorPatches patches = extract_color_patches(mesh, paint);
    if (patches.states.empty()) throw ColorSplitError("The part has no painted colours.");
    if (progress && !progress(5)) throw ColorSplitCancelled();
    // Spec 3.1a: refine BEFORE the normals and the shells - they are what the missing interior vertices
    // would have starved (a two-ring cylinder has no side vertex whose normal is radial).
    BoundingBoxf3 bbox;
    for (const Vec3f &v : patches.surface.vertices) bbox.merge(v.cast<double>());
    patches = refine_color_patches(patches, color_split_refine_length(depths, params, bbox));
    if (progress && !progress(10)) throw ColorSplitCancelled();
    std::vector<std::string> shell_warnings;
    // Ruling 10: skipped micro-components are warnings, not errors - they have to reach the caller.
    std::vector<ColorShell> shells = build_color_shells(patches, depths, params, progress, &shell_warnings);
    ColorSplitResult r = partition_by_shells(patches.surface, shells, params.absorb_islands, progress);
    r.warnings.insert(r.warnings.begin(), shell_warnings.begin(), shell_warnings.end());
    r.depths = ColorSplitDetail::effective_depths(depths, params);
    if (progress) progress(100);
    return r;
}

} // namespace Slic3r
