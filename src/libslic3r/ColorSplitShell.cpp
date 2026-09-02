// Split by painted colour, stage 2: depth groups and the closed inward-offset shell of each of them.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md, 3.1a and 3.5 - 3.7.
#include "ColorSplit.hpp"
#include "ColorSplitInternal.hpp"
#include "TriangleMesh.hpp"
#include "AABBMesh.hpp"
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"
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

Vec3f unit_face_normal(const ColorPatches &p, int f)
{
    const Vec3i32 &t = p.surface.indices[f];
    const Vec3f    n = (p.surface.vertices[t[1]] - p.surface.vertices[t[0]])
                           .cross(p.surface.vertices[t[2]] - p.surface.vertices[t[0]]);
    return n.squaredNorm() > 0.f ? Vec3f(n.normalized()) : Vec3f(Vec3f::Zero());
}

// Spec 3.1a (Ruling 18): two facets of a patch may only be joined across an edge the surface crosses
// smoothly. 30 degrees keeps a faceted cylinder or sphere whole (a 36-sided cylinder bends 10 degrees per
// edge) while cutting a boss's side away from its top cap, a cube's top away from its side, and the floor of
// a groove away from its riser.
constexpr float SMOOTH_COS_30 = 0.8660254f;

// Components of the facet subset `in_set` (indices into patches.surface), joined only across the edges that
// `can_cross` accepts.
template<class CanCross>
std::vector<std::vector<int>> connected_components(const ColorPatches &p, const std::vector<Vec3i32> &nbrs,
                                                   const std::vector<char> &in_set, CanCross can_cross)
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
                if (n >= 0 && in_set[n] && !seen[n] && can_cross(f, n)) { seen[n] = 1; stack.push_back(n); }
            }
        }
        comps.push_back(std::move(comp));
    }
    return comps;
}

// Does the group still cover anything when its outline is pulled `delta` in from every side? The outline is
// the group's triangles projected onto the plane perpendicular to `axis` and unioned; a group narrower than
// 2*delta anywhere in that plane loses those parts, and one narrower everywhere comes back empty.
// Two rules ask this question: spec 3.5's core gate (axis = +Z, delta = 1.5 ws - the 2D rule's own XY
// projection, flat_cap_component_ex, MultiMaterialSegmentation.cpp:1493-1578) and Ruling 22's case A gate
// (axis = the group's mean normal, delta = ws).
bool projection_survives(const ColorPatches &p, const std::vector<int> &comp, const Vec3f &axis, double delta)
{
    const Vec3f u = axis.unitOrthogonal(), v = axis.cross(u);
    Polygons    tris;
    tris.reserve(comp.size());
    for (int f : comp) {
        const Vec3i32 &t = p.surface.indices[f];
        Polygon poly;
        for (int k = 0; k < 3; ++k) {
            const Vec3f &q = p.surface.vertices[t[k]];
            poly.points.emplace_back(scaled<coord_t>(q.dot(u)), scaled<coord_t>(q.dot(v)));
        }
        if (poly.area() < 0)   // a facet facing the other way projects clockwise; union_ex wants one winding
            poly.reverse();
        tris.push_back(std::move(poly));
    }
    return ! offset_ex(union_ex(tris), - scaled<float>(delta)).empty();
}

// Spec 3.5's core gate: a flat component may only be capped when its XY projection survives an inward offset
// of 1.5 wall stacks, i.e. when it is at least three wall stacks wide somewhere. A narrower strip has no core
// to cap - capping it would lift the whole strip to the shell depth and leave the body colour under a feature
// that is nothing but walls.
bool flat_core_survives(const ColorPatches &p, const std::vector<int> &comp, double ws)
{
    return projection_survives(p, comp, Vec3f::UnitZ(), 1.5 * ws);
}

// The group's area-weighted mean normal - the axis its outline is measured across.
Vec3f group_mean_normal(const ColorPatches &p, const std::vector<int> &group)
{
    Vec3f n = Vec3f::Zero();
    for (int f : group) {
        const Vec3i32 &t = p.surface.indices[f];
        n += (p.surface.vertices[t[1]] - p.surface.vertices[t[0]]).cross(p.surface.vertices[t[2]] - p.surface.vertices[t[0]]);
    }
    return n.squaredNorm() > 0.f ? Vec3f(n.normalized()) : Vec3f(Vec3f::UnitZ());
}

