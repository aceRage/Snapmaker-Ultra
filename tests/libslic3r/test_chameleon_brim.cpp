#include <catch2/catch.hpp>
#include "libslic3r/WallSampleIndex.hpp"
#include "libslic3r/BrimFilament.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include <algorithm>

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

static ExtrusionEntityCollection make_support_fills(float y_iface)
{
    ExtrusionEntityCollection c;
    auto* base = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    base->polyline = Polyline({Point(scale_(0), scale_(-5)), Point(scale_(30), scale_(-5))});
    c.entities.push_back(base);
    for (int i = 0; i < 2; ++i) {
        auto* p = new ExtrusionPath(erSupportMaterialInterface, 1.0, 0.4f, 0.2f);
        p->polyline = Polyline({Point(scale_(0), scale_(y_iface + i)), Point(scale_(30), scale_(y_iface + i))});
        c.entities.push_back(p);
    }
    return c;
}

TEST_CASE("partition_support_interfaces splits by contact walls, base untouched", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);    // left wall above -> extruder 1
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);   // right wall above -> extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);
    REQUIRE(out.count(1) == 1);
    REQUIRE(out.count(2) == 1);
    // base path remains; matched interface originals removed
    size_t base_n = 0, iface_n = 0;
    for (auto* e : fills.entities) (e->role() == erSupportMaterial ? base_n : iface_n)++;
    CHECK(base_n == 1);
    CHECK(iface_n == 0);
}

TEST_CASE("partition_support_interfaces uniform-fallback fast path keeps entities", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 30, 6), 0, 1);    // only fallback-extruder walls
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    const size_t before = fills.entities.size();
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);
    CHECK(out.empty());
    CHECK(fills.entities.size() == before);          // untouched (off-parity)
}

// C1: chameleon_assign_support_interfaces (Print.cpp) is not idempotent when it
// re-runs partition_support_interfaces over already-partitioned support_fills - the
// scenario a shared-object copy (aliasing the same SupportLayer*) or a stale
// posSupportMaterial re-run produces without the Print.cpp-side
// chameleon_interface_visited guard. That guard is orchestration-level and not
// reachable from this engine-only fixture (no Print/PrintObject/SupportLayer here -
// see the fix-wave report for the hand-walked trace through Print.cpp). What IS
// reachable via the public engine API is the underlying mechanism the review calls
// out as deterministic: partition_support_interfaces mutates support_fills in place
// (matched originals deleted, fallback-voted runs re-inserted pre-split at vote
// boundaries with a boundary vertex borrowed from the adjacent run - see
// split_polyline_by_vote's continuity fix). Re-running the SAME pass on that
// already-split output re-samples each fragment from its own start: the one
// borrowed boundary point is a single 0mm-long "run" that absorb_short_runs
// (min_run_mm=2.0 default) always folds into its neighbour's majority vote before
// switch_boundaries is computed - so every fragment re-splits to runs.size()==1 and
// contributes 0 switches, even the ones that get sent to a foreign extruder's bucket.
static ExtrusionEntityCollection three_zone_interface_fill()
{
    ExtrusionEntityCollection c;
    auto* p = new ExtrusionPath(erSupportMaterialInterface, 1.0, 0.4f, 0.2f);
    p->polyline = Polyline({Point(scale_(0), scale_(5)), Point(scale_(45), scale_(5))});
    c.entities.push_back(p);
    return c;
}

