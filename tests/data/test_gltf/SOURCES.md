# glTF / GLB test fixtures

Used by `tests/libslic3r/test_gltf.cpp` (`libslic3r_tests "[gltf]"`) and as the seed corpus for
`tests/sandboxes/fuzz_gltf`.

## A. Self-authored

Produced by `make_fixtures.py` in this directory (`python tests/data/test_gltf/make_fixtures.py`).
No licence question: they are ours. Regenerate them whenever the expectations in
`test_gltf.cpp` change — the script and the test are two halves of one statement.

| File | What it is | What it pins |
|---|---|---|
| `box_10_20_30.glb` | box 10 (X) × 20 (Y) × 30 (Z) in glTF space, one node at identity, one material, 24 split vertices | `mesh().size() == Vec3d(10, 30, 20)` — the up-axis rule and the unit rule in one line; welding (`open_edges == 0`) |
| `box_10_20_30.gltf` + `.bin` | the same box with an external buffer | identical geometry to the `.glb` |
| `Geräte/box-čřšřěá.glb` | a copy of the box at a non-ASCII path | the nowide file read (mirrors `test_stl.cpp`) |
| `box_meters.glb` | the same box scaled to 0.01 units | `Model::looks_like_saved_in_meters()` — the rescue prompt the units decision relies on |
| `two_parts_two_materials.glb` | one mesh, two primitives, red and blue | 2 volumes, `material_colors.size() == 2`, per-part material index |
| `nested_trs.glb` | parent T(5,0,0)·Ry(90°), child T(1,0,0)·S(2), a 1×2×4 box | TRS composition and the up-axis rotation together; its grey material is linear 0.2158605 = sRGB 0.5, which pins `linear_to_srgb` |
| `strip_and_fan.glb` | one TRIANGLE_STRIP (4 verts → 2 tris) and one TRIANGLE_FAN (5 verts → 3 tris), no indices | de-stripping incl. the odd-triangle winding swap, and de-fanning |
| `sparse_triangle.gltf` + `.bin` | a triangle whose POSITION accessor is sparse | sparse accessors — the gotcha a hand-rolled reader fails |
| `points_only.glb` | one POINTS primitive | the "only points or lines" refusal |
| `box_draco.glb` | `box_10_20_30.glb` run through real Draco compression | the named Draco refusal (Stage 3 inverts this into a geometry comparison) |
| `unknown_extension.glb` | the box with `KHR_texture_basisu` required | an unknown extension is named in the error |
| `escaping_buffer.gltf` | a `.gltf` whose buffer URI is `..%2F..%2Fsecret.bin` | the buffer URI containment check |
| `truncated.glb` | the first 200 bytes of `box_10_20_30.glb` | the "damaged or incomplete" message, no crash |

`box_draco.glb` is genuinely Draco-compressed — `box_10_20_30.glb` through glTF-Transform v4.5.0
(`npx --yes @gltf-transform/cli@4 draco box_10_20_30.glb box_draco.glb`), which is the one step
`make_fixtures.py` shells out for and skips when node is unavailable. Stage 1 refuses it by name
on `extensionsRequired`; **Stage 3 inverts that assertion against this same file** (same geometry
as `box_10_20_30.glb`) rather than deleting it, so the behaviour change is visible in the diff.

## B. Vendored from Khronos glTF-Sample-Assets

Repository: <https://github.com/KhronosGroup/glTF-Sample-Assets> (the repo itself is CC-BY-4.0;
**each model carries its own licence in its `README.md`**). Only CC0 models are vendored here, so
there is no attribution obligation; the provenance is recorded anyway. Checked 2026-09-03.

| File | Source (raw, under `.../glTF-Sample-Assets/main/`) | Licence | Covers |
|---|---|---|---|
| `SimpleMeshes.gltf` + `SimpleMeshes.bin` | `Models/SimpleMeshes/glTF/SimpleMeshes.{gltf,bin}` | CC0 1.0 Universal, © 2017 Public, credit Marco Hutter | two nodes sharing one mesh → 2 volumes, distinct names, and an external `.bin` |
| `TriangleWithoutIndices.gltf` + `.bin` | `Models/TriangleWithoutIndices/glTF/TriangleWithoutIndices.{gltf,bin}` | CC0 1.0 Universal, © 2017 Public, credit Marco Hutter | a non-indexed primitive → 1 triangle |
| `BoxVertexColors.glb` | `Models/BoxVertexColors/glTF-Binary/BoxVertexColors.glb` | CC0 1.0 Universal, © 2023 Public | `COLOR_0` → `GltfInfo::vertex_colors` |

### Deliberately not vendored

`Models/SimpleSparseAccessor` is listed as CC0 in the implementation plan, but its `README.md`
says **Creative Commons Attribution 4.0 International**, not CC0. Rather than take on an
attribution obligation for a 1 KB test file, `sparse_triangle.gltf` above was authored instead and
covers the same behaviour with numbers we chose.

`Models/Box` is CC-BY-4.0, © 2017 Cesium — not used.
