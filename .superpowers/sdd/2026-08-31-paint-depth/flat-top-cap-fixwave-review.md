# Flat-top cap fix wave — scoped review of `b0b3db51dd`

Reviewed AS COMMITTED (`git show b0b3db51dd`), worktree `C:\Dev\SnapmakerOrcaNext`, branch
`feat/paint-depth`, against `flat-top-cap-fixwave-report.md` and the review it answers
(`flat-top-cap-review.md`: I1 ledge beside a riser, I2 striped sub-6.5° slopes, Minors 1/3/4).
Read-only: no worktree edits, no commits. The parallel review of `f3075afc50` was ignored; the
uncommitted `progress.md` edit in the working tree was noted, not reviewed.

## Verdict: SHIP — 0 Critical / 0 Important / 6 Minor

The per-component classifier does what the user decided on every geometry class that matters:
a flat ledge beside a riser is capped whole, rim included, at exactly 3 wall stacks and up
(measured cliff between 2.60 and 2.70 mm; 3·ws = 2.6356 mm); every slope from 2.2° up is
polygon-exact (symmetric difference 0.000000 mm²) to a cap-disabled reference at every layer,
gap fill on and off, so I2's stripes and the setting-dependent absorb interplay are gone; the
10/15/20° fixtures are polygon-exact across all 30 layers, apex included (Minor 1 closed); the
absorb is inert beside an active neighbour with the widened kill width. Nothing here is visible
on a print (every change sits below the effective solid shell) and nothing regresses a slope.

What this round found is real but small and hidden: in the narrow band just BELOW the new cliff
(1.63°–2.17° at 0.1 mm layers, painted slope with an unpainted crown) the full rings are capped
but the apex half-ring is not, which leaves a painted fin inside a base annulus at depths 6–14
(the "base annulus inside the claim" pattern the check asked about — 2 polygons / 2 holes,
measured). Minor, not Important: hidden, no tool-change delta, deterministic, narrow band, and
the tests never claimed that band. The other Minors are documentation precision and coverage.

## Evidence base (all reproducible; nothing asserted from reading alone)

- Working tree `git diff HEAD -- src tests` was empty when the binaries were snapshotted; by the
  end of the review it carried uncommitted fix-wave-3 edits (547 lines in
  `MultiMaterialSegmentation.cpp`, 29 in the test file), which were ignored. Every `:N` in this
  document is a line in the COMMITTED file (`git show b0b3db51dd:<path>`), and the probe TU was
  verified byte-equal to the committed test file (5100 lines) ahead of its appended probes.
  Test binary snapshotted before any run: `build/tests/libslic3r/Release/libslic3r_tests.exe` sha256 `3f4da20880bacc25…4d052c`
  (built 20:00, commit 20:05), copied to scratchpad with its DLLs; `libslic3r.lib` sha256
  `8f882c93b0f10784…` snapshotted likewise. Every suite run below used the snapshot, never the
  live exe.
- `[paintdepth]`: **92 cases / 1500 assertions** — default order, `--order rand --rng-seed 1`,
  `--rng-seed 2`, identical. `[chameleon]`: **133 / 605**. TRUE full suite (no filters):
  **520 | 518 passed | 2 failed as expected; 51579 | 51577 | 2**, exit 0, identical under seeds
  1 and 2. All match the report.
- `spike/verify_paintdepth.sh` ×2 against `build/src/Release/snapmaker-orca.exe` (sha256
  `46e6b35847d1d710…` before and after): **17/17 both**, `RESULT: ALL PASS` (unpainted
  byte-parity vs the frozen baseline, determinism, config surface).
- Debug-residue grep over every `+` line of the commit (`printf|cout|cerr|#if 0|if (false|TEMP|
  DIAG|UNGATE|TODO|FIXME|XXX|BOOST_LOG|getenv|dump|debug`): **zero matches**. Production diff:
  one 49-line function+header insertion, two one-line call-site swaps, two comment rewrites;
  nothing else in `MultiMaterialSegmentation.cpp` touched.
