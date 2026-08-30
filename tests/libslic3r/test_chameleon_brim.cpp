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

    // Mirror Print.cpp's v2.2 apply_bucket_caps merge-back (BrimFilament.cpp): fold
    // the out-of-fallback runs back into support_fills, leaving it holding all three
    // pre-split runs as separate entities - exactly what either trigger in C1
    // (aliased shared-object copy, or a stale posSupportMaterial re-run) hands the
    // pass on its second visit to this layer.
    fills.append(std::move(out1.at(1).entities));

    std::map<unsigned, ExtrusionEntityCollection> out2;
    const size_t switches2 = partition_support_interfaces(fills, 0, idx, p, out2);

    // Pass 2 re-votes each already-split fragment independently and reports FEWER
    // switch boundaries than pass 1 found for the identical geometry. This
    // switch-boundary return value is no longer what caps anything (v2.2 Task 1,
    // spec C1-C3: apply_bucket_caps ranks/gates by bucket TOTAL PATH LENGTH, not by
    // this switch count - see BrimFilament.hpp's apply_bucket_caps doc comment), but
    // the underlying non-idempotency this test documents is exactly why Print.cpp's
    // chameleon_interface_visited guard (C1 fix, guard (b)) exists: a second
    // apply_bucket_caps call over these same pre-split fragments would rank the
    // middle zone's bucket by ITS OWN (now fragment-local) length rather than the
    // true length pass 1 matched, silently skewing the length-based trim/gate.
    CHECK(switches2 < switches1);
    // The undercounting is not from the geometry becoming inert: the middle zone's
    // material is still (correctly) matched to the foreign extruder on pass 2 - the
    // switch that causes is simply no longer reflected in the switch-boundary count.
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

TEST_CASE("brim_vote max_dist_mm cap filters the WHOLE electorate, not just the nearest sample (I1)", "[chameleon]")
{
    // v2.1 final-review I1: a denser cluster of samples just beyond the cap must not be
    // able to outvote a single sample inside the cap. The in-cap sample deliberately
    // carries the HIGHER extruder id (2): with the beyond-cap cluster (extruder 1) in
    // the electorate, the score gap (2/1.1^2 ~= 1.65 vs 1/0.9^2 ~= 1.23, within 30%)
    // plus the 0.2mm nearest-distance gap (< 0.3mm) sends the pre-fix code down the tie
    // path, whose empty-object_area fallback picks min(extruder id) = 1 - the beyond-cap
    // wall. Post-fix, the two 1.1mm samples are erased from the electorate before any
    // scoring or tie-breaking, so only in-cap extruder 2 can win. (With the ids the
    // other way round the tie path would coincidentally return the right answer and the
    // test could not fail pre-fix - re-review finding N1.)
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 0.9, 0, 0.9), 2, 1);     // extruder 2 @ 0.9mm - inside cap
    idx.add_polyline(segment(1.1, 0, 1.1, 0), 1, 2);     // extruder 1 @ 1.1mm - outside cap
    idx.add_polyline(segment(-1.1, 0, -1.1, 0), 1, 3);   // extruder 1 @ 1.1mm - outside cap (denser cluster)
    BrimVoteParams p;
    p.fallback_extruder = 9;
    p.max_dist_mm = 1.0;
    CHECK(brim_vote(idx, Point(0, 0), p) == 2);
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

TEST_CASE("select_layers_overlapping_span finds a straddling layer that select_layers_in_band misses (I2)", "[chameleon]")
{
    // v2.1 final-review I2: a band only one support-layer tall (here (9.8, 10.0]) must
    // still pick up an object layer whose TOP overshoots hi_z but whose BOTTOM still
    // dips into the band - its walls flank the band at this z even though its own top
    // lies above it (unsynced support/object layer grids, variable layer height).
    // Object layer tops: 9.6, 9.9, 10.2, 10.5 -> intervals (0,9.6], (9.6,9.9],
    // (9.9,10.2], (10.2,10.5].
    std::vector<double> zs = {9.6, 9.9, 10.2, 10.5};

    // Old top-z-only test: only the layer whose TOP lies in (9.8, 10.0] - index 1 (top
    // 9.9). The straddling layer at index 2 (top 10.2, bottom 9.9 - its interval
    // (9.9,10.2] overlaps (9.8,10.0] even though its own top overshoots hi_z) is missed
    // - this is the defect I2 fixes.
    auto old_band = select_layers_in_band(zs, 9.8, 10.0);
    REQUIRE(old_band.size() == 1);
    CHECK(old_band[0] == 1);

    // select_layers_overlapping_span finds BOTH the in-band layer (index 1) and the
    // straddling layer (index 2, found). The fully-disjoint layers are still excluded:
    // index 0's interval (0,9.6] never reaches lo_z=9.8 (not found); index 3's interval
    // (10.2,10.5] starts at/after hi_z=10.0 (not found).
    auto overlap = select_layers_overlapping_span(zs, 9.8, 10.0);
    REQUIRE(overlap.size() == 2);
    CHECK(overlap[0] == 1);
    CHECK(overlap[1] == 2);

    // Sanity: on a band wide enough that top-z-in-band already finds every overlapping
    // layer (no straddler at the far edge), the two selectors agree.
    auto wide_old = select_layers_in_band(zs, 9.5, 10.6);
    auto wide_new = select_layers_overlapping_span(zs, 9.5, 10.6);
    CHECK(wide_old == wide_new);
}

