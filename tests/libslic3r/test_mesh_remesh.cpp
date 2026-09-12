#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

#include "libslic3r/MeshRemesh.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// ----------------------------------------------------------------------------
// A stand-in voxel remesher.
//
// The unit tests must not depend on OpenVDB being compiled in (OpenVDBUtils.cpp is
// conditional on the OpenVDB target), and more importantly they must be
// deterministic. This stub reproduces the two artefacts the real remesher has and
// which the code under test exists to undo:
//
//   * sub-voxel jitter on every vertex, in all three axes, so a flat face comes back
//     rippled, and
//   * a slight inward pull, so a sharp edge comes back rounded.
//
// It changes no topology, which is enough: neither post-pass cares how the remesh
// retriangulated, only where the vertices ended up.
// ----------------------------------------------------------------------------
static indexed_triangle_set jitter_remesher(const indexed_triangle_set &in, double voxel)
{
    indexed_triangle_set out = in;
    // A cheap deterministic hash of the vertex index, so the same input always gives
    // the same output and a failure is reproducible.
    for (size_t i = 0; i < out.vertices.size(); ++i) {
        auto h = [i](int axis) {
            uint32_t x = uint32_t(i) * 2654435761u + uint32_t(axis) * 40503u;
            x ^= x >> 13;
            x *= 1274126177u;
            x ^= x >> 16;
            // [-0.4, 0.4] of a voxel: big enough to break a 1e-4 flatness assertion,
            // small enough to stay inside the one-voxel snap radius.
            return (float(x % 10000u) / 10000.f - 0.5f) * 0.8f;
        };
        out.vertices[i] += Vec3f(h(0), h(1), h(2)) * float(voxel);
    }
    return out;
}

// A remesher that does nothing at all, for the "unchanged behaviour" check.
static indexed_triangle_set identity_remesher(const indexed_triangle_set &in, double) { return in; }

// ----------------------------------------------------------------------------
// Fixtures
// ----------------------------------------------------------------------------

// A box with its four vertical edges chamfered, sitting on z = 0: a flat square-ish
// base, vertical-ish walls, a flat top. The chamfer gives the base outline more than
// four corners so the area check is not trivially satisfied by a square.
static indexed_triangle_set make_chamfered_box(float w, float d, float h, float chamfer)
{
    // Base outline, counter-clockwise seen from +Z, corners cut by `chamfer`.
    const std::vector<Vec2f> outline = {
        {chamfer, 0.f},         {w - chamfer, 0.f},
        {w, chamfer},           {w, d - chamfer},
        {w - chamfer, d},       {chamfer, d},
        {0.f, d - chamfer},     {0.f, chamfer},
    };
    const int n = int(outline.size());

    indexed_triangle_set its;
    // bottom ring, top ring, then a centre vertex for each cap.
    for (const Vec2f &p : outline) its.vertices.emplace_back(p.x(), p.y(), 0.f);
    for (const Vec2f &p : outline) its.vertices.emplace_back(p.x(), p.y(), h);
    const int c_bot = int(its.vertices.size());
    its.vertices.emplace_back(w * 0.5f, d * 0.5f, 0.f);
    const int c_top = int(its.vertices.size());
    its.vertices.emplace_back(w * 0.5f, d * 0.5f, h);

    for (int i = 0; i < n; ++i) {
        const int j = (i + 1) % n;
        // wall quad, outward facing
        its.indices.emplace_back(i, j, n + j);
        its.indices.emplace_back(i, n + j, n + i);
        // bottom fan, facing -Z
        its.indices.emplace_back(c_bot, j, i);
        // top fan, facing +Z
        its.indices.emplace_back(c_top, n + i, n + j);
    }
    return its;
}

// Shoelace area of the base outline of `its`: the xy area of every facet within
// `eps` of z_min that faces the bed. Same quantity remesh_bottom_area() reports, but
// written out here so the test is not checking the implementation against itself for
// the ORIGINAL mesh - both are simple enough that agreement is meaningful.
static double base_area_xy(const indexed_triangle_set &its, double eps)
{
    float z_min = std::numeric_limits<float>::max();
    for (const Vec3f &v : its.vertices) z_min = std::min(z_min, v.z());
    double area = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3f &a = its.vertices[f(0)], &b = its.vertices[f(1)], &c = its.vertices[f(2)];
        if (a.z() > z_min + eps || b.z() > z_min + eps || c.z() > z_min + eps) continue;
        const double cross = double(b.x() - a.x()) * double(c.y() - a.y()) -
                             double(c.x() - a.x()) * double(b.y() - a.y());
        // Facets pointing at the bed wind clockwise in xy, so their shoelace cross is
        // negative; take the downward-facing ones only, as the footprint.
        if (cross >= 0.) continue;
        area += -0.5 * cross;
    }
    return area;
}

