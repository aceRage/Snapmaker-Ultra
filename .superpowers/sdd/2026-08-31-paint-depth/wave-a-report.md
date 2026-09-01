# Wave A — correctness + classic-generator fixes

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, parent `193d2e0675`.
Scope: `bleed-and-walls-fixwave-review.md` (C-1, I-1..I-6) + `classic-generator-investigation.md`
(the classic floor, the gap-fill filament leak, the test-coverage gap). Wave B (Option N,
normal-thickness shell) is deliberately untouched — see §7 for what this wave did to keep it easy.

---

## 1. What changed, item by item

### C-1 (Critical) — the degradation ladder is floored at one printable extrusion

`MultiMaterialSegmentation.cpp` `paint_depth_clamp_keep_core()`.

F2's ladder starts at `b = band/4` and halves. With no lower bound the **widest** claim it could
ever produce was a quarter of the band, and in walls mode that is unconditionally under one
external extrusion: `band/4 = (N·s + 2h(1−π/4) + 0.25s)/4` first reaches 0.45 mm at N ≥ 3.9 walls.
At the shipped default (walls = 3, 0.45 mm lines, 0.1 mm layers, band 1.4357) every activation
produced ≤ 0.359 mm — 0.80 bead at step 0, 0.40 at step 1, 0.20 at step 2.

Downstream that strip is its own `PrintRegion` whose perimeters are generated on the strip alone
(`Layer.cpp:184`, `:257-260`). At `b = 0.25` Arachne sees `T = 0.207 mm`, above `min_feature_size`
and below `min_bead_width`, so `WideningBeadingStrategy` widens it to a 0.34 mm bead in a 0.207 mm
gap — **~64 % local over-extrusion on both faces of every thin wall, on every painted layer**. One
step down (`T = 0.047 mm`) the strip yields **no toolpath at all** while the base region has
already been cut back by it.

Fix as the review specifies: `ext_perimeter_width` (already computed at the band call site) is
plumbed through `segmentation_by_painting` → `cut_segmented_layers` → the helper as
`min_claim_width`, and the ladder's continuation condition gains `b >= min_claim_width`. Below it
the loop stops and the pre-F2 no-op stands. **Max across regions**, not min: segmentation cannot
tell which region a strip will land in, so a strip printable in the narrowest region but not the
widest must still be refused.

Fuzzy skin passes its own `max_external_perimeter_width` (which is literally its clamp width), so
its ladder never runs and thin fuzzy geometry reverts to upstream behaviour.

### I-1 — `keep_core` moved below the empty-claim guard

`cut_segmented_layers`. F2's hoist out of the per-extruder loop was right but landed *above* the
"claim is empty" guard, so a layer with no painted claim paid a whole-layer `offset_ex` plus, where
any part of the layer is thinner than `2·band`, up to six ladder steps (each a whole-layer opening
= two offsets, plus an intersection, a diff and a union) and discarded all of it. Now an
`std::all_of(... empty())` check `continue`s first. Worst on exactly the shapes the feature
targets: a tall object with a small painted area, and millimetres mode at 4–6 mm where `2·band` is
8–12 mm and essentially every layer of an organic model enters the ladder.

### I-2 — ladder thresholds taken from the un-notched band

