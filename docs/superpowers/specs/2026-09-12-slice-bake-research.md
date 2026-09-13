# Slice baking: research spec

Research only. No code changes. Goal: turn a sliced object's *printed appearance* (outer
perimeters as extruded, optionally fuzzy skin / Z contouring / seams / painted-color
boundaries) into a new watertight mesh that can be re-sliced, excluding supports, brim,
skirt, wipe tower, and internal infill.

## 1. What geometry exists after slicing

### Perimeter loops (pre-G-code)

`PrintObject::make_perimeters()` (`src/libslic3r/PrintObject.cpp:297`) drives the pipeline:
after `slice()`, it calls `Layer::make_perimeters()` per layer in parallel
(`PrintObject.cpp:401`), which calls `LayerRegion::make_perimeters(...)`
(`src/libslic3r/Layer.hpp:87`, impl in `src/libslic3r/Layer.cpp` /
`PerimeterGenerator.cpp`). The output lives on `LayerRegion`:

- `LayerRegion::perimeters` — `ExtrusionEntityCollection` of `ExtrusionLoop`s, one
  collection of collections per island (`Layer.hpp:72`).
- `LayerRegion::fills` — infill, incl. `erTopSolidInfill`/`erBottomSurface` solid
  surfaces (`Layer.hpp:76`).
- `LayerRegion::thin_fills`, `fill_expolygons`, `slices`/`raw_slices` — gap fills and the
  raw per-region slice polygons (`Layer.hpp:44-61`).

Each `LayerRegion` belongs to one `PrintRegion` (`m_region`, `Layer.hpp:38,122`), and a
`PrintRegion` carries `print_object_region_id()` (`src/libslic3r/Print.hpp:175`) — this is
the hook for painted/MMU color regions: a painted object is split into multiple
`PrintRegion`s at slice time, so per-loop region id is already available before G-code.

`ExtrusionLoop`/`ExtrusionPath` (`src/libslic3r/ExtrusionEntity.hpp:130+`) carry `role`
(`ExtrusionRole`, line 21), `width`, `height` per path, and a `Polyline` (2D, scaled
integer `Points`). Loop role is one of `erPerimeter` (inner), `erExternalPerimeter`
(outer wall), `erOverhangPerimeter`, `erOverSupportPerimeter` (`ExtrusionEntity.hpp:24-31`).

**Fuzzy skin** modifies the loop's `Points` directly, at this same pre-G-code stage:
`Feature::FuzzySkin::apply_fuzzy_skin(const Polygon&, ...)` and the `Arachne::ExtrusionLine`
overload (`src/libslic3r/Feature/FuzzySkin/FuzzySkin.hpp:23-24`, impl
`FuzzySkin.cpp:433`, `:534`) are called from `PerimeterGenerator.cpp:235` and `:498`,
i.e. inside perimeter generation, before the loop is ever handed to `GCode.cpp`. **This
means fuzzy-skin jitter is fully present in the `ExtrusionLoop` polyline** — a bake from
perimeter loops reproduces it exactly, with no G-code route needed.

**Z contouring ("ZAA")** is the opposite: it is a G-code-time effect. Per
`src/libslic3r/ContourZ.hpp:15-35`, the contoured Z delta is *not* baked into the
`ExtrusionPath`'s 2D `Polyline` (this fork intentionally kept the pre-ZAA 2D `Polyline`,
unlike upstream's `Polyline3` change — see the "MINIMAL VARIANT" note at
`ContourZ.hpp:21-35`). Instead each contoured path is associated with a separate,
immutable `ContourZSamples` (`ContourZ.hpp:36-70`) keyed by point coordinates, consulted
during G-code emission (`GCode.cpp`) to raise/lower Z and rescale extrusion/feedrate
(`contour_z_sample_delta`, `contour_z_extrusion_ratio`, `ContourZ.hpp:269,278`). **The
perimeter loop geometry at the end of `make_perimeters()` has no Z variation within a
layer** — contouring only exists once G-code positions are computed.