// Spec 3.5: split ONE smooth patch (spec 3.1a) into (facet group, cap depth) pairs - cap depth 0 means the
// group is built at d(v). The flat components inside the patch that pass the core gate above become capped
// groups; whatever is left of the patch forms the uncapped ones. Running this inside a patch rather than
// across the whole state is what keeps the two decompositions independent: a patch is never re-joined here,
// only cut further, so every group is still a smooth patch or part of one.
template<class CanCross>
std::vector<std::pair<std::vector<int>, double>> classify_depth_groups(
    const ColorPatches &p, const std::vector<Vec3i32> &nbrs, CanCross can_cross, const std::vector<int> &patch,
    const ColorSplitDepths &depths, const ColorSplitParams &params, double D)
{
    std::vector<std::pair<std::vector<int>, double>> groups;
    // Spec 3.5's gates, mirroring the 2D ones (MultiMaterialSegmentation.cpp:1874-1877: a normal shell needs
    // a wall stack to exist and D to reach at least that far; pdmUnlimited sends a band of 0 down that path,
    // so unlimited mode caps nothing at all - here D is +inf instead, hence the isfinite test).
    const bool cap_allowed = params.flat_cap && std::isfinite(D) && depths.ws > 0. && D >= depths.ws;
    if (!cap_allowed) {
        groups.emplace_back(patch, 0.);   // the patch IS one component: it came from connected_components
        return groups;
    }
    // Spec 3.5: flat means tan(theta) < h / (3 ws) off horizontal, i.e. |n_z| > cos(atan(h / (3 ws))).
    const double      tan_flat = depths.layer_height / (3. * depths.ws);
    const float       nz_min   = float(1. / std::sqrt(1. + tan_flat * tan_flat));
    std::vector<char> flat_up(p.surface.indices.size(), 0), flat_down(p.surface.indices.size(), 0), capped(p.surface.indices.size(), 0);
    for (int f : patch) {
        const float nz = unit_face_normal(p, f).z();   // zero for a degenerate facet: neither flat
        if (nz >  nz_min) flat_up[f]   = 1;
        if (nz < -nz_min) flat_down[f] = 1;
    }
    for (int dir = 0; dir < 2; ++dir) {
        const std::vector<char> &flat = dir == 0 ? flat_up : flat_down;
        const double             cap  = dir == 0 ? depths.cap_top : depths.cap_bottom;
        if (cap <= 0. || cap >= D)
            continue;      // spec 3.5's second gate: a cap that reaches as deep as D has nothing to cap,
                           // and a cap of zero would ask for a shell with no depth at all
        for (std::vector<int> &comp : connected_components(p, nbrs, flat, can_cross)) {
            if (!flat_core_survives(p, comp, depths.ws))
                continue;
            for (int f : comp) capped[f] = 1;
            groups.emplace_back(std::move(comp), cap);
        }
    }
    std::vector<char> rest(p.surface.indices.size(), 0);
    for (int f : patch) rest[f] = !capped[f];
    for (std::vector<int> &comp : connected_components(p, nbrs, rest, can_cross))
        groups.emplace_back(std::move(comp), 0.);
    return groups;
}

// Spec 3.6/3.7: everything about one facet group that its working depth cannot change - the facet set, the
// boundary, the wedges a pinch vertex splits into, their nudges, and the spec 3.6 classification of every
// boundary vertex (crease wall or crease step). It is built once per group so the fold guard and the shell
// it guards judge the SAME offset, and so the per-vertex boundary data has one home.
// its_face_neighbors convention (MeshSplitImpl.hpp create_face_neighbors_index via its_triangle_edge,
// TriangleMesh.hpp:253-257): neighbour k sits across the edge (v[k], v[(k+1)%3]).
struct CreaseWall {
    Vec3f n_p{Vec3f::Zero()};    // unit mean normal of the group's facets there - the direction the wall runs
    float half_thickness = 0.f;  // spec 3.4's mid-thickness clamp, measured along THAT direction
};

// Spec 3.6's two CONVEX crease cases, the ones the crease-step option turns on. Case A is the painted face
// that is more horizontal than its neighbour (a painted top meeting a side face): its ring vertex steps a
// wall stack inward and one layer down, then the bottom continues straight down n_P. Case B is the painted
// face that is less horizontal (a painted side meeting the top): the ring steps a wall stack straight in
// along n_P and the bottom then tapers along the vertex normal.
struct CreaseStep {
    Vec3f n_p{Vec3f::Zero()};      // unit mean normal of the group's facets there
    Vec3f t_miter{Vec3f::Zero()};  // case A only: the inward step per unit of wall stack (mitred, see below)
    // Rulings 20 and 21: BOTH cases move along n_P, but d(v) was measured along the vertex normal n(v), so it
    // says nothing about how much material lies that way. A 1.2 mm plate's top corner has d = 1.037 along its
    // bisector while the vertical half-thickness is 0.598; a 1 mm wall's top-edge corner has d = 0.864 while
    // half its thickness is 0.498, and case B's wall-stack step alone would land 0.372 past the mid-surface.
    // So each case re-probes the mid-thickness along n_P - as the concave and same-state walls do, through
    // the same probe (see probe_half_thickness) - and spends at most that.
    float half_thickness = 0.f;
    bool  case_a = false;
};

struct GroupTopology {
    std::vector<char>                            in;              // per facet of p.surface: part of this group
    std::vector<int>                             boundary_count;  // per vertex: boundary edges meeting it
    std::map<std::pair<int, int>, int>           wedge_of;        // (vertex, facet) -> wedge; pinch vertices only
    std::map<std::pair<int, int>, Vec3f>         nudge;           // (vertex, wedge) -> Ruling 8 separation
    std::map<std::pair<int, int>, Vec3f>         n_patch;         // (vertex, wedge) -> unit n_P, zero if degenerate
    std::map<std::pair<int, int>, CreaseWall>    wall;            // (vertex, wedge) -> spec 3.6 crease wall
    std::map<std::pair<int, int>, CreaseStep>    step;            // (vertex, wedge) -> spec 3.6 convex crease

    // .at(): every (pinch vertex, incident group facet) pair was assigned a wedge below, so a miss is a bug
    // in that walk - it must throw rather than quietly hand back wedge 0 and weld the wedges together again.
    int wedge(int v, int f) const { return boundary_count[v] > 2 ? wedge_of.at({v, f}) : 0; }
};

