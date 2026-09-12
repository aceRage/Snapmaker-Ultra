#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <set>

#include "libslic3r/MeshEdit.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;
using namespace Slic3r::MeshEdit;

// ----------------------------------------------------------------------------
// helpers
// ----------------------------------------------------------------------------

namespace {

// A facet on the face of an axis-aligned cube whose outward normal is `axis`.
// its_make_cube() puts the cube's min corner at the origin, so the +Z face sits
// at z == size_z; the caller passes the axis it wants and gets the first facet
// whose normal matches.
static int facet_on_face(const indexed_triangle_set &its, const Vec3f &axis)
{
    for (size_t i = 0; i < its.indices.size(); ++i) {
        const auto  &t = its.indices[i];
        const Vec3f &a = its.vertices[t[0]];
        const Vec3f &b = its.vertices[t[1]];
        const Vec3f &c = its.vertices[t[2]];
        const Vec3f  n = (b - a).cross(c - a).normalized();
        if (n.dot(axis) > 0.999f)
            return int(i);
    }
    return -1;
}

// Extrude a closed profile (given in XZ) along +Y into a solid prism.
//
// The profile must be listed CLOCKWISE in (x, z). That is deliberate: the 3D map
// is (x, y, z) = (p.x, y, p.y), and x cross z is -y, so a profile that looks
// counter-clockwise on paper extrudes along +Y into an INSIDE-OUT solid.
// Clockwise here is what gives outward normals.
//
// `fan_apex` is the profile vertex the two end caps are triangulated from. A fan
// is only valid from a vertex that "sees" the whole polygon, which for a convex
// profile is any vertex but for a non-convex one (the L below) is not - so the
// caller names it rather than the helper guessing.
static indexed_triangle_set extrude_profile(const std::vector<Vec2f> &profile, float depth, size_t fan_apex)
{
    const size_t n = profile.size();
    indexed_triangle_set its;
    its.vertices.reserve(2 * n);
    for (int side = 0; side < 2; ++side) {
        const float y = side == 0 ? 0.f : depth;
        for (const Vec2f &p : profile)
            its.vertices.emplace_back(Vec3f(p.x(), y, p.y()));
    }

    // Side walls, one quad per profile segment.
    for (size_t i = 0; i < n; ++i) {
        const int a0 = int(i);
        const int b0 = int((i + 1) % n);
        const int a1 = a0 + int(n);
        const int b1 = b0 + int(n);
        its.indices.emplace_back(stl_triangle_vertex_indices(a0, b0, b1));
        its.indices.emplace_back(stl_triangle_vertex_indices(a0, b1, a1));
    }
    // Caps: a fan from `fan_apex` on each ring, the -Y one wound the other way.
    for (size_t k = 1; k + 1 < n; ++k) {
        const int a = int(fan_apex);
        const int b = int((fan_apex + k) % n);
        const int c = int((fan_apex + k + 1) % n);
        its.indices.emplace_back(stl_triangle_vertex_indices(a, c, b));
        its.indices.emplace_back(stl_triangle_vertex_indices(a + int(n), b + int(n), c + int(n)));
    }
    return its;
}

// A box with one edge chamfered: the cube's +X/+Z edge is replaced by a 45 deg
// flat. Every face is exactly planar, so the coplanar set of each is known by
// construction: 5 side faces (top, the chamfer, +X, bottom, -X) plus the two end
// caps - 7 planar faces in total.
static indexed_triangle_set make_chamfered_box(float w, float d, float h, float c)
{
    return extrude_profile({ Vec2f(0.f, h), Vec2f(w - c, h), Vec2f(w, h - c), Vec2f(w, 0.f), Vec2f(0.f, 0.f) },
                           d, /* fan_apex */ 0);
}

// An L-shaped prism, 20 x 20 with a 10 x 10 notch cut out of the +X/+Z corner.
//
// This exists because a cube CANNOT be pushed into a self-intersection: driving
// its top face down through the bottom leaves the four walls on exactly the same
// footprint, so the solid merely INVERTS - no two triangles ever cross, and CGAL
// is right to say so. A genuine self-intersection needs a face whose travel runs
// into OTHER geometry, which is what the notch provides: the inner wall at
// x == 10 faces +X, and pushing it past x == 20 drives it clean through the
// outer +X wall.
static indexed_triangle_set make_L_prism(float size, float depth, float notch)
{
    const std::vector<Vec2f> profile = {
        Vec2f(0.f, size), Vec2f(size - notch, size), Vec2f(size - notch, size - notch),
        Vec2f(size, size - notch), Vec2f(size, 0.f), Vec2f(0.f, 0.f)
    };
    // The reflex vertex is (size-notch, size-notch); a fan from the origin corner
    // is the one that stays inside the L.
    return extrude_profile(profile, depth, /* fan_apex */ 5);
}

// A closed tessellated cylinder: `segs` side quads, flat top and bottom caps.
static indexed_triangle_set make_cylinder(float r, float h, int segs)
{
    indexed_triangle_set its;
    for (int k = 0; k < segs; ++k) {
        const float a = 2.f * float(M_PI) * float(k) / float(segs);
        its.vertices.emplace_back(Vec3f(r * std::cos(a), r * std::sin(a), 0.f));
    }
    for (int k = 0; k < segs; ++k) {
        const float a = 2.f * float(M_PI) * float(k) / float(segs);
        its.vertices.emplace_back(Vec3f(r * std::cos(a), r * std::sin(a), h));
    }
    const int cb = int(its.vertices.size());
    its.vertices.emplace_back(Vec3f(0.f, 0.f, 0.f));
    const int ct = int(its.vertices.size());
    its.vertices.emplace_back(Vec3f(0.f, 0.f, h));

    for (int k = 0; k < segs; ++k) {
        const int a0 = k, b0 = (k + 1) % segs;
        const int a1 = a0 + segs, b1 = b0 + segs;
        its.indices.emplace_back(stl_triangle_vertex_indices(a0, b0, b1));
        its.indices.emplace_back(stl_triangle_vertex_indices(a0, b1, a1));
        its.indices.emplace_back(stl_triangle_vertex_indices(cb, b0, a0));   // bottom
        its.indices.emplace_back(stl_triangle_vertex_indices(ct, a1, b1));   // top
    }
    return its;
}

static bool watertight(const indexed_triangle_set &its)
{
    return its_num_open_edges(its) == 0;
}

// Every edge has exactly two incident facets: the manifold test the volume
// assertion leans on.
static bool manifold(const MeshTopology &topo)
{
    for (int e = 0; e < topo.num_edges; ++e)
        if (topo.edge_face_count[size_t(e)] != 2)
            return false;
    return true;
}

} // namespace