TEST_CASE("select_layers_overlapping_span raft-aware first bottom (N2)", "[chameleon]")
{
    // Re-review N2: with a raft, the object's first layer starts well above the plate.
    // First layer spans (0.9, 1.1] (raft below it); a raft-level band (0.1, 0.3] must
    // NOT match it. The default first_bottom_z = 0.0 wrongly treats layer 0 as spanning
    // (0.0, 1.1] and matches.
    std::vector<double> zs = {1.1, 1.3};
    auto wrong = select_layers_overlapping_span(zs, 0.1, 0.3);
    REQUIRE(wrong.size() == 1);           // documents the default-parameter hazard
    CHECK(wrong[0] == 0);
    auto raft_aware = select_layers_overlapping_span(zs, 0.1, 0.3, 0.9);
    CHECK(raft_aware.empty());
    // A band genuinely overlapping the first layer still matches with the true bottom.
    auto touching = select_layers_overlapping_span(zs, 0.85, 1.0, 0.9);
    REQUIRE(touching.size() == 1);
    CHECK(touching[0] == 0);
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

TEST_CASE("chameleon_pick_projection_region falls back to the nearest region on the SAME layer when lslices hit but no region slice does (C5)", "[chameleon]")
{
    // v2.2 Task 2 (spec C5) deliberately changes this from v2.1's behavior: lslices
    // covers p but the (only) region's own slice polygon doesn't. v2.1 treated this as
    // a miss for the whole layer and fell through to the next band layer (see this
    // test's own pre-C5 history). C5's nearest-region fallback now applies uniformly
    // whenever a layer is a hit but no region's raw slices contain p - including this
    // pre-existing degenerate case, not just the new margin-ring case - so layer0 is
    // resolved using ITS OWN (distant, non-containing) region instead of skipping to
    // layer1's exact-containing one. "Surface above [i.e. the lower band layer] wins"
    // outranks "prefer an exact containment on a higher layer".
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
    CHECK(out_layer == 0);   // v2.1 expected 1 here - C5 changes this, see comment above
    CHECK(out_region == 0);  // layer0's only region, chosen by the nearest-region fallback
}

TEST_CASE("chameleon_pick_projection_region still falls through when a hit layer offers NO region with any raw geometry at all", "[chameleon]")
{
    // The genuinely-unresolvable case the C5 fallback does NOT paper over: layer0's
    // lslices cover p, but EVERY region on layer0 contributes zero raw slice polys (not
    // just non-containing ones - there is nothing at all to measure "nearest" against).
    // nearest_region_to_point has no candidate, so this layer still yields no region,
    // and the scan correctly continues to layer1.
    ExPolygons layer0_lslices = { square_expoly(0, 0, 10) };
    ExPolygons layer1_lslices = { square_expoly(0, 0, 10) };
    ExPolygons layer1_region_slice = { square_expoly(0, 0, 10) };  // does cover (0,0)

    ProjectionLayerView layer0, layer1;
    layer0.lslices = &layer0_lslices;
    layer0.region_slice_polys = { {} };  // one region, contributes nothing
    layer1.lslices = &layer1_lslices;
    layer1.region_slice_polys = { { &layer1_region_slice[0] } };

    std::vector<ProjectionLayerView> layers = { layer0, layer1 };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, Point(0, 0), out_layer, out_region));
    CHECK(out_layer == 1);
    CHECK(out_region == 0);
}

// --- v2.1 Task 3: pass rewiring (two-call sequence, shared out map) -------

