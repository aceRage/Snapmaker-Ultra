# Task 5 report — Surface refinement pre-pass, concave creases, file split

Branch `feat/color-split`, worktree `C:\Dev\SnapmakerOrcaNext`. Two commits:

| SHA | Subject |
|---|---|
| `d687d1768a` | `refactor(color-split): split ColorSplit.cpp into shell/partition units` |
| `a1aa0997d6` | `feat(color-split): refinement pre-pass and concave-crease walls (spike follow-up)` |

**Status: DONE_WITH_CONCERNS.** Everything in the brief is implemented and all three suites are green, but the
boss test's `piece >= 0.9 x exposed` expectation is **not met** (measured 84.8 %). That is reported as
evidence below rather than loosened into a weaker threshold — the controller rules on it.

---

## 1. Commit 1 — the pure file split (Ruling 15)

`ColorSplit.cpp` (640 lines, three unrelated concerns) became four files:

| File | Holds |
|---|---|
| `src/libslic3r/ColorSplit.cpp` | `extract_color_patches`, `color_split_depths`, `color_split_normals`, `compute_vertex_depths`, `check_shell`, `split_volume_by_paint` |
| `src/libslic3r/ColorSplitShell.cpp` | `PINCH_NUDGE_MM`, `too_small_warning`, `connected_components`, `ShellBuilder`, `build_color_shells` |
| `src/libslic3r/ColorSplitPartition.cpp` | `to_manifold64`, `from_meshgl64`, `from_manifold`, `require_ok`, `faces_by_original_id`, `partition_by_shells` — the only unit that includes `manifold/manifold.h` |
| `src/libslic3r/ColorSplitInternal.hpp` | `namespace Slic3r::ColorSplitDetail` — the declarations shared across units |

Registered in `src/libslic3r/CMakeLists.txt` next to `ColorSplit.cpp` (the main `lisbslic3r_sources` list).

Also dropped the unreachable "shell depth reduced" warning branch and its `depth_reduced_warning` helper, plus
the now-unused `halvings` counter (Ruling 15).

**Deviation from the brief's file list, deliberate:** the brief suggested `ColorSplitInternal.hpp` might carry
`connected_components`, `to_manifold64` and `from_manifold` ("if needed"). Each of those is used by exactly
one translation unit, so they stayed file-local in an anonymous namespace; declaring them in a shared header
would widen their linkage for nothing. In commit 1 the header therefore carried a single declaration
(`effective_depths`); the feature commit added two more (`half_thickness_along`, `vertex_depths`).

Verification: built, `"[colorsplit]"` green with the test file **unchanged** — 230 assertions in 25 cases,
exactly the pre-split count.

---

## 2. Commit 2 — the feature

### 2.1 Refinement pre-pass (Ruling 13, spec §3.1a)

New public API in `ColorSplit.hpp`:

```cpp
ColorPatches refine_color_patches(const ColorPatches &patches, double max_edge_mm);
double color_split_refine_length(const ColorSplitDepths &, const ColorSplitParams &, const BoundingBoxf3 &mesh_bbox);
```

* `color_split_refine_length` (ColorSplit.cpp) = `max(ws, min(D_eff, bbox.size().norm() / 20))`, with `D_eff`
  taken through `ColorSplitDetail::effective_depths` so the dialog's depth override sizes the refinement the
  same way it sizes the cut, and `+inf` for unlimited.
* `refine_color_patches` (ColorSplitPartition.cpp): a cheap O(n) edge-length scan first — if no edge exceeds
  `max_edge_mm` the patches come back untouched and no Manifold work happens at all. Otherwise
  `to_manifold64(surface, ctx, as_original = false)` → `RefineToLength` → `from_manifold`; closedness is
  re-checked (`ColorSplitError` if it ever broke); each refined facet's state comes from
  `AABBMesh(patches.surface).squared_distance(centroid, face, closest)` — the refined triangle lies inside its
  parent, so the nearest facet is the parent at distance zero. Same triangle count out ⇒ input returned.
