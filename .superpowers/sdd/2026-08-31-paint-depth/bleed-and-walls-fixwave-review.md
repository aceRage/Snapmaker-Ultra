# Bleed-and-walls fix wave — scoped review

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, commit under review `193d2e0675`
(parent `cfe7fae1df`). Read-only; nothing edited. Reviewed against
`bleed-and-walls-fixwave-report.md`, `outward-bleed-investigation.md`, `wall-count-investigation.md`.

**Verdict: FIX FIRST.** F1, F3 and F4's arithmetic are correct and independently re-derived. F2's
degradation ladder is mathematically sound but has no lower bound, and in the *default* walls mode
every activation of it produces a painted claim narrower than one extrusion. One Critical, five
Important, seven Minor.

Gates re-run by hand in this worktree (binary `build/tests/libslic3r/Release/libslic3r_tests.exe`,
mtime 2026-09-01 06:28, newer than every source file in the commit):

| gate | result |
|---|---|
| `[paintdepth]` | **416 assertions in 36 test cases, all pass** |
| `[chameleon]` | **605 assertions in 133 test cases, all pass** |
| `spike/verify_paintdepth.sh` | **17/17 ALL PASS** (unpainted byte-parity vs frozen baseline holds on both runs) |

---

## Check results (one line each)

1. **F1 correctness — PASS.** Inset is taken at `input_expolygons[last_idx]`
   (`MultiMaterialSegmentation.cpp:1781` top, `:1831` bottom) — the layer the claim is *deposited*
   on (`:1787` / `:1837`), not the surface layer and not the layer above; sign and magnitude are
   correct and in scaled units; interior features are provably unclipped; the contour-contact
   invariant holds on every descent layer for every geometry (proof in Finding I-4's preamble
   below). See Minor M-5 for a consequence the report understates.
2. **I1 absorb removal — PASS for the class it was written for, with an overstated argument.** The
   contour-sliver class (`base_rest` thin *between the claim and the layer contour*) is provably
   gone. `base_rest` is nonetheless **not** always "an annulus at least one wall stack wide", so
   "the absorb's diff is empty by construction" is false as written — see Finding M-1 and the
   answer at the end of this document.
3. **F2 semantics — PARTIAL FAIL.** Ladder bounds `t/4 < b <= t/2` verified; termination verified
   (6-step bound, `:1198`); the `band/64` floor behaves as described. "Thick geometry is
   bit-identical" holds only because Clipper's default join is `jtMiter` (M-3). The Voronoi
   wrap-around limitation is accurately *stated* but under-scoped (M-4). The ladder has no lower
   bound on `b` — **Finding C-1**.
4. **F3 formula — PASS.** N=1/3/6 re-derived to the quoted literals; the whole Arachne count rule
   re-verified at source (`WallToolPaths.cpp:510-514`, `RedistributeBeadingStrategy.cpp:42-49`,
   `DistributedBeadingStrategy.cpp:92-98`, `PerimeterGenerator.cpp:2212/2231/2255`). The 0.212 mm
   margin at N=3 is right. "Parity-independent" is not (M-6), and the *tightest* margin in the new
   design is an unmentioned, unpinned 0.112 mm on the **upward** side (I-4).
5. **F4 clamp — PARTIAL FAIL.** Cannot be bypassed by a large user value; 3 beads hand-walked on
   both parities at 0.1 mm and 0.2 mm layers and both land in the window; the feature is reduced,
   not disabled, and the cap is in the tooltip. But the clamp is applied in **millimetres mode**
   too, where its entire justification is absent — **Finding I-3**.
6. **Test honesty — PARTIAL FAIL.** The F4 band-width assertion is *weaker* than "N beads fit on
   both parities": it is one-sided (I-4). Of the three re-cast tests, two keep real discriminating
   power; the flipped chamfered-frustum test lost the "ring >= one wall stack" half of its
   invariant (I-5).
