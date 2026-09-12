# ImageMap Phase 4: side walls, the preview swatch, ironing and Arachne top surfaces

Date: 2026-09-12
Branch: `feat/imagemap-p4-walls`, from `feat/ultra-preferences` @ `944457b8bb`
Worktree: `C:\Dev\wt_imgp4`

Phases 1-3 are merged. Phase 3
(`docs/superpowers/specs/2026-09-07-imagemap-phase3-imagerow.md`) gave nozzle-resolution
dithering on **top solid surfaces only**, and its own "What is NOT wired" section listed the
four gaps this phase closes: side walls, ironing, Arachne/Concentric top surfaces, and the
3D preview colour swatch.

---

## 0. Summary of what changed

| File | What |
| --- | --- |
| `src/libslic3r/ImageRowWalls.{hpp,cpp}` | **New.** The wall context, the per-segment wall normal, and the multi-path arc-length cutter that the wall split, the ironing split and the Arachne top-surface split all share. |
| `src/libslic3r/GCode.cpp` | The wall split itself, inside the existing per-island `PERIMETERS` block; plus the per-region context cache; plus hoisting the existing per-run override check above its `INFILL`-only gate so perimeters honour it. |
| `src/libslic3r/GCode/ToolOrdering.cpp` | Registers an image-row wall region's candidate filament set into each layer's tool list. |
| `src/libslic3r/Fill/Fill.cpp` | Step 4 (Arachne/Concentric top surfaces) and step 3 (ironing), both by delegating to the new cutter. |
| `tests/fff_print/test_image_row_preview.cpp` | **New.** Pins the preview swatch (step 2). |
| `tests/fff_print/test_image_row_walls.cpp` | **New.** Four cases: box-projected cube, wrapped cylinder, fuzzy skin, determinism. |

---

## 1. Step 2 first: the preview swatch (done first at the coordinator's request)

**Finding: the swatch was already correct, in both colour views, and no code change was needed.
What was missing was any test that said so.** Phase 3's step-5 write-up asserted this
("already correct, verified rather than changed") but verified it only by eyeballing `T<n>`
counts in one Bar B slice; nothing in the suite would have caught a regression.

The chain, established by reading rather than assuming:

- `GCodeViewer::refresh_render_paths()`'s `extrusion_color` lambda
  (`src/slic3r/GUI/GCodeViewer.cpp:3267-3299`) is the single place an extrusion's colour is
  chosen. The two colour-relevant views read different fields:
  - `EViewType::Tool` -> `m_tools.m_tool_colors[path.extruder_id]` (`:3280`)
  - `EViewType::ColorPrint` -> `m_tools.m_tool_colors[path.cp_color_id]` (`:3285`)
- `Path::extruder_id` / `Path::cp_color_id` are copied verbatim from the `MoveVertex`
  (`GCodeViewer.cpp:228`).
- A `MoveVertex` takes them from the processor's live state at `store_move_vertex()`
  (`GCodeProcessor.cpp:4806-4807`): `m_extruder_id`, which is set **only** by `process_T()`
  reading a real `T<n>` out of the G-code, and `m_cp_color.current`.
- With no colour change in the print, `m_extruder_colors[i] == i`
  (`GCodeProcessor.cpp:977-979`), so `cp_color_id == extruder_id` and the two views agree.

So the preview is correct **because** the image row's override reaches it through the G-code's
own tool changes, not through any `ExtrusionEntity` attribute - the viewer has no channel to
learn about `image_row_extruder_1based` and does not need one. Phase 3's `ToolOrdering` /
`GCode.cpp` wiring emits the right `T<n>` per run, and the preview follows.

### The hazard that is real, and why it does not fire

`Plater::get_extruder_colors_from_plater_config()` appends the **mixed (virtual) filaments'**
display colours after the physical ones (`Plater.cpp:24304-24312`), and `GCodeViewer.cpp:5113`
calls it with `include_mixed` defaulting to true. So `m_tool_colors` is laid out
`[physical 0..N-1][virtual N..N+M-1]`. If a *virtual* id ever reached a `MoveVertex`, the Tool
view would index into that tail and paint the run with the row's **blended average display
colour** - which is exactly the "region's nominal colour" failure this step was asked about.

It does not happen, because `configured_extruder_id` (`GCode.cpp`) resolves
`image_row_extruder_1based` to a physical index before emission, and `process_T` refuses any id
`>= m_result.extruders_count` anyway (`GCodeProcessor.cpp:4005`). But that is two independent
guards away from a wrong swatch, so the new test asserts it directly.

### What ships

