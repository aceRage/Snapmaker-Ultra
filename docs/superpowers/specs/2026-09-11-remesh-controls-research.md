# Remesh controls research: flat-bottom protection and optional quad remeshing

Research spec, not an implementation plan for this session. Scope: `ObjectList::repair_by_remesh()`
(`src/slic3r/GUI/GUI_ObjectList.cpp:5731`) and `remesh_by_voxels()` / `mesh_to_grid()` /
`grid_to_mesh()` in `src/libslic3r/OpenVDBUtils.{hpp,cpp}`.

## A. Why the flat bottom is lost today

### What the code does

`repair_by_remesh()` picks one auto voxel size per volume:

```cpp
const double voxel = std::clamp(bb.size().norm() / 300., 0.05, 0.3);   // GUI_ObjectList.cpp:5747
indexed_triangle_set its = remesh_by_voxels(mv.mesh().its, voxel);
```

`remesh_by_voxels` (`OpenVDBUtils.cpp:90`):

```cpp
auto grid = mesh_to_grid(mesh, {}, voxel_scale, 3.0f, 3.0f);   // exterior/interior band = 3 voxels
indexed_triangle_set its = grid_to_mesh(*grid, 0.0, 0.0);      // isovalue 0, adaptivity 0
```

`mesh_to_grid` builds a narrow-band level set via `openvdb::tools::meshToVolume` (exterior and
interior band width both **3 voxels**), unions per-shell subgrids with `csgUnion`, then
`levelSetRebuild`s at iso 0 with the same 3-voxel bands. `grid_to_mesh` calls
`openvdb::tools::volumeToMesh(grid, points, tris, quads, isovalue=0.0, adaptivity=0.0)`.

### Why a flat face becomes bumpy

`meshToVolume` stores, per voxel, the distance to the nearest sampled surface point - close to
exact for a plane, but perturbed by two things:

1. **Grid quantization of the reconstruction, not just the sampling.** `volumeToMesh` extracts the
   zero-crossing of a trilinearly-interpolated grid on a fixed 1/voxel lattice. A flat input plane
   that doesn't land on whole-voxel z-boundaries reconstructs as coplanar triangles with sub-voxel
   z jitter - at the default 0.05-0.3 mm auto voxel range this is a visibly rippled base, not float
   noise. Iso-surface reconstruction from a level set is only accurate to roughly the voxel size,
   and a perfectly flat plane gets no special-cased protection from that.
2. **`adaptivity = 0.0` doesn't fix this.** Adaptivity collapses near-coplanar voxel faces into
   fewer triangles; it is a simplification pass on the reconstructed mesh, not a correction to the
   sub-voxel z jitter baked into vertex positions during reconstruction. Bumpiness survives
   regardless of the adaptivity value.

### Why the outline edge rounds

The bottom meets the side wall at a sharp dihedral edge. The narrow band is **3 voxels wide on
each side of the surface** (`exteriorBandWidth = interiorBandWidth = 3.0f`, in `mesh_to_grid` and
again in the post-`csgUnion` `levelSetRebuild`). At a sharp convex edge the true SDF isn't
differentiable; voxels within about one voxel of the edge are influenced by both incident faces,
averaging the corner into a rounded fillet roughly half a voxel to a full voxel in radius.
`levelSetRebuild` re-derives a clean SDF from an existing grid but cannot restore resolution the
narrow band never had, so it doesn't undo the rounding. This is a textbook property of low-order
narrow-band level sets, and it's the same reason the sculpt spec already treats voxel remeshing as
lossy below the voxel size (comment in code, `GUI_ObjectList.cpp:5729`).

**Net effect:** at a 0.2 mm auto voxel, the base ripples and the outline erodes/rounds by up to
~0.2 mm all around - the first layer is no longer flush with the bed, matching the owner's
complaint exactly.

## Options for "Keep the bottom flat"

All three take `z_min` from the *original* mesh and act on the output of today's
`remesh_by_voxels`, leaving interior/wall behavior untouched.