// ----------------------------------------------------------------------------
// feature-edge chains
// ----------------------------------------------------------------------------

TEST_CASE("MeshEdit: a cube has 12 feature edges in 12 chains at 45 deg", "[MeshEdit]")
{
    const indexed_triangle_set its  = its_make_cube(10., 10., 10.);
    const MeshTopology         topo = build_topology(its);
    REQUIRE(topo.valid());
    REQUIRE(manifold(topo));

    const std::vector<uint8_t> mask = feature_edge_mask(topo, 45.f);
    const int n_feature = int(std::count(mask.begin(), mask.end(), uint8_t(1)));

    // A cube's 12 real edges have a 90 deg dihedral. The 6 diagonals that split
    // each square face into two triangles are coplanar (0 deg) and must not
    // count, and neither may anything else.
    CHECK(n_feature == 12);

    const std::vector<EdgeChain> chains = all_edge_chains(its, topo, 45.f, 35.f);
    // Every cube vertex has THREE incident feature edges, so every vertex is a
    // corner and no chain can ever extend: 12 chains of one edge each. This is
    // exactly the corner rule the spec asks for, measured.
    CHECK(chains.size() == 12);
    for (const EdgeChain &c : chains) {
        CHECK(c.edges.size() == 1);
        CHECK_FALSE(c.closed);
    }

    // Every feature edge is claimed by exactly one chain.
    std::set<int> claimed;
    for (const EdgeChain &c : chains)
        for (int e : c.edges) {
            CHECK(claimed.insert(e).second);
            CHECK(mask[size_t(e)] == 1);
        }
    CHECK(int(claimed.size()) == n_feature);
}

