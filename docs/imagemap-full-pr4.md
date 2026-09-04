# ImageMap → Ultra FULL PR4 (UI / gizmos / surgical Plater)

Feature-patch extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb).

Depends on FULL **PR1** libs (#10), FULL **PR2** slicer hooks (#11), and FULL **PR3** G-code/3MF (#12). This branch stacks PR1–PR4.

This PR ships a usable ImageMap surface on Ultra: create/edit texture-mapping zones in the sidebar, preview painted/imported textures, and slice. Gizmo merge is **onto** Ultra (new IDs). Plater changes are **surgical hunks** plus extracted `TextureMappingPlaterHooks` — not a wholesale Plater replace.

C3 is **best-effort**: no crash; paint-depth clamp holds. Same-object paint-depth + TM is easier with this UI but is not marketing-supported.

## How Ultra paint-depth gizmo paths were preserved

- Live Ultra `GLGizmoMmuSegmentation.cpp` (~1.4k lines) is **unchanged except** one additive “Manage Color Data” button after the existing remap UI.
- ImageMap’s ~1.05 MiB `GLGizmoMmuSegmentation` was copied as `GLGizmoTextureMappingTools.cpp` and **renamed**:
  - `GLGizmoMmuSegmentation` → `GLGizmoMmuSegmentationImageMap` (compiled, **not registered**)
  - `GLMmSegmentationGizmo3DScene` → `GLMmSegmentationGizmo3DSceneImageMap`
- New gizmos are appended at the **end** of Ultra `EType` so Emboss/Svg/Measure/Assembly/Simplify/BrimEars IDs do not shift:
  - `TrueColorPainting`
  - `ImageProjection`
  - `TextureGradientPointPicker` (`on_is_selectable() == false`, hidden from toolbar)
- `GLGizmoPainterBase` default `should_render_triangle_texture_preview()` is **false**. Ultra FdmSupports / Seam / FuzzySkin / MMU paint-depth therefore do not enter the TM preview path.
- Default `set_render_triangle_slope_uniforms` keeps Ultra’s exact slope uniforms (`slope.actived`, `slope.volume_world_normal_matrix`, `slope.normal_z`). `PaintDepth.*` / `paint_depth_*` / `test_paint_depth*.cpp` are untouched.

## Preview gating

`TriangleSelectorPatch::render_texture_preview` returns immediately unless:

1. `m_texture_preview_needed` (TrueColor / ImageMap MMU ask for it), and
2. `m_model_volume != nullptr`, and
3. the volume has texture preview data **or** vertex-color preview **or** texture-mapping color facets.

Helpers: `model_volume_has_texture_preview_data`, `model_volume_has_vertex_color_preview_data`, `model_volume_has_texture_mapping_color_preview_data`.

`MMUPaintedTexturePreview.cpp/.hpp` is additive. Ultra paint-depth painting does not set `m_texture_preview_needed` (default false).

## New gizmo IDs

| EType | Toolbar name | Notes |
| --- | --- | --- |
| `TrueColorPainting` | RGB Color Painting | New ID after `BrimEars` |
| `ImageProjection` | Project image… | New ID |
| `TextureGradientPointPicker` | Set linear gradient point | Hidden (`on_is_selectable() == false`) |

Ultra `MmSegmentation` remains Color Painting + paint-depth.

## Surgical Plater hunks

Helpers live in `TextureMappingPlaterHooks.cpp/.hpp` (preferred over dumping into `Plater.cpp`). **PartPlate.cpp is not edited.**

| Hunk | Where | What |
| --- | --- | --- |
| Include + `get_extruders_colors()` | `Plater.hpp` / `Plater.cpp` | RGBA colors via hooks (physical + TM display colors) |
| Color list | `get_extruder_colors_from_plater_config` | After mixed-filament colors, append TM display colors |
| Sidebar | after mixed-filament panel init | `init_texture_mapping_panel` → `TextureMappingSidebarPanel.cpp` |
| 3MF import | `load_files` after `read_from_archive` | `texture_mapping_zone_ids_from_import_config` + `assign_imported_3mf_texture_mapping_zones` |
| OBJ import | `read_from_file` | `assign_imported_texture_mapping_zone` when imported texture data exists |
| Slice apply | before `background_process.apply` | `canonicalize_texture_mapping_config` |
| Undo/redo | `update_after_undo_redo` | Reload defs when `model.texture_mapping_definitions_valid` |
| Config change | `on_config_change` | Refresh TM panel on `texture_mapping_*` / `filament_colour` |
| Wipe-tower **count** | `ArrangeJob.cpp` only | `estimate_wipe_tower_filaments_count_for_texture_mapping` — **not** prime-tower images |

Forbidden regions were not edited: phone preset combo, `load_ams_list`, `get_select_machine_dialog`, PartPlate core, `SendJob` / Stream / Remote feature paths.

## Tab / object list

- Multimaterial page: new **Texture Mapping** optgroup (outer-wall gradient strength / max / min line width) **after** the existing paint-depth Advanced block. Paint-depth UI is not hidden.
- `GUI_ObjectList::total_filaments_count` also considers `texture_mapping_zones.total_filaments`.
- Filament-delete remapping skips virtual TM zone IDs.

## Out of scope (intentionally)

- GLTF/tinygltf import (export GLB in Color Data dialog is stubbed: “GLTF/GLB export is not supported.”)
- Calibration Electron app
- Prime-tower **image** assets
- Stream/phone *feature* work
- Changing `paint_depth_*` / `PaintDepth.*`

## Tests

`test_paint_depth*.cpp` is untouched. Maintainers should run `[paintdepth],[texturemapping]`. Cloud cannot build `libslic3r_tests` or run GUI smoke.

## U1–U4 manual checks (REAPER)

Cloud cannot run GUI smoke. Please run:

- **U1:** Create a TM zone in the sidebar, assign filaments A/B, slice. No crash. Wipe-tower count uses `texture_mapping_zones`, not prime-tower images.
- **U2:** Import OBJ+PNG and/or a 3MF with texture data → zone assigned; `texture_mapping_definitions` / global settings persist after save/reopen.
- **U3:** Ultra color paint (MMU / paint-depth) still works. `paint_depth_*` settings remain visible. TrueColor + Image Projection gizmos open from the toolbar.
- **U4:** Same object with paint-depth + TM: no crash; paint-depth clamp still bounds painted claims; texture preview only appears when the volume has texture preview data.
