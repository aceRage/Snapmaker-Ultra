#include "ColorSplit.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "MeshBoolean.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <manifold/manifold.h>
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <numeric>
#include <set>
#include <sstream>

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
        // std::max(0., ...): on a feature thinner than 2 * delta the clamp t/2 - delta turns negative, which
        // would push the bottom copies OUTSIDE the part. The fold guard cannot catch that - where the offset
        // is parallel (one flat patch, one normal) a negative depth translates the bottom triangle without
        // changing its orientation, so the guard's orientation test still passes.
        d[v] = float(std::max(0., std::min(double(d[v]), t / 2. - 0.002)));
    }
    return d;
}

namespace {

// The dialog's depth override wins over both the depth AND the unlimited flag. Both entry points have to
// apply it identically - build_color_shells to cut with, split_volume_by_paint to report back - so neither
// gets to spell the rule out for itself.
ColorSplitDepths effective_depths(const ColorSplitDepths &depths, const ColorSplitParams &params)
{
    ColorSplitDepths out = depths;
    if (params.depth_override_mm > 0.) { out.D = params.depth_override_mm; out.unlimited = false; }
    return out;
}

// Ruling 8: how far each wedge's copies of a pinch vertex move along that wedge's inward tangent bisector,
// capped per vertex at a tenth of its shortest boundary edge (Ruling 11).
// Without it the two wedges keep identical coordinates and their side walls are coincident along the pinch
// segment, which CGAL rightly calls a self-intersection. The uncovered sliver this leaves on the surface is
// one micron wide - orders of magnitude below slicer resolution.
constexpr float PINCH_NUDGE_MM = 1e-3f;

// Spec 7 (rev 2.3): the note left behind when a component cannot carry a shell. The size quoted is the
// diagonal of the painted component's own bounding box - the feature the user can point at - rather than
// anything about the failed offset geometry.
std::string too_small_warning(const ColorPatches &p, const std::vector<int> &comp, int state)
{
    Vec3f lo = p.surface.vertices[p.surface.indices[comp.front()][0]], hi = lo;
    for (int f : comp)
        for (int k = 0; k < 3; ++k) {
            const Vec3f &v = p.surface.vertices[p.surface.indices[f][k]];
            lo = lo.cwiseMin(v);
            hi = hi.cwiseMax(v);
        }
    std::ostringstream os;
    os << "Filament " << state << ": a painted feature about " << std::fixed << std::setprecision(2)
       << (hi - lo).norm() << " mm across is too small to split and stays in the body colour.";
    return os.str();
}

// Spec 9: the note left behind when the validity fallback salvaged a component by halving its depth. The
// user's colour boundary is then shallower than the depth they asked for on that one feature, and the spike
// needs to see how often that path is taken at all.
std::string depth_reduced_warning(int state, int rounds)
{
    return "Filament " + std::to_string(state) + ": shell depth reduced (" + std::to_string(rounds) +
           (rounds == 1 ? " halving" : " halvings") + ") on one painted feature to keep the split valid.";
}

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
    const std::vector<float>   &d0;            // compute_vertex_depths output; the halving floor may not exceed it
    std::vector<float>          depth;         // working copy of d0 (the fold guard lowers it)
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

        // .at(): every (pinch vertex, incident group facet) pair was assigned a wedge above, so a miss is a
        // bug in that walk - it must throw rather than quietly hand back wedge 0 and weld the wedges again.
        auto wedge = [&](int v, int f) { return boundary_count[v] > 2 ? wedge_of.at({v, f}) : 0; };

