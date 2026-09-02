# Wave A review — `ef9c20d90f` as committed

Scope: `git show ef9c20d90f` against `wave-a-report.md`, `bleed-and-walls-fixwave-review.md`
(C-1, I-1..I-6) and `classic-generator-investigation.md` (items 8–10).
Read-only review; no edits made. Wave B's in-flight edits ignored — the working tree was clean
(`git status --porcelain` empty for tracked files) and `libslic3r_tests.exe` (08:05:29) is newer
than every source file in the commit, so every number below is measured on the committed code.

**Verdict: FIX FIRST.** 1 Critical, 4 Important, 5 Minor.

---

## Per-check summary

| # | check | result |
|---|---|---|
| 1 | C-1: `min_claim_width` plumbing, no-op is the OLD behaviour, max-across-regions, fuzzy | **PASS** |
| 2 | Item 9: `fill_filament_source` correct for every caller and role | **FAIL** — C-1 (Critical), I-3 (Important) |
| 3 | Item 8: classic predicate, floors only where intended, "meets by construction" | **FAIL** — I-1 (Important), M-2/M-3/M-4 |
| 4 | I-2 decision: thresholds from the un-notched band | **FAIL** — I-2 (Important) |
| 5 | I-3 decision: notch clamp walls-mode only | **CONCERN** — I-4 (Important) |
| 6 | Test integrity: mutation discrimination, per-generator split | **PASS** (mutations verified); coverage gap folded into I-1 |
| 7 | Regression sweep re-run | **PASS** for the three paint gates; full-suite line misreported (M-1) |

---

# Critical

## C-1 — item 9 changes output on **unpainted** prints that use a mixed (chameleon) filament with a grouped manual pattern

`src/libslic3r/GCode.cpp:5216-5221` (`configured_extruder_id`) →
`src/libslic3r/PrintRegion.cpp:41-50` (`fill_filament_source`) →
`src/libslic3r/GCode/ToolOrdering.cpp:286-292` vs `:306-313`.

The report's §6.4 argues item 9 is "inert wherever `wall_filament == sparse_infill_filament` (all
single-filament prints, hence the unpainted byte-parity gate)". That is true of the *first* lambda
(`configured_filament_id_1based`, which reads the config values directly) but **not** of the second.
`LayerTools::wall_filament` and `LayerTools::sparse_infill_filament` are not the same function of the
same id:

```cpp
unsigned int LayerTools::wall_filament(const PrintRegion &region) const {          // ToolOrdering.cpp:286
    unsigned int id = (extruder_override == 0) ? region.config().wall_filament.value : extruder_override;
    return resolve_mixed_1based(id) - 1;                                            // <- no grouped-pattern step
}
unsigned int LayerTools::sparse_infill_filament(const PrintRegion &region) const { // ToolOrdering.cpp:306
    unsigned int id = (extruder_override == 0) ? sparse_infill_filament_id_1based(region) : extruder_override;
    const unsigned int grouped = grouped_manual_pattern_infill_filament_1based(*this, region, id);
    return ((grouped != 0) ? grouped : resolve_mixed_1based(id)) - 1;
}
```

`grouped_manual_pattern_infill_filament_1based` (ToolOrdering.cpp:118-135) returns non-zero whenever
the configured filament is a mixed filament whose normalized `manual_pattern` contains a comma, and
then resolves through `mixed_mgr->resolve_perimeter(grouped_id, …, innermost_perimeter_index, …)`.
`wall_filament` skips that entirely.

**Failure scenario (no paint anywhere).** Classic wall generator + a chameleon filament with a
grouped manual pattern, `wall_filament == sparse_infill_filament == that filament`. Before: every
gap-fill line resolved via `resolve_perimeter(grouped_id, …, innermost_perimeter_index)`. After: via
`resolve_mixed_1based(id)`. Same configured filament id, different **physical extruder** → gap fill
changes colour on an object that has never been painted. The grouped-manual-pattern split at
GCode.cpp:5935-5989 only runs for `entity_type == PERIMETERS`, so for an INFILL collection
`correct_extruder_id` is used verbatim and there is nothing downstream to absorb the difference.

**Why no gate caught it.** `verify_paintdepth.sh`'s unpainted byte-parity run slices with the harness
default (Arachne), and `process_arachne` never emits gap fill — the one configuration that could show
this is exactly the one the parity gate does not exercise. `[chameleon]` (605/133, at baseline)
asserts on config/remap logic, never on `Print::process()` G-code. The new classic tests set
`paint_infill_override=false` on a plain two-filament setup, never a mixed one.

