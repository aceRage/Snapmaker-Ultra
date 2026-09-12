#include <catch2/catch.hpp>

#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/FlexiJoint.hpp>
#include <libslic3r/Format/3mf.hpp>
// PHASE 4: Metadata/cut_information.xml - where a connector's baked position and
// frame live - is a BambuStudio-lineage part, so the connector round trip needs
// the PROJECT writer/reader pair rather than the generic 3MF one.
#include <libslic3r/Format/bbs_3mf.hpp>
#include <libslic3r/PresetBundle.hpp>
// PHASE 4: SelfAdjointEigenSolver, for fitting a connector's axis out of its
// own vertices. Eigen's core header does not pull the decompositions in.
#include <Eigen/Eigenvalues>
#include <libslic3r/Format/STL.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleMeshSlicer.hpp>

#include <boost/filesystem/operations.hpp>
#include <boost/filesystem/path.hpp>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <set>

using namespace Slic3r;

// The test article: a 40 mm cube centred on the origin, so the cut plane frame
// (z == 0) runs through its middle.
static const double CUBE = 40.0;

static indexed_triangle_set centred_cube(double s = CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

// A dome: the centre control point raised, the rim left at zero.
static CurvedCutSheet dome_sheet(double height, int resolution = 5, double half_size = 40.0)
{
    CurvedCutSheet sheet(resolution);
    sheet.set_half_size(half_size);
    const int c = resolution / 2;
    sheet.at(c, c) = height;
    return sheet;
}

// ---------------------------------------------------------------------------
// (1) Zero displacement reproduces the flat cut EXACTLY.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a flat sheet is the flat cut", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();

    // The flat reference: the same cut_mesh() call perform_with_plane() makes.
    indexed_triangle_set ref_upper, ref_lower;
    cut_mesh(cube, 0.0f, &ref_upper, &ref_lower);

    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    REQUIRE(sheet.is_flat());

    // Everything on the sheet must read as exactly zero, in double precision,
    // not merely near it - that is what lets the cut dispatch to the flat path.
    for (int j = 0; j <= 20; ++ j)
        for (int i = 0; i <= 20; ++ i)
            REQUIRE(sheet.evaluate(double(i) / 20.0, double(j) / 20.0) == 0.0);

    // And the whole Cut path: a flat sheet must produce the same objects as
    // perform_with_plane(), because it calls exactly that.
    Model model;
    ModelObject* mo = model.add_object();
    mo->add_volume(TriangleMesh(cube));
    mo->add_instance();
    mo->ensure_on_bed();

    // Cut clones the object it is handed, so the same mo can drive both runs.
    const Transform3d cut_matrix = Geometry::translation_transform(Vec3d(0., 0., 0.5 * CUBE));
    auto cut_with = [&](bool curved) {
        Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& res = curved ? cut.perform_with_curved_sheet(sheet) : cut.perform_with_plane();
        std::vector<indexed_triangle_set> out;
        for (const ModelObject* obj : res)
            for (const ModelVolume* v : obj->volumes)
                out.push_back(v->mesh().its);
        return out;
    };

    const std::vector<indexed_triangle_set> flat   = cut_with(false);
    const std::vector<indexed_triangle_set> curved = cut_with(true);

    REQUIRE(flat.size() == curved.size());
    REQUIRE(!flat.empty());
    for (size_t k = 0; k < flat.size(); ++ k) {
        // Same triangle count and the same vertex set, to 1e-9.
        REQUIRE(flat[k].indices.size() == curved[k].indices.size());
        REQUIRE(flat[k].vertices.size() == curved[k].vertices.size());
        for (size_t i = 0; i < flat[k].vertices.size(); ++ i)
            REQUIRE((flat[k].vertices[i] - curved[k].vertices[i]).cwiseAbs().maxCoeff() < 1e-9f);
        for (size_t i = 0; i < flat[k].indices.size(); ++ i)
            REQUIRE(flat[k].indices[i] == curved[k].indices[i]);
    }
}

// ---------------------------------------------------------------------------
// (2) A domed sheet on a 40 mm cube: volume conservation and face heights.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a domed sheet splits a cube by volume and by height", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const float cube_volume = its_volume(cube);
    REQUIRE(cube_volume == Approx(CUBE * CUBE * CUBE).epsilon(1e-4));

    const CurvedCutSheet sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());

    const double vu = double(its_volume(upper));
    const double vl = double(its_volume(lower));
    // Volume conservation within 1e-6 relative.
    REQUIRE(std::abs(vu + vl - double(cube_volume)) / double(cube_volume) < 1e-6);

    // The cut face: sample f(u,v) at 25 points inside the cube's footprint and
    // check the lower half's top surface really sits at that height. Shoot a
    // ray straight down from above and take the FIRST hit on the lower half;
    // that hit is the mating face.
    auto surface_height = [](const indexed_triangle_set& its, double x, double y, bool topmost) {
        double best = topmost ? -1e30 : 1e30;
        bool   found = false;
        for (const Vec3i32& f : its.indices) {
            const Vec3d a = its.vertices[f(0)].cast<double>();
            const Vec3d b = its.vertices[f(1)].cast<double>();
            const Vec3d c = its.vertices[f(2)].cast<double>();
            // Barycentric containment in XY.
            const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (std::abs(d) < 1e-12)
                continue;
            const double l1 = ((b.y() - c.y()) * (x - c.x()) + (c.x() - b.x()) * (y - c.y())) / d;
            const double l2 = ((c.y() - a.y()) * (x - c.x()) + (a.x() - c.x()) * (y - c.y())) / d;
            const double l3 = 1.0 - l1 - l2;
            if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9)
                continue;
            const double z = l1 * a.z() + l2 * b.z() + l3 * c.z();
            found = true;
            best = topmost ? std::max(best, z) : std::min(best, z);
        }
        REQUIRE(found);
        return best;
    };

    const double half = 0.5 * CUBE;
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            // Stay off the cube's own edges, where the mating face meets the wall.
            const double x = -half + (double(i) + 0.5) * (CUBE / 5.0);
            const double y = -half + (double(j) + 0.5) * (CUBE / 5.0);
            const double expected = sheet.evaluate_local(x, y);
            const double got_lower = surface_height(lower, x, y, /*topmost*/ true);
            const double got_upper = surface_height(upper, x, y, /*topmost*/ false);
            INFO("x=" << x << " y=" << y << " expected=" << expected);
            REQUIRE(std::abs(got_lower - expected) < 0.02);
            REQUIRE(std::abs(got_upper - expected) < 0.02);
        }
}

// ---------------------------------------------------------------------------
// (3) Both halves are watertight and they do not overlap.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: halves are closed and do not intersect", "[CurvedCut]")
{
    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));

    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);

    // Non-overlap: intersecting the two halves must give (essentially) nothing.
    TriangleMesh              mu(upper), ml(lower);
    std::vector<TriangleMesh> dst;
    const bool                ok = MeshBoolean::mfd::make_boolean(mu, ml, dst, "INTERSECTION");
    REQUIRE(ok);
    double overlap = 0.0;
    for (const TriangleMesh& m : dst)
        overlap += std::abs(double(its_volume(m.its)));
    // Any leftover is boolean noise at the shared face, not real interpenetration.
    REQUIRE(overlap / (CUBE * CUBE * CUBE) < 1e-6);
}

// ---------------------------------------------------------------------------
// (4) Grid resize preserves the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a grid resize keeps the surface", "[CurvedCut]")
{
    // A SMOOTH surface, not a single-cell spike: resampling a 5x5 grid onto a
    // 7x7 one is a refit, not a refinement (the 5x5 nodes at u = 0.25 and 0.75
    // are not 7x7 nodes), so how well the shape survives depends on how much of
    // it lives between the old nodes. A spike concentrated in one cell is the
    // worst case and loses ~0.76 mm out of 8; a surface the control grid can
    // actually resolve - which is what dragging with a falloff brush produces -
    // survives far better. Both are measured below.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            const double x = double(i) / 4.0 * 2.0 - 1.0;
            const double y = double(j) / 4.0 * 2.0 - 1.0;
            sheet.at(i, j) = 8.0 * std::cos(0.5 * PI * x) * std::cos(0.5 * PI * y) - 1.5 * x * y;
        }

    // Reference: the surface as it stands, on a dense sample grid.
    const int    N = 33;
    std::vector<double> ref(size_t(N) * N);
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i)
            ref[size_t(j) * N + i] = sheet.evaluate(double(i) / (N - 1), double(j) / (N - 1));

    CurvedCutSheet round_trip = sheet;
    round_trip.set_resolution(7);
    REQUIRE(round_trip.resolution() == 7);
    // The 5x5 grid's own nodes are not all nodes of a 7x7 grid, so 7x7 is a
    // re-fit, not a refinement: check the surface stays close, then that coming
    // back to 5x5 lands on the same control values.
    double max_err = 0.0;
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i)
            max_err = std::max(max_err, std::abs(round_trip.evaluate(double(i) / (N - 1), double(j) / (N - 1)) -
                                                 ref[size_t(j) * N + i]));
    // 5 -> 7 is a REFIT, not a refinement: the 5x5 interior nodes (u = 0.25,
    // 0.75) are not 7x7 nodes, so the shape is re-fitted through a differently
    // phased grid and some detail between the old nodes is lost. On this smooth
    // 8 mm surface that costs ~0.26 mm, about 3% of amplitude. The bound is
    // stated relative to amplitude so it means the same thing if the fixture
    // changes, and it is a REGRESSION guard, not an accuracy claim.
    INFO("5 -> 7 max surface error " << max_err << " mm");
    REQUIRE(max_err < 0.05 * 8.0);

    round_trip.set_resolution(5);
    REQUIRE(round_trip.resolution() == 5);
    double max_ctl_err = 0.0;
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            max_ctl_err = std::max(max_ctl_err, std::abs(round_trip.at(i, j) - sheet.at(i, j)));
    INFO("5 -> 7 -> 5 max control point error " << max_ctl_err << " mm");
    REQUIRE(max_ctl_err < 0.05 * 8.0);

    // The worst case for the record: a spike in a single cell cannot survive a
    // refit onto nodes that miss it. This is a property of the representation,
    // not a defect - it is documented so a later change that makes it WORSE is
    // visible.
    CurvedCutSheet spike = dome_sheet(8.0, 5);
    double spike_err = 0.0;
    {
        std::vector<double> before_spike(size_t(N) * N);
        for (int j = 0; j < N; ++ j)
            for (int i = 0; i < N; ++ i)
                before_spike[size_t(j) * N + i] = spike.evaluate(double(i) / (N - 1), double(j) / (N - 1));
        spike.set_resolution(7);
        for (int j = 0; j < N; ++ j)
            for (int i = 0; i < N; ++ i)
                spike_err = std::max(spike_err, std::abs(spike.evaluate(double(i) / (N - 1), double(j) / (N - 1)) -
                                                         before_spike[size_t(j) * N + i]));
    }
    INFO("5 -> 7 max surface error for a one-cell spike " << spike_err << " mm");
    REQUIRE(spike_err < 1.0);

    // 3 -> 9 -> 3 IS a refinement in both directions (the 3x3 nodes u = 0,
    // 0.5, 1 are all 9x9 nodes), so it must round-trip essentially exactly.
    CurvedCutSheet coarse(3);
    coarse.set_half_size(40.0);
    coarse.at(1, 1) = 6.0;
    coarse.at(0, 2) = -2.0;
    const std::vector<double> before = coarse.values();
    coarse.set_resolution(9);
    coarse.set_resolution(3);
    for (size_t k = 0; k < before.size(); ++ k)
        REQUIRE(coarse.values()[k] == Approx(before[k]).margin(1e-9));
}

// ---------------------------------------------------------------------------
// The building blocks the interactive editor rests on.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the sheet interpolates its control points", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0);
    sheet.at(2, 2) = 5.0;
    sheet.at(0, 4) = -1.5;

    // Catmull-Rom is interpolating: at a control point the surface IS its value.
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            REQUIRE(sheet.evaluate(sheet.control_u(i), sheet.control_u(j)) == Approx(sheet.at(i, j)).margin(1e-9));

    // control_xy and evaluate_local agree with (u,v).
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            const Vec2d xy = sheet.control_xy(i, j);
            REQUIRE(sheet.evaluate_local(xy.x(), xy.y()) == Approx(sheet.at(i, j)).margin(1e-9));
        }
}

TEST_CASE("Curved cut: grab and smooth behave like the sculpt brush", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0);

    // A grab centred on the middle control point with a radius that reaches its
    // neighbours: the centre gets the full delta, the neighbours less, the rim
    // nothing.
    sheet.grab(Vec2d(0.0, 0.0), 12.0, 4.0, /*falloff*/ true);
    REQUIRE(sheet.at(2, 2) == Approx(4.0).margin(1e-9));
    REQUIRE(sheet.at(1, 2) > 0.0);
    REQUIRE(sheet.at(1, 2) < 4.0);
    REQUIRE(sheet.at(0, 2) == Approx(0.0).margin(1e-9));
    REQUIRE_FALSE(sheet.is_flat());

    // No falloff: everything inside the radius moves the whole way.
    CurvedCutSheet hard(5);
    hard.set_half_size(20.0);
    hard.grab(Vec2d(0.0, 0.0), 12.0, 4.0, /*falloff*/ false);
    REQUIRE(hard.at(2, 2) == Approx(4.0).margin(1e-9));
    REQUIRE(hard.at(1, 2) == Approx(4.0).margin(1e-9));

    // Smooth pulls the spike down towards its neighbours without moving the
    // surface's mean far.
    const double before = sheet.at(2, 2);
    sheet.smooth(0.5);
    REQUIRE(sheet.at(2, 2) < before);
    REQUIRE(sheet.at(2, 2) > 0.0);

    // Reset really is flat, and a flat sheet is what the flat-cut path keys on.
    sheet.reset();
    REQUIRE(sheet.is_flat());
    REQUIRE(sheet.max_displacement() == 0.0);
}

// Sheet size against boolean time, for the record in the spec. Not an
// assertion about wall-clock (CI machines vary) - it prints, and only fails if
// a size becomes outright unusable.
TEST_CASE("Curved cut: sheet size against boolean cost", "[CurvedCut][.perf]")
{
    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    for (int samples : {32, 64, 128, 192, 256}) {
        indexed_triangle_set upper, lower;
        const auto t0 = std::chrono::steady_clock::now();
        const bool ok = curved_cut_split(cube, sheet, &upper, &lower, samples);
        const auto t1 = std::chrono::steady_clock::now();
        const double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        REQUIRE(ok);
        BoundingBoxf3 dummy;
        dummy.merge(Vec3d(-20, -20, -20));
        dummy.merge(Vec3d(20, 20, 20));
        const size_t slab_tris = curved_cut_lower_slab(sheet, dummy, samples).indices.size();
        WARN("samples " << samples << "x" << samples
             << "  slab tris " << slab_tris
             << "  both booleans " << ms << " ms"
             << "  result tris " << (upper.indices.size() + lower.indices.size()));
    }
}

// ---------------------------------------------------------------------------
// (2b) The cut MATRIX, not just the sheet: a vertical cut plane, off-centre.
//
// The gizmo lets the base plane be rotated and translated, and the sheet rides
// on that plane's frame. So the interesting failure is not the sheet maths - it
// is whether Cut::perform_with_curved_sheet carries the SAME frame the flat cut
// uses. If it did not, a bent sheet under a rotated plane would land somewhere
// else entirely, or the two halves would not add up to the cube.
//
// Here the plane is rotated 90 degrees about X (so its normal is world -Y: a
// vertical cut plane) and translated 7 mm off centre along that normal. The
// proof is threefold: the halves' volumes sum to the cube, both are closed, and
// the mating face sits at f(u,v) in the PLANE'S OWN frame - which is only true
// if the frame survived the round trip.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rotated, off-centre cut plane keeps its frame", "[CurvedCut]")
{
    const indexed_triangle_set cube        = centred_cube();
    const double               cube_volume = double(its_volume(cube));

    // A dome, tall enough that a frame mistake could not hide inside tolerance.
    const double         dome   = 8.0;
    const CurvedCutSheet sheet  = dome_sheet(dome);
    const double         offset = 7.0;

    // The gizmo builds the cut matrix as translation * rotation, and Cut splits
    // it back into get_rotation_matrix() and get_offset(); build it the same way.
    // 90 degrees about X sends local +Z to world -Y, so the plane is vertical.
    const Transform3d rotation = Geometry::rotation_transform(Vec3d(0.5 * PI, 0., 0.));
    // Off centre ALONG THE NORMAL, so the cut is not through the middle: the
    // plane's own origin ends up at world (0, -7, 0).
    const Vec3d       plane_origin = rotation * Vec3d(0., 0., offset);
    const Transform3d cut_matrix   = Geometry::translation_transform(plane_origin) * rotation;

    // Sanity: the frame really is rotated and off centre.
    REQUIRE((rotation * Vec3d::UnitZ() - Vec3d(0., -1., 0.)).norm() < 1e-12);
    REQUIRE((plane_origin - Vec3d(0., -offset, 0.)).norm() < 1e-12);

    Model model;
    ModelObject* mo = model.add_object();
    mo->add_volume(TriangleMesh(cube));
    mo->add_instance();
    // NOTE: no ensure_on_bed(). The cube stays centred on the origin so the
    // plane frame above is the frame the assertions below use; dropping it onto
    // the bed would add an instance offset the test would have to undo again.

    Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& res = cut.perform_with_curved_sheet(sheet);
    REQUIRE(res.size() == 2);

    // Cut re-frames each half onto the cut plane (reset_instance_transformation),
    // so pull the meshes back into world coordinates the way the viewer does.
    auto world_its = [](const ModelObject* obj) {
        indexed_triangle_set out;
        const Transform3d inst = obj->instances.front()->get_transformation().get_matrix();
        for (const ModelVolume* v : obj->volumes) {
            if (!v->is_model_part())
                continue;
            TriangleMesh m(v->mesh());
            m.transform(inst * v->get_matrix(), true);
            its_merge(out, m.its);
        }
        return out;
    };

    const indexed_triangle_set a = world_its(res[0]);
    const indexed_triangle_set b = world_its(res[1]);
    REQUIRE(!a.empty());
    REQUIRE(!b.empty());

    // (i) The halves add up to the cube.
    const double va = double(its_volume(a));
    const double vb = double(its_volume(b));
    REQUIRE(std::abs(va + vb - cube_volume) / cube_volume < 1e-5);
    // Both halves are real: the off-centre plane cannot have thrown one away.
    REQUIRE(va > 0.05 * cube_volume);
    REQUIRE(vb > 0.05 * cube_volume);

    // (ii) Both halves are closed.
    REQUIRE(its_num_open_edges(a) == 0);
    REQUIRE(its_num_open_edges(b) == 0);

    // (iii) The cut face is f(u,v) in the PLANE'S frame. Take every vertex of
    // both halves into that frame; a vertex whose (x,y) is inside the cube's
    // footprint under the plane and whose local z is anywhere near the sheet
    // must be ON the sheet. A frame error - the rotation applied the wrong way
    // round, the offset taken along the wrong axis - moves the face bodily and
    // shows up here immediately.
    const Transform3d world_to_plane = cut_matrix.inverse();

    size_t sampled = 0;
    double worst   = 0.;
    for (const indexed_triangle_set* half : { &a, &b })
        for (const Vec3f& vf : half->vertices) {
            const Vec3d p = world_to_plane * vf.cast<double>();
            // Well inside the cube's cross-section, so this is face, not rim:
            // the cube spans +-20 in x and (after the rotation) +-20 in the
            // plane's y as well.
            if (std::abs(p.x()) > 16.0 || std::abs(p.y()) > 16.0)
                continue;
            const double f = sheet.evaluate_local(p.x(), p.y());
            // The sampled sheet sits a chord sag below the true surface, and the
            // boolean adds vertices along the object's own faces too, so only
            // judge the vertices that are meant to be on the face at all.
            if (std::abs(p.z() - f) > 0.5)
                continue;
            worst = std::max(worst, std::abs(p.z() - f));
            ++ sampled;
        }

    // The face has to have actually been found - a frame error that moved it
    // out of the window above would leave this at zero.
    REQUIRE(sampled > 100);
    INFO("worst deviation from f(u,v) in the plane frame: " << worst << " mm over " << sampled << " vertices");
    REQUIRE(worst < 0.02);

    // And the dome really did bend the cut: the face is not the flat plane.
    // A dome of `dome` mm over the cube's footprint must show up as a spread of
    // local z on the face far larger than the 0.02 mm tolerance above.
    double zmin = 1e30, zmax = -1e30;
    for (const Vec3f& vf : a.vertices) {
        const Vec3d p = world_to_plane * vf.cast<double>();
        if (std::abs(p.x()) > 16.0 || std::abs(p.y()) > 16.0)
            continue;
        if (std::abs(p.z() - sheet.evaluate_local(p.x(), p.y())) > 0.5)
            continue;
        zmin = std::min(zmin, p.z());
        zmax = std::max(zmax, p.z());
    }
    REQUIRE(zmax - zmin > 0.5 * dome);
}

