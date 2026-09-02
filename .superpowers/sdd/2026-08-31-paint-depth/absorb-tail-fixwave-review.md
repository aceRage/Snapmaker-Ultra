# Absorb-tail fix-wave review: `9227fa72b1` (feat/paint-depth, worktree `C:\Dev\SnapmakerOrcaNext`)

Scope: the single commit `9227fa72b1` reviewed AS COMMITTED (`git show`), against
absorb-tail-fixwave-report.md and the review it answers (absorb-tail-review.md I1, I2, Minors
A/B/C = M1/M3/M4). Read-only; every number below was produced in this session on the committed
binary or on a probe binary built from the committed sources (see "Method").

**Verdict: FIX FIRST.** 0 Critical, 4 Important, 6 Minor.

The two headline claims of the report that do not survive hand-execution: (1) the I1 pin's floor
is 1.0mm2 in the committed code, not the 0.3mm2 the report describes, and the measured post-fix
residual (0.315mm2) is ABOVE 0.3 - the floor was raised to clear it; (2) the "site 3/4" tests
measure site 1 (their metric is re-derived from `detect_surfaces_type` AFTER the perimeter
generator runs), while Arachne site 4 is demonstrably live and detectable with a one-line
perimeter-length assertion the commit never makes.

---

## Binary faithfulness / method

- `git status`: no tracked source modified (only `progress.md` +2 lines, untracked docs and
  spike outputs). `build/tests/libslic3r/Release/libslic3r_tests.exe` 17:32:19 and
  `libslic3r.lib` 17:31:55 postdate every source mtime (latest: `test_paint_depth_clamp.cpp`
  17:30:49, `PerimeterGenerator.cpp` 17:29:55 = the mutation revert); commit at 17:45:31.
  sha256 `7b083e4d...` snapshotted to the scratchpad before any run so the sibling agent's
  rebuilds cannot contaminate the numbers. `build/src/Release/snapmaker-orca.exe` 17:40:16.
- Probe binary: the committed `test_paint_depth_clamp.cpp` (`git show 9227fa72b1:...`) plus
  four `[reviewprobe]` TEST_CASEs appended, compiled with the tlog's exact `cl` line and linked
  with the tlog's exact `link` line against the committed build's own objects/libs (only
  `TEST_PAINT_DEPTH_CLAMP.OBJ` swapped). Source and logs:
  `%TEMP%\claude\C--Dev\85fd2715-...\scratchpad\probe\{probe_append.cpp,probeA..E.log}`.
  No pre-fix (old-formula) measurement was possible without rebuilding libslic3r in a worktree
  shared with a parallel implementer; the report's pre-fix numbers (0.72 / 3.12mm2) are taken
  as reported, the post-fix numbers are mine.
- Contamination check: the sibling (flat-top cap) agent's first rebuild landed at 18:15:36
  (`test_paint_depth_clamp.obj`) / 18:16:46 (`libslic3r.lib`), AFTER every run here (test runs
  17:55-18:02 on the sha256-snapshotted exe; probe linked 18:07:10 against the 17:31:55 lib), so
  nothing below reflects the working-tree edits.

## One-line result per mandatory check

