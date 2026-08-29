# Slice Compare Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** In-app visual + numerical comparison of two sliced results (session snapshots or external gcode files): config diff, per-feature stats, Z-keyed layer diff, and a per-layer path overlay canvas.

**Architecture:** A UI-free engine in `libslic3r/SliceCompare/` (Snapshot built from `GCodeProcessorResult`, pure diff functions) unit-tested with Catch2, plus a wxWidgets non-modal frame in `slic3r/GUI/SliceCompare/` that renders engine output. Session snapshots are captured in `Plater::priv::on_process_completed`; file mode runs `GCodeProcessor::process_file` into a local result.

**Tech Stack:** C++17, wxWidgets 3.x, Catch2 (existing tests/libslic3r), miniz (in-tree, for gcode.3mf), no new dependencies.

**Spec:** `docs/superpowers/specs/2026-08-29-slice-compare-design.md`

## Global Constraints

- Build: `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\67c5db31-fac5-4c54-8520-420f5e315bcc\scratchpad\build_snorca.bat slicer` (full app) — do NOT run while another build is running (a parallel session shares this checkout; check `Get-Process cl,link,MSBuild` first).
- Tests: `cmake --build build --config Release --target libslic3r_tests` then run `build\tests\libslic3r\Release\libslic3r_tests.exe "[slice_compare]"`.
- Commit ONLY the files this plan names (explicit `git add <paths>` — never `git add -A`; a parallel session has unrelated edits in `src/ultranet/`).
- Namespace: `Slic3r::SliceCompare`. New files use 4-space indent, `#pragma once` — hmm, existing libslic3r headers use `#ifndef` guards; use `#ifndef slic3r_SliceCompare_*_hpp_` guards to match.
- Quantization constants (spec): Z key = `lround(z*100)` (10 µm); segment coords 10 µm; cells 10 mm; `Z_TOL` 0.05 mm; rescue radius 0.3 mm; identical-layer thresholds 0.5 (g/s) and overlap > 0.85.
- `MoveVertex::time` is an INDEX, not a duration (store_move_vertex fills it with `moves.size()`). Per-move seconds are estimated as `xy_length / feedrate` (feedrate is mm/s in MoveVertex). Snapshot totals use `print_statistics.modes[Normal]` (exact).
- Known spec deviation: canvas is `wxPanel` + `wxAutoBufferedPaintDC` (not `wxGLCanvas`) — simpler, no GL-context plumbing, per-layer segment counts (≤ ~30k lines) are fine for GDI with collinear merge.

## File Structure

| File | Responsibility |
|---|---|
| Create `src/libslic3r/SliceCompare/Snapshot.hpp/.cpp` | `Snapshot`/`LayerRec`/`Seg` types, `build_snapshot()`, `SnapshotStore`, gcode-text config parse, `load_snapshot_from_file()` |
| Create `src/libslic3r/SliceCompare/Diff.hpp/.cpp` | Pure diff functions: config, feature stats, layer match+flags, segment diff |
| Create `tests/libslic3r/test_slice_compare.cpp` | All engine unit tests, tag `[slice_compare]` |
| Create `src/slic3r/GUI/SliceCompare/SliceCompareFrame.hpp/.cpp` | Non-modal frame, pickers, header, tables, slider |
| Create `src/slic3r/GUI/SliceCompare/CompareCanvas.hpp/.cpp` | 2D overlay/side-by-side canvas, zoom/pan |
| Modify `src/libslic3r/CMakeLists.txt` (near line 216) | add the two libslic3r cpp/hpp pairs |
| Modify `tests/libslic3r/CMakeLists.txt` (source list at top) | add test file |
| Modify `src/slic3r/CMakeLists.txt` (near GUI/Monitor.cpp, line ~305) | add the two GUI cpp/hpp pairs |
| Modify `src/slic3r/GUI/Plater.cpp` (`on_process_completed`, ~15232) | capture hook |
| Modify `src/slic3r/GUI/MainFrame.cpp` (viewMenu, before `m_menubar->Append(viewMenu…)` ~3170) | two menu items |

---

### Task 1: Engine types + build_snapshot