TEST_CASE("v2.1 Task 3: interface-then-base calls share one out map; buckets can mix roles; cap merge-back is role-agnostic", "[chameleon]")
{
    // Mirrors chameleon_assign_support_interfaces' v2.1 shape: an interface-role call
    // and a base-role call both consult the same coplanar wall data and write into the
    // SAME `out` map (Print.cpp's `partitioned`), differing only in role_filter/
    // fallback_extruder (the pass's interface_resolver/base_resolver both ultimately
    // call brim_vote against one shared coplanar WallSampleIndex; only projection - not
    // exercised by this synthetic-resolver test - and fallback_extruder differ). This
    // test is about what happens when BOTH calls write into the SAME out map, not
    // about resolver composition (already covered by the projection-region tests
    // above and the Print.cpp hand-walk in the task report).
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);    // left wall -> extruder 1
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);   // right wall -> extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    // 1 base path @ y=-5 (x 0->30), 2 interface paths @ y=5/6 (x 0->30) - both roles'
    // paths span both walls' x-ranges, so both calls are expected to split their own
    // role's entities across extruder 1 and extruder 2 (established individually by
    // the "role_filter=base"/"role_filter=interface" tests above).
    auto fills = make_support_fills(5.0f);

    std::map<unsigned, ExtrusionEntityCollection> partitioned;

    const size_t interface_switches = partition_support_entities(fills, erSupportMaterialInterface,
        0, resolver, p, partitioned);
    const size_t base_switches = partition_support_entities(fills, erSupportMaterial,
        0, resolver, p, partitioned);

    // Task 3: "ONE shared out map" - both calls write into the same `partitioned`
    // map (v2.2 Task 1: that map is what a single apply_bucket_caps call then
    // gates/trims as a whole by bucket path length, not by these switch-boundary
    // return values - see BrimFilament.hpp's apply_bucket_caps doc comment). Both
    // calls contributed here (neither degenerated to the uniform-fallback fast path).
    CHECK(interface_switches > 0);
    CHECK(base_switches > 0);

    REQUIRE(partitioned.count(1) == 1);
    REQUIRE(partitioned.count(2) == 1);

    // Every original entity (both roles) was fully matched away in this geometry -
    // nothing fell back to support_fills (mirrors the individual base/interface tests
    // above, now happening back-to-back into the shared map).
    CHECK(fills.entities.empty());

    // The crux of Task 3's shared-map design: at least one extruder bucket holds BOTH
    // roles (an interface run and a base run both matched the same wall's extruder),
    // and every path still carries its own true source role - never hardcoded by
    // either call, never clobbered by the second call overwriting the first's work.
    bool saw_interface_role = false, saw_base_role = false, found_mixed_bucket = false;
    for (auto &kv : partitioned) {
        bool has_iface = false, has_base = false;
        for (const ExtrusionEntity *e : kv.second.entities) {
            CHECK((e->role() == erSupportMaterialInterface || e->role() == erSupportMaterial));
            if (e->role() == erSupportMaterialInterface) { has_iface = true; saw_interface_role = true; }
            if (e->role() == erSupportMaterial)          { has_base  = true; saw_base_role      = true; }
        }
        if (has_iface && has_base)
            found_mixed_bucket = true;
    }
    CHECK(saw_interface_role);
    CHECK(saw_base_role);
    CHECK(found_mixed_bucket);

    // Mirror apply_bucket_caps' merge-back (BrimFilament.cpp: merge_back_target.
    // append(std::move(it->second.entities)) for every gated/trimmed bucket) -
    // append(ExtrusionEntitiesPtr&&) is role-agnostic, so merging a map built from TWO
    // different role_filter calls back into one collection must restore BOTH roles'
    // geometry, not just whichever role happened to be appended first/last. This is
    // the same mechanic the old whole-layer revert used (just applied per-bucket by
    // apply_bucket_caps now, rather than unconditionally to every bucket per-layer).
    for (auto &kv : partitioned)
        fills.append(std::move(kv.second.entities));

    size_t restored_iface = 0, restored_base = 0;
    for (const ExtrusionEntity *e : fills.entities) {
        if (e->role() == erSupportMaterialInterface) ++restored_iface;
        if (e->role() == erSupportMaterial)           ++restored_base;
    }
    CHECK(restored_iface > 0);
    CHECK(restored_base > 0);
}

// --- v2.2 Task 1: cap semantics rework (spec C1-C3) -------------------------

// Builds one bucket's ExtrusionEntityCollection: a single straight path of exactly
// `len_mm` length (x-axis, so total_path_length_mm's unscale<double>(length()) sum is
// trivial to hand-verify), role/flow attributes arbitrary (mirrors make_support_fills'
// fixture style above - only the geometry matters to these tests).
static ExtrusionEntityCollection bucket_of_length(double len_mm)
{
    ExtrusionEntityCollection c;
    auto *p = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    p->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(len_mm), scale_(0))});
    c.entities.push_back(p);
    return c;
}

