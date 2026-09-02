# Wave B review — Option N, the normal-thickness paint shell (`b1075d660a`)

Scoped review of `b1075d660a` **as committed** (worktree `C:\Dev\SnapmakerOrcaNext`, branch
`feat/paint-depth`), against `wave-b-report.md` and `curved-gap-design.md`. Read-only; a sibling
agent is fixing Wave A findings on top of this commit in parallel. All line numbers are as
committed at `b1075d660a` (the worktree was byte-identical to the commit for `src/` and `tests/`
when this review was taken).

**Verdict: FIX FIRST** — 0 Critical, 3 Important, 6 Minor. Nothing here threatens the default
configuration or the headline result; the two code findings are an edge-config sliver and an
unaddressed painted-vs-painted interaction, and the third Important is a factual correction to
what the user has been told about cost.

---

## Gates, re-run by the reviewer

Run against the committed binary `build/tests/libslic3r/Release/libslic3r_tests.exe` (08:56,
newer than every source file in the commit, older than the commit itself) and
`build/src/Release/snapmaker-orca.exe` (08:52).

| gate | reviewer's result | report's claim | verdict |
|---|---|---|---|
| `[paintdepth]` | **682 assertions in 50 test cases, all pass** | 682 / 50 | ✅ confirmed |
| `[chameleon]` | **605 assertions in 133 test cases, all pass** | 605 / 133 | ✅ confirmed |
| `spike/verify_paintdepth.sh` | **17/17 ALL PASS**, exit 0 | 17/17 | ✅ confirmed |
| full `libslic3r_tests` | **478 test cases · 476 passed · 2 failed-as-expected; 50761 assertions · 50759 passed · 2 failed-as-expected; exit 0** | 478 / 476 / 2, exit 0 | ✅ confirmed |
| test file has zero deleted lines | `git diff --numstat b1075d660a^ b1075d660a -- tests/libslic3r/test_paint_depth_clamp.cpp` → **`377  0`** | "zero deletions" | ✅ confirmed |

The two failed-as-expected assertions are `test_mixed_filament.cpp:3469` and `:4415`, both in
`[!shouldfail]` cases whose own names say "(KNOWN bug)" / "(differential)". They are Wave A's.

**True full-suite number: 478 cases / 50761 assertions, exit 0.**

---

## Mandatory checks

1. **The measured numbers** — **PASS.** Ceiling derivation and all four slope numbers verified independently; see §A.
2. **The break relaxation** — **PASS.** Correct in both directions, cannot over-run, terminates; see §B.
3. **The TBB fix** — **PASS.** Provably race-free, inert for unpainted objects, no deadlock and no memory growth at large `granularity`; see §C.
4. **The base-colour inversion fix** — **PASS** for the base colour (complete and byte-identical to legacy), but the predicate leaves a real painted-vs-painted case unaddressed — **Important 2**; see §D.
5. **F1 invariant preserved** — **PASS.** Holds on every descent layer for both terms, by a subset argument; tripwire genuinely unmodified; see §E.
6. **Cost claim** — **FAIL.** "Total extrusion is unchanged" is not true — **Important 3**; see §F.
7. **Re-run gates** — **PASS**, table above.

---

## Findings

### Critical

None.

### Important 1 — the `D ≥ wall_stack` gate ignores the interlocking notch, so at the gate boundary the "zero-width sandwiched ring" is actually notch-wide on every even layer

`src/libslic3r/MultiMaterialSegmentation.cpp:1859-1860` (the `normal_shell` predicate), interacting
with `cut_segmented_layers`'s even-layer notch at `:1274` / `:1279`.

The gate is

```cpp
out.normal_shell = paint_depth_normal_mm > 0.f && color_idx > 0 &&
                   scaled<float>(paint_depth_normal_mm) + float(SCALED_EPSILON) >= out.extrusion_spacing + out.extrusion_width;
```

