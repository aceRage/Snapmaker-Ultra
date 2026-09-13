#include <catch2/catch.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <vector>

#include "libslic3r/MeshRound.hpp"
#include "libslic3r/TriangleMesh.hpp"

using namespace Slic3r;

// ----------------------------------------------------------------------------
// A stand-in rounder.
//
// These tests must not depend on OpenVDB being compiled in (OpenVDBUtils.cpp is
// conditional on the OpenVDB target) and must be deterministic, so the rounder is
// injected the way MeshRemesh's tests inject a remesher.
//
// This one is not a fake: it builds a real, analytically exact rounded box - the
// Minkowski sum of a shrunk box with a ball of radius r, tessellated as a sphere
// whose eight octants are pulled apart to the eight corners. That is precisely
// the surface the OpenVDB round trip approximates, so every assertion below
// (volume against the closed form, no dihedral above the feature angle,
// watertight) is checking the SAME properties on the stub that the real path has
// to satisfy - and the real path is exercised by the click-tests in the spec.
//
// It ignores the input mesh's shape beyond its bounding box, which is why the
// tests that use it feed it boxes.
// ----------------------------------------------------------------------------

// A UV sphere of radius r, centred at the origin, with `bands` rings of latitude
// and `sectors` of longitude - but with each vertex displaced by `half`, signed
// per octant, so the sphere is split into eight octants sitting at the corners of
// a box of half-extent `half`. Stitching the octants back together with the
// quarter-cylinders and the flat faces is what makes the rounded box.
//
// `sectors` must be a multiple of 4 and `bands` even, so the seams fall exactly on
// the octant boundaries and no facet straddles two octants.
// The default density is deliberately high. The quantity these tests measure is
// the volume LOST to the rounding, which is a small difference of two large
// numbers - a 0.6% shortfall in the inscribed arc area (what 16 x 32 gives)
// becomes a 33% error in the loss. At 64 x 128 the arc is within 0.04% and the
// loss lands inside the 10% the brief asks for, which is what lets the headline
// test assert against the closed form at all.
static indexed_triangle_set make_rounded_box(const Vec3d &size, double r, int bands = 64, int sectors = 128)
{
    REQUIRE(sectors % 4 == 0);
    REQUIRE(bands % 2 == 0);
    REQUIRE(size.x() > 2. * r);
    REQUIRE(size.y() > 2. * r);
    REQUIRE(size.z() > 2. * r);

    // Half-extent of the CORE box the ball rolls over.
    const Vec3d half(size.x() * 0.5 - r, size.y() * 0.5 - r, size.z() * 0.5 - r);

    indexed_triangle_set its;

    // Vertex grid: (bands + 1) rings by sectors columns, poles included as full
    // rings so the offset stays well defined there (a pole would otherwise have to
    // pick one octant arbitrarily).
    auto vid = [sectors](int i, int j) { return i * sectors + (j % sectors); };

    for (int i = 0; i <= bands; ++i) {
        // phi from 0 (north pole) to pi (south).
        const double phi = M_PI * double(i) / double(bands);
        const double sp = std::sin(phi), cp = std::cos(phi);
        for (int j = 0; j < sectors; ++j) {
            const double th = 2. * M_PI * double(j) / double(sectors);
            const double st = std::sin(th), ct = std::cos(th);
            const Vec3d  n(sp * ct, sp * st, cp);
            // The octant offset: the sign of each component of the normal says
            // which corner this piece of sphere belongs to. On a seam the
            // component is 0 and the offset is 0, which is exactly what puts the
            // seam ring on the flat face / cylinder boundary.
            auto sgn = [](double v) { return v > 1e-12 ? 1. : (v < -1e-12 ? -1. : 0.); };
            const Vec3d off(sgn(n.x()) * half.x(), sgn(n.y()) * half.y(), sgn(n.z()) * half.z());
            const Vec3d p = n * r + off;
            its.vertices.emplace_back(p.cast<float>());
        }
    }

    for (int i = 0; i < bands; ++i) {
        for (int j = 0; j < sectors; ++j) {
            const int a = vid(i, j), b = vid(i, j + 1);
            const int c = vid(i + 1, j + 1), d = vid(i + 1, j);
            its.indices.emplace_back(a, d, c);
            its.indices.emplace_back(a, c, b);
        }
    }

    // The construction duplicates positions in three places: both poles are whole
    // rings of coincident vertices, the longitude seam repeats column 0, and every
    // octant boundary is shared by the two octants either side of it. Those are the
    // same POINT with different indices, so the mesh is geometrically closed but
    // topologically full of holes - and is_watertight_manifold() counts indices.
    //
    // Merging coincident vertices first welds all three, and only THEN can the
    // now-genuinely-degenerate facets (the ones whose three corners collapsed onto
    // one welded vertex) be dropped by index without opening anything.
    // its_merge_vertices welds by exact equality, and the two sides of a seam are
    // computed by different trigonometry (sin/cos of angles that are equal only in
    // exact arithmetic), so they land a few ULPs apart and would not weld. Snapping
    // to a grid far finer than any feature here, but far coarser than that drift,
    // makes the weld exact without moving anything that matters.
    for (Vec3f &v : its.vertices)
        for (int k = 0; k < 3; ++k)
            v[k] = std::round(v[k] * 1e5f) / 1e5f;
    its_merge_vertices(its);
    its_remove_degenerate_faces(its);
    its_compactify_vertices(its);
    return its;
}

