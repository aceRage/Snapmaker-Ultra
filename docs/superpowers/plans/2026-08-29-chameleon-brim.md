# Chameleon Brim (Part 1) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Opt-in `brim_filament_source = nearest_wall`: brim extrusions are partitioned per extruder by nearest-layer-0-wall vote, so merged and multi-color brims match what they touch.

**Architecture:** A layer-generic `WallSampleIndex` (2 mm grid of wall sample points) + pure vote/split/guard functions feed a partitioning pass after `make_brim`: each object's brim collection is split into an own-extruder part (stays in `m_brimMap` — today's print path, byte-identical when off) and foreign-extruder parts in a new `Print::m_brimMapByExtruder`, which GCode prints at the top of the per-extruder loop and ToolOrdering registers as layer-0 extruders (spike-proven mechanism).

**Tech Stack:** C++17, libslic3r (Point/ExPolygon/ExtrusionEntity), Catch2 tests, spike CLI harness for gcode-level assertions.

**Spec:** `docs/superpowers/specs/2026-08-29-chameleon-brim-design.md` (this worktree). Spike evidence: `spike/FINDINGS.md`.

## Global Constraints

- Repo: `C:\Dev\SnapmakerOrcaSupports` (worktree, branch `feat/color-matched-supports`). NEVER touch `C:\Dev\SnapmakerOrca` (parallel sessions).
- Build: `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\2e201e48-473f-4a73-881b-80bbcd86e8cc\scratchpad\build_supports_wt.bat` (full), or `cmake --build build --config Release --target libslic3r_tests` from repo root for tests. Run: `build\tests\libslic3r\Release\libslic3r_tests.exe "[chameleon]"`.
- BEFORE any build: PowerShell `Get-Process cl,link,MSBuild -ErrorAction SilentlyContinue`; if busy wait 60s and re-check (other sessions build the sibling checkout).
- **The spike code must be removed first (Task 0)** — no `SPIKE_SPLIT_BRIM` may remain when feature work lands.
- Constants (spec, verbatim): wall/brim sample spacing 0.8 mm; grid cell 2 mm; k=3 inverse-square vote; tie-break when top-two scores differ < 30% or nearest distances differ < 0.3 mm → larger layer-0 object area, then lower extruder id; runs < 2 mm absorbed; guard cap 4 runs per object brim.
- Off-mode requirement: `brim_filament_source=object` (default) or single configured extruder ⇒ byte-identical gcode to pre-feature baseline.
- All coordinates scaled (`Point`, `scale_(mm)`); extruder ids 0-based internally (config values are 1-based; resolve as `value > 0 ? value - 1 : object_default`).
- Commits: stage only files each task names (never `git add -A`).
- Tests tag: `"[chameleon]"`. Header guards `#ifndef slic3r_WallSampleIndex_hpp_` style.

## File Structure

| File | Responsibility |
|---|---|
| Create `src/libslic3r/WallSampleIndex.hpp/.cpp` | Grid index of `(Point, extruder, object_id)`; build from sampled polylines; k-NN query |
| Create `src/libslic3r/BrimFilament.hpp/.cpp` | Pure functions: sample paths, vote, split-to-runs, absorb, guard-coalesce, partition a brim collection |
| Create `tests/libslic3r/test_chameleon_brim.cpp` | All unit tests, tag `[chameleon]` |
| Modify `src/libslic3r/PrintConfig.cpp/.hpp` | `brim_filament_source` enum option + map |
| Modify `src/libslic3r/Print.hpp` (~1110) / `Print.cpp` (~2626) | `m_brimMapByExtruder` member + clear + partition call after `make_brim` |
| Modify `src/libslic3r/GCode/ToolOrdering.cpp` | Union brim-assigned extruders into first-layer `LayerTools` |
| Modify `src/libslic3r/GCode.cpp` (per-extruder loop top ~6305, spike blocks removed) | Print foreign brim partitions under their extruder |
| Modify `src/libslic3r/CMakeLists.txt`, `tests/libslic3r/CMakeLists.txt` | Register new files |

---

### Task 0: Remove spike code

**Files:**
- Modify: `src/libslic3r/GCode.cpp` (all `SPIKE_SPLIT_BRIM` blocks: the `#define` at top, brim split ~6540-6570, `set_extruder` instrumentation, interface split in `extrude_support`, forced ids in `GCode::set_extruders`)

**Interfaces:** none — pure removal. The spike branch history preserves the probes.