TEST_CASE("total_path_length_mm sums a bucket's entities", "[chameleon]")
{
    ExtrusionEntityCollection c = bucket_of_length(12.5);
    CHECK_THAT(total_path_length_mm(c), Catch::Matchers::WithinAbs(12.5, 1e-6));

    // Two paths -> sum, not just the first.
    auto *p2 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    p2->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(0), scale_(17.5))});
    c.entities.push_back(p2);
    CHECK_THAT(total_path_length_mm(c), Catch::Matchers::WithinAbs(30.0, 1e-6));

    // Empty collection -> 0, not a throw (ExtrusionEntityCollection::length() itself
    // throws - this helper must never call it directly).
    ExtrusionEntityCollection empty;
    CHECK_THAT(total_path_length_mm(empty), Catch::Matchers::WithinAbs(0.0, 1e-6));
}

TEST_CASE("apply_bucket_caps: under-cap map is left untouched", "[chameleon]")
{
    // 2 buckets, max_extruders=2 -> nothing to trim, and both are well above the
    // min-benefit floor -> nothing to gate either.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(100.0);
    map[2] = bucket_of_length(80.0);
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    REQUIRE(map.count(1) == 1);
    REQUIRE(map.count(2) == 1);
    CHECK(!map.at(1).entities.empty());
    CHECK(!map.at(2).entities.empty());
    CHECK(merge_back.entities.empty());
    CHECK(result.kept == std::set<unsigned>{1, 2});
    CHECK(result.buckets_dropped_min_benefit == 0);
    CHECK(result.buckets_trimmed_cap == 0);
}

TEST_CASE("apply_bucket_caps: sub-40mm bucket is dropped by the min-benefit gate", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(100.0);
    map[2] = bucket_of_length(39.9);   // just under the 40mm floor
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(map.count(1) == 1);
    CHECK(map.count(2) == 0);                 // gated out
    CHECK(!merge_back.entities.empty());      // its geometry landed back in fallback
    CHECK(result.kept == std::set<unsigned>{1});
    CHECK(result.buckets_dropped_min_benefit == 1);
    CHECK(result.buckets_trimmed_cap == 0);
}

TEST_CASE("apply_bucket_caps: 3-extruder partition trims to the 2 longest", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(50.0);
    map[2] = bucket_of_length(100.0);
    map[3] = bucket_of_length(75.0);
    std::set<unsigned> prev_kept;             // no hysteresis pressure - pure length rank
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    // Longest two survive (2: 100mm, 3: 75mm); shortest (1: 50mm) is trimmed.
    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 1);
    CHECK(!merge_back.entities.empty());
    CHECK(result.kept == std::set<unsigned>{2, 3});
    CHECK(result.buckets_dropped_min_benefit == 0);
    CHECK(result.buckets_trimmed_cap == 1);
}

TEST_CASE("apply_bucket_caps: hysteresis preference flips which bucket the trim keeps", "[chameleon]")
{
    // Same 3 buckets both times: ext 1 is the SHORTEST (30mm - would lose on pure
    // length to both 2 and 3), ext 2 is longest (100mm), ext 3 is middle (90mm).
    auto make_map = [] {
        std::map<unsigned, ExtrusionEntityCollection> m;
        m[1] = bucket_of_length(30.0);
        m[2] = bucket_of_length(100.0);
        m[3] = bucket_of_length(90.0);
        return m;
    };

    // Without hysteresis (empty prev_kept): pure length rank keeps the two longest,
    // {2, 3} - ext 1 loses despite being above the 40mm... wait, ext1 is BELOW the
    // min-benefit floor at 30mm in the default fixture; use a lower floor here so the
    // gate doesn't remove it before the trim even runs - this test is about the TRIM's
    // ranking, not the gate.
    {
        std::map<unsigned, ExtrusionEntityCollection> map = make_map();
        std::set<unsigned> prev_kept;   // ext 1 has no seniority
        ExtrusionEntityCollection merge_back;
        BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 10.0, merge_back);
        CHECK(result.kept == std::set<unsigned>{2, 3});   // pure length: 1 (30mm) loses
    }

    // With hysteresis (prev_kept = {1}): ext 1 outranks BOTH 2 and 3 outright despite
    // being the shortest bucket by far - stability up the column beats raw length.
    // Among the two non-prev-kept candidates (2, 3), the trim still falls back to
    // length to pick the second survivor: 2 (100mm) over 3 (90mm). This is the flip:
    // the committed set changes from {2,3} to {1,2} purely because of prev_kept.
    {
        std::map<unsigned, ExtrusionEntityCollection> map = make_map();
        std::set<unsigned> prev_kept{1};
        ExtrusionEntityCollection merge_back;
        BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 10.0, merge_back);
        CHECK(result.kept == std::set<unsigned>{1, 2});
        CHECK(map.count(1) == 1);
        CHECK(map.count(2) == 1);
        CHECK(map.count(3) == 0);
        CHECK(result.buckets_trimmed_cap == 1);
    }
}

