# Wall-count investigation — why `paint_depth_walls = 3` shows as 1–2 walls

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `cfe7fae1df`.
Read-only; no source edited. Scope: **wall-count only** (a sibling agent has the
millimeters-mode outward-bleed symptom).

Everything below marked **[V]** was verified by reading committed code (file:line given).
Everything marked **[D]** is arithmetic derived from that code (no build/run was performed —
no G-code or GUI evidence was produced by this investigation).

---

## Verdict in one paragraph

The band arithmetic is **not** the primary defect: at stock settings a `walls = 3` band does
fit 3 Arachne beads, with **0.083 mm** of downward margin (≈ 19 % of one line spacing). The
primary defect is **our own `mmu_segmented_region_interlocking_depth = 0.3 mm` default**,
which on every even-indexed layer narrows the band by 0.3 mm — **3.6× the available margin** —
and drops the painted region from 3 loops to 2. The result alternates **3 / 2 / 3 / 2 …** layer
by layer, which is exactly "clearly only 1–2 walls being used". Secondary: users on the
**classic** wall generator get only **2 loops** (plus gap fill) from the same band, always.

---

## 1. Where the band goes, and who lays the loops

**[V]** `paint_depth_band_mm(pdmWalls)` = `ext_perimeter_width + (walls−1)·perimeter_spacing`
(`src/libslic3r/PaintDepth.cpp:15-19`), maxed over all printing regions
(`src/libslic3r/MultiMaterialSegmentation.cpp:2540-2545`).

**[V]** It is applied as a lateral clamp in `cut_segmented_layers`
(`MultiMaterialSegmentation.cpp:1175`):
`diff_ex(painted, offset_ex(input_expolygons[layer_idx], -region_cut_width))` — i.e. the painted
colour survives only inside an **annulus of width `region_cut_width`** measured in from the
*layer* contour (holes included).

**[V]** The painted region is a *separate* `PrintRegion` whose `wall_filament` is the painted
extruder (`src/libslic3r/PrintApply.cpp:1088-1090`). `Layer::is_perimeter_compatible`
(`src/libslic3r/Layer.cpp:184`) compares `wall_filament` first, so the painted region is **never**
merged with its parent for perimeter generation — `Layer::make_perimeters` takes the
`layerms.size() == 1` branch (`Layer.cpp:257-260`) and **generates perimeters on the annulus
alone**. So "how many walls does the paint get" is literally "how many beads fit across a strip
of width `band`".

---

## 2. Question 1 — does a 1.307 mm band actually receive 3 loops?

### 2a. What Arachne is fed

**[V]** `PerimeterGenerator::process_arachne` (`PerimeterGenerator.cpp:2231-2256`):

```
last          = offset_ex(surface, apply_precise_outer_wall ? -(ext_w − ext_s)
                                                             : -(ext_w/2 − ext_s/2))
bead_width_0  = ext_perimeter_spacing        // NOT width
bead_width_x  = perimeter_spacing
inset_count   = wall_loops (+extra)
```

**[V]** `apply_precise_outer_wall = precise_outer_wall && wall_sequence == InnerOuter`, and both
are **on by default** (`PrintConfig.cpp:1222` → `precise_outer_wall = true`; `:1888` →
`wall_sequence = InnerOuter`). So by default the annulus is pre-shrunk by `(ext_w − ext_s)` on
**each** side ⇒ Arachne's strip thickness is

> `T = band − 2·(ext_w − ext_s) = band − 2·h·(1 − π/4)`   (**[D]**, `Flow.cpp:182-184`)

(with `precise_outer_wall` off it is `band − 1·h·(1−π/4)`.)

### 2b. The exact bead-count rule

**[V]** `BeadingStrategyFactory::makeStrategy` stacks Distributed → **Redistribute** → Widening
→ (OuterWallInset) → Limited (`BeadingStrategyFactory.cpp:33-56`).

**[V]** `RedistributeBeadingStrategy::getOptimalBeadCount` (`RedistributeBeadingStrategy.cpp:42-49`)
and `DistributedBeadingStrategy::getOptimalBeadCount` (`DistributedBeadingStrategy.cpp:92-98`)
give, for `T > 2·s_ext`:

```
count(T) = 2 + D(T − 2·s_ext)
D(x)     = n + (r ≥ thr(n)·s)         n = ⌊x/s⌋,  r = x − n·s
thr(n)   = n odd ? split_thr : add_thr
```