GroupTopology group_topology(const ColorPatches &p, const std::vector<Vec3i32> &nbrs, const AABBMesh &aabb,
                             const ColorSplitDepths &depths, const ColorSplitParams &params,
                             const std::vector<int> &group, int state)
{
    auto unit_normal = [&p](int f) { return unit_face_normal(p, f); };

    GroupTopology topo;
    topo.in.assign(p.surface.indices.size(), 0);
    for (int f : group) topo.in[f] = 1;
    const std::vector<char> &in = topo.in;

    // Boundary edges of the group, and how many of them each vertex carries.
    topo.boundary_count.assign(p.surface.vertices.size(), 0);
    for (int f : group)
        for (int k = 0; k < 3; ++k) {
            int n = nbrs[f][k];
            if (n < 0 || !in[n]) {
                int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                ++topo.boundary_count[a]; ++topo.boundary_count[b];
            }
        }

    // Per-vertex wedge ids: a boundary vertex with more than 2 boundary edges (pinch) gets one copy per wedge.
    // Wedge = the fan of group triangles around the vertex between an incoming and the next outgoing boundary
    // edge; triangles reachable from each other by rotating around the vertex through GROUP neighbours belong
    // to the same wedge. Duplicating them keeps every shell edge on exactly two faces.
    std::map<int, int> wedges_at;   // vertex -> wedge count
    for (int f : group)
        for (int k = 0; k < 3; ++k) {
            int v = p.surface.indices[f][k];
            if (topo.boundary_count[v] <= 2) continue;
            if (topo.wedge_of.count({v, f})) continue;
            int w = wedges_at[v]++;
            std::vector<int> stack{f};
            topo.wedge_of[{v, f}] = w;
            while (!stack.empty()) {
                int g = stack.back(); stack.pop_back();
                for (int e = 0; e < 3; ++e) {
                    int n = nbrs[g][e];
                    if (n < 0 || !in[n]) continue;
                    // neighbour across an edge that contains v -> same wedge
                    int ea = p.surface.indices[g][e], eb = p.surface.indices[g][(e + 1) % 3];
                    if ((ea == v || eb == v) && !topo.wedge_of.count({v, n})) { topo.wedge_of[{v, n}] = w; stack.push_back(n); }
                }
            }
        }

    // Ruling 8: separate the wedges of a pinch vertex. Each wedge is bounded at the vertex by exactly two
    // boundary edges; the unit inward tangent of a boundary edge a->b of facet f is n_f x (b - a), which
    // lies in the facet's plane and points into the group. Their normalised sum is the wedge's inward
    // bisector, and every copy of the vertex in that wedge - top and bottom alike - moves PINCH_NUDGE_MM
    // along it, so the wedges no longer share any geometry.
    std::map<int, float> shortest_edge_at;    // pinch vertex -> its shortest boundary edge
    // Spec 3.6: the boundary as seen from each boundary vertex, per wedge. n_P is the mean normal of the
    // group's own facets there, n_Q the mean normal of the outside facets across its boundary edges, t_in the
    // SUM (not the mean - the mitre below needs its length) of the unit inward tangents of those edges. All
    // four spec 3.6 cases are classified from these: the concave and same-state ones become walls, the two
    // convex ones become steps.
    struct BoundaryInfo {
        Vec3f n_q{Vec3f::Zero()}, t_in{Vec3f::Zero()};
        int   same_state = 0;    // boundary edges whose OUTSIDE facet carries this group's own state
        int   edges      = 0;    // boundary edges of this wedge meeting the vertex
    };
    std::map<std::pair<int, int>, BoundaryInfo> boundary;

    // n_P takes EVERY group facet at the vertex, boundary edge or not: it is the painted surface's own
    // orientation there, which is what the wall at a crease follows - and, since spec 3.4a (Ruling 24), the
    // reference the mitre measures the bisector against, so it is kept for INTERIOR vertices too. An interior
    // vertex of a curved patch does not share the group's mean normal, so this has to stay per vertex.
    for (int f : group) {
        const Vec3f nf = unit_normal(f);
        for (int k = 0; k < 3; ++k) {
            const std::pair<int, int> key{p.surface.indices[f][k], topo.wedge(p.surface.indices[f][k], f)};
            // emplace, not operator[]: an Eigen vector's default constructor leaves it UNINITIALISED, so a
            // map's value-initialised slot would be garbage to accumulate onto.
            topo.n_patch.emplace(key, Vec3f(Vec3f::Zero())).first->second += nf;
        }
    }
    for (std::pair<const std::pair<int, int>, Vec3f> &entry : topo.n_patch)
        entry.second = entry.second.squaredNorm() > 0.f ? Vec3f(entry.second.normalized()) : Vec3f(Vec3f::Zero());
    for (int f : group)
        for (int k = 0; k < 3; ++k) {
            const int n = nbrs[f][k];
            if (n >= 0 && in[n]) continue;
            const Vec3i32 &t    = p.surface.indices[f];
            const int      a    = t[k], b = t[(k + 1) % 3];
            const Vec3f    edge = p.surface.vertices[b] - p.surface.vertices[a];
            const Vec3f    tang = unit_normal(f).cross(edge);
            if (tang.squaredNorm() <= 0.f) continue;   // degenerate facet or edge: no tangent to work from
            const Vec3f t_in = tang.normalized();
            const Vec3f n_q  = n >= 0 ? unit_normal(n) : Vec3f(Vec3f::Zero());
            const bool same_state = n >= 0 && p.facet_state[size_t(n)] == state;
            for (int v : {a, b}) {
                BoundaryInfo &info = boundary[{v, topo.wedge(v, f)}];
                info.n_q  += n_q;
                info.t_in += t_in;
                info.same_state += same_state ? 1 : 0;
                ++info.edges;
                if (topo.boundary_count[v] <= 2) continue;
                const std::pair<int, int> key{v, topo.wedge(v, f)};
                auto it = topo.nudge.find(key);
                if (it == topo.nudge.end()) it = topo.nudge.emplace(key, Vec3f(Vec3f::Zero())).first;
                it->second += t_in;
                auto shortest = shortest_edge_at.find(v);
                if (shortest == shortest_edge_at.end()) shortest_edge_at.emplace(v, edge.norm());
                else                                    shortest->second = std::min(shortest->second, edge.norm());
            }
        }
    for (std::pair<const std::pair<int, int>, Vec3f> &entry : topo.nudge) {
        const float len = entry.second.norm();
        // Ruling 11: never move a vertex further than a tenth of the shortest boundary edge meeting it,
        // so the nudge cannot invert a micron-scale triangle.
        const float eps = std::min(PINCH_NUDGE_MM, 0.1f * shortest_edge_at.at(entry.first.first));
        entry.second = len > 0.f ? Vec3f(entry.second * (eps / len)) : Vec3f(Vec3f::Zero());
    }

    // Spec 3.6: at two kinds of boundary the wall runs straight down the patch's own normal n_P instead of
    // along the vertex bisector - no step, no taper.
    //   * The CONCAVE crease (third bullet, Ruling 14): the surfaces meet at more than 15 degrees and the
    //     outside neighbour rises over the painted face (n_Q . t_in > 0) - a painted boss side meeting the
    //     block top. A bisector there would carry a hidden painted skirt into the neighbouring body and cost
    //     toolchanges on layers that carry no paint at all.
    //   * The SAME-STATE crease (spec 3.1a, Ruling 18): the facet across the boundary carries this very
    //     state, so the boundary is inside the painted region - either a crease between two patches or, once
    //     spec 3.5 has cut a capped group out of a patch, the seam between that group and the rest of it -
    //     and the group on the other side of it is claiming the material right behind this wall (there the
    //     rule is not about a crease at all: it is what makes a capped top and the uncapped slope beside it
    //     meet). Straight is what makes the two claims meet - the
    //     top slab's walls go straight down, the side tube's straight inward - and it is what lets a coarse
    //     two-ring cylinder come out solid with no extra vertices at all. A bisector would tilt this wall
    //     into the neighbour's claim and leave the feature hollow.
    // Both are always on, independent of the crease-step option.
    // The other two cases are the CONVEX creases the crease-step option owns (spec 3.6, bullets 2 and 3):
    // case A where the painted face is the more horizontal of the two, case B where it is the less
    // horizontal one. They only describe where the ring and bottom copies go, so they are recorded here and
    // read by the builder; the builder ignores them when the option is off.
    constexpr float CREASE_COS_15 = 0.96592583f;
    // Case A insets the ring by one wall stack along the boundary's inward tangent. At a corner both edges'
    // insets have to hold at once, so the tangent is MITRED: with S the sum of the k unit inward tangents,
    // k*S/|S|^2 insets every one of them by exactly 1 (a cube's top corner then steps (0.87, 0.87, 0), not
    // 0.87 along the diagonal, and a straight boundary still steps its plain 0.87). The limit is Clipper's
    // idea (ClipperUtils' default miter limit is 3): at a needle-sharp corner the exact mitre runs away and
    // would fling the ring vertex clear of the patch.
    constexpr float CREASE_MITER_LIMIT = 4.f;
    // Ruling 22's gate, answered at most once per group and only when a case A vertex actually asks for it -
    // it costs a Clipper union of the whole group.
    enum class CaseAGate { Unknown, Allowed, Denied };
    CaseAGate case_a_gate = CaseAGate::Unknown;
    // Rulings 20/21: the mid-thickness along `dir`, measured ONE WALL STACK INTO the patch and snapped back
    // onto the surface, for every rule that walks a vertex down a direction of its own. Two reasons for the
    // inset. A probe launched from the boundary vertex itself grazes the neighbouring faces that vertex also
    // sits on: measured on a 1 mm wall, the ray along -n_P from a top corner slides along both the top and
    // the end face, reports NO hit, and the clamp silently becomes +inf - no clamp at all. And case A's wall
    // really does descend one wall stack in, not at the boundary. The inset point is tangential, so it is
    // generally not ON the surface (on a convex patch it floats ws^2/2R above it, where the probe would
    // measure the air gap); snapping it to the nearest surface point is what makes half_thickness_along -
    // which measures a thickness FROM a surface point - answer the question the wall is asking.
    auto probe_half_thickness = [&](int v, const Vec3f &t_miter, const Vec3f &dir) {
        int   face    = 0;
        Vec3d closest = Vec3d::Zero();
        aabb.squared_distance((p.surface.vertices[v] + float(depths.ws) * t_miter).cast<double>(), face, closest);
        return ColorSplitDetail::half_thickness_along(aabb, closest.cast<float>(), dir);
    };
    for (const std::pair<const std::pair<int, int>, BoundaryInfo> &entry : boundary) {
        const BoundaryInfo &info = entry.second;
        // n_P is all the same-state rule needs, so only the concave test may demand n_Q and t_in as well
        // (an outside neighbourhood whose normals cancel must not cost a patch its straight same-state wall).
        const Vec3f n_p = topo.n_patch.at(entry.first);
        if (n_p.squaredNorm() <= 0.f)
            continue;                                  // degenerate facets: no orientation to judge
        const bool  outside = info.n_q.squaredNorm() > 0.f && info.t_in.squaredNorm() > 0.f;
        const Vec3f n_q     = outside ? Vec3f(info.n_q.normalized()) : Vec3f(Vec3f::Zero());
        const Vec3f t_in    = outside ? Vec3f(info.t_in.normalized()) : Vec3f(Vec3f::Zero());
        const bool  crease  = outside && n_p.dot(n_q) < CREASE_COS_15;
        const bool  concave = crease && n_q.dot(t_in) > 0.f;
        // A vertex carries ONE ring copy and ONE bottom copy, so it gets ONE classification however many
        // boundary edges of however many kinds meet there: a mixed vertex is judged on the MEAN outside
        // normal above, except that a same-state crease always wins. The neighbouring patch is claiming the
        // material immediately behind that wall and tilting it is exactly what opens a gap between them,
        // whereas an ordinary boundary only loses the cosmetic taper spec 3.6 gives it.
        // The mitred inward tangent (see below) is what every one of the four rules measures its
        // mid-thickness from, so it is computed for all of them; only case A also STEPS along it.
        const float len     = info.t_in.norm();
        const Vec3f t_miter = outside ? Vec3f(info.t_in * (std::min(float(info.edges) / len, CREASE_MITER_LIMIT) / len))
                                      : Vec3f(Vec3f::Zero());
        if (concave || info.same_state > 0) {
            topo.wall.emplace(entry.first, CreaseWall{n_p, probe_half_thickness(entry.first.first, t_miter, n_p)});
            continue;
        }
        if (!crease || !params.crease_step)
            continue;                                  // plain boundary, or the option is off: no step
        // Ruling 25: both convex cases are about the ASYMMETRY between the painted face and a neighbour that
        // is more (case A) or less (case B) horizontal than it - case B exists so a painted SIDE face keeps
        // its outer wall stack up to the TOP edge, where the layers above it end. Between two equally
        // horizontal faces - two vertical faces meeting at a vertical edge - there is no such asymmetry and
        // no layer argument, and the 2D segmentation simply draws its 45 degree Voronoi diagonal down the
        // shared edge. A tie therefore keeps the plain mitred bisector (spec 3.4a), which is that diagonal,
        // instead of falling into case B on the tie-break of a strict comparison.
        constexpr float CREASE_TIE_EPS = 1e-3f;
        const float p_z = std::abs(n_p.z()), q_z = std::abs(n_q.z());
        if (std::abs(p_z - q_z) <= CREASE_TIE_EPS)
            continue;
        CreaseStep step;
        step.n_p    = n_p;
        step.case_a = p_z > q_z;
        if (step.case_a) {
            // Ruling 22: case A's inset only makes sense on a group that HAS a wall stack to give. On one
            // narrower than 2 ws - an embossed text stroke, a rib - the insets from opposite sides cross,
            // the ring inverts and the group is dropped by the validity fallback. Where the group's own
            // outline cannot survive the inset, its case A vertices fall back to the plain bisector rule.
            if (case_a_gate == CaseAGate::Unknown)
                case_a_gate = projection_survives(p, group, group_mean_normal(p, group), depths.ws)
                                  ? CaseAGate::Allowed : CaseAGate::Denied;
            if (case_a_gate == CaseAGate::Denied)
                continue;
        }
        step.t_miter        = t_miter;
        step.half_thickness = probe_half_thickness(entry.first.first, t_miter, n_p);
        topo.step.emplace(entry.first, step);
    }
    return topo;
}

