# Flat-top cap — scoped review of `8c5bf752de`

Reviewed AS COMMITTED (`git show 8c5bf752de`), worktree `C:\Dev\SnapmakerOrcaNext`, branch
`feat/paint-depth`, against `flat-top-cap-report.md`. Read-only: no worktree edits, no commits.
Working-tree edits by the parallel fix-wave agent were ignored (at review time `src/` was still
byte-identical to the commit; the sibling had begun editing `tests/libslic3r/test_paint_depth_clamp.cpp`
only after my binary snapshot was taken).

## Verdict: FIX FIRST — 0 Critical / 2 Important / 5 Minor

Nothing here is visible on a print (every change sits at or below the effective solid shell, and
check 3 confirms the floor lands exactly on the shell boundary), and nothing crashes or races.
But the discriminator does not implement the user's rule on two of the three geometry classes it
has to handle: a painted flat top that is not the object's own highest face (a ledge beside a
riser) keeps a wall-stack-wide painted ring to the full D depth on every layer 6–14 below it
(saving zero tool changes), and every slope below ~6.49° is *partially* capped into a striped
bullseye — some of which the absorb stage then silently undoes depending on `gap_infill_speed`.
Both are one root cause with one fix (§Important 1). The report's "byte-identical on slopes" and
"pointwise split verified by a dedicated test" claims are contradicted by measurement (§Minor 1/2).

## Evidence base (all reproducible; nothing asserted from reading alone)

- Test binary snapshotted before any run: `build/tests/libslic3r/Release/libslic3r_tests.exe`
  sha256 `c9d7cb9e…9d77d3` (built 18:18:38, commit 18:21:23, `git diff 8c5bf752de -- src tests`
  empty at snapshot time). Every run below used the scratchpad copy, never the live exe.
- `[paintdepth]`: **80 cases / 1153 assertions** — default order, `--order rand --rng-seed 1`,
  `--order rand --rng-seed 2`, all identical. `[chameleon]`: **133 / 605**. TRUE full suite:
  **508 cases | 506 passed | 2 failed as expected; 51232 | 51230 | 2** (the pre-existing
  `[!shouldfail]` `test_mixed_filament.cpp` pair), exit 0. All numbers match the report.
- `spike/verify_paintdepth.sh` ×2 against `build/src/Release/snapmaker-orca.exe` (sha unchanged
  across both runs): **17/17 both**, unpainted byte-parity + determinism intact.
- Debug-residue grep over every `+` line of the commit (`printf|cout|cerr|#if 0|if (false|TEMP|
  DIAG|UNGATE|TODO|FIXME|XXX|BOOST_LOG|getenv|dump|debug`): **zero matches**. Production diff is
  53 purely additive lines; test diff 206 purely additive lines (no existing test modified).
- **Probe build** (the load-bearing evidence for checks 1/2/4/6): a scratchpad copy of the
  committed test file plus five `[probe]` cases was compiled with the exact `cl` line from
  `libslic3r_tests.tlog/CL.command.1.tlog` and linked with the exact `link` line from
  `link.command.1.tlog`, against a **snapshot** of the committed `libslic3r.lib` (sha256
  `58a93f7f…4fb8`), into `scratchpad/probe/probe_tests.exe`. Cross-validation: that exe reproduces
  `[paintdepth]` **80/1153** exactly. Probe source: `scratchpad/probe/probe_append.cpp`; output:
  `scratchpad/log_probe.txt` (scratchpad =
  `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad`).

## One line per mandatory check

1. Discriminator — **FAIL.** Pointwise wall-stack yardstick classifies the *inner* wall-stack band of
   any patch that touches the layer above as sloped, whatever the patch's true slope: ledge band
   kept to D (Important 1), sub-6.49° slopes striped (Important 2). Pure flat-cap-over-steep-flank
   transition (the committed fixture) is clean: no hole, no double-deposit.
2. Slope invariance — **PASS with a caveat.** Pin tolerance 0.03 mm < one-layer shift (0.094–0.098 mm),
   so a 1-layer regression would be caught; the subtraction cannot touch a ring narrower than one
   wall stack. But the topmost origin layer is always "flat" (early return), so the 10° fixture is
   NOT byte-identical at layers 15–23 (Minor 1) — the pinned layer 12 simply cannot see it.
