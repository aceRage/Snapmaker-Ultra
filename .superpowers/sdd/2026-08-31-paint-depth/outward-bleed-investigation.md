# Outward paint-bleed investigation (paint_depth, mm mode 4–6 mm)

Scope: the **outward-bleed symptom only** — painted colour appearing on the visible EXTERIOR
skin below/beyond where the user painted. Wall-count under-delivery in `walls` mode is a
sibling investigation and is deliberately untouched here.

Read-only. Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `cfe7fae1df`.
All line numbers are HEAD unless a commit is named.

Legend: **[V]** = verified by reading the code at the cited anchor. **[I]** = inferred
(reasoning from verified code, not directly observed in a slice or a test).

---

## 0. Executive summary

The exterior bleed is **mechanism (a): the full-width vertical descent**, amplified by the
**sub-wall-stack absorb added in `cfe7fae1df` (I1)**, which is what makes it fire on the
chamfered/filleted/organic geometry the user actually printed.

The **mm magnitude is not the cause of the exterior effect** and cannot be: the lateral band
never reaches the top/bottom claim at all. `cut_segmented_layers` runs at
`MultiMaterialSegmentation.cpp:2489`, *before* `segmentation_top_and_bottom_layers` is even
called at `:2496`, and it only ever touches `segmented_regions` (the side claims). **[V]**
The correlation the user reports is partly incidental and partly a *second, genuinely
mm-dependent* mechanism that is not the same bug (§2.3).

This is a **regression against both `3448111acd` and the pre-feature upstream baseline
`f1e9f78696`**, whose descent loops are byte-identical to each other and always inset the
claim by at least one wall stack. **[V]**

---

## 1. Q1 — Which mechanism paints the exterior below the painted facet?

### 1.1 The pipeline fact that decides the ranking **[V]**

`MultiMaterialSegmentation.cpp:2488-2500`:

```
2488  if ((segmentation_max_width > 0.f || segmentation_interlocking_depth > 0.f) && !segmentation_interlocking_beam) {
2489      cut_segmented_layers(input_expolygons, segmented_regions, scale_(segmentation_max_width), scale_(segmentation_interlocking_depth), ...);
2491  }
2495  if (include_top_and_bottom_layers == IncludeTopAndBottomLayers::Yes) {
2496      top_and_bottom_layers = segmentation_top_and_bottom_layers(print_object, input_expolygons, ...);
2498  }
2500  segmented_regions_merged = merge_segmented_layers(segmented_regions, std::move(top_and_bottom_layers), ...);
```

`segmentation_max_width` — the value `paint_depth_band_mm()` computes from `paint_depth_mm`
(`PaintDepth.cpp:7-21`) — has **exactly one use site in the whole file**, line 2489. **[V]**
(`grep -n segmentation_max_width` → 2277 (parameter), 2488 (gate), 2489 (call). Nothing else.)

So the lateral clamp applies to the **side** claims only. The top/bottom claim is computed
afterwards, unclamped, and then in `merge_segmented_layers`:

- `:2166` — `segmented_regions_trimmed = diff_ex(segmented_regions_trimmed, top_and_bottom_by_extruder[layer_idx])`
  → the clamped side claims are **cut back by** the top/bottom claim.
- `:2176` — `append(segmented_regions_merged[layer_idx][extruder_id], top_and_bottom_layers[extruder_id][layer_idx])`
  → the top/bottom claim is appended **at full, unclamped width**.
- `:2180` — the only subsequent cleanup is `offset2_ex(±SCALED_EPSILON)`, a nanometre-scale
  closing. It cannot pull a boundary back off the contour. **[V]**

**The top/bottom claim therefore overrides the side segmentation and is never lateral-clamped.**
Whatever width `segmentation_top_and_bottom_layers` hands back is what the exterior gets.

### 1.2 Ranked mechanisms

#### **(a) Full-width vertical descent — WINNER** **[V]**

`:1642-1643` computes `top_exposed_ex = exposed_surface_part(top_ex, input_expolygons, layer_idx + 1, ...)`.
`:1652-1656`, inside the descent loop:

```
1651   ExPolygons last = intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset));   // legacy eroded term
1652   if (! top_exposed_ex.empty()) {
1656       append(last, intersection_ex(top_exposed_ex, layer_slices_trimmed));              // FULL-WIDTH term
```

`layer_slices_trimmed` is the running `intersection_ex` of layer outlines from `layer_idx`
down to `last_idx` (`:1650`). It is **never eroded** — `offset` is applied only to the legacy
term at `:1651`. So the full-width term is bounded by a *layer outline*, i.e. by the
silhouette itself.

How this paints exterior the user never painted, concretely:

> **Flat top on a straight prism.** Nothing above the painted face, so
> `reference_layer_idx >= num_layers` and `exposed_surface_part` early-returns the whole patch
> with **no clearance test at all** (`:1331-1332`). Below the top, the cross-section is
> unchanged, so `layer_slices_trimmed == input_expolygons[layer_idx] == input_expolygons[last_idx]`.
> The claim is `top_ex ∩ (that cross-section)` — which, for a fully painted top face, **is the
> cross-section, right out to its contour**. Every one of the `top_shell_layers` below the top
> therefore has the painted colour owning the outermost perimeter band. The user painted one
> horizontal facet; the slicer paints a vertical ring of side wall beneath it.

Height of that ring: the loop bound is `last_idx > max(int(layer_idx) - stat.top_shell_layers, 0)`
(`:1646`), and since the vertical-depth fix wave `stat.top_shell_layers` is
`max(configured count, thickness-driven count)` via `effective_shell_layers_by_thickness`
(`:1219-1267`, called at `:1578-1581`). With the common `top_shell_layers = 5`,
`top_shell_thickness = 0.8 mm`:

| layer height | effective count | layers below top that bleed | exterior ring height |
|---|---|---|---|
| 0.20 mm | max(5, 4) = 5 | 4 | **0.80 mm** |
| 0.10 mm | max(5, 8) = 8 | 7 | **0.70 mm** |

**[I]** A 0.7–1.0 mm halo of the wrong colour ringing every painted flat-ish facet is exactly
"paint bleed on the exterior". The mirrored bottom loop (`:1704-1737`) does the same on the
underside of painted downward-facing facets.

This mechanism was **flagged unvalidated by its own author**,
`taper-bound-report.md:336-341`: *"The colour boundary now reaches the exterior wall on
sub-surface shell layers where the paint reaches the silhouette… it is a visible change on
every painted flat-topped object… **Not visually validated**."* The user has now validated it,
negatively.

#### **(a′) The `cfe7fae1df` sub-wall-stack absorb — CO-WINNER / amplifier** **[V]**

`:1673-1678` (top) and `:1725-1730` (bottom):

```
1673   const float wall_stack = stat.extrusion_spacing + stat.extrusion_width;
1674   ExPolygons base_rest = diff_ex(input_expolygons[last_idx], last);
1675   if (! base_rest.empty()) {
1676       append(last, diff_ex(base_rest, opening_ex(base_rest, 0.5f * wall_stack)));
```

`diff_ex(X, opening_ex(X, r))` is "the parts of X thinner than `2r`". With `r = 0.5·wall_stack`
this **absorbs into the painted claim any base remainder narrower than one wall stack**.

On a straight prism the base remainder is empty and this is inert. But on **any chamfer,
fillet, draft angle or organic taper below a painted flat top** — which is essentially every
real model — the base remainder at descent depth *k* is a thin annulus of radial width
`≈ k · layer_height / tan(slope)`:

| geometry | annulus at depth *k* | wall_stack | absorbed for |
|---|---|---|---|
| 45° chamfer, 0.1 mm layers, 0.45 mm wall | `k × 0.100 mm` | 0.878 mm | k ≤ 8 → **all 7 descent layers** |
| 45° chamfer, 0.2 mm layers, 0.45 mm wall | `k × 0.200 mm` | 0.857 mm | k ≤ 4 → **all 4 descent layers** |
| 20° draft, 0.1 mm layers | `k × 0.275 mm` | 0.878 mm | k ≤ 3 |

