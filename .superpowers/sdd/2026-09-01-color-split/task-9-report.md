# Task 9 report — Mitred bisector offsets (Ruling 24, spec rev 2.9)

Branch `feat/color-split` in `C:\Dev\SnapmakerOrcaNext`. Commit **`06fdb3fcf8`** —
`feat(color-split): mitred bisector offsets so the perpendicular depth equals D (Ruling 24)`.

## What I implemented

**The rule.** A segment that travels along the vertex bisector n(v) now spends `d / max(n(v)·n_P, 0.5)` of
length and is then clamped by the half-thickness probe along that same direction:
`min(d / cosang, t(v)/2 − δ)`. d(v) is a depth measured *perpendicular to the patch*, so the mitre is what
makes the piece actually D deep; the 0.5 floor is the mitre limit of 2 (a 60° half-angle).

| File | Change |
|---|---|
| `src/libslic3r/ColorSplitShell.cpp` | `GroupTopology::n_patch` — unit n_P per (vertex, wedge) for **every** group vertex, not just boundary ones (the accumulation loop that already existed, with the `boundary_count > 0` filter dropped and the result normalised once); `BoundaryInfo::n_p` deleted and the crease classification reads `topo.n_patch` instead, so both rules share one n_P by construction. `ShellBuilder::half` (the raw clamp) and `ShellBuilder::bisector_length(v, n_p, d)`. Three call sites in `side_offsets`. |
| `src/libslic3r/ColorSplit.cpp`, `ColorSplitInternal.hpp` | `ColorSplitDetail::vertex_depths` gained an optional `std::vector<float> *half_thickness` out-parameter returning `t(v)/2 − δ` **before** D is applied. **This is the cheapest correct option**: the probe was already being run for d(v), so the clamp costs zero extra ray casts. `depth[v]` could not serve — it is `min(D, t/2 − δ)`, and using it as the clamp would cap the mitred length at D and cancel the mitre exactly. `compute_vertex_depths`'s public signature is unchanged (the parameter defaults to `nullptr`). |
| `src/libslic3r/ColorSplit.hpp` | The comment on `compute_vertex_depths` now states the depth model as spec 3.4 + 3.4a: d(v) is a perpendicular depth, the mitred length is what is applied along n(v), and segments along n_P spend d(v) as it stands. |

**Mitred** (all four places the brief lists): the plain boundary's ring (h) and bottom (d); spec 3.6 case B's
**second** segment; every interior vertex's bottom. The `!crease_step || boundary_count == 0` branch covers
interior vertices *and* boundary vertices when the crease step is off — both take the plain bisector rule, so
both are mitred. **Untouched**: everything travelling along n_P — case A's ring and bottom, case B's
wall-stack first segment, and the concave and same-state walls (they return before the mitre is reached).
Pinch vertices' nudge is applied in `vertex_ids` to the top copy, so ring and bottom still carry it.

Two invariants worth recording. (1) The mitred length is never *shorter* than the old one: `depth[v] ≤ half[v]`
always (it starts as `min(D, half[v])` and the halving floor is `min(h, d0[v]) ≤ half[v]`), so
`min(d/cosang, half[v]) ≥ d`. Nothing that passed before can lose depth. (2) Case B's n_P budget (Ruling 21)
becomes *exact* rather than merely bounded: the second segment buries `L₂·cosang = d_p − first` along n_P, so
the bottom sits `first + (d_p − first) = d_p` behind the surface, never more.

## RED / GREEN evidence

**RED** — tests first, production code untouched, built and run: **8 of 55 `[colorsplit]` cases failed**, each
with the old 1/√3 number as the expansion:

```
colorsplit: shell of a painted top face is a closed slab of depth D
  REQUIRE_THAT( c.volume, WithinRel(frustum, 1e-4) )        1326.5075683594 vs 2224.5
colorsplit: depth_override_mm replaces D and clears unlimited
  REQUIRE_THAT( min_z, WithinAbs(20.f - 0.7f, 1e-4f) )      19.59586f vs 19.2999992371
colorsplit: flat top is capped at the solid shell depth, slopes are not
  REQUIRE_THAT( min_z(shells[0].mesh), WithinAbs(20.f - 0.6f, 1e-4f) )  19.65359f vs 19.3999996185
colorsplit: a painted cube top and side are two smooth patches with straight walls    (vertex pin, 0 == 1)
colorsplit: a plain painted face is D deep perpendicular to itself (mitred bisector)  (vertex pin, 0 == 1)
colorsplit: a painted top narrower than two wall stacks falls back to the bisector    (vertex pin, 0 == 1)
colorsplit: a capped group and the uncapped group beside it meet along a straight wall (vertex pin, 0 == 1)
colorsplit e2e: split parts slice like the 2D paint-depth claim on a painted side face
  REQUIRE_THAT( a3, WithinAbs(claim_3d, 0.05) )   45.8866865812 is within 0.05 of 55.9797166823
    layer 25 print_z 5.2: 2D 54.3691, 3D 45.8867, diff 8.48242 (bound 5.38882)
```

`test cases: 55 | 47 passed | 8 failed`. The parity RED line is the strongest evidence the numbers were
*derived*: `claim_3d = 55.9797166823` and `bound = 5.38882` are computed by the test from D and ws and were
predicted by hand before the run; the old code produced 45.8867, the Task 8 figure to the last digit.

**GREEN** — after the implementation, and after the two comment fixes from self-review were rebuilt:

```
[colorsplit]        All tests passed (871 assertions in 55 test cases)
[colorsplit_spike]  All tests passed (24 assertions in 3 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

`[colorsplit]` carries exactly two `warning:` lines: the pre-existing S1 boss WARN (test_color_split.cpp:705)
and the parity WARN the brief asked for (:1734). No source was edited after the final build.

## Re-derived expectations

| Case | Was | **Now** | Derivation |
|---|---|---|---|
| Block-top slab volume | 1326.507 mm³ | **2224.5 mm³** | corners mitred to 1.5·√3 → 1.5 per axis; square frustum h/3·(A₁+A₂+√(A₁A₂)) with A₁ = 40², A₂ = 37², h = 1.5 |
| Flat cap, step **off**, zmin | 19.6536 | **19.4** | 0.6·√3 along the bisector = 0.6 of z (the step-**on** value was already 19.4 and is unchanged) |
| `depth_override_mm` 0.7, min z | 19.5959 | **19.3** | 0.7·√3 along the bisector = 0.7 of z |
| Cube top+side, plain-bisector corners | (0.866, 0.866, 19.134) etc. | top slab **(1.5, 1.5, 18.5)**, side slab **(38.5, 1.5, 1.5)** | every bottom of the top slab now at z = 18.5 and of the side slab at x = 38.5, by two different rules |
| Narrow top (1.5 mm) ring | 0.1155/axis | **0.2/axis** | h mitred to 0.2·√3 = one *real* layer down. The bottom does **not** move: 1.29704 is already the half-thickness clamp |
| Capped/uncapped meeting, case B `rim` | 0.3637 | **0.63** = D − ws | the mitred second segment spends D − ws per axis, so the piece is exactly D deep along n_P |
| **New** direct pin, +X-painted cube | — | bottoms at **(38.5, 1.5, 1.5), (38.5, 38.5, 1.5), (38.5, 1.5, 18.5), (38.5, 38.5, 18.5)**; volume 1069.5 mm³ | the brief's pin, plus the prismatoid h/6·(A_top + 4·A_mid + A_bot) = 1.5/6·(800 + 4·712.25 + 629) |
| Plate fixtures (0.3 mm, 1.2 mm, ultra-thin, both-sides) | — | **unchanged, verified passing without edits** | their corners are clamped by the diagonal half-thickness (1.03723 on the 1.2 mm plate, 0.25781 on the 0.3 mm), and `min(d/cosang, half) = half` whenever `d = half` |

## Parity — the brief's 4.0 mm² bound is not reachable (evidence, not a loosening)

Measured over layers 25-74, `WARN`ed by the case itself:

```
parity over layers 25..74: 2D odd 54.3691, 2D even 50.6409, 3D 55.9797 mm^2;
worst diff 5.33886 of 5.38882 (2D claim 54.3691, 3D claim 55.9797,
case B ring 1.61059, notch 3.72823; D 1.40885, ws 0.79708, one outer wall line 16.8)
```

The 3D per-layer area is **not** 40·D − D² = 54.3691 but **40·D − (D − ws)² = 55.9797**, constant on all 50
layers. The brief's derivation assumed the 45° taper starts at the painted surface; spec 3.6 **case B** holds
the full wall stack first (the ±Y vertical edges of the painted face are convex creases with
`|n_P·z| = |n_Q·z| = 0`, which the code classifies as case B), so the chamfer starts at a depth of ws. The gap
is exactly `ws·(2D − ws) = 1.611 mm²` — the deviation §3.6 documents and asks for. Against the odd layers the
agreement improved 5.3× (8.482 → 1.611 mm²).

The worst layer is an **even** one, where the 2D path's interlocking notch removes a further
`i·(40 − 2D + i) = 3.728 mm²` (i = `mmu_segmented_region_interlocking_depth` = 0.1, left at its default as
Task 8 decided). **1.611 + 3.728 = 5.339 mm²** — measured 5.33886 — so a 4.0 mm² budget cannot pass whatever
the mitre does. Nothing was loosened to hide geometry: instead the case now

* pins the 3D area to the derived `40·D − (D − ws)²` within **0.05 mm²** (a far tighter statement than any
  parity bound — it says the mitre buries exactly D), and
* derives its 2D-vs-3D bound from those two terms plus 0.05 of slack, **5.38882 mm²** (down from 16.8), so
  either deviation growing fails the case, and
* `WARN`s all five numbers, with `one_line` now read from the config's `outer_wall_line_width` instead of the
  literal 0.42 (the folded Task 8 minor) and reported alongside.

Two alternatives were available and rejected: zeroing `mmu_segmented_region_interlocking_depth` in
`e2e_config()` would bring the worst diff to 1.611 mm² and let 4.0 pass, but Task 8 deliberately kept the
default and the notch is real 2D behaviour; and comparing only odd layers would hide half the data. **The
controller's ruling is invited** — if the 4.0 figure is wanted, the honest route is zeroing the interlocking
depth, not moving the geometry.

## Re-recorded spike numbers (`spike-report.md`, not committed)

**S1 — 2 mm boss.** Shells 2 (side tube **9.37426** mm³, top slab **4.43006** — the slab is unchanged to five
digits; it is case A throughout). Piece **9.37566 mm³** = **99.48 %** of the 9.42478 mm³ exposed boss (was
9.30484 / 98.73 %). z_min still 10.000, nothing below the block top. As predicted the boss is all but
unaffected: its walls run along n_P (concave crease at the base, case A on the cap, case B's first step on the
rim); the 0.071 mm³ gained is the one bisector segment it has, case B's second, whose length grows by √2 at a
90° rim.

**S3 — timing.** One colour **1.017-1.020 s**, three colours **6.56-7.20 s** (two runs). Breakdown on the
99 224-triangle sphere (shells 69 520 tri): patches 0.020 s, shells 0.2585 s (of which `check_shell`/CGAL
0.1176 s = 45.5 % of that stage, 11.1 % of the split), partition 0.7858 s. **Not comparable like-for-like**
with the last recorded S3 (Ruling 18: 0.889 / 3.867 s, 66 992 shell triangles): that measurement predates
Tasks 6-8, which added a closest-point + ray probe per crease boundary vertex and changed which vertices step.
Ruling 24 cannot account for any of it on this fixture — the sphere bends 1.15° per edge, so n(v)·n_P ≥ 0.9998
at every painted vertex and the mitre lengthens those segments by ≤ 0.02 %, far too little to move a collapse
test or a triangle count. Its own cost is one dot product and one `std::map` lookup per vertex.

**S4 — the wedge, unchanged.** Layer 98 (print_z 19.8) body area: `crease_step` off **47.6398 mm²**, on
**124.991 mm²** — identical to Task 8, and the ≥ 0.9 × 127.533 requirement still passes at 124.99. Expected:
the unmitred corner offset d·(±1,±1,1)/√3 already descended at 45°, so 0.3 mm below the surface the piece was
already inset 0.3 mm. The mitre changes how far *down* that taper runs (0.813 → 1.409 mm), not its slope.

## Files changed

- `src/libslic3r/ColorSplitShell.cpp`, `ColorSplit.cpp`, `ColorSplit.hpp`, `ColorSplitInternal.hpp`,
  `tests/libslic3r/test_color_split.cpp` — committed in `06fdb3fcf8` (+154 / −47).
- `.superpowers/sdd/2026-09-01-color-split/spike-report.md` — "Re-measured after Ruling 24" section appended
  (not committed).
- This report (not committed).

## Self-review findings (all fixed before reporting)

1. **`std::map<..., Vec3f>::operator[]` would have accumulated onto garbage.** Eigen's default constructor
   leaves a vector uninitialised, so a value-initialised map slot is not zero (this is why the existing
   `nudge` map uses find/emplace, and why `BoundaryInfo` gets away with `operator[]` — its NSDMIs zero the
   members). Written as `emplace(key, Vec3f::Zero()).first->second += nf`, with the reason in a comment.
2. **`BoundaryInfo::n_p` became a second, independent copy of n_P.** Deleted it and pointed the crease
   classification at `topo.n_patch`, so the mitre and the crease rules cannot drift apart.
3. **Two comments were sloppy** (an ungrammatical sentence about the interior-vertex n_P; an elided formula in
   the parity comment). Fixed, rebuilt, and all three suites re-run — the GREEN numbers above are from that
   final binary.
4. **Checked that the mitre can only lengthen, never shorten**, so no existing fixture could silently lose
   depth (proof in "What I implemented"). This is also why the 0.3 mm / 1.2 mm plate cases needed no edits.

## Concerns

1. **The 4.0 mm² parity bound** — see above. The bound now sits at 5.38882 mm², derived from the two documented
   deviations rather than chosen; the controller may want to rule on zeroing the interlocking depth instead.
2. **Case B's second segment is clamped by the probe launched from the vertex** (`half[v]`), not re-probed from
   the ring point one wall stack in along n_P. The constraint Ruling 21 cared about — the depth along n_P — is
   now met *exactly* rather than approximately, and the new clamp is strictly tighter than the none this
   segment had before, but the residual approximation is the same species as Ruling 20's own (which probes
   from a point one wall stack in and snaps it back to the surface). No fixture in the suite exercises a
   non-zero case B second segment on a part thin along n(v): the thin-wall cases collapse it to zero.
3. **Case B at a vertical/vertical crease.** The mitre made visible that the code sends `|n_P·z| == |n_Q·z|`
   ties (a painted side face meeting another side face) down case B, so the ±Y edges of a painted side face
   get the wall-stack ring. Spec 3.6 words the two cases as "P more horizontal" / "P less horizontal" and does
   not say where a tie goes. This is Task 6 behaviour, not something this task changed, and it is the whole of
   the 1.611 mm² parity gap — flagged in case the controller wants a ruling.

---

# Fix report — Ruling 25 (crease-case ties take the mitred bisector)

Commit **`315ed3e071`** — `fix(color-split): a vertical/vertical crease is a tie, not case B (Ruling 25)`,
on top of the coordinator's spec rev 2.10 (`efc4b514e6`).

## What changed

`src/libslic3r/ColorSplitShell.cpp`, the convex-case selection, exactly as spec §3.6 rev 2.10 words it:

```cpp
constexpr float CREASE_TIE_EPS = 1e-3f;
const float p_z = std::abs(n_p.z()), q_z = std::abs(n_q.z());
if (std::abs(p_z - q_z) <= CREASE_TIE_EPS)
    continue;                       // tie: no step recorded -> the plain mitred-bisector branch
