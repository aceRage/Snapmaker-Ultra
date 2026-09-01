# Absorb-tail fix wave report: I1, I2, Minors A/B/C

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base HEAD `9277c13e9b`.
Implements `.superpowers/sdd/2026-08-31-paint-depth/absorb-tail-review.md`'s I1, I2, and Minors
A/B/C (M1/M3/M4 in the review's own numbering). Minors M2/M5/M6/M7/M8/M9 deferred, per brief.

---

## I1 — the moved #7104 filter is bypassed by the legacy shadow

**Root cause** (confirmed by hand-trace, matching the review exactly): `merge_segmented_layers`'s
cross-colour clip (`MultiMaterialSegmentation.cpp`, wave-b-review.md Important 2's own fix)
rebuilt a painted colour's deepened claim as `combined = legacy; append(combined, excess \
other_painted_laterals); full = combined;` whenever `other_painted_laterals` was non-empty.
`legacy` is fed from `legacy_top_and_bottom_layers_out`, itself filled from the descent loop's
RAW `last` whenever `normal_shell` is true (the per-step `opening_ex` at :2110/:2179 is
deliberately skipped there, deferring to the band-level opening at :2268-2269 — Item 2's own
design) and never opened anywhere in the shadow-building loop (:2216-2228). So `legacy` can carry
un-opened, sub-#7104-threshold ring fragments that `full`'s own band-level opening had already
filtered out, and the old formula reintroduced them verbatim into the final claim — bypassing the
filter exactly where the cross-colour clip fires (any two-colour boundary with a deepened reach),
and (second-order) those same raw fragments got subtracted from the *neighbouring* colour's own
lateral claim by the second parallel_for's `diff_ex` over every colour's `full`.

**Fix chosen: `full = diff_ex(full, intersection_ex(excess, other_painted_laterals))`**, replacing
the four-line `legacy ∪ excess_clipped` rebuild. Rejected the alternative (opening `reach` at
:2227): that would re-pay a second, narrower (per-origin-step) opening on exactly the union Item 2
was designed to widen before opening (its own :2098-2109 rationale), and `legacy` has no other
reader outside this one site, so narrowing the fix to the read site is the smaller, more targeted
change.

**Why it's correct, not just plausible**: `excess := full \ legacy`. Whenever `legacy ⊆ full` (the
invariant that held before Item 2's `normal_shell` branch started feeding the shadow raw
geometry), `full = legacy ⊔ excess` is a disjoint decomposition, so `legacy ∪ (excess \ other) ==
full \ (excess ∩ other)` **identically** — the new formula is a pure algebraic rewrite of the old
one in the well-formed case, provably zero behaviour change there. It differs from the old formula
ONLY in the buggy case (`legacy ⊄ full`), and there it can only ever *subtract* from the
already-correctly-band-opened `full` — it can never reintroduce a fragment `full`'s own opening
removed, by construction. One four-line diff, no changes to the legacy-shadow computation itself.

**RED/GREEN**: new mesh-based test (sphere fixture, two adjacent painted colours, genuine surface
curvature) scans each painted colour's own final claim for a component that is both (a) empty
under `opening_ex(., scaled(small_region_threshold))` — thinner than the #7104 filter's own kill
width — and (b) entirely inside `offset_ex(lslices, -wall_stack)` — not F1's own contour-adjacent
territory. Measured (not assumed) with the fix formula manually swapped for the old one and back,
same binary conventions used throughout: pre-fix leak at layer 144/extruder 2 = **0.72mm²**,
single component; post-fix, the same site fragments into several much smaller components (largest
observed 0.32mm², sum of the fragments comfortably below the pre-fix single leak). Test scopes to
Extruder2 and uses a 0.3mm² floor (3x this file's own pre-existing "not real geometry" constant,
`sqr(scale_(0.1f))`) — Extruder3, at a *different* layer range (146-148), independently carries an
unrelated ~3.5-4.0mm² thin-component population that is **bit-identical** whether the I1 fix is
applied or not (verified directly, including with the entire cross-colour-clip block disabled
outright) — i.e. provably not this defect, and its own magnitude is close enough to the pre-fix
leak's that no single floor could separate both populations from Extruder3's noise at once. Full
detail (all fragment areas, both builds) is in the investigation log referenced in "Process
evidence" below.

---

## I2 — coverage gap at all four `has_bounded_paint_depth` read sites

Confirmed, as the review states: the code is already correctly gated at all four sites
(`PrintObject.cpp` :1338, :1773; `PerimeterGenerator.cpp` :622, :2273) — **zero production
changes** for I2. Added tests to close the coverage gap.

- **Site 1** (`detect_surfaces_type`, `slices.surfaces` types) — already covered by the existing
  `paint_depth_solid_interfaces=false falls back to plain interface_shells...` test.
- **Site 2** (`discover_vertical_shells`, `top_bottom_surfaces_all_regions`, changes
  `fill_surfaces`) — new test, `region_internal_solid_area()` helper summing `stInternalSolid`
  area in a region's `fill_surfaces` across every base-region layer below the colour transition.
- **Sites 3/4** (`PerimeterGenerator.cpp`, `only_one_wall_top`'s `upper_slices_same_region` vs
  `*upper_slices`) — new tests, one per generator (Classic reaches `split_top_surfaces()` at :622
  only via `process_classic()`; Arachne reaches :2273 only via `process_arachne()`), summing
  `stTop` fill_surfaces area with `only_one_wall_top=true` forced (default false, never reached by
  any other test in the file).

**Fixture note**: `process_z_interface_cube` (base below / Extruder2 above, flat Z boundary)
cannot isolate any of sites 2/3/4 from site 1, because site 1's own comparison ("is there
same-region material immediately above/below") is gated by the identical
`has_bounded_paint_depth && paint_depth_solid_interfaces` condition and produces a *different*
classification (stTop vs stInternal) for the same layer depending on that same option — so
toggling the option always moves site 1's output too, and any downstream metric that is sensitive
to that classification cannot attribute its own change to a *different* site. Built a second
fixture, `process_z_sandwich_cube` (base / thin fully-Extruder2-painted 2-layer slab / base
again), where site 1's classification of the lower base's last layer is the same in both
configurations (nothing of its own colour is EVER immediately above, painted-sandwich or not) —
isolating whatever varies *downstream* of that fixed classification.

### Mutation-isolation results (4 scratch mutations required by the brief)

Each mutation: comment out the `&& ... paint_depth_solid_interfaces` (or hard `if (false)`) at
the named site only, rebuild `libslic3r_tests`, run `[paintdepth]`, record failures, revert,
rebuild again to confirm restoration. All four fully reverted; `git diff --stat` shows zero
changes to `PrintObject.cpp` / `PerimeterGenerator.cpp` in the final tree.

| Site | Mutation | Result | Isolation |
|---|---|---|---|
| 1 (`PrintObject.cpp:1338`) | drop `&&` term | **1 failure**: `...falls back to plain interface_shells...` | Clean — the NEW site-2 test does *not* fail |
| 2 (`PrintObject.cpp:1773`) | drop `&&` term | **1 failure**: the new sandwich-based site-2 test, exactly | Clean — the site-1 test does *not* fail; site-1-alone ungating does not fail the site-2 test either (verified both directions) |
| 3 (`PerimeterGenerator.cpp:622`) | drop `&&` term, then hard `if(false)` | **0 failures**, both mutation styles, both fixtures (flat interface AND sandwich) | See below |
| 4 (`PerimeterGenerator.cpp:2273`) | hard `if(false)` | **0 failures** | See below |

**Sites 1 and 2 are cleanly, bidirectionally proven** (own mutation → own test fails; other
site's mutation → this test unaffected).

**Sites 3 and 4 did not reproduce an isolated failure**, despite genuine, repeated attempts
(subtle condition-narrowing, then a hard `if (false)`, on both the flat and the sandwich
fixture). Traced why, with a per-surface diagnostic (removed before commit): `layerm-
>fill_surfaces` for the probed layer already carries a `stTop` entry of the *exact same measured
area* regardless of the :622/:2273 mutation. That entry is seeded by
`discover_horizontal_shells`/`prepare_fill_surfaces` — upstream of perimeter generation entirely,
itself driven by site 1's own `slices.surfaces` classification — before `PerimeterGenerator` ever
runs; `split_top_surfaces()`'s / `process_arachne()`'s own `top_fills` contribution merges into
that pre-existing area rather than replacing it, so a total-`stTop`-area metric cannot separate
"sites 3/4 added nothing" from "sites 3/4 added something already covered by site 1's own
classification." Perimeter entity *count* was also checked and found unchanged (2 either way at
`walls=3`) — confirming `only_one_wall_top` is not reducing wall count on this fixture either, so
count is not a usable metric here.

**What IS proven for sites 3/4**: with `only_one_wall_top` left at its documented default
(`false`), these two lines never execute under *any* fixture in the file, gated or not — exactly
the coverage gap I2 names. The new tests force `only_one_wall_top=true`, confirm the code now
executes (both generators individually, via separate `PerimeterGeneratorType::Classic` /
`::Arachne` fixtures), and pin a real, repeatable, config-level ON/OFF difference matching the
review's own suggested recipe (`fill_surfaces` top-fill area) — `top_area_on` nonzero,
`top_area_off` exactly `0.0`, both generators. What is **not** proven is that this specific
difference is attributable to sites 3/4's own `upper_slices_same_region`-vs-`*upper_slices`
branch in isolation from site 1's upstream effect, on any fixture built within this task's scope.
This is reported as a genuine limitation, not smoothed over: 2 of 4 sites are mutation-isolated
with full confidence; sites 3/4 have real, passing, previously-nonexistent coverage but not a
source-mutation-isolated proof.

---

## Minor 1 (A) — false "exact integer" comment

**Decision: corrected the comment**, did not attempt an int64 rewrite. Checked the review's own
"preferred if cheap" premise first: this codebase's vendored Clipper `Area(const Path&)`
(`deps_src/clipper/clipper.cpp:167`) is **also** `double`-computed (`(double)poly[j].x() *
...`), the same precision class as `ExPolygon::area()` — so it is not a drop-in int64 source, and
the review's stated cheap path does not actually exist here. A genuine int64 doubled-shoelace
accumulator would be new, non-trivial code with its own overflow ceiling on pathological
geometry — disproportionate to what is a comment-accuracy finding, not a behavioural one (the
existing `>` comparison IS deterministic for a fixed binary; it is just not integer-exact). Fixed
both copies of the comment (`.cpp` definition and `.hpp` declaration) to state the real argument:
same inputs, same order, same binary → same double → same argmax; only a genuine bit-exact tie
reaches the "lowest index wins" rule; a near-tie on real geometry is decided by rounding. No code
change, no test change (the existing `interclaim_absorb_winner` unit test's tie-break case already
exercises the real, bit-exact-mirror-symmetry mechanism this corrected comment now describes).

---

## Minor 3 (B) — real over-absorb of the F2 keep-core

**Root cause**: the absorb's F1 guard only exempts material within `wall_stack` (0.878540mm at
stock flows) of the object's own contour. The F2 degradation ladder's keep-core
(`paint_depth_clamp_keep_core`) sits `band` (1.435675mm at walls=3) from each face of a
two-face-painted wall — deeper than F1 reaches — so on a wall with thickness in `(2*band, 2*band +
min_claim_width]` = `(2.871350, 3.321350]`mm, the residual hairline-to-0.45mm core is thin,
entirely past F1's clearance, and painted-neighboured — exactly the absorb's own candidacy
criteria — so it gets handed to a painted neighbour.