TEST_CASE("MeshEdit: a cylinder's rim is one closed chain, its wall seams are not features", "[MeshEdit]")
{
    const int                  segs = 32;
    const indexed_triangle_set its  = make_cylinder(5.f, 10.f, segs);
    const MeshTopology         topo = build_topology(its);
    REQUIRE(topo.valid());
    REQUIRE(watertight(its));
    REQUIRE(manifold(topo));

    const std::vector<uint8_t> mask = feature_edge_mask(topo, 30.f);

    // The two rims: 2 * segs edges. Nothing else may qualify - a 32-segment
    // cylinder turns 11.25 deg per wall seam, and each wall quad's diagonal is
    // coplanar with its own quad.
    CHECK(int(std::count(mask.begin(), mask.end(), uint8_t(1))) == 2 * segs);

    // Seed on a top-rim edge: the walk must close into the whole rim, because
    // every rim vertex has exactly two incident feature edges and the turn per
    // step (360/segs = 11.25 deg) is under the 35 deg continuation threshold.
    int rim_edge = -1;
    for (int e = 0; e < topo.num_edges && rim_edge < 0; ++e) {
        if (!mask[size_t(e)])
            continue;
        const Vec2i32 &ev = topo.edge_vertices[size_t(e)];
        if (its.vertices[size_t(ev(0))].z() > 9.99f && its.vertices[size_t(ev(1))].z() > 9.99f)
            rim_edge = e;
    }
    REQUIRE(rim_edge >= 0);

    const EdgeChain chain = grow_edge_chain(its, topo, mask, rim_edge, 35.f);
    CHECK(chain.closed);
    CHECK(chain.edges.size() == size_t(segs));

    // Both rims, and nothing else: two chains.
    const std::vector<EdgeChain> chains = all_edge_chains(its, topo, 30.f, 35.f);
    CHECK(chains.size() == 2);
}

// ----------------------------------------------------------------------------
// planar face regions
// ----------------------------------------------------------------------------

TEST_CASE("MeshEdit: a cube face region is exactly its 2 triangles", "[MeshEdit]")
{
    const indexed_triangle_set its  = its_make_cube(10., 10., 10.);
    const MeshTopology         topo = build_topology(its);
    const int                  seed = facet_on_face(its, Vec3f::UnitZ());
    REQUIRE(seed >= 0);

    RegionParams p;
    p.mode          = RegionMode::Planar;
    p.angle_tol_deg = 1.f;

    const FaceRegion region = grow_face_region(its, topo, size_t(seed), p);
    CHECK(region.facets.size() == 2);
    CHECK(region.normal.z() == Approx(1.f).margin(1e-5));
    CHECK(region.area == Approx(100.f).margin(1e-4));
    // Sorted and unique, as documented.
    CHECK(std::is_sorted(region.facets.begin(), region.facets.end()));
    CHECK(std::adjacent_find(region.facets.begin(), region.facets.end()) == region.facets.end());

    // Smooth mode with the same tolerance must agree on a flat face - the extra
    // per-step test can only ever remove facets, never add them.
    p.mode = RegionMode::Smooth;
    const FaceRegion smooth = grow_face_region(its, topo, size_t(seed), p);
    CHECK(smooth.facets == region.facets);
}

