# Task 3 report — Shell construction with validity checks

Branch `feat/color-split` in `C:\Dev\SnapmakerOrcaNext`. Two commits:

- `48ae71fcd8` `fix(color-split): refuse depth configs that cannot yield a depth; pin patch states to geometry`
- `dcb077c19f` `feat(color-split): closed shells per painted component with fold guard and validity check`

## What I implemented

### Header (`src/libslic3r/ColorSplit.hpp`)

`ColorSplitParams` (flat_cap / absorb_islands / crease_step / depth_override_mm), `ColorShell`, `ShellCheck`,
`check_shell(const indexed_triangle_set &)`, `build_color_shells(patches, depths, params, progress)` — exactly the
brief's declarations.

### Implementation (`src/libslic3r/ColorSplit.cpp`)

Anonymous-namespace helpers plus two exported functions:

- `connected_components(patches, nbrs, in_set)` — edge-connected components of a facet subset.
- `struct ShellBuilder` — `build(group, cap_depth)` (top + reversed bottom + side strips, wedge duplication at
  pinch vertices), `fold_guard(group)` (spec 3.4), `halve_depth(group)` (the validity retry) and the shared
  private-in-spirit `halve(vertices)`.
- `check_shell` — `its_num_open_edges == 0`, `its_volume` (signed; positive for an outward-oriented closed mesh —
  verified in TriangleMesh.cpp:1463, it accumulates `area * signed height / 3`), and
  `MeshBoolean::cgal::does_self_intersect(TriangleMesh(shell))`.
- `build_color_shells` — per painted state, per component: fold guard (≤ 8 rounds) → build → check → up to 6
  halve-and-rebuild retries → refuse with the filament named. `flat_cap`/`crease_step` are behaviourally OFF
  (cap depth is always 0), which is Task 5's slot; `ColorShell::capped` is therefore always false.

### Verifications the brief asked for

1. **`its_face_neighbors` edge convention — the brief's assumption is CORRECT, no adaptation needed.**
   `create_face_neighbors_index` (MeshSplitImpl.hpp:224-272) derives its edge from
   `its_triangle_edge(triangle_indices, edge_index)`, which is `{t[k], t[(k+1)%3]}` (TriangleMesh.hpp:253-257).
   The reverse assignment `neighbors[other_face][vertex_index] = face_idx` uses the position of `b` in the other
   face and is gated on `edge_indices[0] == face_indices[(vertex_index + 1) % 3]`, i.e. the same convention on
   the other side. Both places in `build()` that derive `(a, b)` from `k` are therefore correct as written.
2. **`TriangleMesh(const indexed_triangle_set &)`** (TriangleMesh.cpp:66-69) copies `its` verbatim and only fills
   statistics — no repair, no reordering — so it is the right constructor for the CGAL call, which reads
   `mesh.its.vertices/indices` directly (MeshBoolean.cpp:460-465).
3. **`its_volume` is signed** (TriangleMesh.cpp:1463-1483): `area * normal·(p - p0) / 3` summed per triangle,
   positive for an outward-oriented closed mesh. Confirmed empirically: the block-top shell measures +1326.51.

### Three folded Task-2 follow-ups (commit `48ae71fcd8`)

1. `tests/libslic3r/test_color_split.cpp` — the tautological "all non-painted facets are state 0" check is
   replaced by a per-facet geometry correlation: centroid z ≈ 20 ⇒ state 2, otherwise state 0.
2. `color_split_depths` throws `ColorSplitError("Printer profile has no nozzle diameters.")` when
   `nozzle_diameter` is empty (the `nozzles.size() - 1` index underflowed).
3. `color_split_depths` throws `ColorSplitError("No filaments to derive the split depth from.")` on an empty
   `filaments` list, with a `REQUIRE_THROWS_AS` test for both.

## Deviations from the brief (please rule)

### 1. The block-top volume bound in the brief's first test is unreachable — corrected with a derivation

