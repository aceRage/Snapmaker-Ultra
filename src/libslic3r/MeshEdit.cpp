#include "MeshEdit.hpp"

#include "MeshBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <unordered_map>
#include <utility>

namespace Slic3r {
namespace MeshEdit {

namespace {

inline float deg2rad(float deg) { return deg * float(M_PI) / 180.f; }

// The unnormalised facet normal: its length is twice the facet's area, which is
// what makes the area weighting in FaceRegion free.
inline Vec3f facet_area_normal(const indexed_triangle_set &its, size_t f)
{
    const auto &t = its.indices[f];
    const Vec3f &a = its.vertices[t[0]];
    const Vec3f &b = its.vertices[t[1]];
    const Vec3f &c = its.vertices[t[2]];
    return (b - a).cross(c - a);
}

inline Vec3f facet_unit_normal(const indexed_triangle_set &its, size_t f)
{
    const Vec3f n = facet_area_normal(its, f);
    const float l = n.norm();
    return l > 1e-20f ? Vec3f(n / l) : Vec3f::Zero();
}

} // namespace

// ----------------------------------------------------------------------------
// Topology
// ----------------------------------------------------------------------------

MeshTopology build_topology(const indexed_triangle_set &its)
{
    MeshTopology topo;
    const size_t F = its.indices.size();
    if (F == 0)
        return topo;

    topo.face_normals.resize(F);
    for (size_t f = 0; f < F; ++f)
        topo.face_normals[f] = facet_unit_normal(its, f);

    topo.face_neighbors = its_face_neighbors(its);
    // assign_unbound_edges: an open (boundary) edge gets its own id too, so the
    // chain walk can treat it as the feature edge it obviously is rather than
    // fall off a -1.
    int num_edges = 0;
    topo.face_edge_ids = its_face_edge_ids(its, topo.face_neighbors, /*assign_unbound_edges*/ true, &num_edges);
    topo.num_edges     = num_edges;

    topo.edge_vertices.assign(size_t(num_edges), Vec2i32(-1, -1));
    topo.edge_faces.assign(size_t(num_edges), Vec2i32(-1, -1));
    topo.edge_face_count.assign(size_t(num_edges), 0);

    for (size_t f = 0; f < F; ++f) {
        const auto &tri = its.indices[f];
        for (int j = 0; j < 3; ++j) {
            const int e = topo.face_edge_ids[f][j];
            if (e < 0 || e >= num_edges)
                continue;
            const uint8_t n = topo.edge_face_count[e];
            if (n == 0) {
                const Vec2i32 ev = its_triangle_edge(tri, j);
                topo.edge_vertices[e] = ev;
                topo.edge_faces[e](0) = int(f);
            } else if (n == 1) {
                topo.edge_faces[e](1) = int(f);
            }
            // A count above two is recorded but the facets past the second are
            // not: nothing in phase 1 does anything with a non-manifold edge
            // except refuse it, and the count is enough to refuse on.
            if (n < 255)
                topo.edge_face_count[e] = uint8_t(n + 1);
        }
    }

    return topo;
}

// ----------------------------------------------------------------------------
// Feature edges and chains
// ----------------------------------------------------------------------------

float edge_dihedral_deg(const MeshTopology &topo, int edge_id)
{
    if (edge_id < 0 || edge_id >= topo.num_edges)
        return 0.f;
    const uint8_t n = topo.edge_face_count[edge_id];
    // A boundary edge is maximally a feature; a non-manifold edge is reported
    // flat so nothing selects it by accident.
    if (n == 1)
        return 180.f;
    if (n != 2)
        return 0.f;
    const Vec2i32 &ef = topo.edge_faces[edge_id];
    if (ef(0) < 0 || ef(1) < 0)
        return 180.f;
    const Vec3f &n0 = topo.face_normals[size_t(ef(0))];
    const Vec3f &n1 = topo.face_normals[size_t(ef(1))];
    if (n0.squaredNorm() < 1e-12f || n1.squaredNorm() < 1e-12f)
        return 0.f;
    const float c = std::clamp(n0.dot(n1), -1.f, 1.f);
    return std::acos(c) * 180.f / float(M_PI);
}

std::vector<uint8_t> feature_edge_mask(const MeshTopology &topo, float threshold_deg)
{
    std::vector<uint8_t> mask(size_t(std::max(0, topo.num_edges)), 0);
    for (int e = 0; e < topo.num_edges; ++e)
        mask[size_t(e)] = edge_dihedral_deg(topo, e) >= threshold_deg ? 1 : 0;
    return mask;
}

namespace {

// The other endpoint of `edge` given one of them; -1 when `v` is not on it.
inline int edge_other_vertex(const MeshTopology &topo, int edge, int v)
{
    const Vec2i32 &ev = topo.edge_vertices[size_t(edge)];
    if (ev(0) == v) return ev(1);
    if (ev(1) == v) return ev(0);
    return -1;
}

// vertex -> the feature edges incident to it, as a CSR pair.
struct VertexFeatureEdges
{
    std::vector<int> edges;
    std::vector<int> start;   // size = vertex count + 1

