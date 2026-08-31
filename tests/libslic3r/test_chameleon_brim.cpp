#include <catch2/catch.hpp>
#include "libslic3r/WallSampleIndex.hpp"
#include "libslic3r/BrimFilament.hpp"
#include "libslic3r/libslic3r.h"
#include "libslic3r/ExtrusionEntityCollection.hpp"
#include <algorithm>
#include <cmath>

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

TEST_CASE("partition_support_interfaces uniform-fallback vote buckets pointer-stable (v2.5c root cause fix)", "[chameleon]")
{
    // v2.5c root cause fix: pre-v2.5c, an entity whose every sample voted the
    // fallback extruder took a special "stays in support_fills, untouched" fast
    // path - this definitionally excluded a REAL painted color that happens to
    // equal the fallback extruder from ever being bucketed/matched (the claw-wrap
    // bug: CHAMELEON_DEBUG logging on the user's own GUI slice showed fallback=0
    // with white wall samples plentiful (194-593/layer) yet ZERO e0 buckets across
    // all 560 layers - white could never win because it happened to be extruder
    // 0). This test used to be named "...fast path keeps entities" and asserted
    // `out.empty()` / entity count unchanged; it is RE-POINTED here at the new
    // contract - "uniform anything moves whole into its bucket" now applies to
    // fallback too, not just non-fallback winners. The wall below genuinely votes
    // extruder 0 (not "no candidates, so return fallback") - it just happens that
    // 0 is also p.fallback_extruder, exactly the collision the root cause
    // describes.
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 30, 6), 0, 1);    // wall's own extruder equals fallback_extruder
    BrimVoteParams p; p.fallback_extruder = 0;
    auto fills = make_support_fills(5.0f);
    ExtrusionEntity *iface_a = fills.entities[1]; // make_support_fills: base, then 2 interface paths
    ExtrusionEntity *iface_b = fills.entities[2];
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_interfaces(fills, 0, idx, p, out);

    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 2);
    // Pointer-stable: the ORIGINAL entities moved (no clone, no churn) - same
    // no-churn contract the old fast path gave only to a "stays" outcome; now the
    // outcome is "moves", but the pointer-stability itself is preserved.
    CHECK(std::find(out.at(0).entities.begin(), out.at(0).entities.end(), iface_a) != out.at(0).entities.end());
    CHECK(std::find(out.at(0).entities.begin(), out.at(0).entities.end(), iface_b) != out.at(0).entities.end());

    // The base path (a different role_filter) is never touched by this
    // interface-only call; both interface originals are gone from support_fills
    // (moved into out[0], not left behind and not duplicated).
    size_t base_n = 0, iface_n = 0;
    for (auto* e : fills.entities) (e->role() == erSupportMaterial ? base_n : iface_n)++;
    CHECK(base_n == 1);
    CHECK(iface_n == 0);
}

TEST_CASE("partition_support_entities (v2.5c): a uniform NON-fallback top-level leaf now also moves whole, pointer-stable - loop-ness preserved", "[chameleon]")
{
    // v2.5c unifies the top-level leaf fast path: "uniform anything moves whole
    // into its bucket" (item 1 of the fix), not just uniform fallback. Pre-v2.5c, a
    // uniform NON-fallback vote already left support_fills (matching the pre-C5
    // header doc's own "non-fallback majority moves whole" wording for the
    // COLLECTION case) but at the LEAF level it was still REBUILT into a plain new
    // ExtrusionPath (see BrimFilament.hpp's pre-v2.5c partition_support_entities
    // doc: "ExtrusionLoop/MultiPath-ness is not preserved, same pre-existing
    // tradeoff"). Post-v2.5c that tradeoff is gone for the uniform case: the
    // ORIGINAL entity pointer moves whole, so an ExtrusionLoop stays a loop instead
    // of collapsing into a straight ExtrusionPath.
    auto resolver = [](const Point &) -> unsigned { return 2u; }; // every sample votes extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;

    Polygon sq({ Point(scale_(0), scale_(0)), Point(scale_(10), scale_(0)),
                 Point(scale_(10), scale_(10)), Point(scale_(0), scale_(10)) });
    ExtrusionPath src_path(erSupportMaterial, 1.0, 0.4f, 0.2f);
    src_path.polyline = Polyline(sq.points);
    src_path.polyline.points.push_back(sq.points.front());
    auto *loop = new ExtrusionLoop(src_path);
    REQUIRE(loop->is_loop());
    REQUIRE(loop->role() == erSupportMaterial);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(loop);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    CHECK(fills.entities.empty());
    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    CHECK(out.at(2).entities.front() == loop); // moved WHOLE, pointer-stable - no clone, no rebuild
    CHECK(out.at(2).entities.front()->is_loop()); // loop-ness survives (a rebuild would have lost it)
}

TEST_CASE("partition_support_entities (v2.5c root cause): a real painted color that equals the fallback extruder is no longer excluded from matching", "[chameleon]")
{
    // The money-shot regression test for the v2.5c fix. Diagnostic-log-proven root
    // cause (CHAMELEON_DEBUG on the user's own GUI slice): white wall samples were
    // plentiful (194-593/layer in the claw band), fallback_extruder resolved to 0
    // (white, the layer-0 min external-perimeter color), and ZERO e0 buckets formed
    // across all 560 layers even though every gate/trim/redirect mechanism kept
    // every OTHER bucket - because partition_support_entities pre-v2.5c treated a
    // vote == fallback_extruder as definitionally UNMATCHED, so white geometry could
    // never become a bucket no matter how many samples voted it. This fixture
    // reproduces that shape directly: TWO real walls exist, extruder 0 (white, which
    // also happens to be fallback_extruder) and extruder 2 (a different color) -
    // both must become real, separately addressable buckets; neither may fall back
    // to support_fills or be silently absorbed into the other's bucket.
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 14, 6), 0, 1);    // left wall: extruder 0 == fallback_extruder
    idx.add_polyline(segment(16, 6, 30, 6), 2, 2);   // right wall: extruder 2
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    auto fills = make_support_fills(5.0f); // 1 base path @ y=-5, 2 interface paths @ y=5/6, spanning x 0->30
    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterialInterface, 0, resolver, p, out);

    // Both colors bucketed - the fallback-equal color is NOT excluded.
    REQUIRE(out.count(0) == 1);
    CHECK(!out.at(0).entities.empty());
    REQUIRE(out.count(2) == 1);
    CHECK(!out.at(2).entities.empty());

    // Every bucketed path still carries its true source role (never hardcoded),
    // matching the pre-existing role-fidelity contract this fix does not touch.
    for (auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erSupportMaterialInterface);

    // Nothing fell back to support_fills's residual/fallback path: both interface
    // originals were matched away (into a bucket, not left/returned as residual).
    size_t iface_n = 0;
    for (auto *e : fills.entities)
        if (e->role() == erSupportMaterialInterface) ++iface_n;
    CHECK(iface_n == 0);
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
// (matched originals deleted, EVERY run - fallback included, v2.5c - re-inserted
// into its own out[] bucket, pre-split at vote boundaries with a boundary vertex
// borrowed from the adjacent run - see split_polyline_by_vote's continuity fix).
// v2.5c update: pre-v2.5c, only the NON-fallback runs left this collection via
// out[]; fallback runs returned to support_fills, and THIS test fed exactly those
// fallback fragments back in to reproduce the aliasing scenario. Post-v2.5c, ALL
// three runs (fallback included) leave via out[] on pass 1 - support_fills ends
// pass 1 empty - so the fixture below instead feeds EVERY pass-1 bucket's geometry
// back in (mirroring apply_bucket_caps' legacy no-survivor merge-back, which still
// dumps a whole layer's matched geometry back into support_fills when every bucket
// gets gated/trimmed away). Re-running the SAME pass on that already-split output
// re-samples each fragment from its own start: the one borrowed boundary point is a
// single 0mm-long "run" that absorb_short_runs (min_run_mm=2.0 default) always
// folds into its neighbour's majority vote before switch_boundaries is computed -
// so every fragment re-splits to runs.size()==1 and (v2.5c) is now itself a
// pointer-stable MOVE into its own bucket, contributing 0 switches even for the
// fragment that lands in the foreign extruder's bucket.
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
    idx.add_polyline(segment(0, 6, 10, 6), 0, 1);   // zone A: extruder 0 (== fallback_extruder)
    idx.add_polyline(segment(15, 6, 30, 6), 1, 2);  // zone B: foreign extruder 1 (wide -> unambiguous interior)
    idx.add_polyline(segment(35, 6, 45, 6), 0, 3);  // zone C: extruder 0 (== fallback_extruder)
    BrimVoteParams p; p.fallback_extruder = 0;

    auto fills = three_zone_interface_fill();
    std::map<unsigned, ExtrusionEntityCollection> out1;
    const size_t switches1 = partition_support_interfaces(fills, 0, idx, p, out1);

    // Pass 1: three zones -> three runs (0,1,0) -> 2 switch boundaries. v2.5c: ALL
    // three runs now bucket by their own vote - the middle (foreign) run lands in
    // out1[1], and BOTH end runs (zone A, zone C - voting 0, same as
    // fallback_extruder) land in out1[0], not back in support_fills. Nothing
    // returns to support_fills at all.
    CHECK(switches1 == 2);
    REQUIRE(out1.count(1) == 1);
    CHECK(!out1.at(1).entities.empty());
    REQUIRE(out1.count(0) == 1);
    CHECK(out1.at(0).entities.size() == 2); // zone A's run and zone C's run, as separate fragments
    CHECK(fills.entities.empty());          // v2.5c: nothing left behind - every run bucketed

    // Mirror Print.cpp's apply_bucket_caps legacy no-survivor merge-back
    // (BrimFilament.cpp: every gate/trim drop appends into merge_back_target when
    // NOTHING survives the caps that layer) - fold EVERY pass-1 bucket's geometry
    // back into support_fills, leaving it holding all three pre-split runs as
    // separate entities - exactly what either trigger in C1 (aliased shared-object
    // copy, or a stale posSupportMaterial re-run) hands the pass on its second
    // visit to this layer.
    for (auto &kv : out1)
        fills.append(std::move(kv.second.entities));
    REQUIRE(fills.entities.size() == 3);

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

TEST_CASE("split_polyline_by_resolver (LOOP): seam sector counted once when first/last runs share an extruder (C7)", "[chameleon]")
{
    // v2.3 Task 2 (spec C7, root cause 8): a 4-sector ring whose SEAM (the array index
    // where the sample chain wraps from its last sample back to poly.front() for a loop)
    // sits INSIDE one color's angular range, not on a boundary between two colors.
    // Without the C7 merge, the raw run-building pass (which only ever groups
    // CONSECUTIVE same-vote chain samples) sees that one physical sector as TWO separate
    // runs - one at the very start of the sample array, one at the very end - because
    // array wraparound isn't "consecutive" to it. Colors are assigned by angle from the
    // origin, in 90-degree bands offset so each of the ring's four corners sits at the
    // MIDPOINT of its own color's range (never on a boundary, so no sample-precision
    // flakiness): color 0 covers (-45,45], color 1 (45,135], color 2 (135,225], color 3
    // (225,315]. The ring's four corners sit at exactly 0/90/180/270 degrees - color 0's
    // corner (angle 0, poly.front()) is both the FIRST sample and, via the loop closure,
    // the LAST sample too, so color 0 is exactly the seam-straddling sector.
    auto resolver = [](const Point &pt) -> unsigned {
        const double x = unscale<double>(pt.x());
        const double y = unscale<double>(pt.y());
        double deg = std::atan2(y, x) * 180.0 / PI;   // (-180, 180]
        if (deg < 0.0)
            deg += 360.0;                              // normalize to [0, 360)
        return unsigned(std::floor(std::fmod(deg + 45.0, 360.0) / 90.0));
    };

    const double R = 20.0;
    Points ring = { Point(scale_(R), scale_(0)), Point(scale_(0), scale_(R)),
                    Point(scale_(-R), scale_(0)), Point(scale_(0), scale_(-R)) };

    BrimVoteParams p;
    p.min_run_mm = 0.0;   // isolate the C7 merge from absorb_short_runs
    p.max_runs   = 8;     // isolate the C7 merge from guard_max_runs - must not let the
                           // pre-fix 5-run case get trimmed down to 4 by coincidence
    // v2.3 final-review M4 fix: the merge itself is now gated on merge_ring_seam
    // (BrimVoteParams, default false) so Part 1's own brim path - which shares this same
    // split_polyline_core - stays unaffected by the C7 merge. This test exercises the
    // merge MECHANISM directly (split_polyline_by_resolver, not a real brim/support call
    // site), so it opts in explicitly, the same way the support pass's own vote_params
    // does (Print.cpp chameleon_assign_support_interfaces).
    p.merge_ring_seam = true;

    auto runs = split_polyline_by_resolver(ring, /*is_loop=*/true, resolver, p);

    // Must FAIL pre-change: without the C7 merge this is 5 runs (0,1,2,3,0 - color 0
    // counted twice, once at each end of the sample array).
    REQUIRE(runs.size() == 4);
    CHECK(runs[0].extruder == 0);
    CHECK(runs[1].extruder == 1);
    CHECK(runs[2].extruder == 2);
    CHECK(runs[3].extruder == 3);

    // Boundary-vertex invariant holds end to end (the ordinary inter-run gap-fix loop
    // still runs on the post-merge run list).
    for (size_t k = 1; k < runs.size(); ++k) {
        REQUIRE(!runs[k].pts.empty());
        REQUIRE(!runs[k - 1].pts.empty());
        CHECK(runs[k].pts.front() == runs[k - 1].pts.back());
    }

    // The merged seam run really does contain the seam vertex itself (poly.front(),
    // where the pre-merge first and last runs used to join) somewhere in its points -
    // proof the merge actually glued the two color-0 fragments back together, not that
    // guard_max_runs coincidentally discarded one of them down to the same count.
    const bool seam_vertex_present = std::find(runs[0].pts.begin(), runs[0].pts.end(),
                                                Point(scale_(R), scale_(0))) != runs[0].pts.end();
    CHECK(seam_vertex_present);

    // v2.3 final-review I1 fix: the CIRCULAR wrap boundary (runs.back() -> runs.front(),
    // the pair the ordinary k=1.. loop above never visits since k never wraps to 0) must
    // be closed exactly like every other pair - runs.front()'s own first point must equal
    // runs.back()'s own last point. Must FAIL pre-fix: the seam merge buries the chain's
    // shared closing vertex as an interior point of the merged run (see split_polyline_
    // core's own I1 comment), so pre-fix runs.front().pts.front() is still the ORIGINAL
    // last run's own first point - a different, unrelated sample - not runs.back().pts.
    // back(), leaving the wrap segment between them unextruded.
    REQUIRE(!runs.front().pts.empty());
    REQUIRE(!runs.back().pts.empty());
    CHECK(runs.front().pts.front() == runs.back().pts.back());
}