The brief asserts `c.volume > 37. * 37. * 1.5` (= 2053.5). That assumes the slab is 1.5 mm deep **vertically**,
but d is measured **along the normal**, and the only vertices of a plain cube's top face are its four corners,
whose angle-weighted normals are the `(±1,±1,1)/√3` bisectors. Each corner therefore moves 1.5/√3 = 0.866 mm in
x, in y **and** in z: the shell is the frustum between the 40×40 top and a 38.268×38.268 bottom 0.866 mm below,
whose exact volume is `h/3·(A_top + A_bot + √(A_top·A_bot))` = **1326.507 mm³**. That is < 2053.5 by construction,
for any implementation that follows spec 3.4.

Measured RED output: `1326.5075683594 > 2053.5` — i.e. the implementation matches the independent analytic value
to **6.8 × 10⁻⁷ relative**. I replaced the two bounds with `REQUIRE(c.volume < 40*40*off)` (inside the straight
prism of the same height) and `REQUIRE_THAT(c.volume, WithinRel(frustum, 1e-4))`, with the derivation in a
comment. This is a stronger assertion than the brief's, not a weaker one.

### 2. Halving is applied once per vertex per round, not once per incident facet

The brief's `fold_guard` and its inline self-intersection fallback both write
`depth[v] = max(h, depth[v] * 0.5f)` inside a per-facet loop, so a vertex shared by *n* group facets is halved
*n* times in a single round — dividing by 2^valence and dropping straight to the layer-height floor immediately,
instead of the spec's "halve … and repeat (≤ 8 rounds)". I deduplicate the affected vertices and halve each once;
`halve()` also returns false when nothing moved, which terminates the guard loop early instead of spinning.
This is a fidelity gain (the shell keeps the deepest valid depth rather than collapsing to one layer) and is
consistent with the chameleon cost-fidelity ruling.

### 3. `ShellBuilder::params` removed

The brief gives `ShellBuilder` a `const ColorSplitParams &params` member, but nothing in Task 3 reads it (the
cap depth is passed as an argument, and `depth_override_mm` is consumed in `build_color_shells`). I dropped the
member as dead weight; Task 5 re-adds it when `flat_cap`/`crease_step` land. `ColorSplitParams` itself is
unchanged and still used.

## TDD evidence

### RED 1 — the follow-up tests (before the two `color_split_depths` guards)

Command: build wrapper, then
`C:\Dev\SnapmakerOrcaNext\build\tests\libslic3r\Release\libslic3r_tests.exe "[colorsplit]"`

```
Testing colorsplit: depths refuse a config that cannot produce a depth
-------------------------------------------------------------------------------
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(198): FAILED:
  REQUIRE_THROWS_AS( color_split_depths(cfg, {}), ColorSplitError )
because no exception was thrown where one was expected:
...
test cases:  7 |  6 passed | 1 failed
assertions: 79 | 78 passed | 1 failed
```

Expected: `color_split_depths` returned D = ws = 0 silently for an empty filament list. (The empty-nozzle
assertion on the next line was not reached — `REQUIRE_THROWS_AS` aborts the test case.)

### RED 2 — the shell tests (before the implementation)

Command: `cmd //c "<scratchpad>\build_next_wt_tests.bat"`

```
test_color_split.cpp(274,8):  error C4430: missing type specifier - int assumed   [ColorSplitParams]
test_color_split.cpp(280,19): error C3861: 'build_color_shells': identifier not found
test_color_split.cpp(280,63): error C3861: 'no_cap_no_step': identifier not found
test_color_split.cpp(282,5):  error C2065: 'ShellCheck': undeclared identifier
test_color_split.cpp(282,20): error C3861: 'check_shell': identifier not found
```

Expected: none of `ColorSplitParams`, `ColorShell`, `ShellCheck`, `check_shell`, `build_color_shells` existed yet.

### RED 3 — the brief's block-top volume bound, after the implementation

```
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp(288): FAILED:
  REQUIRE( c.volume > 37. * 37. * 1.5 )
with expansion:
  1326.5075683594 > 2053.5
...
test cases:  12 |  11 passed | 1 failed
```

