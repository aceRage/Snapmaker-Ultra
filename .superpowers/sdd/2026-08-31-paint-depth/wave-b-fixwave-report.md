# Wave B review fix wave — Important 1, Important 2, Important 3

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, parent `a7fe18884d` (Wave A fix
wave, landed on top of Wave B AFTER `wave-b-review.md` was written). Scope:
`wave-b-review.md`'s Important 1–3. All anchors located by reading the current file, not by
trusting the review's line numbers (Wave A's fixwave shifted them).

---

## Important 1 — already closed by Wave A's I-1 notch cap; regression pin added, no production fix

**File**: `tests/libslic3r/test_paint_depth_clamp.cpp` only. No production code change.

The review's finding: the `D >= wall_stack` descent gate
(`MultiMaterialSegmentation.cpp:1878-1879`, `out.normal_shell`) is written against the
un-notched band `D`, while the lateral band the classic generator actually deposits on even
layers is narrowed by the interlocking notch — leaving a closed 0.1mm ring of base filament at
`paint_depth_walls = 1`. That review was written *before* Wave A's own fixwave landed. Wave A's
I-1 fix (`paint_depth_classic_notch_cap_mm`, `PaintDepth.cpp:41-49`) caps the notch at whatever
slack the classic-floored band has above `wall_stack`:

```
interlocking_depth' = min(interlocking_depth, max(0, cut_width - wall_stack))
```

Re-derivation against the current code: let `a = interlocking_depth`, `s = max_width -
max_wall_stack` (never clamped to 0, since the per-region classic floor guarantees `max_width >=
max_wall_stack` term-by-term, hence also after the `max()`). Then

```
effective_even_layer_band = max_width - min(a, s) = max(max_width - a, max_wall_stack)
```

The right-hand `max()` means the even-layer band can **never** fall below `max_wall_stack`, for
**any** `D` on the classic generator — not only at the `walls = 1` boundary the review's own
numbers use. And the `D >= wall_stack` gate is itself only ever open on the classic generator
*because* that same per-region floor guarantees `max_width (== D) >= every region's own
wall_stack`. So wherever the gate is open on the classic generator, the "ring"
(`wall_stack - effective_even_layer_band`) is `<= 0`: Wave A's I-1 fix closes Important 1 as a
side effect of closing its own, differently-framed finding — the same notch narrowing the same
band below the same `wall_stack`, observed from the classic-floor side (Wave A) rather than the
descent-gate side (this review).

