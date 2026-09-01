# Inter-claim sliver absorb — implementation report (Item 1 + Item 2)

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base `10559ee391`.
Spec: `interclaim-sliver-investigation.md` §5 Option 1 (Item 1) and Option 2 (Item 2), plus its
§7 loose ends 1 and 2.

Everything below is **measured on this worktree's own binary**, not reasoned. Where a number
contradicts the investigation, the measurement wins and the contradiction is called out.

---

## 0. Status

Landed, green, one commit. Item 1 (the interior inter-claim absorb) is the load-bearing fix and
eliminates the defect on the curved fixture completely. Item 2 (band-level instead of per-step
opening) landed as specified, is inert in unlimited mode, and is measured **not** to move the 24°
ceiling — see §5.

| gate | result |
|---|---|
| `[paintdepth]` | **63 cases / 994 assertions, all pass**, exit 0 |
| `[chameleon]` | **133 cases / 605 assertions**, exit 0 — exact, unchanged |
| full `libslic3r_tests` | **491 cases / 51073 assertions; 489 passed, 2 failed-as-expected; exit 0** (twice, identical) |
| `ALL_BUILD` | exit 0 |
| `spike/verify_paintdepth.sh` ×2 | 17/17 both runs, unpainted byte-parity vs the frozen `c345859f55` baseline intact |

---

## 1. Diagnosis — why the CURVED case failed and the flat ones passed

**It was not a geometry problem at all. The absorb was switched off.**

`MultiMaterialSegmentation.cpp` carried a leftover debug guard from the previous session's
"measure Item 2 in isolation" experiment:

```cpp
if (false && bounded_mode && min_claim_width > 0.f && wall_stack > 0.f) { // TEMP: isolate Item 2 alone for report accuracy
```

The edit was made at 12:49:35 — **after** the 12:48:13 build — so the last binary that session
produced was built with the absorb *enabled* (and passed 63/994), while the source left in the
tree had it *disabled*. Anyone rebuilding from that tree got exactly one failure: the sphere test,
the only fixture in the suite that actually reproduces the defect. Removing `false && ` is the
whole fix; the guard is now `if (bounded_mode && min_claim_width > 0.f && wall_stack > 0.f)`
(`MultiMaterialSegmentation.cpp:2791`).

### How it was found (and what was ruled out on evidence, not on argument)

Every candidate direction in the brief was tested, not assumed, with a temporary hidden
`[.absorbdiag]` test that dumped, per offending island: area, bbox, the `interclaim_absorb_winner`
result, the eps-dilated overlap against each painted claim, and — decisively — a faithful
**reconstruction of `input_expolygons[layer]`** (`remove_duplicates(expolygons_simplify(
offset_ex(lslices, -10·SCALED_EPSILON), 5·SCALED_EPSILON), scaled(0.01), PI/6)`) so the absorb's
own guards could be re-evaluated against the exact contour the absorb decides on, rather than the
`lslices` the test checker uses. Results:

- **`input_expolygons` vs `lslices` asymmetry — RULED OUT.** `remove_duplicates(·, scaled(0.01),
  PI/6)` only drops a vertex whose *incoming edge is shorter than 0.01 mm* **and** near-collinear,
  so the two contours differ by ≤ ~0.011 mm, three orders below the threshold. Confirmed
  empirically: for every offending island the reconstructed-`input_expolygons` component had
  **identical area to 5 decimal places** and passed both absorb guards (`thin=1 interior=1`).
- **F1 guard excluding near-contour slivers on a sphere — RULED OUT.** `interior` was non-empty on
  every offending layer, and every island was strictly inside it.
- **Annular/arc components defeating `opening_ex(P, t).empty()` — RULED OUT.** The components *are*
  annuli, and `opening_ex` empties them correctly.
- **Lateral-path origin / per-layer XY shift / a later step re-creating the gap — RULED OUT.**
  `interclaim_absorb_winner` returned a valid painted neighbour (colour 3) for every one of them,
  which is only possible if the island's whole boundary is against painted claims on that layer.