7. **Regression sweep — PASS.** Unpainted byte-parity holds (verify script). `[chameleon]`
   133/605, exactly at baseline. F1 cannot alter an object with no painted top/bottom faces (it
   lives inside `if (! top_exposed_ex.empty())`, itself inside the `top_raw[color]` non-empty
   guard); F2 *can and does* — by design, it is the lateral clamp — and so do F3/F4 via the band
   and notch. For objects with **no paint at all**, the whole path is gated behind `is_mm_painted()`
   and the frozen-baseline byte-parity confirms it.
8. **Re-run — PASS.** All three gates reproduced above at exactly the reported counts.

---

# CRITICAL

## C-1. F2's ladder has no lower bound, so in the default walls mode *every* activation produces a painted claim narrower than one extrusion

`src/libslic3r/MultiMaterialSegmentation.cpp:1197` (`float b = 0.25f * band;`), `:1203`
(`append(core, intersection_ex(offset_ex(layer_slices, -b), fits));`).

The ladder's **first and widest** step is `b = band/4`. Every subsequent step halves it. So the
maximum lateral claim width the degradation can ever produce is `band/4`.

At the shipped default (`paint_depth_mode = walls`, `paint_depth_walls = 3`, 0.45 mm lines,
0.1 mm layers) `band = 1.4356748 mm`, so:

| ladder step | `b` (claim width per face) | local full thickness it serves | vs. `ext_perimeter_width` = 0.45 mm |
|---|---|---|---|
| 0 | 0.35892 | 1.436 – 2.871 mm | **0.80 bead** |
| 1 | 0.17946 | 0.718 – 1.436 mm | **0.40 bead** |
| 2 | 0.08973 | 0.359 – 0.718 mm | **0.20 bead** |
| 3–5 | 0.0449 … 0.0112 | below that | 0.10 – 0.02 bead |

`band/4 = 0.359 mm < ext_perimeter_width = 0.45 mm` — **the inequality is unconditional in walls
mode**, because `band/4 = (N·s + 2h(1-pi/4) + 0.25s)/4` only reaches 0.45 mm at N >= 3.9 walls.
So on any model region locally thinner than `2*band` = 2.87 mm, the painted lateral claim stops
being "the whole cross-section" (the old, printable no-op) and becomes a sub-bead skin on each
face.

