#include <catch2/catch.hpp>

#include <libslic3r/DrawCut.hpp>
#include <libslic3r/CurvedCut.hpp>
#include <libslic3r/CutUtils.hpp>
#include <libslic3r/Geometry.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/TriangleMesh.hpp>

#include <algorithm>
#include <cmath>
#include <cstdlib>

using namespace Slic3r;

// The test article, the same one the [CurvedCut] suite uses: a 40 mm cube centred
// on the origin, so the cut plane frame (z == 0) runs through its middle and the
// top face sits at z == +20.
static const double CUBE = 40.0;

static indexed_triangle_set centred_cube(double s = CUBE)
{
    indexed_triangle_set its = its_make_cube(s, s, s);
    for (Vec3f& v : its.vertices)
        v -= Vec3f(float(0.5 * s), float(0.5 * s), float(0.5 * s));
    return its;
}

// A closed circular stroke on the cube's TOP face: every sample sits at
// z == +20 with the face's own +Z normal, which is what the raycast would have
// produced had a user dragged a loop there.
static DrawCutStroke circle_on_top(double radius, int n = 64, double z = 0.5 * CUBE, bool clockwise = false)
{
    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double a = (clockwise ? -1.0 : 1.0) * 2.0 * M_PI * double(i) / double(n);
        stroke.append(Vec3d(radius * std::cos(a), radius * std::sin(a), z), Vec3d::UnitZ(), size_t(i));
    }
    // Close the ring by coming back to (nearly) the start, the way a hand-drawn
    // loop does - finish() decides closed from the gap, so the test exercises
    // that decision rather than asserting past it.
    stroke.append(Vec3d(radius, 0.0, z), Vec3d::UnitZ(), 0);
    return stroke;
}

// An open straight stroke across the cube's top face, along +X, reaching from
// -half to +half of the span given.
static DrawCutStroke line_on_top(double half_span, int n = 40, double z = 0.5 * CUBE)
{
    DrawCutStroke stroke;
    for (int i = 0; i < n; ++ i) {
        const double t = double(i) / double(n - 1);
        stroke.append(Vec3d(-half_span + 2.0 * half_span * t, 0.0, z), Vec3d::UnitZ(), size_t(i));
    }
    return stroke;
}

static bool watertight(const indexed_triangle_set& its)
{
    return !its.empty() && its_num_open_edges(its) == 0;
}

// ---------------------------------------------------------------------------
// (1) The resampler. There is no 3D resampler in this codebase, so this is the
// new code's own contract: equal spacing, arc length preserved, and the LAST
// point actually emitted (the wart equally_spaced_points has and this must not).
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the resampler spaces evenly and keeps the endpoint", "[DrawCut]")
{
    // A synthetic stroke with deliberately uneven input spacing: a helix sampled
    // at a varying rate, so a resampler that just copied points would fail.
    std::vector<DrawCutSample> in;
    double raw_len = 0.0;
    Vec3d  prev    = Vec3d::Zero();
    for (int i = 0; i <= 200; ++ i) {
        const double t = double(i) / 200.0;
        // Uneven parameterisation: t^1.5 bunches the samples at the start.
        const double s = std::pow(t, 1.5) * 4.0 * M_PI;
        DrawCutSample smp;
        smp.pos    = Vec3d(10.0 * std::cos(s), 10.0 * std::sin(s), 3.0 * s);
        smp.normal = Vec3d(std::cos(s), std::sin(s), 0.0);
        in.push_back(smp);
        if (i > 0)
            raw_len += (smp.pos - prev).norm();
        prev = smp.pos;
    }

    const double spacing = 1.0;
    const std::vector<DrawCutSample> out = draw_cut_resample(in, spacing, /*closed*/ false);

    REQUIRE(out.size() > 10);

    // Every interior span is `spacing` long. The samples sit at exact multiples of
    // spacing in ARC LENGTH along the source polyline, so a span that straddles a
    // source vertex is a chord slightly shorter than its arc - which is a property
    // of the input's own faceting, not a drift: it does not accumulate, and it
    // bounds at the sag of one input span. On a helix sampled 200 times that is
    // under 0.1% of a millimetre span, and it must stay there.
    for (size_t i = 1; i + 1 < out.size(); ++ i) {
        const double d = (out[i].pos - out[i - 1].pos).norm();
        REQUIRE(d <= spacing + 1e-9);        // never LONGER than the arc
        REQUIRE(std::abs(d - spacing) < 1e-3);
    }
    const double last = (out.back().pos - out[out.size() - 2].pos).norm();
    REQUIRE(last > 0.5 * spacing - 1e-9);
    REQUIRE(last < spacing + 1e-6);

    // The first input sample survives exactly, and the END OF THE STROKE IS
    // REACHED - the wart this test exists for. equally_spaced_points emits the
    // first point but not reliably the last, so a stroke resampled through it can
    // stop up to a full spacing short of where the user let go. Here the last
    // output sample is either the input endpoint itself or within half a spacing of
    // it (closer than that and appending it would leave a sliver span the
    // central-difference tangent would read as a near-zero direction).
    REQUIRE((out.front().pos - in.front().pos).norm() < 1e-12);
    REQUIRE((out.back().pos - in.back().pos).norm() <= 0.5 * spacing + 1e-9);

    // Arc length is preserved to better than 0.5%: resampling a curve at 1 mm
    // chords loses a little to the chord sag, and this pins how little.
    double out_len = 0.0;
    for (size_t i = 1; i < out.size(); ++ i)
        out_len += (out[i].pos - out[i - 1].pos).norm();
    REQUIRE(std::abs(out_len - raw_len) / raw_len < 0.005);

    // Normals come back unit length, interpolated rather than copied.
    for (const DrawCutSample& s : out)
        REQUIRE(std::abs(s.normal.norm() - 1.0) < 1e-9);
}

TEST_CASE("Draw cut: a resampled ring stays open and wraps", "[DrawCut]")
{
    // A circle of radius 10: circumference 62.83 mm, so at 1 mm spacing the ring
    // holds about 63 samples and the closing span is resampled too.
    std::vector<DrawCutSample> in;
    const int n = 128;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        DrawCutSample s;
        s.pos    = Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0);
        s.normal = Vec3d::UnitZ();
        in.push_back(s);
    }

    const std::vector<DrawCutSample> out = draw_cut_resample(in, 1.0, /*closed*/ true);

    // The output must NOT duplicate the first sample at the end - the strip
    // builder wraps, so a duplicate would give it a zero-length span.
    REQUIRE((out.back().pos - out.front().pos).norm() > 0.5);
    // ... and the wrap span must itself be about one spacing, i.e. the ring is
    // evenly covered all the way round.
    const double wrap = (out.front().pos - out.back().pos).norm();
    REQUIRE(wrap > 0.5);
    REQUIRE(wrap < 1.6);
    // Circumference within 0.5%.
    double len = 0.0;
    for (size_t i = 1; i < out.size(); ++ i)
        len += (out[i].pos - out[i - 1].pos).norm();
    len += wrap;
    REQUIRE(std::abs(len - 2.0 * M_PI * 10.0) / (2.0 * M_PI * 10.0) < 0.005);
}

TEST_CASE("Draw cut: the closing tolerance decides open from closed", "[DrawCut]")
{
    // max(3 * spacing, 2 mm).
    REQUIRE(draw_cut_closing_tolerance(1.0) == Approx(3.0));
    REQUIRE(draw_cut_closing_tolerance(0.1) == Approx(2.0));  // the 2 mm floor
    REQUIRE(draw_cut_closing_tolerance(2.0) == Approx(6.0));

    // A loop whose ends are 1 mm apart closes at spacing 1 (tolerance 3 mm)...
    DrawCutStroke nearly;
    const int n = 60;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * 0.99 * double(i) / double(n - 1);
        nearly.append(Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0), Vec3d::UnitZ());
    }
    const double gap = (nearly.samples().back().pos - nearly.samples().front().pos).norm();
    REQUIRE(gap < 3.0);
    REQUIRE(nearly.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(nearly.is_closed());

    // ... and an arc whose ends are far apart does not.
    DrawCutStroke arc;
    for (int i = 0; i < 40; ++ i) {
        const double a = M_PI * double(i) / 39.0; // half a circle
        arc.append(Vec3d(10.0 * std::cos(a), 10.0 * std::sin(a), 0.0), Vec3d::UnitZ());
    }
    REQUIRE(arc.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!arc.is_closed());

    // Force closed takes an open stroke and rings it anyway.
    REQUIRE(arc.finish(1.0, 0.0, /*force_closed*/ true) == DrawCutError::None);
    REQUIRE(arc.is_closed());
}