Its stated purpose (report §3.6, code comment `:1850-1858`) is to stop the base region being left a
closed ring of width `wall_stack − D` between the lateral band and the F1-inset descent claim. The
test is written against the **un-notched** band `D`. But the lateral claim that actually lands on
even layers is the **notched** band: `cut_segmented_layers:1279` uses
`region_cut_width = cut_width − interlocking_depth` when `layer_idx % 2 == 0`. The descent claim's
inner edge is `offset_ex(input_expolygons[last_idx], -wall_stack)` (`:2035`, `:2096`) on **every**
layer, both parities. So the ring's real width on even layers is `wall_stack − (D − notch)`, and
the gate must be `D − notch ≥ wall_stack`, not `D ≥ wall_stack`.

**Failure scenario, exactly the configuration the new gate test pins.** Classic generator,
`paint_depth_walls = 1`, 0.45 mm lines, 0.1 mm layers, 15° painted slope
(`test_paint_depth_clamp.cpp:2641-2654`, probe layer 12 — an **even** layer):

- `ext_w = 0.45`, `ext_s = s = 0.4285398`, `wall_stack = 0.8785398`
- `band(1) = 0.5785952` → Wave A's classic floor → `D = 0.8785398` (equal to `wall_stack`, so the gate opens)
- notch = `min(0.1, 0.25·s = 0.1071)` = **0.1**
- even-layer lateral band = `0.8785398 − 0.1` = **0.7785398**
- `M = 9`; first descent slot that survives F1 is `m = 2`, clipped to `[0.8785398, 1.1196]`; union of slots = `[0.8785398, 3.3588]`

