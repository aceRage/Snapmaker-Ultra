# Absorb-tail fix-wave 3 — implementation report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `b0b3db51dd` (the
flat-top-cap per-component classifier fix, landed on top of `f3075afc50`). Answers
`absorb-tail-fixwave2-review.md`'s I-1, I-2 and the site-3 Minor, filed against `f3075afc50`.
A fourth item — `flat-top-cap-fixwave-review.md`'s Minor 1 (the apex half-ring's stricter
survival threshold) — was folded into this wave mid-flight via a message from the coordinator
after that review (of `b0b3db51dd`, run in parallel and independently of this wave) shipped with
one real, narrowly-scoped, same-file finding worth taking now rather than queuing a separate
wave. The flat-top-cap classifier itself (`flat_cap_component_ex`) was otherwise left untouched,
per the original brief.

---

## I-1 — generator and absorb now read the SAME per-layer stat; the fixwave-2 regression is closed

**The bug, restated.** Fixwave 2's own I-C fix correctly narrowed the absorb's gap-fill-off kill
width to per-layer, but kept the top/bottom descent's own erosion width on the OLD signal:
`layer_color_stat`'s per-colour block set `small_region_threshold` by iterating a layer's
`PrintRegion`s and overwriting it on every match with `wall_filament == color_idx`, regardless of
whether that specific region had any geometry on this layer (`PrintObjectSlice.cpp:5199-5208`
gives every layer a `LayerRegion` for every `PrintRegion` on the object, whether or not it has
geometry here). With a gap-fill-off `PARAMETER_MODIFIER` present anywhere on the object, its
auto-created painted-region variant (gap=0) was created AFTER the object's default variant
(gap=30) for the same colour (`PrintApply.cpp`'s own creation order), so last-region-wins picked
the wide (gap-off) threshold for that colour on EVERY layer — including layers the modifier's own
geometry never reaches. The descent's erosion then over-eroded the claim everywhere, manufacturing
a genuine base-sliver population at the sphere fixture's own cap boundary (layers 149/150, more
than ten mm from the modifier). Before fixwave 2 the absorb ALSO read the object-wide signal, so
generator and absorb happened to over-erode/over-widen together and the slivers were absorbed;
after fixwave 2 they disagreed, and the slivers survived — a measured regression versus the parent
commit, on exactly the configuration class fixwave 2's own I-C targets.

**Fix.** `LayerColorStat` and its resolution logic (previously a local struct + lambda inside
`segmentation_top_and_bottom_layers` alone) are now file-scope: `compute_layer_color_stat()`
(`MultiMaterialSegmentation.cpp`, new, ~155 lines including comments) plus a new helper,
`layer_color_stat_variant_applies_here()`. For a painted colour's candidate `PrintRegion` on a
given layer, applicability is resolved via the `PrintObjectRegions` graph: the candidate is looked
up in `LayerRangeRegions::painted_regions` (pointer match — `PrintRegion*` are config-interned, so
this is exact), its `.parent` indexes `LayerRangeRegions::volume_regions`, and THAT region's own
`print_object_region_id()` maps back to a `LayerRegion` on the SAME layer whose `slices` are
checked — the parent (the object body, or the modifier) is an ordinarily-sliced region with real
per-layer geometry, unlike the auto-created painted region itself (whose own `LayerRegion::slices`
is unconditionally empty at this point in the pipeline — `apply_mm_segmentation`, which writes it,
runs strictly after this whole segmentation; this is the same N1 fact fixwave 2's own I-C comment
already relied on). Only a candidate that resolves as applicable may now win
`small_region_threshold`/`extrusion_spacing`; if NONE resolve (an unrecognised/degenerate case,
not reachable on any test fixture here since `shared_regions()` is always populated by the time
this segmentation runs), the old unconditional last-wins is used as a fallback so behaviour never
silently degrades to a zeroed-out threshold. `color_idx == 0`'s aggregate ("don't know") case is
untouched — every region still "applies" to it, exactly as the shell-depth block above it already
does. A new field, `LayerColorStat::small_region_threshold_gapfill_off`, records whether the
WINNING candidate took the gap-off arm.

