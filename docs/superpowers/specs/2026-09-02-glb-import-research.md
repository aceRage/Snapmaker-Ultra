# GLB / glTF 2.0 import — research and design

Date: 2026-09-02
Branch context: `feat/ultra-preferences`
Status: research only. No code was changed.

All web facts were checked on 2026-09-02 and the source URL is given inline. All
code claims are `file:line` references into this working tree at the time of writing.

---

## 0. Executive summary

* Today the tree has **no glTF/GLB support at all**. The only mentions of `.glb`/`.gltf`/`.fbx`
  anywhere under `src/` are three dead `iends_with` checks in a single boolean at
  `src/slic3r/GUI/Plater.cpp:11635-11637` — they can never be true, because those extensions are
  rejected earlier by the file dialog filter, the drag-and-drop regex and
  `Model::read_from_file`.
* **Upstream OrcaSlicer already carries the code we would need**, ported from Bambu Studio:
  `src/libslic3r/Format/AssimpImport.cpp/.hpp` plus `deps/Assimp/Assimp.cmake` (Assimp v5.4.3 with
  only the glTF, OBJ and FBX importers compiled in). OrcaSlicer also carries `deps/Draco` and a
  standalone `src/libslic3r/Format/DRC.cpp` `.drc` reader/writer. Our fork predates all of this.
* Because of that, the cheapest credible v1 is **port, don't invent**: bring
  `deps/Assimp` + `Format/AssimpImport` across, then write a thin
  `Format/GLTF.cpp` that turns the Assimp scene into `ModelObject`/`ModelVolume` with the
  Y-up→Z-up rotation and mm handling that Bambu's code does *not* do.
* The main things nobody upstream has solved and we would have to solve ourselves:
  **up-axis conversion**, **unit handling**, **node hierarchy → objects vs parts**, and
  **per-primitive material colour → filament slots** (the existing OBJ colour path is hard-wired to
  "exactly one object with exactly one volume").
* Licence is a non-issue: repo is AGPL-3.0 (`LICENSE.txt`), and every candidate library is
  MIT / BSD-3 / Apache-2.0, all one-way compatible with AGPLv3.
* Biggest non-obvious risk: **Assimp's CVE history versus our network-facing upload path**.
  `src/slic3r/GUI/RemoteHub.cpp:1050` accepts uploads from a phone over the LAN and hands the file
  to the loader. Adding a large C++ binary parser behind that surface deserves a deliberate decision.

---

## 1. How import works today

### 1.1 The single funnel: `Model::read_from_file`

`src/libslic3r/Model.cpp:229` declares `Model::read_from_file(...)`; the extension dispatch is a
plain `if/else if` chain on `boost::algorithm::iends_with`:

| Extension | Line | Loader |
|---|---|---|
| `.stl` | `Model.cpp:265` | `load_stl` |
| `.oltp` | `Model.cpp:267` | `load_stl(..., 256)` (custom 256-byte header) |
| `.obj` | `Model.cpp:269` | `load_obj` + colour post-processing (below) |
| `.svg` | `Model.cpp:299` | `load_svg` |
| `.amf` | `Model.cpp:303` | `load_amf` |
| `.3mf` | `Model.cpp:306` | `load_bbs_3mf` |
| `.usd/.usda/.usdc/.usdz/.abc/.ply` | `Model.cpp:313-321`, `__APPLE__` only | Apple ModelIO → temp STL → `load_stl` (`src/libslic3r/Format/ModelIO.mm`) |
| anything else | `Model.cpp:325` | `throw Slic3r::RuntimeError(_L("Unknown file format. Input file must have .stl, .obj, .amf(.xml) extension."))` |

After a successful load the common tail does the work every format shares
(`Model.cpp:332-352`): raise `message` if the loader set one else the generic
"Loading of a model file failed." (`:332-337`), reject an empty model (`:339`),
stamp `o->input_file = input_file` on every object (`:342`),
and optionally `model.add_default_instances()`.

Note that **STEP is not in this chain**. STEP has its own entry point,
`Model::read_from_step` (declared `src/libslic3r/Model.hpp:1584`), called directly from the GUI at
`src/slic3r/GUI/Plater.cpp:12232-12278` because it needs a mesh-tessellation dialog, a
progress callback, an encoding-sniffing callback and linear/angular deflection parameters.
`Model::read_from_archive` (`Model.hpp:1618`, body around `Model.cpp:359-410`) is a third entry
point for project archives (`.3mf`, `.zip.amf`).

**This matters for design:** a new format can either join the `read_from_file` chain (cheap, but you
inherit its narrow callback surface) or get its own `read_from_gltf` sibling like STEP did
(more code, but room for its own progress/options/colour callbacks).

### 1.2 STEP: which library, how bundled

STEP is OCCT (Open CASCADE). `src/libslic3r/Format/STEP.hpp` includes OCCT headers directly
(`XCAFDoc_DocumentTool.hxx`, `XCAFApp_Application.hxx`, `Message_ProgressIndicator.hxx`), and
`src/libslic3r/CMakeLists.txt:550-584` does `find_package(OpenCASCADE REQUIRED)` and links a
hand-listed set of `TK*` libraries. OCCT is built as a prebuilt external dependency:
`deps/OCCT/OCCT.cmake` fetches `V7_6_0.zip` from GitHub via the repo's own
`Snapmaker_Orca_add_cmake_project` helper (`deps/CMakeLists.txt:113-211`), is `include()`d at
`deps/CMakeLists.txt:366`, and is listed in `_dep_list` (`deps/CMakeLists.txt:384`) so the
top-level deps build depends on it. Shared on Windows, static elsewhere.

This is the template a new heavyweight dependency (Assimp, or Draco) would follow.

### 1.3 OBJ: what it does and does not do

`src/libslic3r/Format/OBJ.cpp:24` `load_obj(path, TriangleMesh*, ObjInfo&, message)`:

* Parses with the vendored `objparser` (`src/libslic3r/Format/objparser.cpp`).
* Reads `mtllib`/`usemtl` and parses the `.mtl` next to the file (`OBJ.cpp:36-68`).
* Faces: **triangles and quads only**. A face with >4 vertices is a hard error
  ("The file contains polygons with more than 4 vertices.", `OBJ.cpp:79-82`); quads are split into
  two triangles (`OBJ.cpp:135-141`, `OBJ.cpp:186-192`).
* Vertex colours: the extended `v x y z r g b a` form is read into `ObjInfo::vertex_colors`
  (`OBJ.cpp:103-108`).
* Face colours: per-face material colour derived from `Ka + Kd` (or just `Kd` if the sum clips)
  plus `Tr` as alpha, pushed into `ObjInfo::face_colors` (`OBJ.cpp:138-170`).