TEST_CASE("partition_support_interfaces re-run on its own output undercounts switches (C1)", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 10, 6), 0, 1);   // zone A: fallback extruder 0
    idx.add_polyline(segment(15, 6, 30, 6), 1, 2);  // zone B: foreign extruder 1 (wide -> unambiguous interior)
    idx.add_polyline(segment(35, 6, 45, 6), 0, 3);  // zone C: fallback extruder 0
    BrimVoteParams p; p.fallback_extruder = 0;

    auto fills = three_zone_interface_fill();
    std::map<unsigned, ExtrusionEntityCollection> out1;
    const size_t switches1 = partition_support_interfaces(fills, 0, idx, p, out1);

    // Pass 1: three zones -> three runs (0,1,0) -> 2 switch boundaries, and the
    // middle (foreign) run lands in out1[1]. This is the real, correct result.
    CHECK(switches1 > 0);
    REQUIRE(out1.count(1) == 1);
    CHECK(!out1.at(1).entities.empty());

    // Mirror Print.cpp's cap-revert merge-back (Print.cpp:2516-2519): fold the
    // out-of-fallback runs back into support_fills, leaving it holding all three
    // pre-split runs as separate entities - exactly what either trigger in C1
    // (aliased shared-object copy, or a stale posSupportMaterial re-run) hands the
    // pass on its second visit to this layer.
    fills.append(std::move(out1.at(1).entities));

    std::map<unsigned, ExtrusionEntityCollection> out2;
    const size_t switches2 = partition_support_interfaces(fills, 0, idx, p, out2);

    // Pass 2 re-votes each already-split fragment independently and reports FEWER
    // switches than pass 1 found for the identical geometry - the per-layer/
    // per-object switch caps (Print.cpp's ">3" and ">20" guards) would see this
    // second run as cheaper than it really is ("the reverted layer is partitioned
    // after all, with the per-layer cap counting 0" - final-review.md C1).
    CHECK(switches2 < switches1);
    // The undercounting is not from the geometry becoming inert: the middle zone's
    // material is still (correctly) matched to the foreign extruder on pass 2 - the
    // switch that causes is simply no longer reflected in the cap accounting.
    REQUIRE(out2.count(1) == 1);
    CHECK(!out2.at(1).entities.empty());
}

// --- v2.1 Task 1: engine generalization -----------------------------------

TEST_CASE("brim_vote max_dist_mm cap returns fallback beyond the cap", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 5, 10, 5), 1, 1);   // wall at y=5
    BrimVoteParams p;
    p.fallback_extruder = 9;
    p.max_dist_mm = 1.0;
    // 0.5mm from the wall -> within the cap -> votes for the wall's extruder
    CHECK(brim_vote(idx, Point(scale_(5), scale_(5.5)), p) == 1);
    // 3mm from the wall -> beyond the cap -> fallback
    CHECK(brim_vote(idx, Point(scale_(5), scale_(8.0)), p) == 9);
}

TEST_CASE("brim_vote max_dist_mm default 0 is uncapped (Part 1 brim path unchanged)", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 5, 10, 5), 1, 1);
    BrimVoteParams p;
    CHECK(p.max_dist_mm == 0.0);
    // far away, but 0 means uncapped -> still votes the nearest wall, no fallback
    CHECK(brim_vote(idx, Point(scale_(5), scale_(50.0)), p) == 1);
}

TEST_CASE("split_polyline_by_resolver produces runs matching a synthetic resolver", "[chameleon]")
{
    BrimVoteParams p;
    // synthetic resolver: left half of the line -> extruder 5, right half -> extruder 6
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 20.0 ? 5u : 6u;
    };
    Points line = segment(0, 5, 40, 5);
    auto runs = split_polyline_by_resolver(line, false, resolver, p);
    REQUIRE(runs.size() == 2);
    CHECK(runs[0].extruder == 5);
    CHECK(runs[1].extruder == 6);
    // I1: shared boundary vertex, same as split_polyline_by_vote.
    REQUIRE(!runs[1].pts.empty());
    CHECK(runs[1].pts.front() == runs[0].pts.back());
    // Coverage still holds end to end.
    CHECK(runs.front().pts.front() == Point(scale_(0), scale_(5)));
    CHECK(runs.back().pts.back()   == Point(scale_(40), scale_(5)));
}

TEST_CASE("split_polyline_by_resolver honors min_run_mm and max_runs like the vote variant", "[chameleon]")
{
    BrimVoteParams p; p.min_run_mm = 0.0; p.max_runs = 2;
    // Four zones -> would be four runs, but max_runs=2 forces a coalesce.
    auto resolver = [](const Point &pt) -> unsigned {
        const double x = unscale<double>(pt.x());
        if (x < 10.0) return 0u;
        if (x < 20.0) return 1u;
        if (x < 30.0) return 2u;
        return 3u;
    };
    auto runs = split_polyline_by_resolver(segment(0, 5, 40, 5), false, resolver, p);
    CHECK(runs.size() <= 2);
    CHECK(runs.front().pts.front() == Point(scale_(0), scale_(5)));
    CHECK(runs.back().pts.back()   == Point(scale_(40), scale_(5)));
}