Expected value cross-checked analytically (see Deviation 1): exact frustum = 1326.5066714589, measured
1326.5075683594, relative difference 6.8e-7 (its_volume accumulates in float32). The other 11 cases already
passed, so only the bound was wrong.

### GREEN — final build

```
> libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions
All tests passed (110 assertions in 12 test cases)

> libslic3r_tests.exe "[paintdepth]" --order rand --warn NoAssertions
All tests passed (1568 assertions in 94 test cases)
```

Build was clean (no warnings from `ColorSplit.cpp` or `test_color_split.cpp`). The 94 `[paintdepth]` cases stay
green.

## Measured `check_shell` cost per fixture

Measured with a temporary hidden `[.][colorsplitdiag]` test case (removed before the final build and not
committed). Each number is one full `check_shell` call — `TriangleMesh` construction + `its_num_open_edges` +
`its_volume` + CGAL conversion + `does_self_intersect`:

| fixture | shell triangles | check_shell |
|---|---|---|
| painted cube top | 12 | 0.0125 ms |
| fully painted sphere (r = 1, 10° facets) | 2448 | 4.5058 ms |
| thin plate, one face | 12 | 0.0094 ms |
| L bracket, floor + riser | 20 | 0.0222 ms |

So roughly **1.8 µs per triangle** on this machine. The brief's "can take seconds per shell" does not hold at
these sizes; a 100 k-triangle patch would extrapolate to ~0.2 s, still cheap relative to the Manifold split. The
retry loop can multiply this by up to 7 for a pathological component.

## Concerns

### C1 — a GENUINE pinch vertex still fails the split (blocking for spec 3.7 as written)

**The brief's pinch test does not test a pinch.** Cells (1,1) and (2,2) of the grid box are not edge-connected,
so `connected_components` yields two separate one-cell components; inside each, every boundary vertex carries
exactly two boundary edges. The wedge-duplication code is never entered.

I built a fixture that does produce a pinch: paint the cell path (1,1)-(1,0)-(2,0)-(3,0)-(3,1)-(3,2)-(2,2) on
`make_grid_box(40, 40, 10, 4, 4)`. That is one edge-connected component which touches itself at grid vertex
(2,2) — four boundary edges there, two wedges. Diagnostic result:

```
pinch: threw Could not build a valid shell for filament 2 (self-intersecting surface).
```

Cause (confirmed by construction, not guessed): the two wedges' duplicated copies of the pinch vertex sit at the
*same* coordinates top and bottom, so wedge A's side wall in the plane x = 20 and wedge B's side wall in the same
plane are coplanar and meet along the full segment (20, 20, 8.5)–(20, 20, 10). They share no vertex index (that
is the point of the duplication), so CGAL reports them as intersecting faces. Halving the depth cannot help — the
coincidence is scale-invariant — so all six retries fail and the whole split is refused.

Spec 3.7's "duplicate per wedge so every edge has exactly two faces" fixes *topology* but leaves the two wedges
*geometrically* coincident along the pinch line. Resolving it is a design decision I did not want to make alone.
Options, for the controller: (a) split the component at pinch vertices and emit one shell per wedge (Manifold
takes several closed shells per filament anyway, §3.8); (b) nudge each wedge's duplicated pair inward along that
wedge's mean inward tangent by a sub-resolution ε; (c) exempt the pinch line from the self-intersection verdict.
I did **not** add a test pinning the current refusal, since that would cement a defect as expected behaviour.

### C2 — the layer-height floor can raise a depth above mid-thickness

Both `fold_guard` and the retry use `max(layer_height, d/2)`, per spec 3.4 ("floor d = h"). Where the part is
thinner than 2h + 2δ (≈ 0.404 mm at h = 0.2) the vertex's honest depth is already below h, and the floor *raises*
it — breaking the invariant in Ruling 2 that the δ margin keeps shells off the mid-surface. No fixture in this
task reaches it, and it only bites on sub-half-millimetre features, but it is a real hole in the "never crosses
the mid-surface by construction" guarantee. The obvious fix is `max(min(d_initial(v), h), d/2)`; I left the spec's
wording in place rather than change behaviour un-briefed.