* Textures: if a material has `map_Kd`, `ObjInfo::has_uv_png` is set and per-face UVs are collected
  (`OBJ.cpp:157-168`) — but the actual texture→colour path is **commented out** at
  `Model.cpp:290-296` ("Importing obj with png function is developing.").

`src/libslic3r/Format/OBJ.cpp:204` `load_obj(path, Model*, ...)` then does
`model->add_object(object_name, path, std::move(mesh))` — **one file becomes exactly one
`ModelObject` with exactly one `ModelVolume`**. OBJ `o`/`g` groups are flattened away.
`src/libslic3r/Format/STL.cpp:17-40` does the same. AMF is the counter-example: it creates one
object per `<object>` (`Format/AMF.cpp:301`) and one volume per `<volume>` (`Format/AMF.cpp:374`).

### 1.4 OBJ colours → filaments (the machinery to reuse)

`ObjInfo` is defined at `src/libslic3r/Format/OBJ.hpp:12-22`; the callback type is
`ObjImportColorFn` at `OBJ.hpp:10`.

The flow is:

1. `Model::read_from_file` (`Model.cpp:271-289`) loads the mesh, then — if `obj_info.vertex_colors`
   is non-empty, else if `obj_info.face_colors` is non-empty and there is no texture — calls the
   GUI callback `objFn`.
2. The GUI callback is a lambda built in `Plater::priv::load_files`
   (`src/slic3r/GUI/Plater.cpp:12222-12230`). It early-returns unless the path ends in `.obj`
   (`Plater.cpp:12224` — **this guard would have to be widened for glTF**), then opens
   `ObjColorDialog` (`Plater.cpp:12227`, class at `src/slic3r/GUI/ObjColorDialog.hpp:101-116`).
   The same lambda is duplicated in the reload-from-disk path at `Plater.cpp:14678-14709`.
3. `ObjColorDialog`/`ObjColorPanel` (`ObjColorDialog.hpp:20-98`) k-means-clusters the input colours
   down to a user-chosen count (`deal_algo(cluster_number)`), lets the user map each cluster to a
   filament slot or add a new filament, and writes back `filament_ids` (one entry per input colour)
   and `first_extruder_id`.
4. Back in `Model.cpp:279` / `:287` the result is applied by
   `Model::obj_import_vertex_color_deal` (`Model.cpp:3148`) or
   `Model::obj_import_face_color_deal` (`Model.cpp:3270`).

Both of those write into `ModelVolume::mmu_segmentation_facets`
(`src/libslic3r/Model.hpp:882`, a `FacetsAnnotation`; `is_mm_painted()` at `Model.hpp:1016`) —
i.e. **imported colour becomes ordinary multi-material painting**, exactly as if the user had
painted it with the MMU gizmo. Filament ids are encoded through
`get_real_filament_id` (`Model.cpp:3140`) against the table `CONST_FILAMENTS`
(`Model.cpp:47-49`), which has 16 usable slots. The vertex-colour variant is the clever one: for a
triangle whose three vertices land on three different filaments it emits a split-triangle
annotation string (`Model.cpp:3213-3245`).

**Two hard constraints in that code, both relevant to glTF:**

```cpp
// Model.cpp:3154-3157 (and identically 3274-3277)
if (model->objects.size() == 1 ) {
    auto obj = model->objects[0];
    obj->config.set("extruder", first_extruder_id);
    if (obj->volumes.size() == 1) {
```

and

```cpp
// Model.cpp:3281
if (volume->mesh().its.indices.size() != face_filament_ids.size()) { return false; }
```

So the colour path **only works for a single object with a single volume whose face/vertex count
matches the colour array exactly**. A glTF with several primitives cannot use it unchanged.

### 1.5 "One object with parts" vs "many objects"

Three separate rules, all in the GUI:

* `Plater::priv::load_files` (`Plater.cpp:11528`) sets
  `one_by_one = input_files.size() == 1 || printer_technology == ptSLA` (`Plater.cpp:11548`).
  With one file it is always true, so each object in the loaded `Model` becomes a separate plater
  object via `load_model_objects` (`Plater.cpp:12474`, definition at `Plater.cpp:12669`).
* If a *single non-project* file produced several single-volume objects sitting at different Z,
  `Model::looks_like_multipart_object()` (`Model.cpp:849`) returns true and the user is asked
  *"…should the file be loaded as a single object having multiple parts?"* (`Plater.cpp:12405-12414`);
  Yes calls `Model::convert_multipart_object(filaments_cnt)` (`Model.cpp:877`), which flattens every
  object's volumes into one object, baking instance transforms into volume transforms.
* If **several** files were dropped at once, `new_model` accumulates them and the user is asked
  *"Load these files as a single object with multiple parts?"* at `Plater.cpp:12513`.

Post-load, every object gets `center_around_origin(false)` for non-3MF/AMF (`Plater.cpp:12432`),
a Z rotation by the `preferred_orientation` setting (`Plater.cpp:12311`), and — behind this fork's
`auto_drop_on_import` preference — `ensure_on_bed()` (`Plater.cpp:12450`).

Unit rescue prompts also live here: `looks_like_saved_in_meters()` (`Model.cpp:969`, threshold
0.008 mm³) and `looks_like_imperial_units()` (`Model.cpp:929`, threshold 8 mm³) drive the
"object is too small, scale to millimeters?" dialogs at `Plater.cpp:12369-12386`.
**This is free infrastructure a glTF reader can lean on.**

### 1.6 Every place an extension is whitelisted

A new format has to be added to *all* of these, or it will only half work:

| Where | Reference | Current value |
|---|---|---|
| `FileType` enum | `src/slic3r/GUI/GUI_App.hpp:103-125` | `FT_STEP, FT_STL, FT_OBJ, FT_AMF, FT_3MF, …` |
| File-dialog wildcards | `src/slic3r/GUI/GUI_App.cpp:652-677` | `FT_MODEL` = `.3mf .stl .oltp .stp .step .svg .amf .obj` (+ USD/ABC/PLY on Apple, `:661-665`) |
| Import menu label | `src/slic3r/GUI/MainFrame.cpp:2694,2700` | `"Import 3MF/STL/STEP/SVG/OBJ/AMF"` |
| Add-model dialog | `src/slic3r/GUI/GUI_App.cpp:4321`, `Plater.cpp:14459` | uses `file_wildcards(FT_MODEL)` |
| Drag-and-drop | `src/slic3r/GUI/Plater.cpp:20063` | `pattern_drop = ".*[.](stp|step|stl|oltp|obj|amf|3mf|svg|zip)"` (drop target at `Plater.cpp:9972`, `:10611`) |
| Loader dispatch | `src/libslic3r/Model.cpp:265-325` | see §1.1 |
| Windows file association | `src/slic3r/GUI/GUI_App.cpp:2279-2287`, `:2827-2839`, `:6382-6395`, `associate_files` at `:7556` | `3mf`, `stl`, `step`/`stp`, `gcode` |
| macOS document types | `src/dev-utils/platform/osx/Info.plist.in:40+` | `stl`, `obj`, `amf`, `3mf`, `gcode` |
| CLI `--load` | `src/Snapmaker_Orca.cpp:1620-1654` | iterates `m_input_files`, special-cases `.3mf` as first file (`:1638`), otherwise `Model::read_from_file` (`:1654`) — **so the CLI inherits any new extension for free** |
| Phone upload (server) | `src/slic3r/GUI/RemoteHub.cpp:1050` `spool_upload`, allow-list at `:1061-1065` | `.3mf .stl .obj .step .stp` |
| Phone upload (client) | `resources/web/orca/stream_center.html:1161` | `/\.(3mf|stl|obj|step|stp)$/i` |

