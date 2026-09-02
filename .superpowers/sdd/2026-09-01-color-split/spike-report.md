# Colour Split — spike report (plan Task 4, decision checkpoint)

Spec: `docs/superpowers/specs/2026-09-01-color-split-design.md` §9 (rev 2.3).
Measured 2026-09-02 on `feat/color-split` in `C:\Dev\SnapmakerOrcaNext`, Release x64, from
`libslic3r_tests.exe "[colorsplit_spike]"`. Manifold 3.5.2, `MANIFOLD_PAR=OFF` (single-threaded).

## Verdict

**Engine A as designed.** No reduction of the check set, no engine B.

The partition is exact and complementary, coincident colour walls are handled, and the whole split of a
100 k-triangle part takes **0.89 s for one colour / 3.82 s for three** — interactive-job scale. The CGAL
self-intersection check that §9 asked us to price is **11 % of the split**, so there is nothing to gain by
weakening it; the cost sits in Manifold's `Split` (~76 %), which is the engine's whole point.

One spec claim did **not** survive the spike and needs a ruling: §3.8's parenthetical "a painted 1 mm boss is
entirely its colour". See S1 below. It does not change the engine choice.

## Fixture

`its_make_sphere(20.0, fa)` builds `ceil(2π/fa)` sectors × `ceil(π/fa)` stacks, i.e.
`sectors × (2·stacks − 2)` triangles. The brief's `PI/90` gives **32 040** triangles — a third of the 100 k
§9 calls for — so the spike uses **`fa = PI/158` → 316 × 314 = 99 224 triangles**
(`SPIKE_SPHERE_FA`, `tests/libslic3r/test_color_split.cpp`).

## S3 — timing (99 224-triangle sphere, r = 20 mm, D = 1.5 mm, `ColorSplitParams{}`)

| Case | Wall time | Pieces | Warnings |
|---|---|---|---|
| One colour (cap, z > 10) | **0.893 s** | 1 | 0 |
| Three colours (bands at z > 10, > −5, > −15) | **3.821 s** | 3 | 0 |

Stage breakdown, one colour (shell = 66 992 triangles):

| Stage | Time | Share of the split |
|---|---|---|
| `extract_color_patches` | 0.021 s | 2.3 % |
| `build_color_shells` | 0.195 s | 21.8 % |
| — of which `check_shell` (CGAL `does_self_intersect`) | **0.100 s** | **11.0 %** (51.5 % of that stage) |
| `partition_by_shells` (Manifold Split + island pass + volume check) | 0.678 s | 75.9 % |

`check_shell` was timed separately with `std::chrono` by re-running it on each finished shell
(`colorsplit spike: S3 stage breakdown and the CGAL check share`); `build_color_shells` calls it exactly once
per component when no fallback round is needed, which is the case here, so the two are directly comparable.

Notes:
- Three colours costs **4.3×** one colour, not 3×: each `Split` re-evaluates the shrinking `rest`, and the
  three band shells together cover ~87 % of the sphere against the cap's 25 %.
- Cancellation granularity: `Manifold::Split` is an *eager* op that does **not** observe an attached
  `ExecutionContext` (manifold.h:147-171 — only `Refine`/`Hull`/`Minkowski` and a deferred tree's `Status()`
  do). Cancellation therefore lands *between* shells. Worst-case latency here ≈ one Split ≈ 1 s.

## S1 — 2 mm boss on a block (painted entirely) — **expectation not met, engine not at fault**

`make_cube(40,40,10)` ∪ `its_make_cylinder(1.0, 4.0, PI/18)` translated to (20,20,9): a Ø2 × 3 mm boss with
1 mm of the cylinder buried in the block. Everything above z = 10.05 painted with filament 2, D = 1.5.