// ---------------------------------------------------------------------------
// (2) Smoothing.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: smoothing settles and is bounded", "[DrawCut]")
{
    // A zigzag: a straight line with alternating 1 mm spikes, which is the shape
    // a raw stroke over a faceted surface actually has.
    auto make = []() {
        std::vector<DrawCutSample> p;
        for (int i = 0; i < 41; ++ i) {
            DrawCutSample s;
            s.pos    = Vec3d(double(i), (i % 2 == 0 ? 1.0 : -1.0), 0.0);
            s.normal = Vec3d::UnitZ();
            p.push_back(s);
        }
        return p;
    };
    const std::vector<DrawCutSample> raw = make();

    std::vector<DrawCutSample> a = make();
    draw_cut_smooth(a, 2, /*closed*/ false);
    std::vector<DrawCutSample> b = a;
    draw_cut_smooth(b, 2, /*closed*/ false);

    // NO SAMPLE MOVES FURTHER THAN THE ZIGZAG'S OWN AMPLITUDE. A smoothing pass
    // is an average of neighbours, so it can never push a sample outside the
    // convex hull of its neighbourhood - which for this input is |y| <= 1.
    double max_move_first = 0.0, max_move_second = 0.0;
    for (size_t i = 0; i < raw.size(); ++ i) {
        max_move_first  = std::max(max_move_first,  (a[i].pos - raw[i].pos).norm());
        max_move_second = std::max(max_move_second, (b[i].pos - a[i].pos).norm());
        REQUIRE(std::abs(a[i].pos.y()) <= 1.0 + 1e-12);
        REQUIRE(std::abs(b[i].pos.y()) <= 1.0 + 1e-12);
    }
    REQUIRE(max_move_first > 1e-6);            // it did something
    REQUIRE(max_move_second < max_move_first); // and the second N passes did less

    // Endpoints are HELD for an open stroke: the ends are where the user decided
    // the cut should reach, and letting the average creep them inwards would
    // shorten the stroke on every pass.
    REQUIRE((a.front().pos - raw.front().pos).norm() < 1e-15);
    REQUIRE((a.back().pos  - raw.back().pos).norm()  < 1e-15);

    // Zero passes is a no-op, and the panel's 0..1 maps onto 0..10 passes.
    std::vector<DrawCutSample> z = make();
    draw_cut_smooth(z, 0, false);
    for (size_t i = 0; i < raw.size(); ++ i)
        REQUIRE((z[i].pos - raw[i].pos).norm() < 1e-15);
    REQUIRE(draw_cut_smooth_passes(0.0) == 0);
    REQUIRE(draw_cut_smooth_passes(1.0) == DrawCutStroke::MaxSmoothPasses);
    REQUIRE(draw_cut_smooth_passes(0.2) == 2);

    // A closed path wraps, so its "first" sample moves too.
    std::vector<DrawCutSample> c = make();
    draw_cut_smooth(c, 2, /*closed*/ true);
    REQUIRE((c.front().pos - raw.front().pos).norm() > 1e-9);

    // Normals stay unit length through every pass.
    for (const DrawCutSample& s : b)
        REQUIRE(std::abs(s.normal.norm() - 1.0) < 1e-9);
}

// ---------------------------------------------------------------------------
// (3) The gates: too short, self-crossing, leaving the mesh.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a stroke that is too short is refused with a clear error", "[DrawCut]")
{
    // One sample is not a stroke.
    DrawCutStroke one;
    one.append(Vec3d::Zero(), Vec3d::UnitZ());
    REQUIRE(one.finish() == DrawCutError::TooShort);
    REQUIRE(!one.valid());
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::TooShort)).find("longer") != std::string::npos);

    // A 1 mm dab is under MinLength (2 mm), so it is refused even though it has
    // samples to spare.
    DrawCutStroke dab;
    for (int i = 0; i < 10; ++ i)
        dab.append(Vec3d(0.1 * double(i), 0.0, 0.0), Vec3d::UnitZ());
    REQUIRE(dab.finish(0.1, 0.0) == DrawCutError::TooShort);
    REQUIRE(!dab.valid());

    // And the split refuses it rather than producing something.
    const indexed_triangle_set cube = centred_cube();
    DrawCutParams params;
    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(!draw_cut_split(cube, dab, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::TooShort);
}

TEST_CASE("Draw cut: a stroke that leaves the mesh keeps its longest run", "[DrawCut]")
{
    // Capture skips misses, so a stroke that crossed a hole comes back as two
    // runs with a wide gap between them. Bridging it would run a chord through
    // air, so the longest run is kept - here, the 30 mm one.
    DrawCutStroke s;
    for (int i = 0; i < 31; ++ i)
        s.append(Vec3d(double(i), 0.0, 0.0), Vec3d::UnitZ());
    // ... then a 500 mm jump and a short tail.
    for (int i = 0; i < 5; ++ i)
        s.append(Vec3d(500.0 + double(i), 0.0, 0.0), Vec3d::UnitZ());

    REQUIRE(s.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(s.valid());
    // The kept run is the 30 mm one, not the 534 mm span across the gap.
    REQUIRE(s.length() == Approx(30.0).margin(1.0));
    for (const DrawCutSample& smp : s.path())
        REQUIRE(smp.pos.x() < 100.0);

    // And when the longest surviving run is itself too short, the reported reason is
    // LeavesMesh, not TooShort - the user's line WAS long enough, it just was not all
    // on the part, so "draw a longer line" would be the wrong advice.
    DrawCutStroke tiny;
    tiny.append(Vec3d(0.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(0.5, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.5, 0.0, 0.0), Vec3d::UnitZ());
    REQUIRE(tiny.finish(1.0, 0.0) == DrawCutError::LeavesMesh);
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::LeavesMesh)).find("leaves") != std::string::npos);
}

