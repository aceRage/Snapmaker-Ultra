# Task 6 report — Flat cap depth groups and crease step

Commit: `a311ebab9a` — `feat(color-split): flat-cap depth groups and crease wall-stack step`
(base `57f815218e`; one commit, source + tests together as the brief's step 5 asks).

## What I implemented

### Spec 3.5 — flat-cap depth groups (`src/libslic3r/ColorSplitShell.cpp`)

* `flat_core_survives(p, comp, ws)` — the core gate: the component's XY projection (its facets as
  Clipper polygons, `union_ex`) must survive `offset_ex(-1.5*ws)`.
* `classify_depth_groups(p, nbrs, can_cross, patch, depths, params, D)` — runs INSIDE one smooth patch
  (spec 3.1a stays the outer decomposition) and returns `(facets, cap depth)` pairs. Flat means
  `|n_z| > cos(atan(h/(3 ws)))`; up-facing components take `cap_top`, down-facing `cap_bottom`; what is left
  of the patch forms the uncapped groups at `d(v)`. Gates, transliterated from the 2D ones
  (`MultiMaterialSegmentation.cpp:1874-1877` and `:2228`): `params.flat_cap`, `D` finite (pdmUnlimited feeds
  the 2D path a band of 0, so unlimited mode caps nothing), `ws > 0` (the 2D `normal_shell` has the same
  guard, and it also keeps `tan_flat` out of a divide-by-zero), `D >= ws`, and `cap < D` per direction.
* `build_color_shells` iterates the depth groups instead of raw patches, pushes a state's CAPPED groups
  before its uncapped ones (spec 3.8) and sets `ColorShell::capped = cap > 0`.

### Spec 3.6 — the crease step (ring vertices)

* `GroupTopology::step` — a new per-`(vertex, wedge)` map of `CreaseStep{n_p, t_miter, case_a}` filled in the
  same pass that fills `wall`. A boundary vertex is classified once (it carries one ring and one bottom
  copy): same-state or concave → wall (unchanged, always on); otherwise crease (`n_P·n_Q < cos 15°`) →
  case A when `|n_P·z| > |n_Q·z|`, else case B; otherwise plain.
* `ShellBuilder::side_offsets(v, wedge, cap)` replaces `bottom_offset` and returns `{ring, bottom, stepped}`:
  * wall → `ring == bottom == -min(d, half_thickness_along(n_P)) * n_P` (no step, exactly as before);
  * plain → `ring = -min(d,h)*n(v)`, `bottom = -d*n(v)`;
  * case A → `ring = ws*t_miter - min(d,h)*n_P`, `bottom = ring - (d-min(d,h))*n_P`;
  * case B → `ring = -min(d,ws)*n_P`, `bottom = ring - (d-min(d,ws))*n(v)`;
  * collapse: `|bottom - ring| <= h` ⇒ `bottom := ring`, one strip only.
  Every first segment is clamped to `d` so a ring can never sink past its own bottom (the brief's formulas
  clamp case B only; without the same clamp on the plain/case-A first segment a vertex with `d < h` would
  invert its strip and push the ring past the mid-surface).