    int  count(int v) const { return start[size_t(v) + 1] - start[size_t(v)]; }
    const int *begin_of(int v) const { return edges.data() + start[size_t(v)]; }
};

VertexFeatureEdges build_vertex_feature_edges(const indexed_triangle_set &its,
                                              const MeshTopology         &topo,
                                              const std::vector<uint8_t> &is_feature)
{
    VertexFeatureEdges out;
    const size_t V = its.vertices.size();
    out.start.assign(V + 1, 0);
    for (int e = 0; e < topo.num_edges; ++e) {
        if (!is_feature[size_t(e)])
            continue;
        const Vec2i32 &ev = topo.edge_vertices[size_t(e)];
        if (ev(0) < 0 || ev(1) < 0)
            continue;
        ++out.start[size_t(ev(0)) + 1];
        ++out.start[size_t(ev(1)) + 1];
    }
    for (size_t i = 1; i <= V; ++i)
        out.start[i] += out.start[i - 1];
    out.edges.assign(size_t(out.start[V]), -1);
    std::vector<int> cursor(out.start.begin(), out.start.end() - 1);
    for (int e = 0; e < topo.num_edges; ++e) {
        if (!is_feature[size_t(e)])
            continue;
        const Vec2i32 &ev = topo.edge_vertices[size_t(e)];
        if (ev(0) < 0 || ev(1) < 0)
            continue;
        out.edges[size_t(cursor[size_t(ev(0))]++)] = e;
        out.edges[size_t(cursor[size_t(ev(1))]++)] = e;
    }
    return out;
}

// Unit direction of `edge`, pointing AWAY from `from_vertex`.
inline Vec3f edge_direction(const indexed_triangle_set &its,
                            const MeshTopology         &topo,
                            int                         edge,
                            int                         from_vertex)
{
    const int other = edge_other_vertex(topo, edge, from_vertex);
    if (other < 0)
        return Vec3f::Zero();
    const Vec3f d = its.vertices[size_t(other)] - its.vertices[size_t(from_vertex)];
    const float l = d.norm();
    return l > 1e-12f ? Vec3f(d / l) : Vec3f::Zero();
}

// From `vertex`, reached by `incoming_edge`: the single feature edge to continue
// along, or -1 to stop. The "exactly one other" rule is the corner test - three
// or more incident feature edges means a corner, and a corner terminates.
int next_chain_edge(const indexed_triangle_set &its,
                    const MeshTopology         &topo,
                    const VertexFeatureEdges   &vfe,
                    int                         vertex,
                    int                         incoming_edge,
                    float                       continuation_deg)
{
    if (vertex < 0 || vfe.count(vertex) != 2)
        return -1;
    const int *inc = vfe.begin_of(vertex);
    const int  cand = inc[0] == incoming_edge ? inc[1] : (inc[1] == incoming_edge ? inc[0] : -1);
    if (cand < 0)
        return -1;

    // The turn: the incoming edge arrives at `vertex`, so its direction there is
    // the reverse of "away from vertex"; the candidate leaves along "away from
    // vertex". A straight continuation has the two agreeing, i.e. an angle of 0.
    const Vec3f in_dir  = -edge_direction(its, topo, incoming_edge, vertex);
    const Vec3f out_dir =  edge_direction(its, topo, cand, vertex);
    if (in_dir.squaredNorm() < 1e-12f || out_dir.squaredNorm() < 1e-12f)
        return -1;
    const float turn = std::acos(std::clamp(in_dir.dot(out_dir), -1.f, 1.f)) * 180.f / float(M_PI);
    return turn <= continuation_deg ? cand : -1;
}

} // namespace

EdgeChain grow_edge_chain(const indexed_triangle_set &its,
                          const MeshTopology         &topo,
                          const std::vector<uint8_t> &is_feature,
                          int                         seed_edge,
                          float                       continuation_deg)
{
    EdgeChain chain;
    if (seed_edge < 0 || seed_edge >= topo.num_edges)
        return chain;
    if (is_feature.size() != size_t(topo.num_edges) || !is_feature[size_t(seed_edge)])
        return chain;

    const VertexFeatureEdges vfe = build_vertex_feature_edges(its, topo, is_feature);

    const Vec2i32 &seed_ev = topo.edge_vertices[size_t(seed_edge)];
    if (seed_ev(0) < 0 || seed_ev(1) < 0)
        return chain;

    // Walk forward from seed_ev(1), then backward from seed_ev(0), and splice.
    std::vector<int> fwd_edges, fwd_verts;
    std::vector<uint8_t> visited(size_t(topo.num_edges), 0);
    visited[size_t(seed_edge)] = 1;

    bool closed = false;
    {
        int edge = seed_edge, vertex = seed_ev(1);
        while (true) {
            const int next = next_chain_edge(its, topo, vfe, vertex, edge, continuation_deg);
            if (next < 0)
                break;
            if (visited[size_t(next)]) {
                // Came back onto the chain: it closes. (Only a return to the
                // SEED is a genuine loop; anything else would be a figure-eight
                // through a corner, which the valence-2 rule already excludes.)
                closed = (next == seed_edge);
                break;
            }
            visited[size_t(next)] = 1;
            fwd_edges.push_back(next);
            fwd_verts.push_back(vertex);
            const int nv = edge_other_vertex(topo, next, vertex);
            if (nv < 0)
                break;
            vertex = nv;
            edge   = next;
        }
        if (!closed)
            fwd_verts.push_back(vertex);   // the far endpoint
    }

    std::vector<int> bwd_edges, bwd_verts;
    if (!closed) {
        int edge = seed_edge, vertex = seed_ev(0);
        while (true) {
            const int next = next_chain_edge(its, topo, vfe, vertex, edge, continuation_deg);
            if (next < 0 || visited[size_t(next)])
                break;
            visited[size_t(next)] = 1;
            bwd_edges.push_back(next);
            bwd_verts.push_back(vertex);
            const int nv = edge_other_vertex(topo, next, vertex);
            if (nv < 0)
                break;
            vertex = nv;
            edge   = next;
        }
        bwd_verts.push_back(vertex);
    }

    // Assemble: reversed backward half, the seed, then the forward half.
    chain.closed = closed;
    chain.edges.reserve(bwd_edges.size() + 1 + fwd_edges.size());
    chain.vertices.reserve(chain.edges.capacity() + 1);

    for (auto it = bwd_verts.rbegin(); it != bwd_verts.rend(); ++it)
        chain.vertices.push_back(*it);
    for (auto it = bwd_edges.rbegin(); it != bwd_edges.rend(); ++it)
        chain.edges.push_back(*it);
    if (bwd_verts.empty())
        chain.vertices.push_back(seed_ev(0));
    chain.edges.push_back(seed_edge);
    for (size_t i = 0; i < fwd_edges.size(); ++i)
        chain.edges.push_back(fwd_edges[i]);
    for (size_t i = 0; i < fwd_verts.size(); ++i)
        chain.vertices.push_back(fwd_verts[i]);

    return chain;
}

std::vector<EdgeChain> all_edge_chains(const indexed_triangle_set &its,
                                       const MeshTopology         &topo,
                                       float                       threshold_deg,
                                       float                       continuation_deg)
{
    std::vector<EdgeChain> out;
    const std::vector<uint8_t> is_feature = feature_edge_mask(topo, threshold_deg);
    std::vector<uint8_t> claimed(size_t(std::max(0, topo.num_edges)), 0);
    for (int e = 0; e < topo.num_edges; ++e) {
        if (!is_feature[size_t(e)] || claimed[size_t(e)])
            continue;
        EdgeChain chain = grow_edge_chain(its, topo, is_feature, e, continuation_deg);
        if (chain.empty()) {
            claimed[size_t(e)] = 1;
            continue;
        }
        for (int ce : chain.edges)
            claimed[size_t(ce)] = 1;
        out.push_back(std::move(chain));
    }
    return out;
}

int nearest_edge_of_facet(const indexed_triangle_set &its,
                          const MeshTopology         &topo,
                          size_t                      facet,
                          const Vec3f                &point,
                          float                      *out_distance)
{
    if (facet >= its.indices.size() || facet >= topo.face_edge_ids.size())
        return -1;
    const auto &tri = its.indices[facet];
    int   best      = -1;
    float best_dist = std::numeric_limits<float>::max();
    for (int j = 0; j < 3; ++j) {
        const int e = topo.face_edge_ids[facet][j];
        if (e < 0)
            continue;
        const Vec2i32 ev = its_triangle_edge(tri, j);
        const Vec3f  &a  = its.vertices[size_t(ev(0))];
        const Vec3f  &b  = its.vertices[size_t(ev(1))];
        const Vec3f   ab = b - a;
        const float   l2 = ab.squaredNorm();
        const float   t  = l2 > 1e-20f ? std::clamp((point - a).dot(ab) / l2, 0.f, 1.f) : 0.f;
        const float   d  = (point - (a + ab * t)).norm();
        if (d < best_dist) {
            best_dist = d;
            best      = e;
        }
    }
    if (out_distance != nullptr)
        *out_distance = best >= 0 ? best_dist : std::numeric_limits<float>::max();
    return best;
}

// ----------------------------------------------------------------------------
// Face regions
// ----------------------------------------------------------------------------

FaceRegion grow_face_region(const indexed_triangle_set &its,
                            const MeshTopology         &topo,
                            size_t                      seed_facet,
                            const RegionParams         &params)
{
    FaceRegion region;
    const size_t F = its.indices.size();
    if (seed_facet >= F || topo.face_normals.size() != F || topo.face_neighbors.size() != F)
        return region;

    const Vec3f n_seed = topo.face_normals[seed_facet];
    if (n_seed.squaredNorm() < 1e-12f)
        return region;

    const float cos_cap  = std::cos(deg2rad(std::max(0.f, params.angle_tol_deg)));
    const float cos_step = std::cos(deg2rad(std::max(0.f, params.step_angle_deg)));
    const size_t cap     = std::max<size_t>(1, params.max_facets);

    // DFS. Planar mode tests every candidate against the SEED only (a CAD face);
    // Smooth mode adds the per-step test against the facet we came from, which
    // is what lets it cross a gently curved face - Measure.cpp's grow_curve_patch
    // with both thresholds handed to the caller instead of frozen at 8/20 deg.
    std::vector<uint8_t> seen(F, 0);
    std::vector<int>     stack{int(seed_facet)};
    seen[seed_facet] = 1;
    region.facets.reserve(64);

    while (!stack.empty() && region.facets.size() < cap) {
        const int t = stack.back();
        stack.pop_back();
        region.facets.push_back(t);
        const Vec3f &n_t = topo.face_normals[size_t(t)];
        for (int k = 0; k < 3; ++k) {
            const int u = topo.face_neighbors[size_t(t)][k];
            if (u < 0 || seen[size_t(u)])
                continue;
            const Vec3f &n_u = topo.face_normals[size_t(u)];
            if (n_u.squaredNorm() < 1e-12f)
                continue;
            if (n_u.dot(n_seed) < cos_cap)
                continue;
            if (params.mode == RegionMode::Smooth && n_u.dot(n_t) < cos_step)
                continue;
            seen[size_t(u)] = 1;
            stack.push_back(u);
        }
    }

    std::sort(region.facets.begin(), region.facets.end());

    // Area-weighted normal, area and centroid in one pass. The unnormalised
    // cross product is already 2 * area * unit_normal, so summing them IS the
    // area weighting.
    Vec3f  sum_n = Vec3f::Zero();
    Vec3f  sum_c = Vec3f::Zero();
    double area2 = 0.0;
    for (int f : region.facets) {
        const Vec3f an = facet_area_normal(its, size_t(f));
        const float a2 = an.norm();
        sum_n += an;
        area2 += double(a2);
        const auto &tri = its.indices[size_t(f)];
        const Vec3f  c  = (its.vertices[tri[0]] + its.vertices[tri[1]] + its.vertices[tri[2]]) / 3.f;
        sum_c += c * a2;
    }
    region.area = float(area2 * 0.5);
    const float ln = sum_n.norm();
    region.normal  = ln > 1e-20f ? Vec3f(sum_n / ln) : n_seed;
    region.center  = area2 > 1e-20 ? Vec3f(sum_c / float(area2)) : its.vertices[its.indices[seed_facet][0]];

    return region;
}

// ----------------------------------------------------------------------------
// Push / pull
// ----------------------------------------------------------------------------

bool self_intersects(const indexed_triangle_set &its)
{
    if (its.indices.empty())
        return false;
    return MeshBoolean::cgal::does_self_intersect(TriangleMesh(its));
}

float snap_to_step(float value, float step)
{
    if (!(step > 0.f))
        return value;
    return std::round(value / step) * step;
}

TranslateResult translate_region(const indexed_triangle_set &its,
                                 const MeshTopology         &topo,
                                 const FaceRegion           &region,
                                 const TranslateParams      &params)
{
    TranslateResult res;
    if (region.facets.empty() || its.indices.empty()) {
        res.status = TranslateStatus::EmptyRegion;
        return res;
    }

    Vec3f dir = params.direction;
    if (dir.squaredNorm() < 1e-12f)
        dir = region.normal;
    const float dl = dir.norm();
    if (dl < 1e-12f) {
        res.status = TranslateStatus::EmptyRegion;
        return res;
    }
    dir /= dl;

    if (region.facets.size() >= its.indices.size()) {
        // Moving every facet is moving the part, and there is no surrounding
        // ring left to stretch. Refuse rather than silently translate the model
        // out from under the Move gizmo.
        res.status = TranslateStatus::WholeMesh;
        return res;
    }

    if (params.d == 0.f) {
        res.status = TranslateStatus::NoOp;
        res.mesh   = its;
        return res;
    }

    // Cheap pre-check, before paying for CGAL: a move longer than the mesh's own
    // bounding-box diagonal cannot be anything but a fold-through.
    {
        Vec3f lo = its.vertices.front(), hi = its.vertices.front();
        for (const Vec3f &v : its.vertices) {
            lo = lo.cwiseMin(v);
            hi = hi.cwiseMax(v);
        }
        const float diag = (hi - lo).norm();
        if (std::abs(params.d) > diag * std::max(0.f, params.max_travel_bbox_factor)) {
            res.status = TranslateStatus::OutOfBounds;
            return res;
        }
    }

    // Every vertex used by the region's facets moves - the interior ones AND the
    // border ones. The facets outside the region that share a border vertex are
    // therefore stretched to follow, which is the whole feature: a cube's top
    // face pushed up makes the walls taller.
    std::vector<uint8_t> move(its.vertices.size(), 0);
    for (int f : region.facets) {
        const auto &tri = its.indices[size_t(f)];
        move[size_t(tri[0])] = 1;
        move[size_t(tri[1])] = 1;
        move[size_t(tri[2])] = 1;
    }

    res.mesh = its;
    const Vec3f delta = dir * params.d;
    res.moved_vertices.reserve(region.facets.size() * 2);
    for (size_t v = 0; v < move.size(); ++v)
        if (move[v]) {
            res.mesh.vertices[v] += delta;
            res.moved_vertices.push_back(uint32_t(v));
        }

    // Dirty facets: anything touching a moved vertex. The region's own facets
    // translate rigidly, the ring stretches, and both need their normals and
    // their GPU vertices refreshed.
    {
        std::vector<uint8_t> dirty(its.indices.size(), 0);
        for (size_t f = 0; f < its.indices.size(); ++f) {
            const auto &tri = its.indices[f];
            if (move[size_t(tri[0])] || move[size_t(tri[1])] || move[size_t(tri[2])])
                dirty[f] = 1;
        }
        for (size_t f = 0; f < dirty.size(); ++f)
            if (dirty[f])
                res.dirty_facets.push_back(uint32_t(f));
    }

    if (params.check_self_intersection && self_intersects(res.mesh)) {
        res.status = TranslateStatus::SelfIntersects;
        res.mesh   = indexed_triangle_set();
        res.moved_vertices.clear();
        res.dirty_facets.clear();
        return res;
    }

    (void) topo;   // reserved: phase 3's border-vertex slide needs it
    res.status = TranslateStatus::Ok;
    return res;
}

// ----------------------------------------------------------------------------
// Edge bevel / chamfer - phase 2
// ----------------------------------------------------------------------------
//
// The build in one paragraph, so the code below reads as an implementation of a
// plan rather than a pile of loops.
//
// Every bevelled edge e = (a, b) with incident facets f0, f1 contributes a STRIP:
// on each incident face, the edge is pushed back by the solved width w along that
// face's in-plane normal, giving two offset rails. The strip is the ruled surface
// between them - one quad band for a chamfer, N bands following the tangent arc
// for a round. Every ORIGINAL facet then has to be re-cut so it stops at the rail
// instead of at the old edge.
//
// The re-cut is done per CORNER rather than per facet, and that is the trick that
// keeps the whole thing manageable. Instead of clipping facets against lines (which
// needs robust 2D boolean work and produces slivers), each original vertex v is
// SPLIT into one new vertex per (facet-corner) that touches it, displaced inward
// by the bevels of the edges meeting at v. A facet then just re-indexes its three
// corners to their split copies and keeps its original shape - no clipping at all.
// The hole this opens around v is exactly the corner patch, and the hole opened
// along e is exactly the strip. Everything is then a matter of filling holes whose
// boundaries are already known, which is robust.

namespace {

// The in-plane unit normal of edge (va, vb) inside facet f: perpendicular to the
// edge, lying in f's plane, pointing INTO f (towards f's third vertex). This is
// the direction the edge is pushed back along when f is re-cut, the t0 / t1 of
// the spec's step (1).
Vec3f in_plane_normal(const indexed_triangle_set &its, const MeshTopology &topo, size_t f, int va, int vb)
{
    const Vec3f &A = its.vertices[va];
    const Vec3f &B = its.vertices[vb];
    Vec3f        e = B - A;
    const float  l = e.norm();
    if (l < 1e-12f)
        return Vec3f::Zero();
    e /= l;
    // n x e is in f's plane and perpendicular to e; its sign follows the winding,
    // and for a consistently wound mesh with an outward normal it points into f.
    const Vec3f n = topo.face_normals[f];
    Vec3f       t = n.cross(e);
    const float tl = t.norm();
    if (tl < 1e-12f)
        return Vec3f::Zero();
    t /= tl;
    // Belt and braces against a flipped winding: make it point at the third
    // vertex, which is the definition rather than the shortcut.
    const auto &tri = its.indices[f];
    int         third = -1;
    for (int j = 0; j < 3; ++j)
        if (tri[j] != va && tri[j] != vb)
            third = tri[j];
    if (third >= 0) {
        const Vec3f d = its.vertices[third] - A;
        if (t.dot(d) < 0.f)
            t = -t;
    }
    return t;
}

// The two facets of a manifold edge, or (-1, -1).
inline Vec2i32 edge_two_faces(const MeshTopology &topo, int e)
{
    if (e < 0 || e >= topo.num_edges || topo.edge_face_count[e] != 2)
        return Vec2i32(-1, -1);
    return topo.edge_faces[e];
}

// Length of a global edge.
inline float edge_length(const indexed_triangle_set &its, const MeshTopology &topo, int e)
{
    const Vec2i32 ev = topo.edge_vertices[e];
    return (its.vertices[ev(1)] - its.vertices[ev(0)]).norm();
}

// The half-angle between the two incident faces, as the spec's theta/2: for a
// 90 deg cube edge the dihedral between the FACES is 90, the interior angle of
// the solid is 90, and the offset geometry wants half of that.
//
// Returns the interior angle of the material at the edge, in radians. A convex
// 90 deg cube edge gives pi/2.
float edge_interior_angle(const MeshTopology &topo, int e)
{
    const Vec2i32 ff = edge_two_faces(topo, e);
    if (ff(0) < 0)
        return 0.f;
    const Vec3f &n0 = topo.face_normals[ff(0)];
    const Vec3f &n1 = topo.face_normals[ff(1)];
    const float  c  = std::clamp(n0.dot(n1), -1.f, 1.f);
    // The angle between the outward normals is pi - interior angle.
    return float(M_PI) - std::acos(c);
}

// True when the edge is CONVEX (material on the inside of the fold), which is the
// only case a bevel of this shape is defined for. A concave edge would need the
// strip to bulge outward, and the offset rails cross instead.
bool edge_is_convex(const indexed_triangle_set &its, const MeshTopology &topo, int e)
{
    const Vec2i32 ff = edge_two_faces(topo, e);
    if (ff(0) < 0)
        return false;
    const Vec2i32 ev = topo.edge_vertices[e];
    // Take f0's third vertex: if it lies BEHIND f1's plane, the solid folds away
    // from the normals and the edge is convex.
    const auto &tri = its.indices[ff(0)];
    int         third = -1;
    for (int j = 0; j < 3; ++j)
        if (tri[j] != ev(0) && tri[j] != ev(1))
            third = tri[j];
    if (third < 0)
        return false;
    const Vec3f &n1 = topo.face_normals[ff(1)];
    const Vec3f &p1 = its.vertices[its.indices[ff(1)][0]];
    return n1.dot(its.vertices[third] - p1) < 0.f;
}

// Ear-clip `poly` (indices into its.vertices) in the plane whose normal is `n`,
// appending the triangles to `out`. The polygon is assumed planar and simple,
// which is what a rewritten SIDE boundary is: a side is a planar region bounded
// by creases, and cutting its corners back along its own plane keeps it planar
// and cannot make it self-intersect as long as the widths were clamped - which
// is what the global solve is for.
//
// Ear clipping rather than a fan because a rewritten side is frequently NOT
// convex (an L-shaped face, or a face with one corner cut and another not), and
// a fan from any single vertex folds on those.
//
// The winding of the result follows `n`, so the new facets face the same way the
// side's original facets did.
void triangulate_planar_polygon(indexed_triangle_set &out, const std::vector<int> &poly_in, const Vec3f &n)
{
    std::vector<int> poly = poly_in;
    if (poly.size() < 3)
        return;
    if (poly.size() == 3) {
        const Vec3f &a = out.vertices[poly[0]];
        const Vec3f &b = out.vertices[poly[1]];
        const Vec3f &c = out.vertices[poly[2]];
        if ((b - a).cross(c - a).dot(n) < 0.f) out.indices.emplace_back(poly[0], poly[2], poly[1]);
        else                                   out.indices.emplace_back(poly[0], poly[1], poly[2]);
        return;
    }

    // A 2D basis in the polygon's plane, so the ear test is an ordinary planar
    // one rather than a sequence of 3D cross products.
    Vec3f nn = n;
    if (nn.norm() < 1e-12f)
        return;
    nn.normalize();
    Vec3f ax = std::abs(nn.x()) < 0.9f ? Vec3f::UnitX() : Vec3f::UnitY();
    ax = (ax - nn * nn.dot(ax)).normalized();
    const Vec3f ay = nn.cross(ax);
    auto to2d = [&](int i) {
        const Vec3f p = out.vertices[i];
        return Vec2f(p.dot(ax), p.dot(ay));
    };

    // Make the working order counter-clockwise in that basis, so "convex" below
    // has one meaning rather than two.
    double area2 = 0.;
    for (size_t i = 0; i < poly.size(); ++i) {
        const Vec2f p = to2d(poly[i]), q = to2d(poly[(i + 1) % poly.size()]);
        area2 += double(p.x()) * double(q.y()) - double(q.x()) * double(p.y());
    }
    const bool reversed = area2 < 0.;
    if (reversed)
        std::reverse(poly.begin(), poly.end());

    auto cross2 = [](const Vec2f &o, const Vec2f &p, const Vec2f &q) {
        return double(p.x() - o.x()) * double(q.y() - o.y()) -
               double(p.y() - o.y()) * double(q.x() - o.x());
    };
    auto inside = [&](const Vec2f &a, const Vec2f &b, const Vec2f &c, const Vec2f &p) {
        return cross2(a, b, p) >= 0. && cross2(b, c, p) >= 0. && cross2(c, a, p) >= 0.;
    };

    auto emit = [&](int i0, int i1, int i2) {
        // Undo the reversal when emitting, so the facet faces `n` either way.
        if (reversed) out.indices.emplace_back(i0, i2, i1);
        else          out.indices.emplace_back(i0, i1, i2);
    };

    // O(n^2) ear clipping. A side polygon has a handful of vertices, so this is
    // never hot, and the simple version is the one that is obviously right.
    size_t guard = poly.size() * poly.size() + 8;
    while (poly.size() > 3 && guard-- > 0) {
        bool clipped = false;
        for (size_t i = 0; i < poly.size(); ++i) {
            const size_t h = (i + poly.size() - 1) % poly.size();
            const size_t j = (i + 1) % poly.size();
            const Vec2f  a = to2d(poly[h]), b = to2d(poly[i]), c = to2d(poly[j]);
            // >= 0, not > 0: a COLLINEAR vertex has to be clippable. The side
            // rewrite deliberately produces them - a rail inserted on a boundary
            // edge is collinear with that edge's endpoints - and they are
            // load-bearing, because the strip and the end cap both reference them,
            // so they cannot simply be dropped from the polygon. Requiring a
            // strictly convex corner would leave them permanently un-clippable,
            // stall the loop and fall through to the fan, which then emits slivers.
            // Clipping a collinear ear costs one zero-area triangle, and
            // its_remove_degenerate_faces() takes that out at the end.
            if (cross2(a, b, c) < 0.)
                continue;                       // reflex: not an ear
            bool empty = true;
            for (size_t k = 0; k < poly.size() && empty; ++k) {
                if (k == h || k == i || k == j)
                    continue;
                if (inside(a, b, c, to2d(poly[k])))
                    empty = false;
            }
            if (!empty)
                continue;
            emit(poly[h], poly[i], poly[j]);
            poly.erase(poly.begin() + long(i));
            clipped = true;
            break;
        }
        if (!clipped)
            break;      // no ear found: degenerate input, fall through to the fan
    }
    if (poly.size() == 3) {
        emit(poly[0], poly[1], poly[2]);
    } else if (poly.size() > 3) {
        // Should not happen for a simple polygon, but a fan is better than a hole
        // and the closedness check will still catch it if it is wrong.
        for (size_t k = 1; k + 1 < poly.size(); ++k)
            emit(poly[0], poly[k], poly[k + 1]);
    }
}

// Find every genuinely open boundary of `out` - an edge used by exactly one
// facet - stitch them into loops and fill each one, counting the fills.
//
// Used for the bevel's corner patches. Deriving them from the assembled mesh
// rather than predicting them from the input is what makes the corner handling
// independent of how the holes came to be there.
//
// `capped` names the vertices where a strip END CAP was already emitted. A loop
// that only touches those is already closed, and filling it again is what made
// every edge of the cap at one cube vertex carry three facets instead of two
// (measured: tris=21 where 20 is right, corners=1, and edges (7,9) (7,11) (9,11)
// all at n=3). The filler is still the right tool for a real corner, where
// several bevelled edges meet and no cap was emitted - so it is skipped per
// loop, not disabled.
void fill_open_loops(indexed_triangle_set &out, size_t &patches, const std::set<int> &capped)
{
    // Directed edge (a -> b) appears once per facet using it in that direction.
    // On a closed surface each undirected edge carries one of each; a hole leaves
    // the boundary direction unmatched.
    std::map<std::pair<int, int>, int>    directed;
    std::map<std::pair<int, int>, size_t> facet_of_directed;
    for (size_t fi = 0; fi < out.indices.size(); ++fi) {
        const Vec3i32 &f = out.indices[fi];
        for (int s = 0; s < 3; ++s) {
            const auto key = std::make_pair(f[s], f[(s + 1) % 3]);
            ++directed[key];
            facet_of_directed.emplace(key, fi);
        }
    }

    // The successor is taken only where the UNDIRECTED edge really is short of a facet.
    // The directed test alone ("this direction has no reverse partner") also fires on an
    // edge that has both its facets wound the same way, which the bevel's producers do
    // on every corner edge - and filling that adds a third facet rather than closing
    // anything. Winding is settled globally by orient_consistently() after the build, so
    // the only question here is what is genuinely MISSING.
    std::map<std::pair<int, int>, int> undirected;
    for (const auto &d : directed) {
        const int u = d.first.first, v = d.first.second;
        undirected[u < v ? std::make_pair(u, v) : std::make_pair(v, u)] += d.second;
    }
    std::map<int, int> next;
    for (const auto &d : directed) {
        const int u = d.first.first, v = d.first.second;
        auto      it = undirected.find(u < v ? std::make_pair(u, v) : std::make_pair(v, u));
        if (it != undirected.end() && it->second == 1)
            next.emplace(u, v);
    }

    std::set<int> visited;
    for (const auto &seed : next) {
        if (visited.count(seed.first) > 0)
            continue;
        std::vector<int> loop;
        int              cur = seed.first;
        while (visited.insert(cur).second) {
            loop.push_back(cur);
            auto it = next.find(cur);
            if (it == next.end()) { loop.clear(); break; }
            cur = it->second;
            if (cur == seed.first) break;
        }
        if (loop.size() < 3)
            continue;

        // Already closed by a strip end cap: leave it alone. A loop that touches a
        // capped vertex IS that cap's own boundary, and filling it a second time is
        // what put three facets on each of the cap's edges.
        {
            bool on_cap = false;
            for (int c : loop)
                if (capped.count(c) > 0) {
                    on_cap = true;
                    break;
                }
            if (on_cap)
                continue;
        }

        // Outward direction for this hole: the mean normal of the facets that
        // border it. Those already face outward, so the patch must too.
        Vec3f vn = Vec3f::Zero();
        for (size_t i = 0; i < loop.size(); ++i) {
            auto it = facet_of_directed.find(std::make_pair(loop[(i + 1) % loop.size()], loop[i]));
            if (it == facet_of_directed.end())
                continue;
            const Vec3i32 &f = out.indices[it->second];
            vn += (out.vertices[f[1]] - out.vertices[f[0]]).cross(out.vertices[f[2]] - out.vertices[f[0]]);
        }

        // A three-sided hole IS a triangle - the ordinary cube corner - so it is
        // emitted as one rather than a centroid plus three slivers.
        if (loop.size() == 3) {
            const Vec3f p0 = out.vertices[loop[0]];
            const Vec3f p1 = out.vertices[loop[1]];
            const Vec3f p2 = out.vertices[loop[2]];
            if ((p1 - p0).cross(p2 - p0).dot(vn) < 0.f) out.indices.emplace_back(loop[0], loop[2], loop[1]);
            else                                        out.indices.emplace_back(loop[0], loop[1], loop[2]);
            ++patches;
            continue;
        }

        // Four or more: fan from the CENTROID, which stays valid for the
        // non-planar (often saddle-shaped) polygon a higher-valence patch is,
        // where a fan from one of its own vertices would fold.
        Vec3f centroid = Vec3f::Zero();
        for (int c : loop)
            centroid += out.vertices[c];
        centroid /= float(loop.size());
        const int cv = int(out.vertices.size());
        out.vertices.emplace_back(centroid);

        const bool flip = [&] {
            const Vec3f p0 = out.vertices[loop[0]];
            const Vec3f p1 = out.vertices[loop[1]];
            return (p0 - centroid).cross(p1 - centroid).dot(vn) < 0.f;
        }();
        for (size_t i = 0; i < loop.size(); ++i) {
            const int p = loop[i], q = loop[(i + 1) % loop.size()];
            if (p == q)
                continue;
            if (flip) out.indices.emplace_back(cv, q, p);
            else      out.indices.emplace_back(cv, p, q);
        }
        ++patches;
    }
}

// Make every facet agree with its neighbours about which way is out, by flooding an
// orientation across the mesh from one seed and flipping whatever disagrees, then
// flipping the whole result if the seed happened to face inward.
//
// WHY THIS IS HERE, measured. The bevel assembles its surface from three independent
// producers - the rewritten sides (ear-clipped against their own face normal), the
// strips (wound by emit_quad() against the mean of the two incident face normals) and
// the caps and corner patches. Each is individually correct and they do NOT agree with
// each other: the pair of facets meeting on a strip's END edge, where a side's chord
// runs into the strip's last band, came out traversing that edge in the SAME direction.
// Undirected, the edge has its two facets and is_closed_manifold() is satisfied; wound,
// it is inconsistent, and its_num_open_edges() - which watertight() uses - counts it as
// open. The probe said this unambiguously: at a chamfer corner the three edges read
// (8,9)=2/0, (8,24)=0/2, (9,24)=2/0, two facets each and none of them opposing.
//
// Chasing it back into the producers was the wrong shape of fix. Each one derives its
// winding from a normal, and at a corner the reference vectors are near-degenerate
// precisely where the agreement matters - the same trap the end caps fell into. The
// mutual agreement is a property of the assembled surface, so it is settled on the
// assembled surface, once, by the standard flood: on a closed manifold it is exact, it
// needs no geometry at all beyond the final inside/outside test, and it cannot be
// fooled by a degenerate reference. A patch or a strip that is locally wound "wrong" is
// no longer a bug to hunt.
//
// Deterministic: the facets are visited in index order from facet 0, and the final
// global flip is decided by the signed volume, so the same input gives the same output.
void orient_consistently(indexed_triangle_set &its)
{
    if (its.indices.empty())
        return;
    // Undirected edge -> the (up to two) facets on it.
    std::map<std::pair<int, int>, std::vector<size_t>> faces_of;
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const Vec3i32 &t = its.indices[f];
        for (int s = 0; s < 3; ++s) {
            const int u = t[s], v = t[(s + 1) % 3];
            faces_of[u < v ? std::make_pair(u, v) : std::make_pair(v, u)].push_back(f);
        }
    }

