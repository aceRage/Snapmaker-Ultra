# Vertical paint-depth alignment fix — report

Worktree: `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base `e64a4e154e`.
Inputs (read first, per the task): `vertical-depth-investigation.md`,
`shell-coverage-investigation.md`, `docs/superpowers/specs/2026-08-31-paint-depth-design.md`.

## Status: COMPLETE

## 1. The defect (recap)

`segmentation_top_and_bottom_layers` (`src/libslic3r/MultiMaterialSegmentation.cpp:1194-1484`
pre-fix line numbers) claims a painted top/bottom face exactly `top_shell_layers` /
`bottom_shell_layers` layers deep and never consulted `top_shell_thickness` /
`bottom_shell_thickness`, while the actual solid shell (`discover_vertical_shells` /
`discover_horizontal_shells`, `PrintObject.cpp:1954-1967`, `:1983-1996`, `:4141-4147`) is
built to whichever is **deeper** of "N layers" or "T millimeters". At thin layer heights
(e.g. 0.1mm, stock defaults 4 layers / 0.6mm ⇒ 6 shell layers) the solid shell is deeper than
the painted claim, so the base-colored solid shell layers beneath a painted top/bottom skin
show through — the user-reported symptom on a real model. A second gap: with
`top_shell_layers == 0` and `top_shell_thickness > 0`, the old code's gate produced a
**zero** painted claim even though a real solid shell was still being built underneath.

## 2. Changes made

### 2.1 `src/libslic3r/MultiMaterialSegmentation.cpp`

**New helper `effective_shell_layers_by_thickness()`** (inserted just above
`segmentation_top_and_bottom_layers`, ~40 lines incl. comment): given a painted surface
layer, a direction (top/bottom), the configured layer count, and the configured thickness,
returns `max(configured_layers, layers_needed_to_cover_thickness)`, where the
thickness-driven count is computed by **walking this object's real per-layer `print_z` /
`bottom_z()`** — the same walk `discover_vertical_shells` performs
(`PrintObject.cpp:1960-1961` for top, mirrored for bottom), including its exact
`< thickness - EPSILON` boundary. Because layer heights are read from each `Layer` object
directly (never assumed uniform), variable/adaptive layer height is handled automatically —
there is no `ceil(thickness / layer_height)` anywhere in this path.

Proof sketch (also in the code comment): the shell generators' loop condition is
`(count-not-yet-met) OR (thickness-not-yet-met)`, evaluated fresh at each candidate layer.
Both the "count" half and the "thickness" half are individually monotonic in descent depth
(each holds for depths `1..k` and fails beyond, because `print_z`/`bottom_z` are monotonic
per layer) — i.e. each is exactly the prefix set `{1..k}` for its own `k`. The union of two
prefix sets `{1..k1} ∪ {1..k2}` is simply `{1..max(k1,k2)}`. So feeding
`max(n_layers, thickness_driven_count)` into the **original, unchanged** count-only descent
loop bound reproduces the interleaved walk's result exactly, including its boundary
handling — this is not an approximation.

**Global gate/granularity sizing** (`max_top_layers` / `max_bottom_layers` / `granularity`,
originally :1205-1213): now also folds in a thickness-driven upper bound, computed
conservatively via the **thinnest layer height anywhere in the object**
(`min_layer_height`) rather than the exact per-layer walk (that global computation has no
single "current layer" to walk from — it sizes the whole-object TBB double-buffer margin and
the projection gate before any per-layer loop runs). This is provably a safe upper bound: every
real layer's height is `>= min_layer_height`, so real cumulative height after `k` layers is
`>= k * min_layer_height`, meaning the real per-layer walk can only need `<=` as many layers
as the uniform-`min_layer_height` estimate. This fixes the `top_shell_layers == 0 &&
top_shell_thickness > 0` gate bug directly: `max_top_layers` is now `> 0` whenever *either*
term is nonzero, so the existing gate condition
(`if (max_top_layers > 0 || max_bottom_layers > 0)`, unchanged) now opens correctly — no edit
to the gate expression itself was needed, only to its inputs.

**`layer_color_stat` lambda** (originally :1354-1381): the loop over `layer.regions()` is
restructured so `top_shell_layers`/`bottom_shell_layers` (now populated via
`effective_shell_layers_by_thickness`) are maxed over **all** regions on the layer,
unconditionally — not filtered to `color_idx == 0 || wall_filament == color_idx` (required
change #2 / shell-coverage-investigation.md fix (2)): a painted patch can span regions with
differing shell settings, and the shell the base material builds underneath doesn't care
which color is painted above it, so the deepest of all regions' shells wins. `extrusion_width`,
`small_region_threshold`, and `extrusion_spacing` — which size the lateral erosion/taper, not
the shell depth — stay scoped to `color_idx` exactly as before (untouched). The consuming
descent loops (originally :1400 top / :1420 bottom) needed **zero structural changes**: they
already read `stat.top_shell_layers` / `stat.bottom_shell_layers`, which now simply carry a
bigger (correct) number.

### 2.2 `src/libslic3r/PrintObject.cpp` — invalidation (required change #3)

`top_shell_thickness` / `bottom_shell_thickness` moved from the `posPrepareInfill` group
(originally :1100-1101) into the `posSlice` group (:956-976, alongside the other
`paint_depth_*` keys). `PrintObject::invalidate_step(posSlice)` already cascades forward to
`posPrepareInfill` and everything after it, so this is a move, not a duplicate listing.
Without this, editing either thickness in the GUI would re-run shell generation but not
re-run the MMU segmentation that now also depends on it, leaving a stale (too-shallow) paint
claim until some *other* option forced a re-slice.

### 2.3 Untouched (verified, not just assumed)

- `cut_segmented_layers` (:2181-2184) vs. `segmentation_top_and_bottom_layers` (:2188-2191)
  ordering: unchanged. Re-read `segmentation_by_painting`'s call sequence after the fix to
  confirm no incidental reordering; still cut-then-project-then-merge.
- The per-layer erosion (`offset -= stat.extrusion_spacing + stat.extrusion_width`) and its
  `if (last.empty()) break;` (originally :1403/:1407): only the loop's *bound input*
  (`stat.top_shell_layers`/`bottom_shell_layers`) changed value; the erosion formula, the
  intersection/opening calls, and the early-break logic are byte-identical to before.

## 3. Tests (TDD)

Added to `tests/libslic3r/test_paint_depth_clamp.cpp` (reused the existing end-to-end harness
pattern: real painted mesh → `TriangleSelector`/`mmu_segmentation_facets` →
`print.apply()`/`PrintObject::slice()` → inspect the applied per-region layer geometry). New
fixture `slice_capped_slab()` builds a 40x40x4mm slab and paints one whole top or bottom cap
(facets `{2,3}`/`{0,1}`, matching the file's own documented `its_make_cube` facet map), with
`layer_height` / `top_shell_layers` / `top_shell_thickness` / `bottom_shell_layers` /
`bottom_shell_thickness` all set explicitly per test. `paint_depth_mode` is pinned to
`pdmUnlimited` throughout — the vertical projection is a data path independent of the Stage 1
lateral clamp (investigation A, section 2), and nothing in these fixtures paints a side facet
for that clamp to act on anyway, so this isolates the vertical mechanism cleanly.

Four new `[paintdepth]` cases:

1. **"thin layers make the painted top claim reach the full (thickness-driven) shell
   depth"** — 0.1mm layers, stock defaults (`top_shell_layers=4`, `top_shell_thickness=0.6`)
   ⇒ asserts depths 0-5 (6 layers) are claimed and depth 6 is not.
2. **"top_shell_layers=0 with nonzero top_shell_thickness still claims the painted top
   surface"** — asserts a nonzero claim where pre-fix the gate produced zero.
3. **"layer-count-driven shell depth is unchanged when it already exceeds the thickness
   bound"** — regression guard at a normal 0.2mm layer height (4 layers already `>=` the
   0.6mm bound): asserts the claim stays exactly 4 layers, both before and after the fix.
4. **Bottom-face mirror of (1)** — same shape, `BOTTOM_CAP_FACE`, `bottom_z()`-based walk.

**Real RED, captured before any implementation edit:** built `libslic3r_tests` (targeted,
pre-fix) and ran `[paintdepth]` — 3 test cases / 6 assertions failed exactly as predicted
(tests 1, 2, and 4 above; test 3 already passed pre-fix, confirming it's a true no-op
regression guard, not accidentally red). Full transcript in
`red_run_vertdepth.log`/`red_build_vertdepth.log` (scratchpad). Sample:
```
multi_material_segmentation_by_painting: thin layers make the painted top claim
                                         reach the full (thickness-driven) shell depth