⇒ a **closed 0.1 mm ring of base filament** at inset `[0.7785, 0.8785]`, sandwiched between two
painted annuli, on every even sub-surface layer of the descent. That is precisely the
sub-extrusion-width sliver class (#7104) the gate exists to prevent, and it survives downstream:
the only cleanup on the base leftover is `opening(..., scaled(5·EPSILON), scaled(5·EPSILON))`
(`PrintObjectSlice.cpp:4585` and `:5151`) — 5 × 10⁻⁴ mm, three orders of magnitude too small.

The report's §3.6 table states this ring is **"zero width by construction"**. It is zero width only
on odd layers.

Window of exposure: `D ∈ [wall_stack, wall_stack + notch)` = `[0.8785, 0.9785)` at stock flows.
Reachable two ways — classic + `paint_depth_walls = 1` (lands exactly on the lower edge, *because*
Wave A's floor puts it there), and `pdmMillimeters` with `paint_depth_mm` in that window. In
millimetres mode the notch is **not** capped at `0.25·s`
(`PaintDepth.cpp:50-52`, walls-mode-only per Wave A / I-3), so raising
`mmu_segmented_region_interlocking_depth` widens the window proportionally — e.g. notch 0.3 with
`paint_depth_mm = 1.0` gives a 0.18 mm ring. Defaults (`walls`, `walls = 3`, `D = 1.435675`) are
**unaffected**: the band overlaps the descent claim by 0.457 mm.

**Fix.** Subtract the notch before the gate test. `interlocking_depth` is already computed in
`multi_material_segmentation_by_painting` (`:2962`); thread it to
`segmentation_top_and_bottom_layers` alongside `paint_depth_normal_mm` and make the predicate

```cpp
out.normal_shell = paint_depth_normal_mm > 0.f && color_idx > 0 &&
                   scaled<float>(paint_depth_normal_mm - paint_depth_interlocking_mm) + float(SCALED_EPSILON)
                       >= out.extrusion_spacing + out.extrusion_width;
```

This closes classic `walls = 1` (`0.7785 < 0.8785` ⇒ descent off, legacy behaviour, no ring) and
leaves defaults untouched. It does move classic `walls = 1` back to band-only, so the second section
of the `D ≥ wall_stack` gate test needs its expectation flipped and its comment rewritten — that is
the honest outcome, not a regression. Add a probe at the 0.78–0.88 mm inset on an **even** layer to
pin the absence of the ring.

*Related, worth a look while in there:* `paint_depth_clamp_keep_core`'s degradation ladder
(`:1300`) can also narrow the band below `wall_stack` on geometry thinner than 2·band, with the
descent claim still starting at `wall_stack`. Same ring, different cause. Not verified here.

### Important 2 — the base-colour restriction is complete, but the same inversion mechanism is now 15 layers deep between two *painted* colours, unaddressed and untested

`merge_segmented_layers`, `src/libslic3r/MultiMaterialSegmentation.cpp:2530-2547`.

The §2.2 fix is correct and complete for `color_idx == 0` (verified in §D below). But the mechanism
it fixes is not specific to the base colour:

```cpp
for (const std::vector<ExPolygons> &top_and_bottom_by_extruder : top_and_bottom_layers)
    segmented_regions_trimmed = diff_ex(segmented_regions_trimmed, top_and_bottom_by_extruder[layer_idx]);
```

**Every** extruder's lateral claim is trimmed by **every** extruder's top/bottom claim, and only the
extruder's own top/bottom claim is appended back (`:2547`). So a painted colour A whose cap claim
now descends 15 layers instead of 6 removes 15 layers instead of 6 from painted colour B's lateral
band underneath it.

**Failure scenario.** A two-colour model: colour B painted on a side wall right up to the top edge,
colour A painted on the flat top. At stock defaults the top **1.5 mm** of B's painted stripe now
turns into A (was 0.6 mm). The user's stated framing is "each colour should behave like its own
object" — under that framing the boundary in the overlap volume should be the bisector, not
"top/bottom beats lateral, unconditionally". No test covers a two-painted-colour overlap, and the
report does not mention the case at all: §2.2 discusses only `color_idx 0`.

This is a design consequence rather than a coding error, and it is arguably the correct tie-break —
but it is a visible, 2.5× larger behaviour change on the most common multi-colour model shape, and
it is currently undocumented and unpinned.

**Fix (minimum).** Add a `[paintdepth]` case with two painted colours (a painted side + a painted
cap) asserting where the boundary lands, so the tie-break is a recorded decision rather than an
emergent one; and state the consequence in the report's §6/§7 so the user is not surprised. A real
fix (bisector tie-break) is a separate wave.

### Important 3 — "Total extrusion is unchanged" is false; it is what the user was told

`wave-b-report.md` §6: *"Material: none wasted. … Filament shifts from base to painted colour;
total extrusion is unchanged."* The very next paragraph then says each newly painted layer costs
"one extra tool change and its purge". Those two statements contradict each other, and the second
one is correct.

Two mechanisms add real extrusion, neither of them a shift:

1. **Purge / prime tower.** 6 → 15 painted layers on a flat cap is **9 extra tool changes**.
   `flush_volumes_matrix`'s default in this tree is **280 mm³ per change**
   (`PrintConfig.cpp:6519`), so ≈ **2520 mm³ ≈ 2.5 cm³ ≈ ~3 g of PLA** of purged filament added per
   painted flat cap, at stock defaults. On a small painted part that can exceed the object's own
   volume. Purge is extruded material, not a shift.
2. **Colour-boundary perimeters.** `apply_mm_segmentation` splits each layer's slices per
   PrintRegion and the perimeter generator runs per `LayerRegion`, so every newly split layer gains
   a full set of wall loops around the new colour boundary. Layers 7–15 under a flat cap are
   sparse-infill layers (`top_shell_layers = 4` / `top_shell_thickness = 0.6` ⇒ solid shell is 6
   layers at 0.1 mm — the report's own number), so those loops are dense extrusion replacing
   ~15 %-density infill. Net material up.

The claim *is* true of the object's own solid volume at constant infill density —
`paint_infill_override` defaults to `true` (`PrintConfig.cpp:3984`), so the painted claim's sparse
infill stays sparse and merely changes colour, which is what makes the "shift, not waste" intuition
feel right. But that is a strictly narrower statement than the one in §6.

**Fix.** Rewrite §6's headline to: *"The claim volume is a re-colouring, not extra solid — but the
job's total extrusion goes UP, by ~9 × the flush volume per painted flat cap (≈2.5 cm³ at the stock
280 mm³ default) plus the wall loops the new colour boundary adds on each newly split layer."*
Relay the correction to the user.

### Minor

1. **"Everything at 25° and above is byte-identical to before this wave"** (report §1, §2.1) is not
   provable as stated — it holds for a *pure* ≥24° slope, where `opening_ex(top_ex, 0.1125)`
   (`:1905`) erases the staircase ring before the descent starts, but a mixed patch whose steep part
   locally survives the opening does take the new path. The **stronger and provable** statement is
   monotonicity: on the painted path Wave B's claim is a **strict superset** of the legacy claim at
   every slope, so nothing anywhere can be degraded. N1 replaces `exposed_surface_part(top_ex, …)`
   (a subset of `top_ex`) with `top_ex` (a superset); N2 only raises the loop bound; N3 only removes
   break opportunities; and the one added break, `normal_shell && reachable.empty()` (`:2033`,
   `:2094`), fires only when the legacy term
   `intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset))` is itself empty (it is a
   subset of `reachable`), so it removes nothing legacy would have kept. Use that argument in the
   report instead of "byte-identical".
2. **RED-run bookkeeping.** Report §4 records the RED run as "48 cases, 647 assertions", but the
   baseline is 43 cases and 7 cases were added ⇒ 50. The three failing cases plus the three named as
   passing account for exactly 5 new cases (43 + 5 = 48), so the slope-table case and the
   base-filament case were added **after** the RED run and have no recorded RED. That is fine — the
   base-colour behaviour had a RED witness via the I-3 case — but §4 should say so rather than
   present all seven as RED-first.
3. **The byte-parity gate does not cover the TBB restructure.** Report §5 and concern #3 say the
   change "is inert for unpainted objects … the byte-parity gate covers that". The gate slices an
   **unpainted** object, which never enters `segmentation_top_and_bottom_layers` at all, so it
   proves nothing about the restructured loop. The real coverage is `[paintdepth]` / `[chameleon]` /
   the full suite. Separately, `fuzzy_skin_segmentation_by_painting` also traverses the restructured
   loop (passing `segmentation_normal_depth = 0`) and `--list-tags` shows **no fuzzy-skin tests at
   all** in `libslic3r_tests` — that path is exercised by nothing.
4. **Tooltips omit two real limits.** `paint_depth_mm`'s new tooltip promises the thickness is "the
   same everywhere: … a slope gets it measured along its own normal", but the vertical half is
   suppressed entirely above ~24° at 0.1 mm layers (§2.1) **and** whenever
   `top_shell_layers` / `bottom_shell_layers` is 0 (the C1 gate at `:1920` / `:2066`, which N2
   deliberately does not deepen). A user with `top_shell_layers = 0` gets no normal shell on top at
   all, from a control whose tooltip says otherwise.
5. **`int(layer_idx - stat.top_descent_layers)` at `:1961`** relies on `size_t` wrap-around followed
   by a narrowing conversion that is implementation-defined before C++20. It is correct on MSVC/GCC/
   Clang and the expression is inherited from the parent, but Wave B now exercises it on the bottom
   `descent − 1` = 14 layers of every object instead of 4. Consider
   `for (int last_idx = int(layer_idx) - 1; last_idx > std::max(int(layer_idx) - stat.top_descent_layers, 0); --last_idx)`.
6. **The top descent never reaches layer 0** — `:1961`'s bound is strict (`last_idx > max(…, 0)`),
   so layer 0 receives no top claim even when it is within `D`. Pre-existing upstream, unchanged,
   and masked by layer 0's own bottom claim and the lateral band; noted only because the deeper
   descent makes it reachable from 15 layers up instead of 6.

---

## Working notes behind the check verdicts

### §A — check 1: the measured numbers and the 23.96° ceiling

*Ceiling derivation.* `small_region_threshold` = `scaled(0.5 · 0.5 · outer_wall_line_width)` with
gap fill on (`:1811-1816`) = 0.1125 mm at a 0.45 mm outer wall. `opening_ex` erodes then dilates, so
a band survives only if its width exceeds `2 × 0.1125 = 0.225 mm`. The staircase ring is
`r = h/tanθ` wide, giving `θ < atan(h/0.225)` = **23.96° at h = 0.1**, **41.63° at h = 0.2**. ✅
Both figures reproduce. The filter runs at `:1905`, before the descent — so it caps the vertical
half of the claim, exactly as claimed.

*Quantisation.* `M = ceil(D/h)`: for `D = 1.435675`, `h = 0.1` ⇒ **M = 15**. I re-derived `M` from
the code path rather than the formula: `effective_shell_layers_by_thickness(layers, k, top, 1, D)`
(`:1348-1396`) breaks at the first `m` with `m·h ≥ D − EPSILON` ⇒ `m = 15`. ✅

*The table.* Reach should be `M·r = M·h/tanθ`, normal thickness `reach·sinθ = M·h·cosθ = 1.5·cosθ`:

| θ | predicted reach `1.5/tanθ` | report's measured reach | predicted `1.5·cosθ` | report's measured normal |
|---|---|---|---|---|
| 10° | 8.507 | 8.50 | 1.477 | 1.476 |
| 15° | 5.598 | 5.55 | 1.449 | 1.436 |
| 20° | 4.121 | 4.10 | 1.410 | 1.402 |

All three agree to within the test's 0.05 mm scan step (`claim_reach_mm`,
`test_paint_depth_clamp.cpp:2512-2525`, which under-reports by up to one step). ✅ Sub-24° numbers
are consistent with `M = ceil(D/h)` quantisation, and land **above** the design's `D·cosθ` by
exactly the ceil, as claimed.

*25°.* `r = 0.1/tan25° = 0.2145 < 0.225` ⇒ `top_ex` is erased by the opening at `:1905` and the
whole block is skipped. The reported 0.549 is `1.30 × sin25°`, i.e. the notched band
`1.435675 − 0.1 = 1.335675` scan-quantised to 1.30 — the lateral band alone. Note the report's
"before" column (0.607) is `D·sin25°`, a *derived* number, and the "measured now" (0.549) is a
*measured* one, so the row compares two different kinds of quantity; they are the same claim.

*24–45° not degraded.* Established by the monotonic-superset argument in **Minor 1**, which is
stronger than the report's "byte-identical" and does not depend on the patch being purely steep. ✅

### §B — check 2: the break relaxation

New form, `:2039-2044` (top) and `:2100-2104` (bottom):

```cpp
last = opening_ex(last, stat.small_region_threshold);
if (last.empty()) { if (normal_shell && ! deposited) continue; break; }
deposited = true;
```

- **Placement is correct in both directions.** The two sites are structurally identical, and both
  sit after `opening_ex` and before the `append`, so `deposited` means "post-opening material was
  actually written". Keying on the post-opening term is the right call: at 15° the first surviving
  strip is `[0.87854, 1.1196]`, 0.2411 mm against the 0.225 mm printability threshold — a 16 µm
  margin that Clipper's arc approximation could eat.
- **Cannot over-run.** Depth is bounded by the loop, not by the break. Top: `:1961`'s bound gives
  `last_idx ∈ [layer_idx − descent + 1, layer_idx − 1]`, i.e. `descent − 1` layers below the
  surface. Bottom: `:2081` gives `last_idx ∈ [layer_idx + 1, layer_idx + descent − 1]`. Both are
  exactly `descent` layers of material including the surface layer. `continue` skips a step; it
  never extends the range. ✅ On tall objects the bound is additionally capped by
  `layers_for_thickness`'s `min(int(num_layers), …)`.
- **Variable layer height is exact.** `descent` comes from
  `effective_shell_layers_by_thickness(layers, layer_idx, dir, 1, D)` (`:1866`, `:1869`), which
  walks this object's real `print_z`/`bottom_z` with the same `< thickness − EPSILON` boundary as
  `discover_vertical_shells`. No `D / layer_height` division anywhere. ✅
- **Loop state advances on the skipped steps.** `offset` and `layer_slices_trimmed` are updated at
  `:1964-1965` (`:2084-2085`) *before* the `continue`, so a skipped step is not a skipped update. ✅
- **Termination.** Bounded by the loop; the `reachable.empty()` early-out (`:2033`, `:2094`) is a
  correct monotone early-out — `layer_slices_trimmed` only shrinks and `top_exposed_ex` is fixed
  under `normal_shell`, so once empty it stays empty, and the legacy term (a subset of `reachable`)
  is empty too. Worst case is the report's own concern #5: `descent − 1` empty Clipper rounds.
- **One characteristic worth knowing, not a defect.** At the *lower boundary of a painted region on
  a slope*, the two layers immediately below the last painted surface layer receive nothing from it
  (slots `m = 1, 2` are inside the F1 inset). On a continuous painted slope every layer is its own
  surface layer so the union covers; at the paint's edge the lateral band covers it.

### §C — check 3: the TBB fix

**The pre-existing race is real.** The parent (`:1735-1739`) used
`blocked_range<size_t>(0, num_layers, granularity)` with `group_idx = range.begin() / granularity`.
`blocked_range` halves until a chunk is ≤ grainsize, so with `N = 32, G = 14` the chunks are
`[0,8) [8,16) [16,24) [24,32)` and `range.begin()/14` labels the first **two** as group 0 — same
`layer_idx_offset`, adjacent. Layer 8's top descent writes
`shell_triangles_by_color_top[c][1..7]` while `[0,8)` is concurrently appending to the same slots.
Concurrent `append` (a `vector::insert`) to one `ExPolygons` is a genuine data race. ✅ Correctly
diagnosed.

**The new form is provably race-free.** `:1892-1898`:

```cpp
const size_t num_groups = (num_layers + size_t(granularity) - 1) / size_t(granularity);
tbb::parallel_for(size_t(0), num_groups, [...](const size_t group_idx) {
    size_t layer_idx_offset = (group_idx & 1) * num_layers;
    const size_t group_end  = std::min(num_layers, (group_idx + 1) * size_t(granularity));
    for (size_t layer_idx = group_idx * size_t(granularity); layer_idx < group_end; ++ layer_idx) {
```

Let `G = granularity`. I verified the prerequisite `descent − 1 ≤ G` holds unconditionally:

- `G = max(1, M − 1)` where `M = max` over regions of `max(top_descent_eff, bottom_descent_eff)`
  (`:1597-1601`).
- `stat.top_descent_layers = max(out.top_shell_layers, eff(layers, k, top, 1, D))` (`:1861-1869`).
- `out.top_shell_layers` is maxed over the layer's regions, all of which are in
  `print_object.printing_region(i)`, and `eff(…, n, T) ≤ max(n, layers_for_thickness(T))` because
  `layers_for_thickness(T) = min(num_layers, int(T/min_layer_height) + 1)` and the walk accumulates
  real heights, each `≥ min_layer_height`; formally `ceil((T − ε)/h_min) ≤ floor(T/h_min) + 1`, and
  the run-off-the-end branch is capped by `num_layers`. So `stat.*_descent_layers ≤ M`, i.e.
  `descent − 1 ≤ M − 1 ≤ G`. ✅ (Also holds at `M ≤ 2`, where `G` is clamped to 1.)

Write ranges under that bound, for group `g` (buffer chosen by `g & 1`):

| array | indices group `g` writes |
|---|---|
| `shell_…_top` | `[(g−1)·G, (g+1)·G − 2]` |
| `shell_…_bottom` | `[g·G + 1, (g+2)·G − 1]` |
| `triangles_by_color_top/bottom` | `[g·G, (g+1)·G)` |

Same-parity groups differ by ≥ 2. Top: `g`'s max `(g+1)G − 2` < `g+2`'s min `(g+1)G`. Bottom: `g`'s
max `(g+2)G − 1` < `g+2`'s min `(g+2)G + 1`. Surface arrays are trivially disjoint. **Disjoint on
every array, for every pair of concurrently-running groups.** ✅ TBB handing several *consecutive*
groups to one task is sequential within the task and therefore safe; the index-form `parallel_for`
never splits a single index. All other state in the loop (`input_expolygons`, `top_raw`,
`bottom_raw`, `layers`, configs) is read-only, and the per-colour outer vectors are pre-`assign`ed
and never resized.

**Inert for unpainted objects** — the function is reached only from
`multi_material_segmentation_by_painting` and `fuzzy_skin_segmentation_by_painting`, both gated on
painted facets; and `paint_depth_normal_layers = layers_for_thickness(0) = 0`, so for the fuzzy
path `granularity` is bit-identical to the parent's. ✅ (But see **Minor 3** — the byte-parity gate
is not what proves this, and the fuzzy path has no tests.)