- **Probe build** (the load-bearing evidence for checks 1–5): the committed test file with six
  `[reviewprobe]` cases appended (`scratchpad/rev_b0b3/probe_tests.cpp`), compiled with the exact
  `cl` line from `libslic3r_tests.tlog/CL.command.1.tlog` and linked with the exact `link` line
  against the SNAPSHOT `libslic3r.lib`, into `scratchpad/rev_b0b3/probe.exe`. Cross-validation:
  that exe reproduces `[paintdepth]` **92/1500** exactly. Output: `scratchpad/rev_b0b3/
  probe_run.log` (scratchpad = `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\
  85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad`).

## One line per mandatory check

1. Classifier semantics — **PASS.** Opening at ws kills any exposed component narrower than
   2·ws; exposed = patch minus a ws band at every reference contour, so a ledge/ring beside a
   riser needs W > 3·ws = 2.6356 mm and a free-standing top needs W > 2·ws = 1.757 mm. Measured
   cliff: 2.60 mm → 15 painted layers, 2.70 mm → 6 (top) / 3 (bottom). Consequence: a 2 mm step
   beside a riser is NOT capped at all — same 15 tool-change layers as pre-fix, whole 1.12 mm
   ring inside F1 painted to depth 14 (166.5 mm²/layer vs ≈130 mm² for the pre-fix 0.88 mm band).
   No stripes, no sliver: one decision per ledge. Sane trade (Minor 3), not a re-creation of I1.
2. Dilate-back overshoot — **PASS.** The dilation is clipped to the origin's OWN patch and every
   flank ring is a separate per-layer patch (occlusion trim `:1805`), so it cannot reach a flank.
   3 mm-crown "dome" at 10/15/3°: the cap ends exactly at the top-layer patch boundary
   (half-width 1.784 / 1.687 / 2.454 = crown 1.5 + r/2), the flank's symmetric difference vs the
   cap-disabled reference is 0.000000 mm² at every layer, and the walls-only variant is exact
   everywhere. The top-layer patch DOES include the flank's topmost half-layer strip (r/2 =
   0.28/0.19/0.95 mm) when crown and flank share a colour — capped with the crown (Minor 2).
3. I2 all-or-nothing — **PASS for 3/4/5° (and 2.2/2.3/2.5°).** Symmetric difference 0 at every
   layer top-17..top, gap fill on AND off; 1 polygon / 1 hole like the reference; reach identical.
   Just BELOW the cliff (2.0°/2.15°, walls-only) the apex half-ring is not capped while the full
   rings are: 2 polygons / 2 holes, a 2.7–2.9 mm base annulus inside the claim at layers 15–22
   (Minor 1). Not setting-dependent (identical with gap fill off), hidden, 1.63°–2.17° only.
4. Slope invariance evidence — **PASS.** Reference is built in the same TEST_CASE
   (`top_shell_layers_override=15` ⇒ `top_cap_active` false, descent still max(15,15)=15);
   0.0001 mm on a 0.05 mm-step scan is equality, not slack — and probe D shows the claims are
   polygon-EXACT (symmetric difference 0.000000 mm²) at all 30 layers for 10/15/20°, stronger
   than the scan. Layers 14–26 cover the apex origin's m = 3..15 (its whole capable range).
5. Interaction with fixwave2 — **PASS.** The cap only edits `last` at m ≥ shell; the legacy
   shadow is copied at m < shell before that (`:2232`), so `reach`'s opening (`:2368`) sees
   byte-identical input; `t_keep_core = t` (`:3217`) untouched and `keep_core` derives from
   lslices only (`:1330`). Probe E (ledge cap + active Extruder3 wall, gap on/off): Extruder2 =
   0 at d ≥ 6, Extruder3 area identical at d = 6..20 (55.37/51.64 interlock parity), base is one
   island — nothing annexed. Committed absorb test now covers the widened kill width.
6. Re-run — **PASS.** 92/1500 ×3, 133/605, 17/17 ×2, 520|518|2 ×3, residue grep clean;
   unlimited is untouched by construction (`paint_depth_normal_mm = 0` at `:3871` ⇒
   `normal_shell` false ⇒ both `*_cap_active` false) and every `pdmUnlimited` test is green.