* `to_manifold64` gained an `as_original` flag: refinement has no use for the provenance stamp, and the header
  documents nothing about what `AsOriginal` does to the triangulation it stamps over, so it opts out rather
  than relying on it being harmless.
* `split_volume_by_paint` computes the surface's bounding box and refines between `extract_color_patches` and
  `build_color_shells`; the progress contract becomes 0 → patches → **5** → refinement → 10 → shells (10–50) →
  partition (50–95) → 100.

**On the "verify RefineToLength is flat" instruction:** `manifold.h` files `Refine`/`RefineToLength`/
`RefineToTolerance` under a `@name Smoothing` group header and carries **no per-function doc comment** for any
of them, so the header cannot settle it. Flatness is pinned by test instead: the refined cylinder keeps the
coarse one's `its_volume` to 1e-5 relative, has zero open edges, and its new side vertices sit on the chords
with *exactly* radial angle-weighted normals (`|n.z| < 1e-3`) — none of which a curved refinement produces.
`RefineToLength` is also the only variant used.

### 2.2 Concave creases (Ruling 14, spec §3.6 third bullet)

Per boundary vertex **per wedge**, `group_topology` accumulates `n_P` (mean unit normal of the group's facets
there — every incident group facet, boundary edge or not), `n_Q` (mean unit normal of the outside facets
across its boundary edges) and `t_in` (mean unit inward tangent `n_f × (b − a)` of those edges). A vertex is a
concave crease when `n_P · n_Q < cos 15°` **and** `n_Q · t_in > 0`; its bottom copy then walks
`top − d · n_P` instead of `top − d · n(v)`. Always on, independent of `crease_step`.

The three vectors live in one `BoundaryInfo` struct keyed by `(vertex, wedge)`, exactly so Task 6 can add the
convex ring-vertex cases (spec §3.6 bullets 2 and 3) without re-deriving anything.

**One addition beyond the brief's wording, and why it is required:** the brief said only "its bottom copy is
`top − d·n_p`". Applied literally that breaks §3.4's invariant. On the boss, the base-ring vertices have
`d(v) = 1.5` because §3.4 measured the thickness along `n(v)` (the ~43° bisector, whose ray runs down through
10 mm of block); walking 1.5 mm *radially* from radius 1.0 crosses the axis and inverts the ring. §3.4's
clamp exists precisely to stop a bottom crossing the mid-surface, so for a concave-crease vertex it is
re-measured along the direction the wall actually travels: `d = min(d(v), half_thickness_along(n_P))`. The
probe (`ColorSplitDetail::half_thickness_along`) is the *same* ray query `compute_vertex_depths` already used,
factored out; `build_color_shells` builds one `AABBMesh` and passes it to both, so there is **no extra cost**
(the tree was already being built and thrown away inside `compute_vertex_depths`). The per-crease
half-thickness is measured once per group, in `group_topology`.

### 2.3 Structural change: `GroupTopology`

Everything about a group that the working depth cannot change — facet set, boundary counts, pinch wedges,
their Ruling-8 nudges and the concave creases — moved out of `ShellBuilder::build()` into a `GroupTopology`
built once per group by `group_topology(p, nbrs, aabb, group)`. `ShellBuilder` holds a reference to it, and
`build()` and `fold_guard()` now share one `bottom_offset(v, wedge, cap_depth)`, so **the fold guard judges
the shell that is actually built** instead of a bisector-only approximation of it. `fold_guard` takes
`cap_depth` for the same reason. This is the hoist Task 6 needs anyway; without it the guard and the builder
would disagree at every crease vertex.

---

## 3. TDD evidence

**RED** — the three brief tests written first, build of `test_color_split.cpp`:

```
test_color_split.cpp(284,22): error C3861: 'refine_color_patches': identifier not found
test_color_split.cpp(307,5):  error C3861: 'color_split_refine_length': identifier not found
test_color_split.cpp(309,5):  error C3861: 'color_split_refine_length': identifier not found
test_color_split.cpp(311,5):  error C3861: 'color_split_refine_length': identifier not found
```