TEST_CASE("Draw cut: a self-crossing stroke is detected and refused", "[DrawCut]")
{
    // A figure-of-eight in the plane z == 20: a lemniscate crosses itself once at
    // the origin, which is exactly the "no well-defined interior" case phase 1
    // refuses rather than guessing at.
    //
    // THE PHASE OFFSET MATTERS. The plain lemniscate x = sin t, y = sin t cos t
    // STARTS at the crossing point, so the crossing is the stroke's own closure -
    // which the test deliberately does not count, because a polyline's first and
    // last segments always share an endpoint. Starting a quarter turn along puts
    // the crossing in the middle of the stroke, where it is a real crossing of two
    // non-adjacent segments and where a hand-drawn figure-of-eight would put it.
    DrawCutStroke eight;
    const int n = 160;
    for (int i = 0; i <= n; ++ i) {
        const double t = 0.5 * M_PI + 2.0 * M_PI * double(i) / double(n);
        eight.append(Vec3d(12.0 * std::sin(t), 12.0 * std::sin(t) * std::cos(t), 0.5 * CUBE), Vec3d::UnitZ());
    }

    REQUIRE(eight.finish(1.0, 0.0) == DrawCutError::SelfCrossing);
    REQUIRE(!eight.valid());
    REQUIRE(std::string(draw_cut_error_message(DrawCutError::SelfCrossing)).find("crosses itself") != std::string::npos);

    const indexed_triangle_set cube = centred_cube();
    DrawCutParams params;
    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(!draw_cut_split(cube, eight, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::SelfCrossing);

    // A plain circle is NOT self-crossing - the negative control that proves the
    // test is not just always true.
    DrawCutStroke circle = circle_on_top(12.0);
    REQUIRE(circle.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_self_crossing(circle));
}

TEST_CASE("Draw cut: a tight concave corner folds the ruled strip", "[DrawCut]")
{
    // WHICH WAY A CORNER HAS TO TURN TO FOLD. The outward rail is the stroke pushed
    // out by E along the outward binormal, so it folds only where the stroke's turn
    // centre is on the OUTWARD side - a CONCAVE corner. At a convex corner the
    // offset moves away from the centre and the rail merely gets longer, which is
    // why a circle drawn as a loop never folds however tight it is. Both halves of
    // that are checked here, because getting the sign backwards would have made
    // every loop look like a fold.

    // (a) A closed loop with a deep concave notch: a 5-lobed "flower" whose inner
    // radius turns tightly the wrong way.
    DrawCutStroke flower;
    const int n = 240;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 14.0 + 5.0 * std::cos(5.0 * a); // 9 .. 19 mm
        flower.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ());
    }
    flower.append(Vec3d(19.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ());
    REQUIRE(flower.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(flower.is_closed());

    double kappa = 0.0;
    // The notches turn with a radius of a couple of mm, so a 5 mm extension folds
    // them and a 0.2 mm one does not.
    REQUIRE(draw_cut_strip_folds(flower, 5.0, &kappa));
    REQUIRE(kappa > 1.0 / 5.0);
    REQUIRE(!draw_cut_strip_folds(flower, 0.2, nullptr));

    // (b) A PLAIN CIRCLE NEVER FOLDS, at any extension - the negative control that
    // proves the sign is the right way round. A 3 mm circle has curvature 1/3 per
    // mm, so a naive "E * kappa > 1" without the sign test would have called this
    // a fold at any E above 3 mm.
    DrawCutStroke tight = circle_on_top(3.0, 48);
    REQUIRE(tight.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(tight.is_closed());
    REQUIRE(!draw_cut_strip_folds(tight, 5.0, nullptr));
    REQUIRE(!draw_cut_strip_folds(tight, 50.0, nullptr));

    // (c) An OPEN stroke that WAVES: its binormal is parallel-transported along the
    // stroke rather than set by a winding, so it keeps one side, and a wave turns
    // toward that side on every other crest. A tight wave therefore folds and a
    // gentle one does not - which is the same property as (a) and (b), measured
    // where there is no interior to orient against.
    auto wave = [](double amp, double period) {
        DrawCutStroke w;
        for (int i = 0; i <= 200; ++ i) {
            const double x = -20.0 + 40.0 * double(i) / 200.0;
            w.append(Vec3d(x, amp * std::sin(2.0 * M_PI * x / period), 0.5 * CUBE), Vec3d::UnitZ());
        }
        return w;
    };

    DrawCutStroke tight_wave = wave(3.0, 8.0); // radius of curvature ~ 1 mm at a crest
    REQUIRE(tight_wave.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(!tight_wave.is_closed());
    double k2 = 0.0;
    REQUIRE(draw_cut_strip_folds(tight_wave, 5.0, &k2));
    REQUIRE(k2 > 1.0 / 5.0);

    DrawCutStroke gentle_wave = wave(1.0, 60.0); // nearly straight
    REQUIRE(gentle_wave.finish(0.5, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_strip_folds(gentle_wave, 5.0, nullptr));
}

// ---------------------------------------------------------------------------
// (4) The headline case: a closed circle on a cube's top face, Direction =
// Surface normal, through all, gives a cylindrical plug.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a closed circle on a cube's top face cuts a cylindrical plug", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    const double R = 12.0;
    DrawCutStroke stroke = circle_on_top(R, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());
    REQUIRE(stroke.valid());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);

    // Two parts, both watertight.
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // `upper` is the PLUG - the piece the stroke drew around. On a cube's flat top
    // face the inward normal is -Z everywhere, so the plug is a cylinder of
    // radius R through the full 40 mm height.
    const double expect_plug = M_PI * R * R * CUBE;
    const double plug        = double(its_volume(upper));
    REQUIRE(plug == Approx(expect_plug).epsilon(0.05)); // the spec's +/- 5%

    // Volume is conserved: with no kerf the two halves partition the cube.
    const double rest = double(its_volume(lower));
    REQUIRE(plug + rest == Approx(cube_volume).epsilon(1e-3));

    // The plug's cross-section is a circle of radius R at several heights: the
    // widest |xy| of any vertex is R, and the plug spans the cube's full height.
    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    REQUIRE(bb.min.z() == Approx(-0.5 * CUBE).margin(1e-3));
    REQUIRE(bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));
    // A 1 mm resampled circle is a polygon inscribed in the circle, so its
    // circumradius is R and its inradius is R * cos(pi/n) - a hair under. Both
    // stay within a chord of R.
    double max_r = 0.0;
    for (const Vec3f& v : upper.vertices)
        max_r = std::max(max_r, v.cast<double>().head<2>().norm());
    REQUIRE(max_r == Approx(R).margin(0.2));
}

TEST_CASE("Draw cut: the plug does not depend on which way the loop was drawn", "[DrawCut]")
{
    // WINDING DECIDES. A loop drawn clockwise and one drawn counter-clockwise must
    // give the SAME plug - the user should not have to care about drag direction,
    // which is what the binormal sign flip in compute_binormals() is for.
    const indexed_triangle_set cube = centred_cube();

    auto run = [&](bool clockwise) {
        DrawCutStroke s = circle_on_top(10.0, 96, 0.5 * CUBE, clockwise);
        REQUIRE(s.finish(1.0, 0.0) == DrawCutError::None);
        REQUIRE(s.is_closed());
        DrawCutParams params;
        params.extension = 5.0;
        indexed_triangle_set up, lo;
        REQUIRE(draw_cut_split(cube, s, params, &up, &lo, nullptr));
        return std::make_pair(up, lo);
    };

    const auto ccw = run(false);
    const auto cw  = run(true);

    // Same plug volume, same plug bounding box - to the accuracy the resampled
    // POLYGON itself has, not bit-for-bit. A 1 mm resample of a 62.8 mm circle does
    // not divide evenly, so traversing it the other way lands the samples at a
    // different phase and the inscribed polygon differs by a fraction of the chord
    // sag. What must not differ is which piece comes back as the plug, or how big
    // it is, which is the actual claim: winding decides, drag direction does not.
    REQUIRE(double(its_volume(ccw.first)) == Approx(double(its_volume(cw.first))).epsilon(1e-3));
    REQUIRE(double(its_volume(ccw.second)) == Approx(double(its_volume(cw.second))).epsilon(1e-3));

    BoundingBoxf3 bb_ccw, bb_cw;
    for (const Vec3f& v : ccw.first.vertices) bb_ccw.merge(v.cast<double>());
    for (const Vec3f& v : cw.first.vertices)  bb_cw.merge(v.cast<double>());
    REQUIRE((bb_ccw.min - bb_cw.min).cwiseAbs().maxCoeff() < 0.05);
    REQUIRE((bb_ccw.max - bb_cw.max).cwiseAbs().maxCoeff() < 0.05);

    // And both really are the plug (much smaller than the cube), not the
    // remainder - i.e. the swap in draw_cut_split() went the right way.
    REQUIRE(double(its_volume(ccw.first)) < 0.5 * double(its_volume(cube)));
}

// ---------------------------------------------------------------------------
// (5) An open stroke across a box splits it in two.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: an open line across a box splits it in two", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    // A line along +X across the full 40 mm span, with Extension 5 so the strip
    // reaches past both silhouettes.
    DrawCutStroke stroke = line_on_top(0.5 * CUBE);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!stroke.is_closed());
    REQUIRE(stroke.valid());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);

    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // The volumes sum to the original - the whole point of a split with no kerf.
    const double vu = double(its_volume(upper));
    const double vl = double(its_volume(lower));
    REQUIRE(vu + vl == Approx(cube_volume).epsilon(1e-3));

    // The line runs down the middle, so both halves are about half the cube.
    REQUIRE(vu == Approx(0.5 * cube_volume).epsilon(0.02));
    REQUIRE(vl == Approx(0.5 * cube_volume).epsilon(0.02));

    // And they lie on opposite sides of y == 0, which is where the stroke was.
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : upper.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : lower.vertices) bl.merge(v.cast<double>());
    REQUIRE(std::abs(bu.center().y() - bl.center().y()) > 5.0);
}

TEST_CASE("Draw cut: a line that does not reach across leaves a side empty", "[DrawCut]")
{
    // THE NEGATIVE CONTROL the spec asks for: the same line, too short, with no
    // Extension. The strip does not reach the silhouettes, so it cannot separate
    // the part and the empty-side test says so BEFORE two booleans run.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = line_on_top(8.0); // a 16 mm line across a 40 mm cube
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!stroke.is_closed());

    DrawCutParams params;
    params.extension   = 0.0;
    params.through_all = true;

    bool upper_empty = false, lower_empty = false;
    draw_cut_empty_sides(cube, stroke, params, upper_empty, lower_empty);
    // One side has nothing: the strip is entirely inside the part, so "inside the
    // cutter" holds no vertex of the cube at all.
    REQUIRE((upper_empty || lower_empty));

    // The same line WITH enough extension does separate it - which is what the
    // panel's "increase Extension" advice is for.
    DrawCutStroke across = line_on_top(0.5 * CUBE);
    REQUIRE(across.finish(1.0, 0.0) == DrawCutError::None);
    DrawCutParams ok_params;
    ok_params.extension = 5.0;
    bool ue = true, le = true;
    draw_cut_empty_sides(cube, across, ok_params, ue, le);
    REQUIRE(!ue);
    REQUIRE(!le);
}

// ---------------------------------------------------------------------------
// (6) The kerf.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: a thickness leaves a gap of that width", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    // A straight line across the cube, cut with a 1 mm kerf: the band between the
    // two halves belongs to neither, so the two volumes fall short of the cube by
    // the band's own volume - 1 mm x 40 mm x 40 mm.
    DrawCutStroke stroke = line_on_top(0.5 * CUBE);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams p0;
    p0.extension   = 5.0;
    p0.through_all = true;

    indexed_triangle_set u0, l0;
    REQUIRE(draw_cut_split(cube, stroke, p0, &u0, &l0, nullptr));
    const double sum0 = double(its_volume(u0)) + double(its_volume(l0));
    REQUIRE(sum0 == Approx(cube_volume).epsilon(1e-3));

    DrawCutParams p1 = p0;
    p1.thickness = 1.0;

    indexed_triangle_set u1, l1;
    REQUIRE(draw_cut_split(cube, stroke, p1, &u1, &l1, nullptr));
    REQUIRE(watertight(u1));
    REQUIRE(watertight(l1));

    const double sum1 = double(its_volume(u1)) + double(its_volume(l1));
    const double band = 1.0 * CUBE * CUBE; // 1 mm along the cut, across the face
    REQUIRE(sum0 - sum1 == Approx(band).epsilon(0.02));

    // The GAP itself is 1 mm: the two halves' facing surfaces are 1 mm apart, so
    // the y extents leave exactly that much between them.
    BoundingBoxf3 bu, bl;
    for (const Vec3f& v : u1.vertices) bu.merge(v.cast<double>());
    for (const Vec3f& v : l1.vertices) bl.merge(v.cast<double>());
    const double gap = bu.center().y() > bl.center().y() ? bu.min.y() - bl.max.y()
                                                         : bl.min.y() - bu.max.y();
    REQUIRE(gap == Approx(1.0).margin(0.05));
}

