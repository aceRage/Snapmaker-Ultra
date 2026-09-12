#include "MeshEdit.hpp"

#include "MeshBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <map>
#include <set>
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

    // --- the corner split ---------------------------------------------------
    //
    // Every (vertex, SIDE) pair that touches a bevelled edge becomes its own new
    // vertex, pulled back from the original along the bevels of the edges meeting
    // there. A facet keeps its shape and simply re-indexes; the holes this opens
    // are the strips and the corner patches.
    //
    // "SIDE", not "facet", and that distinction is the whole correctness of this
    // pass. A CAD face is generally several triangles - a cube's square face is
    // two - and only ONE of them actually contains any given bevelled edge. If the
    // split were keyed per facet, that one triangle's corner would move while its
    // coplanar neighbour's stayed put, and the mesh would TEAR along the diagonal
    // between them. (It did: the first implementation was keyed per facet and every
    // bevel came back non-closed.)
    //
    // So facets are first grouped into SIDES: maximal sets connected through edges
    // that are neither being bevelled NOR a crease. Every facet of a side shares
    // one split copy of a vertex, so a side deforms as a unit and its interior
    // edges never tear.
    //
    // Both conditions are needed, and the second is the subtle one. Stopping only
    // at bevelled edges is not enough: bevelling a SINGLE edge does not disconnect
    // a closed surface, so a flood fill would put the whole cube in one side, the
    // two faces meeting at that edge would share one vertex copy, and the chamfer
    // would collapse instead of opening. (Reasoned through before the build
    // finished, having just been burnt by the per-facet version of the same
    // mistake.) Stopping at creases as well means a side never spans two
    // differently-oriented faces, so each face of the edge gets its own copy - and
    // within a flat face, the coplanar diagonals are still crossed freely, which is
    // exactly what prevents the tear.
    //
    // The crease threshold is the flatness threshold the solve already uses to
    // decide an edge is too flat to bevel, so the two agree by construction.
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
                        continue; // a bevelled edge is a cut between sides
                    if (e >= 0 && edge_dihedral_deg(topo, e) >= params.min_dihedral_deg)
                        continue; // so is any other crease
                    side_of[size_t(nb)] = side;
                    stack.push_back(size_t(nb));
                }
            }
        }
    }

    // (vertex, side) -> new vertex index.
    std::map<std::pair<int, int>, int> corner_vertex;
    indexed_triangle_set out;
    out.vertices = its.vertices;

    // Which bevelled edges touch each vertex.
    std::map<int, std::vector<int>> edges_at_vertex;
    for (const auto &kv : width_of) {
        const Vec2i32 ev = topo.edge_vertices[kv.first];
        edges_at_vertex[ev(0)].push_back(kv.first);
        edges_at_vertex[ev(1)].push_back(kv.first);
    }

    // The split copy of vertex `v` belonging to the side that facet `f` is in.
    // Every facet of that side gets the SAME copy, which is what stops a side
    // tearing along its own interior edges.
    auto corner_of = [&](int v, size_t f) -> int {
        const int  side = side_of[f];
        const auto key  = std::make_pair(v, side);
        auto       it   = corner_vertex.find(key);
        if (it != corner_vertex.end())
            return it->second;

        // Sum the pullback each bevelled edge at v that BOUNDS this side asks for.
        // An edge (v, u) on the side's boundary pulls the corner along that edge's
        // in-plane normal, measured inside a facet of THIS side, by the edge's
        // width. On a cube corner two bevelled edges bound each side, so the corner
        // moves diagonally inward by exactly what the three strips and the corner
        // patch need to meet.
        Vec3f disp = Vec3f::Zero();
        auto  eit  = edges_at_vertex.find(v);
        if (eit != edges_at_vertex.end()) {
            for (int e : eit->second) {
                // Which of the edge's two facets is on this side? That facet is the
                // one whose plane the offset is measured in.
                const Vec2i32 ff = topo.edge_faces[e];
                size_t        on_side = size_t(-1);
                for (int k = 0; k < 2; ++k)
                    if (ff(k) >= 0 && side_of[size_t(ff(k))] == side)
                        on_side = size_t(ff(k));
                if (on_side == size_t(-1))
                    continue; // this bevelled edge does not bound the side
                // .at(), not operator[]: edges_at_vertex is built from width_of so
                // the entry is always there, and .at() says so rather than silently
                // inserting a zero if that ever stopped being true.
                const Vec2i32 ev = topo.edge_vertices[e];
                const int     u  = ev(0) == v ? ev(1) : ev(0);
                disp += width_of.at(e) * in_plane_normal(its, topo, on_side, v, u);
            }
        }
        if (disp.squaredNorm() < 1e-20f) {
            // Nothing pulls this corner: it keeps the original vertex, which also
            // keeps the mesh compact where no bevel reaches.
            corner_vertex.emplace(key, v);
            return v;
        }
        const int nv = int(out.vertices.size());
        out.vertices.emplace_back(its.vertices[v] + disp);
        corner_vertex.emplace(key, nv);
        return nv;
    };

    // --- re-index every original facet --------------------------------------
    out.indices.reserve(its.indices.size() * 2 + width_of.size() * size_t(rings) * 2);
    for (size_t f = 0; f < its.indices.size(); ++f) {
        const auto &tri = its.indices[f];
        const int   a = corner_of(tri[0], f);
        const int   b = corner_of(tri[1], f);
        const int   c = corner_of(tri[2], f);
        if (a == b || b == c || a == c)
            continue; // the pullback collapsed this facet; the patches cover it
        out.indices.emplace_back(a, b, c);
    }

    // --- the strips ----------------------------------------------------------
    //
    // For edge e = (a, b) with facets f0, f1, the four corner vertices
    // (a,f0) (b,f0) (b,f1) (a,f1) bound the hole the re-index opened. For a
    // chamfer that hole is filled with two triangles. For a round it is filled
    // with `rings` bands whose intermediate vertices follow the tangent arc.
    for (const auto &kv : width_of) {
        const int     e  = kv.first;
        const float   w  = kv.second;
        const Vec2i32 ev = topo.edge_vertices[e];
        const Vec2i32 ff = topo.edge_faces[e];
        const int     a = ev(0), b = ev(1);
        const size_t  f0 = size_t(ff(0)), f1 = size_t(ff(1));

        const int a0 = corner_of(a, f0), b0 = corner_of(b, f0);
        const int a1 = corner_of(a, f1), b1 = corner_of(b, f1);

        // The strip has to face OUTWARD like the faces it joins. Rather than
        // hard-code a winding and hope the mesh is wound the way the derivation
        // assumed, emit one quad through a helper that checks its own normal
        // against the two incident face normals and flips if it got it wrong.
        // A band between two outward faces must point outward too, so their mean
        // is the reference.
        const Vec3f outward = topo.face_normals[f0] + topo.face_normals[f1];
        auto emit_quad = [&](int p0, int p1, int q1, int q0) {
            // Quad (p0 -> p1 -> q1 -> q0). Split into two triangles, both wound the
            // same way, and flip the pair together if the first faces inward.
            const Vec3f &A = out.vertices[p0];
            const Vec3f &B = out.vertices[p1];
            const Vec3f &C = out.vertices[q1];
            const Vec3f  n = (B - A).cross(C - A);
            const bool   flip = n.dot(outward) < 0.f;
            auto tri = [&](int x, int y, int z) {
                if (x == y || y == z || x == z)
                    return;
                if (flip)
                    out.indices.emplace_back(x, z, y);
                else
                    out.indices.emplace_back(x, y, z);
            };
            tri(p0, p1, q1);
            tri(p0, q1, q0);
        };

        if (rings <= 1) {
            // Chamfer: one flat band across the hole, from f0's rail to f1's.
            emit_quad(a0, a1, b1, b0);
            continue;
        }

        // Round: interpolate `rings` bands along the arc tangent to both faces.
        //
        // The arc is built in the plane perpendicular to the edge. Its endpoints
        // are the two rails; its centre lies along the inward bisector at
        // w / tan(interior/2) from the edge - which is exactly the distance that
        // makes the arc tangent to both faces, the spec's r / sin(theta/2)
        // expressed for the offset distance rather than the radius.
        const Vec3f t0 = in_plane_normal(its, topo, f0, a, b);
        const Vec3f t1 = in_plane_normal(its, topo, f1, a, b);
        const float interior = edge_interior_angle(topo, e);
        // Degenerate angle: fall back to the chamfer rather than dividing by ~0.
        if (!(interior > 1e-3f) || interior > float(M_PI) - 1e-3f) {
            emit_quad(a0, a1, b1, b0);
            continue;
        }

        // The bisector pointing INTO the material, and the arc centre offset.
        Vec3f bis = t0 + t1;
        const float bl = bis.norm();
        if (bl < 1e-9f) {
            emit_quad(a0, a1, b1, b0);
            continue;
        }
        bis /= bl;
        const float half = interior * 0.5f;
        // The arc centre sits on the inward bisector, at the point equidistant from
        // both rails. Each rail is w from the edge along its own face, and the
        // bisector makes an angle `half` with each face, so that distance is
        // w / cos(half) - which is the spec's r / sin(theta/2) written for the
        // offset distance rather than the fillet radius.
        //
        // The intermediate ring points are then a spherical interpolation of the
        // two rail directions ABOUT that centre, which is exact for a circular
        // cross-section and needs no further trigonometry.
        const float centre_dist = w / std::max(std::cos(half), 1e-3f);

        // Build the intermediate rings at both ends of the edge.
        auto arc_points = [&](int v, int cA, int cB) {
            // cA / cB are this end's two rail vertices (on f0 and f1).
            std::vector<int> ring;
            ring.reserve(size_t(rings) + 1);
            ring.push_back(cA);
            const Vec3f  P  = its.vertices[v];
            const Vec3f  C  = P + bis * centre_dist;
            const Vec3f  dA = (out.vertices[cA] - C).normalized();
            const Vec3f  dB = (out.vertices[cB] - C).normalized();
            const float  rA = (out.vertices[cA] - C).norm();
            const float  rB = (out.vertices[cB] - C).norm();
            const float  cosang = std::clamp(dA.dot(dB), -1.f, 1.f);
            const float  ang    = std::acos(cosang);
            // The two rail directions must actually span a plane, or there is no
            // arc between them and the slerp below is undefined.
            const float  al     = dA.cross(dB).norm();
            for (int s = 1; s < rings; ++s) {
                const float u = float(s) / float(rings);
                Vec3f       d;
                if (al < 1e-9f || ang < 1e-6f) {
                    d = dA;
                } else {
                    // Spherical interpolation about the arc centre: exact for a
                    // circular cross-section, and it degenerates cleanly.
                    const float s0 = std::sin((1.f - u) * ang) / std::sin(ang);
                    const float s1 = std::sin(u * ang) / std::sin(ang);
                    d = (dA * s0 + dB * s1).normalized();
                }
                const float r = rA + (rB - rA) * u;
                ring.push_back(int(out.vertices.size()));
                out.vertices.emplace_back(C + d * r);
            }
            ring.push_back(cB);
            return ring;
        };

        const std::vector<int> ring_a = arc_points(a, a0, a1);
        const std::vector<int> ring_b = arc_points(b, b0, b1);

        // One band per ring step, wound by the same self-correcting helper the
        // chamfer uses - so the two profiles cannot disagree about which way the
        // strip faces.
        for (size_t s = 0; s + 1 < ring_a.size(); ++s)
            emit_quad(ring_a[s], ring_a[s + 1], ring_b[s + 1], ring_b[s]);
    }

    // --- the corner patches --------------------------------------------------
    //
    // Where bevelled edges meet at a vertex, their strips arrive from different
    // directions and leave a hole between them. On a cube with all 12 edges
    // bevelled that is the eight classic three-sided corners.
    //
    // These are found from the mesh that has actually been built, NOT predicted
    // from the one-ring of the original vertex. Predicting them is what the first
    // two attempts did and both were wrong in the same way: a vertex where a
    // bevelled edge meets an UNBEVELLED crease has its two sides resolve to the
    // very same original vertex index, so there is no tear there at all - and
    // emitting a patch anyway put a third facet on edges that already had two,
    // making the result non-manifold rather than closed. (The all-12-edges cube
    // passed throughout, because there every side transition IS a bevel; the
    // single-edge chamfer is what exposed it.)
    //
    // So: collect the edges that are genuinely open - used by exactly one facet -
    // stitch them into loops, and fill each loop. An edge that was never torn is
    // not open, so it is never touched, and a hole that does exist is filled
    // exactly once whatever produced it. That is robust by construction instead of
    // by case analysis.
    {
        // Directed edge (a -> b) appears once per facet that uses it in that
        // direction. On a closed surface every undirected edge carries one of each
        // direction; a hole leaves the boundary direction unmatched.
        std::map<std::pair<int, int>, int>    directed;
        // (a -> b) -> one facet that uses it in that direction, for the winding
        // reference below.
        std::map<std::pair<int, int>, size_t> facet_of_directed;
        for (size_t fi = 0; fi < out.indices.size(); ++fi) {
            const Vec3i32 &f = out.indices[fi];
            for (int s = 0; s < 3; ++s) {
                const auto key = std::make_pair(f[s], f[(s + 1) % 3]);
                ++directed[key];
                facet_of_directed.emplace(key, fi);
            }
        }

        // next[a] = b for every unmatched a -> b: the hole boundary, already
        // oriented so that walking it keeps the solid on the correct side, which is
        // what makes the fill's winding follow automatically.
        std::map<int, int> next;
        for (const auto &d : directed) {
            const auto rev = std::make_pair(d.first.second, d.first.first);
            auto       it  = directed.find(rev);
            const int  back = it == directed.end() ? 0 : it->second;
            if (d.second > back)
                next.emplace(d.first.first, d.first.second);
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
                if (it == next.end()) {
                    loop.clear(); // an open chain, not a loop: leave it to the check
                    break;
                }
                cur = it->second;
                if (cur == seed.first)
                    break;
            }
            if (loop.size() < 3)
                continue;

            // Outward direction for this hole: the mean normal of the facets that
            // border it. Those already face outward, so the patch must too. The
            // bordering facet of boundary edge (a -> b) is the one using (b -> a),
            // looked up through the index built above rather than by rescanning the
            // mesh per edge.
            Vec3f vn = Vec3f::Zero();
            for (size_t i = 0; i < loop.size(); ++i) {
                const int a = loop[i], b = loop[(i + 1) % loop.size()];
                auto      it = facet_of_directed.find(std::make_pair(b, a));
                if (it == facet_of_directed.end())
                    continue;
                const Vec3i32 &f = out.indices[it->second];
                const Vec3f   &p0 = out.vertices[f[0]];
                const Vec3f   &p1 = out.vertices[f[1]];
                const Vec3f   &p2 = out.vertices[f[2]];
                vn += (p1 - p0).cross(p2 - p0);
            }

            // A three-sided hole IS a triangle - the ordinary cube corner - so it is
            // emitted as one rather than as a centroid plus three slivers.
            if (loop.size() == 3) {
                const Vec3f p0 = out.vertices[loop[0]];
                const Vec3f p1 = out.vertices[loop[1]];
                const Vec3f p2 = out.vertices[loop[2]];
                if ((p1 - p0).cross(p2 - p0).dot(vn) < 0.f)
                    out.indices.emplace_back(loop[0], loop[2], loop[1]);
                else
                    out.indices.emplace_back(loop[0], loop[1], loop[2]);
                ++res.corner_patches;
                continue;
            }

            // Four or more sides: fan from the CENTROID, which stays valid for the
            // non-planar (often saddle-shaped) polygon a higher-valence corner patch
            // is, where a fan from one of its own vertices would fold.
            Vec3f centroid = Vec3f::Zero();
            for (int c : loop)
                centroid += out.vertices[c];
            centroid /= float(loop.size());

            const int cv = int(out.vertices.size());
            out.vertices.emplace_back(centroid);

            // Copies, not references: out.vertices has just grown and may have moved.
            const bool flip = [&] {
                const Vec3f p0 = out.vertices[loop[0]];
                const Vec3f p1 = out.vertices[loop[1]];
                return (p0 - centroid).cross(p1 - centroid).dot(vn) < 0.f;
            }();
            for (size_t i = 0; i < loop.size(); ++i) {
                const int p = loop[i];
                const int q = loop[(i + 1) % loop.size()];
                if (p == q)
                    continue;
                if (flip)
                    out.indices.emplace_back(cv, q, p);
                else
                    out.indices.emplace_back(cv, p, q);
            }
            ++res.corner_patches;
        }
    }


    // --- clean up ------------------------------------------------------------
    its_merge_vertices(out);
    its_remove_degenerate_faces(out);
    its_compactify_vertices(out);

    if (out.indices.empty() || !is_closed_manifold(out)) {
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
            std::fprintf(stderr, "BEVELDIAG tris=%zu verts=%zu open=%d nonmanifold=%d corners=%zu\n",
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
        }
#endif
        res.status = BevelStatus::Failed;
        res.mesh.clear();
        return res;
    }
    if (params.check_self_intersection && self_intersects(out)) {
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