**[V]** `WallToolPaths.cpp:510-511` — and note both thresholds are computed from extrusion
**width**, recovered from the spacing that was passed in:

```
split_thr = clamp(2·min_bead_width / ext_width − 1, .01, .99)
add_thr   = clamp(   min_bead_width /     width    , .01, .99)
```

**[V]** `min_bead_width` default **85 %** of nozzle (`PrintConfig.cpp:6887/6897`) = 0.34 mm on a
0.4 nozzle; `min_feature_size` default 25 % = 0.1 mm (`:6852/6861`);
`wall_distribution_count` = 1 (`:6843/6850`).
**[V]** `OuterWallInsetBeadingStrategy` does **not** change the count
(`OuterWallInsetBeadingStrategy.cpp:29-32`) — it only nudges `toolpath_locations[0]`.

### 2c. The numbers (0.4 nozzle, outer = inner line width 0.45 mm, `min_bead_width` 0.34)

`k = h·(1−π/4)`; `s = 0.45 − k`; `add_thr = 0.7556`; `split_thr = 0.5111`.

| | **h = 0.1 mm** (the report's stated case) | **h = 0.2 mm** |
|---|---|---|
| `s = s_ext` | 0.428540 | 0.407080 |
| band(3) = `w + 2s` | **1.307080** | **1.264159** |
| Arachne `T` (precise on) | 1.264159 | 1.178319 |
| 3-bead window `[2s+add·s, 2s+(1+split)·s)` | **[1.180876, 1.504640)** | **[1.121731, 1.429310)** |
| **beads delivered** | **3** | **3** |
| **downward margin** | **0.0833 mm** (0.194·s) | **0.0566 mm** (0.139·s) |
| upward margin | 0.2405 mm | 0.2510 mm |

**[D] So: yes, 3 loops — but not "exactly fitting with zero margin", and not comfortable
either.** The general result is worth stating because it explains the fragility:

> `band(N) = w + (N−1)·s = N·s + (w − s)` ⇒ **`T` lands exactly on `N·s`, the *optimal*
> thickness for N beads**, i.e. dead-centre of the "ideal" but only `(1 − thr)·s` above the
> count-drop boundary. With **odd N** the boundary uses `add_thr` ⇒ margin `= (1 − 0.7556)·s
> ≈ 0.244·s`; with **even N** it uses `split_thr` ⇒ margin `≈ 0.489·s`. **N = 3 sits on the
> tight parity.** The default `precise_outer_wall` then eats a further `h·(1−π/4)` of it
> (0.021 mm at 0.1 mm layers, 0.043 mm at 0.2 mm layers — 26 % / 43 % of the margin).

Margin also collapses with narrower lines, because `add_thr = min_bead_width/width`:
**[D]** at 0.42 mm lines margin = 0.190·s; at 0.40 mm lines = 0.150·s; at 0.36 mm lines =
0.056·s (≈ 0.02 mm — effectively zero).

### 2d. Classic wall generator — much worse

**[V]** `wall_generator` defaults to Arachne (`PrintConfig.cpp:6802`), but classic is a common
profile choice, and **[D]** on the same 1.307 mm annulus `process_classic` yields **one** onion
iteration:

* `i = 0` (`PerimeterGenerator.cpp:1341-1359`, `detect_thin_wall` default false at `:6323`):
  `offsets = offset_ex(annulus, −ext_w/2)` ⇒ residual annulus width `band − ext_w = 2s = 0.857`.
  That single expolygon contributes **two** loops at depth 0 (its contour *and* its hole).
* `i = 1` (`:1376`, `last = offsets` at `:1466`): net step `ext_perimeter_spacing2 = ½(s_ext+s)
  = 0.4285` per side ⇒ `0.857 − 2×0.4285 = 0.000`; the actual `offset2_ex(−(distance +
  min_spacing/2 −1), +(min_spacing/2 −1))` with `min_spacing = 0.6·s` erodes 0.5571/side ⇒
  **empty** ⇒ `loop_number = 0`, break.

**Classic therefore prints 2 external-width loops + one gap-fill line for a "3 wall" band —
always, on every layer.** If the user is on classic, that alone is the whole symptom.

---

## 3. Question 2 — the interlocking notch: **CONFIRMED, it alternates 3 / 2**

**[V]** `cut_segmented_layers` (`MultiMaterialSegmentation.cpp:1164, 1169`):

```
interlocking_cut_width = interlocking_depth > 0 ? max(cut_width − interlocking_depth, 0) : 0
region_cut_width       = (layer_idx % 2 == 0 && interlocking_cut_width > 0)
                          ? interlocking_cut_width : cut_width
```

**[V]** `mmu_segmented_region_interlocking_depth` default is now **0.3 mm**
(`PrintConfig.cpp:3951/3970`, flipped from upstream's 0 by this feature's Stage-1 spec decision 3),
and **[V]** it is armed whenever `paint_depth_mode != pdmUnlimited`
(`MultiMaterialSegmentation.cpp:2554`) — i.e. **on by default, on every painted object**.

**[D]** Even layers (0, 2, 4, …), h = 0.1 mm case:

| | odd layers | **even layers** |
|---|---|---|
| band | 1.307080 | **1.007080** |
| Arachne `T` | 1.264159 | **0.964159** |
| `x = T − 2·s_ext` | 0.407080 | **0.107080** |
| needs `x ≥ add_thr·s` = | 0.323797 | 0.323797 → **fails by 0.2167 mm** |
| **beads** | **3** | **2** |

Same at h = 0.2 mm (x = 0.0642 vs 0.3076 required) ⇒ **2 beads**.

> **Maximum interlock depth that still leaves 3 loops** (= the whole downward margin):
> **0.083 mm** at 0.1 mm layers, **0.057 mm** at 0.2 mm layers (0.105 / 0.099 mm with
> `precise_outer_wall` off). The shipped default of **0.3 mm is 3.6×–5.3× too large**.

**This is self-inflicted and it matches the user's report exactly.** With `walls = 3` the
preview alternates 3-loop and 2-loop bands every other layer; the eye reads the thinner of the
two, i.e. "1–2 walls".

Two further consequences of the same default, **[D]**:

* **`walls = 1` + interlock is destructive.** Band 0.45 → even-layer band 0.15 →
  `T = 0.107` (h = 0.1): below `RedistributeBeadingStrategy`'s `0.5·s_ext = 0.214` ⇒ 0 beads;
  `WideningBeadingStrategy` (`print_thin_walls = true`, `WallToolPaths.hpp:18`) rescues it to
  **one bead widened to `min_bead_width` = 0.34 mm inside a 0.107 mm gap** — gross local
  over-extrusion. At h = 0.2, `T = 0.064 < min_feature_size 0.1` ⇒ **0 beads: the paint
  disappears entirely on every even layer.**
* The interlock cannot be "budgeted for" by simply widening the band: 0.3 mm ≈ 0.7·s, while the
  whole N→N+1 count window is only ≈ 1·s wide, so no single band value yields exactly N on both
  parities. See §5.

---

## 4. Question 3 — other losses, ranked by magnitude

Against the 0.083 mm (h = 0.1) / 0.057 mm (h = 0.2) budget:

| # | Loss | Magnitude | Verdict |
|---|---|---|---|
| 1 | **Interlock notch** (`:1169`) | **0.300 mm** | **[V]/[D]** 3.6–5.3× the budget. Costs exactly one loop on even layers. *The cause.* |
| 2 | **`precise_outer_wall` pre-inset** (`PerimeterGenerator.cpp:2232`) | 0.021 mm (h .1) / **0.043 mm** (h .2) | **[V]/[D]** Structural, always on by default; eats 26 %/43 % of the budget. Not fatal alone, but it is why the h = 0.2 margin is only 0.057 mm. |
| 3 | **Hard cap `2 × wall_loops` beads** (`WallToolPaths.cpp:514`, `LimitedBeadingStrategy.cpp:41-64,115-127`) | not a shave — a **ceiling** | **[V]** `max_bead_count = 2·inset_count`, `inset_count = wall_loops`. With the default `wall_loops = 2` (`PrintConfig.cpp:4671`) **no more than 4 painted loops can ever be produced**, whatever `paint_depth_walls` says. Excess thickness goes to `left_over` (infill). |
| 4 | `simplify_p(resolution)` on the region before Arachne (`:2231`) | ≤ 0.010 mm (`resolution` default 0.01, `PrintConfig.cpp:4960`); ≤ 0.002 with arc fitting | **[D]** ≤ 12 % of budget, local only. |
| 5 | Clipper offset arc/miter approximation on curved contours | ~0.002–0.005 mm | **[D]** noise. |
| 6 | `closing_ex(scaled(10·EPSILON))` in `apply_mm_segmentation`; `offset2_ex(+SCALED_EPSILON, −SCALED_EPSILON)` in `merge_segmented_layers` (`:2179`); `ClipperSafetyOffset` | 0.001 / 0.0001 / 1e-5 mm, and all are **closings** (dilate-then-erode) | **[V]** cannot narrow the band. |
| 7 | `opening_ex(top_ex, small_region_threshold)` (`:1618`, `:1690`) | radius **0.1125 mm** (= ¼·outer width, `:1590-1595`, gap fill on) | **[V]** applies to the **top/bottom projected claims only**, never to the lateral band. Deletes top strips narrower than 0.225 mm — see §6. |
| 8 | `filter_out_small_polygons`, #7104, 0.1 mm² (`:1467`) | area, not width | **[V]** `top_raw`/`bottom_raw` only; a 1.31 mm-wide band would need a perimeter < 0.08 mm to be dropped. Irrelevant except for specks. |
| 9 | Our I1 fix `diff_ex(base_rest, opening_ex(base_rest, 0.5·wall_stack))` (`:1676`, `:1728`) | **additive** | **[V]** it is `append(last, …)` — it *adds* area to the claim; it cannot cost a loop. |
| 10 | Arachne `min_feature_size` / `WideningBeadingStrategy` (`WideningBeadingStrategy.cpp:57-65`) | 0.1 mm | **[V]** only engages below 1 bead — reachable only via §3's `walls = 1` + interlock case. |

Nothing in 4–10 can, alone or summed, cost a loop at the shipped 0.45 mm line width. **Only
item 1 can, and it always does.**

---

## 5. Question 4 — a formula that reliably delivers N *full* loops

### The robustness condition

**[D]** For the strip to hold N beads we need `T ∈ [2·s_ext + ((N−3)+thr_{N−3})·s,
2·s_ext + ((N−2)+thr_{N−2})·s)`, and `T = band − 2·(w_ext − s_ext)` under default
`precise_outer_wall`. The current band puts `T` at the window's *lower-middle* for odd N.

### Proposed band

```
band(N) = N · perimeter_spacing                       // N full bead pitches
        + 2 · (ext_perimeter_width − ext_perimeter_spacing)   // undo the Arachne pre-inset (worst case)
        + 0.25 · perimeter_spacing                    // count-window margin
```

(equivalently `N·s + 2·h·(1−π/4) + 0.25·s`; the `2×` term is safe even when
`precise_outer_wall` is off — it just widens the band by one `h·(1−π/4)`.)

**[D]** h = 0.1 mm, 0.45 lines ⇒ `band(N) = 0.428540·N + 0.150055`:

| N | current band | **proposed band** | proposed `T` | `x = T − 2s` | 3-window check | beads | down-margin |
|---|---|---|---|---|---|---|---|
| 1 | 0.450000 | **0.578595** | 0.535675 | — (≤ 2·s_ext ⇒ Redistribute) | `0.5357 ≤ 0.6476` | **1** | fine |
| **3** | 1.307080 | **1.435675** | 1.392754 | 0.535675 | `[0.323797, 0.647560)` | **3** | **0.2119 mm** (2.5× today) |
| 6 | 2.592699 | **2.721294** | 2.678374 | 1.821294 | n = 4, r = 0.107 < 0.324 ⇒ D = 4 | **6*** | 0.3167 mm |

\* subject to the `2 × wall_loops` ceiling (§4 item 3): N = 6 needs `wall_loops ≥ 3`.

### The interlock must be capped too — widening the band alone cannot fix it

**[D]** With the proposed band *and* the 0.3 mm notch still applied: even-layer `T = 1.0928`,
`x = 0.2357 < 0.3238` ⇒ **still 2 beads**. And *adding* 0.3 mm to the band to compensate pushes
the odd layers to `x = 0.8357 ≥ 0.6476` ⇒ **4 beads**, i.e. it converts 3/2 alternation into
4/3 alternation. Because 0.3 mm ≈ 0.70·s while the whole count window is ≈ 1.0·s wide, **no
single band value yields exactly N on both parities.** The fix must therefore be one of:

1. **Revert `mmu_segmented_region_interlocking_depth` to upstream's `0`** (simplest, and restores
   the "N walls means N walls" contract), or