So on a chamfered or filleted top face the claim is **pushed onto the contour on every shell
layer**, even where the geometry would otherwise have left it clear. This is the mechanism the
reviewer predicted verbatim — `taper-bound-review.md:24` marks check 4 **"PARTIAL FAIL —
safe on vertical walls, reintroduces the sliver class on tapered tops (I1)"**, and
`:292-296` names *"Flat top + any chamfer / fillet / draft / organic taper below it"* as the
failing class. The I1 patch cured the *sliver* by painting the sliver — trading an unprintable
thin base ring for a visibly mis-coloured exterior ring.

Note the direction: **I1 made the exterior bleed strictly worse**, on precisely the geometry
class most common on organic multicolor models.

#### **(b) Lateral band in mm mode — REFUTED as a cause of exterior colour** **[V]**

Two independent reasons, both from code:

1. **It never reaches the top/bottom claim.** §1.1 — ordering at `:2489` vs `:2496`, and
   `segmentation_max_width` has no other use site.
2. **Even on the side claims it clamps the wrong way to explain this.** `cut_segmented_layers`
   at `:1175` is
   `diff_ex(ex_polygons, offset_ex(input_expolygons[layer_idx], -region_cut_width))` — it keeps
   the claim *within `cut_width` of the contour* and **discards the interior**. A larger band
   discards *less interior*; it cannot add exterior. The user's intuition in the task framing
   is correct.

#### **(c) Interlocking band — REFUTED** **[V]**

`:1164` `interlocking_cut_width = interlocking_depth > 0 ? max(cut_width - interlocking_depth, 0) : 0`,
applied at `:1169` on **even layers only**. It makes the side claim *narrower* on alternating
layers, inside `cut_segmented_layers` — same function, same side-claims-only scope as (b).
It can produce alternating-depth teeth at the *inner* boundary of a side claim. It cannot put
colour on an exterior perimeter that the side segmentation did not already own.

#### **(d) Other candidates considered and refuted** **[V]**

- `has_bounded_paint_depth()` → `interface_shells` forced on (`Print.hpp:495`,
  `PrintObject.cpp:1333`, `PerimeterGenerator.cpp:622` / `:2273`,
  `LayerRegion.cpp:227`). Real behaviour change, but **mode-gated, not magnitude-gated** —
  identical in `walls` and `millimeters` mode, so it cannot explain a symptom the user sees
  only at 4–6 mm.
- `paint_infill_override` (`PrintApply.cpp:1858-1863`) — gates whether sparse infill takes the
  painted filament. Interior only; no perimeter effect.
- The `merge_segmented_layers` dimple cleanup `:2180` — `±SCALED_EPSILON`, nanometre scale.

### 1.3 Final ranking

| rank | mechanism | anchor | verdict |
|---|---|---|---|
| **1** | Full-width vertical descent reaching the layer contour | `:1642`, `:1652-1656`; early return `:1331-1332` | **cause** |
| **1=** | Sub-wall-stack absorb pushing the claim onto the contour on tapered tops | `:1673-1678`, `:1725-1730` | **cause (amplifier; the organic-model trigger)** |
| 3 | Lateral mm band | `:1175`, `:2489` | refuted for exterior; see §2.3 for a separate real effect |
| 4 | Interlocking band | `:1164`, `:1169` | refuted |
| 5 | interface_shells / paint_infill_override | `PrintObject.cpp:1333` etc. | refuted (mode-gated, not magnitude) |

---

## 2. Q2 — Does the mm magnitude matter the way the user observed?

### 2.1 For the exterior bleed: **no path exists** **[V]**

`paint_depth_mm` → `paint_depth_band_mm` (`PaintDepth.cpp:13-14`, mm mode returns `float(mm)`
verbatim) → `max_width` (`:2544`) → `segmentation_max_width` → `cut_segmented_layers` (`:2489`)
→ `segmented_regions` only. The vertical claim's **width** comes from `top_ex ∩ layer outlines`
and its **depth** from `stat.top_shell_layers`, which is built solely from
`top_shell_layers` / `top_shell_thickness` (`:1578-1581`). **No term in either expression
reads `paint_depth_mm`.** Changing 4 mm → 6 mm cannot move the top/bottom claim by one micron.