3. Cap depth — **PASS.** `stat.top_shell_layers` IS `effective_shell_layers_by_thickness`
   (layers-or-thickness, real `print_z`/`bottom_z`, total depth incl. surface); cap fires at
   `m >= shell`, legacy shadow copies `m < shell`: complementary at the same boundary, and that
   boundary equals `discover_vertical_shells`' own walk. Floor is exactly the first non-solid layer.
4. Absorb — **PARTIAL.** The full-cap *floor* cannot be a sliver (it is either nothing or a large
   island) and the committed test proves the neighbour cannot annex it. The capped *rim* can be and
   is: capped annuli narrower than the kill width are re-annexed (5° always; 4° only with
   `gap_infill_speed == 0`) — the widened case is NOT covered by the committed test (Minor 4).
5. Unlimited + parity — **PASS.** `top_cap_active`/`bottom_cap_active` require `normal_shell`, false
   in unlimited mode and for colour 0; unpainted parity 17/17; anti-smear tripwire test untouched;
   `paint_depth_solid_interfaces=false` opens no new bleed path (the Z-interface moves from m=15 to
   m=6, both beneath an opaque 6-layer painted solid shell; see §Check 5).
6. Cost evidence — **PASS on the fixture, weak as a test.** Probe: 6 layers carry ANY painted
   polygon, m≥6 has zero polygons, so the 9 tool changes are genuinely saved there. The committed
   test measures a centre point, which on the ledge fixture would still report 6 while 15 layers
   carry paint (Minor 3).
7. Re-run — **PASS.** 80/1153 ×3 orders, 133/605, 17/17 ×2, 508|506|2, residue grep clean (above).

---

## Important 1 — a painted flat top beside a riser keeps a wall-stack-wide painted ring to the full D depth; the cap saves nothing there

**Where.** `src/libslic3r/MultiMaterialSegmentation.cpp:2043-2045` (`top_flat_cap_ex =
exposed_surface_part(top_ex, input_expolygons, layer_idx + 1, …)`), used at `:2149-2150`; mirrored
at `:2218-2220` / `:2243-2244`. `exposed_surface_part` (`:1493-1506`) is
`diff_ex(patch, offset_ex(contour[reference_layer], +wall_stack))`.

**Mechanism.** The yardstick measures each point's distance to the *next layer's contour*, not the
local slope of the patch. On a flat ledge of width W that abuts a riser, the strip of the ledge
within one wall stack (0.8785 mm) of the riser is `< wall_stack` from the layer above and is
therefore classified "sloped" — even though its local slope is 0°. That strip is never subtracted,
so it descends to the full D bound (15 layers) exactly like a flank.

**Measured (probe "ledge": 40×40×4 slab, centred 20×20×4 tower, slab TOP facets painted,
`walls=3`, 0.1 mm layers, top shell 4/0.6 ⇒ effective 6):**

| m below ledge | painted polygons | area mm² | A (mid-ledge, 15 mm) | B (0.4 mm from riser) | C (1.5 mm from riser) |
|---|---|---|---|---|---|
| 0 | 1 | 1200.0 | Y | Y | Y |
| 1–5 | 1 | 1062.5 | Y | Y | Y |
| **6–14** | **1** | **73.4** | – | **Y** | – |
| 15+ | 0 | 0 | – | – | – |

Painted layers at/below the ledge: **15** (cap intent: 6). Scan at m=8 from the +X edge shows the
ring at inset 9.15–10.00 mm, i.e. exactly the [10.0, 10.85] band beside the riser. The ring is
0.85 mm wide (F1/opening trimmed from 0.8785), 80 mm long ⇒ 73 mm², and it is a separate painted
region with its own perimeters on nine layers where nothing else is painted — so the toolchange the
user decided to recover is paid on all nine, plus a thin painted wall loop in the hidden interior.