// Spec 3.7: turns one facet group into a closed solid by offsetting its vertices inward.
struct ShellBuilder {
    const ColorPatches         &p;
    const std::vector<Vec3i32> &nbrs;          // its_face_neighbors(p.surface)
    const std::vector<Vec3f>   &normals;       // color_split_normals
    const GroupTopology        &topo;          // group_topology(p, nbrs, aabb, depths, params, group, state)
    const std::vector<float>   &d0;            // compute_vertex_depths output; the halving floor may not exceed it
    std::vector<float>          depth;         // working copy of d0 (the fold guard lowers it)
    const std::vector<float>   &half;          // raw t(v)/2 - delta along n(v); spec 3.4a's clamp on the mitre
    const ColorSplitDepths     &depths;
    bool                        crease_step;   // ColorSplitParams::crease_step (spec 3.6's convex cases)

    // Where a vertex's ring copy a1 and its bottom copy a' land relative to its top copy (spec 3.6). `stepped`
    // says whether the ring is a copy of its own: when it is not, the side of the shell is the single strip
    // (b, a, a', b') this stage built before the ring existed.
    struct SideOffsets { Vec3f ring{Vec3f::Zero()}, bottom{Vec3f::Zero()}; bool stepped = false; };

    // Spec 3.4a (Ruling 24): how far a segment has to travel ALONG THE BISECTOR n(v) to bury `d` of depth
    // perpendicular to the patch. d is a depth, not a distance: on a plain cube face - whose only vertices
    // are its corners, at 54.7 degrees to it - spending d along n(v) buried only d/sqrt(3), a third of the
    // claim (the Task 8 parity measurement: 45.9 mm^2/layer against the 2D band's 54.4). Dividing by
    // n(v).n_P is the classic mitre, and it lands the corner on the 45 degree diagonal the 2D Voronoi
    // segmentation draws at the same edge. The 0.5 floor is the mitre limit of 2 (a 60 degree half-angle):
    // at a needle-sharp spike the exact mitre runs away, and doubling the depth there is already generous.
    // The half-thickness probe then clamps the LENGTHENED segment - it is measured along n(v), which is
    // exactly the direction travelled - so no mitred wall can cross the mid-surface (spec 3.4).
    float bisector_length(int v, const Vec3f &n_p, float d) const
    {
        const float cosang = n_p.squaredNorm() > 0.f ? std::max(0.5f, normals[v].dot(n_p)) : 1.f;
        return std::min(d / cosang, half[v]);
    }