// Bounding box of a mesh.
static void mesh_bbox(const indexed_triangle_set &its, Vec3d &lo, Vec3d &hi)
{
    lo = Vec3d(std::numeric_limits<double>::max(), std::numeric_limits<double>::max(), std::numeric_limits<double>::max());
    hi = -lo;
    for (const Vec3f &v : its.vertices) {
        lo = lo.cwiseMin(v.cast<double>());
        hi = hi.cwiseMax(v.cast<double>());
    }
}

// The stub rounder: replace whatever came in with the exact rounded box of its
// bounding box, re-centred on the input's own centre.
static indexed_triangle_set analytic_rounder(const indexed_triangle_set &in, double radius, double, bool)
{
    if (in.indices.empty())
        return {};
    Vec3d lo, hi;
    mesh_bbox(in, lo, hi);
    const Vec3d size = hi - lo;
    if (size.x() <= 2. * radius || size.y() <= 2. * radius || size.z() <= 2. * radius)
        return {};
    indexed_triangle_set out = make_rounded_box(size, radius);
    const Vec3d centre = (lo + hi) * 0.5;
    for (Vec3f &v : out.vertices)
        v += centre.cast<float>();
    return out;
}

// Signed volume by the divergence theorem. Positive for an outward-wound closed
// mesh.
static double mesh_volume(const indexed_triangle_set &its)
{
    double v = 0.;
    for (const Vec3i32 &f : its.indices) {
        const Vec3d a = its.vertices[f[0]].cast<double>();
        const Vec3d b = its.vertices[f[1]].cast<double>();
        const Vec3d c = its.vertices[f[2]].cast<double>();
        v += a.dot(b.cross(c));
    }
    return v / 6.;
}

static indexed_triangle_set make_box(const Vec3d &size)
{
    return its_make_cube(size.x(), size.y(), size.z());
}

// ----------------------------------------------------------------------------
// The closed form
// ----------------------------------------------------------------------------

TEST_CASE("MeshRound: the rounded-box volume formula agrees with a tessellation", "[MeshRound]")
{
    const Vec3d  size(20., 20., 20.);
    const double r = 2.;

    const double analytic = rounded_box_volume(size, r);
    // A rounded 20 mm cube at r = 2 is a little under the full 8000.
    CHECK(analytic < 8000.);
    CHECK(analytic > 7800.);

    // The tessellation INSCRIBES the smooth solid - every facet is a chord of the
    // surface it approximates - so it always comes out UNDER the closed form, and
    // refining it closes the gap from below. Both halves of that are asserted:
    // being under is the structural property, and shrinking as the tessellation
    // refines is what shows the two are converging on the same solid rather than
    // merely being close by luck.
    const double coarse = mesh_volume(make_rounded_box(size, r, 16, 32));
    const double fine   = mesh_volume(make_rounded_box(size, r, 32, 64));
    CHECK(coarse < analytic);
    CHECK(fine < analytic);
    CHECK(fine > coarse);
    CHECK(fine == Approx(analytic).epsilon(0.01));

    // 2r must fit, or the formula has nothing to say.
    CHECK(rounded_box_volume(Vec3d(3., 20., 20.), 2.) == Approx(0.));
    CHECK(rounded_box_volume_loss(Vec3d(3., 20., 20.), 2.) == Approx(0.));

    // r = 0 is the box itself.
    CHECK(rounded_box_volume(size, 0.) == Approx(8000.));
}

