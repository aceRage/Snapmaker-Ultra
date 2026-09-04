# ImageMap FULL — known gaps and residual TODOs

Stacked extract from [OrcaSlicer-ImageMap `@92548381056`](https://github.com/sentientstardust-dev/OrcaSlicer-ImageMap/commit/92548381056dbf72836b0a1bdc455f238218dbfb)
(`v1.0.44` lineage). **Does not merge ImageMap `main`.**

This is the residual inventory after FULL **PR1–PR5**. It is **not** a product brochure.

Related notes: [`imagemap-full-pr1.md`](imagemap-full-pr1.md), [`imagemap-full-pr2.md`](imagemap-full-pr2.md), [`imagemap-full-pr3.md`](imagemap-full-pr3.md), [`imagemap-full-pr4.md`](imagemap-full-pr4.md), [`imagemap-full-pr5.md`](imagemap-full-pr5.md).

## Ships (FULL pack)

| Surface | Where |
| --- | --- |
| Side texture mapping (LineWidth / PerimeterPath / V2) | PR1 libs + PR2 `LayerRegionTextureMapping` + PR3 G-code flow/seam |
| Contoning (top-surface schedule + solver) | PR1 `TextureMappingContoningSolver` + PR2 Fill driver + PR3 emission |
| Remap (ImageTexture + region paint) | PR1 `ModelTextureDataRemap` + PR3 CutUtils / 3MF |
| ImportedTexture (PNG/JPEG + OBJ+PNG) | PR1 `Format/ImportedTexture` + PR3 Model import |
| Full UI (zones, gizmos, gated preview) | PR4 sidebar / TrueColor / Image Projection / `MMUPaintedTexturePreview` |
| `lut_wide.png.c` via Git LFS | PR1 `.gitattributes` + PR5 cmake/CI fail-clearly |
| Wipe-tower **filament count** for TM | PR4 `ArrangeJob` (`estimate_wipe_tower_filaments_count_for_texture_mapping`) |

## Gaps (intentional — do not treat as bugs)

| Gap | Stance |
| --- | --- |
| `.gltf` / `.glb` import | **OUT.** Stub throws `RuntimeError("GLTF/GLB import is not supported.")`. No `tinygltf`. |
| Prime-tower **images** | **OUT.** Wipe-tower **count** is in; image assets / `Metadata/texture_mapping` prime-tower tree are not. |
| **C3** — paint-depth + texture mapping on the **same object** | **Best-effort only.** No crash is the bar. Not a marketing or QA “supported” claim. See below. |
| Calibration **Electron** app | **OUT.** No Electron bridge. |
| Transmission-distance (TD) calibration sheets | Optional **static docs only** if added later. No in-app calibration flow. |
| Stream / CLI / phone texture features | **OUT.** Forbidden-region rewrites (Plater phone/AMS/SendJob/Stream/Remote, PartPlate core) stay off this stack. |
| ImageMap `main` | **Do not merge.** Feature-patch extract only. |

## C3 — best-effort, not supported

Same-object Ultra paint-depth (MMU color paint + `paint_depth_*` clamp) **and** a texture-mapping zone / imported texture is **unsupported**.

What “best-effort” means:

- The slicer must **not segfault** if a user paints and also attaches TM on one object.
- Paint-depth clamp still owns painted-claim geometry (`PaintDepth.*` / `paint_depth_*` / `cut_segmented_layers`). Those formulas stay untouched.
- TM must not break paint-depth unit tests (`[paintdepth]`).
- Preview gating (PR4) still requires texture preview data before `TriangleSelectorPatch` draws a TM preview.

What it does **not** mean:

- Correct combined visuals, bounded TM+paint interaction, or a supported product mode.
- A dedicated C3 fixture that asserts clamp-vs-TM-width (that would be a harden; out of PR5 scope).

Automated coverage: `tests/libslic3r/test_texture_mapping.cpp` case
`C3 best-effort: same-object paint-depth facets + texture mapping does not crash`
(`[texturemapping][pr5][c3]`). Crash-only. `test_paint_depth*.cpp` is untouched.

Manual REAPER item: paint + TM on one object → slice → no crash → paint-depth settings still visible. Visual quality is undefined.

## Residual TODOs (intentional landmines, listed)

Do **not** “fix” these in a drive-by unless a later PR explicitly owns them.

| Item | Location | Why it is leftover |
| --- | --- | --- |
| `LayerTools::texture_mapping_extruders` | `ToolOrdering.hpp` / `.cpp` | Declared and de-duplicated; **never populated** on Ultra FULL. ImageMap collected virtual zone IDs here. Ultra resolves via `LayerTools::resolve_filament_id` and `PrintApply::append_texture_mapping_component_extruders` into `painting_extruders`. Emptiness is expected. |
| `LayerTools::texture_mapping_component_extruders` | same | Same stub. Wipe-tower **count** uses `TextureMappingManager::total_filaments` (PR4), not this vector. |
| `LayerTools::top_surface_image_no_fixed_desired_extruders` | `ToolOrdering.cpp` | **Populated** from Fill Contoning tags. Not a stub. |
| SMART_FILL empty tip list | `GLGizmoTextureMappingTools.cpp` (`// TODO:`) | Copied ImageMap gizmo. Cosmetic toolbar tip; SMART_FILL is not a FULL ship requirement. |
| shortkey FIXME comments | `GLGizmoTextureMappingTools.cpp` | ImageMap copy; use Ultra prefix helpers only if a later UI polish PR owns it. |
| Contoning Fill geometry dump | PR2 notes | Schedule driver + solver only. ImageMap’s ~13k polygonize/raster dump was not copied. |
| ImageMap GCode per-segment centerline-shift / dither rewrite | PR3 notes | Ultra `_extrude` is not replaced; PR3 modulates `dE` + seam hint. |
| `GLGizmoMmuSegmentationImageMap` | PR4 | Compiled, **not registered**. Ultra `GLGizmoMmuSegmentation` remains paint-depth. |
| Preview simulate / dithering | `TextureMappingZone` defaults | `DefaultPreviewSimulateColors = false`, `DefaultDitheringEnabled = false`. Heaviest `MMUPaintedTexturePreview` halftone paths stay opt-in so the UI thread stays usable. Do not flip these defaults without a perf budget. |
| Offset weight field | `TextureMappingOffset.cpp` | Built on the **slice / G-code** path (`build_texture_mapping_offset_weight_field`), not the UI thread. Leave it; do not move it onto Plater idle. |

## LFS / CI (required before build or test)

```bash
git lfs install
git lfs pull
wc -c deps_src/pigment-painter/lut_wide.png.c   # expect 38094965
```

- Tracked in `.gitattributes`.
- `deps_src/pigment-painter/CMakeLists.txt` **FATAL_ERROR**s if the file is missing or still an LFS pointer. There is no silent empty LUT.
- GitHub Actions: `actions/checkout` with `lfs: true` on `build_orca.yml` / `build_deps.yml` / `build_check_cache.yml` / `build_all.yml` (flatpak). `build_orca.yml` re-checks the payload after checkout.
- `build_linux.sh` refuses to compile if the LUT is still a pointer.
- Do **not** add ImageMap LFS notes to `doc/developer-reference/How-to-build.md`: the wiki validator indexes only `doc/` (so a `docs/` link is “missing”) and any `doc/**/*.md` touch reports the whole pre-existing wiki-link backlog. Use `docs/imagemap-full-pr5.md` + `README.md` instead.

## REAPER smoke before merge

Cloud agents usually **cannot** build the full slicer or `libslic3r_tests`. That is expected. A REAPER (or other full-tree) host must:

1. Fetch the **PR5 tip** (this branch), not `main` and not an LFS-less shallow clone.
2. `git lfs pull`
3. Rebuild
4. Run `[texturemapping]` and `[paintdepth]`

```bash
git lfs pull
# after rebuild:
./tests/libslic3r/libslic3r_tests "[texturemapping]"
./tests/libslic3r/libslic3r_tests "[paintdepth]"
```

## Instruct Claude manager (merge rules)

- **Stack PR1 → PR2 → PR3 → PR4 → PR5.** Do not flatten onto `main` until the stack is reviewed. PR5’s GitHub base is the PR4 branch (`cursor/feat-imagemap-full-pr4-ui-f9c3`, draft #13) when GitHub allows it.
- **Smoke gates before merge:** `git lfs pull`, then `[paintdepth]` + `[texturemapping]`, plus PR4 U1–U4 and the C3 no-crash checklist.
- **Code Team does not merge.** Draft PRs stay draft. Do not close sibling ImageMap PRs (#10–#13).
- **Do not** merge ImageMap `main`, add tinygltf, add prime-tower images, or claim C3 as supported.
- **Do not** edit `PaintDepth.*`, `paint_depth_*` formulas, or `test_paint_depth*.cpp` on this stack.

## Forbidden regions

PR5 is docs / cmake / tests / comments / conservative-default documentation only. Expect **no** phone / AMS / `PartPlateList` / `SendJob` / Stream / Remote feature hits in `Plater` / `PartPlate` / MMU gizmo diffs on this PR.