TEST_CASE("MeshEdit: a region on a chamfered box grows to exactly the coplanar set", "[MeshEdit]")
{
    // 20 x 10 x 20 box with a 4 mm chamfer on the +X/+Z edge. Seven planar
    // faces; the top is 2 triangles, the chamfer flat is 2, and the +X wall is
    // 2 - and crucially the top must NOT leak across the 45 deg chamfer.
    const indexed_triangle_set its = make_chamfered_box(20.f, 10.f, 20.f, 4.f);
    REQUIRE(watertight(its));
    const MeshTopology topo = build_topology(its);
    REQUIRE(manifold(topo));

    RegionParams p;
    p.mode          = RegionMode::Planar;
    p.angle_tol_deg = 1.f;

    const int top = facet_on_face(its, Vec3f::UnitZ());
    REQUIRE(top >= 0);
    const FaceRegion top_region = grow_face_region(its, topo, size_t(top), p);
    // Top face only: 20 deep x 16 wide (the chamfer ate 4 mm of it).
    CHECK(top_region.facets.size() == 2);
    CHECK(top_region.area == Approx((20.f - 4.f) * 10.f).margin(1e-3));
    CHECK(top_region.normal.z() == Approx(1.f).margin(1e-5));

    // The chamfer flat: normal (1,0,1)/sqrt(2). A separate region of its own.
    const Vec3f chamfer_n = Vec3f(1.f, 0.f, 1.f).normalized();
    const int   cham      = facet_on_face(its, chamfer_n);
    REQUIRE(cham >= 0);
    const FaceRegion cham_region = grow_face_region(its, topo, size_t(cham), p);
    CHECK(cham_region.facets.size() == 2);
    // The two regions are disjoint - the whole point of the strict seed test.
    for (int f : cham_region.facets)
        CHECK(std::find(top_region.facets.begin(), top_region.facets.end(), f) == top_region.facets.end());

    // And the +X wall is a third region, again exactly 2 facets.
    const int px = facet_on_face(its, Vec3f::UnitX());
    REQUIRE(px >= 0);
    CHECK(grow_face_region(its, topo, size_t(px), p).facets.size() == 2);

    // Smooth mode crosses the chamfer where Planar will not - the documented
    // difference between the two modes - and the SEED cap is what decides how far
    // it gets. The angles from the seed (+Z) are: chamfer 45 deg, +X wall 90 deg.
    RegionParams smooth;
    smooth.mode           = RegionMode::Smooth;
    smooth.step_angle_deg = 50.f;

    // A 50 deg cap admits the chamfer (45) and refuses the +X wall (90): top +
    // chamfer = 4 facets. This is the cap doing exactly its job - stopping the
    // grow before it swallows the whole part - so it is asserted, not worked around.
    smooth.angle_tol_deg = 50.f;
    CHECK(grow_face_region(its, topo, size_t(top), smooth).facets.size() == 4);

    // Open the cap past 90 and the step past 45 and all three faces come in.
    smooth.angle_tol_deg  = 100.f;
    smooth.step_angle_deg = 50.f;
    const FaceRegion grown = grow_face_region(its, topo, size_t(top), smooth);
    CHECK(grown.facets.size() == 6);

    // And the per-step guard is independent of the cap: a wide cap with a step
    // too small to cross the 45 deg chamfer still stops at the top face.
    smooth.angle_tol_deg  = 100.f;
    smooth.step_angle_deg = 10.f;
    CHECK(grow_face_region(its, topo, size_t(top), smooth).facets.size() == 2);
}

// ----------------------------------------------------------------------------
// push / pull
// ----------------------------------------------------------------------------

TEST_CASE("MeshEdit: pushing a cube face by 5 mm adds area x 5 to the volume", "[MeshEdit]")
{
    const indexed_triangle_set its  = its_make_cube(10., 10., 10.);
    const MeshTopology         topo = build_topology(its);
    const int                  seed = facet_on_face(its, Vec3f::UnitZ());
    REQUIRE(seed >= 0);

    RegionParams rp;
    rp.mode = RegionMode::Planar;
    const FaceRegion region = grow_face_region(its, topo, size_t(seed), rp);
    REQUIRE(region.facets.size() == 2);
    REQUIRE(region.area == Approx(100.f).margin(1e-4));

    const float v0 = its_volume(its);

    TranslateParams tp;
    tp.d = 5.f;   // direction defaults to the region normal
    const TranslateResult res = translate_region(its, topo, region, tp);
    REQUIRE(res.status == TranslateStatus::Ok);

    // The headline assertion: volume up by exactly area * d.
    CHECK(double(its_volume(res.mesh)) == Approx(double(v0) + 100.0 * 5.0).epsilon(0).margin(1e-6 * 500.0));

    // Watertight and manifold after the move.
    CHECK(watertight(res.mesh));
    CHECK(manifold(build_topology(res.mesh)));

    // The painted-data contract, measured: no vertex or facet was added,
    // removed or renumbered. This is what lets the gizmo skip
    // clear_before_change_mesh().
    REQUIRE(res.mesh.vertices.size() == its.vertices.size());
    REQUIRE(res.mesh.indices.size() == its.indices.size());
    for (size_t i = 0; i < its.indices.size(); ++i)
        CHECK(res.mesh.indices[i] == its.indices[i]);

    // A cube's +Z face uses 4 vertices, and all 4 moved by exactly +5 z.
    CHECK(res.moved_vertices.size() == 4);
    for (uint32_t v : res.moved_vertices) {
        const Vec3f d = res.mesh.vertices[v] - its.vertices[v];
        CHECK(d.x() == Approx(0.f).margin(1e-6));
        CHECK(d.y() == Approx(0.f).margin(1e-6));
        CHECK(d.z() == Approx(5.f).margin(1e-6));
    }
    // And every other vertex is bit-identical.
    for (size_t v = 0; v < its.vertices.size(); ++v)
        if (std::find(res.moved_vertices.begin(), res.moved_vertices.end(), uint32_t(v)) == res.moved_vertices.end())
            CHECK(res.mesh.vertices[v] == its.vertices[v]);

    // The four walls stretched to follow, so their facets are dirty too: 2 top
    // facets plus 8 wall facets.
    CHECK(res.dirty_facets.size() == 10);
}

