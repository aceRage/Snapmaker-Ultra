# Classic wall generator vs. the paint-depth band

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `193d2e0675`.
Read-only; nothing edited. Scope: the **classic** wall generator only
(`wall_generator = classic`, `PerimeterGenerator::process_classic`).

**[V]** = verified by reading committed code (file:line given). **[D]** = arithmetic derived
from that code. No build or slice was run by this investigation.

Reference case throughout, unless stated: 0.4 nozzle, all line widths 0.45 mm, layer 0.1 mm,
stock defaults (`wall_loops = 2`, `precise_outer_wall = true`, `wall_sequence = InnerOuter`,
`detect_thin_wall = false`, `gap_infill_speed = 30 > 0`, `filter_out_gap_fill = 0`,
`paint_infill_override = true`).

Derived constants **[D]** (`Flow::rounded_rectangle_extrusion_spacing`, `Flow.cpp:181-186`):

| symbol | value | source |
|---|---|---|
| `k_h = h(1-π/4)` | 0.021460 | width→spacing conversion |
| `ext_w = w` | 0.450000 | line widths |
| `ext_s = s` | 0.428540 | `w - k_h` |
| `d1 = ext_perimeter_spacing2` | **0.450000** | `0.5(ext_w + w)` — **widths**, because `precise_outer_wall` is on (`PerimeterGenerator.cpp:1233-1236`) |
| `min_spacing` | 0.257124 | `s·(1 - INSET_OVERLAP_TOLERANCE)`, tolerance 0.4 (`libslic3r.h:88`) |
| gap-fill `min` / `max` | 0.054000 / 0.857080 | `0.2·w·(1-tol)` / `2·s` (`PerimeterGenerator.cpp:1651-1652`) |
| `wall_stack` (F1 inset) | 0.878540 | `stat.extrusion_spacing + stat.extrusion_width` (`MultiMaterialSegmentation.cpp:1779`, fed from `outer_wall_line_width` at `:1658/:1665`) |
| `band(3)` | 1.435675 | `PaintDepth.cpp:22-24` |

> Correction to the prior investigation (`wall-count-investigation.md` §2d): it used
> `ext_perimeter_spacing2 = ½(s_ext + s) = 0.4285`. That is the **`precise_outer_wall`-off**
> branch. The default is on, so `d1 = ½(ext_w + w) = 0.45` **[V]** `PerimeterGenerator.cpp:1234`.
> The N = 3 conclusion is unchanged either way (both put the i = 1 opening well under threshold).

---

## 0. The crux: real material defect, or preview/labelling artifact?

### **Verdict: at default settings it is a LABELLING artifact. The full band is filled with painted material.**

**[V]/[D]** At N = 3 (band 1.435675 mm) the classic generator lays **three** extrusions across
the band and they tile it end to end. Band coordinates measured inward from the layer contour:

| # | what | role | flow width | covers |
|---|---|---|---|---|
| 1 | onion i = 0, **contour** loop | `erExternalPerimeter` | 0.450000 | `[0.000, 0.450]` |
| 2 | gap fill (medial axis of the leftover) | `erGapFill` | **0.557135** | `[0.450, 0.986]` + 0.021 overlap each side |
| 3 | onion i = 0, **hole** loop | `erExternalPerimeter` | 0.450000 | `[0.986, 1.436]` |

Nothing else is in the band: at i = 1 the opening comes back empty, so
`loop_number = 0; last.clear(); break` **[V]** `PerimeterGenerator.cpp:1407-1412`, which means
`not_filled_exp` is empty and **no infill surface is emitted for the painted region at all**
**[V]** `:1728-1745`. There is no void, no sparse infill, and no base-colour material in the band.

Why it *looks* like 1–2 walls:

1. **[V]** For an annulus/strip, one onion iteration emits **two** loops — the expolygon's
   contour and its hole (`:1418-1431`) — and both are at depth 0, so both are tagged
   `erExternalPerimeter` (`:112-117`, `is_external() == (depth == 0)`). Classic can therefore
   only ever produce an **even** number of wall loops across a painted band, plus at most one
   gap-fill line in the middle. "3 walls" is structurally unreachable as three *loops*.
