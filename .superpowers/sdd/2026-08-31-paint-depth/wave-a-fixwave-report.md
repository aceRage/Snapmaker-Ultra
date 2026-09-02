# Wave A fix wave — C-1, I-1, I-2, I-3, I-4

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, parent `b1075d660a` (Wave A +
Wave B). Scope: `wave-a-review.md`'s C-1 (Critical) and I-1..I-4 (Important). Minors deferred, as
instructed. All anchors located by reading the current file, not by trusting the review's line
numbers (Wave B shifted them).

---

## C-1 — `LayerTools::wall_filament` now applies the same grouped-manual-pattern resolution as `sparse_infill_filament`/`solid_infill_filament`

**Files**: `src/libslic3r/GCode/ToolOrdering.hpp`, `src/libslic3r/GCode/ToolOrdering.cpp`.

Added a private helper `LayerTools::resolve_grouped_or_mixed_1based(region, id)` — grouped
resolution first (`grouped_manual_pattern_infill_filament_1based`), falling back to
`resolve_mixed_1based` — and routed all three of `wall_filament`, `sparse_infill_filament`,
`solid_infill_filament` through it (they were three copies of nearly the same body; now one).
Before, `wall_filament` called `resolve_mixed_1based` directly, skipping the grouped step the
other two already applied.

**RED.** New test `LayerTools::wall_filament applies the same grouped-manual-pattern resolution
as sparse_infill_filament (C-1)` (`[paintdepth]`, `test_paint_depth_clamp.cpp`). Smallest honest
reproduction: no mesh painting at all — the divergence is a pure property of the two resolver
functions given the same configured id. Built a `MixedFilamentManager` with one custom mixed row
(component_a=1, component_b=2, `manual_pattern="12,21"`), routed it through `Print::apply`'s real
`mixed_filament_definitions` config path (with `MixedFilamentManager::set_auto_generate_enabled
(false)` for the test's duration, so the auto-generated (1,2) pair doesn't collide with the
custom row and shift its virtual id), set `wall_filament = sparse_infill_filament = 3` (the
grouped virtual id) on a plain unpainted 20×20×4mm cube, sliced it, and called
`LayerTools::wall_filament(region)` / `sparse_infill_filament(region)` directly on the real
`PrintRegion` — exactly what `GCode.cpp`'s `configured_extruder_id` lambda does for a gap-fill
collection vs a real infill collection.

RED, pre-fix: `wall_filament(region) == 0`, `sparse_infill_filament(region) == 1` — different
physical extruders for the same configured id, confirmed by the actual failing assertion
(`0 == 1`). GREEN post-fix: both resolve to `1` (component_b, via the grouped path — wall_loops
defaults to 2, so `innermost_perimeter_index = 1` selects manual-pattern group `"21"`, whose
layer-0 token is `"2"`).

Not expressible/needed: a full G-code-level "gap fill on grouped pattern, no paint" fixture would
be strictly redundant with this — `fill_filament_source(cfg, erGapFill) == FillFilamentSource::
Wall` is already pinned by the existing sibling test, and `configured_extruder_id`'s Wall arm
calls `LayerTools::wall_filament` verbatim (confirmed by reading GCode.cpp:5217), so this test
exercises the actual production function the review names.

---

## I-1 — the classic floor now closes the void ring on both layer parities

**Files**: `src/libslic3r/PaintDepth.hpp/.cpp` (new `paint_depth_classic_notch_cap_mm`),
`src/libslic3r/MultiMaterialSegmentation.cpp` (call site in
`multi_material_segmentation_by_painting`).

The classic floor (`paint_depth_band_classic_floor_mm`) guarantees `cut_width >= wall_stack`, so
the odd-layer band (passed through `cut_segmented_layers` untouched) reaches wall_stack exactly.
Even layers additionally subtract the interlocking notch
(`interlocking_cut_width = cut_width - interlocking_depth`), which the floor — computed earlier,
before `interlocking_depth` is even known — could not account for. Fix: **cap the notch itself**
(not the band) at whatever slack the already-floored band has above wall_stack:
`interlocking_depth' = min(interlocking_depth, max(0, cut_width - wall_stack))`.

