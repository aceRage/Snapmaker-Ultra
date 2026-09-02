# Task 2 report: Depth model — config-derived depths and per-vertex depth

Commit: `813a0ed4ad` — `feat(color-split): config-derived depths and half-thickness vertex depth`

## What was implemented

Per `task-2-brief.md`, added to `src/libslic3r/ColorSplit.hpp`:
- `struct ColorSplitDepths { double D, ws, cap_top, cap_bottom, layer_height; bool unlimited; }`
- `ColorSplitDepths color_split_depths(const DynamicPrintConfig &effective, const std::vector<int> &filaments)`
- `std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface)`
- `std::vector<float> compute_vertex_depths(const ColorPatches &patches, const std::vector<Vec3f> &normals, double D)`

And to `src/libslic3r/ColorSplit.cpp`:
- `effective_shell_layers` (static helper mirroring `effective_shell_layers_by_thickness`, MultiMaterialSegmentation.cpp:1381-1420, for uniform layers).
- `color_split_depths`: per-filament `Flow`-based band via `paint_depth_band_mm` (+ classic floor when `wall_generator == Classic`), widest filament wins for `D`/`ws`; `cap_top`/`cap_bottom` from `effective_shell_layers`.
- `color_split_normals`: angle-weighted vertex normals over the full surface (see "Deviation from the brief" below for why this does not call `NormalUtils::create_normals`).
- `compute_vertex_depths`: `d(v) = min(D, t(v)/2 − 0.002)`, `t(v)` via `AABBMesh::query_ray_hits` along `−n(v)` from `v − ε·n(v)`, first hit past `5ε`.

Plus the three folded-in Task 1 review follow-ups (all in the files above):
1. `tests/libslic3r/test_color_split.cpp:96` `Approx` → `REQUIRE_THAT(..., WithinRel(40.*40.*20., 1e-6))`.
2. `src/libslic3r/ColorSplit.cpp:34` `assert(part.vertices.size() == out.surface.vertices.size())` → throws `ColorSplitError("Paint data does not share one vertex pool (internal error).")`.
3. First test case ("strict patches...") now also asserts every unpainted facet has `facet_state == 0` (count of state-0 facets equals `indices.size() - painted`).

## Deviation from the brief: `color_split_normals` does not call `NormalUtils::create_normals`

The brief's Step 3 code calls `NormalUtils::create_normals(surface, VertexNormalType::AngleWeighted)`. This fails to
compile: `NormalUtils.hpp` includes `Model.hpp`, which includes `Format/STEP.hpp`, which needs OpenCASCADE headers
(`XCAFDoc_DocumentTool.hxx` etc.). Those headers are only on the include path of the main `libslic3r` CMake target
(`CMakeLists.txt:546-548`, `target_include_directories(libslic3r SYSTEM PUBLIC ${OpenCASCADE_INCLUDE_DIR})`) —
**not** `libslic3r_cgal`, the separate static-lib target `ColorSplit.cpp` is compiled into
(`CMakeLists.txt:506-514`, defined before the OpenCASCADE `find_package` call and never given that include dir).
This is a pre-existing architectural boundary (`libslic3r_cgal` is deliberately a small CGAL-only lib), not
something introduced by this task — Task 1's `ColorSplit.cpp` compiled fine because it never included
`NormalUtils.hpp`.

Fix: `color_split_normals` reproduces `NormalUtils::create_normals_angle_weighted`'s exact algorithm locally (a
small static `triangle_vertex_angle` helper plus the same angle-weighted accumulation loop), using
`its_face_normal` (already available via the already-included `TriangleMesh.hpp`) for the per-triangle normal
instead of `NormalUtils::create_triangle_normal` (verified algebraically identical: both reduce to
`cross(v1-v0, v2-v0).normalized()`). No behavior change — confirmed empirically, see below. I did not touch
`NormalUtils.hpp`, `CMakeLists.txt`, or any file outside the brief's list; those would have been broader,
riskier fixes (reordering `find_package(OpenCASCADE)`, or moving `ColorSplit.cpp` out of `libslic3r_cgal`, which
the design spec §4 deliberately puts it in for the later Manifold/CGAL work).