TEST_CASE("seam-merged ring collapsing to ONE run via absorb is still closed (N1)", "[chameleon]")
{
    // v2.3 fix-rereview N1: seam merge fires (first/last runs same extruder), then
    // absorb_short_runs swallows the only minority run (< min_run_mm) so the whole ring
    // collapses to a single run. The merge buried the chain's closing vertex as an
    // interior point, so without the single-run closure the run's endpoints are two
    // merely-adjacent samples - a ~sample_mm unextruded wrap hole. Common for small
    // tree rings that barely cross a color boundary.
    auto resolver = [](const Point &pt) -> unsigned {
        // tiny minority zone: ~1.2mm of arc near (0, R) votes extruder 2, rest 1
        return (std::abs(unscale<double>(pt.x())) < 0.6 && unscale<double>(pt.y()) > 0.0)
                   ? 2u : 1u;
    };
    const double R = 6.0;
    Points ring = { Point(scale_(R), scale_(0)), Point(scale_(0), scale_(R)),
                    Point(scale_(-R), scale_(0)), Point(scale_(0), scale_(-R)) };
    BrimVoteParams p;
    p.min_run_mm      = 2.0;   // > minority arc -> absorb collapses to one run
    p.merge_ring_seam = true;
    auto runs = split_polyline_by_resolver(ring, /*is_loop=*/true, resolver, p);
    REQUIRE(runs.size() == 1);
    CHECK(runs.front().extruder == 1);
    // Closed ring: endpoints coincide (would FAIL pre-fix - adjacent samples instead).
    REQUIRE(runs.front().pts.size() >= 3);
    CHECK(runs.front().pts.front() == runs.front().pts.back());
}

TEST_CASE("split_polyline_by_resolver (LOOP): default merge_ring_seam=false leaves the ring seam UNMERGED (M4: Part 1 brim path stays byte-identical)", "[chameleon]")
{
    // Same 4-sector ring/resolver as the C7 merge test above, but with a DEFAULT
    // BrimVoteParams - merge_ring_seam left at its default false, exactly what Part 1's
    // own brim BrimVoteParams does (Print.cpp's Part 1 brim call site never sets this
    // field). Proves the merge is genuinely OPT-IN, not just opt-in in name: without
    // setting merge_ring_seam, the seam-straddling color-0 sector stays split into its
    // pre-v2.3-final-review two fragments (one at each end of the sample array) - the
    // exact "Part 1 brim behavior stays byte-identical" contract M4 restores.
    auto resolver = [](const Point &pt) -> unsigned {
        const double x = unscale<double>(pt.x());
        const double y = unscale<double>(pt.y());
        double deg = std::atan2(y, x) * 180.0 / PI;
        if (deg < 0.0)
            deg += 360.0;
        return unsigned(std::floor(std::fmod(deg + 45.0, 360.0) / 90.0));
    };

    const double R = 20.0;
    Points ring = { Point(scale_(R), scale_(0)), Point(scale_(0), scale_(R)),
                    Point(scale_(-R), scale_(0)), Point(scale_(0), scale_(-R)) };

    BrimVoteParams p;
    p.min_run_mm = 0.0;
    p.max_runs   = 8;
    // merge_ring_seam left at its default (false) - the point of this test.

    auto runs = split_polyline_by_resolver(ring, /*is_loop=*/true, resolver, p);

    // Unmerged: color 0 still counted twice (once at each array end) -> 5 runs.
    REQUIRE(runs.size() == 5);
    CHECK(runs[0].extruder == 0);
    CHECK(runs[1].extruder == 1);
    CHECK(runs[2].extruder == 2);
    CHECK(runs[3].extruder == 3);
    CHECK(runs[4].extruder == 0);
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

// v2.5a (spec: apply_bucket_caps redirect): same shape as bucket_of_length above (a
// single straight x-axis path of exactly `len_mm`), but placed so its bbox CENTER
// (chameleon_collection_bbox_center - what the redirect's nearest-survivor search
// keys off) lands at exactly (center_x, 0) instead of always being anchored at the
// origin - bucket_of_length's fixed (0,0) start makes its own centroid an
// (uncontrollable) function of len_mm alone, too coupled to be useful for tests that
// need to place a bucket's centroid at a chosen coordinate independent of its length
// (the redirect tests below need both: a specific centroid AND a length that clears
// or misses the gate on its own terms).
static ExtrusionEntityCollection bucket_of_length_at(double len_mm, double center_x)
{
    ExtrusionEntityCollection c;
    auto *p = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    p->polyline = Polyline({Point(scale_(center_x - len_mm / 2.0), scale_(0)),
                             Point(scale_(center_x + len_mm / 2.0), scale_(0))});
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

TEST_CASE("apply_bucket_caps: sub-40mm bucket is dropped by the min-benefit gate, REDIRECTED to the sole survivor (v2.5a)", "[chameleon]")
{
    // v2.5a: a gated bucket is no longer unconditionally merged back to
    // merge_back_target (support_fills) - when at least one survivor exists, its
    // geometry redirects into the nearest surviving bucket instead (here, bucket 1
    // is the only survivor, so it's trivially "nearest"). This is the direct RED/
    // GREEN case the residual-paint fix targets: a dropped bucket that pre-v2.5a
    // code merged to support_fills (residual, subject to flush_into_support
    // repainting) must now land inside a MATCHED bucket instead.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(100.0);
    map[2] = bucket_of_length(39.9);   // just under the 40mm floor
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(map.count(1) == 1);
    CHECK(map.count(2) == 0);                 // gated out of its OWN bucket key...
    CHECK(merge_back.entities.empty());       // ...but a survivor exists, so NOT legacy fallback
    REQUIRE(map.at(1).entities.size() == 2);  // ...redirected into the sole survivor instead
    CHECK(result.kept == std::set<unsigned>{1});
    CHECK(result.buckets_dropped_min_benefit == 1);
    CHECK(result.buckets_trimmed_cap == 0);
    CHECK(result.buckets_redirected == 1);
}

TEST_CASE("apply_bucket_caps: 3-extruder partition trims to the 2 longest, trimmed bucket REDIRECTS to its nearest-centroid survivor (v2.5a)", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(50.0);   // trimmed away; bbox center x=25
    map[2] = bucket_of_length(100.0);  // survives; bbox center x=50 (dist from 25: 25)
    map[3] = bucket_of_length(75.0);   // survives; bbox center x=37.5 (dist from 25: 12.5 - nearer)
    std::set<unsigned> prev_kept;             // no hysteresis pressure - pure length rank
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    // Longest two survive (2: 100mm, 3: 75mm); shortest (1: 50mm) is trimmed - same
    // kept set as before v2.5a (the redirect never changes WHICH buckets survive).
    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 1);
    // v2.5a: redirected to bucket 3 (nearest bbox-center survivor), NOT merged back
    // to support_fills - bucket 2 is a survivor too but farther away, so it must be
    // left untouched by the redirect.
    CHECK(merge_back.entities.empty());
    CHECK(map.at(2).entities.size() == 1);
    REQUIRE(map.at(3).entities.size() == 2);
    CHECK(result.kept == std::set<unsigned>{2, 3});
    CHECK(result.buckets_dropped_min_benefit == 0);
    CHECK(result.buckets_trimmed_cap == 1);
    CHECK(result.buckets_redirected == 1);
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

// --- v2.5a: apply_bucket_caps redirect (residual-paint fix, spec item 2) ---------

TEST_CASE("apply_bucket_caps: no survivors -> legacy merge_back_target fallback, byte-identical to pre-redirect behavior", "[chameleon]")
{
    // Sole bucket fails the gate; nothing survives at all. Map empty -> legacy
    // path: the dropped geometry lands in merge_back_target exactly like every
    // pre-v2.5a call, and buckets_redirected stays 0 (there is no survivor to
    // redirect to - the residual pin (item 1) owns this layer's fallback color).
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(5.0);   // well under the 40mm floor
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(map.empty());
    CHECK(result.kept.empty());
    REQUIRE(merge_back.entities.size() == 1);
    CHECK(result.buckets_redirected == 0);
    CHECK(result.buckets_dropped_min_benefit == 1);
}

TEST_CASE("apply_bucket_caps: ALL buckets gated (no survivors) -> every one legacy-falls-back, ascending order preserved", "[chameleon]")
{
    // Two buckets, BOTH fail the gate -> kept ends empty (map.size() > max_extruders
    // is never true once map is already empty, so the trim never even runs) - both
    // buckets' geometry must land in merge_back_target, same as any pre-v2.5a call
    // with these inputs, and in the SAME order the old gate loop's own ascending
    // std::map iteration always appended them in.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(5.0);
    map[2] = bucket_of_length(3.0);
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(map.empty());
    CHECK(result.kept.empty());
    CHECK(merge_back.entities.size() == 2);
    CHECK(result.buckets_redirected == 0);
    CHECK(result.buckets_dropped_min_benefit == 2);
}

TEST_CASE("apply_bucket_caps: survivor centroids are snapshotted BEFORE any redirect append (order-independence)", "[chameleon]")
{
    // Two survivors (extruders 1 and 2) far apart on the x-axis; two dropped
    // buckets (extruders 3 and 4, processed in that ascending order - both fail
    // the gate). D1 (centroid x=400) is unambiguously nearest survivor 1 (centroid
    // x=0, dist 400) over survivor 2 (centroid x=1000, dist 600) under EITHER a
    // correct or a buggy implementation - it redirects first.
    //
    // D2 (centroid x=550) is the discriminating case: measured against survivor 1's
    // ORIGINAL centroid (x=0, dist 550) vs survivor 2 (x=1000, dist 450), survivor 2
    // is nearer - the spec-mandated, snapshot-based answer. But survivor 1's bucket,
    // AFTER absorbing D1, has a bbox spanning [-25, 402.5] -> a LIVE bbox center of
    // 188.75; measured against that shifted value (dist 361.25) vs survivor 2 (dist
    // 450), survivor 1 would appear nearer instead - the wrong answer a
    // recompute-after-each-append implementation would produce. This proves the
    // survivor centroid snapshot is taken once, before ANY redirect append, exactly
    // as the design requires ("order-independence").
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length_at(50.0, 0.0);      // survivor, bbox center x=0
    map[2] = bucket_of_length_at(50.0, 1000.0);   // survivor, bbox center x=1000
    map[3] = bucket_of_length_at(5.0, 400.0);     // dropped (gate), centroid x=400
    map[4] = bucket_of_length_at(5.0, 550.0);     // dropped (gate), centroid x=550
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, 40.0, merge_back);

    CHECK(result.kept == std::set<unsigned>{1, 2});
    CHECK(merge_back.entities.empty());
    REQUIRE(map.at(1).entities.size() == 2);   // survivor 1: its own path + D1
    REQUIRE(map.at(2).entities.size() == 2);   // survivor 2: its own path + D2
    CHECK(result.buckets_redirected == 2);
}

TEST_CASE("apply_bucket_caps: centroid tie-break prefers prev_kept DESC over the lower extruder id", "[chameleon]")
{
    // Two survivors symmetric around the dropped bucket's centroid (x=-10 and
    // x=+10 around a dropped bucket centered at x=0) - an EXACT tie on squared
    // centroid distance. Extruder 5 (the higher id) is in prev_kept; extruder 3
    // is not - prev_kept membership must outrank the extruder-id tie-break.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[5] = bucket_of_length_at(50.0, -10.0);  // survivor, in prev_kept
    map[3] = bucket_of_length_at(50.0, 10.0);   // survivor, NOT in prev_kept
    map[9] = bucket_of_length_at(5.0, 0.0);     // dropped (gate), equidistant from both
    std::set<unsigned> prev_kept{5};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 3, 40.0, merge_back);

    CHECK(result.kept == std::set<unsigned>{5, 3});
    CHECK(merge_back.entities.empty());
    CHECK(map.at(5).entities.size() == 2);   // tie broken TO extruder 5 (prev_kept)
    CHECK(map.at(3).entities.size() == 1);   // extruder 3 untouched despite the lower id
    CHECK(result.buckets_redirected == 1);
}