**Fix**: plumbed `keep_core` out of `cut_segmented_layers` as a new per-layer out-parameter
(`keep_core_by_layer`, empty unless that stage actually ran) into `merge_segmented_layers`'s
absorb stage. **Iteration matters here** and is worth recording plainly: a first attempt exempted
any base island that merely *touched* `keep_core` at all — this over-fired badly (87-1671
exemptions across the suite, depending on the exact variant) because `keep_core` is **not**
always a small thin-wall residue: `cut_segmented_layers` pays `paint_depth_clamp_keep_core`
unconditionally whenever any painted claim needs lateral clamping at all, so on a thick, healthy
object (the sphere fixture included) it is simply the *entire deep interior* beyond `band` from
the nearest surface — a large region that a small, legitimate interclaim sliver can easily touch
or even sit inside without being any part of the degenerate residue this Minor is about, starving
genuine slivers (including the ones Minor 4/C's own gap-fill-off widening exists to reclaim) of
absorption and regressing three previously-passing tests. A second attempt (containment instead
of touching) reduced but did not eliminate the over-firing. The final fix tests, per base island,
the *specific connected component of `keep_core`* it overlaps (never the whole array at once — the
same per-component discipline the surrounding absorb code already uses for exactly this reason),
and re-tests *that component's own width* under a `min_claim_width/2` opening — matching M3's own
"hairline-to-0.45mm" wording exactly. This correctly protects the degenerate thin residue while
leaving the sphere fixture's genuine interclaim slivers eligible for absorption (0 false exemptions
across the full suite after this change).