(The build also flagged `error C2666: 'Catch::Matchers::WithinRel': overloaded functions have similar
conversions` on `WithinRel(its_volume(...), 1e-5)` — `its_volume` is `float` and the literal is `double`, so
Catch2's `float` and `double` matchers tie. Fixed by writing the epsilon as `1e-5f`; the assertion is
unchanged in substance.)

**RED (2)** — with the implementation in, the boss test failed on the volume threshold only:

```
test_color_split.cpp(704): FAILED:
  REQUIRE( volume_of(rb.pieces[0].second) >= 0.9 * exposed )
with expansion:
  7.9901809692 >= 8.4823001647
```

**GREEN** — final run on the committed tree:

| Suite | Result |
|---|---|
| `[colorsplit]` | All tests passed (1107 assertions in 28 test cases) |
| `[colorsplit_spike]` | All tests passed (8 assertions in 2 test cases) |
| `[paintdepth]` | All tests passed (1568 assertions in 94 test cases) |

---

## 4. Findings — why the boss reaches 84.8 %, not 90 %

Investigated with a temporary `[csdiag]` test case (added, measured, removed before committing). Numbers from
the boss fixture, `D = 1.5`, `h = 0.2`, `ws = 0.87`:

1. **Refinement does what §3.1a asked.** `L = 1.5 mm`, 158 → 6 928 triangles. The boss side gains interior
   rings at z = 11 and z = 12 whose angle-weighted normals are exactly radial (`n = (1, ~7e-7, ~-2e-8)`) with
   `d = 0.998` — the half-thickness clamp on a Ø2 mm cylinder. That is precisely the vertex the spike was
   missing.
2. **Ruling 14 fires where it should.** The base ring (z = 10, r = 1) is classified concave
   (`n_P` radial, `n_Q = +z`, `t_in = +z`), so its wall is a flat annulus in the z = 10 plane: the piece's
   `z_min` is exactly 10.000 and it still reaches z = 13 and spans the full 2 mm diameter. Before Ruling 14
   the shell was a cup that hung *below* the block top — Task 4's 8.289 mm³ included that hidden skirt, so
   84.8 % of exposed boss is not a regression against 87.9 %, it is a different (cleaner) solid.
3. **The last 15 % is lost to `RefineToLength`'s uniform subdivision.** Manifold subdivides a triangle by
   `ceil(maxEdge / L)` on *all three* edges. The boss's circumferential edges are 0.174 mm — far below
   L = 1.5 — but they are split into three anyway, because their triangle's diagonal is 3.005 mm. The
   resulting vertices sit at ⅓ and ⅔ of a chord and, being interior to a flat facet, carry the **facet**
   normal (measured: `n = (0.996195, ∓0.0871544, 0)`, `r = 0.996618`, `d = 0.994195`). When such a vertex
   offsets by ~its own half-thickness, its perpendicular distance to the axis collapses to 2 µm while its
   tangential position is preserved — so it lands ≈ 86° away from where the chord's endpoints land. The bottom
   ring becomes a self-crossing star, `check_shell` rejects the full-depth shell, and the fold guard halves
   the side depths to ≈ 0.5 mm. Verified by disabling the guard: the validity fallback then halves the whole
   group uniformly and the piece drops further, to 7.794 mm³ — the guard is doing its job, and doing it
   better than the fallback.
