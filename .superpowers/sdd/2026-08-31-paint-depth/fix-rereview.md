# Paint Depth — Fix-Wave Re-Review

Reviewer: scoped re-review agent (read-only) · Date: 2026-08-31
Scope: commit `e64a4e154e` vs findings F1/F2/F3 (+F4/F5) in final-review.md
Method: hand-executed traces, upstream comparison (WebFetch of PrusaSlicer master), suite re-runs by this reviewer.

Verdict: **CLEAR** — all five findings resolved, no regressions found, no new issues of substance.

---

## F1 — RESOLVED (interlocking sub-band restored to Prusa semantics)

Post-fix `cut_segmented_layers` (MultiMaterialSegmentation.cpp:1146-1183):

```cpp
const float interlocking_cut_width = interlocking_depth > 0.f ? std::max(cut_width - interlocking_depth, 0.f) : 0.f;
...
const float region_cut_width = ((layer_idx % 2 == 0) && (interlocking_cut_width > 0.f)) ? interlocking_cut_width : cut_width;
```

**Upstream comparison**: fetched `prusa3d/PrusaSlicer` master `MultiMaterialSegmentation.cpp`; both the
`interlocking_cut_width` computation and the `region_cut_width` ternary (including the
`interlocking_cut_width > 0.f` gate) match upstream **verbatim**. The fork drift (raw
`interlocking_depth` as a wholesale even-layer replacement band) is gone; the lambda capture list was
correctly updated to capture `interlocking_cut_width`.

**Hand-walk, default config** (walls=3, interlock=0.3, band = 0.45 + 2×spacing ≈ 1.3mm), bounded claim,
layers 0-3:
- Layer 0 (even): `interlocking_cut_width = band − 0.3 ≈ 1.0mm > 0` → region_cut_width ≈ 1.0mm.
  Clamped to the interlock-notched band — notch carved at the INNER boundary, not a 0.3mm sliver.
- Layer 1 (odd): region_cut_width = full band ≈ 1.3mm.
- Layers 2/3: repeat. Every layer stays within the 3-wall band; alternation is a 0.3mm notch at the
  inner edge on even layers only. Matches spec ("Prusa-style ~0.3mm") and GUI checklist items 1/4.

**Interlock ≥ band edge case**: `interlocking_cut_width = max(band − interlock, 0) = 0` → gate false →
even layers fall back to the FULL band. No layer ever cuts deeper than the band anymore (pre-fix, mm-mode
band < 0.3 had even layers cut deeper than requested). Saturating, sane, and identical to upstream's gate.

