#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

#include "libslic3r/QuadRemesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// ----------------------------------------------------------------------------
// Phase 2's quad remesher (QuadriFlow). Every test here needs the real dependency -
// unlike the Phase 1 voxel tests there is no sensible stub for a field-aligned quad
// remesher - so the whole file is behind SLIC3R_QUAD_REMESH except the pieces that
// must keep working in a build without it.
// ----------------------------------------------------------------------------

// Number of directed edges used exactly once. Zero means watertight: every edge is
// shared by exactly two faces with opposite orientation. Computed here rather than
// with its_num_open_edges() so the assertion is about the triangulated QUAD mesh's
// own topology, including the diagonals triangulate() introduces.
static size_t open_edges(const indexed_triangle_set &its)
{
    std::map<std::pair<int, int>, int> count;
    for (const Vec3i32 &f : its.indices)
        for (int e = 0; e < 3; ++e) {
            int a = f(e), b = f((e + 1) % 3);
            // Undirected key, but track the signed count so a pair of same-direction
            // edges (a non-manifold fold) does not look watertight.
            count[{std::min(a, b), std::max(a, b)}] += (a < b) ? 1 : -1;
        }
    size_t open = 0;
    for (const auto &kv : count)
        if (kv.second != 0)
            ++open;
    return open;
}

// Longest edge of a triangulated quad mesh: a crude but effective proxy for "did the
// remesh stay on the surface".
static float max_edge_length(const indexed_triangle_set &its)
{
    float m = 0.f;
    for (const Vec3i32 &f : its.indices)
        for (int e = 0; e < 3; ++e)
            m = std::max(m, (its.vertices[f((e + 1) % 3)] - its.vertices[f(e)]).norm());
    return m;
}

TEST_CASE("Quad remesh: the default target is half the triangle count", "[QuadRemesh]")
{
    // Pure arithmetic on the input - true with or without the dependency, which is
    // why this one test sits outside the #ifdef. The dialog prefills with it.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    REQUIRE(cube.indices.size() == 12);
    // 12 / 2 = 6, but the clamp floor is 20.
    REQUIRE(quad_remesh_default_target(cube) == QUAD_REMESH_TARGET_MIN);

    const indexed_triangle_set sphere = its_make_sphere(10., 2. * PI / 90.);
    REQUIRE(quad_remesh_default_target(sphere) == int(sphere.indices.size() / 2));
}

TEST_CASE("Quad remesh: a build without QuadriFlow refuses rather than crashes", "[QuadRemesh]")
{
    // quad_remesh_available() is the single thing the GUI keys off to hide the menu
    // entry. Whichever way this build went, calling quad_remesh() must be safe and
    // must report, never crash or silently return the input.
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    QuadRemeshReport rep;
    const QuadMesh   qm = quad_remesh(cube, QuadRemeshOptions{}, &rep);
    if (!quad_remesh_available()) {
        CHECK(qm.empty());
        CHECK(rep.status == QuadRemeshStatus::Unavailable);
        CHECK_FALSE(rep.note.empty());
    } else {
        CHECK(rep.status == QuadRemeshStatus::Ok);
    }
}

#ifdef SLIC3R_QUAD_REMESH

TEST_CASE("Quad remesh: a sphere at target 2000 gives ~2000 quads, all quads, watertight", "[QuadRemesh]")
{
    // The spec's headline acceptance test.
    const indexed_triangle_set sphere = its_make_sphere(10., 2. * PI / 90.);
    REQUIRE(sphere.indices.size() > 2000);

    QuadRemeshOptions opts;
    opts.target_faces   = 2000;
    opts.preserve_sharp = false;   // a sphere has no sharp features to preserve

    QuadRemeshReport rep;
    const QuadMesh   qm = quad_remesh(sphere, opts, &rep);

    REQUIRE(rep.status == QuadRemeshStatus::Ok);
    REQUIRE_FALSE(qm.empty());

    // 10% tolerance, as the spec asks: QuadriFlow hits a target approximately, since
    // the singularity placement is solved globally rather than by counting faces.
    CHECK(qm.quads.size() >= 1800);
    CHECK(qm.quads.size() <= 2200);

    // "All quads" - the struct only holds quads, so what this really checks is that
    // every quad is a real one: four distinct, in-range vertices, not a triangle
    // padded with a repeated index.
    for (const Vec4i32 &q : qm.quads) {
        std::set<int> distinct;
        for (int k = 0; k < 4; ++k) {
            CHECK(q(k) >= 0);
            CHECK(q(k) < int(qm.vertices.size()));
            distinct.insert(q(k));
        }
        CHECK(distinct.size() == 4);
    }

    // Watertight after triangulation.
    const indexed_triangle_set tri = qm.triangulate();
    CHECK(tri.indices.size() == qm.quads.size() * 2);
    CHECK(open_edges(tri) == 0);

    // And it is still a sphere of the right size - a remesh that collapsed or blew up
    // would pass every count-based check above.
    BoundingBoxf3 bb;
    for (const Vec3f &v : tri.vertices)
        bb.merge(v.cast<double>());
    CHECK(bb.size().x() == Approx(20.).margin(1.5));
    CHECK(bb.size().z() == Approx(20.).margin(1.5));
    for (const Vec3f &v : tri.vertices)
        CHECK(v.norm() == Approx(10.f).margin(1.f));
}