### 2.2 Why the user nevertheless correlates it — the incidental half **[I]**

- The bleed ring is **constant** in size (0.7–1.0 mm, §1.2). What changes with the mm value is
  how much *legitimately* painted volume surrounds it. At a small band the painted colour is a
  thin skin and the bleed ring reads as a separate defect; at 4–6 mm the painted region is deep
  and the ring merges visually into one continuous mis-coloured area — "spreading".
- Users reach for `millimeters` + a large number precisely on organic models, which is exactly
  the chamfer/fillet geometry class where (a′) fires on *every* descent layer (§1.2 table).
  So the *population* of models sliced at 4–6 mm is enriched for the trigger.

### 2.3 …and a genuinely mm-dependent effect that is a *different* bug **[V] + [I]**

There is one real magnitude dependency, and it is worth separating from the main finding
because it may be part of what the user saw:

**[V]** At `:1175` the clamp is `diff_ex(claim, offset_ex(input_expolygons[layer_idx], -cut_width))`.
Where the local cross-section's half-thickness is **less than `cut_width`**, the inner offset is
**empty**, `diff_ex(claim, ∅) == claim`, and the clamp is a **complete no-op** on that layer.

**[I]** At `cut_width = 4–6 mm` that condition holds across most of a typical organic model
(anything under 8–12 mm thick locally) — so in mm mode at 4–6 mm the lateral clamp effectively
**turns itself off**. The unclamped Voronoi side partition (`extract_colored_segments`, `:2475`)
then survives in full, including the places where the painted colour's region wraps around a
thin fin, a rounded tip or a narrow neck and touches the **opposite** exterior face. In `walls`
mode (`cut_width ≈ 0.45–1.31 mm`, `PaintDepth.cpp:18`) that wrap-around is exactly what the
clamp removes.

This matches the user's phrasing "**outward** instead of **downward**" precisely: past a
certain mm value there is no more depth to add (the band already exceeds the wall), so
increasing it stops deepening and starts revealing lateral wrap-around. Marked **[I]** because
I did not trace `extract_colored_segments`' partition geometry; the no-op condition itself is
**[V]**.

**Recommendation:** treat this as a separate finding. It does not share a fix with (a)/(a′).

---

## 3. Q3 — Regression or pre-existing?

**Regression. Introduced by `65d17c964f`, worsened by `cfe7fae1df`.** **[V]**

Both `3448111acd` (pre-taper-bound) and `f1e9f78696` (pre-feature upstream merge-base) contain
the **identical** descent body — I diffed them; they match line for line:

```
offset -= (stat.extrusion_spacing + stat.extrusion_width);
layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);
ExPolygons last = opening_ex(intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset)), stat.small_region_threshold);
```
(`3448111acd`:1570-1578 / `f1e9f78696`:1388-1396, and the bottom mirrors at :1590-1598 / :1408-1416.)

`offset` is `-(spacing + width)` at the first descent step and grows by that much every step.
The claim was therefore **inset from the layer contour by at least one wall stack — ~0.86–0.88 mm —
on every sub-surface layer, unconditionally**. With an external perimeter of ~0.42–0.45 mm, the
exterior perimeter (and typically the first internal one) was guaranteed base-coloured. The
exterior bleed was **structurally impossible** before `65d17c964f`.

### 3.1 The geometry where it flips

The switch is `exposed_surface_part` (`:1321-1334`), whose test is
`diff_ex(patch, offset_ex(input_expolygons[reference_layer_idx], wall_stack))` — i.e. "is this
patch more than one wall stack clear of the neighbouring layer?" As a slope that is
`layer_height / tan(slope) ≥ wall_stack`:

| layer height | wall (outer) | wall_stack | slope cutoff from horizontal |
|---|---|---|---|
| 0.10 mm | 0.45 mm | 0.878 mm | **≤ 6.5°** |
| 0.20 mm | 0.45 mm | 0.857 mm | **≤ 13.1°** |
| 0.20 mm | 0.42 mm | 0.797 mm | **≤ 14.1°** |