TEST_CASE("apply_bucket_caps: gate runs before the trim, so a gated sliver never occupies a cap slot", "[chameleon]")
{
    // 3 buckets, only 2 survive the 40mm gate; max_extruders=2 -> the trim then has
    // nothing left to do (exactly at the cap, not over it).
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(100.0);
    map[2] = bucket_of_length(80.0);
    map[3] = bucket_of_length(5.0);     // sliver - gated
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(map.count(1) == 1);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 0);
    CHECK(result.kept == std::set<unsigned>{1, 2});
    CHECK(result.buckets_dropped_min_benefit == 1);
    CHECK(result.buckets_trimmed_cap == 0);    // never reached - only 2 buckets left post-gate
}

// --- v2.2 Task 2: gap-aware lateral cap arithmetic (spec C4) ----------------

TEST_CASE("gap_aware_lateral_cap_mm sums the gap, both half-widths, and the 0.35mm slack", "[chameleon]")
{
    // 0.2mm gap + 0.4mm outer wall (half 0.2) + 0.42mm support line (half 0.21) +
    // 0.35mm slack = 0.96mm - well above the old flat 1.0mm cap was frequently able to
    // satisfy at these same inputs (the whole point of C4: 1.0 alone left ~0.04mm of
    // slack over JUST the physical minimum, none at all once real-world jitter is
    // added - see the spec's root cause 1).
    const double cap = gap_aware_lateral_cap_mm(0.2, 0.4, 0.42);
    CHECK_THAT(cap, Catch::Matchers::WithinAbs(0.96, 1e-9));
}

TEST_CASE("gap_aware_lateral_cap_mm with zero widths still returns gap + slack", "[chameleon]")
{
    // Degenerate/defensive input (e.g. a region contributing no printing regions at
    // all, so outer_wall_width_mm resolved to 0) - the formula must not divide, throw,
    // or otherwise misbehave; it's a plain sum, so 0 half-widths just drop out.
    const double cap = gap_aware_lateral_cap_mm(0.15, 0.0, 0.0);
    CHECK_THAT(cap, Catch::Matchers::WithinAbs(0.50, 1e-9));
}

TEST_CASE("gap_aware_lateral_cap_mm is NOT symmetric in its two width args (documents call-site order doesn't matter for the SUM, but values do)", "[chameleon]")
{
    // Sanity check that both widths are actually halved independently rather than one
    // being silently ignored or double-counted - swap outer/support widths and confirm
    // the result tracks the (now-different) sum, not the same cached value.
    const double cap_a = gap_aware_lateral_cap_mm(0.3, 0.6, 0.2);   // 0.3+0.3+0.1+0.35=1.05
    const double cap_b = gap_aware_lateral_cap_mm(0.3, 0.2, 0.6);   // 0.3+0.1+0.3+0.35=1.05
    // Same total in this particular pair (sum is order-insensitive by construction),
    // but a THIRD case with an actually different total proves neither arg is dropped.
    const double cap_c = gap_aware_lateral_cap_mm(0.3, 0.6, 0.6);   // 0.3+0.3+0.3+0.35=1.25
    CHECK_THAT(cap_a, Catch::Matchers::WithinAbs(1.05, 1e-9));
    CHECK_THAT(cap_b, Catch::Matchers::WithinAbs(1.05, 1e-9));
    CHECK_THAT(cap_c, Catch::Matchers::WithinAbs(1.25, 1e-9));
}

// --- v2.2 Task 2: projection margin ring (spec C5) --------------------------