TEST_CASE("partition_support_entities role_filter=base splits base, preserves erSupportMaterial role, leaves interfaces untouched", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);    // left wall -> extruder 1
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);   // right wall -> extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    auto fills = make_support_fills(5.0f);            // 1 base path @ y=-5, 2 interface paths near y=5..6
    const size_t iface_before = std::count_if(fills.entities.begin(), fills.entities.end(),
        [](const ExtrusionEntity *e) { return e->role() == erSupportMaterialInterface; });

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    // Base path was split across both walls -> ends up entirely in out[1]/out[2].
    REQUIRE(out.count(1) == 1);
    REQUIRE(out.count(2) == 1);
    CHECK(!out.at(1).entities.empty());
    CHECK(!out.at(2).entities.empty());
    for (auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erSupportMaterial);      // role copied from source, not hardcoded

    // Interface entities were never touched by a base-role_filter pass.
    size_t iface_after = 0, base_after = 0;
    for (const ExtrusionEntity *e : fills.entities) {
        if (e->role() == erSupportMaterialInterface) ++iface_after;
        if (e->role() == erSupportMaterial) ++base_after;
    }
    CHECK(iface_after == iface_before);
    CHECK(base_after == 0);                              // base fully matched away, none fell back
}

TEST_CASE("partition_support_entities role_filter=interface never touches base entities", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    auto fills = make_support_fills(5.0f);
    const size_t base_before = std::count_if(fills.entities.begin(), fills.entities.end(),
        [](const ExtrusionEntity *e) { return e->role() == erSupportMaterial; });

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterialInterface, 0, resolver, p, out);

    size_t base_after = 0;
    for (const ExtrusionEntity *e : fills.entities)
        if (e->role() == erSupportMaterial) ++base_after;
    CHECK(base_after == base_before);                    // base untouched, same count

    for (auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erSupportMaterialInterface);
}

TEST_CASE("partition_support_interfaces still works as a thin wrapper over partition_support_entities", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);
    REQUIRE(out.count(1) == 1);
    REQUIRE(out.count(2) == 1);
    for (auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erSupportMaterialInterface);
}

TEST_CASE("select_layers_in_band selects the coplanar (lo, hi] band; select_contact_layers matches it", "[chameleon]")
{
    std::vector<double> zs = {0.2, 0.4, 0.6, 0.8, 1.0};
    auto v = select_layers_in_band(zs, 0.4, 0.6);          // exactly one layer top in (0.4, 0.6]
    REQUIRE(v.size() == 1);
    CHECK(v[0] == 2);

    // select_contact_layers is now a thin call onto select_layers_in_band with
    // (support_top_z, support_top_z + gap_mm] -- results must be identical.
    auto band    = select_layers_in_band(zs, 0.4, 0.4 + 2.0);
    auto contact = select_contact_layers(zs, 0.4, 2.0);
    CHECK(band == contact);

    // VLH coplanar-style narrow band still resolves correctly.
    auto narrow = select_layers_in_band({0.2, 0.5, 1.4}, 0.2, 0.55);
    REQUIRE(narrow.size() == 1);
    CHECK(narrow[0] == 1);
}

// --- v2.1 Task 2: projection resolver pure geometric core -------------------

static ExPolygon square_expoly(double cx, double cy, double half)
{
    return ExPolygon(Points{
        Point(scale_(cx - half), scale_(cy - half)),
        Point(scale_(cx + half), scale_(cy - half)),
        Point(scale_(cx + half), scale_(cy + half)),
        Point(scale_(cx - half), scale_(cy + half)),
    });
}

static BoundingBox bbox_of(const ExPolygon &expoly) { return BoundingBox(expoly.contour.points); }

TEST_CASE("chameleon_pick_projection_region picks the lowest band layer that covers p", "[chameleon]")
{
    // Two band layers, caller-ordered lowest first (index 0 = lowest), both with a
    // single region whose slices cover the same 10x10 square -> p in both must still
    // resolve to the LOWER layer (index 0), "surface above wins" only ever escalates
    // to the next layer when the lower one misses (see the next test).
    ExPolygons lower_lslices = { square_expoly(0, 0, 5) };
    ExPolygons upper_lslices = { square_expoly(0, 0, 5) };

    ProjectionLayerView lower, upper;
    lower.lslices = &lower_lslices;
    lower.region_slice_polys = { { &lower_lslices[0] } };
    upper.lslices = &upper_lslices;
    upper.region_slice_polys = { { &upper_lslices[0] } };

    std::vector<ProjectionLayerView> layers = { lower, upper };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 0);
    CHECK(out_region == 0);
}

