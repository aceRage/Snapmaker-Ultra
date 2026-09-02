# Absorb-tail fix-wave 2 review: `f3075afc50` (feat/paint-depth, worktree `C:\Dev\SnapmakerOrcaNext`)

Scope: the single commit `f3075afc50` reviewed AS COMMITTED (`git show`), against
absorb-tail-fixwave2-report.md and the review it answers (absorb-tail-fixwave-review.md I-A..I-D +
the disjointness minor). Read-only; every number below was produced in this session on the
committed binary or on a probe binary built from the committed sources (see "Method"). Working-tree
edits by the parallel flat-top-cap agent were not present at snapshot time (`git diff HEAD --stat`
= progress.md only) and cannot have reached any measurement here.

**Verdict: FIX FIRST.** 0 Critical, 2 Important, 6 Minor.

The two report claims that do not survive hand-execution: (1) I-C's per-layer kill width is
resolved from a per-layer signal that the sliver GENERATOR does not use - `layer_color_stat`'s
per-colour `small_region_threshold` is last-region-wins and deliberately slice-ungated (N1), so
with a gap-fill-off modifier anywhere on the object every layer's painted descent is eroded at the
gap-off width while the absorb now widens only on the modifier's own layers; measured: 8.1 and
6.0 mm2 base slivers at the sphere's cap boundary (layers 149/150) with the modifier at z 2-3,
where the parent commit absorbed them - a regression on exactly the configuration class I-C
targets, and the committed I-C test cannot see it (I-D alone makes that test green). (2) The
Extruder-3 ring is not "unrelated to any paint-depth code": its inner edge is the clamped lateral
band's inner edge to 3 decimals and its outer edge is the first non-legacy descent origin, i.e. it
is the remnant that Wave B's cross-colour clip (`full \ (excess ∩ other_painted_laterals)`, the
very line this wave re-commented) deliberately leaves to colour 3, and the commit pins that
artefact as expected behaviour. It is interior and colour-3 (not base, not visible) - see check 1b.

---

## Method / binary faithfulness

- `build/tests/libslic3r/Release/libslic3r_tests.exe` 19:27:29 and `libslic3r.lib` 19:27:25
  postdate every source mtime (`MultiMaterialSegmentation.cpp` 19:25:02,
  `test_paint_depth_clamp.cpp` 19:11:34, `PrintObject.cpp` 19:16:40 / `PerimeterGenerator.cpp`
  19:19:14 = the mutation reverts); commit 19:29:53. Both were copied to the scratchpad at 19:35
  with sha256 `3bdfcc12...` (exe) / `86cb1a02...` (lib) before any run; all suite runs below used
  the snapshot. `snapmaker-orca.exe` sha `667d2efa...` identical before and after both verify runs.
- Probe binary: the committed `test_paint_depth_clamp.cpp` (`git show f3075afc50:...`) plus six
  `[reviewprobe]` TEST_CASEs appended, compiled with the tlog's exact `cl` line (toolset
  14.44.35207) and linked with the tlog's exact `link` line against the committed build's own
  objects and the SNAPSHOTTED `libslic3r.lib` (only `TEST_PAINT_DEPTH_CLAMP.OBJ` swapped). Sources
  and logs: `%TEMP%\claude\C--Dev\85fd2715-...\scratchpad\{probe_tests.cpp,probe_tests_f.cpp,
  probe_run.log,probe_f_run.log,paintdepth_*.log,chameleon.log,fullsuite.log,verify1.log,verify2.log}`.
- Not re-run: the four I-B source mutations (they need a libslic3r rebuild in a worktree shared
  with a parallel implementer). See check 2 for the consistency evidence I could obtain instead.

## One-line result per mandatory check

