# Colour Split Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** "Split → By painted colour": turn a part's MMU paint into exact, in-place solid parts (one per painted filament, normal thickness D, plus the body) inside the Snapmaker-Ultra slicer.

**Architecture:** A new libslic3r module `ColorSplit` extracts T-joint-free painted patches from the volume's `TriangleSelector` data, extrudes each patch inward into a closed shell (per-vertex depth = min(D, half local thickness), flat cap, wall-stack step at creases), validates shells (closed + CGAL self-intersection check), and partitions the original mesh with sequential `Manifold::Split` (exact tiling, enclosed body islands absorbed). A plater `Job` runs it off-thread; `apply_color_split` replaces the source volume by new volumes (body first, colour parts after). Tasks 1–4 are also the feasibility spike: their `[colorsplit_spike]` measurements gate the rest.

**Tech Stack:** C++17, libslic3r (TriangleSelector, AABBMesh, NormalUtils, Flow, PaintDepth helpers, MeshBoolean::cgal), Manifold 3.5.2 (`<manifold/manifold.h>`, linked PRIVATE into libslic3r), wxWidgets GUI, Catch2 tests, MSVC 2022 + CMake 3.31 (portable, `C:\Dev\tools`).

**Spec:** `docs/superpowers/specs/2026-09-01-color-split-design.md` (rev 2.1) — binding. Research and verified code facts: `docs/superpowers/specs/2026-09-01-color-split-research.md`.

## Global Constraints

- Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/color-split` (base main dff2c65eab). Never touch `C:\Dev\SnapmakerOrca` or other worktrees.
- Build slot protocol: before any build run `powershell -NoProfile -Command "Get-Process cl,link,MSBuild -ErrorAction SilentlyContinue | Select-Object Name,Id,CPU"`; only `cl`/`link` processes with rising CPU are contention (idle MSBuild node-reuse workers are not). If busy, wait and re-check; do NOT set up background monitors or wait loops. Set `MSBUILDDISABLENODEREUSE=1` (the wrappers do). Never edit source after the task's final build.
- Build wrappers (create them in the session scratchpad if missing; content shown in Task 1).
- Test executable: `C:\Dev\SnapmakerOrcaNext\build\tests\libslic3r\Release\libslic3r_tests.exe`; new tests use tags `[colorsplit]` (kept) and `[colorsplit_spike]` (measurements, kept but may be `[!mayfail]`-free — they must pass, they only report numbers with `WARN`).
- Existing suites must stay green: `[paintdepth]` (94 cases), `[chameleon]` (133 cases), then the full `libslic3r_tests` run once before the final commit.
- Commit prefix `feat(color-split):` / `test(color-split):` / `docs(color-split):`; every commit ends with `Co-Authored-By: Claude Fable 5.1 <noreply@anthropic.com>`.
- Painted state value == 1-based filament id; NONE (0) = the volume's own extruder. Depth D, ws, h are world millimetres.
- No placeholder behaviour: every failure path returns `ColorSplitError` with a user-readable message (§7 of the spec).
- Floating-point assertions use Catch2 matchers (`REQUIRE_THAT(x, WithinRel(expected, rel))` / `WithinAbs(expected, abs)` with `using Catch::Matchers::WithinRel; using Catch::Matchers::WithinAbs;` and `#include <catch2/matchers/catch_matchers_floating_point.hpp>`), never `Approx` — `tests/CLAUDE.md` §4 forbids it. `its_volume` is float32: use relative tolerances ≥ 1e-4 on volumes.
- Real TDD: each test must be seen failing (compile error or assertion) before the implementation step.

---

## File structure

| File | Responsibility |
|---|---|
| `src/libslic3r/ColorSplit.hpp` | Public API: depth model, patches, shells, partition, one-shot split, model mutation. No Manifold/CGAL includes. |
| `src/libslic3r/ColorSplit.cpp` | Patches, depths, normals, pipeline (`split_volume_by_paint`). From Task 5 on: `ColorSplitShell.cpp` (smooth-patch groups + ShellBuilder), `ColorSplitPartition.cpp` (Manifold partition), `ColorSplitInternal.hpp` (shared internals). |
| `src/libslic3r/CMakeLists.txt` | Register the two files in the main `lisbslic3r_sources` list after `PaintDepth.cpp` (line ~87), not in the `libslic3r_cgal` list. |
| `tests/libslic3r/test_color_split.cpp` | All `[colorsplit]` / `[colorsplit_spike]` tests + fixtures. |
| `tests/libslic3r/CMakeLists.txt` | Register the test file after `test_paint_depth_clamp.cpp`. |
| `src/slic3r/GUI/Jobs/ColorSplitJob.hpp/.cpp` | Plater job: off-thread split, finalize = snapshot + apply + list refresh. |
| `src/slic3r/GUI/ColorSplitDialog.hpp/.cpp` | Modal options dialog. |
| `src/slic3r/GUI/Plater.hpp/.cpp` | `split_by_color()`, `can_split_by_color()`. |
| `src/slic3r/GUI/GUI_Factories.cpp` | Menu items. |
| `src/slic3r/CMakeLists.txt` | Register job + dialog sources (near `GUI/Jobs/EmbossJob.cpp`, line ~265). |

---

### Task 1: Build gate, module skeleton, patch extraction

**Files:**
- Create: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `tests/libslic3r/test_color_split.cpp`
- Modify: `src/libslic3r/CMakeLists.txt:87` (add `ColorSplit.cpp` and `ColorSplit.hpp` to the MAIN `lisbslic3r_sources` list right after `PaintDepth.cpp` — NOT the `libslic3r_cgal` list at :506-512, which has no OpenCASCADE/Model include path; libslic3r links Manifold and libslic3r_cgal, which Tasks 2/4/6 need), `tests/libslic3r/CMakeLists.txt:9` (add `test_color_split.cpp` after `test_paint_depth_clamp.cpp`)

**Interfaces:**
- Produces: `struct ColorPatches { indexed_triangle_set surface; std::vector<int> facet_state; std::vector<int> states; }`, `ColorPatches extract_color_patches(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint)` (throws `ColorSplitError`), `class ColorSplitError : public std::runtime_error`.
- Test fixtures used by every later task: `paint_data(...)`, `cube_facets` table, `make_grid_box`.

- [ ] **Step 1: Rebuild the worktree on the new branch (build gate)**

Create `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad\build_next_wt_tests.bat` if missing:

```bat
@echo off
setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat"
set MSBUILDDISABLENODEREUSE=1
cd /d C:\Dev\SnapmakerOrcaNext\build
"C:\Dev\tools\cmake-3.31.8-windows-x86_64\bin\cmake.exe" --build . --config Release --target libslic3r_tests
```

and `build_next_wt.bat` identical except `--target ALL_BUILD`. Check the build slot (Global Constraints), then run `build_next_wt_tests.bat`. The tree changed branch (paint-depth merge + main), so expect a long incremental build. Exit code 0 is the gate. Then run the baseline:

```
C:\Dev\SnapmakerOrcaNext\build\tests\libslic3r\Release\libslic3r_tests.exe "[paintdepth]"
```
Expected: all 94 cases pass.

- [ ] **Step 2: Write the failing patch-extraction tests**

`tests/libslic3r/test_color_split.cpp`:

```cpp
#include <catch2/catch.hpp>
#include <libslic3r/ColorSplit.hpp>
#include <libslic3r/TriangleMesh.hpp>
#include <libslic3r/TriangleSelector.hpp>
#include <libslic3r/Model.hpp>
#include <libslic3r/Print.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/MeshBoolean.hpp>
#include <chrono>

using namespace Slic3r;

// Facet indices of its_make_cube(x, y, z) (same table as test_paint_depth_clamp.cpp:39-52):
// 0,1 = bottom (-Z); 2,3 = top (+Z); 4,5 = +X; 6,7 = +Y; 8,9 = -X; 10,11 = -Y.
static const std::vector<int> CUBE_TOP    = {2, 3};
static const std::vector<int> CUBE_BOTTOM = {0, 1};
static const std::vector<int> CUBE_PLUS_X = {4, 5};
static const std::vector<int> CUBE_SIDES  = {4, 5, 6, 7, 8, 9, 10, 11};

// Paint the given facets of `mesh` with `state` and return the serialized paint data,
// exactly what ModelVolume::mmu_segmentation_facets.get_data() would hold.
static TriangleSelector::TriangleSplittingData paint_data(const TriangleMesh &mesh,
                                                          const std::vector<std::pair<int, EnforcerBlockerType>> &facets)
{
    TriangleSelector selector(mesh);
    for (auto [facet, state] : facets)
        selector.set_facet(facet, state);
    return selector.serialize();
}

static std::vector<std::pair<int, EnforcerBlockerType>> all_with(const std::vector<int> &facets, EnforcerBlockerType st)
{
    std::vector<std::pair<int, EnforcerBlockerType>> out;
    for (int f : facets) out.emplace_back(f, st);
    return out;
}

// Paint every facet whose centroid/normal satisfies `pred` (for meshes whose facet order is not known,
// e.g. boolean results). `pred(centroid, normal)`.
template<class Pred>
static TriangleSelector::TriangleSplittingData paint_by_predicate(const TriangleMesh &mesh, Pred pred, EnforcerBlockerType st)
{
    TriangleSelector selector(mesh);
    const indexed_triangle_set &its = mesh.its;
    for (int f = 0; f < int(its.indices.size()); ++f) {
        const Vec3f a = its.vertices[its.indices[f][0]], b = its.vertices[its.indices[f][1]], c = its.vertices[its.indices[f][2]];
        const Vec3f n = (b - a).cross(c - a).normalized();
        if (pred((a + b + c) / 3.f, n))
            selector.set_facet(f, st);
    }
    return selector.serialize();
}

// A box x*y*z whose TOP face is an nx*ny grid (so paint can touch at a single vertex);
// vertices: bottom 4 corners then the (nx+1)*(ny+1) top grid; all faces CCW outward.
static TriangleMesh make_grid_box(double x, double y, double z, int nx, int ny)
{
    indexed_triangle_set its;
    auto V = [&](double px, double py, double pz) { its.vertices.emplace_back(float(px), float(py), float(pz)); return int(its.vertices.size()) - 1; };
    const int b0 = V(0, 0, 0), b1 = V(x, 0, 0), b2 = V(x, y, 0), b3 = V(0, y, 0);
    std::vector<int> top((nx + 1) * (ny + 1));
    for (int j = 0; j <= ny; ++j)
        for (int i = 0; i <= nx; ++i)
            top[j * (nx + 1) + i] = V(x * i / nx, y * j / ny, z);
    auto T = [&](int a, int b, int c) { its.indices.emplace_back(a, b, c); };
    T(b0, b2, b1); T(b0, b3, b2);                                   // bottom (-Z)
    for (int j = 0; j < ny; ++j)                                    // top grid (+Z)
        for (int i = 0; i < nx; ++i) {
            int p = top[j * (nx + 1) + i], q = top[j * (nx + 1) + i + 1], r = top[(j + 1) * (nx + 1) + i + 1], s = top[(j + 1) * (nx + 1) + i];
            T(p, q, r); T(p, r, s);
        }
    // sides: bottom edge -> top grid edge (fans against the grid's edge vertices)
    auto side = [&](int bA, int bB, const std::vector<int> &edge) {           // edge runs from above bA to above bB
        T(bA, bB, edge.front());
        for (size_t k = 0; k + 1 < edge.size(); ++k) T(bB, edge[k + 1], edge[k]);
    };
    std::vector<int> e_front, e_right, e_back, e_left;
    for (int i = 0; i <= nx; ++i) e_front.push_back(top[i]);
    for (int j = 0; j <= ny; ++j) e_right.push_back(top[j * (nx + 1) + nx]);
    for (int i = nx; i >= 0; --i) e_back.push_back(top[ny * (nx + 1) + i]);
    for (int j = ny; j >= 0; --j) e_left.push_back(top[j * (nx + 1)]);
    side(b0, b1, e_front); side(b1, b2, e_right); side(b2, b3, e_back); side(b3, b0, e_left);
    TriangleMesh mesh(std::move(its));
    REQUIRE(its_num_open_edges(mesh.its) == 0);
    return mesh;
}

TEST_CASE("colorsplit: strict patches share boundary vertices and cover the surface", "[colorsplit]")
{
    TriangleMesh cube = make_cube(40., 40., 20.);
    auto data = paint_data(cube, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorPatches p = extract_color_patches(cube.its, data);
    REQUIRE(p.states == std::vector<int>{2});
    REQUIRE(p.facet_state.size() == p.surface.indices.size());
    REQUIRE(its_num_open_edges(p.surface) == 0);
    REQUIRE_THAT(its_volume(p.surface), WithinRel(40. * 40. * 20., 1e-6));
    size_t painted = 0;
    for (int s : p.facet_state) painted += (s == 2);
    REQUIRE(painted == 2);
}

TEST_CASE("colorsplit: a brush stroke cutting through facets still yields a closed surface", "[colorsplit]")
{
    TriangleMesh cube = make_cube(40., 40., 20.);
    TriangleSelector selector(cube);
    // Paint a sphere-shaped patch through the middle of the top face (forces octree splitting / T-joints).
    selector.select_patch(2, std::make_unique<TriangleSelector::Sphere>(Vec3f(20.f, 20.f, 20.f), 6.f),
                          EnforcerBlockerType::Extruder3, Transform3d::Identity(), true, 0.f);
    ColorPatches p = extract_color_patches(cube.its, selector.serialize());
    REQUIRE(p.states == std::vector<int>{3});
    REQUIRE(its_num_open_edges(p.surface) == 0);
    REQUIRE_THAT(its_volume(p.surface), WithinRel(40. * 40. * 20., 1e-6));
    REQUIRE(p.surface.indices.size() > cube.its.indices.size());
}

TEST_CASE("colorsplit: an open mesh is refused", "[colorsplit]")
{
    TriangleMesh cube = make_cube(10., 10., 10.);
    indexed_triangle_set open = cube.its;
    open.indices.pop_back();
    TriangleMesh open_mesh(open);
    auto data = paint_data(open_mesh, all_with({0}, EnforcerBlockerType::Extruder2));
    REQUIRE_THROWS_AS(extract_color_patches(open_mesh.its, data), ColorSplitError);
}
```