**Why it matters.** The user's decision was about hidden material under FLAT tops. The committed
fixture (a 40×40 prism's own top face) is the one case where the reference layer is empty and the
whole patch is trivially flat. On real models a painted flat top adjacent to any taller feature —
a face plate beside a nose, a base beside a boss, every tier of a stepped part — is the common
case, and there the saving silently does not materialise. The bottom mirror (an overhang underside
beside a stem, reference = layer below) has the same band.

**Fix (same as Important 2 — one change).** Classify per patch component, not per point, by
dilating the exposed part back over the patch so that everything within one wall stack of a
genuinely exposed point is flat too:

```cpp
const ExPolygons exposed = exposed_surface_part(top_ex, input_expolygons, layer_idx + 1, num_layers, wall_stack);
// widen the "flat" verdict from the exposed core back across the wall-stack band that abuts the
// layer above: a ledge/ring is flat as a whole or not at all, never split down its own width.
const ExPolygons top_flat_cap_ex = exposed.empty() ? ExPolygons{}
    : intersection_ex(top_ex, offset_ex(opening_ex(exposed, delta), delta + wall_stack));
```

`delta` sets the width a component must have to count as flat: `delta = wall_stack` ⇒ a ring must
be ≥ 3 wall stacks (≈2.6 mm, θ ≤ 2.2° at 0.1 mm layers) — this keeps every slope the user cares
about at D, caps ledges/caps/islands whole, and also stops the apex-ring effect of Minor 1
(a 0.28 mm half-ring never survives the opening). `delta = 0` reproduces the commit's own stated
6.49° rule but applied ring-wise (no stripes, ledge fixed) at the cost of re-introducing that cliff
for hidden material. Either way the split is per component, so the absorb interplay of Minor 4
disappears (capped gaps are whole rings, ≥ 2.6 mm, never sliver-sized). Mirror for bottoms.

**Tests to add.** The ledge fixture above (assert `claim.empty()` for m = 6..14 and `== 6`
painted layers); the bottom mirror (T-shape underside beside the stem); a 4° frustum asserting the
layer-12 claim is ONE contiguous polygon (see Important 2).

## Important 2 — slopes below ~6.49° are partially capped into a striped bullseye, and the absorb then undoes it (or not) depending on `gap_infill_speed`

**Where.** Same site. On a staircase ring of run r > wall_stack the yardstick keeps the inner
wall-stack band (adjacent to the layer above) and caps the outer r − ws remainder — the inverse of
the ring's actual local slope on a dome (the inner band is the flatter part).

**Measured (probe "shallow slopes", four sloped walls painted, `walls=3`, 0.1 mm layers, probe
layer 12; ws = 0.8785):**

| slope | r (mm) | r − ws | gap fill | layer-12 claim scan from the contour (mm) | claim polys | painted layers |
|---|---|---|---|---|---|---|
| 3° | 1.908 | 1.03 | on | [0.05,11.40] [12.50,13.35] [14.40,15.25] [16.30,17.15] [18.25,19.05] [20.15,20.95] [22.05,22.85] [23.95,24.80] [25.85,26.70] [27.75,28.60] | **10** | 30/30 |
| 3° | 1.908 | 1.03 | **off** | identical stripes (gaps 1.03 > 0.75 kill width) | **10** | 30/30 |
| 4° | 1.430 | 0.55 | on | [0.05,8.55] [9.15,10.00] [10.60,11.40] … [20.60,21.45] (10 runs) | **10** | 30/30 |
| 4° | 1.430 | 0.55 | **off** | [0.05,21.45] — stripes absorbed away (0.55 < 0.75) | 1 | 30/30 |
| 5° | 1.143 | 0.26 | on | [0.05,17.10] — stripes absorbed away (0.26 < 0.45) | 1 | 30/30 |
| 5° | 1.143 | 0.26 | off | [0.05,17.10] | 1 | 30/30 |

Reading: contiguous to 6r (the shell depth), then, past it, 0.85 mm painted annuli (the uncapped
inner bands of rings 6..14 above) separated by (r − ws) base annuli, out to the old 15r bound.
`15r` = 28.62 / 21.45 / 17.15 mm is reached in every case that is contiguous — i.e. where the
absorb re-annexed the capped gaps, the final claim is the pre-cap claim: the cap was a no-op that
cost two Clipper passes. Where the gaps survive (3° always, 4° with gap fill on), the hidden
interior under the slope is a concentric pattern of 0.85 mm painted walls and 0.26–1.03 mm base
walls instead of sparse infill — more material and time than before the cap, with no tool change
saved (every layer of a slope carries its own surface ring regardless: 30/30 painted layers).

**Why it matters.** The user's rule is "slopes and walls keep the full normal thickness D". A 3–5°
slope is a slope; this caps part of it, in a pattern that flips with an unrelated setting. It is
hidden (all of it lies past the 6-ring solid shell, in sparse infill), so not Critical — but it is
a net cost regression on exactly the shallow geometry Option N was built for (domed eyes/cheeks
have rings this wide near the apex on any model larger than ~40 mm), and the report's §5 claim
that the cap "does nothing at all on sloped patches" / "neither produces a newly-thin sliver" is
false.

**Fix.** Important 1's change (per-component classification with a width threshold). With
`delta = wall_stack`, 3–5° slopes are untouched (rings 1.1–1.9 mm < 2.6 mm) and stay exactly at
D. If the user instead wants sub-2° slopes treated as flat, the whole ring is then capped (a clean
6-ring shell, no stripes, no absorb dependence).

## Minor 1 — the 10° fixture is NOT byte-identical: the apex half-ring is capped (report §3 and the commit message overclaim)

`exposed_surface_part` returns the whole patch when the reference layer does not exist
(`:1502-1503`), so at the object's topmost layer every painted patch is "flat" — including the
half-height staircase ring of a painted slope. At 10° that ring is r/2 = 0.284 mm (survives the
0.225 mm opening; at 15°/20° it is 0.19/0.14 mm and is erased, which is why those two really are
unchanged). Its deposits at m ≥ 6 are subtracted.

Probe (T-family 10° frustum, `claim_reach_mm` per layer; r = 0.5671):

| layer | m from top | measured reach | uncapped expectation | if top origin capped |
|---|---|---|---|---|
| 12 (the pin) | 17 | 8.50 | 8.51 | 8.51 |
| 16 | 13 | 7.35 | 7.66 | 7.37 |
| 20 | 9 | **5.10** | 5.39 | **5.10** |
| 23 | 6 | **3.40** | 3.69 | **3.40** |
| 24 | 5 | 3.10 | 3.12 | 3.12 |
| 26 | 3 | 1.95 | 1.98 | 1.98 |

Layers 15–23 follow the "capped" column exactly; layers 24–29 the uncapped one. The pinned layer
12 is 17 layers below the apex and cannot see it. Effect is 0.28 mm lateral / 0.05 mm normal in
hidden sparse infill — cosmetically nil — but "confirmed byte-identical to 10 decimal places" is
true of one layer, not of the slope. Fix: Important 1's opening removes it; add a reach pin at
layer 20 of the 10° fixture (5.39 expected) so the apex is covered.

## Minor 2 — the "dome/frustum mix" test does not exercise the pointwise split it is cited for

`tests/libslic3r/test_paint_depth_clamp.cpp:4271`: the 15° frustum's rings are 0.373 mm
(< ws ⇒ wholly sloped) and its top face has no layer above (⇒ wholly flat). No ring wider than one
wall stack, no ledge, so `diff_ex(patch, offset(contour_above, ws))` never splits anything; the
test passes on the trivial case only. Probe of the same fixture at the transition (layers 21–24:
band [0.05, 2.95/2.60/2.20] then full at m=5) confirms that case is clean, but that is all it
shows. Add the ledge and 4° fixtures (Important 1/2) as the real pointwise-split pins.

## Minor 3 — the cost-evidence test measures a point, not painted layers

`:4373` counts layers whose CENTRE is claimed. On the ledge fixture that count is 6 while 15
layers carry a 73 mm² painted region (Important 1) — the proxy passes precisely where the saving is
lost. Assert `extruder2_claim_for_layer(top - m).empty()` for m = 6..14 (what actually removes the
tool change) in both the headline test (`:4201`) and the cost test; on the committed fixture that
holds (probe: 0 polygons at m ≥ 6).

## Minor 4 — absorb-safety test does not cover the capped rim nor the widened kill width

`:4338` proves the full-cap floor is not annexed by an active neighbour (correct, and it cannot be:
the floor is a large island with a printable core). The interplay that does exist is on the rim
(Important 2 table: 5° gaps absorbed with gap fill on, 4° gaps absorbed only with
`gap_infill_speed == 0`). With Important 1's fix no capped gap is ever sliver-sized, so no new
absorb test is strictly needed — but the report's §5 reasoning should be corrected, and a
`gap_infill_speed = 0` variant of the two-colour fixture is cheap insurance.

## Minor 5 — comment/report wording

- `:2028-2033` ("Where the local staircase run r is below one wall stack … the patch is genuinely
  near-flat") describes a per-ring rule the code does not implement (it is per point, see
  Important 1/2); rewrite once the fix lands.
- Report §1 "top_flat_cap_ex == top_ex on a genuine flat top (no neighbouring layer at all)" — true
  only when the entire next layer is empty; a flat top with any geometry elsewhere on the next
  layer goes through the diff path (fine when far, the ledge band when near).

---

## Check details

### Check 1 — discriminator, hand-executed and measured

`top_flat_cap_ex = top_ex \ offset(contour[j+1], +ws)`; `last` at step m ⊆ top_ex (eroded term
`top_ex ∩ offset(slices_trimmed, m·(−ws))` ∪ full-width term `(top_ex ∩ slices_trimmed) ∩
offset(contour[j−m], −ws)`), subtracted as a whole at m ≥ shell. Per origin j:

- Flat cap, empty layer above: flat = top_ex ⇒ `last` empties at m = shell, N3 break fires
  (probe: 0 polygons at m ≥ 6). Clean.
- Steep ring, r < ws: flat = ∅ ⇒ guard `! top_flat_cap_ex.empty()` closes ⇒ byte-identical.
- Crown/flank transition where the crown is a whole flat face and the flank rings are all < ws
  (the committed fixture): the union at target layer t is the flank rings at inset [m·r,(m+1)·r]
  for m ≤ 29−t plus the capped crown; probe layers 21/22/23 = [0, 2.95/2.60/2.20] (=(m+1)·r) and
  layer 24 full. **No hole, no double-deposit** — a union of disjoint per-origin sets, each origin
  subtracting only its own flat part.
- Ring with r > ws, or a ledge touching a riser: flat = the part farther than ws from the layer
  above ⇒ the inner ws band stays, the rest is capped ⇒ Important 1/2 (measured above). This is
  the "band in NEITHER term" the check asked about, in the wrong place: it is a capped gap between
  two uncapped bands, repeated per ring.

### Check 2 — slope invariance

Pin `:4238`: `WithinAbs(expected, 0.03)`; a one-layer change of M moves the realised normal
thickness by h·cos θ = 0.098/0.097/0.094 mm at 10/15/20° ⇒ caught. The scan step (0.05·sin θ =
0.009–0.017 mm) is below the tolerance, so sub-layer drift is not pinned — acceptable for a
layer-quantised quantity. The `! top_flat_cap_ex.empty()` guard makes the subtraction unreachable
for any ring narrower than one wall stack; the reachable exception is the topmost origin (Minor 1),
which the pin's layer cannot observe.

### Check 3 — cap depth (the highest-value check)

- `stat.top_shell_layers` (`:1857-1858`) = max over regions with slices of
  `effective_shell_layers_by_thickness(layers, layer_idx, true, top_shell_layers, top_shell_thickness)`
  (`:1381-1428`): returns the TOTAL depth including the surface layer — walk `++m` per layer
  below, break at the first layer with `base − print_z ≥ thickness − EPS` (that layer counted),
  `max(n_layers, m)`.
- `discover_vertical_shells` (`PrintObject.cpp:1972-1985`): layer i is solid for a top at T iff
  `i < itop (= T + n)` or `print_z[T] − print_z[i] < thickness − EPS`, i.e. solid layers are
  T, T−1, …, T−(m−1) with m the same total — the first NON-solid layer is exactly m below T.
- Cap `:2149`: `layer_idx − last_idx >= top_shell_layers` ⇒ first subtracted step is m = shell =
  first non-solid layer. Legacy shadow `:2180` copies `m < shell`. Complementary, same number.
- Bottom `:2243`: `last_idx − layer_idx >= size_t(bottom_shell_layers)` with the bottom walk on
  `bottom_z` (`:1417-1424`) mirroring `PrintObject.cpp:2001-2014`. ✓
- Variable layer height: both walks use each layer's real `print_z`/`bottom_z`, and the count is
  anchored at the origin layer in both ✓. Guarded by `top_cap_active` requiring
  `descent > shell` (nothing to cap otherwise) and by the enclosing `shell > 0` gate.
- Empirical: flat cap 6 painted layers with thickness 0.6 > count 4 (thickness-driven), bottom
  3 (count-driven, thickness 0), m = shell has zero polygons. **Floor is exactly at the shell
  boundary; the base-coloured-shell-layer bleed is not re-created.**

### Check 4 — absorb

Absorb `:3050-3099`: per base component, skip if `opening_ex(single, effective_claim_width/2)`
non-empty (printable core), skip if it touches the F1 band, skip thin keep-core residue, else hand
to `interclaim_absorb_winner`. The capped floor of a whole patch is either absent (nothing painted
on the layer ⇒ `painted.empty()` ⇒ continue) or a large island ⇒ printable core ⇒ kept; the
committed test `:4338` confirms with an active neighbour. The capped RIM annuli of Important 2 are
sliver-sized (r − ws = 0.26–0.55 mm) and interior ⇒ absorbed into the winner (the same colour on a
one-colour slope; on a two-colour boundary, whichever shares more edge — hidden either way).
Widened kill width (0.75 mm, `gap_infill_speed == 0`, resolved per island `:3054-3056`) extends
that to the 4° case — measured. Not covered by the committed test.

### Check 5 — unlimited, parity, tripwire, solid interfaces

- `top_cap_active = normal_shell && …` (`:2042`); `normal_shell` (`:1926-1928`) requires
  `paint_depth_normal_mm > 0` (0 in unlimited mode) and `color_idx > 0` ⇒ unlimited and the base
  colour never reach the cap; the legacy-parity tests (`:427`, `:559`, `:638`, `:3799`) stay green.
- Unpainted: `verify_paintdepth.sh` byte-parity vs the frozen baseline, twice.
- Anti-smear tripwire (`:1375`) untouched (test diff is append-only) and green.
- `paint_depth_solid_interfaces`: the four consumers (`PrintObject.cpp:1338/1773`, PerimeterGenerator)
  are gated on `has_bounded_paint_depth() && paint_depth_solid_interfaces`. With the cap the base
  region directly under a painted flat cap becomes a Z-interface at m = shell instead of m = D.
  ON: per-region surface detection gives that base layer stTop ⇒ a base-colour solid shell under
  the painted shell (was at m=15–20, now at m=6–11; same material, hidden). OFF: merged detection
  ⇒ the painted region's last solid layer (m=5) sits on base sparse infill exactly as any top shell
  sits on its own sparse infill; no bridging (merged view sees material below), no exposed base.
  The deeper claim never "masked" anything: pre-cap the m=15 boundary was painted-sparse over
  base-sparse, which is weaker than the new solid-over-sparse. No new bleed path.

### Check 6 — cost

Probe "flat-cap fixture": 6 layers with a non-empty Extruder2 region, `m ≥ 6` ⇒ 0 polygons ⇒ no
painted extrusion ⇒ ToolOrdering drops the extruder on those layers: 9 tool changes genuinely
recovered on that fixture. The ledge probe (Important 1) shows 15 layers with a 73 mm² painted
region ⇒ 0 recovered. The committed cost test cannot tell these apart (Minor 3).

### Check 7 — numbers

See "Evidence base". Additionally `[paintdepth]` on the probe exe (committed test file +
snapshot lib): 80/1153, identical to the live snapshot.

## Answers the task asked for verbatim

- **Check 1 (transition):** at a flat-face/steep-flank transition — neither a hole nor a
  double-deposit (measured: band = (m+1)·r at layers 21–23, full at 24). On any ring wider than one
  wall stack (slopes < 6.49°, large-dome crowns, every ledge beside a riser) the split is the wrong
  way round: the inner wall-stack band stays to D and the outer remainder is capped, producing
  capped gaps between uncapped bands — a bullseye at depths ≥ shell (10 disjoint polygons at 3°/4°).
- **Check 3 (floor):** yes, exactly at the shell boundary — `m >= effective_shell_layers_by_thickness`
  total (surface included), complementary to the legacy shadow's `<`, equal to
  `discover_vertical_shells`' first non-solid layer, bottoms on `bottom_z`, variable layer heights
  through real `print_z`; m = shell carries zero painted polygons.