**Files:**
- Create: `src/libslic3r/SliceCompare/Snapshot.hpp`, `src/libslic3r/SliceCompare/Snapshot.cpp`
- Modify: `src/libslic3r/CMakeLists.txt` (add `SliceCompare/Snapshot.hpp` and `.cpp` after the `GCode/GCodeProcessor.cpp` line 216 block)
- Modify: `tests/libslic3r/CMakeLists.txt` (add `test_slice_compare.cpp` to the `add_executable` list)
- Test: `tests/libslic3r/test_slice_compare.cpp`

**Interfaces:**
- Consumes: `Slic3r::GCodeProcessorResult` (`libslic3r/GCode/GCodeProcessor.hpp`), `ExtrusionRole` (`libslic3r/ExtrusionEntity.hpp`)
- Produces (used by every later task):

```cpp
namespace Slic3r { namespace SliceCompare {

struct Seg { float x0, y0, x1, y1; uint8_t role; };

struct LayerRec {
    double z = 0.0;                                   // representative z (mm)
    double extrusion_mm = 0.0;
    std::map<uint8_t, double> feature_seconds;        // key = ExtrusionRole
    std::set<std::pair<int16_t, int16_t>> cells;      // 10 mm grid fingerprint
    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    bool  has_bbox = false;
    std::vector<Seg> segs;
    double seconds() const;                           // sum of feature_seconds
};

struct Snapshot {
    std::string label;
    std::string source;                               // "session" or file path
    std::map<std::string, std::string> config;
    double est_seconds = 0, filament_mm = 0, filament_g = 0, max_speed = 0;
    int    layer_count = 0;
    std::map<int, LayerRec> layers;                   // key = lround(z*100)
};

// config: already-serialized key->value map (caller-provided; see Task 7/6)
Snapshot build_snapshot(const GCodeProcessorResult& result,
                        std::map<std::string, std::string> config,
                        const std::string& label,
                        const std::string& source);

}} // namespaces
```

- [ ] **Step 1: Write the failing test**

`tests/libslic3r/test_slice_compare.cpp`:

```cpp
#include <catch2/catch.hpp>
#include "libslic3r/SliceCompare/Snapshot.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

using namespace Slic3r;
using namespace Slic3r::SliceCompare;

// Build a synthetic result: two layers (z=0.2, z=0.4), each one 10mm
// external-perimeter square drawn as 4 extrude moves at 60 mm/s,
// preceded by a travel move to the start corner.
static GCodeProcessorResult make_result(float y_shift = 0.f)
{
    GCodeProcessorResult r;
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
    return r;
}

TEST_CASE("build_snapshot captures layers, segments, cells", "[slice_compare]")
{
    Snapshot s = build_snapshot(make_result(), {{"layer_height", "0.2"}}, "A", "session");
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
```

- [ ] **Step 2: Add files to both CMakeLists, run test build to verify it fails to link/compile**

Run: `cmake --build build --config Release --target libslic3r_tests`
Expected: FAIL — `SliceCompare/Snapshot.hpp` missing (create empty next step) or unresolved `build_snapshot`.

- [ ] **Step 3: Implement Snapshot.hpp (interface block above, plus includes/guards) and Snapshot.cpp:**