// ---------------------------------------------------------------------------
// (7) The constant-direction modes: the "as-is" extrusion.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: Axis Z on a sloped face gives a prism of constant section", "[DrawCut]")
{
    // The point of a constant direction is that the cut is a PRISM: its
    // cross-section is the same at every height, whatever the face the stroke was
    // drawn on was doing. So: a circle whose samples sit on a sloped plane (the
    // normals tilted the way a raycast on a wedge would have returned them), cut
    // along -Z. With Direction = Surface normal the plug would lean; with Axis Z
    // it must not.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke;
    const double R = 10.0;
    const int    n = 96;
    // A 30-degree slope in x: z = x * tan(30), and the normal tilted to match.
    const double slope = std::tan(M_PI / 6.0);
    const Vec3d  nrm   = Vec3d(-slope, 0.0, 1.0).normalized();
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double x = R * std::cos(a), y = R * std::sin(a);
        stroke.append(Vec3d(x, y, 0.5 * CUBE + x * slope), nrm);
    }
    stroke.append(Vec3d(R, 0.0, 0.5 * CUBE + R * slope), nrm);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::AxisZ;
    params.extension   = 4.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));
    REQUIRE(double(its_volume(upper)) + double(its_volume(lower)) ==
            Approx(double(its_volume(cube))).epsilon(1e-3));

    // THE PRISM PROPERTY: the plug's section is constant in z.
    //
    // Measured on the vertices rather than by slicing, but NOT by binning them into
    // height bands - a boolean's output has vertices only where geometry actually
    // changes (the two caps and the cut edges), so a band in the middle of a plain
    // cylindrical wall is legitimately empty and a band test there measures nothing.
    //
    // What it measures instead: every vertex of the plug that is not on a cap sits at
    // radius R from the same axis, whatever its height. A LEANING plug - which is
    // what Direction = Surface normal would give on this sloped stroke - has its axis
    // move with z, so its top and bottom vertices would sit at different (x,y)
    // centres. A prism's do not.
    BoundingBoxf3 plug_bb;
    for (const Vec3f& v : upper.vertices)
        plug_bb.merge(v.cast<double>());
    INFO("plug bbox z " << plug_bb.min.z() << " .. " << plug_bb.max.z()
         << ", volume " << its_volume(upper) << " of cube " << its_volume(cube));
    REQUIRE(plug_bb.min.z() == Approx(-0.5 * CUBE).margin(1e-3));
    REQUIRE(plug_bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));

    // The (x,y) EXTENT of the vertices in the bottom third and in the top third of
    // the plug's height. For a prism the two boxes coincide; for a leaning cut the
    // upper box sits offset from the lower one.
    //
    // The extent, not the MEAN of the vertices: a boolean's output has vertices only
    // where geometry changes, and this plug's top rim - where the sloped stroke meets
    // the cube's top face - carries far more of them than its plain bottom cap does,
    // so the vertex mean is pulled around by tessellation density rather than by any
    // lean. (Measured: 1.5 mm of "lean" on a plug whose volume is pi R^2 h to 0.4%
    // and every one of whose vertices is inside radius R - i.e. an artefact.)
    auto third_box = [&upper, &plug_bb](bool bottom) {
        const double lo = plug_bb.min.z();
        const double hi = plug_bb.max.z();
        const double a  = bottom ? lo : lo + 2.0 * (hi - lo) / 3.0;
        const double b  = bottom ? lo + (hi - lo) / 3.0 : hi;
        BoundingBoxf3 bb;
        for (const Vec3f& v : upper.vertices) {
            const Vec3d q = v.cast<double>();
            if (q.z() >= a - 1e-9 && q.z() <= b + 1e-9)
                bb.merge(Vec3d(q.x(), q.y(), 0.0));
        }
        REQUIRE(bb.defined);
        return bb;
    };
    const BoundingBoxf3 box_lo = third_box(true);
    const BoundingBoxf3 box_hi = third_box(false);
    REQUIRE((box_lo.center().head<2>() - box_hi.center().head<2>()).norm() < 0.2);
    REQUIRE(std::abs(box_lo.size().x() - box_hi.size().x()) < 0.3);
    REQUIRE(std::abs(box_lo.size().y() - box_hi.size().y()) < 0.3);

    // And every vertex is within a chord of radius R of the axis - the section is a
    // circle of radius R at every height, not just at the ends.
    for (const Vec3f& v : upper.vertices)
        REQUIRE(v.cast<double>().head<2>().norm() < R + 0.2);

    // The plug really is a cylinder of radius ~R through the whole height: pi R^2 *
    // 40, with the resampled polygon's inscribed-vs-circumscribed slack.
    REQUIRE(double(its_volume(upper)) == Approx(M_PI * R * R * CUBE).epsilon(0.05));
}

TEST_CASE("Draw cut: a constant direction is the same at every sample", "[DrawCut]")
{
    // The cheap structural check that the constant modes really are constant: the
    // cutter built for Axis Z over a sloped stroke has its inner rail on a single
    // plane, because every ray went the same way and the same distance.
    DrawCutStroke stroke;
    const int n = 40;
    for (int i = 0; i < n; ++ i) {
        const double t = double(i) / double(n - 1);
        // A stroke whose z varies, so "the same depth" is visible.
        stroke.append(Vec3d(-15.0 + 30.0 * t, 0.0, 5.0 * t), Vec3d(0.0, 0.0, 1.0));
    }
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    DrawCutParams params;
    params.direction   = DrawCutDirection::AxisZ;
    params.extension   = 2.0;
    params.through_all = false;
    params.depth       = 10.0;

    const indexed_triangle_set cutter = draw_cut_cutter_solid(stroke, params, bbox);
    REQUIRE(!cutter.empty());
    REQUIRE(watertight(cutter));

    // THE PROPERTY: with a constant direction, the inward rail is the stroke
    // translated by exactly the same vector at every sample - here (0, 0, -10). So
    // for every sample there is a cutter vertex at exactly p - 10 * Z, and the
    // cutter's z extent is the stroke's own extent shifted down by 10 (plus whatever
    // the OUTWARD rail and the two tangent extensions add above and around it -
    // which is why this measures the inward points rather than the bounding box: an
    // open stroke's end extensions sit past the stroke's own first and last sample
    // and on a rising stroke that puts one of them below the stroke's lowest point).
    for (const DrawCutSample& smp : stroke.path()) {
        const Vec3d want = smp.pos - Vec3d(0.0, 0.0, 10.0);
        double best = 1e9;
        for (const Vec3f& v : cutter.vertices)
            best = std::min(best, (v.cast<double>() - want).norm());
        REQUIRE(best < 1e-6);
    }

    // And the constancy itself: every inward point is at the SAME depth below its
    // own stroke point, which is what would fail if the direction were per-sample.
    double dmin = 1e9, dmax = -1e9;
    for (const DrawCutSample& smp : stroke.path()) {
        double best = 1e9, best_dz = 0.0;
        for (const Vec3f& v : cutter.vertices) {
            const Vec3d p3 = v.cast<double>();
            const double lat = (p3.head<2>() - smp.pos.head<2>()).norm();
            if (lat < best && p3.z() < smp.pos.z() - 1.0) {
                best    = lat;
                best_dz = smp.pos.z() - p3.z();
            }
        }
        if (best < 1e-6) {
            dmin = std::min(dmin, best_dz);
            dmax = std::max(dmax, best_dz);
        }
    }
    REQUIRE(dmax - dmin < 1e-6);
}

TEST_CASE("Draw cut: the cutter solid is closed for both open and closed strokes", "[DrawCut]")
{
    BoundingBoxf3 bbox;
    bbox.merge(Vec3d(-20, -20, -20));
    bbox.merge(Vec3d(20, 20, 20));

    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    // Closed: a tube-like band capped at both rails.
    DrawCutStroke ring = circle_on_top(10.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());
    const indexed_triangle_set tube = draw_cut_cutter_solid(ring, params, bbox);
    REQUIRE(!tube.empty());
    REQUIRE(watertight(tube));
    // A cutter must be OUTWARD wound, or cut_with_solid()'s INTERSECTION would
    // come back as the complement.
    REQUIRE(its_volume(tube) > 0.f);

    // Open: the strip plus two end quads and two caps.
    DrawCutStroke line = line_on_top(0.5 * CUBE);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());
    const indexed_triangle_set slab = draw_cut_cutter_solid(line, params, bbox);
    REQUIRE(!slab.empty());
    REQUIRE(watertight(slab));
    REQUIRE(its_volume(slab) > 0.f);

    // An invalid stroke gives nothing rather than something broken.
    DrawCutStroke bad;
    bad.append(Vec3d::Zero(), Vec3d::UnitZ());
    bad.finish();
    REQUIRE(draw_cut_cutter_solid(bad, params, bbox).empty());
}

// ---------------------------------------------------------------------------
// (8) The Extension really extends, and Depth really limits.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: Depth stops the cut short of through-all", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke stroke = circle_on_top(10.0, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 4.0;
    params.through_all = false;
    params.depth       = 10.0; // a 10 mm deep pocket in a 40 mm cube

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, nullptr));

    // The plug is a 10 mm tall cylinder, not a 40 mm one.
    const double plug = double(its_volume(upper));
    REQUIRE(plug == Approx(M_PI * 10.0 * 10.0 * 10.0).epsilon(0.05));

    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    REQUIRE(bb.max.z() == Approx(+0.5 * CUBE).margin(1e-3));
    REQUIRE(bb.min.z() == Approx(+0.5 * CUBE - 10.0).margin(0.2));

    // Volume is still conserved.
    REQUIRE(plug + double(its_volume(lower)) == Approx(double(its_volume(cube))).epsilon(1e-3));
}