| # | Check | Result |
|---|---|---|
| 1 | I1 fix | **PARTIAL / FAIL on 1c** - (a) the clip can no longer ADD geometry, but the raw legacy shadow still shapes the claim as an exemption mask: 16 thin interior Extruder-2 components remain at layers 144-146 (7 >= 0.01mm2, max 0.315mm2); (b) the set-algebra rewrite is correct but its premise (`legacy` subset-of `full` before Item 2) is false - `legacy` is never priority-trimmed; (c) the 0.3mm2 floor has no provenance (3 x sqr(0.1mm) = 0.03mm2), 0.315 is not below 0.3, and the committed floor is 1.0mm2 - tuned to the residual |
| 2 | I2 sites 3/4 | **LIVE, UNTESTED** - site 4 (Arachne, `PerimeterGenerator.cpp:2273`) changes perimeter length on both committed fixtures (302.65 vs 312.89mm sandwich, 311.56 vs 312.97mm flat); site 3 (classic, `:622`) is inert on both fixtures only because `min_width_top_surface` (300% = 1.35mm) swallows the 1.0mm exposed ring; the committed "site 3/4" tests measure site 1 |
| 3 | Minor B keep-core | **PASS for gap-fill-on, FAIL for gap-fill-off** - no under-absorb constructible (K subset-of S); but the re-test uses `min_claim_width/2` while the candidate test uses the widened `t`, so a 0.53mm keep-core is absorbed once gap fill is off (probe C5) and alternates per layer for w in (3.42, 3.62]mm at the default notch |
| 4 | Minor C per-colour widening | **PARTIAL** - bordering test correct, inert when every bordering colour has gap fill on; but resolution is per-colour OBJECT-WIDE, so the commit's own headline case (a same-colour modifier with gap fill off) still over-absorbs outside the modifier's Z range (probe E2); unit SECTION 1 exercises the empty-claim shortcut, not geometric non-adjacency |
| 5 | Determinism + honesty | **PASS** - `[paintdepth]` 74 / 1056 under `--order rand --rng-seed 1` and `2`, identical after stripping timing lines; no debug residue in the diff (one prose mention of `if (false)`); full suite **502 / 500 passed / 2 failed-as-expected, 51135 / 51133 / 2, exit 0** reproduced |
| 6 | Regression | **PASS** - `[chameleon]` 133 / 605 exact; `spike/verify_paintdepth.sh` 17/17 twice, exit 0 |

---

## Important

### I-A. The I1 pin's floor was tuned to the residual; the residual is real, unpinned and larger than reported, and a second thin-painted population is excluded rather than explained

**Where.** `tests/libslic3r/test_paint_depth_clamp.cpp:4013-4014` (`min_area_mm2 = 1.0`),
`:4022` (Extruder 2 only), `:3988-3999` (justification), absorb-tail-fixwave-report.md L48-49,
progress.md L76; production `MultiMaterialSegmentation.cpp:2816`, `:2859`.

**Measured on the committed binary** (probe A, sphere fixture, thin = empty under
`opening_ex(., 0.1125mm)`, interior = inside `offset_ex(lslices, -0.87854mm)`):

| floor (mm2) | Extruder 2 components (max) | Extruder 3 components (max) |
|---|---|---|
| 0 | 16 (0.315) | 20 (4.025) |
| 0.01 | 7 (0.315) | 2 (4.025) |
| 0.03 | 5 (0.315) | 2 (4.025) |
| 0.1 | 3 (0.315) | 2 (4.025) |
| 0.3 | 1 (0.315) | 2 (4.025) |
| 1.0 | 0 | 2 (4.025) |

`has_painted_unopened_fragment(object, 0.87854, 0.1125, floor)` as committed returns **true at
floor 0.3 and 0.03, false only at 1.0**. So: the report's "0.3mm2 floor" does not exist in the
commit; its provenance claim ("3x this file's own `sqr(scale_(0.1f))`") is wrong by 10x
(`sqr(scale_(0.1f))` at `MultiMaterialSegmentation.cpp:3209` is 0.01mm2, 3x = 0.03mm2, and the
committed 1.0mm2 is 100x it); the residual 0.315mm2 is above, not "below", 0.3 (progress.md
L76 "residual <=0.32mm2, below a ... 0.3mm2 floor"); and the test comment at `:3996` ("1.0mm2
sits with real margin below every pre-fix value (0.72, 3.12)") is false - 1.0 > 0.72. The only
reading consistent with all three documents is that the floor started at 0.3, the pin failed on
the 0.315mm2 fragment, and the floor was raised to 1.0 with the prose left behind.

**What the residual is.** The largest post-fix Extruder-2 fragments are 0.315mm2 (bbox
1.81 x 2.51mm, i.e. a ~0.1mm-wide curved finger ~3mm long, layer 145), 0.133mm2, 0.128mm2
(bbox 2.69 x 2.93mm), 0.079mm2 - all at layers 144-146, exactly the site the report names for the
pre-fix leak. The new formula `full \ (excess AND other)` with `excess = full \ legacy_raw`
exempts `full AND legacy_raw` from the clip, so every un-opened finger of the raw shadow still
carves a thin finger of `full` out of the clip - the raw legacy no longer ADDS area but still
SHAPES the final claim. The report's own before/after (single 0.72 / 3.12mm2 fragment -> several
<= 0.32mm2) describes a reduction, not the "restored #7104 guarantee" the commit message claims;
Arachne prints each such finger as a 0.34mm bead of the wrong colour, Classic as a sub-bead void.