**Verification, not just derivation.** Added the review's exact scenario as a regression pin
(new test, `[paintdepth]`): classic generator, `paint_depth_walls = 1`, 0.45mm lines, 0.1mm
layers, 15° painted slope (`slice_bounded_frustum(40.392, 18, 3, FRUSTUM_SLOPED_WALLS, pdmWalls,
walls=1, 0.1, Classic)`), probing the review's own disputed inset window at `wall_stack - 0.05 =
0.828540mm` on **both** layer 12 (even) and layer 13 (odd). Ran this test against a build with
**zero production changes at all** (the pre-item-2-fix state, i.e. exactly the code the review
was written against, plus Wave A's already-landed I-1): **both parities passed** — empirical
confirmation, not just algebra, that the notch cap already closes the gap. Per the task's own
instruction, this is recorded as a **regression pin**, not a fresh RED: there is no "before the
fix" for item 1, because Wave A's fix already is the fix.

*Consequence noted, not chased further*: the review's own "Related, worth a look" aside
(`paint_depth_clamp_keep_core`'s degradation ladder) is a **different** code path (thin-geometry
degradation, not the classic floor) and was already the subject of Wave A's own I-2 fix
(parity-independent ladder membership). Out of this fix wave's scope; not re-verified here beyond
noting Wave A's I-2 test already covers the ladder-membership question independently.

---

## Important 2 — a painted cap's deepened reach no longer overlaps or annexes a neighbouring painted colour's lateral band beyond its own legacy depth

**Files**: `src/libslic3r/MultiMaterialSegmentation.cpp`
(`segmentation_top_and_bottom_layers`, `merge_segmented_layers`, `segmentation_by_painting`),
`tests/libslic3r/test_paint_depth_clamp.cpp`.

**The problem, confirmed.** `merge_segmented_layers` trims every extruder's lateral claim by
every extruder's top/bottom claim, keyed purely on extruder identity. Wave B's own fix
(`out.normal_shell` gated on `color_idx > 0`) stops the **base** colour's top/bottom claim from
deepening — but does nothing about **two painted colours**: a painted cap's DEEPENED reach (up
to 15 layers at stock defaults, vs. the pre-Wave-B legacy shell's 6) trims a neighbouring painted
colour's lateral band exactly as far as it trims the base colour's, 2.5x deeper than it used to.

**The behaviour chosen: bound the deepened (beyond-legacy) portion of a painted claim to
UNPAINTED (base) material only — implemented as "legacy ∪ (excess clipped against every OTHER
painted colour's own lateral claim)."** This is the review's option "restrict the deepened claim
to trimming only unpainted base material," but discovered mid-implementation to need a second
half: *bounding the trim alone is not sufficient*. If only the trim were bounded (colour B's
lateral survives past legacy depth) while colour A's own excess claim is still appended into
colour A's own region unconditionally (unchanged), the same area ends up claimed by **both**
colours — a real geometric overlap, not merely an unfixed asymmetry. So the excess itself is
clipped against every other painted colour's raw lateral claim and the colour's top/bottom claim
is rebuilt as `legacy ∪ clipped-excess` **before** the existing trim/append-back logic ever runs —
one self-consistent claim that both keep using completely unmodified. This was chosen over
"bound trim to legacy depth" (the review's other framing of the same option) because that framing
alone is the exact form that produces the overlap; and over a full bisector tie-break (the
review's own "real fix," explicitly deferred to a separate wave) as disproportionate scope for
this fix wave. Base colour (`color_idx 0`) never deepens, so its excess is always empty and it is
untouched by this loop — Wave B's own base-colour fix is preserved byte-for-byte.

**Implementation.** `segmentation_top_and_bottom_layers` gained a
`legacy_top_and_bottom_layers_out` parameter: a shadow of `shell_triangles_by_color_top/bottom`
(new `legacy_shell_triangles_by_color_top/bottom` arrays, written at the identical
`[color_idx][last_idx + layer_idx_offset]` slots as the existing arrays — so the review's own §C
race-freedom proof extends unchanged, since the new writes are a strict subset of the
already-proven-disjoint ones) capturing only the descent steps within
`top_shell_layers`/`bottom_shell_layers` of the surface, i.e. exactly what the same walk would
deposit with `paint_depth_normal_mm` forced to 0. `merge_segmented_layers` gained a
`legacy_top_and_bottom_layers` parameter and a new pre-pass (before its existing, completely
unmodified merge loop): for each painted colour, `excess = full − legacy`; if non-empty, clip it
against the union of every *other* painted colour's raw `segmented_regions` lateral claim, then
rebuild `full = legacy ∪ clipped-excess`. `segmentation_by_painting` threads the new structure
through both calls.

**RED, genuine.** Backed up the fixed `MultiMaterialSegmentation.cpp`, reverted it to the
pre-fix (`HEAD`, Wave-A-fixwave) state with `git checkout HEAD --`, keeping only the new tests,
and rebuilt. New fixture (`slice_two_painted_colours`): 40×40×6mm box, `TOP_CAP_FACE` painted
Extruder2 (colour A), the full-height `+X` wall painted Extruder3 (colour B, painted right up to
the shared top edge) — the review's own scenario. `walls = 3`, 0.1mm layers, stock
`top_shell_layers = 4` / `top_shell_thickness = 0.6` (6-layer legacy shell), `D = 1.435675mm` →
15-layer deepened descent. Probe inset 1.1mm (inside colour B's own band, past colour A's
`wall_stack` F1 clearance). RED, at depth 10 (beyond legacy, within deepened reach):

```
test_paint_depth_clamp.cpp(3178): failed: any_contains(claim_for_layer(*object, probe_layer, 3), probe) for: false
test_paint_depth_clamp.cpp(3179): failed: !(any_contains(claim_for_layer(*object, probe_layer, 2), probe)) for: !true
Failed 1 test case, failed 2 assertions.
```

i.e. pre-fix, colour B's stripe does **not** claim the point (line 3178) and colour A's cap
**does** claim it too (line 3179 — confirming the overlap, not just the asymmetry). The depth-3
(within-legacy) sanity block and Important 1's test both passed in this same pre-fix run,
confirming the RED is isolated to exactly the mechanism under test. GREEN after restoring the
fix: both blocks pass, `[paintdepth]` all green (745/56, see Gates below).

---

## Important 3 — cost claim corrected; superset finding recorded

**Files**: `docs/superpowers/specs/2026-08-31-paint-depth-design.md`,
`.superpowers/sdd/2026-08-31-paint-depth/curved-gap-design.md`,
`.superpowers/sdd/2026-08-31-paint-depth/wave-b-report.md`. Documentation only, no test (per the
task's own framing).

Searched the whole repository for the false claim's text ("total extrusion is unchanged" /
"Material: none wasted"); found it in exactly two report/design docs beyond the review itself
(which correctly quotes it only to critique it) — `curved-gap-design.md` §7 and
`wave-b-report.md` §6, both self-contradicted by their own very next paragraph (which correctly
describes the purge/tool-change cost). Corrected both to the review's stronger, accurate
statement: the claim *volume* is a re-colouring at constant infill density (true), but the job's
total extrusion goes **up** — ~9 extra tool changes per painted flat cap at stock defaults (6 →
15 layers), each with its own purge (≈2.5 cm³ at the stock 280mm³ `flush_volumes_matrix`
default), plus the wall loops the new colour boundary adds on every newly split sparse-infill
layer. Added the review's stronger positive result (Minor 1, recorded here per the task's
explicit instruction, not otherwise in scope) alongside the correction in both docs: Wave B's
painted claim is a strict superset of the legacy claim at every slope, so nothing anywhere is
degraded — provable independent of slope purity, unlike the "24°+ is byte-identical" framing
elsewhere in the report (which needs a pure slope to hold; that Minor-1 finding itself is left
untouched, out of this fix wave's scope). Also added a short "Cost note" section to the original
design spec (`docs/superpowers/specs/2026-08-31-paint-depth-design.md`), which predates Option N
and had no cost discussion at all, so future readers of the spec see the corrected numbers rather
than nothing.

---

## Self-review (hand-walk)

- **Classic walls = 1 painted cap ⇒ no base ring on EITHER parity.** Verified two ways: the
  algebraic re-derivation above (general, any `D`), and the Important-1 regression test (both
  layer 12 and layer 13 claim the review's disputed inset). Both hold on the current build.
- **Painted cap over a painted stripe ⇒ the stripe keeps its colour to the depth it had before
  Wave B.** Verified by the Important-2 test: depth 3 (within colour A's legacy shell) is
  unchanged — colour A wins, matching pre- and post-fix behaviour identically; depth 10 (beyond
  legacy, within the deepened reach) now goes to colour B, and colour A does **not** also claim
  it (the `CHECK_FALSE` that specifically pins the overlap fix, not just the trim-bound fix).
- **Single-painted-colour objects (every pre-existing fixture) are untouched.** With
  `num_facets_states <= 2` there is at most one painted colour, so the new "other painted
  colour's lateral claim" union is always empty for it and the clip is skipped
  (`other_painted_laterals.empty()` early-continue) — confirmed by `[paintdepth]`/`[chameleon]`
  landing exactly at baseline-plus-my-two-new-cases, with no other test's assertion count moving.
- **Unpainted objects are untouched.** No painted facets ⇒ every `color_idx >= 1`'s raw/top/
  bottom arrays are empty everywhere ⇒ the new pre-pass in `merge_segmented_layers` is a
  first-line no-op (`full.empty()` continue) — confirmed by `verify_paintdepth.sh`'s
  unpainted-vs-baseline byte-parity check, both runs.

---

## Gates

| Gate | Result |
|---|---|
| `[paintdepth] --order rand --warn NoAssertions` | **745 assertions in 56 test cases, all pass** (baseline 727/54 + this wave's 18/2) |
| `[chameleon] --order rand --warn NoAssertions` | **605 assertions in 133 test cases, all pass** — exactly at baseline, unmoved |
| `ALL_BUILD` (scratchpad `build_next_wt.bat`) | **exit 0**, zero errors |
| `spike/verify_paintdepth.sh` ×2 | **17/17 ALL PASS**, both runs, including `unpainted-run{1,2}-vs-baseline` byte-identical (normalized) |
| full `libslic3r_tests --order rand --warn NoAssertions` | **484 test cases · 480 passed · 2 failed · 2 failed as expected; 50826 assertions · 50822 passed · 2 failed · 2 failed as expected; exit code 2** |

**True full-suite number, not rounded**: 484 cases / 480 passed / 2 failed / 2 failed-as-expected;
50826 assertions / 50822 passed / 2 failed / 2 failed-as-expected; **exit 2**. 484 = Wave A
fixwave's own 482 baseline + this wave's 2 new test cases (Important 1, Important 2). The 2 real
failures are the SAME pre-existing ones prior waves recorded — confirmed by file/name match
(`test_mixed_filament.cpp`, "m1: compute uses num_physical bound..." and "batch_remap mixed
pair-fallback... (KNOWN bug)") at lines 3483/4429, a +14 shift from Wave A's own 3469/4415
consistent with Wave A's fixwave having already shifted that file by +14 — neither touched by
this wave, at either the file or the mechanism level. This is not "clean" — exit code is 2,
matching every prior wave's own stated baseline, not rounded down.

## Commit

One commit, `fix(paint-depth): pin the classic notch cap's closure of the D >= wall_stack ring,
stop a painted cap annexing a neighbouring painted colour's lateral band, and correct the cost
claim (Important 1, Important 2, Important 3)`. Files: `src/libslic3r/MultiMaterialSegmentation.cpp`,
`tests/libslic3r/test_paint_depth_clamp.cpp`,
`docs/superpowers/specs/2026-08-31-paint-depth-design.md`,
`.superpowers/sdd/2026-08-31-paint-depth/curved-gap-design.md`,
`.superpowers/sdd/2026-08-31-paint-depth/wave-b-report.md`.