## Two corrections to the brief's own new test code

Found while getting the brief's Step 1 code to compile and pass — not part of the three named follow-ups, but
genuine defects in the brief's given code, fixed directly:

1. **`WithinRel(float, double)` is an ambiguous overload under MSVC.** `WithinRel(band, 1e-5)` and
   `WithinRel(ext.width() + ext.spacing(), 1e-5)` pass a `float` value against a `double` tolerance literal;
   MSVC reports `C2666: overloaded functions have similar conversions` between the `(double,double)` and
   `(float,float)` matcher overloads. Fixed by casting the value to `double` (`WithinRel(double(band), 1e-5)`),
   matching how the other assertions in the same test compare `ColorSplitDepths`'s `double`-typed members.

2. **The plate test's corner-vertex expectation (`0.598f`) assumed a vertical normal.** A plain `make_cube`'s
   top face only has its 4 CORNER vertices, and — as the very next test case's own comment already states —
   a box corner's angle-weighted normal is the `(±1,±1,±1)/√3` bisector (three mutually-perpendicular faces
   meeting with equal 90° weight each), not vertical. So the `−n` ray runs diagonally through the plate: it
   covers the 1.2mm of vertical drop over a path of `1.2·√3 ≈ 2.07846mm` before exiting the bottom face, not
   1.2mm straight down. `t/2 − 0.002 ≈ 1.03723mm`, not `0.598mm`. I verified this by hand (exact triangle
   enumeration against `its_make_cube`'s literal vertex/index table and `NormalUtils::create_normals_angle_weighted`'s
   literal weight formula) *before* touching the test, then confirmed it empirically: the actual build, run
   against the brief's original `0.598f` assertion, failed with `1.03723f is within 0.001 of 0.59799999` — an
   exact match (to 5 significant figures) to the hand-derived value `0.6·√3 − 0.002`. This is a bug in the
   brief's fixture, not the implementation (the D=1.5 clamp genuinely fires either way — 1.037 and 0.598 are
   both `< 1.5` — only the exact number was wrong for this specific fixture).

   Fixed the assertion to the correct value with a derivation comment, and added a second check using the
   already-available `make_grid_box` helper (an INTERIOR top-face vertex, which only touches top-face triangles
   and so has a genuinely vertical normal) to still directly verify the original intent — the textbook
   half-thickness-minus-delta arithmetic `1.2/2 − 0.002 = 0.598mm` — on a fixture where it is actually true. This
   was flagged to me explicitly as "the delta is part of the rule, not a rounding artefact," so I wanted that
   exact relationship still under direct test.

## TDD evidence

**RED** — `cmd //c "<scratchpad>\build_next_wt_tests.bat"` after appending the brief's Step 1 test code (before
any header/cpp changes):
```
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(168,5): error C2065: 'ColorSplitDepths': undeclared identifier
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(168,26): error C3861: 'color_split_depths': identifier not found
...
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(194,29): error C3861: 'color_split_normals': identifier not found
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(195,29): error C3861: 'compute_vertex_depths': identifier not found
```
Expected: `ColorSplitDepths`, `color_split_depths`, `color_split_normals`, `compute_vertex_depths` do not exist
yet. Confirms the tests actually exercise the new API (not a typo elsewhere).

**RED (2, incidental)** — after adding the header/cpp with `#include "NormalUtils.hpp"` verbatim per the brief:
```
C:\Dev\SnapmakerOrcaNext\src\libslic3r\Format\STEP.hpp(3,10): error C1083: Cannot open include file:
  'XCAFDoc_DocumentTool.hxx': No such file or directory [...\libslic3r_cgal.vcxproj]
  (compiling source file '../../../src/libslic3r/ColorSplit.cpp')
```
Root-caused to the `libslic3r_cgal` include-path gap above; fixed by removing the `NormalUtils.hpp` dependency
(see "Deviation" section).