TEST_CASE("MeshRound: the auto voxel size tracks the radius and stays clamped", "[MeshRound]")
{
    CHECK(round_auto_voxel_size(6.) == Approx(1.));
    CHECK(round_auto_voxel_size(0.6) == Approx(0.1));
    // Clamped at both ends.
    CHECK(round_auto_voxel_size(0.001) == Approx(ROUND_VOXEL_MIN));
    CHECK(round_auto_voxel_size(1000.) == Approx(ROUND_VOXEL_MAX));
    CHECK(round_auto_voxel_size(-1.) == Approx(ROUND_VOXEL_MIN));
}

// ----------------------------------------------------------------------------
// The headline test: a 20 mm cube rounded at r = 2
// ----------------------------------------------------------------------------

TEST_CASE("MeshRound: a 20 mm cube at r = 2 loses close to the analytic volume", "[MeshRound]")
{
    const Vec3d size(20., 20., 20.);
    const double r = 2.;
    const indexed_triangle_set cube = make_box(size);
    REQUIRE(mesh_volume(cube) == Approx(8000.).epsilon(1e-6));

    RoundOptions opts;
    opts.radius           = r;
    opts.round_concave    = true;
    opts.keep_bottom_flat = false; // isolated in its own test below
    RoundReport rep;

    const indexed_triangle_set out =
        round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    REQUIRE_FALSE(out.indices.empty());
    CHECK(rep.radius_used == Approx(r));
    CHECK(rep.triangles_before == cube.indices.size());
    CHECK(rep.triangles_after == out.indices.size());

    // (a) the volume drops by close to the analytic amount for a rounded box.
    const double expected_loss = rounded_box_volume_loss(size, r);
    const double actual_loss   = 8000. - mesh_volume(out);
    INFO("expected loss " << expected_loss << ", actual " << actual_loss);
    REQUIRE(expected_loss > 0.);
    CHECK(std::abs(actual_loss - expected_loss) / expected_loss < 0.10);

    // (b) no face remains sharper than the feature angle. Every edge of the
    // original cube was 90 deg; after the round the worst dihedral is the
    // tessellation step, which for 16 bands is well under 30.
    const double worst = max_dihedral_deg(out);
    INFO("worst dihedral " << worst << " deg");
    CHECK(worst < 30.);

    // (c) watertight.
    CHECK(is_watertight_manifold(out));

    // The rounding is inward: the part never grows past its original envelope.
    Vec3d lo, hi;
    mesh_bbox(out, lo, hi);
    CHECK((hi - lo).x() == Approx(20.).margin(1e-3));
    CHECK((hi - lo).z() == Approx(20.).margin(1e-3));
}

TEST_CASE("MeshRound: a bigger radius removes more material, monotonically", "[MeshRound]")
{
    const Vec3d size(20., 20., 20.);
    const indexed_triangle_set cube = make_box(size);

    double previous = 8000.;
    for (double r : {0.5, 1., 2., 4.}) {
        RoundOptions opts;
        opts.radius           = r;
        opts.keep_bottom_flat = false;
        const indexed_triangle_set out =
            round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), nullptr);
        REQUIRE_FALSE(out.indices.empty());
        const double v = mesh_volume(out);
        INFO("r = " << r << " volume " << v);
        CHECK(v < previous);
        CHECK(v == Approx(rounded_box_volume(size, r)).epsilon(0.02));
        previous = v;
    }
}

// ----------------------------------------------------------------------------
// Keep the bottom flat
// ----------------------------------------------------------------------------