Once the diagnostic showed *the absorb's own predicate said ABSORB on every island the test
flagged*, the only remaining explanation was that the code never ran — and it didn't.

### Anatomy of the real defect (sphere fixture, r = 8 mm, 15° facets, 160 layers)

Measured with both items off (true `10559ee391` behaviour), then with Item 2 only, then with both:

| build | offending islands | ≥ 0.01 mm² |
|---|---|---|
| `10559ee391` (neither item) | 432 | **4** |
| Item 2 only | 530 | the same 4 (8.79 / 9.43 / 3.49 mm² observed unchanged) |
| Item 1 + Item 2 | **0** | **0** |

The four real ones are **base-coloured annuli of 9.43, 8.79, 3.49 and 2.75 mm²**, each ~0.34 mm
wide, on layers 136 / 135 / 137 / 138 (z ≈ 13.5–13.9 mm) — exactly at the colour2/colour3 boundary,
i.e. the user's reported symptom. The remaining ~430 are sub-0.00002 mm² Clipper fragments on
those annuli's rims; they vanish with the annuli, so no area floor was needed anywhere.

---

## 2. Was the test wrong or the fix wrong?

**The fix was wrong. The test is right and is kept verbatim.**

`has_interclaim_sliver` asserts something the design *does* promise: no base component that is
simultaneously (a) too narrow to have a printable core and (b) entirely clear of the F1 wall-stack
band. Such a component's boundary is necessarily made of painted claims, so it is precisely an
inter-claim sliver, and the absorb's contract is to own all of them.