| Quantity | Value |
|---|---|
| Shell volume (`check_shell`) | **8.28923 mm³** |
| Piece volume after the partition | **8.28922 mm³** |
| Body volume | 16001.1 mm³ |
| Brief's expectation (`π·1²·4`, the whole cylinder) | 12.566 mm³ |
| Exposed boss only (`π·1²·3`) | 9.425 mm³ |

**The partition is exact.** Piece and shell agree to six significant figures — `Split` hands the shell straight
back, nothing is lost between the shell builder and the model, and island absorption adds nothing here. The
5 % assertion fails on the *shell*, not on the partition, for three separate reasons:

1. **1 mm of that cylinder is inside the block and carries no painted surface at all.** No rule in the spec
   can claim material that no painted facet bounds, so `π·1²·4` was never reachable; the ceiling is the
   exposed boss, 9.425 mm³.
2. **"Two half-shells meet at the axis" needs lateral vertices with radial normals, and this mesh has none.**
   After the union the boss wall carries exactly two rings — z = 10 (the intersection curve) and z = 13 (the
   cylinder top) — and *both* are junction vertices whose angle-weighted normals are ~45° bisectors. Every
   offset therefore runs down-**and**-inward; at the base ring it would cross the axis, so the §3.4 fold guard
   halves it first. The result is a cup roughly 0.5 mm thick, not a plug. A hand calculation of that geometry
   (base ring halved to d = 0.75, top ring to d = 0.71, cap centre at d = 1.5) gives ≈ 8.4 mm³ against the
   measured 8.289 — the mechanism is confirmed. Note this is the *normal* STL shape of a short cylinder, not a
   fixture artefact.
3. **The hollow core is not an enclosed island.** It opens downwards into the block, so §3.8's absorption
   rule — deliberately restricted to components that touch no source-mesh face — cannot claim it. Absorption
   itself works: the free-floating fully painted sphere (`[colorsplit]`) comes out 100 % its colour with an
   empty body, and turning `absorb_islands` off leaves the core behind exactly as specified.

**Consequence for the user:** the boss's entire visible surface is the painted colour; what stays body-coloured
is a fully hidden core. That is the same trade §3.8 already accepts for thin walls. **Recommended ruling:**
correct §3.8's parenthetical to say that a painted feature is claimed *whole* only when its leftover core is
fully enclosed (a free-standing feature); a feature attached to the body keeps a hidden core in the body
colour. The test now pins the properties that do hold (piece == shell to 1e-4; > 80 % of the exposed boss;
the piece reaches z = 13 and spans the full 2 mm diameter) and carries the evidence in a comment.

## Which paths the fixtures took

| Path | Hit? |
|---|---|
| Manifold `Status() != NoError` | **never** (all fixtures) |
| Volume check (`body + Σ pieces` within 1e-4 of the source) | passed on every fixture (it throws otherwise) |
| Self-intersection **fallback** (`check_shell` fails → halve → rebuild) | **never**, in any fixture |
| **Skip**-with-warning (Ruling 10) | once: the fully painted 0.15 mm ball at h = 0.2 |
| Fold guard (§3.4) | yes — silently, on the boss and on small fully painted spheres |

Why the fallback is never reached: the fold guard and the fallback halve towards the **same** floor,
`min(layer_height, d0)` (Ruling 9). Where the guard can act it gets there first and produces a valid shell on
the first check; where it cannot (floor already at `d0`), the fallback cannot act either and the component is
skipped. Pinned by `colorsplit: the halving floor is what decides skip versus salvage`, which shows the same
0.15 mm ball skipped at h = 0.2 and salvaged silently at h = 0.02.

The `"shell depth reduced (N halvings)"` warning the brief asked for is implemented (it fires only when the
fallback *salvaged* a shell, so it does not double up with the skip note) but **no fixture reaches its emit
line**. It stays as the observability hook for cross-facet self-intersections, which the fixture set does not
contain. Flagged for a ruling — removing it is a one-line revert.

## Other findings worth carrying forward

