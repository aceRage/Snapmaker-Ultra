# Taper bound — fix wave report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `65d17c964f`.
Implements Important 1 and Important 2 from `taper-bound-review.md`. Minors deferred
per instruction. Commit: `cfe7fae1df` — `fix(paint-depth): absorb sub-wall-stack base
rings under tapered painted caps (I1), sharpen anti-smear guard's positive probe (I2)`.

## Important 1 — sub-wall-stack base ring under tapered painted caps

**Root cause** (`src/libslic3r/MultiMaterialSegmentation.cpp:1331-1333`,
`exposed_surface_part()`): the early return fires whenever
`reference_layer_idx >= num_layers` — i.e. for every painted flat top/bottom face,
since `layer_idx + 1 == num_layers` at the object's own top layer — and hands back
the whole projected patch with **no clearance test**. The surrounding comment's
claimed invariant ("the base material left at that layer's perimeter is either
nothing at all or at least one wall stack wide — never a sliver") was enforced by
the `diff_ex`/`offset_ex` call on every other path, but not this one. On a painted
flat cap sitting above a taper (a chamfer, fillet, draft angle, or — as here — a
frustum), the propagated claim stays pinned at the cap's own fixed footprint while
each layer's true contour grows underneath it as the descent goes deeper, leaving a
thin, sub-wall-stack annulus of *base* material at the contour on each of those
layers — a class `is_perimeter_compatible` (`Layer.cpp:184`) can never merge away
(painted vs. base regions differ in `wall_filament`) and that no downstream cleanup
catches (`SCALED_EPSILON`/`5*EPSILON`-scale only).

**Fix** (both descent loops, top at `:1652-1676` and bottom at `:1719-1737`, inside
the existing `if (! top_exposed_ex.empty())` / `if (! bottom_exposed_ex.empty())`
blocks, immediately after `last = union_ex(last);`):

```cpp
const float wall_stack = stat.extrusion_spacing + stat.extrusion_width;
ExPolygons base_rest = diff_ex(input_expolygons[last_idx], last);
if (! base_rest.empty()) {
    append(last, diff_ex(base_rest, opening_ex(base_rest, 0.5f * wall_stack)));
    last = union_ex(last);
}
```

This is the review's suggested fix, implemented verbatim (variable names adapted to
this scope: `wall_stack` for the review's `wall_stack`, `input_expolygons[last_idx]`
for `outline[last_idx]`). `diff_ex(base_rest, opening_ex(base_rest, 0.5*wall_stack))`
isolates exactly the parts of the unclaimed base region narrower than one wall
stack (the standard morphological-opening sliver test) and appends them into the
claim; wide base regions (>= one wall stack) and empty remainders both pass through
unchanged.

### RED evidence

New test `"multi_material_segmentation_by_painting: a chamfered/tapered painted top
leaves no sub-wall-stack base ring on the layers below the cap (taper-bound-review
Important 1)"` in `tests/libslic3r/test_paint_depth_clamp.cpp`: `make_square_frustum
(40., 22., 6.)` painted on its **top cap** (facets `{2,3}`) instead of its sloped
walls — the fixture the anti-smear test's own comment called out as missing (the
only prior "paint reaches the silhouette" fixture was the plain 40×40 slab, whose
vertical walls make the taper-below gap exactly zero). At 0.1mm layers / 0.45mm
walls (`top_shell_layers=4`, `top_shell_thickness=0.6` ⇒ 6-layer effective shell),
the frustum's half-width grows 0.15mm per layer below the cap, so the pre-fix base
ring at depths 1–5 is 0.225/0.375/0.525/0.675/0.825mm — all sub-wall-stack
(`w+s = 0.87854mm`).

