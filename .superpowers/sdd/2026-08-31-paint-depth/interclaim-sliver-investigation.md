# Inter-claim base sliver investigation (round 3 GUI findings)

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `10559ee391`. Read-only.
Every claim below is tagged **[V]** verified by reading the source at the cited line, or **[I]**
inferred (reasoning stated, no build/slice run — no test binary was rebuilt and no fixture with
two painted colours exists in the suite today).

---

## 0. Executive answer

**The leading hypothesis is CONFIRMED as to the outcome and REFUTED as to the mechanism.**

There *are* base-coloured slivers trapped between adjacent painted claims, they *do* print in the
base (yellow) `wall_filament`, and Wave B *did* multiply them. But they are **not** produced by
"a claim-to-claim thin base survives F1's contour inset". F1's inset is measured from the layer
contour and is identical for every colour, so it cannot open a gap between two claims. The actual
generator is upstream and much simpler:

> Every colour's top/bottom claim is passed through its **own private
> `opening_ex(·, small_region_threshold)`** — once on the surface patch
> (`MultiMaterialSegmentation.cpp:1946`) and once per descent step (`:2079` top, `:2146` bottom).
> That filter **deletes any part of that colour's claim locally narrower than
> `2 * small_region_threshold` = 0.225 mm** at stock flows. Nothing anywhere hands the deleted
> area to the neighbouring colour: the claims are never re-unioned or re-partitioned across
> colours, so the deleted strip falls through to the base residue
> (`PrintObjectSlice.cpp:4548-4589`, `mine`), and base gap fill resolves to the base region's
> `wall_filament` (`PrintRegion.cpp:41-50`, Wave A item 9) — the yellow body colour.

Wave B did not add this operator. Wave B changed **where and how often it can surface as base**:
from "the painted surface layer only, and only inside the lateral band" to "up to 15 stacked
layers, reaching up to `15·r` past the lateral band". That is the amplification.

---

## 1. Mechanism, traced end to end

### 1.1 The mesh partition is exact — no sliver there **[V]**

`get_facets_strict()` (`TriangleSelector.cpp:1478-1518`) walks the subdivided triangle tree and
emits leaf triangles whose state matches, **T-joint-split** via `get_facets_split_by_tjoints()`
(`:1520`). The union over all states is the whole watertight mesh, so
`top_raw[c]` / `bottom_raw[c]` (`MultiMaterialSegmentation.cpp:1648-1691`) are an exact partition
of the slab projection. Two adjacent painted colours abut with zero gap at this stage.

### 1.2 The lateral (Voronoi) partition is also exact **[V]**

`extract_colored_segments()` (`:413-482`) builds each colour's cell from shared graph arcs, so the
per-colour cells tile `input_expolygons[layer]`. `cut_segmented_layers()` (`:1271-1329`) then
clamps **every** extruder index, index 0 included (`:1321-1323`), by the *same* `keep_core`, so the
painted bands still abut each other exactly and the interior stays base. **The lateral path
contributes no claim-to-claim sliver at all.**

### 1.3 The top/bottom path is where the partition is broken **[V]**

Three per-colour, independently-applied filters, all in `segmentation_top_and_bottom_layers()`:

| Site | Operation | Effect at stock flows |
|---|---|---|
| `:1706-1707` | `remove_small(raw, sqr(scale_(0.1)))` | drops whole polygons under **0.01 mm²** (the comment says 0.1 mm²; it is the square of 0.1 mm) |
| `:1946` (top) / `:2099` (bottom) | `opening_ex(top_ex, small_region_threshold)` | deletes any part of the **surface patch** narrower than **0.225 mm** |
| `:2079` (top) / `:2146` (bottom) | `opening_ex(last, small_region_threshold)` | deletes any part of **each descent step's deposited ring** narrower than 0.225 mm |

`small_region_threshold` is computed per colour at `:1851-1856`:

```
gap_infill_speed > 0 : 0.5 * outer_wall_line_width            = 0.225
then                 : * 0.5                                  = 0.1125 mm  (scaled)
```
so the kill width is `2 * 0.1125 = 0.225 mm` — which the code's own comment already states
(`:1538-1540`, "a ring narrower than 2*threshold = 0.225mm ... cannot survive it").