- **Manifold emits zero-length runs.** A `Split` output carries a `runOriginalID` entry for an input that
  contributed *no* faces (measured: the leftover core of the painted sphere carries a 0-triangle run for the
  source mesh). The island rule must ignore empty runs — counting them makes every island look as if it
  touched the source and **no island is ever absorbed**. Found by instrumenting the failing sphere test; fixed
  in `faces_by_original_id`.
- **Coincident colour walls are fine.** Three adjacent painted cells sharing side walls give pairwise
  intersections below 1e-3 mm³ and a total that matches the source volume to 1e-4; the three-band sphere does
  the same across two shared band boundaries.
- **Features thinner than ~6 µm get full depth D.** The thickness probe starts 1 µm inside the surface and
  discards any hit closer than 5 µm as a self-intersection, so a 3 µm plate measures `t = ∞` and falls back to
  `d = D`. The shell then overshoots the part, the partition clips it, and the whole feature becomes the
  painted colour — sane, and pinned by `colorsplit: an ultra-thin plate never asks for a negative depth`. The
  same guard makes `t/2 − δ < 0` unreachable, so the new `d ≥ 0` clamp is a guard rather than a live branch.

---

## Re-measured after Task 5 (refinement pre-pass + Ruling 14 concave creases), 2026-09-02

Same fixtures, same machine, `libslic3r_tests.exe "[colorsplit_spike]"` on commit `a1aa0997d6`.

### S3 — timing, unchanged

| Case | Task 4 | Task 5 |
|---|---|---|
| One colour (cap, z > 10) | 0.893 s | **0.908 s** |
| Three colours (bands) | 3.821 s | **3.846 s** |

Stage breakdown, one colour (99 224-triangle sphere, shell 66 992 triangles):

| Stage | Task 4 | Task 5 |
|---|---|---|
| `extract_color_patches` | 0.021 s | **0.020 s** |
| `build_color_shells` | 0.195 s | **0.197 s** |
| — of which `check_shell` (CGAL) | 0.100 s (11.0 % of the split) | **0.102 s (11.3 %, 51.9 % of that stage)** |
| `partition_by_shells` | 0.678 s | **0.687 s** |

The refinement pre-pass costs this fixture **nothing**: the sphere's longest edge is ≈ 0.40 mm against
L = max(0.87, min(1.5, 69.3/20)) = 1.5 mm, so `refine_color_patches`'s edge-length scan returns the patches
untouched before any Manifold work. Differences above are run-to-run noise (< 2 %).

### S1 — 2 mm boss on a block, with refinement and Ruling 14

| Quantity | Task 4 | Task 5 |
|---|---|---|
| Refinement | none | **L = 1.5 mm, 158 → 6 928 triangles** |
| Shell volume (`check_shell`) | 8.28923 mm³ | **7.99018 mm³** |
| Piece volume after the partition | 8.28922 mm³ | **7.99018 mm³** |
| Body volume | 16001.1 mm³ | **16001.4 mm³** |
| Piece below the block top (z < 10) | yes (a hidden painted skirt) | **none — z_min = 10.000** |
| Exposed boss (`π·1²·3`) | 9.425 mm³ | 9.425 mm³ |
| Share of the exposed boss | 87.9 % (incl. the skirt) | **84.8 % (all of it above z = 10)** |

Ruling 14 works: the wall at the boss/block crease is now a flat annulus in the z = 10 plane, so the piece
never leaves the boss footprint. The two numbers are not comparable share-for-share — Task 4's 8.289 counted
a skirt that hung *below* the block top inside the body.