* **Mitred inward tangent** (case A): `t_in` is now accumulated as the SUM of the unit inward tangents plus an
  edge count, and `t_miter = k*S/|S|²` (clamped to 4 wall stacks, Clipper's miter-limit idea). That is what
  makes a cube's top corner step `(0.87, 0.87, 0)` — i.e. 0.87 clear of BOTH side faces, which is what the
  brief pins — while a straight boundary vertex still steps its plain 0.87. A normalised mean would have
  given 0.615 at the corner.
* `build()` now allocates a top, ring and bottom copy per `(vertex, wedge)` (the ring copy only when it is a
  real step) and emits the two strips `(b,a,a₁,b₁)` and `(b₁,a₁,a′,b′)` as four triangles, dropping any
  triangle whose three ids are not distinct. That is what makes an edge between a stepped and an unstepped
  vertex close correctly (3 triangles), and it reproduces the old single strip byte-for-byte when neither end
  steps. Interior (non-boundary) vertices never get a ring copy.
* `fold_guard` judges the bottoms through the same `side_offsets`, so the guard still sees the shell that is
  actually built.

### Five folded Task 5 review follow-ups

1. **Degeneracy guard** (was `ColorSplitShell.cpp:228-230`): only `n_P` is required now; `n_Q`/`t_in` gate the
   concave test (and the convex cases) alone, so an outside neighbourhood whose normals cancel can no longer
   cost a patch its same-state wall.
2. **`halve()` "changed"**: it now snapshots the effective bottom offset of every `(vertex, wedge)` of the
   group, halves, and reports whether any of them MOVED. Self-review found that the naive version of this
   change breaks the fold guard: with a cap of 0.6 and `d = 1.5`, the first halving (0.75) is a geometric
   no-op, and returning "unchanged" would abandon a foldable group that still had room. So `halve` keeps
   halving until the shell moves or every vertex sits on its floor — one EFFECTIVE halving per round.
3. **Stale comments**: `ColorSplit.hpp` (`ColorSplitParams`) and the `build_color_shells` / `GroupTopology`
   comments rewritten to the built behaviour.
4. **Per-boundary-vertex test** (was the one-extremum disjunction): the cube top+side test now pins the
   COMPLETE vertex set of both shells (`require_vertices_are`).
5. **`check_shell` once per shell** in the boss test.

## Tests

TDD, RED first (`build/tests/libslic3r/Release/libslic3r_tests.exe "[colorsplit]"` after adding the tests
only):

```
colorsplit: flat top is capped at the solid shell depth, slopes are not
  test_color_split.cpp(765): FAILED: REQUIRE( shells[0].capped ) with expansion: false
colorsplit: painted top steps one wall stack in below the surface layer at side faces
  test_color_split.cpp(830): FAILED: REQUIRE( rings == 4 ) with expansion: 0 == 4
colorsplit: painted side face keeps its full wall stack up to the top edge
  test_color_split.cpp(847): FAILED: REQUIRE( found_ring ) with expansion: false
test cases:  31 |  28 passed | 3 failed
assertions: 291 | 288 passed | 3 failed
```

GREEN after the implementation, and after the two extra cases added during self-review:

```
[colorsplit]        All tests passed (377 assertions in 32 test cases)
[colorsplit_spike]  All tests passed (10 assertions in 2 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

New/changed cases:

* `flat top is capped at the solid shell depth, slopes are not` — cap 0.6 with the step off (zmin
  = 20 − 0.6/√3, the bisector) AND with the step on (zmin = 19.4, the brief's pin); `cap_top = D` ⇒ not
  capped, zmin 18.5; 3° wedge not flat, 1° wedge flat.
* `narrow flat strip (core < 3 wall stacks) is not capped` — 40×2 top, uncapped.
* `painted top steps one wall stack in below the surface layer at side faces` — 4 ring vertices at
  z = 19.8 and 4 bottoms at 18.5, all inset 0.87 in x AND y (the mitre), shell closed.
* `painted side face keeps its full wall stack up to the top edge` — ring at x = 39.13, z = 20; plus the
  `d <= ws` collapse (D = 0.5: every vertex at x ∈ {40, 39.5}, no taper at all).
* `a capped group and the uncapped group beside it meet along a straight wall` (added in self-review) —
  cube top (capped at 0.8) + same-filament +X side (uncapped): pins `shells[0].capped && !shells[1].capped`
  (spec 3.8 order) and the complete 10-vertex set of BOTH shells, i.e. all four spec 3.6 cases at once:
  same-state wall straight down/in, case A mitre + ring, case B step + bisector taper.
* `a painted cube top and side are two smooth patches with straight walls` — rewritten to the complete
  vertex set of both shells (follow-up 4).
* Spike `S3 stage breakdown` — `shells.size()` 1 → 2 with the reason (below).

## Deviations from the brief, with evidence

1. **The brief's first test cannot hold `crease_step = false` AND zmin = 19.4.** A plain cube's top face has
   only its four corner vertices, whose angle-weighted normal is the (±1,±1,1)/√3 bisector (pinned by the
   existing NormalUtils test), so with the step off a 0.6 mm cap moves the bottom 0.6/√3 = 0.346 in z:
   zmin = 19.654. 19.4 is exactly what spec 3.6 case A produces (one layer down, then 0.6 − 0.1 straight down
   n_P). Rather than loosen either, the test asserts BOTH: 19.654 with the step off and the brief's 19.4 with
   it on — each still pins the CAP (uncapped is 18.5 either way, also asserted). No spec rule was invented;
   in particular I did NOT make capped groups offset along n_P, which would have contradicted §3.6's plain
   boundary rule.
2. **Spike sphere: 2 shells, not 1.** With `depths_for_test(1.5)` (h = 0.2, ws = 0.87) "flat" is 4.38° off
   horizontal, so the painted cap's crown is a disc of radius 20·sin(4.38°) = 1.53 mm, which survives the
   1.305 mm core offset and becomes a capped group. This is the dome-crown split the 2D rule makes as well
   (the comment at `MultiMaterialSegmentation.cpp:2228`: the crown is capped, the rim rolling into the flank
   is not). Expectation updated with that reasoning in the test.

## Self-review findings (fixed before committing)

* `halve()`'s new "did the shell move" semantics would have ended the fold-guard loop on a capped group whose
  first halving is absorbed by the cap → fixed by halving until it takes effect (see follow-up 2 above).
* The brief's plain/case-A first segment is `h` unconditionally; on a vertex with `d < h` that puts the ring
  BELOW the bottom (inverted strip) and past the mid-surface → clamped to `min(d, h)`, matching what the
  brief already does for case B.
* Mixed classifications at one vertex (case A edge + plain edge, same-state + ordinary) are decided once, on
  the mean outside normal, with the same-state rule winning (Ruling 19) — now stated in the comment.
* Edge cases covered by tests instead of reasoning alone: capped group next to an uncapped same-state group,
  `d <= ws` for case B, `cap >= D`.

## Concerns

* **Spike timings moved** (measurement only, no assertion): one colour 0.889 → 1.01 s, three colours
  3.867 → 6.32 s on the 99k-triangle sphere. Cause: one extra capped crown shell per painted cap (an extra
  Manifold `Split`, the stage that was already 76 % of the time) plus the ring row on every boundary strip.
  Task 9 re-records the spike numbers; nothing here asserts a duration.
* **Spec-mandated shortening**: at a plain boundary with `h < d <= 2h` the collapse rule ("a′ collapses onto
  a₁") leaves the shell `h` deep instead of `d`. That is §3.6 as written; it only bites on features whose
  paint depth is under two layers.
* **Case A's ring is not re-probed for half-thickness along n_P.** `d` is clamped along n(v) (§3.4) and the
  ring's first segment is clamped to `d`, but on a plate thinner than about twice the step depth a case-A rim
  could in principle reach past the mid-surface; the fold guard and the CGAL fallback still catch an invalid
  shell. No fixture reaches it, so I did not add another AABB probe (the concave/same-state walls do have
  one). Flagged rather than built.
* **Coverage gap**: pinch vertices are only exercised with the crease step OFF (the existing fixtures use
  `no_cap_no_step`). Structurally the ring is just another offset from the same nudged top copy per
  `(vertex, wedge)`, so it inherits the wedge handling, but it is not pinned.
* `ColorSplitShell.cpp` is now 616 lines (was 421). Still one subject — grouping, topology, builder — so I
  did not restructure it.

---

## Fix round 1 (coordinator rulings, pre-review) — commit `89b7724089`

`fix(color-split): clamp the crease step's descent to the mid-thickness it travels (Ruling 20)`

**Ruling 20 applied.** `CreaseStep` gained a `half_thickness` measured along n_P; case A now descends
`min(d, half_thickness)` instead of `d`. The probe starts at the RING's position ON the surface
(`vertex + ws * t_miter`, tangential, so it is the boundary vertex moved one wall stack along the patch)
and runs along −n_P — i.e. where the wall actually descends, not at the rim. It deliberately does NOT start
one layer down at a₁ itself: `half_thickness_along` measures a thickness from a SURFACE point, so starting
inside the part would measure only the material below the ring and halve that (z = 0.502 on the pinned
plate) — which crosses the mid-surface just as badly as no clamp at all. Case B's second segment travels
along n(v), whose depth already is that probe, and is untouched.

`group_topology` now takes `const ColorSplitDepths &` and `const ColorSplitParams &`: it needs the wall
stack to place the probe, and it now skips the convex-crease classification (and its ray casts) entirely
when `crease_step` is off.

**RED evidence** (the Ruling 20 clamp temporarily disabled — `d_p = d` — everything else in place):

```
colorsplit: the crease step's own descent stops at mid-thickness on a thin plate
  test_color_split.cpp(866): FAILED: REQUIRE_THAT( v.z(), WithinAbs(0.602f, 1e-3f) )
  with expansion: 0.16277f is within 0.001 of 0.6019999981
test cases: 1 | 1 failed
```

0.16277 is exactly the predicted overshoot: ring z = 1.0, minus (d = 1.037 − h = 0.2). The clamp was then
restored and the tree rebuilt.

**New tests**

* `the crease step's own descent stops at mid-thickness on a thin plate` — `make_cube(40, 40, 1.2)` painted
  on `CUBE_TOP`, `depths_for_test(1.5, 0.2, 0.87)`, crease step on, run with `flat_cap` both off and on
  (the 0.8 cap is the looser of the two clamps here, so both give the same answer): 4 tops at z = 1.2,
  4 rings at 1.0, 4 bottoms all at z = 0.602 ± 1e-3, shell closed and self-intersection-free.
* The 7-cell self-touching pinch fixture is now also built with `crease_step = true` (flat cap off so the
  depth stays at D): one shell, closed, no self-intersections, same volume — both wedges of the pinch vertex
  carry ring copies and the nudge still keeps them apart.

**Results after the fix**: `[colorsplit]` 33 cases / 401 assertions, `[colorsplit_spike]` 2 / 10,
`[paintdepth]` 94 / 1568 — all green.

**Concerns after this round**: the three "Case A ring not re-probed" and pinch-coverage items from the
original list are now closed. What remains open is only what the coordinator accepted as is (the dual
step-off/step-on cap expectations, the spike sphere's two shells, the S3 timing shift for Task 10, and the
616-line file — now 625).

---

## Fix round 2 (task review, Rulings 21/22 + minors) — commit `bed40caa7c`

`fix(color-split): bound the crease step by the wall it travels and gate its inset (Rulings 21, 22)`

### Important 1 — Ruling 21 (case B crosses a thin wall's mid-plane)

`CreaseStep::half_thickness` is now populated for case B as well, and case B spends it as ONE budget:
`d_p = min(d, half_thickness)`, `first = min(d_p, ws)`, `bottom = ring − (d_p − first)·n(v)`. Clamping only
the step (as the finding's literal wording would) is not enough — on the pinned wall the step would stop at
y = 0.502 and the taper would then carry the bottom to y = 0.291, still past the mid-plane. With one budget
the bottom's depth along n_P is `first + (d_p − first)·(n(v)·n_P) ≤ d_p ≤ half_thickness`, which is provable
rather than incidental, and it makes the ruling's own pin pass.

### Important 2 — Ruling 22 (case A inverts a group narrower than 2 ws)

`flat_core_survives` was generalised into `projection_survives(p, comp, axis, delta)` — the group's triangles
projected onto the plane ⊥ `axis` (an orthonormal frame from `axis.unitOrthogonal()`), unioned, offset by
−delta. Spec 3.5's core gate is now that helper with `axis = +Z, delta = 1.5 ws`; Ruling 22's gate is the
same helper with `axis = group_mean_normal(...), delta = ws`. When it comes back empty the group's case A
vertices get NO step entry, so `side_offsets` uses spec 3.6's plain bisector rule for them. The gate is
evaluated lazily — at most once per group, and only when a case A vertex asks — because it costs a Clipper
union of the whole group.

### Minor — probe origin (and a defect it uncovered)

The Ruling 20/21 probe origin is now `vertex + ws·t_miter` **snapped to the nearest surface point**
(`AABBMesh::squared_distance`), shared by both cases. Two reasons, the second measured here:

* the tangential inset point floats ~ws²/2R above a convex patch, where the probe would measure the air gap;
* a probe launched from the boundary VERTEX (the finding's suggestion for case B, since the vertex is on the
  surface) grazes the other faces that vertex sits on. On the 1 mm wall the ray from the top corner slid
  along both the top face and the end face, returned NO hit, and `half_thickness_along` therefore answered
  +inf — the clamp silently did nothing and the test still failed at y = 0.136. The inset origin fixed it.

### Other minors

* `halve()` returns false immediately when the caller asked for nothing, before building `corners`.
* `classify_depth_groups`: `if (cap <= 0. || cap >= D) continue;`.
* `ColorSplit.hpp` records that `crease_step` also adds the ring at plain boundaries; the same-state bullet
  in `group_topology` records that the rule also fires between a capped group and the rest of its patch.
* New test: a painted flat BOTTOM (`CUBE_BOTTOM`) is `capped` with zmax = 0.6 = `cap_bottom` — the
  down-facing half of spec 3.5 and the clockwise-projection branch of the core gate.

### Covering tests

New: `the wall-stack step never crosses a thin wall's mid-plane` (1 mm wall, +Y painted: every shell vertex
y ≥ 0.4979, closed, no self-intersections; then +Y/−Y painted with two filaments: 2 shells, both pieces
≥ 0.45 × the wall, body + pieces = the wall within 1e-4); `a painted top narrower than two wall stacks falls
back to the bisector` (40 × 1.5 × 20 top: one shell, no warning, closed, no self-intersections, volume > 0,
and all twelve vertices pinned on the bisector taper); `a painted flat bottom is capped at the bottom shell
depth`.

Also corrected: the cube facet table comment at the top of the test file claimed 6,7 = +Y and 10,11 = −Y.
`its_make_cube` (TriangleMesh.cpp:886-896) gives facets 6,7 = {1,7,6}, {1,6,2}, whose vertices all have
y = 0, so the Y pair is the other way round. No test had painted a Y face before this round; the first run of
the new wall test failed with a vertex at y = 0.0, which is what surfaced it.

### Command and output

`cmd /c build_next_wt_tests.bat` (target libslic3r_tests, build slot checked and free before each build), then
`build/tests/libslic3r/Release/libslic3r_tests.exe`:

```
[colorsplit]        All tests passed (453 assertions in 36 test cases)
[colorsplit_spike]  All tests passed (10 assertions in 2 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

Spike measurements unchanged by this round: S3 one colour 1.027 s, three colours 6.317 s, S1 boss piece
9.30484 mm³ (98.73 %).

### Observation for the final review (not fixed — out of this round's scope)

The concave / same-state WALL probe still starts at the boundary vertex itself
(`group_topology`, `topo.wall.emplace(...)`), so it has exactly the grazing defect the case B probe had: at a
mesh corner the ray can report no hit and the mid-thickness clamp becomes +inf. In today's fixtures that only
ever loosens a clamp that `min(d, ...)` was not going to bind anyway (the cube's same-state walls want
d = 1.5 against a 9.998 half-thickness either way), so no expectation moves — but on a thin painted wall with
a same-state crease it would let the wall cross the mid-surface, which is the very thing Rulings 20/21 close
for the convex cases. The one-line fix is to reuse the same inset+snapped origin there. Flagging rather than
folding it in, since the always-on wall rule was not in this round's scope.

### Conformance with spec rev 2.7 (`a2b37da99f`, landed while this round was building)

* §3.6 "Width guard (rev 2.7, Ruling 22)" — implemented exactly: the group's projection onto the plane
  perpendicular to its mean normal, offset inward by ws; empty ⇒ that group's Case A vertices use the plain
  bisector rule.
* §3.6 "Mid-thickness on every path (Rulings 20/21)" — the guarantee ("no wall ever passes the mid-thickness
  whatever the vertex's bisector normal says") holds, with two deviations from the parenthetical's letter,
  both forced by measurement and both documented in the code:
  1. **Case B's probe origin.** The spec says "from the vertex". Measured on the ruling's own 1 mm wall
     fixture, a probe from the vertex grazes the two other faces that corner sits on, reports no hit, and
     returns +inf — the clamp does nothing and the wall still lands at y = 0.136. This build probes from the
     same inset-and-snapped origin Case A uses, which answers 0.498 and makes the pin pass.
  2. **Case B's second segment.** The spec re-probes "Case B's first segment"; clamping only that leaves the
     taper to carry the bottom to y = 0.291 on the same fixture. The clamp is therefore applied to Case B's
     whole budget, which bounds both segments (`first + (d_p − first)·(n(v)·n_P) ≤ d_p`).
  If the controller wants the spec's parenthetical to match the build, those are the two lines to amend.

---

## Fix round 3 (addendum to Ruling 21: the always-on wall probe) — commit `ee435195ae`

`fix(color-split): probe the concave and same-state walls a wall stack in as well`

### What changed

The concave / same-state wall rule launched its mid-thickness probe from the boundary vertex, so it carried
exactly the grazing defect the convex cases had just shed. All four spec 3.6 rules now go through one lambda,
`probe_half_thickness(v, t_miter, dir)`: the origin is one wall stack into the patch along the mitred inward
tangent, snapped back onto the surface with `AABBMesh::squared_distance`. The mitre is consequently computed
for every boundary vertex (it is ray-free); only case A also STEPS along it. `CreaseWall::half_thickness` and
`CreaseStep::half_thickness` are now filled by the same call.

### RED evidence

With the wall branch's origin temporarily put back to the raw vertex (`t_miter = 0`, everything else in
place):

```
colorsplit: a same-state wall on a thin wall stops at the mid-plane too
  test_color_split.cpp(952): FAILED: REQUIRE( v.y() >= 0.5f - 0.002f - 1e-4f )
  with expansion: 0.13597f >= 0.4979f
test cases: 1 | 1 failed
```

0.13597 = 1 − 0.864, i.e. the bisector depth spent along n_P with no clamp at all: the probe from the shared
top edge grazes both the top face and the end face, finds nothing, and `half_thickness_along` returns +inf.
The origin was then restored and the tree rebuilt.

### Covering test

`a same-state wall on a thin wall stops at the mid-plane too` — `make_cube(40, 1.0, 20)` painted on +Y AND on
the top with ONE filament, which puts a same-state crease along their shared top edge (the side patch's own
bottom corners are case B, already covered). Two smooth patches; every vertex of the side patch's shell has
y ≥ 0.4979; shell closed and self-intersection-free.

### Measured consequence beyond the pin

The S1 boss piece improved from 9.30484 to **9.37566 mm³** of the 9.42478 exposed (98.73 % → **99.48 %**),
body 16000.0. The boss's side tube meets the block top at a concave crease and its own cap at a same-state
crease; both probes used to graze and return +inf, so the tube overran the axis and was reined back in by the
fold guard. It now stops at the mid-thickness by construction. No other expectation moved.

### Command and output

```
[colorsplit]        All tests passed (465 assertions in 37 test cases)
[colorsplit_spike]  All tests passed (10 assertions in 2 test cases)   S3 1.040 s / 6.354 s
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

With this round the "observation for the final review" from fix round 2 is closed: no spec 3.6 wall path
measures its mid-thickness from a grazing origin any more.