**Fix.** Gap fill is the innermost wall line, so the *grouped* resolution was arguably the more
correct one. Either (a) add `LayerTools::gap_fill_filament(region)` that takes
`wall_filament.value` as the id but still runs `grouped_manual_pattern_infill_filament_1based`, and
call that from the `FillFilamentSource::Wall` arm; or (b) suppress the redirect when
`layer_tools.mixed_mgr != nullptr && mixed_mgr->is_mixed(id, num_physical)`. Either way add a
regression test that processes a classic-generator, grouped-manual-pattern, **unpainted** object and
pins the gap-fill extruder.

---

# Important

## I-1 — item 8's void ring is closed on **odd layers only**; the notch reopens it on every even layer

`src/libslic3r/MultiMaterialSegmentation.cpp:1279` and `:2727-2730`,
`src/libslic3r/PaintDepth.cpp:31-39`.

The arithmetic checks out at stock flows (verified by hand): `s = 0.428540`,
`band(1) = 1.25s + 2(W−S) = 0.578595`, `wall_stack = W + S = 0.878540`, and the floor sets
`band(1) = 0.878540`, which is exactly F1's inset at MultiMaterialSegmentation.cpp:1839/1889. So
`wall_stack − band = 0` — **on odd layers**.

But `cut_segmented_layers` narrows the band by the interlocking notch on even layers:

```cpp
const float region_cut_width = ((layer_idx % 2 == 0) && (interlocking_cut_width > 0.f))
                             ? interlocking_cut_width : cut_width;      // = cut_width − notch
```

The F1 top/bottom claim is *not* subject to that (it is unioned in by `merge_segmented_layers` at
:2664, **after** `cut_segmented_layers` at :2652). So on even sub-surface shell layers the base
region again holds a closed annulus of width `wall_stack − (wall_stack − notch) = notch = 0.1 mm`
(the default notch, under the 0.10714 cap at these flows) — and classic still prints a 0.1 mm closed
ring as **nothing** (`offset_ex` empty at i = 0, `last` cleared, gaps only from i ≥ 1), which is the
exact mechanism defect 3 describes. The commit message's "Floored, band(1) EQUALS F1's inset, so the
two meet by construction" and the report's §5 hand-walk ("`wall_stack − band = 0` … by construction")
are true for half the layers.

Net effect: the void ring goes from 0.300 mm on every shell layer to 0.100 mm on every **even** shell
layer. Better, not closed.

**Fix.** Floor the classic band at `wall_stack + interlocking_depth` (so the *effective* even-layer
band still reaches the F1 inset), or suppress the notch on classic when the floor is what is binding.
Note this must be decided against `interlocking_depth` after `paint_depth_interlocking_depth_mm`,
i.e. the floor needs the notch value, which the current call order at :2727 does not have — the notch
is computed at :2752, after the band loop.

**Coverage gap that let it through.** The new classic-floor test paints `PLUS_X_FACE` on a box and
only calls `PrintObject::slice()`; it never builds a painted *cap*, never runs `Print::process()`, and
never probes the annulus between the lateral band and the F1 inset. Item 8's most serious defect
(the void ring) is pinned by no test on either generator — only the band depth is.

## I-2 — the I-2 fix is half applied: the ladder's *entry* still comes from the notched band

`src/libslic3r/MultiMaterialSegmentation.cpp:1229-1248`.

```cpp
ExPolygons core = offset_ex(layer_slices, -band);                                   // NOTCHED
ExPolygons thin = core.empty() ? layer_slices : diff_ex(layer_slices, offset_ex(core, band));  // NOTCHED
float b = 0.25f * ladder_band;                                                       // un-notched (the fix)
```

Only `b` was moved to `ladder_band`. Whether a part enters the ladder at all — `thin`, and the `core`
it is derived from — is still computed from the notched `band`, so the *set* of parts the ladder
touches still alternates with parity, and the discontinuity at that boundary is an order of magnitude
larger than the notch the fix was removing.

**Failure scenario.** `walls = 4`, stock flows: `band = 1.86422`, `notch = 0.1`,
`band_even = 1.76422`, `b₀ = 0.25·1.86422 = 0.46606 ≥ min_claim_width (0.45)`, so the ladder runs. A
part whose local half-thickness `t` lies in `[1.76422, 1.86422]`:

* even layer — `t > band_even`, `core` non-empty there, part not in `thin` → claim = **1.764 mm** per face;
* odd layer — `t < band_odd`, part in `thin`, step 0 membership `2b = 0.932 ≤ t` → claim = **0.466 mm** per face.