---

## 2. What glTF/GLB actually contains, and what it should become

Facts below are from the glTF 2.0 specification
(<https://raw.githubusercontent.com/KhronosGroup/glTF/main/specification/2.0/Specification.adoc>,
fetched 2026-09-02).

| glTF concept | Spec detail | What it should become in the slicer |
|---|---|---|
| **Coordinate system** | Right-handed, **+Y up**; "The front side of a glTF asset faces +Z, the left side faces +X." | Slicer is Z-up. Apply a single Rx(+90°) at the scene root: `(x,y,z)_gltf → (x, −z, y)_slicer`. Do it once on the root transform so it composes with node transforms. |
| **Units** | "The units for all linear distances are meters." | See §2.1 — recommendation is **treat 1 unit = 1 mm** and let the existing "too small, scale to millimetres?" prompt handle genuine metre files. |
| **Scenes / nodes** | A node tree; each node has either `matrix` or TRS (`translation`, `rotation` quaternion, `scale`), composed as `T * R * S`. Nodes may reference a `mesh` and/or children. | Walk the tree accumulating world transforms. Either bake into vertices, or keep per-`ModelVolume` transforms (recommended — see §4.2). Node `name` → volume name; scene/file name → object name. |
| **Meshes / primitives** | A mesh is an **array of primitives**; "Splitting one mesh into several primitives can be useful … to assign different materials to different parts of the mesh." | **One primitive = one `ModelVolume`.** This is the natural unit because material (and therefore colour) is per-primitive. |
| **Primitive modes** | seven modes: points, lines, line loop, line strip, triangles, triangle strip, triangle fan. Quads do not exist. | Accept TRIANGLES / TRIANGLE_STRIP / TRIANGLE_FAN (de-strip/de-fan into triangles). Silently drop POINTS/LINES/LINE_LOOP/LINE_STRIP — they carry no printable volume; count them and mention in the log. |
| **Indices** | Optional. Without `indices` the vertices are consumed sequentially. | Both paths must be handled; a hand-rolled reader that assumes `indices` will fail on real files. |
| **Accessors** | May be **sparse** (base + sparse overrides), and component types vary (float / u8 / u16 / u32, normalized). | A gotcha for hand-rolled readers. cgltf/fastgltf/Assimp all handle it; naive code does not. |
| **`COLOR_0`** | "RGB or RGBA vertex color linear multiplier"; float, u8-normalized or u16-normalized; VEC3 implies alpha 1.0. | Per-vertex colour → the existing `obj_import_vertex_color_deal` shape (§1.4). Note it is a *multiplier* on base colour, so strictly `COLOR_0 × baseColorFactor`. |
| **Materials** | PBR metallic-roughness; `pbrMetallicRoughness.baseColorFactor` is a linear RGBA vec4; `baseColorTexture` optional. | `baseColorFactor` → one RGBA per primitive → a per-face colour array → the existing `ObjColorDialog` clustering → filament slots. Note glTF colours are **linear**, OBJ `Kd` is conventionally sRGB — decide whether to sRGB-encode before showing swatches, otherwise imported colours will look washed out next to filament swatches. |
| **Textures** | PNG/JPEG by default; `KHR_texture_basisu` for KTX2/Basis. | v1/v2: ignore, but *tell the user* the model had a texture that was dropped. v3: sample `baseColorTexture` at each triangle's centroid UV → per-face colour → same clustering path. That is essentially what Bambu Studio's "texture to colour" does. |
| **Skins / morph targets / animations** | Optional; skinned vertices are stored in bind pose. | Ignore. Using the bind pose is correct and gives a printable mesh. Must not error out. |
| **`KHR_draco_mesh_compression`** | Mesh buffers replaced by a Draco payload. | Cannot be read without Draco. Must fail with a *specific* message, not "Loading of a model file failed." |
| **`KHR_mesh_quantization`** | Positions may be i8/u8/i16/u16 instead of float; requires a node/accessor scale to recover real units. | Common in optimized/web assets. Handled by fastgltf and cgltf; if unhandled you silently get a model at the wrong scale — worse than an error. |
| **`EXT_meshopt_compression`** | Buffer views compressed by meshoptimizer. | Needs `meshoptimizer` to decode. Rarer than Draco in the wild but growing. |
| **`KHR_materials_*`** (emissive strength, transmission, volume, specular…) | Purely appearance. | Ignore — they never appear in `extensionsRequired` for geometry purposes. |
| **`extensionsRequired`** | Assets list extensions a reader *must* support. | Read this array first; if it contains anything we cannot honour, name it in the error. This is the single most valuable error-handling detail. |
| **Non-manifold / open meshes** | Scanner and AI-generated GLB are routinely non-manifold, self-intersecting, or have flipped normals. | Same path as any STL: `TriangleMesh` records `RepairedMeshErrors` (`src/libslic3r/TriangleMesh.hpp:19-44`), the object list shows the manifold warning, and the Windows-only "Fix Model" path (`Plater.cpp:16383` `on_repair_model`) is available. Nothing new needed, but expect a higher warning rate than STL. |

### 2.1 The units decision (this one is genuinely contentious)

The spec says metres. A literal reader multiplies by 1000. But the overwhelming majority of `.glb`
files people want to print come out of Blender, Meshy/Tripo-style generators, or phone scanners,
where "1 unit" is whatever the author had on screen — usually intended as millimetres or as an
arbitrary scale to be resized in the slicer. Multiplying those by 1000 produces a 200-metre object
and a confusing "object too big" state.

**Recommendation:** import at **1 glTF unit = 1 mm**, identical to how STL and OBJ (which are
unitless) are treated, and rely on the *existing* rescue prompt: `looks_like_saved_in_meters()`
(`Model.cpp:969`) fires when the object volume is below 0.008 mm³ and offers
`convert_from_meters(true)` (`Plater.cpp:12369-12379`). A genuinely-metre-authored file is tiny in
mm and therefore trips that prompt automatically. This gets the common case right, the spec-pure
case recoverable, and costs zero new code.