- **Painted DOME / curved face (slope > cutoff over almost its whole area).** The projected
  band at each layer is the annulus between consecutive outlines, narrower than one wall stack,
  so it lies entirely inside `offset_ex(reference, wall_stack)` and `exposed_surface_part`
  returns **empty**. The `if (! top_exposed_ex.empty())` guard at `:1652` skips the entire new
  block. **Behaviour is byte-identical to `3448111acd` and to upstream — the erosion still
  prevents exterior contact, and full width was never granted.** So on a dome: **no new bleed,
  and no new depth.** (This is the same fact that answers Q5.)
- **Painted flat cap (slope ≤ cutoff), or any top face with nothing above it.** The early
  return at `:1331-1332` hands back the whole patch **with no clearance test whatsoever**, full
  width descends to the contour, and exterior contact is **guaranteed** wherever the painted
  patch reaches the silhouette.
- **The flip point is the apex of a dome.** Only the cap within ~6.5–14° of horizontal
  qualifies. On a pure dome that cap sits far inside the contour of the layers below (the dome
  widens fast), so the base annulus is thick, the absorb does not fire, and bleed is limited.
  On a **flat top with a chamfer/fillet rim** — a dome that has been truncated, i.e. most real
  models — the whole cap qualifies *and* the rim annulus is thin, so (a) and (a′) both fire.
  That is the worst case and it is the common case.

---

## 4. Q4 — Fix space

Shared quantity in all options: `wall_stack = stat.extrusion_spacing + stat.extrusion_width`,
already computed at `:1673` / `:1725`.

### Option A — clamp the full-width term one wall stack clear of the *descent layer's* contour **(recommended)**

Anchors: `:1656` (top), `:1720` (bottom).

```
// current
append(last, intersection_ex(top_exposed_ex, layer_slices_trimmed));
// proposed
append(last, intersection_ex(intersection_ex(top_exposed_ex, layer_slices_trimmed),
                             offset_ex(input_expolygons[last_idx], -wall_stack)));
```

- **Kills the bleed at its root.** The claim can never own an exterior perimeter on an inferred
  (sub-surface) layer. The surface layer itself is untouched — it is appended separately at
  `:1634` / `:1705` with zero margin, which is correct, because that *is* what the user painted.
- **Preserves the approved intent fully for interior features.** The inset is measured from the
  *layer contour*, not from the patch. A flat painted feature sitting more than one wall stack
  inside the silhouette (a raised boss, a flat shelf, a painted step on a shoulder) is not
  clipped at all and keeps its **entire** footprint at every shell layer — which was the whole
  point of `65d17c964f`.
- **Still strictly more generous than legacy.** Legacy inset grows as `k · wall_stack` with
  depth; Option A holds a **constant** `1 · wall_stack`. At depth 1 they coincide; at depth 5
  legacy is inset 4.4 mm and Option A 0.88 mm. So it keeps most of the taper-bound win.
- **Subsumes and obsoletes the I1 absorb.** With Option A, `base_rest` at `:1674` / `:1726` is a
  ring at least one wall stack wide by construction, so `opening_ex` leaves it intact, the
  `diff_ex` is empty and the absorb becomes **inert** — the sliver class `cfe7fae1df` was fixing
  can no longer form. The absorb blocks (`:1673-1678`, `:1725-1730`) can be deleted, or left as
  dead-but-harmless code. **One fix retires two defects.**
- Trade-off: a fully painted flat top on a plain box loses its painted side-wall ring below the
  top layer — reverting to upstream appearance there. This is the *only* regression against the
  approved intent, and it is confined to paint that reaches the silhouette.
- Cost: one extra `offset_ex` + `intersection_ex` per descent step, same order as `:1651`.

### Option B — restrict `exposed_surface_part` itself with a silhouette-clearance test

Anchor: `:1333`, adding a second `diff_ex` against the surface layer's own perimeter band.

- Cheaper: evaluated **once per painted layer** instead of once per descent step.
- **Has a real correctness hole**, and it is not hypothetical: clearance is tested against
  `input_expolygons[layer_idx]`, but the claim is bounded by `layer_slices_trimmed`, which for
  an object that **narrows downward** (an undercut, a waist, an overhang below a painted top)
  equals `input_expolygons[last_idx] ⊂ input_expolygons[layer_idx]`. A patch clear of the
  surface layer's contour can still land on a lower layer's contour. Option A tests at
  `last_idx` and is immune.