**The remaining 15 % is a new finding (see task-5-report.md §Findings).** Refinement gives the boss side
interior rings at z = 11 and z = 12 whose normals are exactly radial and whose depth is the half-thickness
clamp 0.998 mm — the shell should therefore close on the axis and island absorption should claim the rest.
It does not, because `RefineToLength` subdivides a triangle *uniformly* by `ceil(maxEdge / L)`: the boss's
0.174 mm circumferential edges are split into three as well, and a vertex at ⅓ of a chord offsets along its
**facet** normal. Once the offset approaches the local half-thickness that vertex lands ~90° away from where
the chord's endpoints land, the bottom ring becomes a self-crossing star, and the fold guard halves the side
depths to ≈ 0.5 mm (verified by disabling the guard: the validity fallback then halves the whole group
uniformly and the piece drops further, to 7.794 mm³). The 0.5 mm-radius core that survives keeps a 0.25 mm
hole open at z = 10, so §3.8 cannot absorb it as an island either. Every *visible* surface of the boss is
still the painted filament; the loss is an interior core.

### Footnote — Ruling 16 (Phong-carried normal field) measured and reverted, 2026-09-02

Carrying the **coarse** surface's angle-weighted normals onto the refined surface by barycentric interpolation
was implemented and measured: the boss piece drops to **7.77124 mm³ (82.5 %)** from 7.99018 (84.8 %). The
coarse boss wall has only two vertex rings and both are shared with a cap, so both carry bisector normals
(measured `z=10 n=(0.686363, 3.5e-08, 0.72726)`, `z=13 n=(0.725705, 3.5e-09, 0.688006)`); interpolating them
gives the new interior rings `z=11 n=(0.6997, 0, 0.7144)` instead of the radial `(1, 7.1e-07, -1.9e-08)` the
refined surface's own field gives, i.e. exactly the cup Ruling 13 removed. It *does* fix the angular
scrambling — the bottom ring comes out at one radius (0.252851) in a clean 10° progression — so the two
defects are separable; see task-5-report.md for the decomposition. Source reverted; S1 above stands.

### Footnote 2 — Ruling 17 (in-house midpoint bisection) measured and reverted, 2026-09-02

Edge-selective midpoint bisection gives **7.553 mm³ (80.1 %)** from 23 112 triangles, against 7.990 (84.8 %)
from 6 928 with `RefineToLength`. It does not remove the chord-interior vertices: the first bisection of a
side quad's diagonal lands on the chord midpoint (consistent), but its sub-edges are still over L and the
next pass puts vertices at ¼ and ¾ of the chord — measured `posang=2.495 nang=5.000`. The decisive
measurement is the **null**: with *no* refinement at all the shell still cannot hold its depth (base ring
bottom r = 0.2513, piece 7.604 mm³), so the limit is not the triangulation but that a closed ring cannot be
offset onto its own axis. Five variants measured, the committed one is the best; see task-5-report.md.

### Re-measured after Ruling 18 (smooth-patch shells, no refinement), 2026-09-02

Commit `354ce27c44`.

**S1 — 2 mm boss on a block.** The painted region is now two smooth patches (side tube, top cap), each its own
shell, and the same-state crease between them gives both a straight wall.

| Quantity | Task 4 | Ruling 13/14 (refined) | **Ruling 18** |
|---|---|---|---|
| Shells | 1 | 1 | **2** (side tube 9.242 mm³, top slab 4.430 mm³) |
| Piece volume | 8.28922 mm³ | 7.99018 mm³ | **9.30484 mm³** |
| Share of the 9.425 mm³ exposed boss | 87.9 % (incl. a sub-surface skirt) | 84.8 % | **98.73 %** |
| Piece below the block top | yes | none | **none — z_min = 10.000** |
| Surface triangles fed to the shells | 158 | 6 928 (refined) | **158 (no refinement)** |

**S3 — timing, unchanged.** One colour **0.889 s** (Task 4: 0.893), three colours **3.867 s** (3.821).
Stage breakdown on the 99 224-triangle sphere, shell 66 992 triangles: `extract_color_patches` 0.0205 s,
`build_color_shells` 0.1958 s (of which `check_shell`/CGAL 0.0999 s = 51.0 % of that stage, 11.3 % of the
split), `partition_by_shells` 0.665 s. The smooth-patch predicate costs nothing measurable: the sphere bends
1.15 deg per edge, far under the 30 deg threshold, so it stays a single patch and the grouping is one extra
dot product per adjacency.