2. **Clamp the effective notch** to the band's own slack, e.g.
   `interlock_eff = min(depth, 0.25·perimeter_spacing)` ≈ 0.107 mm at 0.45/0.1 — still a real
   mechanical tooth, but sub-loop, or
3. keep 0.3 mm and **document** that walls-mode delivers `N` on odd layers and `N−1` on even.

Recommendation: (1) or (2) together with the proposed band.

### What N matches the user's "about 3 walls, 2 mm"

**[D]** The two halves of their calibration are not consistent with any wall-based formula
(3 loops of 0.45 mm line ≈ 1.31–1.44 mm, not 2 mm). Taking **2 mm as the real target**:

| target | current formula | proposed formula |
|---|---|---|
| exact 2.000 mm | `N = 1 + (2.0−0.45)/0.428540 = ` **4.62** | `N = (2.0−0.150055)/0.428540 = ` **4.32** |
| nearest N below | N = 4 → 1.7356 mm | N = 4 → 1.8642 mm |
| nearest N at/above | **N = 5 → 2.1642 mm** | **N = 5 → 2.2928 mm** |

**So: `paint_depth_walls = 5`** (≈ 2.16 mm today) is what their 2 mm asks for — **and it requires
`wall_loops ≥ 3`**, because the `2 × wall_loops` bead ceiling caps a 2-wall profile at 4 painted
loops. Their "3 walls" number is best read as "I need to *see* three loops", which today's N = 3
delivers on odd layers only.

