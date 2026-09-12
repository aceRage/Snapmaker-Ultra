# CAD-like mesh editing mode ("Edit" gizmo) — research

Status: research only, no code. Would branch `feat/cad-edit-mode` off `origin/feat/ultra-preferences`.

## Recommendation

**Build a mesh-based `Edit` gizmo on code already in this tree; add no new geometry library for phases 1-3.** Every primitive the brief asks for is
already written here:

| Need | Already in tree |
|---|---|
| Planar face region + ordered border loops | `MeasuringImpl::update_planes` (`Measure.cpp:135-296`) |
| Smooth region grown from a click by dihedral angle | `MeasuringImpl::grow_curve_patch` (`Measure.cpp:538-561`) |
| Half-edge traversal of an `indexed_triangle_set` | `SurfaceMesh` (`SurfaceMesh.hpp:54-96`) |
| Global edge ids + facet adjacency (edge chains) | `its_face_edge_ids` / `its_face_neighbors` (`TriangleMesh.hpp:193-201`) |
| Highlighting an arbitrary facet list | `init_plane_data` (`GLModel.cpp:1477`), used at `GLGizmoMeasure.cpp:564` |
| Undo-per-op, mesh commit, painted-data rules | `GLGizmoSculpt.cpp:474-513` (path a), `:1020-1055` (path b) |
| Self-intersection guard | `MeshBoolean::cgal::does_self_intersect` (`MeshBoolean.hpp:64`) |
| Morphological offset (level set) | `OpenVDBUtils.hpp:29-44`, as used by `SLA/Hollowing.cpp:66-103` |

**The one genuinely new algorithm is the edge bevel** (phase 2). Neither this tree nor *any* permissively-licensed library will do it for us (§2) — it
must be written. That is the honest cost, and it is why phase 1 excludes it.

**Phase 1 (selection + face push/pull + undo) is 6-9 days**, needs no new dependency and no topology surgery, and — because translating a face changes
no indices — **preserves all painted data** (supports, seam, MMU, fuzzy skin), the best reason to scope phase 1 this way.

**OCCT is already a dependency** (7.6.0, `LGPL-2.1-only WITH OCCT-exception-1.0`), but its fillet/chamfer/feature toolkits are **not built**, and —
the real blocker — STEP import **discards the B-rep**. Enabling the toolkits is half a day; making them useful is weeks. See §4.

## 1. What already exists in this tree
### 1.1 Planar face regions — `Measure.cpp`, the load-bearing reuse

`MeasuringImpl::update_planes` (`Measure.cpp:135-296`), once on construction: caches `its_face_normals`/`its_face_neighbors` (`:141-142`); flood-fills
facets into `PlaneData` regions, membership decided by `is_same_normal` — per-component tolerance `0.001` against the **seed** normal (`:147-149`),
i.e. strictly coplanar, not a per-step dihedral; fills `m_face_to_plane[facet] -> region id` (`:168-181`); then under `tbb::parallel_for` walks each
region's **border** with `SurfaceMesh` half-edges (`:200-296`), producing `PlaneData::borders` as ordered loops, with `goto PLANE_FAILURE` bailouts on
broken input (`:218`, `:247`, `:262`, `:267`, `:279`). `PlaneData` (`:86-91`) holds `facets`, `borders`, `normal`, `area`; the public API is
`get_num_of_planes()` (`Measure.hpp:147`), `get_plane_triangle_indices(idx)` (`:150`) and the hover entry `get_feature(...)` (`Measure.hpp:144`, impl
`:774`), already used by the Measure and Assembly gizmos.

Two properties make this decisive: **`m_face_to_plane` turns a raycast facet hit straight into a whole coplanar region** (push/pull selection, free),
and **an edge whose two sides lie in different regions is by definition a feature edge** — the border walk already *is* feature-edge extraction.
Limitation: the strict seed-relative tolerance will **not** grow across a tessellated cylinder or a noisy scanned plane — hence §1.2.
### 1.2 Smooth-region growing, and the snap-radius trap

`grow_curve_patch(seed_facet)` (`Measure.cpp:538-561`), an Ultra addition with no upstream equivalent: DFS over `m_face_neighbors` admitting neighbour
`u` from facet `t` when `n_u . n_t > cos(8 deg)` **and** `n_u . n_seed > cos(20 deg)`, capped at 20000 facets — exactly the "smooth region grown from
a click" the brief asks for, both guards already tuned. **Start face selection here, not on stock `update_planes()`.**