TEST_CASE("chameleon_pick_projection_region: margin-ring point (raw miss, expanded hit) resolves to the nearest region", "[chameleon]")
{
    // p is outside every region's raw slices on this layer (so the pre-existing
    // containment scan finds nothing) but inside the layer's expanded_lslices (the C5
    // margin ring - hand-built here directly, standing in for what
    // chameleon_build_projection_views would compute via offset_ex in production).
    // Two candidate regions at different distances from p; the nearer one must win.
    Point p(0, 0);

    ExPolygons raw_lslices      = { square_expoly(20, 0, 5) };   // spans x in [15,25] - doesn't cover p
    ExPolygons expanded_lslices = { square_expoly(10, 0, 15) };  // spans x in [-5,25] - covers p

    ExPolygons region_far_slice  = { square_expoly(20, 0, 5) };  // nearest vertex (15,*) -> dist ~15.81mm
    ExPolygons region_near_slice = { square_expoly(3, 0, 1) };   // nearest vertex (2,*)  -> dist ~2.24mm

    ProjectionLayerView layer;
    layer.lslices             = &raw_lslices;
    layer.expanded_lslices    = expanded_lslices;
    layer.region_slice_polys  = { { &region_far_slice[0] }, { &region_near_slice[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 0);
    CHECK(out_region == 1);   // region_near_slice, not region_far_slice (index 0)
}

TEST_CASE("chameleon_pick_projection_region: point beyond even the expanded margin still misses", "[chameleon]")
{
    // p is outside BOTH the raw lslices and the expanded (margin-ring) lslices on the
    // only band layer available - the margin doesn't turn every sample into a hit, only
    // ones actually within it.
    Point p(0, 0);

    ExPolygons raw_lslices      = { square_expoly(50, 50, 5) };   // far from the origin
    ExPolygons expanded_lslices = { square_expoly(50, 50, 7) };   // still far - modest margin growth

    ProjectionLayerView layer;
    layer.lslices            = &raw_lslices;
    layer.expanded_lslices   = expanded_lslices;
    layer.region_slice_polys = { { &raw_lslices[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    CHECK_FALSE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 999);
    CHECK(out_region == 999);
}

TEST_CASE("chameleon_pick_projection_region: nearest-region fallback ties broken by ascending region index", "[chameleon]")
{
    // Two regions exactly equidistant from p (mirror images across the origin), neither
    // containing p, both reachable only via the margin ring - the tie must resolve to
    // the LOWER region index deterministically, not by iteration/insertion order tricks.
    Point p(0, 0);

    ExPolygons raw_lslices      = { square_expoly(50, 50, 5) };  // far from p - only expanded covers it
    ExPolygons expanded_lslices = { square_expoly(0, 0, 10) };   // covers p

    ExPolygons region0_slice = { square_expoly(-3, 0, 1) };  // nearest vertex (-2,+-1) -> dist sqrt(5) ~= 2.236mm
    ExPolygons region1_slice = { square_expoly(3, 0, 1) };   // nearest vertex (2,+-1)  -> dist sqrt(5) ~= 2.236mm (exact tie)

    ProjectionLayerView layer;
    layer.lslices             = &raw_lslices;
    layer.expanded_lslices    = expanded_lslices;
    layer.region_slice_polys  = { { &region0_slice[0] }, { &region1_slice[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 0);
    CHECK(out_region == 0);   // ascending-index tie-break, not region1
}

TEST_CASE("chameleon_pick_projection_region: raw containment still wins outright over the margin-ring fallback", "[chameleon]")
{
    // Regression guard for "raw-containment behavior unchanged" (spec C5's own
    // constraint): when p IS inside a region's raw slice polygon, that must be picked
    // even though expanded_lslices is also populated and would otherwise trigger the
    // nearest-region machinery - the raw containment scan runs FIRST and returns before
    // the nearest-region fallback is ever consulted.
    Point p(0, 0);

    ExPolygons raw_lslices      = { square_expoly(0, 0, 10) };
    ExPolygons expanded_lslices = { square_expoly(0, 0, 15) };

    ExPolygons region0_slice = { square_expoly(20, 0, 2) };   // doesn't cover p (would win the nearest search if reached)
    ExPolygons region1_slice = { square_expoly(0, 0, 10) };   // DOES cover p

    ProjectionLayerView layer;
    layer.lslices            = &raw_lslices;
    layer.expanded_lslices   = expanded_lslices;
    layer.region_slice_polys = { { &region0_slice[0] }, { &region1_slice[0] } };

    std::vector<ProjectionLayerView> layers = { layer };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 0);
    CHECK(out_region == 1);
}

// --- v2.2 Task 3: ironing follows its interface (spec C6) + nested collections (spec C7) ---

TEST_CASE("partition_support_entities role_filter=erIroning splits ironing, preserves role, leaves other roles untouched (C6)", "[chameleon]")
{
    // v2.2 Task 3 (spec C6): partition_support_entities was already role-generic
    // (role_filter has never been special-cased, since Task 1) - this documents that
    // genericity extends correctly to erIroning specifically, which is what Print.cpp's
    // new THIRD partition_support_entities call (chameleon_assign_support_interfaces)
    // now actually exercises with role_filter=erIroning. The orchestration wiring
    // itself (the third call site's placement before apply_bucket_caps, and the
    // erIroning-last emission fix in GCode.cpp) is not unit-instantiable - same
    // "layer-loop logic" reason Task 1's own report gives for not unit-testing
    // chameleon_assign_support_interfaces directly.
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 1, 1);
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    auto fills = make_support_fills(5.0f); // 1 base path @ y=-5, 2 interface paths @ y=5/6
    auto *ironing = new ExtrusionPath(erIroning, 1.0, 0.4f, 0.2f);
    ironing->polyline = Polyline({Point(scale_(0), scale_(6.5)), Point(scale_(30), scale_(6.5))});
    fills.entities.push_back(ironing);

    const size_t base_before = std::count_if(fills.entities.begin(), fills.entities.end(),
        [](const ExtrusionEntity *e) { return e->role() == erSupportMaterial; });
    const size_t iface_before = std::count_if(fills.entities.begin(), fills.entities.end(),
        [](const ExtrusionEntity *e) { return e->role() == erSupportMaterialInterface; });

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erIroning, 0, resolver, p, out);

    REQUIRE(out.count(1) == 1);
    REQUIRE(out.count(2) == 1);
    for (auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erIroning); // role copied from source, never hardcoded

    size_t base_after = 0, iface_after = 0, ironing_after = 0;
    for (const ExtrusionEntity *e : fills.entities) {
        if (e->role() == erSupportMaterial)          ++base_after;
        if (e->role() == erSupportMaterialInterface) ++iface_after;
        if (e->role() == erIroning)                  ++ironing_after;
    }
    CHECK(base_after == base_before);   // base untouched by an erIroning-role_filter pass
    CHECK(iface_after == iface_before); // interface untouched by an erIroning-role_filter pass
    CHECK(ironing_after == 0);          // ironing fully matched away, none fell back
}

TEST_CASE("support_role_needs_interface_extruder: erIroning newly needs the interface extruder registered (C6)", "[chameleon]")
{
    // v2.2 Task 3 (spec C6, third anchor): the shared predicate behind ToolOrdering.cpp
    // (~701-703) and GCode.cpp's own support-bucket mirror (~5339-5341) - both
    // orchestration-level and not unit-instantiable directly, but the classification
    // itself was extracted to BrimFilament.hpp/.cpp specifically so it IS testable.
    // Investigation finding: a layer whose support_fills collapses to PURE erIroning
    // (every erSupportMaterial/erSupportMaterialInterface entity matched away by the
    // third partition_support_entities call and/or a C7 whole-collection move, residual
    // fallback ironing left behind) must still count as needing the interface extruder,
    // or that ironing's toolchange/bucket never gets registered and it silently never
    // prints.
    CHECK(support_role_needs_interface_extruder(erIroning));      // v2.2 Task 3 addition
    CHECK(support_role_needs_interface_extruder(erMixed));        // unchanged pre-v2.2
    CHECK(support_role_needs_interface_extruder(erSupportMaterialInterface)); // unchanged pre-v2.2
    CHECK_FALSE(support_role_needs_interface_extruder(erSupportMaterial));
    CHECK_FALSE(support_role_needs_interface_extruder(erSupportTransition));
    CHECK_FALSE(support_role_needs_interface_extruder(erNone));
}

// Builds a nested (non-flat) ExtrusionEntityCollection of two erSupportMaterial leaf
// paths at the given y, spanning the given x half. Mirrors the double-wall-branch /
// no_sort-sheath collections spec root cause 6 calls out as "invisible to the matcher"
// pre-C7.
static ExtrusionEntityCollection *nested_base_collection(double y, bool no_sort)
{
    auto *inner = new ExtrusionEntityCollection();
    inner->no_sort = no_sort;
    auto *leaf_a = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf_a->polyline = Polyline({Point(scale_(0), scale_(y)), Point(scale_(10), scale_(y))});
    auto *leaf_b = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf_b->polyline = Polyline({Point(scale_(20), scale_(y)), Point(scale_(30), scale_(y))});
    inner->entities.push_back(leaf_a);
    inner->entities.push_back(leaf_b);
    return inner;
}

TEST_CASE("partition_support_entities (C7): a role-eligible nested collection is voted as ONE unit and moved whole, never split", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 30, 6), 1, 1); // one wall spanning the whole span -> extruder 1 everywhere
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    ExtrusionEntityCollection *inner = nested_base_collection(6.0, /*no_sort=*/true);
    REQUIRE(inner->role() == erSupportMaterial); // collapsed role - both leaves uniform

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);
    // A plain (non-collection) base path alongside it, to show the new collection
    // branch doesn't interfere with ordinary leaf partitioning.
    auto *plain = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    plain->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(30), scale_(6))});
    fills.entities.push_back(plain);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    REQUIRE(out.count(1) == 1);
    bool found_whole_collection = false;
    for (const ExtrusionEntity *e : out.at(1).entities) {
        if (e == inner) {
            found_whole_collection = true;
            REQUIRE(e->is_collection());
            const auto *coll = static_cast<const ExtrusionEntityCollection *>(e);
            CHECK(coll->entities.size() == 2);  // both leaves intact - never split apart
            CHECK(coll->no_sort == true);       // C7: no_sort travels with the moved pointer, untouched
        }
    }
    CHECK(found_whole_collection);

    // Moved, not cloned: the original pointer is gone from support_fills' top level.
    for (const ExtrusionEntity *e : fills.entities)
        CHECK(e != inner);
}