Extruder 3 additionally carries a **0.2mm-wide ring 6.45mm in diameter (4.03mm2) at layer 146
and a 5.96mm one (3.54mm2) at layer 147** - thin, entirely interior, un-pinned. The report
excludes it as "bit-identical with or without the I1 fix" (not re-measurable here) and therefore
"unrelated"; that establishes only that the cross-colour clip is not its cause. Its position
(1.2mm inside the surface, at layers within the colour-2 cap's legacy depth - `top_shell_thickness`
0.6 => 6 layers) matches the merge loop's `diff_ex(lateral, top_and_bottom_by_extruder)`
(`:2886`) leaving a sub-bead remnant of colour 3's lateral band on the inside of colour 2's
legacy descent ring. It is the very "interior sliver class ... on the painted side where
`has_interclaim_sliver` cannot see it" that review I1 described, and the review's proposed pin
("no painted-colour component in `claim_for_layer(., 2|3)` ... empty under
`opening_ex(., 0.1125)` while inside `offset_ex(lslices, -0.87854)`" - both colours, no floor)
would have caught it. The committed pin was narrowed to Extruder 2 + 1.0mm2 precisely so it
would not.

**Fix.** (1) Stop the raw shadow from shaping the claim: apply the same `stat.normal_shell`-gated
`opening_ex(reach, stat.small_region_threshold)` to `reach` at `:2231-2241` (the review's
alternative; `stat` is in scope there), and/or open `full` once more after the clip. (2)
Re-measure the population at floor 0.001mm2 (the Clipper-noise ceiling `has_interclaim_sliver`
already documents is sub-1e-4mm2 - THAT is a derivable floor). (3) Pin BOTH painted colours with
that floor, or state and justify what residual is acceptable. (4) Explain or fix the Extruder-3
rings (a painted-side counterpart of the base-side absorb: a thin lateral remnant fully
enclosed by another claim should be handed to that claim, or the post-trim result opened). (5)
Correct the report, progress.md and the test comment (floor 1.0 not 0.3; 0.315 > 0.3;
3 x 0.01 = 0.03).

### I-B. Sites 3/4 are live and remain untested; the committed "site 3/4" tests are site-1 tests, and the report's explanation of why is the wrong pipeline order

**Where.** `test_paint_depth_clamp.cpp:809`, `:835` (metric `region_top_fill_area` `:693`),
`:795-806`; report "Sites 3 and 4 did not reproduce an isolated failure".

**Pipeline, as committed.** `PrintObject::make_perimeters()` (`PrintObject.cpp:293`) runs first
and `PerimeterGenerator` appends to `fill_surfaces` ONLY as `stInternal` (`PerimeterGenerator.cpp:1745`
classic, `:2614` Arachne). `prepare_infill()` (`:400`) then calls `detect_surfaces_type()`
(`:420`), which ends with `layerm->slices_to_fill_surfaces_clipped()` (`:1655`;
`LayerRegion.cpp:163-181`): it CLEARS `fill_surfaces` and rebuilds it from the typed `slices`
clipped to `fill_expolygons`. So every `stTop` byte the tests sum comes from site 1's
classification, written AFTER the perimeter generator - not "seeded upstream before
PerimeterGenerator ever runs" as the report and `:795-806` say. The conclusion (the metric cannot
see sites 3/4) is right; the reason given is inverted, and it hid the metric that can.

**Measured (probe B, base region, layer `first_sandwich - 1` = 49, walls default 2):**

| fixture / generator / only_one_wall_top | ON: perimeter len / fill area / stTop | OFF: len / fill / stTop |
|---|---|---|
| sandwich, Arachne, true | **302.65mm / 1508.64mm2** / 226.03 | 312.89 / 1472.37 / 0 |
| sandwich, Arachne, false | 312.89 / 1472.37 / 224.88 | 312.89 / 1472.37 / 0 |
| sandwich, Classic, true | 312.80 / 1470.73 / 224.75 | 312.80 / 1470.72 / 0 |
| flat, Arachne, true | **311.56 / 1483.81** / 57.09 | 312.97 / 1475.17 / 0 |
| flat, Classic, true | 312.80 / 1471.88 / 57.29 | 312.80 / 1471.88 / 0 |