### C3 — `compute_vertex_depths` can return a negative depth

For t(v) < 0.004 mm, `t/2 − 0.002` is negative and the "bottom" vertex lands *outside* the surface. That is Task
2 code, far below print resolution, and the fold guard would catch the resulting inverted bottom — noted only for
completeness.

### C4 — the sphere shell's thickness changed with Deviation 2

The 2448-triangle sphere is the one fixture where the fold guard actually fires (its per-vertex depths vary by a
few microns, which at a 0.002 mm bottom radius is enough to invert the bottom triangles — exactly the case spec
3.4 describes). The 2.018 mm³ shell volume in my diagnostic run was measured **before** the per-round halving fix,
when the guard collapsed straight to the 0.2 mm floor; with the fix it stops at a larger depth. The test's bounds
(`> 0`, `< 4/3·π`) cover both and pass, but I did not re-measure the exact post-fix volume (measuring it would
have meant editing source after the final build).

### C5 — file size

`ColorSplit.cpp` is now 341 lines (from 131). Still one responsibility per function and one file for the split
library, as the plan intends, but Task 5 adds depth groups and the crease step on top of `ShellBuilder::build`;
that is the point at which splitting the file is worth considering.

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.hpp` — `ColorSplitParams`, `ColorShell`, `ShellCheck`,
  `check_shell`, `build_color_shells`.
- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` — the two `color_split_depths` guards, plus
  `connected_components`, `ShellBuilder`, `check_shell`, `build_color_shells`.
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — geometry-correlated state assertion, the
  depth-refusal test, and the five shell test cases.

No build artefacts, no untracked worktree junk, no `.superpowers/` content was committed.

---

# Fix report — controller rulings 8 and 9

Commit `325ba1ae3a` `fix(color-split): nudge pinch wedges apart and cap the halving floor at the mid-thickness
clamp`. Both rulings are implemented; two of the rulings' stated expected values had to be corrected, with the
evidence below.

## Ruling 8 — pinch nudge (closes concern C1)

`ShellBuilder::build` now computes, for every (pinch vertex, wedge) pair, the wedge's inward tangent bisector:
the normalised sum of the unit inward tangents `n_f × (b − a)` of that wedge's two boundary edges at the vertex,
`n_f` being the facet's own normal so the tangent stays in the surface plane. Every copy of that vertex — top and
bottom alike — is offset by `PINCH_NUDGE_MM = 1e-3` along it. The block is skipped entirely when the group has no
pinch vertex, so nothing else changed.

On the pinned fixture the two wedges at the pinch move to opposite sides: wedge A's copies to
(19.99929, 19.99929) and wedge B's to (20.00071, 20.00071), separating the previously coincident side walls by
1.4 µm — some 370 float32 ULPs at that coordinate magnitude, so the separation survives the mesh's `float`
storage. CGAL is satisfied and the shell is accepted on the first attempt (no retry rounds).

**Correction to the ruling's volume expectation — the fixture moved from a 4×4 to a 6×6 grid.** My original
7-cell path on `make_grid_box(40, 40, 10, 4, 4)` runs along the box's top rim: 6 of its 16 boundary edges lie on
`y = 0` or `x = 40`. Rim vertices carry 45° bisector normals, so the shell is bevelled there — its cross-section
over the 10 mm next to a rim edge is 12.008 mm² instead of the prism's 15.0, i.e. 2.992 mm³ lost per mm of rim,
about 180 mm³ over 60 mm of rim. `7 × 100 × 1.5 = 1050` is therefore unreachable on that fixture by roughly 17%,
for any correct implementation. The path `(2,2)-(2,1)-(3,1)-(4,1)-(4,2)-(4,3)-(3,3)` on a 6×6 grid has the
identical topology — one edge-connected component, exactly one pinch at grid vertex (3,3), verified by checking
every interior grid vertex — but every painted cell stays off the rim, so all its vertices have vertical normals
and depth 1.5 and the shell is an exact prism. The ruling's own formula then holds:
`7 × (40/6)² × 1.5 = 466.67 mm³`, asserted `WithinRel(..., 0.02)` as instructed. The check is now meaningful
rather than loosened — the expected value is exact and the 2% band is slack, not fudge.

