# Taper bound — scoped review of `65d17c964f`

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `65d17c964f`.
Reviewed against `taper-bound-report.md`, `vertical-depth-investigation.md`,
`shell-coverage-investigation.md`, `vertical-depth-fixwave-rereview.md`, `n1-fixwave-report.md`.
Read-only; no edits made.

## Verdict: **FIX FIRST**

The mechanism is sound, the steep-surface protection is genuinely preserved, and the
change is a strict widening with no over-claim and no depth-bound movement. But the
report's central safety claim — *"the base material left at that layer's perimeter is
either nothing at all or at least one wall stack wide — never a sliver"* — is **false on
the most common painted-top geometry**, and nothing tests that geometry. Two Important
findings; both fixes are small and provably inert against the existing suite.

## Check summary

| # | Check | Result |
|---|---|---|
| 1 | Guard correctness (geometry / union validity / slope criterion) | **PASS with caveat** — geometry, reference layers and units are all correct; the *stated invariant* is not what the code enforces (I1) |
| 2 | Anti-smear fixture arithmetic | **PASS** — re-derived independently; every number checks out with real margin |
| 3 | Retargeted N1 test | **PASS** — the pin is carried and slightly strengthened; no silent coverage loss |
| 4 | Concern (1) exterior-wall contact | **PARTIAL FAIL** — safe on vertical walls, reintroduces the sliver class on tapered tops (I1) |
| 5 | Concern (2) curved surfaces | **CONFIRMED GAP** — the lateral clamp does not close it between ~6.5° and ~27° |
| 6 | Symmetry + residual | **PASS** — bottom gate mirrors the top exactly; no new asymmetry |
| 7 | Re-run gates | **PASS** — 259/29, 605/133, 17/17 all reproduced |

---

## Findings

### IMPORTANT 1 — `exposed_surface_part()`'s early-return bypasses the clearance test, so the "never a sliver" invariant does not hold on tapered tops

`src/libslic3r/MultiMaterialSegmentation.cpp:1331-1333`

```cpp
if (reference_layer_idx >= num_layers || input_expolygons[reference_layer_idx].empty() || wall_stack_width <= 0.f)
    return projected_patch;
return diff_ex(projected_patch, offset_ex(input_expolygons[reference_layer_idx], wall_stack_width));
```

**The report's proof (§2, "Why this preserves the invariant, not just a proxy for it")
conflates two different `run` values.** Its argument is: the gap from the claim to
`∂input_expolygons[last_idx]` is `(L − last_idx)·run ≥ run`, and `top_exposed_ex` is
non-empty only where `run ≥ w+s`, therefore the gap is ≥ one wall stack. But the `run`
in the *test* is measured against the layer **above** the painted face, while the `run`
that governs the *gap* is the object's silhouette taper **below** it. For a locally
monotone taper through the painted layer those coincide and the argument holds. On the
early-return path they are unrelated: the whole patch is returned with **no geometric
test at all**, and the taper below is unconstrained.

The early-return fires on exactly the case the user cares about most: **`layer_idx + 1 >=
num_layers`, i.e. every painted flat face that is the top of the object.**

**Failure scenario (concrete).** Box, flat 40×40 top at z=20, 45° chamfer down to 41×41
at z=19.5, vertical walls below. Whole flat top painted. 0.1 mm layers, 0.45 mm outer
wall ⇒ `w+s = 0.8785 mm`, 6-layer effective shell.

* `top_ex` = the 40×40 cap (half-width 20.00). `exposed_surface_part` early-returns it whole.
* `layer_slices_trimmed` = `outline(L)` (half-width 20.05, the smallest of the run).
* Claim on every shell layer = half-width 20.00.
* Layer outlines below: L−1 → 20.15, L−2 → 20.25, … L−5 → 20.55.
* **Base annulus left at the contour: 0.15, 0.25, 0.35, 0.45, 0.55 mm — every one of them
  below one wall stack (0.8785 mm), and three of them below one external perimeter (0.45 mm).**