**Seams** are also decided at the G-code stage, not in the stored perimeter loops. The
loop is a closed ring with an arbitrary start point until `GCode.cpp`'s seam placer
(`SeamPlacer.cpp`/`.hpp`, referenced at `GCode.cpp:3262` `m_seam_placer.init(...)`)
picks a split point per loop instance during extrusion (`GCode.cpp:4350-4386`,
`local_z_order_clipped_paths_for_seam`, `LocalZLoopSeamPlacer`). The stored
`ExtrusionLoop` has no persisted "this is the seam point" flag usable pre-G-code.

**Top/bottom solid surfaces**: `LayerRegion::fill_surfaces` / `fills` after
`prepare_fill_surfaces()`/infill generation carry `erTopSolidInfill` and
`erBottomSurface` (`ExtrusionEntity.hpp:34-35`); these close the top/bottom skin and are
available pre-G-code exactly like perimeters.

### Summary: bakeable before G-code vs. G-code-only

| Effect | Stage | Source |
|---|---|---|
| Outer/inner perimeter shape, width/height | pre-G-code | `LayerRegion::perimeters` |
| Fuzzy skin jitter | pre-G-code (modifies loop points) | `PerimeterGenerator.cpp:235,498` → `FuzzySkin.cpp` |
| Top/bottom solid surfaces | pre-G-code | `LayerRegion::fills` (`erTopSolidInfill`/`erBottomSurface`) |
| Painted-region boundary (color) | pre-G-code | `PrintRegion::print_object_region_id()` per `LayerRegion` |
| Z contouring (ZAA) | G-code only | `ContourZSamples`, applied in `GCode.cpp` |
| Seam position/travel | G-code only | `SeamPlacer`/`GCode.cpp` seam split logic |
| Wipe moves | G-code only | never geometry that should be baked |

For the two G-code-only effects (contouring, seams), baking needs the **G-code
processor's path geometry** instead of `LayerRegion`: `GCodeProcessorResult::MoveVertex`
(`src/libslic3r/GCode/GCodeProcessor.hpp:164-197`) already carries `position` (Vec3f,
with real per-move Z — this is where contouring becomes visible), `width`, `height`,
`extrusion_role`, `extruder_id`, and arc-interpolation points. This is literally "what
the machine actually prints" — GCodeViewer builds its preview triangle strips from this
same `moves` vector (`src/slic3r/GUI/GCodeViewer.cpp`, e.g. the per-move role/width/height
consumption around lines 963, 1048, 2484). A G-code-route bake would walk `moves`,
filter to extrusion moves of the wanted roles, and build capsule/box segments from
consecutive `position`s using the recorded `width`/`height` — capturing contouring (real
Z per point) and the seam (implicit in move order/position) for free, at the cost of
losing the clean, closed-loop structure the perimeter route has.

## 2. Methods to build a watertight mesh from extrusion paths

**(a) Swept-capsule + boolean union.** For each `ExtrusionPath` segment, build a stadium
(rounded-rectangle) prism of `width`×`height` swept along the segment, then `union` all
prisms into one solid via `MeshBoolean` (Manifold backend available:
`src/libslic3r/MeshBoolean.hpp:100-104`, `mcut` fallback at `:75-98`). Correct in
principle but expensive: a 100×100×100 mm part at 0.2 mm layers, ~1 mm perimeter segments
easily has 300-800k perimeter segments; each is its own boolean union operand. Even a
fast incremental-boolean library batches best in the thousands, not hundreds of
thousands — expect this to be the slowest option by 1-2 orders of magnitude, and prone to
degenerate-geometry failures at self-tangent walls. Not recommended as primary.