TEST_CASE("MeshRound: a bottom slab leaves the base planar", "[MeshRound]")
{
    const Vec3d size(20., 20., 20.);
    const double r = 2.;
    const indexed_triangle_set cube = make_box(size); // min corner at the origin

    RoundOptions opts;
    opts.radius           = r;
    opts.keep_bottom_flat = true;
    // The SLAB mode, explicitly: since the owner's 2026-09-12 feedback a margin of 0
    // is the default and selects the mirror instead, so the slab path has to ask.
    opts.bottom_margin    = ROUND_BOTTOM_MARGIN_RADII * r;
    RoundReport rep;

    const indexed_triangle_set out =
        round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    REQUIRE_FALSE(out.indices.empty());
    INFO("note: " << rep.note);
    REQUIRE(rep.kept_bottom_flat);
    REQUIRE_FALSE(rep.fell_back);
    CHECK_FALSE(rep.mirrored_base);
    CHECK(rep.bottom_margin_used == Approx(ROUND_BOTTOM_MARGIN_RADII * r));

    // The base is EXACTLY planar and exactly where it was: the slab never entered
    // the rounder, so 1e-4 only allows for the boolean's float round-trip.
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &v : out.vertices)
        z_min = std::min(z_min, double(v.z()));
    CHECK(z_min == Approx(0.).margin(1e-4));

    // Every vertex in the base band sits exactly on the base plane - which is what
    // a fillet running round the bottom edge would have broken.
    size_t base_vertices = 0;
    for (const Vec3f &v : out.vertices) {
        if (double(v.z()) < z_min + 0.25 * r) {
            CHECK(std::abs(double(v.z()) - z_min) < 1e-4);
            ++base_vertices;
        }
    }
    CHECK(base_vertices >= 4);

    // The footprint is still the full 20 x 20: the base outline was not eroded.
    double x_lo = 1e30, x_hi = -1e30, y_lo = 1e30, y_hi = -1e30;
    for (const Vec3f &v : out.vertices) {
        if (double(v.z()) >= z_min + 1e-4)
            continue;
        x_lo = std::min(x_lo, double(v.x())); x_hi = std::max(x_hi, double(v.x()));
        y_lo = std::min(y_lo, double(v.y())); y_hi = std::max(y_hi, double(v.y()));
    }
    CHECK(x_hi - x_lo == Approx(20.).margin(1e-3));
    CHECK(y_hi - y_lo == Approx(20.).margin(1e-3));
}

TEST_CASE("MeshRound: with keep-bottom-flat off the base is rounded too", "[MeshRound]")
{
    const indexed_triangle_set cube = make_box(Vec3d(20., 20., 20.));

    RoundOptions opts;
    opts.radius           = 2.;
    opts.keep_bottom_flat = false;
    RoundReport rep;

    const indexed_triangle_set out =
        round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    REQUIRE_FALSE(out.indices.empty());
    CHECK_FALSE(rep.kept_bottom_flat);
    CHECK_FALSE(rep.fell_back);

    // The bottom edge is filleted, so the vertices just above the base are pulled
    // inward - the footprint at the very bottom is smaller than 20.
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &v : out.vertices)
        z_min = std::min(z_min, double(v.z()));
    double x_lo = 1e30, x_hi = -1e30;
    for (const Vec3f &v : out.vertices) {
        if (double(v.z()) >= z_min + 1e-3)
            continue;
        x_lo = std::min(x_lo, double(v.x())); x_hi = std::max(x_hi, double(v.x()));
    }
    CHECK(x_hi - x_lo < 19.);
}