```cpp
#include "Snapshot.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"
#include <cmath>

namespace Slic3r { namespace SliceCompare {

double LayerRec::seconds() const {
    double t = 0; for (auto& kv : feature_seconds) t += kv.second; return t;
}

Snapshot build_snapshot(const GCodeProcessorResult& result,
                        std::map<std::string, std::string> config,
                        const std::string& label, const std::string& source)
{
    Snapshot s;
    s.label = label; s.source = source; s.config = std::move(config);
    s.est_seconds = result.print_statistics.modes[
        (size_t)PrintEstimatedStatistics::ETimeMode::Normal].time;

    const float diam = result.filament_diameters.empty() ? 1.75f : result.filament_diameters[0];
    const float dens = result.filament_densities.empty() ? 1.24f : result.filament_densities[0];
    // grams per mm of filament = area(cm^2) * 0.1cm * density(g/cm^3)
    const double g_per_mm = M_PI * (diam / 20.0) * (diam / 20.0) * 0.1 * dens;

    const GCodeProcessorResult::MoveVertex* prev = nullptr;
    for (const auto& m : result.moves) {
        if (m.feedrate > s.max_speed) s.max_speed = m.feedrate;
        if (m.type == EMoveType::Extrude && m.delta_extruder > 0.f && prev) {
            const int zkey = (int)std::lround(m.position.z() * 100.0);
            LayerRec& l = s.layers[zkey];
            if (l.segs.empty() && l.extrusion_mm == 0.0) l.z = m.position.z();
            const float x0 = prev->position.x(), y0 = prev->position.y();
            const float x1 = m.position.x(),    y1 = m.position.y();
            l.segs.push_back({x0, y0, x1, y1, (uint8_t)m.extrusion_role});
            l.extrusion_mm += m.delta_extruder;
            const double len = std::hypot((double)x1 - x0, (double)y1 - y0);
            if (m.feedrate > 0.f)
                l.feature_seconds[(uint8_t)m.extrusion_role] += len / m.feedrate;
            l.cells.insert({(int16_t)std::floor(x1 / 10.f), (int16_t)std::floor(y1 / 10.f)});
            l.cells.insert({(int16_t)std::floor(x0 / 10.f), (int16_t)std::floor(y0 / 10.f)});
            if (!l.has_bbox) { l.bx0 = l.bx1 = x1; l.by0 = l.by1 = y1; l.has_bbox = true; }
            l.bx0 = std::min({l.bx0, x0, x1}); l.by0 = std::min({l.by0, y0, y1});
            l.bx1 = std::max({l.bx1, x0, x1}); l.by1 = std::max({l.by1, y0, y1});
            s.filament_mm += m.delta_extruder;
        }
        prev = &m;
    }
    s.filament_g = s.filament_mm * g_per_mm;
    s.layer_count = (int)s.layers.size();
    return s;
}

}} // namespaces
```

- [ ] **Step 4: Build + run test to verify it passes**

Run: `build\tests\libslic3r\Release\libslic3r_tests.exe "[slice_compare]"`
Expected: PASS (all assertions).

- [ ] **Step 5: Commit**

```bash
git add src/libslic3r/SliceCompare/Snapshot.hpp src/libslic3r/SliceCompare/Snapshot.cpp \
        src/libslic3r/CMakeLists.txt tests/libslic3r/CMakeLists.txt tests/libslic3r/test_slice_compare.cpp
git commit -m "feat(slice-compare): Snapshot type + build_snapshot from GCodeProcessorResult"
```

---

### Task 2: SnapshotStore ring buffer

**Files:**
- Modify: `src/libslic3r/SliceCompare/Snapshot.hpp/.cpp`
- Test: `tests/libslic3r/test_slice_compare.cpp`

**Interfaces (produces):**

```cpp
class SnapshotStore {
public:
    static SnapshotStore& instance();
    // returns id of the stored snapshot; evicts oldest beyond capacity (8)
    int  add(Snapshot snap);
    std::shared_ptr<const Snapshot> get(int id) const;             // null if evicted
    std::vector<std::pair<int, std::string>> list() const;         // (id, label), newest first
    void clear();
private:
    mutable std::mutex m_mutex;
    int m_next_id = 1;
    std::deque<std::pair<int, std::shared_ptr<Snapshot>>> m_items; // capacity 8
};
```

- [ ] **Step 1: Write the failing test**

```cpp
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
```

- [ ] **Step 2: Build, verify FAIL (unresolved SnapshotStore)**
- [ ] **Step 3: Implement in Snapshot.cpp** (Meyers singleton; `add` locks, pushes front, `while (m_items.size() > 8) m_items.pop_back();`; `get` linear scan; `list` copies id+label).
- [ ] **Step 4: Build + run `[slice_compare]` — PASS**
- [ ] **Step 5: Commit** `git add src/libslic3r/SliceCompare/Snapshot.hpp src/libslic3r/SliceCompare/Snapshot.cpp tests/libslic3r/test_slice_compare.cpp && git commit -m "feat(slice-compare): SnapshotStore session ring buffer"`

---

### Task 3: Config diff + feature stats

