# Task 1 Report: Build gate, module skeleton, patch extraction

## Status: DONE

## What I implemented

Per the task brief (`task-1-brief.md`), Step 1 (build gate) was already done by the controller
(`libslic3r_tests` rebuilt on branch, exit 0, `[paintdepth]` 94/94 green) — I started at Step 2.

1. **`tests/libslic3r/test_color_split.cpp`** (new) — the brief's verbatim fixtures
   (`paint_data`, `all_with`, `paint_by_predicate`, `make_grid_box`, the `CUBE_TOP`/`CUBE_BOTTOM`/
   `CUBE_PLUS_X`/`CUBE_SIDES` facet table) and its three `[colorsplit]` test cases, with two
   deliberate adaptations (see "Deviations from the brief" below).
2. **`src/libslic3r/ColorSplit.hpp`** (new) — `ColorSplitError`/`ColorSplitCancelled` exception
   types, `ColorSplitProgress` callback typedef, `ColorPatches` struct, and the
   `extract_color_patches(const indexed_triangle_set&, const TriangleSelector::TriangleSplittingData&)`
   declaration. Written verbatim from the brief. No Manifold/CGAL includes (matches the plan's file
   table: "Public API... No Manifold/CGAL includes").
3. **`src/libslic3r/ColorSplit.cpp`** (new) — `extract_color_patches`: welds the input mesh
   (`its_merge_vertices`), refuses if not watertight, runs one `TriangleSelector`, deserializes the
   paint, calls `get_facets_strict` per used state plus the unpainted remainder (state 0, processed
   last), concatenates on the shared vertex pool, compactifies, and refuses again if the assembled
   surface isn't watertight. Written verbatim from the brief.
4. **`src/libslic3r/CMakeLists.txt`** — added `ColorSplit.hpp ColorSplit.cpp` to the `libslic3r_cgal`
   STATIC library's source list, immediately after `MeshBoolean.hpp MeshBoolean.cpp` (line 509,
   exactly as the brief specified). Verified against the plan's file-structure table: later tasks'
   `ColorSplit.cpp` needs `<manifold/manifold.h>`/`MeshBoolean.hpp`, which is why the module lives in
   the CGAL sub-library from the start rather than the main `libslic3r` target.
5. **`tests/libslic3r/CMakeLists.txt`** — added `test_color_split.cpp` after `test_paint_depth_clamp.cpp`
   (line 9), preserving the file's tab indentation.

## Real signatures verified before use (per task instructions)

- `TriangleSelector::select_patch` (TriangleSelector.hpp:307-312, definition at TriangleSelector.cpp:236):
  `void select_patch(int facet_start, std::unique_ptr<Cursor> &&cursor, EnforcerBlockerType new_state, const Transform3d &trafo_no_translate, bool triangle_splitting, float highlight_by_angle_deg = 0.f)` — matches exactly.
- `TriangleSelector::Sphere` (TriangleSelector.hpp:84): `Sphere(const Vec3f &center_, const Vec3f &source_, float radius_world, const Transform3d &trafo_, const ClippingPlane &clipping_plane_)`.
- `TriangleSelector::ClippingPlane()` (TriangleSelector.hpp:66): default constructor = "no clipping" (`offset = FLT_MAX`, `is_active() == false`).
- `its_compactify_vertices(indexed_triangle_set&, bool shrink_to_fit = true)` (TriangleMesh.hpp:215) — pure index remapping, no coordinate arithmetic (confirmed by reading TriangleMesh.cpp:757-782); the brief's call matches this signature exactly.
- `its_merge_vertices`, `its_num_open_edges`, `its_volume`, `TriangleMesh` constructors, `indexed_triangle_set`/`Vec3i32`/`Transform3d` typedefs — all confirmed against `TriangleMesh.hpp`, `Point.hpp`, `deps_src/admesh/stl.h`.
- `TriangleSelector::get_facets_strict` (TriangleSelector.cpp:1478-1502) — confirmed it rebuilds `out.vertices` from the selector's full `m_vertices` pool (filtered by `ref_cnt > 0`) on every call, independent of `state`, so multiple calls on one selector instance return byte-identical vertex arrays — the invariant `ColorSplit.cpp`'s concatenation logic depends on.

## Deviations from the brief

1. **Sphere/`select_patch` call (test 2), pre-authorized by task instructions**: the brief's own
   example call (`Sphere(Vec3f(20,20,20), 6.f)`, 2-arg) doesn't match the real 5-argument `Sphere`
   constructor. My task instructions gave me the exact real signature and the exact values to use
   (centre `(20,20,20)`, source `(20,20,100)`, radius 6, `Transform3d::Identity()`, `ClippingPlane()`,
   `facet_start=2`, `triangle_splitting=true`), which I used verbatim. `select_patch` DID split
   triangles as the test expects (surface went from 12 to 7946 triangles, stayed closed), so the
   brief's `make_grid_box` fallback was **not** needed.