---

## Minor 1 — just below the cliff, the apex half-ring is NOT capped while every full ring is: a painted fin inside a base annulus (hidden)

**Where.** `src/libslic3r/MultiMaterialSegmentation.cpp:1545-1556` (`flat_cap_component_ex`),
via `exposed_surface_part`'s early return `:1502-1503` (no reference layer ⇒ the whole patch is
"exposed", with NO ws erosion). The topmost origin of a painted slope is a HALF-ring (its slab
is half a layer: measured half-widths crown+r/2 in probe B), so its width is r/2 while every
ring below is r. Two different thresholds fall out of one opening radius:

- full ring capped ⇔ r − ws > 2·ws ⇔ r > 3·ws = 2.636 mm ⇔ θ < 2.17°
- apex half-ring capped ⇔ r/2 > 2·ws ⇔ r > 4·ws = 3.514 mm ⇔ θ < 1.63°

**Measured (probe C, "18 over 3" frustum, walls painted, crown not; ws = 0.87854, 0.1 mm layers,
gap fill on and off identical):**

| θ | r | half-ring r/2 | rings capped? | apex capped? | claim at L15–22 (m = 7..14) | ref − cap |
|---|---|---|---|---|---|---|
| 1.50° | 3.819 | 1.910 | yes | yes | 1 poly / 1 hole | half-ring + rings (consistent) |
| 2.00° | 2.864 | 1.432 | yes | **no** | **2 polys / 2 holes** | 271.8 mm² = base annulus [10.43, 13.30] |
| 2.15° | 2.664 | 1.332 | yes | **no** | **2 polys / 2 holes** | 248.5 mm² = base annulus [10.33, 13.00] |
| 2.20° | 2.603 | 1.301 | no | no | 1 / 1, exact | 0.000000 |
| 2.3/2.5/3/4/5° | — | — | no | no | 1 / 1, exact | 0.000000 |

At 2.0° the layer-22 claim is the surface band [13.30, 28.98] PLUS an isolated painted annulus
[9, 10.43] (the apex half-ring descending to D) with a 2.86 mm base annulus between them; same
at every layer down to 15 (the apex origin's last reach), then 1 polygon again at 14. The base
annulus is 2.7–2.9 mm wide (printable core ⇒ the absorb correctly leaves it), the fin is 1.3–1.4
mm wide (≥ 2 beads, printable). Bottom mirror identical by symmetry.

**Why Minor.** Everything sits ≥ 6 layers under the painted solid shell; every layer of a slope
is painted anyway so no tool change is added; the pattern is deterministic (not
`gap_infill_speed`-dependent like I2 was); the band is 0.5° wide and needs a painted slope whose
crown is NOT painted (a tilted flat face is the realistic instance: its high-edge half-band
becomes a 1.4 mm × full-length painted fin 9 layers deep). It does contradict the commit's
"never partially capped" / "no base annuli" wording for that band, and no test covers any slope
below the cliff.

**Fix (one line, optional).** Make the topmost origin's verdict coincide with its neighbour
ring's: in `flat_cap_component_ex`, when `exposed_surface_part` took its early return (no
reference layer / empty reference), open at `0.75f * wall_stack_width` instead of
`wall_stack_width` — a width-r/2 half-ring then survives iff r/2 > 1.5·ws ⇔ r > 3·ws, exactly
the full-ring rule. Side effect on genuine free-standing tops: threshold drops from 2·ws to
1.5·ws, which is a no-op in practice because F1's `offset_ex(contour, -ws)` already leaves a top
narrower than 2·ws with no sub-surface claim at all. Pin with a 2.0° walls-only case (expect 1
polygon / 1 hole at layers 15–22 and reach = 6·r) and a 2.0° crown+walls case (already
consistent today: crown+half-ring patch is wide and caps whole). Alternatively document the band
as a known limitation; it is not a ship blocker either way.

## Minor 2 — when crown and flank share a colour, the flank's topmost half-layer strip is capped with the crown; the comment says the rim is not

