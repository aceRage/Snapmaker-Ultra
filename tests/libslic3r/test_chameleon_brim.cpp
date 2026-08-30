#include <catch2/catch.hpp>
#include "libslic3r/WallSampleIndex.hpp"
#include "libslic3r/BrimFilament.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntityCollection.hpp"

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
    // I1: adjacent runs must share the boundary vertex (no unextruded connecting
    // gap between the last point of one run and the first point of the next).
    REQUIRE(!runs[1].pts.empty());
    CHECK(runs[1].pts.front() == runs[0].pts.back());
}

TEST_CASE("split_polyline_by_vote runs are boundary-continuous end to end", "[chameleon]")
{
    // Four walls in a row -> forces several run boundaries so we can check every
    // consecutive pair, not just a single split.
    WallSampleIndex idx;
    idx.add_polyline(segment(2, -1, 2, -1),   0, 1);
    idx.add_polyline(segment(12, -1, 12, -1), 1, 2);
    idx.add_polyline(segment(22, -1, 22, -1), 2, 3);
    idx.add_polyline(segment(32, -1, 32, -1), 3, 4);
    BrimVoteParams p; p.min_run_mm = 0.0;
    auto runs = split_polyline_by_vote(segment(0, 5, 40, 5), false, idx, p);
    REQUIRE(runs.size() > 1);
    for (size_t k = 1; k < runs.size(); ++k) {
        REQUIRE(!runs[k].pts.empty());
        REQUIRE(!runs[k - 1].pts.empty());
        CHECK(runs[k].pts.front() == runs[k - 1].pts.back());
    }
    // Coverage still holds end to end despite the added boundary overlap points.
    CHECK(runs.front().pts.front() == Point(scale_(0), scale_(5)));
    CHECK(runs.back().pts.back()   == Point(scale_(40), scale_(5)));
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
    // I1: no gaps at any surviving run boundary, even after guard merges.
    for (size_t k = 1; k < runs.size(); ++k) {
        REQUIRE(!runs[k].pts.empty());
        REQUIRE(!runs[k - 1].pts.empty());
        CHECK(runs[k].pts.front() == runs[k - 1].pts.back());
    }
}

static ExtrusionEntityCollection one_loop_brim(double cx, double half, float w = 0.5f)
{
    Polygon sq({ Point(scale_(cx-half), scale_(-half)), Point(scale_(cx+half), scale_(-half)),
                 Point(scale_(cx+half), scale_(half)),  Point(scale_(cx-half), scale_(half)) });
    ExtrusionEntityCollection c;
    ExtrusionPath path(erBrim, 1.0, w, 0.2f);
    path.polyline = Polyline(sq.points); path.polyline.points.push_back(sq.points.front());
    auto* loop = new ExtrusionLoop();
    loop->paths.push_back(path);
    c.entities.push_back(loop);
    return c;
}

TEST_CASE("partition keeps own-extruder loop intact, splits contested loop", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(-5, 0, -5, 0), 0, 1);     // own wall left
    BrimVoteParams p; p.fallback_extruder = 0;
    ExtrusionEntityCollection kept;
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_brim_by_wall(one_loop_brim(0, 10), 0, idx, p, kept, out);
    CHECK(kept.entities.size() == 1);                  // all votes = 0 -> untouched entity
    CHECK(out.empty());

    idx.add_polyline(segment(25, 0, 25, 0), 1, 2);     // foreign wall right
    ExtrusionEntityCollection kept2;
    std::map<unsigned, ExtrusionEntityCollection> out2;
    partition_brim_by_wall(one_loop_brim(10, 10), 0, idx, p, kept2, out2);
    CHECK(!kept2.entities.empty());                     // left portion stays
    REQUIRE(out2.count(1) == 1);                        // right portion -> extruder 1
    CHECK(!out2.at(1).entities.empty());
}

TEST_CASE("select_contact_layers picks the 1-2 layers above", "[chameleon]")
{
    std::vector<double> zs = {0.2, 0.4, 0.6, 0.8, 1.0};
    auto v = select_contact_layers(zs, 0.4, 2.0);          // support top at z=0.4
    REQUIRE(!v.empty());
    CHECK(v.front() == 2);                                  // first layer above (0.4,0.6]
    CHECK(v.back() <= 4);
    auto top = select_contact_layers(zs, 1.0, 2.0);         // nothing above the top
    CHECK(top.empty());
    auto vlh = select_contact_layers({0.2, 0.5, 1.4}, 0.2, 0.35); // VLH: only (0.2,0.55]
    REQUIRE(vlh.size() == 1);
    CHECK(vlh[0] == 1);
}