4. **§3.8 cannot recover the core.** At depth 0.5 mm the wall leaves a 0.25 mm-radius hole at z = 10, so the
   1.43 mm³ core is connected to the block: one body component that touches the source mesh, not an island.
   (Had the base ring kept its full 0.998 mm the hole would be 2 µm and absorption would have claimed the
   core — which is what the brief's ≥ 90 % assumed.)
5. **Knife edge, worth knowing:** with an even subdivision count the extra chord vertex is the chord
   *midpoint*, whose tangential offset is zero, so it offsets along the facet normal and stays correctly
   ordered — no star, and the shell would close on the axis. Here `ceil(3.005 / 1.5) = 3` rather than 2 purely
   because the side diagonal is 3.005 mm and not 3.000 mm.

**What this is not:** a user-visible defect. Every *visible* surface of the boss comes out as the painted
filament; the loss is an enclosed interior core that costs a few toolchanges.

**Options for the controller** (all design-level, hence not taken here): (a) accept 84.8 % and pin the test at
the measured value; (b) replace `RefineToLength` with an edge-selective refinement that only splits edges
longer than L (this is what §3.1a's own wording — "adds vertices only where edges are longer than L" —
describes, and it removes the star entirely); (c) make the fold guard bisect rather than halve, so it settles
near the true limit (≈ 0.66 mm here, ≈ 91 % of the boss) instead of overshooting down to 0.5 mm.

The boss test is committed with everything that *does* hold asserted (one piece, `z_min ≥ 10 − 1e-3`,
`z_max = 13`, full 2 mm span) plus a `WARN` carrying the measured volume and a comment recording the
expectation, the number and the mechanism. No weaker volume threshold was asserted in place of the brief's.

---

## 5. Re-measured S1 / S3

Appended to `.superpowers/sdd/2026-09-01-color-split/spike-report.md` (not committed, per the brief).

* **S3, unchanged:** one colour 0.908 s (was 0.893), three colours 3.846 s (was 3.821). Breakdown on the
  99 224-triangle sphere: patches 0.020 s, shells 0.197 s (of which `check_shell`/CGAL 0.102 s = 51.9 % of
  that stage, 11.3 % of the split), partition 0.687 s. The refinement pre-pass costs this fixture nothing —
  the sphere's longest edge is ≈ 0.40 mm against L = 1.5 mm, so the edge-length scan returns before any
  Manifold work. Differences are run-to-run noise.
* **S1:** refinement L = 1.5 mm, 158 → 6 928 triangles; shell 7.99018 mm³; piece 7.99018 mm³; body
  16001.4 mm³; `z_min` = 10.000 (no sub-surface skirt); 84.8 % of the 9.425 mm³ exposed boss.

---

## 6. Self-review

* **Completeness.** Both new API functions, the pipeline hook, the progress tick, Ruling 14, the file split,
  the CMake registration and the removal of the dead warning branch are all in. The move commit is
  behaviour-neutral (test file untouched, same assertion count). Refinement preserves closedness (asserted and
  enforced at runtime), volume (asserted to 1e-5) and states (asserted per facet).
* **The concave rule only fires on concave creases.** Checked against every existing fixture: the cube top,
  the thin plate rim, the grid-box cells and both sides of the L-bracket groove are all creases with
  `n_Q · t_in < 0` (convex) and are untouched; the spheres have no crease at all (`n_P · n_Q ≈ 1`). Their
  assertions — including the exact frustum volume of the painted cube top — are unchanged and still pass.
* **Quality / YAGNI.** `ColorSplitInternal.hpp` exports only what is genuinely cross-unit. The one structural
  addition (`GroupTopology`) was forced by the guard/builder divergence, and it is the hoist Task 6 needs.
  The `bottom_offset` helper is the single place the offset rule is spelled out.
* **Discipline.** Build-slot check before every build; the diagnostic test case was removed before the final
  build; the final build precedes the final test run and the commit; only source/test/CMake files staged
  (the untracked worktree junk and `.superpowers/` are not).

## 7. Concerns

1. **The boss expectation (§4).** 84.8 % vs the brief's 90 %. Needs a ruling.
2. **`RefineToLength` over-refines.** Spec §3.1a says refinement "adds vertices only where edges are longer
   than L"; Manifold's uniform per-triangle subdivision does not honour that, and on the boss it multiplies
   158 triangles into 6 928 where an edge-selective refinement would produce a few hundred. This is a cost as
   well as the cause of finding §4.3.
3. **The fold guard halves by 2.** On this fixture the true limit is ≈ 0.66 mm and the guard lands on 0.5 mm,
   giving away ~25 % of the core it could have kept. Spec §3.4 prescribes halving, so this is a spec question.
4. **`compute_vertex_depths` is now a one-line wrapper** over `ColorSplitDetail::vertex_depths`. It stays
   public because the tests and (later) the dialog use it; if that ever stops being true it can go.

---

# Fix report — Ruling 16 (Phong-carried normal field): measured, does not hold

Commit `2a1c18ad16` `test(color-split): pin the refined side-normal field on a two-ring cylinder`.
The normal-field change itself was implemented, measured and **reverted**; the source files are unchanged
from `a1aa0997d6`. Numbers below are from that implementation, on the boss and cylinder fixtures.

## What was implemented and measured

Exactly as ruled: `ColorPatches` gained a `normals` field, `extract_color_patches` filled it with
`color_split_normals`, `refine_color_patches` replaced it with a barycentric (Phong) interpolation of the
**coarse** surface's angle-weighted vertex normals over the parent facet (nearest facet by
`AABBMesh::squared_distance`, barycentrics of `closest`), and `build_color_shells` consumed the carried field.
The concave-crease re-measurement along n_P was kept.