### S4 — slice comparison (spec 8.7) and the painted-cube-top wedge (spec 3.6), 2026-09-02

Task 8, test-only (`tests/libslic3r/test_color_split.cpp`). Config for both halves: `split_test_config()` with
`layer_height` = `initial_layer_print_height` = 0.2, classic wall generator, outer/inner line widths 0.42/0.45,
0.4 nozzle, `paint_depth_mode` = walls at 3 walls, `mmu_segmented_region_interlocking_depth` left at its 0.1
default. That resolves to **D = 1.40885 mm, ws = 0.79708 mm** on both paths (the 2D band goes through the same
`paint_depth_band_mm` + `paint_depth_band_classic_floor_mm` pair as `color_split_depths`).

**S4a — slice parity on a vertical wall.** 40x40x20 cube, +X face painted Extruder2, sliced twice through the
real `Print` pipeline: unsplit (2D paint-depth segmentation) and split into body + one part (3D). Same layer
count (100 layers) both ways. Filament-2 area per layer over the compared middle half, layers 25-74
(print_z 5.2-15.0), 50 layers:

| | 2D, odd layers (25 of them) | 2D, even layers (25) | 3D, every layer |
|---|---|---|---|
| filament-2 area | 54.3691 mm² | 50.6409 mm² | 45.8867 mm² |
| \|2D - 3D\| | 8.48242 mm² | 4.75419 mm² | — |

Bound = one outer wall line over the 40 mm painted edge = 40 x 0.42 = **16.8 mm²**; the worst measured
difference is 8.48 mm², **50.5 % of budget**. The 3D area is constant to the last printed digit on all 50 layers.

The 2D alternation is the interlocking notch, not noise: `paint_depth_classic_notch_cap_mm` leaves the
configured 0.1 mm intact here (band slack 1.40885 - 0.79708 = 0.612 mm), so even layers cut at 1.30885 mm.
Both parities sit inside the bound, so no relaxation was needed and `mmu_segmented_region_interlocking_depth`
was left at its default rather than zeroed.

The residual 8.48 mm² is spec 3.6's own geometry, not an error. Case B gives the side patch a ws-deep ring at
the surface and then tapers along the cube's corner bisector, so the piece is ws + (D - ws)/sqrt(3) = 1.150 mm
deep where the 2D band is 1.409 mm deep. 40 x 1.15028 = 46.011 mm² against the measured 45.887; the 0.124 mm²
gap is the shell tapering at the two +-Y ends of the patch (the 2D claim tapers at its ends too, at 45 degrees,
where the +X edge's Voronoi cell meets the +-Y edges' - D x (40 - D) = 54.365 against the measured 54.369).

*Assertion bite (measured, then reverted).* Re-running the same case with `no_cap_no_step()` — crease step off,
so the shell is a pure bisector taper of D/sqrt(3) = 0.813 mm — drops the 3D area to **31.8744 mm²** and the
difference to **22.4947 mm²**, which fails the 16.8 mm² bound. The budget is therefore tight enough to catch a
regression of the crease step, and loose enough for the step as specified.

**S4b — painted cube top, the wedge with and without the wall-stack step.** Same cube with the top face
painted, `flat_cap` off so the step alone decides the shape. Areas read on layer 98 (print_z 19.8), the layer
just below the surface layer:

| crease_step | body (filament 1) | piece (filament 2) | body / (4 x 40 x ws) |
|---|---|---|---|
| off | 47.6398 mm² | 1552.36 mm² | 0.374 |
| on  | **124.991 mm²** | 1475.01 mm² | **0.980** |