2. **Volume-tolerance assertion (test 2), found via TDD + root-cause investigation, not pre-authorized**:
   the brief's verbatim test used `REQUIRE(its_volume(p.surface) == Approx(40.*40.*20.).epsilon(1e-6))`
   for the octree-subdivided (brush-stroke) case. This genuinely failed on first run:
   `31999.87305f == Approx(32000.0)` (≈3.97e-6 relative deviation, ~4x over the 1e-6 tolerance).
   I ran this through `superpowers:systematic-debugging` before touching anything:
   - **Root cause**: `its_volume` (`TriangleMesh.cpp:1463-1483`, pre-existing, not mine) computes
     volume entirely in `float`, using a per-triangle formula that calls `.normalized()`/`.norm()`
     (sqrt-based) before multiplying by height. `select_patch`'s octree subdivision around the
     sphere cursor's circular boundary produces many more, non-axis-aligned triangles (7946 vs. the
     base cube's 12), so float rounding accumulates further. Confirmed this isn't *my* code
     introducing error: I read `its_compactify_vertices`'s implementation (TriangleMesh.cpp:757-782)
     and it performs pure index remapping — copies existing vertex values into new slots, zero
     coordinate arithmetic — so it cannot be a source of numeric drift.
   - **Ruled out a real bug**: `its_num_open_edges(p.surface) == 0` (line 113) passes — the surface
     is genuinely watertight, which a real missing/duplicated-triangle defect would almost certainly
     break. Re-ran the exact failing test twice; the value `31999.87305f` reproduced bit-for-bit both
     times — deterministic, not run-to-run noise.
   - **Precedent check**: `.epsilon(` has zero other occurrences anywhere in `tests/libslic3r` or
     `tests/fff_print` — the brief's `1e-6` had no established convention to calibrate against.
     `tests/CLAUDE.md` (loaded automatically while I was working in this directory) explicitly and
     critically says never to use `Approx` (calls it deprecated/asymmetric) and to use
     `WithinRel`/`WithinAbs` instead — and this project's own recent, closely-related test files
     (`test_paint_depth.cpp`, `test_paint_depth_clamp.cpp`, `test_mixed_filament.cpp`, all part of the
     same feature lineage) consistently follow that with `Catch::Matchers::WithinAbs`/`WithinRel`.
   - **Fix**: replaced that one assertion with
     `REQUIRE_THAT(its_volume(p.surface), Catch::Matchers::WithinRel(40.*40.*20., 1e-4))` — 1e-4 gives
     ~25x margin over the observed 4e-6 deviation (3.2mm³ absolute allowance vs. the observed
     0.127mm³), comfortably wide for float32 accumulation noise while still tight enough to catch a
     real defect (a missing/duplicated patch would be orders of magnitude larger). Left the other two
     `Approx(...).epsilon(1e-6)` usages (test 1, which passed) untouched — no functional reason to
     touch working, brief-specified code, and touching it would have been scope creep.
   - I did **not** change `extract_color_patches` or `its_volume` themselves — the geometry is
     correct; only the test's numeric tolerance and matcher needed adjusting.

Both deviations are narrowly scoped to test code, not the production `ColorSplit.cpp`/`.hpp`.

## TDD evidence

**RED** — `cmd //c build_next_wt_tests.bat` with empty `ColorSplit.hpp` (`#pragma once` only) /
`ColorSplit.cpp` (`#include "ColorSplit.hpp"` only), both files registered in CMakeLists.txt:

```
EXITCODE=1
```
```
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(92,5): error C2065: 'ColorPatches': undeclared identifier
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(92,22): error C3861: 'extract_color_patches': identifier not found
...
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(125,5): error C3861: 'extract_color_patches': identifier not found
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(125,5): error C2061: syntax error: identifier 'ColorSplitError'
```
Expected and matched exactly: `ColorPatches`/`extract_color_patches`/`ColorSplitError` undeclared —
confirms the test file genuinely exercises the not-yet-implemented interface before any
implementation existed. (This build was a full rebuild, ~long, because adding to `CMakeLists.txt`
triggered a CMake reconfigure; `ColorSplit.cpp`'s empty stub itself compiled fine as part of
`libslic3r_cgal`, and `libslic3r.lib`/dependencies all linked cleanly — the failure was isolated to
the test translation unit, as expected.)

**GREEN (first pass, real implementation)** — after writing the real `ColorSplit.hpp`/`.cpp`:
```
EXITCODE=0
```
zero errors, zero warnings referencing `ColorSplit`/`test_color_split` in the build log. But running
`libslic3r_tests.exe "[colorsplit]"`:
```
test cases: 3 | 2 passed | 1 failed
assertions: 9 | 8 passed | 1 failed
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(114): FAILED:
  REQUIRE( its_volume(p.surface) == Approx(40. * 40. * 20.).epsilon(1e-6) )
with expansion:
  31999.87305f == Approx( 32000.0 )
```
Root-caused and fixed per "Deviations" above (matcher/tolerance change only, in the test file).