Note: check the exact `TriangleSelector::select_patch` signature in `src/libslic3r/TriangleSelector.hpp` (search `select_patch`) and the `Cursor` classes (`Sphere`, `Circle`) — adapt the call to the real overload (the intent is a sphere cursor of radius 6 mm centred on the top face, seed facet 2, no triangle-splitting limit). If painting through the selector is awkward, use `selector.set_facet` on a `make_grid_box(40,40,20,8,8)` instead and paint a disc of grid cells by predicate.

- [ ] **Step 3: Register the files and run the tests to verify they fail to compile**

Add `ColorSplit.cpp` / `ColorSplit.hpp` to the main `lisbslic3r_sources` list in `src/libslic3r/CMakeLists.txt` right after `PaintDepth.cpp`, and `test_color_split.cpp` to `tests/libslic3r/CMakeLists.txt` after `test_paint_depth_clamp.cpp`. Create an EMPTY `ColorSplit.hpp` (just `#pragma once`) and `ColorSplit.cpp` (just an include). Run `build_next_wt_tests.bat`.
Expected: compile error in test_color_split.cpp (`ColorPatches`/`extract_color_patches` undeclared).

- [ ] **Step 4: Implement the header and patch extraction**

`src/libslic3r/ColorSplit.hpp`:

```cpp
#pragma once
// Split by painted colour: MMU paint -> in-place solid parts.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "libslic3r.h"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"
#include "PaintDepth.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ModelObject;

class ColorSplitError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class ColorSplitCancelled : public ColorSplitError { public: ColorSplitCancelled() : ColorSplitError("cancelled") {} };

// Progress callback: percent 0..100; return false to cancel.
using ColorSplitProgress = std::function<bool(int)>;

// Spec 3.1: the conforming, T-joint-free retriangulation F of the volume surface with a state per facet.
struct ColorPatches {
    indexed_triangle_set surface;     // welded; zero open edges
    std::vector<int>     facet_state; // per triangle of `surface`: 0 = unpainted, else 1-based filament id
    std::vector<int>     states;      // ascending painted states present (>= 1)
};
ColorPatches extract_color_patches(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint);

} // namespace Slic3r
```

`src/libslic3r/ColorSplit.cpp`:

```cpp
#include "ColorSplit.hpp"
#include "TriangleMesh.hpp"
#include <algorithm>
#include <numeric>

namespace Slic3r {

ColorPatches extract_color_patches(const indexed_triangle_set &mesh_in, const TriangleSelector::TriangleSplittingData &paint)
{
    // Weld exact duplicates so index adjacency (what TriangleSelector uses for T-joint resolution) sees the
    // real topology. its_merge_vertices keeps face order, so the paint's facet indexing stays valid.
    indexed_triangle_set mesh = mesh_in;
    its_merge_vertices(mesh);
    if (its_num_open_edges(mesh) != 0)
        throw ColorSplitError("The part is not watertight; repair it before splitting by colour.");

    TriangleMesh tm(mesh);
    TriangleSelector sel(tm);
    sel.deserialize(paint, /*needs_reset=*/false);

    ColorPatches out;
    std::vector<int> states;
    for (size_t s = 1; s < paint.used_states.size(); ++s)
        if (paint.used_states[s] && sel.has_facets(EnforcerBlockerType(s)))
            states.push_back(int(s));
    states.push_back(0); // unpainted remainder, processed last

    // All get_facets_strict calls from ONE selector share the same vertex pool (TriangleSelector.cpp:1478-1502),
    // so indices from different states refer to the same coordinates and can simply be concatenated.
    bool first = true;
    for (int s : states) {
        indexed_triangle_set part = sel.get_facets_strict(EnforcerBlockerType(s));
        if (first) { out.surface.vertices = part.vertices; first = false; }
        assert(part.vertices.size() == out.surface.vertices.size());
        for (const Vec3i32 &t : part.indices) {
            out.surface.indices.push_back(t);
            out.facet_state.push_back(s);
        }
    }
    its_compactify_vertices(out.surface, /*shrink_to_fit=*/true);
    if (its_num_open_edges(out.surface) != 0)
        throw ColorSplitError("Paint data does not cover the surface consistently (internal error).");
    states.pop_back();
    out.states = std::move(states);
    return out;
}

} // namespace Slic3r
```

Check `its_compactify_vertices`'s exact signature in `TriangleMesh.hpp:215` and adapt.

- [ ] **Step 5: Build and run**

Run `build_next_wt_tests.bat`, then `libslic3r_tests.exe "[colorsplit]"`.
Expected: 3 test cases pass.

- [ ] **Step 6: Commit**

```bash
git add src/libslic3r/ColorSplit.hpp src/libslic3r/ColorSplit.cpp src/libslic3r/CMakeLists.txt tests/libslic3r/test_color_split.cpp tests/libslic3r/CMakeLists.txt
git commit -m "feat(color-split): patch extraction from paint data (T-joint-free surface)"
```

---

### Task 2: Depth model — config-derived depths and per-vertex depth

**Files:**
- Modify: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Produces: `struct ColorSplitDepths { double D, ws, cap_top, cap_bottom, layer_height; bool unlimited; }`,
  `ColorSplitDepths color_split_depths(const DynamicPrintConfig &effective, const std::vector<int> &filaments)`,
  `std::vector<float> compute_vertex_depths(const ColorPatches &, const std::vector<Vec3f> &normals, double D)` (spec 3.4 clamp min(D, t/2 − δ), δ = 0.002 mm),
  `std::vector<Vec3f> color_split_normals(const indexed_triangle_set &)` (AngleWeighted).

- [ ] **Step 1: Write the failing tests**

Append to `tests/libslic3r/test_color_split.cpp`:

```cpp
static DynamicPrintConfig split_test_config(PaintDepthMode mode = pdmWalls, int walls = 3, double mm = 1.5)
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_num_extruders(2);
    config.set_num_filaments(2);
    config.option<ConfigOptionFloats>("nozzle_diameter")->values = {0.4, 0.4};
    config.option<ConfigOptionFloats>("filament_diameter")->values = {1.75, 1.75};
    config.option<ConfigOptionStrings>("filament_colour")->values  = {"#FFFFFF", "#804020"};
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->value   = 0.42;
    config.option<ConfigOptionFloatOrPercent>("outer_wall_line_width")->percent = false;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->value   = 0.45;
    config.option<ConfigOptionFloatOrPercent>("inner_wall_line_width")->percent = false;
    config.option<ConfigOptionFloat>("layer_height")->value = 0.2;
    config.option<ConfigOptionInt>("top_shell_layers")->value = 4;
    config.option<ConfigOptionFloat>("top_shell_thickness")->value = 0.6;
    config.option<ConfigOptionInt>("bottom_shell_layers")->value = 3;
    config.option<ConfigOptionFloat>("bottom_shell_thickness")->value = 0.;
    config.option<ConfigOptionEnum<PaintDepthMode>>("paint_depth_mode")->value = mode;
    config.option<ConfigOptionInt>("paint_depth_walls")->value = walls;
    config.option<ConfigOptionFloat>("paint_depth_mm")->value = mm;
    config.option<ConfigOptionEnum<PerimeterGeneratorType>>("wall_generator")->value = PerimeterGeneratorType::Classic;
    return config;
}

TEST_CASE("colorsplit: depths mirror paint_depth_band_mm and the shell layer rules", "[colorsplit]")
{
    DynamicPrintConfig cfg = split_test_config();
    ColorSplitDepths d = color_split_depths(cfg, {1, 2});
    Flow ext = Flow::new_from_config_width(frExternalPerimeter, *cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width"), 0.4f, 0.2f);
    Flow per = Flow::new_from_config_width(frPerimeter,         *cfg.option<ConfigOptionFloatOrPercent>("inner_wall_line_width"), 0.4f, 0.2f);
    float band = paint_depth_band_mm(pdmWalls, 3, 1.5, ext.width(), ext.spacing(), per.spacing());
    band = paint_depth_band_classic_floor_mm(band, ext.width(), ext.spacing());
    REQUIRE_THAT(d.D, WithinRel(band, 1e-5));
    REQUIRE_THAT(d.ws, WithinRel(ext.width() + ext.spacing(), 1e-5));
    REQUIRE_THAT(d.layer_height, WithinRel(0.2, 1e-5));
    REQUIRE(!d.unlimited);
    // top: max(4 layers, 0.6mm/0.2 = 3 layers) = 4 layers = 0.8mm; bottom: 3 layers, thickness 0 -> 0.6mm
    REQUIRE_THAT(d.cap_top, WithinRel(0.8, 1e-5));
    REQUIRE_THAT(d.cap_bottom, WithinRel(0.6, 1e-5));

    cfg.option<ConfigOptionInt>("top_shell_layers")->value = 0;          // zero count = no shell: surface layer only
    REQUIRE_THAT(color_split_depths(cfg, {1, 2}).cap_top, WithinRel(0.2, 1e-5));

    ColorSplitDepths u = color_split_depths(split_test_config(pdmUnlimited), {1, 2});
    REQUIRE(u.unlimited);
    REQUIRE_THAT(color_split_depths(split_test_config(pdmMillimeters, 3, 2.5), {1, 2}).D, WithinRel(2.5, 1e-5));
}

TEST_CASE("colorsplit: per-vertex depth is min(D, half thickness)", "[colorsplit]")
{
    // 40x40x1.2 plate: D = 1.5 must clamp to 0.6 on the top face; a 40x40x20 block keeps 1.5.
    TriangleMesh plate = make_cube(40., 40., 1.2);
    ColorPatches pp = extract_color_patches(plate.its, paint_data(plate, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<Vec3f> np = color_split_normals(pp.surface);
    std::vector<float> dp = compute_vertex_depths(pp, np, 1.5);
    for (size_t v = 0; v < pp.surface.vertices.size(); ++v)
        if (pp.surface.vertices[v].z() > 1.0f) REQUIRE_THAT(dp[v], WithinAbs(0.598f, 1e-3f));

    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches pb = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    std::vector<Vec3f> nb = color_split_normals(pb.surface);
    std::vector<float> db = compute_vertex_depths(pb, nb, 1.5);
    // Corner normals are bisectors (angle weighted -> exact (±1,±1,1)/sqrt3); the ray along -n exits far away.
    for (size_t v = 0; v < pb.surface.vertices.size(); ++v)
        REQUIRE_THAT(db[v], WithinAbs(1.5f, 1e-4f));
    // Unlimited (D = inf) -> half thickness along the normal: 10mm at the top-face interior direction is not
    // sampled on a plain cube (only corner vertices exist), corners see the body diagonal/2.
    std::vector<float> du = compute_vertex_depths(pb, nb, std::numeric_limits<double>::infinity());
    for (float x : du) REQUIRE(x > 1.5f);
}
```

- [ ] **Step 2: Build, verify the tests fail to compile** (`color_split_depths` undeclared).

- [ ] **Step 3: Implement**

Header additions (inside `namespace Slic3r`):

```cpp
// Spec 3.3/3.5: world-mm depth model derived from the part's effective config.
struct ColorSplitDepths {
    double D            = 0.;   // normal depth; ignored when unlimited
    double ws           = 0.;   // wall stack = external width + external spacing
    double cap_top      = 0.;   // capped-group depth for up-facing flats (>= layer_height)
    double cap_bottom   = 0.;   // same for down-facing flats
    double layer_height = 0.;
    bool   unlimited    = false;
};
// `filaments` = 1-based ids whose nozzle/flow take part (body extruder + painted filaments); the widest wins.
ColorSplitDepths color_split_depths(const DynamicPrintConfig &effective, const std::vector<int> &filaments);

// Spec 3.2: angle-weighted vertex normals of the full surface F.
std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface);
// Spec 3.4 (rev 2.2): d(v) = min(D, t(v)/2 - delta), delta = 0.002 mm, t(v) = thickness along -n(v). D may be +inf (unlimited).
std::vector<float> compute_vertex_depths(const ColorPatches &patches, const std::vector<Vec3f> &normals, double D);
```

Implementation:

```cpp
#include "AABBMesh.hpp"
#include "Flow.hpp"
#include "NormalUtils.hpp"
#include "PrintConfig.hpp"
#include <cmath>
#include <limits>

namespace Slic3r {

static int effective_shell_layers(int n_layers, double thickness, double h)
{
    // Mirrors effective_shell_layers_by_thickness (MultiMaterialSegmentation.cpp:1381-1420) for uniform layers:
    // a zero count means no shell at all; otherwise the larger of the count and the layers spanning the thickness.
    if (n_layers <= 0) return 0;
    int by_thickness = thickness > 0. ? int(std::ceil(thickness / h - EPSILON)) : 0;
    return std::max(n_layers, by_thickness);
}

ColorSplitDepths color_split_depths(const DynamicPrintConfig &cfg, const std::vector<int> &filaments)
{
    ColorSplitDepths out;
    const double h = cfg.opt_float("layer_height");
    out.layer_height = h;
    const PaintDepthMode mode  = cfg.opt_enum<PaintDepthMode>("paint_depth_mode");
    const int            walls = cfg.opt_int("paint_depth_walls");
    const double         mm    = cfg.opt_float("paint_depth_mm");
    const bool classic = cfg.opt_enum<PerimeterGeneratorType>("wall_generator") == PerimeterGeneratorType::Classic;
    ConfigOptionFloatOrPercent ext_w = *cfg.option<ConfigOptionFloatOrPercent>("outer_wall_line_width");
    ConfigOptionFloatOrPercent per_w = *cfg.option<ConfigOptionFloatOrPercent>("inner_wall_line_width");
    if (ext_w.value == 0) ext_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");   // PrintRegion.cpp:95-96
    if (per_w.value == 0) per_w = *cfg.option<ConfigOptionFloatOrPercent>("line_width");
    const auto &nozzles = cfg.option<ConfigOptionFloats>("nozzle_diameter")->values;
    out.unlimited = mode == pdmUnlimited;
    for (int f : filaments) {
        const float nozzle = float(nozzles[std::min<size_t>(std::max(f, 1) - 1, nozzles.size() - 1)]);
        Flow ext = Flow::new_from_config_width(frExternalPerimeter, ext_w, nozzle, float(h));
        Flow per = Flow::new_from_config_width(frPerimeter,         per_w, nozzle, float(h));
        float band = paint_depth_band_mm(mode, walls, mm, ext.width(), ext.spacing(), per.spacing());
        if (classic) band = paint_depth_band_classic_floor_mm(band, ext.width(), ext.spacing());
        out.D  = std::max(out.D,  double(band));
        out.ws = std::max(out.ws, double(ext.width() + ext.spacing()));
    }
    int n_top = effective_shell_layers(cfg.opt_int("top_shell_layers"),    cfg.opt_float("top_shell_thickness"),    h);
    int n_bot = effective_shell_layers(cfg.opt_int("bottom_shell_layers"), cfg.opt_float("bottom_shell_thickness"), h);
    out.cap_top    = std::max(h, n_top * h);
    out.cap_bottom = std::max(h, n_bot * h);
    return out;
}

std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface)
{
    NormalUtils::Normals n = NormalUtils::create_normals(surface, NormalUtils::VertexNormalType::AngleWeighted);
    std::vector<Vec3f> out(n.size());
    for (size_t i = 0; i < n.size(); ++i) out[i] = n[i].normalized();   // check NormalUtils::Normals element type
    return out;
}

std::vector<float> compute_vertex_depths(const ColorPatches &p, const std::vector<Vec3f> &normals, double D)
{
    AABBMesh aabb(p.surface);
    const double eps = 1e-3;
    std::vector<float> d(p.surface.vertices.size(), float(std::isfinite(D) ? D : std::numeric_limits<float>::max()));
    for (size_t v = 0; v < p.surface.vertices.size(); ++v) {
        const Vec3d n   = normals[v].cast<double>();
        const Vec3d src = p.surface.vertices[v].cast<double>() - eps * n;
        double t = std::numeric_limits<double>::infinity();
        for (const AABBMesh::hit_result &hit : aabb.query_ray_hits(src, -n))
            if (hit.is_hit() && hit.distance() > 5. * eps) { t = hit.distance() + eps; break; }
        d[v] = float(std::min(double(d[v]), t / 2. - 0.002));   // delta keeps the bottom strictly short of the mid-surface
    }
    return d;
}

} // namespace Slic3r
```

Check `NormalUtils::Normals` (NormalUtils.hpp:17) — if it is `std::vector<Vec3f>` already, drop the copy loop. Check `AABBMesh::query_ray_hits` returns hits sorted by distance (AABBMesh.cpp); if not, take the minimum over hits with distance > 5·eps.

- [ ] **Step 4: Build and run** `libslic3r_tests.exe "[colorsplit]"` → all pass.

- [ ] **Step 5: Commit** — `feat(color-split): config-derived depths and half-thickness vertex depth`.

---

### Task 3: Shell construction with validity checks (no flat cap, no crease step yet)

**Files:**
- Modify: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Produces:
  `struct ColorShell { int state; bool capped; indexed_triangle_set mesh; }`,
  `struct ShellCheck { bool closed; bool self_intersects; double volume; }`,
  `ShellCheck check_shell(const indexed_triangle_set &)`,
  `struct ColorSplitParams { bool flat_cap = true; bool absorb_islands = true; bool crease_step = true; double depth_override_mm = 0.; }`,
  `std::vector<ColorShell> build_color_shells(const ColorPatches &, const ColorSplitDepths &, const ColorSplitParams &, const ColorSplitProgress &)`.
  Internally: `build_shell_for_group(patches, normals, depths_per_vertex, group_facets, params, depths) -> indexed_triangle_set`.
- Consumes: Task 1 patches, Task 2 normals/depths.

- [ ] **Step 1: Write the failing tests**

```cpp
static ColorSplitDepths depths_for_test(double D, double h = 0.2, double ws = 0.87)
{
    ColorSplitDepths d; d.D = D; d.ws = ws; d.layer_height = h; d.cap_top = 0.8; d.cap_bottom = 0.6; d.unlimited = !std::isfinite(D);
    return d;
}
static ColorSplitParams no_cap_no_step() { ColorSplitParams p; p.flat_cap = false; p.crease_step = false; return p; }

TEST_CASE("colorsplit: shell of a painted top face is a closed slab of depth D", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    // Corner bisector normals lean inward 45deg in x and y and down: the slab is a frustum, volume between the
    // full slab and the slab shrunk by 1.5mm per side.
    REQUIRE(c.volume < 40. * 40. * 1.5);
    REQUIRE(c.volume > 37. * 37. * 1.5);
}

TEST_CASE("colorsplit: painted sphere smaller than D gets a valid thin shell (fold guard)", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    ColorPatches p = extract_color_patches(sphere.its, paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
    REQUIRE(c.volume > 0.);
    REQUIRE(c.volume < 4. / 3. * PI);          // hollow shell (bottom = delta-ball at the centre, or thicker after the guard), less than the full ball
}

TEST_CASE("colorsplit: thin plate painted on both sides gives two shells meeting mid-thickness", "[colorsplit]")
{
    TriangleMesh plate = make_cube(40., 40., 1.2);
    auto data = paint_data(plate, {{2, EnforcerBlockerType::Extruder2}, {3, EnforcerBlockerType::Extruder2}, {0, EnforcerBlockerType::Extruder3}, {1, EnforcerBlockerType::Extruder3}});
    ColorPatches p = extract_color_patches(plate.its, data);
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 2);
    for (const ColorShell &s : shells) {
        ShellCheck c = check_shell(s.mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
        REQUIRE_THAT(c.volume, WithinRel(40. * 40. * 0.6, 0.08));   // frustum-ish, ~0.6mm deep
    }
}

TEST_CASE("colorsplit: pinch boundary (two cells touching at one vertex) builds a closed shell", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 40., 10., 4, 4);
    // cells (1,1) and (2,2) share exactly one vertex; each cell = 2 triangles: index = 2 + 2*(j*4+i) (+1)
    auto cell = [](int i, int j) { int base = 2 + 2 * (j * 4 + i); return std::vector<int>{base, base + 1}; };
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    for (int f : cell(1, 1)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    for (int f : cell(2, 2)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(box.its, paint_data(box, facets));
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 2);          // one shell per edge-connected component
    for (const ColorShell &s : shells) {
        ShellCheck c = check_shell(s.mesh);
        REQUIRE(c.closed);
        REQUIRE(!c.self_intersects);
        REQUIRE_THAT(c.volume, WithinRel(10. * 10. * 1.5, 0.02));
    }
}

TEST_CASE("colorsplit: concave groove painted across the crease still yields a valid shell", "[colorsplit]")
{
    // L bracket: 40x40x20 block minus a 40x20x10 notch on the +Y/+Z corner, made with Manifold union of two boxes.
    TriangleMesh a = make_cube(40., 40., 10.);
    TriangleMesh b = make_cube(40., 20., 20.);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(a, b, out, "UNION"));
    REQUIRE(out.size() == 1);
    TriangleMesh bracket = out.front();
    // paint the floor of the notch (z=10, y in 20..40) and the riser wall (y=20, z in 10..20)
    auto data = paint_by_predicate(bracket, [](const Vec3f &c, const Vec3f &n) {
        return (std::abs(n.z() - 1.f) < 1e-3f && c.z() > 9.9f && c.z() < 10.1f && c.y() > 20.f) || (std::abs(n.y() - 1.f) < 1e-3f && c.z() > 10.f);
    }, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(bracket.its, data);
    auto shells = build_color_shells(p, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(shells.size() == 1);
    ShellCheck c = check_shell(shells[0].mesh);
    REQUIRE(c.closed);
    REQUIRE(!c.self_intersects);
}
```

- [ ] **Step 2: Build, verify failure** (`build_color_shells` undeclared).

- [ ] **Step 3: Implement shell construction**

Header additions:

```cpp
struct ColorSplitParams {
    bool   flat_cap          = true;   // spec 3.5
    bool   absorb_islands    = true;   // spec 3.8
    bool   crease_step       = true;   // spec 3.6
    double depth_override_mm = 0.;     // <= 0: use depths.D
};
struct ColorShell { int state = 0; bool capped = false; indexed_triangle_set mesh; };
struct ShellCheck { bool closed = false; bool self_intersects = true; double volume = 0.; };
ShellCheck check_shell(const indexed_triangle_set &shell);
std::vector<ColorShell> build_color_shells(const ColorPatches &, const ColorSplitDepths &, const ColorSplitParams &, const ColorSplitProgress &progress);
```

Implementation (Task 3 scope: groups = edge-connected components per state; `flat_cap`/`crease_step` are read but Task 5 fills their branches — in this task both are implemented as "off"):