static float mesh_z_min(const indexed_triangle_set &its)
{
    float z = std::numeric_limits<float>::max();
    for (const Vec3f &v : its.vertices) z = std::min(z, v.z());
    return z;
}

// ----------------------------------------------------------------------------

TEST_CASE("Remesh: keep the bottom flat leaves the base exact", "[MeshRemesh]")
{
    const indexed_triangle_set box = make_chamfered_box(20.f, 14.f, 10.f, 3.f);
    const double voxel = 0.2;

    RemeshOptions opts;
    opts.voxel_size          = voxel;
    opts.keep_bottom_flat    = true;
    opts.preserve_sharp_edges = false; // isolated below
    RemeshReport rep;

    const indexed_triangle_set out =
        remesh_with_options(box, opts, jitter_remesher, Vec3d(0., 0., -1.), &rep);

    REQUIRE_FALSE(out.indices.empty());
    INFO("note: " << rep.note);
    REQUIRE(rep.kept_bottom_flat);
    REQUIRE_FALSE(rep.fell_back);

    // Every vertex that reaches the base is exactly on it. The slab never went
    // through the remesher, so this is an equality check, not a tolerance one - 1e-4
    // only allows for the boolean's own float round-trip.
    const float z0 = mesh_z_min(out);
    CHECK(z0 == Approx(0.).margin(1e-4));

    size_t base_vertices = 0;
    for (const Vec3f &v : out.vertices) {
        // "at the base" = within a fraction of a voxel; anything in that band must be
        // AT z_min, which is what the jitter would have broken.
        if (v.z() < z0 + float(voxel) * 0.5f) {
            CHECK(std::abs(v.z() - z0) < 1e-4f);
            ++base_vertices;
        }
    }
    CHECK(base_vertices >= 8); // the chamfered outline has eight corners

    // The footprint is the original's, within 1%.
    const double a_before = base_area_xy(box, 1e-4);
    const double a_after  = base_area_xy(out, 1e-4);
    REQUIRE(a_before > 0.);
    REQUIRE(a_after > 0.);
    CHECK(std::abs(a_after - a_before) / a_before < 0.01);
}

TEST_CASE("Remesh: with the option off, nothing but today's remesh runs", "[MeshRemesh]")
{
    // A sphere has no flat bottom and the option is off anyway: the result must be
    // exactly what the bare remesher returns, vertex for vertex. This is the
    // regression guard that the new default-on control is additive.
    const indexed_triangle_set sphere = its_make_sphere(8., 2. * PI / 60.);

    RemeshOptions opts;
    opts.voxel_size           = 0.15;
    opts.keep_bottom_flat     = false;
    opts.preserve_sharp_edges = false;

    const indexed_triangle_set out =
        remesh_with_options(sphere, opts, jitter_remesher, Vec3d(0., 0., -1.), nullptr);
    const indexed_triangle_set ref = jitter_remesher(sphere, 0.15);

    REQUIRE(out.vertices.size() == ref.vertices.size());
    REQUIRE(out.indices.size() == ref.indices.size());
    for (size_t i = 0; i < out.vertices.size(); ++i)
        REQUIRE(out.vertices[i] == ref.vertices[i]);
}

TEST_CASE("Remesh: a mesh with no flat bottom takes the whole-mesh path", "[MeshRemesh]")
{
    // Same sphere, but now WITH the option on. There is no flat bottom to protect,
    // so the flat-bottom path must decline and the plain remesh must run - the
    // result is again identical to the bare remesher's.
    const indexed_triangle_set sphere = its_make_sphere(8., 2. * PI / 60.);

    RemeshOptions opts;
    opts.voxel_size           = 0.15;
    opts.keep_bottom_flat     = true;
    opts.preserve_sharp_edges = false;
    RemeshReport rep;

    const indexed_triangle_set out =
        remesh_with_options(sphere, opts, jitter_remesher, Vec3d(0., 0., -1.), &rep);

    CHECK(rep.fell_back);
    CHECK_FALSE(rep.kept_bottom_flat);
    CHECK(rep.note == "no flat bottom");

    const indexed_triangle_set ref = jitter_remesher(sphere, 0.15);
    REQUIRE(out.vertices.size() == ref.vertices.size());
    for (size_t i = 0; i < out.vertices.size(); ++i)
        REQUIRE(out.vertices[i] == ref.vertices[i]);

    // And the direct entry point reports the same reason.
    std::string why;
    const indexed_triangle_set none =
        remesh_keep_flat_bottom(sphere, 0.15, 0., jitter_remesher, Vec3d(0., 0., -1.), &why);
    CHECK(none.indices.empty());
    CHECK(why == "no flat bottom");
}