TEST_CASE("Curved cut: the slab is a watertight solid", "[CurvedCut]")
{
    const CurvedCutSheet sheet = dome_sheet(8.0);
    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    for (int samples : {8, 32, 64}) {
        const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, samples);
        INFO("samples = " << samples);
        REQUIRE(its_num_open_edges(slab) == 0);
        // Positive volume with the winding as generated: outward normals point out.
        REQUIRE(its_volume(slab) > 0.f);
    }
}

// ---------------------------------------------------------------------------
// Demo export. Skipped unless EDGESLICER_CURVED_CUT_DEMO_DIR names a directory:
// this is the "cut a cube with a domed sheet, look at both halves" artefact the
// research spec's test plan asks for, produced from the very code under test.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: demo export", "[CurvedCut][.demo]")
{
    const char* dir = std::getenv("EDGESLICER_CURVED_CUT_DEMO_DIR");
    if (dir == nullptr || *dir == '\0')
        return;

    const boost::filesystem::path out(dir);
    boost::filesystem::create_directories(out);

    const indexed_triangle_set cube  = centred_cube();
    const CurvedCutSheet       sheet = dome_sheet(8.0);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));

    TriangleMesh mu(upper), ml(lower);
    REQUIRE(store_stl((out / "curved_cut_upper.stl").string().c_str(), &mu, true));
    REQUIRE(store_stl((out / "curved_cut_lower.stl").string().c_str(), &ml, true));

    // Both halves in one 3MF, as the two parts a keep-as-parts cut produces.
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "curved_cut_demo";
    mo->add_volume(TriangleMesh(upper))->name = "curved_cut_demo_A";
    mo->add_volume(TriangleMesh(lower))->name = "curved_cut_demo_B";
    mo->add_instance();
    mo->ensure_on_bed();
    REQUIRE(store_3mf((out / "curved_cut_demo.3mf").string().c_str(), &model, nullptr, false));

    // A few numbers to read back off the console alongside the files.
    WARN("demo: cube volume " << its_volume(cube)
         << " upper " << its_volume(upper) << " lower " << its_volume(lower)
         << " open edges upper " << its_num_open_edges(upper)
         << " lower " << its_num_open_edges(lower));
}

// ---------------------------------------------------------------------------
// (10) The gizmo's own call sequence must not lose the displacement.
//
// The gizmo re-fits the sheet to the object every time it rebuilds the preview
// model (update_curved_sheet_model -> set_half_size(curved_sheet_half_size())),
// and the control-point slider re-samples the grid. Neither may zero the
// surface, and set_half_size must not move the surface over the object either:
// it changes the sheet's DOMAIN, and the cut has to keep evaluating the same
// f(u,v) over the object's footprint.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the gizmo's fit sequence keeps the displacement", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    // The sequence a drag actually produces: fit, set the grid, drag a handle,
    // then fit again with a different extent (a new bounding box, a moved plane).
    sheet.set_half_size(30.0);
    sheet.set_resolution(5);
    const Vec2d handle = sheet.control_xy(2, 2);
    sheet.grab(handle, 20.0, 6.0, /*falloff*/ true);
    REQUIRE(!sheet.is_flat());
    const double peak = sheet.max_displacement();
    REQUIRE(peak == Approx(6.0));

    sheet.set_half_size(52.0);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() == Approx(peak));

    // A grid resize in between must not flatten it either.
    sheet.set_resolution(9);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() > 0.5 * peak);

    // And the cut must still see a curve: the slab widening curved_cut_split()
    // does may not rescale the sheet, so the height over the object's footprint
    // is the height the gizmo drew.
    const indexed_triangle_set cube = centred_cube();
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(std::abs(double(its_volume(upper)) + double(its_volume(lower)) - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-6);
}

// ---------------------------------------------------------------------------
// (11) Widening the slab past the sheet must not move the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a wider slab keeps the sheet's own heights", "[CurvedCut]")
{
    const CurvedCutSheet sheet = dome_sheet(5.0, 5, 20.0);

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-60, -60, -20));
    bbox.merge(Vec3d(60, 60, 20));

    // The object reaches far past the sheet, so curved_cut_lower_slab has to be
    // built at 70 mm. The dome's own peak stays where the sheet puts it.
    const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, 128, 70.0);
    REQUIRE(its_num_open_edges(slab) == 0);

    double peak = -1e30, peak_x = 0.0, peak_y = 0.0;
    double half_extent = 0.0;
    for (const Vec3f& v : slab.vertices) {
        half_extent = std::max(half_extent, double(std::abs(v.x())));
        if (double(v.z()) > peak) { peak = double(v.z()); peak_x = double(v.x()); peak_y = double(v.y()); }
    }
    REQUIRE(half_extent >= 69.0);              // the slab really was widened
    // ... and the dome kept its height. The 128-sample grid now spans 140 mm, so its
    // 1.1 mm spacing straddles rather than hits the peak: the sampled maximum sits a
    // chord sag below 5 mm. Nowhere near stretched flat, which is what the old
    // set_half_size() rescale would have produced here (~0.9 mm).
    REQUIRE(peak == Approx(5.0).margin(0.15));
    REQUIRE(std::abs(peak_x) < 2.0);           // ... at the sheet's own centre
    REQUIRE(std::abs(peak_y) < 2.0);
    // Outside the sheet's domain the rim value (zero) is extruded, not stretched.
    // (Floor vertices sit far below; only look at the top sheet.)
    for (const Vec3f& v : slab.vertices)
        if (std::abs(double(v.x())) > 25.0 && double(v.z()) > -10.0)
            REQUIRE(std::abs(double(v.z())) < 0.01);
}

// ---------------------------------------------------------------------------
// (12) A ROTATED, OFFSET cut plane through Cut::perform_with_curved_sheet.
//
// The bug the owner hit: a vertical plane (rotated 90 deg about X) offset from
// the object's centre came out FLAT. This drives the whole Cut path, not
// curved_cut_split() on an axis-aligned mesh, and checks the cut face in the
// CUT PLANE'S OWN frame - the frame the sheet lives in.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rotated, offset plane cuts curved", "[CurvedCut]")
{
    // A 40 mm cube with its centre at the origin, one instance.
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    mo->add_instance();

    // The cut plane: rotated 90 degrees about X (a VERTICAL plane whose normal
    // is world -Y), then offset 10 mm from the object's centre along its normal.
    const Transform3d rotation   = Transform3d(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitX()));
    const Vec3d       offset     = rotation * Vec3d(0.0, 0.0, 10.0);
    const Transform3d cut_matrix = Geometry::translation_transform(offset) * rotation;

    // A 5 mm dome, the shape the gizmo's centre handle makes.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 5.0;
    REQUIRE(!sheet.is_flat());

    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
    Cut cut(mo, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    REQUIRE(parts.size() == 2);

    // Volumes sum to the cube, and both halves are closed.
    double total = 0.0;
    std::vector<double> vols;
    for (const ModelObject* part : parts) {
        REQUIRE(part->volumes.size() == 1);
        const indexed_triangle_set& its = part->volumes.front()->mesh().its;
        REQUIRE(!its.empty());
        REQUIRE(its_num_open_edges(its) == 0);
        const double v = std::abs(double(its_volume(its)));
        vols.push_back(v);
        total += v;
    }
    REQUIRE(std::abs(total - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-4);

    // The 10 mm offset means the plane sits 10 mm off centre, so the halves are
    // NOT equal: 30x40x40 and 10x40x40, plus/minus the dome's own volume.
    const size_t small_idx = vols[0] < vols[1] ? 0 : 1;
    REQUIRE(vols[small_idx] < 0.5 * CUBE * CUBE * CUBE);
    REQUIRE(vols[1 - small_idx] > 0.5 * CUBE * CUBE * CUBE);

    // The cut face, back in the CUT PLANE'S frame - the frame the sheet lives in.
    // add_cut_volume() bakes cut_matrix into the mesh it stores, and add_volume()
    // then re-centres that mesh and puts the shift in the volume matrix. So the way
    // back is cut_matrix.inverse() * volume_matrix. The INSTANCE transform must NOT
    // come in: post_process() re-seats it after the cut.
    const ModelObject* small_part = parts[small_idx];
    const Transform3d  m          = cut_matrix.inverse() * small_part->volumes.front()->get_matrix();

    std::vector<Vec3d> pts;
    pts.reserve(small_part->volumes.front()->mesh().its.vertices.size());
    for (const Vec3f& v : small_part->volumes.front()->mesh().its.vertices)
        pts.emplace_back(m * v.cast<double>());

    // The frame mapping itself: the cube spans local z in [-30, +10] (the plane sits
    // 10 mm off centre along its own normal), so the small half is the UPPER slice,
    // between the sheet and local z == +10, and its rim reaches down to z == 0 where
    // the sheet is undisplaced.
    double zmin = 1e30, zmax = -1e30;
    for (const Vec3d& p : pts) { zmin = std::min(zmin, p.z()); zmax = std::max(zmax, p.z()); }
    INFO("small half local z range: [" << zmin << ", " << zmax << "]");
    REQUIRE(zmax == Approx(10.0).margin(0.05));
    REQUIRE(zmin == Approx(0.0).margin(0.05));

    // THE FLAT-RESULT GUARD, in volume. A flat cut at this plane gives a plain
    // 10 x 40 x 40 = 16000 mm3 box. The 5 mm dome lifts the cut face into the upper
    // half and takes a real bite out of it, so a curved cut is measurably smaller -
    // and the missing volume is the dome's own, ~2.3 cm3 for this sheet.
    const double flat_upper = 10.0 * CUBE * CUBE;
    INFO("upper half volume " << vols[small_idx] << " vs flat " << flat_upper);
    REQUIRE(vols[small_idx] < flat_upper - 1000.0);
    REQUIRE(vols[small_idx] > flat_upper - 4000.0);

    // Every point on the cut face has local z == f(x,y) within 0.02 mm, and the
    // face is genuinely NOT planar: for a 5 mm dome its deviation from the best
    // fit plane (z == const, by the dome's symmetry) exceeds 1 mm.
    int    on_face           = 0;
    double max_dev_from_flat = 0.0;
    for (const Vec3d& p : pts) {
        const double f = sheet.evaluate_local(p.x(), p.y());
        if (std::abs(p.z() - f) < 0.02) {
            ++ on_face;
            max_dev_from_flat = std::max(max_dev_from_flat, std::abs(f));
        }
    }
    INFO("points on the cut face: " << on_face << " of " << pts.size());
    REQUIRE(on_face > 20);
    REQUIRE(max_dev_from_flat > 1.0);

    // ... and the half really lies on ONE side of the sheet: being the upper one, no
    // point of it may sit BELOW the sheet beyond boolean noise. This is the check
    // that fails outright if the cut ignored the sheet and went flat at z == 0 - the
    // dome's 5 mm crown would then be 5 mm underneath the flat face.
    double below = 0.0;
    for (const Vec3d& p : pts)
        below = std::max(below, sheet.evaluate_local(p.x(), p.y()) - p.z());
    INFO("deepest point below the sheet: " << below);
    REQUIRE(below < 0.05);
}

// ---------------------------------------------------------------------------
// (13) Rotated-plane demo export, alongside the axis-aligned one.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: rotated demo export", "[CurvedCut][.demo]")
{
    const char* dir = std::getenv("EDGESLICER_CURVED_CUT_DEMO_DIR");
    if (dir == nullptr || dir[0] == 0)
        return;

    const boost::filesystem::path out = boost::filesystem::path(dir) / "rotated";
    boost::filesystem::create_directories(out);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    mo->add_instance();

    const Transform3d rotation   = Transform3d(Eigen::AngleAxisd(0.5 * PI, Vec3d::UnitX()));
    const Vec3d       offset     = rotation * Vec3d(0.0, 0.0, 10.0);
    const Transform3d cut_matrix = Geometry::translation_transform(offset) * rotation;

    CurvedCutSheet sheet(5);
    sheet.set_half_size(40.0);
    sheet.at(2, 2) = 5.0;

    Cut cut(mo, 0, cut_matrix, ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    REQUIRE(parts.size() == 2);

    int idx = 0;
    for (const ModelObject* part : parts) {
        TriangleMesh     m(part->volumes.front()->mesh());
        const std::string name = (idx == 0 ? "rotated_cut_A.stl" : "rotated_cut_B.stl");
        REQUIRE(store_stl((out / name).string().c_str(), &m, true));
        ++ idx;
    }
    WARN("rotated demo written to " << out.string());
}

// ===========================================================================
// PHASE 2
//
// (a) A rectangular sheet, and what an extent change does to the surface.
// (b) The cross-section fit.
// (c) The snap helper.
// ===========================================================================

// ---------------------------------------------------------------------------
// (P2-1) A rectangular domain evaluates and samples over the rectangle, and an
// extent change RE-SAMPLES the surface so it stays put in the plane rather than
// stretching with the new rectangle - the same contract set_resolution() has.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rectangular sheet re-samples on an extent change", "[CurvedCut]")
{
    CurvedCutSheet sheet(7);
    sheet.set_half_size(30.0, 12.0);
    REQUIRE(sheet.half_size_u() == Approx(30.0));
    REQUIRE(sheet.half_size_v() == Approx(12.0));
    // half_size() is the square API: the larger of the two, so a caller that
    // only wants "how big is this sheet" still gets a covering answer.
    REQUIRE(sheet.half_size() == Approx(30.0));

    // The control grid really is laid over the rectangle.
    REQUIRE(sheet.control_xy(0, 0).x() == Approx(-30.0));
    REQUIRE(sheet.control_xy(0, 0).y() == Approx(-12.0));
    REQUIRE(sheet.control_xy(6, 6).x() == Approx(30.0));
    REQUIRE(sheet.control_xy(6, 6).y() == Approx(12.0));

    // ... and so is the dense sample grid.
    {
        const indexed_triangle_set its = sheet.sample_sheet(9);
        double mx = 0.0, my = 0.0;
        for (const Vec3f& v : its.vertices) {
            mx = std::max(mx, double(std::abs(v.x())));
            my = std::max(my, double(std::abs(v.y())));
        }
        REQUIRE(mx == Approx(30.0));
        REQUIRE(my == Approx(12.0));
    }

    // A smooth bump the control grid can actually resolve - the same shape a
    // falloff drag produces, and the case the re-fit has to survive.
    for (int j = 0; j < 7; ++ j)
        for (int i = 0; i < 7; ++ i) {
            const double x = sheet.control_xy(i, j).x();
            const double y = sheet.control_xy(i, j).y();
            sheet.at(i, j) = 6.0 * std::exp(-(x * x) / (2.0 * 14.0 * 14.0) - (y * y) / (2.0 * 6.0 * 6.0));
        }

    // Reference: the surface in LOCAL MILLIMETRES over the region that is inside
    // both the old and the new rectangle. That is the region the contract is
    // about - outside it there is nothing to preserve.
    const double keep_u = 20.0, keep_v = 9.0;
    const int    N = 21;
    std::vector<double> ref(size_t(N) * N);
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i) {
            const double x = (2.0 * double(i) / (N - 1) - 1.0) * keep_u;
            const double y = (2.0 * double(j) / (N - 1) - 1.0) * keep_v;
            ref[size_t(j) * N + i] = sheet.evaluate_local(x, y);
        }
    const double amp = sheet.max_displacement();
    REQUIRE(amp == Approx(6.0).margin(1e-9));

    // Grow the rectangle, re-sampling. The bump must stay where it was, at the
    // size it was: this is the thing the owner would notice if it broke, because
    // the plane moving would drag his bend around with it.
    CurvedCutSheet grown = sheet;
    grown.set_half_size(45.0, 20.0, /*resample*/ true);
    REQUIRE(grown.half_size_u() == Approx(45.0));
    REQUIRE(grown.half_size_v() == Approx(20.0));
    REQUIRE(!grown.is_flat());

    double max_err = 0.0;
    for (int j = 0; j < N; ++ j)
        for (int i = 0; i < N; ++ i) {
            const double x = (2.0 * double(i) / (N - 1) - 1.0) * keep_u;
            const double y = (2.0 * double(j) / (N - 1) - 1.0) * keep_v;
            max_err = std::max(max_err, std::abs(grown.evaluate_local(x, y) - ref[size_t(j) * N + i]));
        }
    INFO("rectangular re-sample max error " << max_err << " mm of " << amp << " mm amplitude");
    // A re-fit onto a differently phased grid, exactly like the 5 -> 7 grid
    // resize test: some detail between the old nodes is lost. 10% of amplitude
    // is the regression guard, not an accuracy claim.
    REQUIRE(max_err < 0.10 * amp);

    // The peak keeps its height and stays over the centre.
    REQUIRE(grown.evaluate_local(0.0, 0.0) == Approx(6.0).margin(0.10 * amp));

    // WITHOUT resample the old behaviour stands: the control values are kept, so
    // the surface is STRETCHED onto the new rectangle. Documented, not a bug -
    // some callers (and the phase 1 tests) want exactly that.
    CurvedCutSheet stretched = sheet;
    stretched.set_half_size(45.0, 20.0, /*resample*/ false);
    REQUIRE(stretched.values() == sheet.values());
    // The whole bump is now spread over a 45 mm half extent instead of 30, so
    // at a fixed 30 mm out the stretched sheet reads what the original read at
    // 20 mm - i.e. much HIGHER, because the bump has been pulled outwards with
    // the rectangle. That is exactly the drift the `resample` flag exists to
    // prevent, and it is what the fitted sheet must never do.
    REQUIRE(stretched.evaluate_local(30.0, 0.0) > sheet.evaluate_local(30.0, 0.0));
    REQUIRE(stretched.evaluate_local(30.0, 0.0) == Approx(sheet.evaluate_local(20.0, 0.0)).margin(1e-9));
    // ... whereas the RE-SAMPLED sheet still reads what the original read there.
    REQUIRE(grown.evaluate_local(30.0, 0.0) == Approx(sheet.evaluate_local(30.0, 0.0)).margin(0.10 * amp));

    // A flat sheet re-sampled is still exactly flat: the phase 1 invariant.
    CurvedCutSheet flat(5);
    flat.set_half_size(10.0, 3.0);
    flat.set_half_size(40.0, 25.0, /*resample*/ true);
    REQUIRE(flat.is_flat());
}

// ---------------------------------------------------------------------------
// (P2-2) The cross-section fit. A 40 x 20 x 10 box cut by a VERTICAL plane: the
// outline is 40 x 10 (or 20 x 10, depending which way the plane faces), and the
// sheet has to be that plus the margin - not the bbox diagonal phase 1 used.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the sheet fits the cut's cross-section", "[CurvedCut]")
{
    // A 40 x 20 x 10 box centred on the origin, already in the plane frame.
    auto box = [](double sx, double sy, double sz) {
        indexed_triangle_set its = its_make_cube(sx, sy, sz);
        for (Vec3f& v : its.vertices)
            v -= Vec3f(float(0.5 * sx), float(0.5 * sy), float(0.5 * sz));
        return its;
    };

    const double margin_rel = 0.15, margin_abs = 5.0;

    // A HORIZONTAL cut (the plane frame is the world frame): the section is the
    // 40 x 20 footprint.
    {
        const indexed_triangle_set b = box(40.0, 20.0, 10.0);
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        // half extents of the outline: 20 and 10.
        REQUIRE(hu == Approx(20.0 + std::max(0.15 * 20.0, 5.0)));   // 20 + 5   = 25
        REQUIRE(hv == Approx(10.0 + std::max(0.15 * 10.0, 5.0)));   // 10 + 5   = 15
        // And it is nothing like the bbox-diagonal extent phase 1 would use:
        // that is 0.5 * sqrt(40^2 + 20^2 + 10^2) ~ 22.9, scaled up again by the
        // plane's radius koef, and SQUARE - so on the short axis the handles
        // sat well off the part.
        REQUIRE(hv < 0.5 * Vec3d(40, 20, 10).norm());
    }

    // A VERTICAL cut: rotate the box 90 degrees about X, so the plane z == 0 now
    // slices it lengthways and the section is 40 x 10.
    {
        indexed_triangle_set b = box(40.0, 20.0, 10.0);
        its_transform(b, Geometry::rotation_transform(0.5 * PI * Vec3d::UnitX()));
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(20.0 + 5.0));   // the 40 mm axis is untouched by an X rotation
        REQUIRE(hv == Approx(5.0 + 5.0));    // the 10 mm axis has rotated into the plane
    }

    // A LARGE section takes the relative margin, not the absolute one.
    {
        const indexed_triangle_set b = box(200.0, 100.0, 10.0);
        double hu = 0.0, hv = 0.0;
        REQUIRE(curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(100.0 * 1.15));
        REQUIRE(hv == Approx(50.0 * 1.15));
    }

    // A plane that MISSES the object reports failure, so the caller keeps the
    // extent it has instead of collapsing the sheet to nothing.
    {
        indexed_triangle_set b = box(40.0, 20.0, 10.0);
        its_translate(b, Vec3f(0.f, 0.f, 100.f));
        double hu = 1.0, hv = 1.0;
        REQUIRE(!curved_cut_fit_extent(b, hu, hv, margin_rel, margin_abs));
        REQUIRE(hu == Approx(1.0));   // untouched
        REQUIRE(hv == Approx(1.0));
    }

    // An empty mesh is a failure too, not a crash.
    {
        const indexed_triangle_set empty;
        double hu = 3.0, hv = 4.0;
        REQUIRE(!curved_cut_fit_extent(empty, hu, hv));
        REQUIRE(hu == Approx(3.0));
        REQUIRE(hv == Approx(4.0));
    }
}

