# ImageMap → Ultra FULL PR3 (G-code + 3MF persist)

Feature-patch extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb).

Depends on FULL **PR1** libs (#10) and FULL **PR2** slicer hooks (#11). This branch stacks PR1+PR2+PR3.

This PR emits texture / Contoning-aware G-code and round-trips `texture_mapping_*` (plus atlas / virtual filament IDs) through `bbs_3mf`. It wires Model Remap call sites needed for mesh simplify / cut. It does **not** add Gizmo/Plater (PR4). Prime-tower *images* stay omitted. `PaintDepth.*` and `paint_depth_*` formulas are untouched.

## Locked product rules

- **C3 (paint-depth + TM same object): best-effort.** Do not change `PaintDepth.*` / `paint_depth_*`.
- **C5:** legacy `mmu_segmented_region_max_width` → `paint_depth_*` migration stays in `PrintConfigDef::handle_legacy_composite` (untouched).
- **C6:** the same 3MF must preserve both `texture_mapping_*` and `paint_depth_*`.
- **C7:** Remap on simplify / cut is wired for ImageTexture and region paint.
- GLTF/GLB import is an explicit unsupported stub (clear error, no crash). Full GLTF is out of scope.

## Side-texture / Contoning G-code hooks

Cloud may not smoke-slice. The emission path is:

| Stage | Where | What |
| --- | --- | --- |
| Tag | PR2 `LayerRegionTextureMapping` + Fill Contoning schedule | Paths already carry TM / Contoning extruder overrides and top-surface flags. |
| Resolve | PR2 `ToolOrdering` / `LayerTools::resolve_filament_id` | Virtual TM zone IDs → physical components. |
| Seam | `GCode::texture_mapping_seam_hiding_hint` → `SeamPlacer::place_seam` | Preferred seam point from texture overhang / seam-hiding (`texture_mapping_seam_hiding_hint`). |
| Width / flow | `GCode::texture_mapping_path_flow_scales` inside `_extrude` | Multiplies `dE` for external perimeters when a LineWidth / offset-gradient / Contoning overhang row is active. Uses PR1 `build_texture_mapping_offset_context_for_layer` + `texture_mapping_offset_surface_inset_mm`. |
| Warnings | `do_export` | Raw-atlas missing-data warnings (`print_has_raw_offset_texture_zone_without_raw_data_for_gcode`, `collect_raw_atlas_warnings_for_gcode`). |
| Background | PrintConfig `texture_mapping_background_color` | Already applied in PR2 collection tagging; G-code uses the resolved filament, not a second color command. |

Helpers live in `GCodeTextureMapping.cpp` (ImageMap GCode.cpp hunks). Ultra `_extrude` is **not** replaced: paint-depth / pointillism / sloped-Z stay. ImageMap’s full per-segment centerline-shift / dither rewrite is not copied; this PR modulates extrusion volume (and seam placement) as the Ultra-adapted hook.

Empty `texture_mapping_path_flow_scales` ⇒ no modulation (identity `dE`).

## 3MF persist (C6)

PrintConfig keys already serialize through `Metadata/project_settings.config` (`config.save_to_json` / `_extract_project_config_from_archive`). Ultra **always** loads full project config (unlike ImageMap’s `dont_load_config` TM-only branch), so `paint_depth_*` and `texture_mapping_*` travel together.

Additional persist in this PR:

| Payload | Path |
| --- | --- |
| `texture_mapping_color` triangle attr | Same pattern as `paint_color` / `mmu_segmentation` |
| Virtual filament IDs 99–255 | `is_texture_mapping_virtual_filament_id` — do not clamp zone IDs to 0 |
| Imported RGBA / UV / raw atlas | `Metadata/imported_texture/o{obj}_v{vol}.{png,json}` encoded with `encode_image_map_raw_filament_offset_atlas` when Offset raw data is present. Written even if print config is omitted. Restore keeps atlas/RGBA if UV triangle counts do not match. |

**Not** persisted: prime-tower image files / `Metadata/texture_mapping` prime-tower tree.

## Model Remap (C7) + OBJ+PNG + GLTF stub

- `CutUtils` snapshots `snapshot_simplify_texture_data` before cut, remaps with `remap_simplify_texture_data`, applies with `apply_simplify_texture_data_result`. Solid-part merge is skipped when a volume has remappable color / ImageTexture data.
- OBJ+PNG: `ObjInfo.triangle_uvs` / `triangle_uvs_valid` filled in `Format/OBJ.cpp`; `Model::read_from_file` auto-imports via `decode_image_texture_rgba_from_file` + `build_obj_texture_atlas` when UV+PNG exist (no Plater mode dialog).
- `.gltf` / `.glb` throw `RuntimeError("GLTF/GLB import is not supported.")` — no crash, no tinygltf.

GUI gizmo simplify (`GLGizmoSimplify.cpp`) remains PR4.

## Tests

`tests/libslic3r/test_texture_mapping.cpp` tags `[texturemapping][pr3]`:

- **C5:** `handle_legacy_composite` still migrates `mmu_segmented_region_max_width`.
- **C6:** `store_bbs_3mf` / `load_bbs_3mf` round-trips the six `texture_mapping_*` keys **and** `paint_depth_*`, plus raw atlas payload, virtual filament IDs 99–255, and `texture_mapping_color` triangle attributes.
- **C7:** Remap snapshot/apply on ImageTexture (+ region-paint facets). `CutUtils::perform_with_plane` remaps ImageTexture UVs onto cut volumes.
- GLTF stub throws a clear error.

Do not change `test_paint_depth*.cpp`. Maintainers should run `[paintdepth],[texturemapping]`. Cloud cannot build `libslic3r_tests`.

## Out of scope (intentionally)

- Prime-tower *image* metadata
- Full GLTF / tinygltf
- `MMUPaintedTexturePreview`, gizmos, Plater (PR4)
- Changing `paint_depth_*` or `PaintDepth.*`