## Result: the boss gets worse, and the requested test cannot pass

| | Committed (`color_split_normals` of the refined surface) | Ruling 16 (Phong-carried coarse field) |
|---|---|---|
| Boss piece | **7.990 mm³ (84.8 % of 9.425)** | **7.771 mm³ (82.5 %)** |
| Refined boss wall normal at z = 11 | `(1, 7.1e-07, -1.9e-08)` — radial | `(0.6997, 2.5e-08, 0.7144)` — 45° bisector |
| Refined boss wall normal at z = 12 | `(1, -6.8e-07, 1.8e-08)` — radial | `(0.7128, 1.4e-08, 0.7013)` |
| Requested cylinder test (`\|n_z\| < 1e-3`) | passes | **fails: `n_z = -0.49444`** |

**Why, in one line:** the coarse boss wall has only two vertex rings and *both* are shared with a cap, so
their angle-weighted normals are bisectors — measured `COARSE r=1 z=10 n=(0.686363, 3.5e-08, 0.72726)` and
`COARSE r=1 z=13 n=(0.725705, 3.5e-09, 0.688006)`. Interpolating that field can only ever produce bisectors
in between, so it hands the interior rings the very normals Ruling 13 added them to avoid, and the offsets go
back to running down-and-inward at ~45°. There is no radial information anywhere in the coarse vertex normals
to interpolate — the radial normals are a property of the *refined* geometry, which only
`color_split_normals(refined)` can see.

## What the ruling did get right — the diagnosis splits cleanly in two

The bottom ring under Ruling 16 is **no longer a star**. Measured, every bottom vertex at z = 10:

```
r=0.252851 ang=-147.448   r=0.252851 ang=-157.449   r=0.252850 ang=-167.448   r=0.252851 ang=-177.447
r=0.252850 ang= 172.552   r=0.252851 ang= 162.551   r=0.252851 ang= 152.551   r=0.252850 ang= 142.552   ...
```

— one radius to six figures and a clean 10° progression, against the scrambled ±86° jumps the committed code
produces. So the ruling's mechanism **does** fix the angular scrambling: a field that is smooth *across a
chord* removes the facet-normal/vertex-normal mismatch. It just also destroys the vertical structure, and the
depth still halves (r = 0.2529 either way) — now because the interior rings' offsets dive at 45° and fold
instead of because the ring scrambles.

Two independent defects, then, and each fix so far cures one and leaves the other:

| | angular order along a chord | vertical direction on the wall |
|---|---|---|
| `color_split_normals(refined)` (committed) | broken (star) | correct (radial) |
| Phong-carried coarse field (Ruling 16) | correct | broken (bisector) |

## The one change that cures both