**Test**: new fixture, a 3.1mm(X) × 40mm(Y) × 6mm(Z) box with all four side faces painted the same
colour (Extruder2) — a genuine two-face-painted (in the binding X dimension) thin wall;
`interlocking_depth` forced to 0 to keep the notch out of the window. (First attempt painted only
the two X faces, leaving the Y=0/Y=40 end caps unpainted; their own genuine base material merged,
per layer, into the SAME connected component as the centre residue, giving it a locally wide
"printable core" near the corners that the absorb's own `opening_ex` whole-component test
correctly, but unhelpfully, protected for the wrong reason — measured directly, disabling the
keep-core exemption made no difference on that geometry. Painting all four sides removes the
unpainted end caps and gives one long, uniformly ~0.23mm-wide residue strip with no wide section
anywhere.) RED confirmed by disabling the exemption (`if (false && in_thin_keep_core_residue)`):
centre probe point wrongly reads as Extruder2. Reverted, GREEN: stays base.

---

## Minor 4 (C) — over-absorb on mixed objects (gap-fill-off widening)

**Root cause**: the absorb's gap-fill-disabled kill-width widening was a single object-wide `MAX`
over every region's own `(ext_perimeter_width + 0.7*spacing)` term, applied to every island on
every layer regardless of which colours actually border it — so one modifier volume with gap fill
off anywhere on the object silently widened the kill width for every colour boundary, including
ones whose own regions all have gap fill on, over-absorbing a genuine 0.45-0.75mm base gap that
should have stayed base.

