# glTF 2.0 / GLB import — phased implementation plan

Date: 2026-09-02 · Branch: `feat/ultra-preferences` · Status: draft for review
(implements `docs/superpowers/specs/2026-09-02-glb-import-research.md`; every `file:line`
below was re-read in this working tree on 2026-09-02, and the ones that had drifted from the
research document are corrected here)

## 1. Goal

`.glb` and `.gltf` load like any other geometry format: File ▸ Import, drag-and-drop, the CLI,
the phone upload, macOS document types. **One file becomes one `ModelObject` with one
`ModelVolume` per drawn mesh primitive**, named from the glTF node/mesh, oriented Z-up, with
1 glTF unit = 1 mm. Materials become filament assignments in a second stage, through the
`ObjColorDialog` machinery the OBJ importer already uses. Compressed assets (Draco, meshopt) are
refused **by name** in v1/v2 rather than failing with the generic loader error, and become a
third stage.

The library is **cgltf** (MIT, single header) vendored into `deps_src/`. No change to the
prebuilt `deps/` tree, so nobody has to rebuild dependencies. FBX is explicitly not wanted, which
is what makes Assimp (research §3.4 Route A) unnecessary.

Not in scope: writing glTF, `.drc`, textures as geometry, animation, skinning beyond the bind
pose, and `.fbx`.

## 2. Stages and gates

Each stage is shippable on its own build. Do not start the next stage until its gate holds.

| Stage | Delivers | Gate (must all pass) |
|---|---|---|
| 1 — Geometry | `deps_src/cgltf`, `src/libslic3r/Format/GLTF.{hpp,cpp}`, registration in every extension list, `tests/libslic3r/test_gltf.cpp` + fixtures | `libslic3r_tests "[gltf]"` green; a 10×20×30 glTF box imports as a `Vec3d(10,30,20)` volume (up-axis **and** unit rule asserted in one line); `SimpleMeshes.gltf` gives 2 volumes; a Draco file fails with a message containing "Draco"; drag-and-drop, File ▸ Import, `--load`, phone upload and the CLI all accept `.glb`; Slice Compare of a GLB-imported box vs the same box as STL reports no layer differences |
| 2 — Colours | per-material `baseColorFactor` and `COLOR_0` → filament slots via `ObjColorDialog`; the two `.obj`-only guards widened; a multi-volume applier | A 3-material GLB pops the dialog once, and each part ends up on the chosen filament with **no** MMU painting; a `COLOR_0` GLB paints; OBJ colour import is byte-identical to before (regression); the dialog is never shown on a hidden instance without the Stage-3 modal hook answering it |
| 3 — Compressed / textured | Draco decode (or a permanent, documented refusal), `KHR_mesh_quantization`, `EXT_meshopt_compression`, texture → per-face colour | `Models/Box/glTF-Draco/Box.gltf` imports with the same geometry as `Models/Box/glTF/Box.gltf`; a quantized asset imports at the right scale; a textured GLB either samples its texture or reports that it dropped it |

## 3. Shared contract

Names and rules the three stages must agree on. Written down here because Stage 2 and 3 will be
done by different sessions.

### 3.1 The reader's API

`src/libslic3r/Format/GLTF.hpp`, deliberately shaped like `Format/OBJ.hpp:10-24`:

```cpp
#ifndef slic3r_Format_GLTF_hpp_
#define slic3r_Format_GLTF_hpp_

#include "libslic3r/Color.hpp"
#include <admesh/stl.h>            // ImportstlProgressFn (deps_src/admesh/stl.h:48)
#include <string>
#include <vector>

namespace Slic3r {

class Model;

// One entry per ModelVolume the reader created, in volume order.
struct GltfPart {
    std::string name;              // the ModelVolume name we assigned
    int         material_index {-1};// index into GltfInfo::material_colors, -1 = no material
    size_t      triangles {0};
};

struct GltfInfo {
    // Stage 2 inputs. material_colors is one sRGB RGBA per glTF material actually used,
    // deduplicated; parts[i].material_index indexes into it.
    std::vector<RGBA>       material_colors;
    std::vector<GltfPart>   parts;
    // Per-vertex COLOR_0, concatenated in volume order, only filled when EVERY drawn
    // primitive has COLOR_0. Parsed in v1, used from v2.
    std::vector<RGBA>       vertex_colors;
    bool                    is_single_material {false};
    bool                    had_textures {false};      // report, never used in v1/v2
    size_t                  dropped_primitives {0};    // points / lines / line loops / strips
    size_t                  skipped_nodes {0};         // over the instance cap
    std::vector<std::string> unsupported_extensions;   // from extensionsRequired
};

// Load a .glb or .gltf into `model` as exactly one ModelObject.
// Returns false and sets `message` (already translated) on every failure.
extern bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message,
                      const char *object_name = nullptr, ImportstlProgressFn progressFn = nullptr);

} // namespace Slic3r
#endif
```

Return contract:

* **true** ⇒ `model->objects.size()` grew by exactly 1, that object has ≥ 1 volume, `info.parts`
  has one entry per volume in the same order, and `message` may still carry a *warning*
  (see 3.8).
* **false** ⇒ `model` is untouched and `message` is a specific, user-facing sentence. Never
  return false with an empty message — `Model.cpp:332-337` would then throw the generic
  "Loading of a model file failed."

### 3.2 What the reader builds

* **One `ModelObject`.** Name = the active glTF scene's name if non-empty, else the file stem.
  `input_file` is stamped by `Model::read_from_file` for every object at `Model.cpp:342-343`, so
  the reader does not have to.
  Because the object has more than one volume, `Model::looks_like_multipart_object()` returns
  false at `Model.cpp:855` and the "several objects at multiple heights" prompt
  (`Plater.cpp:12405-12414`) never fires. That is the intended behaviour, not an accident.
* **One `ModelVolume` per (node, primitive) pair**, `ModelVolumeType::MODEL_PART`.

  *Why the pair and not the mesh:* a glTF `mesh` can be referenced by several `node`s
  (`Models/SimpleMeshes/glTF/SimpleMeshes.gltf` is exactly two nodes sharing one mesh) and each
  reference sits at a different world transform, so the unit has to include the node. And a
  `mesh` is an *array of primitives* whose whole purpose in the spec is to carry different
  materials, so the unit has to include the primitive — otherwise Stage 2 cannot put two
  materials of one mesh on two filaments without per-face painting.

  *Naming*: `node->name` if non-empty, else `mesh->name` if non-empty, else `"part"`. Append
  `"_" + std::to_string(primitive_index + 1)` only when `mesh->primitives_count > 1`. Then
  de-duplicate across the whole object by appending `_2`, `_3`… (SimpleMeshes has two unnamed
  nodes; without this both volumes are called `part`).
* **`source`** (`Model.hpp:805-822`) filled on every volume: `input_file` = the path,
  `object_idx` = the new object's index, `volume_idx` = the volume's index,
  `mesh_offset` = whatever `center_geometry_after_creation()` set (it does that itself,
  `Model.cpp:2695-2696`), and `transform` = the *pre-bake* world matrix (see 3.3) so a future
  round-trip or "reload from disk" can reproduce the placement. This mirrors what
  `bbs_3mf.cpp:4821-4822` does.

### 3.3 Transforms: bake, do not keep

Compute each node's world matrix by composing the tree from the scene root, pre-multiplied by
the fixed up-axis correction (3.4). `cgltf_node_transform_world(node, float[16])` does the
composition for us, including `matrix` vs TRS. **Bake that matrix into the vertices** and give
the volume no transform of its own beyond the centring offset that
`ModelObject::add_volume(TriangleMesh&&)` applies at `Model.cpp:1356-1358` →
`ModelVolume::center_geometry_after_creation()` (`Model.cpp:2681-2697`), which translates the
mesh to its own bbox centre and puts the shift into the volume offset — so world position is
preserved and the volume gets a clean identity rotation/scale.

Rationale (this is a real decision, `Geometry::Transformation` *could* hold the matrix —
it is matrix-backed at `Geometry.hpp:404` and even has `has_skew()` at `:452`):

1. glTF node transforms are authoring artefacts, not user intent. Nothing in the file says
   "this part is rotated 37° relative to that one on purpose"; a 3MF's per-part matrix does.
   After baking, every part in the plater has identity rotation and unit scale, which is what a
   user expects to see in the object list — not a Blender export's leftover 0.01 scale.
2. One code path, one set of test expectations: `volumes[i]->mesh().size()` is directly
   assertable. A conditional bake-if-skewed would need two sets.
3. Negative-determinant node chains (mirrored instances) invert triangle winding. Baking lets us
   fix it once with `its_flip_triangles()` (`TriangleMesh.hpp:203-204`) instead of relying on
   every downstream consumer to respect `Transformation::is_left_handed()`
   (`Geometry.hpp:445-447`).
4. Skew (non-uniform scale under a rotated parent) is common in exported glTF and is a
   second-class citizen everywhere in the slicer's gizmos.

Cost: an instanced mesh is duplicated per instance rather than shared via
`ModelObject::add_volume_with_shared_mesh` (`Model.cpp:1393`). Accepted, with the instance cap
in 3.9.

`Model::looks_like_saved_in_meters()` works either way — `get_object_stl_stats()` scales each
volume's mesh volume by `|det|` of its matrix at `Model.cpp:2521` — but baking means the
determinant is 1 and the number is exact.

### 3.4 Up axis

glTF 2.0 is right-handed **+Y up**, +Z front. The slicer is Z-up. Apply a single
Rx(+90°) at the scene root, i.e. pre-multiply every node's world matrix by

```
[ 1  0  0  0 ]        (x, y, z)_gltf  ->  (x, -z, y)_slicer
[ 0  0 -1  0 ]
[ 0  1  0  0 ]
[ 0  0  0  1 ]
```

Determinant +1, so winding is untouched by this step alone. Because we bake, the rotation ends up
in the vertices and the test in 5.4 asserts it directly.

### 3.5 Units

**1 glTF unit = 1 mm**, no scaling, exactly like STL and OBJ (both unitless;
`STL.cpp:17-40`, `OBJ.cpp:211-228` apply nothing). A genuinely metre-authored file is then tiny
and trips the existing rescue prompt: `Model::looks_like_saved_in_meters()` fires below
0.008 mm³ (`Model.cpp:967-979`) and `Plater.cpp:12364-12371` offers
`Model::convert_from_meters(true)` (`Model.cpp:981-992`). Zero new code. Do **not** set
`ModelVolume::source.is_converted_from_meters` — that flag means "we already scaled it", and
`Model::convert_from_meters` asserts on the sibling flag at `Model.cpp:988`.

### 3.6 Primitive modes, indices, accessors