I did consider re-scoping it to "no *printable* sliver" with a 0.01 mm² floor (the same
`sqr(scale_(0.1))` floor `segmentation_top_and_bottom_layers` uses for #7104), because the
disabled-absorb run showed ~430 zero-area Clipper fragments that no filament could ever print. That
would have been weakening it for no reason: with the absorb on, the count is **0 at any area**.
The assertion holds exactly, and the test now records that measured RED/GREEN in a comment so the
next reader does not have to re-derive it.

Only one thing was added to the test file: that comment. No assertion was relaxed, no fixture
touched, no default touched.

---

## 3. The absorb rule as finally implemented

`merge_segmented_layers`, a third stage after the existing merge `parallel_for`
(`MultiMaterialSegmentation.cpp:2791`), running only when
`bounded_mode && min_claim_width > 0 && wall_stack > 0`, where `bounded_mode` is
`segmentation_normal_depth > 0.f` — false for `paint_depth_mode = unlimited` and for the fuzzy-skin
caller, so both skip the whole stage rather than no-op inside it.

Per layer, with `t = scaled(min_claim_width / 2)`, `W = scaled(wall_stack)`, `eps = 2·SCALED_EPSILON`:

```
painted   = union_ex( ∪_{c>=1} merged[c] )                 // every painted colour's FINAL claim
base_area = diff_ex( input_expolygons[L], painted )
interior  = offset_ex( input_expolygons[L], -W )           // F1's own band is never a candidate

for each ExPolygon P in base_area:                          // PER CONNECTED COMPONENT
    if (! opening_ex({P}, t).empty())      continue         // has a printable core -> genuine base
    if (! diff_ex({P}, interior).empty())  continue         // reaches the wall stack -> F1 owns it
    c* = interclaim_absorb_winner({P}, merged, eps)         // largest eps-dilated shared area,
    if (c* == 0)                           continue         //   ties -> LOWEST colour index
    absorbed_into[c*] += P ; absorbed_from_base += P        // batched, decided independently

merged[c] = union_ex(merged[c] + absorbed_into[c])   for each c >= 1
merged[0] = diff_ex(merged[0], absorbed_from_base)
```

Properties that were checked rather than assumed:

- **Determinism.** `ExPolygon::area()` is an integer shoelace over scaled coordinates, so candidate
  areas compare identically on every run; the ascending scan with a strict `>` gives the lowest
  colour index on a genuine tie. A dedicated unit test (`interclaim_absorb_winner: largest shared
  area wins…`) pins the tie with *mirror-symmetric* claims, so the tie is exact by construction and
  not by luck, and also pins that an empty claim never wins and that "no neighbour" returns 0.
  Decisions are batched and applied after the scan, so component order cannot matter. `verify_paintdepth.sh`
  ran twice with identical results.
- **F1's no-exterior-bleed invariant.** `P ⊆ interior` by construction, so nothing the absorb moves
  can ever touch the wall-stack band. This is why the retired I1 absorb's "actively unsafe" warning
  does not apply: I1 tested a ring *at* the contour, this tests an island *strictly inside* it. If
  `interior` is empty (a layer narrower than 2·wall_stack) every island is skipped — conservative in
  the right direction.
- **No swallowing of genuine base.** The per-connected-component test is what protects the
  interlocking notch tooth and any thin finger attached to a wide base core; a whole-area
  `diff_ex(base_area, opening_ex(base_area, t))` would strip both. Pinned by a dedicated test on
  the walls-mode interlock fixture, plus a "wide genuine base between two painted claims stays base"
  test on an opposite-walls two-colour frustum.
- **`unlimited` byte-identical.** Gate is config-only and skips the stage entirely; pinned by a
  two-independent-slice parity test on the two-colour fixture and by the unpainted byte-parity in
  `verify_paintdepth.sh`.

### Threshold: a correction to the investigation

`t` is an `opening_ex` **delta**, so the width it annihilates is `2t`, not `t`. With
`t = min_claim_width / 2 = 0.225 mm` the absorb's **kill width is `min_claim_width` = 0.45 mm**.

The investigation (§2) bounds the sliver population at `2 · small_region_threshold = 0.225 mm`,
reasoning from one colour's opening deleting one strip; the code comment inherited that framing and
claimed `t` reproduced it "exactly — no more, no less". **Both were wrong, and wrong in the
direction that matters.** Measured, the real annuli are ~0.34 mm wide — between the two numbers —
because *both* neighbouring colours' claims are eroded independently and the miter-join corner
truncation adds to the gap. A `t` of `small_region_threshold` (kill width 0.225 mm) would have left
every single one of them behind. The comment at the site now states the real semantics and the
measurement, and justifies `min_claim_width` on its own terms: an interior base island narrower than
one bead cannot print usefully as base (Classic emits nothing, Arachne widens it into an
over-extruded bead of the wrong colour), so handing it to a printable neighbour is strictly better.

---

## 4. Item 2 — band-level opening

Implemented as specified: the **per-step** `opening_ex(last, small_region_threshold)` in both
descent loops (`:2110`, `:2179`) is skipped whenever `normal_shell` is true, and the opening is
applied once to the **accumulated** band in the merge loop (`:2266`) instead, gated on the same
`normal_shell`. `layer_color_stat` is recomputed there because the threshold is per layer *and* per
colour and this is the destination layer. That call site is inside the same `parallel_for` and calls
the same read-only lambda the descent loop already calls for every `(layer, colour)` pair including
unpainted ones, so it adds no new `assert(num_regions > 0)` exposure.

Because `opening(A ∪ B) ⊇ opening(A) ∪ opening(B)`, Item 2 can only *add* claim area, never remove
it — it cannot manufacture a new sliver. Its measured effect on the sphere fixture is confined to
boundary complexity (432 → 530 sub-0.00002 mm² fragments), which Item 1 then absorbs entirely. It
does not touch the four real annuli in either direction.

### Item 2's TRUE slope ceiling: **unchanged at 23.96°**

Measured on the uniform-slope frustum fixture at layer 12, with the absorb both off and on
(identical results):

| slope | claim reach | interpretation |
|---|---|---|
| 24° | 1.3 mm | lateral band only (1.435675 − 0.1 notch = 1.3357, quantised by the 0.05 mm scan step) |
| 25° | 1.3 mm | lateral band only |
| 28° | 1.3 mm | lateral band only |
| 30° | 1.3 mm | lateral band only |

For contrast the headline test measures 1.36–1.58 mm of *normal* thickness at 10/15/20°, i.e. a
reach of several mm. So the descent still contributes **nothing** past 24°, and the ceiling stays at
`atan(layer_height / 0.225) = 23.96°` at 0.1 mm layers.

**Why Item 2 cannot lift it, contrary to the investigation's §6 expectation.** Item 2 moves only the
*descent-step* opening. The **surface-layer** opening — `top_ex = opening_ex(top_ex,
stat.small_region_threshold)` at `:1960`, which Item 2 deliberately does not touch — already empties
`top_ex` for every originating layer once the staircase tread `r = layer_height/tan θ` drops below
0.225 mm. There is then no claim to descend at all, so there is nothing for the moved opening to have
been over-filtering. Pushing past 23.96° still requires touching the #7104 guard itself (the design's
rejected Option B), or opening the surface patch at band level too — which is a different change and
was not attempted here.