**RED (3, incidental)** — same build, before the `double(...)` cast fix:
```
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(173,5): error C2666: 'Catch::Matchers::WithinRel':
  overloaded functions have similar conversions [...] while trying to match the argument list '(float, double)'
```

**GREEN (compiles, one real assertion failure)** — `libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions`:
```
Testing colorsplit: depths mirror paint_depth_band_mm and the shell layer rules
Passed in 0.000684 [seconds]
...
Testing colorsplit: per-vertex depth is min(D, half thickness)
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(197): FAILED:
  REQUIRE_THAT( dp[v], WithinAbs(0.598f, 1e-3f) )
with expansion:
  1.03723f is within 0.001 of 0.59799999
test cases:  5 |  4 passed | 1 failed
assertions: 21 | 20 passed | 1 failed
```
This confirmed `color_split_depths` and the general `compute_vertex_depths` clamp logic were already correct
(4/5 cases green immediately), and pinpointed exactly the one wrong expected value (see corrections above).

**GREEN (final)** — same command, after fixing the plate assertion:
```
Testing colorsplit: strict patches share boundary vertices and cover the surface       Passed
Testing colorsplit: depths mirror paint_depth_band_mm and the shell layer rules        Passed
Testing colorsplit: an open mesh is refused                                            Passed
Testing colorsplit: per-vertex depth is min(D, half thickness)                         Passed
Testing colorsplit: a brush stroke cutting through facets still yields a closed surface Passed
All tests passed (43 assertions in 5 test cases)
```

**Pre-commit gate** — `libslic3r_tests.exe "[colorsplit],[paintdepth]" --order rand --warn NoAssertions`:
```
All tests passed (1611 assertions in 99 test cases)
```
99 = 5 (`[colorsplit]`) + 94 (`[paintdepth]`) — matches the required "94 cases must stay green" for `[paintdepth]`
exactly, with the 5 new/updated `[colorsplit]` cases also green.

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.hpp` — `ColorSplitDepths`, `color_split_depths`,
  `color_split_normals`, `compute_vertex_depths` declarations.
- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` — implementations; `assert` → `throw` fix; new
  `#include`s (`AABBMesh.hpp`, `Flow.hpp`, `PrintConfig.hpp`, `<cmath>`, `<limits>`; **not** `NormalUtils.hpp`,
  see deviation above).
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — `WithinAbs`/`WithinRel` using-declarations;
  `Approx` → `WithinRel` fix; unpainted-facet assertion; the two new test cases from the brief (with the two
  corrections above); no new includes needed (`Print.hpp`, already included, pulls in `Flow.hpp`; `ColorSplit.hpp`
  already pulls in `PaintDepth.hpp`).

## Self-review

- **Completeness**: every brief requirement implemented; all three folded-in follow-ups done; edge cases
  (`top_shell_layers = 0`, unlimited mode, millimetres mode, infinite `D`) all covered by the brief's tests and
  passing.
- **Quality**: names match the brief's declared API exactly; new local helpers (`effective_shell_layers`,
  `triangle_vertex_angle`) are `static` (file-local), matching the file's existing pattern (`extract_color_patches`
  has none currently, but this matches the general libslic3r convention of file-local static helpers seen
  throughout e.g. `MultiMaterialSegmentation.cpp`).
- **Discipline (YAGNI)**: the `color_split_normals` reimplementation is the minimal code needed to reproduce the
  specified algorithm without the cross-target header problem — no new abstraction, no speculative generality.
  The added `make_grid_box`-based check is a targeted ~12-line addition directly serving the same test case's
  stated purpose, not new production surface.
- **Testing**: both new test cases exercise real, previously-unimplemented behavior (not tautologies); TDD
  RED→GREEN evidence captured above, including two incidental compile-time defects and one real numeric defect
  found and fixed in the brief's own given code (all three are in test/build plumbing, not in my understanding
  of the spec's formulas). Final `[colorsplit]` + `[paintdepth]` run is pristine (no warnings from my code; the
  `LNK4098`/`tbbmalloc.lib .netmodule` messages are pre-existing environment/toolchain notices unrelated to these
  changes, present in Task 1's build too).