**Where.** `:2084-2088` ("the crown is capped, the rim (rolling into the flank) is not"), report
§1, design doc Cost note ("flanks keep the full D bound").

**Measured (probe B, 3 mm-crown frustum, crown + walls painted).** The top-layer patch bbox is
±1.784 / ±1.687 / ±2.454 mm at 10/15/3° = crown 1.5 + r/2, i.e. it contains the flank's first
half-layer strip (slab semantics); that whole patch survives the opening and is capped whole.
At layers 15–23 `ref − cap` = 12.72 / 11.38 / 24.09 mm² = (2·1.784)² / (2·1.687)² / (2·2.454)²,
bbox exactly the top-layer patch; `cap − ref` = 0 everywhere; every full ring below is
polygon-exact. Walls-only: 0 difference at all layers (Minor 1 of the previous review really is
closed for a slope painted without its crown).

**Why Minor.** The strip is r/2 wide (0.28/0.19/0.95 mm), sits under 6 solid painted layers,
and is the same "rim included" rule that fixes I1. **Fix:** wording only — "the crown is capped
together with whatever shares its own layer patch (the flank's topmost half-layer strip, r/2
wide); every full ring below is a separate patch and is untouched".

## Minor 3 — narrow flat features are not capped; the threshold depends on layer height and is stated only for 0.1 mm

**Where.** `:1527-1532` header, commit message, report §1 ("~2.6 mm / ≤ ~2.2°"), design doc.

**Measured (probe A, slab 40×40×4 + centred tower, top and bottom mirrors):**

| ledge width | painted layers top | bottom | at d = 6..14 |
|---|---|---|---|
| 2.0 / 2.5 / 2.6 mm | 15 | 15 | whole ring inside F1 painted (166.5 mm²/layer at 2.0) |
| 2.7 / 3.0 / 4.0 mm | 6 | 3 | 0 |

So the cap recovers nothing on any flat ledge narrower than 3·ws beside a riser (pre-fix it
recovered nothing there either — the 0.88 mm band was painted to depth 14; post-fix the whole
ring is, +28 % hidden painted area on a 2 mm ledge), and nothing on a free-standing top narrower
than 2·ws (moot: F1 gives such a top no sub-surface claim). Two precision points for the docs:
(a) the "≥ 3 wall stacks" figure is the ledge/ring rule; free-standing tops need 2·ws; (b) the
cliff angle is atan(h / 3·ws): 2.17° at 0.1 mm layers, ≈4.8° at 0.2 mm (ws ≈ 0.80), ≈7.2° at
0.3 mm — slopes up to those angles are treated as flat and capped whole at coarser layers.
Both are consistent with the wall-stack yardstick the user accepted (no new angle constant), and
everything capped is under the solid shell regardless of angle, so this is documentation, plus
an open design choice: a layer-below "vertical drop" test could rescue narrow ledges with
vertical outer walls, at the cost of another heuristic. Not required.

## Minor 4 — the per-component verdict is exact only up to 3·ws along a component: a narrow arm attached to a capped component is capped for up to 3·ws of its length

**Where.** `:1554` (`offset_ex(opening_ex(exposed, ws), 2·ws)` then `intersection_ex(patch, …)`).

**Measured (probe F, tower 37×37 at x[0,37] y[1.5,38.5]: ledge = 3 mm +X strip + two 1.5 mm
Y-strips).** At d = 6..14 the +X strip is capped whole; each Y-strip stays painted for
x ∈ [0.879, 36.121] (0.621 × 35.24 mm = 21.9 mm², two components) — capped only for
x ∈ [36.121, 37], i.e. 3·ws − (core distance) = 0.879 mm into the arm, matching the arithmetic
to 3 decimals. The remaining arm strip is the pre-existing D-descent of a 1.5 mm ledge (F1 clips
it to 0.62 mm), not this wave's doing. Bounded, hidden, no sliver (the capped base joins the wide
base region). An ellipsoidal dome's rings, whose width crosses 3·ws around the ring, would be
capped per arc the same way. Informational; fix = mention in the header comment.

## Minor 5 — no test pins the "flat" side of the cliff

