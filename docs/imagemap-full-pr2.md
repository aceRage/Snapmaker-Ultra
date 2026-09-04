# ImageMap → Ultra FULL PR2 (slicer hooks)

Feature-patch extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb).
Depends on FULL **PR1** libs (`TextureMapping` / Offset / Contoning solver / Remap / ImportedTexture).

This PR wires TextureMapping into the slicer. It does **not** add Gizmo/Plater (PR4) or GCode emission / `bbs_3mf` persistence (PR3). Prime-tower *images* stay omitted.

## Locked product rules

- **C3 (paint-depth + TM same object): best-effort.** No crash. Paint-depth clamp still bounds painted claims. TM must not break `cut_segmented_layers`. Not a marketing “supported” claim.
- `PaintDepth.*` and `paint_depth_*` formulas are untouched.
- PerimeterGenerator is **not** rewritten. Ultra `has_bounded_paint_depth` is preserved. IM modulation lives in `LayerRegionTextureMapping` + ToolOrdering + Fill tagging.

## What landed

| Area | Hook |
| --- | --- |
| `Print` | `texture_mapping_manager()` + `texture_mapping_global_settings()`. `Print::apply` loads `texture_mapping_definitions`. Preview-only JSON keys do not invalidate G-code. No prime-tower images. |
| `ToolOrdering` / `LayerTools` | Zone resolve (`resolve_filament_id`). Virtual TM IDs → physical components when manager present; **no-op when empty**. Mixed IDs still resolve after TM. Wipe-tower skip when `has_texture_mapping_zone && extruders.size() <= 1`. |
| MMS | TM outer-wall max width via `texture_mapping_outer_wall_gradient_max_line_width` **before** paint-depth clamp uses the width. `num_total_filaments = max(mixed, tm)`. Clamp code paths unchanged. |
| `LayerRegion` | Side `ModulationLineWidth` / `PerimeterPath` / `PerimeterPathV2` extracted to `LayerRegionTextureMapping.*`. Contoning perimeter branches gated on `top_surface_contoning_perimeters_active()`. |
| Fill | Contoning **schedule driver** (`FillTextureMapping.*` + `SurfaceFillParams` fields + collection tagging). Does **not** boil ImageMap’s ~13k Contoning geometry dump. Solver remains PR1 `TextureMappingContoningSolver`. |
| `ExtrusionEntityCollection` | `texture_mapping_extruder_override` + top-surface flags (default off). |

Modulation enums: `ModulationLineWidth=0`, `ModulationPerimeterPath=1`, `ModulationPerimeterPathV2=2`.

## MMS nesting (critical)

Ultra paint-depth clamp **wins** for painted claims. The TM branch only adds outer-wall width / filament-count awareness **around** that clamp. Dual-gate every MMS change with `[paintdepth]` tests.

## Contoning Fill scope

ImageMap `Fill.cpp` is ~15k lines with ~1198 Contoning hits. This PR ports:

- `SurfaceFillParams` TM / Contoning fields used to group fills
- `TopSurfaceImageContoningStackPlanCache` + `Layer::prebuild_contoning_stack_plan_cache`
- 5-arg `Layer::make_fills` (cache may be null)
- Extrusion collection tagging so ToolOrdering can resolve top-surface desired components

It does **not** copy ImageMap debug SVG export or the polygonization / raster dump. Full infill patterning of Contoning images remains a later thickening of this driver (still without GCode emission in this PR).

## C3 spike (optional)

After these hooks, hardening C3 beyond best-effort still looks **non-cheap**: paint-depth clamp, `cut_segmented_layers`, and TM outer-wall width share `compute_layer_color_stat` but the painted-claim geometry is still owned by Ultra paint-depth. A dedicated C3 product mode would need fixtures that paint **and** attach a TM zone on one object, then assert clamp bounds vs TM width — not just “doesn’t crash”.

## Out of scope (intentionally)

- `MMUPaintedTexturePreview`, `GLGizmo*`, Plater (PR4)
- GCode emission / `bbs_3mf` (PR3)
- `Format/GLTF*`, tinygltf, prime-tower image assets
- Changing `paint_depth_band_*` / `PaintDepth` helpers
- Wholesale `PerimeterGenerator` rewrite
