# Vertical paint-depth alignment fix — scoped review

Reviewed: `41394ce2b4` on `feat/paint-depth`, worktree `C:\Dev\SnapmakerOrcaNext`.
Against: `vertical-depth-fix-report.md`, `vertical-depth-investigation.md`,
`shell-coverage-investigation.md`. Read-only; every generator/consumer line was re-read and
hand-executed, nothing taken from the report.

**Verdict: FIX FIRST** — 1 Critical, 3 Important, 5 Minor.

The core insight and the boundary arithmetic are right: the `< thickness - EPSILON` walk against
real `print_z` / `bottom_z` is a faithful mirror of both generators (see check 2), and the
thin-layer defect the commit set out to fix is genuinely fixed. What is wrong is the *edges* of
the equivalence: the fix claims depth where no shell exists (C1), stops one layer short where the
shell runs to the end of the object (I1), and lets an unrelated region's shell settings set the
depth for the whole object (I2).

---

## Check-by-check

| # | Check | Result |
|---|---|---|
| 1 | Equivalence claim vs. actual generator code | **FAIL** — over-claims (C1), boundary case (I1), "exactly" ignores geometric narrowing (M5). Boundary arithmetic itself verified correct. |
| 2 | Variable layer height / boundary semantics | **PASS** — identical quantity + identical `EPSILON` in all four generator walks; first-layer height and rafts handled by construction. (Untested: I3.) |
| 3 | Max-over-regions (change #2) | **FAIL** — object-wide, not layer-local (I2). |
| 4 | Global gate/granularity bound never too small | **PASS** — proved, including the FP case; looseness quantified, zero memory cost (M4). |
| 5 | Invariants held (`cut_segmented_layers` order, erosion/`break`, `:1225` gate) | **PASS** — ordering and erosion byte-identical; gate traced (but see C1: it should *not* open in the layers==0 case). |
| 6 | Invalidation move | **PASS** — strictly stronger than before; also repairs two pre-existing misses. |
| 7 | Tests discriminate | **PARTIAL** — 1/3/4 discriminate well; test 2 pins the C1 over-claim; no variable-layer-height coverage (I3). |
| 8 | Re-run the three gates myself | **PASS** — reproduced exactly (see bottom). |

---

## CRITICAL

### C1 — `*_shell_layers == 0` now claims paint depth into a shell that is never built

`src/libslic3r/MultiMaterialSegmentation.cpp:1219-1241` (helper), `:1271-1290` (global bound),
`:1451-1456` (per-layer stat).

The commit's second headline change ("fixes a pre-existing bug where `top_shell_layers == 0` with
nonzero `top_shell_thickness` produced a zero painted claim") rests on the premise that "a real
solid shell was still being built underneath" (report §1). **That premise is false.** Three
independent places in the codebase make `*_shell_thickness` dead config when the layer count is 0:

- `src/libslic3r/PrintObject.cpp:1965` — `if (int n_top_layers = region_config.top_shell_layers.value; n_top_layers > 0) {`
  gates the *entire* top gather in `discover_vertical_shells`. Bottom counterpart at `:1994`.
- `src/libslic3r/PrintObject.cpp:4123-4125` — `discover_horizontal_shells`:
  `int num_solid_layers = ...; if (num_solid_layers == 0) continue;` — the thickness term at
  `:4155`/`:4157` is never reached.
- `src/libslic3r/LayerRegion.cpp:1025-1036` — `prepare_fill_surfaces()` demotes every `stTop`
  surface to `stInternal`/`stInternalVoid` when `top_shell_layers == 0`, and every `stBottom` to
  `stInternal` when `bottom_shell_layers == 0`. There is not even a solid *skin*, let alone a shell.

`PrintConfig.cpp:6376-6381` states the intended semantics outright: *"The number of top solid
layers is increased when slicing if the thickness calculated by top shell layers is thinner than
this value... 0 means that this setting is disabled."* Thickness only ever *raises an existing*
count. Both investigations contain the disproof — `shell-coverage-investigation.md:43` documents
the `prepare_fill_surfaces` demotion and `shell-coverage-investigation.md:63` quotes the
`n_top_layers > 0` guard verbatim — but `vertical-depth-investigation.md:312-313` asserts the
opposite ("while a solid shell is still generated") and the implementation followed that.

**Failure scenario.** `top_shell_layers = 0` is a one-click GUI value (`def->min = 0`,
`PrintConfig.cpp:6373`) that users set for open-top / hollow parts; `top_shell_thickness` stays at
its stock `0.6`. At 0.2 mm layers, post-fix:

- `layers_for_thickness(0.6)` = `int(0.6/0.2) + 1` = `int(2.9999999999999996) + 1` = **3** →
  `max_top_layers = 3` → the `:1304` projection gate opens (pre-fix it stayed 0 and the whole
  block was skipped).
- Per-layer: `effective_shell_layers_by_thickness(layers, S, true, 0, 0.6)` walks gaps
  0.2 / 0.4 / 0.6 → breaks at `m = 3` → `effective = max(0, 3) = 3`.
- Descent loop `:1495` → `last_idx > max(S-3, 0)` claims `S-1`, `S-2`, plus the surface layer `S`
  appended at `:1492`. **Three layers claimed** in a region whose top is pure sparse infill.

Cost: painted filament, a tool change and a wipe-tower purge per affected layer, for volume that
prints as sparse infill either way — and it is a silent behavior change vs. stock BBS/Orca for a
supported config. The claim depth is derived from a shell that does not exist.

`tests/libslic3r/test_paint_depth_clamp.cpp:600-616` **pins this behavior**, so it will survive
any later cleanup unless the test is changed too.

**Fix.** Make the thickness term conditional on a nonzero count, exactly as the generators do:

```cpp
// MultiMaterialSegmentation.cpp:1219, first statement of the helper
if (n_layers <= 0)
    return n_layers;          // matches PrintObject.cpp:1965 / :1994 / :4124 - no shell at all.
```
```cpp
// MultiMaterialSegmentation.cpp:1287-1288
const int top_layers_eff    = config.top_shell_layers.value > 0
    ? std::max(config.top_shell_layers.value, layers_for_thickness(config.top_shell_thickness.value)) : 0;
const int bottom_layers_eff = config.bottom_shell_layers.value > 0
    ? std::max(config.bottom_shell_layers.value, layers_for_thickness(config.bottom_shell_thickness.value)) : 0;
```

Then invert the test at `:600` to assert the claim is *absent* (pre-fix behavior was correct here),
and drop the "pre-existing gate bug" paragraphs from the report and the commit message.

---

## IMPORTANT

### I1 — Helper is one layer short when the thickness walk runs off the end of the object

`src/libslic3r/MultiMaterialSegmentation.cpp:1226-1237`.

`++m` happens *before* the break test, so `m` equals "layers inside the thickness, plus the one
that ended it" = the correct total depth **only when the loop breaks**. When the loop instead
exhausts its range (`idx < 0` for top, `idx == num_layers` for bottom) every walked layer was
inside the thickness and `m` is the count of layers *below/above* the surface — one short of the
total depth including the surface layer itself.

Formally, for a top surface at layer `S`, the generators (`PrintObject.cpp:1972` and `:4155`)
solidify layer `D` iff `S - D < N` **or** `print_z[S] - print_z[D] < T - EPSILON`. The deepest
satisfying layer is `D*`; the correct total is `S - D* + 1`. The helper returns `S - D* + 1` when
it breaks and `S - D*` (i.e. `S`, with `D* = 0`) when it does not.

**Live counterexample (bottom direction).** 0.5 mm plate, `layer_height = initial_layer_print_height
= 0.1` → 5 layers, `bottom_z = 0, 0.1, 0.2, 0.3, 0.4`; `bottom_shell_layers = 3`,
`bottom_shell_thickness = 0.6`; bottom cap painted, `S = 0`.

- Helper: `idx = 1..4`, gaps 0.1/0.2/0.3/0.4, never `>= 0.5999`; loop ends at `idx == 5`. `m = 4`,
  `effective = max(3, 4) = 4`.
- Loop bound `:1515`: `last_idx < min(0 + 4, 5) = 4` → claims layers **0..3**.
- Generator (`PrintObject.cpp:2001` / `:4157`): for `D = 4`, `bottom_z[4] - bottom_z[0] = 0.4 <
  0.6 - EPSILON` → true, so the bottom shell **does** reach layer 4.

One base-colored solid layer left above the painted bottom skin — precisely the defect class this
commit exists to remove. Reachable whenever a painted downward-facing surface sits within
`bottom_shell_thickness` of the top of the object (thin painted plates, a top flange/lip painted
underneath).

The top-direction analogue is *masked*, not absent: the pre-existing strict `>` plus
`std::max(..., 0)` at `:1495` makes layer 0 unreachable through the top path for any `effective`,
so the missing `+1` changes nothing there. `vertical-depth-investigation.md` §4 anchor 3 flagged
that exact edge as "a separate, pre-existing edge case worth a deliberate decision"; the report
makes no decision on it while asserting the walk is reproduced "exactly".

**Fix.** Track how each loop terminated and add the missing layer:

```cpp
if (top) {
    int idx = int(surface_layer_idx) - 1;
    for (; idx >= 0; --idx) { ++m; if (base - layers[idx]->print_z >= thickness - EPSILON) break; }
    if (idx < 0) ++m;                    // walk exhausted: every layer below is inside `thickness`
} else {
    size_t idx = surface_layer_idx + 1;
    for (; idx < num_layers; ++idx) { ++m; if (layers[idx]->bottom_z() - base >= thickness - EPSILON) break; }
    if (idx >= num_layers) ++m;
}
```

and, if the top path is to actually match, relax `:1495` to `last_idx >= std::max(int(layer_idx) -
stat.top_shell_layers + 1, 0)` (a deliberate change, out of this commit's stated scope — decide
and record it either way).

### I2 — Max-over-ALL-regions is object-wide, not layer-local

`src/libslic3r/MultiMaterialSegmentation.cpp:1436-1456`.

The report's justification is that "a painted patch can span regions with different shell
settings". True — but `layer.regions()` is not "the regions present on this layer". At
`src/libslic3r/PrintObjectSlice.cpp:5199-5208` every layer is given a `LayerRegion` for **every**
`PrintRegion` in `m_shared_regions->all_regions`, whether or not that region has any geometry
there. So a modifier's shell settings now set the painted claim depth on every layer of the
object, including layers the modifier never touches.

**Failure scenario.** 100×100×100 mm plate, base region `top_shell_layers = 4`, plus one small
modifier volume at mid-height configured `top_shell_layers = 30`. Paint the top face. Post-fix the
top claim descends 30 layers everywhere, though the real shell up there is 4. Blast radius is
bounded only by the descent's own taper (`offset -= stat.extrusion_spacing + stat.extrusion_width`
≈ 0.87 mm/layer at a 0.45 mm outer wall), i.e. ~26 mm of inward erosion over 30 layers → roughly
48×48 mm of top area painted ~5.2 mm deeper than the shell it is supposed to cover.