One wall stack ring = 4 x 40 x ws = 127.533 mm²; the exact ring of a 40 mm square inset by ws is
4 x 40 x ws - 4 ws² = 124.99 mm², which is what "on" measures to five digits — the step lands the piece
exactly one wall stack in from all four side faces. Without it the wedge leaves the body a 0.30 mm ring, under
one outer wall line (0.42 mm), so the top piece's colour would print right out to the side faces on that
layer. The requirement (body >= 0.9 x 4 x 40 x ws = 114.78 mm²) passes at 124.99 and would fail at 47.64.

### Re-measured after Ruling 24 (mitred bisector offsets, spec 3.4a), 2026-09-02

Task 9. Every segment that travels along a vertex bisector n(v) now spends `d / max(n(v)·n_P, 0.5)` of length
instead of `d`, then is clamped by the half-thickness probe along that same direction. Segments along n_P
(spec 3.6 case A, case B's first step, the concave and same-state walls) are untouched.

**S1 — 2 mm boss on a block, painted entirely** (`depths_for_test(1.5)`, `ColorSplitParams{}`):

| Quantity | Ruling 18 | **Ruling 24** |
|---|---|---|
| Shells | 2 | **2** (side tube **9.37426** mm³, top slab **4.43006** mm³) |
| Piece volume | 9.30484 mm³ | **9.37566 mm³** |
| Share of the 9.42478 mm³ exposed boss | 98.73 % | **99.48 %** |
| Piece below the block top | none | **none — z_min = 10.000** |

As predicted, almost unaffected: the boss's walls run along n_P. The bottom of the side tube is a concave
crease (Ruling 14, straight down n_P), its top rim is spec 3.6 case B whose first segment is also along n_P,
and the top slab is case A throughout — the top slab's volume does not move at all (4.430 both times). The
0.071 mm³ the piece gains is case B's SECOND segment on the side tube's rim, which is the one bisector
segment the boss has: its length grows by 1/(n(v)·n_P) ≈ √2 at a 90° rim, so the tube reaches
1 − 0.87 = 0.13 mm behind the cap instead of 0.13/√2.

**S3 — timing** (99 224-triangle sphere, r = 20 mm, D = 1.5 mm, `ColorSplitParams{}`). One colour
**1.017-1.020 s**, three colours **6.56-7.20 s** (two runs; the three-colour figure is the noisy one — it is
three sequential Manifold splits). Stage breakdown, shells 69 520 triangles: `extract_color_patches` 0.020 s,
`build_color_shells` 0.2585 s (of which `check_shell`/CGAL 0.1176 s = 45.5 % of that stage, 11.1 % of the
split), `partition_by_shells` 0.7858 s.

These are **not** comparable like-for-like with the Ruling 18 line above (0.889 s / 3.867 s, shells 66 992
triangles): that measurement predates Tasks 6-8, i.e. Rulings 20-22 added a closest-point + ray probe per
crease boundary vertex and changed which vertices step. Ruling 24 itself cannot account for any of it on this
fixture: the sphere bends 1.15° per edge, so n(v)·n_P ≥ 0.9998 at every vertex of the painted cap — flank and
crown alike — and the mitre lengthens those segments by at most 0.02 %, far too little to move a collapse
test, a triangle count or a CGAL run. Its per-vertex cost is one dot product and one `std::map` lookup.

**S4 — the painted-cube-top wedge, unchanged.** Body area on layer 98 (print_z 19.8) of the 40×40×20 cube with
the top painted, `flat_cap` off: `crease_step` off **47.6398 mm²**, on **124.991 mm²** — both identical to the
Task 8 measurement, and the requirement (body ≥ 0.9 × 4 × 40 × ws = 114.78 mm²) still passes at 124.99.
Expected: the unmitred corner offset d·(±1,±1,1)/√3 already descended at 45°, so on a layer 0.3 mm below the
surface the piece was already inset 0.3 mm. What the mitre changes is how FAR down that taper runs
(0.813 mm → 1.409 mm), not its slope, and layer 98 is inside both.

**S4a — slice parity on a vertical wall, re-measured.** Same fixture and config as the Task 8 table
(D = 1.40885 mm, ws = 0.79708 mm, interlocking depth left at 0.1), layers 25-74:

| | 2D, odd layers (25) | 2D, even layers (25) | 3D, every layer |
|---|---|---|---|
| filament-2 area | 54.3691 mm² | 50.6409 mm² | **55.9797 mm²** (was 45.8867) |
| \|2D − 3D\| | **1.61059 mm²** (was 8.48242) | **5.33886 mm²** (was 4.75419) | — |

The 3D area is now the derived `40·D − (D − ws)² = 55.9797` on all 50 layers, to the last printed digit: the
mitre buries exactly D perpendicular to the painted face, and the only thing between the piece and the 2D
Voronoi trapezoid `40·D − D² = 54.3691` is spec 3.6 case B holding the full wall stack before the 45° taper
starts — `ws·(2D − ws) = 1.611 mm²`, the deliberate deviation §3.6 documents. The odd-layer agreement
therefore improved 5.3×.

**The brief's 4.0 mm² parity bound is not reachable, and not because of the mitre.** The worst layer is an
EVEN one, where the 2D path's interlocking notch (`mmu_segmented_region_interlocking_depth` = 0.1 mm, intact
here) removes a further `i·(40 − 2D + i) = 3.72823 mm²` from the 2D claim while the 3D piece has no such
notch. 1.61059 + 3.72823 = 5.33882 mm² — measured 5.33886 — against a 4.0 budget. The test now derives its
bound from those two terms plus 0.05 of slack (**5.38882 mm²**), pins the 3D area to `40·D − (D − ws)²`
within 0.05 mm², and WARNs all five numbers, so either deviation growing fails the case.