**Important qualifier [V]:** `opening()` uses `DefaultJoinType = jtMiter` with
`DefaultMiterLimit = 3` (`ClipperUtils.hpp:19,27`; `ClipperUtils.cpp:601-611`). Miter joins restore
a convex corner exactly, so this is **not** a disk opening and it does **not** round every corner.
Its two real effects are: (a) annihilate anything locally thinner than 0.225 mm, and (b) truncate
corners sharper than the miter limit, i.e. half-angle α with `1/sin α > 3` ⇒ α < 19.47°, interior
angle < 38.9°. The dominant term is (a).

### 1.4 Nothing re-unions the claims — the deleted strip becomes base **[V]**

- `segmentation_top_and_bottom_layers` `:2210-2231`: the accumulation loop resolves *overlaps*
  (colour 1 beats colour 2 …, then base is diffed by all painted at `:2231`). It never looks for
  *gaps*.
- `merge_segmented_layers` `:2646-2674`: `merged[L][c] = (lateral_c \ ∪_k T_k) ∪ T_c`. Also
  overlap-only.
- `apply_mm_segmentation` `PrintObjectSlice.cpp:4520-4533` takes each painted claim as `stolen`;
  `:4548-4558` computes the base residue `mine = parent.slices \ ∪(foreign claims)`; `:4584-4585`
  filters it with `opening(union_ex(mine), 5*EPSILON, 5*EPSILON)` — **0.0005 mm**, four orders of
  magnitude too small to remove a 0.2 mm sliver.

So a 0.05–0.225 mm strip that one colour's private opening deleted arrives in the base region.

### 1.5 …and prints in the base filament **[V]**

- **Classic**: the base sliver is its own `PrintRegion`; `process_classic` collects `gaps` only in
  the `i > 0` (internal-perimeter) branch (`PerimeterGenerator.cpp:1398-1405`), emits it via
  `variable_width(polylines, erGapFill, solid_infill_flow, …)` (`:1688`), and
  `fill_filament_source()` routes `erGapFill` to `FillFilamentSource::Wall`
  (`PrintRegion.cpp:41-50`) ⇒ **base `wall_filament`**. `filter_out_gap_fill` defaults to 0
  (`PrintConfig.cpp:3193`), so nothing is filtered.
- **Arachne**: `process_arachne` (`:2176`) never calls gap fill. Instead
  `WideningBeadingStrategy::compute` (`Arachne/BeadingStrategy/WideningBeadingStrategy.cpp:27-42`)
  turns **any** thickness ≥ `min_feature_size` into a single bead of
  `max(thickness, min_output_width)`. Defaults: `min_feature_size = 25 % × 0.4 = 0.1 mm`,
  `min_bead_width = 85 % × 0.4 = 0.34 mm` (`PrintConfig.cpp:6924, 6960`).

**This is why Arachne is worse than Classic [V, on the code; I, on the GUI attribution]:** a
0.15 mm base sliver becomes a **0.34 mm yellow bead (127 % over-extrusion)** under Arachne, while
under Classic the same region produces `offsets.empty() && offsets_with_smaller_width.empty()` at
`i == 0`, hits `last.clear(); break;` (`PerimeterGenerator.cpp:1407-1411`) and emits **nothing at
all**. Classic only starts producing yellow gap fill once the sliver is wide enough to admit a
first (possibly squished, `SMALLER_EXT_INSET_OVERLAP_TOLERANCE = 0.22`, `:27`) external loop.
Exactly the reported "Arachne: odd scatter / Classic: same class, less of it".

### 1.6 Where on the part it becomes visible **[I, arithmetic from verified constants]**

A base hole in the top/bottom claim only *surfaces* if the lateral band does not already cover it.
The lateral band reaches `D = 1.435675 mm` from the contour; the descent's F1 inset starts at
`wall_stack = 0.878540 mm`, so the two overlap by 0.557 mm (this is exactly what the
`D >= wall_stack` gate at `:1899-1900` guarantees). Therefore a hole is only visible **more than
1.4357 mm inside the contour**.

On a slope θ the per-layer contour run is `r = layer_height / tan θ`; the descent's step-m ring
sits at lateral inset `[m·r, (m+1)·r]`. Holes surface once `m·r > 1.4357`:

| θ | r (0.1 mm layers) | first exposed descent step | notes |
|---|---|---|---|
| 10° | 0.567 mm | m ≥ 3 | well inside the 14-step descent |
| 15° | 0.373 mm | m ≥ 4 | |
| 20° | 0.275 mm | m ≥ 6 | |
| 24° | 0.225 mm | m ≥ 7 | at exactly this slope every ring is killed by the opening (`:1540`) |
| >24° | < 0.225 mm | — | the whole full-width term is annihilated; claim = lateral band only |
| 45° | 0.100 mm | m ≥ 15 | beyond the 14-step descent — not reached |

So the defect lives in the **shallow band 0° < θ < 24°** — precisely the band Wave B's extension
was written to reach — which on a face model is the cheeks, forehead, brow ridge and lip
platform: exactly where the user reports it.

---

## 2. Numbers

Stock defaults: `paint_depth_mode = walls` (`PrintConfig.cpp:3914`), `paint_depth_walls = 3`
(`:3945`), outer/inner wall line width 0.45 mm, 0.4 mm nozzle, 0.1 mm layers.

```
perimeter_spacing  s   = 0.45 - 0.1*(1 - pi/4)            = 0.428540 mm
wall_stack         W   = ext_w + ext_s                    = 0.878540 mm
band               D   = 3s + 2(ext_w - ext_s) + 0.25s    = 1.435675 mm   (PaintDepth.cpp:9-29)
small_region_thr.  rho = 0.5 * 0.5 * 0.45                 = 0.112500 mm   (:1851-1856)
opening kill width 2rho                                   = 0.225000 mm
min_claim_width        = max ext_perimeter_width          = 0.450000 mm   (:3047, :3069)
interlocking notch     = min(0.1, 0.25s = 0.107135)       = 0.100000 mm   (PaintDepth.cpp:51-64)
Arachne min_feature_size / min_bead_width                 = 0.10 / 0.34 mm
```

### Sliver width **[V for the bound, I for the distribution]**

**Base slivers between two adjacent painted claims are bounded above by 0.225 mm.** A strip wider
than that survives the opening in *both* colours and no gap is created; a strip narrower than that
is deleted from whichever colour owns it. So the population is `(0, 0.225] mm`, plus corner
truncations at paint-boundary corners with interior angle < 38.9°.

Context for that number: **half a bead** (0.45 mm), **a quarter of a wall stack** (0.879 mm),
**2.25× Arachne's `min_feature_size`** — i.e. always printed by Arachne, always as an
over-extruded 0.34 mm bead.

**Config sensitivity worth flagging [V]:** with `gap_infill_speed == 0` the threshold branch at
`:1851-1855` takes the other arm — `0.5*(0.45 + 0.7*0.42854) = 0.375 mm`, kill width **0.75 mm**.
Turning gap fill off therefore **triples** the sliver width, into the range where Classic *will*
print it too.

### Layer counts

`effective_shell_layers_by_thickness(layers, j, top, 1, 1.435675)` at 0.1 mm layers breaks when
`0.1k >= 1.4357 - EPSILON` ⇒ **k = 15** (`:1367-1409`, `:1903-1910`). The descent loop bound
`last_idx > max(layer_idx - 15, 0)` (`:2002`) therefore deposits on **14 sub-surface layers**.

| revision | full-width term | descent bound | layers a per-colour dropout can surface on, curved face |
|---|---|---|---|
| `f1e9f78696` (pre-feature) | none — only `intersection_ex(top_ex, offset_ex(layer_slices_trimmed, -k·W))` (`f1e9f78696:1393`) | `top_shell_layers` | **1** (surface layer only): the growing `k·W` erosion is empty at k=1 for any r < W/(…) — i.e. any slope steeper than ≈6.5° |
| `ef9c20d90f` (pre-Wave-B) | gated by `exposed_surface_part()` (`ef9c20d90f:1771`), which returns EMPTY for every slope steeper than `atan(0.1/0.87854) = 6.49°` | `top_shell_layers` | **1** — identical in practice on organic geometry |
| `10559ee391` (HEAD) | unconditional, `top_exposed_ex = top_ex` (`:1983-1984`), inset only a *constant* `W` from the contour (`:2076`) | `top_descent_layers = 15` | **15** (surface + 14), and up to 14 different surface layers feed any one layer |

Flat painted cap (the other regime): 6 → 15 layers, the 2.5× the in-code comment at `:2607`
already records.