---

## 6. Question 5 — "no noticeable difference in depth on curved / tops"

**Consistent with `taper-bound-review.md` check 5.** That check derives the lateral band's normal
thickness on a slope θ as `t = band·sin θ`, with the vertical (top/bottom shell) path taking over
only below `atan(h/(w+s)) = 6.49°`, leaving a genuine opacity gap from **6.5° to ~27°**.

Two mechanisms I can add, both **[V]** in code:

1. **Below ~6.5°, `paint_depth_walls` is provably inert.** `segmentation_top_and_bottom_layers`
   runs **after** `cut_segmented_layers` and its output is merged in un-clamped
   (`MultiMaterialSegmentation.cpp:2488-2500` — cut at `:2489`, top/bottom computed at `:2496`,
   merged at `:2500`; `merge_segmented_layers` only *diffs against other colours* and closes with
   `±SCALED_EPSILON`). So on a near-flat top the painted depth is set entirely by
   `top_shell_layers` / `top_shell_thickness`, and **changing `paint_depth_walls` changes nothing
   there** — precisely the user's observation.
2. **Above ~24° the top-surface claim is deleted, not merely thinned.**
   `top_ex = opening_ex(top_ex, small_region_threshold)` (`:1618`, bottom twin `:1690`) with
   `small_region_threshold = 0.25 × outer_wall_line_width = 0.1125 mm`
   (`:1590-1595`, gap-fill-enabled branch, `gap_infill_speed` default 30 > 0) removes any strip
   narrower than 0.225 mm. A slope θ produces a per-layer top strip of width `h/tan θ`, so the
   claim vanishes for `θ > atan(h/0.225)` = **23.96° at h = 0.1 mm** (41.6° at h = 0.2 mm). Between
   ~24° and vertical, therefore, the *only* contributor is the lateral band — which is exactly the
   band the interlock notch is halving on alternate layers. Curved shoulders in that range get the
   worst of both defects at once.