Built and ran on unfixed HEAD first: **5 of 18 assertions failed**, exactly the 5
depth-1..5 probes (each `CHECK` at a 0.1mm inset from that layer's own true contour)
— the positive control (depth 0) and the no-over-claim guard (depth 6) both passed,
confirming a clean, targeted RED with no incidental breakage. After implementing
the fix and rebuilding: all 18 assertions pass.

### Self-review hand-walk (review's own 45° chamfer example)

Per the review's concrete scenario (0.1mm layers, 0.45mm wall, 45° chamfer ⇒ run =
layer_height = 0.1mm exactly): base rings at L−1…L−5 are 0.15/0.25/0.35/0.45/0.55mm.
`wall_stack = 0.87854mm`, opening radius `= 0.5*wall_stack = 0.43927mm`. A straight
annulus/strip of width `W` is fully erased by `opening_ex(., r)` whenever
`W < 2r = wall_stack`. The largest ring here (0.55mm at L−5) is well under 0.87854mm
(63% of threshold), so all five rings are fully absorbed — **no sub-wall-stack base
ring survives at L−1…L−5 against the painted claim**, matching the required
property. (The frustum test above is actually a tighter case: its largest ring,
0.825mm at depth 5, sits at 94% of the threshold — closer to the boundary than the
review's own literal example — so it was the more discriminating regression guard
to add.) Diff also re-read in full: both loops carry the identical fix, no leftover
scratch artifacts, no unrelated changes.

## Important 2 — anti-smear guard's positive probe sharpened

**Problem**: the anti-smear test's 0.2mm positive probe, against a `pdmWalls`/1-wall
fixture, sat inside both the ~0.45mm vertical surface band *and* that config's own
0.45mm Stage-1 lateral band (`ext_perimeter_width`, `PaintDepth.cpp`'s `pdmWalls`
case with walls clamped to >= 1) — the two coincide exactly at that probe. Any
regression that killed the vertical projection path alone (`slice_mesh_slabs`
re-classifying the sloped facets as `Vertical`, the `max_top_layers` gate closing,
`top_raw` being filtered away, `stat.top_shell_layers` misfiring) would leave the
positive `CHECK` green via the lateral band alone, proving nothing about the path
the test exists to guard.

**Fix**: switched the fixture's config from `paint_depth_test_config(pdmWalls, 1)`
to `paint_depth_test_config(pdmMillimeters, 1)` with
`config.option<ConfigOptionFloat>("paint_depth_mm")->value = 0.15`, and moved the
positive probe from 0.2mm to 0.30mm in. `paint_depth_band_mm(pdmMillimeters, ...)`
returns `mm` directly (`PaintDepth.cpp:13-14`), so the lateral band shrinks to
`[0, 0.15mm]` — clear of the unaffected ~0.45mm vertical surface band. 0.30mm now
sits outside the lateral band and inside the surface band, so only the vertical
path can satisfy it. Confirmed `interlocking_cut_width = max(0.15 - 0.3, 0) = 0`
(`mmu_segmented_region_interlocking_depth` defaults to 0.3), so
`region_cut_width` stays 0.15mm on both layer parities
(`MultiMaterialSegmentation.cpp:1164,1169`) — no even/odd surprise, as the review
predicted.

Rebuilt with both fixes applied: the modified test passes normally (7/7 assertions,
both `CHECK` at 0.30mm and `CHECK_FALSE` at 1.0mm green).

### Falsification re-run (both outcomes)

Re-ran the report's own falsification experiment against the **modified** test:
scratch-commented both `offset -= (stat.extrusion_spacing + stat.extrusion_width);`
lines (one per descent loop, `:1649` and `:1720` at the time), rebuilt, ran the
anti-smear test alone.

**Result: the test FAILS** — 6/7 assertions pass, 1 fails:
`CHECK_FALSE(any_contains(..., probe))` at the 1.0mm negative probe now evaluates
`!true` (the deep full-width smear reaches the negative probe, exactly the
regression the erosion exists to prevent). The positive probe (0.30mm) still
passes, as expected (an even deeper, unbounded claim trivially still covers it).

Both outcomes the task asked for, stated explicitly:
1. **It remains the anti-smear proof.** The identical falsification (erosion
   deleted) that failed the original test still fails the modified test, via the
   same negative-probe mechanism — Important 2's changes did not weaken this
   guarantee.