### Ruling 25 (a vertical/vertical crease is a tie, not case B), 2026-09-02

S1, S3 and S4 re-run after the tie rule: **S1 unchanged bit for bit** (side tube 9.37426 mm³, top slab
4.43006, piece 9.37566 = 99.48 % of exposed) and **S4 unchanged bit for bit** (body 47.6398 / 124.991 mm²).
Shell triangle count on the S3 sphere unchanged at 69 520; S3 timings are run-to-run noise (one colour
0.986-1.020 s, three colours 6.56-9.64 s over three runs). Expected: the tie only fires where a painted face's
boundary vertex has *no* horizontal neighbour, and none of these fixtures has one — the boss's creases are
side/top, the wall's and the cube's corners each carry a cap edge as well as a vertical one.

**S4a parity is likewise unchanged: 3D 55.9797 mm², worst diff 5.33886 of the derived 5.38882 bound.** The
+X face of a plain cube has no tie vertex: each of its four corners carries a horizontal boundary edge
(against the top or bottom face) as well as a vertical one, so its mean n_Q has |n_Q·z| = 0.707 against the
patch's 0 — a genuine case B, and the one the "painted side face keeps its full wall stack up to the top edge"
case pins. A vertex has one ring copy, so the ws ring those corners earn at the caps is inherited by the whole
±Y edge between them. The new `colorsplit: a vertical crease between two side faces takes the plain bisector,
not case B` case (fixture `make_ringed_box`, side faces split at half height) shows both behaviours on one
patch: corners at (39.13, 0, 0) → (38.5, 0.63, 0.63), the half-height tie at (39.8, 0.2, 10) → **(38.5, 1.5,
10)**, the 2D 45° diagonal exactly.

### Ruling 26 (parity comparison zeroes the 2D interlocking notch), 2026-09-02