**Failure scenario (concrete, and demonstrated by the commit's own test fixture).** The shipped F2
test `tests/libslic3r/test_paint_depth_clamp.cpp:1676` slices a 1.2 mm x 40 mm wall at
`paint_depth_mm = 2.0`. Hand-walking `paint_depth_clamp_keep_core`: `offset_ex(L, -2.0)` is empty,
so `thin = L`; step 0 (`2b = 1.0 > t = 0.6`) misses; step 1 gives `b = 0.25 mm`. The test's own
positive probe sits 0.1 mm from the face and passes — inside a **0.25 mm-wide painted strip**.
Downstream that strip is a separate `PrintRegion` whose perimeters are generated on the strip
alone (`Layer.cpp:184` / `:257-260`): Arachne sees `T = 0.25 - 2h(1-pi/4) = 0.207 mm`, which is
above `min_feature_size` (0.1 mm) and below `min_bead_width` (0.34 mm), so
`WideningBeadingStrategy::getOptimalBeadCount` (`WideningBeadingStrategy.cpp:57-65`) returns 1 bead
and `compute()` widens it to 0.34 mm — **a 0.34 mm bead extruded into a 0.207 mm gap, ~64 % local
over-extrusion, on both faces of every thin wall in the model, on every painted layer.** At ladder
step 2 (`b = 0.0897`, `T = 0.047 mm < min_feature_size`) the strip instead produces **no toolpath at
all**, while the base region has already been cut back by it — a 47 um unfilled band.

This is a new regression: before this commit those geometries hit the (ugly but printable) no-op
and got a single, correctly-beaded painted region. It fires in the default mode on the default
band, and it fires on exactly the thin-organic geometry F2 was written for.

**Fix.** Floor the ladder at one printable bead and fall back to the old no-op below it. The
quantity is already in hand at the call site — `ext_perimeter_width` is computed at
`MultiMaterialSegmentation.cpp:2650` next to `max_width`; plumb its max across regions through
`cut_segmented_layers` (which today only receives `cut_width`) into
`paint_depth_clamp_keep_core` as `min_claim_width`, and in the loop at `:1198` add
`&& b >= min_claim_width` to the continuation condition (or `if (b < min_claim_width) break;`).
Parts that cannot carry a printable painted skin then keep the whole cross-section, as they did
before, instead of getting an unprintable one. This costs F2 nothing on the geometry where it
actually helps (mm mode at 4–6 mm: `band/4` = 1.0–1.5 mm, comfortably above one bead for steps 0–1)
and removes the entire sub-bead regime.

---

# IMPORTANT

## I-1. `keep_core` is computed on every layer, including layers with no painted claim at all

`MultiMaterialSegmentation.cpp:1235-1245`. The hoist out of the per-extruder loop is correct and
the report's rationale for it is right, but it moved the work *above* the
`if (! ex_polygons.empty())` guard at `:1243`. Previously a layer whose every extruder claim was
empty did zero offsets; now it pays one full-layer `offset_ex` plus — if any part of that layer is
thinner than `2*band` — up to six ladder steps, each of which is a whole-layer `opening_ex`
(two offsets) plus an intersection, a diff and a union at `:1200-1205`. All of that is discarded.

**Failure scenario.** A tall object with paint on a small area: every one of its several hundred
layers now runs the clamp machinery for nothing. Worse in mm mode at the user's reported 4–6 mm,
where `2*band` = 8–12 mm means essentially every layer of an organic model reports a non-empty
`thin` and enters the ladder. The report's cost note ("+1 dilation +1 diff per painted layer in the
common case") understates this: the ladder's work is proportional to the whole layer's polygon
complexity, not to the thin parts, and its trigger is "any thin part anywhere on the layer".

**Fix.** Guard the hoist:
`if (std::all_of(segmented_regions[layer_idx].begin(), segmented_regions[layer_idx].end(), [](const ExPolygons &e){ return e.empty(); })) continue;`
immediately before `:1240`.

## I-2. The ladder's step is chosen from the *notched* band, so `b` can jump 2x between adjacent layers

`MultiMaterialSegmentation.cpp:1233` picks `region_cut_width = cut_width - interlocking_depth` on
even layers, and that value is what `:1240` hands to `paint_depth_clamp_keep_core` as `band` — so
the ladder's membership thresholds (`2b = band/2, band/4, ...`, `:1200`) are 0.1 mm lower on even
layers than on odd ones.

**Failure scenario.** A part whose local half-thickness `t` sits just above `band_odd/2` but just
below `band_even/2` selects step 0 (`b = 0.359 mm`) on odd layers and step 1 (`b = 0.179 mm`) on
even layers — the painted skin **halves and doubles on alternating layers**, a 0.18 mm swing, i.e.
a smaller cousin of exactly the 3/2/3/2 alternation F4 exists to remove. It is invisible to the
tests because the F2 test's 1.2 mm fixture lands mid-step on both parities.

**Fix.** Compute the ladder against the un-notched `cut_width` and apply the notch only to the
final diff, or (simpler and consistent with F4's own reasoning) note that the notch is bounded by
`0.25*perimeter_spacing` and snap the ladder's thresholds to the un-notched band by passing
`cut_width` alongside `region_cut_width`.

## I-3. F4's clamp is applied in millimetres mode, where its justification does not exist

`MultiMaterialSegmentation.cpp:2671-2673` runs `paint_depth_interlocking_depth_mm()` whenever
`paint_depth_mode != pdmUnlimited` — i.e. in `pdmMillimeters` as well as `pdmWalls`.

The entire derivation behind the cap (`PaintDepth.hpp:86-110`, and `wall-count-investigation.md`
section 3) is that the notch must fit inside the **count-window margin that `band(N)` builds in**,
so that Arachne still delivers N loops on both parities. In millimetres mode there is no N: the
band is the user's literal `paint_depth_mm` (`PaintDepth.cpp:14-15`), it is not sized to a bead
count, and no wall-count contract is being protected. The tooltip added at `PrintConfig.cpp:3971-3977`
even says so in as many words — it justifies the cap by `"Paint depth walls"` — while the code
applies it regardless of mode.

**Failure scenario.** A user in millimetres mode at `paint_depth_mm = 4` sets the interlocking
depth to 0.5 mm for a genuine mechanical key between colours and silently gets 0.107 mm — 4.7x
less than asked, for a reason that does not apply to them, on a 4 mm band where a 0.5 mm notch
costs nothing anyone can count.

**Fix.** Gate the clamp on the mode: pass the cap only in `pdmWalls`
(`paint_depth_mode == pdmWalls ? paint_depth_interlocking_depth_mm(cfg, min_spacing) : float(cfg)`),
or plumb the mode into the helper. If the cap is deliberately wanted in both modes, the tooltip and
`PaintDepth.hpp`'s header need to say why, because as written they document a walls-mode-only
rationale for an all-modes behaviour.

## I-4. The F3/F4 assertions are one-sided, and the design's tightest margin is on the side they do not test

`tests/libslic3r/test_paint_depth.cpp:138-141` asserts
`band - interlock >= n_bead_optimum - 1e-6`, and `test_paint_depth_clamp.cpp:1755-1765` probes at
`n_bead_optimum - 0.02` mm and requires it to be **claimed** on both parities. Both are lower
bounds only. Neither pins that the band is not *too wide*.

"N beads fit on both parities" is a two-sided condition: with `x = T - 2*s_ext`, Arachne gives
exactly N beads iff `x` is in `[(N-3+thr(N-3))*s, (N-2+thr(N-2))*s)`
(`RedistributeBeadingStrategy.cpp:42-49` + `DistributedBeadingStrategy.cpp:92-98`, thresholds at
`WallToolPaths.cpp:510-511`). The new formula puts `x = (N - 1.75)*s`, giving

| | downward margin | **upward margin** |
|---|---|---|
| odd N (N=3) | 0.4944*s = **0.2119 mm** | 0.2611*s = **0.1119 mm** |
| even N (N=4) | 0.7389*s = 0.3167 mm | 0.5056*s = 0.2167 mm |

The smallest number anywhere in the F3 design is therefore the **0.1119 mm upward margin at odd
N** — smaller than the 0.212 mm the report celebrates, and the direction in which nothing at all is
asserted. A future change that widened the band by 0.12 mm (a different flow model, a
`min_bead_width` profile change, an extra safety term) would silently turn "3 walls" into 4 and
every one of these tests would still pass.

**Fix.** Make the arithmetic test two-sided — one line, fully discriminating:
`CHECK_THAT(double(band) - n_bead_optimum, WithinAbs(0.25 * double(s), 1e-6));`
and add the matching negative geometric probe (unclaimed at `band + 0.02` mm from the face) to the
`F3+F4` case.

## I-5. The re-cast frustum test dropped the half of its invariant that F1 replaced I1 to guarantee

`tests/libslic3r/test_paint_depth_clamp.cpp:1508-1521`. The flipped test now asserts base material
0.1 mm inside the contour and painted material 1.5 mm inside it — i.e. it pins the base ring's
width only to the open interval **(0.1 mm, 1.5 mm)**. The property the whole I1-removal argument
rests on is that the ring is **>= one wall stack (0.87854 mm)** on tapered geometry; a 0.3 mm ring
— a textbook sub-wall-stack sliver, the exact class the review that demanded I1 cited — passes this
test unchanged.

The F1 flat-cap test (`:1620-1637`) does pin the magnitude tightly (`CHECK_FALSE` at 0.2 and 0.8,
`CHECK` at 1.0), but only on a **prism**, where `input_expolygons[last_idx]` equals
`input_expolygons[layer_idx]` and the tapered case is not exercised at all.

**Fix.** Add `CHECK_FALSE(any_contains(..., layer_edge_probe(*out_object, layer_idx, 0.8)));` to the
frustum loop at `:1511`. Post-F1 the claim edge on that fixture sits at exactly 0.87854 mm from
each layer's contour, so 0.8 is a valid negative probe with 0.078 mm of clearance.

## I-6. Nothing tests the property that made Option A beat Option B

The investigation rejected Option B specifically because it measured clearance at `layer_idx`,
which is wrong for objects that **narrow away from the painted face** (undercut, waist, overhang
below a painted top) — section 4, "Option B has a real correctness hole". F1's central design
choice is `input_expolygons[last_idx]` at `:1781` / `:1831`.

Every fixture in the suite is either a prism (`slice_capped_slab`, `slice_capped_prism`,
`slice_painted_box`) or `make_square_frustum(40., 22., 6.)` — 40 mm at the bottom tapering to 22 mm
at the top, i.e. **widening downward**, painted on its top cap. On widening geometry an inset taken
at `layer_idx` is *more* conservative than one taken at `last_idx`, so the frustum test passes under
both. Nothing in the suite distinguishes them, in either direction: the top loop needs a
downward-narrowing fixture, and the bottom loop needs the existing frustum painted on its **bottom**
cap (going up from z=0 the cross-section narrows 40 -> 22, which is the mirror-image discriminator
and costs no new mesh).

**Failure scenario.** Someone "simplifies" `input_expolygons[last_idx]` to `input_expolygons[layer_idx]`
— they read the same, and the diff would look like a cleanup — and the exterior bleed returns on
every undercut, with a green suite.

**Fix.** Add a bottom-direction case reusing `make_square_frustum(40., 22., 6.)` with
`BOTTOM_CAP_FACE`: probe 0.1 mm inside each ascending layer's own (shrinking) contour and assert it
is base. That probe fails if the inset is taken at `layer_idx`.

---

# MINOR

**M-1. The I1-removal argument is stated more strongly than it is true.** Report section 1a and the
in-code comment at `:1768-1772` claim `base_rest` "is an annulus at least one wall stack wide **by
construction** — the absorb's diff is empty". `base_rest = L \ last` is bounded below by the
annulus, but it is not *only* the annulus: wherever the claim approaches itself (two painted patches
close together on one face, a narrow notch in a patch), `base_rest` has a thin part and the absorb's
diff would **not** have been empty. The removal is therefore a real behaviour change, not the no-op
the argument asserts. The change is benign (see the answer below) but the report should say
"cannot re-create a thin base ring *at the contour*" rather than "is empty by construction".

**M-2. F1 collapses the entire sub-surface claim, not just "the exterior ring", on cross-sections
thinner than `2*wall_stack`.** At `:1780` and `:1820-1822`, if
`offset_ex(input_expolygons[last_idx], -wall_stack)` is empty then so is the legacy term (it is
eroded by `k*wall_stack`, `k >= 1`), `last` is empty and the descent `break`s at `:1786`. Any
feature locally thinner than `2 * 0.87854 = 1.757 mm` therefore gets paint on the **painted surface
layer only** — reverting to upstream, but that is more than the report's stated trade-off ("loses
its painted exterior side-wall ring below the top layer"). On an organic model with thin ears/fins
this is user-visible. Correct per Option A and identical to upstream, but it belongs in the report's
concern list and in the GUI validation script.

**M-3. "Thick geometry is bit-for-bit untouched" is true only because the default Clipper join is
`jtMiter`.** `ClipperUtils.hpp:19` (`DefaultJoinType = jtMiter`) and `:27` (`DefaultMiterLimit = 3`)
are what make `diff_ex(L, offset_ex(offset_ex(L,-band), band))` at `:1190-1194` come back empty for
a thick polygon — a round-join opening would round every convex corner and leave four non-empty
corner slivers on a plain square, running the ladder on thick geometry. Miter limit 3 squares off
corners sharper than ~38.9 degrees (`1 - cos a < 2/3^2`), so on those corners `thin` is non-empty
even where the part is thick, and — the other way round — Clipper's mitered opening *under*-reports
the true thin region at an acute tip (0.86*band from the tip of a 30-degree wedge, against a true
2.86*band), so F2 under-treats sharp tips. Both effects are small and local; the claim just needs the
qualifier, and a comment at `:1194` noting the dependency on the join type would stop a future
"let's pass jtRound for accuracy" from quietly changing thick-geometry output.

**M-4. The wrap-around limitation is accurately stated but not scoped.** `:1182-1187` and report
section 2 say what is not fixed but never name the geometry. It is: any convex feature where the
painted surface's Voronoi cell is nearer to the far face than any unpainted boundary is — rounded
fin and blade tips, the free end of a thin wall, an edge with only one of its two faces painted,
and any cross-section handed wholly to one colour. Post-F2 that wrap is no longer the whole far
half but a `b`-wide skin on the far face, so the symptom changes character (a hairline of wrong
colour rather than a wrapped block); worth saying, because a user who validates against the
pre-fix GUI screenshot will not recognise it.

**M-5. The `band/64` floor is a half-thickness, not a thickness.** `:1178-1180` says "any part
thinner than band/64 ... 0.022mm at a 1.44mm walls-mode band". The last membership test is
`opening_ex(layer_slices, 2b)` with `2b = band/64`, which keeps parts of *half*-thickness >= band/64,
so the no-op floor is at 0.022 mm of half-thickness, i.e. 0.045 mm of material. The quoted numbers
are right for what they measure; the word "thinner" is off by a factor of two.

**M-6. "Parity-independent" is wrong for the downward margin.** Commit message, report section 3 and
`PaintDepth.hpp:74` all say the margin becomes parity-independent. Per the table in I-4 it is
0.2119 mm at odd N and 0.3167 mm at even N — still parity-dependent, because the two count windows
have different widths. What *is* parity-independent is `x`'s offset from the window **centre**
(+0.1167*s for both parities, the windows sharing a centre at `(N - 1.8667)*s`). Also, "re-centres T
in the window" overshoots: the old band sat 0.1833*s below centre, the new one sits 0.1167*s above
it. The engineering conclusion is unchanged; the wording should be corrected before it is quoted
again.

**M-7. Two stale comments.** `MultiMaterialSegmentation.cpp:2660` still reads
"mmu_segmented_region_interlocking_depth's default is now 0.3 (Task 1)" ten lines above the code
that lowered it to 0.1. And `paint_depth_interlocking_depth_mm` (`PaintDepth.cpp:35-36`) returns the
configured value verbatim when `perimeter_spacing <= 0` — a documented degenerate path, but it is
the one way a large configured notch reaches `cut_segmented_layers` unclamped, and it is worth an
explicit "unreachable for a real flow" note rather than leaving the bypass silent.

---

## The check-2 answer: is the sliver class provably gone?

**Yes for the class I1 was written for; no for a residual class that I1 never targeted and that
predates it.**

The proof. Both terms appended to `last` are subsets of `offset_ex(input_expolygons[last_idx], -wall_stack)`:
the F1 term by direct intersection (`:1780-1781`), and the legacy term because
`layer_slices_trimmed ⊆ input_expolygons[last_idx]` (assigned at `:1719` before use) and erosion is
monotone with `|offset| = k*wall_stack >= wall_stack` for every `k >= 1`. `opening_ex` at `:1784`
only shrinks. Therefore, on every descent layer and for every geometry — chamfer, fillet, draft,
organic taper, downward-narrowing undercut alike — `last ⊆ erode(L[last_idx], wall_stack)`, and so
`base_rest = L \ last ⊇ L \ erode(L, wall_stack)`, an annulus of width exactly `wall_stack`.

`base_rest` can only be thin where two of its own boundaries come within `wall_stack` of each other,
and there are exactly three ways: contour-to-contour (the layer itself is thin — no colour boundary
is involved, the material is wholly base, no sliver); **contour-to-claim (the I1 class — excluded by
the invariant above)**; and claim-to-claim. Only the third survives, and it is a base strip between
two painted lobes, in the interior, never splitting an exterior perimeter loop. It arises from the
user's own painting (two patches painted close together), it exists identically and unmitigated on
the **surface** layer, which is appended with zero margin at `:1703` in upstream and in every
revision of this branch, and I1 only ever caught it as a side effect. Removing I1 therefore cannot
re-create the chamfer/fillet ring the review that demanded it cited (0.15–0.55 mm at 45 degrees),
and I confirmed that case explicitly: on the frustum, `base_rest` at descent depth k is a ring of
width `max(wall_stack, k * 0.15 mm)` — never below one wall stack.

The report's phrasing ("the absorb's diff is empty by construction") should be narrowed to the
contour case (M-1), and the frustum test should pin the 0.87854 mm ring width so the guarantee is
enforced rather than argued (I-5).

---

Report path: `.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md`