TEST_CASE("chameleon_pick_projection_region falls through to the upper band layer when the lower one misses", "[chameleon]")
{
    ExPolygons lower_lslices = { square_expoly(-20, 0, 5) };  // does NOT cover (0,0)
    ExPolygons upper_lslices = { square_expoly(0, 0, 5) };    // does

    ProjectionLayerView lower, upper;
    lower.lslices = &lower_lslices;
    lower.region_slice_polys = { { &lower_lslices[0] } };
    upper.lslices = &upper_lslices;
    upper.region_slice_polys = { { &upper_lslices[0] } };

    std::vector<ProjectionLayerView> layers = { lower, upper };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 1);
    CHECK(out_region == 0);
}

TEST_CASE("chameleon_pick_projection_region prefers the region with a bottom-surface hint at p", "[chameleon]")
{
    // One layer, two overlapping regions both containing p; only region 1 has a
    // stBottom/stBottomBridge fill surface covering p -> region 1 must win even
    // though region 0 is encountered first.
    ExPolygons lslices = { square_expoly(0, 0, 10) };
    ExPolygons region0_slice = { square_expoly(0, 0, 10) };
    ExPolygons region1_slice = { square_expoly(0, 0, 10) };
    ExPolygons region1_bottom = { square_expoly(0, 0, 10) };  // covers p

    ProjectionLayerView layer;
    layer.lslices = &lslices;
    layer.region_slice_polys = { { &region0_slice[0] }, { &region1_slice[0] } };
    layer.region_bottom_polys = { {}, { &region1_bottom[0] } };  // region 0: no bottom hint

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 0);
    CHECK(out_region == 1);
}

TEST_CASE("chameleon_pick_projection_region returns false when no band layer's lslices cover p", "[chameleon]")
{
    ExPolygons lslices = { square_expoly(50, 50, 5) };  // far from the origin
    ProjectionLayerView layer;
    layer.lslices = &lslices;
    layer.region_slice_polys = { { &lslices[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    CHECK_FALSE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    // out params must be left untouched on a miss.
    CHECK(out_layer == 999);
    CHECK(out_region == 999);
}

TEST_CASE("chameleon_pick_projection_region is bbox-gated but still exact when bboxes are absent", "[chameleon]")
{
    ExPolygons lslices = { square_expoly(0, 0, 5) };
    std::vector<BoundingBox> bboxes = { bbox_of(lslices[0]) };

    ProjectionLayerView layer;
    layer.lslices = &lslices;
    layer.lslices_bboxes = &bboxes;
    layer.region_slice_polys = { { &lslices[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    // Inside both the bbox and the polygon.
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 0);

    // Outside the bbox entirely -> gated out (same result as the exact test would give,
    // just cheaper).
    out_layer = 999;
    CHECK_FALSE(chameleon_pick_projection_region(layers, Point(scale_(50), scale_(50)), out_layer, out_region));
}

TEST_CASE("chameleon_pick_projection_region skips a layer whose lslices hit but no region slice does", "[chameleon]")
{
    // Degenerate/inconsistent input: lslices covers p but the (only) region's own
    // slice polygon doesn't -> must NOT report a false hit on that layer; continues
    // scanning and finds the next band layer instead of crashing or mis-resolving.
    ExPolygons layer0_lslices = { square_expoly(0, 0, 10) };
    ExPolygons layer0_region_slice = { square_expoly(-20, 0, 2) }; // doesn't cover (0,0)
    ExPolygons layer1_lslices = { square_expoly(0, 0, 10) };
    ExPolygons layer1_region_slice = { square_expoly(0, 0, 10) };  // does cover (0,0)

    ProjectionLayerView layer0, layer1;
    layer0.lslices = &layer0_lslices;
    layer0.region_slice_polys = { { &layer0_region_slice[0] } };
    layer1.lslices = &layer1_lslices;
    layer1.region_slice_polys = { { &layer1_region_slice[0] } };

    std::vector<ProjectionLayerView> layers = { layer0, layer1 };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 1);
    CHECK(out_region == 0);
}