**Large `granularity`.** No deadlock: `parallel_for` over `[0, num_groups)` with a plain loop body
and no locks; `granularity ≥ 1` always, so no division by zero, and `num_layers = 0` yields
`num_groups = 0` (no iterations). No memory growth: the four buffers are `num_layers × 2` per
colour regardless of `granularity`. `layers_for_thickness` caps at `num_layers`, so
`G ≤ num_layers − 1` and `num_groups ≥ 2` whenever `num_layers ≥ 2`. Worst realistic case is
throughput: a 300-layer object at `paint_depth_mm = 6` / 0.1 mm gives `G = 60`, `num_groups = 5`
— a ~3× loss of parallelism on this stage on a many-core box. The report's concern #4 is accurate
and the consequence is bounded. ✅

### §D — check 4: the base-colour restriction

`out.normal_shell` (`:1859`) requires `color_idx > 0`, and it is the sole gate on all three of
N1 (`:1942`), N2 (`:1863`) and N3 (`:2040`, `:2101`), plus the `reachable` early-out (`:2033`,
`:2094`). For `color_idx == 0`: `top_descent_layers == top_shell_layers` (`:1861-1862`),
`top_exposed_ex == exposed_surface_part(…)`, the break is eager, and the early-out never fires.
Diffing the parent's loop body against the child's confirms the remaining difference is a pure
hoist — `reachable` is a named `const` for
`intersection_ex(intersection_ex(top_exposed_ex, layer_slices_trimmed), …)`'s inner term, and
`wall_stack` moved out of the `if`. **The base colour's descent is byte-identical to pre-Wave-B.**
✅ Complete.