        // Ruling 8: separate the wedges of a pinch vertex. Each wedge is bounded at the vertex by exactly two
        // boundary edges; the unit inward tangent of a boundary edge a->b of facet f is n_f x (b - a), which
        // lies in the facet's plane and points into the group. Their normalised sum is the wedge's inward
        // bisector, and every copy of the vertex in that wedge - top and bottom alike - moves PINCH_NUDGE_MM
        // along it, so the wedges no longer share any geometry.
        std::map<std::pair<int, int>, Vec3f> nudge;               // (vertex, wedge) -> offset
        std::map<int, float>                 shortest_edge_at;    // pinch vertex -> its shortest boundary edge
        if (!wedge_of.empty()) {
            auto add_tangent = [&](int v, int f, const Vec3f &tangent, float edge_len) {
                if (boundary_count[v] <= 2) return;
                const std::pair<int, int> key{v, wedge(v, f)};
                auto it = nudge.find(key);
                if (it == nudge.end()) it = nudge.emplace(key, Vec3f(Vec3f::Zero())).first;
                it->second += tangent;
                auto shortest = shortest_edge_at.find(v);
                if (shortest == shortest_edge_at.end()) shortest_edge_at.emplace(v, edge_len);
                else                                    shortest->second = std::min(shortest->second, edge_len);
            };
            for (int f : group)
                for (int k = 0; k < 3; ++k) {
                    int n = nbrs[f][k];
                    if (n >= 0 && in[n]) continue;
                    const Vec3i32 &t  = p.surface.indices[f];
                    const Vec3f    nf = (p.surface.vertices[t[1]] - p.surface.vertices[t[0]])
                                            .cross(p.surface.vertices[t[2]] - p.surface.vertices[t[0]]);
                    const Vec3f edge    = p.surface.vertices[t[(k + 1) % 3]] - p.surface.vertices[t[k]];
                    const Vec3f tangent = nf.cross(edge);
                    if (tangent.squaredNorm() <= 0.f) continue;   // degenerate facet or edge: nothing to bisect
                    const Vec3f unit = tangent.normalized();
                    add_tangent(t[k],           f, unit, edge.norm());
                    add_tangent(t[(k + 1) % 3], f, unit, edge.norm());
                }
            for (auto &entry : nudge) {
                const float len = entry.second.norm();
                // Ruling 11: never move a vertex further than a tenth of the shortest boundary edge meeting it,
                // so the nudge cannot invert a micron-scale triangle.
                const float eps = std::min(PINCH_NUDGE_MM, 0.1f * shortest_edge_at.at(entry.first.first));
                entry.second = len > 0.f ? Vec3f(entry.second * (eps / len)) : Vec3f(Vec3f::Zero());
            }
        }

        // Output vertex ids: top copy and bottom copy per (vertex, wedge). Non-pinch vertices: wedge 0 only.
        indexed_triangle_set out;
        std::map<std::pair<int, int>, std::pair<int, int>> ids;   // (vertex, wedge) -> (top id, bottom id)
        auto vertex_ids = [&](int v, int wedge_id) -> std::pair<int, int> {
            auto it = ids.find({v, wedge_id});
            if (it != ids.end()) return it->second;
            auto        nudged = nudge.find({v, wedge_id});
            const Vec3f top    = nudged == nudge.end() ? p.surface.vertices[v] : Vec3f(p.surface.vertices[v] + nudged->second);
            float d = depth[v];
            if (cap_depth > 0.) d = std::min(d, float(cap_depth));
            const Vec3f bottom = top - d * normals[v];
            int ti = int(out.vertices.size()); out.vertices.push_back(top);
            int bi = int(out.vertices.size()); out.vertices.push_back(bottom);
            return ids[{v, wedge_id}] = {ti, bi};
        };

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

    // Uniformly halves the whole group's depth - the retry after a failed validity check. Returns false when
    // every vertex already sits on its floor, so the caller can stop instead of rebuilding an identical shell.
    bool halve_depth(const std::vector<int> &group)
    {
        std::vector<int> vertices;
        vertices.reserve(group.size() * 3);
        for (int f : group)
            for (int k = 0; k < 3; ++k) vertices.push_back(p.surface.indices[f][k]);
        return halve(std::move(vertices));
    }

