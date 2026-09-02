# Shell / infill-side investigation: painted top skin over base-colour material

Read-only investigation. Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, HEAD `e64a4e154e`.
Scope: SHELL and INFILL side only. The segmentation-side projection mechanism is covered by a sibling
investigation; where this report touches `MultiMaterialSegmentation.cpp` it is only to anchor the
shell-depth math that the shell code consumes.

Legend: **[V]** = verified by reading the code at the cited line. **[I]** = inferred from the verified
code but not observed in a slice/GUI run.

---

## 0. TL;DR

* Each `PrintRegion` computes its top/bottom shell stack **strictly inside its own volume**. A painted
  region that exists only in the top N layers cannot generate shell below itself — the shell loops
  intersect against `layer->regions()[region_id]->fill_surfaces`, which is empty where the region has no
  slices. **[V]**
* Under this branch's defaults the material under a thin painted skin is **base-coloured SOLID infill,
  not sparse infill**. There is no missing-material / pillowing defect in the default configuration.
  The symptom is a **COLOUR** problem (base filament reading through a too-thin painted skin), not a
  missing-solid problem. **[V]** for the classification chain, **[I]** for the optical read-through.
* There is exactly one configuration corner where solid really is missing:
  `ensure_vertical_shell_thickness != All` **AND** `paint_depth_mode == Unlimited` **AND**
  `interface_shells == false`. **[V]**
* The already-shipped `has_bounded_paint_depth()` OR into `interface_shells` is what closes that corner
  by default — but it closes it with **base-coloured** solid, which is exactly the material the user is
  seeing through. It does not and cannot change the colour.
* The correct lever is the **depth of the painted claim**, not the shell code. The shell code is
  already doing the right thing.

---

## 1. Solid shell math per region

### 1.1 Pipeline order

`PrintObject::prepare_infill()` — `src/libslic3r/PrintObject.cpp:400`:

| Line | Step |
|---|---|
| `PrintObject.cpp:420` | `detect_surfaces_type()` — classify `layerm->slices` into stTop / stBottom / stBottomBridge / stInternal |
| `PrintObject.cpp:429` | `LayerRegion::prepare_fill_surfaces()` — demote stTop→stInternal if `top_shell_layers == 0` (`LayerRegion.cpp:1025`), stBottom→stInternal if `bottom_shell_layers == 0` (`LayerRegion.cpp:1032`), all stInternal→stInternalSolid at 100% density (`LayerRegion.cpp:1038`) |
| `PrintObject.cpp:435` | `discover_vertical_shells()` — the shell generator when `ensure_vertical_shell_thickness == All` |
| `PrintObject.cpp:458` | `discover_horizontal_shells()` — the shell generator otherwise |

The two shell generators are mutually exclusive per region:
`discover_vertical_shells` skips regions whose `ensure_vertical_shell_thickness != evstAll`
(`PrintObject.cpp:1844-1846`), and `discover_horizontal_shells` `continue`s for regions whose value
**is** `evstAll` (`PrintObject.cpp:4104-4105`). **[V]**

Default `ensure_vertical_shell_thickness` is `evstAll` (`PrintConfig.cpp:1753` def, default at the
`set_default_value(... evstAll)` line in that block), so `discover_vertical_shells` is the live path in
the user's scenario. **[V]**

### 1.2 layers-vs-thickness math (identical shape in both generators)

Both generators use the same "N layers OR M millimetres, whichever is deeper" rule:

`discover_vertical_shells`, `PrintObject.cpp:1954-1967`:

```cpp
if (int n_top_layers = region_config.top_shell_layers.value; n_top_layers > 0) {
    coordf_t print_z = layer->print_z;
    int i    = int(idx_layer) + 1;
    int itop = int(idx_layer) + n_top_layers;
    for (; i < int(cache_top_botom_regions.size()) &&
           (i < itop || m_layers[i]->print_z - print_z < region_config.top_shell_thickness - EPSILON);
         ++i) { ... combine_shells(cache.top_surfaces); }
```

Bottom counterpart at `PrintObject.cpp:1983-1996` (uses `bottom_shell_layers` / `bottom_shell_thickness`).