- Preserves interior intent identically to A. Prefer A unless the per-step cost measures badly.

### Option C — revert the I1 absorb only

Anchors: `:1673-1678`, `:1725-1730` (delete).

- Cheapest possible mitigation and it targets the exact geometry class (chamfer/fillet/organic
  taper) that makes the defect fire on real models, so it removes **most** of the visible bleed.
- Does **not** fix the plain flat-top-to-silhouette case (§1.2), which still reaches the
  contour via `:1656`.
- **Reintroduces the sub-wall-stack base ring** that `cfe7fae1df` was written to eliminate —
  `is_perimeter_compatible` (`Layer.cpp:184`) will not merge it away. Trades a visible colour
  defect back for a printability defect. Acceptable only as a stopgap; Option A gives both.
- Preserves interior intent (the absorb never affected interior features — their `base_rest` is
  thick).

### Option D — make it configurable

Anchor: `PrintConfig.hpp:903-905`, plus a gate at `:1652` / `:1719`.

- Defers a decision the user has already effectively made by reporting this as a bug.
- Adds permanent config surface for a defect, and the "on" setting is still wrong.
- Preserves interior intent trivially (both settings do). **Lowest preference.** If wanted at
  all, ship Option A as the default behaviour and expose the old full-width mode as the opt-in,
  not the reverse.

### Ranking

**A > C > B > D.** A is the only one that fixes both the flat-top and the tapered-top cases,
retires the I1 sliver risk, and keeps the approved full-width behaviour where the user asked
for it. C is the sensible one-line stopgap if a fix is needed before the next GUI validation.

---

## 5. Q5 — Interaction with "curved/top surfaces show no noticeable difference in depth"

**Every option above is exactly NEUTRAL on the curved-surface gap. [V]**

The reason is structural, not incidental. On a curved surface the slope exceeds the ~6.5–14°
cutoff (§3.1), `exposed_surface_part` returns empty, and the guard
`if (! top_exposed_ex.empty())` at `:1652` (bottom: `:1719`) skips the whole new block. Options
A, B and C all modify code **inside or feeding that guard**, which on curved geometry never
executes. Option D adds a second gate on the same dead path.

The two symptoms therefore share one root cause and sit on **opposite sides of the same
threshold**:

- Slope **above** the cutoff (curved, organic, most of a real model) → `exposed_surface_part`
  empty → taper bound is a **no-op** → the user sees **no extra depth**. This is
  `taper-bound-report.md:342-349` concern 2, and `taper-bound-review.md` check 5 concluded the
  lateral clamp does not supply the missing opacity either.
- Slope **below** the cutoff (flat caps, chamfered tops) → full width to the contour → the user
  sees **exterior bleed**.

Fixing the bleed does not and cannot make the curved gap worse, because the bleed fix only ever
*removes* area from a term that is already empty on curved geometry. But it also does not help
it. **The curved-surface depth gap remains open after any of these fixes and needs its own
approach** — and per the report's own analysis (`taper-bound-report.md:346-349`) closing it
requires either an unprintable sub-wall sliver or accepting downward colour smear on the
exterior, i.e. it is a geometry/physics limit, not an implementation oversight. Worth putting
back to the user as a separate decision rather than folding into this fix.

---

## 6. Suggested GUI validation for whichever fix lands

Per `taper-bound-review.md:298-303`: slice a painted flat-topped object **with a chamfered or
filleted top edge** — a plain cube cannot show the (a′) case and proves nothing. Inspect the
exterior wall on the 2nd–6th layers below the painted top. Expected after Option A: a clean
base-coloured exterior on every sub-surface layer, no hairline ring, no groove, and interior
flat features (a raised painted boss well inside the silhouette) still full width at full
shell depth.

Separately, to check §2.3: slice the same model at `paint_depth_mm = 1` and `= 6` and compare a
thin fin or a rounded tip for colour wrapping onto the opposite face.
