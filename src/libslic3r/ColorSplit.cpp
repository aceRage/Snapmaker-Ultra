#include "ColorSplit.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "MeshBoolean.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
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

namespace {

// Edge-connected components of the facet subset `in_set` (indices into patches.surface).
std::vector<std::vector<int>> connected_components(const ColorPatches &p, const std::vector<Vec3i32> &nbrs, const std::vector<char> &in_set)
{
    std::vector<std::vector<int>> comps;
    std::vector<char> seen(p.surface.indices.size(), 0);
    for (int seed = 0; seed < int(p.surface.indices.size()); ++seed) {
        if (!in_set[seed] || seen[seed]) continue;
        std::vector<int> comp, stack{seed};
        seen[seed] = 1;
        while (!stack.empty()) {
            int f = stack.back(); stack.pop_back();
            comp.push_back(f);
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n >= 0 && in_set[n] && !seen[n]) { seen[n] = 1; stack.push_back(n); }
            }
        }
        comps.push_back(std::move(comp));
    }
    return comps;
}

// Spec 3.7: turns one facet group into a closed solid by offsetting its vertices inward along -n(v).
// its_face_neighbors convention (MeshSplitImpl.hpp create_face_neighbors_index via its_triangle_edge,
// TriangleMesh.hpp:253-257): neighbour k sits across the edge (v[k], v[(k+1)%3]).
struct ShellBuilder {
    const ColorPatches         &p;
    const std::vector<Vec3i32> &nbrs;          // its_face_neighbors(p.surface)
    const std::vector<Vec3f>   &normals;       // color_split_normals
    std::vector<float>          depth;         // per vertex (copy: the fold guard lowers it)
    const ColorSplitDepths     &depths;

    // Builds the closed shell of the facet group `group` (indices into p.surface).
    indexed_triangle_set build(const std::vector<int> &group, double cap_depth /* 0 = none */) const
    {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (int f : group) in[f] = 1;

        // Boundary edges of the group, and how many of them each vertex carries.
        std::vector<int> boundary_count(p.surface.vertices.size(), 0);
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n < 0 || !in[n]) {
                    int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                    ++boundary_count[a]; ++boundary_count[b];
                }
            }

        // Per-vertex wedge ids: a boundary vertex with more than 2 boundary edges (pinch) gets one copy per wedge.
        // Wedge = the fan of group triangles around the vertex between an incoming and the next outgoing boundary
        // edge; triangles reachable from each other by rotating around the vertex through GROUP neighbours belong
        // to the same wedge. Duplicating them keeps every shell edge on exactly two faces.
        std::map<std::pair<int, int>, int> wedge_of;   // (vertex, facet) -> wedge index 0..
        std::map<int, int>                 wedges_at;  // vertex -> wedge count
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int v = p.surface.indices[f][k];
                if (boundary_count[v] <= 2) continue;
                if (wedge_of.count({v, f})) continue;
                int w = wedges_at[v]++;
                std::vector<int> stack{f};
                wedge_of[{v, f}] = w;
                while (!stack.empty()) {
                    int g = stack.back(); stack.pop_back();
                    for (int e = 0; e < 3; ++e) {
                        int n = nbrs[g][e];
                        if (n < 0 || !in[n]) continue;
                        // neighbour across an edge that contains v -> same wedge
                        int ea = p.surface.indices[g][e], eb = p.surface.indices[g][(e + 1) % 3];
                        if ((ea == v || eb == v) && !wedge_of.count({v, n})) { wedge_of[{v, n}] = w; stack.push_back(n); }
                    }
                }
            }

        // Output vertex ids: top copy and bottom copy per (vertex, wedge). Non-pinch vertices: wedge 0 only.
        indexed_triangle_set out;
        std::map<std::pair<int, int>, std::pair<int, int>> ids;   // (vertex, wedge) -> (top id, bottom id)
        auto vertex_ids = [&](int v, int wedge_id) -> std::pair<int, int> {
            auto it = ids.find({v, wedge_id});
            if (it != ids.end()) return it->second;
            const Vec3f top = p.surface.vertices[v];
            float d = depth[v];
            if (cap_depth > 0.) d = std::min(d, float(cap_depth));
            const Vec3f bottom = top - d * normals[v];
            int ti = int(out.vertices.size()); out.vertices.push_back(top);
            int bi = int(out.vertices.size()); out.vertices.push_back(bottom);
            return ids[{v, wedge_id}] = {ti, bi};
        };
        auto wedge = [&](int v, int f) { return boundary_count[v] > 2 ? wedge_of[{v, f}] : 0; };

        // Top triangles keep the surface winding; the bottom copies are reversed so they face into the part.
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            auto A = vertex_ids(t[0], wedge(t[0], f)), B = vertex_ids(t[1], wedge(t[1], f)), C = vertex_ids(t[2], wedge(t[2], f));
            out.indices.emplace_back(A.first, B.first, C.first);
            out.indices.emplace_back(A.second, C.second, B.second);
        }
        // Side strip on every boundary edge a->b of triangle f: the top contributes the directed edge a->b and
        // the reversed bottom contributes b_bottom->a_bottom, so the cycle b, a, a_bottom, b_bottom closes both.
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n >= 0 && in[n]) continue;
                int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                auto A = vertex_ids(a, wedge(a, f)), B = vertex_ids(b, wedge(b, f));
                out.indices.emplace_back(B.first, A.first, A.second);
                out.indices.emplace_back(B.first, A.second, B.second);
            }
        return out;
    }

    // Spec 3.4 fold guard: lower depth where a bottom triangle would invert or collapse. Returns true if any change.
    bool fold_guard(const std::vector<int> &group)
    {
        std::vector<int> to_halve;
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            Vec3f a = p.surface.vertices[t[0]], b = p.surface.vertices[t[1]], c = p.surface.vertices[t[2]];
            Vec3f nt = (b - a).cross(c - a);
            if (nt.squaredNorm() <= 0.f)                    // a degenerate top facet has no orientation to preserve
                continue;
            Vec3f a2 = a - depth[t[0]] * normals[t[0]], b2 = b - depth[t[1]] * normals[t[1]], c2 = c - depth[t[2]] * normals[t[2]];
            Vec3f nb = (c2 - a2).cross(b2 - a2);            // reversed winding -> should point along -nt
            // |n| == 2 * area, so the norm ratio IS the area ratio: only a truly collapsed bottom trips the guard.
            if (nb.dot(-nt) <= 0.f || nb.norm() < 1e-6f * nt.norm())
                for (int k = 0; k < 3; ++k) to_halve.push_back(t[k]);
        }
        return halve(std::move(to_halve));
    }

    // Uniformly halves the whole group's depth - the retry after a failed validity check.
    void halve_depth(const std::vector<int> &group)
    {
        std::vector<int> vertices;
        vertices.reserve(group.size() * 3);
        for (int f : group)
            for (int k = 0; k < 3; ++k) vertices.push_back(p.surface.indices[f][k]);
        halve(std::move(vertices));
    }

    // Halves the depth of each listed vertex exactly ONCE, floor = one layer. The list is deduplicated first:
    // a vertex is shared by several facets, and halving it per incident facet would divide by 2^valence and
    // drop straight to the floor in a single round instead of the spec's one halving per round. Returns false
    // when nothing moved (every vertex already sat on the floor), which also terminates the caller's loop.
    bool halve(std::vector<int> vertices)
    {
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
        bool changed = false;
        for (int v : vertices) {
            const float d = std::max(float(depths.layer_height), depth[v] * 0.5f);
            if (d != depth[v]) { depth[v] = d; changed = true; }
        }
        return changed;
    }
};

} // namespace