```cpp
#include "MeshBoolean.hpp"

namespace Slic3r {

namespace {

struct Edge { int a, b; };

// Edge-connected components of the facet subset `in_set` (indices into patches.surface).
static std::vector<std::vector<int>> connected_components(const ColorPatches &p, const std::vector<Vec3i32> &nbrs, const std::vector<char> &in_set)
{
    std::vector<std::vector<int>> comps;
    std::vector<char> seen(p.surface.indices.size(), 0);
    for (int seed = 0; seed < int(p.surface.indices.size()); ++seed) {
        if (!in_set[seed] || seen[seed]) continue;
        std::vector<int> comp, stack{seed};
        seen[seed] = 1;
        while (!stack.empty()) {
            int f = stack.back(); stack.pop_back();
            comp.push_back(f);
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n >= 0 && in_set[n] && !seen[n]) { seen[n] = 1; stack.push_back(n); }
            }
        }
        comps.push_back(std::move(comp));
    }
    return comps;
}

struct ShellBuilder {
    const ColorPatches         &p;
    const std::vector<Vec3i32> &nbrs;          // its_face_neighbors(p.surface)
    const std::vector<Vec3f>   &normals;       // color_split_normals
    std::vector<float>          depth;         // per vertex (copy: the fold guard lowers it)
    const ColorSplitDepths     &depths;
    const ColorSplitParams     &params;

    // Builds the closed shell of the facet group `group` (indices into p.surface).
    indexed_triangle_set build(const std::vector<int> &group, double cap_depth /* 0 = none */)
    {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (int f : group) in[f] = 1;

        // Boundary edges, directed as in the CCW top triangle (interior on the left of a->b).
        std::vector<Edge> boundary;
        std::vector<int>  boundary_count(p.surface.vertices.size(), 0);
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n < 0 || !in[n]) {
                    // its_face_neighbors convention: neighbor k is across edge (v[k], v[(k+1)%3]) — verify in MeshSplitImpl.hpp
                    int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                    boundary.push_back({a, b});
                    ++boundary_count[a]; ++boundary_count[b];
                }
            }

        // Per-vertex wedge ids: a boundary vertex with more than 2 boundary edges (pinch) gets one copy per wedge.
        // Wedge = the fan of group triangles around the vertex between an incoming and the next outgoing boundary edge.
        // Implementation: walk the fan for every group triangle touching a pinch vertex; triangles reachable from each
        // other by rotating around the vertex through GROUP neighbours belong to the same wedge.
        std::map<std::pair<int,int>, int> wedge_of;   // (vertex, facet) -> wedge index 0..
        std::map<int,int> wedges_at;                  // vertex -> wedge count
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int v = p.surface.indices[f][k];
                if (boundary_count[v] <= 2) continue;
                if (wedge_of.count({v, f})) continue;
                int w = wedges_at[v]++;
                std::vector<int> stack{f};
                wedge_of[{v, f}] = w;
                while (!stack.empty()) {
                    int g = stack.back(); stack.pop_back();
                    for (int e = 0; e < 3; ++e) {
                        int n = nbrs[g][e];
                        if (n < 0 || !in[n]) continue;
                        // neighbour across an edge that contains v -> same wedge
                        int ea = p.surface.indices[g][e], eb = p.surface.indices[g][(e + 1) % 3];
                        if ((ea == v || eb == v) && !wedge_of.count({v, n})) { wedge_of[{v, n}] = w; stack.push_back(n); }
                    }
                }
            }

        // Output vertex ids: top copy and bottom copy per (vertex, wedge). Non-pinch vertices: wedge 0 only.
        indexed_triangle_set out;
        std::map<std::pair<int,int>, std::pair<int,int>> ids;   // (vertex, wedge) -> (top id, bottom id)
        auto vertex_ids = [&](int v, int wedge) -> std::pair<int,int> {
            auto it = ids.find({v, wedge});
            if (it != ids.end()) return it->second;
            const Vec3f top = p.surface.vertices[v];
            float d = depth[v];
            if (cap_depth > 0.) d = std::min(d, float(cap_depth));
            const Vec3f bottom = top - d * normals[v];
            int ti = int(out.vertices.size()); out.vertices.push_back(top);
            int bi = int(out.vertices.size()); out.vertices.push_back(bottom);
            return ids[{v, wedge}] = {ti, bi};
        };
        auto wedge = [&](int v, int f) { return boundary_count[v] > 2 ? wedge_of[{v, f}] : 0; };

        // Top and bottom (reversed) triangles.
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            auto A = vertex_ids(t[0], wedge(t[0], f)), B = vertex_ids(t[1], wedge(t[1], f)), C = vertex_ids(t[2], wedge(t[2], f));
            out.indices.emplace_back(A.first, B.first, C.first);
            out.indices.emplace_back(A.second, C.second, B.second);
        }
        // Side strips (b, a, a', b') on every boundary edge a->b of triangle f.
        for (int f : group)
            for (int k = 0; k < 3; ++k) {
                int n = nbrs[f][k];
                if (n >= 0 && in[n]) continue;
                int a = p.surface.indices[f][k], b = p.surface.indices[f][(k + 1) % 3];
                auto A = vertex_ids(a, wedge(a, f)), B = vertex_ids(b, wedge(b, f));
                out.indices.emplace_back(B.first, A.first, A.second);
                out.indices.emplace_back(B.first, A.second, B.second);
            }
        return out;
    }

    // Spec 3.4 fold guard: lower depth where a bottom triangle would invert or collapse. Returns true if any change.
    bool fold_guard(const std::vector<int> &group)
    {
        bool changed = false;
        for (int f : group) {
            const Vec3i32 &t = p.surface.indices[f];
            Vec3f a = p.surface.vertices[t[0]], b = p.surface.vertices[t[1]], c = p.surface.vertices[t[2]];
            Vec3f nt = (b - a).cross(c - a);
            Vec3f a2 = a - depth[t[0]] * normals[t[0]], b2 = b - depth[t[1]] * normals[t[1]], c2 = c - depth[t[2]] * normals[t[2]];
            Vec3f nb = (c2 - a2).cross(b2 - a2);            // reversed winding -> should point along -nt
            if (nb.dot(-nt) <= 0.f || nb.norm() < 1e-6f * nt.norm()) {   // Ruling 2: only truly collapsed bottoms
                for (int k = 0; k < 3; ++k) depth[t[k]] = std::max(float(depths.layer_height), depth[t[k]] * 0.5f);
                changed = true;
            }
        }
        return changed;
    }
};

} // namespace

ShellCheck check_shell(const indexed_triangle_set &shell)
{
    ShellCheck c;
    c.closed = its_num_open_edges(shell) == 0;
    c.volume = double(its_volume(shell));
    c.self_intersects = MeshBoolean::cgal::does_self_intersect(TriangleMesh(shell));
    return c;
}

std::vector<ColorShell> build_color_shells(const ColorPatches &p, const ColorSplitDepths &depths_in, const ColorSplitParams &params, const ColorSplitProgress &progress)
{
    ColorSplitDepths depths = depths_in;
    if (params.depth_override_mm > 0.) { depths.D = params.depth_override_mm; depths.unlimited = false; }
    const double D = depths.unlimited ? std::numeric_limits<double>::infinity() : depths.D;
    std::vector<Vec3i32> nbrs = its_face_neighbors(p.surface);
    std::vector<Vec3f>   normals = color_split_normals(p.surface);
    std::vector<float>   depth = compute_vertex_depths(p, normals, D);

    std::vector<ColorShell> shells;
    size_t done = 0;
    for (int s : p.states) {
        std::vector<char> in(p.surface.indices.size(), 0);
        for (size_t f = 0; f < in.size(); ++f) in[f] = p.facet_state[f] == s;
        for (const std::vector<int> &comp : connected_components(p, nbrs, in)) {
            ShellBuilder sb{p, nbrs, normals, depth, depths, params};
            for (int round = 0; round < 8 && sb.fold_guard(comp); ++round) {}
            indexed_triangle_set mesh = sb.build(comp, 0.);
            ShellCheck check = check_shell(mesh);
            // Self-intersection fallback: halve the component's depth until clean, floor = one layer.
            for (int round = 0; round < 6 && (!check.closed || check.self_intersects); ++round) {
                for (int f : comp) for (int k = 0; k < 3; ++k) sb.depth[p.surface.indices[f][k]] = std::max(float(depths.layer_height), sb.depth[p.surface.indices[f][k]] * 0.5f);
                mesh  = sb.build(comp, 0.);
                check = check_shell(mesh);
            }
            if (!check.closed || check.self_intersects)
                throw ColorSplitError("Could not build a valid shell for filament " + std::to_string(s) + " (self-intersecting surface).");
            shells.push_back({s, false, std::move(mesh)});
            if (progress && !progress(int(10 + 40 * double(++done) / std::max<size_t>(1, p.states.size() * 4)))) throw ColorSplitCancelled();
        }
    }
    return shells;
}

} // namespace Slic3r
```

Verify the `its_face_neighbors` edge convention (MeshSplitImpl.hpp `create_face_neighbors_index`): neighbour k must be across edge (v[k], v[(k+1)%3]); if the convention differs, adapt the two places that derive (a,b) from k.