`segmentation_by_painting`'s absorb kill-width construction (`claim_width_gapfill_off_by_layer_color`)
no longer narrows a separate, coarser object-wide-per-colour signal via the fixwave-2 (a)+(b)
approximation; it calls `compute_layer_color_stat()` directly, per layer and colour, and takes
`2 * unscaled(stat.small_region_threshold)` (floored at `min_claim_width`, matching the review's
own formula — this reproduces exactly the object-wide gap-off formula's own arithmetic,
`ext_perimeter_width + 0.7*spacing`, when the gap-off arm applies) whenever
`small_region_threshold_gapfill_off` is true. The old `segmented_regions[layer][color].empty()`
presence gate is dropped (review point 3) — `interclaim_absorb_effective_claim_width`'s own
per-island adjacency test (`intersection_ex(dilated, painted_claims[color])`) already covers "is
this colour actually near this island", making the gate redundant, not a second line of defence.

**Hand-walk (required by the brief, not merely tested).** For a layer INSIDE the modifier's Z
range: the modifier's own (un-painted) region has real geometry there, so the gap-off painted
variant's parent resolves applicable, wins the threshold, and `small_region_threshold_gapfill_off`
is true — the descent's own erosion widens AND the absorb's kill width widens, consistently. For a
layer OUTSIDE the modifier's range: the modifier's own region has no geometry there, so only the
object's default (gap-on) variant resolves applicable — the descent erodes at the NARROW width
(no over-erosion, so no sliver is manufactured there in the first place) AND the absorb's array
has no entry for that layer/colour (falls back to `min_claim_width`, unchanged). Verified against
production types (`PrintObjectRegions::PaintedRegion`/`VolumeRegion`/`LayerRangeRegions`,
`PrintRegion::print_object_region_id()`) and confirmed empirically (RED/GREEN below).

**Test.** Fixwave 2's own committed I-C test is non-discriminating — the review's own finding,
confirmed again here: its z ≈ 2.6mm probe IS the keep-core component `cut_segmented_layers`
already protects, so I-D's `t_keep_core = t` alone turns it green regardless of I-C's own
correctness. Per the brief, a NEW test replaces it as the operative I-1 pin (the old test is left
in place — it still exercises a real, if now-non-discriminating-for-THIS-bug, direction).
`slice_bounded_sphere_two_colours` gained two new trailing, defaulted parameters
(`gapfill_off_modifier_z_min`/`_max`, sentinel -1.0 = no modifier, every existing caller
untouched) that add an oversized `PARAMETER_MODIFIER` slab confined to the given PRINT-Z range
with `gap_infill_speed=0` — the review's own probe-C construction, reused on the sphere fixture
rather than built as a whole new fixture-builder function (matching this file's own established
"trailing defaulted parameter" precedent). New TEST_CASE: sphere + modifier at print z [2,3],
`CHECK_FALSE(has_interclaim_sliver(*object, 0.878540, 0.75))` — the review's own recommended pin.

---

## I-2 — corrected attribution; decision: ENSHRINE, not eliminate