    // build() and fold_guard() share this, so the guard always judges the shell that is actually built.
    SideOffsets side_offsets(int v, int wedge_id, double cap_depth) const
    {
        const float h = float(depths.layer_height), ws = float(depths.ws);
        float       d = depth[v];
        if (cap_depth > 0.) d = std::min(d, float(cap_depth));

        // Spec 3.6, concave and same-state creases: the wall walks straight down the patch's own normal n_P
        // with no step and no taper, and clamps d to the mid-thickness measured along THAT direction (d(v)
        // was measured along n(v) and says nothing about how much material lies along n_P - on a painted boss
        // it is the difference between stopping at the axis and crossing straight through it).
        const auto wall = topo.wall.find({v, wedge_id});
        if (wall != topo.wall.end()) {
            const Vec3f bottom = -std::min(d, wall->second.half_thickness) * wall->second.n_p;
            return {bottom, bottom, false};
        }
        // Spec 3.4a: the three segments below that travel along n(v) are mitred against the patch normal
        // there, which every group vertex carries - a curved patch's interior does not share one.
        const Vec3f n_patch = topo.n_patch.at({v, wedge_id});
        if (!crease_step || topo.boundary_count[v] == 0) { // interior vertices are never on a strip
            const Vec3f bottom = -bisector_length(v, n_patch, d) * normals[v];
            return {bottom, bottom, false};
        }

        // Every first segment is clamped to d: the ring may never sink past the bottom, and d is what spec
        // 3.4 measured as the room this vertex has before its mid-surface.
        const auto step = topo.step.find({v, wedge_id});
        Vec3f      ring, bottom;
        if (step == topo.step.end()) {                     // plain boundary: one layer down, then the taper
            const float first = std::min(d, h);            // mitred alike, so the ring really is a layer down
            ring   = -bisector_length(v, n_patch, first) * normals[v];
            bottom = -bisector_length(v, n_patch, d) * normals[v];
        } else if (step->second.case_a) {                  // painted face more horizontal: inset, then n_P
            const float d_p   = std::min(d, step->second.half_thickness);   // Ruling 20: clamped along n_P
            const float first = std::min(d_p, h);
            ring   = ws * step->second.t_miter - first * step->second.n_p;
            bottom = ring - (d_p - first) * step->second.n_p;
        } else {                                           // painted face less horizontal: a full stack in
            // Ruling 21: the wall stack is spent along n_P, so it is the n_P mid-thickness that bounds it -
            // and the SAME budget bounds what is left for the taper. Spec 3.4a mitres that second segment,
            // which makes the equality exact: it buries (d_p - first) of depth along n_P, so the bottom sits
            // d_p behind the surface, never more. Clamping only the step would still walk the second segment
            // across the mid-surface on a thin wall.
            const float d_p   = std::min(d, step->second.half_thickness);
            const float first = std::min(d_p, ws);
            ring   = -first * step->second.n_p;
            bottom = ring - bisector_length(v, n_patch, d_p - first) * normals[v];
        }
        // Spec 3.6: when the depth leaves no room for a second strip - less than a layer of it - the bottom
        // collapses onto the ring and only the first strip is emitted.
        if ((bottom - ring).norm() <= h)
            return {ring, ring, false};
        return {ring, bottom, true};
    }