TEST_CASE("Quad remesh: a cube with preserve sharp keeps its twelve edges", "[QuadRemesh]")
{
    // its_make_cube() puts the cube in [0,20]^3.
    const double     side = 20.;
    indexed_triangle_set cube = its_make_cube(side, side, side);

    QuadRemeshOptions opts;
    opts.target_faces   = 600;
    opts.preserve_sharp = true;

    QuadRemeshReport rep;
    const QuadMesh   qm = quad_remesh(cube, opts, &rep);
    REQUIRE(rep.status == QuadRemeshStatus::Ok);
    REQUIRE_FALSE(qm.empty());

    const indexed_triangle_set tri = qm.triangulate();
    CHECK(open_edges(tri) == 0);

    // The cube's twelve edges are the twelve lines where two of {0, side} coordinates
    // are pinned. "Kept within tolerance" means: every one of them still has vertices
    // sitting on it, and no vertex has drifted off the cube's surface. Tolerance is
    // generous on purpose - QuadriFlow aligns the field to a sharp edge, it does not
    // promise a vertex exactly on it - but a cube whose edges were rounded away fails
    // both halves.
    const float tol = 0.6f;

    // (a) No vertex leaves the box, and every vertex is on one of the six faces.
    for (const Vec3f &v : tri.vertices) {
        for (int ax = 0; ax < 3; ++ax) {
            CHECK(v(ax) > -tol);
            CHECK(v(ax) < float(side) + tol);
        }
        const bool on_a_face =
            std::abs(v.x()) < tol || std::abs(v.x() - float(side)) < tol ||
            std::abs(v.y()) < tol || std::abs(v.y() - float(side)) < tol ||
            std::abs(v.z()) < tol || std::abs(v.z() - float(side)) < tol;
        CHECK(on_a_face);
    }

    // (b) Each of the twelve edges still has vertices on it. An edge is the line
    // where the two axes other than `along` are both pinned to 0 or side.
    int edges_found = 0;
    for (int along = 0; along < 3; ++along) {
        const int a1 = (along + 1) % 3, a2 = (along + 2) % 3;
        for (int c1 = 0; c1 < 2; ++c1)
            for (int c2 = 0; c2 < 2; ++c2) {
                const float p1 = c1 ? float(side) : 0.f;
                const float p2 = c2 ? float(side) : 0.f;
                const bool on_this_edge = std::any_of(
                    tri.vertices.begin(), tri.vertices.end(), [&](const Vec3f &v) {
                        return std::abs(v(a1) - p1) < tol && std::abs(v(a2) - p2) < tol;
                    });
                if (on_this_edge)
                    ++edges_found;
            }
    }
    CHECK(edges_found == 12);
}

