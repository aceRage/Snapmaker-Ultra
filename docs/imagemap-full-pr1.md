# ImageMap → Ultra FULL PR1 (libs + LFS)

Feature-patch extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb)
(`v1.0.44` lineage). Does **not** merge ImageMap `main`.

Locked FULL-plan decisions (Lance ACCEPTED):

- Contoning **IN**
- Remap **IN**
- `lut_wide.png.c` via **Git LFS** (~36.3 MiB)
- `ImportedTexture` **IN**
- FULL UI later (**PR4**)
- GLTF / tinygltf **OUT**
- Prime-tower images **OUT**
- C3 best-effort: do not regress `paint_depth_*`

## Phase 0 spike

| Item | Status |
| --- | --- |
| `TextureMapping` + `TextureMappingOffset` | Sources listed next to `PaintDepth`; Offset samples `ColorFacetsAnnotation` / `imported_texture_*` |
| `TextureMappingContoning` | Sources included. `TextureMappingContoningSolver` constructs from zone + `PrintConfig`. Fill / LayerRegion / GCode hooks stay in **PR2+** |
| `ModelTextureDataRemap` | Sources included with real `ColorFacetsAnnotation` + `ModelVolumeImportedVector` |
| `Format/ImportedTexture` | PNG/JPEG decode + atlas builder. No GLTF loader |
| `lut_wide.png.c` | Git LFS (`38094965` bytes). See `deps_src/pigment-painter/README.md` |
| GLTF / tinygltf | **OUT**. No `Format/GLTF*`, no `deps_src/tinygltf` |
| Prime-tower image assets | **OUT** |
| `paint_depth_*` / `handle_legacy_composite` / `PaintDepth.*` | Untouched |

### Stubs / deferred until PR2

- `Print::apply` does **not** load `texture_mapping_definitions` yet. `Print::texture_mapping_manager()` exists (same pattern as mixed filament) and stays empty until PR2.
- The six `texture_mapping_*` keys are listed in `Print::invalidate_state_by_config_options` `steps_gcode` so they do not fall through to `invalidate_all_steps` before slicing hooks exist.
- GCode / LayerRegion / ToolOrdering / MMS / Fill Contoning hooks are **not** in this PR.
- 3MF persistence of `imported_texture_*` / color facets is later (not `bbs_3mf.cpp`).
- OBJ/GLTF import wiring that filled `imported_texture_*` in ImageMap `Model.cpp` is **not** copied (GLTF stays OUT).

## CI: Git LFS

```bash
git lfs install
git lfs pull
```

GitHub Actions:

```yaml
- uses: actions/checkout@v5
  with:
    lfs: true
```

Confirm the LUT payload (not the pointer):

```bash
wc -c deps_src/pigment-painter/lut_wide.png.c
# expect: 38094965
```

## Licenses

| Tree | License | Notice |
| --- | --- | --- |
| `deps_src/colorsolver` | AGPL-3.0 | `NOTICE` + headers |
| `deps_src/pigment-painter` | GPL-3.0 | `COPYING` |
| `deps_src/prusa-fdm-mixer` | MIT | `LICENSE` |