**Files:**
- Create: `src/libslic3r/SliceCompare/Diff.hpp`, `src/libslic3r/SliceCompare/Diff.cpp`
- Modify: `src/libslic3r/CMakeLists.txt` (add the pair)
- Test: `tests/libslic3r/test_slice_compare.cpp`

**Interfaces (produces):**

```cpp
namespace Slic3r { namespace SliceCompare {

struct ConfigRow { std::string key, a, b; };            // a/b empty = absent
std::vector<ConfigRow> diff_configs(const Snapshot& a, const Snapshot& b);

struct FeatureRow {
    uint8_t role;
    double sec_a = 0, sec_b = 0, mm_a = 0, mm_b = 0, len_a = 0, len_b = 0;
};
std::vector<FeatureRow> diff_features(const Snapshot& a, const Snapshot& b); // sorted by sec_a+sec_b desc

}}
```

- [ ] **Step 1: Write failing tests**

```cpp
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
    Snapshot a = build_snapshot(make_result(), {}, "A", "session");
    Snapshot b = build_snapshot(make_result(), {}, "B", "session");
    auto rows = diff_features(a, b);
    REQUIRE(rows.size() == 1);
    CHECK(rows[0].role == (uint8_t)erExternalPerimeter);
    CHECK(rows[0].sec_a == Approx(rows[0].sec_b));
    CHECK(rows[0].mm_a == Approx(4.0));
}
```

- [ ] **Step 2: Build, verify FAIL**
- [ ] **Step 3: Implement Diff.cpp.** Volatile-key filter (exact set): keys starting with `print_host`, `printhost_`, plus `filename`, `preset_name`, `preset_names`, `printer_settings_id`, `print_settings_id`, `filament_settings_id`. `diff_features`: walk both snapshots' layers, accumulate per role seconds/extrusion/length (length = Σ hypot per seg), union of roles, sort by combined seconds desc.
- [ ] **Step 4: Build + run — PASS**
- [ ] **Step 5: Commit** `git add src/libslic3r/SliceCompare/Diff.hpp src/libslic3r/SliceCompare/Diff.cpp src/libslic3r/CMakeLists.txt tests/libslic3r/test_slice_compare.cpp && git commit -m "feat(slice-compare): config diff + per-feature stats"`

---

### Task 4: Z-keyed layer matching + flags

**Files:** Modify `Diff.hpp/.cpp`; test file.

**Interfaces (produces):**

```cpp
struct LayerMatch {
    int zkey_a = -1, zkey_b = -1;         // -1 = no counterpart (a_only/b_only)
    double d_seconds = 0, d_extrusion = 0;
    double overlap = 1.0;                 // cell Jaccard, matched only
    std::vector<std::string> flags;       // RELOCATED, GEOMETRY-CHANGED, SUPPORT-CHANGED, MATERIAL-ADDED-NEW-REGION
    bool changed = false;
};
struct LayerDiff {
    std::vector<LayerMatch> rows;         // z-ascending; includes unmatched
    int matched = 0, identical = 0, changed = 0, a_only = 0, b_only = 0;
    int biggest_zkey_a = -1;              // matched layer w/ max |d_seconds| (ties: |d_extrusion|)
};
LayerDiff diff_layers(const Snapshot& a, const Snapshot& b);
```

Constants (Diff.cpp): `Z_TOL_KEYS = 5` (0.05 mm on the 10 µm key), `MATCH_TOL = 0.5`, `IDENTICAL_OVERLAP = 0.85`.

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("diff_layers matches equal heights, self-diff identical", "[slice_compare]")
{
    Snapshot a = build_snapshot(make_result(), {}, "A", "s");
    LayerDiff d = diff_layers(a, a);
    CHECK(d.matched == 2); CHECK(d.identical == 2);
    CHECK(d.changed == 0); CHECK(d.a_only == 0); CHECK(d.b_only == 0);
}

TEST_CASE("diff_layers flags relocation via cells", "[slice_compare]")
{
    Snapshot a = build_snapshot(make_result(0.f),  {}, "A", "s");
    Snapshot b = build_snapshot(make_result(60.f), {}, "B", "s"); // same amount, moved 60mm
    LayerDiff d = diff_layers(a, b);
    REQUIRE(d.matched == 2);
    CHECK(d.changed == 2);
    const auto& row = d.rows.front();
    CHECK(row.overlap < 0.5);
    CHECK(std::find(row.flags.begin(), row.flags.end(), "RELOCATED") != row.flags.end());
}