TEST_CASE("Quad remesh: non-manifold input is refused cleanly", "[QuadRemesh]")
{
    SECTION("an open mesh") {
        // A cube with one facet removed: still a valid triangle soup, no longer closed.
        indexed_triangle_set open_cube = its_make_cube(10., 10., 10.);
        REQUIRE(open_cube.indices.size() > 1);
        open_cube.indices.pop_back();

        std::string why;
        CHECK_FALSE(quad_remesh_accepts(open_cube, &why));
        CHECK_FALSE(why.empty());

        QuadRemeshReport rep;
        const QuadMesh   qm = quad_remesh(open_cube, QuadRemeshOptions{}, &rep);
        // Refused, not attempted: no crash, no partial mesh, and a reason.
        CHECK(qm.empty());
        CHECK(rep.status == QuadRemeshStatus::NotManifold);
        CHECK_FALSE(rep.note.empty());
    }

    SECTION("two separate shells") {
        // Each shell is closed, but QuadriFlow treats the input as one surface.
        indexed_triangle_set two = its_make_cube(10., 10., 10.);
        const int            base = int(two.vertices.size());
        const indexed_triangle_set other = its_make_cube(10., 10., 10.);
        for (const Vec3f &v : other.vertices)
            two.vertices.emplace_back(v + Vec3f(50.f, 0.f, 0.f));
        for (const Vec3i32 &f : other.indices)
            two.indices.emplace_back(f + Vec3i32(base, base, base));

        std::string why;
        CHECK_FALSE(quad_remesh_accepts(two, &why));

        QuadRemeshReport rep;
        CHECK(quad_remesh(two, QuadRemeshOptions{}, &rep).empty());
        CHECK(rep.status == QuadRemeshStatus::NotManifold);
    }

    SECTION("an empty mesh") {
        QuadRemeshReport rep;
        CHECK(quad_remesh(indexed_triangle_set{}, QuadRemeshOptions{}, &rep).empty());
        CHECK(rep.status == QuadRemeshStatus::EmptyInput);
    }
}

TEST_CASE("Quad remesh: the same seed gives bit-identical output twice", "[QuadRemesh]")
{
    // The determinism gate. QuadriFlow seeds a PRNG and calls srand(), so without the
    // fixed seed and the serialisation in quad_remesh() a model quad-remeshed before
    // slicing would produce a different G-code every run.
    const indexed_triangle_set sphere = its_make_sphere(8., 2. * PI / 60.);

    QuadRemeshOptions opts;
    opts.target_faces   = 800;
    opts.preserve_sharp = true;
    opts.seed           = 12345;

    QuadRemeshReport rep_a, rep_b;
    const QuadMesh a = quad_remesh(sphere, opts, &rep_a);
    const QuadMesh b = quad_remesh(sphere, opts, &rep_b);

    REQUIRE(rep_a.status == QuadRemeshStatus::Ok);
    REQUIRE(rep_b.status == QuadRemeshStatus::Ok);
    REQUIRE_FALSE(a.empty());

    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.quads.size() == b.quads.size());
    // Bit-identical, not approximately equal: anything less and the determinism gate
    // would still fail downstream.
    for (size_t i = 0; i < a.vertices.size(); ++i)
        REQUIRE(a.vertices[i] == b.vertices[i]);
    for (size_t i = 0; i < a.quads.size(); ++i)
        REQUIRE(a.quads[i] == b.quads[i]);

    // The default seed must be just as reproducible - that is the path the GUI takes.
    QuadRemeshOptions def;
    def.target_faces = 800;
    const QuadMesh c = quad_remesh(sphere, def, nullptr);
    const QuadMesh d = quad_remesh(sphere, def, nullptr);
    REQUIRE(c.quads.size() == d.quads.size());
    for (size_t i = 0; i < c.vertices.size(); ++i)
        REQUIRE(c.vertices[i] == d.vertices[i]);
}

TEST_CASE("Quad remesh: triangulate() preserves the surface", "[QuadRemesh]")
{
    // triangulate() is what the volume actually stores, so it has to be more than
    // "two triangles per quad": it must share the diagonal's vertices, or the mesh
    // would look watertight by count and leak when sliced.
    const indexed_triangle_set sphere = its_make_sphere(10., 2. * PI / 90.);
    QuadRemeshOptions opts;
    opts.target_faces = 1000;

    QuadRemeshReport rep;
    const QuadMesh   qm = quad_remesh(sphere, opts, &rep);
    REQUIRE(rep.status == QuadRemeshStatus::Ok);

    const indexed_triangle_set tri = qm.triangulate();
    // Same vertex set - no duplication.
    CHECK(tri.vertices.size() == qm.vertices.size());
    CHECK(open_edges(tri) == 0);
    CHECK(its_num_open_edges(tri) == 0);
    // The report agrees with what came back.
    CHECK(rep.quads_after == qm.quads.size());
    CHECK(rep.triangles_after == tri.indices.size());
    // No degenerate spikes: at a ~1000-quad target on a radius-10 sphere the edges are
    // around 1 mm, so an edge many times that means a face bridged across the model.
    CHECK(max_edge_length(tri) < 5.f);
}

#endif // SLIC3R_QUAD_REMESH