2. **It now also fails if the vertical path dies** (a distinct failure class from
   erosion-deletion — e.g. `top_raw` never populated, the shell-depth gate
   misfiring to 0, etc., leaving layer 10 with *no* vertical contribution at all).
   This is guaranteed by construction rather than by a second falsification build:
   the Stage-1 lateral band is hard-capped at `paint_depth_mm = 0.15mm`
   (`paint_depth_band_mm`'s `pdmMillimeters` case returns `mm` verbatim — not
   subject to any other config value), and the positive probe now sits at 0.30mm,
   strictly outside `[0, 0.15mm]`. If the vertical path contributed nothing, the
   remaining claim at layer 10 would be exactly the lateral band, and 0.30mm would
   fall outside it — `CHECK` would go red. Pre-fix (0.2mm probe / 0.45mm lateral
   band under `pdmWalls`/1) this same hypothetical would have stayed green, which
   is precisely the defect Important 2 identified. Reverted the scratch deletion
   immediately after this build (`git diff` confirmed byte-clean, no leftover
   `SCRATCH-FALSIFICATION` markers); the committed source contains only the
   Important 1 fix.

## Minors

Deferred, per instruction (Minor 1: report's 0.2mm-layer angle figure; Minor 2:
early-return conditions fail open; Minor 3: "flat cap keeps exact footprint" not
unconditional near nearby vertical features; Minor 4: commit references an
uncommitted report path). None required for this fix wave.

## Verification

| Gate | Result |
|---|---|
| Important 1 RED (unfixed HEAD, new frustum-cap test) | 5/18 assertions failed (exactly depths 1–5), as predicted |
| Important 1 GREEN (fix applied) | 18/18 assertions pass |
| `[paintdepth]` full suite | **277 assertions in 30 test cases**, all pass (was 259/29; +1 case, +18 assertions — exactly the new test) |
| `[chameleon]` full suite | **605 assertions in 133 test cases**, all pass — unchanged from baseline, confirms both fixes inert there |
| Anti-smear test, normal build (both fixes applied) | 7/7 assertions pass |
| Anti-smear test, falsification build (erosion scratch-deleted) | 6/7 pass, 1 fails (negative probe) — confirms it's still the anti-smear proof |
| `build_next_wt.bat` (ALL_BUILD via scratchpad wrapper) | exit 0, `snapmaker-orca.exe`, `libslic3r_tests.exe`, `fff_print_tests.exe` etc. all rebuilt cleanly (only pre-existing, unrelated `LNK4098`/`LNK4286` warnings) |
| `spike/verify_paintdepth.sh` run 1 | **17/17 checks passed, RESULT: ALL PASS** |
| `spike/verify_paintdepth.sh` run 2 | **17/17 checks passed, RESULT: ALL PASS** — `unpainted-run1-vs-baseline` / `unpainted-run2-vs-baseline` byte-identical both runs, unpainted parity holds |

## Process notes

- BUILD-SLOT RULE observed throughout: checked `Get-Process cl,link,MSBuild` before
  every build; one cycle found an active `cl.exe` (real compile work in progress)
  and the turn was ended with "WAITING FOR BUILD SLOT" rather than starting a
  competing build; resumed once the coordinator confirmed the slot was free.
- Iterative RED/GREEN/falsification cycles built only the `libslic3r_tests` target
  (via `cmake --build . --config Release --target libslic3r_tests`, same toolchain
  invocation as the scratchpad wrapper) for turnaround speed; the full `ALL_BUILD`
  gate ran once, as its own required step, before the spike verification.
- Fixtures untouched. All work done directly (no subagents dispatched), per
  instruction.
- One commit: `cfe7fae1df`, prefix `fix(paint-depth):`.
- `.superpowers/sdd/2026-08-31-paint-depth/progress.md` remains modified and
  uncommitted in the working tree — pre-existing from a previous wave (per the
  review's Check 6 residual note), not touched by this fix wave and not included
  in this commit.

## Concerns

None outstanding for Important 1 / Important 2. The three Minors and the curved-
surface opacity gap (review's Check 5, ~6.5°–27° slopes) are explicitly out of
scope for this wave per instruction.
