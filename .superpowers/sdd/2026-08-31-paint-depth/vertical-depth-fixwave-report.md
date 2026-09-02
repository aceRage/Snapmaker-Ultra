# Vertical paint-depth alignment — fix wave report

Worktree: `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`.
Base commit reviewed: `41394ce2b4` ("fix(paint-depth): align painted top/bottom shell depth
with the real solid shell").
Source of truth: `.superpowers/sdd/2026-08-31-paint-depth/vertical-depth-fix-review.md`
("the review"). Fixed: C1 (Critical), I1, I2, I3 (Important). Minors (M4/M5/M6/M7/M8) deferred —
none had a one-word comment fix available, so left untouched per the task brief.

All work done directly (no subagent dispatch), file edits via Edit/Write only (no bash
heredocs), fixtures (mesh/STL assets) untouched.

---

## C1 — `*_shell_layers == 0` no longer claims paint depth into a nonexistent shell

**Files:** `src/libslic3r/MultiMaterialSegmentation.cpp` (`effective_shell_layers_by_thickness()`
at the former :1219, and the `top_layers_eff`/`bottom_layers_eff` computation at the former
:1287-1288); `tests/libslic3r/test_paint_depth_clamp.cpp` (the `top_shell_layers=0` test at the
former :600).

**Finding, restated.** The review disproves the premise 41394ce2b4's second headline change
rested on ("a real solid shell was still being built underneath" when `top_shell_layers==0`).
Three independent places in the codebase gate the *entire* shell on a nonzero layer count and
never reach the thickness term when it's 0: `PrintObject.cpp:1965`/`:1994`
(`discover_vertical_shells`'s `if (n_top_layers > 0)` / bottom counterpart),
`PrintObject.cpp:4123-4125` (`discover_horizontal_shells`'s `if (num_solid_layers == 0)
continue;`), and `LayerRegion.cpp:1025-1036` (`prepare_fill_surfaces()` demotes `stTop`/`stBottom`
surfaces to `stInternal`/`stInternalVoid` outright when the count is 0 — there is no solid skin
left to paint, let alone a shell beneath it). `PrintConfig.cpp:6376-6381` states this outright:
thickness only ever *raises* an existing count; "0 means that this setting is disabled."

**Fix applied**, exactly as the review specified:
1. `effective_shell_layers_by_thickness()` now returns `n_layers` immediately when `n_layers <=
   0`, before the thickness walk can raise `effective` above 0.
2. `top_layers_eff` / `bottom_layers_eff` (used for `max_top_layers` / `max_bottom_layers` /
   `granularity` sizing) now gate on `config.top_shell_layers.value > 0` /
   `config.bottom_shell_layers.value > 0` before consulting `layers_for_thickness()`, mirroring
   the helper exactly.
3. Rewrote the two comment blocks that asserted the disproved premise (the "correctly opens when
   layers == 0 but thickness > 0 ... producing a ZERO painted claim despite a nonzero shell being
   built" language) to state the corrected behavior instead.

**Test — inverted, not new.** The existing test (`top_shell_layers=0 with nonzero
top_shell_thickness ...`) previously asserted the painted top surface WAS claimed. Renamed and
inverted to assert `CHECK_FALSE` — the claim must be **absent**, matching pre-41394ce2b4
(correct) behavior — with the test's comment rewritten to cite the three gating sites and explain
why "no claim" is correct, not a regression. Same fixture/config values (layer_height=0.2,
top_shell_layers=0, top_shell_thickness=0.6) — only the assertion polarity and its justification
changed.

**RED (pre-fix, inverted assertion against unfixed production code):**
```
test_paint_depth_clamp.cpp(637): FAILED:
  CHECK_FALSE( any_contains(extruder2_claim_for_layer(*object, top_index), probe) )
with expansion:
  !true
```
**GREEN (post-fix):** passes.

---

## I1 — helper undercounts by one layer when the thickness walk exhausts the object

**Files:** `MultiMaterialSegmentation.cpp` (`effective_shell_layers_by_thickness()`, former
:1226-1237); `test_paint_depth_clamp.cpp` (new test).

**Finding, restated.** `++m` happens before the break test. When the loop breaks, the layer that
triggered the break is already counted by that iteration's `++m`, so `m` is the correct total
depth (surface layer included). When the loop instead runs off the end of the object (`idx < 0`
for top, `idx == num_layers` for bottom) — every visited layer was inside `thickness` and no
break ever fired — `m` counts only the layers strictly below/above the surface, one short of the
true total.

**Fix applied**, exactly as the review specified: track how each loop terminated and add the
missing increment —
```cpp
if (idx < 0)       ++m;   // top: walk exhausted, every layer below is inside `thickness`
if (idx >= num_layers) ++m;   // bottom: same, walk exhausted
```

**Test — new.** `painted bottom claim reaches the object's very last layer when the thickness
walk exhausts (no off-by-one)`. Uses `bottom_shell_thickness=4.0` on a 4mm-tall slab (max real
gap 3.5mm), guaranteeing the walk always exhausts rather than breaks — mirroring
`PrintObject.cpp:2000-2001`'s own generator loop, which in that situation walks every layer down
to (and including) the object's last one. Asserts every layer 0..last is claimed.

*Design note (redesigned once):* the first attempt reused the file's usual 0.1mm-layer, ~40-layer
slab and produced 18 spurious failures, not the intended single off-by-one — the descent loop's
own per-layer inward erosion (`offset -= extrusion_spacing + extrusion_width`, ~0.87mm/layer at
the file's 0.45mm outer wall) shrinks the claimed polygon away from the dead-center probe well
before 40 layers, independent of the I1 bug. Fixed by using a coarser `layer_height=0.5` (8
layers total, ~6mm cumulative erosion, safely inside the 20mm half-width margin) — this produced
a single, correctly-scoped failure at exactly the last layer, as intended.

*Top-direction counterpart deliberately not added:* the review traced that the pre-existing
strict `>` plus `std::max(..., 0)` bound at `MultiMaterialSegmentation.cpp:1495` already makes
layer 0 unreachable through the top descent for any `effective` count once `effective >=` the
surface layer's own index — so the missing `+1` has no observable effect on that path. Hand-
verified this by tracing the clamp arithmetic; a top exhaustion test would pass identically with
or without the I1 fix, i.e. it would not discriminate, so it was omitted rather than added as a
non-discriminating filler test (the review's own Check 7 standard: weak/non-discriminating tests
are flagged as a defect, not a virtue).

**RED (pre-fix, single assertion at the object's last layer):**
```
test_paint_depth_clamp.cpp(723): FAILED:
  CHECK( any_contains(extruder2_claim_for_layer(*object, idx), probe) )
with expansion:
  false
```
**GREEN (post-fix):** passes.

---

## I2 — an empty (geometry-absent) region can no longer inflate another region's claim depth

**Files:** `MultiMaterialSegmentation.cpp` (`layer_color_stat` lambda, former :1436);
`test_paint_depth_clamp.cpp` (new test).

**Finding, restated.** `layer_color_stat` maxes `top_shell_layers`/`bottom_shell_layers` over
every `LayerRegion` returned by `layer.regions()` — but `PrintObjectSlice.cpp:5199-5208` gives
every layer a `LayerRegion` for **every** `PrintRegion` on the object (`layer->m_regions.reserve
(m_shared_regions->all_regions.size())`), whether or not that region has any geometry there. A
region confined to one part of the object (a modifier, or a Z-stacked volume) can therefore
inflate the painted claim's depth on layers it never touches.

**Fix applied**, exactly as the review specified:
```cpp
for (const LayerRegion *region : layer.regions()) {
    if (region->slices.empty())
        continue;               // LayerRegions exist for every PrintRegion on every layer.
    ...
```
`region->slices` is populated per-layer at `PrintObjectSlice.cpp:5229-5235`, before segmentation
runs at `:5267` — valid and free, per the review.

**Test — new.** `a region with no geometry on a layer cannot inflate that layer's painted claim
depth`. Two Z-stacked model-part volumes (same construction pattern as the file's existing
`process_z_interface_cube()`): "lower" (z 0-4mm) carries a per-volume config override
(`top_shell_layers=30`, via `ModelVolume::config.set_key_value`, the same mechanism
`test_mixed_filament.cpp` already uses for per-volume overrides) — a value wildly larger than
anything the test probes; "upper" (z 4-20mm, top cap painted) keeps the stock default
(`top_shell_layers=4`). Asserts the correct 4-layer claim near the top, and `CHECK_FALSE` at
depth 10 (squarely inside "upper", nowhere near "lower") — pre-fix this was wrongly claimed
because "lower"'s inflated setting leaked into the max even on layers "lower" never touches.

**RED (pre-fix, single assertion at the probed depth):**
```
test_paint_depth_clamp.cpp(793): FAILED:
  CHECK_FALSE( any_contains(extruder2_claim_for_layer(*out_object, top_index - 10), probe) )
with expansion:
  !true
```
**GREEN (post-fix):** passes.

---

## I3 — restored non-uniform-layer-height coverage; verified by a real ceil-swap experiment

**Files:** `test_paint_depth_clamp.cpp` only (`slice_capped_slab()` fixture builder + new test).
**No production code changed for I3** — the real per-layer `print_z`/`bottom_z` walk was already
correct (review Check 2: PASS); this finding is a test-coverage gap, not a production bug.

**Finding, restated.** `slice_capped_slab()`'s fixture pins `initial_layer_print_height ==
layer_height` for every caller, which was the suite's only source of non-uniform layer heights.
With that pinned, every one of the four pre-existing vertical-depth cases is exactly reproducible
by a naive `ceil(thickness / layer_height)` — so the report's "walks real print_z, no
uniform-layer-height assumption" claim was asserted but not actually pinned by any test.

**Fix applied:** extended `slice_capped_slab()` with a trailing optional parameter
`initial_layer_print_height` (default `0.` = "unset, use `layer_height`" — preserves every
existing call site's behavior byte-for-byte, since real heights are always `> 0`). Added one new
test that passes an explicit, different value.

**Test — new.** `bottom claim depth follows the real (non-uniform) first-layer height, not a
uniform-layer-height assumption`. `layer_height=0.1`, `initial_layer_print_height=0.2`,
`bottom_shell_thickness=0.6`. Real `bottom_z = 0, 0.2, 0.3, 0.4, 0.5, 0.6` → gaps
`0.2/0.3/0.4/0.5/0.6` → break at `m=5` → correct claim is layers 0-4, **not** layer 5. A uniform
`0.1` assumption computes `ceil(0.6/0.1)=6` and would wrongly claim layer 5 too.

**Ceil-swap experiment (the actual RED evidence for I3), performed and reverted as required:**
Temporarily replaced `effective_shell_layers_by_thickness()`'s real walk (keeping the C1 early
return and the I1 fix's structure untouched, to isolate exactly the walk-vs-formula question)
with `layers_for_thickness()`'s own existing formula — `std::min(int(layers.size()),
int(thickness / h) + 1)` — using `layers[1]->height` as a stand-in for the nominal per-object
`layer_height` (every layer after the first is uniform in every fixture in this file). Rebuilt,
ran `[paintdepth]`:

```
test cases:  22 |  21 passed | 1 failed
assertions: 156 | 155 passed | 1 failed
```

The **single** failure was the new I3 test, at exactly the predicted assertion:
```
test_paint_depth_clamp.cpp(826): FAILED:
  CHECK_FALSE( any_contains(extruder2_claim_for_layer(*object, 5), probe) )
with expansion:
  !true
```
All other 21 cases — including the four pre-existing vertical-depth tests, and the new I1/I2/C1
tests — passed unchanged under the naive formula, confirming by hand-verified construction (not
just assertion) that I3 is the only test in the suite that actually discriminates a real-walk
vs. uniform-height regression. Reverted the swap immediately after capturing this; rebuilt;
confirmed `[paintdepth]`+`[chameleon]` both fully green again (761 assertions / 155 test cases)
before proceeding.

---

## Test summary

| Stage | `[paintdepth]` | `[chameleon]` |
|---|---|---|
| Review's baseline (41394ce2b4) | 19 cases / 122 assertions, all green | 133/605, green |
| RED baseline (tests updated, prod code unfixed) | 22 cases / 156 assertions — **3 failed** (C1, I1, I2; exactly 1 assertion each) | not run (unaffected by test-file-only changes) |
| GREEN (C1+I1+I2 fixes applied) | 22/156, **all green** | 133/605, **all green** |
| Ceil-swap experiment (I3 verification, then reverted) | 22/156 — **1 failed** (only the new I3 test) | not run (temporary experiment, isolated to the walk helper) |
| Final (post-revert) | 22/156, **all green** | 133/605, **all green** |

Net new/changed tests: 1 inverted (C1), 3 new (I1, I2, I3) → 19 → 22 cases, 122 → 156 assertions.

## Build / verify

- `libslic3r_tests` targeted builds: 5 total (RED baseline ×2 — one flawed I1 design, one
  corrected —, GREEN after real fixes, ceil-swap RED, GREEN after revert), all exit 0. Build-slot
  checked (`Get-Process cl,link,MSBuild,cmake`) immediately before each; none ever found
  running work belonging to another process.
- Full `ALL_BUILD` via the scratchpad's `build_next_wt.bat`: **exit 0**, no errors in the log
  (`snapmaker-orca.exe`, `libslic3r_tests.exe`, `fff_print_tests.exe`, `libnest2d_tests.exe`,
  `slic3rutils_tests.exe` all rebuilt/relinked cleanly).
- `spike/verify_paintdepth.sh`, run twice against the ALL_BUILD binary: **17/17 both times**
  (unpainted-fixture byte-parity vs. the frozen baseline, run-to-run determinism, and the three
  config-surface/legacy-migration checks) — unpainted parity holds, as required; expected, since
  none of C1/I1/I2/I3 touch config defaults, the CLI, or unpainted-object behavior.

## Self-review (hand-walk, per the task's binding checklist)

- **(a) `top_shell_layers=0` ⇒ zero painted claim, no paint pushed into sparse infill.** Verified
  by the inverted C1 test (green) and by tracing the code: `effective_shell_layers_by_thickness`
  returns 0 immediately; `top_layers_eff`/`bottom_layers_eff` are 0 for that region regardless of
  thickness; if *every* region on the object has both counts at 0, `max_top_layers ==
  max_bottom_layers == 0` and the entire projection block at the former :1304 never runs — no
  `top_raw`/`bottom_raw` data, no claim anywhere, not even the immediately-painted surface facet
  (matches `LayerRegion.cpp`'s demotion of that surface to plain internal/void).
- **(b) Painted bottom on a thin plate ⇒ claim covers every layer the shell generator
  reaches.** Verified by the I1 test (green): with `bottom_shell_thickness` exceeding what the
  object can ever satisfy, the claim now reaches the object's very last layer, matching
  `PrintObject.cpp:2000-2001`'s own generator loop bound for the same exhausted-walk case.
- **(c) A modifier/absent-geometry region cannot raise another layer's shell count.** Verified by
  the I2 test (green): "lower"'s `top_shell_layers=30` override has zero effect on "upper"'s
  layers once `region->slices.empty()` is checked; the claim stays capped at the correct 4-layer
  depth throughout "upper".

## Deferred (Minors)

M4 (documented looseness, "no defect, for the record" per the review itself), M5 (wording
precision about "exact" vs. geometrically-narrowed), M6 (recompute-once-per-color perf), M7
(unbounded `int(thickness/min_layer_height)` conversion), M8 (`size_t`/`int` wraparound,
correct-but-implementation-defined pre-C++20) — none had a one-word comment fix available under
this task's scope rule, so left untouched, per the task's explicit "Minors: deferred unless a
one-word comment fix" instruction.

## Commit

One commit, `fix(paint-depth):` prefix, touching exactly `src/libslic3r/MultiMaterialSegmentation.cpp`
and `tests/libslic3r/test_paint_depth_clamp.cpp`. Pre-existing untracked files in this worktree
(other `.superpowers/sdd/2026-08-31-paint-depth/*.md` docs, `spike/out/*.gcode` verify-run
output) were left untracked, consistent with their state at the start of this task.
