# Paint Depth — Fix Wave Report

Worktree: C:\Dev\SnapmakerOrcaNext, branch feat/paint-depth, base c7b2a59292
Scope: final-review.md findings F1, F2, F3, plus minors F4 (paint_depth_mm min=0 incoherence)
and F5 (stale interlocking tooltip). F6-F8 deferred (F8's alternation-test gap is closed by
this wave's new test, per the review's own recommendation).

## Status: COMPLETE

All fixes implemented, TDD RED confirmed for F1 and F2 on pre-fix HEAD, GREEN after the fix,
[paintdepth] (15/84, up from 13/56 baseline) + [chameleon] (133/605) green, ALL_BUILD exit 0,
spike/verify_paintdepth.sh 17/17 on two consecutive runs. Committed as a single
`fix(paint-depth): ...` commit.

## F1 — interlocking sub-band semantics restored to Prusa-style

`cut_segmented_layers` (MultiMaterialSegmentation.cpp:1146) already computed the correct
Prusa-style even-layer cut width (`interlocking_cut_width = cut_width - interlocking_depth`,
clamped to 0) but never used it — the per-layer ternary used the raw `interlocking_depth`
as a wholesale REPLACEMENT band on even layers instead. Fixed by using
`interlocking_cut_width` in that ternary (and gating on `interlocking_cut_width > 0.f` rather
than `interlocking_depth != 0.f`, matching upstream PrusaSlicer's `cut_segmented_layers`,
confirmed by fetching the current file from github.com/prusa3d/PrusaSlicer). Also fixed the
lambda's capture list (captures `interlocking_cut_width` by reference instead of the
now-unused `interlocking_depth`).

Effect: even layers now clamp to `band - interlock` (a sub-band notch carved at the INNER
boundary of the claim), not to `interlock` alone. Default config (walls=3, interlock=0.3mm,
band ≈1.3mm): odd layers claim the full ~1.3mm band, even layers claim ~1.0mm — both well
over one external-perimeter width, not the pre-fix ~0.3mm sliver on even layers.

## F2 — legacy migration neutralizes the old key

`handle_legacy_composite` (PrintConfig.cpp:7897) migrated a nonzero
`mmu_segmented_region_max_width` into `{paint_depth_mode=millimeters, paint_depth_mm=value}`
but left the old key untouched at its nonzero value. Since a diff-serialized preset save only
writes keys differing from defaults, a user who explicitly reverted `paint_depth_mode` back to
its default (walls) and saved would drop `paint_depth_mode` from the diff (now equal to
default) while the stale nonzero `mmu_segmented_region_max_width` stayed in the diff forever,
re-arming the `!config.has("paint_depth_mode")` guard on every future load and silently
reverting the walls choice back to millimeters. Fixed by zeroing
`mmu_segmented_region_max_width` (`config.set_key_value(..., new ConfigOptionFloat(0.))`)
immediately after a successful nonzero migration.

## F3 — interface_shells OR extended to PerimeterGenerator's two read sites

T3's `has_bounded_paint_depth()` OR was only applied at PrintObject.cpp's two read sites;
PerimeterGenerator.cpp:622 and :2273 (classic and Arachne one-wall-top/top-surface detection)
also read `object_config->interface_shells` directly and were missed. Fixed by adding a new
`bool has_bounded_paint_depth` member to `PerimeterGenerator` (PerimeterGenerator.hpp), set
by its only construction site (`LayerRegion::make_perimeters`, LayerRegion.cpp) from
`this->layer()->object()->has_bounded_paint_depth()` — PerimeterGenerator itself has no
`PrintObject*` to query directly. Both PerimeterGenerator.cpp sites now read
`object_config->interface_shells || has_bounded_paint_depth`. Off-behavior purity holds:
unpainted/unbounded objects always evaluate `has_bounded_paint_depth` false, so this OR is a
no-op for them (byte-identical, confirmed by verify_paintdepth.sh's unpainted-parity checks).

## F4 (minor) — paint_depth_mm min=0 incoherence

Resolved as an emergent consequence of F1's fix rather than a separate code change: when
`cut_width == 0` (paint_depth_mm=0 in millimeters mode), `interlocking_cut_width =
max(0 - interlocking_depth, 0) == 0` regardless of the interlock setting, so `region_cut_width`
falls through to `cut_width == 0` on EVERY layer (even and odd alike) — the per-layer
`if (region_cut_width > 0.f)` skip then leaves the claim unbounded coherently on both
parities, matching `pdmUnlimited`'s geometry (just without that mode's cheaper early-out at
the outer gate). `min` was deliberately left at 0 rather than raised, with a code comment at
both `paint_depth_mm`'s ConfigOptionDef (PrintConfig.cpp) and `cut_segmented_layers` (F1's
comment, MultiMaterialSegmentation.cpp) documenting why 0 is now intentional and coherent.