`discover_horizontal_shells`, `PrintObject.cpp:4112` and the loop condition at
`PrintObject.cpp:4141-4147`, is the same disjunction expressed as a downward scatter.

Defaults (`PrintConfig.cpp`): `top_shell_layers = 4` (`:6364` block), `top_shell_thickness = 0.6 mm`
(`:6375` block), `bottom_shell_layers = 3` (`:1031` block), `bottom_shell_thickness = 0.0`
(`:1042` block). At 0.2 mm layers the top shell is `max(4, ceil(0.6/0.2)) = 4` layers. **[V]**

### 1.3 Per-region independence — YES, confirmed

Both generators are region-scoped in the same two ways.

**`discover_vertical_shells`** — the *source* of shell polygons is per-region when
`top_bottom_surfaces_all_regions == false`: `PrintObject.cpp:1866` writes
`cache.top_surfaces = offset(layerm.slices.filter_by_type(stTop), ...)` for one `region_id` only
(assignment, not append). The *target* is always per-region: `PrintObject.cpp:2066-2067` intersects the
accumulated shell with `layerm->fill_surfaces.filter_by_types({stInternal, stInternalVoid,
stInternalSolid})` of that same `region_id`, and the results are written back into that region's
`fill_surfaces` at `PrintObject.cpp:2160-2163`. **[V]**

**`discover_horizontal_shells`** — outer loop is `for region_id` (`PrintObject.cpp:4088`), the neighbour
it writes into is `m_layers[n]->regions()[region_id]` (`PrintObject.cpp:4151`), and the intersection
source is that neighbour's own `fill_surfaces` (`PrintObject.cpp:4166-4169`). **[V]**

**Consequence for a shallow painted region:** the painted `PrintRegion` P exists as a `LayerRegion` on
every layer (all printing regions are materialised for every layer at
`PrintObjectSlice.cpp:5230-5234`), but on layers below its claim its `slices`/`fill_surfaces` are empty.
So P's own downward shell projection intersects with nothing and dies at P's own floor. **A small painted
region on a top face computes its top shell only within its own (possibly 1-layer) volume.** There is no
mechanism by which P's `top_shell_layers = 4` can reach into layers P does not occupy. **[V]**

`discover_vertical_shells` bails early for empty results at `PrintObject.cpp:2069-2070` (`shell.empty()`)
and `:2130-2131` (`regularized_shell.empty()`), so the painted region simply contributes nothing below
its claim rather than erroring. **[V]**

### 1.4 The one cross-region path

When `top_bottom_surfaces_all_regions == true` — i.e. multi-region **and** `interface_shells` effectively
false — `PrintObject.cpp:1793-1801` **appends** top/bottom surfaces from *all* regions into one shared
cache entry. The shell built from that merged cache is then trimmed by each region's own internal fill
surfaces at `:2066-2067`. This is the only place where region A's top surface produces solid infill
inside region B. **[V]**

The paint-depth feature deliberately turns this path **off** for painted objects:
`PrintObject.cpp:1755`

```cpp
bool top_bottom_surfaces_all_regions = this->num_printing_regions() > 1 &&
                                       ! (m_config.interface_shells.value || this->has_bounded_paint_depth());
```

---

## 2. The thin-skin case — verdict

### 2.1 What `interface_shells` changes in `detect_surfaces_type`

`PrintObject.cpp:1322`:

```cpp
bool interface_shells = ! spiral_mode && (m_config.interface_shells.value || this->has_bounded_paint_depth());
```

`has_bounded_paint_depth()` is `Print.hpp:495`: `is_mm_painted() && paint_depth_mode != pdmUnlimited`.
`paint_depth_mode` defaults to `pdmWalls` (`PrintConfig.cpp:3907`), so **for any painted object on this
branch, `interface_shells` is effectively ON by default.** **[V]**

Top detection, `PrintObject.cpp:1381-1384`:

```cpp
ExPolygons upper_slices = interface_shells ?
    diff_ex(layerm_slices_surfaces, upper_layer->m_regions[region_id]->slices.surfaces, ApplySafetyOffset::Yes) :
    diff_ex(layerm_slices_surfaces, upper_layer->lslices,                               ApplySafetyOffset::Yes);
surfaces_append(top, opening_ex(upper_slices, offset), stTop);
```