That is a 1.30 mm claim alternating on adjacent layers — the same class of defect I-2 names, on a
0.1 mm-wide window of half-thickness that a prismatic feature can sit in for its whole height.
Reachable whenever `0.25·band ≥ ext_w`, i.e. `band ≥ 1.8 mm` → `paint_depth_walls ≥ 4`, or
millimetres mode ≥ 1.8 mm (the 4–6 mm regime F2 was written for). I-3 makes the window *wider* in
millimetres mode, because the notch is no longer capped there — a hand-set 0.5 mm notch gives a
0.5 mm-wide alternation window.

At the shipped default (`walls = 3`) the ladder is floored off, so the alternation collapses to the
notch itself; this does not bite by default.

**Fix.** Derive `core`/`thin` from `ladder_band` as well and apply the notch only to the final
full-band erosion returned to the caller — or, equivalently, clamp the ladder's output so a degraded
claim is never narrower than `band − notch` when the part is within the notch of the band. Also
correct the in-code claim at :1224-1226 ("makes `b` parity-independent by construction") — `b` is,
the resulting claim is not.

## I-3 — `ToolOrdering::collect_extruders` still buckets gap fill as sparse infill, so the layer's extruder *set* now disagrees with emission

`src/libslic3r/GCode/ToolOrdering.cpp:826-842`.

```cpp
for (const ExtrusionEntity *ee : layerm->fills.entities) {
    ExtrusionRole role = fill->entities.empty() ? erNone : fill->entities.front()->role();
    if (internal_solid_infill_uses_sparse_filament(region, role)) has_sparse_infill = true;
    else if (is_solid_infill(role))                               has_solid_infill  = true;
    else if (role != erNone)                                      has_sparse_infill = true;   // <- erGapFill lands here
}
...
if (has_sparse_infill) layer_tools.extruders.emplace_back(layer_tools.sparse_infill_filament(region) + 1);
```

The report says item 9 "makes G-code emission agree with the tool-ordering plan that was already
being computed". It makes it agree with `LayerTools::extruder` (ToolOrdering.cpp:323-334 — confirmed:
`has_infill()` is non-recursive over direct children and `is_infill(erGapFill)` is false, so a
gap-fill-only collection has always routed to `wall_filament`). It now **disagrees** with
`collect_extruders`, which is the function that decides which tools the layer actually needs.

**Failure scenario.** Multi-extruder print, `wall_filament != sparse_infill_filament`, a region on a
layer whose `fills` carries gap fill but no real infill (routine on thin-walled features where the
perimeters consume the whole cross-section). `sparse_infill_filament` is still pushed into
`layer_tools.extruders`, but nothing is now emitted with it → `process_layer` still walks
`layer_tools.extruders` and issues the tool change / wipe-tower purge for an extruder that prints
nothing on that layer. Wasted filament and time, and a wipe-tower block for an unused tool.

Not a correctness break (GCode.cpp:5993's `layer_tools.has_extruder()` fallback keeps the gap fill on
a real tool), but a new, unintended side effect of a shared path.

**Fix.** Use `fill_filament_source` in `collect_extruders` too — a gap-fill-only collection should
contribute nothing (the region's perimeters already register `wall_filament` at :800-810), which also
removes the last copy of the bucket rule.

## I-4 — the shipped tooltip now states a cap that no longer exists in millimetres mode, and the loop alternation is reachable there

`src/libslic3r/PrintConfig.cpp:3971-3977`.

> "The effective depth is capped at a quarter of one perimeter spacing (about 0.11mm at a 0.45mm line
> width), because a deeper notch would narrow the painted band on alternating layers by more than a
> whole wall loop…"

That sentence is unconditional. Report reason #3 for I-3 — "the tooltip already says walls-mode …
gating is the one where the shipped documentation is already correct" — is not accurate: the tooltip
*motivates* the cap with a walls-mode consequence but *states* the cap as a property of the option.
After I-3 it is false in `pdmMillimeters`, so a user who types 0.5 there and measures 0.5 is now
contradicted by the tooltip in the other direction.

On the review question "can a large mm-mode notch reintroduce the loop-eating alternation F4
removed?" — **yes**. In millimetres mode `band_even = paint_depth_mm − notch`; a hand-set 0.5 mm notch
on a 1.5 mm band gives 1.5 / 1.0 mm strips, which straddle Arachne's bead-count boundaries and
produce exactly the 3/2/3/2 loop alternation the user originally reported. The mitigation is that it
takes a deliberate non-default value in a non-default mode, and that millimetres mode publishes no
N-loop contract — that argument is coherent, but it must be written into the tooltip, not left
contradicted by it.

Two sub-points verified while checking the decision:

* **"nothing changes by default" has an exception.** The cap is `0.25·perimeter_spacing`, and
  `spacing = width − h(1−π/4)`. With a 0.42 mm inner wall at 0.2 mm layers, `s = 0.377` and the cap is
  **0.0943 < 0.1** — a very common Orca profile. In walls mode the default notch is still capped to
  0.0943; in millimetres mode it is now 0.1. Millimetres-mode output is therefore not byte-identical
  at the default notch for those profiles (0.0057 mm on the even-layer band — negligible in print
  terms, but the report's claim is absolute).
* The mode plumbing itself is correct: `pdmUnlimited` is gated at the call site
  (MultiMaterialSegmentation.cpp:2751) and handled identically in the helper for totality.

---

# Minor

**M-1 — the full-suite gate is misreported.** Report §4: "full `libslic3r_tests` | 471 cases,
469 passed, 2 failed-as-expected (pre-existing xfail), exit 0". Actual, re-run on the committed
binary in default order:

```
test cases:   471 |   467 passed | 2 failed | 2 failed as expected
assertions: 50660 | 50656 passed | 2 failed | 2 failed as expected
exit=2
```

The two real failures are `tests/libslic3r/test_mixed_filament.cpp:3469` ("m1: compute uses
num_physical bound, remove_physical uses kMax=64 (differential)") and `:4415` ("batch_remap mixed
pair-fallback (stable_id=0) should match renumbered pair (KNOWN bug)") — deliberately-red
documentation tests written as plain `CHECK`s rather than `[!shouldfail]`, in a file this commit does
not touch, in code (`compute_redundant_filaments`, `batch_remap`) unrelated to anything Wave A edits.
**Not a Wave A regression**, but the gate line as written is not what the suite reports, and the
"exit 0" claim is wrong.

**M-2 — "N ≥ 2 is untouched on classic" holds only at stock flows.**
`band(N) − wall_stack = (N+0.25)·s + W − 3S`. With equal inner/outer widths this is
`0.25·S + h(1−π/4) > 0` at N = 2 for any flow, so N ≥ 2 is provably untouched — but with a wide outer
wall (outer 0.6 / inner 0.42 at 0.1 mm) `band(2) = 0.940 < wall_stack = 1.179` and N = 2 is floored
too. That is what the floor's own rationale asks for, but it silently deepens the band a user asked
for at N = 2 on those profiles, and no test covers it. The commit message's "the only band under the
floor at stock flows" is correctly qualified; the report's §1 item-8 framing ("three defects, all at
`paint_depth_walls = 1`") is not.

**M-3 — "band(1) EQUALS F1's inset" is exact only for one region at a uniform layer height.**
The floor uses `region.flow(print_object, frExternalPerimeter, print_object.config().layer_height)`
(MultiMaterialSegmentation.cpp:2724-2725); F1 uses `stat.extrusion_spacing + stat.extrusion_width`
built at `:1716-1731` from the raw `outer_wall_line_width` option and the **actual** `layer.height`.
Consequences: (a) with adaptive/variable layer height a layer thinner than nominal produces a larger
F1 inset (≈ +0.026 mm going 0.2 → 0.08 mm), reopening a sub-25 µm ring; (b) when
`outer_wall_line_width` is left at 0 (auto), `stat.extrusion_width` is 0 while the floor's `Flow`
resolves the real auto width — the two are not the same quantity at all. The multi-region direction is
safe (`max_r(band_r) ≥ max_r(W_r + S_r) ≥ stat.wall_stack`, since spacing is monotone in width).
Worth a comment at minimum.

**M-4 — the classic predicate does not mirror the real generator selection.**
`MultiMaterialSegmentation.cpp:2721` uses `wall_generator == PerimeterGeneratorType::Classic`, while
the generator is actually chosen at `LayerRegion.cpp:244` as `Arachne && !spiral_mode` — i.e. spiral
vase runs classic even with Arachne configured. Currently unreachable (`Print.cpp:1627` rejects spiral
vase on multi-material objects), but the two should be written the same way so it stays unreachable.

**M-5 — stale in-code claim.** `MultiMaterialSegmentation.cpp:1224-1226`: "Choosing the step from the
un-notched band makes `b` parity-independent by construction." True of `b`; the sentence reads as a
claim about the claim. See I-2.

---

# What passed, with the evidence

**Check 1 — C-1 (PASS).** `ext_perimeter_width` is plumbed
`multi_material_segmentation_by_painting` (:2731) → `segmentation_by_painting` (:2441, :2784) →
`cut_segmented_layers` (:2652, scaled) → `paint_depth_clamp_keep_core` (:1240).

* *The no-op is genuinely the old behaviour.* The floor is in the `for` condition, so when
  `b₀ = 0.25·ladder_band < min_claim_width` the body never runs and `core` is exactly
  `offset_ex(layer_slices, -band)` — byte-identical to pre-F2. Not a partial claim.
* *Max-across-regions is conservative in the right direction.* A larger `min_claim_width` stops the
  ladder earlier → more no-op → the whole cross-section keeps its paint, which is the printable
  pre-F2 outcome. Documented at :2712-2717.
* *Fuzzy skin cannot arm the ladder.* `fuzzy_skin_segmentation_by_painting` (:2803) passes
  `max_external_perimeter_width` as **both** the band and `min_claim_width`, and `b₀ = 0.25·band < band`
  for any positive band, so the loop never executes. Byte-identical to upstream, as claimed. (This is
  a deliberate behaviour change relative to the parent commit for fuzzy skin — F2's ladder is reverted
  there.)
* The floor is also *sufficient*, not merely necessary: at `b = ext_w = 0.45` Arachne sees
  `T = 0.45 − 2h(1−π/4) = 0.407 > min_bead_width (0.34)`, so no `WideningBeadingStrategy` widening.
* I-1's early `continue` is behaviour-preserving: the skipped
  `segmented_regions[layer_idx] = std::move(segmented_regions_cuts)` would have written a
  same-sized, all-empty vector over an all-empty one.

**Check 6 — test integrity (PASS on discrimination).** Two mutation claims spot-verified analytically
against the committed tests, both exact:

* *band margin 0.25 → 0.5 with the spec mirror updated.* `test_paint_depth.cpp:213` uses a **local**
  `margin = 0.25·s` literal, independent of `expected_band`'s mirror, so upper bound A fails for all
  6 walls × 3 notches = **18**. Upper bound B (`:216`) fails only where `interlock < 0.5s − 0.25s`:
  at `configured = 0.1` the notch is uncapped (0.1 < 0.10714) so `band − interlock = opt + 0.1143 >
  opt + 0.1071`; at 0.3 and 1.0 the notch saturates at the cap and B passes by 1e-5. That is
  **6** failures, walls 1–6 at the 0.1 notch — exactly the report's number. The assertions genuinely
  discriminate; they are not passing for an unrelated reason.
* *`last_idx` → `layer_idx`.* On `make_square_frustum(22,40,6)` the half-width is `H_k = 20 − 0.15k`;
  the correct inset puts the claim edge 0.87854 mm inside `H_k`, the mutation puts it
  `0.87854 − 0.15k` inside. At depth 4 that is 0.2785 and at depth 5 it is 0.1285, both < the 0.3 mm
  probe, so `CHECK_FALSE` fails at both depths in both sections = **2 + 2**. Matches. `layer_edge_probe`
  (`:646-651`) measures from the layer's own bbox, which on a square frustum is its own contour, so the
  probe is valid.
* Per-generator split: the behaviours tested on one generator only are all genuinely
  generator-independent (the lateral clamp and the F1 inset happen in segmentation, before perimeter
  generation; the bead-count contract exists only on Arachne; gap fill exists only on classic). The one
  real hole is the void ring — see I-1.

**Check 7 — regression sweep (re-run by me, on the committed binary).**

| gate | re-run result |
|---|---|
| `[paintdepth] --order rand` | **579 assertions in 43 test cases, all pass** ✔ matches report |
| `[chameleon] --order rand` | **605 assertions in 133 test cases, all pass** ✔ at baseline |
| `spike/verify_paintdepth.sh` | **17/17 ALL PASS**, incl. `unpainted-run{1,2}-vs-baseline` byte-identical ✔ |
| defaults | `paint_depth_mode = walls`, `paint_depth_walls = 3`, `paint_depth_mm = 1.5`, `mmu_segmented_region_interlocking_depth = 0.1` ✔ untouched |
| full `libslic3r_tests` | 467 passed / 2 failed / 2 failed-as-expected, **exit 2** — see M-1 |

**Item 9, the parts that are right.** `fill_filament_source` is exactly behaviour-preserving for every
role other than `erGapFill`: `is_solid_infill(erGapFill)` is false and `erGapFill != erSolidInfill`,
so hoisting the gap-fill test to the front cannot reorder any other role's outcome. The role is read
from a *homogeneous* sub-collection — `Layer::make_fills` (Fill.cpp:1351-1360) wraps each thin fill in
its own `ExtrusionEntityCollection`, and GCode.cpp:5726-5730 iterates those sub-collections, so
`entities.front()->role()` never mixes gap fill with real infill in one bucket. The `erNone` /
future-role fall-through is preserved.