TEST_CASE("partition_support_entities (C7): a role-eligible nested collection that votes fallback stays in place, untouched", "[chameleon]")
{
    WallSampleIndex idx; // empty index -> brim_vote always returns fallback_extruder
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    ExtrusionEntityCollection *inner = nested_base_collection(6.0, /*no_sort=*/false);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    CHECK(out.empty());
    REQUIRE(fills.entities.size() == 1);
    CHECK(fills.entities.front() == inner); // untouched pointer, still in place
    CHECK(static_cast<ExtrusionEntityCollection *>(fills.entities.front())->entities.size() == 2);
}

TEST_CASE("partition_support_entities (C7): a mixed-role nested collection is left untouched, never voted or split", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 30, 6), 1, 1);
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    // erSupportMaterial + erSupportMaterialInterface leaves -> collapsed role() ==
    // erMixed, which never equals either role_filter this pass calls with below.
    auto *inner = new ExtrusionEntityCollection();
    auto *leaf_base = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf_base->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(10), scale_(6))});
    auto *leaf_iface = new ExtrusionPath(erSupportMaterialInterface, 1.0, 0.4f, 0.2f);
    leaf_iface->polyline = Polyline({Point(scale_(20), scale_(6)), Point(scale_(30), scale_(6))});
    inner->entities.push_back(leaf_base);
    inner->entities.push_back(leaf_iface);
    REQUIRE(inner->role() == erMixed);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out_base, out_iface;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out_base);
    partition_support_entities(fills, erSupportMaterialInterface, 0, resolver, p, out_iface);

    CHECK(out_base.empty());
    CHECK(out_iface.empty());
    REQUIRE(fills.entities.size() == 1);
    CHECK(fills.entities.front() == inner); // untouched pointer, both passes
    CHECK(static_cast<ExtrusionEntityCollection *>(fills.entities.front())->entities.size() == 2);
}