TEST_CASE("MeshEdit: pulling a cube face in removes the same volume, and d = 0 is the identity", "[MeshEdit]")
{
    const indexed_triangle_set its  = its_make_cube(10., 10., 10.);
    const MeshTopology         topo = build_topology(its);
    const int                  seed = facet_on_face(its, Vec3f::UnitZ());
    const FaceRegion           region = grow_face_region(its, topo, size_t(seed), RegionParams{});
    const float                v0   = its_volume(its);

    SECTION("negative d") {
        TranslateParams tp;
        tp.d = -5.f;
        const TranslateResult res = translate_region(its, topo, region, tp);
        REQUIRE(res.status == TranslateStatus::Ok);
        CHECK(double(its_volume(res.mesh)) == Approx(double(v0) - 500.0).margin(1e-6 * 500.0));
        CHECK(watertight(res.mesh));
    }

    SECTION("d = 0 leaves the mesh bit-identical") {
        TranslateParams tp;
        tp.d = 0.f;
        const TranslateResult res = translate_region(its, topo, region, tp);
        REQUIRE(res.status == TranslateStatus::NoOp);
        REQUIRE(res.mesh.vertices.size() == its.vertices.size());
        for (size_t v = 0; v < its.vertices.size(); ++v)
            CHECK(res.mesh.vertices[v] == its.vertices[v]);
        CHECK(res.moved_vertices.empty());
    }

    SECTION("the whole mesh is refused - that is the Move gizmo's job") {
        FaceRegion all;
        for (size_t f = 0; f < its.indices.size(); ++f)
            all.facets.push_back(int(f));
        all.normal = Vec3f::UnitZ();
        TranslateParams tp;
        tp.d = 1.f;
        CHECK(translate_region(its, topo, all, tp).status == TranslateStatus::WholeMesh);
    }
}

TEST_CASE("MeshEdit: pushing into a self-intersection is detected", "[MeshEdit]")
{
    // An L-prism, 20 x 20 with a 10 x 10 notch. The inner wall of the notch (at
    // x == 10, facing +X) is 10 mm from the outer +X wall, so a push of more than
    // 10 mm drives it clean through that wall: a real crossing of triangles, which
    // is what does_self_intersect() reads.
    //
    // Worth recording WHY this is not a cube. Pushing a cube's top face down
    // through its bottom leaves the four walls on exactly the same footprint, so
    // the solid merely INVERTS - no two triangles ever cross and CGAL correctly
    // reports no self-intersection. An inverted-but-not-crossing result is the
    // OutOfBounds pre-check's job, not this guard's.
    const indexed_triangle_set its  = make_L_prism(20.f, 10.f, 10.f);
    REQUIRE(watertight(its));
    const MeshTopology topo = build_topology(its);
    REQUIRE(manifold(topo));

    // The notch's inner wall: the +X face at x == 10, not the outer one at x == 20.
    int seed = -1;
    for (size_t i = 0; i < its.indices.size() && seed < 0; ++i) {
        const auto  &t = its.indices[i];
        const Vec3f &a = its.vertices[t[0]];
        const Vec3f &b = its.vertices[t[1]];
        const Vec3f &c = its.vertices[t[2]];
        const Vec3f  n = (b - a).cross(c - a).normalized();
        if (n.x() > 0.999f && std::abs(a.x() - 10.f) < 1e-4f)
            seed = int(i);
    }
    REQUIRE(seed >= 0);

    RegionParams rp;
    const FaceRegion region = grow_face_region(its, topo, size_t(seed), rp);
    REQUIRE(region.facets.size() == 2);
    REQUIRE(region.normal.x() == Approx(1.f).margin(1e-5));

    TranslateParams tp;
    tp.d                       = 15.f;   // past the outer wall at x == 20
    tp.check_self_intersection = true;

    const TranslateResult res = translate_region(its, topo, region, tp);
    CHECK(res.status == TranslateStatus::SelfIntersects);
    // A refusal hands back nothing to commit, so a caller that ignores the
    // status cannot accidentally install a broken mesh.
    CHECK(res.mesh.indices.empty());

    // The same move with the guard off does produce a mesh - and that mesh really
    // does self-intersect, so the test measures the guard rather than restating it.
    TranslateParams unguarded = tp;
    unguarded.check_self_intersection = false;
    const TranslateResult raw = translate_region(its, topo, region, unguarded);
    REQUIRE(raw.status == TranslateStatus::Ok);
    CHECK(self_intersects(raw.mesh));

    // A push that stays inside the notch is fine, and the guard says so.
    TranslateParams safe;
    safe.d = 5.f;
    const TranslateResult ok = translate_region(its, topo, region, safe);
    CHECK(ok.status == TranslateStatus::Ok);
    CHECK(watertight(ok.mesh));

    // And the cheap pre-check catches a move well past the bounding box without
    // ever reaching CGAL.
    TranslateParams far_out;
    far_out.d = 1000.f;
    CHECK(translate_region(its, topo, region, far_out).status == TranslateStatus::OutOfBounds);
}