`tests/fff_print/test_image_row_preview.cpp`, one case, on phase 3's own Bar B plaque
(50 x 50 x 3 mm, `ramp_kw.png`, three filaments, Planar/Z). It slices to G-code and captures the
very `GCodeProcessorResult` the preview is built from (`Print::export_gcode()`'s second
parameter is the same object `GLCanvas3D` hands `GCodeViewer::refresh()`), then asserts on the
top layer's `erTopSolidInfill` moves:

1. **more than one `extruder_id`** across the surface - the preview is not painting it one flat
   nominal colour;
2. **several transitions within the layer** - it changes at run boundaries, not once per layer
   (an un-split row falls back to `resolve()`'s per-layer cycle, which gives exactly one
   filament for the whole layer and therefore zero in-layer transitions);
3. **`cp_color_id == extruder_id` on every move** - the ColorPrint view shows the same colours
   as the Tool view, rather than `GRAY()` or a stale colour-change slot;
4. **every id physical, and never the row's virtual id** - the hazard above.

Two incidental defects found while reading, **not fixed** (out of scope, pre-existing, and each
would be its own change): `m_tool_colors[path.extruder_id]` at `GCodeViewer.cpp:3280` and
`:5132` is an unguarded `std::vector` index where every other lookup in that switch is
bounds-checked; and the ColorPrint visibility filter at `GCodeViewer.cpp:3406` gates on
`extruder_id` while colouring on `cp_color_id`.

---

## 2. Step 1: side-wall dithering

### The design decision that shaped everything: where the split runs

The brief required the loop's seam to survive ("keep the loop's seam position; runs are just
colour boundaries"). That single constraint rules out the obvious placement.

A wall's seam is chosen at **G-code time** by `SeamPlacer::place_seam()`
(`GCode.cpp:7599`, inside `GCode::extrude_loop`), long after `PerimeterGenerator` has run. A
split done in `LayerRegion::make_perimeters` - the natural mirror of `Layer::make_fills`, and
where phase 3's fill split lives - would hand G-code a set of open fragments instead of a
closed loop, `place_seam` would never run on them, and **the first run boundary would become
the de facto seam**: a visible scar on every layer, moving with the image.

So the split runs at G-code time instead, inside the existing per-island `PERIMETERS` block in
`GCode.cpp`, immediately before the fork's own outer/inner wall splitter. The existing
`local_z_loop_seam_placer` callback (declared just above that loop, `GCode.cpp:6068-6079`, and
already used by the local-Z clipper for exactly this reason) rotates the loop to its seam
first; only the already-rotated geometry is then cut. A run boundary is therefore a colour
boundary and nothing else.

Two things fall out of this placement for free:

- **Fuzzy skin is already applied.** Fuzz happens inside `PerimeterGenerator`
  (`Feature/FuzzySkin/FuzzySkin.cpp`, called from `PerimeterGenerator.cpp:234` and `:498`), not
  at G-code time. So the geometry being sampled is already jittered, and a run boundary is
  placed on the path the nozzle actually follows rather than on a smooth path the jitter then
  moves out from under it. The brief's "sample after fuzz" is satisfied by construction.
- **Overhang sub-paths keep their attributes.** `PerimeterGenerator` retags parts of an outer
  loop `erOverhangPerimeter`; the cutter walks the original `ExtrusionPath` sequence and emits
  one piece per contiguous run of segments sharing a source path, so an overhang piece still
  prints as an overhang at overhang speed.

### What a split wall looks like

A split loop necessarily stops being a closed `ExtrusionLoop` - there is no such thing as a
partially-coloured loop. Each run becomes its own `ExtrusionEntityCollection` (`no_sort = true`,
so the runs cannot be reordered) holding one `ExtrusionPath` or `ExtrusionMultiPath`, tagged with
`image_row_extruder_1based`. The pieces describe exactly the original geometry, in order,
starting at the seam, so the nozzle follows the same path - it just changes filament part-way
round.

The cost of no longer being an `ExtrusionLoop` is the loop's **seam-gap clipping and scarf
joint**, which only apply on the `ExtrusionLoop` branch of `GCode::extrude_loop` (`:7621`,
`:7603`). This is the same trade the local-Z clipper already makes and is recorded here as a
known deviation, not an oversight.

### The wall normal - the one genuinely new piece of geometry

Phase 3's top-surface sampler hands `image_fill_project()` one synthesised "up" normal for a
whole surface, because a top surface is one plane. A wall loop turns corners, and on a Box
(tri-planar) projection different segments of the same loop belong to **different box faces**.
So the normal is recomputed per segment, from the loop's local tangent rotated 90 degrees in XY,
with the sign fixed by the loop's winding (`make_counter_clockwise()` is guaranteed by
`PerimeterGenerator.cpp:380` and `:703`; a hole loop winds the other way and its "outward"
normal points into the bore - which is what `axis_negative` selects for a cylindrical
projection on a bore).