**Amplification factor ≈ 15× on sloped geometry, 2.5× on flat caps.** And a second, qualitative
change matters as much: pre-Wave-B the legacy erosion grew by `k·W` each step, so any residue
migrated inward layer by layer and washed out; F1's inset is **constant**, so the same crumb now
lands at the **same XY on all 15 layers** — isolated specks became **vertical yellow streaks**.

---

## 3. Regression or amplification?

**Amplification of an upstream defect, with one genuinely new exposure.** Stated honestly:

1. **Not ours [V].** The sliver-producing operators are byte-identical to the pre-feature merge
   base: `f1e9f78696:1383` ≡ `HEAD:1946`, and `f1e9f78696:1393`'s `opening_ex(…, small_region_
   threshold)` ≡ `HEAD:2079`. Upstream PrusaSlicer/BBS has shipped this since #7104. Wave B's
   review already recorded the residual class ("claim-to-claim … interior, user-painting-driven,
   unmitigated on the surface layer in upstream too", `bleed-and-walls-fixwave-review.md:338`) —
   that note was right about the class and wrong about the cause (it attributed it to `base_rest`
   thinning between two claims; the real cause is the per-colour opening).
2. **Ours, by multiplication [V].** 1 → 15 layers, non-migrating instead of migrating.
3. **Ours, genuinely new [V].** F1's constant inset lets the top/bottom claim reach **past the
   lateral band** (beyond 1.4357 mm from the contour) on a slope. Pre-feature it never did: the
   growing `k·W` erosion kept the claim strictly inside the band's shadow, so *any* hole in the
   top/bottom claim was silently backfilled by the lateral claim. The holes existed; they were
   invisible. Wave B uncovered them. This is the part of the defect that is fairly ours.

---

## 4. Symptom 3 (erratic square infill) — a **different** cause

Plainly: **it is not the sliver fragmentation, and it is not `paint_infill_override`.**

### Ruled out **[V]**

- **Pattern de-phasing from fragmentation.** Every filler is anchored to the *object* bounding box
  (`Fill/Fill.cpp:1230` `const BoundingBox bbox = this->object()->bounding_box();`, then
  `f->set_bounding_box(bbox)` at `:1243` and `:1433`; also `:1606`, `:1618`). Splitting a surface
  into many pieces does **not** shift the pattern between pieces.
- **`paint_infill_override`.** It only decides `sparse_infill_filament` for the painted region
  (`PrintApply.cpp:1858-1862`); it has no effect on pattern or geometry.

### The actual cause **[V]**

`PrintObject::has_bounded_paint_depth()` (`Print.hpp:514`) is true for every bounded-depth painted
object, and it **forces `interface_shells` on**:

- `PrintObject.cpp:1333` — `detect_surfaces_type()`:
  `interface_shells = !spiral_mode && (m_config.interface_shells.value || has_bounded_paint_depth())`.
  With it on, `upper_slices` / `lower_slices` are taken **per region** (`:1392`, `:1409`) instead
  of from the whole layer.
- `PrintObject.cpp:1766` — `discover_vertical_shells()`:
  `top_bottom_surfaces_all_regions = num_printing_regions() > 1 && !(interface_shells || has_bounded_paint_depth())`
  ⇒ **false**, so vertical shell thickness is computed per region too.

Consequence: **every layer at which a painted claim's own footprint differs from the layer
above/below becomes `stTop`/`stBottom` inside that painted region**, hence internal solid. On a
curved face the claim's footprint changes on *every* layer, and Wave B multiplied the number of
layers on which a claim exists at all (1 → 15). The result is a dense checkerboard of small
internal-solid patches inside the pink region.

Then `detect_narrow_internal_solid_infill` (default **true**, `PrintConfig.cpp:7013`) reroutes
narrow internal-solid patches from a straight-line pattern to **`ipConcentricInternal`**
(`Fill/Fill.cpp:1126-1159`, via `split_solid_surface()` at `:607-622` which only handles
`ipRectilinear`/`ipMonotonic`/`ipMonotonicLine`/`ipAlignedRectilinear`). Nested concentric loops
inside many small fragments is exactly a **"maze-like / erratic square"** read. The Orca
degenerate-sliver drop right after (`Fill/Fill.cpp:1163-1189`, patches under 3 spacings in *both*
dimensions) removes the smallest ones but keeps every long thin one.

**Secondary contributor [V]:** the interlocking notch alternates the lateral band by 0.1 mm every
other layer (`MultiMaterialSegmentation.cpp:1293`, `:1298`). Under forced `interface_shells` that
0.1 mm alternation is itself a per-layer region-boundary change ⇒ a 0.1 mm solid ring on alternate
layers ⇒ more narrow internal solid ⇒ more concentric fill.

**So: same *family* (per-layer footprint churn in the painted claim, multiplied by Wave B), but a
different mechanism and a different fix.** The absorb (§5) reduces churn but does not remove the
dominant driver, which is the claim's *legitimate* footprint change on a curved surface.

---

## 5. Fix options, ranked

### Option 1 — **Interior inter-claim absorb at the end of `merge_segmented_layers`** ★ recommended

**Site.** `MultiMaterialSegmentation.cpp:2646-2674` — a third stage after the existing merge
`parallel_for` (or folded into its tail). This is the *only* place where all of (a) every colour's
final lateral+top/bottom claim, and (b) the layer contour, are simultaneously in hand.

**Plumbing.** Add three parameters to `merge_segmented_layers`, all already live at the call site
`:2996`: `const std::vector<ExPolygons> &input_expolygons`, `float min_claim_width`
(= `segmentation_min_claim_width`, `:2769`/`:2982`), and the bounded-mode gate
(`segmentation_normal_depth > 0.f`, `:2770`/`:3151`). `wall_stack` is derivable the same way
`:3060-3066` derives it, or plumbed as a fourth float.

**Rule** (per layer, only when bounded):

```
painted   = union_ex( ∪_{c>=1} merged[L][c] )
base_area = diff_ex( input_expolygons[L], painted )
interior  = offset_ex( input_expolygons[L], -wall_stack )     // F1 guard