## Ruling 9 — halving floor capped at d0

`ShellBuilder` keeps a const reference `d0` to the untouched `compute_vertex_depths` output alongside its working
copy, and `halve()` uses `floor = min(layer_height, d0[v])`, i.e. `depth = max(min(h, d0), depth/2)`. No existing
fixture changes behaviour (everywhere else `d0 ≥ h`, so the floor is still `h`).

**Correction to the ruling's threshold: 0.14885 mm of vertical drop / d0 = 0.25781 mm, not 0.148.** On
`make_cube(40, 40, 0.3)` the painted top face has only the four cube corners as vertices, and their
angle-weighted normals are the `(±1,±1,1)/√3` bisectors — so the `−n` ray crosses `0.3·√3 = 0.51962` mm of plate,
giving `d0 = t/2 − 0.002 = 0.25781` mm *along the normal*, which is `0.14885` mm of vertical drop and a bottom at
z = 0.15115. 0.148 is the vertical-normal figure (`0.3/2 − 0.002`), which a plain `make_cube` never samples —
the same distinction the existing Task-2 per-vertex depth test already documents for the 1.2 mm plate (1.03723
at a corner vs 0.598 at a grid-interior vertex). The test asserts both halves of the invariant per shell vertex:
`v.z() > 0.15` (never past mid-thickness) and `0.3 − v.z() ≤ d0/√3 + 1e-4` (never deeper than the vertex's own
clamp).

**Honest limitation: this fixture pins the invariant but does not exercise the new floor.** With `d0 = 0.25781`
above `h = 0.2` the floor is never reached, and on a flat plate the fold guard never fires at all (the offset
bottom is a cleanly shrunk copy). The test passed both before and after the change — see the RED run below. To
actually reach the floor you need the fold guard to fire *at a vertex whose d0 < h*, i.e. a tightly convex
feature under ~0.4 mm thick along its normal. In every such fixture I constructed (a 0.15 mm-radius sphere; the
r = 1 sphere with h = 1.2) the correct outcome is a **refusal**, not a valid shell: the bottom has collapsed to
the δ ball and self-intersects, `halve()` correctly reports "nothing moved", and `build_color_shells` throws.
That is a strict improvement — before Ruling 9 the same input silently produced a shell whose bottom was
*reflected through* the mid-surface (for the r = 1, h = 1.2 case the inverted inner sphere even yielded a
plausible-looking positive volume) — but it cannot be pinned with the ruling's "closed and not self-intersecting"
criterion. If the controller wants that path covered, the natural test is
`REQUIRE_THROWS_AS(build_color_shells(tiny_sphere, depths_for_test(1.5, 1.2), …), ColorSplitError)`; I did not add
it un-ruled.

## TDD evidence

RED (tests added, implementation not yet changed):

```
> libslic3r_tests.exe "[colorsplit]"
Testing colorsplit: a self-touching patch builds one valid shell across its pinch vertex
test_color_split.cpp(365): FAILED:
  {Unknown expression after the reported line}
due to unexpected exception with message:
  Could not build a valid shell for filament 2 (self-intersecting surface).

Testing colorsplit: a shell on a 0.3mm plate stops short of mid-thickness
Passed in 4.9e-05 [seconds]          <-- already green: see the limitation above

test cases:  14 |  13 passed | 1 failed
assertions: 131 | 130 passed | 1 failed
```

GREEN (after the nudge and the capped floor):

```
> libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions
All tests passed (134 assertions in 14 test cases)

> libslic3r_tests.exe "[paintdepth]" --order rand --warn NoAssertions
All tests passed (1568 assertions in 94 test cases)
```