TEST_CASE("apply_bucket_caps: centroid tie-break falls to extruder ASC when prev_kept doesn't distinguish", "[chameleon]")
{
    // Same symmetric-tie geometry as above, but prev_kept is empty this time -
    // neither survivor has seniority, so the tie-break falls through to the final,
    // deterministic rule: lower extruder id wins (3, not 5).
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[5] = bucket_of_length_at(50.0, -10.0);
    map[3] = bucket_of_length_at(50.0, 10.0);
    map[9] = bucket_of_length_at(5.0, 0.0);
    std::set<unsigned> prev_kept;   // no seniority either way
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 3, 40.0, merge_back);

    CHECK(result.kept == std::set<unsigned>{5, 3});
    CHECK(merge_back.entities.empty());
    CHECK(map.at(3).entities.size() == 2);   // tie broken to the LOWER extruder id
    CHECK(map.at(5).entities.size() == 1);
    CHECK(result.buckets_redirected == 1);
}

TEST_CASE("apply_bucket_caps: redirect target is independent of map insertion order (permuted-insertion determinism)", "[chameleon]")
{
    // Same logical bucket set (by key and geometry), built via two different C++
    // insertion sequences. std::map is key-ordered regardless of insertion order,
    // so this is a regression guard against an implementation detail (e.g. an
    // insertion-order-sensitive intermediate container) accidentally leaking
    // through and making the redirect outcome depend on something it must not.
    auto build_ascending = [] {
        std::map<unsigned, ExtrusionEntityCollection> m;
        m[1] = bucket_of_length_at(50.0, 0.0);
        m[2] = bucket_of_length_at(50.0, 1000.0);
        m[3] = bucket_of_length_at(5.0, 400.0);
        return m;
    };
    auto build_descending = [] {
        std::map<unsigned, ExtrusionEntityCollection> m;
        m[3] = bucket_of_length_at(5.0, 400.0);
        m[2] = bucket_of_length_at(50.0, 1000.0);
        m[1] = bucket_of_length_at(50.0, 0.0);
        return m;
    };

    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back_a, merge_back_b;
    std::map<unsigned, ExtrusionEntityCollection> map_a = build_ascending();
    std::map<unsigned, ExtrusionEntityCollection> map_b = build_descending();

    BucketCapResult result_a = apply_bucket_caps(map_a, prev_kept, 2, 40.0, merge_back_a);
    BucketCapResult result_b = apply_bucket_caps(map_b, prev_kept, 2, 40.0, merge_back_b);

    CHECK(result_a.kept == result_b.kept);
    CHECK(result_a.buckets_redirected == result_b.buckets_redirected);
    CHECK(merge_back_a.entities.empty());
    CHECK(merge_back_b.entities.empty());
    // Same survivor (extruder 1, nearest to the dropped bucket 3 at x=400) absorbed
    // the redirect in both insertion orders.
    CHECK(map_a.at(1).entities.size() == map_b.at(1).entities.size());
    CHECK(map_a.at(1).entities.size() == 2);
    CHECK(map_a.at(2).entities.size() == map_b.at(2).entities.size());
    CHECK(map_a.at(2).entities.size() == 1);
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

// --- v2.5a Task 2b: mechanism-B pin (residual-paint fix, dominant matched extruder) --

TEST_CASE("chameleon_dominant_matched_extruder: empty map returns -1 (nothing to pin to)", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    CHECK(chameleon_dominant_matched_extruder(buckets) == -1);
}

TEST_CASE("chameleon_dominant_matched_extruder: a map of only empty buckets also returns -1", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    buckets[1] = ExtrusionEntityCollection();
    buckets[2] = ExtrusionEntityCollection();
    CHECK(chameleon_dominant_matched_extruder(buckets) == -1);
}

TEST_CASE("chameleon_dominant_matched_extruder: single non-empty bucket wins trivially", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    buckets[7] = bucket_of_length(10.0);
    CHECK(chameleon_dominant_matched_extruder(buckets) == 7);
}

TEST_CASE("chameleon_dominant_matched_extruder: the bucket with the LARGEST total path length wins", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    buckets[1] = bucket_of_length(20.0);
    buckets[2] = bucket_of_length(80.0);   // dominant
    buckets[3] = bucket_of_length(50.0);
    CHECK(chameleon_dominant_matched_extruder(buckets) == 2);
}

TEST_CASE("chameleon_dominant_matched_extruder: exact-length tie breaks to the LOWEST extruder id", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    buckets[5] = bucket_of_length(30.0);
    buckets[2] = bucket_of_length(30.0);   // same length, lower id -> wins
    buckets[9] = bucket_of_length(30.0);
    CHECK(chameleon_dominant_matched_extruder(buckets) == 2);
}

TEST_CASE("chameleon_dominant_matched_extruder: an empty bucket never outranks a non-empty one, regardless of key order", "[chameleon]")
{
    std::map<unsigned, ExtrusionEntityCollection> buckets;
    buckets[1] = ExtrusionEntityCollection();  // empty - never a candidate
    buckets[8] = bucket_of_length(1.0);        // tiny, but the only real candidate
    CHECK(chameleon_dominant_matched_extruder(buckets) == 8);
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

TEST_CASE("chameleon_pick_projection_region: a lower layer's margin-ring hit must NOT pre-empt a higher layer's genuine raw containment (C1)", "[chameleon]")
{
    // v2.2 final-review C1: the standard-configuration failure scenario. Band layer 0
    // is the wall-only z-gap layer (its raw lslices only see the laterally-adjacent
    // wall, not the overhang above it yet) - p sits inside layer 0's margin ring but
    // NOT its raw lslices. Band layer 1 is one layer higher and its raw lslices DO
    // genuinely contain p (the overhang body itself). "SURFACE ABOVE WINS" requires
    // layer 1's raw containment to resolve this sample, regardless of layer 0's ring
    // hit underneath it. The single-pass v2.1/pre-fix code returns from layer 0 (ring
    // hit -> nearest_region_to_point) and never reaches layer 1 at all - this is the RED
    // case the two-pass restructure (PASS 1 raw-only over ALL layers, PASS 2 ring-only)
    // fixes.
    Point p(0, 0);

    // Layer 0 (lowest, wall-only z-gap layer): raw lslices are the wall, offset away
    // from p; the margin ring (expanded_lslices) grows just far enough to cover p.
    ExPolygons layer0_raw      = { square_expoly(20, 0, 5) };   // spans x in [15,25] - does NOT cover p
    ExPolygons layer0_expanded = { square_expoly(0, 0, 15) };   // spans x in [-15,15] - covers p (ring)
    ExPolygons layer0_region0  = { square_expoly(20, 0, 5) };   // the wall region's own raw slice - same as layer0_raw, doesn't cover p either

    ProjectionLayerView layer0;
    layer0.lslices            = &layer0_raw;
    layer0.expanded_lslices   = layer0_expanded;
    layer0.region_slice_polys = { { &layer0_region0[0] } };

    // Layer 1 (one layer higher, the overhang body): raw lslices genuinely contain p.
    ExPolygons layer1_raw     = { square_expoly(0, 0, 10) };    // covers p directly
    ExPolygons layer1_region0 = { square_expoly(0, 0, 10) };    // the overhang region's own raw slice - also covers p

    ProjectionLayerView layer1;
    layer1.lslices            = &layer1_raw;
    layer1.region_slice_polys = { { &layer1_region0[0] } };

    std::vector<ProjectionLayerView> layers = { layer0, layer1 };
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 1);   // layer 1's raw containment, NOT layer 0's ring hit
    CHECK(out_region == 0);
}

TEST_CASE("chameleon_pick_projection_region: PASS 2 scans the margin ring HIGHEST band layer first (C4)", "[chameleon]")
{
    // v2.3 Task 2 (spec C4, root cause 4): unlike the C1 test above (where layer 1
    // genuinely raw-contains p, so PASS 1 alone already resolves it), THIS case makes
    // BOTH layers miss on raw containment - p is only ever covered by either layer's
    // margin ring - so PASS 1 finds nothing on either layer and this exercises PASS 2 in
    // isolation. Layer 0 is the wall-only z-gap layer, layer 1 is the overhang one layer
    // higher - the standard-configuration shape "surface above wins" is meant to
    // restore. Pre-change (PASS 2 scanning lowest-first, the same direction as PASS 1)
    // this resolves to layer 0's wall region - the exact inversion spec root cause 4
    // describes ("the 1.2mm-grown contact rim resolves against the adjacent WALL layer
    // below the overhang layer"). Post-change (PASS 2 scans highest-first) it must
    // resolve to layer 1's overhang region instead.
    Point p(0, 0);

    ExPolygons layer0_raw      = { square_expoly(20, 0, 5) };   // wall geometry - doesn't cover p
    ExPolygons layer0_expanded = { square_expoly(0, 0, 15) };   // grown ring - covers p
    ExPolygons layer0_region0  = { square_expoly(20, 0, 5) };   // wall region's own raw slice, far from p

    ProjectionLayerView layer0;
    layer0.lslices            = &layer0_raw;
    layer0.expanded_lslices   = layer0_expanded;
    layer0.region_slice_polys = { { &layer0_region0[0] } };

    ExPolygons layer1_raw      = { square_expoly(20, 0, 5) };   // overhang's raw slice also just misses p
    ExPolygons layer1_expanded = { square_expoly(0, 0, 15) };   // grown ring - covers p
    ExPolygons layer1_region0  = { square_expoly(3, 0, 1) };    // overhang region's own raw slice, near p

    ProjectionLayerView layer1;
    layer1.lslices            = &layer1_raw;
    layer1.expanded_lslices   = layer1_expanded;
    layer1.region_slice_polys = { { &layer1_region0[0] } };

    std::vector<ProjectionLayerView> layers = { layer0, layer1 };  // caller-ordered lowest first, as always
    size_t out_layer = 999, out_region = 999;
    REQUIRE(chameleon_pick_projection_region(layers, p, out_layer, out_region));
    CHECK(out_layer == 1);   // overhang layer, NOT the lower wall layer's ring hit
    CHECK(out_region == 0);
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

TEST_CASE("partition_support_entities (C7/v2.5c): a role-eligible nested collection that votes fallback now moves whole into out[fallback], pointer-stable", "[chameleon]")
{
    // v2.5c root cause fix: this test used to be named "...stays in place,
    // untouched" and asserted `out.empty()` / the collection pointer left inside
    // `fills` - the SAME fallback-exclusion collision the top-level leaf fast path
    // had (BrimFilament.hpp's own v2.5c note), just at the whole-collection level.
    // Re-pointed at the new contract: "uniform anything moves whole into its
    // bucket" (item 1 of the fix) applies to a whole-collection vote too, not just
    // leaves - a fallback-majority collection now moves, pointer-stable, into
    // out[fallback_extruder], exactly like a non-fallback-majority collection
    // already did pre-v2.5c (see the sibling "moved whole, never split" test
    // above).
    WallSampleIndex idx; // empty index -> brim_vote always returns fallback_extruder
    BrimVoteParams p; p.fallback_extruder = 0;
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    ExtrusionEntityCollection *inner = nested_base_collection(6.0, /*no_sort=*/false);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    CHECK(fills.entities.empty()); // moved out of support_fills entirely
    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == inner); // moved WHOLE - same pointer, never split or cloned
    CHECK(static_cast<ExtrusionEntityCollection *>(out.at(0).entities.front())->entities.size() == 2);
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
    // v2.3 Task 3 (spec C5): this fixture's "losing" side of the tie is ALSO, by
    // construction, a genuine ~11.2mm contiguous minority run - exactly root cause 5's
    // motivating shape ("Tree rings spanning sectors: whole-collection votes paint the
    // minority arc the majority color"), which C5 now correctly DESCENDS and splits
    // instead of painting both sectors the tie-winner's color (see the dedicated C5
    // "mixed collection... DESCENDS" test above for that corrected behavior). This test
    // predates C5 and exists to check vote_collection_as_unit's TIE-BREAK itself (the
    // lowest-id winner selection feeding the (a) "uniform/dominant... whole-move" path C5
    // still uses below its own threshold) - min_run_mm is set far above the fixture's
    // ~11.2mm run so this fixture stays on that whole-move path and isolates the
    // tie-break from C5's new descend decision.
    p.min_run_mm = 100.0;
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

// --- v2.2 Task 4 (spec C8): nearest_wall mode ------------------------------

TEST_CASE("union_layer_indices dedupes the overlap between two ascending index lists", "[chameleon]")
{
    // The realistic shape: contact-band indices and coplanar-span indices are each
    // individually ascending/duplicate-free (their own selectors guarantee that), but
    // the two lists commonly share layers (a support layer's coplanar span usually
    // sits inside or beside its 2mm-wide contact band) - the union must count each
    // shared layer's walls exactly once.
    std::vector<size_t> contact  = {2, 3, 4};
    std::vector<size_t> coplanar = {1, 2};
    auto result = union_layer_indices(contact, coplanar);
    REQUIRE(result.size() == 4);
    CHECK(result[0] == 1);
    CHECK(result[1] == 2);
    CHECK(result[2] == 3);
    CHECK(result[3] == 4);
}

TEST_CASE("union_layer_indices: fully disjoint lists just merge, ascending", "[chameleon]")
{
    std::vector<size_t> a = {5, 7};
    std::vector<size_t> b = {1, 3};
    auto result = union_layer_indices(a, b);
    REQUIRE(result.size() == 4);
    CHECK(result[0] == 1);
    CHECK(result[1] == 3);
    CHECK(result[2] == 5);
    CHECK(result[3] == 7);
}

TEST_CASE("union_layer_indices: identical lists collapse to one copy each", "[chameleon]")
{
    std::vector<size_t> a = {0, 1, 2};
    auto result = union_layer_indices(a, a);
    REQUIRE(result.size() == 3);
    CHECK(result[0] == 0);
    CHECK(result[1] == 1);
    CHECK(result[2] == 2);
}

TEST_CASE("union_layer_indices: either side empty returns the other side, deduped", "[chameleon]")
{
    std::vector<size_t> empty;
    std::vector<size_t> some = {4, 4, 2}; // caller-side duplicate, unsorted - documents no assumption
    CHECK(union_layer_indices(empty, empty).empty());
    auto r1 = union_layer_indices(empty, some);
    REQUIRE(r1.size() == 2);
    CHECK(r1[0] == 2);
    CHECK(r1[1] == 4);
    auto r2 = union_layer_indices(some, empty);
    REQUIRE(r2.size() == 2);
    CHECK(r2[0] == 2);
    CHECK(r2[1] == 4);
}

TEST_CASE("brim_vote uncapped (max_dist_mm=0) picks the nearest wall regardless of distance (C8 nearest_wall mode)", "[chameleon]")
{
    // C8: nearest_wall's resolver is brim_vote with max_dist_mm left at 0 (uncapped) -
    // unlike nearest_surface's gap-aware lateral cap (C4), a wall far beyond any
    // physically-plausible gap must still win outright, never fall back.
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 500, 10, 500), 3, 1); // 500mm away - no real cap would admit this
    BrimVoteParams p;
    p.max_dist_mm = 0.0;
    p.fallback_extruder = 9;
    CHECK(brim_vote(idx, Point(scale_(5), scale_(0)), p) == 3);
}