This is the crux the question asks about:

* `interface_shells == true` → the base region B is diffed against **B's own slices** on the layer above.
  The painted patch is missing from B on that layer, so the patch area **becomes stTop for B**. B *does*
  see a top surface under the painted claim. **[V]**
* `interface_shells == false` → B is diffed against `upper_layer->lslices` (the union of *all* regions).
  The painted patch covers B from above, so B's patch area stays **stInternal**. B does **not** see a top
  surface. **[V]**

### 2.2 Full matrix

Let the painted claim occupy the top K layers, and the region under it be the base region B.

| `ensure_vertical_shell_thickness` | `interface_shells` effective (`option \|\| has_bounded_paint_depth`) | What is printed in the layers between the painted skin and the sparse infill |
|---|---|---|
| `All` (default) | **true** (default for painted objects on this branch) | B gets stTop at layer `K+1` (`:1381`) → B's own `discover_vertical_shells` pass projects it down `top_shell_layers`/`top_shell_thickness` (`:1954-1967`) → **base-colour top-solid + base-colour internal solid**. Solid present, wrong colour. **[V]** |
| `All` | false (`paint_depth_mode = Unlimited`, `interface_shells` unchecked) | `top_bottom_surfaces_all_regions == true` → the painted region's stTop is merged into the shared cache (`:1797`) and trimmed into B's internal surfaces (`:2066-2067`) → **base-colour internal solid**. Solid present, wrong colour. **[V]** |
| `None` / `Critical Only` / `Moderate` | **true** | B has stTop at `K+1` → `discover_horizontal_shells` scatters it downward inside B (`:4141-4169`) → **base-colour solid**. Solid present, wrong colour. **[V]** |
| `None` / `Critical Only` / `Moderate` | false | B never gets stTop, and `discover_horizontal_shells` is strictly per-region → **sparse infill directly under the painted skin. Solid genuinely MISSING.** **[V]** |

### 2.3 Verdict

**In the user's configuration (defaults: `ensure_vertical_shell_thickness = All`,
`paint_depth_mode = walls` → `has_bounded_paint_depth() == true`) the problem is COLOUR, not missing
solid material.** The layers between the painted skin and the sparse infill are fully solid; they are
just extruded with the base region's `solid_infill_filament`. **[V]**

Filament selection confirms it. `PrintRegion.cpp:28-31`:

```cpp
else if (role == frSolidInfill)
    extruder = internal_solid_infill_uses_sparse_filament(m_config, role) ? m_config.sparse_infill_filament : m_config.solid_infill_filament;
else if (role == frTopSolidInfill)
    extruder = m_config.solid_infill_filament;
```

`internal_solid_infill_uses_sparse_filament` is true only at 100% sparse density
(`GCode/ToolOrdering.cpp:91-94`), so in the normal case both the hidden top surface and the internal
solid under it use the *base* region's `solid_infill_filament`. **[V]**

Correspondingly, the painted region's own solid infill is **always** painted —
`PrintApply.cpp:837` and `PrintApply.cpp:1090` set `cfg.solid_infill_filament.value = <painted extruder>`
unconditionally; only `sparse_infill_filament` is gated on `paint_sparse_infill`
(`PrintApply.cpp:840-841`, `:1096-1099`, gate at `:1862-1863`). So *within* the painted claim the top
shell is already painted; the deficit is purely that the claim is not as deep as the shell. **[V]**

### 2.4 Side effect worth flagging

Because `has_bounded_paint_depth()` forces `interface_shells` on, the base region now grows a **hidden
stTop** directly beneath every painted claim. That surface is printed as a real top surface: top-surface
pattern, `top_surface_density`, top-surface speed, and it is eligible for ironing
(`Fill/Fill.cpp:1659` gates ironing on `surface_type == stTop && top_shell_layers > 0`). This is a
print-time cost introduced by the branch, on geometry that is never visible. **[I]** — verified that the
surface is created and typed stTop; not verified against a G-code diff.

---

## 3. Existing knobs — what each does and does not fix