* Accept `cgltf_primitive_type_triangles`, `_triangle_strip`, `_triangle_fan`. De-strip and
  de-fan into plain triangles in the reader:
  * strip: triangle *i* is `(i, i+1, i+2)` for even *i* and `(i+1, i, i+2)` for odd *i*
    (the spec's winding rule — get this wrong and every other face is inside-out);
  * fan: triangle *i* is `(0, i+1, i+2)`.
* Silently drop `_points`, `_lines`, `_line_loop`, `_line_strip`; count them in
  `GltfInfo::dropped_primitives` and log. If **every** primitive was dropped, return false with
  *"This file contains no printable surfaces (only points or lines)."*
* `primitive->indices == nullptr` ⇒ consume vertices sequentially. Real files do this
  (`Models/TriangleWithoutIndices`). Read indices with
  `cgltf_accessor_unpack_indices(accessor, out, sizeof(uint32_t), count)`, which handles u8/u16/u32.
* Positions: `cgltf_accessor_unpack_floats(accessor, out, count*3)`. This is the call that
  transparently handles **sparse** accessors and `KHR_mesh_quantization` normalization — do not
  hand-roll accessor reads.
* **Normals are ignored.** The slicer stores only `indexed_triangle_set` (positions + indices);
  face normals are derived (`its_face_normal`, `TriangleMesh.hpp:331-334`). A missing `NORMAL`
  attribute is therefore a non-event; do not generate normals.
* `COLOR_0` (`cgltf_attribute_type_color`, index 0): read with `cgltf_accessor_read_float(...,
  4)` per vertex, which normalizes u8/u16 and fills alpha 1.0 for VEC3. Parse in v1, store in
  `GltfInfo::vertex_colors`, use from v2. It is a *multiplier* on `baseColorFactor` per spec, so
  Stage 2 must multiply before converting to sRGB.

### 3.7 Mesh hygiene, and the order it must happen in

Per primitive, in this order:

1. Build `indexed_triangle_set` (positions transformed by the baked world matrix).
2. If `det(world) < 0`, `its_flip_triangles(its)` (`TriangleMesh.hpp:203-204`).
3. `its_merge_vertices(its)` (`TriangleMesh.cpp:674`). This is exact-coordinate welding and is
   **necessary** for glTF: exporters split vertices at every UV/normal seam, so an unwelded
   glTF cube arrives as 12 disconnected triangles, `its_number_of_patches` reports 12 parts and
   `its_num_open_edges` reports 36 — both fed into `TriangleMeshStats` by `fill_initial_stats`
   (`TriangleMesh.cpp:45-54`) and both shown to the user as a manifold warning. Welding fixes it.
   It preserves the `indices` array order, so any per-face array stays aligned.
4. `its_remove_degenerate_faces(its)` (`TriangleMesh.cpp:742`). **This one removes faces**, so it
   must run *before* a per-face colour array is assembled, or the array desyncs and
   `Model::obj_import_face_color_deal`'s size check at `Model.cpp:3282` rejects it. Simplest
   rule: build face colours only after step 4, by re-walking the surviving faces.
5. `TriangleMesh mesh(std::move(its));` — note this constructor only fills stats
   (`TriangleMesh.cpp:71-75`), it does **not** run admesh repair. Repair
   (`trianglemesh_repair_on_import`, `TriangleMesh.cpp:79-179`) is reachable only through
   `TriangleMesh::from_stl` (`:181-184`) and is therefore STL-only today. glTF follows the OBJ
   precedent and does not repair; the object list's manifold warning and the Windows-only
   "Fix Model" action stay the user's remedy. Expect a higher warning rate than with STL.
6. **Do not** copy OBJ's `if (mesh.volume() < 0) mesh.flip_triangles();` (`OBJ.cpp:206-207`).
   That heuristic is wrong for a part that is legitimately an open shell or a cavity, and step 2
   already handles the only well-defined cause of inversion.

### 3.8 `extensionsRequired` and error messages

Read `data->extensions_required[0..extensions_required_count)` **first**, before touching
geometry. Known-and-honoured in v1: none. So:

| Required extension present | v1/v2 behaviour |
|---|---|
| `KHR_draco_mesh_compression` | false, message: *"This file uses Draco mesh compression, which Snapmaker Orca cannot read yet. Re-export it without Draco compression."* |
| `EXT_meshopt_compression` | false, message names the extension verbatim |
| `KHR_mesh_quantization` | **allowed** — `cgltf_accessor_unpack_floats` de-quantizes. Cover it with a test rather than refusing; silently importing at the wrong scale is the one outcome worse than an error. |
| `KHR_texture_basisu`, `KHR_materials_*`, anything else unknown | false, message: *"This file needs the glTF extension \"%1%\", which Snapmaker Orca does not support."* listing all of them |

`extensionsUsed` (as opposed to *required*) is advisory — never refuse on it. Textures are the
common case there: set `GltfInfo::had_textures` and carry a *warning* in `message` while still
returning true (see 4.4 for how the GUI surfaces it).

### 3.9 Reading the file, buffers and limits

* **Never call `cgltf_parse_file`.** cgltf's default file callbacks use `fopen`, which on Windows
  cannot open a UTF-8 path with non-ASCII characters — and non-ASCII import paths are already a
  guarded case in this repo (`tests/libslic3r/test_stl.cpp:18` loads
  `Geräte/20mmbox-čřšřěá.stl`). Instead: read the whole file with `boost::nowide::ifstream`
  (the house style, e.g. `RemoteHub.cpp:1076`) into a `std::vector<char>` and call
  `cgltf_parse(&options, data, size, &out)`.
* For `.gltf` with a sidecar `.bin`, `cgltf_load_buffers(&options, data, gltf_path)` resolves
  URIs. Supply `options.file.read` / `options.file.release`
  (`cgltf_file_options`, see 3.10) with a nowide implementation that **rejects absolute paths and
  any `..` segment**, resolving strictly under the `.gltf`'s own directory. This is the same
  posture as the OBJ/MTL sibling handling at `OBJ.cpp:44-58`, and it matters because the phone
  upload path (`RemoteHub.cpp:1050`) writes an attacker-controllable file.
* `data:` URIs are decoded by cgltf itself; cap the total decoded buffer size.
* Caps, all of which produce a named error rather than an OOM:
  `MAX_GLTF_FILE` 512 MB (the hub already caps an upload at 2 GB, `RemoteHub.cpp:76`),
  `MAX_GLTF_TRIANGLES` 20,000,000 total, `MAX_GLTF_VOLUMES` 2,000 (`GltfInfo::skipped_nodes`
  counts what a node-instance explosion would have added).
* Progress/cancel: accept `ImportstlProgressFn`
  (`std::function<void(int current,int total,bool& cancel,std::string& model_id,std::string& code)>`,
  `deps_src/admesh/stl.h:48`) and call it once per primitive with the two `std::string&` outputs
  left untouched — the Plater lambda at `Plater.cpp:12285-12297` assigns them to
  `designer_model_id`/`designer_country_code`, which for glTF should simply stay empty.
  Honouring `cancel` lets the existing `ProgressDialog` in `Plater::priv::load_files` cancel a
  large import.

### 3.10 cgltf facts this plan relies on

Checked against `https://raw.githubusercontent.com/jkuhlmann/cgltf/master/cgltf.h` on 2026-09-02:
MIT, single header, version 1.15, no dependencies beyond the C standard library (it embeds JSMN).
API used: `cgltf_parse`, `cgltf_load_buffers`, `cgltf_validate`, `cgltf_free`,
`cgltf_node_transform_world`, `cgltf_accessor_unpack_floats`, `cgltf_accessor_unpack_indices`,
`cgltf_accessor_read_float`, `cgltf_num_components`.
Structures used: `cgltf_options{type, json_token_count, memory, file}` with
`cgltf_file_options{read, release, user_data}`; `cgltf_data{file_type, asset, scene, scenes,
scenes_count, nodes, nodes_count, meshes, materials, extensions_required,
extensions_required_count}`; `cgltf_node{name, parent, children, children_count, mesh,
has_matrix/matrix, has_translation/translation, has_rotation/rotation, has_scale/scale}`;
`cgltf_mesh{name, primitives, primitives_count}`;
`cgltf_primitive{type, indices, material, attributes, attributes_count,
has_draco_mesh_compression}`; `cgltf_attribute{name, type, index, data}` with
`cgltf_attribute_type_position|normal|texcoord|color`;
`cgltf_material{name, has_pbr_metallic_roughness, pbr_metallic_roughness}`;
`cgltf_pbr_metallic_roughness{base_color_texture, base_color_factor[4]}`;
`cgltf_texture_view{texture, ...}`.
`cgltf_primitive::has_draco_mesh_compression` means cgltf *recognised* the extension, not that it
can decode it — decoding needs Draco (Stage 3).

## 4. Decisions taken in this plan

1. **One object with parts** (user decision). Per-(node, primitive) volumes, justified in 3.2.
   No "split to objects" prompt; the user can still use the existing Split action.
2. **Bake node transforms** rather than keep them as volume transforms (3.3). This is the one
   place where I deviated from the research document's §4.2 recommendation, and 3.3 says why.
3. **1 unit = 1 mm** (user decision, 3.5), relying on the existing metres prompt.
4. **cgltf in `deps_src/`**, not Assimp in `deps/` (user decision: no FBX ⇒ no reason to take a
   multi-MB dependency and a full deps rebuild on three platforms).
5. **Textures ignored in v1/v2**, but *reported* — a silently colourless import is the outcome
   users file bugs about.
6. **Stage 2 assigns whole parts to filaments, not per-face paint.** Because a glTF material is
   per-primitive and a primitive is a volume, the natural result is
   `volume->config.set("extruder", id)` — no `mmu_segmentation_facets` at all. Painting is only
   needed for `COLOR_0`. This is both cheaper (the k-means dialog sees one colour per material,
   not one per triangle) and a better result (parts stay editable).
7. **`.glb`/`.gltf` are added to the phone upload allow-list in Stage 1**, not deferred. The
   research flagged this as an open question (§5.4.5). The reasoning for going ahead: cgltf is
   ~7 kLOC of C with no allocation-from-header-field patterns, we control the buffer caps
   (3.9), and the endpoint already accepts `.3mf` (a zip + XML parser) and `.step` (OCCT) — both
   materially larger attack surfaces than cgltf. The mitigation is the fuzz target in 5.6, which
   is a Stage 1 deliverable, not a follow-up.
8. **No `FT_GLTF` FileType entry.** `.glb`/`.gltf` go into `FT_MODEL` only. A dedicated
   single-format filter buys nothing (there is no export path), and every new `FileType` has to
   be inserted in the right slot of the `file_wildcards_by_type[FT_SIZE]` array at
   `GUI_App.cpp:652-677`, which is positional and easy to break.
9. **No `associate_glb` checkbox in v1.** The Windows association machinery
   (`GUI_App.cpp:2280-2286`, `:2827-2839`, `:6382-6395`, `:7599-7602`, Preferences at
   `Preferences.cpp:892-911`, `:1403-1406`) is four coordinated edits for a format nothing else
   on the machine claims by default. Revisit if users ask.

## 5. Known unknowns to settle first (cheap experiments)

Each is under an hour and removes a real risk from the estimate.

1. **cgltf compiles clean under MSVC at this repo's warning level.** `cgltf.h` is C99; the
   single translation unit that defines `CGLTF_IMPLEMENTATION` must be `GLTF.cpp` (C++). Try it
   before anything else; if MSVC complains, the fallback is a tiny `cgltf_impl.c` compiled as C
   and added to the `libslic3r` source list. *Experiment:* a throwaway `.cpp` with
   `#define CGLTF_IMPLEMENTATION` / `#include <cgltf.h>` added to `libslic3r` and built.
2. **`deps_src` really is on libslic3r's include path.** It is — but only as a side effect:
   `deps_src/admesh/CMakeLists.txt:14` exports `${CMAKE_CURRENT_SOURCE_DIR}/..` and admesh is
   `PUBLIC`-linked at `src/libslic3r/CMakeLists.txt:588`, which is why
   `#include <fast_float/fast_float.h>` works from `GCodeReader.cpp:15` even though `fast_float`
   is never linked. **Do not rely on that.** Link the new `cgltf` INTERFACE target explicitly
   (change 1.3) and include `<cgltf/cgltf.h>`.
3. **`QuantKMeans` behaves with a tiny input.** `ObjColorPanel::deal_algo`
   (`ObjColorDialog.cpp:810-837`) runs `QuantKMeans quant(10)` over `m_input_colors`. Stage 2
   feeds it 1–16 colours instead of one per triangle. *Experiment:* call `deal_algo` with 1, 2
   and 30 colours in a scratch build and check `m_cluster_colors_from_algo.size()` and that
   `deal_default_strategy()` does not divide by zero.
4. **`cgltf_node_transform_world` composes the whole chain.** Confirm on
   `Models/SimpleMeshes` (two nodes, one at `translation [1,0,0]`) that the two volumes land 1 mm
   apart in X after the up-axis rotation.
5. **Winding of TRIANGLE_STRIP.** Author a 2-triangle strip fixture with a known outward normal
   and confirm the odd-triangle swap produces a positive `its_volume` when closed.

## 6. Must not change

* **STL / OLTP / OBJ / SVG / AMF / 3MF / STEP import.** Stage 1 adds one `else if` before the
  `else` at `Model.cpp:324-325` and edits the message string in that `throw`; nothing else in the
  dispatch moves. `Model::obj_import_face_color_deal` and
  `Model::obj_import_vertex_color_deal` (`Model.cpp:3270`, `:3148`) keep their exact current
  behaviour and signatures — Stage 2 adds *siblings*, it does not generalise these in place.
  `tests/libslic3r/test_stl.cpp` and `test_3mf.cpp` must stay green untouched.
* **The `.obj` colour dialog.** Widening the guards at `Plater.cpp:12225` and `:14679` from
  `.obj` to `.obj|.glb|.gltf` must not change what an `.obj` does. Keep the guard a positive
  extension list, not a "not a project file" test.
* **`ObjColorDialog` itself** (`ObjColorDialog.hpp:20-116`). Stage 2 reuses it verbatim,
  including its k-means, "Color match", "keep colour" and "add filament" buttons. No new
  callback type: `ObjImportColorFn` (`OBJ.hpp:10`) is the interface.
* **Phone features.** `RemoteHub::spool_upload` gains two extensions in its allow-list
  (`RemoteHub.cpp:1062-1065`) and `stream_center.html:1161` gains them in its regex and message.
  Nothing else on the phone path changes: `RemoteAccess::api_project_open`
  (`RemoteAccess.cpp:1562-1624`) keeps its `mode == "load"` ⇒ `.3mf`-only rule at `:1574`, its
  uploads-directory containment check at `:1569`, and its `Plater::load_files(files,
  LoadStrategy::LoadModel)` call at `:1611`. A `.glb` arrives as `mode == "import"` and needs no
  new code there.
* **The hidden-service instance.** A `.glb` imported from the phone on a hidden instance must not
  block on a modal. In v1 there is no dialog, so nothing to do. In v2 the colour dialog becomes
  reachable from `api_project_open`; it is a `DPIDialog`, so the Stage 3 `wxModalDialogHook` from
  `docs/superpowers/plans/2026-09-02-hidden-service-mode.md` answers it — but the auto-answer for
  a custom-button dialog is *cancel*, which clears `filament_ids` at `Plater.cpp:12229` and
  imports without colour. That is the correct, safe outcome; write it down in the v2 release note
  rather than "fixing" it.
* **The CLI.** `src/Snapmaker_Orca.cpp:1654` calls `Model::read_from_file` unconditionally for
  every non-first-3mf input, so `--load model.glb` works with **zero** CLI changes. Do not add a
  special case.

---

## Stage 1 — Geometry

### Findings

**The dispatch and its tail** (`src/libslic3r/Model.cpp`)

* `:229-241` — `Model::read_from_file(input_file, config, config_substitutions, options,
  plate_data, project_presets, is_xxx, file_version, proFn, stlFn, project, plate_id, objFn)`.
  Declared at `Model.hpp:1596-1605`.
* `:262-264` — `bool result = false; bool is_cb_cancel = false; std::string message;`
* `:265` `.stl` → `load_stl`; `:267` `.oltp` → `load_stl(..., 256)`; `:269-298` `.obj` (colour
  post-processing inside); `:299` `.svg`; `:303` `.amf`; `:306` `.3mf`; `:312-323` Apple-only
  ModelIO formats; `:324-325` `else throw Slic3r::RuntimeError(_L("Unknown file format. Input
  file must have .stl, .obj, .amf(.xml) extension."));`
* `:332-337` — `if (!result) { throw message.empty() ? generic : message; }`. This is why a
  failing `load_gltf` must always set `message`.
* `:339-340` empty-model rejection; `:342-343` `o->input_file = input_file` for every object;
  `:345-346` `AddDefaultInstances`.
* The `.obj` block's shape is the template for the glTF block: `:271` load, `:274` "vertex
  colours present?", `:282` "face colours present and no texture?", `:277`/`:285` call `objFn`,
  `:279`/`:287` apply.

**How a format builds a Model**

* `Format/STL.cpp:17-40` and `Format/OBJ.cpp:211-228` both end in
  `model->add_object(object_name.c_str(), path, std::move(mesh))` — one object, one volume.
  `Model::add_object(name, path, TriangleMesh&&)` is `Model.cpp:466-482`: it creates the object,
  calls `ModelObject::add_volume(std::move(mesh))`, names the volume after the object, and fills
  `source.input_file/object_idx/volume_idx`.
* The multi-volume precedent is `Format/AMF.cpp:301` (`add_object()`) + `:374`
  (`add_volume(TriangleMesh())`), and `Format/bbs_3mf.cpp:4806` + `:4827-4828`
  (`volume->set_transformation(comp * volume->get_transformation())` — i.e. the file's matrix
  composed *onto* the centring offset, never replacing it).
* `ModelObject::add_volume(TriangleMesh&&, type, modify_to_center_geometry = true)` is
  `Model.cpp:1352-1363`; the centring is `ModelVolume::center_geometry_after_creation()`,
  `Model.cpp:2681-2697`.
* `TriangleMesh(indexed_triangle_set&&)` is `TriangleMesh.hpp:94` / `TriangleMesh.cpp:71-75` —
  stats only, no repair (`fill_initial_stats`, `TriangleMesh.cpp:45-54`).
* Mesh helpers: `its_flip_triangles` `TriangleMesh.hpp:204`, `its_merge_vertices` `:209`
  (impl `TriangleMesh.cpp:674-740`, exact float equality, order-preserving),
  `its_remove_degenerate_faces` `:212` (impl `:742-755`, **erases faces**),
  `its_compactify_vertices` `:215`.

**Where extensions are whitelisted** (all re-verified)

| Where | Reference | Current value |
|---|---|---|
| `FileType` enum | `GUI_App.hpp:103-125` | `FT_STEP … FT_MODEL … FT_SL1, FT_SIZE` |
| Wildcard table | `GUI_App.cpp:652-677` | `FT_MODEL` at `:661-664` (Apple) and `:666-667` (rest) |
| Add-model dialog label | `GUI_App.cpp:4314-4321` | `"Choose one or more files (3mf/step/stl/svg/obj/amf):"` (`:4318`), Apple variant `:4316` |
| Import menu label | `MainFrame.cpp:2695` and `:2701` | `"Import 3MF/STL/STEP/SVG/OBJ/AMF"` |
| Drag-and-drop | `Plater.cpp:20063` | `".*[.](stp\|step\|stl\|oltp\|obj\|amf\|3mf\|svg\|zip)"`; the drop target is `Plater.cpp:9972-9979`, `OnDropFiles` at `:10611` |
| Loader dispatch | `Model.cpp:265-325` | above |
| Raw-geometry flag | `Plater.cpp:11633-11637` | already lists `.glb`, `.gltf`, `.fbx` — **dead code today**, becomes live in Stage 1 and drives `Model::InitializeAssemblyPositions` at `:12476-12488` |
| macOS document types | `Info.plist.in:40-109` | `stl`, `obj`, `amf`, `3mf` (+ gcode further down) |
| CLI `--load` | `Snapmaker_Orca.cpp:1620-1654` | `read_from_file` at `:1654`, no extension list |
| Phone upload (server) | `RemoteHub.cpp:1062-1065` | `.3mf .stl .obj .step .stp`, message at `:1063` |
| Phone upload (manifest) | `RemoteHub.cpp:1958` | `"body = a .3mf/.stl/.obj/.step file…"` |
| Phone upload (client) | `stream_center.html:1161` | `/\.(3mf\|stl\|obj\|step\|stp)$/i` + message; picker label `:1154` |

**Post-load behaviour a glTF import inherits for free** (`Plater::priv::load_files`,
`Plater.cpp:11528`)

* `:11548` `one_by_one = input_files.size() == 1 || ptSLA` — with a single dropped file each
  object in the loaded `Model` becomes a plater object via `load_model_objects` (`:12474`).
* `:12301-12312` empty-name fallback and the `preferred_orientation` Z rotation.
* `:12358-12360` zero-volume object removal; `:12364-12371` the metres prompt;
  `:12372-12380` the imperial prompt.
* `:12405-12414` the multipart prompt — cannot fire for us (3.2).
* `:12436` `center_around_origin(false)` for non-3mf/amf; `:12449-12450` `ensure_on_bed()` behind
  this fork's `auto_drop_on_import` preference.
* `:12476-12488` `Model::InitializeAssemblyPositions` for raw geometry imports.
* `:12513-12523` the "load these files as a single object with multiple parts?" prompt when
  several files were dropped. Note `Model::convert_multipart_object` (`Model.cpp:877-925`)
  renames every volume to the object name at `:899-901` — a pre-existing wart (the `counter++`
  only fires in the branch that is never taken first) that would flatten our part names. Not a
  blocker, not ours to fix here; mention in the release note.

**Build wiring**

* `deps_src/CMakeLists.txt:6-13` — the header-only group (`agg, ankerl, fast_float, nanosvg,
  nlohmann, spline, stb_dxt`).
* `deps_src/fast_float/CMakeLists.txt:1-16` — the INTERFACE-target template
  (`add_library(... INTERFACE)`, `target_include_directories(... SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR})`, `target_sources(... INTERFACE ...)`,
  `target_compile_features(... INTERFACE cxx_std_14)`).
  `deps_src/nlohmann/CMakeLists.txt:7-10` is the variant that exports `${...}/..` so the include
  is `<nlohmann/json.hpp>`.
* `src/libslic3r/CMakeLists.txt:192-212` — the `Format/*.cpp|.hpp` block;
  `:586-614` — `target_link_libraries(libslic3r PUBLIC admesh libigl libnest2d miniz opencv_world
  PRIVATE … qhull qoi semver TBB::tbb …)`.
* `CMakeLists.txt:878` `add_subdirectory(deps_src)` before `:879 add_subdirectory(src)`.

**Tests**

* `tests/CMakeLists.txt:4-5` sets `TEST_DATA_DIR` to `tests/data`; `:20-22` the `test_common`
  INTERFACE target carries it as a compile definition.
* `tests/libslic3r/CMakeLists.txt:3-36` lists the sources (alphabetical-ish, `test_stl.cpp` at
  `:27`); `:42` links `test_common libslic3r OpenSSL::Crypto`.
* `tests/libslic3r/test_stl.cpp:8-11` is the `*_path()` helper pattern; `:13-22` the non-ASCII
  scenario; `:19` `REQUIRE(is_approx(model.objects.front()->volumes.front()->mesh().size(),
  Vec3d(20,20,20)))` — the assertion style to copy.
* `tests/data/` currently holds ~25 loose `.obj` files plus `test_3mf/`, `test_stl/`,
  `test_config/`. **`tests/` also has untracked stray files today** (`h2d_*.gcode`,
  `testload*.3mf`, `h2d_5color.3mf`, `scripts/__pycache__/`) — do not `git add -A` in this
  directory; add the new fixtures by explicit path.

### Changes

#### 1.1 `deps_src/cgltf/` — new vendored library

Two files:

* `deps_src/cgltf/cgltf.h` — v1.15 verbatim from
  <https://github.com/jkuhlmann/cgltf/releases/tag/v1.15> (MIT; the licence text is at the end of
  the header and must be kept). ~440 KB, one file.