TEST_CASE("brim_vote k=1 makes uncapped mode a literal nearest-wall pick, immune to the near-tie min-extruder-id pathology (I1 fix)", "[chameleon]")
{
    // v2.2 final-review I1: nearest_wall's decision rule is "nearest wall segment wins
    // outright, no projection" (spec C8) - a single winner, not a vote. The electorate:
    // one sample of extruder 9 strictly nearest to p (1.0mm), and two samples of
    // extruder 3 both farther (1.2mm each) - deliberately given the LOWER extruder id
    // despite being farther, since brim_vote's tie-break chain bottoms out at
    // std::min(winner_ext, runner_ext) (BrimFilament.cpp, after the object_area
    // tie-break - both empty here). At k=3 (vote_params' default, what
    // interface_wall_params/base_wall_params left k at before this fix) all three
    // samples enter the knn electorate: extruder 3's two farther samples score
    // 2 * 1/1.2^2 ~= 1.389 vs extruder 9's one nearer sample at 1/1.0^2 = 1.0 - within
    // brim_vote's tie_dist_mm(0.3mm) of each other (|1.2-1.0| = 0.2mm), so the near-tie
    // path fires regardless of which raw score is higher, and - with no object_area data
    // for either extruder - falls straight to the LOWEST extruder id, returning
    // extruder 3, the FARTHER wall. This is exactly what the spec/I1 forensics describe:
    // "two samples of a farther wall outvote one sample of the strictly nearest wall...
    // near-ties resolve to the LOWEST extruder id." At k=1 (the fix) the electorate is
    // exactly one sample - the nearest one - so brim_vote's score map has a single entry
    // and returns extruder 9 directly, before the tie-break chain (or the id ordering
    // that poisons it) ever runs.
    WallSampleIndex idx;
    idx.add_polyline(segment(1.2, 0, 1.2, 0), 3, 1);    // wall 3, sample 1, farther (1.2mm) - LOWER id despite being farther
    idx.add_polyline(segment(-1.2, 0, -1.2, 0), 3, 1);  // wall 3, sample 2, farther (1.2mm)
    idx.add_polyline(segment(1.0, 0, 1.0, 0), 9, 2);    // wall 9, single sample, strictly nearest (1.0mm) - HIGHER id

    const Point p(scale_(0), scale_(0));

    SECTION("k=3 (pre-fix default): the near-tie min-extruder-id pathology reproduces")
    {
        BrimVoteParams params;
        params.k = 3;
        params.max_dist_mm = 0.0;
        params.fallback_extruder = 99;
        CHECK(brim_vote(idx, p, params) == 3); // WRONG per "nearest wins outright" - the bug I1 fixes
    }

    SECTION("k=1 (I1 fix, the params nearest_wall mode now actually uses): the literal nearest wall always wins")
    {
        BrimVoteParams params;
        params.k = 1;
        params.max_dist_mm = 0.0;
        params.fallback_extruder = 99;
        CHECK(brim_vote(idx, p, params) == 9); // correct: wall 9 is strictly nearest
    }
}

// --- v2.3 Task 1: gate tiers + hysteresis (spec C1/C2/C3/C6) ---------------

TEST_CASE("apply_bucket_caps: free-tier gate admits a bucket the normal 12mm floor would drop", "[chameleon]")
{
    // 5mm bucket: under the normal-tier floor (12mm) but over the free-tier floor
    // (3mm). Two otherwise-identical buckets, only one's extruder is in free_extruders.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(5.0); // NOT free -> gated at 12mm
    map[2] = bucket_of_length(5.0); // free -> survives at 3mm
    std::set<unsigned> prev_kept;
    std::set<unsigned> free_extruders{2};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, /*min_len_mm=*/12.0, merge_back,
                                                free_extruders, /*min_len_free_mm=*/3.0);

    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(result.kept == std::set<unsigned>{2});
    CHECK(result.buckets_dropped_min_benefit == 1);
}

TEST_CASE("apply_bucket_caps (v2.4 spec C): buckets_dropped_min_benefit_free counts only the FREE-tier subset of the total drops", "[chameleon]")
{
    // Three buckets: #1 (5mm, NOT free) drops under the normal 12mm floor; #2 (2mm,
    // free) drops under the free-tier 3mm floor; #3 (20mm, free) survives both floors.
    // Total drops = 2 (buckets_dropped_min_benefit), but only ONE of those two was a
    // free-tier drop (buckets_dropped_min_benefit_free) - the split must not conflate
    // "dropped" with "dropped because it was free".
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(5.0);  // NOT free -> dropped, normal tier
    map[2] = bucket_of_length(2.0);  // free -> dropped, free tier
    map[3] = bucket_of_length(20.0); // free -> survives
    std::set<unsigned> prev_kept;
    std::set<unsigned> free_extruders{2, 3};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, /*min_len_mm=*/12.0, merge_back,
                                                free_extruders, /*min_len_free_mm=*/3.0);

    CHECK(result.buckets_dropped_min_benefit == 2);
    CHECK(result.buckets_dropped_min_benefit_free == 1);
    CHECK(result.kept == std::set<unsigned>{3});
}

TEST_CASE("apply_bucket_caps: default call (no free_extruders passed) behaves exactly like the pre-v2.3 flat gate", "[chameleon]")
{
    // Same 5mm bucket as above, but via the trailing-defaulted call signature (no
    // free_extruders/min_len_free_mm argument at all) - proves existing call sites
    // (this function's own pre-v2.3 tests included) keep compiling AND keep the exact
    // same behavior: nothing is ever "free" by default, so the normal 12mm floor gates
    // a 5mm bucket regardless of extruder id.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[2] = bucket_of_length(5.0);
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, /*min_len_mm=*/12.0, merge_back);

    CHECK(map.count(2) == 0);
    CHECK(result.kept.empty());
    CHECK(result.buckets_dropped_min_benefit == 1);
}

TEST_CASE("apply_bucket_caps: prev_kept halves the effective gate threshold (C2)", "[chameleon]")
{
    // 8mm bucket: under the normal 12mm floor, but over half of it (6mm) - passes the
    // gate ONLY when its extruder is in prev_kept (0.5x eff_min).
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(8.0); // no seniority -> gated
    map[2] = bucket_of_length(8.0); // prev_kept -> half-gate (6mm) admits it
    std::set<unsigned> prev_kept{2};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, /*min_len_mm=*/12.0, merge_back);

    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(result.kept == std::set<unsigned>{2});
    CHECK(result.buckets_dropped_min_benefit == 1);
}

TEST_CASE("apply_bucket_caps: free-tier and prev_kept stack (half of the FREE floor, not the normal one)", "[chameleon]")
{
    // 1.6mm bucket: under the free-tier floor (3mm) but over HALF of it (1.5mm). Only
    // passes when its extruder is BOTH free AND prev_kept - proves the two preferences
    // compose (halve whichever tier's floor free-set membership already selected)
    // rather than either one alone deciding the outcome.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(1.6); // free but no seniority -> gated at 3mm
    map[2] = bucket_of_length(1.6); // free AND prev_kept -> half of 3mm = 1.5mm admits it
    std::set<unsigned> prev_kept{2};
    std::set<unsigned> free_extruders{1, 2};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, 2, /*min_len_mm=*/12.0, merge_back,
                                                free_extruders, /*min_len_free_mm=*/3.0);

    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(result.kept == std::set<unsigned>{2});
}

// --- v2.5b: free-extruder trim exemption (spec: "exempt free extruders from the
// count - they cost no toolchange, which is what C1 prices") ---------------------

TEST_CASE("apply_bucket_caps (v2.5b): a strict-free bucket is exempt from the trim and survives alongside two buckets that fill the budget", "[chameleon]")
{
    // Reproduces the reported symptom directly: a small white claw bucket (8mm)
    // whose extruder is STRICT-free at this layer (white prints model geometry AT
    // this exact z somewhere on the plate - see Print.cpp's free_extruders_exempt,
    // computed with up_mm=0.0) must survive the trim even though two larger,
    // unrelated buckets (teal 60mm, khaki 50mm) already fill the max_extruders=2
    // budget on length alone. Pre-v2.5b, white ranks last (shortest) and gets
    // trimmed - then v2.5a's redirect sends its geometry to the nearest survivor
    // (teal), which is exactly the "claws wrap in teal" symptom this task fixes.
    // min_len_mm=0.0 so the gate (step a) never touches any of the three - this
    // isolates the trim (step b) exemption being tested here.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(60.0); // teal
    map[2] = bucket_of_length(50.0); // khaki
    map[3] = bucket_of_length(8.0);  // white claw - small, but exempt
    std::set<unsigned> prev_kept;
    std::set<unsigned> free_extruders_exempt{3};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, /*max_extruders=*/2,
        /*min_len_mm=*/0.0, merge_back, /*free_extruders=*/{}, /*min_len_free_mm=*/0.0,
        free_extruders_exempt);

    // All three survive - white never enters the ranked trim competition at all, so
    // it never consumes (or costs teal/khaki) one of the two max_extruders slots.
    CHECK(map.count(1) == 1);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 1);
    CHECK(merge_back.entities.empty());
    CHECK(result.kept == std::set<unsigned>{1, 2, 3});
    CHECK(result.buckets_trimmed_cap == 0);
    CHECK(result.buckets_redirected == 0);
    CHECK(result.buckets_exempt_kept == 1);
}

TEST_CASE("apply_bucket_caps (v2.5b): a bucket free only via the up-window (NOT strict-coincident) is not exempt - still competes in the trim and loses", "[chameleon]")
{
    // Same 3 buckets as the exemption test above, but free_extruders_exempt is left
    // EMPTY this time - documents the distinction this task's correctness rests on:
    // Print.cpp deliberately computes free_extruders_exempt from the STRICT
    // (up_mm=0.0) query, never the WINDOWED (up_mm=kContactBandMm) one `free_extruders`
    // uses for the gate's tier selection - an extruder that is only "free" via the
    // up-window (its wall exists on a HIGHER object layer inside the contact band,
    // not at this exact z) must never be passed as free_extruders_exempt, because
    // registering that bucket's geometry into THIS layer's own tool order would add a
    // genuinely NEW toolchange, not a free one (see apply_bucket_caps' own .hpp doc
    // comment for the ToolOrdering.cpp citation). A caller that wrongly reused the
    // windowed set here would not be caught by the exemption test above (both sets
    // contain white there) - this is the case that would catch it: white (8mm) still
    // ranks last on pure length and is trimmed away, exactly the pre-v2.5b outcome.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(60.0); // teal
    map[2] = bucket_of_length(50.0); // khaki
    map[3] = bucket_of_length(8.0);  // white claw - free only via the up-window
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    // free_extruders_exempt intentionally omitted (defaults empty).
    BucketCapResult result = apply_bucket_caps(map, prev_kept, /*max_extruders=*/2,
        /*min_len_mm=*/0.0, merge_back);

    CHECK(map.count(1) == 1);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 0); // trimmed away - never exempt
    CHECK(result.kept == std::set<unsigned>{1, 2});
    CHECK(result.buckets_trimmed_cap == 1);
    CHECK(result.buckets_exempt_kept == 0);
}

TEST_CASE("apply_bucket_caps (v2.5b): 4 non-exempt buckets still trim to 2 - default free_extruders_exempt is a no-op", "[chameleon]")
{
    // No bucket's extruder is ever in free_extruders_exempt (left at its default,
    // empty) - proves the v2.5b signature addition doesn't perturb the pre-existing
    // trim ranking for the common (nothing exempt) case, mirroring the
    // free_extruders/min_len_free_mm precedent (v2.3 Task 1) this same file already
    // established for the gate.
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(10.0);
    map[2] = bucket_of_length(80.0);
    map[3] = bucket_of_length(60.0);
    map[4] = bucket_of_length(40.0);
    std::set<unsigned> prev_kept;
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, /*max_extruders=*/2,
        /*min_len_mm=*/0.0, merge_back);

    // Longest two survive (2: 80mm, 3: 60mm); unchanged from pre-v2.5b ranking.
    CHECK(map.count(1) == 0);
    CHECK(map.count(2) == 1);
    CHECK(map.count(3) == 1);
    CHECK(map.count(4) == 0);
    CHECK(result.kept == std::set<unsigned>{2, 3});
    CHECK(result.buckets_trimmed_cap == 2);
    CHECK(result.buckets_exempt_kept == 0);
}

