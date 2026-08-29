#include <catch2/catch.hpp>
#include "libslic3r/SliceCompare/Snapshot.hpp"
#include "libslic3r/SliceCompare/Diff.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <algorithm>
#include <filesystem>
#include <fstream>

using namespace Slic3r;
using namespace Slic3r::SliceCompare;

// Build a synthetic result: two layers (z=0.2, z=0.4), each one 10mm
// external-perimeter square drawn as 4 extrude moves at 60 mm/s,
// preceded by a travel move to the start corner.
static void make_result(GCodeProcessorResult& r, float y_shift = 0.f)
{
    auto add = [&r](EMoveType t, float x, float y, float z, float de, float f,
                    ExtrusionRole role) {
        GCodeProcessorResult::MoveVertex m;
        m.type = t; m.position = Vec3f(x, y, z); m.delta_extruder = de;
        m.feedrate = f; m.extrusion_role = role;
        r.moves.push_back(m);
    };
    for (float z : {0.2f, 0.4f}) {
        add(EMoveType::Travel, 0.f, 0.f + y_shift, z, 0.f, 200.f, erNone);
        add(EMoveType::Extrude, 10.f, 0.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 10.f, 10.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 0.f, 10.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
        add(EMoveType::Extrude, 0.f, 0.f + y_shift, z, 0.5f, 60.f, erExternalPerimeter);
    }
    r.filament_diameters = {1.75f};
    r.filament_densities = {1.24f};
    r.print_statistics.modes[(size_t)PrintEstimatedStatistics::ETimeMode::Normal].time = 123.f;
}

TEST_CASE("build_snapshot captures layers, segments, cells", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot s = build_snapshot(result, {{"layer_height", "0.2"}}, "A", "session");
    REQUIRE(s.layer_count == 2);
    REQUIRE(s.layers.size() == 2);
    REQUIRE(s.layers.count(20) == 1);   // z=0.20 -> key 20
    REQUIRE(s.layers.count(40) == 1);
    const LayerRec& l = s.layers.at(20);
    CHECK(l.segs.size() == 4);
    CHECK(l.extrusion_mm == Approx(2.0));                 // 4 x 0.5
    // 4 sides x 10mm at 60mm/s = 40/60 s
    CHECK(l.feature_seconds.at((uint8_t)erExternalPerimeter) == Approx(40.0/60.0).margin(1e-3));
    CHECK(l.cells.count({0, 0}) == 1);                    // 10mm grid: square touches cell (0,0)+(1,*)
    CHECK(s.est_seconds == Approx(123.0));
    CHECK(s.filament_mm == Approx(4.0));                  // 8 x 0.5 over both layers
    CHECK(s.config.at("layer_height") == "0.2");
    // grams: mm of 1.75 filament -> volume*density: pi*(0.0875cm)^2 * 0.4cm... just require > 0
    CHECK(s.filament_g > 0.0);
}

TEST_CASE("SnapshotStore ring buffer evicts oldest", "[slice_compare]")
{
    auto& st = SnapshotStore::instance();
    st.clear();
    std::vector<int> ids;
    for (int i = 0; i < 10; ++i) {
        Snapshot s; s.label = "snap" + std::to_string(i);
        ids.push_back(st.add(std::move(s)));
    }
    CHECK(st.list().size() == 8);
    CHECK(st.get(ids[0]) == nullptr);          // evicted
    CHECK(st.get(ids[9]) != nullptr);
    CHECK(st.list().front().second == "snap9"); // newest first
    st.clear();
}

TEST_CASE("diff_configs ignores volatile keys, reports changes", "[slice_compare]")
{
    Snapshot a, b;
    a.config = {{"layer_height","0.2"}, {"wall_loops","2"}, {"print_host","x"}};
    b.config = {{"layer_height","0.16"},{"wall_loops","2"}, {"extra","1"}};
    auto rows = diff_configs(a, b);
    REQUIRE(rows.size() == 2);
    CHECK(rows[0].key == "extra");            // sorted by key; absent in a
    CHECK(rows[0].a.empty());
    CHECK(rows[1].key == "layer_height");
    CHECK(rows[1].a == "0.2"); CHECK(rows[1].b == "0.16");
    // print_host filtered out entirely (volatile)
}

TEST_CASE("diff_features aggregates per role", "[slice_compare]")
{
    // NOTE: make_result here takes an out-param (GCodeProcessorResult&) rather
    // than returning by value — see the file-static helper above; it was
    // changed in commit 061958a1a8 because GCodeProcessorResult holds a mutex
    // and is not copyable/movable, so `build_snapshot(make_result(), ...)` as
    // written in the task brief does not compile against the current helper.
    GCodeProcessorResult result_a, result_b;
    make_result(result_a);
    make_result(result_b);
    Snapshot a = build_snapshot(result_a, {}, "A", "session");
    Snapshot b = build_snapshot(result_b, {}, "B", "session");
    auto rows = diff_features(a, b);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == (uint8_t)erExternalPerimeter);
    CHECK(rows[0].sec_a == Approx(rows[0].sec_b));
    CHECK(rows[0].mm_a == Approx(4.0));
}