    std::vector<uint8_t> done(its.indices.size(), 0);
    std::vector<size_t>  stack;
    for (size_t seed = 0; seed < its.indices.size(); ++seed) {
        if (done[seed])
            continue;
        done[seed] = 1;
        stack.push_back(seed);
        while (!stack.empty()) {
            const size_t f = stack.back();
            stack.pop_back();
            const Vec3i32 t = its.indices[f];
            for (int s = 0; s < 3; ++s) {
                const int u = t[s], v = t[(s + 1) % 3];
                auto      it = faces_of.find(u < v ? std::make_pair(u, v) : std::make_pair(v, u));
                if (it == faces_of.end())
                    continue;
                for (size_t g : it->second) {
                    if (g == f || done[g])
                        continue;
                    // f runs u -> v, so g must run v -> u. If it runs u -> v too, flip it.
                    const Vec3i32 &tg = its.indices[g];
                    bool           same = false;
                    for (int k = 0; k < 3; ++k)
                        if (tg[k] == u && tg[(k + 1) % 3] == v) { same = true; break; }
                    if (same)
                        std::swap(its.indices[g](1), its.indices[g](2));
                    done[g] = 1;
                    stack.push_back(g);
                }
            }
        }
    }

    // The flood only makes the facets agree; it cannot know which way is out. Decide
    // that from the signed volume, which is positive for an outward-facing closed shell.
    double vol6 = 0.;
    for (const Vec3i32 &t : its.indices) {
        const Vec3f &a = its.vertices[t[0]];
        const Vec3f &b = its.vertices[t[1]];
        const Vec3f &c = its.vertices[t[2]];
        vol6 += double(a.dot(b.cross(c)));
    }
    if (vol6 < 0.)
        for (Vec3i32 &t : its.indices)
            std::swap(t(1), t(2));
}

} // namespace

bool is_closed_manifold(const indexed_triangle_set &its)
{
    if (its.indices.empty() || its.vertices.empty())
        return false;
    std::map<std::pair<int, int>, int> counts;
    for (const Vec3i32 &f : its.indices) {
        for (int s = 0; s < 3; ++s) {
            const int u = f[s], v = f[(s + 1) % 3];
            if (u == v)
                return false;
            if (u < 0 || v < 0 || size_t(u) >= its.vertices.size() || size_t(v) >= its.vertices.size())
                return false;
            ++counts[u < v ? std::make_pair(u, v) : std::make_pair(v, u)];
        }
    }
    for (const auto &c : counts)
        if (c.second != 2)
            return false;
    return true;
}

double chamfered_box_volume_loss(const Vec3d &size, double w)
{
    if (!(w > 0.))
        return 0.;
    const double a = size.x(), b = size.y(), c = size.z();
    if (a <= 2. * w || b <= 2. * w || c <= 2. * w)
        return 0.;
    // Each of the twelve edges loses the triangular prism 0.5 * w^2 * L along its
    // own length; four edges run parallel to each axis, so the prisms sum to
    //
    //     2 * w^2 * (a + b + c).
    //
    // Taking each L as the FULL side over-counts the eight corners, where the
    // three prisms meeting there overlap. At one corner the removed region is the
    // UNION of the three wedges {x+y<w}, {y+z<w}, {z+x<w}, and the sum of the three
    // counts it as if they were disjoint. The difference - sum minus union - works
    // out to 3/4 * w^3 per corner.
    //
    // Verified numerically against a Monte-Carlo integration of the chamfered box
    // for several sizes and widths before it was trusted here: the closed form
    // agrees to within the sampling noise (~0.1%), which is what lets the test
    // assert on it at 5%.
    const double prisms           = 2. * w * w * (a + b + c);
    const double corner_overcount = 8. * 0.75 * w * w * w;
    return prisms - corner_overcount;
}

// ---------------------------------------------------------------------------
// Pass 2: the global width solve
// ---------------------------------------------------------------------------
//
// One width per requested edge, and - the whole point - a pure function of the
// mesh and the edge SET. Every limit below is computed from quantities that do
// not depend on visit order, and the minimum of a set is order-independent, so
// permuting `edges` permutes the output and changes nothing else.
std::vector<float> solve_bevel_widths(const indexed_triangle_set &its,
                                      const MeshTopology         &topo,
                                      const std::vector<int>     &edges,
                                      const BevelParams          &params)
{
    std::vector<float> widths(edges.size(), 0.f);
    if (edges.empty() || !topo.valid() || params.width <= 0.f)
        return widths;

    const float frac = std::clamp(params.clamp_fraction, 0.05f, 0.95f);
    const float requested = params.width;

    // Which of the requested edges survive validation and the flatness drop. A
    // set, so membership tests below do not depend on position in `edges`.
    std::set<int> live;
    for (int e : edges) {
        if (e < 0 || e >= topo.num_edges)
            continue;
        if (topo.edge_face_count[e] != 2)
            continue; // non-manifold or boundary: refused elsewhere, skipped here
        if (edge_dihedral_deg(topo, e) < params.min_dihedral_deg)
            continue; // too flat to be worth it
        if (edge_length(its, topo, e) < 1e-9f)
            continue;
        // CONCAVE edges are dropped, and this is a real limitation rather than an
        // oversight: the corner split below pulls each face-corner back ALONG its
        // own face, which removes material. That is right for a convex edge, where
        // the material is on the inside of the fold - but a concave edge needs the
        // strip to ADD material into the valley, and pulling its corners back
        // instead makes the two rails cross and the strip fold through the solid.
        //
        // An inside corner is therefore left sharp and reported as dropped. The
        // interim "Round all edges" (MeshRound.hpp) DOES round concave edges, via
        // the level-set close, and is the tool to reach for until a concave strip
        // is built here.
        if (!edge_is_convex(its, topo, e))
            continue;
        live.insert(e);
    }
    if (live.empty())
        return widths;

    // --- limit (a): the edge's own incident facets ---------------------------
    //
    // The offset rail must stay inside both incident facets, so the width cannot
    // exceed a fraction of the facet's extent perpendicular to the edge - which is
    // its height over the edge, i.e. 2 * area / edge length.
    std::map<int, float> limit;
    for (int e : live) {
        const Vec2i32 ff = topo.edge_faces[e];
        const float   L  = edge_length(its, topo, e);
        float         lim = requested;
        for (int k = 0; k < 2; ++k) {
            const size_t f = size_t(ff(k));
            const float  twice_area = facet_area_normal(its, f).norm();
            const float  height = L > 1e-12f ? twice_area / L : 0.f;
            lim = std::min(lim, frac * height);
        }
        limit[e] = std::max(0.f, lim);
    }

    // --- limit (b): edges competing at a shared vertex ------------------------
    //
    // Two bevelled edges meeting at a vertex each eat into the edges around that
    // vertex. Neither may take more than `frac` of the SHORTEST edge at the
    // vertex, or the two strips meet and cross. Using the shortest incident edge
    // of the vertex - not of the pair - keeps this independent of which pair is
    // considered first.
    //
    // vertex -> the shortest length of any edge incident to it.
    std::map<int, float> vertex_min_edge;
    for (int e = 0; e < topo.num_edges; ++e) {
        if (topo.edge_face_count[e] == 0)
            continue;
        const Vec2i32 ev = topo.edge_vertices[e];
        if (ev(0) < 0)
            continue;
        const float L = edge_length(its, topo, e);
        for (int k = 0; k < 2; ++k) {
            auto it = vertex_min_edge.find(ev(k));
            if (it == vertex_min_edge.end())
                vertex_min_edge.emplace(ev(k), L);
            else
                it->second = std::min(it->second, L);
        }
    }

    // How many bevelled edges meet at each vertex: with three or more arriving,
    // the corner patch has to fit them all, so each gets a smaller share.
    std::map<int, int> bevelled_valence;
    for (int e : live) {
        const Vec2i32 ev = topo.edge_vertices[e];
        ++bevelled_valence[ev(0)];
        ++bevelled_valence[ev(1)];
    }

    for (int e : live) {
        const Vec2i32 ev = topo.edge_vertices[e];
        float         lim = limit[e];
        for (int k = 0; k < 2; ++k) {
            const int v = ev(k);
            auto      mit = vertex_min_edge.find(v);
            if (mit != vertex_min_edge.end())
                lim = std::min(lim, frac * mit->second);
            // With n bevelled edges at the vertex the offsets have to share the
            // room around it. n = 2 is the plain chain case and needs no extra
            // discount; beyond that each gets proportionally less.
            const int n = bevelled_valence[v];
            if (n > 2 && mit != vertex_min_edge.end())
                lim = std::min(lim, frac * mit->second * 2.f / float(n));
        }
        limit[e] = std::max(0.f, lim);
    }

    // --- limit (c): the sharpness of the edge itself --------------------------
    //
    // A very sharp edge is a thin fin with almost no material behind it, and the
    // two rails of a wide bevel there would meet before they reached the far side
    // of the fin. The available depth behind the edge scales with tan(interior/2),
    // so the width is scaled by the same factor once the angle drops below a right
    // angle (tan(45 deg) = 1, where the factor is the identity and a comfortable
    // edge is left entirely to limit (a)).
    //
    // The factor is floored at 0.1 so a near-degenerate fin still gets a small
    // positive width rather than being silently dropped - it is then the caller's
    // "clamped" report that tells the user, which is more use than nothing
    // happening.
    for (int e : live) {
        const float interior = edge_interior_angle(topo, e);
        if (interior <= 1e-4f || interior >= float(M_PI) - 1e-4f)
            continue;
        const float t = std::tan(interior * 0.5f);
        if (t < 1.f)
            limit[e] *= std::max(t, 0.1f);
    }

    for (size_t i = 0; i < edges.size(); ++i) {
        auto it = limit.find(edges[i]);
        widths[i] = it == limit.end() ? 0.f : it->second;
    }
    return widths;
}

// ---------------------------------------------------------------------------
// Pass 3: the build
// ---------------------------------------------------------------------------

BevelResult bevel_edges(const indexed_triangle_set &its,
                        const MeshTopology         &topo,
                        const std::vector<int>     &edges,
                        const BevelParams          &params)
{
    BevelResult res;

    if (its.indices.empty() || !topo.valid() || edges.empty()) {
        res.status = BevelStatus::EmptyChain;
        return res;
    }
    if (params.width <= 0.f || params.segments <= 0) {
        res.status = BevelStatus::NoOp;
        res.mesh   = its;
        return res;
    }

    const int segments = std::clamp(params.segments, 1, BevelMaxSegments);
    // A chamfer IS the one-segment case; collapsing it here means there is exactly
    // one geometry path and the "N=1 round equals a chamfer" test is true by
    // construction rather than by luck.
    const int rings = params.profile == BevelProfile::Chamfer ? 1 : segments;

    // --- validate -----------------------------------------------------------
    // Deduplicate first: the same edge asked for twice is a caller convenience,
    // not an error, but bevelling it twice would be.
    std::set<int> requested;
    for (int e : edges) {
        if (e < 0 || e >= topo.num_edges)
            continue;
        // A non-manifold edge has no well-defined two sides. Refuse the whole
        // operation rather than silently skipping it: the user selected it.
        if (topo.edge_face_count[e] > 2) {
            res.status = BevelStatus::NonManifold;
            return res;
        }
        if (topo.edge_face_count[e] != 2)
            continue; // boundary edge: nothing to bevel between
        requested.insert(e);
    }
    if (requested.empty()) {
        res.status = BevelStatus::EmptyChain;
        return res;
    }

    // --- solve widths globally, BEFORE any geometry -------------------------
    const std::vector<int> ordered(requested.begin(), requested.end());
    const std::vector<float> solved = solve_bevel_widths(its, topo, ordered, params);

    std::map<int, float> width_of;
    float                w_min = std::numeric_limits<float>::max(), w_max = 0.f;
    for (size_t i = 0; i < ordered.size(); ++i) {
        if (solved[i] <= 1e-6f) {
            if (edge_dihedral_deg(topo, ordered[i]) < params.min_dihedral_deg)
                ++res.dropped_flat;
            else if (!edge_is_convex(its, topo, ordered[i]))
                ++res.dropped_concave;
            continue;
        }
        width_of[ordered[i]] = solved[i];
        w_min = std::min(w_min, solved[i]);
        w_max = std::max(w_max, solved[i]);
    }
    if (width_of.empty()) {
        // Say which of the three reasons it was: nothing to bevel (flat or
        // concave) is a different answer from "your width does not fit".
        res.status = (res.dropped_flat > 0 || res.dropped_concave > 0) ? BevelStatus::EmptyChain
                                                                       : BevelStatus::WidthTooSmall;
        return res;
    }
    res.bevelled_edges = width_of.size();
    res.min_width      = w_min;
    res.max_width      = w_max;
    res.clamped        = w_min < params.width - 1e-5f;

    // --- sides -----------------------------------------------------------------
    //
    // Facets grouped into maximal sets connected through edges that are neither
    // being bevelled nor a crease: a side is one flat CAD face, and it is the unit
    // the construction below rewrites. A cube is exactly 6 sides.
    //
    // Both halves of the condition matter. Stopping at bevelled edges makes the
    // bevel a cut between sides; stopping at creases as well keeps a side PLANAR,
    // which the ear clip relies on - and it means a side never spans two
    // differently-oriented faces, while the coplanar diagonals INSIDE a flat face
    // are still crossed freely, so a face is never split along one.
    //
    // The crease threshold is the same min_dihedral_deg the solve uses to decide
    // an edge is too flat to bevel, so the two agree by construction.
    std::vector<int> side_of(its.indices.size(), -1);
    {
        int                 next_side = 0;
        std::vector<size_t> stack;
        for (size_t seed = 0; seed < its.indices.size(); ++seed) {
            if (side_of[seed] >= 0)
                continue;
            const int side = next_side++;
            stack.push_back(seed);
            side_of[seed] = side;
            while (!stack.empty()) {
                const size_t f = stack.back();
                stack.pop_back();
                for (int j = 0; j < 3; ++j) {
                    const int nb = topo.face_neighbors[f][j];
                    if (nb < 0 || side_of[size_t(nb)] >= 0)
                        continue;
                    const int e = topo.face_edge_ids[f][j];
                    if (e >= 0 && width_of.count(e) > 0)
                        continue;   // a bevelled edge parts two sides
                    if (e >= 0 && edge_dihedral_deg(topo, e) >= params.min_dihedral_deg)
                        continue;   // so does any other crease
                    side_of[size_t(nb)] = side;
                    stack.push_back(size_t(nb));
                }
            }
        }
    }

    // --- the construction ------------------------------------------------------
    //
    // INSERT the rail vertices and re-triangulate the faces around them. No
    // original vertex ever moves.
    //
    // That last sentence is the whole design, and it is the research spec's own
    // step (3) - "re-cut the incident facets against the offset line in their own
    // plane". An earlier attempt deviated from it, displacing each vertex once per
    // side instead, to avoid having to re-triangulate. That does not work, and the
    // reason is worth keeping: a side's copy of a vertex is shared by that side's
    // WHOLE boundary at that vertex, so moving it detaches the side from its
    // neighbours along the entire shared edge, not just near the bevel. A
    // single-edge chamfer then opens a sliver down every adjacent edge. Inserting
    // instead of moving cannot do that, because a face that no bevel touches keeps
    // its original triangles exactly.
    //
    // The three pieces:
    //
    //   RAILS  - per bevelled edge e = (a, b), and per incident SIDE, the edge
    //            pushed back by w along that side's in-plane normal. Four points
    //            per bevelled edge.
    //   SIDES  - each side's boundary loop is rewritten: a boundary edge that is
    //            bevelled contributes its two RAIL points instead of its two
    //            original ones, and everything else is left alone. The resulting
    //            polygon is planar (a side is a planar region by construction) and
    //            is ear-clipped in its own plane.
    //   STRIPS - the quad between the two rails of an edge, one band for a
    //            chamfer, `rings` bands along the tangent arc for a round.
    //
    // What is left over at a vertex where several bevelled edges meet is the
    // corner patch, and it is found from the genuinely open edges of the assembled
    // mesh rather than predicted - see the end of this function.

    indexed_triangle_set out;
    out.vertices = its.vertices;

    // Which bevelled edges touch each vertex, and which bound each side.
    std::map<int, std::vector<int>> edges_at_vertex;
    for (const auto &kv : width_of) {
        const Vec2i32 ev = topo.edge_vertices[kv.first];
        edges_at_vertex[ev(0)].push_back(kv.first);
        edges_at_vertex[ev(1)].push_back(kv.first);
    }

    // --- the inset corner points -----------------------------------------------
    //
    // ONE POINT PER (SIDE, VERTEX), not one per (edge, side, vertex).
    //
    // This is the rail model, and getting it wrong is what the previous revision
    // measured but could not name. `rail_of()` used to return `v + w*t` - the edge
    // pushed back by w along the incident face's in-plane normal - and used that
    // same point for the side polygon, for the strip and for the corner patch. On
    // the 20 mm cube at w = 2, corner (20,20,0), that puts the two points of face
    // z = 0 at (20,18,0) and (18,20,0), BOTH OF THEM ON THE CUBE'S OWN EDGES. The
    // side rewrite then dropped v and chorded between them - but the vertical
    // edge's strip had its bottom end pair at exactly that same ((20,18,0),
    // (18,20,0)) too, so the chord edge was claimed twice, no hole was left for the
    // corner triangle, and the surface closed by count while pinching:
    // corner_patches=0, is_closed_manifold true, removed 5626 where 432 is right.
    //
    // The correct geometry says what the point should have been. A face's polygon
    // corner is the intersection of ITS OWN TWO OFFSET LINES: on z = 0 the line
    // y = 18 (the offset of the -x edge) meets x = 18 (the offset of the -y edge)
    // at the single point (18,18,0). A chamfered cube is 6 octagons + 12 rectangles
    // + 8 triangles, and each octagon's corner is that ONE point, not a two-rail
    // chord. Checked against the closed form before this was written: the convex
    // hull of the 24 inset points of a 20 mm cube at w = 2 loses 437.3 against the
    // closed form's 432.0, 1.2% - inside the test's 5%, and the residue is exactly
    // the eight corner tetrahedra the chamfer planes would also have left.
    //
    // The same point then serves all three producers, which is what closes the
    // surface: the strip quad of an edge runs between the inset points of the two
    // ADJACENT SIDES at each of its endpoints, and the corner patch is the ring of
    // inset points of the sides around the vertex. Every seam is then an edge
    // between two inset points, carried by exactly one facet from each side of it.
    //
    // Degrees at the vertex, within one side:
    //   two bevelled boundary edges  -> intersect the two offset lines (the case
    //                                   above, and the ordinary corner);
    //   one                          -> keep the old rail, v + w*t, and the strip
    //                                   end cap that goes with it. The face still
    //                                   reaches v along its un-bevelled edge, so
    //                                   there is no corner to cut, only a point on
    //                                   the way there;
    //   three or more (a pinched side boundary passing through v twice) -> the mean
    //                                   of the pairwise intersections, which
    //                                   degenerates to the same answer when they
    //                                   agree and stays inside the face when they
    //                                   do not.
    //
    // Vertices where a strip END CAP was emitted, so the corner filler can tell
    // an already-closed strip end from a real multi-bevel corner.
    std::set<int> capped_vertices;

    // Which bevelled edges of each SIDE's boundary meet at each vertex. Built from
    // the facet adjacency rather than from the side boundary walk below, so it is
    // available before any geometry is emitted: an edge is on side s's boundary
    // when one of its two facets is in s and the other is not.
    std::map<std::pair<int, int>, std::vector<int>> side_bev_at; // (side, vertex) -> edges
    for (const auto &kv : width_of) {
        const int     e  = kv.first;
        const Vec2i32 ff = topo.edge_faces[e];
        const Vec2i32 ev = topo.edge_vertices[e];
        for (int k = 0; k < 2; ++k) {
            if (ff(k) < 0)
                continue;
            const int s = side_of[size_t(ff(k))];
            for (int j = 0; j < 2; ++j)
                side_bev_at[std::make_pair(s, ev(j))].push_back(e);
        }
    }
    // Deterministic order, and no duplicate when both facets of an edge somehow
    // landed in one side (which the side split forbids, but cheap to be sure of).
    for (auto &kv : side_bev_at) {
        std::sort(kv.second.begin(), kv.second.end());
        kv.second.erase(std::unique(kv.second.begin(), kv.second.end()), kv.second.end());
    }

    // The old per-(edge, side, vertex) rail point, v + w*t. Still the answer when
    // only one bevelled edge of the side meets at v, and still the direction the
    // offset lines are built from.
    auto rail_point = [&](int e, int side, int v, Vec3f *out_t) -> bool {
        const Vec2i32 ff = topo.edge_faces[e];
        size_t        on_side = size_t(-1);
        for (int k = 0; k < 2; ++k)
            if (ff(k) >= 0 && side_of[size_t(ff(k))] == side)
                on_side = size_t(ff(k));
        if (on_side == size_t(-1))
            return false;
        const Vec2i32 ev = topo.edge_vertices[e];
        const int     u  = ev(0) == v ? ev(1) : ev(0);
        const Vec3f   t  = in_plane_normal(its, topo, on_side, v, u);
        if (t.squaredNorm() < 1e-18f)
            return false;
        *out_t = width_of.at(e) * t;
        return true;
    };

    // inset[(side, vertex)] -> index of the inserted point.
    std::map<std::pair<int, int>, int> inset;

    // ...and the same points keyed by POSITION, per original vertex.
    //
    // Still needed, and for the same reason the per-(edge, side) key needed it: two
    // sides that meet at v along an UN-bevelled crease inset to the same point
    // whenever the bevelled edges they see are the same ones - a chain that stops
    // at v presents exactly that - and the strips, the rewritten sides and the
    // corner loops must all name one index for one point or they do not share an
    // edge and the surface is not closed. Merging afterwards is too late: by then
    // the topology has been built around the duplicates (measured before the
    // dedupe existed: coincident_dups=24 on the all-12 cube, and the merge welded
    // them and collapsed every patch the filler had just made).
    //
    // Keyed by a quantised position rather than by exact float equality: the two
    // computations that land on one point run through different face normals and
    // different cross products, so they agree to a few ULPs rather than bitwise.
    // The quantisation is coarse against that error (1e-4 mm) and fine against any
    // real separation (the solve keeps a width well above it), and it is applied to
    // a value that is deterministic, so the same input still gives the same output
    // bit for bit. The key is per ORIGINAL VERTEX as well as per position, so two
    // genuinely distinct corners of a tiny feature can never collide across the mesh.
    std::map<std::pair<int, std::tuple<int64_t, int64_t, int64_t>>, int> inset_by_pos;
    auto pos_key = [](const Vec3f &p) {
        // llround, not a truncating cast: a cast rounds toward zero, so two values
        // straddling zero by an ULP land in different buckets.
        const double q = 1.0e4;
        return std::make_tuple(int64_t(std::llround(double(p.x()) * q)),
                               int64_t(std::llround(double(p.y()) * q)),
                               int64_t(std::llround(double(p.z()) * q)));
    };

    // Intersect the two offset lines of edges `e0` and `e1` inside side `side`'s
    // plane, at the shared vertex v.
    //
    // Each offset line is {v + o_i + s * d_i}, where o_i is the edge's own offset
    // (w_i along the face's in-plane normal) and d_i the edge's direction away from
    // v. Two lines in a plane, so the intersection is the 2x2 solve of
    // o0 + s0*d0 = o1 + s1*d1 projected onto the plane's own basis. Solved in that
    // 2D basis rather than by a 3D least-squares because the plane is exact here (a
    // side is planar by construction) and the 2D determinant is the sine of the
    // corner angle, which is the quantity the clamp already keeps away from zero.
    auto intersect_offsets = [&](int side, int v, int e0, int e1, const Vec3f &n, Vec3f *outp) -> bool {
        Vec3f o0, o1;
        if (!rail_point(e0, side, v, &o0) || !rail_point(e1, side, v, &o1))
            return false;
        const Vec2i32 ev0 = topo.edge_vertices[e0];
        const Vec2i32 ev1 = topo.edge_vertices[e1];
        const Vec3f   d0  = (its.vertices[ev0(0) == v ? ev0(1) : ev0(0)] - its.vertices[v]).normalized();
        const Vec3f   d1  = (its.vertices[ev1(0) == v ? ev1(1) : ev1(0)] - its.vertices[v]).normalized();

        // A 2D basis of the side's plane. Any orthonormal pair spanning it will do;
        // taking it from d0 keeps the determinant below equal to sin(corner angle).
        Vec3f bx = d0 - n * d0.dot(n);
        if (bx.squaredNorm() < 1e-18f)
            return false;
        bx.normalize();
        const Vec3f by = n.cross(bx);

        auto to2 = [&](const Vec3f &p) { return Vec2f(p.dot(bx), p.dot(by)); };
        const Vec2f p0 = to2(o0), p1 = to2(o1);
        const Vec2f a0 = to2(d0), a1 = to2(d1);

        // p0 + s0*a0 = p1 + s1*a1  ->  s0*a0 - s1*a1 = p1 - p0
        const float det = a0.x() * (-a1.y()) - (-a1.x()) * a0.y();
        if (std::abs(det) < 1e-7f)
            return false;   // the two edges are collinear at v: no corner to cut
        const Vec2f rhs = p1 - p0;
        const float s0  = (rhs.x() * (-a1.y()) - (-a1.x()) * rhs.y()) / det;
        *outp = its.vertices[v] + o0 + d0 * s0;
        return true;
    };

    auto inset_of = [&](int side, int v) -> int {
        const auto key = std::make_pair(side, v);
        auto       it  = inset.find(key);
        if (it != inset.end())
            return it->second;

        auto bit = side_bev_at.find(key);
        if (bit == side_bev_at.end() || bit->second.empty())
            return -1;
        const std::vector<int> &es = bit->second;

        // The side's plane. Any of its facets gives it; they are coplanar by the
        // side split's own condition.
        Vec3f n = Vec3f::Zero();
        {
            const Vec2i32 ff = topo.edge_faces[es.front()];
            for (int k = 0; k < 2; ++k)
                if (ff(k) >= 0 && side_of[size_t(ff(k))] == side)
                    n = topo.face_normals[size_t(ff(k))];
        }
        if (n.squaredNorm() < 1e-18f)
            return -1;

        Vec3f p;
        if (es.size() == 1) {
            // Only one bevelled edge of this side reaches v: there is no corner to
            // cut here, just the rail on the way to a vertex the face still keeps.
            Vec3f o;
            if (!rail_point(es.front(), side, v, &o))
                return -1;
            p = its.vertices[v] + o;
        } else {
            // Two (the ordinary corner) or, for a pinched boundary, more: the mean
            // of the pairwise intersections. With two that IS the intersection.
            Vec3f  acc = Vec3f::Zero();
            int    got = 0;
            for (size_t i = 0; i < es.size(); ++i)
                for (size_t j = i + 1; j < es.size(); ++j) {
                    Vec3f q;
                    if (intersect_offsets(side, v, es[i], es[j], n, &q)) {
                        acc += q;
                        ++got;
                    }
                }
            if (got == 0) {
                // Collinear offsets (two bevelled edges continuing straight through
                // v): the offsets coincide, so either rail is the answer.
                Vec3f o;
                if (!rail_point(es.front(), side, v, &o))
                    return -1;
                p = its.vertices[v] + o;
            } else {
                p = acc / float(got);
            }
        }

        const auto pkey = std::make_pair(v, pos_key(p));
        auto       pit  = inset_by_pos.find(pkey);
        if (pit != inset_by_pos.end()) {
            // Another side already inserted this exact point. Share it.
            inset.emplace(key, pit->second);
            return pit->second;
        }
        const int nv = int(out.vertices.size());
        out.vertices.emplace_back(p);
        inset.emplace(key, nv);
        inset_by_pos.emplace(pkey, nv);
        return nv;
    };

    // --- rewrite each side -----------------------------------------------------
    //
    // A side's boundary is the cycle of its edges that are not interior to it. It
    // is walked as a sequence of (vertex, edge) steps so the rewrite can see, at
    // every corner, whether the edge arriving and the edge leaving are bevelled.

    // side -> its facets.
    std::map<int, std::vector<size_t>> facets_of_side;
    for (size_t f = 0; f < its.indices.size(); ++f)
        facets_of_side[side_of[f]].push_back(f);

    for (const auto &sv : facets_of_side) {
        const int side = sv.first;

        // Does any bevelled edge bound this side? If not, the side is untouched
        // and its facets are emitted exactly as they were - which is the property
        // that makes this construction safe.
        bool touched = false;
        for (size_t f : sv.second) {
            for (int j = 0; j < 3 && !touched; ++j) {
                const int e = topo.face_edge_ids[f][j];
                if (e >= 0 && width_of.count(e) > 0)
                    touched = true;
            }
            if (touched)
                break;
        }
        if (!touched) {
            for (size_t f : sv.second)
                out.indices.emplace_back(its.indices[f]);
            continue;
        }

        // The side's boundary, as directed edges (u -> v) that have no partner
        // inside the side. Each facet contributes the sides whose neighbour is on
        // another side (or nothing).
        std::map<int, int>           nxt;      // u -> v along the boundary
        std::map<std::pair<int, int>, int> bedge; // (u,v) -> global edge id
        for (size_t f : sv.second) {
            const auto &tri = its.indices[f];
            for (int j = 0; j < 3; ++j) {
                const int nb = topo.face_neighbors[f][j];
                if (nb >= 0 && side_of[size_t(nb)] == side)
                    continue; // interior to the side
                const int u = tri[j], v = tri[(j + 1) % 3];
                nxt[u] = v;
                bedge[std::make_pair(u, v)] = topo.face_edge_ids[f][j];
            }
        }
        if (nxt.empty())
            continue;

        // Walk it. A side whose boundary is not a single clean cycle is left as
        // its original facets - refusing to guess is better than emitting a fold,
        // and the closedness check will report it if it matters.
        std::vector<int> loop;
        {
            const int start = nxt.begin()->first;
            int       cur   = start;
            std::set<int> seen;
            while (seen.insert(cur).second) {
                loop.push_back(cur);
                auto it = nxt.find(cur);
                if (it == nxt.end()) { loop.clear(); break; }
                cur = it->second;
                if (cur == start) break;
            }
            if (cur != start || loop.size() != nxt.size())
                loop.clear();
        }
        if (loop.size() < 3) {
            for (size_t f : sv.second)
                out.indices.emplace_back(its.indices[f]);
            continue;
        }

        // Rewrite: at each boundary vertex emit, in boundary order, the rail of the
        // arriving bevelled edge, the vertex itself only when NEITHER of its two
        // boundary edges is bevelled, and then the rail of the leaving one.
        //
        // In other words a vertex is REPLACED by the rails of whichever of its
        // edges are being bevelled. Worked through on the cube: chamfering the
        // 4-7 edge turns the +Z square [4, 5, 6, 7] into [railA, 5, 6, railB],
        // which is the square with a w-wide band removed along that edge - area
        // 10x10 - w*10, exactly right.
        //
        // Keeping v ALONGSIDE its rail (tried, and wrong) makes the polygon
        // self-touching, because the rail lies on the very edge v->next that
        // would follow it.
        std::vector<int> poly;
        poly.reserve(loop.size() * 2);
        for (size_t i = 0; i < loop.size(); ++i) {
            const int v    = loop[i];
            const int prev = loop[(i + loop.size() - 1) % loop.size()];
            const int next = loop[(i + 1) % loop.size()];

            auto edge_between = [&](int x, int y) {
                auto it = bedge.find(std::make_pair(x, y));
                return it == bedge.end() ? -1 : it->second;
            };
            const int  e_in    = edge_between(prev, v);   // arriving at v
            const int  e_out   = edge_between(v, next);   // leaving v
            const bool in_bev  = e_in  >= 0 && width_of.count(e_in)  > 0;
            const bool out_bev = e_out >= 0 && width_of.count(e_out) > 0;

            auto push = [&](int idx) {
                if (idx >= 0 && (poly.empty() || poly.back() != idx))
                    poly.push_back(idx);
            };

            // BOTH boundary edges bevelled: the corner is cut away and replaced by
            // the SINGLE inset point where this face's own two offset lines meet.
            // One point, not a two-rail chord - that chord is exactly what pinched
            // the surface, because the strip of the third edge meeting at this
            // vertex claimed the very same pair as its own end, leaving no hole for
            // the corner patch (measured on the all-12 chamfer: corner_patches=0,
            // removed 5626 against 432). On z = 0 of the cube the chord
            // (20,18,0)-(18,20,0) becomes the point (18,18,0), which is the
            // octagon's corner.
            //
            // Only ONE bevelled: v USUALLY stays. The face still reaches the vertex
            // along its un-bevelled edge, and the inset point (which for this case
            // IS the old rail, v + w*t) is an extra point on the way there.
            // Dropping it leaves that un-bevelled edge with one facet on one side
            // and none on the other (measured: the single-edge chamfer came back
            // non-closed, with edge 4-5 open).
            //
            // EXCEPT when the inset point lands ON that un-bevelled edge, which it
            // does whenever the bevelled edge's offset line is parallel to it. Then
            // v is BEYOND the inset point along the same line, and keeping it makes
            // the polygon a zero-area spike: the ear clip trims the spike, emits the
            // chord from the far vertex straight back to the inset point, and that
            // chord is a fourth facet on an edge that already had two. Measured on
            // the chamfered box's rim chain, whose -X face keeps 0 = (0,0,20) after
            // its inset 16 = (0,0,19), with the far vertex 4 = (0,0,0) - all three
            // on x = 0, y = 0: edge (4,16) came back carrying FOUR facets, open=0,
            // nonmanifold=2, and the same at (9,17) on the other end.
            //
            // And the test that decides it is TOPOLOGICAL, not geometric - which is
            // the thing three geometric attempts got wrong before the probe settled
            // it. Collinearity cannot be the discriminator, because the cube's
            // single-edge chamfer has exactly the same geometry and needs v KEPT:
            // there too the inset sits on the un-bevelled edge, strictly between v
            // and the far vertex (measured: side=1 v=4 (10,10,10) inset=8 (9,10,10)
            // far=5 (0,10,10)). Testing collinearity dropped v on both and took the
            // suite from 20/21 to 15/21.
            //
            // What is different about the box is that TWO sides resolve to the SAME
            // inset point. Side 4 is the -X wall and side 5 the -Y end cap; both
            // contain vertex 0, each has one rim edge bevelled, and the two offset
            // lines happen to meet the shared un-bevelled edge 0->4 at the same
            // place, so the position dedupe - correctly - hands them one index, 16
            // at (0,0,19). That point is then a SEAM between the two faces, and the
            // stretch from v down to it is on the far side of the seam: if both
            // faces keep v as well, each emits the run through v and the piece
            // 16..0 is covered from both, which is what put four facets on (4,16)
            // and (9,17).
            //
            // So: v is redundant exactly when some OTHER side at this vertex already
            // resolved to this same inset point. One side owning the point is the
            // cube case (keep v); two sides sharing it is the box case (drop it).
            // Every side at this vertex is resolved FIRST, so the count below does not
            // depend on the order the sides happen to be rewritten in - `inset` is
            // filled lazily, and a test that read it half-built would answer
            // differently for the first side than for the second and break the
            // determinism the whole pass is built around.
            auto v_is_redundant = [&](int inset_idx) {
                if (inset_idx < 0)
                    return false;
                int owners = 0;
                for (const auto &kv : side_bev_at) {
                    if (kv.first.second != v)
                        continue;                       // a different vertex
                    if (inset_of(kv.first.first, v) == inset_idx)
                        ++owners;
                }
                return owners > 1;
            };

            if (in_bev && out_bev) {
                push(inset_of(side, v));
            } else if (in_bev) {
                const int ip = inset_of(side, v);
                push(ip);
                if (!v_is_redundant(ip))
                    push(v);
            } else if (out_bev) {
                const int ip = inset_of(side, v);
                if (!v_is_redundant(ip))
                    push(v);
                push(ip);
            } else {
                push(v);
            }
        }
        while (poly.size() > 1 && poly.front() == poly.back())
            poly.pop_back();
        if (poly.size() < 3)
            continue;

        // Triangulate the rewritten polygon in the side's own plane. A side is
        // planar by construction (it is bounded by creases), so a 2D ear clip in
        // that plane is exact and cannot fold the way a 3D fan would.
        const Vec3f n = topo.face_normals[sv.second.front()];
        triangulate_planar_polygon(out, poly, n);
    }

    // --- the strips ------------------------------------------------------------
    //
    // The open END of every strip, recorded as the strip actually wound it, so the
    // corner patches below are built from the edges that are genuinely there rather
    // than hunted for afterwards. Keyed (vertex, edge): the ring of rail points the
    // strip of `edge` leaves exposed at `vertex`, from its side-0 rail to its
    // side-1 rail (two entries for a chamfer, `rings + 1` for a round).
    // Only the POINTS are needed, not their order: the corner walk below uses them
    // as seeds into the assembled mesh's own open boundary, and takes the direction
    // from there rather than from the strip.
    std::map<std::pair<int, int>, std::vector<int>> strip_end_ring;

    for (const auto &kv : width_of) {
        const int     e  = kv.first;
        const float   w  = kv.second;
        const Vec2i32 ev = topo.edge_vertices[e];
        const Vec2i32 ff = topo.edge_faces[e];
        const int     a = ev(0), b = ev(1);
        const size_t  f0 = size_t(ff(0)), f1 = size_t(ff(1));
        const int     s0 = side_of[f0], s1 = side_of[f1];

        // The strip runs between the INSET POINTS of the two adjacent sides, at each
        // of the edge's two endpoints - the same points the side polygons were
        // rewritten around, which is what makes each seam an edge shared by exactly
        // one facet from either side of it. On the cube's vertical edge at
        // (20,20,z) that is (20,18,2) on face x = 20 and (18,20,2) on face y = 20 at
        // the bottom, and the same pair at z = 18 at the top: the chamfer rectangle
        // of the 6 + 12 + 8 decomposition, no longer a band reaching down onto the
        // cube's own edges.
        const int a0 = inset_of(s0, a), b0 = inset_of(s0, b);
        const int a1 = inset_of(s1, a), b1 = inset_of(s1, b);
        if (a0 < 0 || b0 < 0 || a1 < 0 || b1 < 0)
            continue;

        // The strip must face OUTWARD like the faces it joins, so rather than
        // hard-code a winding and hope the mesh is wound the way the derivation
        // assumed, every quad goes through a helper that checks its own normal
        // against the two incident face normals and flips the pair if it got it
        // wrong. (The hand-derived winding WAS inverted when this was first
        // written, caught by working the cube corner through on paper.)
        const Vec3f outward = topo.face_normals[f0] + topo.face_normals[f1];

        // Which direction each rail-to-rail edge runs in the strip that was
        // actually emitted. The end caps read this back: a cap sits on the strip's
        // open end and must use the REVERSE of the strip's own directed edge, or
        // the two facets sharing that edge agree in direction instead of opposing
        // and the surface is closed but inconsistently wound.
        std::map<std::pair<int, int>, bool> strip_dir; // (x,y) present => strip runs x -> y

        auto emit_quad = [&](int p0, int p1, int q1, int q0) {
            const Vec3f A = out.vertices[p0];
            const Vec3f B = out.vertices[p1];
            const Vec3f C = out.vertices[q1];
            const bool  flip = (B - A).cross(C - A).dot(outward) < 0.f;
            auto tri = [&](int x, int y, int z) {
                if (x == y || y == z || x == z)
                    return;
                if (flip) out.indices.emplace_back(x, z, y);
                else      out.indices.emplace_back(x, y, z);
            };
            tri(p0, p1, q1);
            tri(p0, q1, q0);
            // p0->p1 and q1->q0 are the two ends of this band (the rail pairs at
            // the edge's two endpoints); record them as the strip wound them.
            if (flip) {
                strip_dir[std::make_pair(p1, p0)] = true;
                strip_dir[std::make_pair(q0, q1)] = true;
            } else {
                strip_dir[std::make_pair(p0, p1)] = true;
                strip_dir[std::make_pair(q1, q0)] = true;
            }
        };

        // END CAPS. Where a bevelled edge STOPS at a vertex - because no other
        // bevelled edge continues through it - the strip has an open end, and the
        // triangle (v, rail_on_side_0, rail_on_side_1) is what closes it. That
        // triangle lies in the plane of the faces that still meet at v, so it adds
        // no volume of its own; it simply caps the wedge the strip cut out.
        //
        // It has to be emitted HERE rather than left to the generic hole filler at
        // the end. The hole at such a vertex is not the little triangle - it is a
        // polygon that reaches all the way around v across the faces that were NOT
        // rewritten (on a cube, a pentagon spanning the +Y face), and filling THAT
        // with a centroid fan bulges a patch into the solid: measured, the
        // single-edge chamfer came back at volume 998.3 where 995.0 was right, with
        // a stray vertex at (7.8, y, 7.8) in the middle of the part. Capping the
        // strip end first leaves nothing for the filler to get wrong.
        auto cap_end = [&](int v, int r0, int r1) {
            // Only when the vertex is not carried on by another bevelled edge: if
            // it is, the two strips meet there and the corner patch is the right
            // answer instead.
            auto it = edges_at_vertex.find(v);
            if (it != edges_at_vertex.end() && it->second.size() > 1)
                return;
            if (r0 == r1 || v == r0 || v == r1)
                return;

            // Orient against the strip facet this cap actually adjoins, NOT against
            // `outward`. `outward` is the mean of the two incident FACE normals; it
            // is perpendicular to the edge, while a cap lies roughly perpendicular
            // to the FACES at one end of it - so the sign of their dot product is
            // near-degenerate and comes out right at one end of the edge and wrong
            // at the other. (Measured: for the cube's 7->4 edge the caps were
            // T19 (4,8,10) correct and T18 (7,9,11) flipped, whose three directed
            // edges each occurred twice forward and never reversed. That passes the
            // undirected is_closed_manifold() and fails the winding-aware
            // its_num_open_edges(), which is why watertight() reported status 0.)
            //
            // The strip already knows which way it wound r0..r1; the cap shares that
            // edge and must run it backwards.
            bool have = false, rev = false;
            if (strip_dir.count(std::make_pair(r0, r1)) > 0) { have = true; rev = true;  }
            else if (strip_dir.count(std::make_pair(r1, r0)) > 0) { have = true; rev = false; }

            if (have) {
                // strip ran r0->r1  =>  cap must run r1->r0, i.e. (v, r1, r0).
                if (rev) out.indices.emplace_back(v, r1, r0);
                else     out.indices.emplace_back(v, r0, r1);
            } else {
                // No strip band on this pair (should not happen; kept so a caller
                // that reaches here still gets a plausible triangle rather than
                // none). Fall back to the face-normal reference.
                const Vec3f A = out.vertices[v], B = out.vertices[r0], C = out.vertices[r1];
                if ((B - A).cross(C - A).dot(outward) < 0.f) out.indices.emplace_back(v, r1, r0);
                else                                        out.indices.emplace_back(v, r0, r1);
            }
            capped_vertices.insert(v);
        };

        // Hand the strip's two open ends to the corner fill. A chamfer's end is the
        // rail pair; a round's is its whole arc ring.
        auto note_ends = [&](const std::vector<int> &ring_a, const std::vector<int> &ring_b) {
            strip_end_ring[std::make_pair(a, e)] = ring_a;
            strip_end_ring[std::make_pair(b, e)] = ring_b;
        };

        if (rings <= 1) {
            emit_quad(a0, a1, b1, b0);
            note_ends({a0, a1}, {b0, b1});
            cap_end(a, a0, a1);
            cap_end(b, b0, b1);
            continue;
        }

        // Round: interpolate `rings` bands along the arc tangent to both faces.
        const Vec3f t0 = in_plane_normal(its, topo, f0, a, b);
        const Vec3f t1 = in_plane_normal(its, topo, f1, a, b);
        const float interior = edge_interior_angle(topo, e);
        Vec3f       bis = t0 + t1;
        const float bl  = bis.norm();
        if (!(interior > 1e-3f) || interior > float(M_PI) - 1e-3f || bl < 1e-9f) {
            emit_quad(a0, a1, b1, b0);   // degenerate angle: the chamfer is right
            note_ends({a0, a1}, {b0, b1});
            cap_end(a, a0, a1);
            cap_end(b, b0, b1);
            continue;
        }
        bis /= bl;
        const float half = interior * 0.5f;
        // The arc centre sits on the inward bisector, at the point equidistant
        // from both rails: each rail is w from the edge along its own face and the
        // bisector makes `half` with each, so that distance is w / cos(half) -
        // the spec's r / sin(theta/2) written for the offset rather than the
        // radius. Checked numerically on the cube: for a 90 deg edge at w = 1 the
        // centre lands at sqrt(2) and both rails are exactly 1 from it, so the arc
        // is a true tangent quarter-circle.
        const float centre_dist = w / std::max(std::cos(half), 1e-3f);

        auto arc_points = [&](int v, int cA, int cB) {
            std::vector<int> ring;
            ring.reserve(size_t(rings) + 1);
            ring.push_back(cA);
            // Values, not references: out.vertices grows inside this loop.
            //
            // The centre slides ALONG THE EDGE with the inset points. At a vertex
            // where another bevelled edge meets this one, the inset points are
            // pulled back off the original vertex down the edge, and an arc centre
            // left at `v` would not be equidistant from them any more - the ring
            // would bow out of the cylinder the strip's other end lies on and the
            // two ends would not be parallel. Taking the centre at the MEAN AXIAL
            // POSITION of the two endpoints keeps the arc a true cross-section of
            // the same rolling-ball surface at both ends, and it is exactly
            // `its.vertices[v]` in the one-bevelled-edge case, where the inset
            // points are the old rails and nothing was pulled back.
            Vec3f axis = its.vertices[v == a ? b : a] - its.vertices[v];
            const float axis_len = axis.norm();
            float       slide    = 0.f;
            if (axis_len > 1e-9f) {
                axis /= axis_len;
                slide = 0.5f * ((out.vertices[cA] - its.vertices[v]).dot(axis) +
                                (out.vertices[cB] - its.vertices[v]).dot(axis));
            }
            const Vec3f C  = its.vertices[v] + axis * slide + bis * centre_dist;
            const Vec3f dA = (out.vertices[cA] - C).normalized();
            const Vec3f dB = (out.vertices[cB] - C).normalized();
            const float rA = (out.vertices[cA] - C).norm();
            const float rB = (out.vertices[cB] - C).norm();
            const float ang = std::acos(std::clamp(dA.dot(dB), -1.f, 1.f));
            const float al  = dA.cross(dB).norm();
            for (int s = 1; s < rings; ++s) {
                const float u = float(s) / float(rings);
                Vec3f       d;
                if (al < 1e-9f || ang < 1e-6f) {
                    d = dA;
                } else {
                    // Spherical interpolation about the arc centre: exact for a
                    // circular cross-section, and it degenerates cleanly.
                    const float s0f = std::sin((1.f - u) * ang) / std::sin(ang);
                    const float s1f = std::sin(u * ang) / std::sin(ang);
                    d = (dA * s0f + dB * s1f).normalized();
                }
                ring.push_back(int(out.vertices.size()));
                out.vertices.emplace_back(C + d * (rA + (rB - rA) * u));
            }
            ring.push_back(cB);
            return ring;
        };

        const std::vector<int> ring_a = arc_points(a, a0, a1);
        const std::vector<int> ring_b = arc_points(b, b0, b1);
        for (size_t s = 0; s + 1 < ring_a.size(); ++s)
            emit_quad(ring_a[s], ring_a[s + 1], ring_b[s + 1], ring_b[s]);
        note_ends(ring_a, ring_b);

        // The round profile's end cap is a fan from the vertex over the whole
        // ring, for the same reason the chamfer's is a single triangle.
        auto cap_ring = [&](int v, const std::vector<int> &ring) {
            auto it = edges_at_vertex.find(v);
            if (it != edges_at_vertex.end() && it->second.size() > 1)
                return;
            for (size_t s = 0; s + 1 < ring.size(); ++s)
                cap_end(v, ring[s], ring[s + 1]);
        };
        cap_ring(a, ring_a);
        cap_ring(b, ring_b);
    }

    // --- the corner patches ----------------------------------------------------
    //
    // Where several bevelled edges meet at a vertex, the strips and the rewritten
    // sides leave a hole between them - on a cube with all 12 edges bevelled, the
    // eight classic three-sided corners.
    //
    // ONE LOOP PER CORNER, built from the strips' own end edges. Each bevelled edge
    // at `v` left an open end there (its inset-point pair for a chamfer, its whole
    // arc ring for a round), and those ends were recorded in `strip_end_ring` as
    // they were emitted. Because every strip now terminates on the INSET POINTS of
    // the two sides it runs between, two edges adjacent around `v` terminate on the
    // SAME point - the one inset point of the face they have in common - so the ends
    // chain terminal-to-terminal into exactly one closed ring, which is the patch
    // boundary. Walking that ring is the construction; there is nothing to search
    // for. On the chamfered cube that ring is the three inset points of the three
    // faces at the corner - (18,18,0), (20,18,2), (18,20,2) at (20,20,0) - and the
    // patch is the single triangle between them, which is the eighth of the
    // 6 octagons + 12 rectangles + 8 triangles a chamfered cube is made of.
    //
    // This is what the two-rail chord could not leave room for. With the old rails
    // the face's corner was a CHORD between (20,18,0) and (18,20,0), and the
    // vertical edge's strip ended on that very same pair, so the edge had its two
    // facets, nothing was open, and the corner triangle had nowhere to go
    // (measured: corner_patches=0, is_closed_manifold true, removed 5626 where 432
    // is right - closed by count while pinched).
    //
    // This replaces hunting for open boundary runs after the fact, and the reason is
    // measured rather than aesthetic. The old filler counted directed edges over the
    // whole assembled mesh and stitched whatever came back unmatched. With duplicate
    // rails that found the zero-area slivers BETWEEN each pair of twins - the right
    // number of patches (8) for a topology the following its_merge_vertices() then
    // destroyed, welding the twins and collapsing every patch onto its neighbour
    // (nonmanifold=24). Welding first instead does clear the duplicates, but then a
    // corner presents its several boundary runs to the filler as SEPARATE loops and
    // the cube came back with 20 of them and open=84. Neither ordering can work,
    // because the information the filler needs - which runs belong to which corner -
    // is not in the edge counts at all. It is in which strip left which end open,
    // and that is known at the moment the strip is built.
    //
    // A STAGE PROBE. The final dump below reports the mesh after the merge, which is
    // too late to tell apart the three things that can go wrong here; this one
    // reports the same counts just before the fill and just after it, so a failure
    // can be attributed to the strips, to the corner build, or to the merge.
    // `coincident_dups` is what the merge would have to weld: it was 24 on the
    // all-12 cube while the rails were keyed per (edge, side), and the whole point
    // of sharing them by position is that it is now 0 - if it ever comes back
    // non-zero, the dedupe is what regressed, not the fill.
#ifdef MESHEDIT_BEVEL_DIAG
    auto stage_probe = [&](const char *when) {
        std::map<std::pair<int, int>, int> d;
        for (const Vec3i32 &f : out.indices)
            for (int s = 0; s < 3; ++s)
                ++d[std::make_pair(f[s], f[(s + 1) % 3])];
        int unmatched = 0, bad = 0;
        for (const auto &e : d) {
            auto      it  = d.find(std::make_pair(e.first.second, e.first.first));
            const int rev = it == d.end() ? 0 : it->second;
            if (rev == 0) ++unmatched;
            if (!(e.second == 1 && rev == 1)) ++bad;
        }
        std::map<std::tuple<int, int, int>, int> q;
        for (const Vec3f &v : out.vertices)
            ++q[std::make_tuple(int(v.x() * 1024.f), int(v.y() * 1024.f), int(v.z() * 1024.f))];
        int dup = 0;
        for (const auto &e : q)
            if (e.second > 1) dup += e.second - 1;
        std::fprintf(stderr,
                     "BEVELDIAG(%s) tris=%zu verts=%zu unmatched_dir=%d badwind=%d "
                     "coincident_dups=%d capped=%zu patches=%zu\n",
                     when, out.indices.size(), out.vertices.size(), unmatched, bad, dup,
                     capped_vertices.size(), res.corner_patches);
    };
    stage_probe("pre-fill");
#endif

    // ONE CORNER AT A TIME, and every hole that corner owns.
    //
    // The reason it is a SET of holes rather than always one. A chamfered corner is a
    // single triangle between the three inset points, and with the inset model that
    // is now exactly what the strips leave open. A ROUNDED corner is not: each strip
    // leaves its whole arc ring exposed, so the corner's boundary is a ring of arcs
    // meeting at the sides' inset points, and how that ring decomposes depends on the
    // valence and on whether any side kept its original vertex. So the walk does not
    // try to be clever about which loop is "the" corner. It takes every open loop
    // whose points all belong to this corner - the inset and arc points its own
    // strips left exposed, plus the original vertex when a side kept it - fills each,
    // and counts the whole corner as ONE corner patch. That is what a corner patch
    // means to the caller and to the panel: the geometry that closes one corner,
    // however many triangles and loops it takes.
    //
    // Restricting to the corner's own points is what the old global filler could not
    // do, and it is the entire fix. It never had to decide which of several boundary
    // runs belonged together - a question the edge counts cannot answer - because the
    // strips record, as they are built, which points they left open at which vertex.
    std::set<int> patched_from;      // boundary points already consumed by a patch
    for (const auto &va : edges_at_vertex) {
        const int               v  = va.first;
        const std::vector<int> &es = va.second;
        if (es.size() < 2)
            continue;               // the end-cap case, already closed
        if (capped_vertices.count(v) > 0)
            continue;               // never patch over a cap

        // This corner's own points: everything its strips left exposed, and the
        // original vertex, which is still on the boundary whenever only SOME of the
        // sides at `v` dropped it. That is the case a rim chain on a chamfered box
        // presents at every corner - two bevelled edges, two top-plane rails that do
        // not coincide, and `v` itself bridging them across the two chamfer faces.
        std::set<int> own;
        for (int e : es) {
            auto it = strip_end_ring.find(std::make_pair(v, e));
            if (it == strip_end_ring.end())
                continue;
            for (int idx : it->second)
                own.insert(idx);
        }
        if (own.empty())
            continue;
        own.insert(v);

        // Which of this corner's open edges are the STRIP's own - a consecutive pair of
        // one arc ring, or a chamfer's inset-point pair. Anything else bounding the
        // opening is a cut a rewritten side made across its own corner.
        //
        // The distinction decides the ORDER the holes are filled in, and getting it
        // wrong yields a closed mesh with the wrong volume rather than an obvious
        // failure - which is why it is taken from recorded data rather than guessed
        // from geometry. Where a corner does decompose into several holes, filling the
        // inner one first consumes all of its bounding cuts at once, and the only cycle
        // left is then the outer ring, whose fan covers that inner region a SECOND time
        // - still closed, still manifold, but with a sliver of void sealed inside and
        // the corner's volume wrong. So a cycle carrying a strip edge is always
        // preferred, which fills from the outside in and leaves the inner region to be
        // closed by what surrounds it.
        std::set<std::pair<int, int>> strip_edge;
        for (int e : es) {
            auto it = strip_end_ring.find(std::make_pair(v, e));
            if (it == strip_end_ring.end())
                continue;
            const std::vector<int> &ring = it->second;
            for (size_t i = 0; i + 1 < ring.size(); ++i) {
                const int p = ring[i], q = ring[i + 1];
                strip_edge.insert(p < q ? std::make_pair(p, q) : std::make_pair(q, p));
            }
        }

        // Fill until nothing of this corner is open. Each pass re-reads the boundary
        // from the mesh, because the previous pass changed it. The bound is the number
        // of holes a corner can have, which is one per incident side plus one.
        bool   any = false;
        size_t guard = own.size() + 4;
        while (guard-- > 0) {
            // The open boundary restricted to this corner: an UNDIRECTED edge carried by
            // exactly one facet, both of whose endpoints are this corner's own.
            //
            // Undirected, and that is the correction that mattered. The directed test -
            // a direction with no reverse partner - reads an edge that already has its
            // two facets as open whenever those two disagree about which way is out, and
            // the three producers here did disagree on exactly the corner edges. So the
            // directed test invented a hole where the surface was already complete, a
            // patch was laid over it, and the patch did not fix the disagreement either,
            // so the next pass found the same "hole" again: two identical triangles and
            // four facets on the edge. The probe named it outright - (8,9)=2/0,
            // (8,24)=0/2, (9,24)=2/0, two facets each and not one of them opposing.
            // Winding is now settled globally by orient_consistently() after the build,
            // so here the only question is whether a facet is MISSING, which is what
            // counting facets per undirected edge answers.
            std::map<std::pair<int, int>, int> d;
            for (const Vec3i32 &f : out.indices)
                for (int s = 0; s < 3; ++s) {
                    const int u = f[s], w2 = f[(s + 1) % 3];
                    ++d[u < w2 ? std::make_pair(u, w2) : std::make_pair(w2, u)];
                }
            std::map<int, std::vector<int>> adj;
            std::set<std::pair<int, int>>   border;   // the open edges, sorted, as seeds
            for (const auto &kv : d) {
                if (kv.second != 1)
                    continue;       // already has both its facets
                const int p = kv.first.first, q = kv.first.second;
                if (own.count(p) == 0 || own.count(q) == 0)
                    continue;       // not this corner's edge
                adj[p].push_back(q);
                adj[q].push_back(p);
                border.insert(std::make_pair(p, q));
            }
            if (adj.empty())
                break;              // this corner is closed
#ifdef MESHEDIT_BEVEL_DIAG
            // How many facets each of this corner's open edges carries. One means a
            // genuine hole; the trail that led here was this line reading two.
            std::fprintf(stderr, "  PASS v=%d tris=%zu open_edges=%zu\n", v,
                         out.indices.size(), border.size());
#endif

            // ONE HOLE PER PASS, as the MINIMAL cycle through a chosen open edge.
            //
            // Not a free walk. At a rail of a rounded corner four open edges meet, so
            // "step to any neighbour that is not where I came from" has a real choice to
            // make at every rail and no local information to make it with - it cuts out
            // of one hole into the next and comes back with a cycle that is not the
            // boundary of anything (measured: 9 points of a 12-point ring). What IS
            // well defined is the smallest cycle through a GIVEN edge, and that is
            // exactly one hole's boundary: take the edge out of the graph and find the
            // shortest path between its two endpoints. Breadth-first, so shortest, and
            // ties broken by the lower index so the result does not depend on the order
            // the map happened to be built in.
            //
            // The seed edge is a strip edge whenever the corner still has one open, for
            // the ordering reason above.
            std::pair<int, int> seed(-1, -1);
            for (const auto &b : border) {
                if (seed.first < 0)
                    seed = b;                           // fallback: the lowest chord
                if (strip_edge.count(b) > 0) { seed = b; break; }
            }
            if (seed.first < 0)
                break;

            std::vector<int> loop;
            {
                const int              src = seed.second, dst = seed.first;
                std::map<int, int>     came;            // point -> where it was reached from
                std::deque<int>        q2;
                q2.push_back(src);
                came[src] = src;
                bool found = false;
                while (!q2.empty() && !found) {
                    const int cur = q2.front();
                    q2.pop_front();
                    auto it = adj.find(cur);
                    if (it == adj.end())
                        continue;
                    std::vector<int> nbrs = it->second;
                    std::sort(nbrs.begin(), nbrs.end());
                    for (int nb : nbrs) {
                        // The seed edge itself is removed from the graph, so the path
                        // found is the OTHER way round and the cycle is a real one.
                        if ((cur == src && nb == dst) || (cur == dst && nb == src))
                            continue;
                        if (came.count(nb) > 0)
                            continue;
                        came[nb] = cur;
                        if (nb == dst) { found = true; break; }
                        q2.push_back(nb);
                    }
                }
                if (found) {
                    for (int cur = dst; ; cur = came.at(cur)) {
                        loop.push_back(cur);
                        if (cur == src)
                            break;
                    }
                    // loop now runs dst -> ... -> src; closing it uses the seed edge.
                }
            }
            if (loop.size() < 3)
                break;              // nothing walkable: leave the rest to the filler
#ifdef MESHEDIT_BEVEL_DIAG
            std::fprintf(stderr, "BEVELDIAG(corner) v=%d edges=%zu own=%zu loop=%zu\n",
                         v, es.size(), own.size(), loop.size());
#endif

            // No winding decision here: orient_consistently() settles the whole surface
            // after the build, so a patch only has to be the right SHAPE. That is the
            // point of doing it globally - three producers that each pick a winding from
            // their own normal reference cannot be made to agree locally at a corner,
            // where every such reference is near-degenerate.
            for (int idx : loop)
                patched_from.insert(idx);
            any = true;

            // A three-sided hole IS a triangle - the chamfered cube corner, and the
            // lens between a chord and a one-segment arc - so it is emitted as one
            // rather than as a centroid plus three slivers, which would put a vertex
            // inside the solid and move the volume the closed form is compared against.
            if (loop.size() == 3) {
                out.indices.emplace_back(loop[0], loop[1], loop[2]);
                continue;
            }
            // Four or more: fan from the centroid, which stays valid for the
            // saddle-shaped polygon a lens or a rim corner is, where a fan from one of
            // its own vertices would fold.
            Vec3f centroid = Vec3f::Zero();
            for (int idx : loop)
                centroid += out.vertices[idx];
            centroid /= float(loop.size());
            const int cv = int(out.vertices.size());
            out.vertices.emplace_back(centroid);
            // Same rule, applied to every wedge of the fan: the loop is already in the
            // direction the patch must run, so (cv, p, q) traverses p -> q and the
            // border's q -> p opposes it.
            for (size_t i = 0; i < loop.size(); ++i) {
                const int p = loop[i], q = loop[(i + 1) % loop.size()];
                if (p == q)
                    continue;
                out.indices.emplace_back(cv, p, q);
            }
        }
        if (any)
            ++res.corner_patches;   // one corner, one patch, whatever it took to close
    }

    // Anything the corner walk could not close as a clean cycle - a vertex whose
    // strips do not chain, which the constructions above are not expected to produce
    // but which a pathological input could - still goes to the generic filler, so an
    // unclosed hole is a filled hole rather than a refusal. A corner the walk DID
    // patch needs no protecting from it: the patch leaves no open edge there, so the
    // filler's directed-edge count never reports that region at all. (The capped-
    // vertex skip is still needed, and still only for the end caps, whose hole is
    // genuinely larger than the cap that closes it.)
#ifdef MESHEDIT_BEVEL_DIAG
    stage_probe("post-corners");
#endif
    // The generic filler, now only a fallback: it runs on whatever is STILL open after
    // the corner walk, which for the cases the walk handles is nothing at all. It is
    // deliberately not given the patched points as a skip set - a patch closes the
    // edges it covers, so the filler's own directed-edge count no longer reports them,
    // and suppressing by point would instead stop it closing a leftover run that
    // happens to touch a patched rail. (The skip set is still needed for the end caps,
    // whose hole is genuinely wider than the cap that closes it.) What used to make
    // this double-cover was a wrongly wound patch, and that is now decided from the
    // bordering facets' normals rather than from a strip's own choice of winding.
    fill_open_loops(out, res.corner_patches, capped_vertices);
#ifdef MESHEDIT_BEVEL_DIAG
    stage_probe("post-fill,pre-merge");
#endif

    // --- clean up ------------------------------------------------------------
    its_merge_vertices(out);
    its_remove_degenerate_faces(out);
    its_compactify_vertices(out);

    // Make the three producers agree about which way is out. Each is internally right
    // and they are mutually inconsistent - see orient_consistently() for the measurement
    // - and this is where that is settled: after the topology is final, before anything
    // asks whether the result is watertight. It has to run AFTER the merge, because the
    // flood walks facet adjacency and two coincident but unwelded vertices are not
    // adjacent.
    orient_consistently(out);

    if (out.indices.empty() || !is_closed_manifold(out)) {
        // Define MESHEDIT_BEVEL_DIAG (at the top of this file, or on the compiler
        // command line) to have a failure print WHY: how many edges came out open,
        // how many non-manifold, and where the offending ones are. Kept rather than
        // deleted because it is what finally located the tear that the construction
        // still has - see the spec's "why it is not finished" - and whoever
        // finishes the re-cut will want it on the first run, not after
        // reinventing it.
#ifdef MESHEDIT_BEVEL_DIAG
        {
            std::map<std::pair<int, int>, int> cnt;
            for (const Vec3i32 &f : out.indices)
                for (int s = 0; s < 3; ++s) {
                    const int u = f[s], v2 = f[(s + 1) % 3];
                    ++cnt[u < v2 ? std::make_pair(u, v2) : std::make_pair(v2, u)];
                }
            int open = 0, over = 0;
            for (const auto &c : cnt) {
                if (c.second == 1) ++open;
                else if (c.second > 2) ++over;
            }
            std::fprintf(stderr, "BEVELDIAG(manifold) tris=%zu verts=%zu open=%d nonmanifold=%d corners=%zu\n",
                         out.indices.size(), out.vertices.size(), open, over, res.corner_patches);
            int shown = 0;
            for (const auto &c : cnt) {
                if (c.second == 2 || shown++ > 20) continue;
                std::fprintf(stderr, "  edge (%d,%d) n=%d a=(%.2f,%.2f,%.2f) b=(%.2f,%.2f,%.2f)\n",
                             c.first.first, c.first.second, c.second,
                             out.vertices[c.first.first].x(), out.vertices[c.first.first].y(),
                             out.vertices[c.first.first].z(), out.vertices[c.first.second].x(),
                             out.vertices[c.first.second].y(), out.vertices[c.first.second].z());
            }
            // Winding: a consistently wound closed surface carries each directed
            // edge exactly once. Report the pairs that do not, which is what tells
            // a winding fault apart from a genuine hole.
            std::map<std::pair<int, int>, int> dir;
            for (const Vec3i32 &f : out.indices)
                for (int s = 0; s < 3; ++s)
                    ++dir[std::make_pair(f[s], f[(s + 1) % 3])];
            int badwind = 0, shown2 = 0;
            for (const auto &d : dir) {
                auto      it  = dir.find(std::make_pair(d.first.second, d.first.first));
                const int rev = it == dir.end() ? 0 : it->second;
                if (d.second == 1 && rev == 1)
                    continue;
                ++badwind;
                if (shown2++ > 20) continue;
                std::fprintf(stderr, "  WIND (%d->%d) fwd=%d rev=%d\n",
                             d.first.first, d.first.second, d.second, rev);
            }
            std::fprintf(stderr, "BEVELDIAG badwind=%d\n", badwind);
        }
#endif
        res.status = BevelStatus::Failed;
        res.mesh.clear();
        return res;
    }
    if (params.check_self_intersection && self_intersects(out)) {
#ifdef MESHEDIT_BEVEL_DIAG
        std::fprintf(stderr, "BEVELDIAG(selfint) tris=%zu verts=%zu corners=%zu\n",
                     out.indices.size(), out.vertices.size(), res.corner_patches);
#endif
        res.status = BevelStatus::Failed;
        res.mesh.clear();
        return res;
    }

    res.mesh   = std::move(out);
    res.status = BevelStatus::Ok;
    return res;
}

BevelResult bevel_chain(const indexed_triangle_set &its,
                        const MeshTopology         &topo,
                        const EdgeChain            &chain,
                        const BevelParams          &params)
{
    return bevel_edges(its, topo, chain.edges, params);
}

// ----------------------------------------------------------------------------
// Session
// ----------------------------------------------------------------------------

constexpr size_t EditSession::UndoLimit;

EditSession::EditSession(const indexed_triangle_set &its) : m_its(its)
{
    rebuild_topology();
}

void EditSession::rebuild_topology()
{
    m_topo = build_topology(m_its);
}

FaceRegion EditSession::grow_region(size_t seed_facet, const RegionParams &params) const
{
    return grow_face_region(m_its, m_topo, seed_facet, params);
}

EdgeChain EditSession::grow_chain(int seed_edge, float threshold_deg, float continuation_deg) const
{
    const std::vector<uint8_t> is_feature = feature_edge_mask(m_topo, threshold_deg);
    return grow_edge_chain(m_its, m_topo, is_feature, seed_edge, continuation_deg);
}

int EditSession::nearest_edge(size_t facet, const Vec3f &point, float *out_distance) const
{
    return nearest_edge_of_facet(m_its, m_topo, facet, point, out_distance);
}

TranslateResult EditSession::apply_translate(const FaceRegion &region, const TranslateParams &params)
{
    TranslateResult res = translate_region(m_its, m_topo, region, params);
    if (res.status == TranslateStatus::Ok) {
        push_undo();
        m_its = res.mesh;
        // The indices are untouched, so face_neighbors / face_edge_ids are still
        // correct; only the normals moved. Recomputing the lot is simpler than
        // tracking which, and it is one linear pass per COMPLETED operation, not
        // per drag tick.
        for (size_t f = 0; f < m_its.indices.size(); ++f)
            m_topo.face_normals[f] = facet_unit_normal(m_its, f);
    }
    return res;
}

BevelResult EditSession::apply_bevel(const std::vector<int> &edges, const BevelParams &params)
{
    BevelResult res = bevel_edges(m_its, m_topo, edges, params);
    if (res.status == BevelStatus::Ok) {
        push_undo();
        m_its = res.mesh;
        // Unlike a translate, EVERY index changed - the whole cache is stale, so
        // it is rebuilt rather than patched.
        rebuild_topology();
    }
    return res;
}

BevelResult EditSession::apply_bevel(const EdgeChain &chain, const BevelParams &params)
{
    return apply_bevel(chain.edges, params);
}

BevelResult EditSession::preview_bevel(const std::vector<int> &edges, const BevelParams &params) const
{
    return bevel_edges(m_its, m_topo, edges, params);
}

void EditSession::set_mesh(indexed_triangle_set &&its)
{
    const bool same_topology = its.indices.size() == m_its.indices.size() &&
                               its.vertices.size() == m_its.vertices.size();
    m_its = std::move(its);
    if (same_topology) {
        for (size_t f = 0; f < m_its.indices.size(); ++f)
            m_topo.face_normals[f] = facet_unit_normal(m_its, f);
    } else {
        rebuild_topology();
    }
}

void EditSession::push_undo()
{
    m_undo.push_back(m_its);
    if (m_undo.size() > UndoLimit)
        m_undo.erase(m_undo.begin());
    // A new operation ends the redo branch, the way every undo stack does.
    m_redo.clear();
}

bool EditSession::undo()
{
    if (m_undo.empty())
        return false;
    m_redo.push_back(std::move(m_its));
    m_its = std::move(m_undo.back());
    m_undo.pop_back();
    rebuild_topology();
    return true;
}

bool EditSession::redo()
{
    if (m_redo.empty())
        return false;
    m_undo.push_back(std::move(m_its));
    m_its = std::move(m_redo.back());
    m_redo.pop_back();
    rebuild_topology();
    return true;
}

void EditSession::clear_history()
{
    m_undo.clear();
    m_redo.clear();
}

} // namespace MeshEdit
} // namespace Slic3r