The parity case's `e2e_config()` now sets `mmu_segmented_region_interlocking_depth = 0`: the notch is a
2D-only artefact of segmenting layer by layer (the 3D split's pieces are solids, perimetered once), and at its
0.1 default it alternated the 2D claim by 3.73 mm²/layer and swamped the geometric difference being measured.

**S4a — slice parity on a vertical wall, final** (D = 1.40885 mm, ws = 0.79708 mm, layers 25-74):

| | 2D, odd layers (25) | 2D, even layers (25) | 3D, every layer |
|---|---|---|---|
| filament-2 area | 54.3691 mm² | **54.3691 mm²** | 55.9797 mm² |
| \|2D − 3D\| | 1.61063 mm² | 1.61063 mm² | — |

Odd and even now agree exactly, which is the zeroing working. The worst difference is **1.61063 mm² of the
4.0 mm² bound (40 %)**, and it matches the derived per-vertex case B corner hold `ws·(2D − ws) = 1.61059` to
within 4·10⁻⁵ (slicer polygon arithmetic). The 3D area is pinned separately to the derived
`40·D − (D − ws)² = 55.9797` within 0.05 mm².

S1, S3 and S4b are untouched by this change (it is test-config only, and S4b slices split objects, which carry
no paint for the 2D path to segment): boss piece 9.37566 mm³ / 99.48 %, wedge body 47.6398 (step off) and
124.991 mm² (step on).

### Final verification run (Task 11), 2026-09-02

Full rebuild of `ALL_BUILD` at commit `99fb7ab2f7` (the Tasks 9-10 review polish), then
`libslic3r_tests.exe "[colorsplit_spike]"` and `"[colorsplit]"` on the same binary. The polish is comments,
one renamed constant and GUI error routing, so nothing here was expected to move - and nothing did, beyond
run-to-run timing noise.

**S1 - 2 mm boss on a block, painted whole** (`depths_for_test(1.5)`, `ColorSplitParams{}`):

| Quantity | Ruling 24/25 | **Task 11** |
|---|---|---|
| Side tube shell | 9.37426 mm3 | **9.37426 mm3** |
| Top slab shell | 4.43006 mm3 | **4.43006 mm3** |
| Piece volume | 9.37566 mm3 | **9.37566 mm3** |
| Share of the 9.42478 mm3 exposed boss | 99.48 % | **99.48 %** |
| Body volume | - | 16000 mm3 (whole boss 12.5664) |

**S3 - timing** (99 224-triangle sphere, r = 20 mm, D = 1.5 mm, `ColorSplitParams{}`): one colour
**1.01306 s** (1 piece, 0 warnings), three colours **6.52579 s** (3 pieces, 0 warnings). Stage breakdown,
shells 69 520 triangles: `extract_color_patches` **0.0199538 s**, `build_color_shells` **0.229673 s** (of
which `check_shell`/CGAL **0.103846 s** = 45.2147 % of that stage, **10.3237 % of the whole split**),
`partition_by_shells` **0.756268 s**; 0 shell warnings. Within noise of the Ruling 24 measurement
(1.017-1.020 s / 6.56-7.20 s, 0.020 / 0.2585 / 0.7858 s).

**S4a - slice parity on a vertical wall**, layers 25-74, D = 1.40885 mm, ws = 0.79708 mm, interlocking notch
zeroed (Ruling 26): 2D odd **54.3691 mm2**, 2D even **54.3691 mm2**, 3D **55.9797 mm2** on every layer;
worst difference **1.61063 mm2 of the 4.0 mm2 bound (40 %)**, against the derived case-B corner hold
ws*(2D - ws) = 1.61059 and one outer wall line = 16.8 mm2. Unchanged.

**S4b - painted cube top**, layer 98 (print_z 19.8), `flat_cap` off: `crease_step` off body **47.6398 mm2**
/ piece 1552.36 mm2; on body **124.991 mm2** / piece 1475.01 mm2; one wall stack ring 127.533 mm2 at
ws = 0.79708 mm. Unchanged.

**Suite summaries** (same binary): `"[colorsplit]"` 912 assertions in 56 cases, twice, identical;
`"[colorsplit_spike]"` 24 in 3; `"[paintdepth]"` 1568 in 94; `"[chameleon]"` 605 in 133; the whole
`libslic3r_tests.exe` 581 cases / 52 583 assertions with 2 failing as expected (both pre-date this feature).