- [ ] **Step 1:** Delete every `#ifdef SPIKE_SPLIT_BRIM`...`#endif` block and the top-of-file `#define SPIKE_SPLIT_BRIM 1` + its comment. Restore the plain brim loop:

```cpp
                            for (const ExtrusionEntity* ee : print.m_brimMap.at(instance_to_print.print_object.id()).entities) {
                                gcode += this->extrude_entity(*ee, "brim", m_config.support_speed.value);
                            }
```
and the plain `void GCode::set_extruders(...) { m_writer.set_extruders(extruder_ids); ... }`.
- [ ] **Step 2:** `grep -n "SPIKE" src/libslic3r/GCode.cpp` → zero hits.
- [ ] **Step 3:** Full build (wrapper bat) — exit 0.
- [ ] **Step 4:** Regenerate the baseline: run the spike harness slice (two-cube command from `spike/FINDINGS.md` "Harness notes", using `spike/spike_process_overrides.json`) and save output as `spike/out/baseline_clean.gcode`. Verify `grep -cE "^M620 S" spike/out/baseline_clean.gcode` = 2.
- [ ] **Step 5:** Commit:
```bash
git add src/libslic3r/GCode.cpp spike/out/baseline_clean.gcode
git commit -m "chore(chameleon): remove spike probes; record clean baseline gcode"
```

---

### Task 1: WallSampleIndex

**Files:**
- Create: `src/libslic3r/WallSampleIndex.hpp`, `src/libslic3r/WallSampleIndex.cpp`
- Modify: `src/libslic3r/CMakeLists.txt` (add both near `Brim.cpp`), `tests/libslic3r/CMakeLists.txt` (add `test_chameleon_brim.cpp`)
- Test: `tests/libslic3r/test_chameleon_brim.cpp`

**Interfaces (produces):**

```cpp
#ifndef slic3r_WallSampleIndex_hpp_
#define slic3r_WallSampleIndex_hpp_
#include "Point.hpp"
#include <cstdint>
#include <map>
#include <vector>
namespace Slic3r {

struct WallSample { Point pt; unsigned extruder; size_t object_key; };

class WallSampleIndex {
public:
    // cell_mm: grid cell size (default 2.0). Samples added via add_polyline.
    explicit WallSampleIndex(double cell_mm = 2.0);
    // Sample the polyline every spacing_mm (default 0.8) and insert points.
    void add_polyline(const Points& poly, unsigned extruder, size_t object_key,
                      double spacing_mm = 0.8);
    // k nearest samples to pt (by expanding ring search over grid cells).
    // Returns up to k samples sorted by squared distance ascending, ties by
    // (extruder, object_key) for determinism.
    std::vector<std::pair<const WallSample*, double>> knn(const Point& pt, size_t k) const;
    size_t size() const;
    bool   empty() const;
private:
    double m_cell; // scaled units
    std::map<std::pair<int32_t,int32_t>, std::vector<WallSample>> m_cells;
    size_t m_count = 0;
};
} // namespace Slic3r
#endif
```

- [ ] **Step 1: Write the failing tests** (`tests/libslic3r/test_chameleon_brim.cpp`):

```cpp
#include <catch2/catch.hpp>
#include "libslic3r/WallSampleIndex.hpp"
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
```

- [ ] **Step 2:** Add files to both CMakeLists; build `libslic3r_tests` → FAIL (missing header/symbols).
- [ ] **Step 3: Implement.** `add_polyline`: walk consecutive point pairs; emit points every `scale_(spacing_mm)` along each segment (include segment start; include final endpoint); insert each into `m_cells[{floor(x/cell), floor(y/cell)}]` with `m_cell = scale_(cell_mm)`. `knn`: search the cell of `pt`, then expand ring radius r=1,2,... collecting candidates until at least k found AND the next ring's minimum possible distance exceeds the current k-th best (standard grid knn); final sort by `(d2, extruder, object_key)`, truncate to k. `size/empty` from `m_count`.
- [ ] **Step 4:** Build + run `libslic3r_tests.exe "[chameleon]"` → PASS.
- [ ] **Step 5:** Commit:
```bash
git add src/libslic3r/WallSampleIndex.hpp src/libslic3r/WallSampleIndex.cpp \
        src/libslic3r/CMakeLists.txt tests/libslic3r/CMakeLists.txt tests/libslic3r/test_chameleon_brim.cpp
git commit -m "feat(chameleon): WallSampleIndex grid k-NN over wall samples"
```

---

### Task 2: Vote, split, absorb, guard (pure functions)