TEST_CASE("Remesh: the sharp-edge snap keeps a cube's 12 edges on their lines", "[MeshRemesh]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    const double voxel = 0.3;

    // A cube's only sharp edges are its 12 box edges (90 degrees) - its_make_cube
    // emits each corner once, so the detection should find exactly those and no
    // diagonals (a face's two triangles are coplanar, dihedral 0).
    const std::vector<SharpEdge> edges = detect_sharp_edges(cube, 45.);
    REQUIRE(edges.size() == 12);

    indexed_triangle_set jittered = jitter_remesher(cube, voxel);
    // Sanity: the jitter really did move things off the edges, or the test proves
    // nothing.
    bool any_off = false;
    for (const Vec3f &v : jittered.vertices)
        if ((v - cube.vertices[0]).norm() > 0.05f * float(voxel)) { any_off = true; break; }
    REQUIRE(any_off);

    const size_t moved = snap_to_sharp_edges(jittered, edges, voxel);
    CHECK(moved > 0);

    // Every cube vertex is a corner where three sharp edges meet, so after the snap
    // each one must be back within 0.2 voxel of its original line.
    auto dist_to_nearest_edge = [&edges](const Vec3f &p) {
        float best = std::numeric_limits<float>::max();
        for (const SharpEdge &e : edges) {
            const Vec3f ab = e.b - e.a;
            const float d2 = ab.squaredNorm();
            float t = d2 < 1e-18f ? 0.f : std::clamp((p - e.a).dot(ab) / d2, 0.f, 1.f);
            best = std::min(best, (Vec3f(e.a + t * ab) - p).norm());
        }
        return best;
    };
    for (const Vec3f &v : jittered.vertices)
        CHECK(dist_to_nearest_edge(v) < 0.2f * float(voxel));
}

TEST_CASE("Remesh: the shared sharp detection agrees with Sculpt's vertex test", "[MeshRemesh]")
{
    const indexed_triangle_set cube = its_make_cube(10., 10., 10.);
    // Every vertex of a cube is a corner of three 90-degree edges, so the per-vertex
    // mask - the same pairwise-incident-face test SculptSession runs - is all true.
    const std::vector<bool> mask = sharp_vertex_mask(cube, 45.);
    REQUIRE(mask.size() == cube.vertices.size());
    for (bool m : mask) CHECK(m);

    // A sphere at a 45-degree threshold has no dihedral that steep anywhere.
    const indexed_triangle_set sphere = its_make_sphere(8., 2. * PI / 90.);
    const std::vector<bool> smask = sharp_vertex_mask(sphere, 45.);
    size_t sharp = 0;
    for (bool m : smask) if (m) ++sharp;
    CHECK(sharp == 0);
}

TEST_CASE("Remesh: auto voxel size is unchanged from the old inline value", "[MeshRemesh]")
{
    const indexed_triangle_set cube = its_make_cube(30., 30., 30.);
    BoundingBoxf3 bb;
    for (const Vec3f &v : cube.vertices) bb.merge(v.cast<double>());
    const double expected = std::clamp(bb.size().norm() / 300., 0.05, 0.3);
    CHECK(remesh_auto_voxel_size(cube) == Approx(expected));
    // Clamped at both ends.
    CHECK(remesh_auto_voxel_size(its_make_cube(1., 1., 1.)) == Approx(0.05));
    CHECK(remesh_auto_voxel_size(its_make_cube(400., 400., 400.)) == Approx(0.3));
}

TEST_CASE("Remesh: an identity remesher round-trips the flat-bottom union", "[MeshRemesh]")
{
    // With a remesher that changes nothing, cut + union must give back the same solid
    // - a guard that the slab extension and the boolean are not eroding the model.
    const indexed_triangle_set box = make_chamfered_box(20.f, 14.f, 10.f, 3.f);
    std::string why;
    const indexed_triangle_set out =
        remesh_keep_flat_bottom(box, 0.2, 0., identity_remesher, Vec3d(0., 0., -1.), &why);
    INFO("why: " << why);
    REQUIRE_FALSE(out.indices.empty());

    float z_min = std::numeric_limits<float>::max(), z_max = std::numeric_limits<float>::lowest();
    for (const Vec3f &v : out.vertices) { z_min = std::min(z_min, v.z()); z_max = std::max(z_max, v.z()); }
    CHECK(z_min == Approx(0.).margin(1e-4));
    CHECK(z_max == Approx(10.).margin(1e-4));
    CHECK(base_area_xy(out, 1e-4) == Approx(base_area_xy(box, 1e-4)).epsilon(0.01));
}
