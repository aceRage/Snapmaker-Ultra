# Paint Depth — Final Review

Reviewer: final-review agent (read-only) · Date: 2026-08-31
Scope: `git diff c345859f55..c7b2a59292` (T1 ed726d71e9, T2 89d3f15d44, T3 7579272f9d, T4 c7b2a59292)
Spec: docs/superpowers/specs/2026-08-31-paint-depth-design.md · Plan: docs/superpowers/plans/2026-08-31-paint-depth.md

Verdict: **FIX FIRST** — 1 Critical-leaning Important requiring a decision/fix, 2 further Important, 5 Minor.
Suites re-run by this reviewer: [paintdepth] 13 cases / 56 assertions PASS, [chameleon] 133/605 PASS,
spike/verify_paintdepth.sh 17/17 PASS.

---

## Findings (ranked)

### F1 — IMPORTANT (Critical-leaning): interlocking sub-band semantics are NOT "Prusa-style" — even layers clamp paint to a 0.3 mm sliver, by default, on every painted object

`cut_segmented_layers` (src/libslic3r/MultiMaterialSegmentation.cpp:1146-1170, pre-existing, untouched
by this diff) computes on even layers:

```cpp
const float interlocking_cut_width = interlocking_depth > 0.f ? std::max(cut_width - interlocking_depth, 0.f) : 0.f;  // :1153 — DEAD, never used
...
const float region_cut_width = ((layer_idx % 2 == 0) && (interlocking_depth != 0.f)) ? interlocking_depth : cut_width; // :1159
```

The dead `interlocking_cut_width` variable is the smoking gun of a fork drift from PrusaSlicer, whose
released code uses `interlocking_cut_width` (= band − interlock) as the even-layer cut. In this tree the
even-layer band is the raw `interlocking_depth` itself. Consequence under the feature's new defaults
(walls:3 band ≈ 0.45 + 2×~0.43 ≈ 1.3 mm, interlock 0.3):

- **Odd layers**: painted claim 1.3 mm deep (3 walls — matches spec/GUI checklist item 1).
- **Even layers**: painted claim only **0.3 mm deep** — thinner than one external perimeter width.
  The interlock "teeth" are ~1.0 mm, not the spec's "Prusa-style … ~0.3 mm" (decision 3). Half of all
  layers carry a sub-wall-width painted sliver at the surface (Arachne will emit a thin bead; classic
  perimeters may drop it → possible base-color striping on alternating layers).

This was previously reachable only by users who manually set BOTH mmu keys nonzero; the feature makes it
the DEFAULT for every painted model. No committed test measures band width or even/odd alternation, so
the suite is blind to it (see F8). Also note the walls-clamp E2E test's `near_face` probe at exactly
0.3 mm sits precisely ON the even-layer band's inner edge under these semantics — a fragile coincidence.

