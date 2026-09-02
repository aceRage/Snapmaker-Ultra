# Taper bound — implementation report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`. Base HEAD `3448111acd`
("fix(paint-depth): scope I2's empty-slices guard to the shell-depth max only (N1), gate the
zero-shell surface claim (N2), add bottom-direction C1 coverage (N3)").

**User decision implemented (binding):** *"Painted top/bottom claims keep FULL WIDTH for the
solid-shell depth; taper only below that."* The claim depth IS the shell depth since the
vertical-depth fix wave, so in practice: no width loss anywhere inside the claim.

**Status: done. All gates green.** The erosion was NOT deleted — it was split, and the split is
proved by a test that regresses if the erosion is deleted.

---

## 1. The VERIFIED purpose of the erosion

Both descent loops in `segmentation_top_and_bottom_layers`
(`src/libslic3r/MultiMaterialSegmentation.cpp`) do:

```cpp
offset -= (stat.extrusion_spacing + stat.extrusion_width);
layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);
ExPolygons last = intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset));
if (last.empty()) break;
```

Three separable mechanisms are bundled here, and only the middle one is "the taper":

1. **`layer_slices_trimmed`** — the running intersection of the layer outlines from the surface
   layer down to `last_idx`. A pure **containment** guard: a propagated claim can never land where
   the object does not have material at every intervening layer. Untouched by this change.
2. **`offset_ex(..., -k·(w+s))`** — shrinks the **layer outline** (never the painted patch) by one
   wall stack per layer of descent. **This is the erosion.**
3. **`opening_ex(..., small_region_threshold)`** + the `break` — the #7104 thin-projection filter
   and the descent terminator.

### Verified purpose of (2): a perimeter-safety margin on *inferred* claims

The surface-layer claim is appended with **`offset == 0`** — no margin at all. It is what the user
actually painted. Every layer below it is an *inference* ("the shell under this painted face should
be painted too"), and BBS keeps inferred claims at least one wall stack clear of that layer's own
contour. Evidence, in order of directness:

* **The code's own comment**, immediately above the `offset -=` line in both loops:
  `//BBS: offset width should be 2*spacing to avoid too narrow area which has overlap of wall line`.
  "Too narrow area which has overlap of wall line" is exactly a base-coloured strip thinner than a
  perimeter, sitting where a perimeter loop must print — the loop gets split into a painted arc and
  a base arc.
* **Upstream provenance.** PrusaSlicer's version of this loop
  (`raw.githubusercontent.com/prusa3d/PrusaSlicer/master/src/libslic3r/MultiMaterialSegmentation.cpp`,
  fetched this session) has `offset -= stat.extrusion_width;` — one external-perimeter width per
  descent step, same construct, same target (`offset_ex(layer_slices_trimmed, offset)`). BBS
  widened it to `spacing + width`, i.e. a two-wall stack, and left the original line commented out
  directly above (`//offset -= stat.extrusion_width ;`). So this is a deliberate, twice-tuned
  perimeter clearance, not incidental.
* **The defect class it belongs to.** The same function's `filter_out_small_polygons` cites
  PrusaSlicer **#7104** ("polygons less than 0.1mm², because they are unprintable and causing
  dimples on outer primers") and `merge_segmented_layers` cites **#7235** — fetched this session and
  titled *"MMU Painting still creating dimples in exterior perimeter"*, where the reporter notes
  dimples appear only with MMU painting on, and are worst on irregular geometry. Both are the
  *same* failure: MM colour boundaries landing inside the exterior perimeter band.

### The consequence that makes it load-bearing (the anti-smear property)

`TriangleMeshSlicer.cpp` classifies slab facets by the **sign** of the XY-projected cross product,
so any facet with a non-zero normal-Z is Up or Down — there is no slope threshold anywhere in this
path. A **steep** painted surface therefore does project, but its projection at each layer is, after
the occlusion trim (`diff(top_raw[l], input_expolygons[l+1])`), exactly the annulus between that
layer's outline and the next one up: a staircase band of width `layer_height / tan(slope)` that by
construction **hugs the layer's own contour**.

Propagating such a band at full width would:
* leave a base strip only `layer_height / tan(slope)` wide at the perimeter of every layer below —
  narrower than one wall, i.e. precisely the #7104/#7235 sliver, and
* smear the painted colour `top_shell_layers` deep down the whole wall, painting the *exterior*
  perimeter of layers the user never painted.

Today's erosion annihilates that band at the **first** descent step (the band lies within
`run < w+s` of the contour) and the `break` then ends the descent. **This is the protection that
had to survive, and it does.**

### What the erosion is *not* for

It is **not** what keeps an interior painted island whole — an island in the middle of a wide face
is far from the outline and was never touched by it. And it is **not** a depth bound; the loop bound
is. Its entire effect on the user's case is a rim penalty of `k·(w+s)` on layer `L-k`.

---

## 2. The bound, and why this mechanism

New helper `exposed_surface_part()` (`MultiMaterialSegmentation.cpp`, immediately above
`segmentation_top_and_bottom_layers`):

```cpp
if (reference_layer_idx >= num_layers || input_expolygons[reference_layer_idx].empty() || wall_stack_width <= 0.f)
    return projected_patch;
return diff_ex(projected_patch, offset_ex(input_expolygons[reference_layer_idx], wall_stack_width));
```

`reference_layer_idx` is `layer_idx + 1` for top claims and `layer_idx - 1` for bottom claims — the
same neighbours the occlusion trim already uses. (`layer_idx - 1` wraps to `SIZE_MAX` at layer 0,
which the first clause catches: nothing below layer 0 means the bottom face is fully exposed.)

Each descent step then becomes **legacy eroded term ∪ full-width term**:

```cpp
ExPolygons last = intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset));   // unchanged
if (! top_exposed_ex.empty()) {
    append(last, intersection_ex(top_exposed_ex, layer_slices_trimmed));             // FULL WIDTH
    last = union_ex(last);
}
last = opening_ex(last, stat.small_region_threshold);                                // unchanged
```

It is a pure widening: the result is never smaller than before, and when `top_exposed_ex` is empty
the loop body is byte-identical to the previous code.

### Why the "distance from the object above" test is the right gate

The dangerous case and the user's case are separated by exactly one quantity: how far the projected
patch extends away from the object sitting above it. This is **surface orientation expressed in the
projection domain**, and it is exact rather than a proxy — the XY extent of a slab projection *is*
the facet's horizontal run over one layer. Written as a slope, the criterion is

> `layer_height / tan(slope) >= wall + spacing`

i.e. the same wall-stack yardstick the erosion itself uses, taken from quantities already in hand,
adapting automatically to layer height and extrusion width, with **no invented angle constant**.

Behaviour by case:

| painted surface | `exposed_surface_part` | descent |
|---|---|---|
| flat top/bottom cap (prism, the user's 8 mm feature), interior island | layer above empty / far ⇒ **whole patch** | **full width, every shell layer** |
| steep / near-vertical wall (band within one wall stack of the layer above) | **empty** | **byte-identical to before** |
| organic patch, flat in the middle, rolling over to steep at the rim | split **pointwise** | flat part full width, rim eroded |

The last row is the actual user scenario (a face/bust), and it needs no tuning.

### Why this preserves the invariant, not just a proxy for it

Wherever the full-width term contributes, the base material left at that layer's perimeter is
either **nothing at all** or **at least one wall stack wide** — never a sliver:

* If the claim covers the layer's cross-section out to its contour, there is no base strip to be
  slivered (the strictly *safest* outcome — the layer is single-coloured).
* Otherwise the claim is bounded by its own interior edge, or by `layer_slices_trimmed`. In the
  latter case the gap to `∂input_expolygons[last_idx]` is `(L − last_idx)·run ≥ run`, and
  `top_exposed_ex` is non-empty at that location only where `run ≥ w+s`. So the gap is ≥ one wall
  stack.

Honest caveat: that is a geometric argument over a locally-monotone taper model, not a proof for
arbitrary meshes. Two things bound the residual risk. First, the **surface layer already** claims at
full width with zero clearance today, so full width on sub-surface layers introduces no geometry
*class* the pipeline does not already emit one layer higher. Second, `merge_segmented_layers`' own
`offset2_ex(±SCALED_EPSILON)` dimple clean-up (the #7235 fix) still runs downstream, unchanged.

### Alternatives considered and rejected

1. **Delete the erosion.** Rejected — and *disproved empirically*: with both `offset -=` terms
   neutered in a scratch build, the anti-smear test fails while all four of the user's-bug tests
   pass (§3). It revives the sliver class exactly as predicted.
2. **Constant (non-accumulating) one-wall-stack erosion.** Preserves the perimeter margin at a
   fixed cost, but still removes ~0.88 mm of footprint on *every* sub-surface layer, so
   base-coloured solid still sits under the painted skin at the rim. Violates the user ruling.
3. **Split the painted facets by mesh normal-Z before `slice_mesh_slabs`.** The most literal reading
   of "surface orientation". Rejected on three counts: (a) it needs an arbitrary angle constant with
   no anchor in the config, where the projection-domain test *derives* the equivalent threshold from
   the wall stack the loop already computes; (b) it doubles `slice_mesh_slabs` work per painted
   volume per colour, plus a second occlusion-trim and small-polygon pass; (c) it classifies a facet
   once for every layer it spans, whereas the danger is per-layer — it depends on layer height.
4. **"Erode only past the shell depth."** The descent loop's bound *is* the shell depth, so this is
   identical to (1).
5. **Snap the claim out to the contour wherever the residual base strip is sub-wall-width.** Removes
   the sliver without losing footprint, but on steep/curved surfaces it paints the *exterior* wall of
   layers the user did not paint — visible downward colour smear. Strictly worse than the taper.

### Symmetry fix (required engineering item 3)

The bottom surface-layer claim is now gated on `stat.bottom_shell_layers > 0`, mirroring the top
gate the previous wave added (N2) and deliberately left asymmetric. Reasoning is identical:
`bottom_raw` having geometry only proves *some* region on the object has a non-zero bottom shell
(the object-wide `max_bottom_layers` gate), not that the region(s) present on **this** layer do;
`LayerRegion.cpp:1025-1036` demotes `stBottom` to `stInternal`/`stInternalVoid` at a zero count
exactly as it demotes `stTop`, so there is no solid skin for the colour to land on. The bottom
descent loop was already a no-op at zero. RED test added, see §3.

---

## 3. Tests (TDD, real RED)

All in `tests/libslic3r/test_paint_depth_clamp.cpp`, tag `[paintdepth]`, reusing the existing
harness (`paint_depth_test_config`, `extruder2_claim_for_layer`, `any_contains`,
`slab_center_point`). New fixtures are additive: `slice_capped_slab()` and every pre-existing
fixture are byte-identical.

New helpers: `layer_edge_probe()` (a point N mm in from a layer's own +X silhouette — the erosion is
measured from the silhouette, so this is the only coordinate that means anything on a fixture whose
cross-section varies with height); `slice_capped_prism()` (the capped-slab fixture with the XY
footprint as a parameter); `make_square_frustum()` (a hand-wound 8-vertex/12-facet square frustum —
there is no `its_make_*` frustum helper, and `its_make_pyramid()`'s base facets are wound
normal-up).

**Captured RED** (new tests only, production unchanged, HEAD `3448111acd`):

```
test cases:  29 |  25 passed |  4 failed
assertions: 257 | 241 passed | 16 failed
```

Every failure landed on the exact predicted line and depth:

| test | RED | why |
|---|---|---|
| **small painted TOP feature keeps its full footprint at every solid-shell layer** — 8×8×4 mm prism, top cap painted, 0.1 mm layers, `top_shell_layers=4` / `top_shell_thickness=0.6` ⇒ 6-layer effective shell | 5 × edge probe (depths 1–5) + 1 × centre probe (depth 5) | erosion is 0.8785 mm/step on a 4 mm half-width, so a 0.5 mm-from-edge probe is lost from depth 1; at depth 5 the claim needs 8.785 mm of an 8 mm cross-section ⇒ empty ⇒ `break`, so even the centre is unpainted. **This is the user's bug.** Depth 0 asserts too and passes in both states, which is what makes the tight 0.5 mm probe trustworthy. |
| **small painted BOTTOM feature…** (mirror; `bottom_shell_layers=3` / `bottom_shell_thickness=0.6` ⇒ 6 layers 0–5) | 5 × edge probe + 1 × centre probe | identical erosion in the bottom descent |
| **wide painted top face claims full width within the shell and nothing past it** — 40×40 slab, same 6-layer shell | 3 × edge probe (depths 3–5) | 3 × 0.8785 = 2.64 mm passes the 2 mm-from-edge probe. Also pins depth 6 unclaimed at **centre and edge** — the no-over-claim guard, since removing a `break` source is exactly what could let a descent overrun. |
| **zero-shell region's painted BOTTOM is not claimed even when another region makes the object-wide gate nonzero** — two Z-stacked volumes, `lower` `bottom_shell_layers=0`/`thickness=0.6` with its bottom cap painted, `upper` stock | 1 × centre probe at layer 0 | the ungated bottom surface claim (required engineering item 3) |

All four go **GREEN** with the change.

### The anti-smear proof — and why it is genuinely discriminating

`a steep painted surface gains no deep full-width claim (anti-smear guard)`. A 40 → 22 mm square
frustum over 6 mm at 0.3 mm layers: the four painted walls run 9 mm horizontally over 6 mm of
height, so each layer's painted band is `0.3 × 9/6 = 0.45 mm` wide, measured in from that layer's own
contour. That width is deliberately placed **in the danger window**:

* comfortably **above** the `opening_ex` thin-projection filter (`small_region_threshold` =
  0.5 × 0.45 mm outer wall, halved ⇒ 0.1125 mm radius ⇒ anything narrower than 0.225 mm is erased
  before the descent even starts), so the band reaches the descent loop at all; and
* comfortably **below** one erosion step (`(0.45 − 0.3·(1−π/4)) + 0.45 ≈ 0.836 mm`), so the descent
  must kill it.

With the erosion (and with the guard) layer 10 carries only its own surface band ([0, ~0.45 mm] in
from its contour) plus the Stage-1 lateral band (`pdmWalls`/1 wall ⇒ ≤ ~0.45 mm, pinned by the
pre-existing whole-layer-short-circuit test). Without it, bands 11/12/13 propagate at full width and
layer 10 collects the annulus from 0.45 mm to 1.8 mm. **The probe sits 1.0 mm in** — outside the
surface and lateral bands by more than a full band width, squarely inside band(12)'s
[0.9, 1.35] mm slot.

The test is **two-sided**: it also asserts Extruder2 *is* present at 0.2 mm in from the same
contour, so a pass can never come from "the fixture did not slice" or "the paint vanished".

`pdmWalls` with one wall (not `pdmUnlimited` as elsewhere) because the painted walls do cross every
layer's contour: the Stage-1 lateral path would otherwise claim the whole layer through the
`has_layer_only_one_color` short-circuit. Clamping it to a one-wall band keeps it clear of the 1.0 mm
probe so the test reads the vertical projection alone.

**Empirical discrimination proof.** A scratch build with both `offset -= (stat.extrusion_spacing +
stat.extrusion_width)` lines replaced by `offset -= 0.f` — the naive "just delete the erosion" —
gives, with the rest of the change in place:

```
C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_paint_depth_clamp.cpp(1039): FAILED:
  CHECK_FALSE( any_contains(extruder2_claim_for_layer(*out_object, probe_layer), probe) )
with expansion:
  !true

test cases:  29 |  28 passed | 1 failed
assertions: 259 | 258 passed | 1 failed
```

**Exactly one test fails, and it is this one** — the four user's-bug tests still pass, and nothing
else in the suite notices. That is the proof: this test, and only this test, distinguishes "guard
preserved" from "erosion naively deleted". The scratch edit was reverted before the final build
(`grep -c "SCRATCH EXPERIMENT"` ⇒ 0; the two original lines restored verbatim).

### Retargeted test

The previous wave's N1 test ("the painted top claim's lateral inward taper survives at the deepest
claimed layer") probed a **flat** 40×40 cap 2 mm in from the silhouette at depth 5. That fixture can
no longer express the taper — a genuinely near-horizontal face is now claimed edge-to-edge through
the whole shell **by design**. It was replaced by the anti-smear test, which carries N1's pin
unchanged: if `extrusion_width`/`extrusion_spacing` ever regress to `0.f`, `offset` stays 0, the
steep bands propagate at full width, and the 1.0 mm probe is wrongly claimed — the identical failure
mode, now sited where the taper is actually supposed to bite. A comment at the old site records the
move. Net test count: 25 → 29 cases (−1 retargeted, +4 new, +1 case from the retarget being a new
TEST_CASE).

---

## 4. Self-review hand-walk

**8 mm painted circle on a flat top face, 0.1 mm layers, 6-layer effective shell.** Surface layer
`S` is the top layer, so `S+1 >= num_layers` ⇒ `exposed_surface_part` returns the whole circle.
`layer_slices_trimmed` is the (uniform) prism cross-section and contains the circle at every step,
so the full-width term is the whole circle at each of `S−1 … S−5`; `opening_ex` leaves an 8 mm disc
untouched; `last` is never empty so the descent never breaks early. Six layers, full circle. At
merge, `painted_exploys` on those layers is empty (no colour has a *surface* claim there), so
`diff_ex` does not erode the deep claim. Depth 6 is outside the loop bound ⇒ unclaimed. ✔

**Painted near-vertical wall.** Every point of the band lies within `run < w+s` of
`input_expolygons[L+1]`, so `diff_ex(band, offset_ex(outline(L+1), w+s))` is **empty** ⇒
`top_exposed_ex.empty()` ⇒ loop body byte-identical to before ⇒ first step's
`intersection_ex(band, offset_ex(outline, −0.836))` is empty ⇒ `break`. No new deep claim. ✔

**Regression sweep over the pre-existing suite.** All the side-face fixtures paint *exactly
vertical* facets, which `slice_mesh_slabs` classes as `Vertical` and never slabs, so the whole
top/bottom path is inert for them. The depth-bound tests (C1/I1/I2/I3, N2, N3) all assert at layers
outside or at the loop bound, which this change does not move. Confirmed by the run: 133 `[chameleon]`
cases and the full 457-case `libslic3r` suite unchanged.

---

## 5. Verification (final source == final binary)

Comment-only wording fixes were made after the first green, then **ALL_BUILD re-run** so every
number below is against the exact committed tree.

* `cmake --build . --config Release --target ALL_BUILD` (scratchpad `build_next_wt.bat`) →
  **exit 0**, `error` occurrences in the log: **0**.
* `libslic3r_tests.exe "[paintdepth]"` → **All tests passed (259 assertions in 29 test cases)**
  (baseline 173/25).
* `libslic3r_tests.exe "[chameleon]"` → **All tests passed (605 assertions in 133 test cases)** —
  identical to baseline.
* `libslic3r_tests.exe` (whole suite) → exit 0, **457 cases / 50 338 assertions**, the only two
  failures being the suite's pre-existing "failed as expected" pair.
* `spike/verify_paintdepth.sh` × 2 → **17/17 checks passed, RESULT: ALL PASS** both runs, including
  `unpainted-run1-vs-baseline` / `unpainted-run2-vs-baseline` **byte-identical (normalized)** and
  `unpainted-determinism`. The change lives entirely inside the MM-painted projection, so unpainted
  parity is structural as well as observed.

**Build-slot discipline:** `Get-Process cl,link,MSBuild` was run before every one of the five builds
(RED, GREEN, erosion-deleted experiment, revert-GREEN, ALL_BUILD ×2); `cl`/`link` count was 0 each
time. `MSBUILDDISABLENODEREUSE=1` is set by the wrapper. One earlier check found two `link.exe`
processes with real accumulated CPU — no build was started then; implementation work continued and
the slot was re-checked (and clear) before the first build.

---

## 6. Concerns / residuals

1. **The colour boundary now reaches the exterior wall on sub-surface shell layers** where the paint
   reaches the silhouette. On a flat-topped box the top `top_shell_layers` of the side wall become
   painted instead of only the topmost layer. This is the *point* of the user decision (and the
   standing "colour fidelity outranks material/toolchange cost" ruling), and it arguably reads better
   than a one-layer painted line over base-coloured walls — but it is a visible change on every
   painted flat-topped object, and it is the one thing worth eyeballing in the GUI. **Not visually
   validated**; no slice was inspected, only region geometry via the test harness.
2. **Curved/steep painted surfaces are unchanged by design**, so on an organic model the painted skin
   stays one layer thick wherever the local slope exceeds `atan(layer_height / (wall + spacing))`
   (≈ 6.5° at 0.1 mm layers / 0.45 mm wall, ≈ 12.8° at 0.2 mm). If the user's "eyes/cheeks" sit on a
   genuinely curved surface rather than a flat facet, this change buys them the *flat* part of the
   patch only. Going further would require either an unprintable sub-wall sliver or downward colour
   smear on the exterior (rejected alternative 5) — the physics, not the implementation, is the
   limit. Worth confirming against the user's actual model before calling the report closed.
3. **Cost.** One extra `offset_ex` + `diff_ex` per (painted layer, colour) that has a projection, and
   one extra `intersection_ex` + `union_ex` per descent step. Same order as the offset the loop
   already performs; not measured.
4. **Material/toolchange cost rises** with the wider claim (more painted filament in the shell,
   potentially more tool changes per layer). Explicitly accepted by the standing ruling.
5. `.superpowers/sdd/2026-08-31-paint-depth/progress.md` was already modified in the working tree
   before this session began (not authored here) and is **left alone**, matching the previous wave's
   decision. The commit contains only the two source files.

## Files changed

* `src/libslic3r/MultiMaterialSegmentation.cpp` — `exposed_surface_part()` + the two descent-loop
  splits + the bottom surface-claim gate.
* `tests/libslic3r/test_paint_depth_clamp.cpp` — 3 new fixtures/helpers, 4 new `[paintdepth]` cases,
  1 retargeted case.
