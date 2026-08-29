#include <catch2/catch.hpp>
#include "libslic3r/WallSampleIndex.hpp"
#include "libslic3r/BrimFilament.hpp"
#include "libslic3r/libslic3r.h"

using namespace Slic3r;

static Points segment(double x0, double y0, double x1, double y1) {
    return { Point(scale_(x0), scale_(y0)), Point(scale_(x1), scale_(y1)) };
}

TEST_CASE("WallSampleIndex samples at requested spacing", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 0, 8, 0), 0, 1);  // 8mm line, 0.8mm spacing
    CHECK(idx.size() >= 10);                       // ~11 points incl. endpoints
    CHECK(idx.size() <= 12);
}

TEST_CASE("WallSampleIndex knn finds nearest across cells", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 0, 10, 0), 0, 1);   // extruder 0 along y=0
    idx.add_polyline(segment(0, 6, 10, 6), 1, 2);   // extruder 1 along y=6
    auto near0 = idx.knn(Point(scale_(5.0), scale_(1.0)), 3);
    REQUIRE(near0.size() == 3);
    CHECK(near0[0].first->extruder == 0);           // y=0 wall is nearest
    auto near1 = idx.knn(Point(scale_(5.0), scale_(5.0)), 3);
    CHECK(near1[0].first->extruder == 1);
}

TEST_CASE("WallSampleIndex knn deterministic on exact ties", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 2, 0, 2), 1, 7);    // single point (0,2), ext 1
    idx.add_polyline(segment(0, -2, 0, -2), 0, 9);  // single point (0,-2), ext 0
    auto n = idx.knn(Point(0, 0), 2);               // both exactly 2mm away
    REQUIRE(n.size() == 2);
    CHECK(n[0].first->extruder == 0);               // tie -> lower extruder first
}

TEST_CASE("brim_vote inverse-square favors nearest wall", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 1, 10, 1), 0, 1);
    idx.add_polyline(segment(0, 9, 10, 9), 1, 2);
    BrimVoteParams p;
    CHECK(brim_vote(idx, Point(scale_(5), scale_(2)), p) == 0);
    CHECK(brim_vote(idx, Point(scale_(5), scale_(8)), p) == 1);
}

TEST_CASE("brim_vote tie-break: larger object area wins, then lower extruder", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 2, 0, 2), 1, 7);
    idx.add_polyline(segment(0, -2, 0, -2), 2, 9);
    BrimVoteParams p;
    p.object_area = {{7, 900.0}, {9, 100.0}};
    CHECK(brim_vote(idx, Point(0, 0), p) == 1);      // object 7 bigger
    p.object_area = {{7, 100.0}, {9, 100.0}};
    CHECK(brim_vote(idx, Point(0, 0), p) == 1);      // equal -> lower extruder id
}

TEST_CASE("brim_vote empty index falls back", "[chameleon]")
{
    WallSampleIndex idx;
    BrimVoteParams p; p.fallback_extruder = 3;
    CHECK(brim_vote(idx, Point(0, 0), p) == 3);
}

TEST_CASE("split_polyline_by_vote yields two runs at the midline", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, -1, 0, -1), 0, 1);    // wall A at x=0
    idx.add_polyline(segment(40, -1, 40, -1), 1, 2);  // wall B at x=40
    BrimVoteParams p;
    Points line = segment(0, 5, 40, 5);               // brim path spanning both
    auto runs = split_polyline_by_vote(line, false, idx, p);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].extruder == 0);
    CHECK(runs[1].extruder == 1);
    // boundary within 0.5mm of x=20
    const Point& last0 = runs[0].pts.back();
    CHECK(std::abs(unscale<double>(last0.x()) - 20.0) < 0.5 + p.sample_mm);
}

TEST_CASE("guard coalesces to max_runs", "[chameleon]")
{
    WallSampleIndex idx;   // alternate walls to force many runs
    for (int i = 0; i < 8; ++i)
        idx.add_polyline(segment(i * 5 + 2.5, -1, i * 5 + 2.5, -1), i % 2 ? 1 : 0, i % 2 ? 2 : 1);
    BrimVoteParams p; p.max_runs = 4; p.min_run_mm = 0.0;
    auto runs = split_polyline_by_vote(segment(0, 5, 40, 5), false, idx, p);
    CHECK(runs.size() <= 4);
    // full coverage: concatenated pts span whole line
    CHECK(runs.front().pts.front() == Point(scale_(0), scale_(5)));
    CHECK(runs.back().pts.back()   == Point(scale_(40), scale_(5)));
}