**(i) Post-process snap + 2D outline re-clip.** Project the original bottom-face outline to XY
(Clipper2 is already a dependency, used throughout `Fill`/`SLA`). After remeshing: snap vertices
within one voxel of `z_min` to `z_min`, re-clip the bottom facets against the original outline
polygon, re-triangulate the cap and stitch it to the shortened wall ring. Risk: the stitch must
avoid self-intersection where the clipped cap meets the voxel-remeshed wall if erosion exceeded the
wall's local slope - needs an inward-only bias at the base.

**(ii) Remesh above `z_min + delta`, boolean-union with the original bottom slab.** Cut the
original mesh at `z_min + delta` (delta ~ 1-2 voxels) using the boolean backends already vendored
(`MeshBoolean::mfd::make_boolean` / Manifold, the Boolean gizmo's primary backend,
`MeshBoolean.hpp:108`; `mcut` as the existing fallback, `MeshBoolean.hpp:75-98`). Voxel-remesh only
the upper part; union it with the untouched lower slab. This is the only option that makes the
bottom slab *bit-identical* to the input, since it never passes through OpenVDB. Risk: if `delta`
is smaller than local curvature near the base, the cut plane can slice a non-vertical wall and
leave a visible boolean seam - default `delta` to 2-3x the voxel size and warn when the cut plane
meets a near-vertical face.

**(iii) OpenVDB level-set clip (`tools::clip`) plus a flat cap.** Clip the level set to
`z >= z_min + delta` before `volumeToMesh`, cap the opening at the clip plane, and stitch on the
original bottom slab. Functionally the same result as (ii) but done in level-set space; OpenVDB's
`tools::clip` does not itself guarantee a watertight cap, so this needs bespoke capping code for no
benefit over calling `mfd::make_boolean` directly.

**Recommendation: (ii).** It gives an exact bottom, reuses the house boolean backend already used
for the Boolean gizmo, and concentrates risk in one place (`delta` choice, near-vertical-wall
warning) instead of spreading it across a clip, a snap, and a re-triangulation step.

### Proposed Remesh dialog (house style: `DPIDialog` / `::Label` / `::TextInput`, per
`src/slic3r/GUI/FillBedDialog.cpp`)

- **Voxel size (mm)** - `::TextInput`, numeric validator, pre-filled with today's auto value shown
  as text (not hidden); user can override.