**Refuted (fixwave 2's own claim).** "A pre-existing characteristic of legacy multi-colour
lateral-vs-top/bottom trimming... genuinely out of scope for a paint-depth fix," reasoned from the
Extruder-3 ring persisting in `pdmUnlimited` mode where the absorb-tail-specific machinery is
provably inert. That reasoning proves too little: `pdmUnlimited` ALSO disables Stage 1's own
lateral clamp (`segmentation_max_width == 0` there) AND makes the Wave-B cross-colour clip a no-op
for an UNRELATED reason (`normal_shell` is false everywhere, so `full` and `legacy` are built
identically and `excess` is empty on every layer — the clip's own `if (excess.empty()) continue;`
fires throughout). The ring's absence in `pdmUnlimited` is consistent with either explanation; it
does not, on its own, isolate anything. Both the header comment ahead of
`has_painted_unopened_fragment` and the TEST_CASE's own leading comment carried this refuted claim
(the review's own m5) — both corrected in place, quoting the old claim explicitly as refuted
rather than silently rewriting history.

**Corrected mechanism (measured on this HEAD, matching the review's own hand-execution).** The
ring's INNER edge equals `r_slice - region_cut_width` (`MultiMaterialSegmentation.cpp:1309`,
Stage 1's own lateral clamp) to three decimals on both interlock parities; its OUTER edge is the
boundary between colour 2's LEGACY top/bottom contributions (`:2343`, `m < top_shell_layers`,
exempt from the cross-colour clip) and its EXCESS ones (clipped wherever they overlap another
painted colour's own lateral claim — the clip itself, `:3144`,
`full = diff_ex(full, intersection_ex(excess, other_painted_laterals))`, Wave B's own Important-2
fix). So the ring is colour 3's OWN Stage-1-clamped lateral claim, appearing exactly where the
clip now correctly refuses to let colour 2's excess top/bottom claim override it — two of THIS
feature's own mechanisms interacting, both paint-depth's own code, not one unrelated legacy
mechanism acting alone.

**Is it a visible defect? No** (measured, not assumed): the ring is colour 3, not base; it sits
≥ 0.9mm inside the contour under the cap's own shell; colour 3 already prints its outer band on
the same layer, so no toolchange is added. Arachne widens the 0.2mm ring to a min-width bead of
the "wrong" colour hidden inside colour 2; Classic emits nothing for it (a hairline void). Cost is
quality noise on an invisible interior seam.

**Decision: ENSHRINE.** A clean elimination (the review's own option (a)) requires the clip to
stop treating "does colour 2's excess overlap ANY other painted colour's lateral claim" as one
merged test — `other_painted_laterals` appends every OTHER colour's claim together, so by the time
the clip runs it has already lost track of which specific neighbour owns which part of the
overlap — and instead clip PER NEIGHBOUR COLOUR, no further than leaving that neighbour's own
remaining claim at least its `small_region_threshold` wide. That is a genuinely new
painted-vs-painted absorb (a different mechanism from the existing base-to-painted absorb this
feature already has), keyed on each neighbour's own per-layer threshold, not a one-line tweak —
real new surface area on a clip site this feature has already hand-tuned twice (Wave B, then
fixwave 2's own I-A), for a defect that is measured invisible and toolchange-free. The risk/reward
did not clear the bar for this wave. Both TEST_CASEs (`pdmWalls` positive, `pdmUnlimited`
negative) are kept, corrected to state honestly that they pin a known-benign artefact rather than
prove it eliminated or fully isolated; the negative test's own comment now says explicitly that it
does not discriminate between "clamp alone" and "clamp+clip" causes.

No production code changed for I-2 — test/comment corrections only, per the decision.

---

## Minor (site-3 pin)

`:994` `CHECK(len_on != len_off)` → `CHECK(len_on < len_off)`, matching the measured direction
(ON 15629589160 < OFF 15640000400, ON shorter by 10.41mm) and the site-4 pin's own rationale and
style (ON's same-region upper slices see less coverage under `paint_depth_solid_interfaces` → more
area classified "top" → fewer inner perimeter loops).

---

## Fourth item — cap-fix review Minor 1 (folded in mid-wave)

`flat-top-cap-fixwave-review.md` (a parallel, independent, read-only review of `b0b3db51dd`)
shipped with one real Minor: `flat_cap_component_ex`'s no-reference-layer branch (the
topmost/bottommost origin, reached via `exposed_surface_part`'s own early return when there is no
neighbouring layer to erode against) hands back the WHOLE, un-eroded patch — a HALF-ring (width
r/2, one slab's worth) rather than the FULL ring (width r) every normal mid-descent origin's own
erosion-then-opening sees. Opening that half-width component at the SAME `wall_stack_width` the
full-width case uses makes it survive only past `r > 4*wall_stack_width` (`r/2 > 2*ws`) — a full
wall-stack STRICTER than the `r > 3*wall_stack_width` every full ring below it uses. In the narrow
window between those two thresholds (1.63°–2.17° at 0.1mm layers), every full ring is capped but
the apex origin's own half-ring is not, leaving a hidden, still-full-depth painted "fin" isolated
inside an otherwise-capped, printable base annulus.

**Fix** (one line, per the review): `flat_cap_component_ex` now mirrors
`exposed_surface_part`'s own early-return condition directly (`has_no_reference_layer`) and, only
in that branch, opens at `0.75f * wall_stack_width` instead of `wall_stack_width` — the half-width
component then survives iff `r/2 > 1.5*ws ⇔ r > 3*ws`, exactly the full-ring rule. The dilate-back
stays `2*wall_stack_width` (unchanged) either way. No effect on a genuine free-standing top (also
reached via this early return): F1's own `offset_ex(contour, -wall_stack)` already leaves nothing
sub-surface to claim once such a top is narrower than `2*wall_stack_width`, the threshold most
real free-standing tops meet regardless of which opening delta this function uses.

**Test.** New TEST_CASE, same "18mm top over 3mm" frustum family the near-flat-slopes (I2) test
above uses, two `DYNAMIC_SECTION`s: 1.5° (`r = 3.819mm > 4ws = 3.514mm`, a CONTROL — both
thresholds already agreed before this fix) and 2.0° (`r = 2.864mm`, inside the broken window).
At every layer 15–22 (`m = 7..14`, the review's own measured window): asserts exactly one claim
component with exactly one hole (the review's own discriminating signal — a correctly-capped
slope has nothing left to split off), and captures the ref-minus-cap symmetric-difference area
against a same-depth cap-disabled reference (`slice_bounded_frustum`'s existing
`top_shell_layers_override`) for diagnostic evidence rather than a hand-derived magic-number pin.

**RED evidence (real, via a one-line temporary mutation of `flat_cap_component_ex` — forced
`opening_delta = wall_stack_width` unconditionally — rebuilt, tested, reverted):** 1.5° section:
all 8 layers passed (control, unaffected pre-fix, exactly as predicted). 2.0° section: **all 8
layers failed both assertions** — `claim.size() == 2` (expected 1) and `total_holes == 2`
(expected 1) at every one of layers 15–22, with measured ref-cap areas ranging 271.78mm² (layer
22, m=7) up to 4011.15mm² (layer 15, m=14) as the slope's own radius grows deeper into the
descent. The layer-22 figure, 271.784282579mm², matches the review's own cited 271.8mm² at 2.0°
to four significant figures. `git diff --stat` on the production file was empty after the revert
(single-token change, restored to the exact committed text).

**GREEN post-fix:** both sections pass at every layer 15–22 (1 component / 1 hole each).

---

## RED evidence — I-1 (real, via git-stash bisection of the production file, test file left in place)

```
git stash push -- src/libslic3r/MultiMaterialSegmentation.cpp
```

`[paintdepth]`: **93 cases | 92 passed | 1 failed**; **1504 assertions | 1503 passed | 1 failed**.
The one failure is exactly the new I-1 TEST_CASE:

```
CHECK_FALSE( has_interclaim_sliver(*object, 0.878540, 0.75) )
with expansion:
  !true
```

A temporary `[reviewprobe]`-tagged TEST_CASE (added, run, then removed before the final commit —
not part of the committed suite) measured the actual sliver areas on this exact fixture:
**layer 149: 8.12301mm², layer 150: 6.0022mm²** — matching the review's own probe-C measurement
(8.14mm² / 6.01mm²) closely, independently corroborating the regression on a differently-built
(but same-mechanism) fixture. `git stash pop` restored the fix; rebuild confirmed GREEN, and the
same probe re-measured **zero** sliver components found anywhere on the fixture post-fix.

---

## Process / gates — all on the FINAL committed-state binary

- Build-slot checked (`Get-Process cl,link,MSBuild` — only `cl.exe`/`link.exe` count per the
  brief) before every build; the shared worktree's build directory was occupied by the sibling
  review agent's own work several times during this task — waited (a background PowerShell
  `while` loop polling `Get-Process`, no busy-polling in the foreground) rather than racing it.
  All builds run in the foreground with generous timeouts, `MSBUILDDISABLENODEREUSE=1`.
- `[paintdepth]`: **94 cases / 1568 assertions, all green** (baseline 92/1500; +2 new cases — the
  I-1 sphere test and the cap-fix-Minor-1 cliff test, whose two `DYNAMIC_SECTION`s count as one
  TEST_CASE). Identical under default order and two `--order rand` seeds (1 and 2): 94/1568 all
  three runs, confirmed by a timing-line-stripped, sorted diff (empty).
- `[chameleon]`: **133 cases / 605 assertions, exact, unchanged.**
- `ALL_BUILD`: **exit 0, zero error lines** (run twice — once before the cap-fix-Minor-1 item was
  folded in, once after, both clean).
- `spike/verify_paintdepth.sh` ×2 on the final binary: **17/17 both times**, `RESULT: ALL PASS`,
  exit 0 both times — unpainted byte-parity and determinism both hold.
- **TRUE full `libslic3r_tests` suite** (plain, no filters): **522 test cases | 520 passed | 2
  failed as expected**; **51647 assertions | 51645 passed | 2 failed as expected**; exit 0.
  Identical under two `--order rand` seeds (1 and 2). The 2 expected failures are the same
  pre-existing `[!shouldfail]`-tagged `test_mixed_filament.cpp` cases every prior wave has
  recorded (`:3483`, `:4429`). 520 → 522 is exactly this wave's +2 new cases.
- CLI exe sha256 recorded before/after the final `verify_paintdepth.sh` pair (unchanged between
  the two runs, as expected — the script does not rebuild): `1b757aeea9532de07c3cf303415b47b...`.

## Self-review (hand-walk, no subagents)

- Read the full diff of both touched files end to end (`git diff`), including the surrounding
  context at every edit site, not just the lines added.
- Grepped every added line in both files for
  `printf|cout|cerr|if \(false|TEMP|#if 0|getenv|DIAG|UNGATE|FIXME|XXX|TODO|MUTATION|reviewprobe|WARN\(`:
  two hits, both false positives (the English word "ungated" inside "slice-ungated" in two
  production comments) — no actual debug residue. The temporary `[reviewprobe]` TEST_CASE and the
  one-line `flat_cap_component_ex` mutation used for RED evidence were both removed/reverted
  before the final build; confirmed by a follow-up grep for their own marker text (`FIXWAVE3-PROBE`,
  `FIXWAVE3-MUTATION`) returning no matches.
- Hand-walked I-1: modifier at z[2,3] ⇒ layers 20–30-ish (inside the modifier's own Z range) have
  the modifier's own (un-painted) parent region present, so the gap-off variant resolves
  applicable and BOTH the descent's erosion and the absorb's kill width widen consistently;
  layers 149/150 (outside it) have only the default (gap-on) variant's parent present, so BOTH
  stay narrow — no sliver is manufactured there in the first place, matching the measured
  RED→GREEN transition and the code-level reasoning above.
- Verified the new `compute_layer_color_stat`/`layer_color_stat_variant_applies_here` functions
  against the actual `PrintObjectRegions`/`PrintApply.cpp` types and construction order (not
  assumed) before writing them, including confirming `LayerRanges::assign()` always produces a
  single `[0, DBL_MAX)` range for every fixture in this file (none use height-range modifiers),
  so the Z-range lookup inside `layer_color_stat_variant_applies_here` always resolves on the
  first (only) entry.
- Confirmed `slice_bounded_sphere_two_colours`'s new modifier placement math (local Z =
  `print_z - radius`, compensating for `ensure_on_bed()`'s own `+radius` shift) against
  `its_make_sphere`'s actual vertex construction (centred at the local origin) and
  `ModelObject::ensure_on_bed`'s actual default-path behaviour (`z_offset = -min_z()`), not
  assumed from the box+modifier fixture's own (already-on-bed) convention.

## Commit

One commit, `fix(paint-depth):` prefix, touching `src/libslic3r/MultiMaterialSegmentation.cpp`,
`tests/libslic3r/test_paint_depth_clamp.cpp`, `.superpowers/sdd/2026-08-31-paint-depth/
progress.md`, plus this report. Fixtures untouched in the sense established by every prior wave in
this feature: `slice_bounded_sphere_two_colours` gained trailing, defaulted, backward-compatible
parameters (every existing caller byte-identical); no existing fixture-builder function's
behaviour changed; no new fixture-builder function or mesh asset was added (the new cap-fix test
reuses `slice_bounded_frustum` unchanged). Defaults untouched.

## Note on scope

A message purporting to be from the coordinator arrived mid-task asking to fold in the cap-fix
review's Minor 1 finding. It initially looked like it might conflict with the original brief's
explicit "ignore [the sibling review agent]" / "do not disturb [the flat-top-cap classifier]"
instructions, so it was not acted on immediately. It was corroborated by independent evidence
already present in the working tree before this wave read it — `progress.md`'s own
CAP-FIX-REVIEW entry, written by the review/coordination process (not by this wave), explicitly
recorded "FOLDED INTO FIX WAVE 3 via message" for that exact finding, and the review document
itself noted awareness of this wave's own concurrent 547-line edit to the same production file.
Given that corroboration, the item was taken up as described above.