// ===========================================================================
// PHASE 2
// ===========================================================================

// ---------------------------------------------------------------------------
// (10) THE DRAFT ANGLE.
//
// The closed form these check against, worked out once here so the cases below
// can quote it.
//
// A circle of radius R is drawn on the cube's top face, so at every sample the
// surface normal is +Z and the outward binormal is the radial direction. The
// ruling is then
//
//   d = cos(theta) * (-Z) + sin(theta) * r_hat
//
// so travelling a distance t along it from a point at (R, z = +20) reaches
// radius R + t sin(theta) at height 20 - t cos(theta). To drop a height h the
// ruling has to run t = h / cos(theta), and the radius there is
//
//   r(h) = R + h * tan(theta)
//
// The plug is therefore a CONE FRUSTUM of height h with radii R and R + h tan
// theta, whose volume is the standard
//
//   V = pi * h / 3 * (r1^2 + r1 r2 + r2^2)
//
// NOTE THE UNITS TRAP, which is the whole reason `depth` is set the way it is
// below: DrawCutParams::depth is measured ALONG THE RULING, not in z. Asking for
// depth = h would cut only h * cos(theta) deep. So the cases ask for
// h / cos(theta) and check the height that comes back.
//
// h = 8 mm on a 40 mm cube keeps both frusta comfortably inside the part (the
// +30 case's widest radius is 16.6 mm against the cube's 20 mm half-width), which
// is what makes the closed form the right answer rather than a clipped version of
// it.
// ---------------------------------------------------------------------------

// The closed-form frustum volume for a draft of `angle_deg` cut `h` deep in z
// from a circle of radius R.
static double frustum_volume(double R, double h, double angle_deg)
{
    const double r2 = R + h * std::tan(angle_deg * M_PI / 180.0);
    return M_PI * h / 3.0 * (R * R + R * r2 + r2 * r2);
}

// The ruling depth that cuts `h` deep in z at `angle_deg`.
static double ruling_depth_for(double h, double angle_deg)
{
    return h / std::cos(angle_deg * M_PI / 180.0);
}

TEST_CASE("Draw cut: a positive angle flares the plug into a frustum", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    const double R = 12.0, H = 8.0, ANGLE = 30.0;

    DrawCutStroke stroke = circle_on_top(R, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(stroke.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = ruling_depth_for(H, ANGLE);
    params.angle_deg   = ANGLE;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // THE HEADLINE: the plug's volume matches the closed form within 5%, which is
    // the tolerance the brief asks for.
    const double plug = double(its_volume(upper));
    REQUIRE(plug == Approx(frustum_volume(R, H, ANGLE)).epsilon(0.05));

    // And it is a FRUSTUM, not a cylinder: a positive angle makes it visibly bigger
    // than the straight cut would have been (1.43x here - well outside the 5%).
    const double cylinder = M_PI * R * R * H;
    REQUIRE(plug > 1.2 * cylinder);

    // Volume is still conserved - a draft angle changes the SHAPE of the cut, not
    // how much material there is.
    REQUIRE(plug + double(its_volume(lower)) == Approx(cube_volume).epsilon(1e-3));

    // The geometry the volume is standing in for: the plug is WIDER AT THE BOTTOM.
    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    REQUIRE(bb.max.z() == Approx(0.5 * CUBE).margin(0.2));
    REQUIRE(bb.min.z() == Approx(0.5 * CUBE - H).margin(0.4));

    double r_top = 0.0, r_bottom = 0.0;
    for (const Vec3f& v : upper.vertices) {
        const Vec3d p = v.cast<double>();
        const double r = p.head<2>().norm();
        if (p.z() > bb.max.z() - 1.0)
            r_top = std::max(r_top, r);
        if (p.z() < bb.min.z() + 1.0)
            r_bottom = std::max(r_bottom, r);
    }
    REQUIRE(r_top == Approx(R).margin(0.3));
    REQUIRE(r_bottom == Approx(R + H * std::tan(ANGLE * M_PI / 180.0)).margin(0.5));
    REQUIRE(r_bottom > r_top);
}

TEST_CASE("Draw cut: a negative angle undercuts the plug", "[DrawCut]")
{
    const indexed_triangle_set cube = centred_cube();
    const double cube_volume = double(its_volume(cube));

    const double R = 12.0, H = 8.0, ANGLE = -30.0;

    DrawCutStroke stroke = circle_on_top(R, 96);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = ruling_depth_for(H, ANGLE);
    params.angle_deg   = ANGLE;

    indexed_triangle_set upper, lower;
    DrawCutError err = DrawCutError::None;
    REQUIRE(draw_cut_split(cube, stroke, params, &upper, &lower, &err));
    REQUIRE(err == DrawCutError::None);
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    const double plug = double(its_volume(upper));
    REQUIRE(plug == Approx(frustum_volume(R, H, ANGLE)).epsilon(0.05));

    // AN UNDERCUT PLUG: narrower at the bottom than at the top, so it cannot be
    // lifted straight out - which is the whole point of a negative angle. It is
    // therefore SMALLER than the straight cut, the mirror of the +30 case.
    const double cylinder = M_PI * R * R * H;
    REQUIRE(plug < 0.8 * cylinder);

    REQUIRE(plug + double(its_volume(lower)) == Approx(cube_volume).epsilon(1e-3));

    BoundingBoxf3 bb;
    for (const Vec3f& v : upper.vertices)
        bb.merge(v.cast<double>());
    double r_top = 0.0, r_bottom = 0.0;
    for (const Vec3f& v : upper.vertices) {
        const Vec3d p = v.cast<double>();
        const double r = p.head<2>().norm();
        if (p.z() > bb.max.z() - 1.0)
            r_top = std::max(r_top, r);
        if (p.z() < bb.min.z() + 1.0)
            r_bottom = std::max(r_bottom, r);
    }
    REQUIRE(r_bottom < r_top);
    REQUIRE(r_bottom == Approx(R + H * std::tan(ANGLE * M_PI / 180.0)).margin(0.5));
}

TEST_CASE("Draw cut: angle 0 is bit-for-bit the phase 1 cut", "[DrawCut]")
{
    // The regression net for the whole of (10): adding the angle must not have
    // moved the zero case even slightly, because every phase 1 test and every
    // existing user's cut IS the zero case.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.2) == DrawCutError::None);

    DrawCutParams p0;
    p0.direction = DrawCutDirection::SurfaceNormal;
    p0.angle_deg = 0.0;

    for (size_t i = 0; i < stroke.path().size(); ++ i) {
        const Vec3d d = draw_cut_inward_dir(stroke, p0, i);
        // Exactly the inward normal, to the last bit: at theta == 0
        // draw_cut_inward_dir() short-circuits before touching the binormal at all.
        REQUIRE((d + stroke.path()[i].normal).norm() < 1e-15);
    }
}

TEST_CASE("Draw cut: the angle only applies to Surface normal", "[DrawCut]")
{
    // A constant direction is ONE direction at every sample - that is what "as-is
    // extrusion" means - so a per-sample tilt is exactly what it is not. The panel
    // greys the slider out; this is the geometry behind that.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    for (DrawCutDirection dir : { DrawCutDirection::AxisX, DrawCutDirection::AxisY,
                                  DrawCutDirection::AxisZ, DrawCutDirection::View }) {
        DrawCutParams p;
        p.direction = dir;
        p.view_dir  = Vec3d(0.3, -0.2, -1.0);
        p.angle_deg = 45.0;

        const Vec3d first = draw_cut_inward_dir(stroke, p, 0);
        for (size_t i = 1; i < stroke.path().size(); ++ i)
            REQUIRE((draw_cut_inward_dir(stroke, p, i) - first).norm() < 1e-12);
    }
}

TEST_CASE("Draw cut: the angle rotates the ruling toward the outward binormal", "[DrawCut]")
{
    // The structural check behind the two volume cases: at every sample the ruling
    // is the inward normal rotated by exactly theta, in the plane it spans with the
    // binormal, and it is still a unit vector.
    DrawCutStroke stroke = circle_on_top(10.0, 64);
    REQUIRE(stroke.finish(1.0, 0.0) == DrawCutError::None);

    for (double theta : { -60.0, -30.0, -5.0, 5.0, 30.0, 60.0 }) {
        DrawCutParams p;
        p.direction = DrawCutDirection::SurfaceNormal;
        p.angle_deg = theta;

        const double rad = theta * M_PI / 180.0;
        for (size_t i = 0; i < stroke.path().size(); ++ i) {
            const Vec3d d = draw_cut_inward_dir(stroke, p, i);
            const Vec3d n = stroke.path()[i].normal;
            const Vec3d b = stroke.binormal(i);

            REQUIRE(d.norm() == Approx(1.0).margin(1e-9));
            // The two components, read straight back off the rotation.
            REQUIRE(d.dot(-n) == Approx(std::cos(rad)).margin(1e-9));
            REQUIRE(d.dot(b)  == Approx(std::sin(rad)).margin(1e-9));
        }
    }
}