One thing that is **not** a factor by default: the `has_bounded_paint_depth` hook added to
`split_top_surfaces` / `process_arachne` (`PerimeterGenerator.cpp:622`, `:2273`) only executes when
`only_one_wall_top` is enabled, and that defaults to **false** (`PrintConfig.cpp:1228`). If the user
*has* turned "only one wall on top" on, that hook will additionally reduce painted regions on domed
tops to a single loop — worth asking.

---

## 7. Recommended next actions (in order)

1. **Set `mmu_segmented_region_interlocking_depth`'s default back to 0** (or clamp the effective
   notch to `0.25·perimeter_spacing`). Single-line change; removes the 3/2 alternation.
2. **Adopt the §5 band** `N·s + 2(w_ext − s_ext) + 0.25·s`. Restores ≥ 0.21 mm of margin at N = 3
   and makes the margin parity-independent.
3. **Warn/clamp when `paint_depth_walls > 2 × wall_loops`** — currently silently capped by
   `LimitedBeadingStrategy`.
4. **Ask the user for `wall_generator`.** If it is `classic`, §2d is a separate and larger defect
   (2 loops for any band ≥ 3 walls) and needs its own fix.
5. Tell the user: for their 2 mm target use `paint_depth_walls = 5` with `wall_loops ≥ 3`, and that
   on tops flatter than ~6.5° the control that matters is `top_shell_layers`, not paint depth.