Both defects have the same origin: **vertices that RefineToLength drops inside a coarse facet or inside a
short coarse edge.** Spec §3.1a says refinement "adds vertices only where edges are longer than L"; Manifold
subdivides a triangle *uniformly* by `ceil(maxEdge / L)`, so the boss's 0.174 mm chords are cut into three
because their triangle's diagonal is 3.005 mm. Refine edge-selectively — split only edges longer than L and
retriangulate the strip — and the wall gains its interior rings (radial normals, Ruling 13 satisfied) with
**no** chord-interior vertices at all, so there is no second normal field to mismatch and nothing to
interpolate. That is option (b) from the original report, and both measurements now point at it.

Second-choice option, much cheaper, worth ~6 points on its own: make the fold guard bisect between the floor
and the current depth instead of halving. The true limit on this fixture is ≈ 0.66 mm and the guard overshoots
to 0.5 mm; bisection lands near 0.66 → bottom radius ≈ 0.33 → ~91 % of the exposed boss.

## What was committed

Only the focused test Ruling 16 asked for, asserted against the field the pipeline uses: every refined side
vertex of the two-ring cylinder has `|n_z| < 1e-3` and points radially to within half a facet angle (the
chord quantisation of `fa = PI/18`). The previous refinement test only asserted that *one* such vertex
exists. `[colorsplit]` 1972 assertions in 29 cases, `[colorsplit_spike]` 8 in 2, `[paintdepth]` 1568 in 94 —
all green.

**S1 is unchanged from `a1aa0997d6`** (7.99018 mm³, 84.8 % of the exposed boss, z_min = 10.000): the reverted
experiment left no source change. The spike report's Task 5 section already carries those numbers, with the
Ruling 16 measurement added as a footnote.

---

# Fix report 2 — Ruling 17 (in-house edge-selective refinement): measured, regresses; root cause found

**Nothing committed.** The tree is unchanged from `2a1c18ad16`; all three suites re-verified green
(`[colorsplit]` 1972/29, `[colorsplit_spike]` 8/2, `[paintdepth]` 1568/94, boss 7.99018 mm³).

## What was implemented

Exactly as ruled. `refine_color_patches` moved into ColorSplit.cpp and rewritten as pass-based midpoint
bisection: each pass collects every edge longer than L, splits it at its midpoint, and retriangulates each
facet from whichever of its three edges gained one (1 split → 2 sub-facets, 2 → 3 with the quad cut along its
shorter diagonal, 3 → 4), so the surface stays conforming and 2-manifold with no T-junctions; sub-facets
inherit the parent's `facet_state` directly; capped at 32 passes; the whole Manifold path (and the
`as_original` flag `to_manifold64` had grown for it) deleted. Contract kept — closed, same volume, same
`states`, input returned untouched when nothing exceeds L. Every existing test still passed.

## Result: it regresses the boss, and so does every other variant tried

| Variant | Boss piece | % of 9.425 | Refined triangles |
|---|---|---|---|
| **Uniform `RefineToLength` (committed, `a1aa0997d6`)** | **7.990 mm³** | **84.8 %** | 6 928 |
| Phong-carried coarse normals (Ruling 16) | 7.771 mm³ | 82.5 % | 6 928 |
| Midpoint bisection (Ruling 17) | 7.553 mm³ | 80.1 % | 23 112 |
| Midpoint bisection + crease exempt from the fold guard | 5.891 mm³ | 62.5 % | 23 112 |
| **No refinement at all** (Ruling 14 only) | 7.604 mm³ | 80.7 % | 158 |

Bisection also triples the mesh: a 3.005 mm edge halves to 1.5025, which is still over L = 1.5, so it halves
again to 0.75 — longest-edge bisection refines to ~L/2 in practice.

## Why: midpoint bisection does not remove the chord-interior vertices

Measured on the boss wall, one 10° sector, position angle vs normal angle:

