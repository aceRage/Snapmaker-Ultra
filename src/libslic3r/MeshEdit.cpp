#include "MeshEdit.hpp"

#include "MeshBoolean.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_map>

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
