#include <catch2/catch.hpp>
#include "libslic3r/SliceCompare/Snapshot.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

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