The pinch case costs 0.32 ms end to end (build + two `check_shell` calls on a 46-triangle shell), so the nudge
adds no measurable cost. Build was clean — no new warnings.

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` — `PINCH_NUDGE_MM`, the per-wedge nudge in
  `ShellBuilder::build`, the `d0` member, and the capped floor in `ShellBuilder::halve`.
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — the self-touching-patch test and the
  0.3 mm plate mid-thickness test.

## Remaining concerns

- The Ruling 9 floor is implemented and invariant-pinned but not behaviourally covered (above).
- The nudge is a fixed 1 µm. On a patch whose triangles at a pinch are themselves only a few microns across the
  nudge could invert them; a proportional cap (e.g. min(1e-3, 0.1 × shortest incident edge)) would be safer, but
  that is a ruling I did not want to make unilaterally.
- Concerns C3 (a negative depth for t < 0.004 mm) and C5 (`ColorSplit.cpp` is now 388 lines) from the original
  report stand unchanged.

---

# Fix report — controller rulings 10 and 11

Commit `9026efb990` `fix(color-split): skip un-shellable components with a warning; scale the pinch nudge to the
local edge`. Both rulings applied as stated; no corrections were needed this round.

## Ruling 10 — skip and warn instead of refusing (closes the Ruling 9 coverage gap)

`build_color_shells` gained the optional trailing `std::vector<std::string> *warnings = nullptr`. The
`throw ColorSplitError("Could not build a valid shell …")` is gone: a component whose shell is still not closed
or still self-intersects after the six halving retries is now dropped, and — when the sink is supplied — one note
is recorded:

```
Filament 2: a painted feature about 0.52 mm across is too small to split and stays in the body colour.
```

The size is the diagonal of the *painted component's* bounding box (helper `too_small_warning`), not the failed
shell's — that is the feature the user can point at on screen. The progress tick still fires for a skipped
component, so progress stays monotonic. `ColorSplitError` remains for genuinely invalid input: a non-watertight
mesh, inconsistent paint data, and a config that cannot produce a depth.

This also closes the gap I flagged last round: the halving floor of Ruling 9 now has behavioural coverage. The
0.15 mm-radius ball is exactly the case where `d0 = 0.148 mm` sits *below* the layer height, so `min(h, d0)`
pins the depth at 0.148 and `halve()` correctly reports "nothing moved"; before Ruling 9 the flat layer floor
would instead have lifted the depth to 0.2 and reflected the bottom through the mid-surface. The new test proves
the whole chain end to end — floor, retry exhaustion, skip, warning — and it is the fixture I proposed and the
controller ruled on.

## Ruling 11 — proportional nudge cap

While accumulating each wedge's inward tangents, `build()` now also records the shortest boundary edge incident
to each pinch vertex, and the per-vertex offset is `min(1e-3, 0.1 × shortest_edge)`. On the pinch fixture the
cells are 6.67 mm across, so the cap does not bind and the nudge stays at 1e-3 mm — that test is unchanged and
still green, as the ruling anticipated.

## TDD evidence

RED (test added, five-argument call not yet supported):

```
> cmd /c build_next_wt_tests.bat
test_color_split.cpp(432,5): error C2660: 'Slic3r::build_color_shells': function does not take 5 arguments
```

GREEN:

```
> libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions
Testing colorsplit: a feature too small to carry a shell is skipped with a warning
Passed in 0.013508 [seconds]
All tests passed (139 assertions in 15 test cases)

> libslic3r_tests.exe "[paintdepth]" --order rand --warn NoAssertions
All tests passed (1568 assertions in 94 test cases)