- [ ] **Step 4: Build and run** `libslic3r_tests.exe "[colorsplit]"` → all pass. If the sphere test fails on `self_intersects`, print `sb.depth` statistics and check the fold guard is invoked (the sphere's normals all point to the centre: with D = 1.5 > r = 1, the clamp already gives 1.0 → collapse → guard halves to 0.5).

- [ ] **Step 5: Commit** — `feat(color-split): closed shells per painted component with fold guard and validity check`.

---

### Task 4: Manifold partition, island absorption, one-shot split, spike measurements

**Files:**
- Modify: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Produces: `struct ColorSplitResult { indexed_triangle_set body; std::vector<std::pair<int, indexed_triangle_set>> pieces; std::vector<std::string> warnings; ColorSplitDepths depths; }`,
  `ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells, bool absorb_islands, const ColorSplitProgress &)`,
  `ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint, const ColorSplitDepths &, const ColorSplitParams &, const ColorSplitProgress &)`.
- Consumes: Tasks 1–3. Note `build_color_shells` has a trailing optional `std::vector<std::string> *warnings = nullptr` (Ruling 10): pass `&shell_warnings` and merge into the result's warnings.

- [ ] **Step 1: Write the failing tests (partition + spike measurements)**

```cpp
static double volume_of(const indexed_triangle_set &its) { return double(its_volume(its)); }

TEST_CASE("colorsplit: partition of a painted top is exact and complementary", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorSplitResult r = split_volume_by_paint(block.its, data, depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.pieces[0].first == 2);
    REQUIRE(its_num_open_edges(r.body) == 0);
    REQUIRE(its_num_open_edges(r.pieces[0].second) == 0);
    const double total = volume_of(r.body) + volume_of(r.pieces[0].second);
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
    REQUIRE(volume_of(r.pieces[0].second) < 40. * 40. * 1.5 + 1.);
    // The piece's top surface is the original top face: every piece vertex has z <= 20 and the max is 20.
    float zmax = -1.f; for (const Vec3f &v : r.pieces[0].second.vertices) zmax = std::max(zmax, v.z());
    REQUIRE_THAT(zmax, WithinRel(20.f, 1e-5));
}

TEST_CASE("colorsplit: three adjacent colours tile the top face, lower filament wins overlaps", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 40., 20., 4, 1);   // top = 4 cells in a row
    auto cell = [](int i) { int base = 2 + 2 * i; return std::vector<int>{base, base + 1}; };
    std::vector<std::pair<int, EnforcerBlockerType>> facets;
    for (int f : cell(0)) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    for (int f : cell(1)) facets.emplace_back(f, EnforcerBlockerType::Extruder3);
    for (int f : cell(2)) facets.emplace_back(f, EnforcerBlockerType::Extruder4);
    ColorSplitResult r = split_volume_by_paint(box.its, paint_data(box, facets), depths_for_test(1.5), no_cap_no_step(), nullptr);
    REQUIRE(r.pieces.size() == 3);
    double total = volume_of(r.body);
    for (auto &pc : r.pieces) { REQUIRE(its_num_open_edges(pc.second) == 0); total += volume_of(pc.second); }
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
    // pairwise disjoint: intersect pieces with Manifold and expect empty
    for (size_t i = 0; i < 3; ++i)
        for (size_t j = i + 1; j < 3; ++j) {
            std::vector<TriangleMesh> out;
            REQUIRE(MeshBoolean::mfd::make_boolean(TriangleMesh(r.pieces[i].second), TriangleMesh(r.pieces[j].second), out, "INTERSECTION"));
            double v = 0.; for (auto &m : out) v += volume_of(m.its);
            REQUIRE(v < 1e-3);
        }
}

TEST_CASE("colorsplit: painted sphere smaller than D is wholly its colour (island absorption)", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    auto data = paint_by_predicate(sphere, [](const Vec3f &, const Vec3f &) { return true; }, EnforcerBlockerType::Extruder2);
    ColorSplitParams params = no_cap_no_step();
    ColorSplitResult r = split_volume_by_paint(sphere.its, data, depths_for_test(1.5), params, nullptr);
    REQUIRE(r.pieces.size() == 1);
    REQUIRE(r.body.indices.empty());
    REQUIRE_THAT(volume_of(r.pieces[0].second), WithinRel(volume_of(sphere.its), 1e-4));
    params.absorb_islands = false;
    ColorSplitResult r2 = split_volume_by_paint(sphere.its, data, depths_for_test(1.5), params, nullptr);
    REQUIRE(!r2.body.indices.empty());
}

TEST_CASE("colorsplit: cancellation aborts without a result", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    REQUIRE_THROWS_AS(split_volume_by_paint(block.its, data, depths_for_test(1.5), no_cap_no_step(), [](int) { return false; }), ColorSplitCancelled);
}

// ---- Spike measurements (spec §9). They must pass; numbers go to WARN for the decision checkpoint. ----
TEST_CASE("colorsplit spike: S1 self-intersecting fixtures and S3 timing", "[colorsplit_spike]")
{
    using clock = std::chrono::steady_clock;
    // S3: ~100k-triangle sphere, one colour cap and three colour bands.
    TriangleMesh sphere(its_make_sphere(20.0, PI / 90.));   // check triangle count; adjust fa to land near 100k
    WARN("sphere triangles: " << sphere.its.indices.size());
    auto one = paint_by_predicate(sphere, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.f; }, EnforcerBlockerType::Extruder2);
    auto t0 = clock::now();
    ColorSplitResult r1 = split_volume_by_paint(sphere.its, one, depths_for_test(1.5), ColorSplitParams{}, nullptr);
    auto t1 = clock::now();
    WARN("S3 one colour: " << std::chrono::duration<double>(t1 - t0).count() << " s, pieces " << r1.pieces.size());
    TriangleSelector sel(sphere);
    for (int f = 0; f < int(sphere.its.indices.size()); ++f) {
        const auto &t = sphere.its.indices[f];
        float z = (sphere.its.vertices[t[0]].z() + sphere.its.vertices[t[1]].z() + sphere.its.vertices[t[2]].z()) / 3.f;
        if (z > 10.f) sel.set_facet(f, EnforcerBlockerType::Extruder2);
        else if (z > -5.f) sel.set_facet(f, EnforcerBlockerType::Extruder3);
        else if (z > -15.f) sel.set_facet(f, EnforcerBlockerType::Extruder4);
    }
    t0 = clock::now();
    ColorSplitResult r3 = split_volume_by_paint(sphere.its, sel.serialize(), depths_for_test(1.5), ColorSplitParams{}, nullptr);
    t1 = clock::now();
    WARN("S3 three colours: " << std::chrono::duration<double>(t1 - t0).count() << " s, pieces " << r3.pieces.size());
    double total = volume_of(r3.body); for (auto &pc : r3.pieces) total += volume_of(pc.second);
    REQUIRE_THAT(total, WithinRel(volume_of(sphere.its), 1e-4));

    // S1: 2mm boss on a block (convex feature narrower than 2D) painted entirely.
    TriangleMesh block = make_cube(40., 40., 10.);
    TriangleMesh boss(its_make_cylinder(1.0, 4.0, PI / 18.));
    boss.translate(20.f, 20.f, 9.f);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(block, boss, out, "UNION"));
    TriangleMesh bossed = out.front();
    auto paint = paint_by_predicate(bossed, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.05f; }, EnforcerBlockerType::Extruder2);
    ColorSplitResult rb = split_volume_by_paint(bossed.its, paint, depths_for_test(1.5), ColorSplitParams{}, nullptr);
    REQUIRE(rb.pieces.size() == 1);
    WARN("S1 boss: piece volume " << volume_of(rb.pieces[0].second) << " (boss volume " << PI * 4.0 << ")");
    REQUIRE_THAT(volume_of(rb.pieces[0].second), WithinRel(PI * 1.0 * 1.0 * 4.0, 0.05));   // whole boss: two half-shells meet at the axis (delta sliver), top slab covers the rest
}
```

- [ ] **Step 2: Build, verify failure** (`split_volume_by_paint` undeclared).

- [ ] **Step 3: Implement the partition**

Header additions:

```cpp
struct ColorSplitResult {
    indexed_triangle_set                                   body;      // may be empty
    std::vector<std::pair<int, indexed_triangle_set>>      pieces;    // (filament, mesh), ascending filament
    std::vector<std::string>                               warnings;
    ColorSplitDepths                                       depths;
};
ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells, bool absorb_islands, const ColorSplitProgress &progress);
ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress);
```

Implementation:

```cpp
#include <manifold/manifold.h>
#include <map>

namespace Slic3r {
namespace {

static manifold::Manifold to_manifold64(const indexed_triangle_set &its)
{
    manifold::MeshGL64 m;
    m.numProp = 3;
    m.vertProperties.reserve(its.vertices.size() * 3);
    for (const Vec3f &v : its.vertices) { m.vertProperties.push_back(v.x()); m.vertProperties.push_back(v.y()); m.vertProperties.push_back(v.z()); }
    m.triVerts.reserve(its.indices.size() * 3);
    for (const Vec3i32 &t : its.indices) { m.triVerts.push_back(t[0]); m.triVerts.push_back(t[1]); m.triVerts.push_back(t[2]); }
    m.tolerance = 1e-5;   // mm; well below any print resolution, above float noise
    m.Merge();
    return manifold::Manifold(m).AsOriginal();
}

static indexed_triangle_set from_manifold(const manifold::Manifold &m)
{
    manifold::MeshGL64 out = m.GetMeshGL64();
    indexed_triangle_set its;
    const size_t stride = out.numProp, nv = out.vertProperties.size() / stride;
    its.vertices.reserve(nv);
    for (size_t i = 0; i < nv; ++i)
        its.vertices.emplace_back(float(out.vertProperties[i * stride]), float(out.vertProperties[i * stride + 1]), float(out.vertProperties[i * stride + 2]));
    its.indices.reserve(out.triVerts.size() / 3);
    for (size_t i = 0; i + 2 < out.triVerts.size(); i += 3)
        its.indices.emplace_back(int(out.triVerts[i]), int(out.triVerts[i + 1]), int(out.triVerts[i + 2]));
    return its;
}

static void require_ok(const manifold::Manifold &m, const char *what)
{
    if (m.Status() == manifold::Manifold::Error::Cancelled) throw ColorSplitCancelled();
    if (m.Status() != manifold::Manifold::Error::NoError)
        throw ColorSplitError(std::string("Boolean failed (") + what + ", Manifold status " + std::to_string(int(m.Status())) + ").");
}

} // namespace

ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells, bool absorb_islands, const ColorSplitProgress &progress)
{
    manifold::ExecutionContext ctx;
    auto tick = [&](int pct) { if (progress && !progress(pct)) { ctx.Cancel(); throw ColorSplitCancelled(); } };

    manifold::Manifold original = to_manifold64(mesh).WithContext(ctx);
    require_ok(original, "source mesh");
    const uint32_t original_id = uint32_t(original.OriginalID());
    const double   vol_original = original.Volume();

    ColorSplitResult r;
    std::map<int, std::vector<manifold::Manifold>> piece_parts;   // filament -> Split results
    std::map<uint32_t, int> shell_state;                          // shell OriginalID -> filament
    manifold::Manifold rest = original;
    for (size_t i = 0; i < shells.size(); ++i) {
        manifold::Manifold shell = to_manifold64(shells[i].mesh).WithContext(ctx);
        require_ok(shell, "shell");
        shell_state[uint32_t(shell.OriginalID())] = shells[i].state;
        auto [piece, remainder] = rest.Split(shell);
        require_ok(piece, "split piece"); require_ok(remainder, "split remainder");
        if (!piece.IsEmpty()) piece_parts[shells[i].state].push_back(piece);
        rest = remainder;
        tick(int(50 + 40 * double(i + 1) / shells.size()));
    }

    // Spec 3.8: enclosed body islands -> the colour that contributed most of their surface.
    std::vector<manifold::Manifold> body_parts;
    for (manifold::Manifold &comp : rest.Decompose()) {
        manifold::MeshGL64 gl = comp.GetMeshGL64();
        bool touches_original = false;
        std::map<uint32_t, size_t> faces_by_id;
        for (size_t run = 0; run < gl.runOriginalID.size(); ++run) {
            size_t n = (gl.runIndex[run + 1] - gl.runIndex[run]) / 3;
            faces_by_id[gl.runOriginalID[run]] += n;
            if (gl.runOriginalID[run] == original_id) touches_original = true;
        }
        if (absorb_islands && !touches_original && !faces_by_id.empty()) {
            uint32_t best = 0; size_t best_n = 0; int best_state = std::numeric_limits<int>::max();
            for (auto &[id, n] : faces_by_id) {
                int st = shell_state.count(id) ? shell_state[id] : std::numeric_limits<int>::max();
                if (n > best_n || (n == best_n && st < best_state)) { best = id; best_n = n; best_state = st; }
            }
            if (shell_state.count(best)) { piece_parts[shell_state[best]].push_back(comp); continue; }
        }
        body_parts.push_back(comp);
    }

    double total = 0.;
    for (manifold::Manifold &b : body_parts) { indexed_triangle_set its = from_manifold(b); total += b.Volume(); its_merge(r.body, its); }
    for (auto &[state, parts] : piece_parts) {
        indexed_triangle_set its;
        for (manifold::Manifold &m : parts) { total += m.Volume(); its_merge(its, from_manifold(m)); }
        if (its.indices.empty()) { r.warnings.push_back("Filament " + std::to_string(state) + ": painted area produced no solid (fully covered by lower filaments)."); continue; }
        r.pieces.emplace_back(state, std::move(its));
    }
    if (std::abs(total - vol_original) > 1e-4 * vol_original + 1e-9)
        throw ColorSplitError("Volume check failed after splitting (" + std::to_string(total) + " vs " + std::to_string(vol_original) + " mm^3).");
    tick(95);
    return r;
}

ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress)
{
    if (progress && !progress(0)) throw ColorSplitCancelled();
    ColorPatches patches = extract_color_patches(mesh, paint);
    if (patches.states.empty()) throw ColorSplitError("The part has no painted colours.");
    if (progress && !progress(10)) throw ColorSplitCancelled();
    std::vector<std::string> shell_warnings;
    std::vector<ColorShell> shells = build_color_shells(patches, depths, params, progress, &shell_warnings);   // Ruling 10: skipped micro-components are warnings, not errors
    ColorSplitResult r = partition_by_shells(patches.surface, shells, params.absorb_islands, progress);
    r.warnings.insert(r.warnings.begin(), shell_warnings.begin(), shell_warnings.end());
    r.depths = depths;
    if (params.depth_override_mm > 0.) { r.depths.D = params.depth_override_mm; r.depths.unlimited = false; }
    if (progress) progress(100);
    return r;
}

} // namespace Slic3r
```

Check: `Manifold::Split` returns `std::pair<Manifold, Manifold>` (intersection, difference) — confirm the order in manifold.h:232's doc comment; `its_merge(indexed_triangle_set&, const indexed_triangle_set&)` exists (TriangleMesh.hpp:324); `runIndex` has one more entry than `runOriginalID` (mesh.h:127-129). The `AsOriginal()` call is what makes `OriginalID()` valid for provenance.

- [ ] **Step 4: Build and run** `libslic3r_tests.exe "[colorsplit]"` and `"[colorsplit_spike]"` → all pass; record the WARN numbers.

- [ ] **Step 5: Decision checkpoint (spike outcome)**

Write `.superpowers/sdd/2026-09-01-color-split/spike-report.md` with: S1/S3 numbers, whether any fixture needed the self-intersection fallback (add a counter `warnings` entry "shell depth reduced for filament k" in `build_color_shells` when the fallback loop ran, so the tests can print it), CGAL check share of the runtime (time `check_shell` separately with `std::chrono` inside a `[colorsplit_spike]` case). Verdict per spec §9: A as designed / A with a reduced check set / B. If B: STOP and report to the user before Task 5.

- [ ] **Step 6: Commit** — `feat(color-split): Manifold partition with island absorption; spike measurements`.

---

### Task 5: Smooth-patch decomposition, concave creases, file split (spike follow-up; rev 2.6)

**Files:**
- Create: `src/libslic3r/ColorSplitInternal.hpp`, `src/libslic3r/ColorSplitShell.cpp`, `src/libslic3r/ColorSplitPartition.cpp` (file split, Ruling 15)
- Modify: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `src/libslic3r/CMakeLists.txt`, `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Consumes: everything from Tasks 1-4.
- Produces: grouping by smooth patches (Ruling 18) inside `build_color_shells` — a state's facets connect across an edge only when the dihedral angle is below 30 deg (`n1.dot(n2) > cos(30 deg)`); every smooth patch is one `ColorShell`. Same-state crease boundaries use the patch's own mean normal `n_P` (no step, no bisector). Concave-crease rule (Ruling 14) inside `ShellBuilder::build`. NO refinement pre-pass (`refine_color_patches` / `color_split_refine_length` must not exist).

- [ ] **Step 1: Write the failing tests**

```cpp
TEST_CASE("colorsplit: a painted boss on a block is entirely its colour (smooth-patch decomposition)", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 10.);
    TriangleMesh boss(its_make_cylinder(1.0, 4.0, PI / 18.));
    boss.translate(20.f, 20.f, 9.f);
    std::vector<TriangleMesh> out;
    REQUIRE(MeshBoolean::mfd::make_boolean(block, boss, out, "UNION"));
    TriangleMesh bossed = out.front();
    auto paint = paint_by_predicate(bossed, [](const Vec3f &c, const Vec3f &) { return c.z() > 10.05f; }, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(bossed.its, paint);
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), ColorSplitParams{}, nullptr);
    REQUIRE(shells.size() == 2);                                       // side tube + top slab, both state 2
    for (const ColorShell &sh : shells) { REQUIRE(sh.state == 2); REQUIRE(check_shell(sh.mesh).closed); REQUIRE(!check_shell(sh.mesh).self_intersects); }
    ColorSplitResult rb = split_volume_by_paint(bossed.its, paint, depths_for_test(1.5, 0.2, 0.87), ColorSplitParams{}, nullptr);
    REQUIRE(rb.pieces.size() == 1);
    const double exposed = PI * 1.0 * 1.0 * 3.0;                        // 1 mm of the cylinder is buried in the block
    REQUIRE(volume_of(rb.pieces[0].second) >= 0.95 * exposed);
    float zmin = 1e9f; for (const Vec3f &v : rb.pieces[0].second.vertices) zmin = std::min(zmin, v.z());
    REQUIRE(zmin >= 10.f - 1e-3f);                                       // Ruling 14: nothing painted below the block top
}

TEST_CASE("colorsplit: a painted cube top and side are two smooth patches with straight walls", "[colorsplit]")
{
    TriangleMesh block = make_cube(40., 40., 20.);
    std::vector<std::pair<int, EnforcerBlockerType>> facets = all_with(CUBE_TOP, EnforcerBlockerType::Extruder2);
    for (int f : CUBE_PLUS_X) facets.emplace_back(f, EnforcerBlockerType::Extruder2);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, facets));
    ColorSplitParams params; params.flat_cap = false; params.crease_step = false;
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(shells.size() == 2);
    // the top slab's bottom lies exactly 1.5 below the top at every vertex (straight walls at the same-state crease),
    // and the side slab's bottom exactly 1.5 inside the +X face
    for (const ColorShell &sh : shells) {
        float zmin = 1e9f, xmin = 1e9f;
        for (const Vec3f &v : sh.mesh.vertices) { zmin = std::min(zmin, v.z()); xmin = std::min(xmin, v.x()); }
        REQUIRE(((std::abs(zmin - 18.5f) < 1e-4f) || (std::abs(xmin - 38.5f) < 1e-4f)));
    }
    ColorSplitResult r = split_volume_by_paint(block.its, paint_data(block, facets), depths_for_test(1.5, 0.2, 0.87), params, nullptr);
    REQUIRE(r.pieces.size() == 1);
    double total = volume_of(r.body) + volume_of(r.pieces[0].second);
    REQUIRE_THAT(total, WithinRel(40. * 40. * 20., 1e-4));
}
```

Delete the refinement tests and the interim S1 pin in the spike case (keep the timing WARNs); the boss test above is the S1 acceptance.

- [ ] **Step 2: Build, verify failure** (shell count 1 instead of 2; boss volume below 0.95).

- [ ] **Step 3: Implement**

Grouping (ColorSplitShell.cpp): `connected_components` takes an extra predicate `can_cross(facet_a, facet_b)`; for smooth patches it is `face_normal(a).dot(face_normal(b)) > cos(30 deg)`; `build_color_shells` groups each state's facets with it. Boundary info per vertex (already hoisted into the group topology): record for each boundary edge whether the outside facet has the SAME state; a vertex whose boundary edges are all same-state creases (or mixed with concave creases) uses `bottom = top - d * n_P` where `n_P` is the mean normal of the group's facets at the vertex. Other vertices keep their current rule (plain bisector `n(v)`, Ruling 14 for concave creases). The per-vertex depth `d` for such vertices is re-measured along `n_P` (as the concave rule already does).

Remove `refine_color_patches`, `color_split_refine_length`, their declarations, their tests and the call in `split_volume_by_paint` (the progress tick for refinement goes too). ColorSplitPartition.cpp keeps the Manifold helpers and `partition_by_shells` only.

The file split and the concave-crease rule are already committed (d687d1768a, a1aa0997d6) — keep them.

- [ ] **Step 4: Build and run** `[colorsplit]`, `[colorsplit_spike]`, `[paintdepth]` -> all pass; append the boss number and S3 timings to `.superpowers/sdd/2026-09-01-color-split/spike-report.md`.

- [ ] **Step 5: Commit** — `feat(color-split): smooth-patch shells (Ruling 18); drop the refinement pre-pass`.

---

### Task 6: Flat cap depth groups and crease step

**Files:**
- Modify: `src/libslic3r/ColorSplit.cpp` (ShellBuilder: groups, ring vertices), `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Consumes: `ColorSplitParams::flat_cap`, `crease_step`, `ColorSplitDepths::cap_top/cap_bottom/ws/layer_height`; the file layout from Task 5 (ShellBuilder lives in `ColorSplitShell.cpp`); the smooth-patch grouping and the concave-crease / same-state-crease rules from Task 5 stay in force (the flat-cap classification runs INSIDE each smooth patch; the crease step applies only to boundaries against a DIFFERENT state).
- Produces: `ColorShell::capped == true` for capped groups; behaviour per spec 3.5/3.6. Internal: `classify_depth_groups(patches, nbrs, normals, state, depths) -> std::vector<std::pair<std::vector<int>, double /*cap or 0*/>>`.