The helper now takes both `band` (notched `region_cut_width`, used for the full-band core erosion,
so the interlocking tooth is still carved) and `ladder_band` (un-notched `cut_width`, used for the
ladder's step and membership radii). Previously a part whose local half-thickness sat between
`band_even/2` and `band_odd/2` selected step 0 on one parity and step 1 on the other — the painted
skin **halving and doubling on alternating layers**, a smaller cousin of the 3/2/3/2 wall
alternation F4 removed. Choosing the step from the un-notched band makes `b` parity-independent by
construction.

Consequence, deliberate and documented in-code: the notch is not applied to degraded (thin-geometry)
claims at all. On geometry too thin to carry the full band a mechanical interlocking tooth is not
the priority, and an alternating skin is strictly worse than none.

### I-3 — the notch cap is walls-mode only  *(the semantics decision)*

`paint_depth_interlocking_depth_mm(PaintDepthMode mode, double configured_depth, float perimeter_spacing)`.

**Decision: clamp in walls mode only.** Four reasons, in order of weight:

1. **The cap's entire derivation is the walls-mode count window.** It exists so the notch fits
   inside the `0.25·spacing` margin `band(N)` builds in, so Arachne still delivers N loops on both
   parities. In `pdmMillimeters` there is no N — the band is the user's literal `paint_depth_mm`
   and is not sized to a bead count, so there is no contract to protect.
2. **It contradicted millimetres mode's own documented contract**, which `paint_depth_band_mm`'s
   header states as "`mm` verbatim … wall count and flow widths are ignored entirely, an explicit
   user-chosen depth". A flow-derived cap is precisely a flow width being consulted.
3. **The tooltip already says walls-mode.** `PrintConfig.cpp` justifies the cap by "Paint depth
   walls"; the code applied it regardless of mode. Of the review's two options — gate it, or keep
   it and write an honest tooltip — gating is the one where the shipped documentation is already
   correct and no user is silently overruled.
4. **Nothing changes by default.** The shipped notch (0.1 mm) is below the cap at any realistic
   flow. This only restores agency to a user who deliberately raised the notch in millimetres mode
   — the concrete failure case being a genuine 0.5 mm mechanical key silently cut to 0.107 mm.

Implemented as a mode parameter on the helper rather than a branch at the call site, so the
contract lives in one place and is unit-testable in both modes. `pdmUnlimited` never reaches the
helper (the call site gates on it) but is handled identically for totality.

### I-4 — the F3/F4 assertions are now two-sided

`test_paint_depth.cpp`, the F3+F4 invariant test. The design's tightest number is a **0.1119 mm
upward margin at odd N** (`x = (N−1.75)·s` against a window upper edge at `(N−2+thr(N−2))·s`), and
nothing bounded that side. Added, for every `walls` × configured-notch combination:

* `CHECK_THAT(band − n_bead_optimum, WithinAbs(0.25·s, 1e-5))` — the count-window margin is
  *exactly* one quarter spacing, not "at least".
* `CHECK(band − interlock <= n_bead_optimum + 0.25·s + 1e-6)` — the notch is subtractive only.

Plus the matching **geometric** upper bound in `test_paint_depth_clamp.cpp`'s F3+F4 case: a probe
at `band_spec + 0.05 mm` must be unclaimed on both parities, where `band_spec` is computed from the
**spec** (`n_bead_optimum + 0.25·s`), deliberately not from `paint_depth_band_mm`'s own output, so
it stays discriminating if the production formula drifts.

### I-5 — the flipped frustum test's magnitude probe is tightened

`test_paint_depth_clamp.cpp`, the chamfered/tapered painted-top (F1) test. It pinned the base ring
only to the open interval (0.1, 1.5) mm, so a 0.3 mm sub-wall-stack sliver — the exact class the
review that demanded the I1 absorb cited — passed unchanged. Post-F1 the claim edge sits at exactly
`extrusion_width + extrusion_spacing = 0.87854 mm` from each descent layer's own contour on this
fixture, so a `CHECK_FALSE` at 0.8 mm is valid with 0.078 mm of clearance. The tight probe
previously existed only on a **prism**, where `input_expolygons[last_idx] == input_expolygons[layer_idx]`
and the taper is not exercised at all.

### I-6 — downward-narrowing fixtures, both directions

New `TEST_CASE` with two sections, using a new `slice_painted_frustum()` helper:

* **top**: `make_square_frustum(22, 40, 6)` painted on its top cap — descending, the cross-section
  shrinks 40 → 22.
* **bottom**: `make_square_frustum(40, 22, 6)` painted on its **bottom** cap — ascending from the
  cap, the cross-section shrinks 40 → 22. (No new mesh; the existing F1 fixture flipped, as the
  review suggested. It also gives the bottom descent loop its own copy of the property.)

With `H_k` the contour half-width at descent depth `k`: the correct inset gives a claim edge at
`H_k − 0.87854` (always 0.87854 mm in), the `layer_idx` variant gives
`min(H_k, H_0 − 0.87854)` — only `0.87854 − 0.15k` in. A probe 0.3 mm inside the layer's own
contour is base under the correct inset at every depth and painted under the wrong one from depth 4
on. Margins: 0.17 mm on the RED side, 0.58 mm on the green side.

Worth recording: the RED run showed the pre-existing "interior painted top feature keeps its FULL
footprint" test *also* fails under a `layer_idx` inset (its positive probe fails, because
`layer_idx` there is the small boss's own cross-section). So the suite was not completely blind to
the swap — but only in the *shrinking* direction. Nothing caught the exterior **over-claim** on
downward-narrowing geometry, which is the bleed the choice exists to prevent, and that is what the
new fixtures pin.

### Item 8 — classic-generator band floor

New pure helper `paint_depth_band_classic_floor_mm(band, ext_w, ext_s)` in `PaintDepth.{hpp,cpp}`,
applied **per region** at the band call site and **only** when
`print_object.config().wall_generator == PerimeterGeneratorType::Classic`:

    band = max(band, ext_perimeter_width + ext_perimeter_spacing)     // = one wall_stack = 0.878540

`process_classic`'s onion offset on a strip always returns *both* boundaries, so a painted band can
only hold an even number of external-width loops (plus at most one gap-fill line), never one. The
floor closes three real defects, all at `paint_depth_walls = 1` (band 0.578595, the only band under
the floor at stock flows):

1. the two depth-0 external loops sit 0.128595 mm apart centre-to-centre while each is 0.45 mm wide
   — **+48 % over-extrusion** along the whole painted boundary, knowingly uncompensated
   (`PerimeterGenerator.cpp:1419`'s own FIXME);
2. `band(1) = 1.25·s + 2h(1−π/4)` is independent of `ext_perimeter_width` while classic's survival
   threshold *is* `ext_perimeter_width`, so a wide-outer-wall profile (outer 0.6 / inner 0.42 at
   0.1 mm ⇒ band(1) = 0.541 < 0.6) loses N = 1 paint entirely — zero extrusions on every layer;
3. worst — F1 insets the top/bottom claim by one `wall_stack` and that claim is **unioned with**,
   not clamped by, the lateral band, so wherever `band < wall_stack` the base region holds a closed
   ring of width `wall_stack − band` (0.299945 mm) on every sub-surface shell layer, and classic
   prints that ring as **nothing** (`offset_ex` empty at i = 0, `last` cleared, gaps only collected
   from i ≥ 1): a genuine void ring under every painted cap.

Not unconditional: Arachne's 1 → 2 bead boundary is `T > (1+split_thr)·ext_s = 0.647570`; today's
band(1) gives `T = 0.535675` → 1 bead, a floored band(1) gives `T = 0.835620` → **2 beads**,
breaking the "1 wall means 1 loop" contract F3 established. The Arachne half of the new test is
that pin. A non-positive band (unlimited mode, `paint_depth_mm = 0`) is returned untouched —
flooring it would silently switch the clamp back on.

### Item 9 — the gap-fill filament leak  *(the anchor)*

**Anchor: a new shared role → filament rule, `fill_filament_source(const PrintRegionConfig&, ExtrusionRole)`,
declared in `Print.hpp`, defined in `PrintRegion.cpp`, and used by both of
`GCode::process_layer`'s extruder-id lambdas (`configured_filament_id_1based`, `configured_extruder_id`),
which each carried their own copy of the rule.**

The bug was a **bucket-vs-role** confusion, not a paint bug. Gap fill is produced by the perimeter
generator as part of the wall stack (`process_classic` → `gap_fill` → `LayerRegion::thin_fills`) and
is only *stored* in the fills collection, which `Fill::make_fill()` copies it into. Deciding its
filament from that bucket sent it to `sparse_infill_filament`; a painted `PrintRegion` sets
`wall_filament` and `solid_infill_filament` to the painted extruder unconditionally but
`sparse_infill_filament` only when `paint_sparse_infill` is on (`PrintApply.cpp:1088-1099`). So with
"Paint sparse infill" unchecked, ~37 % of an N = 3 painted band — the middle gap-fill line, sitting
directly behind the single painted outer loop — printed in the **base** filament, while that
option's tooltip promises "walls and solid infill still print in the painted filament".
`process_arachne` never calls `gap_fill`, which is why it was classic-only and invisible to every
Arachne slice this feature had been validated against.

The role already says gap fill is not infill (`is_infill(erGapFill)` is false), and
`LayerTools::extruder` (`ToolOrdering.cpp:323-334`) has always routed a gap-fill-only collection to
`wall_filament` on exactly that basis — so tool ordering and G-code emission **disagreed** about
which extruder the painted band's gap fill needs. They now agree, which also settles the secondary
finding in the investigation. Only `erGapFill` is redirected, deliberately: `erNone` (an empty
collection) and any future role keep the old fall-through.

Why this anchor and not the region config: `sparse_infill_filament` on the painted region *must*
stay base — that is the option. The distinction between gap fill and sparse infill only exists at
the extrusion-role level, and this is the one place both emitters consult.

### Item 10 — classic-generator test coverage

See §3 for the per-generator statement.

---

## 2. RED evidence

Three builds. Logs in the session scratchpad
(`wavea_red1_run.log`, `wavea_red2_run.log`, `wavea_green_*.log`).

### RED-1 — tests written, production = HEAD + the behaviour-preserving `fill_filament_source` extraction

`42 test cases, 557 assertions | 5 cases / 7 assertions failed`

| item | failing assertion | reading |
|---|---|---|
| **C-1** | `CHECK(any_contains(claim, centre))` ×2 (both parities) | the 1.2 mm fin's centre was base — i.e. the shipped code really did cut two 0.25 mm sub-extrusion strips |
| **I-3** | `CHECK_FALSE(any_contains(claim_even, probe(5.7)))` | mm-mode even band was 5.898 (notch capped to 0.1018), not 5.6 |
| **item 8** | `CHECK(any_contains(claim, probe(0.65)))` ×2 | unfloored classic band(1) = 0.595 / 0.493 mm |
| **item 9** | `CHECK(int(fill_filament_source(cfg, erGapFill)) == int(Wall))` → `2 == 0` | gap fill resolved to SparseInfill, i.e. the base filament |

### RED-2 — I-3 applied + three deliberate mutations (all reverted before commit)

Mutations: F1's inset → `offset_ex(input_expolygons[layer_idx], −0.5·wall_stack)` at both descent
sites; `paint_depth_band_mm`'s margin `0.25` → `0.5` **with the test file's `expected_band` spec
mirror updated to match** (simulating the realistic "someone changes the flow model and updates the
obvious mirror"). `42 cases, 561 assertions | 12 cases / 48 assertions failed`:

| item | failing assertion | count |
|---|---|---|
| **I-4** upper bound A | `band − n_bead_optimum == 0.25·s` | 18 (all walls × notch combos) |
| **I-4** upper bound B | `band − interlock <= opt + 0.25·s` | 6 (walls 1–6 at the 0.1 notch, exactly as predicted) |
| **I-4** geometric | F3+F4 `over_probe` unclaimed | 1 (odd parity, as predicted) |
| **I-5** | frustum 0.8 mm negative probe | 2 (depths 1–2, as predicted) |
| **I-6** | 0.3 mm negative probe, top and bottom sections | 2 + 2 |
| **I-2** | F2+I-2 `CHECK_FALSE(probe 1.0)` | 1 (even parity — the skin doubled from 0.75 to 1.4 mm) |
| — | **I-3 test now passes** | confirms the I-3 fix |

Collateral (attributed, not noise): the F3 literal-value section and the Arachne-floor guard probe
also fail under the widened band, and the pre-existing F1 prism/boss tests fail under the halved
`layer_idx` inset — i.e. those guards are load-bearing too.

### GREEN — all fixes, mutations reverted

`[paintdepth]`: **43 test cases, 579 assertions, all pass** (baseline 36 / 416).

---

## 3. Per-generator coverage — stated plainly

**Before this wave: no `[paintdepth]` test anywhere set `wall_generator` (harness default =
Arachne), and no test in the suite asserted on extrusions at all.** That is now false in both
halves.

| behaviour | Arachne | classic |
|---|---|---|
| band arithmetic (F3 formula, two-sided) | ✅ unit | ✅ unit (`paint_depth_band_classic_floor_mm`, all 5 sections) |
| notch cap, walls vs millimetres (I-3) | ✅ unit + ✅ end-to-end geometric | ✅ unit (mode gate is generator-independent) |
| N-bead budget on both parities (F3+F4), now two-sided | ✅ end-to-end | — (no bead-count contract exists on classic) |
| band floor at one wall stack | ✅ end-to-end **negative** pin (must NOT be floored: probe 0.65 unclaimed) | ✅ end-to-end **positive** pin (probe 0.65 claimed, 0.95 not), both parities |
| band tiling (loops across the band) | ✅ `loops > 2` + captured diagnostics | ✅ `loops == 2` (one onion iteration on an annulus = contour + hole) |
| gap fill present in the painted band | ✅ pinned **absent** (`gap_fills == 0`) | ✅ pinned **present** (`gap_fills >= 1`) on a real mid layer |
| gap-fill filament with `paint_infill_override = false` | n/a (no gap fill) | ✅ `erGapFill → Wall` (2), `erInternalInfill → SparseInfill` (1), `erSolidInfill → SolidInfill` (2) |
| lateral clamp: thin-fin no-op (C-1) and printable-skin degradation (F2/I-2) | ✅ end-to-end, both parities | — (generator-independent; it is segmentation geometry) |
| F1 inset reference layer (I-6), both descent directions | ✅ end-to-end | — (generator-independent) |

Two new fixtures run `Print::process()` (not `slice()`), which is what makes perimeter and fill
inspection possible at all — the first tests in this suite to do so for a single painted object.

**One number deliberately not pinned.** Arachne emits 6 closed loops + 4 open paths (2 external) on
the classic test's fixture rather than the 3 beads the flat-band arithmetic predicts. The fixture is
a square annulus, and at each of its four corners the local width across the band is `band·√2`
(~2.0 mm against 1.41 mm along the flats), so Arachne's variable-width beading correctly adds short
extra beads there. Pinning 6 would pin Arachne's corner behaviour, not this feature's, so the test
asserts `loops > 2` and records the full breakdown in `CAPTURE`s.

---

## 4. Gates

| gate | result |
|---|---|
| `[paintdepth]` | **579 assertions in 43 test cases, all pass** (baseline 416 / 36) |
| `[chameleon]` | **605 assertions in 133 test cases, all pass** — exactly at baseline |
| full `libslic3r_tests` | 471 cases, 469 passed, 2 failed-as-expected (pre-existing xfail), exit 0 |
| `ALL_BUILD` (scratchpad `build_next_wt.bat`) | **exit 0** |
| `spike/verify_paintdepth.sh` ×2 | **17/17 ALL PASS** both runs, including `unpainted-run{1,2}-vs-baseline` byte-identical (normalized) against the frozen pre-feature baseline and `unpainted-determinism` |

All four re-run on the final binary (`libslic3r_tests.exe` built 08:05:29, newer than every source
file in the commit).

Defaults untouched, per the standing user decision: `paint_depth_walls` stays 3, `paint_depth_mm`
stays 1.5, `paint_depth_mode` stays walls.

---

## 5. Self-review hand-walks

* **classic, walls = 1, painted cap ⇒ no void ring.** The floor sets band(1) = `ext_w + ext_s` =
  0.878540, which is *exactly* F1's own `wall_stack` inset. `wall_stack − band = 0`, so the base
  annulus the union used to leave between the lateral band and the projected top/bottom claim has
  zero width by construction. Pinned geometrically by the classic section's probe(0.65) claimed /
  probe(0.95) unclaimed on both parities.
* **`paint_infill_override = false` on classic ⇒ gap fill painted, sparse infill base.**
  `fill_filament_source(cfg, erGapFill) = Wall` → `wall_filament` = 2;
  `fill_filament_source(cfg, erInternalInfill) = SparseInfill` → `sparse_infill_filament` = 1. Both
  asserted against the real region config of a processed classic slice whose painted band is
  confirmed to carry gap fill.
* **thin fin ⇒ a ≥ `ext_width` claim, or a clean no-op, never a sub-extrusion sliver.** The
  continuation condition `b >= min_claim_width` means every `b` the ladder can emit is at least one
  external extrusion; the first `b` below it exits the loop, leaving `core = offset_ex(layer, −band)`
  — empty on thin geometry, i.e. the pre-F2 no-op. Both branches are pinned: the 1.2 mm fin at
  2.0 mm depth asserts the *no-op* (centre painted), the 5.8 mm fin at 6.0 mm depth asserts the
  *degradation* (painted at 0.5 mm, base at 1.0 mm and at the centre, identically on both parities).

---

## 6. Concerns / residue

1. **The notch no longer applies to degraded (thin-geometry) claims at all.** This is the price of
   I-2 and is documented in-code. It is the right trade — an alternating skin is worse than no tooth
   — but it is a behaviour change on thin geometry that no user has seen in a GUI slice yet.
2. **`min_claim_width` is `max(ext_perimeter_width)` across regions.** Conservative in the
   "never emit an unprintable strip" direction, which is right, but on a multi-region object with
   one wide-walled region it makes the ladder give up earlier on the narrow-walled ones than it
   strictly must. Documented at the call site.
3. **The classic floor is a `PrintObjectConfig` decision, not per-region.** `wall_generator` is an
   object-level key, so this is exact today; if it ever becomes per-region the branch needs to move.
4. **Item 9 changes a shared code path.** `fill_filament_source` is consulted for *every* region,
   not only painted ones. It is inert wherever `wall_filament == sparse_infill_filament` (all
   single-filament prints, hence the unpainted byte-parity gate), and where they differ it makes
   G-code emission agree with the tool-ordering plan that was already being computed. That is a
   correctness improvement, but it is upstream-wide, not paint-depth-local.
5. **Arachne's corner bead behaviour** (§3) is recorded, not pinned. If Wave B or a later change
   alters the painted band's shape, that number will move without failing anything.
6. **Minors from the review left undone**: M-1 (report wording), M-2 (thin-feature trade-off belongs
   in the GUI validation script), M-3 (`jtMiter` dependency comment), M-4 (naming the wrap-around
   geometry), M-6 ("parity-independent" wording in `PaintDepth.hpp`/commit message). M-5 and M-7's
   stale in-code comments **were** corrected, because both sit in code this wave edited and both had
   become actively misleading.

---

## 7. What this wave did for Wave B (Option N)

Nothing pre-empted, nothing made harder:

* F1's inset is untouched and now **enforced** on downward-narrowing geometry (I-6) — the design doc
  §5 leans on exactly that invariant to argue Option N is safe, and it is now a test rather than an
  argument.
* The `exposed_surface_part` gate, the `break` at `:1785`/`:1835`, `small_region_threshold` and the
  `granularity` sizing at `:1449-1465` are all untouched — Option N's four edit sites are clean.
* The anti-smear fixture (`make_square_frustum(40, 22, 6)` at 0.3 mm layers, `pdmMillimeters 0.15`)
  still passes unmodified, so it remains the untouched regression pin the design doc's T3 relies on.
* `slice_painted_frustum()` and the two new recursive extrusion counters are reusable by Wave B's
  T1/T2/T3 directly.
* The classic floor is a `max()` on the band, so if Wave B redefines the band as a normal thickness
  `D`, the floor keeps meaning "at least two printable lines" without rework.