**(b) Voxel / level-set rasterization.** Per layer, for each outer-wall extrusion,
rasterize the swept capsule/box (footprint = loop polyline offset by ±width/2, extruded
over the layer's Z range) into an `openvdb::FloatGrid` signed distance field via
`mesh_to_grid` (`src/libslic3r/OpenVDBUtils.hpp:29`), union grids by min-SDF across
layers/paths, then `grid_to_mesh` (`OpenVDBUtils.hpp:36`, wraps `volumeToMesh`) at a
voxel size ≈ 0.5× layer height (e.g. 0.1 mm at 0.2 mm layers). Optionally a
morphological closing (dilate+erode, achievable via `redistance_grid` at shifted
isovalues, `OpenVDBUtils.hpp:41`) fills the inter-layer stair grooves for a smooth
variant. **Memory/time estimate** for 100×100×100 mm at 0.1 mm voxels: grid resolution
1000×1000×1000 nominal, but OpenVDB is sparse (only near-surface leaf nodes, 8³ voxels
each, populated) — a thin shell surface at this resolution typically touches on the
order of 10³-10⁴ leaf nodes (~a few hundred thousand active voxels total, not 10⁹), so
memory is roughly tens to a few hundred MB and wall time is dominated by per-layer
rasterization (500 layers × rasterize-and-min-union), expect low-single-digit minutes on
a modern desktop — much cheaper than (a) and tractable for typical parts. This is a good
candidate for the **smooth** bake mode, and the only practical route once Z contouring
(continuous, non-layer-aligned Z) needs to be captured, since it doesn't require the
input to be organized into clean per-layer loops.

**(c) Per-layer 2D union + loft (reverse of slicing).** Per layer, take the outer-wall
loop(s), build the extrusion footprint (offset the loop by ±width/2 into an `ExPolygons`
region, or just use the outer perimeter polygon directly since the bake only needs an
outer skin and the interior is going to be solid anyway — see §3), union per layer into
one `ExPolygons`, then loft consecutive layers into a mesh. **This code already exists**:
`Slic3r::slices_to_mesh` (`src/libslic3r/SlicesToTriangleMesh.hpp/.cpp`) takes a
`std::vector<ExPolygons>` (one per layer) plus `zmin`/layer height/first-layer height and
produces an `indexed_triangle_set` by triangulating the free-top / overhang deltas
between consecutive layers and adding straight side walls (`wall_strip`,
`SlicesToTriangleMesh.cpp:15-46`) — i.e. exactly the stair-stepped "reverse slicing" loft
described in the brief. It's currently used for SLA (`Format/SL1.cpp`); nothing in the
name or implementation is SLA-specific, and it operates purely on `ExPolygons` per layer,
so it is directly reusable by feeding it the outer-wall `ExPolygons` computed from
`LayerRegion::perimeters`/`slices` instead of the SLA raster masks. This gives a
stair-stepped mesh that matches the layer geometry exactly (fuzzy skin survives, since
it's baked into the loop points before this step runs); Z contouring does **not** survive
(needs route (b) with G-code moves, or a hybrid: perturb each layer's assigned Z using
the `ContourZSamples`/`MoveVertex` Z before lofting — see Phase 2). Cost is low: it's
O(layers) unions/triangulations, no global boolean solve, and known to already run inside
the existing SLA pipeline at production sizes.

**(d) Shrinkwrap.** Level-set of the raw union (like (b) without the per-layer split),
then a genus-preserving erode/dilate shrinkwrap. This is really a smoothing variant of
(b) — same OpenVDB machinery, same cost profile, so it's not a fifth option so much as
(b)'s "closing radius" knob (§4, Smooth mode). Not treated separately below.

**Recommendation.** Use **(c) as the primary/default (Exact layers mode)**: it reuses
`slices_to_mesh` almost unmodified, is fast, is exact (preserves fuzzy skin, stair
stepping, and — with per-region `ExPolygons` splits — paint boundaries), and has no new
boolean-robustness risk. Use **(b) as the Smooth mode** for users who want a contoured or
non-stair-stepped shell (also the only route that can incorporate Z contouring, phase 2).
Do not pursue (a) — dominated by (b) on both cost and robustness for this volume of
segments.

## 3. Effects to exclude, and how

Only the *outer skin* should be baked; everything else is either omitted or treated as
solid filler:

- **Supports / support interface** — `erSupportMaterial`, `erSupportMaterialInterface`,
  `erSupportTransition` (`ExtrusionEntity.hpp:47-49`). These live in per-object support
  layers (`SupportLayer`), not in `LayerRegion::perimeters`/`fills` at all — excluded
  simply by never touching support layers/collections.
- **Brim / skirt** — `erBrim`, `erSkirt` (`ExtrusionEntity.hpp:45-46`), stored as
  separate `ExtrusionEntityCollection`s on `Print` (skirt) / per-object brim, not inside
  any `PrintObject`'s per-layer regions — excluded by construction (bake only walks
  `PrintObject::layers()`).
- **Wipe tower** — a wholly separate `WipeTower`/`WipeTowerData` structure on `Print`,
  role `erWipeTower` (`ExtrusionEntity.hpp:50`) — excluded by construction.
  - **Internal infill / inner perimeters** — `erInternalInfill`, `erSolidInfill`,
  `erPerimeter` (inset > 0) are not needed for the bake at all: since the goal is a solid
  shell (a re-sliceable object, not a hollow replica of the original infill), the bake
  only needs the **outermost** loop's `ExPolygons` per layer (`erExternalPerimeter`, plus
  `erOverhangPerimeter`/`erOverSupportPerimeter` where they form part of the outer wall)
  and the top/bottom solid caps (`erTopSolidInfill`, `erBottomSurface`,
  `erBottomSurfaceOverSupport`) to close the ends. Everything radially inward of the
  outer loop is filled in solid by construction of the loft/level-set (union with the
  loop's own interior), so inner perimeters and infill are simply never read — cheaper
  than filtering them out after the fact.
- **Capping the interior**: for method (c), take the outer loop's `ExPolygon` (contour +
  holes if the geometry is genuinely tubular, e.g. a printed pipe) per layer directly —
  `slices_to_mesh` already triangulates the free-top/bottom caps between layers of
  differing footprint, so first/last layer and any layer-to-layer footprint change is
  automatically capped. For method (b), the level-set of the closed outer-wall sweep is
  inherently solid (no separate capping step needed).