**Fix options** (pick one, small either way): (a) restore Prusa semantics in `cut_segmented_layers` by
using the already-computed `interlocking_cut_width` (also fixes legacy manual users; changes behavior for
anyone tuned to the drift); (b) keep the function untouched (plan's stated preference) and feed
`std::max(band - interlock, 0.f)`-style value at the new call site in
`multi_material_segmentation_by_painting`; (c) explicit user decision that the drifted semantics are
acceptable + GUI-round eyeball. Doing nothing means checklist item 1 ("≤3 dark walls") is false on even
layers and item 4's "0.3 mm interlocking" description misstates what will be seen.

### F2 — IMPORTANT: legacy migration flip-flops for diff-serialized user presets

`handle_legacy_composite` (PrintConfig.cpp:7897-7904) migrates a nonzero stored
`mmu_segmented_region_max_width` but **leaves the old key in the config**. User presets serialize as a
diff vs. defaults/parent. Scenario: user loads a legacy preset (old key = 0.8) → migrated in memory to
{millimeters, 0.8} → user explicitly switches mode back to **walls** (the default) → saves. The save
drops `paint_depth_mode` (equals default) but keeps `mmu_segmented_region_max_width = 0.8` (non-default).
On the next load the guard `!config.has("paint_depth_mode")` is true again → **re-migrates to
millimeters/0.8, silently reverting the user's explicit walls choice**, on every reload, forever.
Project 3mfs (full-config dumps) are immune; diff-based preset files are not.
**Fix**: after migrating (and arguably also in the zero case), neutralize the old key —
`config.set_key_value("mmu_segmented_region_max_width", new ConfigOptionFloat(0.))` or erase it — so a
re-save can never re-trigger the migration. T4's CLI checks can't see this (single load, full dump).

### F3 — IMPORTANT: `interface_shells` has FOUR read sites, not two — T3's OR-in misses PerimeterGenerator.cpp

T3's report claims `interface_shells` is "read directly in exactly two places". Wrong:
**PerimeterGenerator.cpp:622 and :2273** also read `object_config->interface_shells` (classic and
Arachne top-surface/one-wall-top logic: with interface_shells they diff against same-region upper slices,
so a region boundary counts as a top surface for single-wall-top treatment). Neither site ORs
`has_bounded_paint_depth()`, so a bounded painted object behaves inconsistently with a genuine
`interface_shells=true` object: `detect_surfaces_type()` types the color Z-interface as solid skin, but
the perimeter generator's top-surface detection does not see it as top. **Effect is conservative**
(solid skin still printed, with normal wall count instead of the one-wall-top treatment; no bleed leak,
no crash — the untouched branch is the pre-existing default path), so this is an inconsistency + report
inaccuracy, not a bleed defect. Decide: extend the OR to both sites for full interface_shells parity, or
document the intentional narrowing (and correct the task-3 report's "exactly two read sites" claim).

### F4 — MINOR: `paint_depth_mm` allows 0, producing incoherent alternating output

`paint_depth_mm` has `min = 0` (PrintConfig.cpp). In millimeters mode with mm=0: `max_width = 0`, but
interlock 0.3 still trips the gate at MultiMaterialSegmentation.cpp:2270
(`segmentation_max_width > 0.f || segmentation_interlocking_depth > 0.f`) → even layers clamp to
0.3 mm, odd layers are **unlimited** (region_cut_width = 0 skips the cut). Neither "disabled" nor a
band. Suggest `min` > 0 (e.g. 0.1), or treat band==0 in a bounded mode as unlimited by also zeroing
interlock at the call site.

### F5 — MINOR: stale tooltip on `mmu_segmented_region_interlocking_depth`

Still says it is "ignored if mmu_segmented_region_max_width is zero" — that key is now hidden/legacy and
no longer the gate (paint_depth_mode is); the "ignored if bigger than max_width" claim was never enforced
in code either (relevant to F1). Update text (i18n round) so the one surviving mmu-named UI row doesn't
document a removed control.

### F6 — MINOR: `paint_infill_override`'s verify_update (fast region-reuse) path has no test

All three override tests fresh-slice, exercising only `generate_print_object_regions()`. The
toggle-only second-apply path through `verify_update_print_object_regions()` (the very thing T3 says
would otherwise be "a silent no-op") is untested. Hand-verified correct by this review:
`config_apply_only` at PrintApply.cpp:1678 updates the object config before the regions loop (~:1852)
computes `paint_sparse_infill`, all four new keys invalidate posSlice (PrintObject.cpp:959-963), and the
verify path rebuilds cfg from the parent and diffs (PrintApply.cpp:835-849). Worth a follow-up test
(apply once, flip only the bool, apply again, assert region config changed).

### F7 — MINOR: migration only runs on load paths that call `handle_legacy_composite`

Covered: ini, ptree, gcode-string, json loads (Config.cpp:1127/1219/1308/1545 — includes Orca preset
and project-json loads). Not covered: any per-key `set_deserialize` path that skips the composite hook
(e.g. Prusa-format 3mf import). This exactly matches the pre-existing `wiping_volumes` composite
migration's exposure, so it is parity, not a regression — noting for completeness.

### F8 — NOTE (test honesty): no committed test pins band WIDTH or even/odd alternation

The E2E probes are 0.3 mm (inside any band) and 10 mm (outside any band) — they prove bounded vs.
unbounded, not the walls-math-to-geometry contract or the interlocking behavior (F1 invisible). The
walls formula itself is pinned only at the pure-helper level. Acceptable for this round given the GUI
handoff, but a width-measuring assertion (e.g. probe at band±ε on an odd layer) would close it.

---

## Mandatory checks

1. **Bounded-by-default band math** — PASS (with F1 caveat). `diff_ex(claim, offset_ex(input, -band))`
   measures the band inward from the slice boundary; walls-mode `ext_width + (N-1)*spacing` lands the
   inner band edge ≈ the Nth wall's inner edge — no off-by-half; matches spec formula verbatim and the
   fuzzy-skin precedent. All-regions-max is conservative (never under-clamps a wide region; may
   over-deepen a narrow region's band when a wider-flow region coexists) and documented in-code.
   Interlocking alternation is the exception — see F1.
2. **Whole-layer short-circuit** — PASS. `has_layer_only_one_color` writes into the same
   `segmented_regions` array; `cut_segmented_layers` runs after the whole per-layer loop with no
   origin-distinguishing branch, so full rings, partial rings, and multi-color layers are all clipped
   identically; interior falls out of every claim and reverts to the base region via
   merge/apply_mm_segmentation. Verified in code AND by the committed all-8-side-facets E2E test, which
   genuinely reproduces the short-circuit. Top/bottom projections (segmentation_top_and_bottom_layers)
   merge after the cut un-clamped — pre-existing and intended (Z-bounded by shell layer counts).
3. **Legacy mapping** — PASS with F2. Nonzero → {millimeters, value verbatim}; zero/absent → walls
   default (approved flip); guard never overwrites a config carrying the new key; old key stays
   parseable and is read by nothing at runtime (only Preset list + the migration itself — grep-verified).
   Re-verified live via verify_paintdepth.sh C2/C3. The stranded-value flip-flop on diff-serialized
   presets is F2.
4. **Interlocking** — PASS on gating, FAIL-leaning on semantics (F1). 0.3 default is force-zeroed in
   pdmUnlimited at the call site (the gate expression at :2270 untouched, as the plan required); beam
   mutual exclusion (`!segmentation_interlocking_beam`) preserved; ConfigManipulation beam toggle
   unchanged. Interlock ≥ band (mm mode < 0.3): even layers cut DEEPER than the requested band; mm=0
   is F4. Interlock < band: see F1.
5. **T3 interface_shells OR-in** — PARTIAL (F3). Both PrintObject.cpp read sites covered symmetrically;
   two PerimeterGenerator.cpp sites missed. Off-behavior purity holds everywhere:
   `has_bounded_paint_depth()` = `is_mm_painted() && mode != unlimited`, so unpainted objects and
   painted+unlimited objects evaluate every changed expression to its pre-feature value (byte-identical
   behavior); confirmed by the paired unlimited-mode E2E test and T4's unpainted parity. The accepted
   whole-object side effect (solid skin at non-paint region boundaries too) is correctly bounded to
   painted+bounded objects only.
6. **paint_infill_override** — PASS. Sparse-only at both PrintApply sites (walls/solid always painted);
   the bounded-mode gate matches the spec's own wording ("bounded claims keep BASE-color sparse
   infill") — the T3 "judgment call" is actually spec-aligned, not a deviation. All four new keys join
   the posSlice invalidation group; toggle-reslices path hand-verified (F6 for the missing test); the
   mismatch-warning update correctly compares against expected value.
7. **Perf** — PASS. Band derivation is O(num_regions) flow computations once per segmentation; the
   per-layer clip is one offset_ex + diff_ex per layer inside the existing tbb::parallel_for — the same
   cost previously paid by any nonzero mmu_segmented_region_max_width, dwarfed by the Voronoi stage. No
   per-segment/per-sample work added.
8. **Tests honesty** — PASS with F8. Clamp tests genuinely discriminate (deep-point assertions fail
   with the clamp off — matches the documented stash-RED, and the unlimited twins prove the bug is
   real); z-interface test runs the full Print::process(). T4 baseline: frozen gcode IS committed
   (spike/out/paintdepth_baseline.gcode), provenance (temp worktree at c345859f55, app-target-only
   build, worktree removed) documented in both the script header and task-4-report.md, with the
   merge-base==pre-T1 source-identity check recorded; normalized strips are each independently asserted
   by the config-surface checks, so the parity compare hides nothing.
9. **Cross-feature + suite re-runs** — PASS. PrintConfig/Preset/Tab/ConfigManipulation changes are
   purely additive next to chameleon's; re-run by this reviewer: [paintdepth] 13/56, [chameleon]
   133/605, verify_paintdepth.sh 17/17 — all green.
10. **Merge risks** — documented. New config surface (paint_depth_mode/walls/mm, paint_infill_override)
    is spec-named; profile compat via still-parseable old key + composite migration (F2/F7 caveats);
    i18n: 5 new L() strings need translation, F5's stale tooltip should ride along; #6892 thin-feature
    caveat is explicitly carried into the GUI checklist (item 7) rather than silently dropped; GUI
    checklist items 1/4 need re-wording if F1 is resolved as "keep drift".

## Bottom line

Stage-1/Stage-2 wiring, legacy flip, off-behavior purity, short-circuit routing, and verification
hygiene are all genuinely solid — the end-to-end harness is the strongest part of this feature. The
ship blockers are exactly where the tests can't see: the interlocking knob's drifted semantics turning
the default into 1.3/0.3 alternating paint depth (F1 — decision or one-line fix), and the preset
migration flip-flop (F2 — neutralize the old key after migrating). F3 needs a decision + report
correction; F4-F8 are follow-ups that can ride the GUI round.