TEST_CASE("MeshRound: a part with no flat bottom falls back to the whole-mesh round", "[MeshRound]")
{
    // Nothing to protect: the flat-bottom path must decline, say why, and the plain
    // round must still run.
    //
    // NOT a sphere, which is the obvious choice and the wrong one: a UV sphere's
    // bottom pole is a fan of facets that all face the bed and all sit within a
    // fraction of a voxel of z_min, so remesh_bottom_area() legitimately reports a
    // small flat bottom and the split is taken. That was measured, not assumed -
    // the first version of this test used a sphere and failed for exactly that
    // reason.
    //
    // A cube standing on one CORNER has no facet facing the bed at its lowest
    // point at all, which is what "no flat bottom" actually means here.
    indexed_triangle_set tipped = its_make_cube(10., 10., 10.);
    {
        // Rotate so the body diagonal (1,1,1) points straight down.
        const Vec3d diag = Vec3d(1., 1., 1.).normalized();
        const Eigen::Quaterniond q = Eigen::Quaterniond::FromTwoVectors(diag, Vec3d(0., 0., -1.));
        for (Vec3f &v : tipped.vertices)
            v = (q * v.cast<double>()).cast<float>();
    }

    // Both mechanisms have to decline it, not just the one that used to run.
    for (double margin : {0., 2.}) {
        RoundOptions opts;
        opts.radius           = 1.;
        opts.keep_bottom_flat = true;
        opts.bottom_margin    = margin;
        RoundReport rep;

        const indexed_triangle_set out =
            round_with_options(tipped, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
        INFO("margin " << margin << ", note: " << rep.note);
        REQUIRE_FALSE(out.indices.empty());
        CHECK(rep.fell_back);
        CHECK_FALSE(rep.kept_bottom_flat);
        CHECK_FALSE(rep.mirrored_base);
        CHECK_FALSE(rep.note.empty());
    }
}

// ----------------------------------------------------------------------------
// Contracts
// ----------------------------------------------------------------------------

TEST_CASE("MeshRound: the radius and voxel size are clamped, and the voxel follows the radius", "[MeshRound]")
{
    const indexed_triangle_set cube = make_box(Vec3d(20., 20., 20.));

    // A radius past the maximum is clamped, not refused.
    RoundOptions big;
    big.radius           = 1e6;
    big.keep_bottom_flat = false;
    RoundReport rep;
    round_with_options(cube, big, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    CHECK(rep.radius_used == Approx(ROUND_RADIUS_MAX));

    // A voxel size coarser than half the radius cannot represent the fillet, so it
    // is pulled down to r/2 rather than producing a garbage surface.
    RoundOptions coarse;
    coarse.radius           = 1.;
    coarse.voxel_size       = 4.;
    coarse.keep_bottom_flat = false;
    round_with_options(cube, coarse, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    CHECK(rep.voxel_used == Approx(0.5));

    // Left at 0 the voxel size is derived from the radius.
    RoundOptions autov;
    autov.radius           = 3.;
    autov.voxel_size       = 0.;
    autov.keep_bottom_flat = false;
    round_with_options(cube, autov, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    CHECK(rep.voxel_used == Approx(round_auto_voxel_size(3.)));
}

TEST_CASE("MeshRound: an empty mesh or a missing rounder gives an empty result", "[MeshRound]")
{
    RoundOptions opts;
    opts.radius = 1.;

    CHECK(round_with_options({}, opts, analytic_rounder, Vec3d(0., 0., -1.), nullptr).indices.empty());
    CHECK(round_with_options(make_box(Vec3d(20., 20., 20.)), opts, VoxelRounder{}, Vec3d(0., 0., -1.), nullptr)
              .indices.empty());
    // A rounder that fails is reported as a failure, not as a silent identity.
    const VoxelRounder failing = [](const indexed_triangle_set &, double, double, bool) {
        return indexed_triangle_set{};
    };
    opts.keep_bottom_flat = false;
    CHECK(round_with_options(make_box(Vec3d(20., 20., 20.)), opts, failing, Vec3d(0., 0., -1.), nullptr)
              .indices.empty());
}

TEST_CASE("MeshRound: the measurement helpers agree with hand-checked meshes", "[MeshRound]")
{
    // A cube's worst dihedral is 90 deg, and it is watertight.
    const indexed_triangle_set cube = make_box(Vec3d(10., 10., 10.));
    CHECK(max_dihedral_deg(cube) == Approx(90.).margin(1e-3));
    CHECK(is_watertight_manifold(cube));

    // A sphere's is small and falls with the tessellation.
    const indexed_triangle_set sphere = its_make_sphere(5., 2. * PI / 60.);
    CHECK(max_dihedral_deg(sphere) < 20.);
    CHECK(is_watertight_manifold(sphere));

    // An open mesh is not watertight: drop a facet from the cube.
    indexed_triangle_set open_cube = cube;
    open_cube.indices.pop_back();
    CHECK_FALSE(is_watertight_manifold(open_cube));
    CHECK_FALSE(is_watertight_manifold(indexed_triangle_set{}));
}

TEST_CASE("MeshRound: rounding the same mesh twice gives an identical result", "[MeshRound]")
{
    const indexed_triangle_set cube = make_box(Vec3d(20., 20., 20.));
    RoundOptions opts;
    opts.radius           = 2.;
    opts.keep_bottom_flat = true;
    opts.bottom_margin    = 3.;

    const indexed_triangle_set a = round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), nullptr);
    const indexed_triangle_set b = round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), nullptr);
    REQUIRE_FALSE(a.indices.empty());
    REQUIRE(a.vertices.size() == b.vertices.size());
    REQUIRE(a.indices.size() == b.indices.size());
    for (size_t i = 0; i < a.vertices.size(); ++i)
        REQUIRE(a.vertices[i] == b.vertices[i]);
    for (size_t i = 0; i < a.indices.size(); ++i)
        REQUIRE(a.indices[i] == b.indices[i]);
}