// NOTE: adapted from the task-4 brief, which calls `make_result()` by value.
// The file-static helper above takes an out-param (`make_result(r)` /
// `make_result(r, y_shift)`) because GCodeProcessorResult holds a mutex and
// is not copyable/movable — same intent as the brief, different call shape.

TEST_CASE("diff_layers matches equal heights, self-diff identical", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot a = build_snapshot(result, {}, "A", "s");
    LayerDiff d = diff_layers(a, a);
    CHECK(d.matched == 2); CHECK(d.identical == 2);
    CHECK(d.changed == 0); CHECK(d.a_only == 0); CHECK(d.b_only == 0);
}

TEST_CASE("diff_layers flags relocation via cells", "[slice_compare]")
{
    GCodeProcessorResult result_a, result_b;
    make_result(result_a, 0.f);
    make_result(result_b, 60.f);
    Snapshot a = build_snapshot(result_a, {}, "A", "s");
    Snapshot b = build_snapshot(result_b, {}, "B", "s"); // same amount, moved 60mm
    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 2);
    CHECK(d.changed == 2);
    const auto& row = d.rows.front();
    CHECK(row.overlap < 0.5);
    CHECK(std::find(row.flags.begin(), row.flags.end(), "RELOCATED") != row.flags.end());
}

TEST_CASE("diff_layers honest about unequal layer heights", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot a = build_snapshot(result, {}, "A", "s");       // z 0.2/0.4
    Snapshot b = a;
    b.layers.clear();
    LayerRec l; l.z = 0.3; l.extrusion_mm = 1.0; b.layers[30] = l;  // z=0.30 only
    LayerDiff d = diff_layers(a, b);
    CHECK(d.matched == 0); CHECK(d.a_only == 2); CHECK(d.b_only == 1);
}

// NOTE: adapted from the task-5 brief, which calls `make_result()` by value
// and passes the result straight into build_snapshot(). The file-static
// helper above takes an out-param (`make_result(r)`) because
// GCodeProcessorResult holds a mutex and is not copyable/movable — same
// intent as the brief, different call shape (see the task-4 note above).

TEST_CASE("diff_segments: identical layers are all both", "[slice_compare]")
{
    GCodeProcessorResult result;
    make_result(result);
    Snapshot s = build_snapshot(result, {}, "A", "s");
    const LayerRec& l = s.layers.at(20);
    SegDiff d = diff_segments(l, l);
    CHECK(d.both.size() == 4);
    CHECK(d.a_only.empty()); CHECK(d.b_only.empty()); CHECK(d.jitter.empty());
}

TEST_CASE("diff_segments: direction-insensitive", "[slice_compare]")
{
    LayerRec a, b;
    a.segs.push_back({0,0, 10,0, 1});
    b.segs.push_back({10,0, 0,0, 1});          // reversed
    SegDiff d = diff_segments(a, b);
    CHECK(d.both.size() == 1); CHECK(d.a_only.empty()); CHECK(d.b_only.empty());
}

TEST_CASE("diff_segments: 0.2mm shift is jitter, 2mm is real", "[slice_compare]")
{
    LayerRec a, b1, b2;
    a.segs.push_back({0,0, 10,0, 1});
    b1.segs.push_back({0,0.2f, 10,0.2f, 1});
    b2.segs.push_back({0,2.f,  10,2.f,  1});
    SegDiff d1 = diff_segments(a, b1);
    CHECK(d1.jitter.size() == 1); CHECK(d1.a_only.empty()); CHECK(d1.b_only.empty());
    SegDiff d2 = diff_segments(a, b2);
    CHECK(d2.a_only.size() == 1); CHECK(d2.b_only.size() == 1); CHECK(d2.jitter.empty());
}

TEST_CASE("load_snapshot_from_file parses gcode + config block", "[slice_compare]")
{
    auto tmp = std::filesystem::temp_directory_path() / "sc_test.gcode";
    std::ofstream f(tmp);
    f << "; CONFIG_BLOCK_START\n; layer_height = 0.2\n; wall_loops = 2\n; CONFIG_BLOCK_END\n"
      << "G21\nG90\nM83\n"
      << "G1 Z0.2 F600\n"
      << "G1 X0 Y0 F6000\n"
      << ";TYPE:Outer wall\n"
      << "G1 X10 Y0 E0.5 F3600\nG1 X10 Y10 E0.5\nG1 X0 Y10 E0.5\nG1 X0 Y0 E0.5\n";
    f.close();
    FileLoadResult r = load_snapshot_from_file(tmp.string());
    REQUIRE(r.snapshot.has_value());
    CHECK(r.snapshot->config.at("layer_height") == "0.2");
    CHECK(r.snapshot->layers.size() == 1);
    CHECK(r.snapshot->layers.begin()->second.segs.size() >= 4);
    CHECK(r.snapshot->source == tmp.string());
    std::filesystem::remove(tmp);
}

TEST_CASE("load_snapshot_from_file reports missing file", "[slice_compare]")
{
    FileLoadResult r = load_snapshot_from_file("Z:/definitely/not/here.gcode");
    CHECK(!r.snapshot.has_value());
    CHECK(!r.error.empty());
}