* `deps_src/cgltf/CMakeLists.txt`, mirroring `deps_src/nlohmann/CMakeLists.txt:1-21` (the `..`
  variant, so the include is `<cgltf/cgltf.h>` and cannot collide):

```cmake
cmake_minimum_required(VERSION 3.13)

project(cgltf)

add_library(cgltf INTERFACE)

target_include_directories(cgltf SYSTEM
    INTERFACE
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_sources(cgltf INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/cgltf.h
)

# cgltf.h is C99; the single implementation TU is compiled as C++ inside libslic3r.
target_compile_features(cgltf INTERFACE cxx_std_14)
```

#### 1.2 `deps_src/CMakeLists.txt:6-13` — register it

```cmake
 # Header-only libraries (INTERFACE)
 add_subdirectory(agg)
 add_subdirectory(ankerl)
+add_subdirectory(cgltf)       # glTF 2.0 / GLB parser (single header, MIT)
 add_subdirectory(fast_float)
```

#### 1.3 `src/libslic3r/CMakeLists.txt` — sources and link

At `:199-200` (keeping the block's rough alphabetical order):

```cmake
     Format/bbs_3mf.cpp
     Format/bbs_3mf.hpp
+    Format/GLTF.cpp
+    Format/GLTF.hpp
     Format/OBJ.cpp
```

and in the `PRIVATE` list at `:593-613`, after `boost_libs`:

```cmake
         cereal::cereal
+        cgltf
         clipper
```

(Explicit, despite finding 5.2 — do not inherit the include path from admesh.)

#### 1.4 `src/libslic3r/Format/GLTF.hpp` — new

Exactly the header in 3.1.

#### 1.5 `src/libslic3r/Format/GLTF.cpp` — new (~650-800 lines)

Structure, in file order:

```cpp
#include "../libslic3r.h"
#include "../Model.hpp"
#include "../TriangleMesh.hpp"
#include "GLTF.hpp"

#include <boost/nowide/fstream.hpp>
#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include "I18N.hpp"
#define _L(s) Slic3r::I18N::translate(s)

#define CGLTF_IMPLEMENTATION            // this TU owns the implementation
#include <cgltf/cgltf.h>

namespace Slic3r {
namespace {

// --- limits (3.9) -----------------------------------------------------------
constexpr uint64_t MAX_GLTF_FILE      = 512ull * 1024 * 1024;
constexpr size_t   MAX_GLTF_TRIANGLES = 20u * 1000 * 1000;
constexpr size_t   MAX_GLTF_VOLUMES   = 2000;

// --- helpers ----------------------------------------------------------------
bool     read_whole_file(const char *path, std::vector<char> &out, std::string &message);
cgltf_result nowide_file_read(const cgltf_memory_options*, const cgltf_file_options*,
                              const char *path, cgltf_size *size, void **data);   // + release
bool     buffer_uri_is_safe(const std::string &uri, const boost::filesystem::path &base);
Matrix4d up_axis_correction();                       // Rx(+90): (x,y,z) -> (x,-z,y)
float    linear_to_srgb(float c);                    // for GltfInfo::material_colors
std::string unique_part_name(std::string base, std::set<std::string> &taken);

// --- geometry ---------------------------------------------------------------
// Append primitive `prim` transformed by `world` to `its`; returns false + message on error.
bool append_primitive(const cgltf_primitive &prim, const Matrix4d &world,
                      indexed_triangle_set &its, std::vector<RGBA> *color0,
                      std::string &message);
// Recursive scene walk. `world` already includes the up-axis correction.
void walk_node(const cgltf_node &node, const Matrix4d &parent_world, ...);

} // anonymous namespace

bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message,
               const char *object_name_in, ImportstlProgressFn progressFn)
{ ... }
} // namespace Slic3r
```

Body of `load_gltf`, in order:

1. `read_whole_file` (nowide, size-capped) → `cgltf_parse` → on any `cgltf_result` other than
   `cgltf_result_success`, map to a specific message
   (`cgltf_result_unknown_format` → *"This is not a glTF or GLB file."*;
   `cgltf_result_legacy_gltf` → *"This is a glTF 1.0 file; only glTF 2.0 is supported."*;
   `cgltf_result_data_too_short`/`invalid_json`/`invalid_gltf` → *"This glTF file is damaged or
   incomplete."*). RAII the `cgltf_data*`.
2. `extensions_required` check (3.8) — before any allocation proportional to file content.
3. `cgltf_load_buffers` with the nowide + containment file callbacks; failure →
   *"A data file this glTF refers to is missing (%1%)."*
4. `cgltf_validate` — log a warning, do not refuse (real-world files trip it on cosmetic
   things).
5. Pick the scene: `data->scene ? *data->scene : data->scenes[0]`; if `scenes_count == 0`, walk
   all `data->nodes` with no parent.
6. `walk_node` over the roots with `world = up_axis_correction()`, using
   `cgltf_node_transform_world` for the node's own chain (it walks to the root itself, so call it
   per *mesh-bearing* node and pre-multiply the correction once, rather than composing by hand —
   this also gets `matrix` vs TRS right for free).
7. Per (node, primitive): mode filter (3.6) → `append_primitive` → hygiene (3.7) →
   `TriangleMesh` → collect into a `std::vector<std::pair<std::string, TriangleMesh>>` plus a
   parallel `GltfPart`. Progress callback per primitive; abort on `cancel`.
8. If nothing survived → false with the "no printable surfaces" message.
9. Build the object: `ModelObject *obj = model->add_object();` then set `obj->name`,
   `obj->input_file`; for each part `ModelVolume *v = obj->add_volume(std::move(mesh),
   ModelVolumeType::MODEL_PART);` (default `modify_to_center_geometry = true`), then
   `v->name`, `v->source.input_file/object_idx/volume_idx`, and
   `v->source.transform = Geometry::Transformation(pre_bake_world)`.
   Finally `obj->invalidate_bounding_box();` and, mirroring `Model.cpp:478-479`,
   `if (!obj->config.has("extruder") || obj->config.extruder() == 0)
    obj->config.set_key_value("extruder", new ConfigOptionInt(0));`
10. Fill `info`. If `had_textures`, append a warning sentence to `message` and still return true.

#### 1.6 `src/libslic3r/Model.cpp:322-325` — the dispatch

Insert immediately before the `else` at `:324`:

```cpp
    else if (boost::algorithm::iends_with(input_file, ".glb") ||
             boost::algorithm::iends_with(input_file, ".gltf")) {
        GltfInfo gltf_info;
        result = load_gltf(input_file.c_str(), &model, gltf_info, message, nullptr, stlFn);
        // Stage 2 hooks the objFn colour path in here.
    }
    else
        throw Slic3r::RuntimeError(_L("Unknown file format. Input file must have .stl, .obj, "
                                      ".amf(.xml), .glb or .gltf extension."));
```

and `#include "Format/GLTF.hpp"` next to the other `Format/` includes at the top of `Model.cpp`.
Note the message at `:325` is a translated string — update `localization/i18n` in the usual pass,
do not invent a new key.

#### 1.7 `src/slic3r/GUI/GUI_App.cpp:661-667` — the file dialog filter

```cpp
 #ifdef __APPLE__
     /* FT_MODEL */
     {"Supported files"sv,
      {".3mf"sv, ".stl"sv, ".oltp"sv, ".stp"sv, ".step"sv, ".svg"sv, ".amf"sv, ".obj"sv,
-      ".usd"sv, ".usda"sv, ".usdc"sv, ".usdz"sv, ".abc"sv, ".ply"sv}},
+      ".glb"sv, ".gltf"sv, ".usd"sv, ".usda"sv, ".usdc"sv, ".usdz"sv, ".abc"sv, ".ply"sv}},
 #else
     /* FT_MODEL */
-    {"Supported files"sv, {".3mf"sv, ".stl"sv, ".oltp"sv, ".stp"sv, ".step"sv, ".svg"sv, ".amf"sv, ".obj"sv}},
+    {"Supported files"sv, {".3mf"sv, ".stl"sv, ".oltp"sv, ".stp"sv, ".step"sv, ".svg"sv, ".amf"sv, ".obj"sv, ".glb"sv, ".gltf"sv}},
 #endif
```

Both branches, or macOS silently keeps rejecting them.

#### 1.8 `src/slic3r/GUI/GUI_App.cpp:4316` and `:4318` — the dialog title

`"Choose one or more files (3mf/step/stl/svg/obj/amf/glb):"` (and the Apple variant).

#### 1.9 `src/slic3r/GUI/MainFrame.cpp:2695` and `:2701` — the menu label

`_L("Import 3MF/STL/STEP/SVG/OBJ/AMF/GLB")` in both `#ifdef` branches.

#### 1.10 `src/slic3r/GUI/Plater.cpp:20063` — drag-and-drop

```cpp
    const std::regex pattern_drop(".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip|glb|gltf)", std::regex::icase);
```

#### 1.11 `src/slic3r/GUI/RemoteHub.cpp:1062-1065` and `:1958` — phone upload (server)

```cpp
     const std::string ext = lower(fs::path(clean).extension().string());
-    if (ext != ".3mf" && ext != ".stl" && ext != ".obj" && ext != ".step" && ext != ".stp") {
-        error = "only .3mf, .stl, .obj and .step files can be opened";
+    if (ext != ".3mf" && ext != ".stl" && ext != ".obj" && ext != ".step" && ext != ".stp" &&
+        ext != ".glb" && ext != ".gltf") {
+        error = "only .3mf, .stl, .obj, .step and .glb files can be opened";
         return false;
     }
```

and the manifest description at `:1958`:
`"body = a .3mf/.stl/.obj/.step/.glb file, header X-File-Name = its name; …"`.

#### 1.12 `resources/web/orca/stream_center.html:1154` and `:1161` — phone upload (client)

```js
        var pick = el('label', 'btn dim', chosenFile ? chosenFile.name : 'Choose 3MF / STL / GLB…');
...
            if (f && !/\.(3mf|stl|obj|step|stp|glb|gltf)$/i.test(f.name)) { chosenFile = null; uploadErr = true; uploadNote = 'Only .3mf, .stl, .obj, .step and .glb files can be opened (' + f.name + ')'; }
```

The comment at `:1156-1157` (no `accept` filter because phones grey out unknown extensions)
stays true and applies doubly to `.glb`.

#### 1.13 `src/dev-utils/platform/osx/Info.plist.in:40+` — macOS document type

One more `<dict>` in `CFBundleDocumentTypes`, copying the `obj` block at `:59-75`:

```xml
    <dict>
      <key>CFBundleTypeExtensions</key>
      <array>
        <string>glb</string>
        <string>GLB</string>
        <string>gltf</string>
        <string>GLTF</string>
      </array>
      <key>CFBundleTypeIconFile</key>
      <string>images/Snapmaker_Orca.icns</string>
      <key>CFBundleTypeName</key>
      <string>GLB</string>
      <key>CFBundleTypeRole</key>
      <string>Viewer</string>
      <key>LISsAppleDefaultForType</key>
      <true/>
      <key>LSHandlerRank</key>
      <string>Alternate</string>
    </dict>
```

(Note the `obj` block's `CFBundleTypeName` says `STL` at `:68` — a pre-existing copy-paste bug.
Do not replicate it.)

#### 1.14 CLI, Windows file association, `shouldInitializeAssemblyPosition`

* CLI: **no change** (`Snapmaker_Orca.cpp:1654`).
* Windows association: **no change** in v1 (decision 9).
* `Plater.cpp:11633-11637`: **no change** — the `.glb`/`.gltf`/`.fbx` checks are already there
  and become live. Confirm in the manual checklist that `InitializeAssemblyPositions`
  (`:12476-12488`) behaves for a multi-volume object.

#### 1.15 `tests/libslic3r/test_gltf.cpp` — new, and `tests/libslic3r/CMakeLists.txt:27`

Add `test_gltf.cpp` next to `test_stl.cpp` in the source list. The test file follows
`test_stl.cpp:8-22` exactly:

```cpp
#include <catch2/catch.hpp>

#include "libslic3r/Model.hpp"
#include "libslic3r/Format/GLTF.hpp"

using namespace Slic3r;

static inline std::string gltf_path(const char *p)
{
    return std::string(TEST_DATA_DIR) + "/test_gltf/" + p;
}
```

### Test assets

Under `tests/data/test_gltf/`. Two groups.

**A. Self-authored (no licence question, exact known numbers).** Author with
`gltf-transform` (<https://gltf-transform.dev>) or a 30-line Python script committed alongside as
`tests/data/test_gltf/make_fixtures.py`; keep each file under 5 KB.

| File | What it is | Asserts |
|---|---|---|
| `box_10_20_30.glb` | a box 10 (X) × 20 (Y) × 30 (Z) in glTF space, single node at identity, single material | `volumes.front()->mesh().size() == Vec3d(10, 30, 20)` — **the up-axis rule and the unit rule in one line**. The single most valuable test in the suite. |
| `box_10_20_30.gltf` + `box_10_20_30.bin` | the same box, external buffer | identical geometry to the `.glb` |
| `two_parts_two_materials.glb` | one mesh, two primitives, `baseColorFactor` red and blue | 2 volumes; `info.material_colors.size() == 2`; `info.parts[i].material_index` set — the Stage 2 fixture, parsed already in Stage 1 |
| `nested_trs.glb` | parent node `translation [5,0,0]`, `rotation` 90° about Y, child node `scale [2,2,2]` with a 1 mm cube | hand-computed world position of the cube's centre after the up-axis rotation |
| `strip_and_fan.glb` | one TRIANGLE_STRIP primitive (4 verts → 2 tris) and one TRIANGLE_FAN (5 verts → 3 tris) | 2 volumes with 2 and 3 triangles, positive `its_volume` on a closed variant (finding 5.5) |
| `points_only.glb` | a single POINTS primitive | `load_gltf` returns false, message contains "points or lines" |
| `box_draco.glb` | `box_10_20_30.glb` run through `gltf-transform draco` | Stage 1: false, message contains "Draco". Stage 3: same geometry as `box_10_20_30.glb` |
| `Geräte/box-čřšřěá.glb` | a copy of `box_10_20_30.glb` at a non-ASCII path | loads (guards the nowide file read, mirroring `test_stl.cpp:18`) |

**B. Khronos glTF-Sample-Assets, CC0 only.** Repo
<https://github.com/KhronosGroup/glTF-Sample-Assets>; the repo itself is CC-BY-4.0 but each model
carries its own licence in its `README.md`/`LICENSE.md`. The four below were checked on
2026-09-02 and are **CC0 1.0 Universal** ("© 2017/2023, Public", credit Marco Hutter), so they can
be vendored with no attribution obligation — record the provenance in
`tests/data/test_gltf/SOURCES.md` anyway.

| Vendored as | Source (raw URL under `.../glTF-Sample-Assets/main/`) | Size | Covers |
|---|---|---|---|
| `SimpleMeshes.gltf` + `SimpleMeshes.bin` | `Models/SimpleMeshes/glTF/SimpleMeshes.{gltf,bin}` | 1359 B + 80 B | two nodes sharing one mesh → **2 volumes**, and an external `.bin` |
| `TriangleWithoutIndices.gltf` + `.bin` | `Models/TriangleWithoutIndices/glTF/TriangleWithoutIndices.{gltf,bin}` | 747 B + 36 B | non-indexed primitive → 1 triangle |
| `SimpleSparseAccessor.gltf` (+ its `.bin`) | `Models/SimpleSparseAccessor/glTF/` | ~1 KB | sparse accessors (the gotcha a hand-rolled reader fails) |
| `BoxVertexColors.glb` | `Models/BoxVertexColors/glTF-Binary/BoxVertexColors.glb` | 1924 B | `COLOR_0` → `GltfInfo::vertex_colors.size() == vertex count` |

`Models/Box` (the obvious choice) is **CC-BY-4.0, © 2017 Cesium** — *not* CC0. Use it only if a
Draco fixture from `Models/Box/glTF-Draco/Box.gltf` (2427 B + 120 B) turns out to be easier to
get than generating one, and then record the attribution. Preferred: generate `box_draco.glb`
ourselves.

Add every fixture to git **by explicit path** — `tests/` currently has a dozen untracked stray
files (`tests/h2d_*.gcode`, `tests/testload*.3mf`, `scripts/__pycache__/`) that must not be swept
in.

### Gate

**Automated** — `build/tests/libslic3r/libslic3r_tests "[gltf]"` green, and the full
`libslic3r_tests` suite still green (guards the "must not change" list):

1. `box_10_20_30.glb` → 1 object, 1 volume, `mesh().size() == Vec3d(10,30,20)`.
2. `.gltf` + `.bin` produces geometry identical to the `.glb` (compare `its.vertices` sorted, or
   `mesh().size()` + `its_volume`).
3. `SimpleMeshes.gltf` → 1 object, 2 volumes, distinct names, 1 mm apart in X.
4. `two_parts_two_materials.glb` → 2 volumes, `info.material_colors.size() == 2`.
5. `nested_trs.glb` → hand-computed centre within `EPSILON`.
6. `strip_and_fan.glb` → 2 and 3 triangles.
7. `TriangleWithoutIndices.gltf` → 1 triangle.
8. `SimpleSparseAccessor.gltf` → the sparse-overridden vertex is where the spec says.
9. `points_only.glb` → false, message contains "points or lines".
10. `box_draco.glb` → false, message contains "Draco".
11. `BoxVertexColors.glb` → true, `info.vertex_colors.size() == its.vertices.size()`.
12. `Geräte/box-čřšřěá.glb` → true.
13. A truncated `box_10_20_30.glb` (first 200 bytes) → false, message is the "damaged or
    incomplete" one, no crash, no leak (run under `_CrtSetDbgFlag` or ASan).

**Manual checklist** (one build, ~30 minutes):

* [ ] File ▸ Import shows `.glb`/`.gltf` in "Supported files" and opens one.
* [ ] Drag a `.glb` onto the plater — it loads (this exercises `pattern_drop`).
* [ ] The object list shows one object with the expected part names, each part selectable and
      movable.
* [ ] A metre-scale `.glb` (box 0.01 units) triggers "The object from file … is too small…" and
      Yes scales it to 10 mm.
* [ ] `Snapmaker_Orca.exe --load box_10_20_30.glb --export-3mf out.3mf` works from the CLI.
* [ ] Phone: pick a `.glb` in the Stream page, "New" opens it in a fresh instance; a `.gif`
      is still rejected with the new message.
* [ ] **Slice Compare control**: slice `box_10_20_30.glb` and the same box as
      `box_10_20_30.stl`, then compare the two snapshots
      (`src/libslic3r/SliceCompare/{Snapshot,Diff}.hpp`, GUI at
      `MainFrame.cpp:73`/`:3063`) — the diff must report no layer, feature or extrusion
      differences. This is the end-to-end proof that the up-axis and unit rules produce the same
      solid as a known-good STL.
* [ ] Plate thumbnail of a GLB-imported object is non-blank.
* [ ] `.stl`, `.obj`, `.3mf`, `.step` import unchanged (open one of each).

### Risks

| Risk | Severity | Mitigation |
|---|---|---|
| cgltf will not compile clean as C++ under MSVC at this repo's warning level | Medium | Experiment 5.1 first; fallback is a `cgltf_impl.c` TU |
| Non-welded glTF meshes look non-manifold and light up the repair warning on every import | Medium | `its_merge_vertices` in the hygiene chain (3.7 step 3); assert `open_edges == 0` for `box_10_20_30.glb` in the test suite so a regression is caught |
| Node-instanced meshes explode memory because we bake instead of sharing | Medium | `MAX_GLTF_VOLUMES` / `MAX_GLTF_TRIANGLES` caps with a named message; `GltfInfo::skipped_nodes` reported |
| A binary parser behind the LAN upload endpoint (`RemoteHub.cpp:1050`) | Medium | Decision 7; the fuzz target below is a Stage 1 deliverable, not a follow-up |
| The up-axis sign is wrong (model imports upside down / lying on its side) | Medium | Test 1 is asymmetric on purpose (10/20/30, not a cube) so a wrong sign fails it |
| Huge photogrammetry GLBs freeze the UI | Low–Medium | `ImportstlProgressFn` wired from the first commit (3.9), so the existing `ProgressDialog` can cancel |
| glTF 1.0 files (`cgltf_result_legacy_gltf`) confuse users | Low | Named message |
| Upstream OrcaSlicer later lands `AssimpImport` and conflicts | Low | `Format/GLTF.hpp` is shaped like `Format/OBJ.hpp`; a future Assimp backend can sit behind the same `load_gltf` |

**Fuzz target** (Stage 1 deliverable, ~0.5 d): a `tests/sandboxes/fuzz_gltf/` main that reads a
file from `argv[1]` and calls `load_gltf` into a throwaway `Model`, so it can be driven by
libFuzzer/AFL or just by a corpus of mutated fixtures in CI. Seed corpus = the fixtures above.
Success criterion for the gate: 10 minutes of mutation over the 13 fixtures with no crash and no
allocation over the caps.

### Effort

**6–8 engineer-days.**

| Piece | Days |
|---|---|
| `deps_src/cgltf` wiring + experiment 5.1 + 5.2 | 0.5 |
| `Format/GLTF.{hpp,cpp}` — scene walk, transform bake, strip/fan, accessors, hygiene, errors | 3.0–4.0 |
| Registration checklist (1.6-1.14, 9 files) | 0.5 |
| Fixtures (author 8, vendor 4, `make_fixtures.py`, `SOURCES.md`) | 1.0 |
| `test_gltf.cpp` + CMake + fuzz target | 1.0 |
| Manual checklist incl. the Slice Compare control, and a review round | 0.5–1.0 |

---

## Stage 2 — Colours → filaments

### Findings

* `ObjImportColorFn` (`OBJ.hpp:10`) is
  `void(std::vector<RGBA>& input_colors, bool is_single_color, std::vector<unsigned char>& filament_ids, unsigned char& first_extruder_id)`.
  **No new callback type is needed.**
* The GUI side is a lambda built twice: `Plater.cpp:12223-12231` (import) and `:14678-14683`
  (reload from disk). Both start with
  `if (!boost::iends_with(path…, ".obj")) { return; }` at `:12225` and `:14679` — the two guards
  to widen.
* `ObjColorPanel::update_filament_ids` (`ObjColorDialog.cpp:426-439`) writes **one filament id
  per input colour**, in input order, and `m_first_extruder_id` from cluster 0. So whatever
  granularity we hand in, we get back at the same granularity.
* Filament ids are 1-based extruder numbers; `get_real_filament_id` (`Model.cpp:3140-3146`) maps
  them into `CONST_FILAMENTS` (`Model.cpp:48-50`, 17 entries: `""` plus 16 usable slots), and
  both appliers skip ids `<= 1` (`Model.cpp:3286`) because 1 is the default.
* `Model::obj_import_face_color_deal` (`Model.cpp:3270-3295`) hard-requires
  `objects.size() == 1` (`:3274`), `volumes.size() == 1` (`:3277`) and
  `its.indices.size() == face_filament_ids.size()` (`:3282`).
  `Model::obj_import_vertex_color_deal` (`:3148-3268`) has the same two guards at `:3154`/`:3157`
  and a vertex-count check at `:3189`; its interesting part is the split-triangle encoding for a
  triangle whose three vertices land on three filaments (`:3213-3245`).
* Both write into `ModelVolume::mmu_segmentation_facets`, i.e. imported colour becomes ordinary
  MMU painting and needs no downstream change (slicing, 3MF save, gizmo editing all just work).
* glTF `baseColorFactor` is **linear**; the dialog's swatches and
  `ObjColorPanel::find_filament_selection_by_color` (`ObjColorDialog.hpp:57`) compare against
  filament hex colours, which are sRGB. `src/libslic3r/Color.hpp` has **no** sRGB helper (checked)
  — add a local one in `GLTF.cpp`.

### Changes

#### 2.1 `Format/GLTF.cpp` — fill the colour fields

* `GltfInfo::material_colors`: for each distinct `cgltf_material*` actually used by a kept
  primitive, `linear_to_srgb` of `pbr_metallic_roughness.base_color_factor[0..3]`
  (default `{1,1,1,1}` when `has_pbr_metallic_roughness` is false). Deduplicate by pointer, then
  by value within 1/255.
* `GltfPart::material_index` per volume; `is_single_material = material_colors.size() <= 1`.
* `had_textures = true` when any kept primitive's material has a non-null
  `pbr_metallic_roughness.base_color_texture.texture`.
* `vertex_colors`: only fill when **every** kept primitive had `COLOR_0`, concatenated in volume
  order, each multiplied component-wise by its primitive's `baseColorFactor` before the sRGB
  encode (the spec calls `COLOR_0` a linear multiplier).

#### 2.2 `src/libslic3r/Model.hpp:1608` — two new static appliers

Next to the existing two, **leaving those untouched**:

```cpp
    static bool    obj_import_vertex_color_deal(const std::vector<unsigned char> &vertex_filament_ids, const unsigned char &first_extruder_id, Model *model);
    static bool    obj_import_face_color_deal(const std::vector<unsigned char> &face_filament_ids, const unsigned char &first_extruder_id, Model *model);
+   // Ultra (glTF): one filament id per ModelVolume of the single imported object. No painting —
+   // each part simply gets its own "extruder" config, which is what a per-primitive material means.
+   static bool    import_volume_color_deal(const std::vector<unsigned char> &volume_filament_ids, const unsigned char &first_extruder_id, Model *model);
+   // Ultra (glTF): per-vertex colours across ALL volumes of the single imported object, with a
+   // running vertex offset. Same encoding as obj_import_vertex_color_deal.
+   static bool    import_multi_volume_vertex_color_deal(const std::vector<unsigned char> &vertex_filament_ids, const unsigned char &first_extruder_id, Model *model);
```

`import_volume_color_deal` (~25 lines): require `objects.size() == 1` and
`volume_filament_ids.size() == obj->volumes.size()`; `obj->config.set("extruder",
first_extruder_id)`; for each volume with id `> 1`, `volume->config.set("extruder", id)`.
Nothing touches `mmu_segmentation_facets`.

`import_multi_volume_vertex_color_deal` (~30 lines + the reused body): refactor the per-triangle
body of `obj_import_vertex_color_deal` (`Model.cpp:3158-3263`) into a file-static
`paint_volume_from_vertex_colors(ModelVolume*, const unsigned char* ids, size_t n)`, then have
**both** the old function and the new one call it. That way the OBJ path is provably unchanged
(the old function keeps its two guards and its size check verbatim) and the glTF path walks the
volumes with a running offset.

#### 2.3 `src/libslic3r/Model.cpp:322-325` — hook the callback in

Extend the glTF branch from 1.6, mirroring the `.obj` block's shape at `:269-298`:

```cpp
        result = load_gltf(input_file.c_str(), &model, gltf_info, message, nullptr, stlFn);
        if (result && objFn) {
            unsigned char first_extruder_id = 1;
            if (!gltf_info.vertex_colors.empty()) {
                std::vector<unsigned char> vertex_filament_ids;
                objFn(gltf_info.vertex_colors, false, vertex_filament_ids, first_extruder_id);
                if (!vertex_filament_ids.empty())
                    result = Model::import_multi_volume_vertex_color_deal(vertex_filament_ids, first_extruder_id, &model);
            } else if (gltf_info.material_colors.size() > 1 && !gltf_info.had_textures) {
                std::vector<unsigned char> material_filament_ids;
                objFn(gltf_info.material_colors, gltf_info.is_single_material, material_filament_ids, first_extruder_id);
                if (material_filament_ids.size() == gltf_info.material_colors.size()) {
                    std::vector<unsigned char> volume_ids;
                    volume_ids.reserve(gltf_info.parts.size());
                    for (const GltfPart &p : gltf_info.parts)
                        volume_ids.push_back(p.material_index < 0 ? 1 : material_filament_ids[p.material_index]);
                    result = Model::import_volume_color_deal(volume_ids, first_extruder_id, &model);
                }
            }
        }
```

Note the `> 1` guard: a single-material glTF should not pop a dialog at all (research §5.4.3 —
this is the answer to that open question, and it is stricter than OBJ, which always shows the
dialog). If a user wants to recolour a one-material import they use the normal filament picker.

#### 2.4 `src/slic3r/GUI/Plater.cpp:12225` and `:14679` — widen the guards

```cpp
-                    if (!boost::iends_with(path.string(), ".obj")) { return; }
+                    if (!boost::iends_with(path.string(), ".obj") &&
+                        !boost::iends_with(path.string(), ".glb") &&
+                        !boost::iends_with(path.string(), ".gltf")) { return; }
```

(and the `path` — not `path.string()` — variant at `:14679`). Keep it a positive list.

#### 2.5 `src/slic3r/GUI/Plater.cpp` — the dropped-texture notification

After the import loop, when `GltfInfo::had_textures` was set — the reader has no GUI access, so
carry it through the `message` warning from 3.8 and push it via
`q->get_notification_manager()->push_plater_warning_notification(...)`
(`NotificationManager.hpp:230`; `push_plater_error_notification` at `:228` is the pattern already
used at `Plater.cpp:12208`). Text: *"This model's colours come from a texture, which was not
imported."*

### Gate

**Automated** (`[gltf]` tag, appended to `test_gltf.cpp`):

1. `two_parts_two_materials.glb` + a stub `ObjImportColorFn` that returns `{2, 3}` and
   `first_extruder_id = 2` → volume 0 has `config.extruder() == 2`, volume 1 `== 3`, and
   **neither volume has any `mmu_segmentation_facets`** (`is_mm_painted()` false).
2. `BoxVertexColors.glb` + a stub returning per-vertex ids → `volumes.front()->is_mm_painted()`
   true and the facet count matches.
3. A two-volume model + a vertex-colour array of the wrong total length →
   `import_multi_volume_vertex_color_deal` returns false and paints nothing.
4. **Regression**: an existing OBJ with `mtl` face colours through
   `Model::obj_import_face_color_deal` produces exactly the same
   `mmu_segmentation_facets` serialisation as before the refactor (capture a golden string, in
   the style of `tests/libslic3r/test_mixed_filament_color_golden.cpp`).

**Manual:**

* [ ] Import a 3-material GLB: the dialog appears once, shows 3 swatches whose colours match the
      file (i.e. the sRGB conversion is right — a linear-vs-sRGB mistake makes them look washed
      out), and after OK the three parts sit on the three chosen filaments in the object list.
* [ ] Import a 1-material GLB: **no** dialog.
* [ ] Import a textured GLB: no dialog, a warning notification about the dropped texture.
* [ ] Import a `COLOR_0` GLB: painting appears in the MMU gizmo and survives a save/reload of the
      project.
* [ ] Re-import the same OBJ that was used before Stage 2 and confirm identical behaviour.
* [ ] From the phone, import a 3-material GLB into a hidden instance: the request completes,
      the modal hook cancels the dialog, the model imports without colour, nothing hangs.

### Risks

| Risk | Severity | Mitigation |
|---|---|---|
| The refactor of `obj_import_vertex_color_deal` changes OBJ behaviour | Medium | Golden-string regression test (gate 4); the old function keeps its guards verbatim and only its loop body moves |
| `QuantKMeans` misbehaves with 1–16 input colours | Medium | Experiment 5.3 before writing 2.3 |
| Linear→sRGB forgotten | Low | The manual swatch check, plus an assertion that a `baseColorFactor` of `[0.216,0.216,0.216,1]` comes out near `0.5` in `material_colors` |
| More than 16 materials | Low | `ObjColorPanel::deal_algo` already clusters down and warns; assert `material_colors.size() <= 16` is *not* required — pass them all and let the dialog cluster |
| The dialog on a hidden instance | Low | Documented outcome (cancel ⇒ no colour), see "must not change" |

### Effort

**3.5–5 engineer-days.** Reader colour extraction 1.0; the two appliers + the
`paint_volume_from_vertex_colors` refactor 1.0–1.5; the `Model.cpp` hook and the two Plater
guards 0.5; notification 0.25; tests incl. the golden regression 1.0; manual + review 0.5–0.75.

---

## Stage 3 — Compressed and textured

### Scope decision

Three separate, independently shippable pieces. **Do them in this order, and stop whenever the
value runs out** — none of them is a prerequisite for the others.

**3a. `KHR_mesh_quantization` (0.5 d).** Already works via `cgltf_accessor_unpack_floats`. All
that is needed is to move it from "refuse" to "allow" in 3.8 (it is already listed as allowed
there) and add a fixture: `gltf-transform quantize box_10_20_30.glb box_quantized.glb`, then
assert the same `Vec3d(10,30,20)`. Do this one regardless — a silent wrong scale is the worst
failure mode in this whole feature.

**3b. Draco (3.5–5 d) — recommendation: vendor the decoder.** Draco is Apache-2.0 (compatible
with this repo's AGPL-3.0, `LICENSE.txt`) but the source tree is ~226 MB and it is a real CMake
project, so it cannot go in `deps_src/` as a header. It belongs in `deps/` following the
`deps/OCCT/OCCT.cmake` template (fetched by `Snapmaker_Orca_add_cmake_project`,
`deps/CMakeLists.txt:113-211`, `include()`d at `:366`, listed in `_dep_list` at `:384`).
OrcaSlicer's `deps/Draco/Draco.cmake` (tag 1.5.7) is directly liftable.

  *The cost is not runtime* — Draco decoding is tens of milliseconds to a couple of seconds for
  print-scale meshes, negligible next to slicing. The cost is that **every developer and every CI
  runner has to rebuild `deps/`**, which is exactly what decision 4 avoided for Stage 1. So:
  only do 3b if real user files are turning up Draco-compressed. Until then the named refusal
  from 3.8 plus the message's "Re-export it without Draco compression" is an honest answer, and
  `gltf-transform` is a one-line fix on the user's side.
  Build with the decoder only (`DRACO_ENCODING=OFF` equivalent) and wire it into cgltf by
  implementing the `KHR_draco_mesh_compression` path yourself — cgltf *recognises* the extension
  (`cgltf_primitive::has_draco_mesh_compression`) but does not decode it.

**3c. `EXT_meshopt_compression` (1 d).** `meshoptimizer` (MIT, 7 MB, small compiled lib) is much
cheaper than Draco and is growing in the wild. It could go in `deps_src/` as a small static
library (the `deps_src/qoi` shape, `deps_src/qoi/CMakeLists.txt:6-9`) rather than in `deps/`,
which keeps the no-deps-rebuild property. Only the decoder (`meshoptimizer/src/vertexcodec.cpp`,
`indexcodec.cpp`, `vertexfilter.cpp`) is needed.

**3d. Textures → per-face colour (2.5–4 d).** libpng and libjpeg are already linked
(`src/libslic3r/CMakeLists.txt:554` `find_package(JPEG REQUIRED)`, `:602 JPEG::JPEG`,
`:607 PNG::PNG`), so decoding an embedded PNG/JPEG needs no new dependency. Sample
`baseColorTexture` at each triangle's centroid UV, produce one RGBA per surviving face, and feed
`obj_import_face_color_deal`'s generalised sibling — which Stage 2 did **not** write (Stage 2
only needed per-volume and per-vertex). So 3d also owns:

```cpp
// Ultra (glTF textures): per-face colours across ALL volumes of the single imported object.
static bool import_multi_volume_face_color_deal(const std::vector<unsigned char> &face_filament_ids,
                                                const unsigned char &first_extruder_id, Model *model);
```

built the same way as 2.2 — refactor the loop body of `Model::obj_import_face_color_deal`
(`Model.cpp:3283-3290`) into a shared static, leave the OBJ entry point untouched.
`KHR_texture_basisu` (KTX2) is out of scope; refuse it by name.

### Gate

* 3a: `box_quantized.glb` → `Vec3d(10,30,20)`.
* 3b: `box_draco.glb` → geometry identical to `box_10_20_30.glb` (same vertex count after
  welding, same `its_volume` within 1e-4); the Stage 1 test that asserted the "Draco" refusal is
  **inverted**, not deleted, so the change is visible in the diff.
* 3c: a `gltf-transform meshopt` fixture round-trips.
* 3d: a textured GLB with two clearly-separated colour regions produces two clusters in the
  dialog and paints the right faces; the dropped-texture notification from 2.5 stops firing.

### Effort

**1–10 engineer-days depending on how much of 3a-3d is taken.**
3a 0.5 · 3b 3.5–5 (mostly deps/CI plumbing on three platforms) · 3c 1.0 · 3d 2.5–4.

---

## 7. Splitting into agent-sized tasks

Branch everything from `feat/ultra-preferences`. Each task below is one branch, one PR, and is
independently buildable and testable; the ordering constraints are only where stated.

| # | Branch | Scope | Depends on | Days |
|---|---|---|---|---|
| T1 | `feat/gltf-deps-cgltf` | 1.1, 1.2, 1.3 + a trivial `Format/GLTF.{hpp,cpp}` stub whose `load_gltf` returns false with "not implemented yet", plus experiments 5.1 and 5.2. Proves the build on Windows/macOS/Linux before anyone writes a scene walk. | — | 0.5 |
| T2 | `feat/gltf-fixtures` | `tests/data/test_gltf/*`, `make_fixtures.py`, `SOURCES.md`. No C++. Can run fully in parallel with T1 and T3. | — | 1.0 |
| T3 | `feat/gltf-reader` | 1.4, 1.5 — the reader itself. Reviewable in isolation because it touches no GUI file. | T1 | 3.0–4.0 |
| T4 | `feat/gltf-registration` | 1.6 through 1.14 — the nine registration edits. Small, mechanical, high blast-radius: review it against the table in "Findings". | T3 | 0.5 |
| T5 | `feat/gltf-tests` | 1.15, the fuzz target, the Slice Compare control run. | T2, T3 | 1.0 |
| T6 | `feat/gltf-colors-reader` | 2.1 — colour extraction in the reader, plus experiment 5.3. GUI-free. | T3 | 1.0 |
| T7 | `feat/gltf-colors-model` | 2.2, 2.3 — the two appliers and the `paint_volume_from_vertex_colors` refactor, with the golden OBJ regression test. The riskiest diff in Stage 2; keep it separate so the OBJ regression is easy to see. | T6 | 1.5 |
| T8 | `feat/gltf-colors-gui` | 2.4, 2.5 — the two Plater guards and the notification. | T7 | 0.75 |
| T9+ | `feat/gltf-quantization`, `feat/gltf-draco`, `feat/gltf-meshopt`, `feat/gltf-textures` | Stage 3, one branch per piece (3a…3d). | T5 | see above |

T1+T2 can start together on day 0. The Stage 1 gate is run on the merge of T1-T5; the Stage 2
gate on the merge of T6-T8.

**Total: 6–8 days for Stage 1, plus 3.5–5 for Stage 2 — roughly two working weeks for the
version users will actually want. Stage 3 adds 1–10 more depending on how much of it is taken;
the recommended slice is 3a alone (0.5 d), deferring Draco until real files demand it.**

---

## Stage 1 status

Done, on branch `feat/glb-import-stage1` (from `feat/ultra-preferences`), built and verified in the
`C:\Dev\SnapmakerOrcaNext` worktree on 2026-09-03. All of Stage 1 shipped; the sections below list
what changed, where it departs from this plan, the gate output, and what the next stage inherits.

### What shipped

| Plan item | File | Note |
|---|---|---|
| 1.1 | `deps_src/cgltf/{cgltf.h,CMakeLists.txt,README.md}` | cgltf 1.15 verbatim, MIT. `README.md` added for provenance and the "only `GLTF.cpp` defines `CGLTF_IMPLEMENTATION`" rule |
| 1.2 | `deps_src/CMakeLists.txt` | `add_subdirectory(cgltf)` in the header-only group |
| 1.3 | `src/libslic3r/CMakeLists.txt` | `Format/GLTF.{cpp,hpp}` in the source list; `cgltf` linked explicitly in the `PRIVATE` block |
| 1.4, 1.5 | `src/libslic3r/Format/GLTF.{hpp,cpp}` | the header exactly as §3.1; the reader ~640 lines |
| 1.6 | `src/libslic3r/Model.cpp` | one `else if` before the `else`, plus the updated message |
| 1.7, 1.8 | `src/slic3r/GUI/GUI_App.cpp` | both `FT_MODEL` lists and both dialog titles |
| 1.9 | `src/slic3r/GUI/MainFrame.cpp` | both `#ifdef` branches |
| 1.10 | `src/slic3r/GUI/Plater.cpp` | `pattern_drop` |
| 1.11 | `src/slic3r/GUI/RemoteHub.cpp` | `spool_upload` allow-list and the API manifest |
| 1.12 | `resources/web/orca/stream_center.html` | picker label, regex and message |
| 1.13 | `src/dev-utils/platform/osx/Info.plist.in` | a `glb`/`gltf` document type. The `obj` block's `CFBundleTypeName = "STL"` copy-paste bug was **not** replicated |
| 1.14 | — | no CLI change, no Windows association, `Plater.cpp:11633-11637` untouched, all as planned |
| 1.15 | `tests/libslic3r/test_gltf.cpp`, `tests/data/test_gltf/*` | 14 self-authored fixtures + 3 CC0 Khronos assets, `make_fixtures.py`, `SOURCES.md` |
| fuzz | `tests/fuzz_gltf/` | driver, `nanosvg_impl.cpp`, `mutate.py` |

`RemoteAccess.cpp` needed **no** change, exactly as §6 predicted: `api_project_open` only filters by
extension for `mode == "load"` (`.3mf`-only), and a `.glb` arrives as `mode == "import"`.

### Deviations from the plan

1. **`cgltf_validate` refuses on `cgltf_result_data_too_short`** instead of only logging (§5 step 4
   said "log a warning, do not refuse"). Reason, and it is not a style call: `cgltf_validate` is the
   only thing that bounds-checks sparse accessor indices (`cgltf.h:1628`,
   `index_bound >= accessor->count`), and `cgltf_accessor_unpack_floats` does **not** re-check the
   sparse writer index before `out[writer_index * floats_per_element]` (`cgltf.h:2437-2440`). Without
   the refusal, a crafted sparse index is a heap write past the end of the reader's own buffer — in a
   parser reachable from the LAN upload endpoint. `data_too_short` is exactly the
   "accessor points outside its buffer" family; every other `cgltf_validate` result (the cosmetic
   `invalid_gltf` family that real files trip) is still only logged, so the plan's intent is kept.
   The reader also does its own `accessor_data_fits()` check as belt and braces.
2. **A fifth hygiene step**: `its_compactify_vertices` after `its_remove_degenerate_faces`. Vertices a
   primitive declares but never indexes would otherwise stay in the mesh and inflate its bounding
   box — which is what the up-axis/unit assertion measures. It also keeps
   `GltfInfo::vertex_colors.size()` equal to the real vertex count.
3. **`SimpleSparseAccessor` is CC-BY-4.0, not CC0.** §"Test assets" group B lists it as CC0; its
   `README.md` says Creative Commons Attribution 4.0. It was **not** vendored. `sparse_triangle.gltf`
   + `.bin` was authored instead and covers the same behaviour with numbers we chose (a base triangle
   whose third vertex is moved from `(0,1,0)` to `(0,5,0)` by a sparse override, so ignoring sparse
   gives a 1 mm triangle instead of a 5 mm one). The other three group-B assets were confirmed CC0.
4. **`box_draco.glb` is genuinely Draco-compressed**, not the hand-declared stub the plan allowed for:
   `npx --yes @gltf-transform/cli@4 draco box_10_20_30.glb box_draco.glb` (glTF-Transform v4.5.0).
   Stage 3b can invert the assertion against this same file with no new fixture.
5. **Six fixtures beyond the plan's list**, all cheap and all pinning a stated rule:
   `unknown_extension.glb` (an unknown required extension is named), `escaping_buffer.gltf` (the
   buffer-URI containment check), `truncated.glb` (gate item 13, committed rather than generated at
   test time), `box_meters.glb` (proves the metres rescue fires, so §3.5's "zero new code" claim is
   tested rather than asserted), `box_10_20_30.stl` (the Slice Compare control), and
   `Geräte/box-čřšřěá.glb`.
6. **The fuzz target lives in `tests/fuzz_gltf/`, not `tests/sandboxes/fuzz_gltf/`** — `tests/sandboxes`
   does not exist in this fork. It is added `EXCLUDE_FROM_ALL`, following the `cpp17` precedent.
7. **`MAX_GLTF_VOLUMES` is a hard, named error**, per §3.9's "produce a named error"; `skipped_nodes`
   records how many (node, primitive) pairs were beyond the cap so the caller can still report it.
8. **Draco is also refused per-primitive**, not only from `extensionsRequired`: an asset may carry
   `KHR_draco_mesh_compression` on a primitive without requiring it, and cgltf still cannot decode it.
9. **Cancel returns `false` with "Import cancelled."** — which `Model::read_from_file` turns into a
   `RuntimeError` and the Plater shows as an error dialog. Honest and non-hanging, but a user-initiated
   cancel arguably should not raise a dialog. **Reviewer decision**, noted rather than guessed at.
10. **`GltfInfo::had_textures` sets a warning in `message` on success, where nothing reads it yet** —
    `Model::read_from_file` only consumes `message` on failure. The flag and the sentence are in place
    for §2.5's notification; in Stage 1 the warning only reaches the log.

### Gate output

**Automated** — `build/tests/libslic3r/Release/libslic3r_tests.exe "[gltf]"`:

```
All tests passed (109 assertions in 4 test cases)
```

Full suite, guarding the "must not change" list — `libslic3r_tests.exe`:

```
test cases:   588 |   586 passed | 2 failed as expected
assertions: 52721 | 52719 passed | 2 failed as expected
```

Every numbered gate item holds. 1 `box_10_20_30.glb` → 1 object, 1 volume, `Vec3d(10,30,20)` (plus
`open_edges == 0`, 8 welded vertices from 24, `its_volume == 6000`). 2 `.gltf`+`.bin` identical to the
`.glb`. 3 `SimpleMeshes.gltf` → 2 volumes, distinct names, 1 mm apart in X. 4
`two_parts_two_materials.glb` → 2 volumes, 2 material colours. 5 `nested_trs.glb` → centre `(5,1,0)`,
size `(8,2,4)`, and identity rotation/scale on the volume. 6 `strip_and_fan.glb` → 2 and 3 triangles,
with **both** strip triangles' face normals asserted (see finding 5 below). 7
`TriangleWithoutIndices.gltf` → 1 triangle. 8 sparse accessor honoured. 9 `points_only.glb` → false,
"points or lines". 10 `box_draco.glb` → false, "Draco". 11 `BoxVertexColors.glb` → one colour per
surviving vertex, and `is_mm_painted()` still false. 12 non-ASCII path loads. 13 truncated file →
"damaged or incomplete", no crash.

**Fuzz** — `python tests/fuzz_gltf/mutate.py --minutes 10 --seed 20260903`:

```
seeds  : 21
rounds=2573 cases=102920 findings=0
```

102,920 mutated files, no crash, no uncaught exception, no `false` with an empty message, and no
`true` whose `info.parts` disagreed with the model. (The driver checks the return contract, not just
survival.)

**Manual**, against an instance on the isolated `dd_next` data dir (hub on port 13641), driven
through its loopback API rather than the GUI:

* `--datadir … box_10_20_30.glb` on the command line → `/api/plates` shows one object named
  `box scene` (the glTF scene name) with plate footprint `[130.5, 121.0, 140.5, 151.0]`, i.e. 10 × 30
  mm; `/api/plates/0/layout` reports `size [10.0, 30.0, 20.0]`, `offset [135.5, 136.0, 10.0]`,
  `rz 0.0`, `scale 1.0` — the up-axis rule, the unit rule and the transform bake, in the real app.
* `/api/plates/0/thumbnail.png` → a 512×512 render of a clean solid box, no manifold artefacts.
* Phone upload: `POST /r/<token>/i/<pid>/open?mode=import` with `two_parts_two_materials.glb` →
  `{"objects":1}` and a second plate object sized `[22.0, 10.0, 10.0]`, exactly the two-box extent
  after the axis swap. A `.gif` is still refused: `only .3mf, .stl, .obj, .step and .glb files can be
  opened`. `POST /r/<token>/api/instances/open` with `nested_trs.glb` spools the file and spawns an
  instance (`ok:true`).
* **Slice Compare control** — the plan asks for a GUI SliceCompare diff; `SliceCompare` is only
  reachable from `MainFrame`, so it was run as the strictly stronger CLI equivalent via
  `scripts/orca_cli.py`: slice `box_10_20_30.glb` and `box_10_20_30.stl` with the same printer,
  process and filament and compare the G-code. Result: **9198 identical lines**, the only differences
  being the `; printing object <name>` comment and the file-name header (the GLB's object is named
  from the glTF scene, the STL's from its file). Identical G-code means no layer, feature or extrusion
  differences by construction.

### Findings

1. **Experiment 5.1 answered: cgltf compiles clean as C++ under MSVC at this repo's warning level.**
   Zero errors and zero warnings from `GLTF.cpp`. The `cgltf_impl.c` fallback is not needed. The
   defensive `#pragma warning(push/disable/pop)` around the include stays as insurance for other
   toolchains.
2. **Experiment 5.4 answered:** `cgltf_node_transform_world` does compose the whole chain —
   `SimpleMeshes` lands its two instances exactly 1 mm apart and `nested_trs` matches the
   hand-computed centre and extents.
3. **Experiment 5.5 answered differently than proposed.** Rather than author a closed solid out of a
   triangle strip (fiddly, and a wrong winding could still cancel out), the test asserts the **face
   normal of each strip triangle** directly. Triangle 1 only agrees with triangle 0 if the
   odd-triangle vertex swap was applied, so the fixture discriminates exactly the bug the rule exists
   to prevent.
4. **`src/libslic3r/Color.hpp` calls `assert()` without including `<cassert>`.** It only compiles
   today because every existing consumer pulls it in first. A new translation unit that includes
   `Format/GLTF.hpp` early hits it. Worked around locally in `tests/fuzz_gltf/fuzz_gltf.cpp`; the
   shared header was deliberately not touched. Worth a one-line fix in a separate change.
5. **nanosvg's implementation section includes `<windows.h>`.** Its `min`/`max`/`GetObject` macros
   break every libslic3r header that follows, so `tests/libslic3r/libslic3r_tests.cpp` only survives
   `#define NANOSVG_IMPLEMENTATION` because no libslic3r header comes after it. The fuzz target puts
   it in its own TU (`nanosvg_impl.cpp`), which is what any future test binary should copy.
6. **`EXCLUDE_FROM_ALL` keeps a target out of the Visual Studio solution**, so
   `cmake --build --target fuzz_gltf` fails with `MSB1009` there. The per-generator command is in the
   header of `fuzz_gltf.cpp`.
7. **Flat primitives log `its_convex_hull: Unable to create convex hull`** (twice for
   `strip_and_fan.glb`'s two zero-volume sheets). Pre-existing qhull behaviour for planar meshes, not
   caused by this change, harmless — but a glTF full of decorative flat geometry will produce log
   noise.
8. **Hidden instances spawned through `POST /api/instances/open` never register with the hub on this
   branch.** The process starts, stays alive and responsive, but writes no `<datadir>/hub/instances/
   <pid>.json`. Reproduced identically with a `.stl`, so it is pre-existing and unrelated to glTF —
   flagged here because it makes that one phone route unverifiable end to end.
9. **`C:\Dev\SnapmakerOrcaNext\build`'s `CMAKE_INSTALL_PREFIX` is `C:/Program Files/Snapmaker_Orca`**,
   unlike the main checkout's `<build>/Snapmaker_Orca`, so `cmake --install` there needs an explicit
   `--prefix` or it fails on permissions.
10. **`build_next.bat`'s `bambu_networking` target does not exist in that build tree**, and the app
    executable is the `Snapmaker_Orca_app_gui` target, not `Snapmaker_Orca` (which is the DLL).

### What Stage 2 inherits

`GltfInfo` is filled in completely already — `material_colors` (deduplicated, sRGB-encoded),
`parts[i].material_index`, `vertex_colors` (only when *every* kept primitive had `COLOR_0`),
`is_single_material`, `had_textures`, `dropped_primitives`, `skipped_nodes`,
`unsupported_extensions`. The sRGB conversion is pinned by a test (`nested_trs.glb`'s linear
0.2158605 → 0.5). The `Model.cpp` glTF branch has a comment marking where the `objFn` colour path
hooks in. Nothing in Stage 1 writes `mmu_segmentation_facets`, and a test asserts
`is_mm_painted() == false` after a `COLOR_0` import, so Stage 2's first change is visible.

---

## Stage 2 status

Done, on branch `feat/glb-import-stage2` (from `feat/glb-import-stage1`), built and verified in the
`C:\Dev\SnapmakerOrcaNext` worktree on 2026-09-03. All of Stage 2 shipped.

### What shipped

| Plan item | File | Note |
|---|---|---|
| 2.1 | `src/libslic3r/Format/GLTF.cpp` | **already done in Stage 1** — `material_colors` (deduplicated, sRGB), `parts[i].material_index`, `vertex_colors`, `is_single_material`, `had_textures`. Re-verified, no change needed |
| 2.2 | `src/libslic3r/Model.{hpp,cpp}` | `paint_volume_from_vertex_colors()` extracted; `import_volume_color_deal` and `import_multi_volume_vertex_color_deal` added |
| 2.3 | `src/libslic3r/Model.cpp` | the `objFn` hook in the glTF branch, with the `> 1` material guard and the `had_textures` skip |
| 2.4 | `src/slic3r/GUI/Plater.cpp` | both `.obj` guards widened to `.obj\|.glb\|.gltf`, kept as a positive extension list |
| 2.5 | `src/libslic3r/Model.{hpp,cpp}`, `Plater.cpp` | the dropped-texture warning, via a new optional out-parameter (see deviation 1) |
| tests | `tests/libslic3r/test_gltf.cpp` | 5 new scenarios, 415 `[gltf]` assertions; 2 tagged `[golden]` |
| fixtures | `tests/data/test_gltf/` | `three_materials.glb`, `textured_two_materials.glb` |

`ObjColorDialog` and `ObjImportColorFn` were reused verbatim — no new callback type, no change to the
dialog. `obj_import_face_color_deal` was not touched at all.

### Deviations from the plan

1. **`Model::read_from_file` gained an optional trailing `std::string *import_warning = nullptr`.**
   Change 2.5 says to carry the dropped-texture sentence "through the `message` warning from 3.8"
   and push it from the Plater — but `read_from_file` reads `message` **only on failure**
   (`Model.cpp`, the `if (!result)` tail), so on a successful load the sentence had nowhere to go.
   A defaulted trailing parameter leaves every existing call site untouched and gives the Plater a
   real channel. The glTF branch moves the sentence out of `message` and clears it, so `message`
   only ever holds an actual error from that point on.
2. **A failed colour step now gets its own message.** The plan's snippet assigns
   `result = Model::import_volume_color_deal(...)`, mirroring OBJ — but a `false` there with an
   empty `message` would surface as the generic "Loading of a model file failed.", which Stage 1
   went to some trouble to avoid. The branch adds *"The colours in this glTF file could not be
   applied to the model."* for that case.
3. **Experiment 5.3 became a permanent test, not a scratch build.** `QuantKMeans` lives in
   `src/libslic3r/ObjColorUtils.hpp` and `libslic3r_tests` already links OpenCV, so the experiment
   is now two scenarios in the suite. Findings below.
4. **The dead `calc_tri_area` lambda moved across with the rest of the body.** It is provably
   unreferenced (one declaration, no uses), but leaving it in keeps the transplant a pure move,
   which is the whole point of doing 2.2 this way. Worth deleting in a separate tidy-up.
5. **Two extra scenarios beyond the plan's four**: a three-material GLB through the full dialog
   path, and the same file with a *cancelled* dialog — which is what a hidden instance actually
   sees (finding 3).

### Gate output

**Automated** — `build/tests/libslic3r/Release/libslic3r_tests.exe "[gltf]"`:

```
All tests passed (415 assertions in 9 test cases)
```

Full suite, guarding the "must not change" list:

```
test cases:   593 |   591 passed | 2 failed as expected
assertions: 53027 | 53025 passed | 2 failed as expected
```

The plan's four gate items, plus what was added:

1. `two_parts_two_materials.glb` + a stub returning `{2, 3}` with `first_extruder_id = 2` → volume 0
   `config.extruder() == 2`, volume 1 `== 3`, and **neither volume `is_mm_painted()`**. The stub also
   asserts the dialog saw exactly **2** colours — one per material, not one per triangle.
2. `BoxVertexColors.glb` + a per-vertex stub → `is_mm_painted()` true, the dialog saw exactly one
   colour per surviving vertex, and the volume's extruder is the first id.
3. A vertex-colour array one entry short → `import_multi_volume_vertex_color_deal` returns false and
   **neither** volume is painted; the correct length paints both.
4. **Golden regression.** Two `[golden]` scenarios pin the exact 3MF/AMF serialisation
   (`FacetsAnnotation::get_triangle_as_string`) that `obj_import_vertex_color_deal` and
   `obj_import_face_color_deal` produce for a cube. The vertex fixture's ids
   (`{2,2,2,3,3,4,2,3}`) make the twelve faces cover all three branches of the moved code —
   `_3_SAME_COLOR`, `_3_DIFF_COLOR` and `_2_SAME_1_DIFF_COLOR`, the last two including the
   split-triangle encodings. Note what backs the *"unchanged"* claim: a scripted comparison against
   `git show HEAD:src/libslic3r/Model.cpp` showed **105 of 105 body lines identical** apart from
   three intended rebinds, so the move is provably pure; the golden strings guard from here on.
5. Single-material GLB → the dialog is never opened.
6. Textured two-material GLB → the dialog is never opened *and* a warning is returned. This file has
   two materials on purpose, so without the `had_textures` guard it would open the dialog — the test
   really tests the guard.
7. A part with no material at all (`strip_and_fan.glb`) → no dialog.
8. Three-material GLB → three parts on filaments 2, 3 and 4, none painted.
9. Three-material GLB with a cancelled dialog → geometry still imports, no colour, no failure.

**Manual, on the isolated `dd_next` data dir** (never the user's real one), through the hub's phone
route:

* A **hidden** instance, 3-material GLB via `POST /r/<token>/i/<pid>/open?mode=import`:
  **`{"objects":1}`, HTTP 200, in 1 second** — no hang. The plate then holds one object named
  `traffic light` (the glTF scene name) of size `[34, 10, 10]`, which is the three 10 mm boxes at
  x 0/12/24 after the axis swap. `needs_attention` stayed false, and the log shows
  `hidden-mode dialog answered ok: wxDialog "Obj file Import color"`.
* The textured GLB into the same hidden instance: imported, and **no second dialog** was answered —
  the `had_textures` guard held in the real app, not just in the test.

### Findings

1. **`read_from_file` cannot report a warning on success.** See deviation 1. The new
   `import_warning` out-parameter is the general fix; STL and OBJ could use it too.
2. **`QuantKMeans` is safe for 1–30 colours** (experiment 5.3, now a test). With automatic cluster
   selection it returns one label per colour and between 1 and n clusters; with an explicit count it
   never asks OpenCV for more clusters than it has samples, because
   `more_than_request()` fails and `compute_num_colors()` clamps to the distinct-colour count. And
   `deal_default_strategy()` **cannot** divide by zero: it early-returns on an empty filament list,
   and `deal_approximate_match_btn()` guards both `m_result_icon_list.size() == 0` and
   `map_count < 1` before indexing. There is no division in that path at all.
3. **On a hidden instance the modal hook answers `ObjColorDialog` with OK, not cancel — and OK
   silently means cancel.** The plan predicted cancel. What actually happens is more interesting:
   `default_answer()` finds a real `wxID_OK` child button and the phone path runs in `Request` mode,
   so the hook returns `wxID_OK`. But the hook returns it *from `ShowModal()` without dispatching any
   button event*, and `ObjColorDialog` only calls `update_filament_ids()` inside its OK **click
   handler** — so the caller's `filament_ids` is never written and stays empty. The Plater's
   `if (ShowModal() != wxID_OK) filament_ids.clear();` then does nothing, because it *was* OK.
   The outcome is exactly what the plan wanted (imports without colour, never hangs), but by a
   different route, and the guard that makes it safe is the
   `material_filament_ids.size() == material_colors.size()` check in the dispatch — that check is
   load-bearing, not decorative. **Any future dialog whose OK handler does the real work has the
   same trap**, and a `ClassRule` entry for `ObjColorDialog` answering `wxID_CANCEL` would make the
   intent explicit rather than accidental. Left alone here because the behaviour is correct and the
   hidden-mode rules are another plan's territory.
4. **A single-material glTF opens no dialog**, which is stricter than OBJ (which always asks). This
   is the answer to research §5.4.3 and it is deliberate: with one material there is nothing to
   choose, and the normal filament picker still works.

### What Stage 3 inherits

* `paint_volume_from_vertex_colors()` is the shared per-volume painter; Stage 3d's
  `import_multi_volume_face_color_deal` should be built the same way — refactor the loop body of
  `Model::obj_import_face_color_deal` into a file-static and leave the OBJ entry point untouched.
  That function is still **completely unmodified** by Stage 2, and its golden test is already in
  place to prove the next refactor is also pure.
* `GltfInfo::had_textures` is set and now reaches the user; when Stage 3d starts sampling textures it
  should stop setting the flag (or the notification will contradict the result).
* The `import_warning` channel exists for any future "loaded, but you should know" message.
* `box_draco.glb` is genuinely Draco-compressed, so Stage 3b inverts an assertion rather than needing
  a new fixture.

---

## Stage 3 status

Done, on branch `feat/glb-import-stage3` (from `feat/glb-import-stage2`), built and verified in the
`C:\Dev\SnapmakerOrcaNext` worktree on 2026-09-03.

**Shipped: 3a (quantization), 3c (meshopt), 3d (textures). 3b (Draco) deliberately not taken** —
this section's own recommendation is to vendor Draco only once real user files turn up compressed,
because it is the one piece that would force every developer and CI runner to rebuild `deps/`. The
named refusal from 3.8 stands, and `box_draco.glb` is a genuinely compressed asset waiting for it.

### What shipped

| Plan item | File | Note |
|---|---|---|
| tidy | `src/libslic3r/Model.cpp` | the dead `calc_tri_area` lambda that rode along with the Stage 2 transplant |
| 3a | `tests/data/test_gltf/box_quantized.glb`, `test_gltf.cpp` | **no reader change needed** — see deviation 1 |
| 3c | `deps_src/meshoptimizer/` (new), `deps_src/CMakeLists.txt`, `src/libslic3r/CMakeLists.txt`, `Format/GLTF.cpp` | decoder-only meshoptimizer v1.2 as a static library in `deps_src/`, the qoi shape |
| 3c | `tests/data/test_gltf/box_meshopt.glb` | all three meshopt modes plus the octahedral filter, quantized as well |
| 3d | `Format/GLTF.{hpp,cpp}` | `GltfInfo::face_colors`, centroid-UV sampling, PNG/JPEG decode, image sourcing |
| 3d | `src/libslic3r/Model.{hpp,cpp}` | `paint_volume_from_face_colors()` extracted, `import_multi_volume_face_color_deal` added, the dispatch branch |
| 3d | `tests/data/test_gltf/textured_two_regions.glb`, `textured_undecodable.glb` | the two-cluster fixture and the still-dropped case |

`KHR_texture_basisu` is refused by name when **required** (covered by `unknown_extension.glb`) and
falls back to the dropped-texture warning when merely **used** (covered by
`textured_undecodable.glb`).

### Deviations from the plan

1. **3a needed no code at all.** The reader has allowed `KHR_mesh_quantization` since Stage 1, and
   `cgltf_accessor_unpack_floats` plus `cgltf_node_transform_world` already did the right thing.
   The work was the fixture and the assertion, which is what the plan wanted from it anyway
   ("silently importing at the wrong scale is the one outcome worse than an error"). The tolerance
   is 0.02 mm, which is what 16-bit positions over a 30 mm span allow.
2. **3d samples *before* welding, not after.** The plan says to build face colours after step 4 of
   the hygiene chain "by re-walking the surviving faces". That cannot work: `its_merge_vertices`
   merges vertices that differ only in UV — which is every seam an exporter makes — so after it the
   UVs no longer line up with the vertices. The array is therefore built at triangle-emission time
   and carried through the chain, with a paired degenerate-removal that drops the same entries from
   both. The plan's underlying concern (that the array must not desync from the faces) is met; only
   the order is inverted.
3. **Image decoding uses the in-tree `png::decode_colored_png` and libjpeg, not OpenCV.** OpenCV is
   linked to libslic3r and `cv::imdecode` would have been three lines, but this path is reachable
   from the phone upload endpoint and OpenCV's imgcodecs is a much larger decode surface than the
   two libraries the plan actually named. Both are capped on encoded bytes (64 MB) and decoded
   pixels (64 Mpx).
4. **`had_textures` no longer drives the warning on its own.** It still means "this model had a base
   colour texture"; the warning now fires only when `face_colors` came back empty, i.e. the texture
   was really lost. That is 3d's "the dropped-texture notification stops firing", made precise.
5. **A Stage 2 test changed meaning and was updated.** `textured_two_materials.glb` used to assert
   "no dialog, texture reported dropped"; with 3d its texture is sampled, so it now asserts "the
   dialog sees one colour per triangle and nothing is dropped". `textured_undecodable.glb` was added
   so the dropped-texture path keeps its coverage rather than losing it.
6. **A primitive that could not be sampled contributes its flat material colour** rather than
   disqualifying the whole file. `face_colors` must cover every triangle of the object or
   `import_multi_volume_face_color_deal` rejects it, so a mixed file (one textured primitive, one
   plain) still works — `textured_two_materials.glb` is exactly that case.

### Gate output

**Automated** — `libslic3r_tests.exe "[gltf]"`:

```
All tests passed (451 assertions in 10 test cases)
```

Full suite:

```
test cases:   594 |   592 passed | 2 failed as expected
assertions: 53063 | 53061 passed | 2 failed as expected
```

Both OBJ `[golden]` scenarios still pass, which is what makes "the OBJ path is unchanged" a claim
rather than a hope — and the face-colour refactor was independently shown to be a **pure move**
(11 of 11 body lines identical against `git show HEAD`), the same check Stage 2 used for the vertex
path.

Per plan item:

* **3a** `box_quantized.glb` → `Vec3d(10, 30, 20)`, one object, one volume.
* **3c** `box_meshopt.glb` → same box, 12 triangles, `open_edges == 0`. The fixture uses TRIANGLES
  for indices, ATTRIBUTES for positions and octahedral-filtered normals, and is quantized too, so
  one file covers the whole decoder plus both extensions interacting.
* **3d** `textured_two_regions.glb` → 12 face colours, exactly 6 red and 6 blue (two clusters, as
  the plan asks); with a stub dialog mapping red→2 and blue→3 the volume ends up painted with
  exactly 6 triangles carrying `"8"` and 6 carrying `"0C"` — the right faces, checked through
  `FacetsAnnotation::get_triangle_as_string`. A per-face array of the wrong length is refused and
  paints nothing. The dropped-texture warning is empty for this file and non-empty for
  `textured_undecodable.glb`.

**Fuzz** — the corpus is now 28 files including three textured ones, so the mutation run also
exercises the PNG and JPEG decoders:

```
seeds  : 28
rounds=2273 cases=90920 findings=0
```

Every seed file also behaves correctly under the driver's contract check (`ok` with matching
`info.parts`, or `no` with a specific message) — including the new
`box_meshopt.glb`, `box_quantized.glb`, `textured_two_regions.glb` and `textured_undecodable.glb`.

**Manual, on the isolated `dd_next` data dir**, hidden instance via the phone route:
`POST /r/<token>/i/<pid>/open?mode=import` with `textured_two_regions.glb` → **HTTP 200 in
1 second, no hang**, one object of the right size on the plate. See finding 3 for what it does
*not* do.

### Findings

1. **`cgltf_validate` dereferences `sparse->indices_buffer_view->buffer` without a null check**, and
   a meshopt-compressed buffer view has no outer buffer at all — the buffer lives in the extension.
   Enabling meshopt made that combination reachable, so the reader now refuses a sparse accessor
   whose index or value view has no buffer, before validate runs. Exotic, but it was a crash.
2. **The in-tree `png::decode_colored_png` only handles 8-bit RGB and RGBA.** A paletted or
   greyscale PNG — common for small, flat textures, which is exactly the kind this feature is most
   useful for — is refused and falls back to the dropped-texture warning. Teaching it
   `png_set_palette_to_rgb` / `png_set_expand_gray_1_2_4_to_8` / `png_set_strip_16` would be a
   handful of lines in `PNGReadWrite.cpp`, but that file is shared with the SLA and thumbnail paths,
   so it is left for a separate change rather than widened here.
3. **A hidden instance still imports a textured GLB *unpainted*.** This is Stage 2 finding 3 again:
   the modal hook answers `ObjColorDialog` with `wxID_OK` straight out of `ShowModal()`, the OK
   *click handler* never runs, `update_filament_ids()` never fills `filament_ids`, and the dispatch's
   length check correctly declines to paint from an empty answer. The import completes in a second
   and the geometry is right — but the faces are not painted, and the `ObjColorDialog → wxID_CANCEL`
   rule being added on `fix/side-fixes` will make that outcome *deliberate* rather than accidental,
   not different. Painting on a headless import would need the hook to run the dialog's accept path
   (or the Plater to auto-match when there is nobody to ask), which is a separate decision.
   Painted faces **are** proven end to end through `Model::read_from_file` in the suite — the whole
   path the app uses, minus the GUI dialog itself.
4. **Sampling at the centroid gives one colour per triangle**, so a detailed texture on a low-poly
   mesh is quantised hard — a 12-triangle box gets 12 colours no matter how intricate its texture.
   That is inherent to the approach this plan chose (and to what the filament count can express),
   but it is worth saying out loud in the release note: texture import approximates, it does not
   reproduce.
5. **meshoptimizer's decoder is four files and ~240 KB of source**, and it built clean under MSVC
   with no warnings at this repo's level. The `deps_src/` placement means no dependency rebuild for
   anyone, which is the property Stage 1's decision 4 bought and 3b would have spent.

### What is left

* **3b (Draco)** — the named refusal stands. `box_draco.glb` is genuinely compressed, so when Draco
  is wanted the Stage 1 assertion is inverted against the same file rather than needing a new one.
* **Paletted / greyscale / 16-bit PNG textures** (finding 2).
* **KTX2 / Basis textures** — refused by name when required, dropped with a warning when used.
* **The hidden-instance colour question** (finding 3), which is a product decision, not a bug.