## 4. UI

Add "Bake slice to mesh…" to the object right-click menu / Prepare-tab object actions,
enabled only when the plate has been sliced (gate on `Print::is_step_done(psSlice)`-style
state, mirroring how existing per-object actions gate on print state — see
`GUI_ObjectList.cpp` for the object-menu pattern already used by `MeshRemesh`'s "Remesh"
dialog, `RemeshDialog.hpp`, as the closest analog: modal options dialog + a background
job). Proposed dialog options:

- **Mode**: Exact layers (loft, method (c)) / Smooth (level set + closing radius,
  method (b)).
- **Include top/bottom** (checkbox, default on).
- **Include seams** (checkbox, default off, disabled/grayed unless G-code route
  available — phase 2).
- **Include Z contouring** (checkbox, default off, disabled/grayed unless G-code route
  available — phase 2; forces the G-code-route bake when checked).
- **Layer subset**: all layers (range picker deferred to a later phase; spec only "all"
  for phase 1).
- **Output**: replace the object with the baked mesh (undoable, like existing
  mesh-modifying operations e.g. Remesh/MeshBoolean gizmos) OR add as a new object OR
  export directly to STL/3MF.

Progress reporting via the existing background-job system (`src/slic3r/GUI/Jobs/`,
pattern e.g. `FillBedJob.cpp/.hpp`, `EmbossJob.cpp` for a mesh-producing job with
progress + main-thread apply-on-finish): a `BakeSliceJob` computes the mesh off the UI
thread and applies/exports on completion.

## 5. Multi-material / color carry-over

