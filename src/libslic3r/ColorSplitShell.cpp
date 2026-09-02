// Split by painted colour, stage 2: depth groups and the closed inward-offset shell of each of them.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md, 3.5 - 3.7.
#include "ColorSplit.hpp"
#include "ColorSplitInternal.hpp"
#include "TriangleMesh.hpp"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <map>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

namespace Slic3r {
namespace {

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

std::vector<ColorShell> build_color_shells(const ColorPatches &p, const ColorSplitDepths &depths_in, const ColorSplitParams &params, const ColorSplitProgress &progress, std::vector<std::string> *warnings)
{
    const ColorSplitDepths depths = ColorSplitDetail::effective_depths(depths_in, params);
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
        // Ruling 15: reining an offset in is ordinary work, not an event - a salvaged component gets no note.
        for (int round = 0; round < 6 && (!check.closed || check.self_intersects); ++round) {
            if (!sb.halve_depth(comp)) break;
            mesh  = sb.build(comp, cap_depth);
            check = check_shell(mesh);
        }
        // Spec 7 (rev 2.3): a component that still fails at its floor depth is a feature too small to
        // split, not an error. Drop it - the body keeps it in its own colour - and note it for the user.
        if (check.closed && !check.self_intersects)
            shells.push_back({s, false, std::move(mesh)});
        else if (warnings)
            warnings->push_back(too_small_warning(p, comp, s));
        if (progress && !progress(int(10 + 40 * double(++done) / double(std::max<size_t>(1, groups.size())))))
            throw ColorSplitCancelled();
    }
    return shells;
}

} // namespace Slic3r