TEST_CASE("apply_bucket_caps (v2.5b): buckets_exempt_kept stays 0 when the trim never runs at all (map already at/under budget)", "[chameleon]")
{
    // map.size() == max_extruders here, so the trim's own partition loop (the only
    // place buckets_exempt_kept is incremented) never executes - an exempt bucket
    // that would have survived anyway is still "kept", just not counted here (see
    // BucketCapResult::buckets_exempt_kept's own doc comment for why this is
    // intentional, not an under-count bug).
    std::map<unsigned, ExtrusionEntityCollection> map;
    map[1] = bucket_of_length(60.0);
    map[2] = bucket_of_length(8.0); // exempt, but the budget is already satisfied
    std::set<unsigned> prev_kept;
    std::set<unsigned> free_extruders_exempt{2};
    ExtrusionEntityCollection merge_back;

    BucketCapResult result = apply_bucket_caps(map, prev_kept, /*max_extruders=*/2,
        /*min_len_mm=*/0.0, merge_back, /*free_extruders=*/{}, /*min_len_free_mm=*/0.0,
        free_extruders_exempt);

    CHECK(result.kept == std::set<unsigned>{1, 2});
    CHECK(result.buckets_exempt_kept == 0);
}

TEST_CASE("chameleon_update_prev_kept: a real commit becomes prev_kept outright and resets the retention grace", "[chameleon]")
{
    PrevKeptState state{ std::set<unsigned>{7}, /*retained_last_layer=*/true };
    PrevKeptState next = chameleon_update_prev_kept(state, std::set<unsigned>{1, 2}, /*had_buckets_pre_gate=*/true);
    CHECK(next.prev_kept == std::set<unsigned>{1, 2});
    CHECK(next.retained_last_layer == false);
}

TEST_CASE("chameleon_update_prev_kept: buckets existed pre-gate but all were gated away -> retains prev_kept for ONE layer (C2)", "[chameleon]")
{
    PrevKeptState state{ std::set<unsigned>{5}, /*retained_last_layer=*/false };
    PrevKeptState next = chameleon_update_prev_kept(state, /*committed=*/{}, /*had_buckets_pre_gate=*/true);
    CHECK(next.prev_kept == std::set<unsigned>{5}); // retained, not erased
    CHECK(next.retained_last_layer == true);        // grace spent
}

TEST_CASE("chameleon_update_prev_kept: a SECOND consecutive all-gated layer decays to empty (grace already spent)", "[chameleon]")
{
    // Same as above, but the grace was already spent on the immediately preceding
    // layer - this is the "counted decay, not indefinite" half of spec C2.
    PrevKeptState state{ std::set<unsigned>{5}, /*retained_last_layer=*/true };
    PrevKeptState next = chameleon_update_prev_kept(state, /*committed=*/{}, /*had_buckets_pre_gate=*/true);
    CHECK(next.prev_kept.empty());
    CHECK(next.retained_last_layer == false);
}

TEST_CASE("chameleon_update_prev_kept: no buckets existed pre-gate at all (uniform fallback) clears to empty even with an unspent grace", "[chameleon]")
{
    // Distinct from the "gated away" case: nothing was ever a candidate this layer (the
    // uniform-fallback fast path), so there is nothing to retain memory OF - this must
    // NOT consume/preserve the grace the way a genuine gate-away does.
    PrevKeptState state{ std::set<unsigned>{5}, /*retained_last_layer=*/false };
    PrevKeptState next = chameleon_update_prev_kept(state, /*committed=*/{}, /*had_buckets_pre_gate=*/false);
    CHECK(next.prev_kept.empty());
    CHECK(next.retained_last_layer == false);
}

TEST_CASE("build_layer_filament_table: EPSILON-merges nearby z samples, unions their extruder sets", "[chameleon]")
{
    std::vector<std::pair<double, unsigned>> raw = {
        {1.0, 0}, {1.0 + EPSILON * 0.5, 2}, {5.0, 1},
    };
    LayerFilamentTable table = build_layer_filament_table(raw);
    REQUIRE(table.size() == 2);
    CHECK_THAT(table[0].first, Catch::Matchers::WithinAbs(1.0, 1e-9));
    CHECK(table[0].second == std::set<unsigned>{0, 2});
    CHECK_THAT(table[1].first, Catch::Matchers::WithinAbs(5.0, 1e-9));
    CHECK(table[1].second == std::set<unsigned>{1});
}

TEST_CASE("build_layer_filament_table: z's farther than EPSILON apart stay separate entries", "[chameleon]")
{
    std::vector<std::pair<double, unsigned>> raw = { {1.0, 0}, {1.5, 1}, {2.0, 2} };
    LayerFilamentTable table = build_layer_filament_table(raw);
    REQUIRE(table.size() == 3);
    CHECK(table[0].second == std::set<unsigned>{0});
    CHECK(table[1].second == std::set<unsigned>{1});
    CHECK(table[2].second == std::set<unsigned>{2});
}

TEST_CASE("build_layer_filament_table: empty input returns an empty table", "[chameleon]")
{
    CHECK(build_layer_filament_table({}).empty());
}

TEST_CASE("chameleon_layer_free_extruders: exact z hit returns that entry's set", "[chameleon]")
{
    LayerFilamentTable table = build_layer_filament_table({ {1.0, 0}, {3.0, 1} });
    CHECK(chameleon_layer_free_extruders(table, 3.0) == std::set<unsigned>{1});
}

TEST_CASE("chameleon_layer_free_extruders: an EPSILON-neighbor z still coincidence-hits", "[chameleon]")
{
    LayerFilamentTable table = build_layer_filament_table({ {3.0, 1} });
    CHECK(chameleon_layer_free_extruders(table, 3.0 + EPSILON * 0.5) == std::set<unsigned>{1});
}

TEST_CASE("chameleon_layer_free_extruders: a z with no coincident entry returns EMPTY, never the nearest entry", "[chameleon]")
{
    // 3.0 and 3.0+10*EPSILON are two DISTINCT table entries (well outside each other's
    // EPSILON window); a query z sitting between them, still outside EPSILON of both,
    // must return empty rather than snapping to whichever entry happens to be nearest.
    LayerFilamentTable table = build_layer_filament_table({ {1.0, 0}, {50.0, 1} });
    CHECK(chameleon_layer_free_extruders(table, 25.0).empty());
}

TEST_CASE("chameleon_layer_free_extruders: empty table returns empty regardless of query_z", "[chameleon]")
{
    CHECK(chameleon_layer_free_extruders({}, 42.0).empty());
}

// --- v2.4 Task B (spec B, the claw fix): windowed query --------------------

TEST_CASE("chameleon_layer_free_extruders: default (no down_mm/up_mm passed) is byte-identical to the pre-v2.4 exact query", "[chameleon]")
{
    // Same fixtures as the four exact-coincidence tests directly above, but calling
    // the 2-argument overload explicitly alongside the 4-argument one with 0.0/0.0 -
    // proves the new defaulted parameters don't perturb a single existing caller
    // (this test IS one of those callers, unmodified) and that omitting the trailing
    // args is truly equivalent to passing them as 0.0 explicitly.
    LayerFilamentTable table = build_layer_filament_table({ {1.0, 0}, {3.0, 1} });
    CHECK(chameleon_layer_free_extruders(table, 3.0) == std::set<unsigned>{1});
    CHECK(chameleon_layer_free_extruders(table, 3.0, 0.0, 0.0) == std::set<unsigned>{1});

    LayerFilamentTable table2 = build_layer_filament_table({ {1.0, 0}, {50.0, 1} });
    CHECK(chameleon_layer_free_extruders(table2, 25.0).empty());
    CHECK(chameleon_layer_free_extruders(table2, 25.0, 0.0, 0.0).empty());
}

TEST_CASE("chameleon_layer_free_extruders: down_mm rescues an entry BELOW query_z that a zero-window query misses", "[chameleon]")
{
    // Entry at z=10.0; querying at z=11.5 (1.5mm above, well outside a zero-window's
    // EPSILON) misses under the default window, but a down_mm=2.0 window
    // ([11.5 - 2.0 - EPS, 11.5 + EPS] = [9.5-EPS, 11.5+EPS]) covers 10.0 - this is the
    // claw fix's own down_mm = support_layer->height usage (Print.cpp call site).
    LayerFilamentTable table = build_layer_filament_table({ {10.0, 2} });
    CHECK(chameleon_layer_free_extruders(table, 11.5).empty());                    // RED without the window
    CHECK(chameleon_layer_free_extruders(table, 11.5, 2.0, 0.0) == std::set<unsigned>{2});

    // down_mm too small to reach: 1.0mm window from 11.5 only covers down to 10.5,
    // still missing the entry at 10.0 - proves the window's SIZE is respected, not
    // just its presence.
    CHECK(chameleon_layer_free_extruders(table, 11.5, 1.0, 0.0).empty());
}

TEST_CASE("chameleon_layer_free_extruders: up_mm rescues an entry ABOVE query_z that a zero-window query misses", "[chameleon]")
{
    // Entry at z=10.0; querying at z=8.0 (2.0mm below) misses under the default
    // window, but an up_mm=2.5 window ([8.0-EPS, 8.0+2.5+EPS] = [8.0-EPS, 10.5+EPS])
    // covers 10.0 - this is the claw fix's own up_mm = kContactBandMm usage.
    LayerFilamentTable table = build_layer_filament_table({ {10.0, 5} });
    CHECK(chameleon_layer_free_extruders(table, 8.0).empty());                     // RED without the window
    CHECK(chameleon_layer_free_extruders(table, 8.0, 0.0, 2.5) == std::set<unsigned>{5});

    // up_mm too small to reach: 1.0mm window from 8.0 only covers up to 9.0, still
    // missing the entry at 10.0.
    CHECK(chameleon_layer_free_extruders(table, 8.0, 0.0, 1.0).empty());
}

TEST_CASE("chameleon_layer_free_extruders: down_mm and up_mm combine into ONE asymmetric window and union every entry inside it", "[chameleon]")
{
    // Three entries at 8.0/10.0/13.0; querying at z=10.0 with down_mm=1.5, up_mm=2.5
    // covers [8.5-EPS, 12.5+EPS] - includes 10.0 (exact) and... not 8.0 (0.5mm short
    // of the down edge) and not 13.0 (0.5mm past the up edge) - proves down/up bound
    // their own sides independently, not a symmetric radius.
    LayerFilamentTable table = build_layer_filament_table({ {8.0, 0}, {10.0, 1}, {13.0, 2} });
    CHECK(chameleon_layer_free_extruders(table, 10.0, 1.5, 2.5) == std::set<unsigned>{1});

    // Widen both sides just enough to catch all three - result is the UNION of every
    // covered entry's set, mirroring the exact-coincidence query's own merge-boundary
    // union behavior (see the EPSILON-merge-boundary test above), just over a wider
    // window instead of a merge artifact.
    CHECK(chameleon_layer_free_extruders(table, 10.0, 2.5, 3.5) == (std::set<unsigned>{0, 1, 2}));
}

TEST_CASE("brim_vote tie-prefers-prev_kept (C3): flips the min-id tie-break outcome", "[chameleon]")
{
    // Same exact-tie setup as "brim_vote tie-break: larger object area wins, then
    // lower extruder" above (two single-point walls equidistant from the origin, no
    // object_area data) - without prev_kept this falls all the way through to
    // std::min(1, 2) == 1 (proven by that other test). This test's whole point is that
    // C3's prev_kept check runs BEFORE that fallback: with prev_kept = {2}, the higher
    // id wins instead. Must FAIL if the C3 branch in brim_vote is reverted (min-id would
    // pick 1, the wrong answer for a column whose previous layer committed 2).
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 2, 0, 2), 1, 7);
    idx.add_polyline(segment(0, -2, 0, -2), 2, 9);
    BrimVoteParams p;
    p.prev_kept = {2};
    CHECK(brim_vote(idx, Point(0, 0), p) == 2);
}

TEST_CASE("brim_vote tie-prefers-prev_kept (C3): both-tied-candidates in prev_kept falls through unchanged", "[chameleon]")
{
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 2, 0, 2), 1, 7);
    idx.add_polyline(segment(0, -2, 0, -2), 2, 9);
    BrimVoteParams p;
    p.prev_kept = {1, 2}; // both tied candidates -> not "exactly one" -> falls through
    CHECK(brim_vote(idx, Point(0, 0), p) == 1); // object_area/min-id fallback, unchanged
}

TEST_CASE("partition_support_entities (C7 whole-collection vote) tie-prefers-prev_kept (C3): flips the lowest-id majority tie", "[chameleon]")
{
    // Identical fixture to "partition_support_entities (C7): whole-collection majority
    // vote ties break to the LOWEST extruder id" above (proven there to land on
    // extruder 1 with prev_kept empty) - only p.prev_kept differs here. Must FAIL if
    // the C3 branch in vote_collection_as_unit is reverted (the tie would still break
    // to the lowest id, 1, ignoring that this column's previous layer committed 2).
    // v2.3 Task 3 (spec C5): same min_run_mm widening as that test, same reason - this
    // fixture's ~11.2mm "losing" side is also a genuine contiguous minority run that
    // would otherwise DESCEND under C5's default threshold, which is not what this test
    // is isolating (the C3 prev_kept tie-preference feeding the (a) whole-move path).
    WallSampleIndex idx;
    idx.add_polyline(segment(0, 6, 10, 6), 1, 1);
    idx.add_polyline(segment(20, 6, 30, 6), 2, 2);
    BrimVoteParams p; p.fallback_extruder = 0;
    p.min_run_mm = 100.0;
    p.prev_kept = {2};
    auto resolver = [&idx, &p](const Point &pt) { return brim_vote(idx, pt, p); };

    ExtrusionEntityCollection *inner = nested_base_collection(6.0, /*no_sort=*/false);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    REQUIRE(out.count(2) == 1);
    CHECK(out.count(1) == 0);
    CHECK(fills.entities.empty());
}