// ----------------------------------------------------------------------------
// Slab height 0: the mirror path
//
// The owner's 2026-09-12 feedback, in assertions. A slab could not be 0, and the
// 1.5 mm default turned every part into a pedestal. 0 is now the default and means
// something better than "no protection": the mesh is reflected in its own base
// plane before the round, so the base face is INTERIOR to the solid the rounder sees
// and comes back flat and sharp, while the upright edges keep their fillet all the
// way down to the plate.
//
// The analytic stub is exactly the right instrument for this. It replaces whatever
// it is handed with the rounded box of that mesh's bounding box - so when the
// pipeline hands it the mirrored solid (20 x 20 x (20 + depth)) it returns a box
// rounded on every edge of THAT box, whose z = 0 plane is strictly interior. Cutting
// there therefore has to produce the corner-rounded footprint and the sharp
// perimeter, and if the pipeline forgot to mirror, the stub's own bottom fillet
// would show up and every one of these checks would fail. That is the property under
// test, and the stub cannot fake it.
// ----------------------------------------------------------------------------

// The footprint of a 20 x 20 square whose four corners are rounded at radius r: the
// full square less what a quarter-circle cuts off each corner.
static double corner_rounded_square_area(double side, double r)
{
    return side * side - (4. - M_PI) * r * r;
}

TEST_CASE("MeshRound: slab height 0 is the default and mirrors the base", "[MeshRound]")
{
    // The default really is 0 - that is half the fix.
    CHECK(RoundOptions{}.bottom_margin == Approx(0.));
    CHECK(RoundOptions{}.keep_bottom_flat);

    const Vec3d  size(20., 20., 20.);
    const double r    = 2.;
    const indexed_triangle_set cube = make_box(size); // min corner at the origin

    RoundOptions opts;
    opts.radius           = r;
    opts.keep_bottom_flat = true;
    opts.bottom_margin    = 0.;
    RoundReport rep;

    const indexed_triangle_set out =
        round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    INFO("note: " << rep.note);
    REQUIRE_FALSE(out.indices.empty());
    REQUIRE(rep.kept_bottom_flat);
    REQUIRE(rep.mirrored_base);
    CHECK_FALSE(rep.fell_back);
    CHECK(rep.bottom_margin_used == Approx(0.));

    // (1) min z is exactly the plate, and NO vertex is below it.
    double z_min = std::numeric_limits<double>::max();
    for (const Vec3f &v : out.vertices)
        z_min = std::min(z_min, double(v.z()));
    CHECK(z_min == Approx(0.).margin(1e-4));
    for (const Vec3f &v : out.vertices)
        REQUIRE(double(v.z()) >= -1e-4);

    // (2) the z = 0 cap is the footprint with its four XY corners rounded at r:
    // between the full 400 mm^2 square and that square less the four corner bites,
    // and nowhere near either the eroded footprint a bottom fillet would leave or a
    // square one that says the vertical fillet stopped short of the plate.
    const double cap = round_base_cap_area(out, Vec3d(0., 0., -1.), 1e-3);
    const double lower_bound = corner_rounded_square_area(20., r); // 400 - (4 - pi) * 4
    INFO("cap area " << cap << ", corner-rounded " << lower_bound);
    CHECK(cap > lower_bound - 1.0);
    CHECK(cap < 400. + 1e-3);
    // Tight, not merely inside the window: the tessellated quarter-circle inscribes
    // the true one, so the cap lands a hair UNDER the closed form and nowhere else.
    CHECK(cap == Approx(lower_bound).margin(0.5));

    // (3) the bottom perimeter is SHARP. Every facet touching z = 0 is either the cap
    // (normal straight down) or a wall rising off it (normal horizontal), with
    // nothing in between, and the dihedral across the perimeter edges is a full 90.
    double off_axis = 0.;
    const double perim = round_base_perimeter_dihedral_deg(out, Vec3d(0., 0., -1.), 1e-3, &off_axis);
    INFO("base perimeter dihedral " << perim << " deg, worst off-axis " << off_axis);
    CHECK(off_axis < 1.0);
    CHECK(perim == Approx(90.).margin(1.0));

    // (4) the upright edges ARE rounded, and by the same amount at the plate as at
    // mid-height: the section at z = 10 has the corner-rounded outline, and so does
    // the one just above the base.
    double area_mid = 0., u_mid = 0., v_mid = 0.;
    REQUIRE(round_section_at_height(out, Vec3d(0., 0., -1.), 10., &area_mid, &u_mid, &v_mid));
    INFO("mid section area " << area_mid);
    CHECK(area_mid == Approx(lower_bound).margin(0.5));
    CHECK(u_mid == Approx(20.).margin(1e-2));
    CHECK(v_mid == Approx(20.).margin(1e-2));
    // The same outline right down at the plate - which is the whole point. A bottom
    // fillet would make this section markedly smaller than the one at mid-height.
    double area_low = 0.;
    REQUIRE(round_section_at_height(out, Vec3d(0., 0., -1.), 0.2, &area_low, nullptr, nullptr));
    INFO("low section area " << area_low << " vs mid " << area_mid);
    CHECK(area_low == Approx(area_mid).margin(0.5));

    // (5) watertight and free of self-intersections.
    CHECK(is_watertight_manifold(out));
    CHECK(has_no_self_intersections(out));

    // And the envelope is unchanged: the round is inward.
    Vec3d lo, hi;
    mesh_bbox(out, lo, hi);
    CHECK((hi - lo).x() == Approx(20.).margin(1e-2));
    CHECK((hi - lo).y() == Approx(20.).margin(1e-2));
    CHECK((hi - lo).z() == Approx(20.).margin(1e-2));
}