Site 4 (`PerimeterGenerator.cpp:2273`) changes the perimeter generator's own output on BOTH
fixtures (10.2mm less wall, 36mm2 more fill on the sandwich); `only_one_wall_top=false` gives
ON == OFF, as it must. Neither `perimeters` nor `fill_expolygons` is touched after
`make_perimeters` (`Layer.cpp:260`, `:290` are the only writers), so `len_on < len_off` (or
`fill_on > fill_off`) is a mutation-sensitive site-4 pin: drop the `&& paint_depth_solid_interfaces`
at `:2273` and OFF becomes identical to ON. The committed test would still pass (the report
measured exactly this: "0 failures").

Site 3 (`:622`, classic `split_top_surfaces`) is inert on both fixtures for a structural
reason: the same-region-uncovered band is the 1.4357mm painted rim, the first loop already
inset `last` by 0.44mm, and `upper_polygons_series_clipped` is grown by `min_width_top_surface`
= 300% x 0.45 = 1.35mm (`PrintConfig.cpp:1231-1244`), so `top_polygons = diff(last, upper_grown)`
is empty and `last` is left untouched. It is live whenever the uncovered band exceeds ~1.8mm -
e.g. a two-volume object whose upper volume is a different region over the whole footprint
(another extruder, or a fully painted Z-face) plus any paint so `has_bounded_paint_depth()` is
true; that is a common real-world shape and is untested.

**Fix.** Replace the `stTop` metric in both tests with the perimeter generator's own output
(perimeter length via a recursive walk of `layerm->perimeters`, or total `fill_surfaces` area)
for Arachne now; build a wide-band fixture (or set `min_width_top_surface = 0`) for Classic;
correct the comment at `:795-806` and the report's causal story.

### I-C. Minor C is not fixed for its own motivating scenario: per-colour resolution is object-wide, so a same-colour modifier with gap fill off still widens the absorb everywhere

**Where.** `MultiMaterialSegmentation.cpp:3508-3529` (MAX over every region of the colour,
regardless of layer), `:2717-2740`, `:3001-3003`.

**Measured (probe E, 3.47 x 40 x 10mm box, all four sides painted Extruder 2, keep-core
0.599mm):** E0 gap fill on everywhere: centre stays base (island 0.599mm) at z = 2.6. E2: same
object plus a `PARAMETER_MODIFIER` covering z 9-10 with `gap_infill_speed = 0` -> regions
`[wf=1 gap=30] [wf=1 gap=0] [wf=2 gap=30] [wf=2 gap=0]` -> the 0.599mm base core at z = 2.6 - a
layer on which every region present has gap fill ON - is absorbed into Extruder 2 (`centre_in2=1`).
That is the review's M4 sentence verbatim ("one modifier volume with gap fill off ... widened the
kill width ... over-absorbing a genuine 0.45-0.75mm base gap"), and the commit message's own
example ("so one modifier volume with gap fill off widened the absorb everywhere"). The fix
helps only when the gap-fill-off region belongs to a colour that does not border the island.

**Fix.** Resolve per LAYER and per colour: build `claim_width_gapfill_off[layer][colour]` from
the regions with non-empty `slices` on that layer (the discipline `layer_color_stat` already
uses at `:1815-1843`) and pass that to `merge_segmented_layers`; keep the per-island bordering
test. Add a mesh-level mixed test (a modifier-based fixture like probe E is 30 lines) and give
unit SECTION 1 a present-but-non-touching claim (see m3).

### I-D. Minor B's keep-core exemption does not track the widened candidate threshold, so the M3 over-absorb (with its per-layer alternation) survives in the gap-fill-off configuration

**Where.** `MultiMaterialSegmentation.cpp:3029` (`t_keep_core = min_claim_width/2`) vs `:3003`
(`t = effective_claim_width/2`).