`tests/libslic3r/test_paint_depth_clamp.cpp:4989` covers 3/4/5° (above the cliff, at D) and
`:5062` covers 10/15/20°. Nothing exercises a slope that IS capped whole (≤ 2.15°) — the intended
behaviour for near-flat rings — so the cliff position (a function of ws and h) and the Minor 1
fin are both unpinned. Add 1.5° and 2.0°, walls-only and crown+walls, asserting 1 polygon / 1
hole and reach = 6·r at layers 15–22 (after Minor 1's fix) and the 2.0/2.2° flip.

## Minor 6 — wording nits

- Report §2 / commit: "15/20° had zero RED failures, confirming they were never actually
  affected" is right; add that this holds for walls-only paint — with the crown painted the
  half-ring is capped at every angle (Minor 2).
- `:1541-1544` "narrower than 2·wall_stack_width at every slope this feature targets": true above
  1.63°; below it the half-ring is capped (correctly). Rephrase around the r/2 vs r asymmetry.
- Report §1 "flat core at least 2·ws wide (a ring or ledge at least 3 wall stacks total)" is the
  precise statement; the commit message's "flat core ≥ 3 wall stacks" conflates the two.

---

## Check details

### Check 1 — classifier arithmetic, hand-executed and measured

`exposed = patch \ offset(contour[ref], +ws)`; `opening_ex(exposed, ws)` = erode ws then dilate
ws, so a component survives iff it contains a disc of radius ws ⇔ width > 2·ws; `offset_ex(·,
2·ws)` then dilates the survivor to 3·ws beyond its core; `intersection_ex(patch, ·)` clips.
Ledge of width W with the riser on one side: exposed width W − ws, survives iff W > 3·ws; core =
[2·ws, W − ws] from the riser; dilated by 3·ws ⇒ [−ws, W + 2·ws] ∩ [0, W] = the whole ledge, band
beside the riser included. Ring of run r: identical with W = r. Free-standing top: no erosion,
survives iff W > 2·ws. Miter joins (`DefaultJoinType = jtMiter`, limit 3) keep square annuli
uniform, so on the square fixtures there is no corner effect; measured cliff 2.60 → 2.70 mm
brackets 2.6356. A 2 mm step beside a riser: one uncapped decision, 15 painted layers, no
stripes (probe A, both mirrors). Bottom mirror byte-symmetric (same function, `layer_idx − 1`).

### Check 2 — dilation vs flanks, hand-executed and measured

Each origin's `last` ⊆ its own `top_ex` (eroded term `top_ex ∩ offset(slices_trimmed, m·(−ws))`,
full-width term `(top_ex ∩ slices_trimmed) ∩ offset(contour[j−m], −ws)`), and the cap subtracts
only the origin's own `top_flat_cap_ex` ⊆ `top_ex`. A flank ring at layer j−k is a different
patch (`top_raw[j−k] = slab projection \ contour[j−k+1]`, `:1805`), for which `exposed` is empty
(r < ws at 10/15°) or opened away (r < 3·ws at 3°) ⇒ the guard `! top_flat_cap_ex.empty()`
stays closed ⇒ byte-identical. Probe B: `cap − ref` = 0.0000 at every layer for all three
angles; `ref − cap` bbox = exactly the top-layer patch; flank reach from the contour identical
(the capped run's smaller reach is the scan hitting the crown boundary, not a flank change).
Where the cap ends: half-width 1.784 mm at 10° (crown 1.5 + 0.284); the slope's first full ring
begins there. Flank normal thickness preserved to the digit (0.000000 mm² symmetric difference,
probe D: all 30 layers at 10/15/20°).

### Check 3 — I2 all-or-nothing

Probe C, layers top−17..top, gap fill on and off: 2.2/2.3/2.5/3/4/5° ⇒ symmetric difference 0 at
every layer, 1 polygon / 1 hole, reach equal to the reference (28.6/21.45/17.15 mm at 3/4/5°
where the scan allows). No disjoint polygons across layers, no base annuli, absorb has nothing to
do. Just above the cliff (2.2°: r − ws = 1.725 < 1.757) is wholly at D. Just below (Minor 1).

### Check 4 — slope-invariance test quality

`:5062`: `cap_disabled` built in the same TEST_CASE with `top_shell_layers_override=15` (`:2899`);
`top_cap_active = normal_shell && descent > shell` ⇒ 15 > 15 false; descent = max(15, 15) = 15,
unchanged. `claim_reach_mm` (`:3054`) scans in 0.05 mm steps from the contour, so `WithinAbs(…,
0.0001)` is equality on a quantised quantity — a genuine tolerance for Clipper noise would have
to be < one step anyway, and probe D shows the underlying polygons are exactly equal. Span 14–26
covers the apex origin's m = 3..15 (loop bound `last_idx > 29 − 15` ⇒ deepest reach = layer 15;
m ≤ 2 cannot be capped), and every lower origin's `exposed` is empty. Adequate.

