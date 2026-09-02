# Vertical paint depth on top/bottom faces — investigation

Date: 2026-08-31 · Worktree: `C:\Dev\SnapmakerOrcaNext` · Branch `feat/paint-depth` @ `e64a4e154e`
Scope: read-only. Question: for an object printed **lying down** with paint on a near-horizontal
**top** (or bottom) face, what gives those painted areas vertical depth, and does the Stage-1
lateral paint-depth clamp (`cut_segmented_layers`, now on by default) interfere with it?

Method note: everything under "Verified" was read directly in the committed tree at the cited
`file:line`. Items under "Inferred" are reasoning over verified code, not separately executed.

---

## Headline answer

**No regression.** The lateral clamp and the top/bottom projection are two independent data paths
that never touch each other's inputs. `cut_segmented_layers` mutates only `segmented_regions` (the
Voronoi/contour-derived *side* segmentation) and runs **before**
`segmentation_top_and_bottom_layers` even executes; the top/bottom result is computed from the
**mesh** plus the raw layer slices, then **unioned in** afterwards. An interior painted island on a
top face is neither deleted nor shrunk by our clamp.

There *is* a real pre-existing gap, unrelated to (and not introduced by) the clamp: the painted
top/bottom claim is `top_shell_layers` / `bottom_shell_layers` deep and **ignores
`top_shell_thickness` / `bottom_shell_thickness`**, while the actual solid shell honours both. At
fine layer heights the stock defaults already diverge. Stage 2's forced `interface_shells`
behaviour largely *masks* the visible consequence, but does not close the gap. Details in §3–§4.

---

## 1. Top/bottom projection mechanism

Function: `segmentation_top_and_bottom_layers`,
`src/libslic3r/MultiMaterialSegmentation.cpp:1194-1484`.

### 1a. What geometry is projected, and in which direction

**Verified:**

- `src/libslic3r/MultiMaterialSegmentation.cpp:1230` — for each model-part `ModelVolume` and each
  colour state, `extract_facets_info(*mv).facets_annotation.get_facets_strict(*mv, EnforcerBlockerType(extruder_idx))`
  pulls the **painted triangle patch itself** out of the mesh (an `indexed_triangle_set`). This is
  mesh geometry, not slice geometry — completely disjoint from the `PaintedLine`/Voronoi path.
- `:1242` / `:1253` — `slice_mesh_slabs(painted, zs, volume_trafo, &top, &bottom, nullptr, …)`
  builds *slabs*: upward-facing painted facets are projected **downwards** onto the layer they cap
  (`out_top`), downward-facing painted facets are projected **upwards** (`out_bottom`). Results land
  in `top_raw[colour][layer]` / `bottom_raw[colour][layer]` as `Polygons`.
  `:1239-1251` is a sinking-object special case (prepends `z=0`, unions the `z[0]` cross-section
  into `bottom[0]`).
- **There is no slope threshold.** `src/libslic3r/TriangleMeshSlicer.cpp:2117-2143`: face
  orientation is the *sign* of the 2D cross product of the XY-projected triangle
  (`d = cross2(b-a, c-b) * mirrored_sign`), so `d > 0 → Up`, `d < 0 → Down`, `d == 0 → Vertical` (or
  `Degenerate`). Any facet with a non-zero normal-Z is Up or Down. "Near-horizontal" is therefore
  not a special case — a steep face is still classified Up, it just projects a vanishingly small XY
  area, and the occlusion trim below removes whatever the layer above covers. This is exactly the
  behaviour you want for a lying-down object: the big flat top face yields a big projection, the
  side walls yield only the thin staircase slivers.