**Files:**
- Create: `src/libslic3r/BrimFilament.hpp`, `src/libslic3r/BrimFilament.cpp`
- Modify: `src/libslic3r/CMakeLists.txt` (add pair)
- Test: `tests/libslic3r/test_chameleon_brim.cpp` (append)

**Interfaces (produces):**

```cpp
#ifndef slic3r_BrimFilament_hpp_
#define slic3r_BrimFilament_hpp_
#include "WallSampleIndex.hpp"
#include "ExtrusionEntity.hpp"
#include <map>
namespace Slic3r {

struct BrimVoteParams {
    size_t k = 3;
    double tie_score_ratio = 0.30;      // top-two scores differ < 30% => tie path
    double tie_dist_mm     = 0.3;       // nearest distances differ < 0.3mm => tie path
    double sample_mm       = 0.8;
    double min_run_mm      = 2.0;       // shorter runs absorbed
    size_t max_runs        = 4;         // guard cap per object brim
    // object_key -> layer-0 area (for tie-break 1); larger area wins
    std::map<size_t, double> object_area;
    unsigned fallback_extruder = 0;     // used when index empty / no candidates
};

// Vote for one point. Deterministic. Returns extruder id.
unsigned brim_vote(const WallSampleIndex& idx, const Point& pt, const BrimVoteParams& p);

// One contiguous same-extruder piece of a source polyline.
struct BrimRun { unsigned extruder; Points pts; };

// Sample `poly` (closed if is_loop), vote per sample, group runs, absorb runs
// shorter than min_run_mm into the previous run, then coalesce smallest runs
// until <= max_runs. Result covers the whole polyline in order.
std::vector<BrimRun> split_polyline_by_vote(const Points& poly, bool is_loop,
                                            const WallSampleIndex& idx,
                                            const BrimVoteParams& p);
} // namespace Slic3r
#endif
```

- [ ] **Step 1: Write the failing tests** (append):

```cpp
#include "libslic3r/BrimFilament.hpp"

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
```