    // Builds the closed shell of the facet group `group` (indices into p.surface).
    indexed_triangle_set build(const std::vector<int> &group, double cap_depth /* 0 = none */) const
    {
        // Output vertex ids per (vertex, wedge): the top copy, the spec 3.6 ring copy and the bottom copy.
        // Non-pinch vertices: wedge 0 only. Where the ring is not a step of its own it IS the bottom copy, so
        // the strip triangles that would carry it degenerate and are dropped below.
        struct Copies { int top = 0, ring = 0, bottom = 0; };
        indexed_triangle_set out;
        std::map<std::pair<int, int>, Copies> ids;
        auto vertex_ids = [&](int v, int wedge_id) -> Copies {
            auto it = ids.find({v, wedge_id});
            if (it != ids.end()) return it->second;
            auto              nudged = topo.nudge.find({v, wedge_id});
            const Vec3f       top    = nudged == topo.nudge.end() ? p.surface.vertices[v] : Vec3f(p.surface.vertices[v] + nudged->second);
            const SideOffsets off    = side_offsets(v, wedge_id, cap_depth);
            Copies            c;
            c.top    = int(out.vertices.size()); out.vertices.push_back(top);
            c.bottom = int(out.vertices.size()); out.vertices.push_back(Vec3f(top + off.bottom));
            c.ring   = c.bottom;
            if (off.stepped) { c.ring = int(out.vertices.size()); out.vertices.push_back(Vec3f(top + off.ring)); }
            return ids[{v, wedge_id}] = c;
        };
        // A strip triangle whose ring copy is its own bottom copy is a degenerate sliver, not geometry: the
        // quad it belonged to is a triangle. Dropping it is what turns the two strips back into one where a
        // vertex has no step - including where only ONE end of the edge has one.
        auto tri = [&out](int a, int b, int c) { if (a != b && b != c && a != c) out.indices.emplace_back(a, b, c); };

        // Top triangles keep the surface winding; the bottom copies are reversed so they face into the part.
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            auto A = vertex_ids(t[0], topo.wedge(t[0], f)), B = vertex_ids(t[1], topo.wedge(t[1], f)), C = vertex_ids(t[2], topo.wedge(t[2], f));
            out.indices.emplace_back(A.top, B.top, C.top);
            out.indices.emplace_back(A.bottom, C.bottom, B.bottom);
        }
        // Side strips on every boundary edge a->b of triangle f: the top contributes the directed edge a->b
        // and the reversed bottom contributes b_bottom->a_bottom, so the two cycles (b, a, a1, b1) and
        // (b1, a1, a', b') close both ends and meet each other along the ring.
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n >= 0 && topo.in[n]) continue;
                int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                auto A = vertex_ids(a, topo.wedge(a, f)), B = vertex_ids(b, topo.wedge(b, f));
                tri(B.top,  A.top,  A.ring);
                tri(B.top,  A.ring, B.ring);
                tri(B.ring, A.ring, A.bottom);
                tri(B.ring, A.bottom, B.bottom);
            }
        return out;
    }

    // Spec 3.4 fold guard: lower depth where a bottom triangle would invert or collapse. Returns true if any change.
    bool fold_guard(const std::vector<int> &group, double cap_depth)
    {
        std::vector<int> to_halve;
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            Vec3f a = p.surface.vertices[t[0]], b = p.surface.vertices[t[1]], c = p.surface.vertices[t[2]];
            Vec3f nt = (b - a).cross(c - a);
            if (nt.squaredNorm() <= 0.f)                    // a degenerate top facet has no orientation to preserve
                continue;
            Vec3f a2 = a + side_offsets(t[0], topo.wedge(t[0], f), cap_depth).bottom,
                  b2 = b + side_offsets(t[1], topo.wedge(t[1], f), cap_depth).bottom,
                  c2 = c + side_offsets(t[2], topo.wedge(t[2], f), cap_depth).bottom;
            Vec3f nb = (c2 - a2).cross(b2 - a2);            // reversed winding -> should point along -nt
            // |n| == 2 * area, so the norm ratio IS the area ratio: only a truly collapsed bottom trips the guard.
            if (nb.dot(-nt) <= 0.f || nb.norm() < 1e-6f * nt.norm())
                for (int k = 0; k < 3; ++k) to_halve.push_back(t[k]);
        }
        return halve(std::move(to_halve), group, cap_depth);
    }

    // Uniformly halves the whole group's depth - the retry after a failed validity check. Returns false when
    // no depth left in the group can move its shell, so the caller can stop instead of rebuilding an
    // identical mesh and re-running CGAL on it.
    bool halve_depth(const std::vector<int> &group, double cap_depth)
    {
        std::vector<int> vertices;
        vertices.reserve(group.size() * 3);
        for (int f : group)
            for (int k = 0; k < 3; ++k) vertices.push_back(p.surface.indices[f][k]);
        return halve(std::move(vertices), group, cap_depth);
    }

    // Every (vertex, wedge) pair the shell of `group` builds a copy for, deduplicated.
    std::vector<std::pair<int, int>> corners(const std::vector<int> &group) const
    {
        std::vector<std::pair<int, int>> out;
        out.reserve(group.size() * 3);
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                const int v = p.surface.indices[f][k];
                out.emplace_back(v, topo.wedge(v, f));
            }
        std::sort(out.begin(), out.end());
        out.erase(std::unique(out.begin(), out.end()), out.end());
        return out;
    }

    // Halves the depth of each listed vertex. The list is deduplicated first: a vertex is shared by several
    // facets, and halving it per incident facet would divide by 2^valence and drop straight to the floor in a
    // single round instead of the spec's one halving per round.
    // Returns whether the SHELL moved, not whether depth[v] did, and keeps halving until it does: a vertex's
    // bottom is clamped by the group's cap and, at a crease wall, by the mid-thickness along n_P, so a
    // halving can leave the geometry untouched. Reporting that as progress makes the caller rebuild and
    // re-run CGAL on an identical mesh; reporting it as "nothing left to try" would abandon a group whose
    // depth still has room below the clamp. One EFFECTIVE halving per round is what both callers want.
    bool halve(std::vector<int> vertices, const std::vector<int> &group, double cap_depth)
    {
        if (vertices.empty())
            return false;                             // nothing was asked for: no snapshot, no rebuild
        std::sort(vertices.begin(), vertices.end());
        vertices.erase(std::unique(vertices.begin(), vertices.end()), vertices.end());
        const std::vector<std::pair<int, int>> keys = corners(group);
        std::vector<Vec3f>                     before;
        before.reserve(keys.size());
        for (const std::pair<int, int> &key : keys) before.push_back(side_offsets(key.first, key.second, cap_depth).bottom);
        for (;;) {
            bool lowered = false;
            for (int v : vertices) {
                // Ruling 9: the floor is one layer, but never deeper than the vertex's own mid-thickness
                // clamp - on a feature thinner than 2h + 2*delta the layer floor would otherwise RAISE the
                // depth and push the bottom across the mid-surface, breaking d(v) <= t(v)/2 - delta.
                const float floor_d = std::min(float(depths.layer_height), d0[v]);
                const float d       = std::max(floor_d, depth[v] * 0.5f);
                if (d != depth[v]) { depth[v] = d; lowered = true; }
            }
            if (!lowered)
                return false;                     // every vertex sits on its floor: nothing left to try
            for (size_t i = 0; i < keys.size(); ++i)
                if ((side_offsets(keys[i].first, keys[i].second, cap_depth).bottom - before[i]).squaredNorm() > 0.f)
                    return true;
        }
    }
};

} // namespace