// ---------------------------------------------------------------------------
// (P2-3) The snap helper: given a mesh and a handle position in the plane frame,
// the SIGNED local-Z distance to the nearest surface, in either direction.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the snap helper finds the nearest surface", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();   // 40 mm, faces at +-20

    SECTION("a cube") {
        double d = 0.0;

        // Above the top face: the nearest surface is the top, 5 mm DOWN.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 25.0), d));
        REQUIRE(d == Approx(-5.0));

        // Below the bottom face: the nearest is the bottom, 5 mm UP.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, -25.0), d));
        REQUIRE(d == Approx(5.0));

        // INSIDE, nearer the top: it snaps out to the top, not through to the
        // bottom. "Nearest hit in either direction" is the whole rule.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 12.0), d));
        REQUIRE(d == Approx(8.0));

        // Inside, nearer the bottom.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, -12.0), d));
        REQUIRE(d == Approx(-8.0));

        // Exactly on the top face: zero, not a jump to the far one.
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(0.0, 0.0, 20.0), d));
        REQUIRE(d == Approx(0.0).margin(1e-9));

        // Off to the side of the cube entirely: no hit, and the caller must
        // leave its control point alone.
        REQUIRE(!curved_cut_snap_distance(cube, Vec3d(100.0, 0.0, 0.0), d));

        // A handle over a corner region but still off the footprint.
        REQUIRE(!curved_cut_snap_distance(cube, Vec3d(21.0, 21.0, 0.0), d));
    }

    SECTION("a domed mesh") {
        // A spherical cap: z = sqrt(R^2 - r^2) - sqrt(R^2 - r_max^2) over a disc
        // of radius r_max, so the apex is at the centre and the rim comes down to
        // z == 0, closed there with a flat base disc. That is what a real STL of
        // a domed top looks like to the helper - a curved shell over a flat face,
        // with the shell nearer over the middle and the base nearer near the rim.
        const double R = 30.0, r_max = 20.0;
        const int    RINGS = 32, SEGS = 64;
        const double z_off = std::sqrt(R * R - r_max * r_max);
        auto dome_z = [R, z_off](double r) { return std::sqrt(std::max(0.0, R * R - r * r)) - z_off; };
        const double apex = dome_z(0.0);   // ~7.64

        indexed_triangle_set dome;
        // Vertex 0 is the apex; then RINGS rings out to the rim at r_max.
        dome.vertices.emplace_back(Vec3f(0.f, 0.f, float(apex)));
        for (int k = 1; k <= RINGS; ++ k) {
            const double r = r_max * double(k) / double(RINGS);
            for (int s = 0; s < SEGS; ++ s) {
                const double a = 2.0 * PI * double(s) / double(SEGS);
                dome.vertices.emplace_back(Vec3f(float(r * std::cos(a)), float(r * std::sin(a)), float(dome_z(r))));
            }
        }
        auto vid = [SEGS](int ring, int seg) { return 1 + (ring - 1) * SEGS + (seg % SEGS); };
        for (int s = 0; s < SEGS; ++ s)
            dome.indices.emplace_back(Vec3i32(0, vid(1, s), vid(1, s + 1)));
        for (int k = 1; k < RINGS; ++ k)
            for (int s = 0; s < SEGS; ++ s) {
                dome.indices.emplace_back(Vec3i32(vid(k, s), vid(k + 1, s), vid(k + 1, s + 1)));
                dome.indices.emplace_back(Vec3i32(vid(k, s), vid(k + 1, s + 1), vid(k, s + 1)));
            }
        // The flat base at z == 0: a fan from the centre out to the rim ring,
        // which really is at z == 0 now, so this is a disc and not a cone.
        const int base_c = int(dome.vertices.size());
        dome.vertices.emplace_back(Vec3f(0.f, 0.f, 0.f));
        for (int s = 0; s < SEGS; ++ s)
            dome.indices.emplace_back(Vec3i32(base_c, vid(RINGS, s + 1), vid(RINGS, s)));

        double d = 0.0;
        // Directly over the apex, 4 mm above it: the shell is 4 mm down, the
        // base further, so it snaps onto the shell.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, apex + 4.0), d));
        REQUIRE(d == Approx(-4.0).margin(1e-6));

        // Over the flank at r = 15, 2 mm above the shell. The MESH is not the
        // analytic surface - a query point between two rings hits a facet that
        // is a chord below the sphere - so the hit is checked against the shell's
        // neighbourhood rather than an exact figure. What matters is that it went
        // DOWN, onto the shell, and not through to the base.
        const double zr   = dome_z(15.0);   // ~5.99
        const double from = zr + 2.0;
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(15.0, 0.0, from), d));
        REQUIRE(d < 0.0);
        const double hit_z = from + d;
        INFO("flank hit at z = " << hit_z << " (analytic shell " << zr << ", apex " << apex << ")");
        REQUIRE(hit_z == Approx(zr).margin(0.15));   // on the shell, within a facet
        REQUIRE(hit_z > 1.0);                        // ... nowhere near the base

        // Near the rim at r = 19.5 the shell is almost down at the base, and a
        // handle just under the base plane snaps UP onto the base.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(19.5, 0.0, -0.3), d));
        REQUIRE(d == Approx(0.3).margin(1e-6));

        // Below the base under the middle: it snaps UP onto the base, not all
        // the way through to the shell above it.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(5.0, 0.0, -6.0), d));
        REQUIRE(d == Approx(6.0).margin(1e-6));

        // Inside the dome, nearer the base than the shell: it snaps DOWN.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, 1.0), d));
        REQUIRE(d == Approx(-1.0).margin(1e-6));

        // Inside, nearer the shell: it snaps UP.
        REQUIRE(curved_cut_snap_distance(dome, Vec3d(0.0, 0.0, apex - 1.0), d));
        REQUIRE(d == Approx(1.0).margin(1e-6));

        // Off the disc: no hit.
        REQUIRE(!curved_cut_snap_distance(dome, Vec3d(25.0, 0.0, 5.0), d));
    }
}

// ---------------------------------------------------------------------------
// (P2-4) The end-to-end gesture the gizmo performs: fit the sheet to a section,
// snap a handle onto the surface, and cut. The point is that the pieces compose
// - the fit does not flatten the sheet and the snap produces a real bend.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: fit then snap produces a real curved cut", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();   // 40 mm, faces at +-20

    double hu = 0.0, hv = 0.0;
    REQUIRE(curved_cut_fit_extent(cube, hu, hv, 0.15, 5.0));
    REQUIRE(hu == Approx(25.0));
    REQUIRE(hv == Approx(25.0));

    CurvedCutSheet sheet(5);
    sheet.set_half_size(hu, hv, /*resample*/ true);
    REQUIRE(sheet.is_flat());

    // Snap the centre handle onto the cube's TOP face. A handle sitting exactly
    // at z = 0 is 20 mm from both faces - a genuine tie, and which of the two a
    // tie resolves to is not a contract worth pinning - so lift it 1 mm first,
    // the way a user who wants the top would. The top is then 19 mm away and the
    // bottom 21, so the helper must report +19.
    // (The rim handles are at +-25, off the cube's 20 mm footprint, so they
    // report no hit - which is what leaves them where they are.)
    const Vec2d c = sheet.control_xy(2, 2);
    sheet.at(2, 2) = 1.0;
    double d = 0.0;
    REQUIRE(curved_cut_snap_distance(cube, Vec3d(c.x(), c.y(), sheet.at(2, 2)), d));
    REQUIRE(d == Approx(19.0));
    // A tie really is a tie: from dead centre it lands on one face or the other,
    // 20 mm away, and either is a correct answer.
    {
        double tie = 0.0;
        REQUIRE(curved_cut_snap_distance(cube, Vec3d(c.x(), c.y(), 0.0), tie));
        REQUIRE(std::abs(tie) == Approx(20.0));
    }
    sheet.at(2, 2) = 0.0;
    REQUIRE(sheet.is_flat());

    double corner = 0.0;
    const Vec2d rim = sheet.control_xy(0, 0);
    REQUIRE(!curved_cut_snap_distance(cube, Vec3d(rim.x(), rim.y(), 0.0), corner));

    // Apply it the way the gesture does, with the falloff pulling the
    // neighbours: a dome, not a spike. The sheet starts flat again (the 1 mm
    // lift above was undone), so the peak is the snap distance itself.
    sheet.grab(c, 1.5 * (2.0 * hu / 4.0), d, /*falloff*/ true);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() == Approx(19.0));

    // ... and it still cuts into two closed halves whose volumes add up. The
    // dome reaches to within 1 mm of the top face over the centre, so this is
    // also a near-tangent boolean - the case most likely to produce a sliver.
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    // 1e-4 relative, looser than the 1e-6 the phase 1 cube tests hold to,
    // because this is deliberately the near-tangent case: the dome comes to
    // within 1 mm of the top face, so the boolean's seam runs almost along that
    // face and the sliver it leaves is a facet of the 128-sample cutter thick.
    // Both halves are closed (checked above), so nothing leaked - the cutter is
    // discretised, and this is how much that costs.
    const double total = double(its_volume(upper)) + double(its_volume(lower));
    REQUIRE(std::abs(total - CUBE * CUBE * CUBE) / (CUBE * CUBE * CUBE) < 1e-4);
    // The sheet reaches almost to the top face over the middle, so the lower
    // half takes clearly more than half the cube - about 61% here. The bend
    // radius is 1.5 control spacings, so the dome covers the middle and the
    // sheet is still flat at the rim; it is not a raised lid over the whole
    // footprint, and the fraction says so.
    const double lower_frac = double(its_volume(lower)) / (CUBE * CUBE * CUBE);
    INFO("lower half is " << 100.0 * lower_frac << "% of the cube");
    REQUIRE(lower_frac > 0.55);
    REQUIRE(lower_frac < 0.75);
}

// ---------------------------------------------------------------------------
// (P2-5) The slab still covers a part LARGER than the sheet, now that the sheet
// is fitted to a cross-section and so is routinely smaller than the object.
// This is the phase 1 guarantee restated for a RECTANGULAR sheet: the widening
// is per axis and it must not move the surface.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a rectangular sheet's slab still covers a larger part", "[CurvedCut]")
{
    // A deliberately lopsided sheet: 18 mm along u, 7 mm along v, with a dome.
    CurvedCutSheet sheet(5);
    sheet.set_half_size(18.0, 7.0);
    sheet.at(2, 2) = 5.0;

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-60, -35, -20));
    bbox.merge(Vec3d(60, 35, 20));

    // Widened per axis, as curved_cut_split() does.
    const indexed_triangle_set slab = curved_cut_lower_slab(sheet, bbox, 128, 65.0, 40.0);
    REQUIRE(its_num_open_edges(slab) == 0);

    double ext_u = 0.0, ext_v = 0.0, peak = -1e30, peak_x = 0.0, peak_y = 0.0;
    for (const Vec3f& v : slab.vertices) {
        ext_u = std::max(ext_u, double(std::abs(v.x())));
        ext_v = std::max(ext_v, double(std::abs(v.y())));
        if (double(v.z()) > peak) { peak = double(v.z()); peak_x = double(v.x()); peak_y = double(v.y()); }
    }
    REQUIRE(ext_u >= 64.0);        // really widened past the object on u ...
    REQUIRE(ext_v >= 39.0);        // ... and on v, independently
    // ... and the dome kept its height at the sheet's own centre. (The sample
    // grid now straddles the peak, so a chord sag below 5 mm is expected.)
    REQUIRE(peak == Approx(5.0).margin(0.25));
    REQUIRE(std::abs(peak_x) < 3.0);
    REQUIRE(std::abs(peak_y) < 3.0);
    // Outside the sheet's own domain the border height (zero) is extruded, not
    // stretched: look only at the top surface, the floor sits far below.
    for (const Vec3f& v : slab.vertices)
        if (double(v.z()) > -10.0 && (double(std::abs(v.x())) > 22.0 || double(std::abs(v.y())) > 11.0))
            REQUIRE(std::abs(double(v.z())) < 0.01);

    // And the whole thing still cuts a part far larger than the sheet in two.
    indexed_triangle_set big = its_make_cube(100.0, 60.0, 30.0);
    its_translate(big, Vec3f(-50.f, -30.f, -15.f));
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(big, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    // 1e-4 relative, not the 1e-6 the 40 mm cube tests use: the slab here is
    // sampled at 128 across 130 mm, so its facets are ~1 mm and the boolean's
    // seam follows them. That is a discretisation of the CUTTER, not a leak -
    // both halves are closed (checked above) and nothing is lost between them
    // beyond a facet's worth.
    const double total = double(its_volume(upper)) + double(its_volume(lower));
    REQUIRE(std::abs(total - 100.0 * 60.0 * 30.0) / (100.0 * 60.0 * 30.0) < 1e-4);
}

// ---------------------------------------------------------------------------
// (P2-6) The resolution ceiling moved to 15, and a 15 x 15 grid still behaves:
// it re-samples, it evaluates, and it cuts.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a 15 x 15 control grid", "[CurvedCut]")
{
    REQUIRE(CurvedCutSheet::MaxResolution == 15);

    CurvedCutSheet sheet(5);
    sheet.set_half_size(20.0, 20.0);
    sheet.at(2, 2) = 6.0;
    const double before = sheet.evaluate(0.5, 0.5);
    REQUIRE(before == Approx(6.0));

    sheet.set_resolution(15);
    REQUIRE(sheet.resolution() == 15);
    REQUIRE(sheet.values().size() == 15u * 15u);
    // 5 -> 15 IS a refinement (u = 0, .25, .5, .75, 1 are all 15-grid nodes), so
    // the surface must come through essentially exactly.
    REQUIRE(sheet.evaluate(0.5, 0.5) == Approx(before).margin(1e-9));
    REQUIRE(sheet.max_displacement() == Approx(6.0).margin(1e-9));

    // Clamping still holds at the top of the range.
    CurvedCutSheet over(5);
    over.set_resolution(99);
    REQUIRE(over.resolution() == 15);

    // And a 15 x 15 sheet cuts.
    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(centred_cube(), sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
}

// ---------------------------------------------------------------------------
// (P2F-1) The owner's case: an ASYMMETRIC part, a rotated off-centre plane and
// a strongly bent S-shaped sheet, with BOTH halves kept. The report was that
// only one half survived - the smaller one vanished.
// ---------------------------------------------------------------------------

// A 30 x 20 x 15 box with one top corner chamfered off, centred on the origin.
// Asymmetric on purpose: a symmetric part cannot tell "the smaller half was
// dropped" from "the halves came out equal".
static indexed_triangle_set chamfered_box(double sx = 30.0, double sy = 20.0, double sz = 15.0)
{
    indexed_triangle_set its = its_make_cube(sx, sy, sz);
    its_translate(its, Vec3f(float(-0.5 * sx), float(-0.5 * sy), float(-0.5 * sz)));
    // Chamfer: pull the +x/+z edge's two top vertices inwards along x. Done by
    // moving vertices rather than by a boolean, so the mesh stays closed and the
    // helper has no dependency on the boolean under test.
    for (Vec3f& v : its.vertices)
        if (v.x() > float(0.5 * sx) - 1e-3f && v.z() > float(0.5 * sz) - 1e-3f)
            v.x() -= float(0.35 * sx);
    return its;
}

// An S-shaped sheet: one half of the grid pushed up, the other pushed down, so
// the surface swings hard through the part rather than doming gently.
static CurvedCutSheet s_sheet(double amp, double hs_u, double hs_v, int resolution = 5)
{
    CurvedCutSheet sheet(resolution);
    sheet.set_half_size(hs_u, hs_v);
    for (int j = 0; j < resolution; ++ j)
        for (int i = 0; i < resolution; ++ i) {
            const double u = sheet.control_u(i);
            sheet.at(i, j) = amp * std::sin(2.0 * PI * u);
        }
    return sheet;
}

// The volume of a chamfered_box(), computed from its own mesh so the expectation
// cannot drift from the helper.
static double chamfered_box_volume()
{
    return std::abs(double(its_volume(chamfered_box())));
}

TEST_CASE("Curved cut: an asymmetric part on a rotated plane keeps BOTH halves", "[CurvedCut]")
{
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "chamfer";
    mo->add_volume(TriangleMesh(chamfered_box()))->name = "chamfer_v";
    mo->add_instance();

    // A plane rotated about two axes and offset off centre along its own normal.
    const Transform3d rotation = Transform3d(Eigen::AngleAxisd(0.35 * PI, Vec3d::UnitX()) *
                                             Eigen::AngleAxisd(0.20 * PI, Vec3d::UnitY()));
    const Vec3d       offset   = rotation * Vec3d(0.0, 0.0, 2.5);
    const Transform3d cut_matrix = Geometry::translation_transform(offset) * rotation;

    // The sheet the gizmo would have after its phase-2 fit: sized to the cut's
    // own cross-section, not to the bounding-box diagonal.
    indexed_triangle_set in_plane = chamfered_box();
    its_transform(in_plane, cut_matrix.inverse());
    double hs_u = 0.0, hs_v = 0.0;
    REQUIRE(curved_cut_fit_extent(in_plane, hs_u, hs_v, 0.15, 5.0));
    INFO("fitted sheet half extents: " << hs_u << " x " << hs_v);

    CurvedCutSheet sheet = s_sheet(4.0, hs_u, hs_v);
    REQUIRE(!sheet.is_flat());
    REQUIRE(sheet.max_displacement() > 3.0);

    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
    Cut cut(mo, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);

    // THE BUG: only one part came back.
    REQUIRE(parts.size() == 2);

    double total = 0.0;
    for (const ModelObject* part : parts) {
        REQUIRE(part->volumes.size() == 1);
        const indexed_triangle_set& its = part->volumes.front()->mesh().its;
        REQUIRE(!its.empty());
        REQUIRE(its_num_open_edges(its) == 0);
        const double v = std::abs(double(its_volume(its)));
        INFO("half volume " << v);
        REQUIRE(v > 1.0);   // neither half is a sliver left over from a bad clip
        total += v;
    }
    const double whole = chamfered_box_volume();
    INFO("total " << total << " vs whole " << whole);
    REQUIRE(std::abs(total - whole) / whole < 1e-4);
}

// ---------------------------------------------------------------------------
// (P2F-2) The same split at the mesh level, plus the inverted-winding input the
// report suspected: a flipped-normal object must not make the "inside" test
// invert and swallow a half.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: inverted winding still gives two halves", "[CurvedCut]")
{
    indexed_triangle_set box = chamfered_box();
    const double whole = std::abs(double(its_volume(box)));
    REQUIRE(its_volume(box) > 0.f);      // the helper is wound outwards

    CurvedCutSheet sheet = s_sheet(4.0, 22.0, 16.0);

    // Baseline: the correctly wound mesh.
    {
        indexed_triangle_set upper, lower;
        REQUIRE(curved_cut_split(box, sheet, &upper, &lower));
        REQUIRE(!upper.empty());
        REQUIRE(!lower.empty());
        REQUIRE(its_num_open_edges(upper) == 0);
        REQUIRE(its_num_open_edges(lower) == 0);
        const double total = std::abs(double(its_volume(upper))) + std::abs(double(its_volume(lower)));
        REQUIRE(std::abs(total - whole) / whole < 1e-4);
    }

    // Flipped: every triangle's winding reversed, so its_volume() goes negative
    // and every face normal points into the solid.
    indexed_triangle_set flipped = box;
    for (Vec3i32& t : flipped.indices)
        std::swap(t(1), t(2));
    REQUIRE(its_volume(flipped) < 0.f);

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(flipped, sheet, &upper, &lower));
    REQUIRE(!upper.empty());
    REQUIRE(!lower.empty());
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    const double total = std::abs(double(its_volume(upper))) + std::abs(double(its_volume(lower)));
    INFO("flipped total " << total << " vs whole " << whole);
    REQUIRE(std::abs(total - whole) / whole < 1e-4);
}

// ---------------------------------------------------------------------------
// (P2F-3) The other keep modes on the same asymmetric case.
// ---------------------------------------------------------------------------