**Measured (probe C, 40mm-long 6mm-tall box, all sides Extruder 2, interlock 0, centre probe at
mid-height):** 3.10 / 3.30mm: base (keep-core 0.229 / 0.429mm, exempted); 3.40mm gap fill on:
base (0.529mm, the absorb's own "printable core" test); **3.40mm gap fill OFF: absorbed
(`centre_in2=1`, no base island left)** - the 0.529mm core is thin under the widened
t = 0.375 (kill 0.75) but the re-test opens it at 0.225 and calls it "not the thin residue".
With the default 0.1mm notch, w in (3.42, 3.62]mm alternates absorbed (odd layers) / kept (even
layers, core > 0.75) - exactly the region churn M3 flagged, now only when gap fill is off.
Gap-fill-on results: 3.20 / 3.10mm with the default notch stay base on consecutive layers
(0.529 / 0.329 and 0.429 / 0.229mm) - no alternation; a +X = colour 2 / -X = colour 3 wall keeps
its 0.229mm base residue (a base hairline between two painted colours - the documented policy).
**Under-absorb:** none constructible. `keep_core` is subtracted from every lateral claim
(`:1336-1338`), so a keep-core component K that touches a base island S satisfies K subset-of S,
and opening is monotone, so S thin => K thin under the same delta - the re-test can only differ
from the candidate test when the deltas differ, which is precisely the gap-fill-off case above.
The one remaining shape - a genuine sliver attached to an exempt residue stays base with it - is
inherent to a whole-component absorb and acceptable.

**Fix.** Use the island's own `t` at `:3029` (one token). Identical for gap fill on
(t == t_keep_core), policy-consistent for gap fill off. Pin with a gap_infill_speed=0 variant of
the Minor-3 test at 3.40mm.

---

## Minor

### m1. The I1 correctness argument's premise is false; the behaviour change is broader (and better) than reported, and untested
`:2839-2858` and the report say the new formula is "provably identical to the old one whenever
`legacy` subset-of `full` (the invariant that held before Item 2)". `legacy` (`reach`,
`:2231-2241`) is by design never priority-trimmed (`:2296`) nor clipped by other colours'
surface contributions (`:2286`) - the code's own comment at `:2216` says so - while `full` is
both. So the invariant never held on any layer where a colour's legacy band overlaps a
lower-index colour's claim or another colour's surface, Item 2 or not; there the OLD formula
resurrected trimmed geometry (a double claim between painted colours) and the NEW one does not.
Probe A: pairwise overlap of the Extruder 2 and 3 claims across all 160 sphere layers post-fix =
0mm2. Fix: state the real argument ("subtract-only: can never add area to a trimmed claim"), and
add a pairwise-disjointness pin - it is the cheapest strong invariant this loop has.