CreaseStep step;
step.case_a = p_z > q_z;            // strict by construction after the tie test
```

Falling through `continue` lands the vertex in `side_offsets`'s `step == topo.step.end()` branch, which is
already the rule the ruling asks for: ring `a − (h/cosang)·n(a)`, bottom `a − (d/cosang)·n(a)`, both clamped
by the half-thickness along n(a).

## New test (RED → GREEN)

The existing suite has **no tie vertex at all** — I checked every fixture — so the rule would have gone in
inert and unprotected. Added `make_ringed_box(x, y, z)` (a box whose four side faces are split by a ring of
vertices at half height; 12 vertices, 20 triangles, open-edge-checked) and
`colorsplit: a vertical crease between two side faces takes the plain bisector, not case B`. Its +X patch has
six boundary vertices of two kinds and pins all eighteen shell vertices:

| Vertex | n_Q | Rule | Ring | Bottom |
|---|---|---|---|---|
| (40, 0, 0) and the other three corners | bottom/top face + a ±Y face, \|n_Q·z\| = 0.707 | **case B**, unchanged | (39.13, 0, 0) | (38.5, 0.63, 0.63) |
| (40, 0, 10), (40, 40, 10) | the ±Y face alone, \|n_Q·z\| = 0 = \|n_P·z\| | **tie → plain mitred bisector** | (39.8, 0.2, 10) | **(38.5, 1.5, 10)** |

n(v) at the tie is (1, −1, 0)/√2, so the mitre spends 1.5·√2 and lands 1.5 in from *both* faces — the 45°
Voronoi diagonal, exactly. **RED** (before the fix, same binary layout, 18 vertices either way):
`test_color_split.cpp(750): FAILED: REQUIRE( std::count_if(...) == 1 ) with expansion: 0 == 1` — the tie was
case B at (39.13, 0, 10) → (38.5, 0.63, 10). **GREEN** after.

## Test results

```
[colorsplit]        All tests passed (912 assertions in 56 test cases)
[colorsplit_spike]  All tests passed (24 assertions in 3 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

Boss and 1.0 mm-wall cases unaffected, as the ruling predicted, and so is everything else: **S1 bit for bit**
(side tube 9.37426 mm³, top slab 4.43006, piece 9.37566 = 99.48 % of exposed) and **S4 bit for bit** (body
47.6398 / 124.991 mm²). S3 shell triangles unchanged at 69 520; its timings are run-to-run noise (one colour
0.986-1.020 s, three colours 6.56-9.64 s over three runs). Recorded in `spike-report.md`.

## The parity re-derivation still does not follow — measured, not argued

**The +X-face 3D per-layer area is unchanged at 55.9797 mm², and the 4.0 mm² bound still cannot pass** (worst
diff 5.33886, against the derived 5.38882 that is committed). The rule is right; it simply does not reach this
fixture:

> A plain cube's +X face has **no vertices but its four corners**, and each corner carries a **horizontal**
> boundary edge (against the top or the bottom face) as well as a vertical one. Its mean n_Q is
> `normalize((0,0,−1) + (0,−1,0))`, so |n_Q·z| = 0.707 against the patch's 0 — a strict case B, not a tie.
> A vertex has one ring copy, so the ws ring those four corners earn *at the caps* is inherited by the whole
> ±Y edge between them, and the middle-layer cross-section keeps `40·D − (D − ws)²`.

This is not a choice I made: making those corners plain would delete the ring at x = 40 − ws, z = 20 that
`colorsplit: painted side face keeps its full wall stack up to the top edge` exists to pin — i.e. the
coordinator's parity target and that case are **mutually exclusive on a four-vertex face**, and case B is
right there (without it the body's outer wall shows on the painted face for the top ws). The spec's own
rationale line — "Holding a wall stack there (Case B) would claim ws·(2D − ws) per layer more than the 2D
path" — is correct about a *pure* vertical/vertical crease; the cube's ±Y edges are not one, because their
only vertices are shared with the caps.

What Ruling 25 *does* buy is visible on the ringed box: the moment a ±Y edge has a vertex of its own, that
vertex lands on the 2D diagonal and the gap closes there. On a real model with any tessellation along its
vertical edges the parity gap therefore shrinks towards zero, leaving only ws-wide bands at the caps.

**Options for the controller** (I have applied none of them unilaterally):
1. **Leave it.** The committed bound is 5.38882 mm², derived from the two documented deviations, with the 3D
   area pinned analytically to 0.05 mm². Nothing is hidden.
2. **Zero `mmu_segmented_region_interlocking_depth` in `e2e_config()`** — the 3.728 mm² notch is a 2D-only
   artefact; the bound then becomes 1.611 + slack and 4.0 passes with room. One line, Task 8 deliberately kept
   the default so this is a controller call.
3. **Slice the parity fixture from `make_ringed_box(40, 40, 20)`** (or a finer split) instead of `make_cube`,
   so the compared layers sit near a tie rather than between two cap-bound corners. This measures the rule
   rather than the four-vertex artefact, but it changes what the case is testing and the layers between the
   ring and the caps would still interpolate, so the bound would need re-deriving per layer band.

## Self-review

- The tie branch reuses the existing plain-boundary path rather than adding a fourth case — no new geometry
  code, and the mitre/clamp behaviour is automatically the same one Task 9 pinned.
- `p_z`/`q_z` are computed once and reused for both the tie test and `case_a`, so the two cannot disagree.
- Checked every existing fixture for tie vertices before assuming "unaffected": none has one (the boss's
  creases are side/top; the 1 mm wall's and the cube's corners each carry a cap edge). The full-suite GREEN
  and the bit-identical S1/S4 numbers confirm it.
- The parity comment previously described the ±Y edges as `|n_P·z| = |n_Q·z| = 0` creases — true of the edges,
  false of the only vertices they have. Corrected in the same commit, with the ringed-box case named as the
  place the tie rule is actually exercised.

---

# Fix report 2 — Ruling 26 (parity comparison zeroes the 2D interlocking notch)

Commit **`8fbeb4fbe0`** — `test(color-split): zero the 2D interlocking notch in the parity comparison
(Ruling 26)`, on top of the coordinator's spec rev 2.11 (`51db134a3e`). Test-only; no production code touched.

## What changed (all in `tests/libslic3r/test_color_split.cpp`)

**(a)** `e2e_config()` sets `mmu_segmented_region_interlocking_depth = 0`, with the comment the ruling asks
for: the notch is an artefact of segmenting one layer at a time, which the 3D split has no counterpart for —
its pieces are solids and the slicer perimeters each of them once — and at the 0.1 default it alternated the
2D claim by `i·(40 − 2D + i) = 3.73 mm²` and swamped the geometric difference the case exists to measure.

**(b)** The bound is back to a literal **4.0 mm²**, and the `i_lock`/`notch` derivation is gone with it (dead
once the config zeroes it). The 3D area keeps its separate analytic pin, `WithinAbs(40·D − (D − ws)², 0.05)`.
The derivation comment now names the residual explicitly as the **per-vertex case B corner hold**,
`ws·(2D − ws) = 1.61 mm²/layer`, a designed deviation bounded by one wall stack along the vertical creases
(the piece is never more than ws wider than the 2D band, and only within D of the face), and keeps the
Ruling 25 paragraph explaining why a four-vertex face has no tie vertex.

**(c)** The per-layer `WARN` stays, with `notch` replaced by `case B corner hold`; `one_line` is still read
from the config and reported. The loop still records the odd and even 2D areas separately — with the notch
zeroed they must now come out equal, which is the check that the zeroing took effect.

## Measured

```
parity over layers 25..74: 2D odd 54.3691, 2D even 54.3691, 3D 55.9797 mm^2;
worst diff 1.61063 of 4 (2D claim 54.3691, 3D claim 55.9797,
case B corner hold 1.61059; D 1.40885, ws 0.79708, one outer wall line 16.8)
```

Odd and even agree exactly. The worst difference is **1.61063 mm², 40 % of the 4.0 budget**, and it matches
the derived corner hold `ws·(2D − ws) = 1.61059` to within 4·10⁻⁵ — i.e. the entire measured 2D/3D difference
is now accounted for analytically, with nothing left over.

## Test results

```
[colorsplit]        All tests passed (912 assertions in 56 test cases)
[colorsplit_spike]  All tests passed (24 assertions in 3 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

S1/S3/S4 unaffected and re-verified (boss piece 9.37566 mm³ / 99.48 %; wedge body 47.6398 off, 124.991 on) —
expected, since the change is test-config only and S4b slices split objects, which carry no paint for the 2D
segmentation to touch. `spike-report.md` updated with the final parity table.

## Self-review

- Checked the zeroed key reaches only what it should: `e2e_config()` feeds the parity case (2D arm affected,
  3D arm unaffected — it slices solids) and S4 (both arms already unpainted after the split). S4's numbers
  came back bit-identical, which confirms it.
- No dead code left behind: `i_lock`/`notch` removed rather than left computed-but-unused; `one_line` is still
  used in the INFO and WARN.
- The odd/even split in the WARN is now a live check rather than decoration — if the zeroing were ever undone
  the two numbers would diverge and the bound would fail at 5.34.
- No concern remains open on this case: the residual is fully derived and 60 % of budget clear.