Per-color correctness is otherwise preserved: `extrusion_width`, `small_region_threshold`,
`extrusion_spacing` and `num_regions` stay scoped to `color_idx`, and a painted color always has a
matching `wall_filament` region (created up front by `generate_print_object_regions`), so the
`num_regions == 0` path is not newly reachable. The problem is only that the depth scalar now
absorbs unrelated regions. In the common case (base region + auto-created painted regions, which
share shell settings) the change is a no-op — it only ever bites via modifiers, and there it bites
object-wide.

**Fix (cheap, preserves the intent).** Skip regions with no geometry on this layer:

```cpp
for (const LayerRegion *region : layer.regions()) {
    if (region->slices.empty())
        continue;               // LayerRegions exist for every PrintRegion on every layer.
    ...
```

`region->slices` is populated at `PrintObjectSlice.cpp:5229-5235`, before the segmentation runs at
`:5267`, so this is valid and free. The fully correct form intersects each region's deep claim with
that region's own area, which requires the scalar to become per-region — worth a note if not done.

### I3 — No test covers non-uniform layer heights; the harness fix removed the only case that had it

`tests/libslic3r/test_paint_depth_clamp.cpp:542`.

The fixture now pins `initial_layer_print_height = layer_height`. Report §3 describes discovering
the mismatch and eliminating it — but that mismatch was the suite's only non-uniform layer. Every
remaining assertion in all four new cases is exactly reproducible by a plain
`ceil(thickness / layer_height)`, which is the cheaper alternative
`vertical-depth-investigation.md` §4 anchor 1 explicitly offers. So the property the fix leans on
hardest ("variable/adaptive layer height is handled automatically — there is no
`ceil(thickness / layer_height)` anywhere in this path", report §2.1) is **not pinned by anything**:
a regression to `ceil()` would keep all 19 `[paintdepth]` cases green.

**Fix.** Add a fifth case that keeps the heights unequal on purpose:
`layer_height = 0.1`, `initial_layer_print_height = 0.2`, bottom cap painted,
`bottom_shell_layers = 3`, `bottom_shell_thickness = 0.6`. Real `bottom_z` = 0, 0.2, 0.3, 0.4, 0.5,
0.6 → gaps 0.2/0.3/0.4/0.5/0.6, break at `m = 5`, `effective = 5` → assert layers 0..4 claimed and
layer 5 **not**. A uniform-0.1 assumption predicts 6 and fails the last assertion.

---

## MINOR

- **M5 — "reproduces the walk exactly" is true of the loop *bound* only.** Both generators also
  narrow the shell geometrically as they descend — `discover_horizontal_shells` reassigns
  `solid = new_internal_solid` and can `goto EXTERNAL` on an empty intersection
  (`PrintObject.cpp:4182-4200`), and `discover_vertical_shells` accumulates through
  `combine_shells`/`combine_holes` intersections — so the real shell can stop short of the bound.
  The claim therefore over-covers relative to the true shell. That is the safe direction (the
  segmentation's own `if (last.empty()) break;` at `:1501`/`:1521` is the analogous limiter), but
  the report's "this is not an approximation" wording should be qualified.
- **M6 — the walk is recomputed once per color.** `layer_color_stat(layer_idx, color_idx)` is called
  inside the color loop at `:1483`, but both shell walks are color-independent. Cost is now
  `O(num_layers × num_facets_states × num_regions × m)` where it used to be `O(1)` per region.
  Hoist the two walks to once per layer.
- **M7 — unbounded `int(thickness / min_layer_height)` at `:1279`.** `top_shell_thickness` has
  `def->min = 0` and no max (`PrintConfig.cpp:6383`); a pathological value overflows the `int`
  conversion (UB). Clamp the quotient in `double` before converting.
- **M8 — `std::max(int(layer_idx - stat.top_shell_layers), int(0))` at `:1495`** does `size_t`
  wraparound followed by a narrowing conversion. Correct on two's-complement, but
  implementation-defined pre-C++20 and now exercised far more often since the counts are larger.
  `std::max(int(layer_idx) - stat.top_shell_layers, 0)` is clean.
- **M4 — granularity looseness, quantified (no defect, for the record).** The bound cannot be too
  small: `m <= ceil((T - EPSILON)/mlh) <= floor(T/mlh) + 1`, and the only FP hazard (an exactly
  integral `T/mlh` floored one low) is absorbed by `EPSILON = 1e-4` (`libslic3r.h:52`) — checked
  for the 0.6/0.2 and 0.6/0.05 cases. Looseness on a 300 mm object at 0.05 mm min layer height
  (~6000 layers, `T = 0.6`): `layers_for_thickness = int(11.999…) + 1 = 12`, granularity 11 vs. 3
  pre-fix → ~545 TBB chunks, still fully parallel. **No memory cost**: the double buffers are sized
  `num_layers * 2` at `:1409-1418` regardless of granularity. With adaptive layer height (min 0.05,
  typical 0.3) the bound is ~6× looser than the exact per-layer walk — harmless. Only pathology:
  `T >= object height` clamps `layers_for_thickness` to `num_layers`, making granularity
  `num_layers - 1` and collapsing the projection `parallel_for` to one chunk. Perf only.

---

## Detail on the passing checks

**Check 2 — boundary semantics, hand-compared against all four generator walks.** For a top surface
at `S` and a candidate layer `D`, every walk compares the same pair with the same epsilon:

| Walk | Condition | Gap expression |
|---|---|---|
| `discover_vertical_shells` top, `PrintObject.cpp:1971-1972` | `i < itop \|\| m_layers[i]->print_z - print_z < top_shell_thickness - EPSILON` | `print_z[S] - print_z[D]` |
| `discover_horizontal_shells` top, `:4153-4155` | `int(i) - n < num_solid_layers \|\| print_z - m_layers[n]->print_z < ... - EPSILON` | `print_z[S] - print_z[D]` |
| `discover_vertical_shells` bottom, `:2000-2001` | `i > ibottom \|\| bottom_z - m_layers[i]->bottom_z() < ... - EPSILON` | `bottom_z[D] - bottom_z[S]` |
| `discover_horizontal_shells` bottom, `:4156-4157` | `n - int(i) < num_solid_layers \|\| m_layers[n]->bottom_z() - bottom_z < ... - EPSILON` | `bottom_z[D] - bottom_z[S]` |
| Helper `:1228` / `:1234` | break on `>= thickness - EPSILON` | identical, exact negation |

The count halves also agree: `i < itop` ⟺ `S - D < N` ⟺ `int(i) - n < num_solid_layers`. Both
halves are monotonic in depth, and neither gather loop has an area threshold, per-surface
condition, or `break` inside it, so the walk **is** a prefix in depth — the report's prefix-union
argument is sound as far as the bound goes. First-layer height and rafts need no special-casing
because both sides read the same `m_layers` array (`bottom_z() = print_z - height`, so a
differing `initial_layer_print_height` or a raft offset is already baked into both).

**Check 5 — invariants.** `cut_segmented_layers` at `MultiMaterialSegmentation.cpp:2277` still
precedes `segmentation_top_and_bottom_layers` at `:2284` (cut → project → merge, unchanged). The
erosion `offset -= (stat.extrusion_spacing + stat.extrusion_width)`, the
`intersection_ex`/`opening_ex` calls and the `if (last.empty()) break;` at `:1501`/`:1521` are
untouched by the diff — only the loop's bound *input* changed. The `:1304` gate opens as traced
above (correctly for the thin-layer case; incorrectly for layers==0, see C1).

**Check 6 — invalidation move is strictly stronger, and repairs two pre-existing misses.** The
posSlice branch does only `steps.emplace_back(posSlice)` (`PrintObject.cpp:986`), and
`PrintObject::invalidate_step(posSlice)` (`:1280-1285`) cascades to `posPerimeters`,
`posPrepareInfill`, `posInfill`, `posIroning`, `posSupportMaterial`, `posSimplifyPath`,
`posSimplifyInfill` plus `psSkirtBrim`, and clears `m_slicing_params` / the local-Z plan. The old
posPrepareInfill branch (`:1105-1128`) emplaces only `posPrepareInfill`, a strict subset. Nothing
else keys off the old grouping — the only other consumers of these two options in the slicing
pipeline are `PrintObjectSlice.cpp:197` (`slicing_mode_normal_below_layer`, a **posSlice**-time
computation) and `LayerRegion.cpp:199-200` (spiral-mode gate, **posPerimeters**-time), both of
which were *under*-invalidated before this move and are now correct. Remaining references are
GUI/preset-list only (`ConfigManipulation.cpp:654-655`, `GUI_Factories.cpp:147-148/185`,
`Preset.cpp:874`, `PresetHints.cpp`). Cost: editing either key now forces a full re-slice rather
than a re-infill. Acceptable and, given `PrintObjectSlice.cpp:197`, required.

**Check 7 — test discrimination, hand-verified.**

- Test 1 (`:576`, 0.1 mm, `N=4`, `T=0.6`): helper `m=6` → `effective=6` → bound `last_idx >
  max(S-6,0)` claims `S..S-5`, not `S-6`. Assertions match exactly; fails if the helper returns 5
  or 7. **Discriminates.**
- Test 2 (`:600`): asserts only the surface layer. Weakly discriminating **and** pins the C1
  over-claim (the real claim in that config is 3 layers deep).
- Test 3 (`:618`, 0.2 mm, `N=4`, `T=0.6`): helper `m=3`, `effective = max(4,3) = 4` → claims
  `S..S-3`, not `S-4`. Genuinely pins "no over-claim" (would fail on `max`→`sum` or an inflated
  helper), and legitimately passed pre-fix. **Discriminates.**
- Test 4 (`:640`, bottom, 0.1 mm, `N_b=3`, `T_b=0.6`): helper `m=6`, `effective=6`, bound
  `last_idx < min(0+6, num_layers) = 6` → claims 0..5, not 6. **Discriminates.**
- The harness fix (pinning `initial_layer_print_height`) makes the arithmetic exact and does not
  weaken what the tests claim — but it costs the suite its only variable-layer-height coverage
  (I3).

**Check 8 — re-run by me, this session, on the committed binary
(`build/tests/libslic3r/Release/libslic3r_tests.exe`, mtime 18:44:04, newer than all three touched
sources):**

```
[paintdepth]            All tests passed (122 assertions in 19 test cases)
[chameleon]             All tests passed (605 assertions in 133 test cases)
spike/verify_paintdepth.sh   17/17 checks passed.  RESULT: ALL PASS
```

All three reproduce the report's numbers exactly. Note that none of them can catch C1, I1, or I2 —
C1 is pinned green by a test, and I1/I2 live in configurations no fixture builds.
