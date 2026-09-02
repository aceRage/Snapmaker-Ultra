# Absorb-tail fix-wave 3 review: `0068687400` (feat/paint-depth, worktree `C:\Dev\SnapmakerOrcaNext`)

Scope: the single commit `0068687400` reviewed AS COMMITTED (`git show`), against
absorb-tail-fixwave3-report.md and the two reviews it answers (absorb-tail-fixwave2-review.md
I-1 / I-2 / m1; flat-top-cap-fixwave-review.md Minor 1). Read-only: no source edits, no commits.
Last gate before GUI round 4.

**Verdict: SHIP to GUI round 4.** 0 Critical, 1 Important, 8 Minor. The one Important is a
test-contract defect (the I-2 pin asserts the artefact's PRESENCE, not its benign properties)
that does not touch the binary; fix it in the next wave before merge, it need not hold the slice.
Every production change checked here does what the report claims; every number in the report
reproduces on a snapshot of the committed test binary; and the app DLL the user will slice with
is code-identical to the commit (the one edit after it was linked is a comment reflow, proven from
the fix-wave agent's own transcript, see check 5).

---

## Method / binary faithfulness

- Snapshot: `build/tests/libslic3r/Release/libslic3r_tests.exe` (21:02:01, sha256
  `8696f622...0de35d2`) plus its DLLs copied to
  `%TEMP%\claude\C--Dev\85fd2715-...\scratchpad\rev_0068\bin\` before any run; all suite runs
  below used the snapshot. It postdates every source mtime (`MultiMaterialSegmentation.cpp`
  21:00:41, `test_paint_depth_clamp.cpp` 20:55:53) and `libslic3r.lib` (21:01:52); commit
  21:05:26. Working tree: `src`/`tests` clean against HEAD.
- App binary: `build/src/Release/Snapmaker_Orca.dll` 20:59:26 (72 MB; `snapmaker-orca.exe` is a
  733 KB stub, sha256 `1b757aee...` = the report's). The DLL PREDATES the production source's
  last mtime by 75 s - resolved in check 5 via the fix-wave agent's transcript
  (`~/.claude/projects/C--Dev/85fd2715-.../subagents/agent-a76661f0022a5baa1.jsonl`), which is
  on disk and records every Edit/Bash call with timestamps.
- The fix-wave agent's own run logs are in this session's scratchpad (`red_run.log` 20:42:55,
  `green_run.log` 20:44:18, `minor1_red.log` 20:55:19, `minor1_red2.log` 20:56:47,
  `pd2_*.log` 20:58, `full2_*.log` 20:58, `all_build2.log` 20:59:30, `verify*_final.log`
  20:59:41/44) and were read, not just trusted. Dangling stash commits (`git fsck`) date the I-1
  bisection stash to 20:40:56 ("WIP on feat/paint-depth: b0b3db51dd").
- Not done: a probe build against the parent commit (shared build directory; the brief is
  read-only). Where the report claims "polygon-identical to the parent", this review argues it by
  construction from the code and by the 92 pre-existing `[paintdepth]` pins staying green.

## One-line result per mandatory check

| # | Check | Result |
|---|---|---|
| 1a | Shared stat: one colour on TWO volumes with different gap-fill settings on one layer | **PASS, answered** - both variants resolve "applicable", the LAST one in `layer.regions()` order wins for the WHOLE layer (`:1807-1812` overwrite; order = `print_object_region_id()` = creation order in `PrintApply.cpp:1032`, the modifier-parented variant is interned after the body's). Deterministic (creation order, no TBB influence), not geometric (per layer, not per volume/XY), identical to the parent's last-wins on such layers, and generator + absorb read the same value so nothing leaks. m4 |
| 1b | No behaviour change for single-volume / uniform models | **PASS by construction** - one candidate per painted colour; it resolves through its parent (the body has slices wherever the object does) and the fallback selects the same candidate where it does not; colour 0, `extrusion_width`, `num_regions` untouched (`:1788`, `:1813`); all 92 pre-existing pins (exact reach / area / polygon-count on sphere, frustum, box) green |
| 1c | Erosion threshold for the non-widened case (0.225 mm) unchanged | **PASS** - gap-on arm `0.5 * outer_wall_line_width` -> `scaled(0.1125)` textually unchanged (`:1791-1796`); the same value reaches all six erosion sites (`:2158`, `:2355`, `:2375`, `:2433`, `:2503`, `:2545`); gap-on objects skip the absorb construction entirely (object-wide pre-check vector empty, `:3755`) so the kill width stays `min_claim_width` |
| 1d | Graph walk cost bounded | **PASS** - O(R * (L + P)) per (layer, colour) call, R regions ~3-10, L ranges = 1, P painted entries <= colours x parents; the new `parallel_for` (`:3756`) runs only when a gap-off region exists; ~microseconds per call, sequential inner loop, per-layer writes only |
| 2 | I-2 attribution right; pin asserts benign properties, not existence | **Attribution PASS, pin FAIL** - inner edge = Stage-1 clamp band (`:1309-1330`, `keep_core` = interior eroded by `region_cut_width`), outer edge = the clip (`:3167`) removing colour 2's EXCESS (origins deeper than `top_shell_layers`, gate `:2366`) only where it overlaps colour 3's band: ring = band ∩ excess, exactly the header's account. But `:4410` `CHECK(has_painted_unopened_fragment(*object, 3))` asserts PRESENCE - Important I-1 |
| 3 | Site-3 pin `<` | **PASS** - `:999` `CHECK(len_on < len_off)`, measured ON 15629589160 < OFF 15640000400 (ON shorter by 10.41 mm), same direction as site 4 (`:958`) |
| 4 | Apex half-ring fix: no over-cap above the cliff; 1.5° control principled | **PASS** - half-ring width r/2 survives `opening_ex(., 0.75ws)` iff r/2 > 1.5ws iff r > 3ws, the full-ring rule; at 2.2° r/2 = 1.301 < 1.5ws = 1.318 mm (17 000 scaled units of margin, no Clipper risk) so the cap term is empty and the code path is identical to before; the 3/4/5° and 10/15/20° reach pins run through the new branch and stay exact; 1.5° is a control because r = 3.819 > 4ws = 3.514 caps under BOTH deltas - a function of ws, h, theta only, not of the 18 mm top. Caveats m5 |
| 5 | Regression sweep | **PASS** - snapshot: `[paintdepth]` 94/1568 default + seeds 1, 2, timing-stripped sorted logs identical; `[chameleon]` 133/605; TRUE full suite 522 \| 520 \| 2 failed-as-expected, 51647 \| 51645 \| 2, exit 0, x2 (default, seed 1), identical after stripping timestamped `[warning]` log lines; `verify_paintdepth.sh` 17/17 `RESULT: ALL PASS` on the app binary; unlimited: cap and absorb inert by construction (`normal_shell` false, `bounded_mode` false `:3215`), see m6 for the stat; tripwire: created 65d17c964f, sharpened once by cfe7fae1df (Aug 31, reviewed I2), untouched by the 12 commits since; residue: none in added lines (`#ifdef MM_SEGMENTATION_DEBUG*` SVG exports are upstream); exe/DLL: m8 |
| 6 | GUI-readiness | see the list at the end |

---

## Important

### I-1. The I-2 pin is existence-only: it freezes the colour-3 stripe as a contract instead of bounding its benignity

**Where.** `tests/libslic3r/test_paint_depth_clamp.cpp:4405-4411` (`CHECK(has_painted_unopened_
fragment(*object, /*Extruder3*/ 3))`), helper `:4298-4321`.

**What the pin actually asserts.** `has_painted_unopened_fragment` returns true iff SOME layer has
a colour-3 component that (a) has no core under `opening_ex(., 0.1125 mm)` and (b) lies wholly
>= 0.879 mm inside the contour. (a) and (b) are SELECTION criteria for returning true - not
assertions about every thin fragment - and the CHECK direction is "at least one exists". Nothing
asserts the properties the header itself lists as the reason the artefact is benign: that colour 3
already prints a non-thin claim on the same layer (no added toolchange), that the fragment is
narrower than one bead, that it is confined to colour 2's shell depth at the cap boundary
(layers 146-148), or that there are no OTHER thin colour-3 fragments that are NOT interior. The
brief's criterion is explicit: an existence-only pin freezes a defect as a contract, and the
fixwave-2 review's option (b) already spelled out the alternative ("pin the ring's WIDTH bound and
enclosure so a future fix is a green delta, not a red one"). The wave took the honest attribution
and the honest "ENSHRINE" decision, but kept the pin pointing the wrong way.

**Failure scenario.** The per-neighbour clip the header sketches (or any absorb that hands a
sub-bead remnant back to the colour that surrounds it) turns `:4410` red on a test whose own
header calls the geometry a defect; the path of least resistance then is to delete the test or
re-add a floor - the exact history the fixwave-2 review's I-2 was written against.

**Fix (test-only).** Replace the positive pin with a benignity pin: a helper that returns EVERY
thin colour-3 component (layer, min distance to the contour, area) without the interiority
filter, then for each: `CHECK(min_distance >= wall_stack)`, `CHECK(claim_for_layer(object,
layer, 3)` has a component surviving `opening_ex(., t))` (colour 3 already on the layer, no
toolchange added), `CHECK(layer` within `top_shell_layers` of the cap boundary`)`; report the
count via CAPTURE and bound it above only (`CHECK(count <= 3)`), so elimination is a green
delta. Keep the `pdmUnlimited` negative test (`:4413-4425`) as is; its corrected comment is
right.

---

## Minor

### m1. `layer_color_stat_variant_applies_here` checks only the FIRST parent of an interned painted region
`MultiMaterialSegmentation.cpp:1708-1715`. `painted_regions` holds one entry per (extruder,
parent volume region); a painted config resolves to ONE interned `PrintRegion*`
(`PrintApply.cpp:1093 get_create_region`), so two model parts with identical settings both
painted colour 2 yield two entries with the same `region` pointer and different `parent`s.
`std::find_if` returns the first, so on a layer where the first-listed part is absent and the
second is present the variant is judged NOT applicable. Without a modifier this is harmless (the
fallback picks the same, only, candidate); with a Z-confined gap-off modifier on the object the
fallback last-wins reinstates the pre-fix over-erosion on exactly those layers (generator and
absorb still agree, so no sliver leaks - the per-layer precision is what is lost). Fix:
`std::any_of` over all entries with `pr.region == candidate_region`, true if ANY parent has
slices on this layer.

### m2. Range lookup uses `print_z` inclusive on both ends; the slicer assigns layers by `slice_z`
`:1706` `first <= layer.print_z && layer.print_z <= second` vs `PrintObjectSlice.cpp:237-249`
`layer_range_first(layer_ranges, layer.slice_z)` (`:4226`), half-open with a "z == second goes to
the next range" rule. On a height-range-modifier object a layer whose upper half straddles a
range boundary is looked up in the range ABOVE, where the candidate is usually not listed ->
`break` -> `! candidate->slices.empty()` -> false -> fallback last-wins for that one layer.
Graceful, one layer per boundary, unexercised (every fixture has the single `[0, DBL_MAX)`
range, as the report notes). Fix: look up by `layer.slice_z` with `first <= z < second`.

### m3. `extrusion_width` still maxes over NON-applicable variants while `extrusion_spacing` is now per-applicable
`:1788` vs `:1807-1812`. `wall_stack = spacing + width` therefore mixes provenance on an object
with a line-width `PARAMETER_MODIFIER`: outside the modifier's Z it is the body's spacing plus
the modifier's (wider) width - neither the old (modifier spacing + max width) nor the correct
(body + body) answer. Sub-0.1 mm, hidden in the F1 inset, no fixture. Fix: apply the same
applicability rule to `extrusion_width` (max over applicable candidates, fallback max over all).

### m4. Per-layer, not per-volume: the check-1a limitation is still undocumented in code
fixwave-2 review m4, not in this wave's brief and not addressed. When two variants of a colour
both apply on a layer (part + modifier side by side, or two painted parts with different gap-fill
settings) the last-created variant's threshold erodes BOTH volumes' claims on that layer, and the
absorb kills at that width on both. Same choice as the parent commit, deterministic, consistent
between generator and absorb - but `compute_layer_color_stat`'s header says "resolved from ONLY
the candidate(s) that actually apply here" without saying what happens when two do. Document
there (one sentence), or resolve per island against the parent's own `slices` (now reachable).

### m5. The apex fix's r/2 premise holds only for a grid-aligned top; dome apices are disks, not half-rings
`:1575-1577`. `generate_object_layers` (`Slicing.cpp:765-795`) emits a layer iff its mid-plane is
below the object top, so the top slab `[slice_z_n, mesh_top]` is phi * h thick with phi in
(0, 1], not h/2: the fixtures (top at 3.0 = 30 x 0.1) sit at phi = 0.5, where the review's
arithmetic is exact. In general the apex ring is phi * r wide and is capped iff r > 1.5ws / phi,
against the full-ring rule r > 3ws - they agree only at phi = 0.5. At phi -> 1 the apex ring alone
is capped for 2.17° < theta < 4.35° (the old delta gave 2.17-3.25°). Consequences: hidden below
the shell, no toolchange delta, and NO isolated island (the freed apex column is contiguous with
the base crown interior; a painted crown makes one wide patch that caps whole either way).
Bottoms are always exactly half (the bed is at z = 0). Separately, a true dome's topmost patch is
a DISK of width 2 * sqrt(2 R s): the lower delta caps the apex disk of domes down to R ~ 4.3 mm at
s = 0.05 (was 7.7 mm), i.e. a hidden base well under the apex from the shell depth to D on small
domes - a pre-existing behaviour of the cap for larger domes, widened slightly here. Neither is a
regression of visible output. Fix (optional): pin 2.2° (17 um margin) and a non-grid-aligned top
(e.g. frustum height 3.04) in the cliff test; scale the apex opening delta by the actual top-slab
fraction if the asymmetry is ever to be closed for real; otherwise rewrite `:1553-1554` ("always
agrees with its neighbours'") to state the phi = 0.5 premise.

### m6. Unlimited mode is no longer "untouched by construction" on modifier objects
The per-layer resolution lives in `compute_layer_color_stat`, which also feeds the legacy
per-colour openings that run in every mode (`:2158`, `:2355`, `:2375`, `:2433`). So
`pdmUnlimited` output is byte-identical to the parent only where each painted colour has one
applicable variant per layer (every fixture; every `pdmUnlimited` test green). On a Z-confined
gap-off-modifier object unlimited mode now resolves per layer too - an improvement over upstream's
last-wins, but a change to the legacy reference mode that the earlier reviews recorded as
untouched. Document in the ledger (or gate the per-layer resolution on `bounded_mode` if strict
upstream parity in unlimited mode is a contract; this review recommends documenting).

### m7. Stale line references in the new I-2 header and the commit message; one garbled comment
`test_paint_depth_clamp.cpp` I-2 header: ":2343" (legacy gate) is at `:2366`; ":3144" (the
clip) is at `:3167` - both drifted by the 23 lines the cap-fix Minor-1 comment block added above
them after the header was written; ":1309" is right. Same refs in the commit message.
`MultiMaterialSegmentation.cpp:1553` "a 2 mm2-scale annulus of 271.8mm2" - the annulus is
2.7-2.9 mm WIDE.

### m8. Process: the committed text differs from every binary the gates ran on by one comment reflow, and the app DLL was never relinked after it
Timeline (mtimes + transcript): `all_build2` linked `Snapmaker_Orca.dll` 20:59:26 and the tests
exe; `verify*_final` 20:59:41/44; then at 21:00:41 an `Edit` on `MultiMaterialSegmentation.cpp`
whose old/new strings differ only in where the line `// No effect on a genuine free-standing
top (also reached via this same early return)` wraps; then `libslic3r_tests.vcxproj` rebuilt
(21:02:01) and NOTHING run on it; report 21:04:47; commit 21:05:26. So the app binary is
code-identical to the commit (the user's slice is safe), and this review's own runs on the 21:02
exe reproduce every number - but the report's "all gates on the FINAL committed-state binary" is
true only modulo that reflow, and the brief's "app exe newer than the last source change" fails on
raw mtimes. Rule for the next wave: no edits after the final `ALL_BUILD`; any edit, however
cosmetic, means rebuild + rerun.

---

## Check details

### 1. I-1 shared stat (`:1643-1888`, `:3754-3776`)

Types verified against production: `PrintObjectRegions::PaintedRegion{extruder_id, parent,
region}` (`Print.hpp:296-301`), `parent` indexes `LayerRangeRegions::volume_regions`
(`PrintApply.cpp:1116`), `VolumeRegion::region` may be null (guarded `:1712`),
`print_object_region_id()` is the index into `all_regions` (`PrintApply.cpp:1032`) and
`layer.regions()` is built in `all_regions` order (`PrintObjectSlice.cpp:5205-5207`), so
`layer.regions()[parent_id]->region().print_object_region_id() == parent_id`. `shared_regions()`
is non-null whenever slicing runs. Thread safety: the function reads const data only; the new
`parallel_for` writes one per-layer vector per iteration.

Formula equivalence for the kill width: `2 * unscaled(small_region_threshold)` on the gap-off arm
= `outer_wall_line_width + 0.7 * rounded_rectangle_extrusion_spacing(width, layer.height)` =
0.7500 mm at 0.45/0.1, the same expression the caller's object-wide vector used
(`:3929-3934`) except that it now takes THIS layer's height (first layer, variable layers) and
the raw `outer_wall_line_width` rather than the Flow-resolved width - identical for explicit
widths (every profile and fixture here), and now consistent with the erosion that created the
population, which is the point. An auto (0) line width would floor the kill width and zero the
generator's threshold - pre-existing upstream behaviour of the stat, unchanged.

Dropped presence gate (`segmented_regions[layer][colour].empty()`): correct to drop -
`interclaim_absorb_effective_claim_width` (`:2980-3001`) tests adjacency against `merged`
(final claims incl. descents) per island, so a colour present only via its descent now widens
the kill width where it borders an island, closing the fixwave-2 review's "theoretical" case.

Which layer's stat the erosion reads: the accumulated-band opening (`:2545-2546`) and the
`reach` opening (`:2503`) read the DESTINATION layer's stat, which is the layer the absorb reads
- consistent on the bounded path. The per-step opening (`:2355`, `:2433`) reads the ORIGIN
layer's stat, but it runs only when `! normal_shell` (legacy/unlimited), where the absorb does
not run (`:3215`), so no disagreement is reachable.

RED/GREEN: `red_run.log` (20:42:55, production file stashed at 20:40:56): 93 cases, 1 failed at
`:4770` (the new I-1 test), `red_probe.log` 8.123 / 6.002 mm2; `green_run.log` 93/1504 after the
pop; the committed 94/1568 adds the cliff test. The fixture's modifier placement (local z =
print z - radius before `ensure_on_bed`) was checked against `its_make_sphere` (centred) and
`ModelObject::ensure_on_bed` (`z_offset = -min_z`) - correct.

### 2. I-2 attribution (`:1309-1330`, `:2366`, `:3167`)

Hand-derivation on the sphere at L146 (radial, mm, from the fixwave-2 review's measurement):
colour 3's lateral claim after the Stage-1 clamp is the band `[r_slice - region_cut_width,
r_slice]` = `[3.027, 4.363]` (`:1309` parity-notched width, `:1330` `keep_core` subtracted from
every lateral claim). Colour 2's top/bottom `full` at L146 is the union of the descents of
origins 147..~160; `legacy` (`:2366`, `m < top_shell_layers = 6`) is the near-surface rings
(origins 147-151, radii 3.20-3.45); `excess = full \ legacy` is the inner disk out to 3.20. The
clip (`:3167`, `full = full \ (excess ∩ other_painted_laterals)`) removes colour 2 exactly on
band ∩ excess = `[3.027, 3.20]`, and the per-extruder trim (`diff_ex(lateral, top_and_bottom)`)
removes colour 3 wherever colour 2 keeps a claim - so colour 3 survives precisely on
`[3.027, 3.20]`: inner edge = clamp band, outer edge = legacy/excess boundary. The corrected
header (`:4361-4402`) states this mechanism; the `pdmUnlimited` comment (`:4419-4424`)
correctly says the negative case does not discriminate (clamp off AND `excess` empty there).
The "≥ 0.9 mm inside" claim: the helper enforces ≥ 0.879 mm (`wall_stack`), the review measured
1.16-1.43 mm. Pin direction: I-1 above.

### 3. Site-3 pin

`:999` `CHECK(len_on < len_off)`; fixture and reasoning unchanged from site 4 (`:958`); the
fixwave-2 review's measured values put ON 10.41 mm shorter.

### 4. Apex fix (`:1562-1578`)

`has_no_reference_layer` mirrors `exposed_surface_part`'s early return (`:1503`) token for
token. Full ring at layer j: patch `[r_{j+1}, r_j]` width r, `exposed` = patch minus
`offset(contour[j+1], +ws)` = width r - ws, survives `opening(ws)` iff r - ws > 2ws iff r > 3ws.
Apex half-ring (phi = 0.5): width r/2, no erosion, survives `opening(0.75ws)` iff r/2 > 1.5ws iff
r > 3ws - identical. Dilate-back `2ws` and `intersection_ex(patch, .)` unchanged. Above the cliff:
2.2° r = 2.603, r/2 = 1.301 < 1.318 (empty), 2.3° 1.245, 2.5° 1.145, 3° 0.954, 5° 0.572 - all
empty, so `top_flat_cap_ex` is empty and the guard at `:2352` never fires: byte-identical path.
Free-standing tops: threshold drops from 2ws to 1.5ws; on a straight column F1's `-ws` inset
leaves nothing to subtract below 2ws, on a mushroom (narrow top over a wider body) a 1.5-2ws top
is now capped at the shell like any wider flat top - consistent, hidden. RED evidence is real:
`minor1_red.log`/`minor1_red2.log` (20:55/20:56) fail `:5239`/`:5244` at 2.0° on all 8 layers,
pass at 1.5°; `pd2_*` (20:58) all green, so the mutation was reverted before `all_build2`
(20:59) - the DLL has the fix. Coverage gap: nothing committed pins 2.2-2.9° (m5).

### 5. Sweep numbers (this review's runs, snapshot exe)

`paintdepth_default/seed1/seed2.log`: `All tests passed (1568 assertions in 94 test cases)` x3,
sorted timing-stripped diff empty. `chameleon.log`: 605 / 133. `fullsuite.log`,
`fullsuite_seed1.log`: `522 | 520 passed | 2 failed as expected`, `51647 | 51645 | 2`
(`test_mixed_filament.cpp:3483`, `:4429`), exit 0; sorted diff empty after stripping
`[warning] MF_REMAP ...` lines that carry wall-clock timestamps. `verify_mine.log`: 17/17,
`RESULT: ALL PASS` (writes only the script's untracked `spike/out/*.gcode`). Residue grep over
the commit's added lines (`printf|cout|cerr|BOOST_LOG|fprintf|getenv|SVG|dump|TODO|FIXME|XXX|
HACK|#if 0|if (false`): no hits. Tripwire: `git log -L1602,1652` shows 3448111acd (create,
original form), 65d17c964f (rewritten into the anti-smear guard, Aug 31 20:54), cfe7fae1df
(positive probe sharpened, Aug 31 21:36, taper-bound review I2), nothing since.

---

## GUI-readiness: what to expect on the domed-face model versus round 3

1. Inter-claim slivers: gone - no body-coloured gap fill or single beads between adjacent
   painted features on the face, Arachne and Classic, with `gap_infill_speed` on OR off (absorb +
   kill-width tracking, now with generator and absorb reading one per-layer stat).
2. Erratic square/maze infill under painted areas: UNCHANGED by default - the new checkbox
   `paint_depth_solid_interfaces` (default ON) keeps the forced solid interfaces that cause it;
   untick it to compare (calmer infill, less solid material; trade-off: no forced solid shell
   under painted tops, so base colour may show through a thin painted top).
3. Flat-cap cost: any painted flat top/bottom at least ~2.6 mm wide beside a riser (or ~1.76 mm
   free-standing) now stops at the solid-shell depth - fewer toolchange layers and less purge per
   painted flat cap in the slicer's estimate, nothing visible changes; the dome's flank rings
   (slopes above ~2.2° at 0.1 mm layers) keep the full normal thickness.
4. Everything else visible is as in round 3 (normal-thickness shell, exterior-bleed fix, wall
   count) - this and the last three waves changed only what sits below the solid shell.

Known limitations still open (none visible on a print):
- The 24° surface-layer filter ceiling: surfaces steeper than ~24° at 0.1 mm layers get no
  top/bottom descent, only the lateral wall band; coverage on the steep part of the dome is the
  band's normal thickness, as in round 3. Judge against the slice before deciding on the #7104
  guard.
- Narrow flats: a painted ledge narrower than ~2.6 mm beside a riser (1.76 mm free-standing)
  is not capped - full depth, same cost as round 3 (cap review Minor 3). Cliff scales with layer
  height (~4.8° at 0.2 mm).
- Hidden artefacts: a 0.2 mm colour-3 stripe inside colour 2 under a two-colour cap boundary
  (I-2, enshrined - Arachne prints a hidden min-width bead, Classic a hairline void); a base
  well/annulus under a dome apex from the shell depth down (cap classifier, m5); a possible
  painted/base ring mismatch at the apex of 2.2-4.4° slopes when the model's top is not on the
  layer grid (m5). All below the shell, no toolchange delta.