// v2.3 Task 3 (spec C5, "tree selective descent") tests below. Resolvers here are
// direct point->extruder lambdas (same idiom as "split_polyline_by_resolver produces
// runs matching a synthetic resolver" above), not WallSampleIndex/brim_vote, since the
// per-x-coordinate zone rule is simpler and fully deterministic - no knn/1/d^2 scoring
// to reason about when the thing under test is the collection-level descend decision.

TEST_CASE("partition_support_entities (C5/v2.5c): a mixed collection whose minority run clears the threshold DESCENDS; EVERY leaf re-buckets by its own uniform vote, fallback included", "[chameleon]")
{
    // Two leaves, each internally uniform: leaf A (20mm, x in [0,20)) votes fallback (0)
    // throughout; leaf B (10mm, x in [25,35)) votes extruder 2 throughout. Whole-
    // collection histogram: ~27 fallback samples vs. ~14 extruder-2 samples (0.8mm
    // sampling over 20mm/10mm respectively) - a genuine, CONTIGUOUS minority run of
    // ~14*0.8 = 11.2mm, comfortably clearing the p.min_run_mm = 5.0 threshold set below.
    //
    // v2.5c root cause fix: this test used to assert leaf A's OWN uniform-fallback
    // vote left it "in place, untouched" inside `inner` (the descend path's old
    // per-leaf fast path only special-cased fallback), while leaf B's uniform
    // non-fallback vote got REBUILT into a new ExtrusionPath (`!= leafB`) rather
    // than moved. Both halves of that asymmetry are gone: "uniform anything moves
    // whole into its bucket" (item 1 of the fix) now applies inside a descended
    // collection exactly like it does at the top level - leaf A moves whole,
    // pointer-stable, into out[fallback]; leaf B moves whole, pointer-stable
    // (`== leafB`, no longer rebuilt), into out[2]. With both leaves gone, `inner`
    // is left empty and is deleted (the same "emptied shell -> single delete" rule
    // the pre-existing "collection emptied by descend" test already covers) -
    // `fills` ends up empty too, since nothing stays behind to keep the (now-
    // deleted) collection pointer alive in it.
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 25.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 5.0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(25), scale_(6)), Point(scale_(35), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->no_sort = true;
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);
    REQUIRE(inner->role() == erSupportMaterial);

    // Snapshotted BEFORE the call (pure function of leaf points, read-only) - proves
    // descended_out records the SAME column key partition_support_entities computed
    // internally, not some coincidentally-matching value.
    const Point expected_key = chameleon_quantize_point(chameleon_collection_bbox_center(*inner));

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    // DESCENDED, and both leaves individually re-bucketed (each votes uniformly on
    // its own) - the now-empty collection shell is deleted, so nothing is left
    // behind in support_fills.
    CHECK(fills.entities.empty());

    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == leafA); // moved WHOLE, pointer-stable - no rebuild

    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    CHECK(out.at(2).entities.front() == leafB); // moved WHOLE, pointer-stable - no rebuild
    CHECK(out.at(2).entities.front()->role() == erSupportMaterial);

    // The whole collection was never moved into any out[] bucket as a UNIT (that's
    // the not-descended whole-move path, not what happened here) - it was deleted
    // once its leaves emptied it out.
    for (const auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e != inner);

    REQUIRE(descended.count(expected_key) == 1);
    CHECK(descended.at(expected_key) == true);
}

TEST_CASE("partition_support_entities (C5/v2.5c): a mixed collection whose minority run stays BELOW threshold does not descend, and its fallback-majority whole moves into out[fallback]", "[chameleon]")
{
    // Same shape as the DESCEND test above, but leaf B is short: 2mm (x in [25,27)) ->
    // 2 + floor(2/0.8) = 4 samples -> a minority run of 4*0.8 = 3.2mm, under the SAME
    // p.min_run_mm = 5.0 threshold this time - the collection does NOT descend (same
    // not-genuinely-mixed classification as before, unchanged by v2.5c - see item 2 of
    // the v2.5c fix: the descend-vs-not-descend THRESHOLD decision is untouched).
    // v2.5c root cause fix: this test used to assert the not-descended, fallback-
    // majority collection stayed pointer-stable IN `fills` ("current behavior
    // (pointer-stable)" per spec C5's own wording, at the time meaning "stays"). It
    // is re-pointed here at the new contract: "not genuinely mixed" still means
    // "whole-move, pointer-stable, never split" - but the destination for a
    // fallback-majority whole-move is now out[fallback], the SAME unified rule a
    // non-fallback-majority whole-move already used (see the C7 "moved whole, never
    // split" test above, now joined by its own C7 fallback sibling).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 25.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 5.0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(25), scale_(6)), Point(scale_(27), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    CHECK(fills.entities.empty()); // moved out of support_fills entirely, not staying
    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == inner); // moved WHOLE - same collection pointer, never split
    REQUIRE(inner->entities.size() == 2);
    CHECK(inner->entities[0] == leafA); // both original leaf pointers intact - never split apart
    CHECK(inner->entities[1] == leafB);
    CHECK(descended.empty()); // never wrote a descend entry - it never descended
}

TEST_CASE("partition_support_entities (C5/M1): a minority split across TWO leaves, each individually sub-threshold, does NOT descend", "[chameleon]")
{
    // v2.3 final-review M1 fix: ordered_votes carries no leaf-boundary markers pre-fix,
    // so a "contiguous" minority run scan could silently concatenate the tail of one leaf
    // with the head of the NEXT leaf in collection order - two locations adjacent only in
    // SAMPLE-SEQUENCE order, not in space. Three leaves here: leaf A (20mm, x in [0,20))
    // votes fallback (0) throughout; leaf B (2mm, x in [25,27)) and leaf C (2mm, x in
    // [30,32)) each vote extruder 2 throughout - same shape as the "stays BELOW threshold"
    // test above (2mm -> 4 samples -> 3.2mm run each, under p.min_run_mm = 5.0), but split
    // into TWO leaves placed back-to-back in the collection (B immediately before C), so
    // their extruder-2 votes land adjacent in ordered_votes. Pre-fix, the scan concatenates
    // them into one 8-sample/6.4mm run that CLEARS the 5.0mm threshold - a spurious
    // DESCEND, even though B and C are two separate, non-adjacent support leaves (they
    // don't even touch - x in [27,30) is a gap covered by neither). Post-fix, the run
    // resets at the B/C boundary: each leaf's own run is 3.2mm, neither clears 5.0mm, so
    // the collection stays whole (does not descend) - unchanged by v2.5c, which only
    // changes what "stays whole" means for a fallback-majority winner (see the sibling
    // "stays BELOW threshold" test's own v2.5c note above: moves into out[fallback],
    // pointer-stable, instead of staying in `fills`).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 25.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 5.0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(25), scale_(6)), Point(scale_(27), scale_(6))});
    auto *leafC = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafC->polyline = Polyline({Point(scale_(30), scale_(6)), Point(scale_(32), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);
    inner->entities.push_back(leafC);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    // Must FAIL pre-fix: pre-fix, B+C's concatenated 6.4mm "run" clears the 5.0mm
    // threshold, so the collection DESCENDS (out.count(2) == 1, inner shrinks to just
    // leafA, descended records the column) instead of staying whole.
    CHECK(fills.entities.empty()); // v2.5c: not-genuinely-mixed fallback winner moves whole, not stays
    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == inner); // moved WHOLE - same collection pointer, never split
    REQUIRE(inner->entities.size() == 3);
    CHECK(inner->entities[0] == leafA); // all three original leaf pointers intact
    CHECK(inner->entities[1] == leafB);
    CHECK(inner->entities[2] == leafC);
    CHECK(descended.empty()); // never wrote a descend entry - it never descended
}

TEST_CASE("partition_support_entities (C5/M2): no prior descend on this column -> FULL threshold, minority stays below it, no descend", "[chameleon]")
{
    // v2.3 final-review M2 fix: the halved-threshold branch (p.min_run_mm * 0.5, gated on
    // p.descended_last_layer membership - BrimFilament.cpp's collection-descend decision)
    // had zero test coverage (grep for descended_last_layer in this file found nothing
    // pre-fix). This test and its sibling below pin BOTH sides of that branch with the
    // SAME fixture: leaf B is 2mm (x in [25,27)) -> 4 samples -> a 3.2mm minority run,
    // deliberately chosen BETWEEN half (2.5mm) and full (5.0mm) of p.min_run_mm = 5.0 -
    // the one honest seam that can only clear the descend threshold when the halving
    // actually applies. Here, p.descended_last_layer is empty (this column never
    // descended last layer), so the threshold stays the FULL 5.0mm - 3.2mm never clears
    // it, and the collection does not descend (v2.5c: its fallback-majority whole now
    // moves into out[fallback], pointer-stable - see the "stays BELOW threshold" test's
    // own v2.5c note above for the same not-descended/fallback-winner update).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 25.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 5.0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(25), scale_(6)), Point(scale_(27), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended; // empty: column never recorded as descended last layer

    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    CHECK(fills.entities.empty());
    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == inner);
    REQUIRE(inner->entities.size() == 2);
    CHECK(inner->entities[0] == leafA);
    CHECK(inner->entities[1] == leafB);
    CHECK(descended.empty());
}

TEST_CASE("partition_support_entities (C5/M2): column descended last layer -> HALVED threshold, same minority now clears it and DESCENDS", "[chameleon]")
{
    // Identical fixture to the sibling test above (leaf B's 3.2mm minority run, p.min_run_mm
    // = 5.0), except p.descended_last_layer is pre-seeded with this exact collection's own
    // quantized column key - the halved threshold (2.5mm) now applies, and 3.2mm clears
    // it: the collection DESCENDS, same shape as the full-threshold DESCEND test further
    // above (v2.5c: both leaves re-bucket by their own uniform vote, fallback included -
    // see that test's own v2.5c note for the full contract change).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 25.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 5.0; // halved below to 2.5mm; 3.2mm run clears THAT

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(25), scale_(6)), Point(scale_(27), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);

    // Snapshotted BEFORE the call (pure function of leaf points, read-only), same pattern
    // the existing DESCEND tests use - proves the pre-seeded key is the SAME key
    // partition_support_entities computes internally, not a coincidentally-matching value.
    const Point expected_key = chameleon_quantize_point(chameleon_collection_bbox_center(*inner));

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended;
    p.descended_last_layer[expected_key] = true; // pre-seed: this column descended last layer

    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    // DESCENDED, and (v2.5c) both leaves individually re-bucket by their own uniform
    // vote: leaf A (fallback) moves whole into out[0], leaf B moves whole into
    // out[2] - both pointer-stable, no rebuild. The now-empty collection shell is
    // deleted, so `fills` ends up empty.
    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    CHECK(out.at(2).entities.front() == leafB); // moved WHOLE, pointer-stable - no rebuild
    CHECK(out.at(2).entities.front()->role() == erSupportMaterial);

    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1);
    CHECK(out.at(0).entities.front() == leafA); // moved WHOLE, pointer-stable - no rebuild

    CHECK(fills.entities.empty());

    REQUIRE(descended.count(expected_key) == 1);
    CHECK(descended.at(expected_key) == true);
}

TEST_CASE("partition_support_entities (C5): a uniform mixed-extruder-free collection still whole-moves, pointer-stable", "[chameleon]")
{
    // v2.2 Task 3 (C7)'s own "moved whole" test already covers this scenario and keeps
    // passing unmodified after C5 (regression proof the new histogram-size<=1 short
    // circuit changes nothing for a genuinely uniform vote); this is a second, C5-named
    // instance with THREE leaves (a slightly larger "ring") for a clear regression
    // trip-wire tied to this changeset specifically.
    auto resolver = [](const Point &) -> unsigned { return 3u; }; // every sample -> extruder 3
    BrimVoteParams p;
    p.fallback_extruder = 0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(10), scale_(6))});
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(20), scale_(6)), Point(scale_(30), scale_(6))});
    auto *leafC = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafC->polyline = Polyline({Point(scale_(40), scale_(6)), Point(scale_(50), scale_(6))});

    auto *inner = new ExtrusionEntityCollection();
    inner->no_sort = true;
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);
    inner->entities.push_back(leafC);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    REQUIRE(out.count(3) == 1);
    REQUIRE(out.at(3).entities.size() == 1);
    CHECK(out.at(3).entities.front() == inner); // moved WHOLE - same pointer, never split
    CHECK(static_cast<ExtrusionEntityCollection *>(out.at(3).entities.front())->entities.size() == 3);
    CHECK(fills.entities.empty());
}