Feasible for method (c), out of scope for (b) in phase 1. Each painted/MMU region is a
distinct `PrintRegion` with `print_object_region_id()` (`Print.hpp:175`), and
`LayerRegion::m_region` (`Layer.hpp:38,122`) ties every loop/fill back to its region. To
carry color onto the baked mesh: instead of unioning all outer-wall `ExPolygons` from
every region into one `ExPolygons` per layer for `slices_to_mesh`, keep the per-region
split (each region's outer-facing boundary, where it faces outward, becomes an
`ExPolygons` slice tagged with that region's id/color) and after lofting, propagate the
source region id to the wall_strip triangles generated across that boundary segment (the
loop point order is preserved through `wall_strip`, `SlicesToTriangleMesh.cpp:37-43`, so
a parallel per-vertex or per-point region-id array can ride along and be written as
per-facet color/paint data). This needs new code (`slices_to_mesh` and `wall_strip`
currently discard which `ExPolygon`/region a point came from) but no new algorithmic
approach. For (b), OpenVDB grids are unioned by a scalar SDF min with no notion of
"which input contributed this voxel," so recovering per-facet region id after
`volumeToMesh` needs a second nearest-surface-point lookup against the original tagged
input geometry — meaningfully more work, hence deferred (phase 3, smooth-mode color is
explicitly out of scope for the first color pass too).

## 6. Tests

- **20 mm cube, 0.2 mm layers, Exact mode**: baked mesh has exactly 100 layer steps
  (visible as 100 distinct Z bands in the loft); watertight (manifold check, e.g.
  `its_is_manifold`/existing mesh-repair test helpers); volume within 2% of 8000 mm³.
- **20 mm cube with fuzzy skin, Exact mode**: baked mesh's outer surface deviates from
  the ideal cube face by no more than the fuzzy skin's configured thickness (max
  displacement bound); re-slicing the baked STL with fuzzy skin *off* reproduces
  comparable jitter in its own outer wall polyline (a jitter-amplitude comparison against
  the original object's fuzzy-perturbed loop, not the frozen mesh's nominal face).
- **Cylinder, Exact mode**: baked mesh step count equals layer count across the
  cylinder's height; radius (fit a circle to a horizontal cross-section) matches the
  source cylinder's radius within the perimeter's nominal width/2 tolerance.
- **Sphere, Smooth mode**: baked mesh volume within 3% of the ideal sphere volume
  (4/3 π r³) — the level-set closing radius is expected to round the layer steps, so a
  looser tolerance than the exact-mode cube is appropriate here.

## 7. Phases with estimates

- **Phase 1** (~2-3 weeks): Exact-layer loft from perimeter loops only (method (c)),
  reusing `slices_to_mesh`; no G-code route, no contouring/seams. Object-replace and
  STL/3MF export outputs. `BakeSliceJob` wired into the existing Jobs system with basic
  progress. Tests from §6 (cube, fuzzy cube, cylinder — Exact mode only). This is the
  bulk of the value (visible fuzzy skin + top/bottom + paint-boundary shape) at the
  lowest engineering risk, since the core mesh algorithm already exists in the tree.
- **Phase 2** (~3-4 weeks): G-code-route bake (walk `GCodeProcessorResult::moves`,
  building capsule/box segments per extrusion move) to capture Z contouring and seam
  geometry; Smooth mode (method (b), OpenVDB level set + closing radius) as an
  alternative to the exact loft, needed for a genuinely smooth contoured surface rather
  than a lofted approximation of it. Requires the plate to have been G-code-processed
  (not just sliced) before the action is enabled.
- **Phase 3** (~1-2 weeks): Color/paint carry-over for Exact mode via per-region tagged
  `ExPolygons` threaded through `slices_to_mesh`/`wall_strip` into per-facet mesh color;
  Smooth-mode color explicitly deferred beyond this.

## Summary

`Slic3r::slices_to_mesh` (`src/libslic3r/SlicesToTriangleMesh.cpp`) is an existing,
currently-SLA-only "slices to mesh" loft that is directly reusable as the phase-1 bake
algorithm — no new meshing algorithm needs inventing. Bakeable before G-code: outer
perimeter shape, fuzzy skin jitter (modifies loop points in
`PerimeterGenerator.cpp`/`FuzzySkin.cpp`), top/bottom solid surfaces, and per-region
paint/color boundaries (`PrintRegion::print_object_region_id()`). G-code-only, needing
the `GCodeProcessorResult::MoveVertex` route instead: Z contouring (`ContourZSamples`,
applied in `GCode.cpp`) and seam position (`SeamPlacer`/`GCode.cpp`), since neither is
persisted in the stored `ExtrusionLoop` polyline. Phase 1 (exact-layer loft, object
replace/export, tests) is estimated at 2-3 weeks given the reusable `slices_to_mesh`
base; phases 2 (G-code route + smooth mode) and 3 (color) add roughly 4-6 weeks more.