> libslic3r_tests.exe "[colorsplit]"   (pinch fixture, unchanged by the Ruling 11 cap)
Testing colorsplit: a self-touching patch builds one valid shell across its pinch vertex
Passed in 0.000473 [seconds]
```

The new fixture costs 13.5 ms: a 1224-triangle ball, so the retry loop runs seven `check_shell` calls on a
2448-triangle shell before giving up — consistent with the 4.5 ms/2448-triangle figure measured earlier, and the
worst case the retry loop can produce. Build clean, no new warnings.

The test pins the message contract by equality, building the expected string from the source mesh's own bounding
box rather than hard-coding 0.52, so a change to either the wording or the measured size is caught.

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.hpp` — the `warnings` parameter.
- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` — `too_small_warning`, the skip-instead-of-throw
  branch, and the per-vertex nudge cap.
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — the too-small-feature test (plus `<sstream>`
  and `<iomanip>`).

## Remaining concerns

- Nothing new. `ColorSplit.cpp` is now 419 lines (concern C5: Task 5 adds depth groups and the crease step on
  top of `ShellBuilder::build`, which is the point to reconsider splitting the file). Concern C3 (a negative
  depth for a part under 0.004 mm thick) stands; note that Ruling 10 now makes such a component degrade to a
  warning rather than anything worse.

---

# Fix report — review round 1

Commit `1bdd9a1ec4` `fix(color-split): keep the shell progress ticks inside the 0..100 contract`. All four
findings applied; nothing else touched.

## Important — progress overflowed the 0..100 contract

`build_color_shells` divided the work done by `p.states.size() * 4`, a guess at the component count, so the
eleventh component of any state reported above 100. Every component is now materialised into a single
`std::vector<std::pair<int, std::vector<int>>>` of (state, facets) before the build loop — the components were
already being materialised per state, so this only moves the work — and the tick is
`10 + 40 * done / groups.size()`, bounded by construction to the 10..50 band this stage owns.

New test `colorsplit: progress stays inside the 0..100 contract with many components`: a 6×6 grid box with an
18-cell checkerboard, so every painted cell touches its neighbours only diagonally and is its own component.
It records every reported percentage and asserts all of them are within 0..100, that there is exactly one tick
per component, and that the last one is ≤ 50.

## Minors

- **`halve_depth` now returns whether anything moved**, and the validity-retry loop does `if (!sb.halve_depth(comp)) break;`.
  A component already at its floor no longer rebuilds and re-checks six identical times. Measurable: the
  too-small-feature test (the only fixture that exhausts the retries) went from **13.5 ms to 2.8 ms** — seven
  `check_shell` calls on a 2448-triangle shell down to one.
- **`wedge_of.at({v, f})`** replaces `operator[]` in the `wedge` lambda, so a missed wedge assignment throws
  instead of silently returning wedge 0 and welding the two wedges back together.
- **Two-sided plate test** now pins the shells' `state` values to `{2, 3}` order-independently (sorted, so the
  build order stays an implementation detail).
- **New test `colorsplit: depth_override_mm replaces D and clears unlimited`**: the block-top fixture with
  `depths_for_test(infinity)` (so `unlimited` is set) and `params.depth_override_mm = 0.7`. Unlimited would take
  every corner to half the block's thickness; the override has to win over both the depth and the flag. Asserted
  through the shell's own geometry: max z = 20 and min z = 20 − 0.7/√3 = 19.59585.

## TDD evidence

RED (tests added, implementation unchanged):

```
> libslic3r_tests.exe "[colorsplit]"
Testing colorsplit: progress stays inside the 0..100 contract with many components
test_color_split.cpp(478): FAILED:
  REQUIRE( percent <= 100 )
with expansion:
  110 <= 100

test cases:  17 |  16 passed | 1 failed
assertions: 167 | 166 passed | 1 failed
```

110 is the eleventh of eighteen components under the old formula (`10 + 40·11/4`), exactly the overflow the
review predicted. The `depth_override_mm` test and the `{2, 3}` state assertion passed straight away — they
cover behaviour that already worked and had simply never been pinned.

GREEN:

```
> libslic3r_tests.exe "[colorsplit]" --order rand --warn NoAssertions
All tests passed (184 assertions in 17 test cases)

> libslic3r_tests.exe "[paintdepth]" --order rand --warn NoAssertions
All tests passed (1568 assertions in 94 test cases)
```

Build clean, no new warnings.

## Files changed

- `C:\Dev\SnapmakerOrcaNext\src\libslic3r\ColorSplit.cpp` — component list built up front and the corrected
  progress fraction; `halve_depth` returns bool and the retry loop breaks on it; `wedge_of.at()`.
- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — the progress test, the `depth_override_mm`
  test, the `{2, 3}` state assertion (plus `<algorithm>`).