for each ExPolygon P in base_area:
    if (! opening_ex({P}, t).empty())      continue;   // P has a printable core -> genuine base
    if (! diff_ex({P}, interior).empty())  continue;   // P touches the contour band -> F1 owns it
    // P is a fully-interior, fully-thin base island bounded by painted claims
    c* = argmax_{c>=1} area( intersection_ex( offset_ex({P}, eps), merged[L][c] ) )
    if (no painted neighbour) continue;                // never orphan it
    merged[L][c*] += P
```

**Threshold `t = min_claim_width / 2 = 0.225 mm`.** Chosen deliberately: it is *exactly*
`2 * small_region_threshold`, so the absorb reclaims precisely what the per-colour thin-projection
filter deleted — no more, no less. `min_claim_width` is already the codebase's "narrowest painted
claim that may be emitted" quantity (Wave A / C-1, `:1211-1216`), plumbed and unit-tested.
(Alternative `t = wall_stack/2 = 0.439 mm` absorbs up to a 0.879 mm island; more aggressive than
the defect requires — do not start there.)

**Neighbour rule: largest shared area of the eps-dilated island**, `eps = 2*SCALED_EPSILON` (or
`scaled(0.01)`). Chosen over "largest shared boundary length" (needs edge-level bookkeeping across
`ExPolygon`s) and over "nearest centroid" (wrong for a crescent). **Determinism [important — this
codebase has been bitten]:** `ExPolygon::area()` returns an exact integer in scaled coordinates, so
the comparison is exact — no float ordering. Scan `c` ascending with a strict `>`, so **lowest
colour index wins ties**, matching the precedence already established at `:2227-2231`. Clipper's
output ordering for a fixed input is deterministic, and the absorb never depends on the *order* of
`base_area` because each component is decided independently.

**Interactions.**
- **F1's contour inset**: preserved by construction. `P ⊆ interior` means the absorb can never
  touch the wall-stack band at the contour, so the exterior bleed F1 closed cannot return. This
  also side-steps the explicit warning at `:2051-2065` about the retired I1 absorb ("actively
  unsafe … sits exactly on the knife-edge … appended straight back onto the contour"): I1 tested a
  ring *at* the contour; this tests an island *strictly inside* it. Different set, no knife edge.
- **Interlocking notch**: the ≤0.1 mm tooth is **connected to the base core**, so the per-component
  test `opening_ex({P}, t).empty()` is false for its component and it is skipped. Crucially the
  test must be **per connected component**, never `diff_ex(base_area, opening_ex(base_area, t))` —
  the latter would strip the notch tooth and thin fingers off the genuine base core.
- **`paint_depth_mode = unlimited`**: gated off by `segmentation_normal_depth == 0`
  (`:3151`), so the legacy path stays byte-identical. `cut_segmented_layers` is already skipped
  there (`:2980`).
- **The base colour's own legitimate regions**: the pink face is a *painted* colour and is never a
  candidate — `base_area` is `layer \ ∪_{c>=1}`. The unpainted yellow body's keep-core has a
  printable core and fails the first test. Only interior islands under 0.45 mm across, entirely
  surrounded by paint, are absorbed.

**Cost.** One `diff_ex`, one `offset_ex` and one `opening_ex` per thin base component per layer.
Bounded by component count, not by layer complexity — unlike the F2 ladder that `:1301-1311` had to
guard.

**Testability in the existing `slice()` harness.** `tests/libslic3r/test_paint_depth_clamp.cpp`
already has everything but a two-colour fixture:
- extend `paint_depth_test_config()` (`:58`) to 3 extruders and add a `slice_painted_box` variant
  that takes `{facet -> EnforcerBlockerType}` so `Extruder2` and `Extruder3` can be painted on
  adjacent facets of `make_square_frustum(40., 22., 6.)` (its walls slope 33.7° at 6 mm — use a
  shallower frustum, e.g. `make_square_frustum(40., 34., 6.)` ⇒ 18.4°, inside the <24° band);
- generalise `extruder2_claim_for_layer()` (`:193`) to `extruderN_claim_for_layer()`;
- **assertion**: on every layer, no `ExPolygon` of
  `diff_ex(layer_slices, ∪ painted claims)` may be simultaneously (a) empty after
  `opening_ex(·, scaled(0.225))` and (b) contained in `offset_ex(layer_slices, -scaled(0.87854))`.
  That assertion **fails on HEAD today** and is the regression pin.
- **parity/no-op pins**: reuse the shape of the existing "unlimited mode … legacy parity" test
  (`:334`) to assert the absorb is inert in unlimited mode, and the existing
  `verify_paintdepth.sh` determinism check for run-to-run identity.

### Option 2 — **Filter the accumulated claim, not each descent contribution**

**Site.** Move `opening_ex(last, stat.small_region_threshold)` from `:2079` / `:2146` to the
accumulation loop at `:2213-2222`, applying it to
`union_ex(shell_triangles_by_color_top[c][L], shell_triangles_by_color_top[c][L + num_layers])`.

**Why it is right in principle.** The rings deposited by *different* surface layers `j` land at
*adjacent* insets `[m·r, (m+1)·r]` in the *same* slot, so they form a **contiguous band** once
unioned. Filtering each contribution separately punches holes in a band that is never actually thin
— #7104's purpose was to delete *isolated unprintable projections*, not to hole a band. This is
also the clean answer to the 24° question (§6).

**Why it is ranked second.** The `deposited` / `break` logic at `:2080-2085` is *deliberately* keyed
on post-opening emptiness (its own comment, `:1997-1999`: "Deliberately keyed on 'deposited'
(post-opening) rather than on the raw term"). Moving the filter forces re-keying termination on the
raw term, which changes when the descent stops on every geometry. `stat.small_region_threshold` also
has to be recomputed in the second loop (it is per layer *and* per colour). And it does not touch the
surface-layer filter at `:1946`. Land it **after** Option 1, with Option 1's assertion in place as a
safety net.

### Option 3 — **Widen the residue filter in `apply_mm_segmentation`** (stopgap only)

`PrintObjectSlice.cpp:4585`, `opening(union_ex(mine), 5*EPSILON, 5*EPSILON)` → `scaled(0.1125)`.
One line, kills the yellow scatter immediately. **But** whatever `mine` drops is owned by *nobody*
(the painted `stolen` sets were taken at `:4520-4533`), so it becomes a ≤0.225 mm **void** — the
two neighbouring extrusions will usually bridge it at 0.45 mm width, but it is a hole in the
geometry, not a colour decision. Use only if a release must ship before Option 1.

### Option 4 — Absorb inside `segmentation_top_and_bottom_layers` at `:2210-2231` — **rejected**

The lateral claims do not exist yet at that point (`cut_segmented_layers` has run, but
`merge_segmented_layers` has not), so `base_area` computed there is grossly overstated and the
absorb would eat area the lateral band legitimately owns. The tempting part is that `:2227-2231`
already does per-colour precedence there; reuse the *tie-break convention*, not the site.

**Recommended sequence: 1, then re-measure in the GUI, then 2 (which also settles §6).**

---

## 6. Would the fix help symptom 3, and how does it interact with the #7104 guard?

**Symptom 3 (erratic infill): helps, does not fix. [I]** Absorbing interior base islands removes
some per-layer footprint churn (each absorbed island is one fewer region-boundary flicker, and
under forced `interface_shells` every flicker is a `stTop`/`stBottom` event), so the density of
narrow internal-solid patches drops. But the dominant driver is the *legitimate* layer-to-layer
change of a normal-thickness claim on a curved surface, which the absorb does not and should not
touch. Symptom 3 needs its own lever, and there are two candidates worth a separate design pass:
(i) scope the forced `interface_shells` to genuine colour boundaries rather than OR-ing it in
globally at `PrintObject.cpp:1333` / `:1766` (the `has_bounded_paint_depth()` comment in
`Print.hpp:505-514` already records that this was a deliberate over-approximation because
"`LayerRegion::slices` carries no 'why did this region differ' provenance"); or (ii) suppress the
narrow-internal-solid → `ipConcentricInternal` reroute for surfaces whose narrowness comes from a
paint boundary. Neither belongs in this fix.

**#7104 guard / pushing coverage past 24°: the absorb makes lowering it largely unnecessary, and
Option 2 is the better lever. [V on the mechanism, I on the recommendation]**

- Lowering `small_region_threshold` would keep more sub-0.225 mm strips **in the painted colour
  instead of leaking them to base**, so it *would* reduce the sliver. But it re-arms exactly the
  problem #7104 exists for — unprintable sub-bead strips — now in the *painted* filament, and under
  Arachne each one still becomes a widened 0.34 mm bead (`WideningBeadingStrategy.cpp:27-42`). It
  trades a yellow unprintable sliver for a red one. Not a win.
- **The absorb recovers the same coverage by a better route**: the strip is handed to a
  neighbouring *printable* claim instead of being emitted as an unprintable one of its own. For the
  **claim-to-claim** case that fully subsumes the reason to lower the guard.
- It does **not** subsume the **claim-to-contour** case (a shallow-slope ring hugging the contour,
  whose only neighbour is base). There F1's inset means base is the correct owner anyway, and the
  absorb correctly declines (the `P ⊆ interior` guard).
- **Option 2 is the honest way to push past 23.96°**, because it does not weaken the guard's
  constant at all: it applies the same 0.225 mm test to the *accumulated* claim, where a band built
  from several adjacent per-step rings is genuinely wide even though each ring is not. Expected
  reach after Option 2: limited by `M·r` and the band overlap rather than by `r` alone.
- **Ordering matters**: with the sliver noise still present, any GUI evaluation of a lowered guard
  is unreadable. Land Option 1 first, re-shoot the model, then evaluate Option 2 / the guard.

---

## 7. Loose ends found while reading (not part of this defect)

1. **`:1706-1707` comment is off by 100×** — "Filter out polygons less than 0.1mm^2" but the
   argument is `sqr(scale_(0.1f))`, i.e. 0.01 mm². Harmless, misleading. **[V]**
2. **`layer_color_stat` degenerate path** — if a painted colour has no region with
   `wall_filament == color_idx` on a layer, `extrusion_width`/`extrusion_spacing` stay 0, so
   `wall_stack == 0` and F1's `offset_ex(contour, -0)` is a no-op, while `normal_shell` at
   `:1899-1900` still evaluates **true** (`scaled(D) + eps >= 0`). `assert(out.num_regions > 0)`
   (`:1861`) catches it in debug only. Unreachable for a real object per the N1 comment
   (`:1809-1824`), but the release-build failure mode is "claim reaches the contour", i.e. the
   exact bleed F1 exists to stop. Worth a `wall_stack > 0` term in the `normal_shell` condition.
   **[V]**
3. **`gap_infill_speed == 0` triples the sliver width** to 0.75 mm (`:1851-1855`, other arm). Any
   GUI validation of the fix should be run in both states. **[V]**
4. **`extract_colored_segments` `:464-478`** can exit its repair `while` loop having appended
   nothing, silently dropping a Voronoi face. A pre-existing, unrelated source of unclaimed base.
   **[V]**

---

Report path: `.superpowers/sdd/2026-08-31-paint-depth/interclaim-sliver-investigation.md`