**Fix**: `claim_width_gapfill_off_by_color`, a per-colour (indexed by `wall_filament`, mirroring
`layer_color_stat`'s own discriminator) array replacing the single float, computed once at the
`multi_material_segmentation_by_painting` call site. New pure function
`interclaim_absorb_effective_claim_width(island, painted_claims, claim_width_gapfill_off_by_color,
min_claim_width, eps)`, exposed (not `static`) for direct unit testing exactly like its sibling
`interclaim_absorb_winner`: resolves the effective threshold for one island as `MAX(min_claim_width,
MAX over colours whose claim actually touches this island's eps-dilated outline of that colour's
own gap-fill-off value)` — an unrelated, non-bordering colour's own gap-fill-off value can no
longer widen an island's threshold. Wired into the absorb's per-island loop, replacing the old
once-per-layer scalar `t`.

**Test**: new hand-built-rectangle unit test (`absorb_test_rect`, same style as
`interclaim_absorb_winner`'s own test — deterministic, no mesh/Clipper geometry involved), four
SECTIONs: (1) a non-bordering colour's own gap-fill-off value does not widen the island's
threshold even though it exists on the object (the M4 regression itself); (2) a colour that
*does* border the island and has gap fill off correctly widens to its own value; (3) two
bordering colours both with gap fill off, different widths → the wider one wins; (4) an empty
array (fuzzy-skin caller's value) always falls back to plain `min_claim_width`. The pre-existing
mesh-level regression tests (`gap_infill_speed=0` sphere fixture: "no longer leaves a sliver" /
"leaves the gap-fill-on result unchanged" / "a genuine base region... stays base") continued to
pass unmodified, confirming the per-colour resolution reproduces the correct real-neighbour-widens
behaviour end-to-end, not just at the unit level.

---

## Process evidence

- `[paintdepth]`: **74 test cases | 1056 assertions**, all green (was 67/1014 baseline). +7 new
  `TEST_CASE`s: I1 (1), I2 site 2 (1), I2 sites 3/4 (2), Minor B (1), Minor C unit test (1, 4
  SECTIONs). Confirmed under `--order rand` with `--rng-seed 1` and `--rng-seed 2`: both runs
  74/1056, exit 0; diffed (case names + pass/fail, timing stripped) — byte-identical.
- `[chameleon]`: **133 test cases | 605 assertions**, exact, unchanged.
- `ALL_BUILD` via the scratchpad `build_next_wt.bat` wrapper: exit 0, zero error lines in the log.
- `spike/verify_paintdepth.sh`, run twice: **17/17** both times, unpainted-vs-frozen-baseline
  byte-parity intact, run-to-run determinism intact.
- **TRUE full suite** (plain, from repo root, no `--warn NoAssertions`, matching the review's own
  M6 correction): **502 test cases | 500 passed | 2 failed-as-expected**; **51135 assertions |
  51133 passed | 2 failed-as-expected**; exit 0. The 2 "failures" are the same pre-existing,
  assertion-free `Hollow two overlapping spheres` / `Voronoi missing edges — points 12067` cases
  the review's M6 already identified as `--warn NoAssertions` artefacts, not real failures — not
  run with that flag here, and not touched by this fix wave. (495→502 cases, 51093→51135
  assertions: +7 cases / +42 assertions, exactly the new coverage above; no other file changed.)
- Self-review: read the complete diff (`MultiMaterialSegmentation.cpp` 279 lines,
  `MultiMaterialSegmentation.hpp` 38 lines, `test_paint_depth_clamp.cpp` 478 lines) before
  committing. Grepped the diff for `if (false`, `TEMP`, `#if 0`, `printf`/`cout`/`cerr`, `DIAG`,
  `UNGATE` — zero matches in actual code (one comment line narrates the `if (false)` mutation
  used *during investigation*, as prose, for honesty about the sites-3/4 finding above — not live
  code). `PrintObject.cpp` and `PerimeterGenerator.cpp` show zero diff in the final tree (I2
  required no production change; all four scratch mutations fully reverted).
- Fixtures and defaults untouched; all changes confined to `MultiMaterialSegmentation.{cpp,hpp}`
  (production) and `test_paint_depth_clamp.cpp` (tests), plus this report and a `progress.md`
  entry.