// ----------------------------------------------------------------------------
// session: undo / redo and the snap step
// ----------------------------------------------------------------------------

TEST_CASE("MeshEdit: the session undo stack takes one entry per operation", "[MeshEdit]")
{
    const indexed_triangle_set its = its_make_cube(10., 10., 10.);
    EditSession session(its);

    const int        seed   = facet_on_face(its, Vec3f::UnitZ());
    const FaceRegion region = session.grow_region(size_t(seed), RegionParams{});
    REQUIRE(region.facets.size() == 2);

    const float v0 = its_volume(session.mesh());
    CHECK_FALSE(session.can_undo());

    TranslateParams tp;
    tp.d = 5.f;
    REQUIRE(session.apply_translate(region, tp).status == TranslateStatus::Ok);
    CHECK(session.undo_depth() == 1);
    CHECK(double(its_volume(session.mesh())) == Approx(double(v0) + 500.0).margin(1e-3));

    // A second push, on the region re-grown against the MOVED mesh.
    const FaceRegion region2 = session.grow_region(size_t(seed), RegionParams{});
    REQUIRE(session.apply_translate(region2, tp).status == TranslateStatus::Ok);
    CHECK(session.undo_depth() == 2);
    CHECK(double(its_volume(session.mesh())) == Approx(double(v0) + 1000.0).margin(1e-3));

    REQUIRE(session.undo());
    CHECK(double(its_volume(session.mesh())) == Approx(double(v0) + 500.0).margin(1e-3));
    REQUIRE(session.undo());
    CHECK(double(its_volume(session.mesh())) == Approx(double(v0)).margin(1e-3));
    CHECK_FALSE(session.can_undo());

    // Fully restored, vertex for vertex.
    REQUIRE(session.mesh().vertices.size() == its.vertices.size());
    for (size_t v = 0; v < its.vertices.size(); ++v)
        CHECK(session.mesh().vertices[v] == its.vertices[v]);

    REQUIRE(session.redo());
    CHECK(double(its_volume(session.mesh())) == Approx(double(v0) + 500.0).margin(1e-3));

    // A new operation ends the redo branch.
    const FaceRegion region3 = session.grow_region(size_t(seed), RegionParams{});
    REQUIRE(session.apply_translate(region3, tp).status == TranslateStatus::Ok);
    CHECK_FALSE(session.can_redo());

    // A refused operation pushes nothing.
    const size_t depth = session.undo_depth();
    TranslateParams bad;
    bad.d = -1000.f;
    CHECK(session.apply_translate(session.grow_region(size_t(seed), RegionParams{}), bad).status != TranslateStatus::Ok);
    CHECK(session.undo_depth() == depth);
}

TEST_CASE("MeshEdit: the snap step rounds to the nearest multiple", "[MeshEdit]")
{
    CHECK(snap_to_step(4.8f, 0.5f) == Approx(5.0f));
    CHECK(snap_to_step(4.7f, 1.0f) == Approx(5.0f));
    CHECK(snap_to_step(-4.8f, 0.5f) == Approx(-5.0f));
    // A non-positive step is the identity, so "no snapping" needs no branch in
    // the caller.
    CHECK(snap_to_step(4.8321f, 0.f) == Approx(4.8321f));
    CHECK(snap_to_step(4.8321f, -1.f) == Approx(4.8321f));
    // Typing the value and dragging to it must give the same mesh, which is
    // only true if both go through here.
    CHECK(snap_to_step(5.0f, 0.5f) == Approx(5.0f));
}