### m2. Sandwich fixture comment is wrong about its own geometry
`:290` "a THIN (2-layer) fully Extruder2-painted slab": `ALL_SIDE_FACE = {4..11}` are the side
facets (`its_make_cube`: 0-3 are bottom/top), so the slab's 37mm interior is base and the
mechanism is a 1.44mm painted rim, not a fully painted slab. The test still discriminates site 2
(per the report's mutation table); the comment does not describe why.

### m3. Unit SECTION 1 does not test geometric non-adjacency
`:3557` leaves `claims[3]` EMPTY, so the "non-bordering colour" case exits through the
`painted_claims[color_idx].empty()` shortcut at `:2728` and the `intersection_ex(dilated, ...)`
adjacency test is only ever exercised in the positive direction. Add a non-empty, non-touching
claim (e.g. `absorb_test_rect(3000, 0, 4000, 1000)`).

### m4. Index alignment of `claim_width_gapfill_off_by_color` is by convention only
Indexed by `wall_filament` (`:3524-3529`), consumed by painting state index (`Extruder1 = 1`,
`TriangleSelector.hpp`), the same convention `layer_color_stat` relies on at `:1855` - correct,
but the base region's own `wall_filament` (usually 1) also lands in the array and would widen
islands bordering an Extruder-1-painted claim from the BASE region's gap-fill setting; document,
or skip regions whose config is the object's base config.

### m5. Report / progress.md / commit message assert things the commit does not contain
Floor 0.3mm2 (code: 1.0); "3x sqr(scale_(0.1f))" (= 0.03mm2); "residual <=0.32mm2, below ... 0.3mm2";
"restores the #7104 guarantee" (residual remains); "pre-seeded upstream before PerimeterGenerator
runs" (it is re-typed downstream); "one modifier volume with gap fill off widened the absorb
everywhere ... replaced" (it still does for the same colour). None of these is a code defect on
its own; together they are the pattern the track record warns about.

### m6. Per-island keep-core cost
`:3030-3038` opens every keep-core component an island overlaps, per island; on a thick object
the component is the whole interior and the opening is paid once per candidate island per layer.
Cache the per-component thinness verdict once per layer (candidates are rare, so this is a nit).

Minor A (winner-area comment) - PASS, no finding: `Polygon::area` (`Polygon.cpp:47`, `:61`) and
`ExPolygon::area` (`ExPolygon.cpp:50`) are `double`; the rewritten comment at `:2666-2689` and
the header at `MultiMaterialSegmentation.hpp:87-92` are accurate; the sequential-loop determinism
argument holds.

---

## Check details (what was executed / traced)

**1a.** `full` at `:2814` is `triangles_by_color_merged` = surface contributions + band-opened
descent (`:2280-2284`) minus `painted_exploys` (`:2286`), priority-trimmed (`:2296`); `excess`
(`:2816`) = `full \ legacy_raw`; the new `:2859` subtracts only. The exempt set `full AND legacy_raw`
inherits every raw finger of the shadow (`:2123` / `:2190` copy the un-opened `last` whenever
`normal_shell`). Post-fix population: probe A table above. Pairwise 2x3 overlap 0mm2.
**1b.** With `excess = full \ legacy` and `legacy` subset-of `full`: `full = legacy (disjoint-union)
excess`, so `full \ (excess AND other) = legacy OR (excess \ other)` - correct as sets (Clipper
output differs only in boundary rounding and in `append` vs `diff_ex` normalisation). Premise:
m1.
**1c.** Floor provenance: `remove_small_and_small_holes(ex_polygons, sqr(scale_(0.1f)))` at
`:3209` = 0.01mm2. Committed floor 1.0mm2 (`:4014`). Probe A: committed pin true at 0.3 / 0.03,
false at 1.0.
**2.** `has_bounded_paint_depth` consumers at the commit: `LayerRegion.cpp:227` plumbing,
`PerimeterGenerator.cpp:622` / `:2273`, `PrintObject.cpp:1338` / `:1773`, `Print.hpp:514` - no fifth
consumer, unchanged from the previous review. Perimeter-output metrics from probe B; pipeline
order `PrintObject.cpp:293` -> `:400` -> `:420` -> `:1655`; `LayerRegion.cpp:163-181`;
`PerimeterGenerator.cpp:1745`, `:2614`; `min_width_top_surface` default `300%`
(`PrintConfig.cpp:1244`); `wall_loops` default 2 (`:4758`) - hence `loops=2` in the probe.
**3.** `cut_segmented_layers` subtracts `keep_core` from every lateral claim (`:1336-1338`) and
now records it (`:1333`); `paint_depth_clamp_keep_core` (`:1239-1275`) at walls=3 never enters
the ladder (`b = 0.359 < min_claim_width`), so keep-core = `offset(layer, -band)` exactly; band
1.435675mm, wall_stack 0.878540mm (= 0.45 + 0.42854) re-derived. Probe C widths match to 1e-5mm.
**4.** `interclaim_absorb_effective_claim_width` (`:2717-2740`): lazy dilation, MAX semantics,
empty array -> `min_claim_width` (fuzzy caller `:3667`), zero entries never pay geometry. Probe E.
**5.** Logs: `paintdepth_seed1.log` / `_seed2.log` (74 / 1056 both; sorted, timing-stripped
diff shows only `Passed in ... [seconds]` lines), `fullsuite.log` (502 / 500 / 2 xfail:
`test_mixed_filament.cpp:3483`, `:4429`, the same `[!shouldfail]` cases as before; 51135 / 51133 /
2; exit 0). Residue grep over the added lines (`printf|cout|cerr|if \(false|TEMP|#if 0|getenv|
DIAG|UNGATE`): one prose hit in a test comment.
**6.** `chameleon.log` 133 / 605; `verify1.log` / `verify2.log` 17/17, `RESULT: ALL PASS`, exit 0.