`granularity` / `max_top_layers` / `max_bottom_layers` (`:1597-1601`) *are* widened without a colour
test — but they are only a sizing upper bound and a "is there a shell at all" boolean, and
`*_descent_eff > 0 ⟺ *_layers_eff > 0`, so C1 and the `slice_mesh_slabs` gate are untouched and the
over-estimate is conservative for the race. ✅

*Is `color_idx > 0` the right predicate?* Yes for the failure it fixes. `color_idx` indexes
`EnforcerBlockerType`, where 0 is `NONE` — unpainted facets — so painting always produces
`color_idx ≥ 1`, including when the user paints with the same filament as the object's base. There
is no path by which an explicitly painted face lands in `color_idx 0`, so "painted colours only" is
exactly "not the unpainted cap". What the predicate does **not** address is the same trimming
asymmetry between two *painted* colours — see **Important 2**.

### §E — check 5: F1 preserved at the new depths

The invariant is "an inferred claim is at least one wall stack clear of the deposit layer's own
contour". Both terms satisfy it on **every** descent step, not just the first:

- Full-width term: `intersection_ex(reachable, offset_ex(input_expolygons[last_idx], -wall_stack))`
  (`:2035`, `:2096`) — the inset is applied literally, unchanged from the parent, on every step.
- Legacy eroded term: `intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset))` (`:1966`,
  `:2086`) with `offset = −m·wall_stack`, `m ≥ 1`, and
  `layer_slices_trimmed ⊆ input_expolygons[last_idx]` (updated at `:1965` / `:2085` *before* use).
  Erosion is monotone in both the set and the amount, so this is a subset of
  `offset_ex(input_expolygons[last_idx], -wall_stack)`. ✅

The surface layer itself is still appended with zero margin at `:1921` / `:2067` — correct, it is
the facet the user painted. The invariant therefore holds on all 14 descent layers at defaults, and
T2 (`:2454-2474`) pins it at depths 1–5 with ≥ 0.58 mm margin while also asserting the claim is
non-empty at depths 1–3, so it cannot be satisfied vacuously. The anti-smear tripwire
(`:1073`) is genuinely unmodified — `git diff --numstat` gives `377 0`, i.e. **zero deleted lines
in the whole test file**. ✅ It stays on the legacy path because its `pdmMillimeters 0.15` is below
`wall_stack` at 0.3 mm layers, which the `D ≥ wall_stack` gate turns into a hard mode gate.

### §F — check 6: the cost claim

See **Important 3**. Short answer: **no, extrusion is not unchanged** — the claim holds only for the
object's own solid volume at constant infill density, and omits ~9 × 280 mm³ of purge per painted
flat cap plus the colour-boundary wall loops added on each newly split sparse-infill layer. The
report's own §6 contradicts itself two paragraphs apart.
