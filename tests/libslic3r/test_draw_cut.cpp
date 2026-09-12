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

    // And when the longest surviving run is itself too short, that is reported.
    DrawCutStroke tiny;
    tiny.append(Vec3d(0.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(0.5, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.0, 0.0, 0.0), Vec3d::UnitZ());
    tiny.append(Vec3d(900.5, 0.0, 0.0), Vec3d::UnitZ());
    REQUIRE(tiny.finish(1.0, 0.0) == DrawCutError::TooShort);
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