**RED-claim mechanics verified**: the new test (`test_paint_depth_clamp.cpp`, "walls-mode band width is
pinned...") probes the even layer at `band − interlock − 0.05` ≈ 0.95mm. Pre-fix the even-layer claim
reached only ~0.3mm, so `CHECK(probe_at_depth(even_layer, well_inside_both_mm))` genuinely fails on
pre-fix code — the RED claim is mechanically sound, not incidental. The tooth probe (`band − interlock/2`,
odd-claimed / even-unclaimed) pins the alternation itself, and the `band + 0.2` probe pins the outer edge
on both parities. Probe geometry is correct: +X face painted, probes measured inward from `bb.max.x()`;
layer parity indexing aligns because `input_expolygons` is built from `print_object.layers()` (same index
space as `object->get_layer()`); mid-height layers avoid the un-clamped top/bottom projection merge.

## F2 — RESOLVED (legacy key neutralized; flip-flop pin discriminates)

`handle_legacy_composite` (PrintConfig.cpp:7913-7927) now zeroes
`mmu_segmented_region_max_width` immediately after the nonzero migration. Hand-trace of the flip-flop
scenario (mirrors the new test in `test_paint_depth.cpp`):

1. Sparse load of `mmu_segmented_region_max_width = 0.8` → migration → {millimeters, 0.8} AND old key = 0.
2. User reverts mode to walls; diff-save vs defaults: mode==default drops out; old key==0==default
   (verified: `set_default_value(new ConfigOptionFloat(0.))` at PrintConfig.cpp:3886) drops out;
   only `paint_depth_mm = 0.8` survives.
3. Reload of that diff: no `mmu_segmented_region_max_width` in the file → guard's `config.has(...)`
   half is false → no re-migration → walls survives. Pre-fix, the 0.8 old key stayed in the diff and
   re-armed `!config.has("paint_depth_mode")` → millimeters, forever — exactly what the test's final
   `CHECK(... == pdmWalls)` catches; RED mechanics confirmed (pre-fix, both the step-1 neutralization
   CHECK and the final CHECK fail).
   The test correctly uses sparse `DynamicPrintConfig` objects (matching Preset.cpp's real
   representation) — `full_print_config()` would defeat the guard and test nothing.
4. Zero-legacy case needs no neutralization: 0 == default, never enters a diff, guard falls through
   to walls default (re-confirmed live by verify_paintdepth.sh legacy-zero-* checks).

**Prusa-3mf regression check**: the fix is a single `set_key_value` inside the nonzero branch of
`handle_legacy_composite`; per-key `set_deserialize` paths that skip the composite hook are untouched.
F7's exposure (parity with the pre-existing `wiping_volumes` composite migration) is unchanged.

## F3 — RESOLVED (all four read sites OR'd; member initialization safe)

Grep of `interface_shells` across src/: exactly four runtime read sites, all now covered —
- PrintObject.cpp:1322 and :1755 — `m_config.interface_shells.value || this->has_bounded_paint_depth()` (T3, unchanged)
- PerimeterGenerator.cpp:622 and :2273 — `object_config->interface_shells || has_bounded_paint_depth` (this fix)

(Remaining hits are comments, Preset.cpp's key list, and PrintObject.cpp:1096's invalidation key — none
read the flag's value at slice time.)

**Initialization on every construction path**: `PerimeterGenerator` has exactly ONE constructor
(PerimeterGenerator.hpp:105); `has_bounded_paint_depth` is NOT in its mem-init list, so the in-class
default initializer `= false` (hpp:104) applies on every construction — no uninitialized read is
possible even for a hypothetical future caller. The one and only instantiation in the tree
(LayerRegion.cpp:202, `LayerRegion::make_perimeters`) sets it explicitly from
`this->layer()->object()->has_bounded_paint_depth()` before `process_classic/process_arachne` run.

**Byte-inertness**: `has_bounded_paint_depth()` = `is_mm_painted() && mode != pdmUnlimited`, so
unpainted or unlimited objects evaluate both new ORs to their pre-feature value. Confirmed live:
verify_paintdepth.sh unpainted-run byte-parity vs the frozen pre-feature baseline PASSES post-fix
(runs 1 and 2, plus determinism).

## F4 — RESOLVED (paint_depth_mm=0 is coherent unlimited, traced)

Trace, millimeters mode with mm=0: `paint_depth_band_mm(pdmMillimeters, ..., 0.0, ...)` returns 0
(PaintDepth.cpp:13-14) → `max_width = 0`. Interlock stays 0.3 (mode ≠ unlimited), so the outer gate at
MultiMaterialSegmentation.cpp:2181 (`max_width > 0 || interlock > 0`) still enters
`cut_segmented_layers` — but inside, `interlocking_cut_width = max(0 − 0.3, 0) = 0` → gate false on
even layers → `region_cut_width = cut_width = 0` on EVERY layer → `if (region_cut_width > 0.f)` skips
the cut on both parities. Unbounded coherently everywhere (geometry identical to pdmUnlimited, minus
its cheaper early-out) — NOT the pre-fix alternating 0.3mm-even / unlimited-odd incoherence.
Documented at both the ConfigOptionDef and the F1 comment; tooltip updated to state 0 = unlimited.
Leaving `min = 0` is a defensible call given the now-coherent semantics.

## F5 — RESOLVED (tooltip)

`mmu_segmented_region_interlocking_depth` tooltip (PrintConfig.cpp:3948-3949) now names the real gate
("Only active when \"Paint depth mode\" is not \"Unlimited\"") and drops both false claims (the
legacy-key gate and the never-enforced "ignored if bigger than" bound). Accurate against the post-fix
code (over-large depth saturates at a full-band even-layer cut, per the F1 gate).

## Suite re-runs (this reviewer, fresh binaries — build 17:34 postdates all fix sources ≤17:32)

- `[paintdepth]` (libslic3r_tests): **15 cases / 84 assertions, all passed** (13/56 + the 2 new pins).
- `[chameleon]` (libslic3r_tests): **133 cases / 605 assertions, all passed**.
- `[chameleon]` (fff_print_tests): 1 case / 8 assertions, passed (not part of the reported 133/605
  count — noting for bookkeeping only; green either way).
- `spike/verify_paintdepth.sh`: **17/17 PASS** (unpainted byte-parity vs frozen baseline ×2,
  determinism, defaults, legacy nonzero/zero migration checks).

## New issues

- **None blocking.** Two trivia: (1) the F4 comment in PrintConfig.cpp says "the :2170 gate" — the gate
  now sits at MultiMaterialSegmentation.cpp:2181 (comment line-number drift only); (2) the reported
  chameleon count omits the one fff_print [chameleon] case (green). Neither warrants a change.

## Bottom line

The fix restores upstream-verbatim interlock semantics with a genuinely discriminating alternation pin,
kills the preset flip-flop with a correctly sparse-config test, closes interface_shells parity at all
four read sites with a safely-initialized threaded flag, and makes mm=0 coherent as a free consequence —
with unpainted byte-inertness re-proven live. CLEAR to proceed to the GUI round.