The alternative — spec-pure ×1000 with a "looks too big, divide by 1000?" prompt — would need a new
`looks_like_saved_in_kilometres()` equivalent and would annoy the majority. Flagged as an open
question in §5.4 because it is a product call, not a technical one.

---

## 3. Library options

Repo licence is **AGPL-3.0** (`LICENSE.txt`). MIT, BSD-3-Clause and Apache-2.0 are all
one-way compatible with AGPLv3 (their terms can be satisfied inside an AGPL distribution), so
**none of the candidates below present a licence problem**. Apache-2.0 (Draco) is explicitly
GPLv3/AGPLv3-compatible.

GitHub metadata below was read from `api.github.com` on 2026-09-02.

### 3.1 Candidates

| Library | Licence | Repo size / shape | Activity (as of 2026-09-02) | glTF coverage | Verdict |
|---|---|---|---|---|---|
| **Assimp** <https://github.com/assimp/assimp> | BSD-3-Clause (`NOASSERTION` on GitHub because the text is customised; the LICENSE file is a standard 3-clause BSD) | ~272 MB repo, compiled static/shared lib. With only glTF+OBJ+FBX importers enabled the binary is a few MB. | last push 2026-09-02; latest release **v6.0.5, 2026-04-30** | glTF1+glTF2, GLB, FBX, plus 40+ others. Draco behind `ASSIMP_BUILD_DRACO` (default OFF). Does its own triangulation, normal generation and node flattening. | **Recommended for v1**, purely because upstream OrcaSlicer/Bambu already ship a working integration we can port. Downsides: size, and a long CVE history in the fuzz-prone importers. |
| **cgltf** <https://github.com/jkuhlmann/cgltf> | MIT | 488 KB repo; **single-header C99**, `cgltf.h` (drop into `deps_src/`) | last push 2026-02-02; latest release **v1.15, 2024-02-09**; 60 open issues; not archived | Full glTF 2.0 parse; `EXT_meshopt_compression`, `KHR_texture_basisu` and `KHR_draco_mesh_compression` are *recognised* but decompression needs the external library. Gives you accessor readers; you write the triangulation and scene walk. | **Recommended if we want our own reader.** Smallest possible footprint, matches the `deps_src/` header-only pattern exactly, no new external build. Release cadence has slowed (2 years since v1.15) but the repo is alive. |
| **fastgltf** <https://github.com/spnda/fastgltf> | MIT (embeds simdjson, Apache-2.0) | 2.6 MB repo; a compiled C++17 library, not header-only | last push **2026-08-31**; latest tagged release v0.9.0 (2024-07-08); 579 stars | Broadest extension coverage of the three: `KHR_draco_mesh_compression` support was added in v0.9.0, `EXT_meshopt_compression` parsing supported. SIMD base64/JSON: benchmarks claim ~24× tinygltf and ~7× cgltf on embedded-buffer assets (<https://fastgltf.readthedocs.io/latest/overview.html>). | **Best pure-quality choice**, and the most actively developed. Costs a real dependency (CMake subproject + simdjson) rather than a header drop. Smaller community than cgltf/tinygltf. |
| **tinygltf** <https://github.com/syoyo/tinygltf> | MIT | 8.8 MB repo | last push 2026-08-02; 2518 stars | **Caveat:** v3 (`tiny_gltf_v3.h` + `tiny_gltf_v3.c`) is now the mainline and is a **C11 library, not header-only C++**; the old C++ `tiny_gltf.h` has been moved to `attic/` and is "no longer built, tested, or installed". Draco via `TINYGLTF_ENABLE_DRACO` (you supply Draco). | **Not recommended.** The API most people know is now deprecated; adopting it means either pinning a dead header or absorbing a v2→v3 API break. |
| **Draco** <https://github.com/google/draco> | Apache-2.0 | ~226 MB repo; sizeable C++ library, decoder-only build possible (`draco_dec`) | last push 2026-08-18; release 1.5.7 | Only needed for `KHR_draco_mesh_compression` (and standalone `.drc`). | Phase 3 only. Note OrcaSlicer already vendors it at `deps/Draco/Draco.cmake` (tag 1.5.7) for its `.drc` format — that .cmake is directly liftable. |
| **meshoptimizer** <https://github.com/zeux/meshoptimizer> | MIT | 7.1 MB repo, small compiled lib | last push 2026-09-01; 8296 stars | Decoder for `EXT_meshopt_compression`. | Phase 3, optional. Cheap and well-maintained if we ever see meshopt assets. |

### 3.2 What upstream slicers actually do