static double cut_total_volume(ModelObjectCutAttributes attributes, const CurvedCutSheet& sheet,
                               const Transform3d& cut_matrix, size_t& n_objects, size_t& n_volumes)
{
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "chamfer";
    mo->add_volume(TriangleMesh(chamfered_box()))->name = "chamfer_v";
    mo->add_instance();

    Cut cut(mo, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    n_objects = parts.size();
    n_volumes = 0;
    double total = 0.0;
    for (const ModelObject* part : parts) {
        n_volumes += part->volumes.size();
        for (const ModelVolume* v : part->volumes)
            total += std::abs(double(its_volume(v->mesh().its)));
    }
    return total;
}

TEST_CASE("Curved cut: keep-one and cut-to-parts on an asymmetric part", "[CurvedCut]")
{
    const Transform3d rotation = Transform3d(Eigen::AngleAxisd(0.35 * PI, Vec3d::UnitX()) *
                                             Eigen::AngleAxisd(0.20 * PI, Vec3d::UnitY()));
    const Transform3d cut_matrix = Geometry::translation_transform(rotation * Vec3d(0.0, 0.0, 2.5)) * rotation;
    const CurvedCutSheet sheet = s_sheet(4.0, 22.0, 16.0);
    const double whole = chamfered_box_volume();

    size_t n_obj = 0, n_vol = 0;

    // Both: two objects, one volume each, volume conserved.
    const double both = cut_total_volume(ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower,
                                         sheet, cut_matrix, n_obj, n_vol);
    REQUIRE(n_obj == 2);
    REQUIRE(n_vol == 2);
    REQUIRE(std::abs(both - whole) / whole < 1e-4);

    // Upper only.
    const double up = cut_total_volume(ModelObjectCutAttribute::KeepUpper, sheet, cut_matrix, n_obj, n_vol);
    REQUIRE(n_obj == 1);
    REQUIRE(n_vol == 1);
    REQUIRE(up > 1.0);
    REQUIRE(up < whole);

    // Lower only.
    const double lo = cut_total_volume(ModelObjectCutAttribute::KeepLower, sheet, cut_matrix, n_obj, n_vol);
    REQUIRE(n_obj == 1);
    REQUIRE(n_vol == 1);
    REQUIRE(lo > 1.0);
    REQUIRE(lo < whole);

    // The two one-sided runs reproduce the two-sided split.
    INFO("upper " << up << " + lower " << lo << " vs whole " << whole);
    REQUIRE(std::abs(up + lo - whole) / whole < 1e-4);

    // Cut to parts: ONE object holding BOTH halves as separate volumes.
    const double parts = cut_total_volume(ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts,
                                          sheet, cut_matrix, n_obj, n_vol);
    REQUIRE(n_obj == 1);
    REQUIRE(n_vol == 2);
    REQUIRE(std::abs(parts - whole) / whole < 1e-4);
}

// ---------------------------------------------------------------------------
// (P2F-4) Side visibility: the alpha the gizmo hands the shader per state.
// Nobody can look at the scratch instance, so the contract is pinned here
// instead - Visible solid, Ghost translucent, Hidden negative (a discard, not a
// zero alpha, because a zero-alpha fragment still writes depth).
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: side visibility alpha", "[CurvedCut]")
{
    REQUIRE(curved_cut_side_alpha(CurvedCutSideVisibility::Visible) == Approx(1.f));
    REQUIRE(curved_cut_side_alpha(CurvedCutSideVisibility::Ghost)   == Approx(0.25f));
    REQUIRE(curved_cut_side_alpha(CurvedCutSideVisibility::Hidden)  < 0.f);

    // Ghost is what the two-pass draw keys off: strictly between 0 and 1.
    const float g = curved_cut_side_alpha(CurvedCutSideVisibility::Ghost);
    REQUIRE(g > 0.f);
    REQUIRE(g < 1.f);
    REQUIRE(curved_cut_side_is_ghost(g));
    REQUIRE(!curved_cut_side_is_ghost(curved_cut_side_alpha(CurvedCutSideVisibility::Visible)));
    REQUIRE(!curved_cut_side_is_ghost(curved_cut_side_alpha(CurvedCutSideVisibility::Hidden)));

    // And the pass split: with one side ghosted the opaque pass must still draw
    // the OTHER side, and the ghost pass only the ghosted one.
    const float vis = curved_cut_side_alpha(CurvedCutSideVisibility::Visible);
    REQUIRE(curved_cut_has_ghost_side(vis, g));
    REQUIRE(curved_cut_has_ghost_side(g, vis));
    REQUIRE(!curved_cut_has_ghost_side(vis, vis));
    REQUIRE(!curved_cut_has_ghost_side(vis, curved_cut_side_alpha(CurvedCutSideVisibility::Hidden)));
}

// ---------------------------------------------------------------------------
// (P2F-5) Demo export for the asymmetric rotated case.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: asymmetric demo export", "[CurvedCut][.demo]")
{
    const char* dir = std::getenv("EDGESLICER_CURVED_CUT_DEMO_DIR");
    if (dir == nullptr || dir[0] == 0)
        return;

    const boost::filesystem::path out = boost::filesystem::path(dir) / "asym";
    boost::filesystem::create_directories(out);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "chamfer";
    mo->add_volume(TriangleMesh(chamfered_box()))->name = "chamfer_v";
    mo->add_instance();

    const Transform3d rotation = Transform3d(Eigen::AngleAxisd(0.35 * PI, Vec3d::UnitX()) *
                                             Eigen::AngleAxisd(0.20 * PI, Vec3d::UnitY()));
    const Transform3d cut_matrix = Geometry::translation_transform(rotation * Vec3d(0.0, 0.0, 2.5)) * rotation;
    const CurvedCutSheet sheet = s_sheet(4.0, 22.0, 16.0);

    ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower;
    Cut cut(mo, 0, cut_matrix, attributes);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(sheet);
    REQUIRE(parts.size() == 2);

    for (size_t i = 0; i < parts.size(); ++ i) {
        TriangleMesh m(parts[i]->volumes.front()->mesh());
        m.transform(parts[i]->volumes.front()->get_matrix());
        its_write_stl_ascii((out / (i == 0 ? "upper.stl" : "lower.stl")).string().c_str(),
                            i == 0 ? "curved_cut_upper" : "curved_cut_lower", m.its);
    }
}

// ---------------------------------------------------------------------------
// (P2F-6) The ONE case that really does yield a single half, and what it must
// do about it.
//
// Hunting the owner's "only one half survives" report turned up exactly one
// mechanism that empties a half, and it is geometric rather than a bug in the
// boolean: a sheet that lies entirely ABOVE (or entirely BELOW) the part does
// not cross it, so one side is genuinely empty. curved_cut_lower_slab() extrudes
// the sheet's BORDER height outwards past the sheet's own domain, so once the
// border clears the part's top the whole object is inside the slab and the upper
// half is nothing - which is the right answer, not a lost half.
//
// What was wrong is what happened NEXT: the split cleared that side and returned,
// and downstream an empty mesh is indistinguishable from "this half does not
// exist" (add_cut_volume() returns early, the cloned ModelObject has no volumes,
// post_process() drops it). The user got one part back with nothing to say why.
// So the contract pinned here is: the split REPORTS the failure (ok == false),
// the surviving half is the whole part, and the recovery does not invent
// geometry to paper over it.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a sheet clear of the part reports the empty half", "[CurvedCut]")
{
    indexed_triangle_set box = chamfered_box();          // z in [-7.5, +7.5]
    const double whole = std::abs(double(its_volume(box)));

    double hs_u = 0.0, hs_v = 0.0;
    REQUIRE(curved_cut_fit_extent(box, hs_u, hs_v, 0.15, 5.0));

    // A sheet raised bodily above the part's top face. Every control point is at
    // +9, so the surface - and the border the slab extrudes outwards - is at +9,
    // clear of the +7.5 top.
    CurvedCutSheet high(5);
    high.set_half_size(hs_u, hs_v);
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            high.at(i, j) = 9.0;
    REQUIRE(!high.is_flat());

    indexed_triangle_set upper, lower;
    const bool ok = curved_cut_split(box, high, &upper, &lower);

    // The empty half is REPORTED, not swallowed.
    REQUIRE(!ok);
    // The lower half is the whole part - the sheet is above everything.
    REQUIRE(!lower.empty());
    REQUIRE(std::abs(double(its_volume(lower))) == Approx(whole).epsilon(1e-4));
    // ... and nothing was invented for the upper. The complement recovery runs
    // (object - lower) and correctly gets nothing, rather than manufacturing a
    // sliver that would print as a stray shell.
    REQUIRE(upper.empty());

    // Mirrored: a sheet pushed bodily BELOW the part.
    CurvedCutSheet low(5);
    low.set_half_size(hs_u, hs_v);
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i)
            low.at(i, j) = -10.0;

    indexed_triangle_set upper2, lower2;
    const bool ok2 = curved_cut_split(box, low, &upper2, &lower2);
    REQUIRE(!ok2);
    REQUIRE(!upper2.empty());
    REQUIRE(std::abs(double(its_volume(upper2))) == Approx(whole).epsilon(1e-4));
    REQUIRE(lower2.empty());

    // And the boundary: a sheet that still CROSSES the part keeps both halves,
    // however hard it is bent. This is the guard that says the extrusion of the
    // border is only ever a problem once the sheet has left the part entirely.
    for (double h : { 4.0, 8.0, 10.0, 14.0 }) {
        CurvedCutSheet bent(5);
        bent.set_half_size(hs_u, hs_v);
        for (int j = 0; j < 5; ++ j)
            for (int i = 3; i < 5; ++ i)   // the +u half, border column included
                bent.at(i, j) = h;
        indexed_triangle_set u, l;
        INFO("one-sided bend of " << h << " mm");
        REQUIRE(curved_cut_split(box, bent, &u, &l));
        REQUIRE(!u.empty());
        REQUIRE(!l.empty());
        const double total = std::abs(double(its_volume(u))) + std::abs(double(its_volume(l)));
        // 5e-4 relative, not the 1e-4 the gentler cases hold: a 14 mm bend over a
        // 17 mm half-extent is steep enough that the slab's 128-sample facets are
        // a visible fraction of a millimetre where they cross the part, and the
        // boolean's seam follows those facets. That is a discretisation of the
        // CUTTER, not a leak - both halves are closed (checked above) and nothing
        // is lost between them beyond a facet's worth.
        REQUIRE(std::abs(total - whole) / whole < 5e-4);
    }
}

// ---------------------------------------------------------------------------
// (P3-1) The re-fit contract, NOW HONOURED.
//
// set_half_size(..., resample=true) is documented to keep the SURFACE fixed in
// the plane while the rectangle around it changes. Phase 2 met that for ONE
// re-fit but not for a stream of them: each re-sample read the surface through
// Catmull-Rom and wrote control values back, and feeding each re-sample its
// predecessor's output compounded the loss without bound over a plane drag.
// This test used to pin that drift; phase 3 makes the re-sample read a stored
// REFERENCE grid instead (see CurvedCutSheet::set_half_size), so it is now
// idempotent and the test asserts the contract rather than the shortfall.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: repeated re-fits do not drift the surface", "[CurvedCut]")
{
    CurvedCutSheet sheet(5);
    sheet.set_half_size(17.375, 15.0);
    sheet.at(2, 2) = 6.0;
    // at() is a raw accessor, so tell the sheet this is the shape to remember.
    sheet.commit_reference();

    const double centre0 = sheet.evaluate(0.5, 0.5);
    const double border0 = sheet.evaluate(0.0, 0.5);
    REQUIRE(centre0 == Approx(6.0));
    REQUIRE(border0 == Approx(0.0).margin(1e-9));

    // ONE re-fit, out and back, now returns EXACTLY where it started: the second
    // call re-samples the reference at the original extent, which reproduces the
    // reference values themselves.
    {
        CurvedCutSheet s = sheet;
        s.set_half_size(17.375 * 0.7, 15.0 * 0.7, true);
        s.set_half_size(17.375, 15.0, true);
        REQUIRE(s.evaluate(0.5, 0.5) == Approx(centre0).margin(1e-9));
        REQUIRE(s.evaluate(0.0, 0.5) == Approx(border0).margin(1e-9));
        for (size_t k = 0; k < s.values().size(); ++ k)
            REQUIRE(s.values()[k] == Approx(sheet.values()[k]).margin(1e-12));
    }

    // TWELVE alternating re-fits, as a plane drag produces. The proof bar's
    // number: back at the starting extent, every control value within 1e-6.
    {
        CurvedCutSheet s = sheet;
        for (int k = 0; k < 12; ++ k) {
            const double f = (k % 2 == 0) ? 0.7 : 1.0 / 0.7;
            s.set_half_size(s.half_size_u() * f, s.half_size_v() * f, true);
        }
        INFO("after 12 re-fits: hs " << s.half_size_u() << " centre " << s.evaluate(0.5, 0.5)
             << " border " << s.evaluate(0.0, 0.5) << " max_disp " << s.max_displacement());
        REQUIRE(s.half_size_u() == Approx(17.375).epsilon(1e-9));
        REQUIRE(s.evaluate(0.5, 0.5) == Approx(centre0).margin(1e-9));
        REQUIRE(s.max_displacement() == Approx(6.0).margin(1e-9));
        const double drift = std::abs(s.evaluate(0.0, 0.5) - border0);
        INFO("border drift after 12 re-fits: " << drift << " mm");
        REQUIRE(drift < 1e-6);
        for (size_t k = 0; k < s.values().size(); ++ k)
            REQUIRE(std::abs(s.values()[k] - sheet.values()[k]) < 1e-6);
    }

    // Re-fitting TWICE to the same NEW extent is a no-op the second time: the
    // idempotence the drag actually leans on, since a fit that recomputes the
    // same extent must not keep moving the surface.
    {
        CurvedCutSheet a = sheet;
        a.set_half_size(24.0, 21.0, true);
        const std::vector<double> once = a.values();
        a.set_half_size(24.0 + 1e-12, 21.0, true);   // force the early-out to miss
        a.set_half_size(24.0, 21.0, true);
        for (size_t k = 0; k < once.size(); ++ k)
            REQUIRE(std::abs(a.values()[k] - once[k]) < 1e-9);
    }

    // An EDIT republishes the reference, so the surface the user just drew - not
    // some ancestor of it - is what later re-fits preserve.
    {
        CurvedCutSheet s = sheet;
        s.set_half_size(30.0, 26.0, true);
        s.grab(s.control_xy(1, 1), 12.0, 3.0, /*falloff*/ true);
        const std::vector<double> after_edit = s.values();
        s.set_half_size(12.0, 10.0, true);
        s.set_half_size(30.0, 26.0, true);
        for (size_t k = 0; k < after_edit.size(); ++ k)
            REQUIRE(std::abs(s.values()[k] - after_edit[k]) < 1e-9);
    }
}

// ---------------------------------------------------------------------------
// (P3-2) The fit covers the WHOLE part, not the cross-section.
//
// The failure it fixes: beyond the sheet's domain the cut slab extrudes the
// sheet's RIM height outwards, so a part that is wider above or below the plane
// than it is AT the plane got cut by that extruded rim - and a strongly bent
// sheet could take the rim clear off the part, leaving one side empty (the
// owner's "it removes the smaller part").
// ---------------------------------------------------------------------------

// A T: a narrow stem from z = -15 to 0, a wide flange from z = 0 to +10.
// A plane at z = -7 crosses only the STEM, so a cross-section fit sizes the
// sheet to the stem while the flange hangs far outside it.
static indexed_triangle_set t_part(double stem = 10.0, double flange = 60.0)
{
    indexed_triangle_set s = its_make_cube(stem, stem, 15.0);
    for (Vec3f& v : s.vertices)
        v -= Vec3f(float(0.5 * stem), float(0.5 * stem), 15.f);
    indexed_triangle_set f = its_make_cube(flange, flange, 10.0);
    for (Vec3f& v : f.vertices)
        v -= Vec3f(float(0.5 * flange), float(0.5 * flange), 0.f);
    its_merge(s, f);
    return s;
}

TEST_CASE("Curved cut: the fit covers the whole projection", "[CurvedCut]")
{
    indexed_triangle_set t = t_part();
    // The plane frame: z == 0 is the cut. Put the cut through the stem by moving
    // the part up 7 mm, so the section is the 10 mm stem and the 60 mm flange is
    // 7 mm above it.
    its_translate(t, Vec3f(0.f, 0.f, 7.f));

    double sec_u = 0.0, sec_v = 0.0;
    REQUIRE(curved_cut_fit_extent(t, sec_u, sec_v, 0.15, 5.0));
    // The cross-section fit sees the 10 mm stem: 5 + 5 = 10 mm half extent.
    REQUIRE(sec_u == Approx(10.0));
    REQUIRE(sec_v == Approx(10.0));

    double prj_u = 0.0, prj_v = 0.0;
    REQUIRE(curved_cut_fit_projection_extent(t, prj_u, prj_v, 0.15, 5.0));
    // The projection fit sees the 60 mm flange: 30 + max(0.15*30, 5) = 34.5.
    // 30 mm half width + max(0.15 * 30, 5) = 30 + 5 = 35.
    REQUIRE(prj_u == Approx(35.0));
    REQUIRE(prj_v == Approx(35.0));
    // The handles span the wide top, which is the whole point.
    REQUIRE(prj_u > 0.5 * 60.0);

    // And it MATTERS: a sheet fitted to the section, bent hard, loses a half;
    // the same bend on the projection-fitted sheet keeps both.
    const double whole = std::abs(double(its_volume(t)));

    // A dome: the centre raised, the RIM raised too - the shape a user gets after
    // dragging a handle with the bend radius covering the sheet. What matters for
    // this test is that the rim is well above the plane, because it is the rim
    // that gets extruded outwards beyond the sheet's own domain.
    auto bent = [](double hs_u, double hs_v, double amp) {
        CurvedCutSheet s(5);
        s.set_half_size(hs_u, hs_v);
        for (int j = 0; j < 5; ++ j)
            for (int i = 0; i < 5; ++ i)
                s.at(i, j) = amp;
        s.commit_reference();
        return s;
    };

    // The part in the plane frame: stem z in [-8, +7], flange z in [+7, +17].
    {
        // SECTION-FITTED, the phase 2 behaviour. The sheet is only 10 mm wide (the
        // STEM's cross-section), and outside that domain its rim height is
        // extruded straight out - so over the 60 mm flange the cutter is a flat
        // shelf at +20, CLEAR ABOVE the part's +17 top. The upper half comes back
        // empty: a bend the user drew over the stem has silently taken the whole
        // flange into the bottom half. This is the owner's "it removes the
        // smaller part".
        indexed_triangle_set u, l;
        const CurvedCutSheet s = bent(sec_u, sec_v, 20.0);
        const bool ok = curved_cut_split(t, s, &u, &l);
        INFO("section fit: upper " << its_volume(u) << " lower " << its_volume(l) << " whole " << whole);
        REQUIRE(!ok);
        REQUIRE(u.empty());
        REQUIRE(std::abs(double(its_volume(l))) == Approx(whole).epsilon(2e-3));
        // And the cheap test the panel warns from agrees, before any boolean runs.
        bool ue = false, le = false;
        curved_cut_empty_sides(t, s, ue, le);
        REQUIRE(ue);
        REQUIRE(!le);
    }
    {
        // PROJECTION-FITTED, phase 3. The domain now covers the whole flange, so
        // the surface the user drew is what cuts everywhere and nothing is decided
        // by an extruded rim. A level sheet at +20 is still above the part, so this
        // one is empty too - correctly, and for a reason the user can see (the
        // handles are over the flange, visibly above it). The interesting case is
        // a HEIGHT THE PART REACHES, where the projection fit keeps both halves
        // that the section fit would have lost.
        indexed_triangle_set u, l;
        const CurvedCutSheet s = bent(prj_u, prj_v, 12.0);
        REQUIRE(curved_cut_split(t, s, &u, &l));
        const double up = std::abs(double(its_volume(u)));
        const double lo = std::abs(double(its_volume(l)));
        INFO("projection fit: upper " << up << " lower " << lo << " whole " << whole);
        REQUIRE(up > 0.1 * whole);
        REQUIRE(lo > 0.1 * whole);
        REQUIRE(up + lo == Approx(whole).epsilon(3e-3));
        bool ue = true, le = true;
        curved_cut_empty_sides(t, s, ue, le);
        REQUIRE(!ue);
        REQUIRE(!le);

        // A RAMP, which is where the two fits part company on shape rather than on
        // emptiness. The same surface z = 0.4 * x is expressed on both sheets; on
        // the section-fitted one it can only be drawn over |x| <= 10 and is then
        // extruded FLAT at +/-4 over the rest of the flange, while the
        // projection-fitted one carries the ramp right across it.
        //
        // Sample the CUTTER both sheets present at the flange's outer edge: the
        // section-fitted sheet says +4 there (its rim value, extruded), the
        // projection-fitted one says the ramp's own 0.4 * 30 = 12.
        auto ramp = [](double hs_u, double hs_v) {
            CurvedCutSheet s(5);
            s.set_half_size(hs_u, hs_v);
            for (int j = 0; j < 5; ++ j)
                for (int i = 0; i < 5; ++ i)
                    s.at(i, j) = 0.4 * s.control_xy(i, j).x();
            s.commit_reference();
            return s;
        };
        const CurvedCutSheet r_sec = ramp(sec_u, sec_v);
        const CurvedCutSheet r_prj = ramp(prj_u, prj_v);
        INFO("at x = 30: section fit says " << r_sec.evaluate_local(30.0, 0.0)
             << ", projection fit says " << r_prj.evaluate_local(30.0, 0.0));
        REQUIRE(r_sec.evaluate_local(30.0, 0.0) == Approx(0.4 * sec_u).margin(1e-6));   // clamped at the rim
        // The ramp itself, to within Catmull-Rom's own overshoot between nodes
        // (a 5-point grid does not reproduce a line exactly off its nodes).
        REQUIRE(r_prj.evaluate_local(30.0, 0.0) == Approx(12.0).margin(0.6));
        // Three times what the clamped rim gave: this is the whole difference the
        // fit change makes over a part that hangs outside the cross-section.
        REQUIRE(r_prj.evaluate_local(30.0, 0.0) > 2.5 * r_sec.evaluate_local(30.0, 0.0));
        // Over the STEM, where both sheets have a domain, they agree - the fit
        // change moves nothing the user had already drawn over the cut.
        for (double x : { -4.0, 0.0, 4.0 })
            REQUIRE(r_sec.evaluate_local(x, 0.0) == Approx(r_prj.evaluate_local(x, 0.0)).margin(1e-9));
    }
}