`ImageRowWallContext::fixed_normal_mesh` overrides this per-segment computation for callers that
already know the surface's normal, which is how the ironing and Arachne-top-surface reuses below
avoid getting a wall normal on a surface that faces up.

### The `ToolOrdering` asymmetry (a real difference from how phase 3 wired fills)

`ToolOrdering::collect_extruders` runs **before** G-code emission. For fills that is fine: the
split happened during slicing, so the loop at `ToolOrdering.cpp:880` can read
`image_row_extruder_1based` straight off a collection the slicer already produced. For walls the
runs **do not exist yet**.

So the perimeters branch registers the row's whole **candidate filament set**, read from config
(`image_row_wall_candidate_filaments()`), rather than the per-run tags. That set is by
construction a superset of what the runs use - both are built by the same filter - and it is
what makes `GCode.cpp`'s `layer_tools.has_extruder(correct_extruder_id)` check find each run's
filament in the layer's tool list. Without it a run would be silently reassigned to
`layer_tools.extruders.back()`, which is the precise failure mode phase 3's own spec flags as
"the fix that matters".

Over-registering is harmless: `reorder_extruders()` deduplicates and reorders the list for
minimum switches, and an extruder with no extrusions on a layer emits no tool change. It is
recorded here because it is the one place phase 4's wiring is *not* a mirror of phase 3's, and
anyone reading the two side by side will notice.

### Tool-change economics

Unchanged from phase 3, and the reason it still holds is worth restating: G-code emission groups
by extruder for the **whole layer** (the outer loop iterates `layer_tools.extruders`, already
reordered for minimum switches, and emits every entity across every region that resolves to each
one). Since each run is its own tagged collection, this pre-existing mechanism buckets wall runs
exactly as it buckets fill runs. Per-layer tool changes are bounded by
`layer_tools.extruders.size()`, **not** by the number of runs or loops. The wall tests assert
this bound directly.

### The first inner perimeter

`ImageRowWallContext::split_first_inner` claims `inset_idx == 1` as well as the outer loop, when
the region has more than one wall loop. A single outer line is often translucent enough that the
layer below shows through and the dither reads muddy; backing it with the same colour doubles
the opacity. It costs no extra tool changes, because runs are grouped per filament per layer -
an inner run of filament F is free once F is already being visited for the outer runs.

---

## 3. Step 3: ironing - wired, not skipped

The brief allowed either "apply the row split to ironing passes" or "explicitly skip with a note
in the tooltip". **Wired**, for a reason that only became clear on reading the code: ironing is
a cover pass over already-printed material, so the ironed skin is *the layer you actually see*.
Leaving it on the row's per-layer colour cycle meant smearing one flat colour over a correctly
dithered surface - the feature finishing by hiding itself.

Phase 3's stated obstacle ("ironing's own fill loop has different geometry semantics") turned out
not to apply to the split: `Layer::make_ironing` emits plain `ExtrusionPath`s into a `no_sort`
`ExtrusionEntityCollection` (`Fill.cpp`, the `extrusion_entities_append_paths(eec->entities, ...,
erIroning, ...)` call), which is structurally what the splitter already handles. The split is
therefore the same operation, keyed on `erIroning` instead of `erTopSolidInfill`.

`image_row_wall_iron_context()` reads `solid_infill_filament` (the key
`Layer::make_ironing` itself uses to pick `ironing_params.extruder`), i.e. the **same row** the
top solid infill beneath the skin is using, and supplies the surface's "up" normal via
`fixed_normal_mesh`. Sampling resolution is the ironing pass's own extrusion width, which is
narrower than a top-infill line - so the ironed skin resolves the image at least as finely as
the surface under it, never more coarsely.

`log_image_row_ironing_not_split_once()`'s warning is now only reachable when the split itself
declines (one colour, or the projection declines everywhere).

---

## 4. Step 4: Arachne / Concentric top surfaces

`split_top_infill_by_image_row()` previously recognised only a plain `ExtrusionPath` tagged
`erTopSolidInfill`. A Concentric-family or otherwise Arachne-driven top surface produces an
`ExtrusionMultiPath` or `ExtrusionLoop` instead, so those surfaces fell back to the row's
per-layer colour cycle (and said so, once, in the log).