I first tried the mirror-image fix (raise `max_width`/`cut_width` by `wall_stack +
interlocking_depth`), per the review's own literal wording ("Floor the classic band at wall_stack
+ interlocking_depth"). That is **wrong**: `cut_width` also feeds the ODD-layer band directly
(`cut_segmented_layers` passes it through untouched), so raising it widens odd layers too, for no
reason — caught by the pre-existing "walls-mode band is floored at one wall stack on the classic
generator only" test regressing (0.85708mm → 0.95708mm reach, past that test's own upper-bound
probe at 0.95mm). Capping the notch instead leaves `cut_width` — and the odd-layer band — provably
untouched, since `interlocking_depth` is never subtracted from it.

**RED.** New test `the classic floor closes the void ring on BOTH layer parities, not odd ones
only (I-1)` (`[paintdepth]`). 40×40×6mm box, `PLUS_X_FACE` painted, `pdmWalls`, `walls=1`, 0.1mm
layers, Classic — the review's own worked numbers (`s=0.428540`, `wall_stack=0.878540`,
default notch `0.1` uncapped). Probes at `wall_stack ± 0.05mm` on both an even and an odd layer
index. RED pre-fix: even-layer probe at `wall_stack - 0.05` not claimed (even-layer reach was
`0.778540`, short by exactly the notch); odd layer already correct. GREEN post-fix: both parities
reach `wall_stack` (notch capped to 0 at `walls=1`, since the floored band has zero slack above
wall_stack there — see below).

This test pins the arithmetic invariant the review's defect is built on (the floor's promised
reach not actually holding on even layers), not the downstream "classic prints the ring as
nothing" G-code consequence directly — that would need per-role toolpath inspection of a
sub-millimetre annulus, which this suite has no existing harness for, and which is moot once the
segmentation-level boundary itself is correct (no G-code has anything to be wrong about a gap
that no longer exists).

**Consequence worth naming, not fixed unilaterally**: at `paint_depth_walls = 1` the floored band
already equals `wall_stack` exactly (zero slack), so the notch is now capped to **0** there — a
mechanical interlocking tooth is not printable at the narrowest classic band at all, since
carving one needs *more* than one wall_stack of unpainted material at the exact point the floor
guarantees only one. This is the correct consequence of the invariant, not an incidental side
effect: the floor and the notch made contradictory demands on the same millimetre and something
had to give; giving up the notch (rather than the floor, or the intended even-layer band width)
is the only option that doesn't regress anything else.

---

## I-2 — the degradation ladder's membership no longer alternates with parity

**File**: `src/libslic3r/MultiMaterialSegmentation.cpp` (`paint_depth_clamp_keep_core`).