TEST_CASE("partition_support_entities (C5/v2.5c): descend re-buckets every leaf including fallback-voted ones - no more splice-back-in-place", "[chameleon]")
{
    // Three leaves in one no_sort collection: leaf0 (fallback-only), leaf1 (SPANS the
    // vote boundary - part fallback, part extruder 2), leaf2 (fallback-only). p.min_run_mm
    // = 0.0 isolates the mechanic under test from the length-threshold arithmetic already
    // covered above (any minority, however short, descends).
    //
    // v2.5c root cause fix: this test used to be named "...splices a leaf's
    // fallback-voted run back AT ITS OWN INDEX, preserving no_sort collection
    // order" and asserted leaf0/leaf2 stayed untouched inside `inner` while leaf1's
    // fallback half got spliced back in at its own index (never appended to
    // support_fills's end) - the descend path's own mirror of the top-level leaf
    // case's old fallback-stays fast path. Re-pointed at the new contract: EVERY
    // leaf inside a descended collection now resolves through the SAME per-leaf
    // rule the top-level case uses - leaf0 and leaf2 (each uniformly voting
    // fallback) move whole, pointer-stable, into out[fallback]; leaf1 (genuinely
    // mixed) splits, and BOTH halves - fallback included - land in out[], never
    // spliced back into `inner`. With all three leaves gone, `inner` empties and is
    // deleted (same "emptied shell -> single delete" rule as the top-level
    // collection-emptied test below), and `fills` ends up empty too.
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 15.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 0.0;

    auto *leaf0 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf0->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(5), scale_(6))});
    auto *leaf1 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf1->polyline = Polyline({Point(scale_(8), scale_(6)), Point(scale_(18), scale_(6))}); // crosses x=15
    auto *leaf2 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf2->polyline = Polyline({Point(scale_(2), scale_(8)), Point(scale_(7), scale_(8))});

    auto *inner = new ExtrusionEntityCollection();
    inner->no_sort = true;
    inner->entities.push_back(leaf0);
    inner->entities.push_back(leaf1);
    inner->entities.push_back(leaf2);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    // DESCENDED (leaf1 alone supplies a mixed vote -> histogram.size() > 1), and
    // every leaf re-buckets: nothing left behind anywhere.
    CHECK(fills.entities.empty());

    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 3); // leaf0 (whole), leaf2 (whole), leaf1's fallback half (rebuilt)
    CHECK(std::find(out.at(0).entities.begin(), out.at(0).entities.end(), leaf0) != out.at(0).entities.end());
    CHECK(std::find(out.at(0).entities.begin(), out.at(0).entities.end(), leaf2) != out.at(0).entities.end());

    // The third out[0] entity is leaf1's own fallback half - a NEW rebuilt path,
    // not any of the three original leaf pointers.
    const ExtrusionPath *spliced = nullptr;
    for (ExtrusionEntity *e : out.at(0).entities)
        if (e != leaf0 && e != leaf2)
            spliced = static_cast<const ExtrusionPath *>(e);
    REQUIRE(spliced != nullptr);
    CHECK(spliced != leaf1);
    REQUIRE(!spliced->polyline.points.empty());
    // Starts where leaf1 itself started (its own chain's first sample)...
    CHECK(unscale<double>(spliced->polyline.points.front().x()) == 8.0);
    // ...and never reaches the x=15 vote boundary (that part went to out[2] instead).
    CHECK(unscale<double>(spliced->polyline.points.back().x()) < 15.0);

    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    const auto *matched = static_cast<const ExtrusionPath *>(out.at(2).entities.front());
    REQUIRE(!matched->polyline.points.empty());
    // split_polyline_core's own shared-boundary-vertex invariant (same as the
    // "split_polyline_by_resolver produces runs matching a synthetic resolver" test
    // above): run k's first point is run (k-1)'s last point, so `matched`'s FRONT point
    // is legitimately `spliced`'s BACK point (x=14.4, < 15) - only the run's LAST point
    // is guaranteed to have truly crossed into the matched (>=15) zone.
    CHECK(matched->polyline.points.front() == spliced->polyline.points.back());
    CHECK(unscale<double>(matched->polyline.points.back().x()) >= 15.0);
}

TEST_CASE("partition_support_entities (C5/v2.5c): can_reverse is carried onto split runs, inside a descended collection", "[chameleon]")
{
    // Single leaf, self-contained, whose OWN vote is mixed (spans x=15) so descend
    // splits it into exactly two runs: one into out[fallback], one into out[2].
    // set_reverse() is called on the SOURCE leaf before the fixture is built - both
    // resulting pieces must inherit can_reverse() == false, mirroring how
    // Support/SupportCommon.cpp:660-663/765-767 builds a branch anchor path with
    // reversal disabled.
    //
    // v2.5c root cause fix: pre-v2.5c the fallback half was spliced back into
    // `inner` rather than bucketed; now it lands in out[fallback] like the matched
    // half lands in out[2], and the single leaf's departure leaves `inner` empty,
    // so it is deleted and `fills` ends up empty too (see this file's sibling
    // splice-back-in-place test, re-pointed under the same v2.5c note, for the
    // full contract change).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 15.0 ? 0u : 2u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0;
    p.min_run_mm = 0.0;

    auto *leaf = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    REQUIRE(leaf->can_reverse()); // default true, before set_reverse()
    leaf->set_reverse();
    REQUIRE_FALSE(leaf->can_reverse());

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leaf);

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    CHECK(fills.entities.empty()); // inner emptied by the split (both halves bucketed) and deleted

    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1); // the fallback half
    CHECK_FALSE(out.at(0).entities.front()->can_reverse());

    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1); // the matched half
    CHECK_FALSE(out.at(2).entities.front()->can_reverse());

    // Control: the SAME shape, but the source leaf's can_reverse defaults true (never
    // called set_reverse()) - both rebuilt pieces must stay true. Proves the mechanism
    // actually forwards the source's own state rather than always emitting false.
    auto *leaf2 = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf2->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    auto *inner2 = new ExtrusionEntityCollection();
    inner2->entities.push_back(leaf2);
    ExtrusionEntityCollection fills2;
    fills2.entities.push_back(inner2);
    std::map<unsigned, ExtrusionEntityCollection> out2;
    partition_support_entities(fills2, erSupportMaterial, 0, resolver, p, out2);

    CHECK(fills2.entities.empty());
    REQUIRE(out2.count(0) == 1);
    REQUIRE(out2.at(0).entities.size() == 1);
    CHECK(out2.at(0).entities.front()->can_reverse());
    REQUIRE(out2.count(2) == 1);
    REQUIRE(out2.at(2).entities.size() == 1);
    CHECK(out2.at(2).entities.front()->can_reverse());
}

TEST_CASE("partition_support_entities (C5/v2.5c): can_reverse is carried for a top-level (non-collection) leaf entity too", "[chameleon]")
{
    // Same mechanism, exercised directly at the shared partition_support_leaf_entity
    // call site the top-level (non-nested) leaf branch already used pre-C5 - this is
    // the actual code location the can_reverse fix landed in, so it is worth proving
    // independently of the descend/collection machinery above.
    //
    // v2.5c root cause fix: pre-v2.5c the fallback half of this split returned to
    // `fills` ("spliced at top level"); it now lands in out[fallback] like every
    // other run, so `fills` ends up empty after the split (matched original
    // deleted, both halves bucketed).
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 15.0 ? 0u : 2u;
    };
    BrimVoteParams p; p.fallback_extruder = 0; p.min_run_mm = 0.0;

    auto *leaf = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leaf->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(20), scale_(6))});
    leaf->set_reverse();

    ExtrusionEntityCollection fills;
    fills.entities.push_back(leaf);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out);

    CHECK(fills.entities.empty()); // leaf's own vote is mixed -> split, deleted, both halves bucketed
    REQUIRE(out.count(0) == 1);
    REQUIRE(out.at(0).entities.size() == 1); // the fallback half
    CHECK_FALSE(out.at(0).entities.front()->can_reverse());
    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    CHECK_FALSE(out.at(2).entities.front()->can_reverse());
}

TEST_CASE("partition_support_entities (C5): a collection emptied by descend is removed and deleted exactly once, never left as a dangling shell", "[chameleon]")
{
    // Two leaves, each uniformly voting a DIFFERENT non-fallback extruder (2 and 3) -
    // every leaf moves out of the collection entirely (nothing stays fallback), so the
    // rebuilt collection ends up with ZERO entities. partition_support_entities must
    // then delete the (now-empty) collection shell rather than leaving it behind empty
    // in support_fills or inside any out[] bucket. Deletion itself isn't observable
    // through the public API without UB (can't dereference freed memory to prove
    // non-existence) - this asserts the pointer appears NOWHERE reachable afterward,
    // the strongest black-box proof available; the report's ownership hand-walk covers
    // the rest (single delete, no leak, no double-free).
    //
    // v2.5c update: each leaf's vote here is UNIFORM (not just non-fallback), so
    // under the new contract both now move WHOLE, pointer-stable, into their
    // buckets (`== leafA`/`== leafB`, not rebuilt) - the pre-v2.5c "rebuilt, not
    // the original leaf" assertions below are flipped to match. The test's own
    // point (the collection shell itself is still deleted exactly once) is
    // unaffected.
    auto resolver = [](const Point &pt) -> unsigned {
        return unscale<double>(pt.x()) < 15.0 ? 2u : 3u;
    };
    BrimVoteParams p;
    p.fallback_extruder = 0; // never actually voted by either leaf below
    p.min_run_mm = 0.0;

    auto *leafA = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafA->polyline = Polyline({Point(scale_(0), scale_(6)), Point(scale_(10), scale_(6))});   // all < 15 -> extruder 2
    auto *leafB = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    leafB->polyline = Polyline({Point(scale_(20), scale_(6)), Point(scale_(25), scale_(6))});  // all >= 15 -> extruder 3

    auto *inner = new ExtrusionEntityCollection();
    inner->entities.push_back(leafA);
    inner->entities.push_back(leafB);
    const Point expected_key = chameleon_quantize_point(chameleon_collection_bbox_center(*inner));

    ExtrusionEntityCollection fills;
    fills.entities.push_back(inner);

    std::map<unsigned, ExtrusionEntityCollection> out;
    DescendColumnMap descended;
    partition_support_entities(fills, erSupportMaterial, 0, resolver, p, out, &descended);

    CHECK(fills.entities.empty()); // the (emptied, deleted) collection is gone, nothing left behind

    REQUIRE(out.count(2) == 1);
    REQUIRE(out.at(2).entities.size() == 1);
    CHECK(out.at(2).entities.front() == leafA); // moved WHOLE, pointer-stable - no rebuild (v2.5c)
    REQUIRE(out.count(3) == 1);
    REQUIRE(out.at(3).entities.size() == 1);
    CHECK(out.at(3).entities.front() == leafB);

    // The deleted collection pointer itself is not reachable anywhere: not in
    // support_fills (checked above, empty), and not smuggled into either bucket either.
    for (const auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e != inner);

    REQUIRE(descended.count(expected_key) == 1);
    CHECK(descended.at(expected_key) == true);
}

// --- v2.5d: role-split sampling bands (base votes coplanar-only, not the union) --
//
// Root cause (GUI round 5, debug-log-driven): a wrap/base support run abuts the wall
// AT ITS OWN Z, but Print.cpp's chameleon_assign_support_interfaces built ONE
// WallSampleIndex over the union of the coplanar layer and the 16-layer contact band
// ABOVE it, and used that SAME union for both the interface/ironing resolver (correct
// - an interface touches the surface above, so the contact band IS the right thing to
// sample) and the base resolver (wrong - the claw tapers, so a higher/whiter wall's
// samples can land XY-nearer in the flattened 2D index than the wall genuinely
// coplanar with the base run, and k=1 picks whichever sample is nearest regardless of
// which band it came from). The fix splits Print.cpp's single WallSampleIndex into
// two: `coplanar_idx` (coplanar-span layers only) feeds the base resolver;
// `band_idx` (the unchanged coplanar+contact-band union) keeps feeding interface/
// ironing. chameleon_assign_support_interfaces itself is a Print.cpp file-local
// static (not unit-instantiable outside a full Print/PrintObject scaffold - the same
// constraint every other "Print.cpp orchestration" comment in this file notes), so
// these tests exercise the reachable public engine seam the wiring fix is built out
// of: WallSampleIndex + brim_vote + partition_support_entities, with k=1/max_dist_mm=0
// matching Print.cpp's real interface_wall_params/base_wall_params construction. This
// cannot be a literal pre-fix-RED/post-fix-GREEN test against Print.cpp itself (no
// engine primitive here changes - the fix is purely which already-correct index each
// resolver captures at the Print.cpp call site) - it instead proves the two indices
// yield genuinely different, individually-verifiable answers for the identical query,
// which is the failure mode the fix removes for the base role specifically.