They are now split by the **same multi-path cutter the walls use**, which is exactly what a
variable-width Arachne path needs: it walks a sequence of `ExtrusionPath`s and cuts by arc
length while keeping each piece's own attributes, so a cut does not homogenise a width that
varies from path to path. The top surface's own "up" normal is passed via `fixed_normal_mesh`,
so a Box projection picks the top face rather than a side face computed from the path's tangent.

A Concentric top-surface **loop** is cut open by this, the same way a wall loop is. That is the
intended trade; the alternative is phase 3's behaviour, i.e. no dither on those patterns at all.

---

## 5. Tests

`tests/fff_print/test_image_row_walls.cpp`, four cases, all measured on **G-code** rather than on
the `ExtrusionEntity` tree - which is not a stylistic choice. The wall split happens at G-code
emission time, so there is no tagged tree left to inspect afterwards; the G-code (and the
`GCodeProcessorResult` the preview reads) is the only place the result exists. That also makes
these the honest end-to-end test: a run shows up as its own tool only if the split produced it,
`ToolOrdering` registered its filament, **and** `GCode.cpp` resolved the override into a real
`T<n>`.

1. **Box-projected stripes on a cube** (`bands3.png`, three vertical R/G/B bands, three saturated
   filaments, 30 mm cube). Asserts the mid-height layer's outer wall uses >= 2 filaments, changes
   colour >= 2 times going round, uses only physical ids (never the row's virtual id), and that
   the layer's tool changes stay within `2*(filaments-1)`.
2. **Wrapped gradient on a cylinder** (`wrap4.png`, cylindrical/Z). Buckets the layer's wall
   moves by angle about the part's centre and asserts the dominant filament is not constant
   around the loop - i.e. the colours are in the right *places*, not merely present.
3. **Fuzzy skin** (same cube, `fuzzy_skin = External`). Asserts fuzz does not collapse the split
   (still multi-coloured), reaches the same number of distinct filaments, and produces a
   transition count of the same order - deliberately a tolerance, since fuzz lengthens the path;
   only a collapse or a per-jitter-tooth fragmentation fails.
4. **Determinism**: two runs of the same project produce byte-identical G-code once the
   generation timestamp and `M73` time estimates are stripped.

`tests/fff_print/test_image_row_preview.cpp` is step 2's case, described in section 1.

---

## 6. Results, deviations and what is NOT verified

### Tests

| Suite | Result |
| --- | --- |
| `fff_print_tests` (whole suite) | **95 cases, 4072 assertions, all pass** - includes the 5 new image-row cases |
| `libslic3r_tests` (whole suite) | **955 cases, 952 passed, 3 failed as expected** |
| `libslic3r_tests "[imagefill],[ImageRow]"` | **29 cases, 5789 assertions, all pass** - the phase 2/3 tags, unchanged |

The 3 expected failures are the only three `[!mayfail]`-tagged cases in the tree
(`test_voronoi.cpp`'s "Voronoi NaN coordinates 12139" and `test_wipe_tower_estimate.cpp`'s
"A second nozzle adds the ramming of one nozzle change per layer"); both are pre-existing and
unrelated. Neither new test file uses that tag.

### Measured cost on the test cube

A 30 mm cube, `bands3.png` (three vertical R/G/B bands) box-projected, three filaments, row bound
to `wall_filament`; sliced twice from the same fixture with the row bound and unbound (the
`[.measure]` case in `test_image_row_walls.cpp`, not run by default).

| | row OFF | row ON |
| --- | --- | --- |
| Layers | 150 | 150 |
| `T<n>` commands in the file | 1 | 301 |
| Tool changes per layer, max | 0 | **2** |
| Tool changes per layer, mean | 0 | **2.00** |
| Distinct tools on a layer, max | 1 | 2 |
| Filament used (mm) | 3746.75 / 0 / 0 | 2932.79 / 400.36 / 414.01 |
| Estimated print time | **1h 22m 40s** | **1h 50m 41s** |

**Print-time delta: +28m 01s, i.e. +33.9%** - which is the cost of 300 tool changes on a print
that previously needed none, not the cost of the extra geometry (the split reproduces the same
path; only the filament changes part-way round).

**The tool-change bound holds exactly**: every one of the 150 layers has precisely 2 changes,
against a bound of `2*(filaments-1) = 4`. This is the economics rule working as designed - the
cube's wall is cut into many colour runs per layer, but the per-layer per-extruder grouping
visits each needed filament once, so tool changes track *filament count*, not run count.

One detail worth recording because it looks wrong at first glance: **only 2 distinct tools appear
on any single layer, yet all three filaments are used across the print.** That is the Box
projection behaving correctly - `bands3.png` varies with `u` and is constant in `v`, so every
layer's wall crosses the same set of bands, and which two dominate depends on where the loop
runs. A projection that varied along the build axis would put all three on each layer.

### Two bugs the tests caught, both in this phase's own code

1. **`no_sort` stopped perimeters reaching the nozzle.** The per-run collections were created
   `no_sort = true`, on the reasoning that runs describe one continuous path. But
   `ObjectByExtruder::Island::Region::append()` splats a *sortable* collection's children into the
   flat `perimeters` list and pushes a *non*-sortable one in whole; `extrude_perimeters()` then
   hands each element to `extrude_entity()`, which accepts only
   `ExtrusionPath`/`ExtrusionMultiPath`/`ExtrusionLoop` and throws on a collection. All four wall
   cases died with `Invalid argument supplied to extrude()`. Fills descend into nested
   collections; perimeters have no such handling - which is exactly why phase 3's fill runs could
   set `no_sort` and these cannot. Nothing is lost by clearing it: each run collection holds
   exactly one entity, and the ordering that matters comes from the per-layer per-extruder
   grouping, not from this flag.
2. **A 0-based / 1-based mix-up in the dispatch.** `image_row_extruder_1based` is 1-based; the
   island bucket key is 0-based (the canonical dispatch keys it on `correct_extruder_id`, i.e. on
   `LayerTools::wall_filament()`, documented there as "a zero based extruder"). Found by tracing
   rather than by a failure, and fixed before the tests ran.

### An off-by-one found in passing, deliberately NOT fixed

`layer_tools.extruders` is **1-based** (`ToolOrdering.cpp:938` and `:941` register
`solid_infill_filament()`/`sparse_infill_filament()` **+ 1**, and phase 3's fills loop at `:905`
registers a 1-based id directly), but every `layer_tools.has_extruder()` call on the emission
path - the canonical one included - passes a **0-based** id. So that check usually misses and
falls back to `extruders.back()`. This is pre-existing and file-wide, not introduced here. The
wall split deliberately matches the canonical path's convention rather than "correcting" it
locally, because behaving exactly like every other entity is the property that matters; fixing
the underlying discrepancy needs its own change and its own Bar A.

### Bar A (feature off, byte-identical)

**Not run as a G-code diff**, because it needs the full GUI slicer binary and the
`snorca_hubtest` harness, neither of which is in this worktree. What is established instead:

- **Structurally**, every new path is gated on `image_row_wall_configured_virtual_id()` (or its
  ironing/solid-infill equivalents) returning non-zero, which requires an *enabled*
  `ImageWeighted` row with a decodable `image_fill_ref` named by the region's wall or
  solid-infill filament. `MixedFilamentManager::is_mixed()` is false for every physical id, so no
  ordinary slice can reach any of it. The per-region context is memoised, so an ordinary region
  costs one integer comparison per layer. The two `GCode.cpp` lambda hoists are gated on
  `image_row_extruder_1based != 0`, which is 0 on every collection the feature did not create.
- **Empirically**, the whole 95-case `fff_print` suite and 955-case `libslic3r` suite pass, which
  is a large body of ordinary slicing exercised against these changes.

That is an argument plus broad regression coverage, not the byte-identity proof phases 1-3
recorded. **It is the most significant gap in this phase's acceptance** and should be run before
merge.

### Known deviations from the brief, decided deliberately

- **A split wall is no longer an `ExtrusionLoop`**, so it loses the loop's seam-gap clipping and
  scarf joint (section 2). The seam *position* is preserved, which is what the brief asked for.
- **`ToolOrdering` registers the candidate set, not the per-run tags** (section 2), because the
  runs do not exist when it runs. This is the one place phase 4 is not a mirror of phase 3.
- **Ironing was wired rather than skipped** (section 3); the brief permitted either.

### Carried forward unchanged from phase 3

- Sample spacing and run-length cuts treat mesh-space arc length as equal to print-space arc
  length, exact only for a uniform scale.
- Inner perimeters beyond the first are never split.

### Not verified

- **No hardware print.** Phase 3's "Hardware test for the owner" remains the actual acceptance
  bar, now extended to a part whose image lands on its sides: a cube with a box-projected
  two-colour image, printed with the row bound to the walls, compared against the same cube with
  the row unbound.
- **Nobody clicked the dialog.** The GUI has no control for binding an image row to WALLS - the
  phase 3 checkbox binds `solid_infill_filament` only. Everything here is reachable from a
  project that names the row on `wall_filament`, which the tests do through the model-level API;
  exposing it in `ImageFillDialog` is the obvious next change and is not attempted here.
- **`libslic3r_gui` was not built**, because no GUI file was touched by this phase.
