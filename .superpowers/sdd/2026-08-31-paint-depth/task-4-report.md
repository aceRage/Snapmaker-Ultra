# Task 4 — Verification + GUI handoff

Worktree: C:\Dev\SnapmakerOrcaNext, branch feat/paint-depth, base 7579272f9d (Task 3)

## Status: COMPLETE

`spike/verify_paintdepth.sh` is written and passes 17/17 checks on two consecutive runs.
`[paintdepth]` (13 test cases / 56 assertions) and `[chameleon]` (133 test cases / 605
assertions) both pass on two consecutive `ctest`/direct-binary runs. No C++ was touched —
this task is verification/spike-script only, so the BUILD-SLOT RULE's "no build" path
applies; the only compilation done was a throwaway baseline binary at the feature's
merge-base commit, in a temporary worktree that has since been removed (see below).

## 1. `spike/verify_paintdepth.sh`

New file: `spike/verify_paintdepth.sh`. Modeled directly on the supports feature's
`C:\Dev\SnapmakerOrcaSupports\spike\verify_chameleon.sh` (read first, per the task):
reused its `normalize()` sed/awk pipeline verbatim (timestamp/M73/filament-used/id-suffix
strips + `normalize_strip_slivers.awk`'s degenerate-solid-infill-sliver filter — this
worktree's copy of that script, `spike/normalize_strip_slivers.awk`, was already present
via the prior `Merge feat/color-matched-supports` merge into this branch's history), and
its `run_slice`/`record`/report-table helper shape. Empirically confirmed the sliver strip
is still needed here: a raw `diff` of two back-to-back identical-command slices of
`cube30.stl` differs; the normalized diff is empty.

Three fixtures created (`spike/spike_paintdepth_*.json`, all inheriting
`0.20mm Standard @BBL X1C` against `Bambu Lab X1 Carbon 0.4 nozzle`, matching the
chameleon fixtures' shape):
- `spike_paintdepth_overrides.json` — no paint-depth keys at all (loads unmodified on
  both the pre-feature baseline binary and the current one).
- `spike_paintdepth_legacy_nonzero.json` — literal old key
  `mmu_segmented_region_max_width = 0.8`.
- `spike_paintdepth_legacy_zero.json` — literal old key
  `mmu_segmented_region_max_width = 0` (present but zero).

### (a) UNPAINTED-fixture inertness

Two signals, both against `cube30.stl` (unpainted, no `mmu_segmentation_facets`):

1. **Byte-parity (normalized) vs a frozen pre-feature baseline** —
   `spike/out/paintdepth_baseline.gcode`.
2. **Determinism** — two consecutive current-binary slices, normalized-identical.

**Baseline provenance (honest procedure, documented in the script's own header
comment):** confirmed `c345859f55` (the design-spec commit, this branch's plan-declared
BASE) is byte-identical in source to the commit immediately preceding Task 1's first code
change (`git diff --stat c345859f55 ed726d71e9~1 -- . ':!docs' ':!.superpowers'` returned
empty — the two intervening commits touch only `docs/superpowers/`). Used
`git worktree add ../SnapmakerOrcaBaseline c345859f55` to get a clean pre-feature source
tree without disturbing this worktree or any other checkout, configured cmake identically
to Task 1's setup (`CMAKE_PREFIX_PATH` against the main checkout's deps, VS17/x64,
Release), and built **only the app executable target** (`Snapmaker_Orca_app_gui`, not the
full `ALL_BUILD` gate — this is a throwaway comparison binary, not a change to the branch
under test, and no source in `feat/paint-depth` was touched by this step). Sliced
`cube30.stl` with `spike_paintdepth_overrides.json` via that baseline exe, recorded
`spike/out/paintdepth_baseline.gcode` (committed, frozen — same treatment as the supports
feature's `baseline_clean.gcode`/`p2_baseline.gcode`), then ran
`git worktree remove ../SnapmakerOrcaBaseline --force` — worktree and its build tree are
gone; `git worktree list` now shows only the three persistent checkouts
(SnapmakerOrca / SnapmakerOrcaNext / SnapmakerOrcaSupports).

Confirmed empirically which CONFIG_BLOCK lines legitimately differ on an unpainted object
purely from config-surface changes (not toolpath): the pre-feature baseline dump has NO
`paint_depth_mode`/`paint_depth_walls`/`paint_depth_mm`/`paint_infill_override` lines at
all (options didn't exist), and `mmu_segmented_region_interlocking_depth = 0` (vs `0.3`
now — Task 1's default flip). All five lines are stripped by `normalize()`'s paint-depth-
specific additions; everything else (including `brim_filament_source` and
`support_filament_matching`, both pre-existing chameleon-feature keys) compares
unstripped and is confirmed identical. The config-surface checks below independently
assert the RESOLVED values of every stripped key, so stripping them from the parity
compare doesn't hide a real regression — it's exactly the set of lines this feature is
expected to change on any object, painted or not.

### (b) Determinism

Folded into (a)'s second signal above (two consecutive slices, normalized-identical).

### (c) Config surface checks

- **Defaults resolve**: `paint_depth_mode = walls`, `paint_depth_walls = 3`,
  `paint_depth_mm = 1.5`, `mmu_segmented_region_interlocking_depth = 0.3` — all asserted
  directly against the resolved CONFIG_BLOCK dump of a default-overrides slice.
- **Legacy nonzero `mmu_segmented_region_max_width` maps to millimeters mode**: sliced
  with the literal old key set to `0.8`; asserted `paint_depth_mode = millimeters` and
  `paint_depth_mm = 0.8` (the legacy value carried verbatim) in the resolved CONFIG_BLOCK,
  plus (per the supports feature's legacy-check pattern, `--debug=3` to surface
  `BOOST_LOG_TRIVIAL(info)`) the loader's `"no substitutions performed from file ..."`
  line as supporting evidence the file loaded cleanly.
- **Legacy zero does NOT carry forward as unlimited**: sliced with the literal old key
  present but `0`; asserted `paint_depth_mode = walls` (NOT `unlimited`) and
  `paint_depth_mm = 1.5` (own default) — this is the deliberate "bounded-by-default must
  apply to every reslice of an old project" decision from task-1-report.md, verified here
  at the CLI config-load level rather than just by code-reading.
  (Note: unlike the supports feature's legacy-enum-removal case, `mmu_segmented_region_max_width`
  remains a real, still-parseable `ConfigOptionDef` — Task 1's "full replacement" choice
  kept it legacy-parse-only rather than removing it — so the "no substitutions performed"
  signal here mainly proves clean loading, not that `handle_legacy_composite` specifically
  fired; the RESOLVED `paint_depth_mode`/`paint_depth_mm` values are the actual proof of
  the migration, and are asserted directly.)

Painted-fixture CLI checks were **deliberately not attempted**, per the plan's own
narrowing: Task 2/3's `tests/libslic3r/test_paint_depth_clamp.cpp` ([paintdepth] tag)
already validates painted behavior end-to-end at the unit level (real painted mesh ->
sliced `PrintObject` -> inspected applied region geometry), sidestepping the known CLI
limitation that GUI-exported painted project 3mfs segfault the CLI
(`spike/FINDINGS.md`, supports feature).

## 2. Verification runs

- `bash spike/verify_paintdepth.sh` — **run 1: 17/17 PASS.** **run 2: 17/17 PASS.**
  (Identical check list both times; full tables reproduced below.)
- `libslic3r_tests.exe "[paintdepth]"` — **13 test cases / 56 assertions, all passed**,
  both runs.
- `libslic3r_tests.exe "[chameleon]"` — **133 test cases / 605 assertions, all passed**,
  both runs (unchanged from Task 1/2/3's baseline — no regression from this task, which
  touched no C++).

```
CHECK                                  RESULT DETAIL
-------------------------------------- ------ ----------------------------------------
unpainted-run1-exit0                   PASS   exit 0
unpainted-run2-exit0                   PASS   exit 0
unpainted-run1-vs-baseline             PASS   byte-identical (normalized) — feature is inert without paint
unpainted-run2-vs-baseline             PASS   byte-identical (normalized) — feature is inert without paint
unpainted-determinism                  PASS   run1 == run2 (normalized)
defaults-paint_depth_mode              PASS   = walls
defaults-paint_depth_walls             PASS   = 3
defaults-paint_depth_mm                PASS   = 1.5
defaults-mmu_segmented_region_interlocking_depth PASS   = 0.3
legacy-nonzero-exit0                   PASS   exit 0
legacy-nonzero-load-clean              PASS   config file loaded without deserialize-failure substitution
legacy-nonzero-mode-millimeters        PASS   paint_depth_mode = millimeters
legacy-nonzero-mm-value                PASS   paint_depth_mm = 0.8 (legacy mmu_segmented_region_max_width value carried verbatim)
legacy-zero-exit0                      PASS   exit 0
legacy-zero-load-clean                 PASS   config file loaded without deserialize-failure substitution
legacy-zero-mode-defaults-to-walls     PASS   paint_depth_mode = walls (NOT unlimited — bounded-by-default preserved on reslice)
legacy-zero-mm-stays-default           PASS   paint_depth_mm = 1.5 (own default, not consulted in walls mode anyway)

17/17 checks passed.
RESULT: ALL PASS
```

## 3. GUI handoff checklist

Manual/GUI verification points (Task 2/3's unit harness already proves the underlying
geometry logic; these are the corresponding visual confirmations for a human reviewer).
Expected values below follow directly from the implemented defaults (walls mode, 3 walls,
0.3mm interlocking) and the plan's Task 4 item 2:

1. **Brown/dark spot on a light, thick body, defaults (walls mode, 3 walls)**: paint a
   roughly circular patch on one face of a thick (>10mm) box or similar solid. In the
   cross-section/plater preview, the claimed (painted-color) region should form a band
   **≤3 walls deep** from the painted surface — concretely, `ext_perimeter_width +
   2*perimeter_spacing` per `paint_depth_band_mm`'s walls-mode formula (Task 1) — with the
   base-color visible immediately behind it (unlike pre-feature/legacy behavior, where the
   claim could bleed arbitrarily deep toward the object's center).
2. **Dark FULL RING (paint an entire circumference, not just a spot)**: still bounded to
   the same wall-depth band all the way around — confirm the object's **interior/center is
   NOT claimed** even though the boundary is 100% one color on that layer (this is the
   `has_layer_only_one_color` whole-layer short-circuit path, Task 2 item 2 — reproduced
   as a real unit test, but worth eyeballing in the GUI too since it's the plan's
   explicitly called-out risk case).
3. **Mode toggles**:
   - `paint_depth_mode = unlimited` should restore the **pre-feature/legacy** unbounded
     behavior — the same full ring from (2) should now claim the object's center (this is
     the intentionally-preserved legacy bug/behavior, not a regression — Task 2's
     "unlimited mode reproduces the whole-layer-claims-interior bug" test documents this
     exact case).
   - `paint_depth_mode = millimeters` with a custom `paint_depth_mm` should size the band
     by that literal mm value regardless of wall count/line width.
   - `paint_depth_walls` (in walls mode) should visibly widen/narrow the band as the wall
     count changes.
4. **Interlocking band visible at the inner boundary**: with a bounded mode active
   (walls or millimeters — anything but unlimited), the claimed region's inner edge
   against the base color should show the interlocking zig-zag/dovetail pattern at
   `mmu_segmented_region_interlocking_depth`'s default 0.3mm (Task 1 flipped this
   default from 0 -> 0.3; Task 2 confirmed it's force-zeroed specifically in unlimited
   mode so legacy parity holds there). Toggling the "beam interlocking" checkbox should
   still mutually-exclude this band per existing (untouched) behavior.
5. **Stage 2 — solid skin above/below the claim**: slice an object with a color boundary
   partway up its height (e.g., paint the top half a different color than the bottom) in
   a bounded mode. The layers immediately above/below that Z-boundary, in the painted
   region, should show **solid infill/skin** rather than sparse internal fill — confirm
   visually in the preview's "top surface"/solid-fill color coding. In `unlimited` mode,
   this should NOT happen (documents the pre-existing, intentionally-preserved bleed-path
   bug — Task 3's paired end-to-end tests pin exactly this both ways).
6. **`paint_infill_override = false`**: with a bounded mode + a painted claim, the
   claim's **walls and solid infill/skin should still be the painted color**, but its
   **sparse infill should revert to the base/unpainted color** (light stays light).
   Confirm the box is greyed out (inert) whenever `paint_depth_mode = unlimited` — Task 3
   gated the override's effect on bounded mode to match this UI greying.
7. **Thin-feature caveat (#6892)**: paint two different colors close together on a thin
   wall (wall thickness comparable to 2-3x the walls-mode band). This is a known upstream
   caveat, not something this feature claims to fix — Task 2 explicitly fenced it as
   "Voronoi-stage bounding is the fenced fallback only if this reproduces badly" and did
   not build an automated fixture for it (impractical synthetic-geometry effort vs. time
   available). **Report what you actually see** rather than assuming pass/fail: it's
   plausible (and acceptable, per the design fence) that small/thin painted parts still
   let the claimed regions merge/bleed across the thin section in ways the per-object
   `max_width` band doesn't fully predict, same as upstream PrusaSlicer's own documented
   behavior on #6892.

## Concerns / notes for reviewers

- The baseline binary build (throwaway, temp worktree) was the only compilation performed
  in this task. It built cleanly with no code changes of its own (pure checkout at
  `c345859f55`), and the worktree/build tree has been fully removed — nothing from it
  persists except the frozen `spike/out/paintdepth_baseline.gcode` gcode file. If this
  baseline ever needs to be re-recorded (e.g., a future rebase changes the merge-base
  commit), repeat the same `git worktree add <path> <new-merge-base>` /
  build-`Snapmaker_Orca_app_gui`-only / slice / `git worktree remove` procedure.
- `spike/datadir` (this worktree's own local CLI datadir, distinct from the supports
  feature's `C:\Dev\SnapmakerOrcaSupports\spike\datadir` that `verify_chameleon.sh`
  references by absolute path) was created fresh for this task rather than reused
  cross-worktree, per the "never touch other checkouts' files" instruction. It's empty on
  disk (git doesn't track empty dirs) and is recreated automatically by the CLI on first
  use if ever deleted.
- Did not attempt a painted-3mf CLI fixture (see "(c) Config surface checks" section
  above for why) — this is a deliberate scope narrowing stated in the plan itself, not an
  omission.
- Only `spike/spike_paintdepth_overrides.json` (no paint-depth keys) is used for the
  baseline-vs-current parity comparison, specifically because it's the one fixture
  guaranteed to load unmodified on the pre-feature binary — the two legacy fixtures are
  only ever run against the current binary.

Report path: `.superpowers/sdd/2026-08-31-paint-depth/task-4-report.md`