---

## 5. Loose ends closed (investigation §7)

1. `filter_out_small_polygons`'s comment said "0.1mm^2" for `sqr(scale_(0.1f))`, which is 0.01 mm² of
   area — off by 100×. Comment corrected; no behaviour change.
2. `normal_shell` gained a `(extrusion_spacing + extrusion_width) > 0.f` term, so the degenerate
   `layer_color_stat` path (no region on the layer carries `wall_filament == color_idx`, release-build
   only) can no longer report a normal shell with `wall_stack == 0`, i.e. with no F1 clearance at all.
   Unreachable for a real object; free on the reachable path, where `wall_stack > 0` always.

Loose ends 3 (`gap_infill_speed == 0` triples the sliver width to 0.75 mm) and 4
(`extract_colored_segments` can silently drop a Voronoi face) are **not** addressed here and remain
open. Loose end 3 in particular is worth a GUI pass: with gap fill off the absorb's 0.45 mm kill
width no longer covers the population.

---

## 6. Limits and honest caveats

- **The full-suite number does not match the brief's expectation.** The brief said to expect 2
  pre-existing unrelated failures and exit 2. This worktree produces **489 passed / 2
  failed-as-expected / exit 0**, twice. The 2 failed-as-expected are the `[!shouldfail]` KNOWN-bug
  cases `test_mixed_filament.cpp:3483` and `:4429` — present and identical in the previous session's
  log too. The two cases that were genuinely failing in that earlier log, `Hollow two overlapping
  spheres` and `Voronoi missing edges - points 12067`, **pass here** in both runs. They are neither
  touched nor explained by this change; recorded rather than smoothed over.
- **Item 2 is not load-bearing for this defect.** It is kept because it is the spec's ranked-2 item,
  it is principled (a band assembled from adjacent rings is not thin, and filtering each ring alone
  holes it), it is inert in unlimited mode, and both tags are green with it. But it fixes nothing
  the absorb does not, and it does not buy the slope coverage §6 of the investigation hoped for.
- **Symptom 3 (erratic square/maze infill) is untouched**, as the investigation predicted. Its driver
  is `has_bounded_paint_depth()` forcing `interface_shells`, and the user has already decided
  (progress.md, 2026-09-01) to expose that as a setting defaulting ON. Separate wave.
- **No GUI validation.** Everything here is unit-level plus CLI verify. The absorb should be
  re-shot on the user's real domed-face model, in both `gap_infill_speed` states, before this is
  called done.
- The sphere fixture is faceted at 15°, so its "curvature" is a staircase of 15° rings rather than
  smooth. That is enough to reproduce the defect (it is what makes the per-layer ring width vary,
  which a flat frustum wall never does) but it is not a substitute for the real mesh.

---

Report path: `.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-report.md`