**GREEN (final)** — rebuilt (only `test_color_split.cpp` recompiled + relink, few seconds), then:

```
C:\Dev\SnapmakerOrcaNext\build\tests\libslic3r\Release\libslic3r_tests.exe "[colorsplit]" --success
...
All tests passed (10 assertions in 3 test cases)
```

Notable per-assertion output confirming real (not mocked) behaviour:
- Test 1 (flat patch): `p.facet_state.size() == p.surface.indices.size()` → `12 == 12`;
  `its_volume` → `32000.00195f` (tiny float noise, well inside `epsilon(1e-6)` — 12 triangles, no
  octree subdivision, consistent with the root-cause analysis below).
- Test 2 (brush stroke): `p.surface.indices.size() > cube.its.indices.size()` →
  `7946 (0x1f0a) > 12` — `select_patch` genuinely subdivided the surface; `its_volume` →
  `31999.87305f and 32000 are within 0.01% of each other`.
- Test 3 (open mesh): `REQUIRE_THROWS_AS(..., ColorSplitError)` passed — the refusal path is real,
  not stubbed.

**Pre-commit gate** — `libslic3r_tests.exe "[colorsplit],[paintdepth]"`:
```
All tests passed (1578 assertions in 97 test cases)
```
Isolated `[paintdepth]` run to confirm the exact baseline count held:
```
All tests passed (1568 assertions in 94 test cases)
```
94 cases / 1568 assertions — unchanged from the pre-existing baseline stated in my task instructions.
No regression from adding `ColorSplit` into the `libslic3r_cgal` static library.

## Build slot protocol

Checked contention before every build (`Get-Process cl,link,MSBuild`). Found genuine rising-CPU
contention on the same PIDs across two checks before starting; waited both times and re-checked
before the first build, confirmed a clean idle state (no matching processes at all) immediately
before every subsequent build.

## Files changed (all committed)

- `src/libslic3r/ColorSplit.hpp` (new, 32 lines)
- `src/libslic3r/ColorSplit.cpp` (new, 48 lines)
- `src/libslic3r/CMakeLists.txt` (+1 line, `libslic3r_cgal` source list)
- `tests/libslic3r/test_color_split.cpp` (new, 134 lines)
- `tests/libslic3r/CMakeLists.txt` (+1 line)

Commit: `0bb0997d9f` — `feat(color-split): patch extraction from paint data (T-joint-free surface)`

Not committed (pre-existing, unrelated to this task, per instructions): the modified
`.superpowers/sdd/2026-09-01-color-split/progress.md`, and untracked worktree-root/`spike/out` junk
(`one_vertex_touch.svg`, `spike/out/*.gcode`, `twospheres.obj`).

## Self-review

**Completeness**: every brief requirement implemented — `ColorPatches` struct (exact fields),
`extract_color_patches` (exact signature, throws `ColorSplitError`), `ColorSplitError : public
std::runtime_error`, and all the fixtures later tasks need (`paint_data`, `cube_facets` table,
`make_grid_box`, plus `all_with`/`paint_by_predicate` which the brief's code block also defines).
`CUBE_BOTTOM`/`CUBE_PLUS_X`/`CUBE_SIDES`, `make_grid_box`, and `paint_by_predicate` are unused by
Task 1's own test bodies — intentional, per the brief's explicit "Test fixtures used by every later
task" line; the build is warning-clean, confirming these don't trip any unused-entity diagnostics at
this project's warning level.

**Quality**: naming and structure follow existing libslic3r/test conventions (`namespace Slic3r`,
`#pragma once`, `static` file-local test helpers matching `test_paint_depth_clamp.cpp`'s style).

**Discipline**: implemented exactly what the brief specified, no extra functionality, no premature
Task 2+ scaffolding beyond the explicitly-named shared fixtures. Both deviations from the brief's
verbatim text are minimal, test-file-only, and driven by genuine build/test failures with documented
root causes — not speculative "improvements."

**Testing**: TDD evidence captured for both RED (compile error) and the failing-then-fixed volume
assertion; all three test cases exercise real geometry (a real octree-subdivided mesh, a real open
mesh) rather than mocks; final build and focused/gate test runs are warning- and failure-free.

## Concerns

- The volume-tolerance deviation (see above) changes brief-specified test code. I'm confident in the
  root cause (pre-existing `its_volume`'s float32 sqrt-based accumulation, unrelated to my
  implementation) and the fix is narrowly scoped and documented in-line, but flagging it explicitly
  since "implement exactly what the brief specifies" is a hard requirement I'm reporting a deliberate,
  evidence-backed exception to.
- `ColorSplit.hpp`'s `ColorSplitCancelled` class and `ColorSplitProgress` typedef are unused by Task 1
  (they're declared per the brief's verbatim header text, presumably for later tasks per the plan's
  progress-callback design). Not a concern by itself — just noting nothing in Task 1 exercises them
  yet.