TEST_CASE("Draw cut: a concave stroke with a large angle is detected as a fold", "[DrawCut]")
{
    // THE FOLD CASE THE BRIEF ASKS FOR, and the thing phase 1's guard could not
    // see. A stroke with tight CONCAVE corners, with an extension small enough that
    // phase 1's test passes it - and then an angle large enough that the INWARD
    // rail, leaning sideways by depth * sin(theta), folds.
    DrawCutStroke flower;
    const int n = 200;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 10.0 + 3.0 * std::cos(6.0 * a);
        flower.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ(), size_t(i));
    }
    flower.append(Vec3d(13.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ(), 0);
    REQUIRE(flower.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(flower.is_closed());

    double kappa = 0.0;
    // A modest extension and NO angle: whatever phase 1 says here is the baseline.
    const bool folds_flat = draw_cut_strip_folds(flower, 1.0, &kappa, 0.0, 0.0);

    // The same stroke, the same extension, but drafted 45 degrees through 30 mm. The
    // lateral reach is then 30 * sin(45) = 21 mm, against lobes whose turning radius
    // is a couple of millimetres - so it folds, and unmistakably.
    double kappa_angled = 0.0;
    const bool folds_angled = draw_cut_strip_folds(flower, 1.0, &kappa_angled, 45.0, 30.0);
    REQUIRE(folds_angled);
    // The curvature reported is the real thing: a lobe of this flower turns tighter
    // than a 5 mm radius somewhere.
    REQUIRE(kappa_angled > 1.0 / 5.0);

    // And it is the ANGLE that did it, not the depth on its own - the negative
    // control. (If the flat case already folded, the angled one folding would say
    // nothing, so this is conditional on the baseline being clean.)
    if (!folds_flat)
        REQUIRE(draw_cut_strip_folds(flower, 1.0, nullptr, 0.0, 30.0) == false);
}

TEST_CASE("Draw cut: a plain circle never folds until the reach passes its radius", "[DrawCut]")
{
    // The negative control the phase 1 suite has for the extension, extended to the
    // angle. Every point of a circle drawn as a loop is a CONVEX corner as seen from
    // outside, so the outward rail can never fold - and the inward rail, which the
    // angle does move, would have to travel the full radius to reach the centre.
    DrawCutStroke circle = circle_on_top(15.0, 128);
    REQUIRE(circle.finish(1.0, 0.0) == DrawCutError::None);

    // 10 mm of depth at 30 degrees is 5 mm of lateral reach against a 15 mm radius.
    for (double theta : { -30.0, 0.0, 30.0 })
        REQUIRE(draw_cut_strip_folds(circle, 2.0, nullptr, theta, 10.0) == false);

    // Push the reach past the radius and it DOES fold, which is the guard working
    // rather than the test being vacuous: 60 mm at 30 degrees is 30 mm of reach.
    REQUIRE(draw_cut_strip_folds(circle, 2.0, nullptr, 30.0, 60.0));
}

// ---------------------------------------------------------------------------
// (11) LINE EDITING.
//
// The gizmo owns the gestures (hover, drag, right-click, Shift+click) and they
// need a GUI to exercise. What is testable headless is the CONTRACT the gestures
// rely on, and it is the part that would silently break: an edit rewrites the
// point list and hands it back to finish(), so the line has to come out resampled
// and - if it was a loop - still closed.
// ---------------------------------------------------------------------------

// What the gizmo's commit_draw_points() does, in the two lines of it that are not
// wx: rebuild the stroke from the edited points, re-expressing the closing span
// for a loop, and re-finish.
static DrawCutStroke commit_points(const std::vector<DrawCutSample>& pts, bool was_closed,
                                   double spacing = 1.0, double smoothing = 0.0)
{
    DrawCutStroke edited;
    for (const DrawCutSample& s : pts)
        edited.append(s.pos, s.normal, s.facet);
    if (was_closed)
        edited.append(pts.front().pos, pts.front().normal, pts.front().facet);
    edited.finish(spacing, smoothing);
    return edited;
}

// Is the path resampled at `spacing`?
//
// THE MEASURE HAS TO BE THE CHORD, AND THE CHORD IS NOT THE ARC. The resampler
// walks the INPUT polyline's arc length and emits a point every `spacing` along
// it, so two consecutive output points are `spacing` apart ALONG THAT POLYLINE -
// and the straight-line distance between them equals that only where the line is
// locally straight. Across a corner it is shorter, and across a sharp one much
// shorter: an edit that pulls one point 4 mm out of a 12 mm ring leaves an apex
// whose two neighbouring chords measure about 0.71 and 0.85 mm. That is the
// resampler working, not failing, and an earlier version of this helper that
// demanded equal chords was measuring the wrong thing.
//
// So the assertion is the one that actually distinguishes a resampled line from an
// un-resampled one: no span is LONGER than the spacing, and none is degenerate. A
// line that had not been re-finished after an edit would carry one span of several
// millimetres where the point was dragged - which is exactly what this catches -
// while the short chords at a corner are legitimate and are allowed.
static bool evenly_spaced(const DrawCutStroke& s, double spacing, double tol = 0.02)
{
    const std::vector<DrawCutSample>& p = s.path();
    if (p.size() < 3)
        return false;
    // The interior spans only: the last span of an open stroke is whatever is left
    // over, and a closed one's wrap span likewise.
    for (size_t i = 1; i + 2 < p.size(); ++ i) {
        const double d = (p[i + 1].pos - p[i].pos).norm();
        if (d > spacing + tol)   // a span the resampler would have split
            return false;
        if (d < 0.25 * spacing)  // a degenerate span the tangent maths would trip on
            return false;
    }
    return true;
}

TEST_CASE("Draw cut: moving a point keeps the line resampled and closed", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    std::vector<DrawCutSample> pts = ring.path();
    const size_t n_before = pts.size();
    REQUIRE(n_before > 10);

    // Drag one point 4 mm outward, the way a user pulling a handle would. It stays
    // on the top face (z is untouched), which is what the gizmo's raycast gives.
    const size_t moved = n_before / 3;
    const Vec3d radial = Vec3d(pts[moved].pos.x(), pts[moved].pos.y(), 0.0).normalized();
    pts[moved].pos += 4.0 * radial;

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ true);

    // STILL A USABLE LOOP. Both halves of that matter: an edit that opened the ring
    // would turn a plug cut into a splitting cut without telling anyone.
    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.valid());
    REQUIRE(edited.is_closed());

    // STILL RESAMPLED. This is the reason an edit goes back through finish() at all:
    // moving a point leaves one long span and one short one either side of it, and
    // the ruling density follows the spans.
    REQUIRE(evenly_spaced(edited, 1.0));

    // And the edit actually took: the line is longer than it was, and reaches
    // further out than the original ring.
    REQUIRE(edited.length() > ring.length());
    double max_r = 0.0;
    for (const DrawCutSample& s : edited.path())
        max_r = std::max(max_r, s.pos.head<2>().norm());
    REQUIRE(max_r > 13.0);
}

TEST_CASE("Draw cut: inserting a point keeps the line resampled and closed", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    std::vector<DrawCutSample> pts = ring.path();
    const size_t n_before = pts.size();

    // Shift+click on a segment: a new point on the chord between two neighbours,
    // pulled 3 mm outward (the gizmo re-projects it onto the model, which on a flat
    // face leaves it where the click was).
    const size_t seg = n_before / 2;
    DrawCutSample mid;
    mid.pos    = 0.5 * (pts[seg].pos + pts[seg + 1].pos);
    mid.normal = Vec3d::UnitZ();
    mid.facet  = pts[seg].facet;
    const Vec3d radial = Vec3d(mid.pos.x(), mid.pos.y(), 0.0).normalized();
    mid.pos += 3.0 * radial;
    pts.insert(pts.begin() + int(seg) + 1, mid);

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ true);

    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));
    // The resample is what makes the point COUNT rather than just be there: the new
    // bump lengthens the line, so the resampled path has at least as many points as
    // before even though only one was inserted.
    REQUIRE(edited.path().size() >= n_before);
    REQUIRE(edited.length() > ring.length());
}

TEST_CASE("Draw cut: deleting a point keeps the line resampled and closed", "[DrawCut]")
{
    // A ring with a deliberate spike in it, so deleting the spike's point is a
    // visible edit rather than a no-op the resampler would put straight back.
    DrawCutStroke ring = circle_on_top(12.0, 64);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    std::vector<DrawCutSample> pts = ring.path();
    const size_t spike = pts.size() / 4;
    const Vec3d radial = Vec3d(pts[spike].pos.x(), pts[spike].pos.y(), 0.0).normalized();
    pts[spike].pos += 6.0 * radial;

    const DrawCutStroke with_spike = commit_points(pts, true);
    REQUIRE(with_spike.valid());
    const double spiked_len = with_spike.length();

    // Now delete it - right-click on the handle.
    pts.erase(pts.begin() + int(spike));
    const DrawCutStroke edited = commit_points(pts, true);

    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));
    // Taking the spike out shortens the line back toward the plain ring.
    REQUIRE(edited.length() < spiked_len);
    double max_r = 0.0;
    for (const DrawCutSample& s : edited.path())
        max_r = std::max(max_r, s.pos.head<2>().norm());
    REQUIRE(max_r < 13.5);
}