- `:1288-1289` — projections smaller than `0.1 mm²` are dropped (`filter_out_small_polygons`,
  upstream issue #7104, unprintable dimples).
- `:1315-1327` — **occlusion trim**: `top_raw[c][l] = diff(top_raw[c][l], input_expolygons[l+1])`
  and `bottom_raw[c][l] = diff(bottom_raw[c][l], input_expolygons[l-1])`. Only genuinely exposed
  top/bottom surface survives; a painted facet buried under solid material contributes nothing here.
- `:1225` — the whole block is gated on `max_top_layers > 0 || max_bottom_layers > 0`. If **both**
  `top_shell_layers` and `bottom_shell_layers` are 0 on every region, nothing is projected at all.

### 1b. How many layers deep — the config keys

**Verified:**

- `:1205-1213` — object-wide maxima, over `print_object.num_printing_regions()`:
  ```
  max_top_layers    = max(max_top_layers,    config.top_shell_layers.value);      // :1210
  max_bottom_layers = max(max_bottom_layers, config.bottom_shell_layers.value);   // :1211
  granularity       = max(granularity, max(top_shell_layers, bottom_shell_layers) - 1);  // :1212
  ```
  (`granularity` + `layer_idx_offset = (group_idx & 1) * num_layers` at `:1386-1387` is the
  double-buffer trick that keeps neighbouring TBB groups from racing on the same layer slot; both
  buffers are folded back together at `:1445-1448`.)
- `:1354-1381` — `layer_color_stat(layer_idx, colour_idx)` recomputes the per-colour figures from
  the `LayerRegion`s whose `config.wall_filament == colour_idx` (or **all** regions when
  `colour_idx == 0`, `:1361`):
  - `:1366` `out.top_shell_layers    = max(out.top_shell_layers,    config.top_shell_layers)`
  - `:1367` `out.bottom_shell_layers = max(out.bottom_shell_layers, config.bottom_shell_layers)`
  - `:1365` `extrusion_width` from `outer_wall_line_width`; `:1374` `extrusion_spacing` from
    `Flow::rounded_rectangle_extrusion_spacing`; `:1368-1373` `small_region_threshold`.
- **The depth loop (top)**, `:1392-1411`:
  ```cpp
  top_ex = opening_ex(union_ex(top[layer_idx]), stat.small_region_threshold);          // :1395
  append(triangles_by_color_top[color_idx][layer_idx + layer_idx_offset], top_ex);      // :1397  <- surface layer
  float offset = 0.f;
  ExPolygons layer_slices_trimmed = input_expolygons[layer_idx];
  for (int last_idx = int(layer_idx) - 1;
       last_idx > std::max(int(layer_idx - stat.top_shell_layers), int(0)); --last_idx) {   // :1400
      offset -= (stat.extrusion_spacing + stat.extrusion_width);                            // :1403
      layer_slices_trimmed = intersection_ex(layer_slices_trimmed, input_expolygons[last_idx]);  // :1404
      ExPolygons last = opening_ex(intersection_ex(top_ex, offset_ex(layer_slices_trimmed, offset)),
                                   stat.small_region_threshold);                             // :1405
      if (last.empty()) break;                                                                // :1406-1407
      append(shell_triangles_by_color_top[color_idx][last_idx + layer_idx_offset], std::move(last)); // :1408
  }
  ```
- **Bottom is the mirror**, `:1412-1431`, with
  `for (size_t last_idx = layer_idx + 1; last_idx < std::min(layer_idx + stat.bottom_shell_layers, num_layers); ++last_idx)`
  at `:1420`.

**Depth arithmetic (verified by reading the bounds).** The comparison at `:1400` is **strict** `>`.
For a surface layer `L` with `top_shell_layers = N` (and `L > N`), `last_idx` runs
`L-1, L-2, … , L-N+1` — that is `N-1` sub-surface layers, plus the surface layer itself appended at
`:1397`. **Total claimed depth = `top_shell_layers` layers.** Symmetrically the bottom loop claims
`bottom_shell_layers` layers total.

**The depth is therefore set by, and only by:**

| key | role | default |
|---|---|---|
| `top_shell_layers` | layer count for the downward (top-face) claim | **4** (`src/libslic3r/PrintConfig.cpp:6364-6374`) |
| `bottom_shell_layers` | layer count for the upward (bottom-face) claim | (`PrintConfig.cpp` `bottom_shell_layers`) |
| `top_shell_thickness` | **NOT consulted here** | 0.6 mm (`PrintConfig.cpp:6375-6385`) |
| `bottom_shell_thickness` | **NOT consulted here** | — |

There is no hardcoded depth constant. The only hardcoded numbers in this path are the `0.1 mm²`
small-polygon filter (`:1288`) and the per-layer inward taper step
`extrusion_spacing + extrusion_width` (`:1403`), both derived from flow, not from a depth setting.

### 1c. The inward taper (important for reading §2 correctly)

`:1403-1405` shrinks the claim by one `(spacing + width)` **per layer of descent**, but it offsets
`layer_slices_trimmed` — the running intersection of the *layer slices* — **not** the painted island.
So:

- An island in the **middle** of a large top face keeps its **full area at every one of the `N`
  layers**, because the layer outline shrunk by `k·(w+s)` still fully contains it.
- An island **near the object contour**, or a painted region that *is* the whole top face, tapers
  inward with depth (a deliberate BBS anti-overlap measure, comment at `:1401`) and `break`s early
  when nothing survives (`:1406`).

### 1d. Assembly

`:1436-1480`. Per layer: surface polygons from both parity buffers are unioned into
`triangles_by_color_merged[c][l]` (`:1444-1452`); `painted_exploys` is the union across **all**
colours (`:1451, :1454`); the deeper `shell_triangles_by_color_*` claims are added only **after**
being diffed against `painted_exploys` (`:1460-1469`) so a deep claim can never overwrite another
colour's actual painted surface at that layer; `:1473-1478` resolves colour-vs-colour overlap in
index order and finally strips all painted area out of colour 0 (`:1478`).

Note colour index 0 (unpainted/default) participates fully — `:1229` iterates from 0 and
`get_facets_strict(…, EnforcerBlockerType(0))` returns the *unpainted* facets — so unpainted top
faces get their own projection too, which is what makes the `:1478` subtraction meaningful.

---

## 2. Order of operations vs. our clamp — **does it harm interior islands? NO**

### The call order, verified in `segmentation_by_painting`

`src/libslic3r/MultiMaterialSegmentation.cpp`:

| step | line | what it touches |
|---|---|---|
| 1. paint → `PaintedLine`s on the slice **contour** | `:2050-2136` | `painted_lines` |
| 2. per-layer Voronoi → **lateral** segmentation | `:2139-2177` | writes `segmented_regions` |
| 3. **`cut_segmented_layers(...)`** ← our clamp | **`:2181-2184`** | **mutates `segmented_regions` only** |
| 4. `segmentation_top_and_bottom_layers(print_object, input_expolygons, extract_facets_info, …)` | `:2188-2191` | reads the **mesh** + `input_expolygons`; **does not read `segmented_regions`** |
| 5. `merge_segmented_layers(segmented_regions, std::move(top_and_bottom_layers), …)` | `:2193` | unions (4) into (3) |

`cut_segmented_layers` has exactly **one** call site in the tree (grep over `src/` and `tests/`:
definition `:1146`, call `:2182`; all other hits are comments/tests).

### What the clamp actually does

`:1146-1181`, the operative line being `:1175`:
```cpp
segmented_regions_cuts[extruder_idx] = diff_ex(ex_polygons, offset_ex(input_expolygons[layer_idx], -region_cut_width));
```
i.e. keep only the part of the claim lying within `region_cut_width` of the layer boundary. Applied
to `segmented_regions[layer_idx][extruder_idx]` for **every** index including 0, with
`region_cut_width` alternating to `interlocking_cut_width = max(cut_width - interlocking_depth, 0)`
on even layers (`:1164, :1169` — the F1 fix-wave semantics).

**In isolation this operation absolutely would delete an interior island** — an island touching no
boundary lies entirely inside `offset_ex(layer, -band)` and diffs away to nothing. That is exactly
the failure mode the question anticipates.

### Why it does not happen

**Verified, three independent reasons:**

1. **Interior top-face islands are never in `segmented_regions` in the first place.** That array is
   built purely from paint that lands on the slice **contour** (`PaintedLineVisitor` → `colorize_contours`
   → `extract_colored_segments`, `:2139-2170`). A painted island in the middle of a top face
   intersects no layer contour, so it contributes nothing to `painted_lines[L]` and nothing to
   `segmented_regions[L]`. The clamp has no such island to delete. (Corollary, pre-dating our
   feature: without `IncludeTopAndBottomLayers::Yes` such islands would get **no** colour at all.
   `multi_material_segmentation_by_painting` passes `Yes` at `:2278`.)
2. **The clamp finishes before the projection starts, on a different array.** Step 3 mutates
   `segmented_regions`; step 4 takes `print_object`, `input_expolygons`, `extract_facets_info` and
   `num_facets_states` (`:2189`) — `segmented_regions` is not among its arguments and it reads no
   such state. `input_expolygons` is the untouched layer-slice array.
3. **At merge time the flow is the opposite direction.** `merge_segmented_layers`, `:1838-1881`:
   - `:1855-1861` — the **clamped lateral claim is trimmed by** the top/bottom claim
     (`segmented_regions_trimmed = diff_ex(segmented_regions_trimmed, top_and_bottom_by_extruder[layer_idx])`).
   - `:1867-1873` — the top/bottom claim is then **appended verbatim** (`append(...)`, then an
     `offset2_ex(±SCALED_EPSILON)` dimple clean-up only when both were non-empty, upstream #7235).

   The top/bottom claim is never diffed, offset inward, or intersected against the clamp band.

**Verdict: the lateral clamp does NOT delete or shrink interior painted islands on top/bottom
faces. There is no regression here.** The clamp bounds only how deep paint reaches *sideways* from
the silhouette; vertical depth on horizontal faces is supplied by a mechanism the clamp cannot see.

### Two secondary interactions checked and cleared

- **Clamping colour 0.** `cut_segmented_layers` clamps the base channel too. Verified inert at the
  consumer: `src/libslic3r/PrintObjectSlice.cpp:4296` moves channel 0 out into
  `default_segmentation`, which is used only for the component-bias clamp
  (`:4402-4413, :4471-4476`). Region assignment is *steal-based* — painted channels ≥ 1 steal from
  the parent (`:4515-4527`), and the parent keeps the remainder via `preserve_parent_region`
  (`:4426-4437, :4439-4452`). An under-covered channel 0 costs the base region nothing.
- **`paint_depth_mm = 0` / `pdmUnlimited`.** `:1164` makes `interlocking_cut_width` collapse to 0
  when `cut_width == 0`, so `region_cut_width` falls through to `cut_width == 0` on *both* parities
  and the `:1171` guard skips the layer entirely — genuinely unlimited, no half-clamped even layers
  (fix-wave F4).

---

## 3. Shell interaction — who owns the layers under a painted top island

### The region split

The painted claim becomes its own `PrintRegion`, cloned from the parent with only the filament
indices changed (`src/libslic3r/PrintApply.cpp:1088-1099`): `wall_filament`,
`solid_infill_filament`, and — gated on our new `paint_sparse_infill` — `sparse_infill_filament`.
**`top_shell_layers`, `top_shell_thickness`, `bottom_shell_*` are identical between the painted
region and its parent**, since `cfg` starts as `parent_region.region->config()` (`:1087`).

### Ownership (verified from §1b's depth arithmetic)

For a painted island whose visible top surface is layer `L`, with `top_shell_layers = N`:

- Layers `L-N+1 … L` (`N` layers, full island area for an interior island per §1c) → **PAINTED
  region**.
- Layer `L-N` and below, within the island footprint → **BASE region**.

### What the shell stack then does — and why Stage 2 matters here

`PrintObject::detect_surfaces_type`, `src/libslic3r/PrintObject.cpp:1311-1470`. The pivotal line is
our Stage-2 change at **`:1322`**:
```cpp
bool interface_shells = ! spiral_mode && (m_config.interface_shells.value || this->has_bounded_paint_depth());
```
(`has_bounded_paint_depth()` = `is_mm_painted() && paint_depth_mode != pdmUnlimited`,
`src/libslic3r/Print.hpp:495`.)

- **Top detection, `:1380-1384`.** With `interface_shells` **on**, a region's top surface is
  `diff_ex(layerm_slices, upper_layer->m_regions[region_id]->slices)` — *same region above*. With it
  **off**, it is `diff_ex(layerm_slices, upper_layer->lslices)` — *whole layer above*.
- **Bottom detection, `:1414-1426`.** `interface_shells` adds the non-bridging
  `stBottom` surfaces: `diff_ex(intersection(layerm_slices, lower->lslices), lower->m_regions[region_id]->slices)`.
- `discover_vertical_shells`, **`:1755`**, mirrors it:
  `top_bottom_surfaces_all_regions = num_printing_regions() > 1 && !(interface_shells || has_bounded_paint_depth())`,
  so with our flag the per-region cache path is taken instead of the all-regions merge at
  `:1759-1839`.
- `PerimeterGenerator.cpp:622` and `:2273` got the same OR (fix-wave F3), threaded in via
  `PerimeterGenerator::has_bounded_paint_depth` (`PerimeterGenerator.hpp:98-103`) set at
  `LayerRegion.cpp:227`, so the classic and Arachne one-wall-top / top-surface detection agree with
  the two `PrintObject` sites.

**Consequence, and the direct answer to "are there base-coloured solid layers under the painted skin,
or sparse infill closer to the surface than it should be?"**

- **Without** the Stage-2 OR (i.e. `pdmUnlimited`, or pre-feature): the base region at layer `L-N`
  is *not* a top surface (the painted region sits above it in `lslices`), so it stays **sparse
  infill butted directly against the bottom of the painted claim** — a bare colour Z-interface. That
  is bleed path (c) from the design spec, and it is the *pre-existing* behaviour.
- **With** the Stage-2 OR (default, `pdmWalls`): the base region at `L-N` **is** a top surface of
  its own region, so it grows its own solid top shell downwards. The painted region likewise gets an
  `stBottom` at `L-N+1`. Result: **solid painted skin over solid base shell**, no sparse infill at
  the colour interface.

So yes — there *are* base-coloured solid layers directly under the painted skin. They are **below**
the painted claim, i.e. hidden by it, and they are the *fix*, not the defect. Our feature improves
the vertical case rather than harming it.

### The real gap: `*_shell_thickness` is invisible to the segmentation

**Verified asymmetry:**

- `discover_vertical_shells` gathers the solid shell with **both** conditions —
  `PrintObject.cpp:1954-1967`:
  ```cpp
  for (; i < int(cache_top_botom_regions.size()) &&
         (i < itop || m_layers[i]->print_z - print_z < region_config.top_shell_thickness - EPSILON); ++i)
  ```
  and the mirror for bottom at `:1989-1996`. Same pattern at `:4144-4146`.
- `segmentation_top_and_bottom_layers` uses **only** the layer counts (`:1210-1211`, `:1366-1367`,
  loop bounds `:1400` / `:1420`). `top_shell_thickness` appears nowhere in
  `MultiMaterialSegmentation.cpp` (grep: zero hits).

**With stock defaults** (`top_shell_layers = 4`, `top_shell_thickness = 0.6 mm`):

| layer height | solid shell (max of count, thickness) | painted claim | gap |
|---|---|---|---|
| 0.20 mm | 4 layers (0.8 mm ≥ 0.6) | 4 | 0 — fine |
| 0.10 mm | **6** layers (0.6 mm bound wins) | 4 | **2 layers** |
| 0.08 mm | **8** layers | 4 | **4 layers** |

**Inferred** (reasoning over the verified code, not separately executed): in the gap layers the
solid shell is base-coloured rather than painted. Because Stage 2 forces the per-region
classification, those base layers *are* solid — so the practical symptom is a colour/material
boundary sitting `N` layers below the surface with base-coloured solid infill beneath it, not a
sparse-infill-too-close-to-the-surface defect. The visible top skin is painted correctly in all
cases. The residual risks are (i) show-through if the painted skin is thin and the filaments are
strongly contrasting, and (ii) the same issue in the degenerate config `top_shell_layers = 0` +
`top_shell_thickness > 0`, where `:1225` skips the projection wholesale and the painted top face
gets **no** vertical claim at all while a solid shell is still generated.

---

## 4. What a minimal fix would touch

Goal: *"a painted top/bottom claims at least the full solid shell depth."* The clean statement is to
make the segmentation's shell-layer count agree with `discover_vertical_shells`' effective count.

### Primary anchors (the whole fix could live here)

1. **`src/libslic3r/MultiMaterialSegmentation.cpp:1354-1381` — `layer_color_stat`.**
   The single best insertion point. `:1366-1367` currently take
   `max(config.top_shell_layers)` / `max(config.bottom_shell_layers)`. Replace with an *effective*
   count that also satisfies `*_shell_thickness`, mirroring `PrintObject.cpp:1961` / `:1990`. The
   lambda already captures `layers` and `print_object`, so `m_layers[i]->print_z` /
   `bottom_z()` are reachable — the same walk `discover_vertical_shells` does, or a cheaper
   `ceil(thickness / layer.height)` for uniform layer heights (variable layer height needs the walk).
2. **`src/libslic3r/MultiMaterialSegmentation.cpp:1205-1213` — the object-wide maxima.**
   `max_top_layers` / `max_bottom_layers` gate the *entire* projection at `:1225`, and `granularity`
   at `:1212` sizes the TBB double-buffer. **Both must be raised in step with (1)** or the deeper
   claim will be computed but the parallel_for's parity buffers will be undersized relative to the
   group span. Also the place to fix the `top_shell_layers == 0 && top_shell_thickness > 0`
   degenerate case (make the gate `effective_top_layers > 0`).
3. **`src/libslic3r/MultiMaterialSegmentation.cpp:1400` and `:1420` — the loop bounds.**
   No change needed *if* (1) supplies the effective count via `stat.top_shell_layers`; listed
   because they are where the count is finally consumed, and because the strict `>` at `:1400` plus
   `max(..., 0)` means layer 0 is never claimed through the top path (a separate, pre-existing edge
   case worth a deliberate decision).

### Secondary anchor (only if a taper-free guarantee is wanted)

4. **`src/libslic3r/MultiMaterialSegmentation.cpp:1403-1407` — the inward taper and its `break`.**
   `offset -= (extrusion_spacing + extrusion_width)` per layer plus `if (last.empty()) break;` can
   cut the claim short before the nominal count is reached, for islands near the contour. Deepening
   the count alone will not help those. Leave alone unless a case is demonstrated — the taper exists
   to stop wall lines overlapping (comment `:1401`).

### Interaction with our own feature — check all three before changing anything

- **`has_bounded_paint_depth()` / `interface_shells` OR-in** —
  `Print.hpp:495`; read sites `PrintObject.cpp:1322`, `PrintObject.cpp:1755`,
  `PerimeterGenerator.cpp:622`, `PerimeterGenerator.cpp:2273` (fed from `LayerRegion.cpp:227` via
  `PerimeterGenerator.hpp:98-103`).
  A deeper painted claim **moves the colour Z-interface downwards** but does not remove it, so the
  OR-in stays necessary and its behaviour is unchanged. Note the direction of benefit: deepening the
  claim reduces how much *extra* solid the forced `interface_shells` conjures, since the painted
  region would then already span the full shell. The two changes are complementary, not redundant.
- **`paint_infill_override` / `paint_sparse_infill`** — `PrintApply.cpp:1862` (gate),
  `:1092-1099` (generate path), `:839-840` (region-reuse path), `:756` / `:990` (signatures).
  This only affects `sparse_infill_filament`. Deepening the top/bottom claim enlarges the painted
  region's footprint, so **more** volume falls under the override. With `paint_infill_override =
  false` and a deepened claim you would get painted walls + painted solid infill + **base** sparse
  infill in the deepened layers — but those layers are inside the solid shell, so they contain
  little or no sparse infill anyway. Low-risk; worth a fixture check rather than a code change.
- **`cut_segmented_layers` / `paint_depth_*`** — `MultiMaterialSegmentation.cpp:1146-1181`,
  `:2181-2184`, `:2229-2247`, `PaintDepth.hpp/.cpp`. **Untouched by any fix in §4**: per §2 the two
  paths are disjoint, and the top/bottom claim is unioned in after the clamp. No ordering change is
  required, and none should be attempted — moving `cut_segmented_layers` after step 4 or 5 *would*
  create exactly the interior-island deletion this investigation ruled out.
- **Invalidation** — `PrintObject.cpp:956-972` already lists `paint_depth_mode`,
  `paint_depth_walls`, `paint_depth_mm`, `paint_infill_override` under `posSlice`.
  `top_shell_layers` / `top_shell_thickness` are handled at `PrintObject.cpp:1100-1101`; if the
  segmentation starts consuming `*_shell_thickness`, confirm those keys reach `posSlice`
  invalidation too (currently they invalidate later stages — **this is the one thing a fix must not
  forget**, or a thickness edit will not re-run the segmentation).

### Testing hooks that already exist

`tests/libslic3r/test_paint_depth_clamp.cpp` (band width + even/odd non-alternation pins),
`tests/libslic3r/test_paint_depth.cpp` (config-level legacy-migration pin), and
`spike/verify_paintdepth.sh` (17 checks incl. unpainted byte-parity against
`spike/out/paintdepth_baseline.gcode`). A vertical-depth fix wants a new fixture: a flat plate lying
down with an interior painted spot on its top face, sliced at 0.1 mm layer height with the default
`top_shell_layers = 4` / `top_shell_thickness = 0.6`, asserting the painted region spans 6 layers,
not 4. The unpainted-parity check in `verify_paintdepth.sh` should hold unchanged.