TEST_CASE("diff_layers honest about unequal layer heights", "[slice_compare]")
{
    Snapshot a = build_snapshot(make_result(), {}, "A", "s");       // z 0.2/0.4
    Snapshot b = a;
    b.layers.clear();
    LayerRec l; l.z = 0.3; l.extrusion_mm = 1.0; b.layers[30] = l;  // z=0.30 only
    LayerDiff d = diff_layers(a, b);
    CHECK(d.matched == 0); CHECK(d.a_only == 2); CHECK(d.b_only == 1);
}
```

- [ ] **Step 2: Build, verify FAIL**
- [ ] **Step 3: Implement.** Greedy two-pointer over both key-sorted maps (`|ka - kb| <= Z_TOL_KEYS` matches). For matches: `d_seconds = lb.seconds()-la.seconds()`, `d_extrusion`, `overlap = |cells∩| / |cells∪|` (1.0 if either empty). `changed` unless `|d_extrusion| <= 0.5 && |d_seconds| <= 0.5 && overlap > 0.85`. Flags exactly per spec thresholds: RELOCATED (`overlap < 0.5 && |d_extrusion| < 0.5`); GEOMETRY-CHANGED (bbox width or height differ > 5 mm, both bboxes valid); SUPPORT-CHANGED (`|Δ(erSupportMaterial + erSupportMaterialInterface seconds)| > 0.5`); MATERIAL-ADDED-NEW-REGION (`overlap < 0.5 && d_extrusion > 0.5`). Track biggest.
- [ ] **Step 4: Build + run — PASS**
- [ ] **Step 5: Commit** `git add src/libslic3r/SliceCompare/Diff.hpp src/libslic3r/SliceCompare/Diff.cpp tests/libslic3r/test_slice_compare.cpp && git commit -m "feat(slice-compare): Z-keyed layer diff with change flags"`

---

### Task 5: Segment diff + proximity rescue

**Files:** Modify `Diff.hpp/.cpp`; test file.

**Interfaces (produces):**

```cpp
struct SegDiff { std::vector<Seg> both, a_only, b_only, jitter; };
// Compare one matched layer pair; q_um = quantization in µm (default 10)
SegDiff diff_segments(const LayerRec& a, const LayerRec& b, double rescue_radius = 0.3);
```

- [ ] **Step 1: Write failing tests**

```cpp
TEST_CASE("diff_segments: identical layers are all both", "[slice_compare]")
{
    Snapshot s = build_snapshot(make_result(), {}, "A", "s");
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
```

- [ ] **Step 2: Build, verify FAIL**
- [ ] **Step 3: Implement.** Key = endpoints quantized to 10 µm as `int64` pack, canonically ordered (`(p0,p1)` with `p1 < p0` swapped). Hash-map A keys → exact intersection = `both` (one representative). Rescue pass: index remaining B-only midpoints in a 1 mm grid; for each remaining A-only seg, search its midpoint's 3×3 neighborhood for an unclaimed B seg with `|lenA - lenB| < 0.2` and midpoint distance ≤ `rescue_radius`; greedy claim → push the A seg into `jitter` (drop the B partner). Remainders → `a_only`/`b_only`.
- [ ] **Step 4: Build + run — PASS**
- [ ] **Step 5: Commit** `git add src/libslic3r/SliceCompare/Diff.hpp src/libslic3r/SliceCompare/Diff.cpp tests/libslic3r/test_slice_compare.cpp && git commit -m "feat(slice-compare): per-layer segment diff with proximity rescue"`

---

### Task 6: File mode — config-block parse + load_snapshot_from_file

**Files:** Modify `Snapshot.hpp/.cpp`; test file.

**Interfaces (produces):**

```cpp
// Parse "; key = value" lines between CONFIG_BLOCK_START/END (Orca/Bambu),
// falling back to PrusaSlicer-style "; key = value" header lines.
std::map<std::string, std::string> parse_gcode_config(const std::string& gcode_path);

// .gcode: GCodeProcessor::process_file -> build_snapshot.
// .3mf / .gcode.3mf: extract Metadata/plate_1.gcode (miniz) to a temp file first.
// Returns nullopt + error message on failure.
struct FileLoadResult { std::optional<Snapshot> snapshot; std::string error; };
FileLoadResult load_snapshot_from_file(const std::string& path);
```

- [ ] **Step 1: Write failing test** (covers .gcode; .3mf is manual-tested in Task 10)

```cpp
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
```

- [ ] **Step 2: Build, verify FAIL**
- [ ] **Step 3: Implement.** `parse_gcode_config`: stream lines; inside CONFIG_BLOCK capture `; k = v`; if no block found, second pass over first+last 400 lines for `; k = v` (skip keys with spaces). `load_snapshot_from_file`: if extension is `.3mf`, open with miniz (`mz_zip_reader_init_file`), find first entry matching `Metadata/plate_*.gcode`, extract to `%TEMP%`, recurse on that path (label from original). For gcode: `GCodeProcessor p; p.process_file(path);` wrap in try/catch, take `p.extract_result()` — NOTE check the accessor: `GCodeProcessor` exposes `extract_result()` (moves the result) or `get_result()`; use whichever exists in `GCodeProcessor.hpp` (grep `Result.*result()` — both variants exist upstream; prefer `extract_result()`). Label = filename stem. Config from `parse_gcode_config` (processor does not expose the parsed block).
- [ ] **Step 4: Build + run — PASS**
- [ ] **Step 5: Commit** `git add src/libslic3r/SliceCompare/Snapshot.hpp src/libslic3r/SliceCompare/Snapshot.cpp tests/libslic3r/test_slice_compare.cpp && git commit -m "feat(slice-compare): external gcode/3mf file loading"`

---

### Task 7: Session capture hook

**Files:**
- Modify: `src/slic3r/GUI/Plater.cpp` — in `Plater::priv::on_process_completed` (~line 15232), after the existing error handling establishes `has_error` (declared ~15300)

**Interfaces:**
- Consumes: `SnapshotStore::instance().add`, `build_snapshot` (Task 1/2)
- Produces: session snapshots labeled `"P<plate+1> · <printer preset> · HH:MM:SS"`

- [ ] **Step 1: Add the hook** (after `has_error` is fully determined and before the function's tail UI updates; guard everything):

```cpp
    // Ultra: capture a Slice Compare snapshot of every successful slice.
    if (!has_error && !evt.cancelled()) {
        try {
            GCodeProcessorResult* res = partplate_list.get_current_slice_result();
            if (res != nullptr && !res->moves.empty()) {
                std::map<std::string, std::string> cfg;
                const DynamicPrintConfig full_cfg = this->background_process.fff_print()->full_print_config();
                for (const std::string& key : full_cfg.keys())
                    if (const ConfigOption* opt = full_cfg.option(key); opt != nullptr)
                        cfg[key] = opt->serialize();
                const std::string label = (boost::format("P%1% · %2% · %3%")
                    % (partplate_list.get_curr_plate_index() + 1)
                    % wxGetApp().preset_bundle->printers.get_selected_preset_name()
                    % wxDateTime::Now().FormatTime().ToStdString()).str();
                SliceCompare::SnapshotStore::instance().add(
                    SliceCompare::build_snapshot(*res, std::move(cfg), label, "session"));
            }
        } catch (...) { BOOST_LOG_TRIVIAL(warning) << "slice-compare snapshot capture failed"; }
    }
```

Add `#include "libslic3r/SliceCompare/Snapshot.hpp"` to Plater.cpp's include block.

- [ ] **Step 2: Full build** (`build_snorca.bat slicer`) — verify compiles clean.
- [ ] **Step 3: Manual check** — launch, slice a plate twice; no crash, log has no capture warning.
- [ ] **Step 4: Commit** `git add src/slic3r/GUI/Plater.cpp && git commit -m "feat(slice-compare): capture snapshot on each successful slice"`

---

### Task 8: SliceCompareFrame — pickers, header, tables + menu items

**Files:**
- Create: `src/slic3r/GUI/SliceCompare/SliceCompareFrame.hpp/.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (add pair near `GUI/Monitor.cpp` line ~305)
- Modify: `src/slic3r/GUI/MainFrame.cpp` — in `init_menubar_as_editor` where `viewMenu` items are appended (insert before `m_menubar->Append(viewMenu, …)` at ~3170):

```cpp
        viewMenu->AppendSeparator();
        append_menu_item(viewMenu, wxID_ANY, _L("Compare Slices") + dots, _L("Compare two sliced results visually"),
            [this](wxCommandEvent&) { Slic3r::GUI::open_slice_compare_frame(this, false); }, "", nullptr,
            []() { return true; }, this);
        append_menu_item(viewMenu, wxID_ANY, _L("Compare with Previous Slice"), _L("Compare the last two slices of this session"),
            [this](wxCommandEvent&) { Slic3r::GUI::open_slice_compare_frame(this, true); }, "", nullptr,
            []() { return true; }, this);
```

**Interfaces (produces):**

```cpp
namespace Slic3r { namespace GUI {
// Singleton-ish opener: raises the existing frame or creates one.
// preselect_last_two: pick the two newest session snapshots as A/B.
void open_slice_compare_frame(wxWindow* parent, bool preselect_last_two);

class SliceCompareFrame : public wxFrame {
public:
    SliceCompareFrame(wxWindow* parent);
    void set_snapshots(std::shared_ptr<const SliceCompare::Snapshot> a,
                       std::shared_ptr<const SliceCompare::Snapshot> b);  // recomputes everything
private:
    void rebuild_pickers();          // SnapshotStore list + "Browse…" entries
    void recompute();                // runs diff_* on m_a/m_b, fills header+tables, notifies canvas
    // wxChoice m_pick_a/m_pick_b; header wxStaticTexts; wxDataViewListCtrl m_cfg_table, m_feat_table;
};
}}
```

Layout (wxBoxSizers): top row = A picker, swap button ("⇄"), B picker; header strip (est time / filament g / layers / max speed, "A → B (Δ)" formatted); `wxNotebook` left (min width 380): "Settings changes" table (Key/A/B columns), "By feature" table (Feature/Δtime/Δgrams/B-time; role names via `ExtrusionEntity.cpp`'s role→string helper — grep `role_to_string`/`extrusion_role_to_string` and use the existing one). Right side: placeholder `wxPanel` for the canvas (Task 9). Frame: 1200×800 default, `wxDEFAULT_FRAME_STYLE`, destroys on close, opener keeps a static `wxWeakRef`/pointer nulled on destroy. Picker "Browse…" entry opens `wxFileDialog` (`*.gcode;*.3mf`), calls `load_snapshot_from_file`, error → `wxMessageBox`, success → snapshot held by the frame (file snapshots live outside the store).

- [ ] **Step 1: Implement frame + opener + menu items** (code above; tables fed from `diff_configs`/`diff_features`).
- [ ] **Step 2: Full build** — clean.
- [ ] **Step 3: Manual test** — slice twice with a tweaked setting (e.g. wall_loops 2→3): View → Compare with Previous Slice shows both snapshots preselected, config table lists wall_loops 2 vs 3, feature table shows wall Δ; Browse… loads an exported .gcode.
- [ ] **Step 4: Commit** `git add src/slic3r/GUI/SliceCompare/SliceCompareFrame.hpp src/slic3r/GUI/SliceCompare/SliceCompareFrame.cpp src/slic3r/CMakeLists.txt src/slic3r/GUI/MainFrame.cpp && git commit -m "feat(slice-compare): compare frame with pickers, stats and config tables"`

---

### Task 9: Overlay canvas + layer slider + jump-to-change

**Files:**
- Create: `src/slic3r/GUI/SliceCompare/CompareCanvas.hpp/.cpp`
- Modify: `src/slic3r/CMakeLists.txt` (add pair), `SliceCompareFrame.cpp` (mount canvas + slider)

**Interfaces (produces):**

```cpp
class CompareCanvas : public wxPanel {
public:
    CompareCanvas(wxWindow* parent);
    void set_layers(const SliceCompare::LayerRec* a,
                    const SliceCompare::LayerRec* b);   // either may be null; recomputes SegDiff
    void set_side_by_side(bool on);                      // Task 10
    void fit_view();                                     // zoom-to-content
private:
    // wxAutoBufferedPaintDC painting; world(mm)->screen: m_scale (px/mm), m_pan;
    // mouse wheel = zoom about cursor, left-drag = pan; EVT_PAINT/EVT_MOUSEWHEEL/EVT_MOTION.
};
```

Rendering rules (from spec): background #111; draw order both → jitter → a_only → b_only; colors both `#9E9E9E` @ 35% alpha (`wxGraphicsContext` for alpha — create from the buffered DC), jitter `#2E7D32`, a_only `#1565C0`, b_only `#C62828`; pen widths 1 px (both/jitter) and 2 px (diffs); Y flipped (gcode +Y up). Collinear merge before painting: consecutive same-class segments sharing an endpoint with cross-product < 1e-3 merge into one polyline point list.

Frame additions: `wxSlider` (vertical, right edge) over the union of matched+unmatched z keys (index into a sorted vector of `(zkey_a, zkey_b)` rows from `diff_layers`); slider label "z=X.XX"; changed layers marked in a thin custom strip next to the slider (paint colored ticks); "Jump to biggest change" button selects `LayerDiff::biggest_zkey_a`'s row; status line under canvas: `Δt=+X.Xs  Δe=+X.XXg  overlap=0.NN  [FLAGS]` for the current row, or the layer-height-mismatch banner text when `matched == 0 && (a_only+b_only) > 0`: "Layer heights differ — showing coincident layers only; see Settings/Feature tabs".

- [ ] **Step 1: Implement canvas + wire into frame** (slider EVT_SLIDER → `set_layers` with the row's LayerRecs; recompute SegDiff lazily per shown layer).
- [ ] **Step 2: Full build** — clean.
- [ ] **Step 3: Manual test** — same-plate A/B: overlay mostly gray/green; tweak infill density: red/blue texture difference; slider markers align with changed layers; jump button lands on the biggest Δ; zoom/pan smooth on a dense top-solid layer.
- [ ] **Step 4: Commit** `git add src/slic3r/GUI/SliceCompare/CompareCanvas.hpp src/slic3r/GUI/SliceCompare/CompareCanvas.cpp src/slic3r/GUI/SliceCompare/SliceCompareFrame.cpp src/slic3r/CMakeLists.txt && git commit -m "feat(slice-compare): overlay canvas with layer slider and jump-to-change"`

---

### Task 10: Side-by-side toggle + guards + final pass

**Files:** Modify `CompareCanvas.cpp` (+hpp), `SliceCompareFrame.cpp`.

- [ ] **Step 1: Side-by-side mode** — `set_side_by_side(true)`: canvas splits client rect into two viewports sharing scale/pan (camera transform applied per pane, A left/B right, pane captions "A"/"B"); each pane draws its own layer's segments feature-neutral (all `#9E9E9E` full alpha) — differences are for overlay mode; toolbar `wxToggleButton` "Side by side".
- [ ] **Step 2: Guards** — session picker with <2 snapshots shows a hint line "Slice something (or Browse…) to compare"; snapshot evicted while selected: `SnapshotStore::get` null → picker rebuild + clear selection; file parse failure keeps frame usable (already in Task 8); dense-layer merge verified (top solid infill layer paints < 100 ms — log paint time once at debug).
- [ ] **Step 3: Full build + run all `[slice_compare]` tests** — all PASS.
- [ ] **Step 4: Manual checklist (from spec Testing):**
  - re-slice same plate unchanged → overlay ~all gray/green;
  - preset A/B on one plate → config diff + colored overlay agree with the change;
  - two external Orca .gcode files → loads, compares;
  - a `gcode.3mf` export → loads via miniz path;
  - 0.20 vs 0.16 layer heights → banner shown, tables still populated, no crash.
- [ ] **Step 5: Commit** `git add src/slic3r/GUI/SliceCompare/CompareCanvas.hpp src/slic3r/GUI/SliceCompare/CompareCanvas.cpp src/slic3r/GUI/SliceCompare/SliceCompareFrame.cpp && git commit -m "feat(slice-compare): side-by-side mode and edge-case guards"`