### 3.1 `interface_shells` (+ the `has_bounded_paint_depth()` OR)

Four read sites, all confirmed:

| Site | Effect |
|---|---|
| `PrintObject.cpp:1322` (`detect_surfaces_type`) | base region sees its own top/bottom at colour boundaries |
| `PrintObject.cpp:1755` (`discover_vertical_shells`) | per-region rather than merged top/bottom cache |
| `PerimeterGenerator.cpp:622` | one-wall-top detection uses `upper_slices_same_region` |
| `PerimeterGenerator.cpp:2273` | top-surface extraction uses `upper_slices_same_region` |

(The 3rd and 4th are fed by `LayerRegion.cpp:227`, `g.has_bounded_paint_depth = ...`.)

* **Fixes:** guarantees solid material at the colour interface in every configuration, including the
  `evst != All` corner in §2.2 row 4. Guarantees the base region gets a proper top surface (and thus
  proper first-layer-over-sparse anchoring) beneath the painted patch.
* **Does NOT fix:** colour. `interface_shells` is a *surface classification* switch; it never changes
  which filament a region uses. The solid it creates belongs to the base region and therefore prints in
  the base filament (§2.3). It cannot make the painted skin thicker.
* **Cost:** extra solid infill + a hidden top surface per painted patch (material and time).

### 3.2 `ensure_vertical_shell_thickness` modes

`evstAll` selects `discover_vertical_shells` and, when `interface_shells` is *off*, gives the only
cross-region solid path (`PrintObject.cpp:1797`). `evstNone` / `evstCriticalOnly` also loosen the
`too_narrow` filters in `discover_horizontal_shells` (`:4178`, `:4191-4197`, `:4222`).

* **Fixes:** presence of solid on sloped/stepped colour boundaries; it is the reason the default config
  never shows a void under a painted patch.
* **Does NOT fix:** colour, and it does not deepen the painted claim. Setting it to `All` when it is
  already `All` changes nothing here.

### 3.3 `minimum_sparse_infill_area`

`LayerRegion.cpp:672-687` (inside `process_external_surfaces`) converts sparse islands smaller than the
threshold into solid — **within the same region**, so the colour is unchanged.

* **Fixes:** nothing about this symptom, except incidentally converting tiny sparse pockets to solid.
* **Does NOT fix:** colour; does not act across regions.

### 3.4 `internal_bridge` / `enable_extra_bridge_layer` / `bridge_over_infill`

`PrintObject.cpp:1533` (`enable_extra_bridge_layer`), `PrintObject.cpp:3064`, `bridge_over_infill`.
These affect how the *first solid layer over sparse infill* is extruded (bridge flow/speed/density) and
can add a second bridge layer.

* **Fixes:** surface quality of the first solid layer over sparse infill — i.e. it would help if the
  problem were pillowing.
* **Does NOT fix:** colour, and it is irrelevant in the default config because the layer immediately
  under the painted skin is already solid, not a bridge over sparse.

### 3.5 `solid_infill_below_area`

**Not present in this fork.** `grep` over `src/libslic3r` returns no definition or use; it is a
Prusa-only option. The nearest analogue is `minimum_sparse_infill_area` (§3.3).

### 3.6 `paint_infill_override`

`PrintConfig.cpp:3940` block, default `true`. Gate: `PrintApply.cpp:1862-1863`
(`paint_infill_override || paint_depth_mode == pdmUnlimited`).

* **Fixes:** whether the painted claim's *sparse* infill burns painted filament.
* **Does NOT fix:** the top shell. Inside `top_shell_layers` of a top face there is no sparse infill at
  all — it is all solid — so this knob has no effect on what sits under a painted top skin. Turning it
  **off** is what produces the user's observation that "the base colour is correctly used for the sparse
  infill under the painted area". **[V]**

### 3.7 The segmentation-side top/bottom shell projection (context only)

`MultiMaterialSegmentation.cpp:1400-1409` already extends a painted **top** claim downward:

```cpp
for (int last_idx = int(layer_idx) - 1; last_idx > std::max(int(layer_idx - stat.top_shell_layers), int(0)); --last_idx) {
    offset -= (stat.extrusion_spacing + stat.extrusion_width);
    layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);
    ExPolygons last = opening_ex(intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset)), stat.small_region_threshold);
    if (last.empty()) break;
    append(shell_triangles_by_color_top[color_idx][last_idx + layer_idx_offset], std::move(last));
}
```

with the bottom counterpart at `:1420-1429`. Crucially, this runs **after** the paint-depth clamp —
`cut_segmented_layers` at `MultiMaterialSegmentation.cpp:2182`, then
`segmentation_top_and_bottom_layers` at `:2189`, then `merge_segmented_layers` at `:2193`, where
`:1859` trims the side segmentation by the top/bottom claims and `:1869` appends them. So the paint-depth
band does **not** shorten a painted top claim. **[V]**

Three verified gaps in that loop that bear directly on this symptom (analysis of *why* they fire is the
sibling investigation's territory; recorded here because the fix candidates in §4 target them):

1. **No `top_shell_thickness` term.** The bound is `top_shell_layers` only. The shell generators use
   `layers OR millimetres` (`PrintObject.cpp:1961`, `:4144`). At small layer heights the shell is deeper
   in layers than the painted claim, so the bottom of the top shell is base-coloured by construction. **[V]**
2. **Erosion + early `break`.** `offset` accumulates `-(extrusion_spacing + extrusion_width)` per layer
   and is applied to `layer_slices_trimmed` (the object cross-section), pulling the claim inward from the
   object silhouette; `break` at `:1407` terminates the whole descent the first time the result is empty.
   A painted feature near the silhouette, on a thin cross-section, or on a shrinking taper loses its
   deeper layers. **[V]** for the mechanism; **[I]** that this is what fired on the user's model.
3. **`stat.top_shell_layers` is scoped to the painted colour.** `:1361` filters regions by
   `config.wall_filament == int(color_idx)` and `:1366` takes the max over those only. The shell that
   must be covered is the **base** region's, whose `top_shell_layers` may be larger. **[V]**

---

## 4. Minimal fix shapes

Ranked. All are per-symptom colour fixes; none of them is needed to prevent a void in the default config.

### (1) Add the millimetre term to the segmentation top/bottom shell descent — recommended

**Anchor:** `MultiMaterialSegmentation.cpp:1400` (top) and `:1420` (bottom).
Mirror the shell generators' disjunction. Conceptually, change the loop bound from
`last_idx > max(layer_idx - stat.top_shell_layers, 0)` to also continue while
`layers[layer_idx]->print_z - layers[last_idx]->print_z < stat.top_shell_thickness - EPSILON`, exactly as
`PrintObject.cpp:1961` and `PrintObject.cpp:4144` do. Requires adding `top_shell_thickness` /
`bottom_shell_thickness` to `LayerColorStat` (`MultiMaterialSegmentation.cpp:1340-1353`, populated at
`:1366-1367`), and raising `granularity` / `max_top_layers` at `:1210-1212` so the TBB group overlap still
covers the deepest possible descent (that overlap is what `layer_idx_offset` at `:1387` and the two-slot
buffers at `:1331-1338` exist for — getting this wrong silently truncates the claim at group boundaries).

* **Fixes:** the "shell is 4 layers, claim is 3" mismatch at fine layer heights. Makes the painted claim
  cover the full top shell by construction.
* **Does not fix:** the erosion `break` (fix 2).
* **Cost:** more painted filament in the top shell, plus potentially one extra tool change per affected
  layer. Zero extra *material volume* — the same solid volume is printed, just in the other colour.
  On a tool-changer / AMS this is purge cost, which can be significant.
* **Risk:** low; it is a bound widening on an existing loop, and it is gated by the same
  `max_top_layers > 0` guard at `:1225`.

### (2) Take the max of the painted colour's and the base region's shell counts

**Anchor:** `MultiMaterialSegmentation.cpp:1354-1381` (`layer_color_stat`), specifically the filter at
`:1361` and the max at `:1366-1367`.
Today a painted colour's descent depth is drawn only from regions whose `wall_filament` is that colour.
Taking the max over *all* regions on the layer (i.e. treating `color_idx != 0` like `color_idx == 0`
already is, per the comment at `:1359-1360`) makes the claim cover whatever shell the base region will
actually build.

* **Fixes:** the case where the painted region inherits a smaller `top_shell_layers` than the base region
  it sits on (per-object or per-modifier overrides).
* **Does not fix:** anything when all regions share one shell setting — which is the common case.
* **Cost:** same class as (1), only in the mixed-settings case.
* **Risk:** low, but it changes claim depth for *every* painted object, including unbounded ones. Would
  need the `verify_paintdepth.sh` unpainted-inertness + determinism checks re-run.

### (3) Bound the erosion so the descent cannot terminate early

**Anchor:** `MultiMaterialSegmentation.cpp:1403` (`offset -= (stat.extrusion_spacing + stat.extrusion_width)`)
and the `break` at `:1407`.
Two sub-options: (a) clamp the accumulated `offset` to some floor so it stops eating the claim after the
first wall or two; (b) turn the `break` into a `continue` guarded by a "have we produced anything yet"
flag, so a claim that vanishes at layer k because of a local silhouette pinch can resume at k-1.

* **Fixes:** the small-feature / near-silhouette case, which is the most likely real-world trigger for
  "the painted skin is only 1-3 layers".
* **Does not fix:** the millimetre mismatch (fix 1) — they are independent and complementary.
* **Cost:** the eroded band exists on purpose (it keeps the painted top claim from colliding with the
  base region's perimeters and creating unprintable slivers, which is what `small_region_threshold` at
  `:1368-1373` also guards). Relaxing it risks reintroducing the dimple/sliver class of defects that
  `:1871-1873` was added to fix (upstream #7235).
* **Risk:** **highest of the four.** Needs visual validation on a small painted logo *and* on a painted
  patch that runs to the object edge.

### (4) Colour-only: give the base region's under-claim solid the painted filament

**Anchors:** `PrintApply.cpp:1084-1116` (`generate_print_object_regions` painted-region creation) and
`PrintApply.cpp:831-841` (`verify_update_print_object_regions`, must stay in sync — the comment at
`:752-756` says so explicitly).
Filament is a `PrintRegionConfig` property, not a per-surface property (`PrintRegion.cpp:19-34`), so
"paint just the solid under the claim" means minting a *third* region: base geometry, base
`wall_filament` and `sparse_infill_filament`, but `solid_infill_filament` = painted extruder — and then
arranging for the base region's under-claim stTop/stInternalSolid to land in that region rather than the
plain base region. There is no existing splitting mechanism for that; the split would have to happen in
segmentation anyway.

* **Fixes:** the colour, without deepening the claim's walls.
* **Does not fix:** anything the other options don't, and it produces a *worse* result at the claim
  perimeter — painted solid infill butting against base-coloured walls.
* **Cost:** high implementation cost; one more interned `PrintRegion` per painted extruder, which
  inflates `num_printing_regions()` and therefore the per-region cost of every loop in §1.
* **Verdict:** **not recommended.** It is strictly worse than (1)+(3), which achieve the same colour
  outcome by making the claim itself the right depth.

### Non-option: guarantee the painted region's own top shell count within its claim

The task listed this as candidate (b). It is **not implementable as stated**: the painted region has no
`LayerRegion` volume below its claim (`PrintObject.cpp:2066-2067`, `:4166-4169` intersect against that
region's own `fill_surfaces`, which are empty there). "Guaranteeing the painted region's shell count"
*is* "making the claim deeper" — it reduces to options (1)–(3). Recording this so it is not re-proposed.

---

## 5. What was NOT verified

* No slice was run; every claim about what the G-code contains is a reading of the classification code,
  not an observation of output. Marked **[I]** where it matters.
* The specific reason the user's model produced a 1-3 layer claim was not isolated — §3.7 lists three
  candidate mechanisms, all verified as present in the code, none confirmed as the one that fired.
* `slice_mesh_slabs`' facet-normal criterion (`MultiMaterialSegmentation.cpp:1242`, `:1253`) was not
  read; if the painted face is not classified as an upward slab at all, the top-shell descent never runs
  and all of §4's options are moot. **This is the first thing to check on the actual model**, and it is
  the sibling investigation's area.