2. **[V]** The middle line is `erGapFill`, which the preview colours as gap infill, not as a
   wall, and which the wall-count overlays do not count.

So the user's "3 walls set, only 1–2 walls being used" is a correct reading of the preview and a
correct count of *loops*, but the painted **depth** is the full 1.4357 mm.

### Material actually delivered

**[V]** `variable_width` converts the medial-axis width `g` to an extrusion via
`flow.with_width(g + h(1-π/4))` (`VariableWidth.cpp:317`), so the gap-fill line's
`mm3_per_mm` is exactly `h·g` — it fills precisely the leftover, with the standard rounded-end
overlap onto its neighbours. There is **no clamp** to the flow's nominal width.

**[D]** Volume balance across the band, `[2·ext_s + 2(k-1)·s + g] / W` (k = onion iterations):

| N | band W | k | gap g | filled | note |
|---|---|---|---|---|---|
| 1 | 0.578595 | 1 | — (none) | **148.1 %** | **over-extrusion, see §2c** |
| 2 | 1.007135 | 1 | 0.107135 | 95.7 % | |
| **3** | **1.435675** | **1** | **0.535675** | **97.0 %** | the reported case |
| 4 | 1.864215 | 2 | 0.085675 | 96.5 % | |
| 5 | 2.292754 | 2 | 0.514214 | 97.2 % | |

**[D]** The 3–4 % shortfall is exactly `(k+1)·k_h` — the width-vs-spacing rounded-edge allowance
at the strip's two outer edges plus one per `precise_outer_wall` ext→internal junction. Every
ordinary wall stack in the slicer carries the same allowance; it is not paint-specific and it is
not a void.

### The one configuration where it IS a real defect: `paint_infill_override = false`

**[V]** Chain: classic gap fill → `PerimeterGenerator::gap_fill` → `LayerRegion::thin_fills`
(`LayerRegion.cpp:216`) → wrapped and pushed into `layerm->fills` (`Fill/Fill.cpp:1355-1360`) →
walked as `entity_type == INFILL` in `GCode::process_layer` (`GCode.cpp:5723-5725`) →
`configured_extruder_id` (`:5209-5221`, called at `:5872`) which, for a non-solid infill role,
returns **`sparse_infill_filament(region)`**. `is_solid_infill(erGapFill)` is false
(`ExtrusionEntity.hpp:89-97`).

**[V]** The painted `PrintRegion` sets `wall_filament` and `solid_infill_filament` to the painted
extruder unconditionally, but `sparse_infill_filament` **only when `paint_sparse_infill` is true**
(`PrintApply.cpp:1088-1099`), and `paint_sparse_infill = paint_infill_override || mode == pdmUnlimited`
(`:1862-1863`). `paint_infill_override` defaults to **true** (`PrintConfig.cpp:3955`), so the
default path is safe.

**[D]** With the user unchecking "Paint sparse infill" (an option this feature itself added,
and which is only greyed out in Unlimited mode): on **classic**, the middle 0.5357 mm of the
1.4357 mm N = 3 band — **37 % of the painted depth, sitting directly behind the single painted
outer loop** — flips to the **base filament**. **[V]** `process_arachne` never touches
`this->gap_fill` (no `gap_fill` reference anywhere after `:2200`), so this trap does not exist on
Arachne, and the option's tooltip ("walls and solid infill still print in the painted filament")
is simply untrue for classic users.

**Secondary, worth a separate look:** `LayerTools::extruder()` (`GCode/ToolOrdering.cpp:323-334`)
routes the same gap-fill-only collection to **`wall_filament`**, because `has_infill()` is false
for `erGapFill`. Tool ordering and G-code emission therefore disagree about which extruder the
painted band's gap fill needs whenever `paint_infill_override = false`.

---

## 1. Loop math on classic — how many loops does a region of width W get?

**[V]** `PerimeterGenerator.cpp:1318-1459`. With `detect_thin_wall = false`
(default, `PrintConfig.cpp:6345`) the onion is:

```
i = 0 :  offsets = offset_ex(last, -ext_w/2)                       (:1357)
i ≥ 1 :  offsets = offset2_ex(last, -(d_i + min_spacing/2 - 1),
                                    +(min_spacing/2 - 1))          (:1395-1397)
         gaps   += diff_ex(offset(last, -0.5·d_i),
                           offset(offsets, +0.5·d_i + 10))          (:1403-1405)
         d_1 = ext_perimeter_spacing2 ;  d_i≥2 = perimeter_spacing
```

For an annulus/strip of width `W` **[D]**:

* i = 0 survives iff `W > ext_w`; residual `W₁ = W − ext_w`. Emits **2 loops** (contour + hole).
* i ≥ 1 survives iff `W_i > 2·d_i + min_spacing` (the opening's erosion must not close the strip);
  residual `W_{i+1} = W_i − 2·d_i`. Emits **2 more loops**.
* The loop also stops at `i > loop_number = wall_loops + extra − 1` (`:1413`), i.e. **at most
  `wall_loops` iterations = `2 × wall_loops` loops** — the exact classic twin of Arachne's
  `LimitedBeadingStrategy` `2 × wall_loops` bead ceiling.
* Leftover after the last iteration becomes one gap-fill line if wider than `min = 0.054`
  (`opening_ex(gaps, min/2)`, `:1655`); its width is `W_k − d_k`, always ≤ 1.6·s < `max = 2s`,
  so a single medial-axis line always suffices. **[D]**

### W needed for genuine loops (0.45 lines / 0.1 mm layers, precise_outer_wall on)

| loops | iterations | requires W > | (precise off) |
|---|---|---|---|
| 0 | — | — | region vanishes for `W ≤ 0.450000` |
| **2** | 1 | **0.450000** | 0.450000 |
| **4** | 2 | **1.607124** | 1.564204 |
| **6** | 3 | **2.464204** (needs `wall_loops ≥ 3`) | 2.421283 |
| **8** | 4 | **3.321283** (needs `wall_loops ≥ 4`) | 3.278363 |

General **[D]**: `W_min(k) = ext_w + 2·d1 + 2(k−2)·s + min_spacing` for k ≥ 2.

### …and in *lines of painted material* (2k, plus 1 gap-fill line)

| lines | W window | at stock flows |
|---|---|---|
| 0 | `W ≤ ext_w` | ≤ 0.450000 |
| 2 | `ext_w < W ≤ ext_w + d1 + 0.054` | 0.450 – 0.954040 |
| **3** | up to `W_min(2)` | 0.954040 – **1.607124**  ← band(3) = 1.4357 lives here |
| 4 | up to `ext_w + 2d1 + s + 0.054` | 1.607124 – 1.832540 |
| 5 | up to `W_min(3)` | 1.832540 – **2.464204** |
| 6, 7 … | needs `wall_loops ≥ 3` | above 2.464204 |

**[D]** Above `W_min(wall_loops+1)` (2.464204 at `wall_loops = 2`) the onion stops via
`i > loop_number` **without** clearing `last` (`:1413-1415`), so the remaining core is emitted as
`stInternal` (`:1745`) — **sparse infill at the object's sparse density**, i.e. genuinely mostly
air. That is the first W at which the classic band stops being solid painted material. Arachne
behaves the same way past its own `2 × wall_loops` cap.

---

## 2. Does our band under-deliver material on classic?

### 2a. At N ≥ 2: no.
Answered in §0 — 95.7–97.2 % filled, deficit = the standard rounded-edge allowance.
Discretionary losses, all quantified **[D]** and all small:

| loss | size | when |
|---|---|---|
| leftover strip below `min = 0.054` is deleted (`:1655`) | ≤ 0.054 mm (≤ 3.8 % of band(3)) | only when the band lands within 0.054 of a loop-count boundary |
| `filter_out_gap_fill` drops short gap-fill polylines (`:1680-1683`) | whole middle line | default **0** (`PrintConfig.cpp:3193`); only bites on painted specks, since the band's gap fill is a closed ring the length of the painted patch |
| `douglas_peucker(resolution)` before medial axis (`:1660`) | ≤ 0.01 mm | noise |
| core → sparse infill above `W_min(wall_loops+1)` | unbounded | `W > 2.4642` at `wall_loops = 2` (N ≥ 6) |
| gap fill → `sparse_infill_filament` | 37 % of band(3), wrong **colour**, not missing material | `paint_infill_override = false` (§0) |

### 2b. At N = 1: yes — but the failure is over-extrusion, not shortfall.
**[D]** `band(1) = 0.578595`. The i = 0 offset leaves a 0.128595 mm annulus, so the two depth-0
external loops sit **0.128595 mm apart centre-to-centre** while each is 0.45 mm wide — 30 % of the
normal `ext_s = 0.428540` packing. Volume delivered ÷ volume available = **1.481, i.e. +48 %
over-extrusion** along the whole painted boundary. The overlap is knowingly uncompensated:
`:1419-1423` — *"Outer contour may overlap with an inner contour … FIXME evaluate the overlaps"*.

**[D]** This is not a regression: with the *pre-F3* band (`band(1) = ext_w = 0.45`) the i = 0
offset was empty and N = 1 paint produced **nothing at all** on classic. F3 moved it from
invisible to over-extruded.

**[D]** Classic cannot render a one-line-wide painted band at all: `offset_ex` on a strip always
returns both boundaries. The narrowest honest classic band is **two properly-spaced lines**,
`ext_w + ext_s = 0.878540` — which is exactly one `wall_stack`.

### 2c. A flow-dependent N = 1 cliff
**[D]** `band(1) = 1.25·s + 2·k_h` is **independent of `ext_w`** (both `ext_w − ext_s` and
`w − s` equal `k_h`), while classic's survival threshold *is* `ext_w`. So any profile with
`outer_wall_line_width > 1.25·s + 2·k_h` loses N = 1 paint entirely under classic. Example
**[D]**: outer 0.6 / inner 0.42 at 0.1 mm → `s = 0.398540`, `band(1) = 0.541095 < 0.6` → the
N = 1 painted region produces **zero extrusions on every layer**. Arachne rescues the same strip
with `WideningBeadingStrategy`.

---

## 3. One formula or two?

**Recommendation: one formula plus one classic-only floor.** The `0.25·s` count-window margin and
the `2(ext_w − ext_s)` pre-inset are Arachne artifacts, but on classic they are *harmless* — they
only widen the middle gap-fill line, because classic tiles whatever width it is given. The single
thing classic needs that Arachne does not is a **minimum**:

```
band(N) = N·perimeter_spacing + 2·(ext_w − ext_s) + 0.25·perimeter_spacing      // unchanged
if (wall_generator == classic)
    band = max(band, ext_perimeter_width + ext_perimeter_spacing)               // = one wall_stack
```

`print_object.config().wall_generator` is available at the band's call site
(`MultiMaterialSegmentation.cpp:2648`; it is a `PrintObjectConfig` key — `LayerRegion.cpp:242`
reads it the same way) **[V]**, so the branch costs one line.

**[D]** Numbers:

| N | today (both) | classic w/ floor | Arachne w/ floor | unified (unconditional `max`) |
|---|---|---|---|---|
| 1 | 0.578595 | **0.878540** | 0.578595 | 0.878540 |
| 3 | 1.435675 | 1.435675 | 1.435675 | 1.435675 |
| 6 | 2.721294 | 2.721294 | 2.721294 | 2.721294 |

**Why not the unconditional `max`** (which would be simpler): **[V]/[D]** Arachne's 1→2 bead
boundary is `T > (1 + split_thr)·ext_s` (`RedistributeBeadingStrategy.cpp:42-48`,
`split_thr = 2·min_bead_width/ext_w − 1 = 0.511111`) = 0.647570. Today's `band(1)` gives
`T = 0.535675` → **1 bead** ✓. A floored `band(1) = 0.878540` gives `T = 0.835620` → **2 beads**,
breaking the "1 wall means 1 loop" contract F3 just established (and pinned at 0.578595 in
`test_paint_depth.cpp:68`). So: branch, or knowingly accept 2 beads for "1 wall" on Arachne.

**The floor is not arbitrary.** `ext_w + ext_s` is *the same `wall_stack`* F1 uses for its inset
(§6) — with it, the lateral band and the F1-inset top/bottom claim meet exactly, and the two
mechanisms stop being able to leave a gap between them.

---

## 4. Which generator do the tests exercise? — **Arachne. Every one of them.**

**[V]** State it plainly: **no `[paintdepth]` test has ever exercised the classic wall generator,
and none has ever asserted anything about wall loops on either generator.**

* `tests/libslic3r/test_paint_depth.cpp` — pure arithmetic on `paint_depth_band_mm` /
  `paint_depth_interlocking_depth_mm`. No slicing. Its comments (`:49-51`, `:120-127`) state the
  acceptance criterion as the **Arachne bead-count window**; §127's invariant
  (`band − interlock ≥ N·s + 2(ext_w − ext_s)`) is an Arachne-only statement.
* `tests/libslic3r/test_paint_depth_clamp.cpp` — builds real meshes and calls
  `PrintObject::slice()` / `Print::process()`, but asserts only on **segmentation geometry**
  (claimed ExPolygon areas/extents). `grep -c "perimeters|ExtrusionEntity|thin_fills"` over the
  file returns **0** — no assertion anywhere on loops, extrusions or gap fill.
* No test file anywhere under `tests/` sets `wall_generator`; the harness starts from
  `DynamicPrintConfig::full_print_config()`, whose default is **Arachne**
  (`PrintConfig.cpp:6824`).
* Every G-code artifact in `spike/out/` carries `; wall_generator = arachne`
  (`paintdepth_baseline`, `paintdepth_defaults_1/2`, `paintdepth_legacy_zero/nonzero`).

So the entire verification chain for this feature — unit tests, end-to-end clamp tests, and the
G-code spikes the fix waves were signed off against — ran the generator this user does **not**
use. The classic behaviours in §0 and §2 were never observable to it.

---

## 5. The user's "3 walls ≈ 2 mm" on classic

**[D]** What 2 mm of genuinely painted material costs under classic:

| target | setting | band | classic result | filled |
|---|---|---|---|---|
| ~1.9 mm | `paint_depth_walls = 4` | 1.864215 | 4 loops + 1 gap line (0.086) | 96.5 % |
| **exactly 2.0 mm** | **`paint_depth_mode = millimetres`, `paint_depth_mm = 2.0`** | 2.000000 | 4 loops + 1 gap line (0.221) | 96.6 % |
| ~2.3 mm | `paint_depth_walls = 5` | 2.292754 | 4 loops + 1 gap line (0.514) | 97.2 % |

**Millimetres mode is the better answer for a classic user**: classic has no bead-count window, so
the `0.25·s` margin term buys nothing, and the band maps one-to-one to painted depth.

**[D]** Two notes that differ from the Arachne advice in `wall-count-investigation.md` §5:

* On classic, N = 5 needs **only 2 onion iterations**, so it works at the default `wall_loops = 2`
  — no `wall_loops ≥ 3` bump required. (Arachne at the same band wants 5 beads and is capped at
  4 by `LimitedBeadingStrategy`, dumping the rest into infill.) Classic users get *better*
  coverage than Arachne users at N = 5 with stock walls.
* Keep the band **≤ 2.464204 mm** at `wall_loops = 2` (N ≤ 5, or `paint_depth_mm ≤ 2.46`).
  Past that the leftover core becomes sparse infill (§1) and the band stops being solid.

---

## 6. Interaction with the just-landed F1 inset and F4 notch clamp

### F4 — notch clamped to `0.25·s = 0.107135` (`PaintDepth.cpp:38`)

**[D]** On classic, for every N ≥ 2 the notch **cannot change the loop count**. It only narrows
the middle gap-fill line on even layers: at N = 3 the gap goes 0.535675 → 0.428540 (extruded
0.557 → 0.450 mm), a ~20 % width oscillation with full coverage on both parities. Classic is
structurally immune to the 3/2/3/2 alternation, because its middle line is a variable-width gap
fill rather than a counted bead — even the **unclamped** 0.3 mm notch left N = 3 at three lines
(band 1.135675, still inside the 3-line window). The alternation defect F4 fixes was
Arachne-only.

**[D]** But F4 is load-bearing at N = 1 on classic, in a different way: even-layer band =
`0.578595 − 0.107135 = 0.471460` against the `ext_w = 0.45` disappear-entirely cliff — margin
**0.021460 mm** (= `k_h`). With the pre-F4 0.3 mm notch the even-layer band was 0.278595 < 0.45,
so **N = 1 paint vanished completely on every even layer** under classic. F4 rescued that by
0.02 mm. §2c shows the cliff is still reachable on wide-outer-wall profiles.

### F1 — top/bottom claim inset by one `wall_stack = 0.878540` (`MultiMaterialSegmentation.cpp:1779-1781`, `:1829-1831`)

**[V]** The band clamp (`cut_segmented_layers`, `:2591`) and the projected top/bottom claim
(`:2598`) are **unioned** at merge (`:2602`); the top/bottom claim is not band-clamped.

**[D]** Whenever `band < wall_stack`, the union leaves the **base** region holding a closed ring
of width `wall_stack − band` on every sub-surface shell layer. Today that is N = 1 only:
`0.878540 − 0.578595 = 0.299945 mm`. Under classic that base ring is narrower than `ext_w = 0.45`,
so `offset_ex` returns empty at i = 0, `last` is cleared, no gaps are collected (gaps only start
at i ≥ 1) — **the ring prints nothing at all: a genuine 0.3 mm void ring** beneath every painted
top/bottom face at N = 1. Arachne would widen it into one `min_bead_width` bead instead.
(Checked the narrow-island escape at `:1346-1354`: `offset2_ex` is empty, but the ring's area
exceeds the `(ext_w + ext_min_spacing_smaller)·10 = 7.84 mm²` threshold for any real object, and
even the smaller-width path uses 0.402861 > 0.3 — still empty.)

The §3 floor sets `band(1) = wall_stack` exactly, closing this ring by construction. That is the
single strongest argument for it.

### Minor, both generators
**[V]** Both edges of a painted band are depth-0 / inset-0, hence `erExternalPerimeter`. If a
user sets `outer_wall_filament > 0` (default 0, `PrintConfig.cpp:4663`), **both** edges of the
painted band follow that filament rather than the painted one (`GCode.cpp:5929-5930`). Same on
Arachne; noted for completeness.

---

## 7. Recommended actions (classic-side only)

1. **Floor the band at `ext_perimeter_width + ext_perimeter_spacing` when
   `wall_generator == classic`** (§3). One line at `MultiMaterialSegmentation.cpp:2653`. Fixes
   the N = 1 +48 % over-extrusion, the N = 1 wide-outer-wall total-disappearance cliff, and the
   F1 void ring, and changes nothing for N ≥ 2 or for Arachne.
2. **Fix or document the `paint_infill_override = false` + classic trap** (§0): the option's own
   tooltip promises that only *sparse infill* loses the paint, but on classic it silently sends
   the band's middle gap-fill line — 37 % of the painted depth at N = 3 — to the base filament.
   Cleanest fix: route `erGapFill` inside a painted region to `wall_filament`
   (`GCode.cpp:5209-5221`), which would also settle the `LayerTools::extruder` disagreement.
3. **Add classic coverage to the test suite** (§4). Today zero `[paintdepth]` tests set
   `wall_generator`, and none assert on extrusions at all. The cheap high-value assertions are:
   a painted band on classic emits `2k` perimeter loops plus one gap fill whose widths sum to
   ≥ 95 % of the band; and at N = 1 no two same-depth loops sit closer than `ext_s`.
4. **Tell the user** (classic): prefer **millimetres mode**; `paint_depth_mm = 2.0` gives exactly
   2 mm, 96.6 % filled, at stock `wall_loops = 2`. Their "only 1–2 walls" reading is the preview
   counting loops — the paint really is the full band deep. Keep the band ≤ 2.46 mm at
   `wall_loops = 2`, and leave "Paint sparse infill" **on**.
