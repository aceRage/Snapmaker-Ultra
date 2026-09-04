# ImageMap → Ultra FULL PR5 (harden + LFS/CI + known-gaps)

Feature-patch extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb).

Depends on FULL **PR1** libs (#10), **PR2** hooks (#11), **PR3** G-code/3MF (#12), and **PR4** UI (#13).
This branch stacks PR1–PR5 on PR4 tip `e3e839962758b61bbb3542696b11c931bf79ef91`.

**No new feature surfaces.** This PR hardens LFS/CI, documents C3 as best-effort only, records residual gaps, and keeps conservative preview defaults. It does **not** merge ImageMap `main`.

## Locked product rules (unchanged)

| Decision | Status |
| --- | --- |
| Contoning / Remap / ImportedTexture | **IN** (PR1–PR4) |
| `lut_wide.png.c` via Git LFS | **IN** — cmake now **FATAL_ERROR**s on a pointer/missing payload |
| Wipe-tower TM filament **count** | **IN** (PR4 `ArrangeJob`) |
| GLTF / tinygltf | **OUT** (unsupported stub) |
| Prime-tower images | **OUT** |
| C3 (paint-depth + texture same object) | **best-effort only** — crash-only; not a supported product claim |
| Calibration Electron app | **OUT** |
| `PaintDepth.*` / `paint_depth_*` / `test_paint_depth*.cpp` | **untouched** |

## What landed

1. **C3 stance** — `docs/imagemap-full-known-gaps.md` plus a crash-only `[texturemapping][pr5][c3]` case in `tests/libslic3r/test_texture_mapping.cpp`. Same-object paint-depth + TM is **unsupported**. Do not market it. Do not expand this PR into a C3 harden.
2. **LFS / CI** — `.gitattributes` already tracks `deps_src/pigment-painter/lut_wide.png.c`. `pigment_painter` cmake now refuses a Git LFS pointer or missing file (no silent empty LUT). Workflows already checkout with `lfs: true`; `build_orca.yml` also verifies the payload after checkout. Build docs require `git lfs pull`.
3. **Perf sanity** — Offset weight-field build stays on the slice/G-code path. UI preview simulation (`MMUPaintedTexturePreview` halftone) stays **off** by default (`DefaultPreviewSimulateColors = false`, `DefaultDitheringEnabled = false`). Create / edit / slice still work. Heaviest halftone paths remain opt-in.
4. **Known gaps** — `docs/imagemap-full-known-gaps.md` lists ships, gaps, residual TODOs, and Claude-manager merge rules.
5. **Cheap residual** — `LayerTools::texture_mapping_extruders` / `texture_mapping_component_extruders` are documented as unused ImageMap leftovers (Ultra uses `resolve_filament_id` + `PrintApply` painting extruders + PR4 wipe-tower count). No expensive ToolOrdering rewrite.

## LFS / cmake fail-clearly

Expected payload: **38094965** bytes (`deps_src/pigment-painter/lut_wide.png.c`).

```bash
git lfs install
git lfs pull
wc -c deps_src/pigment-painter/lut_wide.png.c
# expect: 38094965
# a ~130-byte file starting with "version https://git-lfs.github.com/spec/v1" is the pointer — cmake will FATAL_ERROR
```

`deps_src/pigment-painter/CMakeLists.txt` reads the first bytes and fails configure if the file is missing or still an LFS pointer. Do not comment that check out.

## REAPER smoke (owed — cloud cannot build)

Fetch this tip, materialize LFS, rebuild, then run tagged suites:

```bash
git fetch origin cursor/feat-imagemap-full-pr5-harden-02d1
git checkout cursor/feat-imagemap-full-pr5-harden-02d1
git lfs pull
# rebuild libslic3r_tests (or the full slicer) after LFS materializes
./tests/libslic3r/libslic3r_tests "[texturemapping]"
./tests/libslic3r/libslic3r_tests "[paintdepth]"
```

`test_paint_depth*.cpp` is **untouched**. `[paintdepth]` must stay green.

Manual C3 checklist (not a supported-mode claim):

- Same object: Ultra color paint (paint-depth) **and** a TM zone / imported texture.
- Slice. **No segfault.**
- `paint_depth_*` still visible; clamp still bounds painted claims.
- Texture preview only when the volume has texture preview data (PR4 gating).
- Visual quality of the combination is **undefined** — log and move on.

## Out of scope (intentionally)

- GLTF / tinygltf import
- Prime-tower image assets
- Calibration Electron bridge
- Supported / hardened C3 product claim
- Stream / CLI / phone texture features
- Wholesale Plater / PartPlate / AMS / SendJob / Stream / Remote rewrites