## F5 (minor) — stale interlocking tooltip

`mmu_segmented_region_interlocking_depth`'s tooltip claimed it was "ignored if
mmu_segmented_region_max_width is zero" (that key is legacy-parse-only now — the real gate is
`paint_depth_mode`) and "ignored if bigger than max_width" (never enforced in code; F1
clarified the actual saturating behavior instead). Tooltip rewritten to name the real gate and
drop the false "ignored if bigger" claim.

## Test added (F8 gap)

Two new tests, TDD RED-confirmed against pre-fix HEAD (git-stashed the two fix source files,
built, ran, confirmed genuine failures reproducing the exact bugs; unstashed, rebuilt, confirmed
green):

- `tests/libslic3r/test_paint_depth_clamp.cpp`: "walls-mode band width is pinned and the
  interlock sub-band alternates only at the inner edge" — computes the expected band via the
  same `paint_depth_band_mm` formula/inputs production uses (read off the real sliced
  PrintObject), then probes two adjacent (even/odd) layers at three depths: well inside both
  the full and interlock-shrunk band (must be claimed on BOTH parities — this is what F1
  broke), inside the interlock "tooth" itself between `band-interlock` and `band` (claimed on
  ODD only, not EVEN — pins the alternation), and past the full band (unclaimed on both).
  Pre-fix: failed at the first probe (even layer reduced to the ~0.3mm sliver). Post-fix: green.

- `tests/libslic3r/test_paint_depth.cpp`: "reverting to paint_depth_mode=walls and re-saving
  does not re-migrate on the next load" — a config-level load→revert→diff-save→reload round
  trip using bare (sparse) `DynamicPrintConfig` objects, matching Preset.cpp's real preset
  representation (not `full_print_config()`, which would defeat the `!config.has(...)` guard
  entirely and test nothing). Pre-fix: the stale `mmu_segmented_region_max_width=0.8` survived
  into the diff and re-triggered the migration on reload (`paint_depth_mode` ended up
  millimeters instead of walls — genuine bug reproduction). Post-fix: green.

## Verification

- TDD RED (pre-fix HEAD, via `git stash` of the two fix source files): both new tests failed
  with the exact predicted symptoms. Restored fixes, rebuilt: both green.
- `[paintdepth]`: 15 test cases / 84 assertions, all green (was 13/56 baseline + 2 new cases).
- `[chameleon]`: 133 test cases / 605 assertions, all green (unchanged from baseline).
- `ALL_BUILD` (required — PrintConfig.cpp, PerimeterGenerator.cpp/.hpp, LayerRegion.cpp,
  MultiMaterialSegmentation.cpp are all core): exit 0, per BUILD-SLOT RULE (no active
  cl/link/MSBuild processes before starting).
- `spike/verify_paintdepth.sh`: 17/17 on two consecutive runs — unpainted-fixture byte parity
  vs the frozen baseline, determinism, and all config-surface/legacy checks pass, confirming
  F1/F3 (both paint-gated) have zero effect on unpainted objects.

## Self-review (hand-walk, per process)

Default config (paint_depth_mode=walls, paint_depth_walls=3, mmu_segmented_region_
interlocking_depth=0.3), a bounded painted claim, layers 0-3:
- Layer 0 (even): region_cut_width = interlocking_cut_width ≈ band-0.3mm — claim clamped to
  the interlock-notched band, NOT the bare 0.3mm interlock value.
- Layer 1 (odd): region_cut_width = cut_width = full band — claim reaches the full 3-wall band.
- Layer 2 (even), Layer 3 (odd): repeat the same pattern.
Every layer's claim depth equals a band (full or interlock-notched); the interlock sub-band is
carved only at the inner edge of the claim on even layers, never a full-band replacement.
Matches the design intent and the new test's assertions exactly.