std::vector<ColorShell> build_color_shells(const ColorPatches &p, const ColorSplitDepths &depths_in, const ColorSplitParams &params, const ColorSplitProgress &progress, std::vector<std::string> *warnings)
{
    const ColorSplitDepths depths = ColorSplitDetail::effective_depths(depths_in, params);
    const double D = depths.unlimited ? std::numeric_limits<double>::infinity() : depths.D;
    std::vector<Vec3i32> nbrs    = its_face_neighbors(p.surface);
    std::vector<Vec3f>   normals = color_split_normals(p.surface);
    // One tree for the whole stage: the depth model probes it per vertex, and spec 3.6's crease walls
    // re-probe it along their own direction.
    const AABBMesh       aabb(p.surface);
    // half = the same probe without D applied: spec 3.4a's mitre spends more than d(v) along the bisector and
    // this is what stops it at the mid-surface all the same.
    std::vector<float>   half;
    std::vector<float>   depth   = ColorSplitDetail::vertex_depths(aabb, p, normals, D, &half);

    // Materialise every group up front: the progress callback is documented as a 0..100 percentage
    // (ColorSplit.hpp) and this stage owns the 10..50 band, so the tick needs the real group total - a single
    // state can hold dozens of them (painted text, a logo), spec 3.1a cuts each of those again at every
    // crease, and spec 3.5 cuts the flat cap out of what is left.
    auto smooth = [&p](int a, int b) { return unit_face_normal(p, a).dot(unit_face_normal(p, b)) > SMOOTH_COS_30; };
    struct DepthGroup { int state; std::vector<int> facets; double cap; };   // cap 0 = built at d(v)
    std::vector<DepthGroup> groups;
    for (int s : p.states) {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (size_t f = 0; f < in.size(); ++f) in[f] = char(p.facet_state[f] == s);
        std::vector<DepthGroup> uncapped;
        for (std::vector<int> &patch : connected_components(p, nbrs, in, smooth))
            for (std::pair<std::vector<int>, double> &g : classify_depth_groups(p, nbrs, smooth, patch, depths, params, D))
                // Spec 3.8: within a filament the capped groups are split out first, so where a capped group
                // and an uncapped one overlap the shallower claim wins.
                (g.second > 0. ? groups : uncapped).push_back({s, std::move(g.first), g.second});
        for (DepthGroup &g : uncapped) groups.push_back(std::move(g));
    }

    std::vector<ColorShell> shells;
    size_t done = 0;
    for (const DepthGroup &group : groups) {
        const int               s    = group.state;
        const std::vector<int> &comp = group.facets;
        const double            cap_depth = group.cap;
        const GroupTopology topo = group_topology(p, nbrs, aabb, depths, params, comp, s);
        // d0 (reference) and its working copy
        ShellBuilder sb{p, nbrs, normals, topo, depth, depth, half, depths, params.crease_step};
        for (int round = 0; round < 8 && sb.fold_guard(comp, cap_depth); ++round) {}
        indexed_triangle_set mesh = sb.build(comp, cap_depth);
        ShellCheck check = check_shell(mesh);
        // Validity fallback: halve the component's depth until the shell is clean, floor = one layer. Once
        // every vertex sits on its floor there is nothing left to try and the rebuild would be identical.
        // Ruling 15: reining an offset in is ordinary work, not an event - a salvaged component gets no note.
        for (int round = 0; round < 6 && (!check.closed || check.self_intersects); ++round) {
            if (!sb.halve_depth(comp, cap_depth)) break;
            mesh  = sb.build(comp, cap_depth);
            check = check_shell(mesh);
        }
        // Spec 7 (rev 2.3): a component that still fails at its floor depth is a feature too small to
        // split, not an error. Drop it - the body keeps it in its own colour - and note it for the user.
        if (check.closed && !check.self_intersects)
            shells.push_back({s, cap_depth > 0., std::move(mesh)});
        else if (warnings)
            warnings->push_back(too_small_warning(p, comp, s));
        if (progress && !progress(int(10 + 40 * double(++done) / double(std::max<size_t>(1, groups.size())))))
            throw ColorSplitCancelled();
    }
    return shells;
}

} // namespace Slic3r