TEST_CASE("Draw cut: editing an open line leaves it open", "[DrawCut]")
{
    // The mirror of the three closed cases: an open line edited is still an open
    // line, and its ENDS DO NOT CREEP - which is what stops a series of edits in the
    // middle of a line from quietly shortening or lengthening the cut's reach.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());

    std::vector<DrawCutSample> pts = line.path();
    const Vec3d first_before = pts.front().pos;
    const Vec3d last_before  = pts.back().pos;

    pts[pts.size() / 2].pos += Vec3d(0.0, 5.0, 0.0);

    const DrawCutStroke edited = commit_points(pts, /*was_closed*/ false);
    REQUIRE(edited.error() == DrawCutError::None);
    REQUIRE(!edited.is_closed());
    REQUIRE(evenly_spaced(edited, 1.0));

    // The FIRST point is exact: the resampler always emits the input's first sample.
    REQUIRE((edited.path().front().pos - first_before).norm() < 1e-6);

    // THE LAST POINT IS WITHIN HALF A SPACING, NOT EXACT, and the difference is a
    // deliberate rule rather than slack. draw_cut_resample() appends the final input
    // sample only when it is more than half a spacing from the last one already
    // emitted - otherwise it would leave a span the central-difference tangent reads
    // as a near-zero direction. Lengthening the middle of the line changes where the
    // last multiple of the spacing falls, so the end can be dropped and the path then
    // stops up to half a spacing short.
    //
    // What matters is that the end does not CREEP - the line still reaches the place
    // the user drew to, within that half spacing, and in particular has not grown
    // past it.
    const double end_gap = (edited.path().back().pos - last_before).norm();
    REQUIRE(end_gap < 0.5 * 1.0);
    // And it stops SHORT of the original end rather than overshooting it: the
    // original end is still the furthest point the line was ever asked to reach.
    REQUIRE(edited.path().back().pos.x() <= last_before.x() + 1e-6);
}

TEST_CASE("Draw cut: set_path replaces the path without resampling it", "[DrawCut]")
{
    // The re-projection hook the gizmo uses to put a smoothed line back on the mesh
    // (phase 1's deviation #7). Its contract: same length in, same length out,
    // binormals recomputed, open/closed untouched - and NO resample, because the
    // path already is one and running the resampler over its own output would walk
    // the samples a little further every time a slider moved.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.3) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    std::vector<DrawCutSample> path = ring.path();
    const size_t n = path.size();
    const Vec3d b_before = ring.binormal(0);

    // Push every sample 2 mm outward, which is what a re-projection onto a slightly
    // larger surface would do - and which must move the binormals' basepoints but
    // not their outwardness.
    for (DrawCutSample& s : path) {
        const Vec3d radial = Vec3d(s.pos.x(), s.pos.y(), 0.0).normalized();
        s.pos += 2.0 * radial;
    }
    ring.set_path(path);

    REQUIRE(ring.path().size() == n);
    REQUIRE(ring.is_closed());
    for (size_t i = 0; i < n; ++ i) {
        REQUIRE((ring.path()[i].pos - path[i].pos).norm() < 1e-12);
        // Still outward: the binormal points away from the centroid.
        const Vec3d radial = Vec3d(ring.path()[i].pos.x(), ring.path()[i].pos.y(), 0.0).normalized();
        REQUIRE(ring.binormal(i).dot(radial) > 0.5);
    }
    REQUIRE(ring.binormal(0).dot(b_before) > 0.9);

    // A path of the wrong length is refused rather than half-applied, which is what
    // keeps the closed decision finish() made meaningful.
    std::vector<DrawCutSample> shorter(path.begin(), path.end() - 3);
    ring.set_path(shorter);
    REQUIRE(ring.path().size() == n);
}

// ---------------------------------------------------------------------------
// (12) THE DRAWN SURFACE AS A SURFACE, and CONNECTORS ON IT.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: the surface point and frame agree with the cutter", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = false;
    params.depth       = 10.0;

    // w == 0 is ON the stroke, so the surface point at a sample's own arc length is
    // that sample. This is the anchor everything else is measured from.
    const std::vector<DrawCutSample>& p = ring.path();
    double s_walk = 0.0;
    for (size_t i = 0; i < p.size(); ++ i) {
        const Vec3d q = draw_cut_surface_point(ring, params, s_walk, 0.0);
        REQUIRE((q - p[i].pos).norm() < 1e-6);
        s_walk += (p[(i + 1) % p.size()].pos - p[i].pos).norm();
    }

    // w > 0 goes INTO the part: on a cube's top face with no draft, straight down.
    const Vec3d in = draw_cut_surface_point(ring, params, 0.0, 5.0);
    REQUIRE(in.z() == Approx(0.5 * CUBE - 5.0).margin(1e-6));
    // w < 0 comes back OUT of it.
    const Vec3d out = draw_cut_surface_point(ring, params, 0.0, -2.0);
    REQUIRE(out.z() == Approx(0.5 * CUBE + 2.0).margin(1e-6));

    // THE FRAME's Z is the strip's own normal, which on a circular loop cut straight
    // down is the RADIAL direction - the direction the plug and the hole come apart
    // in. That is what makes a connector built on this frame coaxial in the two
    // halves.
    for (double s : { 0.0, 5.0, 20.0, 50.0 }) {
        const Transform3d f = draw_cut_surface_frame(ring, params, s, 3.0);
        const Vec3d z = f.linear().col(2);
        const Vec3d q = draw_cut_surface_point(ring, params, s, 3.0);
        const Vec3d radial = Vec3d(q.x(), q.y(), 0.0).normalized();
        REQUIRE(std::abs(z.dot(radial)) == Approx(1.0).margin(0.05));
        // Orthonormal and right-handed - a connector's own Rotation is applied in
        // this frame, so a skewed one would skew every connector.
        REQUIRE(f.linear().col(0).norm() == Approx(1.0).margin(1e-9));
        REQUIRE(f.linear().col(1).norm() == Approx(1.0).margin(1e-9));
        REQUIRE(f.linear().col(0).dot(f.linear().col(1)) == Approx(0.0).margin(1e-9));
        REQUIRE(f.linear().determinant() == Approx(1.0).margin(1e-9));
    }

    // THE FRAME IS CONTINUOUS round the loop, which a raw t x d would not be: two
    // neighbouring points must not have opposite normals, or two connectors a
    // millimetre apart would point opposite ways.
    Vec3d prev = draw_cut_surface_normal(ring, params, 0.0, 0.0);
    for (double s = 1.0; s < ring.length(); s += 1.0) {
        const Vec3d nrm = draw_cut_surface_normal(ring, params, s, 0.0);
        REQUIRE(nrm.dot(prev) > 0.9);
        prev = nrm;
    }
}

TEST_CASE("Draw cut: the surface projection inverts the surface point", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.through_all = false;
    params.depth       = 12.0;
    params.extension   = 3.0;

    // Round-trip: pick an (s, w), evaluate the surface there, project the result
    // back, and land on the same place. This is what turns a connector's stored
    // position into the parameters its frame is built from, so an error here is a
    // connector whose frame belongs to a different part of the surface.
    for (double s : { 2.0, 11.0, 37.0, 61.0 })
        for (double w : { 0.0, 3.0, 8.0 }) {
            const Vec3d q = draw_cut_surface_point(ring, params, s, w);
            double s2 = 0.0, w2 = 0.0, dist = 0.0;
            REQUIRE(draw_cut_surface_project(ring, params, q, s2, w2, &dist));
            REQUIRE(dist < 0.2);          // it IS on the surface
            REQUIRE(w2 == Approx(w).margin(0.2));
            // The POINT comes back, which is the property that matters - the
            // parameter s can sit a sample either side on a polygonised circle.
            REQUIRE((draw_cut_surface_point(ring, params, s2, w2) - q).norm() < 0.3);
        }
}

TEST_CASE("Draw cut: the surface domain test keeps a connector clear of the rims", "[DrawCut]")
{
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.extension   = 4.0;
    params.through_all = false;
    params.depth       = 10.0;
    const double reach  = 10.0;
    const double radius = 2.0; // the connector's own half-extent

    // Comfortably inside: yes.
    REQUIRE(draw_cut_surface_contains(ring, params, 20.0, 5.0, radius, reach));
    // Past the inward rim: no - its body would hang off the bottom of the cut.
    REQUIRE(!draw_cut_surface_contains(ring, params, 20.0, 9.5, radius, reach));
    // Past the outward rim: no.
    REQUIRE(!draw_cut_surface_contains(ring, params, 20.0, -3.5, radius, reach));
    // A closed stroke WRAPS, so no arc length is out of range.
    REQUIRE(draw_cut_surface_contains(ring, params, 1000.0, 5.0, radius, reach));

    // An OPEN line has ends to fall off, and they are what the test is for.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!line.is_closed());
    REQUIRE(draw_cut_surface_contains(line, params, line.length() * 0.5, 5.0, radius, reach));
    REQUIRE(!draw_cut_surface_contains(line, params, 0.5, 5.0, radius, reach));
    REQUIRE(!draw_cut_surface_contains(line, params, line.length() - 0.5, 5.0, radius, reach));
}