```
r=1        z=11.5   posang=0.000  nang=0.000   nz=0        d=0.998     <- new ring corner, radial
r=0.996196 z=11.5   posang=5.000  nang=5.000   nz=-1e-14   d=0.994196  <- chord MIDPOINT, consistent
r=0.997148 z=10.75  posang=2.495  nang=5.000   nz=-9e-08   d=0.994195  <- 1/4 chord: 2.5 deg MISMATCH
r=0.997147 z=10.75  posang=7.505  nang=5.000   nz=-6e-08   d=0.994195  <- 3/4 chord
```

The first bisection of a side quad's diagonal lands on the chord midpoint, where position and normal agree —
that part of the ruling is right. But the sub-edges it creates are still longer than L, so the next pass
splits them too, and *those* midpoints sit at ¼ and ¾ of the chord: position angle 2.495°, normal angle
5.000°. Offsetting such a vertex by its own half-thickness (0.994 mm) leaves 2 µm of perpendicular distance
but preserves the 0.0436 mm tangential offset, so it lands ~87° away from where the chord's endpoints land.
The star is back, with more vertices in it than before — hence 80.1 % against 84.8 %.

## The root cause is not the triangulation at all

The decisive measurement is the **null**: with no refinement whatsoever — the coarse two-ring boss, which has
no chord-interior vertices anywhere — the shell still cannot hold its depth. Bottom ring at z = 10 comes out
at r = 0.2513, i.e. the base ring was halved from 0.998 to 0.75, and the piece is 7.604 mm³.

So the binding constraint is simply that **a closed ring cannot be offset onto its own axis.** Spec §3.4's
clamp `d = t/2 − δ` is written for a two-sided plate, where the two half-shells meet at a mid-*surface*. On a
pin or a boss the medial set is a 1D axis, so the whole bottom ring collapses onto a 2 µm needle, the cap's
bottom cone folds back inside the side's bottom tube, `check_shell` rejects it, and the guard/fallback rein
the depth back to a bottom radius of 0.25–0.5 mm. That leaves a hole of the same radius in the crease
annulus at z = 10, which keeps the leftover core connected to the block — so §3.8 sees one body component
that touches the source mesh, not an enclosed island, and cannot absorb it.

Exempting the crease vertices from the fold guard (so the annulus could close the footprint on its own) was
tried and is the worst result of the five: the guard simply hammers the neighbouring vertices instead, and
the validity fallback still halves everything, 5.891 mm³.

## What would actually move it

Not a refinement or normal-field change — three have now been measured and the best is the one already
committed. The mechanism has to change, and both options are spec-level:

1. **§3.8's island rule.** Absorb a body component whose *only* contact with the source surface lies under a
   painted feature's own footprint — i.e. treat "connected to the body only through the crease hole" the way
   the spec already treats "fully enclosed". This is the smallest change that reaches ≥ 95 % on this fixture
   and it is exactly the intent §3.8 states ("a painted 1 mm boss is entirely its colour").
2. **§3.7's construction for a closed component.** Where a painted group wraps a feature completely (its
   boundary is a single crease loop and the offset would pass the medial axis), build the shell as the
   feature's own solid — the pin/boss analogue of the spec's "two half-shells meet" — instead of top ∪
   offset-bottom.

Neither is a Task 5 change. The committed behaviour is the best of everything measured, every visible surface
of the boss is the painted filament, and the shortfall is an invisible interior core costing a few
toolchanges.

**S1/S3 unchanged** (no source change landed): S1 7.99018 mm³, 84.8 %, z_min = 10.000; S3 0.908 s / 3.846 s.

---

# Fix report 3 — Ruling 18 (smooth-patch shells): implemented, boss at 98.7 %

Commit `354ce27c44` `feat(color-split): smooth-patch shells (Ruling 18); drop the refinement pre-pass`.

## What changed

**Smooth-patch grouping.** `connected_components` gained a `can_cross(a, b)` predicate;
`build_color_shells` passes `unit_face_normal(a).dot(unit_face_normal(b)) > cos 30 deg`, so a state's facets
join only across edges the surface crosses smoothly. A boss's side and its top cap, a cube's top and its side,
a groove's floor and its riser are each two patches and two shells; a 36-sided cylinder (10 deg per edge) and
the 99k-triangle sphere (1.15 deg) stay whole.