- [ ] **Step 1: Write the failing tests**

```cpp
static ColorSplitParams cap_and_step() { return ColorSplitParams{}; }

TEST_CASE("colorsplit: flat top is capped at the solid shell depth, slopes are not", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.1, 0.87);   // cap_top = 0.8 set in depths_for_test
    d.cap_top = 0.6;
    TriangleMesh block = make_cube(40., 40., 20.);
    auto data = paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ColorPatches p = extract_color_patches(block.its, data);
    ColorSplitParams params = cap_and_step(); params.crease_step = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    REQUIRE(shells[0].capped);
    float zmin = 1e9f; for (const Vec3f &v : shells[0].mesh.vertices) zmin = std::min(zmin, v.z());
    REQUIRE_THAT(zmin, WithinAbs(20.f - 0.6f, 1e-4f));

    // 3 degree slope: a 40x40 wedge whose top rises 40*tan(3deg): NOT flat (tan 3deg = 0.052 > h/(3ws) = 0.038)
    indexed_triangle_set wedge = its_make_cube(40., 40., 20.);
    for (Vec3f &v : wedge.vertices) if (v.z() > 19.f) v.z() += float(v.x() * std::tan(3. * PI / 180.));
    TriangleMesh wedge_mesh(wedge);
    auto wd = paint_by_predicate(wedge_mesh, [](const Vec3f &c, const Vec3f &n) { return n.z() > 0.9f; }, EnforcerBlockerType::Extruder2);
    ColorPatches pw = extract_color_patches(wedge_mesh.its, wd);
    auto ws_ = build_color_shells(pw, d, params, nullptr);
    REQUIRE(ws_.size() == 1);
    REQUIRE(!ws_[0].capped);

    // 1 degree slope IS flat (tan 1deg = 0.017 < 0.038)
    indexed_triangle_set wedge1 = its_make_cube(40., 40., 20.);
    for (Vec3f &v : wedge1.vertices) if (v.z() > 19.f) v.z() += float(v.x() * std::tan(1. * PI / 180.));
    TriangleMesh w1(wedge1);
    ColorPatches p1 = extract_color_patches(w1.its, paint_by_predicate(w1, [](const Vec3f &, const Vec3f &n) { return n.z() > 0.9f; }, EnforcerBlockerType::Extruder2));
    REQUIRE(build_color_shells(p1, d, params, nullptr)[0].capped);
}

TEST_CASE("colorsplit: narrow flat strip (core < 3 wall stacks) is not capped", "[colorsplit]")
{
    TriangleMesh box = make_grid_box(40., 2., 20., 1, 1);   // top is 40 x 2mm: inward offset by 1.5*0.87 = 1.3 kills it
    auto data = paint_data(box, {{2, EnforcerBlockerType::Extruder2}, {3, EnforcerBlockerType::Extruder2}});
    ColorPatches p = extract_color_patches(box.its, data);
    ColorSplitParams params = cap_and_step(); params.crease_step = false;
    auto shells = build_color_shells(p, depths_for_test(1.5, 0.1, 0.87), params, nullptr);
    REQUIRE(shells.size() == 1);
    REQUIRE(!shells[0].capped);
}

TEST_CASE("colorsplit: painted top steps one wall stack in below the surface layer at side faces", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_TOP, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = cap_and_step(); params.flat_cap = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    // Ring vertices at z = 20 - 0.2 must be inset 0.87 from the side faces; bottom vertices at z = 20 - 1.5 too.
    for (const Vec3f &v : shells[0].mesh.vertices) {
        if (std::abs(v.z() - 19.8f) < 1e-4f || std::abs(v.z() - 18.5f) < 1e-4f) {
            REQUIRE((std::abs(v.x() - 0.87f) < 1e-3f || std::abs(v.x() - (40.f - 0.87f)) < 1e-3f));
            REQUIRE((std::abs(v.y() - 0.87f) < 1e-3f || std::abs(v.y() - (40.f - 0.87f)) < 1e-3f));
        }
    }
    REQUIRE(check_shell(shells[0].mesh).closed);
}

TEST_CASE("colorsplit: painted side face keeps its full wall stack up to the top edge", "[colorsplit]")
{
    ColorSplitDepths d = depths_for_test(1.5, 0.2, 0.87);
    TriangleMesh block = make_cube(40., 40., 20.);
    ColorPatches p = extract_color_patches(block.its, paint_data(block, all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2)));
    ColorSplitParams params = cap_and_step(); params.flat_cap = false;
    auto shells = build_color_shells(p, d, params, nullptr);
    REQUIRE(shells.size() == 1);
    // Ring vertices at the top edge: x = 40 - 0.87, z = 20 (no downward move); bottom vertices then taper along the bisector.
    bool found_ring = false;
    for (const Vec3f &v : shells[0].mesh.vertices)
        if (std::abs(v.x() - (40.f - 0.87f)) < 1e-3f && std::abs(v.z() - 20.f) < 1e-4f) found_ring = true;
    REQUIRE(found_ring);
    REQUIRE(check_shell(shells[0].mesh).closed);
}
```

- [ ] **Step 2: Build, verify failure** (capped stays false; ring vertices absent).

- [ ] **Step 3: Implement depth groups and the crease step**

In `ColorSplit.cpp`:

```cpp
#include "ClipperUtils.hpp"
#include "ExPolygon.hpp"

namespace {

// Spec 3.5: split the facets of one state into (group, cap_depth) pairs.
static std::vector<std::pair<std::vector<int>, double>> classify_depth_groups(
    const ColorPatches &p, const std::vector<Vec3i32> &nbrs, int state, const ColorSplitDepths &depths, const ColorSplitParams &params, double D)
{
    std::vector<char> in(p.surface.indices.size(), 0);
    for (size_t f = 0; f < in.size(); ++f) in[f] = p.facet_state[f] == state;
    std::vector<std::pair<std::vector<int>, double>> groups;
    const bool cap_allowed = params.flat_cap && std::isfinite(D) && D >= depths.ws;
    if (!cap_allowed) {
        for (auto &c : connected_components(p, nbrs, in)) groups.emplace_back(std::move(c), 0.);
        return groups;
    }
    const double tan_flat = depths.layer_height / (3. * depths.ws);
    const double nz_min   = 1. / std::sqrt(1. + tan_flat * tan_flat);
    std::vector<char> flat_up(in.size(), 0), flat_down(in.size(), 0);
    for (size_t f = 0; f < in.size(); ++f) {
        if (!in[f]) continue;
        const Vec3i32 &t = p.surface.indices[f];
        Vec3f n = (p.surface.vertices[t[1]] - p.surface.vertices[t[0]]).cross(p.surface.vertices[t[2]] - p.surface.vertices[t[0]]).normalized();
        if (n.z() >  nz_min) flat_up[f] = 1;
        if (n.z() < -nz_min) flat_down[f] = 1;
    }
    std::vector<char> capped(in.size(), 0);
    for (int dir = 0; dir < 2; ++dir) {
        const std::vector<char> &flat = dir == 0 ? flat_up : flat_down;
        const double cap = dir == 0 ? depths.cap_top : depths.cap_bottom;
        if (cap >= D) continue;   // spec 3.5 gate: cap only when shallower than D
        for (auto &comp : connected_components(p, nbrs, flat)) {
            // core gate: XY projection survives an inward offset of 1.5*ws
            Polygons tris;
            for (int f : comp) {
                const Vec3i32 &t = p.surface.indices[f];
                Polygon poly;
                for (int k = 0; k < 3; ++k) poly.points.emplace_back(scaled<coord_t>(p.surface.vertices[t[k]].x()), scaled<coord_t>(p.surface.vertices[t[k]].y()));
                if (poly.area() < 0) poly.reverse();
                tris.push_back(std::move(poly));
            }
            ExPolygons proj = union_ex(tris);
            if (offset_ex(proj, -scaled<float>(1.5 * depths.ws)).empty()) continue;
            for (int f : comp) capped[f] = 1;
            groups.emplace_back(comp, cap);
        }
    }
    std::vector<char> rest(in.size(), 0);
    for (size_t f = 0; f < in.size(); ++f) rest[f] = in[f] && !capped[f];
    for (auto &c : connected_components(p, nbrs, rest)) groups.emplace_back(std::move(c), 0.);
    return groups;
}

} // namespace
```

ShellBuilder changes (spec 3.6): every boundary vertex gets a ring vertex. Extend `vertex_ids` to return `{top, ring, bottom}` and compute per boundary vertex:

```cpp
// per boundary vertex (and wedge): classify crease and case
struct BoundaryInfo { Vec3f n_p = Vec3f::Zero(), n_q = Vec3f::Zero(), t_in = Vec3f::Zero(); int edges = 0; };
// gather while scanning boundary edges (for edge a->b of facet f with outside neighbour n):
//   face normal of f -> info[a].n_p += , info[b].n_p += ; face normal of n (or of f if n < 0) -> n_q +=;
//   t_in += n_f.cross(vb - va).normalized() for both a and b
// then for each boundary vertex: n_p.normalize(); n_q.normalize(); t_in.normalize();
//   crease = n_p.dot(n_q) < cos(15deg); case_A = crease && |n_p.z| > |n_q.z|; case_B = crease && !case_A
```