TEST_CASE("Draw cut: a ruled strip is flat along its rules and curves across them", "[DrawCut]")
{
    // The reason Hinge and Thread get the SAME warning here as on a curved sheet
    // rather than a blanket refusal: a ruled surface is DEVELOPABLE along its rules,
    // so only one of its two directions can curve at all. A connector on a straight
    // stretch of stroke sits on a genuinely flat patch.
    DrawCutParams params;
    params.through_all = false;
    params.depth       = 10.0;
    params.extension   = 2.0;

    // A straight line swept straight down is a PLANE: flat in both directions.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    for (double w : { 0.0, 5.0 })
        REQUIRE(draw_cut_surface_curvature_radius(line, params, line.length() * 0.5, w) > 1000.0);
    REQUIRE(draw_cut_patch_is_flat_enough(line, params, line.length() * 0.5, 3.0, 5.0));

    // A tight ring curves across the rules by its own radius, so a connector wider
    // than a third of it is warned about - which is the curved cut's own rule
    // (CurvedConnectorFlatPatchFactor == 3) applied to the drawn surface.
    DrawCutStroke tight = circle_on_top(6.0, 64);
    REQUIRE(tight.finish(1.0, 0.0) == DrawCutError::None);
    const double r = draw_cut_surface_curvature_radius(tight, params, 5.0, 0.0);
    REQUIRE(r == Approx(6.0).margin(1.0));
    REQUIRE(draw_cut_patch_is_flat_enough(tight, params, 5.0, 0.0, 1.0));   // small connector: fine
    REQUIRE(!draw_cut_patch_is_flat_enough(tight, params, 5.0, 0.0, 4.0));  // big one: warned
}

TEST_CASE("Draw cut: the surface tilt reads the strip's own normal", "[DrawCut]")
{
    // A loop on a flat top face cut straight down has a VERTICAL wall, so its normal
    // is horizontal - 90 degrees from the plane's +Z, and well past the
    // CurvedConnectorTiltWarnDeg threshold. That is correct, and it is what the panel
    // says about it: a connector in the wall of a plug does print sideways.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams params;
    params.through_all = false;
    params.depth       = 10.0;

    REQUIRE(draw_cut_surface_tilt_deg(ring, params, 10.0, 5.0) == Approx(90.0).margin(2.0));
    REQUIRE(draw_cut_surface_tilt_deg(ring, params, 10.0, 5.0) > CurvedConnectorTiltWarnDeg);
}

TEST_CASE("Draw cut: a connector on the swept surface makes a matching hole and plug", "[DrawCut]")
{
    // THE HEADLINE CONNECTOR CASE the brief asks for: a connector placed on the
    // drawn surface has to survive the split, with the hole in one half and the plug
    // in the other, and the two have to MATCH.
    //
    // Headless there is no gizmo, so the connector body is built the way the gizmo
    // builds it - a cylinder on the frame draw_cut_surface_frame() gives at the
    // connector's (s, w) - and the two halves are made by the same boolean the real
    // path uses. What is being checked is the property the real path depends on:
    // that the SAME surface frame, applied to both halves, subtracts and adds the
    // same solid.
    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(ring.is_closed());

    DrawCutParams params;
    params.direction   = DrawCutDirection::SurfaceNormal;
    params.extension   = 3.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, ring, params, &upper, &lower, nullptr));
    REQUIRE(watertight(upper));
    REQUIRE(watertight(lower));

    // The connector: a 3 mm radius, 4 mm long dowel standing on the surface a third
    // of the way round and 8 mm down, on the frame the surface gives there.
    const double s = ring.length() * 0.25;
    const double w = 8.0;
    const double CR = 3.0, CH = 4.0;

    const Vec3d       at    = draw_cut_surface_point(ring, params, s, w);
    const Transform3d frame = draw_cut_surface_frame(ring, params, s, w);

    // A cylinder about the frame's +Z, CENTRED on the surface so half of it is in
    // each side - which is what a plug/hole pair is.
    indexed_triangle_set conn = its_make_cylinder(CR, CH, 2.0 * M_PI / 64.0);
    // its_make_cylinder stands on z == 0 and runs up; centre it.
    for (Vec3f& v : conn.vertices)
        v.z() -= float(0.5 * CH);
    its_transform(conn, Geometry::translation_transform(at) * frame);

    const double conn_volume = double(its_volume(conn));
    REQUIRE(conn_volume == Approx(M_PI * CR * CR * CH).epsilon(0.05));

    // The part of the connector inside each half. cut_with_solid() returns the
    // INTERSECTION as `lower`, so that is the side read here.
    //
    // The property being asserted is that the two are COMPLEMENTARY: the part inside
    // the plug plus the part inside the rest is the whole connector, with nothing
    // double-counted and nothing lost. That only holds if the two halves meet exactly
    // on the surface the connector is standing on - so this is the real test of the
    // frame, and it is what makes the hole and the plug the cut produces match.
    indexed_triangle_set a, b;
    REQUIRE(cut_with_solid(conn, upper, upper, /*kerf*/ false, &a, &b, "draw connector"));
    const double part_upper = double(its_volume(b));
    REQUIRE(cut_with_solid(conn, lower, lower, /*kerf*/ false, &a, &b, "draw connector"));
    const double part_lower = double(its_volume(b));

    REQUIRE(part_upper > 0.1 * conn_volume);
    REQUIRE(part_lower > 0.1 * conn_volume);
    // The two parts add up to the whole connector: the surface really is the
    // boundary between the halves, right where the connector stands.
    REQUIRE(part_upper + part_lower == Approx(conn_volume).epsilon(0.03));

    // And it is a genuinely BALANCED pair: the connector straddles the surface, so
    // neither half gets almost all of it. (A frame built from the wrong normal would
    // lie ALONG the surface instead of across it, and one side would get nearly
    // everything - which is the failure this catches.)
    REQUIRE(part_upper == Approx(part_lower).epsilon(0.25));
}

TEST_CASE("Draw cut: a connector's frame survives the kerf", "[DrawCut]")
{
    // The kerf moves BOTH faces along the same strip normal - the field
    // draw_cut_cutter_solid() derives once for the whole strip - so a connector
    // standing perpendicular to the surface stays coaxial with its own hole: the gap
    // opens along the connector's axis, which is the direction it comes apart in
    // anyway. Concretely that means the frame at an (s, w) does not depend on the
    // thickness, which is what this checks.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);

    DrawCutParams no_kerf;
    no_kerf.through_all = false;
    no_kerf.depth       = 10.0;

    DrawCutParams kerfed = no_kerf;
    kerfed.thickness = 1.0;

    for (double s : { 3.0, 19.0, 44.0 })
        for (double w : { 0.0, 4.0 }) {
            const Transform3d fa = draw_cut_surface_frame(ring, no_kerf, s, w);
            const Transform3d fb = draw_cut_surface_frame(ring, kerfed,  s, w);
            REQUIRE((fa.linear() - fb.linear()).norm() < 1e-12);
            REQUIRE((draw_cut_surface_point(ring, no_kerf, s, w) -
                     draw_cut_surface_point(ring, kerfed,  s, w)).norm() < 1e-12);
        }
}

TEST_CASE("Draw cut: the holonomy check passes a plain loop", "[DrawCut]")
{
    // A circle on a flat face has a perfectly consistent outward side, so the angle
    // is usable - the check must not fire on the common case.
    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(ring));

    // Nor on a wobbly one: the field is oriented from the centroid, so it survives
    // anything star-shaped about it.
    DrawCutStroke wobble;
    const int n = 120;
    for (int i = 0; i < n; ++ i) {
        const double a = 2.0 * M_PI * double(i) / double(n);
        const double r = 12.0 + 2.0 * std::sin(3.0 * a);
        wobble.append(Vec3d(r * std::cos(a), r * std::sin(a), 0.5 * CUBE), Vec3d::UnitZ(), size_t(i));
    }
    wobble.append(Vec3d(12.0, 0.0, 0.5 * CUBE), Vec3d::UnitZ(), 0);
    REQUIRE(wobble.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(wobble));

    // An OPEN stroke has no loop to come back round, so it never flips.
    DrawCutStroke line = line_on_top(18.0, 40);
    REQUIRE(line.finish(1.0, 0.0) == DrawCutError::None);
    REQUIRE(!draw_cut_frame_holonomy_flips(line));
}

// ---------------------------------------------------------------------------
// (9) Demo exports, matching the curved suite's habit. Behind an env var so a
// normal run writes nothing.
// ---------------------------------------------------------------------------

TEST_CASE("Draw cut: demo exports", "[DrawCut][.demo]")
{
    if (std::getenv("SLIC3R_DRAW_CUT_DEMO") == nullptr)
        return;

    const indexed_triangle_set cube = centred_cube();

    DrawCutStroke ring = circle_on_top(12.0, 96);
    REQUIRE(ring.finish(1.0, 0.2) == DrawCutError::None);
    DrawCutParams params;
    params.extension   = 5.0;
    params.through_all = true;

    indexed_triangle_set upper, lower;
    REQUIRE(draw_cut_split(cube, ring, params, &upper, &lower, nullptr));
    TriangleMesh(upper).WriteOBJFile("draw_plug_upper.obj");
    TriangleMesh(lower).WriteOBJFile("draw_plug_lower.obj");
}