Pre-change the same layers left 0.98–4.4 mm of base — a clean multi-wall band. The general
form is `gap(k) ≈ (k + ½)·r` where `r` is the silhouette's horizontal run per layer, so
`gap(1) < w+s` for any wall steeper than **≈10° from horizontal** at 0.1 mm layers. That
is essentially every chamfer, fillet, draft angle or organic taper under a painted top face.

**Why it matters at the perimeter generator** (the specific question asked):
`Layer::make_perimeters()` (`src/libslic3r/Layer.cpp:220-299`) groups only
`is_perimeter_compatible` regions before generating loops over their merged slices, and
`is_perimeter_compatible` (`Layer.cpp:184`) requires `wall_filament ==
other_config.wall_filament`. The MM-painted region is created precisely by overriding
`wall_filament`, so **the base region and the painted region are never grouped** — the
thin base annulus is handed to Arachne as its own `slices` set. At 0.15–0.35 mm it lands
under `min_bead_width`: Arachne either widens it (bulge) or drops it (groove where the
painted region's own external perimeter is inset behind the true silhouette). That is
#7104/#7235 exactly.

There is **no downstream filter that would absorb it**. The report cites
`merge_segmented_layers`' `offset2_ex(±SCALED_EPSILON)` (`MultiMaterialSegmentation.cpp:2150`)
as bounding the residual risk — `SCALED_EPSILON` is **0.0001 mm**. The other cleanups in
`apply_mm_segmentation` (`PrintObjectSlice.cpp:4585, 5033, 5151`) are `opening(5·EPSILON)`
= **0.0005 mm**. Neither touches a 0.15–0.55 mm ring. That bounding argument should be
struck from the report.

The report's *other* bounding argument — "the surface layer already claims at full width
with zero clearance today" — is true and does soften this: the class already exists one
layer up. But at the surface layer the annulus is `½·r` ≈ 0.05 mm (sub-`min_feature_size`,
silently dropped); the change moves it into the 0.15–0.55 mm window where it *does* print.

**Fix (enforces the invariant the report already claims, ~6 lines, inside the existing
`if (! top_exposed_ex.empty())` block in both loops):**

```cpp
append(last, intersection_ex(top_exposed_ex, layer_slices_trimmed));
last = union_ex(last);
// Enforce the stated invariant: base material left at this layer's contour is either
// nothing or at least one wall stack wide. Absorb anything thinner into the claim.
const float wall_stack = stat.extrusion_spacing + stat.extrusion_width;
ExPolygons base_rest = diff_ex(input_expolygons[last_idx], last);
if (! base_rest.empty()) {
    append(last, diff_ex(base_rest, opening_ex(base_rest, 0.5f * wall_stack)));
    last = union_ex(last);
}
```

This is inert on every existing test: the anti-smear frustum never enters the block
(`top_exposed_ex` is empty); the wide-slab and small-prism fixtures leave `base_rest`
empty. It only fires on the tapered case, and its smear is bounded at one wall stack —
far less than rejected alternative 5, which snapped out unconditionally on steep surfaces.

**Also needed: a test for this geometry.** `make_square_frustum(40., 22., 6.)` already
exists; paint its **top cap** (facets `{2,3}`) instead of the walls and assert that the
base remainder at each shell layer's contour is either empty or ≥ `w+s`. Currently the
only "paint reaches the silhouette" fixture is the 40×40 slab, whose vertical walls make
the gap exactly zero — the safe case, and the only one covered.

---

### IMPORTANT 2 — the anti-smear test's positive probe is satisfied by the lateral band alone, so the guard's proof can go green for the wrong reason

`tests/libslic3r/test_paint_depth_clamp.cpp` (anti-smear guard case, the two-sided probe pair)

```cpp
CHECK(any_contains(extruder2_claim_for_layer(*out_object, probe_layer),
                   layer_edge_probe(*out_object, probe_layer, 0.2)));
const Point probe = layer_edge_probe(*out_object, probe_layer, 1.0);
CHECK_FALSE(any_contains(extruder2_claim_for_layer(*out_object, probe_layer), probe));
```

The report claims: *"a pass can never come from 'the fixture did not slice' or 'the paint
vanished'."* True for total absence — but the positive probe sits at **0.2 mm** in, and
the config is `pdmWalls, 1`, whose Stage-1 lateral band is
`paint_depth_band_mm(pdmWalls, 1, …) = ext_perimeter_width = 0.45 mm`
(`src/libslic3r/PaintDepth.cpp:15-19`), applied by `cut_segmented_layers`
(`MultiMaterialSegmentation.cpp:1175`) as the annulus `[0, 0.45 mm]` in from the contour.
The layer's own vertical surface band is *also* `[0, 0.45 mm]`. **The two coincide
exactly at the probe.**

**Failure scenario.** Any regression that kills the vertical top/bottom projection path
while leaving Stage-1 intact — `slice_mesh_slabs` re-classifying the sloped facets as
`Vertical`, the `max_top_layers > 0` gate closing, `top_raw` being filtered away, or the
`stat.top_shell_layers > 0` gate mis-firing — leaves `CHECK` green via the lateral band
and `CHECK_FALSE` trivially green. The test that is *the* proof of the guard would then
pass while proving nothing. This is precisely the "passes for the wrong reason" case.

**Fix.** Separate the two bands so the positive probe can only be satisfied by the
vertical path. Switch the fixture to `paint_depth_mode = pdmMillimeters` with
`paint_depth_mm = 0.15` (lateral band `[0, 0.15 mm]`) and move the positive probe to
0.30 mm in — inside the 0.45 mm surface band, outside the lateral band. The 1.0 mm
negative probe is unaffected, and `interlocking_cut_width = max(0.15 − 0.3, 0) = 0` so
`region_cut_width` stays 0.15 mm on both layer parities (`MultiMaterialSegmentation.cpp:1164,
1169`) — no even/odd surprise.

---

### MINOR 1 — the report's 0.2 mm-layer threshold figure is slightly off

`taper-bound-report.md` §6 concern 2 gives `atan(layer_height / (wall + spacing))` as
"≈ 12.8° at 0.2 mm". Recomputing: `spacing = 0.45 − 0.2·(1 − π/4) = 0.40708`,
`w+s = 0.85708`, `atan(0.2/0.85708) = 13.13°`. The 0.1 mm figure (6.49° ⇒ "≈ 6.5°") is correct.

### MINOR 2 — all three early-return conditions fail *open*

`MultiMaterialSegmentation.cpp:1331`. `wall_stack_width <= 0.f` and
`input_expolygons[ref].empty()` both return the **whole** patch, i.e. maximum claim. For
the degenerate-stats case this matches legacy (`offset` also stays 0), so it is not a new
bug, and the anti-smear test pins it. But the three conditions have quite different
semantics — "nothing above" (genuinely exposed) vs "we could not evaluate the test" — and
the comment presents all three as the former. Worth splitting the comment, or returning
`{}` on the `wall_stack_width <= 0.f` branch so a stats regression fails closed.

### MINOR 3 — "a flat cap keeps its exact footprint" is not unconditional

The criterion is Euclidean distance to the *nearest* part of `input_expolygons[ref]`, not
to the part up-slope. A flat painted face with any vertical feature rising off it within
`w+s` (a boss, a pin, a raised letter) loses full width in that neighbourhood. The result
is conservative (narrower, therefore safe), but the doc table's "flat top face ⇒ **whole
patch**" row and the commit message's "A flat cap keeps its exact footprint" are both
stronger than the code.

### MINOR 4 — commit message references an uncommitted path

`65d17c964f` cites `.superpowers/sdd/2026-08-31-paint-depth/taper-bound-report.md`, which
is untracked (`git status`: `??`). Consistent with the previous waves' convention, but the
referenced artifact is not in the repo for anyone reading the history later.

---

## Check 1 — the guard's correctness (detail)

**(a) Geometry.** Correct.
* Reference layers: top uses `layer_idx + 1`, bottom uses `layer_idx - 1` — the same
  neighbours as the occlusion trim at `:1500` / `:1503`. **No off-by-one.**
* `layer_idx - 1` at `layer_idx == 0` wraps to `SIZE_MAX` on `size_t`; well-defined
  unsigned wraparound, caught by `reference_layer_idx >= num_layers`. Correct, not UB.
* Order of operations: `diff_ex(patch, offset_ex(outline(ref), +w+s))` — dilate the
  reference outline, then subtract. That is the right order for "more than `w+s` clear of".
* Units: `stat.extrusion_width` / `extrusion_spacing` are scaled at `:1601-1602`; both
  `offset_ex` calls consume scaled deltas. Consistent with the loop's own
  `offset_ex(layer_slices_trimmed, offset)`.
* `top_ex` is already occlusion-trimmed against `outline(L+1)`, so the helper removes a
  *further* `w+s` band rather than double-counting.
* `top_exposed_ex ⊆ top_ex` and the full-width term is `∩ layer_slices_trimmed`, so the
  claim can never exceed the painted patch or escape the containment guard. **No XY
  over-claim.** The loop bound is untouched, so **no depth over-claim** — pinned by the
  wide-face test's depth-6 assertions at both centre and edge.

**(b) Union validity.** Safe. `append(last, …); last = union_ex(last);` — the two terms
genuinely overlap (neither contains the other in general), and `union_ex(ExPolygons)` is
the standard Slic3r overlap-resolving union, the same idiom used at `:1726` and `:1747` in
this very function. `opening_ex` then runs on already-valid input. The `break`-on-empty
path is preserved: if `layer_slices_trimmed` degenerates, both terms go empty together.

**(c) The slope criterion.** The code tests Euclidean clearance from
`input_expolygons[reference_layer_idx]`, which for a locally straight, locally monotone
slope is exactly the horizontal run `layer_height / tan(slope)` — so
`layer_height/tan(slope) ≥ wall + spacing` **is** what is tested, not an approximation,
for that model. It is not what is tested on the early-return path (I1) or near unrelated
geometry within `w+s` (Minor 3).

## Check 2 — anti-smear arithmetic, re-derived independently

Confirmed. `Layer::slice_z = 0.5·(lo+hi)` (`PrintObjectSlice.cpp:57`) and
`zs_from_layers` returns `slice_z` (`Layer.hpp:343-350`), so layer *i* slices at
`0.3i + 0.15`.

| quantity | value | note |
|---|---|---|
| frustum half-width | `20 − 1.5·z` | 40→22 mm over 6 mm |
| run per layer | `1.5 × 0.3 = 0.45 mm` | matches report |
| layer 10 contour | 15.275 mm | `slice_z = 3.15` |
| `small_region_threshold` | `0.5×0.45 × 0.5 = 0.1125 mm` radius ⇒ erases < 0.225 mm | band survives |
| one erosion step | `0.45 + (0.45 − 0.3·(1−π/4)) = 0.83562 mm` | band (0.45) well below ⇒ killed at step 1 |
| `top_shell_layers` | 4 ⇒ 3 descent steps | bands 11, 12, 13 reach layer 10 |
| band(11) / (12) / (13), measured in from 15.275 | `[0.45, 0.9]` / `[0.9, 1.35]` / `[1.35, 1.8]` | matches report |
| **1.0 mm probe** | inside band(12) | margins **0.10 mm** outward, **0.35 mm** inward |
| **0.2 mm probe** | inside band(10) `[0, 0.45]` | margins 0.20 / 0.25 mm |

Guard fires as claimed: `exposed_surface_part(band(13), …, ref=14, …)` — `outline(14)` is
half-width 13.475, dilated to 14.311, which fully contains band(13) `[13.475, 13.925]` ⇒
**empty** ⇒ loop body byte-identical to legacy ⇒ step 1 intersects with `outline(13)`
eroded to 13.089 ⇒ empty ⇒ `break`. Verified.

The numbers put the probes exactly where the report claims, with real margin. The
*negative* side is sound. The *positive* side's discriminating power is overstated — see
Important 2. I did not rebuild to re-confirm the RED capture or the erosion-deleted
scratch experiment; those rest on the report's evidence.

## Check 3 — the retargeted N1 test

**The pin is carried, and slightly strengthened. No silent coverage loss.**

N1's real content is that `layer_color_stat`'s `region->slices.empty()` guard
(`:1564`) must scope **only** the shell-depth max, never the per-colour extrusion-stat
block below it — because the auto-created painted region (the only one with
`wall_filament == color_idx`) has empty slices at this point in the pipeline. The old
test pinned it *indirectly*: zeroed stats ⇒ `offset` stays 0 ⇒ no taper ⇒ the 2 mm probe
on a flat cap is wrongly claimed.

Under the same regression, the retargeted test fails **two ways at once**:
1. `offset` stays 0, so the eroded term no longer kills the bands; and
2. `wall_stack_width == 0.f` hits `exposed_surface_part`'s third early-return, so
   `top_exposed_ex` becomes the whole band and the *full-width* term propagates it too.

Bands 11/12/13 reach layer 10 at full width, the 1.0 mm probe is claimed, `CHECK_FALSE`
fires. Identical failure mode, strictly more paths to it.

Neither the old nor the new test pins `num_regions` — `assert(out.num_regions > 0)`
(`:1600`) is compiled out in Release, so that is unchanged either way. The new test adds
a pin the old one lacked: if `small_region_threshold` grew, the 0.45 mm band would be
erased by `opening_ex` and the 0.2 mm `CHECK` would fail. `pdmUnlimited` coverage at
depth, which the old fixture carried, is preserved by the three other new cases (all via
`slice_capped_prism` / `slice_capped_slab`). The deleted assertion — "a flat cap tapers" —
is deliberately reversed behaviour, and its replacement pins the new contract including
the depth bound at both centre and edge.

## Check 4 — concern (1), exterior-wall contact: **verdict**

**Split verdict. Safe where paint covers the whole cross-section; not safe where the
object tapers below the painted flat top.**

* **Flat top + vertical walls (plain box, the wide-face test's fixture).** The claim
  reaches the contour exactly; the base remainder is empty; the layer is single-coloured.
  This is the strictly *safest* outcome and it is the report's "nothing at all" case.
  The visible change — the top `top_shell_layers` of the side wall now print in the
  painted colour — is real, intended by the user ruling, and carries **no** sliver risk.
* **Flat top + any chamfer / fillet / draft / organic taper below it.** The guard **does**
  trade the deep-claim bug for the bug it was protecting against, on up to
  `top_shell_layers` layers. Base annulus `≈ (k+½)·r`; sub-wall-stack for any wall steeper
  than ~10° from horizontal at 0.1 mm layers; 0.15–0.55 mm for a 45° chamfer. See
  Important 1 for the mechanism through `Layer::make_perimeters` and the fix.

**What to look for in the GUI:** slice a painted flat-topped object **with a chamfered or
filleted top edge** (not a plain cube — a plain cube cannot show this). Inspect the
exterior wall on the 2nd–6th layers below the top. Expect either a hairline base-coloured
ring or a slight groove/bulge where the painted region's external perimeter sits behind
the true silhouette. A plain cube will look correct and proves nothing about this.

## Check 5 — concern (2), curved surfaces: **verdict with numbers**

**The lateral clamp does NOT supply the missing opacity. A real gap remains, and it is
worst exactly where "dark features on a curved face" live.**

Lateral band, `pdmWalls` / walls = 3 (`PaintDepth.cpp:15-19`):
`b = ext_perimeter_width + 2·perimeter_spacing = 0.45 + 2×0.42854 = **1.307 mm**`
(0.45 mm walls, 0.1 mm layers). `cut_segmented_layers` applies it as a band measured
horizontally in from the contour (`MultiMaterialSegmentation.cpp:1175`), so the painted
thickness **measured normal to a surface at slope θ** is `t = b·sin θ`.

| slope θ | lateral `t = b·sin θ` | vertical path | verdict |
|---|---|---|---|
| < 6.49° | 1.31·sinθ (small) | **full shell, 0.6 mm × cos θ ≈ 0.60 mm** | fixed by this change |
| 6.49° (threshold) | **0.148 mm** | 1 layer, 0.099 mm | **4.0× cliff** |
| 10° | **0.227 mm** | 1 layer | gap |
| 15° | **0.338 mm** | 1 layer | gap |
| 20° | **0.447 mm** | 1 layer | marginal |
| 25° | **0.552 mm** | 1 layer | marginal |
| ~27.3° | **0.600 mm** | 1 layer | **parity with the flat case regained** |
| 30° | **0.654 mm** | 1 layer | fine |

The single-layer vertical claim contributes `h·cos θ` ≈ 0.087–0.099 mm and is dominated by
the lateral band for every θ above 4.4° (`tan θ > h/b = 0.0765`), so the union's normal
thickness is `b·sin θ` throughout the 10–30° range — the lateral clamp adds nothing on top
of itself. Against a ~0.4–0.8 mm rule of thumb for hiding a base colour, **6.5°–~27° is a
genuine opacity gap**, worst at the shallow end.

Two things that are worth telling the user:
* **Thinner layers make this worse, not better.** The threshold is
  `atan(h/(w+s))` — dropping to 0.05 mm layers moves it to **3.2°**, so *fewer* slopes get
  the full-shell treatment. Counter-intuitive and actionable.
* **`paint_depth_walls` is the lever that works.** `t` scales linearly in `b`: walls = 6
  gives `b = 2.59 mm` ⇒ 0.45 mm at 10°, 0.60 mm at 13.4°. `paint_depth_mm` mode is the
  direct control (`t = mm·sin θ`; 3.5 mm reaches 0.60 mm at 10°). Below ~7° neither is
  practical — but below ~6.5° the vertical path already delivers the full shell.

**What to look for in the GUI:** on the user's actual model, measure the local slope where
the dark features sit. If those faces are shallower than ~27°, expect the base colour to
read through, and the remedy is raising `paint_depth_walls` / switching to `paint_depth_mm`
— not a further change to this commit. The report's framing ("the painted skin stays one
layer thick above ~6.5°") slightly *understates* what is there, because it omits the
lateral band; the corrected picture is above.

## Check 6 — symmetry and residual

**PASS.** The bottom surface-claim gate `stat.bottom_shell_layers > 0` (`:1683`) mirrors
the top gate at `:1633` exactly, with a RED-first test. Both descent loops call
`exposed_surface_part` with the correct mirrored reference layer, the same
`num_layers`, the same `wall_stack_width`, the same `∩ layer_slices_trimmed`,
`union_ex`, `opening_ex`, `break` sequence. The mirrored sliver risk (an object widening
*upward* above a painted bottom face) is identical in kind and is covered by Important 1's
fix applied to both loops. Effective depth is `bottom_shell_layers` total in both
directions (surface layer + `n−1` descent steps). No new asymmetry introduced.

Residual: `progress.md` remains modified in the working tree and uncommitted, as the
previous wave decided; `git show --stat 65d17c964f` confirms exactly the two source files.

## Check 7 — gates re-run by this reviewer

Binary `build/tests/libslic3r/Release/libslic3r_tests.exe`, timestamp 20:51:17, newer than
both edited sources (20:50:01 / 20:42:59) — the committed tree.

* `libslic3r_tests.exe "[paintdepth]"` → **All tests passed (259 assertions in 29 test cases)**, exit 0 ✔
* `libslic3r_tests.exe "[chameleon]"` → **All tests passed (605 assertions in 133 test cases)**, exit 0 ✔
* `spike/verify_paintdepth.sh` → **17/17 checks passed, RESULT: ALL PASS**, exit 0 ✔
  (including `unpainted-run1-vs-baseline` / `unpainted-run2-vs-baseline` byte-identical
  and `unpainted-determinism`)

All three reproduce the report's claimed numbers exactly.