TEST_CASE("Curved cut: the default resolution follows the extent", "[CurvedCut]")
{
    // ~10 mm spacing, clamped into [5, MaxResolution].
    REQUIRE(curved_cut_default_resolution(10.0, 10.0, 10.0, 5) == 5);    // 20 mm span -> 3, clamped up
    REQUIRE(curved_cut_default_resolution(30.0, 20.0, 10.0, 5) == 7);    // 60 mm span -> 7
    REQUIRE(curved_cut_default_resolution(50.0, 10.0, 10.0, 5) == 11);   // 100 mm span -> 11
    REQUIRE(curved_cut_default_resolution(200.0, 200.0, 10.0, 5) == CurvedCutSheet::MaxResolution);
    REQUIRE(CurvedCutSheet::MaxResolution == 15);
    // The spacing it actually lands on, for the sizes a real part gives.
    for (double hs : { 15.0, 25.0, 40.0, 60.0 }) {
        const int    n  = curved_cut_default_resolution(hs, hs, 10.0, 5);
        const double sp = 2.0 * hs / double(n - 1);
        INFO("hs " << hs << " -> n " << n << " spacing " << sp);
        REQUIRE(sp >= 5.0);
        REQUIRE(sp <= 15.0);
    }
}

// ---------------------------------------------------------------------------
// (P3-3) The empty-side test the panel warns from.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: the empty-side test agrees with the split", "[CurvedCut]")
{
    const indexed_triangle_set box = chamfered_box();     // z in [-7.5, +7.5]
    double hs_u = 0.0, hs_v = 0.0;
    REQUIRE(curved_cut_fit_projection_extent(box, hs_u, hs_v, 0.15, 5.0));

    auto level = [&](double h) {
        CurvedCutSheet s(5);
        s.set_half_size(hs_u, hs_v);
        for (int j = 0; j < 5; ++ j)
            for (int i = 0; i < 5; ++ i)
                s.at(i, j) = h;
        s.commit_reference();
        return s;
    };

    // A sheet raised clear ABOVE the part: nothing above it.
    {
        bool ue = false, le = false;
        curved_cut_empty_sides(box, level(9.0), ue, le);
        REQUIRE(ue);
        REQUIRE(!le);
        // ... and the split agrees.
        indexed_triangle_set u, l;
        REQUIRE(!curved_cut_split(box, level(9.0), &u, &l));
        REQUIRE(u.empty());
        REQUIRE(!l.empty());
    }
    // Pushed clear BELOW.
    {
        bool ue = false, le = false;
        curved_cut_empty_sides(box, level(-10.0), ue, le);
        REQUIRE(!ue);
        REQUIRE(le);
        indexed_triangle_set u, l;
        REQUIRE(!curved_cut_split(box, level(-10.0), &u, &l));
        REQUIRE(!u.empty());
        REQUIRE(l.empty());
    }
    // Through the middle: neither side empty.
    {
        bool ue = true, le = true;
        curved_cut_empty_sides(box, level(0.0), ue, le);
        REQUIRE(!ue);
        REQUIRE(!le);
    }
    // A bent sheet that still crosses the part: still neither.
    {
        CurvedCutSheet s(5);
        s.set_half_size(hs_u, hs_v);
        for (int j = 0; j < 5; ++ j)
            for (int i = 3; i < 5; ++ i)
                s.at(i, j) = 6.0;
        s.commit_reference();
        bool ue = true, le = true;
        curved_cut_empty_sides(box, s, ue, le);
        REQUIRE(!ue);
        REQUIRE(!le);
    }
    // A KERF wide enough to eat the part reports both sides empty. The box is
    // 15 mm tall, so a 40 mm band centred on the mid plane leaves nothing.
    {
        bool ue = false, le = false;
        curved_cut_empty_sides(box, level(0.0), ue, le, 40.0);
        REQUIRE(ue);
        REQUIRE(le);
    }
    // A kerf taken entirely from ABOVE leaves the lower half intact.
    {
        bool ue = false, le = false;
        curved_cut_empty_sides(box, level(0.0), ue, le, 40.0, CutThicknessOffset::Above);
        REQUIRE(ue);
        REQUIRE(!le);
    }
}

// ---------------------------------------------------------------------------
// (P3-4) The face offsets a thickness produces.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: thickness face offsets", "[CurvedCut]")
{
    double lo = 9.0, hi = 9.0;
    curved_cut_thickness_faces(0.0, CutThicknessOffset::Centred, lo, hi);
    REQUIRE(lo == Approx(0.0));
    REQUIRE(hi == Approx(0.0));

    curved_cut_thickness_faces(2.0, CutThicknessOffset::Centred, lo, hi);
    REQUIRE(lo == Approx(-1.0));
    REQUIRE(hi == Approx(1.0));

    curved_cut_thickness_faces(2.0, CutThicknessOffset::Above, lo, hi);
    REQUIRE(lo == Approx(0.0));
    REQUIRE(hi == Approx(2.0));

    curved_cut_thickness_faces(2.0, CutThicknessOffset::Below, lo, hi);
    REQUIRE(lo == Approx(-2.0));
    REQUIRE(hi == Approx(0.0));

    // Negatives clamp to zero rather than inverting the band.
    curved_cut_thickness_faces(-3.0, CutThicknessOffset::Centred, lo, hi);
    REQUIRE(lo == Approx(0.0));
    REQUIRE(hi == Approx(0.0));

    REQUIRE(CutThicknessMin == Approx(0.0));
    REQUIRE(CutThicknessMax == Approx(20.0));
}

// ---------------------------------------------------------------------------
// (P3-5) Cut thickness, FLAT. Two half-space slices instead of one, and t == 0
// bit-identical to the no-thickness path.
// ---------------------------------------------------------------------------

static void flat_cut_halves(double thickness, CutThicknessOffset offset,
                            indexed_triangle_set& upper, indexed_triangle_set& lower)
{
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    mo->add_instance();

    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_plane(thickness, offset);
    REQUIRE(parts.size() == 2);
    REQUIRE(parts[0]->volumes.size() == 1);
    REQUIRE(parts[1]->volumes.size() == 1);
    upper = parts[0]->volumes.front()->mesh().its;
    lower = parts[1]->volumes.front()->mesh().its;
}

TEST_CASE("Curved cut: a flat cut with a thickness removes a slab", "[CurvedCut]")
{
    const double whole = CUBE * CUBE * CUBE;

    // t == 0 is BIT-IDENTICAL to the no-thickness call: same counts, same
    // coordinates. This is the Bar-A-style guard that the kerf did not disturb
    // the path everybody already uses.
    {
        indexed_triangle_set u0, l0, u1, l1;
        flat_cut_halves(0.0, CutThicknessOffset::Centred, u0, l0);
        {
            Model model;
            ModelObject* mo = model.add_object();
            mo->name = "cube";
            mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
            mo->add_instance();
            Cut cut(mo, 0, Transform3d::Identity(),
                    ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
            const ModelObjectPtrs& parts = cut.perform_with_plane();   // no argument at all
            REQUIRE(parts.size() == 2);
            u1 = parts[0]->volumes.front()->mesh().its;
            l1 = parts[1]->volumes.front()->mesh().its;
        }
        REQUIRE(u0.vertices.size() == u1.vertices.size());
        REQUIRE(u0.indices.size()  == u1.indices.size());
        REQUIRE(l0.vertices.size() == l1.vertices.size());
        REQUIRE(l0.indices.size()  == l1.indices.size());
        for (size_t k = 0; k < u0.vertices.size(); ++ k)
            REQUIRE(u0.vertices[k] == u1.vertices[k]);
        for (size_t k = 0; k < u0.indices.size(); ++ k)
            REQUIRE(u0.indices[k] == u1.indices[k]);
        for (size_t k = 0; k < l0.vertices.size(); ++ k)
            REQUIRE(l0.vertices[k] == l1.vertices[k]);
        for (size_t k = 0; k < l0.indices.size(); ++ k)
            REQUIRE(l0.indices[k] == l1.indices[k]);
    }

    // t == 2 on a 40 mm cube: the halves' volumes sum to 64000 - 40*40*2.
    {
        indexed_triangle_set u, l;
        flat_cut_halves(2.0, CutThicknessOffset::Centred, u, l);
        const double up = std::abs(double(its_volume(u)));
        const double lo = std::abs(double(its_volume(l)));
        const double want = whole - CUBE * CUBE * 2.0;
        INFO("upper " << up << " lower " << lo << " sum " << (up + lo) << " want " << want);
        REQUIRE(std::abs(up + lo - want) / want < 1e-4);
        // Symmetric, so each half is 19 mm tall.
        REQUIRE(up == Approx(CUBE * CUBE * 19.0).epsilon(1e-4));
        REQUIRE(lo == Approx(CUBE * CUBE * 19.0).epsilon(1e-4));
        // Both closed.
        REQUIRE(its_num_open_edges(u) == 0);
        REQUIRE(its_num_open_edges(l) == 0);
        // The gap faces are 2 mm apart, sampled across the footprint. The two
        // halves are cut at z = +1 and z = -1 in the CUT frame; after the cut the
        // parts are re-placed on the bed, so measure the faces in each half's own
        // mesh instead: the lower half's top and the upper half's bottom.
        double lo_top = -1e9, up_bot = 1e9;
        for (const Vec3f& v : l.vertices) lo_top = std::max(lo_top, double(v.z()));
        for (const Vec3f& v : u.vertices) up_bot = std::min(up_bot, double(v.z()));
        // Each half is 19 mm tall, so the check that matters is the heights.
        double lo_bot = 1e9, up_top = -1e9;
        for (const Vec3f& v : l.vertices) lo_bot = std::min(lo_bot, double(v.z()));
        for (const Vec3f& v : u.vertices) up_top = std::max(up_top, double(v.z()));
        REQUIRE(lo_top - lo_bot == Approx(19.0).margin(1e-4));
        REQUIRE(up_top - up_bot == Approx(19.0).margin(1e-4));
        // 40 - 19 - 19 == 2: the band that is gone.
        REQUIRE(CUBE - (lo_top - lo_bot) - (up_top - up_bot) == Approx(2.0).margin(1e-4));
    }

    // The offset choice moves the band, not its width.
    {
        indexed_triangle_set u, l;
        flat_cut_halves(2.0, CutThicknessOffset::Above, u, l);
        // The band is [0, +2], so the lower half keeps its full 20 mm and the
        // upper half loses 2.
        REQUIRE(std::abs(double(its_volume(l))) == Approx(CUBE * CUBE * 20.0).epsilon(1e-4));
        REQUIRE(std::abs(double(its_volume(u))) == Approx(CUBE * CUBE * 18.0).epsilon(1e-4));
    }
    {
        indexed_triangle_set u, l;
        flat_cut_halves(2.0, CutThicknessOffset::Below, u, l);
        REQUIRE(std::abs(double(its_volume(l))) == Approx(CUBE * CUBE * 18.0).epsilon(1e-4));
        REQUIRE(std::abs(double(its_volume(u))) == Approx(CUBE * CUBE * 20.0).epsilon(1e-4));
    }
}

// ---------------------------------------------------------------------------
// (P3-6) Cut thickness, CURVED.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: a curved cut with a thickness removes a slab", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double whole = CUBE * CUBE * CUBE;
    const CurvedCutSheet sheet = dome_sheet(8.0, 5, 40.0);

    // t == 0 is bit-identical to the no-thickness call.
    {
        indexed_triangle_set u0, l0, u1, l1;
        REQUIRE(curved_cut_split(cube, sheet, &u0, &l0, CurvedCutSheet::CutSamples, 0.0));
        REQUIRE(curved_cut_split(cube, sheet, &u1, &l1));
        REQUIRE(u0.vertices.size() == u1.vertices.size());
        REQUIRE(u0.indices.size()  == u1.indices.size());
        REQUIRE(l0.vertices.size() == l1.vertices.size());
        REQUIRE(l0.indices.size()  == l1.indices.size());
        for (size_t k = 0; k < u0.vertices.size(); ++ k)
            REQUIRE(u0.vertices[k] == u1.vertices[k]);
        for (size_t k = 0; k < u0.indices.size(); ++ k)
            REQUIRE(u0.indices[k] == u1.indices[k]);
        for (size_t k = 0; k < l0.vertices.size(); ++ k)
            REQUIRE(l0.vertices[k] == l1.vertices[k]);
        for (size_t k = 0; k < l0.indices.size(); ++ k)
            REQUIRE(l0.indices[k] == l1.indices[k]);
    }

    // t == 2 on the domed sheet.
    indexed_triangle_set u, l;
    REQUIRE(curved_cut_split(cube, sheet, &u, &l, CurvedCutSheet::CutSamples, 2.0));
    REQUIRE(!u.empty());
    REQUIRE(!l.empty());
    const double up = std::abs(double(its_volume(u)));
    const double lo = std::abs(double(its_volume(l)));
    INFO("curved t=2: upper " << up << " lower " << lo << " sum " << (up + lo) << " whole " << whole);
    // The removed band follows the dome, which over the cube's 40 x 40 footprint
    // is a 2 mm sheet of area 40 x 40 plus whatever the slope adds - so the loss
    // is at least the flat 3200 mm^3 and not much more (the dome's max slope
    // over this span is gentle).
    const double lost = whole - (up + lo);
    REQUIRE(lost > 0.98 * CUBE * CUBE * 2.0);
    REQUIRE(lost < 1.15 * CUBE * CUBE * 2.0);
    REQUIRE(its_num_open_edges(u) == 0);
    REQUIRE(its_num_open_edges(l) == 0);

    // THE GAP, measured along the normal (local Z, which IS the normal for a
    // height field's offset - the two faces are the same surface shifted along
    // Z, so the vertical separation is the offset separation). At 25 sample
    // points across the footprint, cast a vertical line through both halves and
    // check the lower half's top and the upper half's bottom are 2 mm apart.
    auto surface_z = [](const indexed_triangle_set& its, double x, double y, bool want_max, double& out) {
        bool found = false;
        double best = want_max ? -1e9 : 1e9;
        for (const Vec3i32& tri : its.indices) {
            const Vec3d a = its.vertices[tri(0)].cast<double>();
            const Vec3d b = its.vertices[tri(1)].cast<double>();
            const Vec3d c = its.vertices[tri(2)].cast<double>();
            // Barycentric in xy.
            const double d = (b.y() - c.y()) * (a.x() - c.x()) + (c.x() - b.x()) * (a.y() - c.y());
            if (std::abs(d) < 1e-12)
                continue;
            const double l1 = ((b.y() - c.y()) * (x - c.x()) + (c.x() - b.x()) * (y - c.y())) / d;
            const double l2 = ((c.y() - a.y()) * (x - c.x()) + (a.x() - c.x()) * (y - c.y())) / d;
            const double l3 = 1.0 - l1 - l2;
            if (l1 < -1e-9 || l2 < -1e-9 || l3 < -1e-9)
                continue;
            const double z = l1 * a.z() + l2 * b.z() + l3 * c.z();
            if (want_max ? (z > best) : (z < best)) { best = z; found = true; }
        }
        out = best;
        return found;
    };

    int checked = 0;
    for (int j = 0; j < 5; ++ j)
        for (int i = 0; i < 5; ++ i) {
            // Stay inside the footprint, away from the silhouette edges.
            const double x = -14.0 + 7.0 * double(i);
            const double y = -14.0 + 7.0 * double(j);
            double lz = 0.0, uz = 0.0;
            if (!surface_z(l, x, y, /*want_max*/ true, lz))  continue;
            if (!surface_z(u, x, y, /*want_max*/ false, uz)) continue;
            INFO("at (" << x << ", " << y << "): lower top " << lz << " upper bottom " << uz
                 << " gap " << (uz - lz));
            REQUIRE(uz - lz == Approx(2.0).margin(0.05));
            ++ checked;
        }
    REQUIRE(checked == 25);
}

// ---------------------------------------------------------------------------
// (P3-7) A flexi joint with a cut thickness. The kerf is added to the joint's
// own gap so the bodies move with the faces and the joint stays assembled.
// ---------------------------------------------------------------------------

// The harness mirrors test_flexi_joint.cpp's cut_with_joint(): the two halves come
// back as VOLUMES of one object, each with its own transform, so the meshes have to
// be brought into object coordinates before they can be compared to each other.
struct KerfFlexiHalves
{
    TriangleMesh upper;
    TriangleMesh lower;
};

static KerfFlexiHalves flexi_cut_with_kerf(double thickness, FlexiJointKind kind)
{
    static const double CYL_R = 12.0, CYL_H = 30.0, CUT_Z = 15.0;

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "flexi_cylinder";
    ModelVolume* v = mo->add_volume(TriangleMesh(its_make_cylinder(CYL_R, CYL_H, 2.0 * PI / 180.0)));
    v->set_type(ModelVolumeType::MODEL_PART);
    v->name = "cyl";
    mo->add_instance()->set_transformation(Geometry::Transformation());

    FlexiJointParams p;
    p.kind = kind;

    CutConnector connector;
    connector.pos        = Vec3d(0.0, 0.0, CUT_Z);
    connector.rotation_m = Transform3d::Identity();
    connector.z_angle    = 0.f;
    connector.radius     = flexi_outer_extent(p);
    connector.height     = flexi_protrusion_height(p);
    connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint, CutConnectorStyle::Prism, CutConnectorShape::Circle);
    connector.flexi      = p;
    add_flexi_joint_volume(mo, connector, "Flexi joint-1");

    Cut cut(mo, 0, Geometry::translation_transform(Vec3d(0.0, 0.0, CUT_Z)),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
            ModelObjectCutAttribute::KeepAsParts);
    const ModelObjectPtrs& res = cut.perform_with_plane(thickness);

    KerfFlexiHalves out;
    REQUIRE(res.size() == 1);
    REQUIRE(res.front()->volumes.size() == 2);
    for (const ModelVolume* vol : res.front()->volumes) {
        TriangleMesh m(vol->mesh());
        m.transform(vol->get_matrix());
        if (vol->is_from_upper())
            out.upper = m;
        else
            out.lower = m;
    }
    return out;
}

static double kerf_intersection_volume(const TriangleMesh& a, const TriangleMesh& b)
{
    std::vector<TriangleMesh> dst;
    if (!MeshBoolean::mfd::make_boolean(a, b, dst, "INTERSECTION")) {
        dst.clear();
        MeshBoolean::mcut::make_boolean(a, b, dst, "INTERSECTION");
    }
    double vol = 0.0;
    for (const TriangleMesh& m : dst)
        vol += std::abs(double(its_volume(m.its)));
    return vol;
}