    // Halves the depth of each listed vertex exactly ONCE. The list is deduplicated first: a vertex is shared
    // by several facets, and halving it per incident facet would divide by 2^valence and drop straight to the
    // floor in a single round instead of the spec's one halving per round. Returns false when nothing moved
    // (every vertex already sat on its floor), which also terminates the caller's loop.
    bool halve(std::vector<int> vertices)
    {
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
        bool changed = false;
        for (int v : vertices) {
            // Ruling 9: the floor is one layer, but never deeper than the vertex's own mid-thickness clamp -
            // on a feature thinner than 2h + 2*delta the layer floor would otherwise RAISE the depth and push
            // the bottom across the mid-surface, breaking the invariant d(v) <= t(v)/2 - delta.
            const float floor_d = std::min(float(depths.layer_height), d0[v]);
            const float d       = std::max(floor_d, depth[v] * 0.5f);
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

std::vector<ColorShell> build_color_shells(const ColorPatches &p, const ColorSplitDepths &depths_in, const ColorSplitParams &params, const ColorSplitProgress &progress, std::vector<std::string> *warnings)
{
    const ColorSplitDepths depths = effective_depths(depths_in, params);
    const double D = depths.unlimited ? std::numeric_limits<double>::infinity() : depths.D;
    std::vector<Vec3i32> nbrs    = its_face_neighbors(p.surface);
    std::vector<Vec3f>   normals = color_split_normals(p.surface);
    std::vector<float>   depth   = compute_vertex_depths(p, normals, D);

    // Spec 3.5 (flat cap) and 3.6 (crease step) are not built yet: every group is one whole patch component
    // built at d(v) with a single side strip, i.e. exactly what flat_cap = crease_step = false will mean.
    const double cap_depth = 0.;

    // Materialise every component up front: the progress callback is documented as a 0..100 percentage
    // (ColorSplit.hpp) and this stage owns the 10..50 band, so the tick needs the real component total - a
    // single state can hold dozens of them (painted text, a logo).
    std::vector<std::pair<int, std::vector<int>>> groups;      // (state, component facets)
    for (int s : p.states) {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (size_t f = 0; f < in.size(); ++f) in[f] = char(p.facet_state[f] == s);
        for (std::vector<int> &comp : connected_components(p, nbrs, in))
            groups.emplace_back(s, std::move(comp));
    }

    std::vector<ColorShell> shells;
    size_t done = 0;
    for (const std::pair<int, std::vector<int>> &group : groups) {
        const int               s    = group.first;
        const std::vector<int> &comp = group.second;
        ShellBuilder sb{p, nbrs, normals, depth, depth, depths};   // d0 (reference) and its working copy
        for (int round = 0; round < 8 && sb.fold_guard(comp); ++round) {}
        indexed_triangle_set mesh = sb.build(comp, cap_depth);
        ShellCheck check = check_shell(mesh);
        // Validity fallback: halve the component's depth until the shell is clean, floor = one layer. Once
        // every vertex sits on its floor there is nothing left to try and the rebuild would be identical.
        int halvings = 0;
        for (int round = 0; round < 6 && (!check.closed || check.self_intersects); ++round) {
            if (!sb.halve_depth(comp)) break;
            ++halvings;
            mesh  = sb.build(comp, cap_depth);
            check = check_shell(mesh);
        }
        // Spec 7 (rev 2.3): a component that still fails at its floor depth is a feature too small to
        // split, not an error. Drop it - the body keeps it in its own colour - and note it for the user.
        if (check.closed && !check.self_intersects) {
            if (halvings > 0 && warnings)
                warnings->push_back(depth_reduced_warning(s, halvings));
            shells.push_back({s, false, std::move(mesh)});
        } else if (warnings)
            warnings->push_back(too_small_warning(p, comp, s));
        if (progress && !progress(int(10 + 40 * double(++done) / double(std::max<size_t>(1, groups.size())))))
            throw ColorSplitCancelled();
    }
    return shells;
}

namespace {

// ---- Manifold conversion and the sequential-Split partition (spec 3.8) -----------------------------------

// MeshGL64 is MeshGLP<double, uint64_t> (mesh.h:101-163): double coordinates, uint64 triangle indices. The
// explicit tolerance is spec 3.7's - Manifold otherwise derives one from the bounding box, and a shell whose
// bottom sits microns below the surface must not be simplified away. AsOriginal() (manifold.h:198-200) stamps
// a fresh OriginalID on the result; that ID is what the island pass below reads back off the Split output to
// tell which input each face came from.
manifold::Manifold to_manifold64(const indexed_triangle_set &its, manifold::ExecutionContext &ctx)
{
    manifold::MeshGL64 m;
    m.numProp = 3;
    m.vertProperties.reserve(its.vertices.size() * 3);
    for (const Vec3f &v : its.vertices) {
        m.vertProperties.push_back(v.x());
        m.vertProperties.push_back(v.y());
        m.vertProperties.push_back(v.z());
    }
    m.triVerts.reserve(its.indices.size() * 3);
    for (const Vec3i32 &t : its.indices) {
        m.triVerts.push_back(uint64_t(t[0]));
        m.triVerts.push_back(uint64_t(t[1]));
        m.triVerts.push_back(uint64_t(t[2]));
    }
    // mesh.h:159-162: the tolerance actually used is the MAXIMUM of this and a baseline derived from the
    // bounding box, and any edge shorter than it may be collapsed - so this cannot switch simplification off,
    // it only pins the floor at the spec's value (3.7). 1e-5 mm is well below any print resolution and above
    // float noise on part-sized coordinates.
    m.tolerance = 1e-5;
    m.Merge();            // fuse duplicated vertices so a triangle soup can still form a topological manifold
    // FromMeshGL (common.h:262-271) is the ctx-aware ingest: its heavy phases check for cancellation, which
    // plain Manifold(m) does not. AsOriginal() then stamps the provenance ID and drops the attachment, so
    // callers re-attach with WithContext.
    return ctx.FromMeshGL(m).AsOriginal();
}

// Exporting a MeshGL64 is the expensive step, so callers that already hold one (the island pass needs it
// for the provenance runs) convert straight from it rather than asking the Manifold for a second copy.
indexed_triangle_set from_meshgl64(const manifold::MeshGL64 &out)
{
    indexed_triangle_set its;
    const size_t stride = size_t(out.numProp);                                    // mesh.h:110 - always >= 3
    const size_t nv     = stride > 0 ? out.vertProperties.size() / stride : 0;
    its.vertices.reserve(nv);
    for (size_t i = 0; i < nv; ++i)
        its.vertices.emplace_back(float(out.vertProperties[i * stride]), float(out.vertProperties[i * stride + 1]),
                                  float(out.vertProperties[i * stride + 2]));
    its.indices.reserve(out.triVerts.size() / 3);
    for (size_t i = 0; i + 2 < out.triVerts.size(); i += 3)
        its.indices.emplace_back(int(out.triVerts[i]), int(out.triVerts[i + 1]), int(out.triVerts[i + 2]));
    return its;
}

indexed_triangle_set from_manifold(const manifold::Manifold &m) { return from_meshgl64(m.GetMeshGL64()); }

void require_ok(const manifold::Manifold &m, const char *what)
{
    const manifold::Manifold::Error status = m.Status();
    if (status == manifold::Manifold::Error::Cancelled) throw ColorSplitCancelled();
    if (status != manifold::Manifold::Error::NoError)
        throw ColorSplitError(std::string("Boolean failed (") + what + ", Manifold status " + std::to_string(int(status)) + ").");
}

// Faces of one Split output grouped by the OriginalID of the input they came from. mesh.h:125-138: the runs
// cover all of triVerts and runIndex is one longer than runOriginalID - if that ever stops holding, the
// provenance this pass relies on is not what we think it is, so say so instead of guessing.
std::map<uint32_t, size_t> faces_by_original_id(const manifold::MeshGL64 &gl)
{
    if (gl.runIndex.size() != gl.runOriginalID.size() + 1)
        throw ColorSplitError("Manifold returned no usable face provenance (internal error).");
    std::map<uint32_t, size_t> faces;
    for (size_t run = 0; run < gl.runOriginalID.size(); ++run) {
        // Manifold emits an EMPTY run for an input that contributed nothing to this component (measured on
        // the fully painted sphere: the leftover core carries a 0-triangle run for the source mesh). Counting
        // those would make every island look as if it touched the source and no island would ever be absorbed.
        const size_t n = size_t(gl.runIndex[run + 1] - gl.runIndex[run]) / 3;
        if (n > 0) faces[gl.runOriginalID[run]] += n;
    }
    return faces;
}

} // namespace

ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells, bool absorb_islands, const ColorSplitProgress &progress)
{
    // The context is the plumbing spec rev 2 (M7) asks for, and require_ok turns Error::Cancelled into
    // ColorSplitCancelled - but note that Split is an EAGER op that does not observe an attached ctx
    // (manifold.h:147-171: only Refine / Hull / Minkowski and a deferred tree's Status() do). Cancellation
    // therefore lands BETWEEN shells, not inside a boolean: each Split runs to completion.
    manifold::ExecutionContext ctx;
    auto tick = [&](int pct) { if (progress && !progress(pct)) { ctx.Cancel(); throw ColorSplitCancelled(); } };

    manifold::Manifold original = to_manifold64(mesh, ctx).WithContext(ctx);
    require_ok(original, "source mesh");
    const uint32_t original_id  = uint32_t(original.OriginalID());
    const double   vol_original = original.Volume();

    ColorSplitResult r;
    // (volume, mesh): every Manifold is exported to a MeshGL64 exactly once, where we first have one in hand.
    using Part = std::pair<double, indexed_triangle_set>;
    std::map<int, std::vector<Part>> piece_parts;   // filament -> its share of the part
    std::map<uint32_t, int>          shell_state;   // shell OriginalID -> filament
    // Spec 3.8/7: a filament can lose ALL of its area to a lower one, in which case piece_parts never gains a
    // key for it. The states have to be remembered up front or that colour would vanish without a word.
    std::set<int> shell_states;
    for (const ColorShell &s : shells) shell_states.insert(s.state);
    manifold::Manifold rest = original;
    for (size_t i = 0; i < shells.size(); ++i) {
        manifold::Manifold shell = to_manifold64(shells[i].mesh, ctx).WithContext(ctx);
        require_ok(shell, "shell");
        shell_state[uint32_t(shell.OriginalID())] = shells[i].state;
        // manifold.cpp:950-967: Split returns (intersection, difference) - the piece first, the remainder
        // second, from one evaluation, so the two are complementary with no epsilon mismatch between them.
        auto [piece, remainder] = rest.Split(shell);
        require_ok(piece, "split piece"); require_ok(remainder, "split remainder");
        if (!piece.IsEmpty()) piece_parts[shells[i].state].emplace_back(piece.Volume(), from_manifold(piece));
        rest = remainder;
        tick(int(50 + 40 * double(i + 1) / double(shells.size())));
    }

    // Spec 3.8: enclosed body islands -> the colour that contributed most of their surface. A component none
    // of whose faces come from the source mesh is completely wrapped by colour pieces and therefore invisible.
    std::vector<Part> body_parts;
    for (manifold::Manifold &comp : rest.Decompose()) {
        require_ok(comp, "body component");
        const manifold::MeshGL64         gl    = comp.GetMeshGL64();   // one export, used for both purposes
        const std::map<uint32_t, size_t> faces = faces_by_original_id(gl);
        const bool touches_original = faces.count(original_id) != 0;
        if (absorb_islands && !touches_original && !faces.empty()) {
            uint32_t best = 0; size_t best_n = 0; int best_state = std::numeric_limits<int>::max();
            for (const std::pair<const uint32_t, size_t> &face_run : faces) {
                auto      it = shell_state.find(face_run.first);
                const int st = it != shell_state.end() ? it->second : std::numeric_limits<int>::max();
                // Most surface wins; ties go to the lowest filament.
                if (face_run.second > best_n || (face_run.second == best_n && st < best_state)) {
                    best = face_run.first; best_n = face_run.second; best_state = st;
                }
            }
            auto winner = shell_state.find(best);
            if (winner != shell_state.end()) { piece_parts[winner->second].emplace_back(comp.Volume(), from_meshgl64(gl)); continue; }
        }
        body_parts.emplace_back(comp.Volume(), from_meshgl64(gl));
    }

    double total = 0.;
    for (Part &b : body_parts) { total += b.first; its_merge(r.body, b.second); }
    for (auto &[state, parts] : piece_parts) {
        indexed_triangle_set its;
        for (Part &part : parts) { total += part.first; its_merge(its, part.second); }
        r.pieces.emplace_back(state, std::move(its));
    }
    // Spec 3.8: empty pieces are dropped with a warning - the colour was painted but a lower filament claimed
    // every last bit of it (or its shell fell outside the part), and the user has to hear about that.
    for (int state : shell_states)
        if (piece_parts.count(state) == 0)
            r.warnings.push_back("Filament " + std::to_string(state) + ": painted area produced no solid (fully covered by lower filaments).");
    if (std::abs(total - vol_original) > 1e-4 * std::abs(vol_original) + 1e-9)
        throw ColorSplitError("Volume check failed after splitting (" + std::to_string(total) + " vs " + std::to_string(vol_original) + " mm^3).");
    tick(95);
    return r;
}

ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress)
{
    if (progress && !progress(0)) throw ColorSplitCancelled();
    ColorPatches patches = extract_color_patches(mesh, paint);
    if (patches.states.empty()) throw ColorSplitError("The part has no painted colours.");
    if (progress && !progress(10)) throw ColorSplitCancelled();
    std::vector<std::string> shell_warnings;
    // Ruling 10: skipped micro-components are warnings, not errors - they have to reach the caller.
    std::vector<ColorShell> shells = build_color_shells(patches, depths, params, progress, &shell_warnings);
    ColorSplitResult r = partition_by_shells(patches.surface, shells, params.absorb_islands, progress);
    r.warnings.insert(r.warnings.begin(), shell_warnings.begin(), shell_warnings.end());
    r.depths = effective_depths(depths, params);
    if (progress) progress(100);
    return r;
}

} // namespace Slic3r