**Same-state crease walls.** `BoundaryInfo` gained `same_state`, the count of a vertex's boundary edges whose
outside facet carries the group's own state. `ConcaveCrease` generalised to `CreaseWall` (the map is now
`topo.wall`): a vertex takes `bottom = top - min(d, half_thickness_along(n_P)) * n_P` when its boundary is a
concave crease (Ruling 14, unchanged) **or** carries a same-state crease. `bottom_offset` is still the single
place the rule lives, so `build()` and `fold_guard()` judge the same geometry.

**Refinement retired.** `refine_color_patches`, `color_split_refine_length`, their declarations, their three
tests and the pipeline call plus its progress tick are gone; `ColorSplitPartition.cpp` is back to the Manifold
helpers and `partition_by_shells`, and `to_manifold64` lost the `as_original` flag it only needed for
refinement. The `2a1c18ad16` cylinder side-normal test went with them, as ruled.

## Deviation from the brief's wording, and why

The brief said a vertex uses `n_P` when its boundary edges are *all* same-state creases (or mixed with
concave ones). Implemented literally, the brief's own cube test cannot pass. The cube's top patch has exactly
four boundary vertices, each with exactly two boundary edges, and only the +X rim is same-state:

| top-patch corner | its two boundary edges | same-state |
|---|---|---|
| (40, 0, 20) | -Y rim (unpainted), +X rim (painted) | 1 of 2 |
| (40, 40, 20) | +X rim (painted), +Y rim (unpainted) | 1 of 2 |
| (0, 0, 20), (0, 40, 20) | -X and -+Y rims (unpainted) | 0 of 2 |

No vertex is all-same-state, so every corner would keep the bisector and the top shell would be the frustum
whose geometry the existing test `shell of a painted top face is a closed slab of depth D` already pins:
`zmin = 20 - 1.5/sqrt(3) = 19.1340` and `xmin = 1.5/sqrt(3) = 0.8660`. Neither branch of
`|zmin - 18.5| < 1e-4 || |xmin - 38.5| < 1e-4` holds.

The rule shipped is therefore **any same-state boundary edge at the vertex wins**. A vertex carries one bottom
copy and has to choose: at a corner where a same-state crease meets an ordinary boundary, the neighbouring
patch is claiming the material immediately behind the same-state wall, and tilting that wall is exactly what
opens a gap between the two claims — whereas the ordinary boundary only loses the cosmetic taper spec 3.6
gives it. It also leaves every fixture without a same-state crease untouched, which is what keeps the frustum
test (top face painted alone: no same-state edge anywhere) passing unchanged.

## Measurements

| | Value |
|---|---|
| Boss piece | **9.30484 mm³ of 9.42478 exposed = 98.73 %** (acceptance: >= 95 %) |
| Boss `z_min` | **10.000** (acceptance: >= 10 - 1e-3) |
| Boss shells | 2, both state 2 — side tube 9.242 mm³, top slab 4.430 mm³, each closed and free of self-intersections |
| Cube top + side | 2 shells; top slab `zmin` 18.5, side slab `xmin` 38.5; piece + body = 32000 to 1e-4 |

For contrast, the five refinement variants Task 5 measured earlier topped out at 84.8 % (see fix reports 1
and 2). The overlap the smooth-patch decomposition allows — the boss's cap slab and side tube share the
region z in [11.5, 13] — is what the sequential Split of spec 3.8 was already built to settle.

One existing fixture changed with the rule, not around it: the L-bracket groove's floor and riser meet at
90 deg, so they are now two patches and two shells (both closed, both clean); the assertion was updated and
the reason recorded in the test.

## Suites

`[colorsplit]` 250 assertions in 27 cases, `[colorsplit_spike]` 8 in 2, `[paintdepth]` 1568 in 94 — all green.
The `[colorsplit]` assertion count fell from 1972 because the deleted refinement tests looped a REQUIRE over
every facet of a refined mesh.