`facet_snap_extent` (`:570-580`) caps a view-scaled snap radius at `snap_facet_fraction = 0.25` (`:41`) of the hit facet's shortest edge. Its comment
records a real Ultra bug: without the cap, on a part small on screen, every hover — even dead centre of a flat face — was claimed by a corner vertex
or border edge. **An Edit gizmo offering both edge and face picking hits this exact bug**; reuse the cap.
### 1.3 Sculpt gizmo — undo, commit, painted-data discipline

Two commit paths, and which one an operation takes is the central design question.

**(a) Vertices-only, topology preserved** — `end_stroke()` (`GLGizmoSculpt.cpp:474-513`): `TakeSnapshot(plater, _u8L("Sculpt"),
SnapshotType::GizmoAction)` on button-up before the volume is touched, one undo step per stroke (`:486`); then `commit_sculpted_mesh(...)` (`:491`)
with **deliberately no `clear_before_change_mesh()`** (`:489-490`) — indices untouched, so every painted annotation (`supported_facets`,
`seam_facets`, `mmu_segmentation_facets`, `fuzzy_skin_facets`) stays valid; contract at `MeshSculpt.hpp:6-14`. Then `changed_mesh()` (`:495`) and
`rebuild_tree()` (`:503`).

**(b) Topology-changing** — Subdivide (`:1020-1055`): `TakeSnapshot` (`:1026`), then **`clear_before_change_mesh()`** (`:1029`) because subdivision
renumbers every facet "exactly the way the Simplify gizmo and 'Repair by remeshing' handle it" (`:1027-1028`); then `set_mesh`,
`calculate_convex_hull`, `set_new_unique_id`, `ensure_on_bed` (`:1043-1049`). `GLGizmoSimplify` matches (`:538`, `:552`).

**Consequence:** push/pull-by-translation only moves vertices -> path (a), **painted data kept**. Bevel, extrude and inset insert geometry -> path
(b), painted data dropped. Raycasting goes through `m_c->raycaster()->raycasters()[m_mesh_id]->unproject_on_mesh(...)` (`:241-254`), returning hit
point, normal and **facet index**; `on_get_requirements()` returns `SelectionInfo | Raycaster` (`:108`).

### 1.4 Other reusable pieces

