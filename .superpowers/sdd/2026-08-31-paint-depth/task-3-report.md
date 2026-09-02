# Task 3 — Stage 2 vertical bleed

Worktree: C:\Dev\SnapmakerOrcaNext, branch feat/paint-depth, base 89d3f15d44 (Task 2)

## Status: COMPLETE

Both work items are implemented, `[paintdepth]` (13 test cases / 56 assertions, stable
across repeated runs) and `[chameleon]` (133 test cases / 605 assertions, byte-identical to
Task 1/2's baseline) are green, and the full `ALL_BUILD` gate passed exit 0. A real-compile
build slot was free on both checks (`Get-Process cl,link,MSBuild` returned no matches), so
no waiting was required.

## Implementation

### 1. Z-interface solid skin (bleed path (c))

**Read first**: `interface_shells` (`PrintObjectConfig`, `PrintObject.cpp`) is a single
object-level bool read directly in exactly two places — `detect_surfaces_type()` (:1317,
gates whether top/bottom surface detection diffs against per-region `slices.surfaces`
instead of whole-layer `lslices`) and `discover_vertical_shells()` (:1746, mirrors the same
choice for vertical-shell-thickness computation). Both call sites are otherwise symmetric:
a region boundary only gets solid skin (instead of being absorbed into `stInternal`) when
`interface_shells` is true. There is no finer-grained flag anywhere in the data model that
distinguishes *why* two regions differ (paint vs. an unrelated modifier/volume) —
`LayerRegion::slices` surfaces carry no such provenance.

**Chosen form: flip the effective `interface_shells` value for the object**, not a scoped
reclassification. Added `PrintObject::has_bounded_paint_depth()` (`Print.hpp`, beside
`is_mm_painted()`/`is_fuzzy_skin_painted()`): `is_mm_painted() && paint_depth_mode !=
pdmUnlimited`. Both `PrintObject.cpp` call sites now OR this into the raw config read:
```cpp
bool interface_shells = ! spiral_mode && (m_config.interface_shells.value || this->has_bounded_paint_depth());
// discover_vertical_shells():
bool top_bottom_surfaces_all_regions = this->num_printing_regions() > 1 && ! (m_config.interface_shells.value || this->has_bounded_paint_depth());
```

**Trade documented**: this activates solid skin at *every* region boundary in an affected
object (paint-caused or not — e.g. an unrelated modifier-volume boundary would also gain
solid skin), not only paint color boundaries. A scoped alternative (reclassify only
paint-caused boundaries) would need to plumb "why did this region differ" provenance
through `LayerRegion`/`Surface`, which does not exist today — a materially larger change
for a benefit (avoiding solid skin on a *non-paint* boundary in an object that also happens
to be MM-painted with bounded depth) the spec doesn't ask for. The gate itself
(`is_mm_painted() && bounded`) keeps the blast radius to exactly the objects the plan
requires — an unpainted object, or a painted object in `unlimited` mode, is provably
unaffected: `has_bounded_paint_depth()` is `false`, so `interface_shells`'s effective value
is byte-identical to before this task.

### 2. `paint_infill_override`

- New `coBool` option (`PrintConfig.hpp`/`.cpp`, `PrintObjectConfig`, beside
  `paint_depth_mm`), default `true` (today's behavior).
- Applied at the region-override site — but that site exists in **two** places, both of
  which had to change together: `generate_print_object_regions()` (PrintApply.cpp, builds
  a fresh `PrintObjectRegions` from scratch) *and* `verify_update_print_object_regions()`
  (the fast region-reuse path that runs on **every** `Print::apply()` call when the cached
  regions are still considered valid). Missing the second one would mean toggling the
  checkbox alone — with no accompanying geometry change — silently did nothing, because
  the cached `PrintRegion::config()` would never get re-diffed. Both now take a
  `paint_sparse_infill` bool and only set `cfg.sparse_infill_filament.value =
  painted_extruder_id` when it's true; wall/solid always stay painted.
- **Gating decision**: `paint_sparse_infill` at the `Print::apply()` call site is computed
  as `paint_infill_override.value || paint_depth_mode == pdmUnlimited` — i.e. the override
  only has an effect once paint depth is actually bounded. This wasn't explicit in the
  design doc's item 2 text (which doesn't literally say "only in bounded mode"), but the
  Work Items' explicit UI instruction ("greyed unless a bounded mode") only makes sense if
  the underlying behavior matches the greying, so I gated both together — a judgment call,
  flagged here for review. In `unlimited` mode the box is greyed *and* inert.
- Preset registration (`Preset.cpp`), Tab.cpp UI row (Multimaterial > Advanced, beside
  `paint_depth_mm`), `ConfigManipulation.cpp` greying, and `PrintObject.cpp`'s posSlice
  invalidation group (joins `paint_depth_mode`/`walls`/`mm` per the task instructions) are
  all wired.
- The pre-existing "Painted region filament mismatch" sanity-check log line in
  `generate_print_object_regions()` was updated to compare `sparse_infill_filament` against
  an `expected_sparse_infill_filament` (painted id when override active, else whatever the
  parent/base config already had) instead of unconditionally `painted_extruder_id`, so it
  doesn't fire a false-positive warning when the override is off.

## Testing (reused/extended `tests/libslic3r/test_paint_depth_clamp.cpp`, `[paintdepth]`)

**paint_infill_override (3 new cases, region-config level — the right level per the plan,
since the option's effect is a config decision, not clamped geometry):**
- `paint_infill_override=false` → `extruder2_region_config()` (new helper: finds the
  painted `PrintRegion` by `wall_filament==2`, returns its `PrintRegionConfig`) shows
  `wall_filament==2`, `solid_infill_filament==2`, **`sparse_infill_filament==1`** (base).
- `paint_infill_override=true` (default) → all three `==2`.
- `paint_infill_override=false` in `pdmUnlimited` → `sparse_infill_filament==2` (the
  gating: override is a no-op outside bounded mode, matching the UI greying).

**Z-interface (2 new cases) — unit-expressible, gold-standard end-to-end, not a hand-walk.**
Investigated whether surface classification is inspectable on the harness's sliced
`PrintObject`: `detect_surfaces_type()`/`discover_vertical_shells()` run at the private
`prepare_infill()` step (`posPrepareInfill`), which `PrintObject::slice()` (what Task 2's
harness calls) does not reach — only the public `Print::process()` pipeline gets there
(`make_perimeters()`/`prepare_infill()`/`infill()` are private, callable only via `Print`'s
friendship). Built a new fixture, `process_z_interface_cube()`: two stacked 40×40×10mm cube
**ModelVolumes** in one `ModelObject` (lower z 0-10 unpainted, upper z 10-20 with its +X
face painted Extruder2) — this creates a genuine color Z-interface at z=10 along the +X
wall *without* needing a custom subdivided mesh (each `make_cube()` side facet spans its
own volume's full height as one triangle, so partial-height painting on a single cube isn't
otherwise expressible). Ran the object through `print.process()` (not just `slice()`), then
inspected `layerm->slices.surfaces` types on the extruder2 region's first layer above z=10
via a new `extruder2_layer_has_solid_skin()` helper (any surface type other than
`stInternal`). `print.process()` worked cleanly against `full_print_config()` defaults, no
crashes, ~17ms per test.
- Walls mode (bounded): the extruder2 region's first layer above the boundary **has** solid
  skin.
- Unlimited mode: it does **not** (documents the pre-existing bleed-path-(c) bug that
  `pdmUnlimited` deliberately preserves).

**Real TDD RED observed, twice (via targeted `git stash push -- <file>` / `git stash pop`,
keeping the rest of the diff — including the tests — in place):**
1. Stashed only `PrintApply.cpp`: the `paint_infill_override=false` test failed
   (`sparse_infill_filament.value == 1` got `2`) against the pre-Task-3 region-override
   code — the other 10 then-existing tests stayed green, confirming the RED was specific to
   the new code path.
2. Stashed only `Print.hpp` + `PrintObject.cpp`: the "bounded color Z-interface gets solid
   skin" test failed (`extruder2_layer_has_solid_skin(...) == false`) against the
   pre-`has_bounded_paint_depth()` code, with the paired "unlimited... no solid skin"
   documentation test still passing (both pass-through paths correctly still absent).
Restoring each stash (`git stash pop`) turned both back to real GREEN, rebuild confirmed.

**Regression checks**: `[chameleon]` 133/605 unchanged (config/PrintObject/PrintApply
changes are shared infra). `[MixedFilament]` (broader check on the shared
`generate_print_object_regions`/`verify_update_print_object_regions` signature changes):
181 test cases, 178 passed / 1 failed / 2 failed-as-expected — identical counts to Task 2's
report, confirming no new regression from threading `paint_sparse_infill` through both
functions.

## Self-review (hand-walk, per the task's binding review criteria)

- **Brown claim in yellow body, `override=false` → brown walls+solid, yellow sparse**:
  confirmed directly by the region-config tests — `extruder2_region_config()` after
  `override=false` shows `wall_filament=2, solid_infill_filament=2,
  sparse_infill_filament=1` (base/yellow).
- **Z-top of the claim → solid skin above**: confirmed by the z-interface end-to-end test —
  a real painted mesh, sliced and run through the full `process()` pipeline, shows the
  painted region's first layer above the color boundary is typed as a real
  top/bottom-family surface (not `stInternal`) only when `has_bounded_paint_depth()` is
  active; the paired `pdmUnlimited` test confirms the pre-existing bug is otherwise
  untouched.

## Concerns / notes for reviewers

- Item 1's chosen form (object-wide `interface_shells` OR, not a scoped per-boundary
  reclassification) is a documented trade, not a limitation I consider risky: it can only
  ever ADD solid skin, never remove it, and only for objects that are both MM-painted and
  in a bounded mode — an explicit, narrow, and easily-reasoned-about activation condition.
- Item 2's "gate the override on bounded mode" is a judgment call (see above) inferred from
  the UI-greying instruction rather than stated outright in the design doc's Stage 2(b)
  text. If a reviewer wants the override to apply unconditionally (including in
  `pdmUnlimited`), that's a one-line change (drop the `|| paint_depth_mode == pdmUnlimited`
  disjunct from `paint_sparse_infill`'s computation in `Print::apply()`) plus removing the
  matching UI-greying line and the "no-op in unlimited mode" test's assertion — flagging
  for visibility rather than treating it as settled.
- Did not attempt to extend the #6892 thin-feature fixture (Task 2's fenced item) — out of
  this task's scope.
- `print.process()` for the z-interface fixture completed cleanly against
  `full_print_config()` bare defaults (no bed/skirt/wipe-tower config beyond what Task 2's
  harness already sets) — I did not stress-test it against more exotic printer profiles,
  since the test's only goal is to reach `posPrepareInfill` for two small painted cubes.

Report path: `.superpowers/sdd/2026-08-31-paint-depth/task-3-report.md`