TEST_CASE("v2.5d: tapering claw - base's coplanar-only index picks the wall AT its own z, not the XY-nearer wall from the contact band above", "[chameleon]")
{
    const unsigned white_ext    = 3; // wall genuinely coplanar with the base run's own z
    const unsigned teal_ext     = 5; // wall that only exists in the contact band above (taper: XY-inward, but geometrically nearer)
    const unsigned base_fallback = 9;

    // band_idx: Print.cpp's union(contact_idx, coplanar_idx) - BOTH walls present.
    // teal sits only 1mm from the query run; white sits 3mm away - mirrors the
    // root cause's "teal samples at/near the ring's own z are nearest" (in the
    // flattened 2D index, XY proximity is all that matters, z-locality is lost).
    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 3, 30, 3), white_ext, 1); // coplanar wall, 3mm away
    band_idx.add_polyline(segment(0, 1, 30, 1), teal_ext,  2); // contact-band wall, 1mm away

    // coplanar_idx: ONLY the coplanar-span layers - the contact-band-only teal wall
    // is genuinely absent here (it doesn't exist at the base run's own z).
    WallSampleIndex coplanar_idx;
    coplanar_idx.add_polyline(segment(0, 3, 30, 3), white_ext, 1);

    BrimVoteParams params; // mirrors Print.cpp's base_wall_params: k explicitly 1 (default is 3), max_dist_mm=0 (default, uncapped)
    params.k = 1;
    REQUIRE(params.max_dist_mm == 0.0);
    params.fallback_extruder = base_fallback;

    auto band_resolver     = [&](const Point &pt) { return brim_vote(band_idx, pt, params); };
    auto coplanar_resolver = [&](const Point &pt) { return brim_vote(coplanar_idx, pt, params); };

    // Sanity: the two resolvers genuinely disagree at the query point (otherwise
    // this fixture wouldn't reproduce the bug mechanism at all).
    const Point query(scale_(15), scale_(0));
    REQUIRE(brim_vote(band_idx, query, params) == teal_ext);
    REQUIRE(brim_vote(coplanar_idx, query, params) == white_ext);

    auto make_base_run = [] {
        ExtrusionEntityCollection c;
        auto *base = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
        base->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(30), scale_(0))});
        c.entities.push_back(base);
        return c;
    };

    // Simulates the PRE-v2.5d bug: base wired to the union index picks TEAL, the
    // wrong color for a run that is genuinely coplanar with the white wall only.
    {
        auto fills = make_base_run();
        std::map<unsigned, ExtrusionEntityCollection> out;
        partition_support_entities(fills, erSupportMaterial, base_fallback, band_resolver, params, out);
        REQUIRE(out.count(teal_ext) == 1);
        CHECK(!out.at(teal_ext).entities.empty());
        CHECK(out.count(white_ext) == 0);
    }

    // v2.5d fix: base wired to the coplanar-only index picks WHITE - the correct
    // answer for a run that abuts the wall at its own z.
    {
        auto fills = make_base_run();
        std::map<unsigned, ExtrusionEntityCollection> out;
        partition_support_entities(fills, erSupportMaterial, base_fallback, coplanar_resolver, params, out);
        REQUIRE(out.count(white_ext) == 1);
        CHECK(!out.at(white_ext).entities.empty());
        CHECK(out.count(teal_ext) == 0);
    }
}

TEST_CASE("v2.5d: interface role keeps voting the union band (unchanged) - same fixture that flips base to coplanar-only", "[chameleon]")
{
    // Same two-wall fixture as the base test above, but exercised with role_filter =
    // erSupportMaterialInterface against band_idx - interfaces touch the surface
    // ABOVE them, so the contact band is deliberately still part of their
    // electorate; this must keep picking teal (the nearer union sample), proving
    // the v2.5d split changed ONLY the base role's wiring.
    const unsigned white_ext = 3;
    const unsigned teal_ext  = 5;
    const unsigned iface_fallback = 9;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 3, 30, 3), white_ext, 1);
    band_idx.add_polyline(segment(0, 1, 30, 1), teal_ext,  2);

    BrimVoteParams params;
    params.fallback_extruder = iface_fallback;
    auto band_resolver = [&](const Point &pt) { return brim_vote(band_idx, pt, params); };

    ExtrusionEntityCollection fills;
    auto *iface = new ExtrusionPath(erSupportMaterialInterface, 1.0, 0.4f, 0.2f);
    iface->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(30), scale_(0))});
    fills.entities.push_back(iface);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterialInterface, iface_fallback, band_resolver, params, out);

    REQUIRE(out.count(teal_ext) == 1);
    CHECK(!out.at(teal_ext).entities.empty());
    CHECK(out.count(white_ext) == 0);
    for (const auto &kv : out)
        for (const ExtrusionEntity *e : kv.second.entities)
            CHECK(e->role() == erSupportMaterialInterface);
}

TEST_CASE("v2.5d: empty coplanar_idx (support above the model's top walls) buckets the base run to base_fallback, deterministically", "[chameleon]")
{
    // Print.cpp spec item 3: brim_vote against an empty WallSampleIndex returns
    // p.fallback_extruder unconditionally (WallSampleIndex::knn on an empty index
    // returns an empty vector - brim_vote's own empty-knn_result early-return).
    // Post-v2.5c every resolver outcome buckets, fallback included (no more
    // "uniform fallback stays untouched" fast path), so an entirely empty
    // coplanar_idx - e.g. a base support column that exists above the model's own
    // topmost wall, with nothing coplanar underneath it at all - still produces a
    // single deterministic bucket keyed by base_fallback, not a silent no-op that
    // leaves the run sitting untouched in support_fills.
    const unsigned base_fallback = 7;
    WallSampleIndex empty_coplanar_idx;
    REQUIRE(empty_coplanar_idx.empty());

    BrimVoteParams params;
    params.fallback_extruder = base_fallback;
    auto resolver = [&](const Point &pt) { return brim_vote(empty_coplanar_idx, pt, params); };

    ExtrusionEntityCollection fills;
    auto *base = new ExtrusionPath(erSupportMaterial, 1.0, 0.4f, 0.2f);
    base->polyline = Polyline({Point(scale_(0), scale_(0)), Point(scale_(30), scale_(0))});
    ExtrusionEntity *base_ptr = base;
    fills.entities.push_back(base);

    std::map<unsigned, ExtrusionEntityCollection> out;
    partition_support_entities(fills, erSupportMaterial, base_fallback, resolver, params, out);

    REQUIRE(out.count(base_fallback) == 1);
    REQUIRE(out.at(base_fallback).entities.size() == 1);
    CHECK(out.at(base_fallback).entities.front() == base_ptr); // moved whole, pointer-stable (v2.5c)
    CHECK(fills.entities.empty()); // nothing left behind untouched in support_fills
}

// v2.5e: chameleon_build_support_resolvers (BrimFilament.hpp/.cpp) - the v2.5d final-
// review I1 fix. Print.cpp's chameleon_assign_support_interfaces is pipeline glue with
// no unit RED expressible of its own (same precedent as every prior "pass wiring" task
// in this file - apply_bucket_caps/chameleon_update_prev_kept/chameleon_dominant_
// matched_extruder were all extracted from Print.cpp for exactly this reason), so these
// tests pin the ACTUAL resolver-construction wiring the pass now calls into, not a
// hand-rolled proxy that only exercises brim_vote/partition_support_entities directly
// (the I1 gap: such a proxy could pass even if Print.cpp's real wiring diverged).

TEST_CASE("chameleon_build_support_resolvers (v2.5e wiring pin): interface/ironing try projection first, base NEVER consults band_idx", "[chameleon]")
{
    // Three DISTINCT extruders, one per source, so ANY wiring mistake swaps in a value
    // this test catches directly:
    //  - if base_resolver were re-wired to band_idx (or their union) instead of
    //    coplanar_wall_idx alone, it would return band_only_ext (10) instead of 20.
    //  - if interface_resolver ever stopped trying projection FIRST (e.g. fell straight
    //    to the band vote), it would return band_only_ext (10) instead of 30.
    const unsigned band_only_ext     = 10; // present ONLY in band_idx - base must never see this
    const unsigned coplanar_only_ext = 20; // present ONLY in coplanar_wall_idx - base's true answer
    const unsigned projection_ext    = 30; // returned by projection_lookup - interface's true answer
    const unsigned iface_fallback    = 91;
    const unsigned base_fallback     = 92;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(-5, 0, 5, 0), band_only_ext, 1);

    WallSampleIndex coplanar_wall_idx;
    coplanar_wall_idx.add_polyline(segment(-5, 0, 5, 0), coplanar_only_ext, 1);

    // Unconditional hit - any query point resolves to projection_ext.
    std::function<bool(const Point &, unsigned &)> projection_lookup =
        [projection_ext](const Point &, unsigned &out_extruder) -> bool {
            out_extruder = projection_ext;
            return true;
        };

    BrimVoteParams params;
    const Point    query(scale_(0), scale_(0));

    ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
        band_idx, coplanar_wall_idx, projection_lookup, params, iface_fallback, base_fallback);

    // Interface: projection wins outright over band_idx's own (otherwise-nearest) wall.
    CHECK(resolvers.interface_resolver(query) == projection_ext);
    // Ironing: same callable as interface - see the dedicated "ironing == interface"
    // test below for the fuller pin across hit/miss/fallback cases.
    CHECK(resolvers.ironing_resolver(query) == projection_ext);
    // Base: coplanar_wall_idx only - band_only_ext (10) must NEVER surface here.
    CHECK(resolvers.base_resolver(query) == coplanar_only_ext);
}

TEST_CASE("chameleon_build_support_resolvers (v2.5e): projection hit overrides a NEARER band_idx wall", "[chameleon]")
{
    // Hand-walk fixture for the z=11.10 GUI defect this task fixes: a khaki-painted
    // underside starts here; the interface sample directly under it must resolve to
    // khaki even though a teal wall in band_idx (the coplanar+contact-band union) sits
    // GEOMETRICALLY nearer - "an interface touches the surface above it" outranks
    // nearest-wall voting outright, it is not merely a tie-break.
    const unsigned khaki_ext = 3; // the model surface directly above the sample (projection)
    const unsigned teal_ext  = 5; // a NEARER wall in band_idx - must lose
    const unsigned fallback  = 9;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 0.2, 30, 0.2), teal_ext, 1); // 0.2mm away
    WallSampleIndex coplanar_wall_idx; // unused by interface_resolver, left empty

    std::function<bool(const Point &, unsigned &)> projection_lookup =
        [khaki_ext](const Point &, unsigned &out_extruder) -> bool {
            out_extruder = khaki_ext;
            return true;
        };

    BrimVoteParams params;
    const Point    query(scale_(15), scale_(0));

    // Sanity: band_idx alone really would pick teal here - proves this is a genuine
    // "nearer wall" fixture, not a vacuous one where projection had nothing to beat.
    BrimVoteParams sanity = params;
    sanity.k                 = 1;
    sanity.fallback_extruder = fallback;
    REQUIRE(brim_vote(band_idx, query, sanity) == teal_ext);

    ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
        band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);

    CHECK(resolvers.interface_resolver(query) == khaki_ext);
    CHECK(resolvers.ironing_resolver(query) == khaki_ext);
}

TEST_CASE("chameleon_build_support_resolvers (v2.5e): a projection miss falls through to the band_idx nearest-wall vote", "[chameleon]")
{
    const unsigned band_ext = 7;
    const unsigned fallback = 9;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 0, 30, 0), band_ext, 1);
    WallSampleIndex coplanar_wall_idx; // irrelevant to interface_resolver

    // Unconditional MISS.
    std::function<bool(const Point &, unsigned &)> projection_lookup =
        [](const Point &, unsigned &) -> bool { return false; };

    BrimVoteParams params;
    const Point    query(scale_(15), scale_(0));

    ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
        band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);

    CHECK(resolvers.interface_resolver(query) == band_ext);
    CHECK(resolvers.ironing_resolver(query) == band_ext);
}

TEST_CASE("chameleon_build_support_resolvers (v2.5e): an empty/default-constructed projection_lookup is treated as an unconditional miss", "[chameleon]")
{
    // Documents the header's own contract for a caller with nothing to project onto -
    // no special case needed, the `if (projection_lookup)` guard inside the resolver
    // short-circuits before ever invoking an empty std::function.
    const unsigned band_ext = 7;
    const unsigned fallback = 9;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 0, 30, 0), band_ext, 1);
    WallSampleIndex coplanar_wall_idx;

    std::function<bool(const Point &, unsigned &)> projection_lookup; // default-constructed, empty

    BrimVoteParams params;
    const Point    query(scale_(15), scale_(0));

    ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
        band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);

    CHECK(resolvers.interface_resolver(query) == band_ext);
}

TEST_CASE("chameleon_build_support_resolvers (v2.5e): ironing_resolver always equals interface_resolver, hit/miss/fallback alike", "[chameleon]")
{
    const unsigned band_ext = 4;
    const unsigned proj_ext = 6;
    const unsigned fallback = 9;

    WallSampleIndex band_idx;
    band_idx.add_polyline(segment(0, 0, 30, 0), band_ext, 1);
    WallSampleIndex coplanar_wall_idx;

    BrimVoteParams params;
    const Point    p(scale_(15), scale_(0));

    SECTION("projection hits")
    {
        std::function<bool(const Point &, unsigned &)> projection_lookup =
            [proj_ext](const Point &, unsigned &out_extruder) -> bool { out_extruder = proj_ext; return true; };
        ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
            band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);
        CHECK(resolvers.ironing_resolver(p) == resolvers.interface_resolver(p));
        CHECK(resolvers.ironing_resolver(p) == proj_ext);
    }

    SECTION("projection misses, falls to band vote")
    {
        std::function<bool(const Point &, unsigned &)> projection_lookup =
            [](const Point &, unsigned &) -> bool { return false; };
        ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
            band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);
        CHECK(resolvers.ironing_resolver(p) == resolvers.interface_resolver(p));
        CHECK(resolvers.ironing_resolver(p) == band_ext);
    }

    SECTION("projection misses AND band_idx is empty, both fall to fallback_extruder")
    {
        WallSampleIndex empty_band_idx;
        std::function<bool(const Point &, unsigned &)> projection_lookup =
            [](const Point &, unsigned &) -> bool { return false; };
        ChameleonSupportResolvers resolvers = chameleon_build_support_resolvers(
            empty_band_idx, coplanar_wall_idx, projection_lookup, params, fallback, fallback);
        CHECK(resolvers.ironing_resolver(p) == resolvers.interface_resolver(p));
        CHECK(resolvers.ironing_resolver(p) == fallback);
    }
}