**Curved cut** (`CurvedCut.hpp:12-26`) records the instinct worth copying: the cut sheet is a single-valued height field *precisely so it is
self-intersection-free by construction*, and its zero case degrades exactly to the flat cut — a testable invariant. Hold the same line: bevel radius 0
and push distance 0 must be provably the identity. **Booleans** (`MeshBoolean.hpp`): `cgal::{plus,minus,intersect}` (`:52-62`) with
`does_self_intersect` (`:64-65`), `mcut::` (`:75-96`) and `mfd::` (Manifold 3.5.2, the Mesh Boolean gizmo's primary backend) — **`does_self_intersect`
is the ready-made push/pull guard**. **Level-set offset**: `OpenVDBUtils.hpp:29-44` wraps the mesh -> level set -> mesh round trip used by SLA
hollowing, which already performs a signed offset via `redistance_grid(*gridptr, -(offset + D), ...)` (`Hollowing.cpp:88`) — **a working, in-tree
morphological offset** (see §3.3). **CGAL exposure** (licence-relevant): `MeshBoolean.cpp:16-29` already includes **Polygon Mesh Processing**
(`corefinement.h`, `repair.h`, `remesh.h`, `orientation.h`, …) plus `Surface_mesh.h`, so the project **already ships CGAL PMP** and using more of it
adds no new licence exposure — see §2.

### 1.5 OCCT presence — exact status

**Version 7.6.0** (`deps/OCCT/OCCT.cmake`), Shared on Windows. **Licence, verified from the unpacked source**: SPDX **`LGPL-2.1-only WITH
OCCT-exception-1.0`** — LGPL-2.1-**only**, not "or later"; do not write "LGPL-2.1+". The exception lets object code incorporate material from OCCT
**header files** under terms of your choice, given prominent notice that the work uses OCCT; read narrowly, it waives only the LGPL §5 inlined-header
problem and does **not** grant general static-linking freedom. **AGPL compatibility does not depend on it**: LGPL §6 is satisfied by dynamic linking,
which Windows already does (DLLs copied at `CMakeLists.txt:826-855`), and LGPL §3 conversion to GPL-3 would also reach AGPL §13. Either way **add the
required attribution notice** — check whether the about/licence screen already carries it.

Linked toolkits (`src/libslic3r/CMakeLists.txt:579-605`): 26 STEP/XCAF/topology toolkits incl. `TKBO TKPrim TKTopAlgo TKGeomAlgo TKBRep TKMesh
TKShHealing`. In-tree users: `Format/STEP.cpp`, `Shape/TextShape.cpp` (`:153`), `Format/svg.cpp` (`:288`).

**Gap 1 — the modelling toolkits are not built.** `deps/OCCT/OCCT.cmake` passes `-DBUILD_MODULE_ModelingAlgorithms=OFF`; the install ships 38 DLLs
including `TKBO TKBool TKTopAlgo TKPrim TKGeomAlgo TKMesh TKShHealing` — **but not `TKFillet`, `TKOffset`, `TKFeat`**. Per OCCT's `adm/MODULES` those
belong to `ModelingAlgorithms`; most of it builds anyway because the STEP toolkits pull it in transitively, and these three fall out because nothing
else needs them. Cost to enable, measured from the source: `TKFillet` = 9 packages (incl. `BRepFilletAPI`), `TKOffset` = 4 (incl. `BRepOffsetAPI`),
`TKFeat` = 2 (`LocOpe`, `BRepFeat`); their combined `EXTERNLIB` set is `TKBRep TKernel TKMath TKGeomBase TKGeomAlgo TKG2d TKG3d TKTopAlgo TKBool
TKShHealing TKBO TKPrim` — **every one already built and shipped**, so enabling them adds **no new transitive dependency**: 15 packages of compile
time, 3 DLLs in the copy list, 3 names in `OCCT_LIBS`.

**Gap 2 — the real one: no B-rep survives import.** `Format/STEP.cpp` tessellates with `BRepMesh_IncrementalMesh` (`:290`, `:475`, `:502`) and hands
only a `TriangleMesh` to `add_volume(std::move(triangle_mesh))` (`:393-395`); the `TopoDS_Shape` is a local and is discarded. Nothing exists for
`BRepFilletAPI` to operate on today. See §4.

## 2. Library survey — licence and actual capability

**Licence spine:** AGPL-3.0 §13 ¶2 explicitly permits combining an AGPLv3 work with **GPLv3** code, so GPLv3 CGAL is fine (already the status quo,
§1.4). Apache-2.0 -> AGPLv3 is one-way OK per the ASF; GPL-2.0-**or-later** can be elected to v3 (GPL-2.0-**only** could not); MPL-2.0, BSD-3 and MIT
are all fine.

**Headline: no library implements bevel, chamfer, inset or face push/pull.** Confirmed by source grep across libigl, OpenMesh, PMP-library, CGAL PMP,
Geogram, MeshLib, FreeCAD Mesh and MeshLab. All three operations are net-new work on half-edge primitives: libraries buy the kernel, traversal,
feature-edge *seeding* and repair/remesh cleanup — never the operators. The part **nobody** implements is corner blending where 3+ bevelled chains meet.

| Library | Licence | AGPL-ok | Capability | Verdict |
|---|---|---|---|---|
| **libigl** 2.6.3 | MPL-2.0 core; `igl/copyleft/` GPL-encumbered (holds `cgal/`, all booleans) | yes | `sharp_edges` (dihedral), `edge_flaps`, `triangle_triangle_adjacency`, `boundary_loop`, `arap`. **`igl::bevel` does not exist** | Skip: those are ~50 lines each over structures we already cache |
| **CGAL** 5.4 (in tree) | PMP, Surface_mesh, AABB = **GPLv3+**; halfedge DS, kernels = LGPL | yes — already shipped | `detect_sharp_edges`, **`region_growing_of_planes_on_faces`**, `remesh_planar_patches`, `isotropic_remeshing`. **`extrude_mesh` asserts `!is_closed(input)`** — it turns an *open* surface into a solid, so a closed STL trips the assertion; **it is not face push/pull**. No bevel | Already a dep; the region-growing calls are tolerant alternatives if §1.1's strict-normal limit bites |
| **Manifold** 3.5.x (in tree) | Apache-2.0 | yes | `LevelSet(sdf, …)`, `MinkowskiSum/Difference`, `Revolve`, `Hull`. **`Manifold::Offset` does not exist**; `Extrude` is a 2D-profile sweep, not face push/pull | Keep for booleans; Minkowski is a valid but costlier global-rounding route than OpenVDB |
| **OpenVDB** — **ours is 8.2.0, MPL-2.0** (pinned fork commit; upstream went Apache-2.0 at 12.0.0) | MPL-2.0 | yes | Round trip already wrapped in-tree. **Our 8.2.0 `LevelSetFilter` has `offset(v, mask)`, `meanCurvature(mask)`, `laplacian`, `gaussian/median/mean` — all taking an optional alpha mask, so *localised* offsetting works today.** Upstream's dedicated `fillet(mask)` is **12.0.0-only, verified absent from our pinned header**, and rounds only *concave* edges | **Best non-bevel option** via `offset(+r)`/`offset(-r)` (§3.3). Do not assume `fillet()` |
| **OpenMesh** 4.0+ / **PMP-library** | BSD-3-Clause (<=3.3 was LGPLv3) / MIT | yes | Half-edge + properties, remeshing, decimation, subdivision; OpenMesh's status bits (`SELECTED`/`FEATURE`) are the best selection substrate of the candidates; PMP's `detect_features(mesh, angle)` only sets per-edge flags and does **not** chain them into loops. **Zero grep matches for bevel/chamfer/inset/extrude in either** | Neither is worth a new dependency — `SurfaceMesh` already gives traversal and a `vector<char>` gives flags |
| **MeshLib** | **Proprietary** non-commercial/education licence; no SPDX | **NO** | (voxel offsets only; no fillet/bevel) | **Excluded** — not redistributable in an AGPL binary |
| **Blender bmesh bevel** | GPL-2.0-**or-later** | legally yes, practically no | `bmesh_bevel.cc`, ~8,500 lines deeply coupled to BMesh ngons/loop-customdata | **Algorithm reference only** (below); porting is not a shortcut |
| **Instant Meshes / QuadriFlow**; **Geogram / MeshLab / FreeCAD / Dust3D** | BSD-3 (+ non-standard clause) / BSD-3 / GPL-3 / LGPL-2.1+ / MIT | yes | Quad remeshing (both unmaintained since 2019); no mesh bevel in any of the rest. **FreeCAD's Part fillet is literally `#include <BRepFilletAPI_MakeFillet.hxx>`** — it is OCCT | Quad remeshers **out of scope**: loops follow principal curvature, not design features, and both *approximate*, so output no longer matches the imported part to tolerance |
| **OCCT** 7.6.0 (in tree) | `LGPL-2.1-only WITH OCCT-exception-1.0` | yes (dynamic link) | Real B-rep `BRepFilletAPI_MakeFillet`/`MakeChamfer`, `BRepFeat_MakeDPrism` | The only true fillet kernel — see §4 |

**No permissively-licensed standalone C++ mesh bevel/inset/extrude exists.** Worth stating plainly.

**Blender's bevel as reference** (algorithm only, no code copied): `bmesh_bevel.cc` builds a per-vertex "vmesh" and per-edge strips of `segments`
quads on a superellipse profile (0.5 = circular arc), joining strips inside each vertex's vmesh — which is why vertex handling dominates. The more
valuable lesson is a failure: the author's **first version solved widths greedily and depth-first and was scrapped because it was order-dependent** —
topologically identical parts got different widths. The rewrite solves all widths **globally, before building any geometry**.

## 3. Design: a mesh-based `Edit` gizmo

New `GLGizmoEdit`, registered as Sculpt is: an `EType::Edit` entry (`GLGizmosManager.hpp:93`), construction alongside `GLGizmosManager.cpp:226` with
`toolbar_edit.svg`/`toolbar_edit_dark.svg` (the `_dark` pairing is the convention), membership in the mouse-capturing list (`:663`) and `get_name`
(`:1529`). Backing module `src/libslic3r/MeshEdit.{hpp,cpp}` mirrors `MeshSculpt.{hpp,cpp}`: an `EditSession` owning the working
`indexed_triangle_set`, cached topology and normals, and the AABB tree.

### 3.1 Selection

**Face region.** The raycast returns `(hit, normal, facet_idx)` (`GLGizmoSculpt.cpp:253`). Then either *planar mode* — `m_face_to_plane[facet_idx]`
then `get_plane_triangle_indices(region)` — or *smooth mode* — `grow_curve_patch(facet_idx)` with both thresholds exposed. Both yield a
`std::vector<int>` facet list, exactly what the highlight renderer consumes.

**Feature-edge chain.** Once per session, `its_face_edge_ids(its, face_neighbors)` (`TriangleMesh.hpp:197`) gives a global edge id per (facet, side).
For an edge with incident facets `f0, f1` the dihedral is `acos(clamp(n_f0 . n_f1))`; it is a **feature edge** above a threshold (default 30 deg).
Build `vertex -> incident feature edges`. A *chain* from a seed edge is a walk: at each endpoint, if **exactly one** other feature edge is incident
and turns by less than a continuation threshold (default 35 deg), extend; else stop. Vertices with three or more incident feature edges are
**corners** and terminate the chain — that is what makes a chain on a cube's top face close into the 4-edge loop instead of leaking down the sides.
Hovering any edge highlights the whole chain. **Loop selection** (phase 3) is the same walk with continuation replaced by "go straight across".

**Rendering.** Regions: `init_plane_data(its, facet_list, normal_offset)` (`GLModel.cpp:1477`), drawn as `GLGizmoMeasure::init_plane_glmodel` does
(`GLGizmoMeasure.cpp:545-566`) with the same `MEASURE_PLNE_NORMAL_OFFSET` z-fight offset; chains as a line-strip or the Measure gizmo's
cylinder-per-segment approach (`:195-197`, `:943`). Hover vs selected = two colours, per `render_glmodel(..., hover)` (`:878`).

### 3.2 Push/pull of a planar face region — phase 1

Region facet list `F`, normal `n` (area-weighted mean; exact for a planar region), distance `d`. `V_int` = vertices used **only** by `F`'s facets;
`V_bnd` = border vertices shared with non-`F`.

**Translate (default, topology-preserving).** Move `V_int` and `V_bnd` by `d*n`. Neighbouring facets sharing a border vertex stretch to follow: a
cube's top face pushed up 5 mm makes the four walls 5 mm taller. No index changes -> **path (a)**, painted data survives. *Caveat:* where side walls
are not parallel to `n` (a tapered face), this shears them rather than lengthening them; "move border vertices along their adjacent-face planes" is
phase 3.

**Extrude (topology-changing).** Duplicate the border loop: for each border vertex `v` create `v' = v + d*n`; rewire `F`'s facets to `v'`; stitch the
wall with two triangles per border edge `(a,b)`: `(a,b,b')` and `(a,b',a')`. Winding follows the border-loop orientation `update_planes` already
produces, so walls face outward for `d > 0` (boss); reversed gives a pocket. **Path (b).**

**Self-intersection guard.** Before commit, `does_self_intersect(...)` (`MeshBoolean.hpp:64`); on a positive, refuse and notify. Cheap pre-checks
first: reject a `d` pushing past the far side of the bounding box; clamp negative `d` at local thickness.

### 3.3 Bevel/chamfer of an edge chain — phase 2, the real work

Per chain: (1) **offset the two incident sides** — for edge `e=(a,b)` with facets `f0,f1`, `a0 = a + r*t0`, `a1 = a + r*t1`, `t0` the in-plane unit
normal to `e` pointing into `f0`; (2) **cross-section** — chamfer joins `a0`-`a1` straight, fillet interpolates `N` segments along the arc tangent to
both faces (centre at `r / sin(theta/2)` along the inward bisector); `N = 1` reproduces the chamfer exactly, the degeneracy test; (3) **re-cut the
incident facets** against the offset line in their own plane, discarding the sliver; (4) **re-triangulate the strip** as a quad strip through the
`N+1` rings; (5) **corners** — where chains meet, strips overlap, so fill the spherical polygon formed by the arriving end-rings. First
implementation: **valence-3 via triangle fan**, **refuse valence >= 4 with a clear message** rather than emit garbage.

**Take from prior art (§2): solve all widths globally before building geometry, never greedily.** Blender's first bevel was scrapped for exactly that
order-dependence, and this repo already guards determinism (`tests/determinism_1.gcode` / `determinism_2.gcode`).

**Guards:** clamp `r` to `0.5 * min(adjacent edge length)` over the chain, showing the clamped value; skip non-manifold edges and report, as
`update_planes` bails to `PLANE_FAILURE`; finish with `its_remove_degenerate_faces` (`TriangleMesh.hpp:212`), `its_merge_vertices` (`:209`) and
`its_compactify_vertices` (`:215`); drop near-flat edges. Always path (b); always require `its_num_open_edges(result) == 0` (`:232`).

**Global alternative — "Round all edges by r".** `mesh_to_grid` -> `redistance_grid(+r)` -> `redistance_grid(-r)` -> `grid_to_mesh` is a morphological
closing that rounds concave edges by ~`r` (reverse order rounds convex ones). All three already exist and SLA hollowing already uses them this way —
~40 lines, no bevel algorithm. Costs: **global**, **remeshes everything** (original triangulation and all painted data destroyed), resolution-limited
(`r` below ~2 voxels unreliable). Our OpenVDB 8.2.0 filters **do** take an alpha mask, so a *localised* variant restricted to a selected region is
possible without a version bump. Ship as a separate, clearly-labelled command — never as the edge-chain bevel.

### 3.4 Follow-ups (phase 3) and the commit matrix

**Inset** is the extrude construction with the new loop displaced along the in-plane inward bisector; **move face in plane** and **scale face loop** are
topology-preserving (path a); **draft/taper** rotates the region about a border edge.

| Operation | Changes indices? | Commit path | Painted data |
|---|---|---|---|
| Face push/pull (translate), move in plane, scale loop | no | `commit_sculpted_mesh` | **kept** |
| Extrude / inset | yes | `clear_before_change_mesh` | dropped |
| Edge bevel / chamfer | yes | `clear_before_change_mesh` | dropped |
| Round all edges (level set) | yes (remesh) | `clear_before_change_mesh` | dropped |

One `TakeSnapshot(_u8L("Edit"), SnapshotType::GizmoAction)` per applied operation, before the volume is touched (`GLGizmoSculpt.cpp:486`). The drag
preview must **not** snapshot per tick — preview in the render volume, commit once on release.

## 4. The OCCT track (optional; the trap is not the licence)

On paper this is the "just use the CAD kernel" answer: `BRepFilletAPI_MakeFillet` (`Add(radius, edge)`, plus two-radius and `Law_Function`
variable-radius overloads), `BRepFilletAPI_MakeChamfer` (`Add(dist, edge)`, `AddDA`) and `BRepFeat_MakeDPrism` for a real push/pull boss or pocket —
mature, and correct on curved geometry where a mesh bevel only approximates. **Enabling the toolkits is cheap** (§1.5): about half a day.

**Making them useful is not**, because the slicer has no B-rep to edit. `Format/STEP.cpp` discards the `TopoDS_Shape`, so editing means **keeping**
one per `ModelVolume` — and it must **survive save/load**: 3MF stores meshes, so keeping a B-rep means embedding the STEP or a `BRepTools::Write`
serialisation as an extra 3MF part, plus a rule for when mesh and B-rep disagree. They will disagree the moment any existing tool (cut, boolean,
sculpt, simplify) touches the volume, so every such tool must invalidate the B-rep or learn to use it. Every edit also **re-tessellates**, renumbering
facets, so painted data dies anyway; selection must map mesh facet -> `TopoDS_Face` (tractable, but real work in the import path); and it applies
**only to STEP-imported volumes**, so every STL and 3MF gets nothing — a feature that silently does nothing on most models is a support burden.

**The mesh -> sew -> fillet shortcut does not work**, and the spec should say so plainly: `BRepBuilderAPI_Sewing` adds topological connectivity
without changing geometry, so a sewn STL is N planar triangular faces. OCCT then propagates a fillet to all edges in smooth continuity with the picked
one — but on a triangulated shell every shared edge is a real crease, so one visually-single edge becomes hundreds of micro-edges that must solve
simultaneously. Slow, and it fails often. The only honest mesh -> B-rep route is surface refitting, and OCCT's `ShapeAnalysis_CanonicalRecognition` is
a **building block, not a pipeline**.

**Recommendation: do not start here.** Make it a separate later project, worth a spike only once the mesh Edit gizmo has shipped and demand for exact
fillets on imported CAD is proven; the estimate is **dominated by B-rep persistence and mesh/B-rep coherence, not the fillet calls**
(`BRepFilletAPI_MakeFillet` is ~20 lines, keeping a B-rep consistent across the existing mesh tools is weeks). One narrow exception: a **STEP-in /
STEP-out "fillet these edges" utility** that never integrates with the plater sidesteps persistence entirely and is a **2-3 day spike** — a much
smaller feature than the brief, but the cheapest route to real fillets on CAD parts.

## 5. Compatibility: nothing portable exists

No open-source slicer ships an edge-bevel/chamfer or face push/pull gizmo. Every modelling-ish tool in all of them is *volume-level* (boolean, cut,
emboss, simplify), never *sub-object-level*.

- **OrcaSlicer**: AdvancedCut, Assembly, BrimEars, Cut, Emboss, FaceDetector, FdmSupports, Flatten,
  FuzzySkin, Hollow, Measure, MeshBoolean, MmuSegmentation, Move, Rotate, SVG, Scale, Seam, Simplify,
  SlaSupports, Text. No sculpt, bevel or push/pull. The one relevant open request is
  **[#12459 "Chamfer Along Cut"](https://github.com/OrcaSlicer/OrcaSlicer/issues/12459)** (Feb 2026,
  open, unimplemented) — and it asks only for a chamfer *parameter on the existing Cut gizmo*. Useful
  evidence that demand exists and upstream has not moved.
- **BambuStudio**: near-identical plus `GLGizmoAlignment`. Its 2.8.0 "Assembly Guide" splits a model
  into build steps from a STEP file — sequencing that *consumes* CAD data, not mesh editing. Its Mesh
  Boolean is known-flaky ([#7788](https://github.com/bambulab/BambuStudio/issues/7788)), users
  round-tripping through Fusion — more evidence of the gap.
- **PrusaSlicer** (2.9/3.0): Cut with connectors/dovetails, Measure, Emboss, SVG, Simplify, Seam, MMU, supports, SLA
  — no modelling gizmo. **SuperSlicer** diverges only on slicing parameters. **Cura** has none in core;
  [Mesh Tools](https://github.com/fieldOfView/Cura-MeshTools) is analysis/repair and a third-party
  [Boolean plugin](https://github.com/walshj05/Cura-Boolean-Plugin) does CSG — **no bevel or push/pull plugin exists**.
- **Adjacent**: Chitubox, Lychee, IdeaMaker are closed and volume-level. The only tools with true direct-modelling
  gizmos are general 3D apps or [Forma3D](https://forma3d.app/) (iOS, closed), which does exactly extrude/inset/bevel
  on faces and edges — a good **UX reference**, no code.

**Worth borrowing? It already happened.** PrusaSlicer's `Measure.cpp` feature detection (inherited via Bambu) is its own code, no external library,
and is exactly planar-region growing plus feature-edge extraction (§1.1) — and this fork has *already extended* it with `grow_curve_patch` (§1.2),
which has no upstream equivalent and tolerates curvature. **Consequence:** no upstream patch to track, no rebase risk, and no chance an upstream
implementation lands that makes this redundant.

## 6. Tests

Pure geometry in `tests/libslic3r/test_mesh_edit.cpp`, registered beside `test_mesh_sculpt.cpp` (`CMakeLists.txt:51`) and `test_curved_cut.cpp`
(`:20`). Primitives already used by `test_curved_cut.cpp`: `its_volume` (`TriangleMesh.hpp:321`; at `:120`, `:400`) and `its_num_open_edges` (`:232`;
at `:190-191`, `:461`). Follow `test_assembly_face_pick.cpp` for the "build a cube, find a facet on the +Z face" helper.

*Selection* — cube gives exactly 6 planes of 2 facets; feature-edge extraction at 30 deg gives exactly 12 edges, and a chain seeded on any top edge
closes into the 4-edge top loop without descending a side; `grow_curve_patch` from a +Z facet returns exactly those 2 facets. On a tessellated
cylinder the top rim is a closed chain and the side wall's quad-diagonal edges are **not** feature edges at 30 deg.

*Push/pull (phase 1)* — cube 10 mm, push +Z by +5 mm: volume up by `area*5 = 500` mm^3 within 1e-3, `its_num_open_edges == 0`, and **vertex and facet
counts unchanged** — the test that actually protects the painted-data contract. `d = 0` leaves the mesh bitwise identical; `-5` mm drops the volume
500 mm^3; a push far enough to invert makes `does_self_intersect` true and is refused.

*Extrude (phase 3)* — extruding the +Z face by 5 mm stays closed with volume +500 mm^3 and facet count up by exactly 8; by `-d` it stays closed with
volume down the same.

*Bevel (phase 2)* — chamfer one top edge (`r=1`, `N=1`): watertight, volume down by exactly the prism `0.5*r^2*L` (5 mm^3 for `L=10`) within 1e-3.
Fillet the same edge (`N=8`): watertight, volume between the chamfer's and the unbevelled cube's, approaching `(1 - pi/4) r^2 L` as `N` grows. `r=0`
or `N=0` leaves it unchanged; an over-large `r` is clamped and still watertight; a valence-3 corner stays watertight while valence >= 4 is refused
with no mesh change; non-manifold input is refused. Finally **determinism**: two topologically identical parts bevel to identical output — the global
width-solve guard of §3.3.

*Owner click-tests* — Load a cube STL, Edit gizmo, hover the top face (whole face highlights), drag up 5 mm (box taller, walls follow), Ctrl+Z (fully
restored). Paint support-blocker facets on a side wall, push the top face, confirm **the paint survives**; then Extrude and confirm it is dropped
*after a warning*. Hover a top edge, confirm the whole 4-edge loop highlights, bevel it (r = 1 mm, 6 segments) and slice with no errors. On a
tessellated cylinder, hover the rim and confirm one chain. Push a face on an MMU-painted part and confirm the segmentation still renders. Undo/redo
ten mixed operations and confirm one stack entry per operation.

## 7. Phases and estimates

Implementation days for one developer familiar with this codebase, excluding deps rebuilds and review.

**Phase 1 — selection + face push/pull (translate) + undo. 6-9 days.** `MeshEdit.{hpp,cpp}` with `EditSession`, cached topology, region selection
wrapping `Measuring` and `grow_curve_patch` (1.5 d); feature-edge chain extraction and the chain walk, needed for *highlighting* even here (2 d);
`GLGizmoEdit` skeleton — registration, icons, raycast, hover/selection state, panel (2 d); highlight rendering (1 d); push/pull translate with live
preview, self-intersection guard, path-(a) commit (1.5 d); tests and click-tests (1 d). Lands a genuinely useful tool — push a face, keep your paint —
with no new dependency and no topology surgery.

**Phase 2a (optional, take it early) — "Round all edges" via level set. 1-2 days**, mostly UI and warnings since the OpenVDB round trip already exists.
A morale win, and some users only ever wanted "round everything a bit".

**Phase 2 — edge bevel/chamfer with segments. 10-15 days.** Strip construction and incident-face re-cut (4 d); **global width solve** before geometry
(2 d); valence-3 corner patches (4 d — where bevels go wrong); clamps and degeneracy guards (2 d); tests (2 d); UI (1 d). The spread is wide because
corner handling is genuinely hard; refusing valence >= 4 keeps the upper bound at 15, not 25.

**Phase 3 — extrude / inset / loop tools. 5-8 days.** Extrude (2 d; the border-loop stitch is shared with inset), inset (1.5 d), move-face-in-plane
and scale-loop (1.5 d, both path (a)), loop selection (1 d), tests (1 d).

**Optional OCCT track — not recommended now.** Enabling the three toolkits ~0.5 d; a throwaway STEP-in/STEP-out fillet utility 2-3 d; genuine
integrated B-rep editing is **weeks** and should wait until the mesh Edit gizmo has shipped and demand is proven (§4).

**Suggested order: phase 1, then 2a, then 3, then 2** — topology-light, high-value items first, deferring the hardest algorithm until selection,
preview and undo have been exercised by real use.