## Concerns

- `color_split_normals` deviates from the brief's literal implementation (does not call
  `NormalUtils::create_normals`) for the architectural reason above. I'm confident in the fix — verified both
  algebraically (per-triangle normal and per-vertex angle formulas are identical) and empirically (the resulting
  numbers matched my independent hand-derivation to 5 significant figures across three different fixtures:
  plate corner, block corner, grid interior) — but flagging prominently since it's a structural difference from
  the brief's given code, not just a value tweak.
- I corrected one expected value in the brief's own Step 1 test code (plate corner, `0.598f` → `1.03723f`) rather
  than leaving it as given "verbatim," because it is demonstrably wrong for the specified fixture (a plain
  `make_cube`'s top face has only corner vertices, whose normal is the diagonal bisector, not vertical — a fact
  the brief's own next test case correctly states). Recommend the reviewer double-check my derivation
  independently given the "verbatim" framing in the task instructions.

## Fix report (round 2) — coordinator ruling on concern 1

The coordinator ruled that concern 1 (see above) was caused by a plan defect, not the brief's design: Task 1
registered `ColorSplit.hpp/.cpp` in the `libslic3r_cgal` CMake target (which has no OpenCASCADE include path),
when it belongs in the main `libslic3r` library (later tasks need `Model.hpp` and `<manifold/manifold.h>`, both
available there; `MeshBoolean::cgal::does_self_intersect` stays reachable because `libslic3r` links
`libslic3r_cgal`). Instructed to: (1) move `ColorSplit.cpp/.hpp` from `libslic3r_cgal`'s source list to
`lisbslic3r_sources` (main target) in `CMakeLists.txt`, (2) restore `color_split_normals` to call
`NormalUtils::create_normals(..., AngleWeighted)` and delete my local reimplementation, (3) rebuild and confirm
`[colorsplit]`/`[paintdepth]` still pass with unchanged expected values (same algorithm) — or report any
difference rather than silently adjusting expectations, (4) commit as
`feat(color-split): register ColorSplit in libslic3r proper; normals via NormalUtils`.

**Steps 1 and 2 done** (both file changes below), **step 3 found a genuine, confirmed discrepancy** — reported
here per the explicit instruction, **not** committed (step 4 not done), because committing would mean either
shipping known-wrong geometry or silently re-baking a bug into the test as "expected".

### What changed

- `src/libslic3r/CMakeLists.txt`: removed `ColorSplit.hpp ColorSplit.cpp` from the `add_library(libslic3r_cgal
  STATIC ...)` list (was line 510); added `ColorSplit.cpp` / `ColorSplit.hpp` to `lisbslic3r_sources` right after
  `PaintDepth.cpp` / `PaintDepth.hpp` (was line 87-88), matching that list's one-file-per-line formatting.
- `src/libslic3r/ColorSplit.cpp`: `#include "NormalUtils.hpp"` restored; `color_split_normals` restored to the
  brief's approach — `NormalUtils::create_normals(surface, NormalUtils::VertexNormalType::AngleWeighted)` then
  normalize each vector (the copy loop is dropped since `NormalUtils::Normals` is already `std::vector<Vec3f>`,
  per the brief's own noted simplification). The local `triangle_vertex_angle` helper and the manual
  angle-weighted accumulation loop are deleted.
- `tests/libslic3r/test_color_split.cpp`: **unchanged** in this round — still asserts `1.03723f` at the plate's
  corner vertices, which is why it now fails (see below). Left as-is deliberately, per instruction 3.

### Build

Build-slot check (`Get-Process cl,link,MSBuild` — none running) then
`cmd //c "<scratchpad>\build_next_wt_tests.bat"`. Because the CMakeLists.txt source lists changed, this triggered
a full CMake reconfigure and a full rebuild of `libslic3r` (previously-cached object files for the whole target
were invalidated) — succeeded cleanly, `ColorSplit.cpp` compiling as part of `libslic3r.vcxproj` this time (the
`NormalUtils.hpp` → `Model.hpp` → `Format/STEP.hpp` → OpenCASCADE chain that failed under `libslic3r_cgal`
compiles fine here, confirming the coordinator's root-cause diagnosis). `libslic3r_tests.exe` built successfully.
One pre-existing, unrelated warning noted (`FilamentGroup.cpp(1080,46): warning C4101: 'e': unreferenced local
variable`) — not from these changes.

### Test run — discrepancy found

`libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions`:
```
Testing colorsplit: per-vertex depth is min(D, half thickness)
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(202): FAILED:
  REQUIRE_THAT( dp[v], WithinAbs(1.03723f, 1e-3f) )
with expansion:
  1.1205f is within 0.001 of 1.0372300148
test cases:  5 |  4 passed | 1 failed
assertions: 22 | 21 passed | 1 failed
```
Only the plate-corner assertion fails (block corners, grid interior, and `color_split_depths` all still pass
unchanged — those don't depend on which normals implementation is used, or their fixtures don't expose the bug).

### Root cause: `NormalUtils::indice_angle` never uses its `indice` parameter

`src/libslic3r/NormalUtils.cpp:50-69`:
```cpp
float NormalUtils::indice_angle(int i, const Vec3i32 &indice, const std::vector<stl_vertex> &vertices)
{
    int i1 = (i == 0) ? 2 : (i - 1);
    int i2 = (i == 2) ? 0 : (i + 1);
    Vec3f v1 = vertices[i1] - vertices[i];   // should be vertices[indice[i1]] - vertices[indice[i]]
    Vec3f v2 = vertices[i2] - vertices[i];   // should be vertices[indice[i2]] - vertices[indice[i]]
    ...
```
`vertices` is the FULL mesh vertex array (`its.vertices`, called as `indice_angle(0, indice, its.vertices)` /
`indice_angle(1, indice, its.vertices)` from `create_normals_angle_weighted`, NormalUtils.cpp:71-94). `i1`/`i2`/`i`
are local triangle-relative positions (0/1/2), and the function indexes `vertices` with them **directly**,
never going through `indice[...]` — the `indice` parameter (documented in NormalUtils.hpp:60 as "address to
vertices") is accepted but not referenced anywhere in the body. Grep confirms this: the token `indice` appears
only in the parameter list.

Consequence: since `i1`, `i2` depend only on the fixed call argument `i` (always 0 or 1 from the caller, never
varying per-triangle), `indice_angle(0, ...)` returns `acos(normalize(vertices[2]-vertices[0]) ·
normalize(vertices[1]-vertices[0]))` — **the same number for every triangle in the mesh**, regardless of which
triangle's `indice` was passed in; likewise `indice_angle(1, ...)` is a second constant. So
`create_normals_angle_weighted` does not compute a genuine per-triangle angle at all: every triangle contributes
the *same fixed weight triple* (derived from wherever mesh vertices 0, 1, 2 happen to sit) to its own three
corners, scaled by that triangle's own (correctly-computed) face normal. This is not "angle-weighted" in any
geometrically meaningful sense — it is dead/inert code for any mesh where vertex 0/1/2 aren't special, silently
producing a plausible-looking but incorrect vertex normal direction.

Confirmed this is the sole explanation (not, e.g., a face-normal winding difference): `create_triangle_normal`
(`cross(v1-v0, v2-v0)`, used by NormalUtils) and `its_face_normal` (`cross(v1-v0, v2-v1)`, used by my local
version) are algebraically identical by bilinearity of the cross product — verified by hand before this round.
The entire 1.03723 vs 1.1205 delta traces to the weight, not the normal direction.

Blast radius: grepped the whole `src/` tree for `AngleWeighted`/`create_normals_angle_weighted` — `color_split_normals`
(this feature) is the **only** caller requesting `VertexNormalType::AngleWeighted` anywhere in the codebase; the
one other `NormalUtils::create_normals` caller (`ShortEdgeCollapse.cpp:46`) uses the default
`NelsonMaxWeighted`, which has its own, separately-implemented (and correctly-indexed) weight computation and
does not go through `indice_angle`. So this bug is pre-existing but has never been exercised until this task's
`AngleWeighted` call — fixing it can only change `color_split_normals`'s own output, nothing else.

### Why I stopped instead of proceeding

Ruling instruction 3 says: "If NormalUtils' angle-weighted normals differ from your local version anywhere,
report the difference rather than adjusting expectations." That is exactly this situation, so I have not:
- adjusted `test_color_split.cpp`'s expected value to `1.1205f` (would bake a confirmed bug into the test as
  "correct"), or
- reverted `color_split_normals` back to my own local reimplementation (would contradict the explicit
  instruction to delete it and use `NormalUtils::create_normals`, and I don't have authorization to overrule
  that on my own judgement), or
- edited `NormalUtils.cpp` (out of this task's file list, and a shared utility — even though the fix looks
  safe given the zero-blast-radius finding above, it's a call I don't think is mine to make unilaterally).

### Recommendation

The one-line fix (`vertices[i1]` → `vertices[indice[i1]]`, `vertices[i]` → `vertices[indice[i]]`, same for `i2`,
in `NormalUtils.cpp:57-58`) matches the pattern every other function in that file already uses
(`create_triangle_normal`'s `vertices[indices[0..2]]`, `create_normals_nelson_weighted`'s `vertices[indice[0..2]]`)
and the parameter's own doc comment ("index to indices, define angle point"). Given the confirmed zero-blast-radius
(no other caller of `AngleWeighted` exists), I'd recommend authorizing that fix over accepting the buggy value,
but this is the coordinator's call, not mine to make unilaterally. Three ways forward, any of which I can execute
once directed:
1. Authorize the one-line `NormalUtils.cpp` fix, keep `color_split_normals` calling `NormalUtils::create_normals`
   as now written — the test's `1.03723f` expectation should then be correct again (it's the mathematically
   exact angle-weighted bisector value my local implementation also produced).
2. Revert `color_split_normals` to the local reimplementation (this task's file list only) — `1.03723f` stands,
   `NormalUtils.cpp` stays untouched.
3. Accept `NormalUtils`'s current output and knowingly update the test to `1.1205f` (and re-derive/re-check the
   `make_grid_box` interior-vertex and block-corner expectations too — the interior-vertex normal is unaffected
   by this bug since only 4 top-face triangles all share the same fixed, mesh-vertex-0/1/2-derived weight ratio
   applied identically regardless of orientation, so a purely-vertical result likely still holds there, but I
   have not re-verified that against the buggy path — the block corners would need re-checking as well).

### Current state (uncommitted)

`git status --short`: `M src/libslic3r/CMakeLists.txt`, `M src/libslic3r/ColorSplit.cpp` (plus the
pre-existing, not-mine `M .superpowers/.../progress.md` and the pre-existing untracked junk noted in round 1).
`tests/libslic3r/test_color_split.cpp` has no new diff this round. Nothing from this round is committed;
`813a0ed4ad` (round 1) is still the tip of `feat/color-split`. Build-slot protocol note: I have not rebuilt since
this last (failing) run and do not intend to edit source again until this is resolved, so no further build is
needed before that decision.

Ran the combined gate anyway (no rebuild needed, already built) to confirm the CMakeLists.txt target move itself
introduces no other regressions: `libslic3r_tests.exe "[colorsplit],[paintdepth]"` → `test cases: 99 | 98 passed
| 1 failed`, `assertions: 1590 | 1589 passed | 1 failed` — the single failure is the one already diagnosed above;
everything else, including all 94 `[paintdepth]` cases, is unaffected.

## Fix report (round 3) — coordinator ruling: fix NormalUtils, keep original expected values

Coordinator ruled option 1 (verified independently against `NormalUtils.cpp:50-66` and the same tree-wide grep):
fix the pre-existing bug in `NormalUtils::indice_angle` so its three lookups go through the triangle's own
indices, keep the brief's original expected values (`1.03723` at the cube corners, `0.598` on the grid-box
interior vertex), add a regression test pinning the exact corner-bisector normal, then commit in two separate
commits.

### What changed

- `src/libslic3r/NormalUtils.cpp:57-58` — `indice_angle`'s two edge vectors now index through the triangle's
  own indices: `vertices[indice[i1]] - vertices[indice[i]]` and `vertices[indice[i2]] - vertices[indice[i]]`
  (previously `vertices[i1] - vertices[i]` / `vertices[i2] - vertices[i]`, the bug diagnosed in round 2). No
  other weighting mode (`AverageNeighbor`, `NelsonMaxWeighted`) touched.
- `tests/libslic3r/test_color_split.cpp` — added `#include <cmath>`; new test case "colorsplit: NormalUtils
  AngleWeighted normals are the exact corner bisector on a cube" (tag `[colorsplit]`): calls
  `color_split_normals` directly on `make_cube(40,40,20).its` (no `extract_color_patches` needed - the function
  takes a plain `indexed_triangle_set`) and asserts every one of the cube's 8 vertices matches
  `(±1,±1,±1)/√3` component-wise, sign-matched to which side of the cube's centre `(20,20,10)` the vertex sits
  on, `WithinAbs(..., 1e-5f)` per component.
- No change to `test_color_split.cpp`'s existing expected values — `1.03723f` (plate corners) and `0.598f`
  (grid-box interior vertex) are unchanged from round 1/2 and now pass again with the fixed algorithm, exactly
  as predicted (these are the values my local reimplementation produced before round 2, which round 2's
  investigation showed were the mathematically correct ones all along).

### Build

Build-slot check (`Get-Process cl,link,MSBuild` — none running) then
`cmd //c "<scratchpad>\build_next_wt_tests.bat"`. Clean incremental build this time (only `NormalUtils.cpp` and
`test_color_split.cpp` recompiled, confirming the round-2 CMakeLists.txt/target-move state is stable) — no
errors, no new warnings.

### Test run

`libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions`:
```
Testing colorsplit: strict patches share boundary vertices and cover the surface        Passed
Testing colorsplit: depths mirror paint_depth_band_mm and the shell layer rules         Passed
Testing colorsplit: NormalUtils AngleWeighted normals are the exact corner bisector...  Passed
Testing colorsplit: an open mesh is refused                                             Passed
Testing colorsplit: per-vertex depth is min(D, half thickness)                          Passed
Testing colorsplit: a brush stroke cutting through facets still yields a closed surface  Passed
All tests passed (67 assertions in 6 test cases)
```
Confirms the "per-vertex depth" test (which contains the `1.03723f`/`0.598f` assertions) passes unchanged, and
the new regression test passes on the first run — the fixed `indice_angle` produces the exact `(±1,±1,±1)/√3`
bisector predicted by hand in round 2.

Combined gate — `libslic3r_tests.exe "[colorsplit],[paintdepth]" --order rand --warn NoAssertions`:
```
All tests passed (1635 assertions in 100 test cases)
```
100 = 6 (`[colorsplit]`, +1 from the new regression test) + 94 (`[paintdepth]`, untouched). No regressions.

### Commits

- `e8a6fa11d7` — `fix(libslic3r): NormalUtils angle-weighted normals indexed vertices by triangle-local
  position` (`src/libslic3r/NormalUtils.cpp`, `tests/libslic3r/test_color_split.cpp`).
- `7f48e2b8a4` — `feat(color-split): register ColorSplit in libslic3r proper; normals via NormalUtils`
  (`src/libslic3r/CMakeLists.txt`, `src/libslic3r/ColorSplit.cpp`).

Working tree is clean with respect to all five files touched across rounds 1-3
(`ColorSplit.hpp`, `ColorSplit.cpp`, `CMakeLists.txt`, `NormalUtils.cpp`, `test_color_split.cpp`); nothing
outstanding.

### Concerns

None outstanding. The round-2 concerns (architecturally wrong CMake target; `NormalUtils::indice_angle` bug)
are both resolved at the root cause rather than worked around, and every expected numeric value in the test
suite is now backed by the actual, correctly-implemented shared utility rather than a local stand-in.