FAILED: CHECK( any_contains(extruder2_claim_for_layer(*object, top_index - depth), probe) )
with expansion: false
...
multi_material_segmentation_by_painting: top_shell_layers=0 with nonzero top_shell_thickness
                                         still claims the painted top surface
FAILED: CHECK( any_contains(extruder2_claim_for_layer(*object, top_index), probe) )
with expansion: false
test cases:  19 |  16 passed | 3 failed
assertions: 122 | 116 passed | 6 failed
```

One test-harness bug found and fixed during RED→GREEN (not a production bug): the bottom-face
test initially left `initial_layer_print_height` at its 0.2mm default while `layer_height`
was set to 0.1mm, so layer 0 (exactly where the bottom test probes) was really 0.2mm thick —
the fixed helper now pins `initial_layer_print_height` to the same `layer_height`, and the
fix's own math (which correctly used the *real*, non-uniform layer 0 height) is what exposed
the mismatch — a good sign the "no assumption of constant layer height" property actually
holds.

## 4. Verification results

- `[paintdepth]`: was 15 cases / 84 assertions; now **19 cases / 122 assertions, all green**
  (the 4 new cases above; no existing case's expected behavior changed).
- `[chameleon]`: **133 cases / 605 assertions, all green, byte-for-byte the same count as
  baseline** — confirms no cross-suite regression from the `PrintObject.cpp` invalidation
  edit (chameleon/mixed-filament code shares `PrintConfig`/`Tab` surface with this branch).
- `ALL_BUILD` via `build_next_wt.bat`: **exit 0** (full app + all test targets; only
  pre-existing, unrelated linker warnings LNK4098/LNK4286, nothing on the touched files).
- `spike/verify_paintdepth.sh`: **17/17 twice** (`verify_vertdepth_run1.log`,
  `verify_vertdepth_run2.log`) — unpainted byte-parity vs. the frozen pre-feature baseline
  holds (this fix's entire code path is gated behind `is_mm_painted()` at the
  `multi_material_segmentation_by_painting` call site in `PrintObjectSlice.cpp:5252-5254`, so
  a genuinely unpainted object never reaches any changed line), determinism holds, and the
  config-surface/legacy-migration checks are untouched and still pass.
- Extra (beyond the binding gate): full `libslic3r_tests` suite, unfiltered — 447 cases,
  445 green + 2 pre-existing `[!shouldfail]`-tagged known-bug cases in the unrelated
  `[MixedFilament][batch_remap]`/`[differential]` subsystem (`test_mixed_filament.cpp:3540`,
  `:4386` — the latter's own test name literally says "(KNOWN bug)"). Confirmed unrelated:
  different feature area, no `top_shell_thickness`/`bottom_shell_thickness`/segmentation
  code touched by this diff anywhere near them; `grep` over `tests/` also confirms no other
  test references either thickness key.

## 5. Self-review hand-walk (required)

0.1mm layers, `top_shell_layers=4`, `top_shell_thickness=0.6`: at the surface layer,
`effective_shell_layers_by_thickness` walks downward — gaps 0.1, 0.2, 0.3, 0.4, 0.5mm at
m=1..5 (all `< 0.6-EPSILON`, keep going), gap 0.6mm at m=6 (`>= 0.6-EPSILON`, stop) ⇒
`m=6`, `effective = max(4,6) = 6`. Fed into the unchanged descent loop bound
(`last_idx > layer_idx - 6`), the claim covers the surface layer plus 5 sub-layers = **6
layers deep**, matching the solid shell `discover_vertical_shells` builds at the same
settings. The base region's forced `interface_shells` top surface (Stage 2) now starts below
layer 6, not layer 4, so the layers between depth 4 and depth 6 are the *painted* region's
own solid infill instead of the base region's — the base-colored solid no longer sits under
the painted skin. This exact scenario is TEST 1 above, which passes post-fix (and was
confirmed RED pre-fix).

## 6. Concerns / residual risk

- The global gate/granularity bound (§2.1, `min_layer_height`-based) is intentionally
  conservative, not exact — it can only make `granularity`/`max_top_layers` *larger* than
  strictly necessary (more TBB overlap margin, never less), so it cannot under-cover the real
  per-layer claim computed later. No observed cost: `[paintdepth]`/`[chameleon]` timings were
  unaffected (sub-20ms per case).
- `layer_color_stat` now does a bounded walk (typically a handful of layers) on every call,
  including for `color_idx == 0` on fully unpainted-within-a-painted-object regions and even
  when the outer projection gate is closed for a *specific* color but the function still runs
  per the existing (unchanged) loop structure. This is a minor constant-factor cost, not a
  behavior change, and is irrelevant for genuinely unpainted objects (the whole function is
  gated behind `is_mm_painted()` at the call site, never reached at all — confirmed by
  `verify_paintdepth.sh`'s byte-parity check).
- Not covered by this fix (out of scope per the task): the erosion/`break` short-circuit
  (investigation B fix (3)) can still cut a claim short near the object silhouette or on a
  thin/tapering cross-section, independent of shell depth. Left untouched per explicit
  instruction.

## 7. Files changed

- `src/libslic3r/MultiMaterialSegmentation.cpp` — new helper + 2 call sites.
- `src/libslic3r/PrintObject.cpp` — invalidation group move.
- `tests/libslic3r/test_paint_depth_clamp.cpp` — 4 new `[paintdepth]` cases + fixture helpers.