TEST_CASE("partition_support_entities (C7): whole-collection majority vote ties break to the LOWEST extruder id", "[chameleon]")
{
    // Two walls, each covering exactly one of the collection's two EQUAL-length leaves
    // (10mm each) - build_chain's 0.8mm-cadence sampling of each congruent leaf
    // produces the identical sample count on each side, so votes tie EXACTLY between
    // extruder 1 and extruder 2. The lower id must win outright (no split, no crash).
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 10, 6), 1, 1);
    idx.add_polyline(segment(20, 6, 30, 6), 2, 2);
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    ExtrusionEntityCollection *inner = nested_base_collection(6.0, /*no_sort=*/false);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    REQUIRE(out.count(1) == 1);
    CHECK(out.count(2) == 0);  // tie broken to the lower id, never split between both buckets
    CHECK(fills.entities.empty());
}

TEST_CASE("total_path_length_mm recurses into a nested collection a C7 whole-collection move can now leave in a bucket", "[chameleon]")
{
    // v2.2 Task 1's own report flagged this recursion as "not exercised by today's
    // tests" until a nested-collection vote (C7) could land a whole
    // ExtrusionEntityCollection* in a bucket - it now can (the test just above proves
    // the move), so apply_bucket_caps' gate/trim measures such a bucket correctly.
    ExtrusionEntityCollection bucket;
    auto *inner = new ExtrusionEntityCollection();
    auto *leaf1 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf1->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(10), scale_(0))});
    auto *leaf2 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf2->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(15), scale_(0))});
    inner->entities.push_back(leaf1);
    inner->entities.push_back(leaf2);
    bucket.entities.push_back(inner);

    CHECK_THAT(total_path_length_mm(bucket), Catch::Matchers::WithinAbs(25.0, 1e-6));
}