Ring/bottom positions (h = layer_height, ws, d = depth[v] (capped by the group's cap)):

```cpp
Vec3f top = P[v];
Vec3f ring, bottom;
if (!params.crease_step || !info.crease) { ring = top - float(h) * n[v];            bottom = top - d * n[v]; }
else if (info.case_A)                    { ring = top + float(ws) * info.t_in - float(h) * info.n_p;  bottom = ring - std::max(0.f, d - float(h)) * info.n_p; }
else /* case B */                        { ring = top - float(std::min<double>(ws, d)) * info.n_p;    bottom = ring - std::max(0.f, d - float(ws)) * n[v]; }
// collapse: if (bottom - ring).norm() <= h -> bottom = ring (emit only the first strip for edges whose both ends collapsed)
```

Side strips become `(b, a, a1, b1)` and `(b1, a1, a', b')` → triangles `(B.top, A.top, A.ring)`, `(B.top, A.ring, B.ring)`, `(B.ring, A.ring, A.bottom)`, `(B.ring, A.bottom, B.bottom)`; the bottom triangles use the bottom ids as before; interior (non-boundary) vertices have ring == bottom == the plain offset and no strips.

`build_color_shells` now iterates `classify_depth_groups(...)` instead of raw components, passes each group's cap into `build(comp, cap)` (which applies `min(depth, cap)` per vertex when cap > 0 — already in `vertex_ids`), and sets `ColorShell::capped = cap > 0`. Capped groups are pushed before uncapped groups of the same state (spec 3.8 order).

- [ ] **Step 4: Build and run** `[colorsplit]` (all, including Task 3/4 cases which now run with cap/step off explicitly) → pass.

- [ ] **Step 5: Commit** — `feat(color-split): flat-cap depth groups and crease wall-stack step`.

---

### Task 7: Model mutation and coordinate space

**Files:**
- Modify: `src/libslic3r/ColorSplit.hpp`, `src/libslic3r/ColorSplit.cpp`, `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Produces:
  `struct ColorSplitSpace { Transform3d to_split; Transform3d from_split; double depth_scale; bool world_path; }`,
  `ColorSplitSpace color_split_space(const ModelObject &, const ModelVolume &)`,
  `ColorSplitDepths scale_depths(const ColorSplitDepths &, double s)`,
  `std::vector<ModelVolume*> apply_color_split(ModelObject &, size_t source_volume_idx, ColorSplitResult &&, const ColorSplitSpace &, bool solid_interfaces, bool keep_base_sparse_infill)`.

- [ ] **Step 1: Write the failing tests**

```cpp
static Model painted_model(const TriangleMesh &mesh, const std::vector<std::pair<int, EnforcerBlockerType>> &facets, int extruder = 1)
{
    Model model;
    ModelObject *object = model.add_object();
    object->name = "split-test";
    ModelVolume *volume = object->add_volume(mesh);
    volume->config.set("extruder", extruder);
    TriangleSelector selector(volume->mesh());
    for (auto [f, st] : facets) selector.set_facet(f, st);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    object->ensure_on_bed();
    return model;
}

static BoundingBoxf3 world_bbox(const ModelObject &o) { return o.instance_bounding_box(0, false); }

TEST_CASE("colorsplit: apply replaces the source by body + parts in place with extruders set", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    BoundingBoxf3 before = world_bbox(object);
    ColorSplitSpace space = color_split_space(object, src);
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(), scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    auto created = apply_color_split(object, 0, std::move(r), space, /*solid_interfaces=*/true, /*keep_base_sparse=*/false);
    REQUIRE(object.volumes.size() == 2);
    REQUIRE(created.size() == 2);
    REQUIRE(object.volumes[0]->name == "split-test");
    REQUIRE(object.volumes[0]->config.has("extruder"));
    REQUIRE(object.volumes[0]->config.extruder() == 1);
    REQUIRE(object.volumes[1]->config.extruder() == 2);
    REQUIRE(!object.volumes[0]->is_mm_painted());
    REQUIRE(!object.volumes[1]->is_mm_painted());
    REQUIRE(object.config.opt_bool("interface_shells"));
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-4);
    REQUIRE((after.max - before.max).norm() < 1e-4);
}

TEST_CASE("colorsplit: a rotated, scaled and mirrored PART stays in place", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    ModelVolume &src = *object.volumes.front();
    src.set_rotation(Vec3d(0.3, 0.2, 0.7));
    src.set_scaling_factor(Vec3d(1.5, 1.5, 1.5));
    src.set_mirror(Vec3d(-1., 1., 1.));
    src.set_offset(Vec3d(5., -3., 2.));
    object.invalidate_bounding_box();
    BoundingBoxf3 before = world_bbox(object);
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(!space.world_path);                       // isotropic: mesh-space path
    REQUIRE_THAT(space.depth_scale, WithinRel(1.5, 1e-5));
    ColorSplitResult r = split_volume_by_paint(src.mesh().its, src.mmu_segmentation_facets.get_data(), scale_depths(depths_for_test(1.5), space.depth_scale), ColorSplitParams{}, nullptr);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
}

TEST_CASE("colorsplit: anisotropic instance scale takes the world path", "[colorsplit]")
{
    Model model = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
    ModelObject &object = *model.objects.front();
    object.instances.front()->set_scaling_factor(Vec3d(2., 1., 1.));
    ModelVolume &src = *object.volumes.front();
    ColorSplitSpace space = color_split_space(object, src);
    REQUIRE(space.world_path);
    indexed_triangle_set its = src.mesh().its;
    its_transform(its, space.to_split);              // world mm
    ColorSplitResult r = split_volume_by_paint(its, src.mmu_segmentation_facets.get_data(), depths_for_test(1.5), ColorSplitParams{}, nullptr);
    BoundingBoxf3 before = world_bbox(object);
    apply_color_split(object, 0, std::move(r), space, false, false);
    object.invalidate_bounding_box();
    BoundingBoxf3 after = world_bbox(object);
    REQUIRE((after.min - before.min).norm() < 1e-3);
    REQUIRE((after.max - before.max).norm() < 1e-3);
    // the piece is 1.5mm deep in WORLD z (instance scale is in x only)
    const ModelVolume *piece = object.volumes.back();
    BoundingBoxf3 pb = piece->mesh().bounding_box();
    REQUIRE_THAT(pb.size().z(), WithinRel(1.5, 0.02));
}

TEST_CASE("colorsplit: empty body removes the source and keeps only pieces", "[colorsplit]")
{
    TriangleMesh sphere(its_make_sphere(1.0, PI / 18.));
    Model model;
    ModelObject *object = model.add_object();
    ModelVolume *volume = object->add_volume(sphere);
    TriangleSelector selector(volume->mesh());
    for (int f = 0; f < int(volume->mesh().its.indices.size()); ++f) selector.set_facet(f, EnforcerBlockerType::Extruder2);
    REQUIRE(volume->mmu_segmentation_facets.set(selector));
    object->add_instance();
    ColorSplitSpace space = color_split_space(*object, *volume);
    ColorSplitResult r = split_volume_by_paint(volume->mesh().its, volume->mmu_segmentation_facets.get_data(), depths_for_test(1.5), ColorSplitParams{}, nullptr);
    REQUIRE(r.body.indices.empty());
    apply_color_split(*object, 0, std::move(r), space, false, false);
    REQUIRE(object->volumes.size() == 1);
    REQUIRE(object->volumes[0]->config.extruder() == 2);
}
```

- [ ] **Step 2: Build, verify failure.**

- [ ] **Step 3: Implement**

Header:

```cpp
class ModelVolume;
struct ColorSplitSpace {
    Transform3d to_split   = Transform3d::Identity();   // mesh -> split space (identity on the mesh-space path)
    Transform3d from_split = Transform3d::Identity();
    double      depth_scale = 1.;                       // divide world depths by this on the mesh-space path
    bool        world_path  = false;
};
ColorSplitSpace  color_split_space(const ModelObject &, const ModelVolume &);
ColorSplitDepths scale_depths(const ColorSplitDepths &, double s);   // D, ws, caps, layer_height / s
// Spec §4. Returns the created volumes (body first if any). The source volume is deleted.
std::vector<ModelVolume*> apply_color_split(ModelObject &, size_t source_volume_idx, ColorSplitResult &&, const ColorSplitSpace &, bool solid_interfaces, bool keep_base_sparse_infill);
```

Implementation:

```cpp
#include "Model.hpp"
#include "Geometry.hpp"

namespace Slic3r {

ColorSplitSpace color_split_space(const ModelObject &object, const ModelVolume &volume)
{
    ColorSplitSpace s;
    const Transform3d T = (object.instances.empty() ? Transform3d::Identity() : object.instances.front()->get_matrix()) * volume.get_matrix();
    // isotropic iff the three column norms of the linear part agree
    const Matrix3d L = T.linear();
    const double sx = L.col(0).norm(), sy = L.col(1).norm(), sz = L.col(2).norm();
    if (std::abs(sx - sy) < 1e-6 * sx && std::abs(sx - sz) < 1e-6 * sx) {
        s.depth_scale = sx;
        return s;                                  // mesh-space path, identity transforms
    }
    s.world_path = true;
    s.to_split   = T;
    s.from_split = T.inverse();
    return s;
}

ColorSplitDepths scale_depths(const ColorSplitDepths &d, double s)
{
    ColorSplitDepths o = d;
    o.D /= s; o.ws /= s; o.cap_top /= s; o.cap_bottom /= s; o.layer_height /= s;
    return o;
}

static ModelVolume *add_split_volume(ModelObject &object, const ModelVolume &src, indexed_triangle_set &&its, const ColorSplitSpace &space, const std::string &name)
{
    if (space.world_path) its_transform(its, space.from_split);
    TriangleMesh mesh(std::move(its));
    if (space.world_path && space.from_split.linear().determinant() < 0.) mesh.flip_triangles();   // mirrored: keep outward winding
    ModelVolume *v = object.add_volume(src, std::move(mesh));         // public overload: copies name/config/transformation, centres
    // Rotation/scale-safe placement (review I7): offset = src.offset + R*S * init_shift
    v->set_offset(src.get_offset() + src.get_transformation().get_matrix_no_offset() * v->mesh().get_init_shift());
    v->name = name;
    v->text_configuration.reset();
    v->emboss_shape.reset();
    v->cut_info = ModelVolume::CutInfo();
    v->source = ModelVolume::Source();
    v->set_type(ModelVolumeType::MODEL_PART);
    return v;
}

std::vector<ModelVolume*> apply_color_split(ModelObject &object, size_t src_idx, ColorSplitResult &&r, const ColorSplitSpace &space, bool solid_interfaces, bool keep_base_sparse_infill)
{
    ModelVolume &src = *object.volumes[src_idx];
    const std::string src_name = src.name;
    const int body_extruder = src.extruder_id();
    const bool src_has_extruder_key = src.config.has("extruder") && src.config.extruder() != 0;
    const size_t first_new = object.volumes.size();
    std::vector<ModelVolume*> created;

    if (!r.body.indices.empty()) {
        ModelVolume *body = add_split_volume(object, src, std::move(r.body), space, src_name);
        if (src_has_extruder_key) body->config.set("extruder", body_extruder); else body->config.erase("extruder");
        created.push_back(body);
    }
    for (auto &[state, its] : r.pieces) {
        ModelVolume *part = add_split_volume(object, src, std::move(its), space, src_name + " F" + std::to_string(state));
        part->config.set("extruder", state);
        if (keep_base_sparse_infill) part->config.set("sparse_infill_filament", std::max(1, body_extruder));
        created.push_back(part);
    }
    // Move the new volumes into the source's slot and delete the source (index API only, Model.hpp).
    std::rotate(object.volumes.begin() + src_idx + 1, object.volumes.begin() + first_new, object.volumes.end());
    object.delete_volume(src_idx);
    if (solid_interfaces) object.config.set("interface_shells", true);
    object.invalidate_bounding_box();
    return created;
}

} // namespace Slic3r
```

Verify names: `ModelVolume::text_configuration` / `emboss_shape` are `std::optional` (reset ok), `cut_info` type name (`ModelVolume::CutInfo`), `ModelObject::delete_volume(size_t)`, `ModelVolume::set_type`, `ModelObject::instance_bounding_box(size_t, bool)`; adjust to the real declarations (Model.hpp).

- [ ] **Step 4: Build and run** `[colorsplit]` → pass.

- [ ] **Step 5: Commit** — `feat(color-split): apply split to the model in place with rotation-safe placement`.

---

### Task 8: End-to-end slicing parity and the S4 wedge measurement

**Files:**
- Modify: `tests/libslic3r/test_color_split.cpp`

**Interfaces:**
- Consumes: everything above; `Print::apply`, `PrintObject::slice()`, `PrintObject::layers()`, `LayerRegion::slices`, `PrintRegion::config().wall_filament`.

- [ ] **Step 1: Write the tests**

```cpp
static DynamicPrintConfig e2e_config() { DynamicPrintConfig c = split_test_config(); c.option<ConfigOptionFloat>("layer_height")->value = 0.2; return c; }

// Area of the filament-2 region on `layer` (sum over regions whose wall_filament == 2).
static double filament2_area(const Layer *layer)
{
    double a = 0.;
    for (const LayerRegion *lr : layer->regions())
        if (lr->region().config().wall_filament.value == 2)
            for (const ExPolygon &e : lr->slices.surfaces_to_expolygons())   // check the exact accessor: LayerRegion::slices is SurfaceCollection
                a += unscaled(unscaled(e.area()));
    return a;
}

TEST_CASE("colorsplit e2e: split parts slice like the 2D paint-depth claim on a painted side face", "[colorsplit]")
{
    // Unsplit painted object (2D path)
    Model painted = painted_model(make_cube(40., 40., 20.), all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2));
    Print p2d; p2d.set_status_silent();
    p2d.apply(painted, e2e_config());
    PrintObject *o2d = p2d.objects_mutable().front(); o2d->slice();
    // Split object
    Model split = painted_model(make_cube(40., 40., 20.), all_with(CUBE_PLUS_X, EnforcerBlockerType::Extruder2));
    ModelObject &obj = *split.objects.front();
    ColorSplitDepths depths = color_split_depths(e2e_config(), {1, 2});
    ColorSplitSpace space = color_split_space(obj, *obj.volumes.front());
    ColorSplitResult r = split_volume_by_paint(obj.volumes.front()->mesh().its, obj.volumes.front()->mmu_segmentation_facets.get_data(), scale_depths(depths, space.depth_scale), ColorSplitParams{}, nullptr);
    apply_color_split(obj, 0, std::move(r), space, true, false);
    Print p3d; p3d.set_status_silent();
    p3d.apply(split, e2e_config());
    PrintObject *o3d = p3d.objects_mutable().front(); o3d->slice();
    REQUIRE(o2d->layer_count() == o3d->layer_count());
    // Compare on mid-height layers (away from the top/bottom edges where the crease rules differ by design).
    const double line = 0.42;
    for (size_t i = o2d->layer_count() / 4; i < 3 * o2d->layer_count() / 4; ++i) {
        double a2 = filament2_area(o2d->layers()[i]), a3 = filament2_area(o3d->layers()[i]);
        INFO("layer " << i << " 2D " << a2 << " 3D " << a3);
        REQUIRE(std::abs(a2 - a3) <= 40. * line);   // one line width over the 40mm edge
    }
}

TEST_CASE("colorsplit e2e S4: painted cube top keeps a body outer wall on the side faces", "[colorsplit_spike]")
{
    for (bool step : {false, true}) {
        Model split = painted_model(make_cube(40., 40., 20.), all_with(CUBE_TOP, EnforcerBlockerType::Extruder2));
        ModelObject &obj = *split.objects.front();
        ColorSplitDepths depths = color_split_depths(e2e_config(), {1, 2});
        ColorSplitSpace space = color_split_space(obj, *obj.volumes.front());
        ColorSplitParams params; params.crease_step = step; params.flat_cap = false;
        ColorSplitResult r = split_volume_by_paint(obj.volumes.front()->mesh().its, obj.volumes.front()->mmu_segmentation_facets.get_data(), scale_depths(depths, space.depth_scale), params, nullptr);
        apply_color_split(obj, 0, std::move(r), space, true, false);
        Print p; p.set_status_silent(); p.apply(split, e2e_config());
        PrintObject *o = p.objects_mutable().front(); o->slice();
        // Layer just below the surface layer: body region must reach the contour with >= ws width when step is on.
        const Layer *layer = o->layers()[o->layer_count() - 2];
        double body = 0.; for (const LayerRegion *lr : layer->regions()) if (lr->region().config().wall_filament.value == 1) for (const ExPolygon &e : lr->slices.surfaces_to_expolygons()) body += unscaled(unscaled(e.area()));
        WARN("S4 step=" << step << " body area on layer " << o->layer_count() - 2 << ": " << body << " mm^2 (ws ring = " << 4 * 40 * depths.ws << ")");
        if (step) REQUIRE(body >= 4 * 40 * depths.ws * 0.9);
    }
}
```

Check the accessor names (`Layer::regions()`, `LayerRegion::region()`, `LayerRegion::slices` and how test_paint_depth_clamp.cpp:204-222 reads the region ExPolygons — copy that recipe verbatim).

- [ ] **Step 2: Build, run** `[colorsplit]` and `[colorsplit_spike]`; the e2e case must pass. If the 3D parity exceeds one line width on the vertical-wall fixture, investigate (likely the classic floor or the interlocking notch on the 2D side — compare with `interlocking_depth = 0`) before loosening the bound; document the finding in the spike report.

- [ ] **Step 3: Commit** — `test(color-split): end-to-end slice parity and S4 wedge measurement`.

---

### Task 9: GUI — Plater action, dialog, job, menu

**Files:**
- Create: `src/slic3r/GUI/ColorSplitDialog.hpp/.cpp`, `src/slic3r/GUI/Jobs/ColorSplitJob.hpp/.cpp`
- Modify: `src/slic3r/GUI/Plater.hpp` (near `void split_object();` :732), `src/slic3r/GUI/Plater.cpp` (near `Plater::split_object()`), `src/slic3r/GUI/GUI_Factories.cpp` (:1424-1436, :1462-1474, :1540-1549, :1595-1607, :1987-2002), `src/slic3r/CMakeLists.txt` (:265 area)

**Interfaces:**
- Consumes: Task 7 API (`color_split_space`, `scale_depths`, `split_volume_by_paint`, `apply_color_split`), `color_split_depths`.
- Produces: `void Plater::split_by_color();`, `bool Plater::can_split_by_color() const;`, `class ColorSplitJob : public Job`, `class ColorSplitDialog : public wxDialog`.

- [ ] **Step 1: Job**

`src/slic3r/GUI/Jobs/ColorSplitJob.hpp`:

```cpp
#pragma once
#include "Job.hpp"
#include "libslic3r/ColorSplit.hpp"
#include "libslic3r/ObjectID.hpp"
#include <vector>