Wave A moved only the ladder's *step size* (`b = 0.25 * ladder_band`) to the un-notched band;
`core`/`thin` — whether a part is thick enough to skip the ladder *at all* — still read the
(possibly notched) `band`. Fixed: `core_full`/`thin` are now computed from `ladder_band`
(parity-independent) throughout; the actual baseline erosion for non-degraded geometry still uses
the real (possibly notched) `band` (reusing `core_full` when they're equal, i.e. every layer where
the notch isn't currently narrowing this call, to avoid a second erosion). Only *whether* a part
counts as degraded is now parity-independent — the interlocking tooth's intentional alternation on
thick geometry is untouched.

**RED.** New test `the degradation ladder's membership does not alternate with parity at walls =
5 (I-2)` (`[paintdepth]`). 4.40×40×6mm bar (half-thickness 2.20mm), `ALL_SIDE_FACE`, `walls=5`
(stock flow: `band=2.249835`, `band_even=2.149835`, `b0=0.562459 ≥ min_claim_width` — armed with a
comfortable margin over the review's own `walls≥4` threshold), 0.1mm layers, Arachne. 2.20mm sits
inside the narrow `(band_even, band)` window this defect needs. RED pre-fix: even layer gave the
full `2.149835mm` reach (not classified as thin under the narrower notched threshold), odd layer
gave the ladder-degraded `~0.562mm` — a probe at 0.6mm inset was claimed on even, not on odd.
GREEN post-fix: both parities give the identical degraded `~0.562mm` reach.

---

## I-3 — `collect_extruders` buckets gap fill the same way emission does

**File**: `src/libslic3r/GCode/ToolOrdering.cpp` (`ToolOrdering::collect_extruders`).

Replaced the bespoke role-check (`internal_solid_infill_uses_sparse_filament` /
`is_solid_infill` / else-sparse) with a `switch` on `fill_filament_source(region.config(), role)`
— the same function GCode.cpp's emission now uses. `FillFilamentSource::Wall` (gap fill)
contributes nothing to `has_solid_infill`/`has_sparse_infill`, since the region's perimeters loop
already registers `wall_filament` for that region when it has any.

**RED, and a real debugging detour worth recording.** New test `ToolOrdering::collect_extruders
does not add the base filament to a layer whose only fill content is gap fill (I-3)`
(`[paintdepth]`). Fixture: a 20×2.4×4mm `ALL_SIDE_FACE`-painted bar — walls-mode `band(3) =
1.435675mm` exceeds half the 2.4mm short side, so the paint claim covers the whole cross-section
(no base-coloured region exists on the probed layer at all); two classic wall loops leave a
~0.64–0.69mm residual that classic fills with gap fill, not sparse infill (confirmed: `gap_fills
== 1`). `paint_infill_override=false` keeps `sparse_infill_filament` on the base colour.

First pass at the assertion checked `layer_tools.extruders` for a literal `1` and got confusing
results after the I-1 redesign (extruder "1" present when I expected only "2"). Traced it with a
temporary production-code diagnostic (since reverted) into `ToolOrdering::reorder_extruders`:
`LayerTools::extruders` is genuinely **zero-based** in its final, post-`reorder_extruders` state
("Reindex the extruders, so they are zero based, not 1 based", ToolOrdering.cpp) — the header
comment I'd dismissed earlier as stale was correct; I had the indexing backwards in my first
assertion. Fixed the test to check for `0u` (base, 0-based) rather than `1u` (which is the
*painted* extruder, 0-based). The production fix was correct throughout this detour; only the
test's own indexing assumption was wrong.

RED pre-fix (re-derived from the very first diagnostic run, before any I-3 production fix):
`layer_tools.extruders` (1-based, pre-reindex) held both `1` (base — spurious, from gap fill
mis-bucketed as sparse infill) and `2` (painted — legitimate, from the perimeters loop); after
reindexing that is `{0, 1}`, and `has_base_extruder` (checking for `0u`) is true. GREEN post-fix:
`layer_tools.extruders == {1}` only (0-based — physical filament 2, the painted one); base (`0u`)
absent. A positive-control `REQUIRE(has_painted_extruder)` guards against a fixture that silently
stopped producing gap fill or perimeters and made the main assertion pass for the wrong reason.

---

## I-4 — tooltip corrected; mm-mode recommendation

**File**: `src/libslic3r/PrintConfig.cpp` (`mmu_segmented_region_interlocking_depth` tooltip).

The tooltip stated the quarter-perimeter-spacing cap unconditionally. Corrected to state it is
walls-mode only ("Limited by walls"), and added that in "Limited by distance" mode the configured
value is honoured exactly as entered, with a caution that setting it large relative to
`paint_depth_mm` can reintroduce a loop-count difference between alternating layers.

**Recommendation on whether mm mode should *also* clamp: no, keep it uncapped, as coded.** The
existing design rationale (`PaintDepth.hpp`'s header for `paint_depth_interlocking_depth_mm`) is
sound and the review's own "CONCERN" framing agrees it's coherent, just under-documented: mm mode
publishes no N-loop contract, the band is the user's literal chosen depth, and capping it would
silently shrink a deliberately-set mechanical key (0.5mm → 0.107mm) for a bead-count guarantee
that mode doesn't make. The failure mode requires a user to *deliberately* raise the notch well
past its 0.1mm default in a *non-default* mode — that's a legitimate, disclosed trade-off, not a
default-path regression. If this is revisited, the shape I'd reach for is a **separate, mm-mode-
specific cap relative to the user's own `paint_depth_mm`** (e.g. a quarter of it), not reuse of the
walls-mode perimeter-spacing cap, since mm mode's band isn't spacing-derived at all — but that is
a new semantic decision, not a bug fix, and is out of this fix wave's scope.

---

## Full-suite result, honestly

**`libslic3r_tests`, no filter, default binary, `--order rand --warn NoAssertions`:**

```
test cases:   482 |   478 passed | 2 failed | 2 failed as expected
assertions: 50808 | 50804 passed | 2 failed | 2 failed as expected
exit code: 2
```

482 = Wave B's 478 baseline + this wave's 4 new test cases (C-1, I-1, I-2, I-3). The 2 real
failures are the SAME pre-existing ones Wave A/B recorded (`test_mixed_filament.cpp` "m1: compute
uses num_physical bound..." and "batch_remap mixed pair-fallback... (KNOWN bug)" — confirmed by
name match, not just line number, since this wave's edits shifted line numbers in that file by
+14). Neither is touched by this wave. **This is not "clean" — exit code is 2, matching the task's
own stated baseline, not rounded down.**

**One test in `test_mixed_filament.cpp` needed updating, not just this wave's own new tests.**
"Grouped manual wall patterns make infill follow the innermost perimeter tool" pinned
`wall_filament()`'s *pre-C-1* behavior as if it were correct (`layer1.wall_filament(region) ==
1`, the flattened-pattern reading that ignores the manual-pattern grouping). That is exactly the
divergence C-1 fixes; updated the expectation to `0` (the grouped reading, matching
`solid_infill_filament`'s already-correct value on the same id/region) and renamed the test to
describe the new, consistent behavior, with a comment explaining why.

---

## Gates

| Gate | Result |
|---|---|
| `[paintdepth] --order rand` | **727 assertions in 54 test cases, all pass** (baseline 682/50 + this wave's 45/4) |
| `[chameleon] --order rand` | **605 assertions in 133 test cases, all pass** — exactly at baseline |
| full `libslic3r_tests` | 482 cases, 478 passed, 2 failed (pre-existing), 2 failed-as-expected, **exit 2** |
| `ALL_BUILD` (scratchpad `build_next_wt.bat`) | **exit 0**, zero errors, twice (once before, once after a post-review comment fix) |
| `spike/verify_paintdepth.sh` ×2 | **17/17 ALL PASS**, both runs, including `unpainted-run{1,2}-vs-baseline` byte-identical (normalized) — unpainted parity holds |

## Self-review (hand-walk)

- **Grouped-pattern print with no paint ⇒ gap fill unchanged from pre-Wave-A.** Verified directly
  by the C-1 test (no mesh painting at all) and indirectly by `verify_paintdepth.sh`'s unpainted
  byte-parity gate staying green (C-1/I-3 only change resolution paths that are no-ops when
  `wall_filament == sparse_infill_filament`, which is every non-mixed and every non-grouped-
  pattern print).
- **Classic walls=1 painted cap ⇒ no void ring on either parity.** Verified by the I-1 test:
  both an even and an odd layer index reach `wall_stack` from the boundary, within a 0.05mm
  margin on both sides.
- **Walls=4 ⇒ claim width equal on adjacent layers.** Verified by the I-2 test at walls=5 (a
  larger, more comfortably-armed margin over the review's own walls≥4 threshold; the mechanism is
  identical): both parities give the same ~0.562mm degraded claim.

## Commit

One commit, `fix(paint-depth): close the classic-floor void ring on both parities, kill the
ladder's parity alternation, unify grouped-pattern filament resolution, and align tool-ordering
with emission (C-1, I-1, I-2, I-3, I-4)`. Files: `src/libslic3r/GCode/ToolOrdering.{hpp,cpp}`,
`src/libslic3r/MultiMaterialSegmentation.cpp`, `src/libslic3r/PaintDepth.{hpp,cpp}`,
`src/libslic3r/PrintConfig.cpp`, `tests/libslic3r/test_paint_depth_clamp.cpp`,
`tests/libslic3r/test_mixed_filament.cpp`.