* **OrcaSlicer** (main branch, checked 2026-09-02): `src/libslic3r/Format/` contains
  **`AssimpImport.cpp` (11,377 B) and `AssimpImport.hpp` (213 B)**, and `deps/` contains both
  **`Assimp`** and **`Draco`** folders. `deps/Assimp/Assimp.cmake` fetches assimp v5.4.3
  (v5.3.1 for CMake < 3.22) with all importers off except **glTF, OBJ, FBX**, tests/samples/tools
  off, and **no** `ASSIMP_BUILD_DRACO`. `deps/Draco/Draco.cmake` fetches Draco 1.5.7 — but that is
  for `src/libslic3r/Format/DRC.{cpp,hpp}`, a standalone `.drc` reader/writer
  (`load_drc`/`store_drc`), not for glTF Draco.
  The OrcaSlicer wiki's importable-format list is *"STL, 3MF, STEP, DRC, OBJ, AMF, SVG, ZIP"*
  (<https://github.com/OrcaSlicer/OrcaSlicer/wiki/import_export>) — **glTF/GLB/FBX are not offered
  as general import formats**, only through the Assimp textured-model path.
  The user-facing feature request <https://github.com/OrcaSlicer/OrcaSlicer/issues/3926>
  ("adding support of GLB/GLTF file format", opened 2024-02-01) was **closed as not planned / stale**.
* **Bambu Studio**: same `AssimpImport.cpp/.hpp` and same `deps/Assimp/Assimp.cmake`. Bambu ships
  it as the *Texture-to-Colour* feature (2.7 beta onwards): import a textured OBJ/glTF/GLB/FBX and
  the texture is converted into multi-colour painting. Bambu's own documentation states
  **Draco-compressed glTF/GLB is not yet supported**, and that only one textured model can be
  converted at a time
  (<https://wiki.bambulab.com/en/software/bambu-studio/import_obj>; the page itself returned 402 to
  automated fetch, content confirmed via search snippets and via
  <https://github.com/bambulab/BambuStudio/releases/tag/v02.07.00.55>).
* **PrusaSlicer**: no glTF path — `src/libslic3r/Format/` does not exist as a directory at that
  path in their tree layout, and Prusa's supported-format documentation lists 3MF, STL, STEP, OBJ
  only (<https://help.prusa3d.com/article/supported-file-formats_1772>). No port opportunity.
* **Cura**: supports `.gltf` and `.glb` (plus `.dae`, `.ply`, `.zae`) through
  `plugins/TrimeshReader/TrimeshReader.py`, which delegates to the Python `trimesh` package. It
  handles `trimesh.Scene` with multiple geometries by grouping them under a parent node, and does
  **no** unit or colour handling. LGPLv3, and Python — **not portable to us**, but a useful
  precedent that "scene with several meshes → one grouped object" is an accepted behaviour.
  (<https://github.com/Ultimaker/Cura/blob/main/plugins/TrimeshReader/TrimeshReader.py>)

### 3.3 What Bambu/Orca's `AssimpImport.cpp` actually does (and doesn't)

Read from
<https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer/main/src/libslic3r/Format/AssimpImport.cpp>:

```cpp
bool load_assimp_textured_model(const std::string& path, TexturedMesh& out, std::string* error_message);
unsigned int assimp_import_flags(const std::string& path);
void collect_materials(const aiScene& scene, const boost::filesystem::path& base_dir, TexturedMesh& out);
```

* Flags: `aiProcess_Triangulate | aiProcess_GenNormals | aiProcess_PreTransformVertices |
  aiProcess_SortByPType`, plus `aiProcess_FlipUVs` for FBX/GLB.
* Node hierarchy is **not** walked; it relies on `aiProcess_PreTransformVertices` with
  `AI_CONFIG_PP_PTV_KEEP_HIERARCHY = true`, then iterates `scene->mMeshes[i]` flat.
* Materials: `AI_MATKEY_BASE_COLOR` falling back to `AI_MATKEY_COLOR_DIFFUSE` (default white);
  textures from `aiTextureType_BASE_COLOR` then `aiTextureType_DIFFUSE`, embedded via
  `scene->GetEmbeddedTexture()` or loaded from disk relative to the model.
* Fills a `TexturedMesh` (vertices, indices, uvs, textures, `material_ids`,
  `material_texture_map`, `material_colors`) — **not** `ModelObject`/`ModelVolume`.
* **No unit scaling and no up-axis conversion.** Assimp's internal convention is Y-up right-handed,
  the same as glTF, so nothing rotates it into the slicer's Z-up world. We would have to add that.

So the port gives us the parsing and the material/texture extraction for free, and leaves us the
three slicer-specific pieces: Z-up, mm, and `Model` construction.

### 3.4 Recommendation

Two defensible routes.

**Route A — port Assimp (recommended if we also want FBX, or want Bambu's texture-to-colour later).**
Lift `deps/Assimp/Assimp.cmake` and `Format/AssimpImport.{cpp,hpp}` from OrcaSlicer, adapt the deps
helper name (`Snapmaker_Orca_add_cmake_project`), add `dep_Assimp` to `_dep_list`
(`deps/CMakeLists.txt:369-390`), then write `Format/GLTF.cpp` on top of it. Buys `.fbx`,
`.dae`, `.ply` almost free. Costs: a multi-MB dependency, a full deps rebuild on three platforms,
and a parser with a real CVE record sitting behind the LAN upload endpoint.

**Route B — vendor cgltf (recommended if we want the smallest, most auditable change).**
Drop `cgltf.h` into `deps_src/cgltf/` with a five-line `CMakeLists.txt` in the exact shape of
`deps_src/fast_float/CMakeLists.txt` (INTERFACE library, `target_include_directories(... SYSTEM
INTERFACE ...)`, `target_compile_features(... cxx_std_14)`), add `add_subdirectory(cgltf)` to
`deps_src/CMakeLists.txt`, link it in `src/libslic3r/CMakeLists.txt:586-604`. **No change to the
prebuilt `deps/` tree at all** — nobody has to rebuild dependencies, which matters a lot for a
fork with prebuilt deps. Costs: we write the scene walk, the strip/fan de-indexing and the
colour extraction ourselves (~600-900 lines).

My preference is **Route B for v1**, because it is a self-contained `deps_src/` addition with no
deps rebuild, no new attack surface of Assimp's scale, and full control over up-axis/units/hierarchy
— which we have to write in either case. Route A becomes the better answer the moment FBX or
Bambu-style texture-to-colour is also wanted; and Routes A and B are not exclusive (Assimp could be
added later behind the same `Format/GLTF.hpp` API).

---

## 4. Design

### 4.1 New files

```
src/libslic3r/Format/GLTF.hpp        // public API
src/libslic3r/Format/GLTF.cpp        // reader
deps_src/cgltf/cgltf.h               // vendored (Route B)
deps_src/cgltf/CMakeLists.txt        // INTERFACE target, mirrors deps_src/fast_float/CMakeLists.txt
tests/libslic3r/test_gltf.cpp
tests/data/test_gltf/*.glb|*.gltf
```

Proposed API, deliberately shaped like `Format/OBJ.hpp`:

```cpp
namespace Slic3r {

struct GltfInfo {
    // one entry per triangle of the concatenated volumes, in volume order
    std::vector<RGBA>  face_colors;
    std::vector<RGBA>  vertex_colors;      // only when every primitive has COLOR_0
    bool               is_single_material {false};
    bool               had_textures       {false};   // v1: report, do not use
    size_t             dropped_primitives {0};       // points/lines
    std::vector<std::string> unsupported_extensions; // from extensionsRequired
};

extern bool load_gltf(const char *path, Model *model, GltfInfo &info, std::string &message,
                      const char *object_name = nullptr);

} // namespace Slic3r
```

### 4.2 What the reader returns

* **One `ModelObject`** named after the glTF `scene` name, else the file stem.
  (Rationale: the multi-object alternative would trip `looks_like_multipart_object()`'s
  "different Z" heuristic (`Model.cpp:849-866`) only sometimes, giving inconsistent prompts; and
  scanner/AI GLBs are almost always one physical thing. See §5.4 — this is an open question.)
* **One `ModelVolume` per mesh primitive**, `ModelVolumeType::MODEL_PART`, named
  `<node name>` (or `<node name>_<primitive index>` when a mesh has several primitives, falling back
  to `<mesh name>` then `part_<n>`).
* **Transforms kept, not baked.** Compute each node's world matrix by composing
  `T*R*S` (or `matrix`) down the tree, pre-multiply by the fixed
  `Rx(+90°)` up-axis correction, and set it with `ModelVolume::set_transformation(Transform3d)`
  (`Model.hpp:972`). Keeping them means the user can still move a part independently and the volume
  matrices round-trip through 3MF. Bake only if a node's matrix is non-uniformly scaled *and*
  mirrored in a way `Geometry::Transformation` handles badly — decompose-and-check, fall back to
  baking into `its.vertices`.
* **Scale:** none applied (1 unit = 1 mm, §2.1). Set
  `ModelVolume::source.is_converted_from_meters` only if we ever add spec-pure mode.
* **`source`** fields (`Model.hpp:806-820`) filled: `input_file`, `object_idx`, `volume_idx`,
  `mesh_offset`, `transform` — same as 3MF/AMF do, so "reload from disk" works.
* Degenerate results: if every primitive was points/lines, return `false` with a message naming
  what was found, so `Model.cpp:332-337` surfaces it rather than the generic
  "Loading of a model file failed."

### 4.3 Registration points (the checklist from §1.6)

1. `src/libslic3r/Model.cpp` — add before the `else` at `:325`:
   `else if (iends_with(input_file, ".glb") || iends_with(input_file, ".gltf")) { GltfInfo info; result = load_gltf(...); … }`
   and update the message string at `:325`.
2. `src/libslic3r/CMakeLists.txt:192-212` — add `Format/GLTF.cpp` / `Format/GLTF.hpp`;
   link `cgltf` in the `PRIVATE` block at `:593-604`.
3. `deps_src/CMakeLists.txt` — `add_subdirectory(cgltf)` under the header-only group.
4. `src/slic3r/GUI/GUI_App.hpp:103-125` — add `FT_GLTF` (optional; only needed if we want a
   dedicated filter entry).
5. `src/slic3r/GUI/GUI_App.cpp:661-667` — add `".glb"sv, ".gltf"sv` to **both** the Apple and
   non-Apple `FT_MODEL` lists; add an `FT_GLTF` row if step 4 was taken.
6. `src/slic3r/GUI/MainFrame.cpp:2694,2700` — label becomes
   `"Import 3MF/STL/STEP/SVG/OBJ/AMF/GLB"` (both `#ifdef` branches).
7. `src/slic3r/GUI/Plater.cpp:20063` — extend `pattern_drop` to
   `(stp|step|stl|oltp|obj|amf|3mf|svg|zip|glb|gltf)`.
8. `src/slic3r/GUI/Plater.cpp:12224` — widen the `.obj` guard in the colour callback (phase 2).
   Same edit at `Plater.cpp:14679` for reload-from-disk.
9. `src/slic3r/GUI/RemoteHub.cpp:1061-1065` — add `.glb` and `.gltf` to the upload allow-list, and
   `resources/web/orca/stream_center.html:1161` to the client-side regex and its message string.
10. `src/dev-utils/platform/osx/Info.plist.in:40+` — add a `CFBundleDocumentTypes` entry for
    `glb`/`GLB` (and `gltf`).
11. `src/slic3r/GUI/GUI_App.cpp:2279-2287`, `:2827-2839`, `:6382-6395`, and the Preferences page —
    optional `associate_glb` checkbox mirroring `associate_stl`.
12. CLI needs **no change** — `src/Snapmaker_Orca.cpp:1654` calls `Model::read_from_file`
    unconditionally for non-3MF inputs.

### 4.4 Colour / material mapping (phase 2)

Reuse the OBJ machinery rather than inventing anything:

1. Reader emits `GltfInfo::face_colors` — one RGBA per triangle of the concatenated volumes,
   from `material.pbrMetallicRoughness.baseColorFactor` (sRGB-encoded for display, see §2), with
   `is_single_material` set when the whole file has one material.
2. `Model::read_from_file` calls the existing `ObjImportColorFn` — no new callback type. That means
   `ObjColorDialog` (`ObjColorDialog.hpp:101`) is reused verbatim, including its k-means clustering,
   "approximate match", "keep colour" and "add filament" buttons.
3. Apply with a **generalised** version of `Model::obj_import_face_color_deal`. The current one
   (`Model.cpp:3270-3295`) hard-fails unless `objects.size() == 1 && volumes.size() == 1` and the
   face count matches exactly. Add a sibling that walks all volumes of the single object with a
   running triangle offset:

   ```cpp
   // proposed: Model.cpp, next to obj_import_face_color_deal
   static bool import_face_color_deal(const std::vector<unsigned char>& face_filament_ids,
                                      unsigned char first_extruder_id, Model* model);
   ```
   and keep `obj_import_face_color_deal` as a one-volume wrapper so the OBJ path is untouched.
   Same treatment for `obj_import_vertex_color_deal` (`Model.cpp:3148`) if we support `COLOR_0`.
4. Filament ids are indices into `CONST_FILAMENTS` (`Model.cpp:47-49`) — **16 slots**. Materials
   beyond that must be clustered down; `ObjColorPanel::deal_algo` already does this and warns.
5. Result lands in `ModelVolume::mmu_segmentation_facets`, i.e. it becomes ordinary MMU painting
   and needs no changes anywhere downstream (slicing, 3MF save, gizmo editing all just work).

### 4.5 Texture handling

* **v1/v2: ignore.** Set `GltfInfo::had_textures` and push a notification
  ("This model's colours come from a texture, which was not imported") via the notification manager
  — the same channel `Plater.cpp` already uses for import warnings. Silently dropping the colour is
  the worst outcome; users will file bugs.
* **v3 option A (cheap):** sample `baseColorTexture` at each triangle's centroid UV, produce a
  per-face colour, feed the existing clustering path. libpng and libjpeg are already linked
  (`src/libslic3r/CMakeLists.txt:554`, `:602`), so decoding embedded PNG/JPEG needs no new dep;
  KTX2/Basis (`KHR_texture_basisu`) would.
* **v3 option B (proper):** port Bambu's `TexturedMesh` + texture-to-colour pipeline wholesale.
  Much larger, but it is a shipped, user-tested design.

### 4.6 Error handling

| Condition | Behaviour |
|---|---|
| `extensionsRequired` contains `KHR_draco_mesh_compression` | Fail with: *"This file uses Draco mesh compression, which Snapmaker Orca cannot read yet. Re-export it without Draco compression."* Do **not** say "file is corrupt". |
| `extensionsRequired` contains anything else unknown | Fail naming the extension(s) verbatim. |
| `KHR_mesh_quantization` present | Either decode it properly (cgltf/fastgltf do) or refuse — never silently import at the wrong scale. |
| `EXT_meshopt_compression` present | Refuse with a named message in v1/v2. |
| Only points/lines primitives | *"This file contains no printable surfaces (only points or lines)."* |
| Buffer URI points outside the file (`.gltf` with sidecar `.bin`) | Resolve relative to the `.gltf` directory only; reject absolute paths and `..` traversal, and reject `data:` URIs above a size cap. Same posture as the OBJ/MTL sibling-file handling (`OBJ.cpp:44-58`). |
| Very large asset | Honour the existing `ImportstlProgressFn`-style progress/cancel contract so the `ProgressDialog` in `Plater::priv::load_files` can cancel. |

### 4.7 Tests

Mirror `tests/libslic3r/test_stl.cpp` exactly — Catch2 `SCENARIO`/`GIVEN`/`WHEN`/`THEN`, data
addressed through the `TEST_DATA_DIR` compile definition (`tests/CMakeLists.txt:4-21`), e.g.:

```cpp
static inline std::string gltf_path(const char* p) { return std::string(TEST_DATA_DIR) + "/test_gltf/" + p; }
```

Register the new source in `tests/libslic3r/CMakeLists.txt` alongside `test_stl.cpp`.

Cases worth having:

1. `.glb` and the equivalent `.gltf` + `.bin` produce identical geometry.
2. A 20 mm box: `model.objects.front()->volumes.front()->mesh().size()` is `Vec3d(20,20,20)`
   after import — i.e. **the Y-up→Z-up rotation and the unit rule are both asserted**. This is the
   single most valuable test.
3. Multi-primitive mesh → expected number of `ModelVolume`s with expected names.
4. Nested nodes with TRS → world positions match hand-computed values.
5. Non-indexed primitive, TRIANGLE_STRIP and TRIANGLE_FAN each produce the right triangle count.
6. Draco-compressed file → `load_gltf` returns false with a message containing "Draco".
7. Skinned/animated file loads (bind pose) without error.
8. `COLOR_0` file → `GltfInfo::vertex_colors` populated, size == vertex count.
9. Path with non-ASCII characters (the STL suite already guards this at `test_stl.cpp:15-20`).

**Sample assets:** the Khronos **glTF-Sample-Assets** repository,
<https://github.com/KhronosGroup/glTF-Sample-Assets> — the repo itself is CC-BY-4.0 and each model
carries its own licence in its README (many are CC0, some CC-BY; **check per model before
vendoring** and record attribution). Useful directories seen in `Models/`: `Box`, `Cube`,
`BoxInterleaved`, `BoxTextured`, `BoxVertexColors` (COLOR_0), `MultipleScenes`,
`NodePerformanceTest`, `MultiUVTest`, `MeshoptCubeTest`. For Draco specifically, generating a small
`.glb` with `gltf-transform draco` (<https://gltf-transform.dev/modules/functions/functions/draco>)
gives a self-authored fixture with no licence question at all — preferable for test data.

Keep fixtures small; `tests/data/` is already 25+ OBJ files and nobody wants a 50 MB test asset.

---

## 5. Effort, risk, phases

### 5.1 Phases

**v1 — geometry only.** `.glb` + `.gltf` load as a single object with one part per primitive;
correct Z-up, node hierarchy honoured, 1 unit = 1 mm with the existing "too small" rescue prompt;
registered in the file dialog, drag-and-drop, CLI, phone upload, macOS document types. Materials
parsed but only used to name parts. Draco/meshopt/quantization refused by name.

**v2 — colours → filaments.** Per-primitive `baseColorFactor` and optional `COLOR_0` feed the
existing `ObjColorDialog`; generalise `obj_import_face_color_deal` to multi-volume; result becomes
MMU painting. Texture presence reported, not used.

**v3 — compressed and textured.** Draco decode (either `ASSIMP_BUILD_DRACO` or a
`deps/Draco/Draco.cmake` lifted from OrcaSlicer, tag 1.5.7), optional meshoptimizer for
`EXT_meshopt_compression`, and texture→per-face colour sampling.

### 5.2 Engineer-days (for *this* codebase, including build plumbing on Windows/macOS/Linux, tests
and a round of review)

| Phase | Route B (cgltf, `deps_src/`) | Route A (Assimp, `deps/`) |
|---|---|---|
| v1 geometry | **6–9 d** — reader ~600-900 lines (scene walk, strip/fan, accessors incl. sparse + quantization, transform composition) 4–6 d; registration checklist 1 d; tests + fixtures 1–2 d | **5–8 d** — port `AssimpImport` 1–2 d, `Format/GLTF.cpp` adapter 2–3 d, **deps build + CI on 3 platforms 2–3 d** (the real cost; every dev and CI runner must rebuild `deps/`) |
| v2 colours | **4–6 d** — generalise the two `*_color_deal` functions and their tests 2 d; wire `ObjColorDialog` and widen the two `.obj` guards 1 d; linear→sRGB and cluster-count UX 1–2 d; tests 1 d | same, **4–6 d** |
| v3 Draco + textures | **6–10 d** — Draco dep + wiring 3–4 d; meshopt 1 d; texture sampling + image decode 2–4 d; tests 1 d | **5–9 d** — flip `ASSIMP_BUILD_DRACO` 1–2 d; texture path partly comes with `AssimpImport` |

A v1-only ship is therefore roughly **one to two working weeks**, and v1+v2 (the version that
actually delights the multi-colour audience) roughly **two to three weeks**.

### 5.3 Risks

| Risk | Severity | Notes / mitigation |
|---|---|---|
| **Security: binary parser behind the LAN upload endpoint** | **High** | `RemoteHub::spool_upload` (`RemoteHub.cpp:1050`) writes an attacker-controllable file which is then parsed. Assimp has a long history of parser CVEs; cgltf is smaller but not audited either. Mitigations: fuzz the new reader; cap decompressed sizes; consider *not* adding `.glb` to the hub allow-list in v1; keep the reader free of `new[]`-sized-by-file-header patterns. |
| Units guessed wrong | Medium | §2.1. Whichever default we pick, half the corpus is "wrong". The existing rescue prompts make it recoverable in one click; document the choice in the release notes. |
| Non-manifold scanner/AI meshes | Medium | Expect a much higher rate of `RepairedMeshErrors` (`TriangleMesh.hpp:19-44`) than with STL, and "Fix Model" is Windows-only (`Plater.cpp:25050` even says so). Not a blocker, but support load will rise. |
| Binary size / build time (Route A) | Medium | Assimp with 3 importers is still several MB and adds minutes to a cold deps build for every developer and CI job. Route B adds one header. |
| Huge assets | Medium | Multi-hundred-MB GLBs exist (photogrammetry). Need the progress/cancel plumbing wired from the start, and a sane cap. |
| Draco decode cost | Low–Medium | Draco decoding is roughly linear and typically **tens of milliseconds to a couple of seconds** for print-scale meshes — negligible next to slicing. The cost is the ~226 MB source dependency and its build time, not runtime. |
| Licence | **Low** | AGPL-3.0 repo; MIT / BSD-3 / Apache-2.0 deps are all compatible. Only obligation is retaining notices. Sample assets are the sharper edge — check per-model licences in glTF-Sample-Assets, or author fixtures ourselves. |
| Colour fidelity | Low | glTF `baseColorFactor` is **linear**; OBJ `Kd` and our filament swatches are sRGB-ish. Without conversion imported swatches will look wrong. One line, easy to forget. |
| Upstream divergence | Low | If OrcaSlicer later promotes glTF to a first-class import format, our `Format/GLTF.cpp` may conflict with a future `AssimpImport` merge. Keeping the public API shaped like `Format/OBJ.hpp` limits the blast radius. |

### 5.4 Open questions for the user

1. **One object with parts, or one object per node?** Recommendation above is *one object, one part
   per primitive* (matches Cura's grouping behaviour and the scanner/AI use case). The alternative
   — one `ModelObject` per top-level node — suits "a scene of several props" but produces a
   confusing plater for the common single-thing case. A middle path: import as one object, and
   surface the existing "Split to objects" action in the import notification. Which do you want as
   the default?
2. **Units: 1 unit = 1 mm (pragmatic) or ×1000 per spec?** §2.1 recommends mm plus the existing
   rescue prompt. Confirm.
3. **Should material colours auto-assign filaments, or always ask?** OBJ always pops
   `ObjColorDialog`. For a 2-material GLB that dialog is friction; for a 30-material one it is
   essential. Proposal: auto-assign silently when the distinct-colour count ≤ the configured
   filament count *and* every colour matches an existing filament within a tolerance (the panel
   already has `find_filament_selection_by_color`), otherwise show the dialog. Confirm.
4. **Do we want `.fbx` too?** It is nearly free under Route A and impossible under Route B. If FBX
   is wanted, that alone decides the library choice.
5. **Should `.glb` be accepted by the phone-upload endpoint in v1?** See the security row above.
6. **Is `.drc` (standalone Draco) interesting?** OrcaSlicer supports it as a first-class import
   format and the code is small and directly liftable — but it is a niche format and it pulls the
   full Draco dependency in for phase 1 rather than phase 3.

---

## 6. Reference index

Codebase (this tree, 2026-09-02):

* `src/libslic3r/Model.cpp:229`, `:265-325`, `:337-352` — `read_from_file` and its dispatch
* `src/libslic3r/Model.cpp:849`, `:877`, `:929`, `:954`, `:969`, `:981` — multipart and unit helpers
* `src/libslic3r/Model.cpp:47-49`, `:3140`, `:3148`, `:3270` — filament table and colour→painting
* `src/libslic3r/Model.hpp:806-820`, `:882`, `:1016`, `:1584`, `:1596`, `:1607-1608`, `:1618`
* `src/libslic3r/Format/OBJ.hpp:10-28`; `Format/OBJ.cpp:24`, `:79`, `:135`, `:204`
* `src/libslic3r/Format/STL.cpp:17-40`; `Format/AMF.cpp:301`, `:374`; `Format/STEP.hpp`
* `src/libslic3r/CMakeLists.txt:192-212`, `:550-584`, `:586-604`
* `src/slic3r/GUI/GUI_App.hpp:103-125`; `GUI_App.cpp:652-677`, `:683`, `:2279-2287`, `:4321`,
  `:7556`, `:7581`
* `src/slic3r/GUI/MainFrame.cpp:2694-2703`
* `src/slic3r/GUI/Plater.cpp:9972`, `:10611`, `:11528`, `:11548`, `:11633-11637`, `:12222-12230`,
  `:12232-12278`, `:12279-12298`, `:12311`, `:12369-12386`, `:12405-12414`, `:12432`, `:12450`,
  `:12474`, `:12513`, `:12669`, `:14678-14709`, `:20063`
* `src/slic3r/GUI/ObjColorDialog.hpp:20-116`
* `src/slic3r/GUI/RemoteHub.cpp:1050`, `:1061-1065`; `resources/web/orca/stream_center.html:1161`
* `src/Snapmaker_Orca.cpp:1620-1654`
* `src/dev-utils/platform/osx/Info.plist.in:40+`
* `deps/CMakeLists.txt:113-211`, `:366`, `:369-390`; `deps/OCCT/OCCT.cmake`
* `deps_src/CMakeLists.txt`; `deps_src/nlohmann/CMakeLists.txt`; `deps_src/fast_float/CMakeLists.txt`
* `tests/CMakeLists.txt:4-21`; `tests/libslic3r/CMakeLists.txt`; `tests/libslic3r/test_stl.cpp:8-20`;
  `tests/test_utils.hpp`
* `LICENSE.txt` (AGPL-3.0)

External (all accessed 2026-09-02):

* glTF 2.0 spec — <https://raw.githubusercontent.com/KhronosGroup/glTF/main/specification/2.0/Specification.adoc>
* Khronos glTF-Sample-Assets — <https://github.com/KhronosGroup/glTF-Sample-Assets>
* OrcaSlicer `Format/` listing — <https://api.github.com/repos/OrcaSlicer/OrcaSlicer/contents/src/libslic3r/Format>
* OrcaSlicer `AssimpImport.cpp` — <https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer/main/src/libslic3r/Format/AssimpImport.cpp>
* OrcaSlicer `Format/DRC.hpp` — <https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer/main/src/libslic3r/Format/DRC.hpp>
* OrcaSlicer `deps/` listing — <https://api.github.com/repos/OrcaSlicer/OrcaSlicer/contents/deps>
* OrcaSlicer `deps/Assimp/Assimp.cmake` — <https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer/main/deps/Assimp/Assimp.cmake>
* OrcaSlicer `deps/Draco/Draco.cmake` — <https://raw.githubusercontent.com/OrcaSlicer/OrcaSlicer/main/deps/Draco/Draco.cmake>
* OrcaSlicer import/export wiki — <https://github.com/OrcaSlicer/OrcaSlicer/wiki/import_export>
* OrcaSlicer issue #3926 (GLB/GLTF request, closed not-planned) — <https://github.com/OrcaSlicer/OrcaSlicer/issues/3926>
* Bambu Studio `Format/` listing — <https://api.github.com/repos/bambulab/BambuStudio/contents/src/libslic3r/Format>
* Bambu Studio 2.7.0 beta release notes — <https://github.com/bambulab/BambuStudio/releases/tag/v02.07.00.55>
* Bambu Lab wiki, importing textured models — <https://wiki.bambulab.com/en/software/bambu-studio/import_obj>
* Cura `TrimeshReader.py` — <https://github.com/Ultimaker/Cura/blob/main/plugins/TrimeshReader/TrimeshReader.py>
* PrusaSlicer supported formats — <https://help.prusa3d.com/article/supported-file-formats_1772>
* tinygltf — <https://github.com/syoyo/tinygltf>
* cgltf — <https://github.com/jkuhlmann/cgltf>, releases <https://github.com/jkuhlmann/cgltf/releases>
* fastgltf — <https://github.com/spnda/fastgltf>, releases <https://github.com/spnda/fastgltf/releases>, overview <https://fastgltf.readthedocs.io/latest/overview.html>
* Assimp — <https://github.com/assimp/assimp>, licence <https://raw.githubusercontent.com/assimp/assimp/master/LICENSE>, `ASSIMP_BUILD_DRACO` <https://github.com/assimp/assimp/blob/master/Build.md>
* Draco — <https://github.com/google/draco>
* meshoptimizer — <https://github.com/zeux/meshoptimizer>
* gltf-transform `draco` function — <https://gltf-transform.dev/modules/functions/functions/draco>