TEST_CASE("Curved cut: a flexi joint survives a cut thickness", "[CurvedCut]")
{
    // A Double ring, the kind whose two loops thread through each other, at t = 0
    // (the baseline the flexi suite already pins) and again at t = 2.
    //
    // The kerf is added to the joint's own gap (see perform_with_flexi_joints), so
    // the two flat faces move apart by 2 mm AND every body is regenerated relative
    // to the moved faces - which is what keeps the ring bodies bridging the gap
    // rather than being left behind in one half.
    double gap0 = 0.0;
    for (double t : { 0.0, 2.0 }) {
        INFO("cut thickness " << t);
        const KerfFlexiHalves h = flexi_cut_with_kerf(t, FlexiJointKind::DoubleRing);
        REQUIRE(!h.upper.empty());
        REQUIRE(!h.lower.empty());
        REQUIRE(std::abs(double(its_volume(h.upper.its))) > 100.0);
        REQUIRE(std::abs(double(its_volume(h.lower.its))) > 100.0);

        // THE EXISTING CONTRACT, unchanged by the kerf: the two segments are
        // genuinely separate solids, so the joint is printable in place.
        const double inter = kerf_intersection_volume(h.upper, h.lower);
        INFO("intersection volume " << inter);
        REQUIRE(inter == Approx(0.0).margin(1e-3));

        // The two flat cut faces, measured on the cylinder WALL (r near CYL_R) so
        // the joint bodies, which straddle the middle, stay out of it.
        const double CYL_R = 12.0;
        double lo = -1e9, hi = 1e9;
        for (const Vec3f& v : h.lower.its.vertices)
            if (std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
                lo = std::max(lo, double(v.z()));
        for (const Vec3f& v : h.upper.its.vertices)
            if (std::hypot(double(v.x()), double(v.y())) > 0.9 * CYL_R)
                hi = std::min(hi, double(v.z()));
        const double gap = hi - lo;
        INFO("face-to-face gap " << gap);
        if (t == 0.0)
            gap0 = gap;
        else {
            // The kerf really did widen the gap, by exactly the thickness.
            REQUIRE(gap == Approx(gap0 + 2.0).margin(1e-3));
        }

        // THE BODIES STILL BRIDGE IT. Each half's material spans the mid plane -
        // its z range crosses the other's - so the rings still interlock rather
        // than sitting as two loose pieces on either side of a wider gap.
        auto spans = [](const TriangleMesh& m) {
            double a = 1e9, b = -1e9;
            for (const Vec3f& v : m.its.vertices) { a = std::min(a, double(v.z())); b = std::max(b, double(v.z())); }
            return std::make_pair(a, b);
        };
        const auto su = spans(h.upper), sl = spans(h.lower);
        INFO("upper z [" << su.first << ", " << su.second << "]  lower z [" << sl.first << ", " << sl.second << "]");
        // Their z ranges OVERLAP across the gap: at least one half's body reaches
        // past the other half's face, which is what an interlocking joint means
        // (the Double ring's male body protrudes from the lower half into the
        // upper half's socket, so the reach is one-sided by construction).
        REQUIRE(std::min(su.second, sl.second) > std::max(su.first, sl.first));
        REQUIRE(sl.second > hi);   // the lower half's body reaches past the upper face
        // How far it reaches is unchanged by the kerf, because the body is
        // generated relative to the (moved) faces rather than to the mid plane.
        INFO("lower body reach past the upper face: " << (sl.second - hi));
        REQUIRE(sl.second - hi > 0.5);
    }
}

// ---------------------------------------------------------------------------
// (P3-8) Thickness demo export.
// ---------------------------------------------------------------------------

TEST_CASE("Curved cut: thickness demo export", "[CurvedCut][.demo]")
{
    const char* dir_env = std::getenv("EDGESLICER_CUT_THICKNESS_DEMO_DIR");
    if (dir_env == nullptr || *dir_env == 0)
        return;
    const boost::filesystem::path dir(dir_env);
    boost::filesystem::create_directories(dir);

    const indexed_triangle_set cube = centred_cube();

    // FLAT, t = 2, through the Cut path so the demo is what the gizmo produces.
    {
        Model model;
        ModelObject* mo = model.add_object();
        mo->name = "cube";
        mo->add_volume(TriangleMesh(cube))->name = "cube_v";
        mo->add_instance();
        Cut cut(mo, 0, Transform3d::Identity(),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& parts = cut.perform_with_plane(2.0);
        REQUIRE(parts.size() == 2);
        TriangleMesh mu(parts[0]->volumes.front()->mesh().its), ml(parts[1]->volumes.front()->mesh().its);
        REQUIRE(store_stl((dir / "flat_t2_upper.stl").string().c_str(), &mu, true));
        REQUIRE(store_stl((dir / "flat_t2_lower.stl").string().c_str(), &ml, true));
        WARN("flat t=2: upper " << its_volume(mu.its) << " lower " << its_volume(ml.its));
    }

    // CURVED, t = 2, domed sheet.
    {
        const CurvedCutSheet sheet = dome_sheet(8.0, 5, 40.0);
        indexed_triangle_set u, l;
        REQUIRE(curved_cut_split(cube, sheet, &u, &l, CurvedCutSheet::CutSamples, 2.0));
        TriangleMesh mu(u), ml(l);
        REQUIRE(store_stl((dir / "curved_t2_upper.stl").string().c_str(), &mu, true));
        REQUIRE(store_stl((dir / "curved_t2_lower.stl").string().c_str(), &ml, true));
        WARN("curved t=2: upper " << its_volume(u) << " lower " << its_volume(l));
    }

    WARN("thickness demo written to " << dir.string());
}

// ---------------------------------------------------------------------------
// PHASE 4: connectors on a curved cut.
//
// A connector on a curved cut is the SAME object it is on a flat one - a
// position plus a rotation - only both are now taken from the SHEET rather than
// from the plane. Everything below proves that one sentence:
//
//   * on a flat sheet the two are the same matrix, so the cut is bit-identical;
//   * on a domed sheet the connector stands along the surface normal at its own
//     (u,v), which is checked against the analytic gradient;
//   * the kerf lengthens the connector along THAT normal, so it still bridges;
//   * the footprint test is done on the sheet, so a rim connector is rejected;
//   * and what persists to a 3MF is the baked position and frame, which is all a
//     re-opened project needs.
//
// The gizmo's own frame builder is curved_cut_sheet_frame(); these tests drive
// it directly and then drive the cut with the volumes it produces, which is
// exactly what apply_connectors_in_model() does in the app.
// ---------------------------------------------------------------------------

// The connector volume the gizmo builds, built here the same way
// GLGizmoCut3D::apply_cut_connectors() builds it: a unit cylinder scaled by
// (radius, radius, height) and placed by translation * rotation. Keeping this in
// step with the gizmo is what makes these tests mean anything about the app.
static ModelVolume* add_plug_volume(ModelObject* mo, const Vec3d& pos, const Transform3d& rot,
                                    double radius, double height, const std::string& name)
{
    using namespace Slic3r::Geometry;
    TriangleMesh mesh(its_make_cylinder(1.0, 1.0, 2 * PI / 360.));
    ModelVolume* v = mo->add_volume(std::move(mesh), ModelVolumeType::NEGATIVE_VOLUME);
    v->set_transformation(translation_transform(pos) * rot * scale_transform(Vec3d(radius, radius, height)));
    v->cut_info = ModelVolume::CutInfo(CutConnectorType::Plug, 0.f, 0.1f);
    v->name = name;
    return v;
}

// The centre shift apply_connectors_in_model() applies to a Plug: the body
// starts at the lower kerf face and is `kerf` longer, so its centre lands at
// face_lo + height/2 - measured along the CONNECTOR's own normal.
struct PlugPlacement
{
    Vec3d       pos;      // centre of the body, in the object frame
    Transform3d rot;      // the connector's frame
    double      height;   // after the kerf lengthened it
};

static PlugPlacement plug_placement(const CurvedCutSheet& sheet, double x, double y,
                                    double height, double thickness, CutThicknessOffset off)
{
    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(thickness, off, face_lo, face_hi);
    const double kerf = face_hi - face_lo;

    PlugPlacement p;
    p.rot    = curved_cut_sheet_frame(sheet, x, y);
    p.height = height + kerf;
    const Vec3d n = p.rot.linear() * Vec3d::UnitZ();
    // The point ON THE SHEET, then the centre shift along the local normal.
    p.pos = Vec3d(x, y, sheet.evaluate_local(x, y)) + n * (face_lo + 0.5 * p.height);
    return p;
}

// Fit a cylinder's axis from its vertices: the direction of largest variance is
// the axis of a solid of revolution that is longer than it is wide.
static Vec3d fit_axis(const indexed_triangle_set& its)
{
    REQUIRE(!its.vertices.empty());
    Vec3d c = Vec3d::Zero();
    for (const Vec3f& v : its.vertices)
        c += v.cast<double>();
    c /= double(its.vertices.size());

    Matrix3d cov = Matrix3d::Zero();
    for (const Vec3f& v : its.vertices) {
        const Vec3d d = v.cast<double>() - c;
        cov += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Matrix3d> es(cov);
    // Largest eigenvalue last.
    Vec3d axis = es.eigenvectors().col(2);
    if (axis.z() < 0.0)
        axis = -axis;
    return axis.normalized();
}

// The analytic normal of the Catmull-Rom dome, by finite differences on the
// sheet itself - i.e. the same thing curved_cut_sheet_normal() computes, but
// spelled out here so the test is not just calling the code it is testing.
static Vec3d analytic_normal(const CurvedCutSheet& sheet, double x, double y)
{
    const double h = 1e-4;
    const double fx = (sheet.evaluate_local(x + h, y) - sheet.evaluate_local(x - h, y)) / (2 * h);
    const double fy = (sheet.evaluate_local(x, y + h) - sheet.evaluate_local(x, y - h)) / (2 * h);
    return Vec3d(-fx, -fy, 1.0).normalized();
}

static double its_volume_of(const indexed_triangle_set& its)
{
    return double(its_volume(its));
}

// (a) A flat sheet with connectors == the plane cut with the same connectors.
TEST_CASE("Curved cut: connectors on a flat sheet are the plane cut's connectors", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    // The frame builder itself: on a flat sheet it is the identity, EXACTLY -
    // not "identity to within epsilon" - which is what lets the two paths
    // produce the same matrices and therefore the same meshes.
    CurvedCutSheet flat(5);
    flat.set_half_size(40.0);
    REQUIRE(flat.is_flat());
    for (double x : { -20.0, 0.0, 13.5 })
        for (double y : { -8.0, 0.0, 17.0 }) {
            const Transform3d f = curved_cut_sheet_frame(flat, x, y);
            REQUIRE(f.matrix() == Transform3d::Identity().matrix());
            const Vec3d n = curved_cut_sheet_normal(flat, x, y);
            REQUIRE(n.x() == 0.0);
            REQUIRE(n.y() == 0.0);
            REQUIRE(n.z() == 1.0);
            REQUIRE(curved_cut_sheet_tilt_deg(flat, x, y) == Approx(0.0).margin(1e-9));
        }

    struct Halves { indexed_triangle_set upper, lower; size_t nvol_u, nvol_l; };

    auto run = [&](bool curved) {
        Model model;
        ModelObject* mo = model.add_object();
        mo->name = "cube";
        mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
        // Two plugs, the way apply_connectors_in_model() leaves them: centre
        // shifted up by height/2 (no kerf), frame = the sheet's frame there.
        for (const Vec2d& xy : { Vec2d(-8.0, 3.0), Vec2d(9.5, -6.0) }) {
            const PlugPlacement p = plug_placement(flat, xy.x(), xy.y(), 10.0, 0.0, CutThicknessOffset::Centred);
            add_plug_volume(mo, p.pos, p.rot, 4.0, p.height, "plug");
        }
        mo->add_instance();

        Cut cut(mo, 0, Transform3d::Identity(),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& parts = curved ? cut.perform_with_curved_sheet(flat) : cut.perform_with_plane();
        REQUIRE(parts.size() == 2);

        Halves h;
        h.nvol_u = parts[0]->volumes.size();
        h.nvol_l = parts[1]->volumes.size();
        h.upper  = parts[0]->volumes.front()->mesh().its;
        h.lower  = parts[1]->volumes.front()->mesh().its;
        return h;
    };

    const Halves plane  = run(false);
    const Halves curved = run(true);

    // Same number of volumes on each side - the connectors reached both halves.
    REQUIRE(curved.nvol_u == plane.nvol_u);
    REQUIRE(curved.nvol_l == plane.nvol_l);
    REQUIRE(plane.nvol_u == 3); // the solid + two connector pockets
    REQUIRE(plane.nvol_l == 3); // the solid + two connector plugs

    // BIT-IDENTICAL, vertex by vertex. A flat curved cut routes into
    // perform_with_plane(), so this is the same function - the test is that
    // nothing in the connector path took a different branch on the way in.
    REQUIRE(curved.upper.vertices.size() == plane.upper.vertices.size());
    REQUIRE(curved.upper.indices.size()  == plane.upper.indices.size());
    REQUIRE(curved.lower.vertices.size() == plane.lower.vertices.size());
    REQUIRE(curved.lower.indices.size()  == plane.lower.indices.size());
    for (size_t i = 0; i < plane.upper.vertices.size(); ++ i)
        REQUIRE(curved.upper.vertices[i] == plane.upper.vertices[i]);
    for (size_t i = 0; i < plane.lower.vertices.size(); ++ i)
        REQUIRE(curved.lower.vertices[i] == plane.lower.vertices[i]);
}

// (b) A domed sheet: the plug stands along the sheet normal at its own point.
TEST_CASE("Curved cut: a plug on a dome stands along the sheet normal", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    const CurvedCutSheet dome = dome_sheet(8.0, 5, 40.0);
    REQUIRE_FALSE(dome.is_flat());

    // OFF-CENTRE on purpose: at the apex the normal is +Z and the test would
    // pass with a plane frame too. Here the surface really slopes.
    const double px = -12.0, py = 7.0;
    const Vec3d  want = analytic_normal(dome, px, py);
    // The slope has to be worth measuring, or the proof is vacuous.
    REQUIRE(std::acos(std::clamp(want.z(), -1.0, 1.0)) * 180.0 / PI > 5.0);

    const Vec3d got = curved_cut_sheet_normal(dome, px, py);
    REQUIRE(std::acos(std::clamp(got.dot(want), -1.0, 1.0)) * 180.0 / PI < 0.5);

    // The frame is orthonormal, right handed, and its Z IS that normal.
    const Transform3d frame = curved_cut_sheet_frame(dome, px, py);
    const Matrix3d    m     = frame.linear();
    REQUIRE((m.transpose() * m - Matrix3d::Identity()).norm() < 1e-9);
    REQUIRE(m.determinant() == Approx(1.0).margin(1e-9));
    REQUIRE((m.col(2) - got).norm() < 1e-9);
    // Local X is the plane's X projected onto the tangent plane: it has no
    // component along the normal and it still points broadly along +X.
    REQUIRE(std::abs(m.col(0).dot(got)) < 1e-9);
    REQUIRE(m.col(0).x() > 0.5);

    const double radius = 4.0, height = 10.0;
    const PlugPlacement p = plug_placement(dome, px, py, height, 0.0, CutThicknessOffset::Centred);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    add_plug_volume(mo, p.pos, p.rot, radius, p.height, "plug");
    mo->add_instance();

    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(dome);
    REQUIRE(parts.size() == 2);

    ModelObject* upper = parts[0];
    ModelObject* lower = parts[1];
    // The solid plus the connector on each side.
    REQUIRE(upper->volumes.size() == 2);
    REQUIRE(lower->volumes.size() == 2);

    const indexed_triangle_set& up_solid = upper->volumes.front()->mesh().its;
    const indexed_triangle_set& lo_solid = lower->volumes.front()->mesh().its;
    REQUIRE(its_num_open_edges(up_solid) == 0);
    REQUIRE(its_num_open_edges(lo_solid) == 0);

    // THE AXIS. The plug volume that landed in the lower half is the solid pin;
    // fit its axis from its own vertices, in the OBJECT frame, and compare with
    // the analytic normal.
    const ModelVolume* pin = lower->volumes.back();
    REQUIRE(pin->cut_info.is_connector);
    indexed_triangle_set pin_its = pin->mesh().its;
    its_transform(pin_its, pin->get_matrix());
    const Vec3d axis = fit_axis(pin_its);
    const double ang = std::acos(std::clamp(std::abs(axis.dot(want)), -1.0, 1.0)) * 180.0 / PI;
    INFO("plug axis " << axis.transpose() << " vs sheet normal " << want.transpose());
    REQUIRE(ang < 0.5);

    // ... and it is NOT the plane normal, or the test would pass on the flat path.
    const double ang_plane = std::acos(std::clamp(std::abs(axis.z()), -1.0, 1.0)) * 180.0 / PI;
    REQUIRE(ang_plane > 5.0);

    // Volume accounting: the two solid halves plus the pocket the plug carved out
    // of the upper half account for the whole cube. The pocket IS the pin's own
    // volume where the pin sits inside the upper half, so upper + lower + (the
    // part of the pin above the sheet) == the cube.
    const double whole = CUBE * CUBE * CUBE;
    const double v_up  = its_volume_of(up_solid);
    const double v_lo  = its_volume_of(lo_solid);
    // The pin protrudes into... no: the pin's SOLID sits in the LOWER half and
    // its pocket is cut from the UPPER one, so the sum of the two solids is the
    // cube MINUS the pocket. The pocket is the pin above the sheet, which for a
    // plug placed at height/2 above the surface is half the cylinder.
    const double pocket = PI * radius * radius * (0.5 * p.height);
    INFO("upper " << v_up << " lower " << v_lo << " pocket " << pocket << " whole " << whole);
    REQUIRE(std::abs((v_up + v_lo + pocket) - whole) / whole < 1e-2);

    // THE PROTRUSION. A Plug is not cut in half the way a Dowel is: the WHOLE
    // body goes to the lower half as a solid part and the upper half gets the
    // matching pocket. So what "protrudes" means for a plug is that the body
    // stands on the cut surface and reaches `height` PAST it into the upper
    // half - measured, again, along the LOCAL normal, which is the whole point.
    const Vec3d sheet_pt = Vec3d(px, py, dome.evaluate_local(px, py));
    double lo_end = std::numeric_limits<double>::max();
    double hi_end = -std::numeric_limits<double>::max();
    double off_axis = 0.0;
    for (const Vec3f& v : pin_its.vertices) {
        const Vec3d d = v.cast<double>() - sheet_pt;
        const double along = d.dot(want);
        lo_end = std::min(lo_end, along);
        hi_end = std::max(hi_end, along);
        off_axis = std::max(off_axis, (d - along * want).norm());
    }
    INFO("plug spans " << lo_end << " .. " << hi_end << " along the local normal, radius " << off_axis);
    // It starts ON the surface and reaches `height` beyond it.
    REQUIRE(lo_end == Approx(0.0).margin(1e-3));
    REQUIRE(hi_end == Approx(height).margin(1e-3));
    // ... and it really is a cylinder of the asked-for radius about that axis,
    // i.e. the body was not sheared by the tilted frame.
    REQUIRE(off_axis == Approx(radius).margin(0.02));
}

// (c) A DoubleRing Flexi on a dome: the rings stand on the local normal and the
//     two halves still do not intersect.
TEST_CASE("Curved cut: a Flexi double ring on a dome uses the sheet's frame", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    const CurvedCutSheet dome = dome_sheet(6.0, 5, 40.0);
    const double px = -10.0, py = 6.0;
    const Vec3d  want = analytic_normal(dome, px, py);
    REQUIRE(std::acos(std::clamp(want.z(), -1.0, 1.0)) * 180.0 / PI > 4.0);

    FlexiJointParams params;
    params.kind         = FlexiJointKind::DoubleRing;
    params.outer_radius = 5.0f;
    params.ring_width   = 2.0f;
    params.ring_height  = 1.5f;
    params.clearance    = 0.35f;

    CutConnector connector;
    connector.attribs = CutConnectorAttributes(CutConnectorType::FlexiJoint,
                                               CutConnectorStyle::Prism, CutConnectorShape::Circle);
    connector.flexi   = params;
    // The gizmo's phase 4 placement: on the sheet, on the sheet's frame.
    connector.pos        = Vec3d(px, py, dome.evaluate_local(px, py));
    connector.rotation_m = curved_cut_sheet_frame(dome, px, py);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    add_flexi_joint_volume(mo, connector, "joint");
    REQUIRE(has_flexi_joint(mo));
    mo->add_instance();

    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
            ModelObjectCutAttribute::KeepAsParts);
    // A flexi joint takes the flexi path even from the curved entry point - the
    // joint's own gap IS the surface between the segments. The point of the test
    // is that the joint stands on the SHEET's frame while it does so.
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(dome);
    REQUIRE(parts.size() == 1);
    REQUIRE(parts.front()->volumes.size() == 2);

    TriangleMesh a(parts.front()->volumes[0]->mesh());
    a.transform(parts.front()->volumes[0]->get_matrix());
    TriangleMesh b(parts.front()->volumes[1]->mesh());
    b.transform(parts.front()->volumes[1]->get_matrix());

    // Print-in-place: the two segments must not share any volume.
    std::vector<TriangleMesh> inter;
    const bool ok = MeshBoolean::mfd::make_boolean(a, b, inter, "INTERSECTION");
    REQUIRE(ok);
    double v = 0.0;
    for (TriangleMesh& m : inter)
        v += double(m.volume());
    INFO("intersection volume " << v);
    REQUIRE(std::abs(v) < 1e-3);

    // THE RINGS' AXIS. The joint volume was placed on the sheet frame, so the
    // ring bodies - which are solids of revolution about their own local Z -
    // come out with their axis along the sheet normal. Fit it from the body the
    // gizmo actually built.
    indexed_triangle_set ring;
    for (const indexed_triangle_set& its : flexi_lower_bodies(params))
        its_merge(ring, its);
    its_transform(ring, translation_transform(connector.pos) * connector.rotation_m);
    // A ring is a flat disc: its axis is the direction of SMALLEST variance, so
    // fit_axis (largest) is wrong here - take the plane normal instead.
    Vec3d c = Vec3d::Zero();
    for (const Vec3f& p : ring.vertices)
        c += p.cast<double>();
    c /= double(ring.vertices.size());
    Matrix3d cov = Matrix3d::Zero();
    for (const Vec3f& p : ring.vertices) {
        const Vec3d d = p.cast<double>() - c;
        cov += d * d.transpose();
    }
    Eigen::SelfAdjointEigenSolver<Matrix3d> es(cov);
    Vec3d axis = es.eigenvectors().col(0).normalized(); // smallest variance
    const double ang = std::acos(std::clamp(std::abs(axis.dot(want)), -1.0, 1.0)) * 180.0 / PI;
    INFO("ring axis " << axis.transpose() << " vs sheet normal " << want.transpose());
    REQUIRE(ang < 0.5);
    REQUIRE(std::acos(std::clamp(std::abs(axis.z()), -1.0, 1.0)) * 180.0 / PI > 4.0);
}

// (d) A kerf on a dome: the plug grows by t ALONG THE LOCAL NORMAL and still
//     bridges the gap.
TEST_CASE("Curved cut: a plug on a dome bridges a cut thickness", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    const CurvedCutSheet dome = dome_sheet(8.0, 5, 40.0);
    const double px = -12.0, py = 7.0;
    const Vec3d  want = analytic_normal(dome, px, py);
    const double t = 2.0;
    const double height = 10.0;

    const PlugPlacement p0 = plug_placement(dome, px, py, height, 0.0, CutThicknessOffset::Centred);
    const PlugPlacement p2 = plug_placement(dome, px, py, height, t,   CutThicknessOffset::Centred);

    // The body got exactly t longer...
    REQUIRE(p2.height == Approx(p0.height + t));
    // ... its frame did not turn (the kerf moves the body, it does not tilt it)...
    REQUIRE((p2.rot.matrix() - p0.rot.matrix()).norm() < 1e-12);
    // ... and its centre moved DOWN along the LOCAL NORMAL, not along +Z: with a
    // centred kerf the body starts at -t/2 and is t longer, so the centre sits
    // t/2 lower than it did, measured along n.
    const Vec3d dpos = p2.pos - p0.pos;
    REQUIRE(dpos.dot(want) == Approx(0.0).margin(1e-9));
    // The shift is purely along the normal - no sideways drift.
    REQUIRE((dpos - dpos.dot(want) * want).norm() < 1e-9);

    // THE BRIDGE. The body has to reach past BOTH kerf faces: its lower end sits
    // at face_lo relative to the sheet along n, its upper end at face_lo+height+t.
    double face_lo = 0.0, face_hi = 0.0;
    curved_cut_thickness_faces(t, CutThicknessOffset::Centred, face_lo, face_hi);
    const Vec3d sheet_pt = Vec3d(px, py, dome.evaluate_local(px, py));
    const double lo_end = (p2.pos - sheet_pt).dot(want) - 0.5 * p2.height;
    const double hi_end = (p2.pos - sheet_pt).dot(want) + 0.5 * p2.height;
    REQUIRE(lo_end == Approx(face_lo).margin(1e-9));
    REQUIRE(hi_end == Approx(face_lo + height + t).margin(1e-9));
    // It spans the whole removed band: below face_lo and above face_hi.
    REQUIRE(lo_end <= face_lo + 1e-9);
    REQUIRE(hi_end >= face_hi + 1e-9);

    // And the cut itself runs: two closed halves with the plug in one of them.
    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    add_plug_volume(mo, p2.pos, p2.rot, 4.0, p2.height, "plug");
    mo->add_instance();

    Cut cut(mo, 0, Transform3d::Identity(),
            ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
    const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(dome, t, CutThicknessOffset::Centred);
    REQUIRE(parts.size() == 2);
    REQUIRE(its_num_open_edges(parts[0]->volumes.front()->mesh().its) == 0);
    REQUIRE(its_num_open_edges(parts[1]->volumes.front()->mesh().its) == 0);
    // The kerf really removed material.
    const double whole = CUBE * CUBE * CUBE;
    const double sum = its_volume_of(parts[0]->volumes.front()->mesh().its) +
                       its_volume_of(parts[1]->volumes.front()->mesh().its);
    INFO("kerfed halves sum " << sum << " of " << whole);
    REQUIRE(sum < whole - 0.5 * CUBE * CUBE * t);
}

// (e) The footprint test on the sheet: a connector near the rim is rejected, one
//     in the middle is accepted.
//
// The gizmo asks the CLIPPER, which needs a GL context. The geometry underneath
// it does not: the footprint is sampled in the connector's own tangent plane,
// projected back down onto the cut plane, and tested against the object's
// section there. That projection is the whole phase 4 change, so it is what this
// pins - with a plain point-in-polygon test standing in for the clipper.
TEST_CASE("Curved cut: a connector's footprint is tested on the sheet", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    const CurvedCutSheet dome = dome_sheet(8.0, 5, 40.0);
    const double half = 0.5 * CUBE;

    // The footprint of a connector at (x,y), sampled in the sheet's TANGENT
    // plane there and projected back onto the cut plane - i.e. what
    // is_outside_of_cut_contour() now feeds the clipper.
    auto footprint_outside = [&](double x, double y, double radius) {
        const Transform3d frame = curved_cut_sheet_frame(dome, x, y);
        const Vec3d       pos(x, y, dome.evaluate_local(x, y));
        for (int k = 0; k < 60; ++ k) {
            const double a = 2.0 * PI * double(k) / 60.0;
            const Vec3d  local(radius * std::cos(a), radius * std::sin(a), 0.0);
            const Vec3d  world = pos + frame * local;
            // Project down the plane normal onto the cut plane: drop z.
            if (std::abs(world.x()) > half || std::abs(world.y()) > half)
                return true;
        }
        return false;
    };

    const double radius = 4.0;
    // In the middle: accepted.
    REQUIRE_FALSE(footprint_outside(0.0, 0.0, radius));
    REQUIRE_FALSE(footprint_outside(-8.0, 5.0, radius));
    // Near the rim: rejected, because the footprint hangs over the edge.
    REQUIRE(footprint_outside(half - 1.0, 0.0, radius));
    REQUIRE(footprint_outside(0.0, -(half - 2.0), radius));

    // The TILT is what makes this different from the flat test: on a slope the
    // footprint's projection is an ELLIPSE, narrower than the disc, so a
    // connector that the flat test would reject can be accepted - and the
    // projection is what says so. Measure the projected width directly.
    const double px = -14.0, py = 0.0;
    const Transform3d frame = curved_cut_sheet_frame(dome, px, py);
    REQUIRE(curved_cut_sheet_tilt_deg(dome, px, py) > 5.0);
    double wx = 0.0;
    for (int k = 0; k < 180; ++ k) {
        const double a = 2.0 * PI * double(k) / 180.0;
        const Vec3d  local(radius * std::cos(a), radius * std::sin(a), 0.0);
        wx = std::max(wx, std::abs((frame * local).x()));
    }
    INFO("projected half width " << wx << " of " << radius);
    REQUIRE(wx < radius);            // foreshortened by the tilt
    REQUIRE(wx > 0.5 * radius);      // but not collapsed

    // The tilt and flat-patch advisories, which the panel reports.
    REQUIRE(curved_cut_sheet_tilt_deg(dome, 0.0, 0.0) == Approx(0.0).margin(1e-6));
    // A 3 mm connector on this dome is fine; a 40 mm one is not.
    REQUIRE(curved_cut_patch_is_flat_enough(dome, px, py, 3.0));
    REQUIRE_FALSE(curved_cut_patch_is_flat_enough(dome, px, py, 200.0));
    // On a flat sheet everything is flat enough, at any size.
    CurvedCutSheet flat(5);
    flat.set_half_size(40.0);
    REQUIRE(curved_cut_patch_is_flat_enough(flat, 3.0, -2.0, 1e6));
    REQUIRE(curved_cut_sheet_curvature_radius(flat, 3.0, -2.0) > 1e30);
    // The dome's radius is finite and of the right order: an 8 mm rise over a
    // 40 mm half span is a radius in the tens of mm, not in the thousands.
    const double r = curved_cut_sheet_curvature_radius(dome, 0.0, 0.0);
    INFO("dome apex curvature radius " << r);
    REQUIRE(r > 5.0);
    REQUIRE(r < 2000.0);
}

// (f) The 3MF round trip: a curved-cut connector's baked position and frame.
//
// WHAT ACTUALLY PERSISTS. A CutConnector - the entry in ModelObject::cut_connectors
// - is pre-cut gizmo session state and reaches no 3MF, on a flat cut or a curved
// one. What persists is the connector VOLUME the gizmo bakes out of it
// (apply_cut_connectors), whose transform is
//
//     translation_transform(pos) * rotation_m * rotation(-z_angle) * scale(r,r,h)
//
// - i.e. the connector's position and its frame, baked into the volume matrix,
// plus its type and tolerances in Metadata/cut_information.xml. That is exactly
// what phase 4 needed to keep working, and it needed NOTHING NEW: the frame is a
// plain rotation whether it came from the plane or from the sheet, so a project
// re-opened without the sheet still puts the connector back where it stood and
// standing the way it stood.
TEST_CASE("Curved cut: a curved-cut connector survives a 3MF round trip", "[CurvedCut]")
{
    using namespace Slic3r::Geometry;

    const CurvedCutSheet dome = dome_sheet(8.0, 5, 40.0);
    const double px = -12.0, py = 7.0;

    const double radius = 4.0, height = 10.0;
    const PlugPlacement p = plug_placement(dome, px, py, height, 0.0, CutThicknessOffset::Centred);
    const Vec3d normal_in = p.rot.linear() * Vec3d::UnitZ();
    // The frame really is tilted, or the test would prove nothing a flat cut
    // does not already prove.
    REQUIRE(std::acos(std::clamp(normal_in.z(), -1.0, 1.0)) * 180.0 / PI > 5.0);
    // ... and the position really is off the cut plane, which is the other half
    // of "a curved connector is not a flat one".
    REQUIRE(std::abs(dome.evaluate_local(px, py)) > 1.0);

    Model model;
    ModelObject* mo = model.add_object();
    mo->name = "cube";
    mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
    ModelVolume* plug = add_plug_volume(mo, p.pos, p.rot, radius, p.height, "Connector-1");
    // is_cut_connector() gates the writer, and it demands a PROCESSED connector
    // on a cut object - which is what a connector looks like after the cut has
    // run, i.e. the state a saved project is actually in.
    plug->cut_info.set_processed();
    mo->cut_id.init();
    model.add_default_instances();
    const Transform3d matrix_in = plug->get_matrix();

    const boost::filesystem::path tmp_root = boost::filesystem::temp_directory_path() / "snorca_tests";
    boost::filesystem::create_directories(tmp_root);
    Slic3r::set_temporary_dir(tmp_root.string());
    const std::string test_file = (tmp_root / "edgeslicer_curved_connector.3mf").string();

    DynamicPrintConfig store_config = DynamicPrintConfig::full_print_config();
    StoreParams store_params;
    store_params.path     = test_file.c_str();
    store_params.model    = &model;
    store_params.config   = &store_config;
    store_params.strategy = SaveStrategy::Zip64 | SaveStrategy::Silence | SaveStrategy::SkipAuxiliary;
    REQUIRE(store_bbs_3mf(store_params));

    Model                     back;
    DynamicPrintConfig        dst_config;
    ConfigSubstitutionContext ctxt{ ForwardCompatibilitySubstitutionRule::EnableSilent };
    PlateDataPtrs             plate_data;
    std::vector<Preset*>      project_presets;
    bool                      is_bbl_3mf = false;
    Semver                    file_version;
    REQUIRE(load_bbs_3mf(test_file.c_str(), &dst_config, &ctxt, &back, &plate_data, &project_presets,
                         &is_bbl_3mf, &file_version, nullptr,
                         LoadStrategy::LoadModel | LoadStrategy::LoadConfig |
                         LoadStrategy::AddDefaultInstances | LoadStrategy::Silence));
    release_PlateData_list(plate_data);
    if (!std::getenv("SNORCA_CURVED_KEEP"))
        boost::filesystem::remove(test_file);

    REQUIRE(back.objects.size() == 1);
    const ModelVolume* out = nullptr;
    for (const ModelVolume* v : back.objects.front()->volumes)
        if (v->cut_info.is_connector)
            out = v;
    REQUIRE(out != nullptr);
    REQUIRE(out->cut_info.connector_type == CutConnectorType::Plug);

    // THE FRAME. The volume's rotation is the sheet's frame at the connector's
    // (u,v), and it came back turning the connector the same way - so the
    // re-opened project draws it standing on the surface normal it was placed
    // on, with no sheet anywhere in the file.
    const Transform3d matrix_out = out->get_matrix();
    const Vec3d normal_out = (matrix_out.linear() * Vec3d::UnitZ()).normalized();
    INFO("normal in " << normal_in.transpose() << " out " << normal_out.transpose());
    REQUIRE(std::acos(std::clamp(normal_out.dot(normal_in), -1.0, 1.0)) * 180.0 / PI < 0.1);
    // ... and it is NOT the plane normal, which is the phase 4 part.
    REQUIRE(std::acos(std::clamp(std::abs(normal_out.z()), -1.0, 1.0)) * 180.0 / PI > 5.0);

    // THE POSITION, including its height off the cut plane. A flat connector's
    // baked position sits on z == 0 in the plane frame; this one does not, and
    // the 3MF carries all three components either way.
    const Vec3d pos_in  = matrix_in.translation();
    const Vec3d pos_out = matrix_out.translation();
    INFO("pos in " << pos_in.transpose() << " out " << pos_out.transpose());
    REQUIRE((pos_out - pos_in).norm() < 1e-3);

    // The whole transform, so a scale or a shear introduced on the way through
    // would show up here rather than in a later surprise.
    REQUIRE((matrix_out.matrix() - matrix_in.matrix()).norm() < 1e-3);

    // And the round-tripped volume still cuts: the mesh it carries is the same
    // plug, so re-cutting the reopened project puts the same solid in the lower
    // half and the same pocket in the upper one.
    REQUIRE(out->mesh().its.vertices.size() == plug->mesh().its.vertices.size());
}

// Demo export: a dome-cut cube with two plugs and one double ring.
// Skipped unless EDGESLICER_CURVED_CONNECTOR_DEMO_DIR names a directory.
TEST_CASE("Curved cut: connector demo export", "[CurvedCut][.demo]")
{
    using namespace Slic3r::Geometry;

    const char* dir = std::getenv("EDGESLICER_CURVED_CONNECTOR_DEMO_DIR");
    if (dir == nullptr)
        return;
    boost::filesystem::create_directories(dir);

    const CurvedCutSheet dome = dome_sheet(8.0, 5, 40.0);

    // ---- two plugs -------------------------------------------------------
    {
        Model model;
        ModelObject* mo = model.add_object();
        mo->name = "cube";
        mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
        for (const Vec2d& xy : { Vec2d(-11.0, 6.0), Vec2d(10.0, -7.0) }) {
            const PlugPlacement p = plug_placement(dome, xy.x(), xy.y(), 10.0, 0.0, CutThicknessOffset::Centred);
            add_plug_volume(mo, p.pos, p.rot, 4.0, p.height, "plug");
        }
        mo->add_instance();

        Cut cut(mo, 0, Transform3d::Identity(),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower);
        const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(dome);
        REQUIRE(parts.size() == 2);
        for (size_t i = 0; i < parts.size(); ++ i) {
            TriangleMesh merged;
            for (const ModelVolume* v : parts[i]->volumes) {
                TriangleMesh m(v->mesh());
                m.transform(v->get_matrix());
                merged.merge(m);
            }
            const std::string name = std::string(dir) + "/plugs_" + (i == 0 ? "upper" : "lower") + ".stl";
            REQUIRE(store_stl(name.c_str(), &merged, true));
        }
    }

    // ---- one double ring -------------------------------------------------
    {
        FlexiJointParams params;
        params.kind         = FlexiJointKind::DoubleRing;
        params.outer_radius = 6.0f;
        params.ring_width   = 2.0f;
        params.ring_height  = 1.5f;

        CutConnector connector;
        connector.attribs    = CutConnectorAttributes(CutConnectorType::FlexiJoint,
                                                      CutConnectorStyle::Prism, CutConnectorShape::Circle);
        connector.flexi      = params;
        connector.pos        = Vec3d(-9.0, 5.0, dome.evaluate_local(-9.0, 5.0));
        connector.rotation_m = curved_cut_sheet_frame(dome, -9.0, 5.0);

        Model model;
        ModelObject* mo = model.add_object();
        mo->name = "cube";
        mo->add_volume(TriangleMesh(centred_cube()))->name = "cube_v";
        add_flexi_joint_volume(mo, connector, "joint");
        mo->add_instance();

        Cut cut(mo, 0, Transform3d::Identity(),
                ModelObjectCutAttribute::KeepUpper | ModelObjectCutAttribute::KeepLower |
                ModelObjectCutAttribute::KeepAsParts);
        const ModelObjectPtrs& parts = cut.perform_with_curved_sheet(dome);
        REQUIRE(parts.size() == 1);
        const ModelObject* out = parts.front();
        for (size_t i = 0; i < out->volumes.size(); ++ i) {
            TriangleMesh m(out->volumes[i]->mesh());
            m.transform(out->volumes[i]->get_matrix());
            const std::string name = std::string(dir) + "/ring_" + (i == 0 ? "upper" : "lower") + ".stl";
            REQUIRE(store_stl(name.c_str(), &m, true));
        }
    }
}

// ---------------------------------------------------------------------------
// Flipping the cut plane must CARRY the sheet.
//
// The gizmo's "switch sides" gesture turns the plane frame 180 degrees about its
// own X (m_rotation_m * rotation_transform(PI * UnitX)). That frame change alone
// would leave the sheet's control values describing a DIFFERENT world surface -
// the reported "right-click partially flattens the sheet". flip_about_u() is the
// matching change to the height field, and the contract it has to keep is the
// strongest one available: the surface in WORLD space is identical before and
// after, only which half counts as upper and which as lower swaps.
// ---------------------------------------------------------------------------
TEST_CASE("Curved cut: flipping the frame carries the sheet", "[CurvedCut]")
{
    // A deliberately ASYMMETRIC surface: a symmetric one would pass a mirror
    // test for the wrong reason.
    auto make_lopsided = []() {
        CurvedCutSheet s(7);
        s.set_half_size(30.0, 20.0);
        for (int j = 0; j < s.resolution(); ++ j)
            for (int i = 0; i < s.resolution(); ++ i) {
                const Vec2d xy = s.control_xy(i, j);
                s.at(i, j) = 0.05 * xy.x() + 0.004 * xy.y() * xy.y() - 0.0009 * xy.x() * xy.x() * xy.y();
            }
        s.commit_reference();
        return s;
    };

    // The frame turn the gizmo performs, as a transform of the plane frame.
    const Transform3d flip_u = Eigen::Affine3d(Eigen::AngleAxisd(PI, Vec3d::UnitX()));
    const Transform3d flip_v = Eigen::Affine3d(Eigen::AngleAxisd(PI, Vec3d::UnitY()));

    // A point of the sheet in world space, given the plane frame the sheet lives in.
    auto world_point = [](const CurvedCutSheet& s, const Transform3d& frame, double x, double y) {
        return frame * Vec3d(x, y, s.evaluate_local(x, y));
    };

    SECTION("flip about u: the world surface is unchanged")
    {
        const CurvedCutSheet before = make_lopsided();
        CurvedCutSheet       after  = before;
        after.flip_about_u();

        // Same domain, same resolution: a flip must not re-fit anything.
        REQUIRE(after.resolution() == before.resolution());
        REQUIRE(after.half_size_u() == Approx(before.half_size_u()));
        REQUIRE(after.half_size_v() == Approx(before.half_size_v()));

        // Sample densely over the whole domain, off the control points as well as
        // on them, and compare the two surfaces in WORLD space. The flipped sheet
        // is read in the flipped frame, which is what the gizmo will do.
        const int    N  = 41;
        double       worst = 0.0;
        for (int b = 0; b < N; ++ b) {
            const double y = before.half_size_v() * (2.0 * double(b) / double(N - 1) - 1.0);
            for (int a = 0; a < N; ++ a) {
                const double x = before.half_size_u() * (2.0 * double(a) / double(N - 1) - 1.0);
                // The SAME world column: in the flipped frame that column is at
                // local (x, -y), because the frame's Y negated.
                const Vec3d p0 = world_point(before, Transform3d::Identity(), x, y);
                const Vec3d p1 = world_point(after,  flip_u,                 x, -y);
                worst = std::max(worst, (p1 - p0).norm());
            }
        }
        INFO("worst world-space deviation: " << worst << " mm");
        REQUIRE(worst < 1e-6);
    }

    SECTION("flip about v: the world surface is unchanged")
    {
        const CurvedCutSheet before = make_lopsided();
        CurvedCutSheet       after  = before;
        after.flip_about_v();

        const int N  = 41;
        double    worst = 0.0;
        for (int b = 0; b < N; ++ b) {
            const double y = before.half_size_v() * (2.0 * double(b) / double(N - 1) - 1.0);
            for (int a = 0; a < N; ++ a) {
                const double x = before.half_size_u() * (2.0 * double(a) / double(N - 1) - 1.0);
                const Vec3d p0 = world_point(before, Transform3d::Identity(), x, y);
                const Vec3d p1 = world_point(after,  flip_v,                 -x, y);
                worst = std::max(worst, (p1 - p0).norm());
            }
        }
        INFO("worst world-space deviation: " << worst << " mm");
        REQUIRE(worst < 1e-6);
    }

    SECTION("a flip is its own inverse, bit for bit")
    {
        const CurvedCutSheet before = make_lopsided();
        CurvedCutSheet       there  = before;
        there.flip_about_u();
        there.flip_about_u();
        REQUIRE(there.values() == before.values());

        CurvedCutSheet there_v = before;
        there_v.flip_about_v();
        there_v.flip_about_v();
        REQUIRE(there_v.values() == before.values());
    }

    SECTION("a flat sheet stays flat, and a flip republishes the reference")
    {
        CurvedCutSheet flat(5);
        flat.flip_about_u();
        REQUIRE(flat.is_flat());

        // The reference has to follow the flip: otherwise the next extent re-fit
        // would re-sample the PRE-flip surface and quietly undo it. Re-fitting to
        // a different extent and back must reproduce the FLIPPED shape.
        CurvedCutSheet s = make_lopsided();
        s.flip_about_u();
        const std::vector<double> flipped = s.values();
        const double hs_u = s.half_size_u(), hs_v = s.half_size_v();
        s.set_half_size(hs_u * 1.4, hs_v * 1.4, /*resample*/ true);
        s.set_half_size(hs_u, hs_v, /*resample*/ true);
        REQUIRE(s.values().size() == flipped.size());
        for (size_t k = 0; k < flipped.size(); ++ k)
            REQUIRE(s.values()[k] == Approx(flipped[k]).margin(1e-9));
    }
}

// ---------------------------------------------------------------------------
// PHASE 5: non-square control grids (nx columns x ny rows).
//
// The sheet's DOMAIN has been a rectangle since phase 2; the GRID was still one
// count. Splitting it lets the owner ask for a RULED sheet - 10 x 2, where each
// of the ten columns is one straight line along v that can be grabbed from
// either of its two ends.
//
// Two traps, both silent on a square grid, both pinned below:
//   - smooth()'s "too small to smooth" guard used to fire when EITHER axis was
//     under 3, which would switch smoothing off entirely on a 10 x 2 sheet;
//   - flip_about_u() mirrors along V, so it must index over NY (and _v over NX).
//     Swapping them is invisible while nx == ny and out of bounds when it is not.
// ---------------------------------------------------------------------------

// A 10 x 2 sheet whose two rows sit at different, per-column heights: the shape
// the owner described, a ruled bend controlled from either end of each column.
static CurvedCutSheet ruled_sheet_10x2()
{
    CurvedCutSheet s(10, 2);
    s.set_half_size(50.0, 20.0);
    for (int i = 0; i < s.nx(); ++ i) {
        const double t = double(i) / double(s.nx() - 1);
        // Row 0 and row 1 get DIFFERENT functions of the column, so the surface
        // is genuinely ruled rather than a plain extrusion.
        s.at(i, 0) =  6.0 * std::sin(PI * t);
        s.at(i, 1) = -4.0 * t + 1.0;
    }
    s.commit_reference();
    return s;
}

TEST_CASE("Curved cut: a 10 x 2 grid is a ruled surface", "[CurvedCut]")
{
    const CurvedCutSheet s = ruled_sheet_10x2();

    SECTION("the grid counts are what was asked for, and resolution() is the larger")
    {
        REQUIRE(s.nx() == 10);
        REQUIRE(s.ny() == 2);
        // The square compatibility API reports the larger count, the same rule
        // half_size() follows for the rectangular domain.
        REQUIRE(s.resolution() == 10);
        REQUIRE(s.values().size() == 20);
    }

    SECTION("along v the surface is EXACTLY the linear blend of the two rows")
    {
        // This is what "ruled" means: every column is a straight line in the
        // (v, z) plane. It is also the reason a 2-count axis interpolates
        // linearly rather than through the clamped spline - see evaluate().
        // catmull_rom(a, a, b, b, t) is a + (b-a)(0.5t + 1.5t^2 - t^3), an
        // S-curve that departs from the straight line by up to ~4.7% of the
        // rise; a sheet built from that is not ruled and the cut face bows.
        for (int i = 0; i < s.nx(); ++ i) {
            const double u  = s.control_u(i);
            const double z0 = s.at(i, 0);
            const double z1 = s.at(i, 1);
            for (double v : { 0.0, 0.125, 0.25, 1.0 / 3.0, 0.5, 0.75, 0.875, 1.0 }) {
                const double got = s.evaluate(u, v);
                INFO("column " << i << " v " << v);
                REQUIRE(got == Approx(z0 + (z1 - z0) * v).margin(1e-9));
            }
        }
    }

    SECTION("between columns too: v is linear everywhere, not only over a control column")
    {
        // Off a control column the row values are themselves Catmull-Rom
        // interpolations along u, but the v blend must STILL be linear between
        // them - the evaluation is separable, so linearity along v is a property
        // of the axis, not of where we sample u.
        for (double u : { 0.03, 0.17, 0.41, 0.62, 0.88, 0.97 }) {
            const double top = s.evaluate(u, 0.0);
            const double bot = s.evaluate(u, 1.0);
            for (double v : { 0.2, 0.5, 0.9 }) {
                INFO("u " << u << " v " << v);
                REQUIRE(s.evaluate(u, v) == Approx(top + (bot - top) * v).margin(1e-9));
            }
        }
    }

    SECTION("along u it interpolates its control points exactly")
    {
        // Catmull-Rom's defining property, unchanged by the rectangular grid.
        for (int j = 0; j < s.ny(); ++ j)
            for (int i = 0; i < s.nx(); ++ i) {
                INFO("control " << i << "," << j);
                REQUIRE(s.evaluate(s.control_u(i), s.control_v(j)) == Approx(s.at(i, j)).margin(1e-9));
            }
    }

    SECTION("control_v is its own axis, so the handles land on the rectangle's rows")
    {
        // control_xy() used to read control_u(j) for the v axis - the core square
        // assumption. With ny = 2 the two rows must sit on the domain's edges.
        REQUIRE(s.control_v(0) == Approx(0.0));
        REQUIRE(s.control_v(1) == Approx(1.0));
        REQUIRE(s.control_xy(0, 0).y() == Approx(-s.half_size_v()));
        REQUIRE(s.control_xy(0, 1).y() == Approx(+s.half_size_v()));
        // ... and the columns span u evenly, all ten of them.
        REQUIRE(s.control_xy(0, 0).x() == Approx(-s.half_size_u()));
        REQUIRE(s.control_xy(9, 0).x() == Approx(+s.half_size_u()));
        REQUIRE(s.control_xy(1, 0).x() == Approx(-s.half_size_u() + 2.0 * s.half_size_u() / 9.0));
    }
}

TEST_CASE("Curved cut: resampling a non-square grid keeps the shape", "[CurvedCut]")
{
    // A dense (u,v) lattice to measure the surface on, before and after.
    auto sample = [](const CurvedCutSheet& s) {
        std::vector<double> out;
        for (int a = 0; a <= 40; ++ a)
            for (int b = 0; b <= 40; ++ b)
                out.push_back(s.evaluate(double(a) / 40.0, double(b) / 40.0));
        return out;
    };

    SECTION("10 x 2 -> 5 x 5 -> 10 x 2 survives within the resample error")
    {
        const CurvedCutSheet      start  = ruled_sheet_10x2();
        const std::vector<double> before = sample(start);

        CurvedCutSheet s = start;
        s.set_grid(5, 5);
        REQUIRE(s.nx() == 5);
        REQUIRE(s.ny() == 5);
        REQUIRE(s.values().size() == 25);

        // Going 10 -> 5 along u THROWS AWAY nodes (u = 1/9, 2/9 ... are not
        // fifths), so this leg is genuinely lossy and the tolerance is the
        // Catmull-Rom resample error, not zero. Pin the measured figure so a
        // regression that makes it worse is visible.
        s.set_grid(10, 2);
        REQUIRE(s.nx() == 10);
        REQUIRE(s.ny() == 2);
        const std::vector<double> after = sample(s);
        REQUIRE(after.size() == before.size());
        double worst = 0.0;
        for (size_t k = 0; k < before.size(); ++ k)
            worst = std::max(worst, std::abs(after[k] - before[k]));
        INFO("worst round-trip deviation " << worst << " mm on a 6 mm surface");
        REQUIRE(worst < 1.5);

        // The v direction is untouched by the round trip: 2 -> 5 -> 2 keeps the
        // domain's two edge rows, which ARE nodes of the 5-row grid, and the
        // surface is linear along v at both ends. So the ruled property holds
        // again afterwards.
        for (double u : { 0.1, 0.5, 0.9 }) {
            const double top = s.evaluate(u, 0.0), bot = s.evaluate(u, 1.0);
            REQUIRE(s.evaluate(u, 0.5) == Approx(0.5 * (top + bot)).margin(1e-9));
        }
    }

    SECTION("8 x 2 -> 15 x 3 is EXACT at the shared nodes")
    {
        // A refinement in both axes whose nodes include the originals: u = i/7
        // is (2i)/14, and v = 0, 1 are v = 0 and 2/2. Catmull-Rom interpolates
        // its control points, so those must come back bit-for-bit.
        //
        // 8 -> 15 rather than 10 -> 19 because MaxResolution is 15: the true
        // refinement of a 10-column axis is 19 columns, which clamps. That is a
        // limit of the grid's range, not of the resampling.
        CurvedCutSheet start(8, 2);
        start.set_half_size(50.0, 20.0);
        for (int i = 0; i < start.nx(); ++ i) {
            const double t = double(i) / double(start.nx() - 1);
            start.at(i, 0) =  6.0 * std::sin(PI * t);
            start.at(i, 1) = -4.0 * t + 1.0;
        }
        start.commit_reference();

        CurvedCutSheet s = start;
        s.set_grid(15, 3);
        REQUIRE(s.nx() == 15);
        REQUIRE(s.ny() == 3);
        for (int i = 0; i < start.nx(); ++ i) {
            INFO("shared node column " << i);
            REQUIRE(s.at(2 * i, 0) == Approx(start.at(i, 0)).margin(1e-12));
            REQUIRE(s.at(2 * i, 2) == Approx(start.at(i, 1)).margin(1e-12));
            // The new middle row is the ruled surface's own midpoint.
            REQUIRE(s.at(2 * i, 1) == Approx(0.5 * (start.at(i, 0) + start.at(i, 1))).margin(1e-12));
        }
    }

    SECTION("a re-fit of the EXTENT resamples over the right counts")
    {
        // set_half_size(resample) loops the control grid; with the loops or the
        // stride on the wrong axis a 10 x 2 sheet would read out of bounds or
        // transpose. Re-fit out and back: the reference makes it idempotent.
        CurvedCutSheet            s    = ruled_sheet_10x2();
        const std::vector<double> want = s.values();
        const double hs_u = s.half_size_u(), hs_v = s.half_size_v();
        s.set_half_size(hs_u * 1.6, hs_v * 1.3, /*resample*/ true);
        REQUIRE(s.values().size() == 20);
        s.set_half_size(hs_u, hs_v, /*resample*/ true);
        REQUIRE(s.values().size() == want.size());
        for (size_t k = 0; k < want.size(); ++ k)
            REQUIRE(s.values()[k] == Approx(want[k]).margin(1e-9));
    }
}

TEST_CASE("Curved cut: flipping a non-square grid mirrors over the right axis", "[CurvedCut]")
{
    // THE TRAP. flip_about_u() mirrors along v (over ny); flip_about_v() mirrors
    // along u (over nx). On a square grid the counts are equal and a swap is
    // invisible - on 10 x 2 it indexes off the end of the grid.

    SECTION("flip_about_u satisfies f_new(x, -y) == -f_old(x, y)")
    {
        const CurvedCutSheet before = ruled_sheet_10x2();
        CurvedCutSheet       after  = before;
        after.flip_about_u();

        // The grid keeps its shape and its domain: a flip is an index mirror, not
        // a re-fit.
        REQUIRE(after.nx() == before.nx());
        REQUIRE(after.ny() == before.ny());
        REQUIRE(after.half_size_u() == Approx(before.half_size_u()));
        REQUIRE(after.half_size_v() == Approx(before.half_size_v()));

        for (double x : { -50.0, -31.0, -7.0, 0.0, 12.5, 33.0, 50.0 })
            for (double y : { -20.0, -9.0, 0.0, 4.0, 15.0, 20.0 }) {
                INFO("local (" << x << ", " << y << ")");
                REQUIRE(after.evaluate_local(x, -y) == Approx(-before.evaluate_local(x, y)).margin(1e-9));
            }
    }

    SECTION("flip_about_v satisfies f_new(-x, y) == -f_old(x, y)")
    {
        const CurvedCutSheet before = ruled_sheet_10x2();
        CurvedCutSheet       after  = before;
        after.flip_about_v();

        for (double x : { -50.0, -31.0, -7.0, 0.0, 12.5, 33.0, 50.0 })
            for (double y : { -20.0, -9.0, 0.0, 4.0, 15.0, 20.0 }) {
                INFO("local (" << x << ", " << y << ")");
                REQUIRE(after.evaluate_local(-x, y) == Approx(-before.evaluate_local(x, y)).margin(1e-9));
            }
    }

    SECTION("both flips are their own inverse, bit for bit")
    {
        const CurvedCutSheet before = ruled_sheet_10x2();

        CurvedCutSheet u2 = before;
        u2.flip_about_u();
        u2.flip_about_u();
        REQUIRE(u2.values() == before.values());

        CurvedCutSheet v2 = before;
        v2.flip_about_v();
        v2.flip_about_v();
        REQUIRE(v2.values() == before.values());
    }

    SECTION("the mirrors act on the axis they name, and on no other")
    {
        // The sharpest form of the trap: flip_about_u swaps ROW 0 with ROW 1 and
        // leaves the COLUMN order alone; flip_about_v does the opposite. Read the
        // control values directly, so nothing hides behind the evaluation.
        const CurvedCutSheet before = ruled_sheet_10x2();

        CurvedCutSheet fu = before;
        fu.flip_about_u();
        for (int i = 0; i < before.nx(); ++ i) {
            INFO("column " << i);
            REQUIRE(fu.at(i, 0) == Approx(-before.at(i, 1)).margin(1e-12));
            REQUIRE(fu.at(i, 1) == Approx(-before.at(i, 0)).margin(1e-12));
        }

        CurvedCutSheet fv = before;
        fv.flip_about_v();
        for (int j = 0; j < before.ny(); ++ j)
            for (int i = 0; i < before.nx(); ++ i) {
                INFO("control " << i << "," << j);
                REQUIRE(fv.at(i, j) == Approx(-before.at(before.nx() - 1 - i, j)).margin(1e-12));
            }
    }
}

TEST_CASE("Curved cut: smoothing a 2-row grid works and leaves v alone", "[CurvedCut]")
{
    // The other trap. The guard used to be "either count < 3 -> do nothing",
    // which would have made Smooth a no-op on every ruled sheet.

    SECTION("it does not crash, and it does change the surface along u")
    {
        CurvedCutSheet            s      = ruled_sheet_10x2();
        const std::vector<double> before = s.values();
        s.smooth(0.5);
        REQUIRE(s.values().size() == before.size());
        REQUIRE(s.values() != before);   // the u axis has eight interior columns
        REQUIRE(std::isfinite(s.max_displacement()));
    }

    SECTION("the two rows keep their separation: no smoothing happens ALONG v")
    {
        // A 2-point axis takes no part in the Laplacian, so the pass is a 1D
        // smooth along u applied to each row and the rows are never blended into
        // each other. This is NOT what edge clamping alone would give: at ny == 2
        // row 0's j+1 neighbour is row 1, a real point, so a plain 4-neighbour
        // average would drag 5 and -3 to 3 and -1 in a single full-strength pass
        // and flatten the ruling the sheet exists for.
        CurvedCutSheet s = ruled_sheet_10x2();
        // Make both rows constant along u, so the u smoothing has nothing to do
        // and only a v leak could move anything.
        for (int i = 0; i < s.nx(); ++ i) {
            s.at(i, 0) =  5.0;
            s.at(i, 1) = -3.0;
        }
        s.commit_reference();
        s.smooth(1.0);
        for (int i = 0; i < s.nx(); ++ i) {
            INFO("column " << i);
            REQUIRE(s.at(i, 0) == Approx(5.0).margin(1e-12));
            REQUIRE(s.at(i, 1) == Approx(-3.0).margin(1e-12));
        }
    }

    SECTION("a grid short on BOTH axes still smooths to nothing")
    {
        // 2 x 2 is a tilted plane: no interior point on either axis, so there is
        // genuinely nothing for a Laplacian to do and the guard still holds.
        CurvedCutSheet s(2, 2);
        s.set_half_size(20.0, 20.0);
        s.at(0, 0) = 1.0;
        s.at(1, 1) = -2.0;
        const std::vector<double> before = s.values();
        s.smooth(1.0);
        REQUIRE(s.values() == before);
    }
}

TEST_CASE("Curved cut: the square API still means what it meant", "[CurvedCut]")
{
    // The compatibility proof for resolution() / set_resolution(int) / reset(int).
    SECTION("the one-int constructors and setters make a SQUARE grid")
    {
        CurvedCutSheet s(7);
        REQUIRE(s.nx() == 7);
        REQUIRE(s.ny() == 7);
        REQUIRE(s.resolution() == 7);

        s.set_resolution(9);
        REQUIRE(s.nx() == 9);
        REQUIRE(s.ny() == 9);

        s.reset(4);
        REQUIRE(s.nx() == 4);
        REQUIRE(s.ny() == 4);
        REQUIRE(s.is_flat());
    }

    SECTION("set_resolution on a non-square grid squares it up")
    {
        CurvedCutSheet s = ruled_sheet_10x2();
        s.set_resolution(6);
        REQUIRE(s.nx() == 6);
        REQUIRE(s.ny() == 6);
    }

    SECTION("reset() with no argument keeps the counts, square or not")
    {
        CurvedCutSheet s = ruled_sheet_10x2();
        s.reset();
        REQUIRE(s.nx() == 10);
        REQUIRE(s.ny() == 2);
        REQUIRE(s.is_flat());
    }

    SECTION("MinResolution allows a ruled axis; Max is unchanged")
    {
        REQUIRE(CurvedCutSheet::MinResolution == 2);
        REQUIRE(CurvedCutSheet::MaxResolution == 15);
        // Out-of-range counts clamp per axis rather than being refused.
        CurvedCutSheet s(1, 99);
        REQUIRE(s.nx() == CurvedCutSheet::MinResolution);
        REQUIRE(s.ny() == CurvedCutSheet::MaxResolution);
    }
}

TEST_CASE("Curved cut: the default grid follows each extent separately", "[CurvedCut]")
{
    // The single-answer form is set by the LONGER axis, which on a long thin part
    // stretches that density across the short one. The two-output form gives each
    // axis the count its own extent asks for.
    int nx = 0, ny = 0;
    curved_cut_default_grid(50.0, 10.0, nx, ny, 10.0, 5);
    REQUIRE(nx == 11);                                   // 100 mm span
    REQUIRE(ny == 5);                                    // 20 mm span -> 3, floored at 5
    // The single-answer form would have given 11 for BOTH.
    REQUIRE(curved_cut_default_resolution(50.0, 10.0, 10.0, 5) == 11);

    // Square in, square out - so the fit's behaviour on a cube is unchanged.
    curved_cut_default_grid(30.0, 30.0, nx, ny, 10.0, 5);
    REQUIRE(nx == ny);
    REQUIRE(nx == curved_cut_default_resolution(30.0, 30.0, 10.0, 5));

    // The floor applies to both axes, so the automatic fit NEVER chooses a ruled
    // grid on its own: 10 x 2 is the user's decision about the shape of the cut.
    curved_cut_default_grid(60.0, 0.5, nx, ny, 10.0, 5);
    REQUIRE(ny >= 5);
}

TEST_CASE("Curved cut: a ruled 10 x 2 sheet cuts a cube", "[CurvedCut]")
{
    const indexed_triangle_set cube = centred_cube();

    // A ruled bend across the cube: the sheet tilts one way at one end and the
    // other at the far end, so the cut face is a genuine ruled surface rather
    // than a plane. Keep it inside the cube's half height (20 mm) with margin.
    CurvedCutSheet s(10, 2);
    s.set_half_size(30.0, 30.0);
    for (int i = 0; i < s.nx(); ++ i) {
        const double t = double(i) / double(s.nx() - 1);
        s.at(i, 0) = -6.0 + 12.0 * t;
        s.at(i, 1) =  6.0 - 12.0 * t;
    }
    s.commit_reference();
    REQUIRE(!s.is_flat());

    indexed_triangle_set upper, lower;
    REQUIRE(curved_cut_split(cube, s, &upper, &lower));

    // Both halves closed, and their volumes account for the whole cube.
    REQUIRE(its_num_open_edges(upper) == 0);
    REQUIRE(its_num_open_edges(lower) == 0);
    const double total = CUBE * CUBE * CUBE;
    const double vu = double(its_volume(upper));
    const double vl = double(its_volume(lower));
    INFO("upper " << vu << " lower " << vl << " total " << total);
    REQUIRE(std::abs(vu + vl - total) / total < 1e-3);
    REQUIRE(vu > 0.05 * total);
    REQUIRE(vl > 0.05 * total);

    // The cut face is RULED: along v the surface is a straight line, so the
    // midpoint of every column sits exactly halfway between its two ends. This
    // is the property the whole feature exists for, checked on the sheet the
    // cut was actually built from.
    for (double u : { 0.0, 0.2, 0.37, 0.5, 0.63, 0.8, 1.0 }) {
        const double top = s.evaluate(u, 0.0), bot = s.evaluate(u, 1.0);
        for (double v : { 0.25, 0.5, 0.75 }) {
            INFO("u " << u << " v " << v);
            REQUIRE(s.evaluate(u, v) == Approx(top + (bot - top) * v).margin(1e-9));
        }
    }
}