- [ ] **Step 2:** Build → FAIL (missing BrimFilament).
- [ ] **Step 3: Implement.** `brim_vote`: knn(k); empty → fallback. Accumulate `score[extruder] += 1/max(d2, epsilon)` (epsilon = scaled 0.01mm²). Winner = max score; find runner-up; if `runner >= winner * (1 - tie_score_ratio)` OR `|d_nearest(winner) - d_nearest(runner)| < scale_(tie_dist_mm)`: pick by `object_area` of each side's nearest sample's `object_key` (larger wins), then lower extruder id. `split_polyline_by_vote`: resample the polyline at `sample_mm` (reuse index sampling helper or a local lambda), vote each sample, group consecutive equal votes into runs carrying their sample points (append original segment endpoints so geometry is preserved: build each run's pts from the resampled chain — acceptable per spec since 0.8 mm sampling ≤ brim resolution). Absorb: any run with path length < `min_run_mm` merges into the PREVIOUS run (first run merges forward). Guard: while runs.size() > max_runs, find the shortest run, merge into its longer neighbor. Merge = concatenate pts, keep neighbor's extruder.
- [ ] **Step 4:** Build + run `[chameleon]` → PASS.
- [ ] **Step 5:** Commit:
```bash
git add src/libslic3r/BrimFilament.hpp src/libslic3r/BrimFilament.cpp \
        src/libslic3r/CMakeLists.txt tests/libslic3r/test_chameleon_brim.cpp
git commit -m "feat(chameleon): nearest-wall vote, run splitting, absorb + guard"
```

---

### Task 3: Config key + Print storage + partition pass

**Files:**
- Modify: `src/libslic3r/PrintConfig.hpp` (add `enum BrimFilamentSource { bfsObject = 0, bfsNearestWall };` near `BrimType`; add `CONFIG_OPTION_ENUM_DECLARE_STATIC_MAPS(BrimFilamentSource)` in the declare block)
- Modify: `src/libslic3r/PrintConfig.cpp` (enum map + option def near the `brim_type` def; add `"brim_filament_source"` to the same options group list `brim_type` belongs to — grep `"brim_type"` in Preset.cpp print options and add alongside)
- Modify: `src/libslic3r/Preset.cpp` (print options list containing `"brim_type"`: add `"brim_filament_source"`)
- Modify: `src/libslic3r/Print.hpp` (~1110): add member + getter; `src/libslic3r/Print.cpp` (~2626): clear + partition call
- Create: partition function in `BrimFilament.hpp/.cpp` (this task extends Task 2's files)
- Test: `tests/libslic3r/test_chameleon_brim.cpp` (append partition test)

**Interfaces (produces):**

```cpp
// PrintConfig.cpp map:
static t_config_enum_values s_keys_map_BrimFilamentSource {
    { "object",       int(BrimFilamentSource::bfsObject) },
    { "nearest_wall", int(BrimFilamentSource::bfsNearestWall) }
};
CONFIG_OPTION_ENUM_DEFINE_STATIC_MAPS(BrimFilamentSource)

// option def (labels): "Brim filament" / tooltip: "object: brim uses each
// object's filament (default). nearest_wall: each brim extrusion uses the
// filament of the nearest first-layer wall, so brims match what they touch
// (multi-filament prints only)."  mode comAdvanced, default bfsObject.

// Print.hpp member (next to m_brimMap):
std::map<ObjectID, std::map<unsigned int, ExtrusionEntityCollection>> m_brimMapByExtruder;
const std::map<ObjectID, std::map<unsigned int, ExtrusionEntityCollection>>& get_brimMapByExtruder() { return m_brimMapByExtruder; }

// BrimFilament.hpp addition:
// Partition `brim` (one object's collection, plate coords): entities whose
// dominant vote == own_extruder stay in `kept`; others land in out[extruder].
// Loop/path entities are split via split_polyline_by_vote; runs become
// ExtrusionPaths (erBrim) copying the source entity's flow attributes.
void partition_brim_by_wall(const ExtrusionEntityCollection& brim,
                            unsigned own_extruder,
                            const WallSampleIndex& idx,
                            const BrimVoteParams& p,
                            ExtrusionEntityCollection& kept,
                            std::map<unsigned, ExtrusionEntityCollection>& out);
```

- [ ] **Step 1: Write the failing partition test** (uses real ExtrusionLoop):

```cpp
#include "libslic3r/ExtrusionEntityCollection.hpp"

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
```

- [ ] **Step 2:** Build → FAIL (missing partition + enum).
- [ ] **Step 3: Implement.** Enum + option + Preset list entry. `partition_brim_by_wall`: for each entity, gather its full point chain (loops: closed polyline; paths/multipaths: polyline; collections: recurse). Fast path: vote entity's samples; if every vote == own_extruder → push the ORIGINAL entity clone into `kept` unmodified (preserves loops → off-behavior parity). Else run `split_polyline_by_vote`; for each run build `ExtrusionPath(erBrim, source mm3_per_mm, width, height)` with `polyline = run.pts`; runs with extruder == own_extruder go to `kept`, others to `out[extruder]`. In `Print::process` after `make_brim` (~2631): when `config().brim_filament_source == bfsNearestWall` and `this->extruders().size() > 1`: build the index (next bullet), compute per-object layer-0 areas, then for each `m_brimMap` entry partition in place (`kept` replaces the map value; foreign parts into `m_brimMapByExtruder`). Index build: for each object + each instance shift: layer 0 `layer->regions()`: for each `LayerRegion* lr`, walk `lr->perimeters.entities` recursively; for each ExtrusionPath with role erExternalPerimeter use the region's outer wall filament (`region.config().outer_wall_filament.value > 0 ? ... - 1 : wall_default`), else `wall_filament.value > 0 ? value-1 : object default extruder`; `add_polyline(path.polyline.points shifted by instance.shift, ext, object_key)`. Own extruder for partition = the object's brim extruder from `objPrintVec` (the pair vector passed to make_brim — expose or recompute: use `object->config().extruder`-style default via `print.get_object_extruders`? Simplest correct: reuse the same value make_brim used — extend `make_brim`'s caller loop in Print.cpp where `objPrintVec` is in scope, doing the partition there). `m_brimMapByExtruder.clear()` beside `m_brimMap.clear()` (~2626).
- [ ] **Step 4:** Build + run `[chameleon]` → PASS. Full app build → exit 0.
- [ ] **Step 5:** Commit:
```bash
git add src/libslic3r/PrintConfig.hpp src/libslic3r/PrintConfig.cpp src/libslic3r/Preset.cpp \
        src/libslic3r/Print.hpp src/libslic3r/Print.cpp \
        src/libslic3r/BrimFilament.hpp src/libslic3r/BrimFilament.cpp tests/libslic3r/test_chameleon_brim.cpp
git commit -m "feat(chameleon): brim_filament_source config + brim partition pass"
```

---

### Task 4: ToolOrdering hook + GCode emission + integration gcode test

**Files:**
- Modify: `src/libslic3r/GCode/ToolOrdering.cpp` — in the `ToolOrdering::ToolOrdering(const Print& print, ...)` constructor (the whole-print one, ~L500 region), after layers are collected: for the FIRST layer's `LayerTools`, union in every extruder key of `print.m_brimMapByExtruder` (use `tools_for_layer(first print_z)`; insert into `.extruders`, keep sorted-unique like the surrounding code does).
- Modify: `src/libslic3r/GCode.cpp` — top of the per-extruder loop (`for (unsigned int extruder_id : layer_extruders)`, ~L6305), after the skirt block, add:

```cpp
        // Chameleon brim: print this extruder's foreign brim partitions (layer 0 only).
        if (first_layer && !print.get_brimMapByExtruder().empty()) {
            for (const auto& obj_entry : print.get_brimMapByExtruder()) {
                auto it = obj_entry.second.find(extruder_id);
                if (it == obj_entry.second.end() || it->second.entities.empty())
                    continue;
                this->set_origin(0., 0.);
                m_avoid_crossing_perimeters.use_external_mp();
                for (const ExtrusionEntity* ee : it->second.entities)
                    gcode += this->extrude_entity(*ee, "brim", m_config.support_speed.value);
                m_avoid_crossing_perimeters.use_external_mp(false);
                m_avoid_crossing_perimeters.disable_once();
            }
        }
```
  (`first_layer` bool exists in scope in process_layer; verify the exact local name — grep `bool first_layer` in the function — and use it.)
- Test: gcode-level via the spike harness (no unit test — GCode is not unit-testable here).

**Interfaces:**
- Consumes: `print.get_brimMapByExtruder()` (Task 3), `m_brimMapByExtruder` populated only in nearest_wall mode.

- [ ] **Step 1:** Implement both modifications.
- [ ] **Step 2:** Full build → exit 0.
- [ ] **Step 3: Integration test (manual-scripted, record in commit message):** create `spike/spike_chameleon_overrides.json` = copy of `spike_process_overrides.json` plus `"brim_filament_source": "nearest_wall"` and `"brim_object_gap": "0"`. Slice the two-cube fixture placed adjacent (the arrange puts them apart — pass `--arrange 0` if supported, else accept the arranged plate: with PLA+PETG loaded and objects on both filaments unavailable via CLI, assert instead on the single-object multi-wall case): **minimum assertion set** — (a) exit 0; (b) `grep -cE "^M620 S"` ≥ baseline (2) and ≤ 2 + 2×4 (guard bound); (c) gcode contains `; FEATURE: Brim` and the file loads in the GUI preview without error; (d) with `"brim_filament_source": "object"` the output is byte-identical to `spike/out/baseline_clean.gcode` (modulo `; generated by`/`; at` header lines).
- [ ] **Step 4:** If (d) fails, fix before committing — off-mode purity is a spec requirement.
- [ ] **Step 5:** Commit:
```bash
git add src/libslic3r/GCode/ToolOrdering.cpp src/libslic3r/GCode.cpp spike/spike_chameleon_overrides.json
git commit -m "feat(chameleon): register brim extruders in ToolOrdering + emit foreign partitions"
```

---

### Task 5: Off-mode byte-identity regression + determinism + GUI wiring check

**Files:**
- Create: `spike/verify_chameleon.sh` (the Task 4 assertions as a repeatable script)
- Modify: `src/slic3r/GUI/Tab.cpp` — add `optgroup->append_single_option_line("brim_filament_source");` beside the existing `"brim_type"` line in the process/others brim group (grep `append_single_option_line("brim_type")`).

- [ ] **Step 1:** Write `spike/verify_chameleon.sh`: slices the fixture twice in `object` mode → byte-compare vs `baseline_clean.gcode` and vs each other (determinism); once in `nearest_wall` mode → M620 bounds check + two consecutive nearest_wall slices byte-identical. Exit non-zero on any failure, print a PASS/FAIL table.
- [ ] **Step 2:** Run it → all PASS.
- [ ] **Step 3:** Full build with the Tab.cpp line; launch the app manually is NOT required — confirm compile only (GUI visual check is the user's manual validation step).
- [ ] **Step 4:** Commit:
```bash
git add spike/verify_chameleon.sh src/slic3r/GUI/Tab.cpp
git commit -m "feat(chameleon): UI option + off-mode/determinism verification script"
```
- [ ] **Step 5:** Report: feature ready for GUI validation (user slices a multi-color plate with nearest_wall and inspects the preview) and physical AMS test.