ShellCheck check_shell(const indexed_triangle_set &shell)
{
    ShellCheck c;
    c.closed = its_num_open_edges(shell) == 0;
    c.volume = double(its_volume(shell));        // signed: positive for an outward-oriented closed mesh
    c.self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(shell));
    return c;
}

std::vector<ColorShell> build_color_shells(const ColorPatches &p, const ColorSplitDepths &depths_in, const ColorSplitParams &params, const ColorSplitProgress &progress)
{
    ColorSplitDepths depths = depths_in;
    if (params.depth_override_mm > 0.) { depths.D = params.depth_override_mm; depths.unlimited = false; }
    const double D = depths.unlimited ? std::numeric_limits<double>::infinity() : depths.D;
    std::vector<Vec3i32> nbrs    = its_face_neighbors(p.surface);
    std::vector<Vec3f>   normals = color_split_normals(p.surface);
    std::vector<float>   depth   = compute_vertex_depths(p, normals, D);

    // Spec 3.5 (flat cap) and 3.6 (crease step) are not built yet: every group is one whole patch component
    // built at d(v) with a single side strip, i.e. exactly what flat_cap = crease_step = false will mean.
    const double cap_depth = 0.;

    std::vector<ColorShell> shells;
    size_t done = 0;
    for (int s : p.states) {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (size_t f = 0; f < in.size(); ++f) in[f] = char(p.facet_state[f] == s);
        for (const std::vector<int> &comp : connected_components(p, nbrs, in)) {
            ShellBuilder sb{p, nbrs, normals, depth, depths};
            for (int round = 0; round < 8 && sb.fold_guard(comp); ++round) {}
            indexed_triangle_set mesh = sb.build(comp, cap_depth);
            ShellCheck check = check_shell(mesh);
            // Validity fallback: halve the component's depth until the shell is clean, floor = one layer.
            for (int round = 0; round < 6 && (!check.closed || check.self_intersects); ++round) {
                sb.halve_depth(comp);
                mesh  = sb.build(comp, cap_depth);
                check = check_shell(mesh);
            }
            if (!check.closed || check.self_intersects)
                throw ColorSplitError("Could not build a valid shell for filament " + std::to_string(s) + " (self-intersecting surface).");
            shells.push_back({s, false, std::move(mesh)});
            if (progress && !progress(int(10 + 40 * double(++done) / std::max<size_t>(1, p.states.size() * 4))))
                throw ColorSplitCancelled();
        }
    }
    return shells;
}

} // namespace Slic3r