| # | Check | Result |
|---|---|---|
| 1a | I-A floor gone / exact zero / parity | **PASS** - `has_painted_unopened_fragment(obj, 2)` re-measured false with no floor; `legacy` has exactly one reader (`:2907`); the new opening at `:2316-2317` is gated on `stat.normal_shell`, which is false on every layer/colour when `paint_depth_normal_mm == 0` (`:3820`, unlimited) and for colour 0, so unlimited is untouched by construction; the three legacy-parity tests (`:447`, `:658`, `:3943`) are green; probe A in pdmUnlimited: no thin component either colour. In the continuous domain the opening cannot delete surface geometry (`top_ex` was already opened with the same threshold at `:1974`, and opening(A ∪ B) ⊇ A for open A); the only Clipper-scale losses are boundary nibbles, which the 5.2e-9 mm2 disjointness residual is consistent with |
| 1b | Extruder-3 ring | **FAIL as explained, benign as printed** - it is the Wave-B cross-colour clip's remnant of colour 3's clamped lateral band (paint-depth code), not "legacy lateral-vs-top/bottom trimming"; a 0.2 mm colour-3 stripe with colour 2 on both sides, >= 0.9 mm inside the contour, both colours already on the layer: not visible, not base-coloured, no toolchange added; but pinned as expected (`:4237`) with a false cause and a stale refuted-hypothesis comment (`:4146-4156`) - Important I-2 |
| 2 | I-B pins | **PASS on direction/magnitude**, mutations not re-run - site 4 `15634061952 < 15644293200` (ON shorter by 10.23 mm total, i.e. the whole effect sits on the one interface layer, matching the prior review's 302.65 vs 312.89 mm single-layer probe); site 3 `15629589160 != 15640000400` (ON shorter by 10.41 mm - `<` holds, `!=` is weaker than the data, m1). The report's mutated-equal values are exactly my ON values, consistent with `interface_shells \|\| has_bounded_paint_depth` collapsing OFF onto ON. `PrintObject.cpp` / `PerimeterGenerator.cpp` are not in the commit and the working tree is clean: byte-identical to `8c5bf752de`; no `FIXWAVE2-MUTATION`/`UNGATE` residue |
| 3 | I-C per layer | **FAIL** - per layer and geometry-derived on the ABSORB side, inert with gap fill on (control: 0 slivers), but the sliver GENERATOR's threshold is a config-order artefact (`:1869-1874`, last matching region wins, slice-ungated by N1) and the two now disagree: sphere + gap-off modifier at print z 2-3 -> `has_interclaim_sliver(0.75)` TRUE, 8.14 mm2 at L149 / 6.01 mm2 at L150; the committed test is non-discriminating (I-D alone greens it) - Important I-1 |
| 4 | I-D `t_keep_core = t` | **PASS** - right quantity: the exemption is meant to recognise the residue the ladder deliberately preserved under the SAME delta the outer test just used; a wider `t` can only exempt keep-core components (policy M3 says never absorb them), never a non-keep-core sliver. Sweep w = 3.3..3.7, gap off and on, both parities: centre base everywhere; at 3.5 odd = 0.6286 mm (inside (0.45, 0.75], the pre-fix absorb) / even = 0.8286 (above 0.75), so the fixture straddles the churn window as claimed |
| 5 | Disjointness pin | **PASS on data, stale provenance** - sphere: one 5.215e-9 mm2 fragment at L148 only; sphere gap0, sphere unlimited, frustum negY/posX (Arachne and Classic), frustum negY/posY gap0, cap+plusX box: exactly 0 on every layer. The `0.0001 mm2` "already documented in this file" citation points at a `has_painted_unopened_fragment` header comment this same commit deleted; the surviving documented number is `has_interclaim_sliver`'s 0.00002 mm2 (m2) |
| 6 | Re-runs / residue | **PASS** - `[paintdepth]` 87 / 1197 under default order, `--rng-seed 1` and `2`, byte-identical after stripping timing lines; `[chameleon]` 133 / 605; full suite `515 \| 513 passed \| 2 failed as expected`, `51276 \| 51274 \| 2` (`test_mixed_filament.cpp:3483`, `:4429`), exit 0; `verify_paintdepth.sh` 17/17 `RESULT: ALL PASS` x2, exit 0; no debug residue in the commit's added lines |

---

## Important

### I-1. I-C's per-layer kill width tracks a signal the sliver generator does not use; a gap-fill-off modifier anywhere now leaves 0.45-0.75 mm base slivers on every layer it does not cover (regression vs `8c5bf752de`), and the committed I-C test cannot see it

**Where.** `MultiMaterialSegmentation.cpp:3562-3581` (per-layer narrowing: `:3567` Z-applicability
= any region with non-empty `slices` and `gap_infill_speed <= 0`; `:3575` presence =
`segmented_regions[layer][colour]` non-empty), consumed at `:3100-3102` / `:3122-3124`; the
generator side `:1862-1877` (`layer_color_stat` per-colour block: `out.small_region_threshold =`
ASSIGNED for every region with `wall_filament == colour`, last one wins, deliberately not gated on
`slices` - N1). Test `:4469-4548`.

**Measured (probe C, `slice_bounded_sphere_two_colours` geometry + a 16x16 mm
`PARAMETER_MODIFIER` slab with `gap_infill_speed = 0`; kill width from the production formula
= 0.7500 mm):**

| configuration | regions() order at L136 (wf/gap/slices) | sliver@0.45 | sliver@0.75 | thin interior base components |
|---|---|---|---|---|
| no modifier, gap on (control) | 1/30/SET, 2/30/SET, 3/30/SET | 0 | 0 | none |
| modifier at print z **2-3** | 1/30/SET, 1/0/empty, 2/30/empty, 2/0/empty, 3/30/SET, **3/0/empty** | 0 | **1** | **L149 8.136 mm2, L150 6.015 mm2** |
| modifier at print z 13-15.5 | 1/30/empty, 1/0/SET, ..., 3/0/SET | 0 | 0 | none |
| object-wide gap 0 | 1/0/SET, 2/0/empty, 3/0/SET | 0 | 0 | none |

Mechanism. With the modifier present, colours 2 and 3 each have TWO PrintRegions (`[wf=2 gap=30]`
then `[wf=2 gap=0]`, `PrintApply.cpp:1082-1123` creates the painted variants per parent volume
region in that order). `layer_color_stat` iterates `layer.regions()` and, for the per-colour block,
overwrites `small_region_threshold` on every match regardless of `slices` (`:1869-1874`), so the
LAST variant - the gap-off one - wins on EVERY layer: the per-colour opening at `:1974` /
`:2189` and the band-level opening at `:2359-2360` erode both colours' descent at the gap-off
width everywhere, producing the 0.45-0.75 mm inter-claim population Item 2 documented. Before this
wave the absorb widened object-wide too, so generator and absorb happened to agree. After this
wave the absorb widens only where a gap-off region has geometry (`:3567`), so on every other
layer the population is generated at 0.75 and killed at 0.45. Probe C's L149/L150 slivers are at
the cap boundary (colour 2 takes the contour at L149), inside the cap's own solid top shell - the
Item 2 defect class ("prints body-coloured between two painted features"), hidden under 4-5
layers of cap here, but the parent commit absorbed them and this one does not. The report's
"a layer with NO gap-fill-disabled region anywhere nearby ... correctly gets nothing" is the
wrong half of the truth: that layer's population was NOT generated at the gap-on width.

**The committed test does not pin the mechanism.** In the I-C fixture the z ~ 2.6 mm centre
island IS the keep-core component (probe E: base width 0.5987 mm = 3.47 - 2 x 1.435675), so
with I-D's `t_keep_core = t` (`:3166`) it is exempted at `:3171-3178` whatever
`effective_claim_width` is: I-D alone turns `:4519` green. The report's RED run stashed the whole
production file (both fixes reverted together) and therefore cannot distinguish. Deleting
`:3562-3581` would leave the suite green.

**Also (theoretical, no bite measured).** `:3575` gates presence on `segmented_regions` - LATERAL
presence - while the absorb's neighbours are the MERGED claims (`interclaim_absorb_effective_
claim_width` already tests adjacency against `merged`, `:2811`). A colour present on a layer only
via its top/bottom descent never widens the kill width even with gap fill off object-wide. Probe
F (colour 3 confined to nz > 0.75 so layers 124-139 have no lateral paint at all, gap off) found no
surviving sliver there, so this is a latent inconsistency, not a measured defect; the gate is
redundant with the adjacency test and should go.

**Fix.** Make generator and absorb read the SAME per-layer quantity, and make that quantity
geometric: (1) in `layer_color_stat`'s per-colour block, resolve which variant of a colour applies
on this layer via its PARENT volume region's `slices` (the parent is known:
`PrintObjectRegions::PaintedRegion::parent` -> `layer_range.volume_regions[parent].region
->print_object_region_id()`, `PrintApply.cpp:1116`; fall back to today's last-wins only when no
variant's parent has geometry), so a Z-confined modifier stops widening the opening on layers it
does not cover; (2) build `claim_width_gapfill_off_by_layer_color[layer][colour]` from
`2 * unscaled(stat.small_region_threshold)` of that same stat (the gap-off arm is exactly
`ext_perimeter_width + 0.7 * spacing`, i.e. the current object-wide formula), clamped below by
`min_claim_width`, so the absorb can never disagree with the erosion that created the population;
(3) drop the `segmented_regions` presence gate (the per-island adjacency test already covers it);
(4) pin with the probe-C fixture (sphere + gap-off modifier at z 2-3: `has_interclaim_sliver(obj,
0.87854, 0.75)` must be false) and keep the box test as the keep-core regression it actually is.

### I-2. The Extruder-3 ring is the Wave-B cross-colour clip's remnant of colour 3's clamped lateral band - paint-depth code - and the commit pins the artefact as expected behaviour under a false causal claim

**Where.** `tests/libslic3r/test_paint_depth_clamp.cpp:4232-4238` (`CHECK(has_painted_unopened_
fragment(*object, 3))` asserts the defect is PRESENT), `:4240-4249` (negative case), header
`:4143-4156` (still says "it reproduces on the SAME fixture in pdmUnlimited ... yet the SAME ring
persists" - the hypothesis the report says it refuted; the TEST_CASE two lines below asserts the
opposite); production `MultiMaterialSegmentation.cpp:2969` (the clip), `:2180-2181` (legacy =
descent steps with `m < top_shell_layers`), `:1309` / `:1330-1338` (clamp + keep-core cut).
Report I-A "Extruder-3 ring ... the interaction of TWO mechanisms neither Wave B nor this
fix-wave touches ... unrelated to any paint-depth code at all".

**Measured (probe A, sphere pdmWalls, radial layout from the slice centre, mm):**

| layer (parity) | r_slice | colour-2 disk | **colour-3 ring** | colour-2 ring | colour-3 lateral | r_slice - band | r_slice - wall_stack |
|---|---|---|---|---|---|---|---|
| 146 (even, notched band 1.3357) | 4.363 | 0.38-3.00 | **3.03-3.20** (4.025 mm2, thin, interior) | 3.20-3.45 (5.04 mm2) | 3.45-4.36 | **3.027** | 3.485 |
| 147 (odd, band 1.4357) | 4.232 | 0.38-2.78 | **2.80-2.96** (3.537 mm2) | 2.97-3.35 (5.41 mm2) | 3.35-4.23 | **2.796** | 3.354 |
| 148 | 4.102 | - | 9 zero-area fragments at 2.96-2.98 | 3.20-3.23 outer edge | 3.20-4.10 | 2.766 | 3.224 |

The ring's INNER edge equals `r_slice - region_cut_width` (the clamp's notched/un-notched band)
on both parities to three decimals; its OUTER edge steps inward by one descent slab per layer
(3.20 -> 2.96 -> 2.97-3.20 region boundary) and is the boundary between the colour-2 contributions
that are `legacy` (m < top_shell_layers, exempt from the clip) and those that are `excess`
(clipped where they overlap colour 3's lateral claim). So the gap in colour 2's claim that the
ring fills is exactly `excess ∩ other_painted_laterals` - the subtrahend at `:2969`. The
per-extruder trim (`:2984-2988`) only subtracts top/bottom claims FROM laterals; nothing else in
the pipeline subtracts a lateral band from a top/bottom claim. Without the clip colour 2 would own
r < 3.45 solid and colour 3's remnant would be the 0.9 mm outer band, not a thin ring; without the
clamp the remnant would extend to the centre. Both are required, and one of them is Wave B's
Important-2 code. The negative pdmUnlimited case does not discriminate: in unlimited mode the
clamp is off AND the descent is legacy-gated (no `excess` at all, probe A: colour 3 owns the full
disk at L145-148 and colour 2 has no claim there), so the ring's absence there is consistent with
either explanation.

**Is it a visible defect?** No. The stripe is colour 3, not base; it sits >= 0.9 mm inside the
contour under the cap's shell; colour 3 already prints the outer band on the same layer so no
toolchange is added. As printed: Arachne widens the 0.2 mm ring to a min-width bead of the wrong
colour inside colour 2 (hidden); Classic emits nothing for it (a hairline void). It is the
"interior sliver class on the painted side" the previous review named, and its cost is quality
noise, not fidelity. The finding is the misattribution plus the pin: `:4237` will FAIL the moment
anyone fixes the clip's thin remnant, and the header comment records the refuted hypothesis as
proof.

**Fix.** Either (a) painted-side counterpart of the base absorb at the clip site: after
`:2969`, any component of `other_lateral \ full` (per neighbour colour) that is thin under that
colour's `small_region_threshold` AND fully enclosed by this colour's claim is handed to this
colour (or simply: do not exempt the neighbour's lateral band where the remnant would be
sub-bead), or (b) leave the geometry but replace `:4232-4249` with a test that documents the
class without asserting its presence (e.g. pin the ring's WIDTH bound and enclosure so a future
fix is a green delta, not a red one). In both cases rewrite `:4143-4156` and the report's I-A
paragraph with the clip-based cause above.

---

## Minor

### m1. Site-3 pin asserts `!=` where the data supports `<`
`:994` `CHECK(len_on != len_off)`; measured ON 15629589160 < OFF 15640000400 (ON shorter by
10.41 mm, the same direction and the same one-layer magnitude as site 4's 10.23 mm). `!=` passes
for any perturbation; tighten to `<` with the site-4 rationale (ON's same-region upper slices see
less coverage -> more "top" -> fewer inner loops).

### m2. Disjointness ceiling provenance is stale
`:4260-4269` cite "has_painted_unopened_fragment's own documented Clipper-rounding ceiling
(sub-0.0001mm2)"; that header text was deleted by this commit (`:4128-4156` no longer mention it;
`grep 0.0001` hits only the disjointness test itself). The surviving documented figure is
`has_interclaim_sliver`'s "~430 sub-0.00002 mm2 Clipper fragments" (`:4109`). Cite 0.00002 (the
measured 5.2e-9 is still 4000x below it) or measure a ceiling of its own.

### m3. The I-C test's in-modifier CHECK measures the base colour's top-shell claim, not the keep-core
`:4520-4547`: the comment says the z ~ 9.5 mm centre "stays base ... for a DIFFERENT reason
(I-D's keep-core protection)". Probe E: the base island there is **1.7129 mm** wide =
3.47 - 2 x 0.878540 - the unpainted top face's base top/bottom claim (F1-inset to one wall stack)
covering the solid top shell, five layers under the top. It is neither thin nor a candidate;
`:4547` cannot fail for any absorb behaviour. Move the modifier to mid-height (z 4-5 on the 10 mm
box, clear of both shells) if that direction is to be probed, and fix the comment.

### m4. I-C is per layer but not per XY - undocumented half of the "documented imprecision"
`:3562-3581` widens every painted colour present on a layer once ANY region on that layer has gap
fill off; a modifier confined in XY (a corner of a plate) widens islands on the far side of the
same layer. The report documents only the multi-colour/multi-Z case. Document, or resolve per
island against the modifier's own footprint (the parent region's `slices` are available).

### m5. Stale refuted-hypothesis text left in the `has_painted_unopened_fragment` header
`:4146-4156` (see I-2): "it reproduces on the SAME fixture in pdmUnlimited ... yet the SAME ring
persists ... proof that the ring predates and is independent of every line this feature has ever
touched" - contradicted by `:4248` `CHECK_FALSE` directly below and by the report's own account.
This is the "prose left behind" pattern the previous review's m5 flagged.

### m6. Mutation evidence is consistent but was not reproduced here
The four I-B mutations were not re-run (shared build directory). What could be checked: the
report's mutated-equal values (15634061952.0 site 4, 15629589160.0 site 3) equal my measured ON
values to the digit, which is what `interface_shells || has_bounded_paint_depth` predicts once
`&& paint_depth_solid_interfaces` is dropped; the two production files are byte-identical to the
parent commit. A future wave should include the mutation diff and the failing assertion text in
the report so a reviewer can verify without a rebuild.

---

## Check details (what was executed / traced)

**1a.** `legacy_top_and_bottom_layers` readers: `:2819` (param), `:2882` (assert), `:2899-2907`
(the clip, sole reader), `:3598-3604` (caller). `normal_shell` (`:1926-1928`) requires
`paint_depth_normal_mm > 0` and `color_idx > 0`; `:3820` forces it to 0 in pdmUnlimited. The
descent appends the RAW `last` to the shadow at `:2180-2181` / `:2256-2257` only when
`normal_shell` (per-step opening skipped at `:2168-2169` / `:2246-2247`); the band-level opening
`:2357-2361` and the new shadow opening `:2316-2317` both use `layer_color_stat(layer_idx,
color_idx).small_region_threshold` of the destination layer - identical quantity. `[paintdepth]`
legacy-parity tests `:447`, `:658`, `:3943` green; probe A pdmUnlimited `unopened2=0 unopened3=0`.
Probe A pdmWalls: `unopened2=0` (exact, no floor), thin-interior Extruder-3 components only at
L146 (4.025 mm2), L147 (3.537 mm2), L148 (9 x ~0 mm2).
**1b.** Probe A radial table above; `region_cut_width` parity at `:1309` (even -> `cut_width -
interlocking_depth`); legacy gate `:2180`; clip `:2969`; trim `:2984-2988`.
**2.** `-s` expansions from `paintdepth_success.log:1593-1597`, `:1645-1649`; gate shapes
`PerimeterGenerator.cpp:622`, `:2273` (`interface_shells || (has_bounded_paint_depth &&
paint_depth_solid_interfaces)`), `PrintObject.cpp:1338`, `:1773`; `git show --stat` (4 files, no
production file but MMS); `git diff HEAD --stat` = progress.md only; marker grep empty.
**3.** Probe C table above (`probe_run.log`), region creation order `PrintApply.cpp:1082-1123`,
per-layer LayerRegion construction `PrintObjectSlice.cpp:5199-5235`, generator assignment
`:1869-1874`, narrowing `:3562-3581`. Probe E (committed fixture): L25 `[1/30/SET][1/0/empty]
[2/30/SET][2/0/empty]`, centre base 0.5987; L94 centre base 1.7129. Probe F (`probe_f_run.log`):
no sliver at 0.45 or 0.75 in any of the four nz3_min x gap combinations.
**4.** `:3124` (`t` from `effective_claim_width`), `:3166` (`t_keep_core = t`), `:3167-3175`
(per-component re-test), `:1330-1338` (keep-core subtracted from every lateral claim). Probe D
widths (mm): w=3.30 0.6286/0.4286, 3.40 0.7287/0.5286, 3.45 0.7786/0.5786, 3.50 0.8286/0.6286,
3.55 0.8786/0.6786, 3.60 0.9286/0.7287, 3.70 1.0287/0.8286 (even/odd), `centre_in2=0` for all 28
(width x gap x parity) cases.
**5.** Probe B: sphere walls max 5.215e-9 mm2 at L148 (1 layer with any overlap); sphere gap0,
sphere unlimited, frustum negY/posX Arachne, frustum negY/posX Classic, frustum negY/posY gap0,
cap+plusX box: 0.000e+00, 0 layers. `grep -n "0\.0001\|0\.00002"` on the committed test file:
`:4109` (0.00002, has_interclaim_sliver), `:4261`/`:4269` (0.0001, the new test only).
**6.** `paintdepth_default.log` / `_seed1.log` / `_seed2.log`: `All tests passed (1197 assertions
in 87 test cases)` x3, sorted timing-stripped diff empty; `chameleon.log` 605/133;
`fullsuite.log` `test cases: 515 | 513 passed | 2 failed as expected`, `assertions: 51276 | 51274
passed | 2 failed as expected` (`test_mixed_filament.cpp(3483)`, `(4429)`), exit 0;
`verify1.log` / `verify2.log` `17/17 checks passed. RESULT: ALL PASS`, exit 0, CLI sha unchanged.
Residue grep over the commit's added lines (`printf|cout|cerr|if \(false|TEMP|#if 0|getenv|DIAG|
UNGATE|FIXME|XXX|TODO|FIXWAVE2-MUTATION`): no hits.