### Check 5 — fixwave2 sites

`git show b0b3db51dd -- src` touches only `:1508-1556` (new function), `:2068-2098` (comment +
call swap), `:2264-2273` (comment + call swap). `reach` opening `:2368`, `t_keep_core = t`
`:3217`, per-layer gap-fill-off resolution, and both `exposed_surface_part` legacy calls
(`:2061`, `:2265`) are untouched. Ordering inside the descent step: cap (m ≥ shell) → legacy copy
(m < shell) → full append; the two depth ranges are disjoint, so I-A's shadow input is
byte-identical. `keep_core` (`:1330`) is a function of `input_expolygons` and widths only. Probe E
(ledge cap beside a riser + slab +X wall painted Extruder3, gap on/off): d 0–5 Extruder2 =
1062.5 mm² (ledge inside F1), d ≥ 6 Extruder2 = 0, Extruder3 = 55.37/51.64 mm² alternating
(interlock parity, identical from d = 6 to 20), base = 1 island of 1544.6 mm² — the capped floor
and rim are one large base component with a printable core; nothing absorbed. The committed
`:4808` test covers the plain-box floor with both kill widths; a ledge variant of it would be
cheap insurance but is not needed for correctness.

### Check 6 — numbers

See "Evidence base". Unlimited: `paint_depth_normal_mm` is 0 for `pdmUnlimited` (`:3871`) ⇒
`normal_shell` false (`:1975`) ⇒ `top_cap_active`/`bottom_cap_active` false ⇒
`flat_cap_component_ex` never called, `last` never diffed; identical gate to the parent commit,
so unlimited output is byte-identical to `f3075afc50` by construction; `pdmUnlimited` tests
(`:450`, `:634`, `:4351`, …) green. Performance: the classifier adds four Clipper ops per painted
origin patch (computed once outside the descent loop, only when `*_cap_active`), not per step.

## Answers the task asked for verbatim

- **Check 1 (narrow ledge):** a flat feature narrower than 3·ws (2.64 mm at 0.45/0.1) beside a
  riser — e.g. a 2 mm painted step — is not capped at all: the whole ledge keeps the full D
  depth (15 painted layers, measured on both mirrors), exactly the tool-change cost it had
  pre-fix (which painted a 0.88 mm band to the same depth), with ≈28 % more hidden painted
  infill area than pre-fix. It does NOT re-create I1's defect class: one whole-ledge decision,
  no stripes, no sliver, no absorb dependence. Sane trade given the yardstick the user accepted
  (the alternative, delta = 0, would cap 2.2–6.5° slopes whole); documented as Minor 3.
- **Check 2 (dilation into flanks):** no. The dilation is clipped to the origin's own patch and
  every flank ring is a separate per-layer patch whose own classifier returns empty, so the
  flank's deposits are polygon-exact (0.000000 mm² symmetric difference at every layer, 10/15/3°
  and 10/15/20°). On a 3 mm-crown dome the cap ends at half-width crown + r/2 (1.784 mm at 10°)
  and the slope begins there; the only flank material capped is the topmost half-layer strip
  that slice_mesh_slabs puts in the crown's own patch (r/2 = 0.28 mm at 10°), Minor 2.