- **Detail** - a slider mapped inversely to voxel size, live-updating the Voxel size field (two
  views of one number, like `FillBedDialog`'s live estimate).
- **Keep the bottom flat** (`::CheckBox`, default on) - implements option (ii); reveals a
  **Bottom margin (mm)** input for `delta`, default `2 * voxel size`, min `1 * voxel`.
- **Preserve sharp edges** (`::CheckBox`, default on) - `adaptivity` alone doesn't protect a
  dihedral edge's position (only reduces triangle count near it, per the diagnosis above). Real
  protection needs a post-pass: reuse the sharp-edge *detection* already shipped for Sculpt
  (`SculptSession::detect_sharp_edges`, `MeshSculpt.cpp:517`, dihedral-angle threshold via pairwise
  face-normal dots on a vertex's incident faces) on the original mesh, then snap any remeshed
  vertex within one voxel of an original sharp edge back onto that edge's line (the bed-plane snap
  idea, generalized from a plane to a line). Expose the feature-angle threshold, defaulting to
  Sculpt's 60° (`GLGizmoSculpt.cpp` `m_sharp_dihedral`) so both tools agree.
- **Triangle count preview**: report before/after count in the completion notification rather than
  live in the dialog, to avoid double-remeshing just for a preview.

### Interactions to call out

- **Painted supports/seams/MMU/fuzzy-skin**: already handled. `repair_by_remesh` calls
  `plater->clear_before_change_mesh(obj_idx)` (`GUI_ObjectList.cpp:5762`), which resets
  `supported_facets` / `seam_facets` / `mmu_segmentation_facets` / `fuzzy_skin_facets`
  (`Plater.cpp:24513-24534`) and pushes the existing `CustomSupportsAndSeamRemovedAfterRepair`
  notification when anything was painted. The new controls change nothing here - facet indices are
  meaningless after any voxel remesh regardless.
- **Sculpt masks**: `SculptSession`'s bed-contact/sharp-edge masks (`MeshSculpt.hpp:189-206`) are
  runtime-only, recomputed from geometry each time the gizmo opens - not stored on `ModelVolume`,
  so nothing needs explicit invalidation. Worth a tooltip note that a remeshed flat bottom will
  look flatter to Sculpt's own bed-contact auto-mask afterward.

## B. Quad remeshing candidates

| Library | License | Fit |
|---|---|---|
| **Instant Meshes** (wjakob/instant-meshes) | **BSD-3-Clause** (verified from repo `LICENSE.txt`) | OK |
| **QuadriFlow** (hjwdzh/QuadriFlow) | **BSD-3-Clause** core (verified) | OK, dependency caveat below |
| libQEx / QEx | GPL | Out - hard copyleft for no gain over two clean BSD options |
| CGAL | GPL/LGPL dual, commercial option | Out for new use. This tree already vendors CGAL v5.4 (`deps/CGAL/CGAL.cmake`) for `MeshBoolean.cpp`'s CGAL backend, and AGPL-3.0 combining with GPL code is an established-compatible pattern - but that precedent doesn't justify growing CGAL's footprint for quad remeshing when BSD options exist |
| OpenFlipper / QuadCover | mixed (BSD-ish core + GPL plugins) | Out - full end-user app, not a library; QuadCover is outperformed by QuadriFlow/Instant Meshes in published comparisons |
| MeshLab's QuadriFlow filter | GPL (VCGlib) | Out - GPL wrapper around the same BSD QuadriFlow core; use upstream directly |
| Blender's QuadriFlow adapter | GPL (glue) around BSD core | Out - same reasoning as MeshLab |

### Instant Meshes

BSD-3-Clause. The core field-aligned algorithm is usable without its NanoGUI-based interactive
viewer, but upstream ships it as a GUI app first, so isolating the library means pulling algorithm
sources and leaving NanoGUI/OpenGL glue out of the CMake target - moderate integration work, not a
straight `add_subdirectory`. Dependencies: Eigen and TBB (both already in this tree), plus small
bundled header-only utilities (`pcg32`, `dset`, `pss`). API: triangle mesh in, target face/vertex
count, optional crease-angle parameter, boundary handling via orientation-field constraints - maps
well to "Target faces" + "Sharp features". Known for very even, well-aligned quads and strong
singularity placement; a stable, small, single-author academic codebase with old but low-risk
upstream activity.

### QuadriFlow

BSD-3-Clause core (verified). **Dependency caveat**: default build uses Boost's Boykov max-flow
solver and bundles Lemon (both Boost Software License, permissive); a `BUILD_FREE_LICENSE` CMake
option swaps in an MPL2 sparse-LU solver instead. None of these block AGPL-3.0 redistribution, but
the choice must be pinned explicitly so it isn't flipped unknowingly later. Boost is already a
dependency here (`deps/CGAL/CGAL.cmake` DEPENDS `dep_Boost`), so the default path adds no new
third-party license. API: triangle mesh in, **explicit target face count** (first-class, matching
the owner's ask directly), optional sharp-feature flag. Built for scalability via global
optimization for singularity placement; trades a little per-quad regularity for robustness on
noisy input versus Instant Meshes, and is production-proven inside Blender's own remesh operator.

### Recommendation: QuadriFlow

Target-face-count is a first-class parameter (Instant Meshes targets edge length instead, needing
back-solving into an approximate count); QuadriFlow is library/CLI-first with no GUI shell to
strip; and it is production-validated in Blender. Pin the default (non-`BUILD_FREE_LICENSE`) CMake
path since Boost is already vendored.

### Sketch of integration

- New optional CMake dependency `deps/QuadriFlow/QuadriFlow.cmake`, `DEPENDS dep_Boost`, gated by a
  build flag (e.g. `SLIC3R_QUAD_REMESH`), Windows MSVC first, macOS/Linux CI once the dependency
  graph is confirmed cross-platform - the phased pattern this repo already uses for new native deps.
- `src/libslic3r/QuadRemesh.{hpp,cpp}` wrapper mirroring `OpenVDBUtils.hpp`'s style:
  `indexed_triangle_set quad_remesh(const indexed_triangle_set&, int target_faces, bool
  preserve_sharp_features)`, triangulating internally for every existing consumer (slicing, convex
  hull), plus a second accessor exposing raw quad topology (`Vec4i` quads) for the one consumer
  that wants quads: the Sculpt panel's future Subdivide workflow, where evenly sized quad faces
  subdivide more predictably than today's arbitrary triangle sizes (sculpt spec v2 "Subdivide under
  the brush only", `2026-09-07-sculpt-mode.md:780`).
- UI: a **"Quad remesh..."** action next to "Repair by remeshing" in the object context menu
  (`GUI_ObjectList.cpp`), plus a second entry point from the Sculpt panel (`GLGizmoSculpt.cpp`).
  Dialog: **Target faces** (`::TextInput`, integer), **Sharp features** (`::CheckBox`, on).
  When `SLIC3R_QUAD_REMESH` isn't compiled in, the menu entry is hidden or disabled with an
  explanatory tooltip, matching how other optionally-compiled features already degrade.
- Quads vs triangles downstream: the wrapper defaults to a triangulated `indexed_triangle_set`, so
  slicing and every existing consumer are unaffected; only the Sculpt/Subdivide path opts into the
  quad-aware accessor.

## C. Tests

**Flat-bottom guarantee** (unit test, `tests/libslic3r/`): build a chamfered box, run the new
"Keep the bottom flat" path, assert every base vertex is at `z_min +/- 1e-4` and the base outline's
shoelace area is within 1% of the original - same style of check already implemented for Sculpt's
bed-contact pinning (`loop_area_xy` / `loop_perimeter_xy`, `MeshSculpt.hpp:407-409`); reuse or
mirror that helper.

**Quad path** (unit test): remesh a UV sphere at target 2000 faces; assert 1800-2200 faces
returned (10% tolerance), every face is a quad (4 indices), and the triangulated result passes
whatever manifoldness/watertight check `MeshBoolean`'s own tests already use for inputs.

**Owner click-tests** (manual): (1) remesh a model with a painted region, confirm the existing
"Custom supports... removed" notification still fires and the base is flat/crisp at defaults;
(2) remesh with "Keep the bottom flat" off, confirm today's rounded behavior is unchanged
bit-for-bit - regression guard that the new default-on control is additive; (3) quad-remesh a
mid-poly organic model at a few target counts, confirm normal slicing (quads triangulated
transparently) and sane Subdivide behavior afterward in Sculpt.

## D. Phases and estimates

**Phase 1 - Remesh dialog (voxel size, Keep bottom flat, Preserve sharp edges).** Scope: the
dialog (small, `FillBedDialog.cpp` at ~360 lines is a good size reference), the option (ii)
boolean-union flat-bottom path reusing `MeshBoolean::mfd::make_boolean` with `mcut` fallback (no
new dependency), the sharp-edge post-pass factored out of `SculptSession::detect_sharp_edges`'s
dihedral logic into a shared free function, and the two new unit tests. Estimate: **3-4 days**
(dialog + wiring ~1 day; flat-bottom boolean path + near-vertical-wall edge cases ~1.5 days;
sharp-edge snap post-pass ~0.5 day; tests + polish ~1 day).

**Phase 2 - Quad remesher integration.** Scope: vendor QuadriFlow via a new `deps/QuadriFlow` CMake
module (Windows MSVC first, pin non-`BUILD_FREE_LICENSE`), the `QuadRemesh.{hpp,cpp}` wrapper, the
"Quad remesh..." menu action and dialog, the Sculpt-panel entry point, the triangulate-for-slicing
default, and the quad-path unit test. Estimate: **2-2.5 weeks**, front-loaded by dependency
bring-up risk - a new native dependency with its own solver sub-dependency, built for the first
time on this project's Windows MSVC toolchain, is the main unknown; budget the first 3-4 days
purely for a clean build/link before any slicer-side integration code. macOS/Linux CI follows once
Windows is proven, per the brief's stated ordering.