TEST_CASE("MeshRound: slab 0 and a slab differ exactly where the owner said they do", "[MeshRound]")
{
    const Vec3d  size(20., 20., 20.);
    const double r = 2.;
    const indexed_triangle_set cube = make_box(size);

    auto run = [&](double margin, RoundReport &rep) {
        RoundOptions opts;
        opts.radius           = r;
        opts.keep_bottom_flat = true;
        opts.bottom_margin    = margin;
        return round_with_options(cube, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    };

    RoundReport rep0, rep15;
    const indexed_triangle_set zero = run(0., rep0);
    const indexed_triangle_set slab = run(1.5, rep15);
    REQUIRE_FALSE(zero.indices.empty());
    REQUIRE_FALSE(slab.indices.empty());
    REQUIRE(rep0.mirrored_base);
    REQUIRE_FALSE(rep15.mirrored_base);

    // Both sit flat on the plate.
    auto min_z = [](const indexed_triangle_set &m) {
        double z = std::numeric_limits<double>::max();
        for (const Vec3f &v : m.vertices)
            z = std::min(z, double(v.z()));
        return z;
    };
    CHECK(min_z(zero) == Approx(0.).margin(1e-4));
    CHECK(min_z(slab) == Approx(0.).margin(1e-4));

    // The difference is the footprint: the slab keeps the original SQUARE 400 mm^2
    // base (that is the pedestal), the mirror gives the corner-rounded one.
    const double cap_zero = round_base_cap_area(zero, Vec3d(0., 0., -1.), 1e-3);
    const double cap_slab = round_base_cap_area(slab, Vec3d(0., 0., -1.), 1e-3);
    INFO("cap zero " << cap_zero << ", cap slab " << cap_slab);
    CHECK(cap_slab == Approx(400.).margin(1.0));
    CHECK(cap_zero < cap_slab - 1.0);
    CHECK(cap_zero == Approx(corner_rounded_square_area(20., r)).margin(0.5));

    // And the slab's lip is straight: its section 1 mm up (inside the 1.5 mm band)
    // is still the full square, whereas the mirror's is already the rounded one.
    double slab_low = 0., zero_low = 0.;
    REQUIRE(round_section_at_height(slab, Vec3d(0., 0., -1.), 1.0, &slab_low, nullptr, nullptr));
    REQUIRE(round_section_at_height(zero, Vec3d(0., 0., -1.), 1.0, &zero_low, nullptr, nullptr));
    INFO("section at 1 mm: slab " << slab_low << ", zero " << zero_low);
    CHECK(slab_low == Approx(400.).margin(1.0));
    CHECK(zero_low < slab_low - 1.0);
}

TEST_CASE("MeshRound: the mirror declines rather than mangling a part shorter than the fillet", "[MeshRound]")
{
    // A 20 x 20 x 3 plate at r = 2: the reflection would have to reach 2 mm below a
    // part only 3 mm tall, which leaves no untouched geometry above the mirrored
    // band. Declining and rounding whole is the honest answer; silently producing a
    // lens is not.
    const indexed_triangle_set plate = make_box(Vec3d(20., 20., 3.));

    std::string why;
    const indexed_triangle_set direct =
        round_mirror_flat_bottom(plate, 2., 0.3, true, analytic_rounder, Vec3d(0., 0., -1.), &why);
    INFO("why: " << why);
    CHECK(direct.indices.empty());
    CHECK_FALSE(why.empty());

    // And the pipeline falls back rather than failing.
    RoundOptions opts;
    opts.radius           = 2.;
    opts.keep_bottom_flat = true;
    opts.bottom_margin    = 0.;
    RoundReport rep;
    const indexed_triangle_set out =
        round_with_options(plate, opts, analytic_rounder, Vec3d(0., 0., -1.), &rep);
    INFO("note: " << rep.note);
    CHECK(rep.fell_back);
    CHECK_FALSE(rep.mirrored_base);
    // r = 2 does not fit in a 3 mm plate at all, so the plain round legitimately
    // returns nothing; what matters is that the mirror declined instead of throwing.
    CHECK(out.indices.empty());
}

TEST_CASE("MeshRound: the mirror path honours a rotated bed direction", "[MeshRound]")
{
    // The same cube with the bed along +X rather than -Z. Everything the zero-slab
    // path does is expressed against bed_dir, so the base must come out flat and
    // sharp in THAT frame - which is what a rotated volume on the plater is.
    const indexed_triangle_set cube = make_box(Vec3d(20., 20., 20.));
    const Vec3d bed(1., 0., 0.);

    RoundOptions opts;
    opts.radius           = 2.;
    opts.keep_bottom_flat = true;
    opts.bottom_margin    = 0.;
    RoundReport rep;

    const indexed_triangle_set out = round_with_options(cube, opts, analytic_rounder, bed, &rep);
    INFO("note: " << rep.note);
    REQUIRE_FALSE(out.indices.empty());
    REQUIRE(rep.mirrored_base);

    // The base is the +X face here, and nothing pokes through it.
    double x_max = -1e30;
    for (const Vec3f &v : out.vertices)
        x_max = std::max(x_max, double(v.x()));
    CHECK(x_max == Approx(20.).margin(1e-3));

    double off_axis = 0.;
    const double perim = round_base_perimeter_dihedral_deg(out, bed, 1e-3, &off_axis);
    INFO("rotated base perimeter " << perim << " deg, off-axis " << off_axis);
    CHECK(off_axis < 1.0);
    CHECK(perim == Approx(90.).margin(1.0));

    const double cap = round_base_cap_area(out, bed, 1e-3);
    CHECK(cap == Approx(corner_rounded_square_area(20., 2.)).margin(0.5));
    CHECK(is_watertight_manifold(out));
}

TEST_CASE("MeshRound: the self-intersection check agrees with hand-built meshes", "[MeshRound]")
{
    // A plain cube does not intersect itself.
    CHECK(has_no_self_intersections(make_box(Vec3d(10., 10., 10.))));
    CHECK(has_no_self_intersections(its_make_sphere(5., 2. * PI / 30.)));
    CHECK(has_no_self_intersections(indexed_triangle_set{}));

    // Two triangles that genuinely cross, sharing no vertex index: a plus sign
    // standing on edge. Nothing else in the set, so the only possible verdict comes
    // from that pair.
    indexed_triangle_set crossing;
    crossing.vertices = {Vec3f(-1.f, 0.f, -1.f), Vec3f(1.f, 0.f, -1.f), Vec3f(0.f, 0.f, 1.f),
                         Vec3f(0.f, -1.f, 0.f), Vec3f(0.f, 1.f, 0.f),  Vec3f(0.f, 0.f, 0.5f)};
    crossing.indices  = {Vec3i32(0, 1, 2), Vec3i32(3, 4, 5)};
    CHECK_FALSE(has_no_self_intersections(crossing));
}

TEST_CASE("MeshRound: the section helper measures what it claims to", "[MeshRound]")
{
    // A 10 mm cube: every horizontal section is 100 mm^2 and 10 x 10, and a plane
    // that misses the part reports nothing rather than zero area.
    const indexed_triangle_set cube = make_box(Vec3d(10., 10., 10.));
    double a = 0., u = 0., v = 0.;
    REQUIRE(round_section_at_height(cube, Vec3d(0., 0., -1.), 5., &a, &u, &v));
    CHECK(a == Approx(100.).margin(1e-3));
    CHECK(u == Approx(10.).margin(1e-3));
    CHECK(v == Approx(10.).margin(1e-3));
    CHECK_FALSE(round_section_at_height(cube, Vec3d(0., 0., -1.), 20., &a, nullptr, nullptr));

    // The base cap of an untouched cube is its whole 100 mm^2 face.
    CHECK(round_base_cap_area(cube, Vec3d(0., 0., -1.), 1e-3) == Approx(100.).margin(1e-3));
    // ...and its bottom perimeter is a sharp 90 with no off-axis facet.
    double off = 0.;
    CHECK(round_base_perimeter_dihedral_deg(cube, Vec3d(0., 0., -1.), 1e-3, &off) == Approx(90.).margin(1e-3));
    CHECK(off == Approx(0.).margin(1e-6));
}