namespace Slic3r { class Plater; namespace GUI {

class ColorSplitJob : public Job {
public:
    struct Target {
        ObjectID object_id, volume_id;
        uint64_t paint_timestamp = 0;          // mmu_segmentation_facets.timestamp() at queue time
        indexed_triangle_set mesh;             // copy, in split space
        TriangleSelector::TriangleSplittingData paint;
        ColorSplitSpace space;
        ColorSplitDepths depths;               // already scaled for the mesh-space path
        ColorSplitResult result;               // filled by process()
        bool ok = false;
        std::string error;
    };
    ColorSplitJob(Plater *plater, std::vector<Target> targets, ColorSplitParams params, bool solid_interfaces, bool keep_base_sparse);
    void process(Ctl &ctl) override;
    void finalize(bool canceled, std::exception_ptr &eptr) override;
private:
    Plater *m_plater;
    std::vector<Target> m_targets;
    ColorSplitParams m_params;
    bool m_solid_interfaces, m_keep_base_sparse;
};

}} // namespace Slic3r::GUI
```

`ColorSplitJob.cpp`:

```cpp
#include "ColorSplitJob.hpp"
#include "slic3r/GUI/Plater.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/GUI_ObjectList.hpp"
#include "slic3r/GUI/NotificationManager.hpp"
#include "slic3r/GUI/I18N.hpp"
#include "libslic3r/Model.hpp"

namespace Slic3r { namespace GUI {

ColorSplitJob::ColorSplitJob(Plater *plater, std::vector<Target> targets, ColorSplitParams params, bool solid_interfaces, bool keep_base_sparse)
    : m_plater(plater), m_targets(std::move(targets)), m_params(params), m_solid_interfaces(solid_interfaces), m_keep_base_sparse(keep_base_sparse) {}

void ColorSplitJob::process(Ctl &ctl)
{
    const std::string status = _u8L("Splitting by painted colour");
    for (size_t i = 0; i < m_targets.size(); ++i) {
        Target &t = m_targets[i];
        try {
            t.result = split_volume_by_paint(t.mesh, t.paint, t.depths, m_params, [&](int pct) {
                ctl.update_status(int((100 * i + pct) / m_targets.size()), status);
                return !ctl.was_canceled();
            });
            t.ok = true;
        } catch (const ColorSplitCancelled &) {
            return;
        } catch (const ColorSplitError &e) {
            t.error = e.what();
        }
    }
    ctl.update_status(100, _u8L("Split by painted colour done."));
}

void ColorSplitJob::finalize(bool canceled, std::exception_ptr &eptr)
{
    if (canceled || eptr) return;
    Model &model = m_plater->model();
    bool any = false;
    std::vector<std::string> messages;
    Plater::TakeSnapshot snapshot(m_plater, "Split by painted colour");
    for (Target &t : m_targets) {
        if (!t.ok) { messages.push_back(t.error); continue; }
        // Re-find by id: the model may have changed while the job ran.
        ModelObject *object = nullptr; size_t obj_idx = 0, vol_idx = 0;
        for (size_t oi = 0; oi < model.objects.size() && !object; ++oi)
            for (size_t vi = 0; vi < model.objects[oi]->volumes.size(); ++vi)
                if (model.objects[oi]->id() == t.object_id && model.objects[oi]->volumes[vi]->id() == t.volume_id) { object = model.objects[oi]; obj_idx = oi; vol_idx = vi; break; }
        if (!object || object->volumes[vol_idx]->mmu_segmentation_facets.timestamp() != t.paint_timestamp) {
            messages.push_back(_u8L("The painted part changed while splitting; nothing was changed."));
            continue;
        }
        apply_color_split(*object, vol_idx, std::move(t.result), t.space, m_solid_interfaces, m_keep_base_sparse);
        for (const std::string &w : t.result.warnings) messages.push_back(w);
        wxGetApp().obj_list()->add_volumes_to_object_in_list(obj_idx);
        wxGetApp().obj_list()->changed_object(int(obj_idx));
        wxGetApp().obj_list()->notify_instance_updated(int(obj_idx));
        wxGetApp().obj_list()->update_info_items(obj_idx);
        object->input_file.clear();
        any = true;
    }
    if (!messages.empty()) {
        std::string text; for (auto &m : messages) text += m + "\n";
        if (any) wxGetApp().notification_manager()->push_plater_warning_notification(text);
        else      show_error(m_plater, from_u8(text));
    }
}

}} // namespace Slic3r::GUI
```

Check: `Plater::TakeSnapshot` constructor signature (Plater.hpp:893-910), `ObjectList::changed_object(int)`, `show_error(wxWindow*, const wxString&)` (GUI.hpp), `ModelObject::input_file`.

- [ ] **Step 2: Dialog**

`ColorSplitDialog.hpp/.cpp`: a `wxDialog` titled "Split by painted colour" with a static summary (computed D, ws, caps, painted filaments, triangle count), a `wxTextCtrl` "Depth (mm)" prefilled with D (or "unlimited" state via a checkbox "Unlimited"), four `wxCheckBox`es: "Cap flat tops/bottoms at solid shell depth" (on), "Absorb enclosed islands" (on), "Keep base-colour sparse infill" (default = !paint_infill_override), "Solid colour interfaces (interface_shells)" (on); OK/Cancel (`CreateStdDialogButtonSizer`). Public getters return a `ColorSplitParams` plus the two bools. Keep it plain wx; wrap it in `wxGetApp().UpdateDlgDarkUI(this)` like other dialogs in the fork (search for `UpdateDlgDarkUI` for the call pattern).

- [ ] **Step 3: Plater action and menu**

Plater.hpp (next to `split_object`): `void split_by_color(); bool can_split_by_color() const;`

Plater.cpp:

```cpp
bool Plater::can_split_by_color() const
{
    int obj_idx = get_selection().get_object_idx();
    if (obj_idx < 0 || obj_idx >= int(model().objects.size())) return false;
    for (const ModelVolume *v : model().objects[obj_idx]->volumes)
        if (v->is_model_part() && v->is_mm_painted()) return true;
    return false;
}

void Plater::split_by_color()
{
    int obj_idx = get_selection().get_object_idx();
    if (obj_idx < 0) return;
    ModelObject &object = *model().objects[obj_idx];
    auto gizmo = canvas3D()->get_gizmos_manager().get_current_type();
    if (gizmo == GLGizmosManager::FdmSupports || gizmo == GLGizmosManager::Seam || gizmo == GLGizmosManager::FuzzySkin || gizmo == GLGizmosManager::MmSegmentation) {
        get_notification_manager()->push_plater_warning_notification(_u8L("Close the painting tool before splitting by colour."));
        return;
    }
    if (!get_ui_job_worker().is_idle()) {
        get_notification_manager()->push_plater_warning_notification(_u8L("Another operation is running; try again when it finishes."));
        return;
    }
    // Effective config: full preset config overlaid by object and (per target) volume config.
    DynamicPrintConfig base = wxGetApp().preset_bundle->full_config();
    base.apply(object.config.get(), true);
    std::vector<GUI::ColorSplitJob::Target> targets;
    ColorSplitDepths shown_depths; size_t tri_count = 0; std::vector<int> all_filaments;
    for (size_t vi = 0; vi < object.volumes.size(); ++vi) {
        ModelVolume *v = object.volumes[vi];
        if (!v->is_model_part() || !v->is_mm_painted()) continue;
        DynamicPrintConfig cfg = base; cfg.apply(v->config.get(), true);
        std::vector<int> filaments = v->get_extruders();            // painted states + own extruder (1-based)
        GUI::ColorSplitJob::Target t;
        t.object_id = object.id(); t.volume_id = v->id(); t.paint_timestamp = v->mmu_segmentation_facets.timestamp();
        t.space = color_split_space(object, *v);
        ColorSplitDepths d = color_split_depths(cfg, filaments);
        t.depths = t.space.world_path ? d : scale_depths(d, t.space.depth_scale);
        t.mesh = v->mesh().its;
        if (t.space.world_path) its_transform(t.mesh, t.space.to_split);
        t.paint = v->mmu_segmentation_facets.get_data();
        shown_depths = d; tri_count += t.mesh.indices.size(); all_filaments = filaments;
        targets.push_back(std::move(t));
    }
    if (targets.empty()) return;
    GUI::ColorSplitDialog dlg(this, shown_depths, all_filaments, tri_count, !base.opt_bool("paint_infill_override"));
    if (dlg.ShowModal() != wxID_OK) return;
    ColorSplitParams params = dlg.params();
    if (params.depth_override_mm > 0.)
        for (auto &t : targets) if (!t.space.world_path) t.depths.D = params.depth_override_mm / t.space.depth_scale, t.depths.unlimited = false;
    replace_job(get_ui_job_worker(), std::make_unique<GUI::ColorSplitJob>(this, std::move(targets), params, dlg.solid_interfaces(), dlg.keep_base_sparse_infill()));
}
```

Note the override handling: on the mesh-space path the override must be scaled like the other depths, so the job receives `depth_override_mm = 0` and the scaled D instead (set `params.depth_override_mm = 0.` after applying it per target). On the world path pass it through unchanged.

GUI_Factories.cpp — in each Split submenu block (:1424-1436, :1462-1474, :1595-1607, :1987-2002) add after the "To parts" item:

```cpp
    append_menu_item(split_menu, wxID_ANY, _L("By painted colour"), _L("Convert the part's colour painting into separate parts, one per filament"),
        [](wxCommandEvent&) { plater()->split_by_color(); }, "menu_split_parts", menu,
        []() { return plater()->can_split_by_color(); }, m_parent);
```

and change the submenu's enable lambda to `[]() { return plater()->can_split(true) || plater()->can_split_by_color(); }`. In the plain part menu (:1540-1549) add a sibling item "Split by painted colour" with the same callback/condition.

- [ ] **Step 4: Register sources and build the app**

Add `GUI/ColorSplitDialog.cpp GUI/ColorSplitDialog.hpp` and `GUI/Jobs/ColorSplitJob.cpp GUI/Jobs/ColorSplitJob.hpp` to `src/slic3r/CMakeLists.txt`. Check the build slot, run `build_next_wt.bat` (ALL_BUILD). Expected exit 0.

- [ ] **Step 5: GUI round (user)**

Report to the user with instructions: open a painted project, right-click → Split → By painted colour, accept defaults; expect body + `F<n>` parts in the list with filaments set; slice; verify the colour depth in the preview cross-section; undo restores the painted part; try with the paint gizmo open (refused); 3MF save/reload keeps the parts.

- [ ] **Step 6: Commit** — `feat(color-split): Split by painted colour action, dialog and job`.

---

### Task 10: Verification, docs and ledger

**Files:**
- Modify: `docs/superpowers/specs/2026-09-01-color-split-design.md` (status line + any spike-driven changes), `.superpowers/sdd/2026-09-01-color-split/progress.md`
- Create: `.superpowers/sdd/2026-09-01-color-split/task-N-report.md` as produced by each task's implementer

- [ ] **Step 1: Full verification**

Run, in this order, and paste the summary lines into the ledger:

```
libslic3r_tests.exe "[colorsplit]"
libslic3r_tests.exe "[colorsplit_spike]"
libslic3r_tests.exe "[paintdepth]"
libslic3r_tests.exe "[chameleon]"
libslic3r_tests.exe
```

Expected: all green (the full run has 2 known xfail cases from before). Run `[colorsplit]` twice to catch nondeterminism.

- [ ] **Step 2: Update the spec status** to "implemented (v1)" with a "Measured" section: S1/S3/S4 numbers from the spike report, and any deviation from §3 that the spike forced.

- [ ] **Step 3: Commit** — `docs(color-split): verification results, spec status, SDD ledger` (include the `.superpowers/sdd/2026-09-01-color-split/` directory; exclude build artefacts).

---

## Self-review notes (done while writing)

- Spec coverage (after the Task-5 insertion, 2026-09-02): 3.1/3.1a → Tasks 1/5 (smooth patches); 3.2–3.4 → Task 2/3; 3.5–3.7 → Task 3/5/6; 3.8 → Task 4; 3.9 + §4 → Task 7; §5 → Task 9; §6/§8.7 → Task 8; §7 errors → Tasks 3/4/9; §8 tests → Tasks 1–8; §9 spike → Task 4 step 5 (verdict: engine A) + Task 5 re-measure + Task 8 S4; §10 fallback engine → not needed; §11 deferred → out of scope.
- Type consistency: `ColorSplitProgress`, `ColorSplitDepths`, `ColorSplitParams`, `ColorShell`, `ShellCheck`, `ColorSplitResult`, `ColorSplitSpace` are defined once (Tasks 1–6) and used with the same names later; `apply_color_split` takes `(ModelObject&, size_t, ColorSplitResult&&, const ColorSplitSpace&, bool, bool)` everywhere.
- Known verification points for implementers (marked "check" in the code): `its_face_neighbors` edge convention, `TriangleSelector::select_patch` overload, `NormalUtils::Normals` type, `AABBMesh::query_ray_hits` ordering, `Manifold::Split` pair order, `LayerRegion::slices` accessor, `ModelVolume` member names for text/emboss/cut info, `Plater::TakeSnapshot` and `show_error` signatures.
