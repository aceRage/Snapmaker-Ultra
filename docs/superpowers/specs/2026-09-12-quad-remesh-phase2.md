# Quad remesh (Phase 2): QuadriFlow integration

Implementation record for Phase 2 of `2026-09-11-remesh-controls-research.md`. Phase 1 (the
Remesh dialog, the flat-bottom boolean path and the sharp-edge post-pass) shipped as
`src/libslic3r/MeshRemesh.{hpp,cpp}` + `src/slic3r/GUI/RemeshDialog.{hpp,cpp}`; this document
covers the optional quad remesher built on top of it.

Branch: `feat/quad-remesh`, from `feat/ultra-preferences`.

## What was built

| Layer | Files |
|---|---|
| Dependency | `deps/QuadriFlow/QuadriFlow.cmake`, `deps/QuadriFlow/CMakeLists.txt.in`, `deps/CMakeLists.txt` |
| Build option | `CMakeLists.txt` (`SLIC3R_QUAD_REMESH`), `src/libslic3r/CMakeLists.txt` |
| Geometry | `src/libslic3r/QuadRemesh.{hpp,cpp}` |
| GUI | `src/slic3r/GUI/QuadRemeshDialog.{hpp,cpp}`, `GUI_ObjectList.{hpp,cpp}`, `GUI_Factories.cpp`, `Gizmos/GLGizmoSculpt.cpp`, `AboutDialog.cpp` |
| Tests | `tests/libslic3r/test_quad_remesh.cpp` (tag `[QuadRemesh]`) |

## 1. The dependency

**Pin: `810b7a0967c35b0dc85b4464e3835e26a756c967`** (hjwdzh/QuadriFlow HEAD). Upstream has cut no
releases and has been quiet for years, so a commit is the only pin available. Archive SHA256
`0e530a1374dd7edd68d8bd9777395dc0be80e4cbca96485cc9a82cfacb8942ce`.

### Licence audit

The research spec's table said "BSD-3-Clause core (verified)". Re-verified here by reading the
pinned tree rather than the README, because two of its claims did not survive contact:

| Component | Licence | Compiled? |
|---|---|---|
| `src/*` (QuadriFlow) | **BSD-3-Clause** (`LICENSE.txt`) | yes |
| `3rd/lemon-1.3.1` | **Boost Software License 1.0** | yes, four `.cc` files |
| `3rd/pcg32`, `3rd/pss` | permissive, header-only | yes (headers) |
| Boost.Graph (Boykov max-flow) | BSL-1.0, already a dep | yes |
| Eigen | MPL2 | yes, `EIGEN_MPL2_ONLY` set |
| `3rd/MapleCOMSPS_LRB` | MIT (MiniSat derivative) | **no** |
| `src/post-solver.cpp` | needs Ceres (Apache-2.0) | **no** |
| `src/Optimizer.cu` | CUDA | **no** |

Nothing copyleft, nothing that constrains AGPL-3.0 redistribution. Both shipped licence texts are
installed to `share/quadriflow/` by the deps build, and **QuadriFlow** and **LEMON** were added to
`CopyrightsDialog::fill_entries()` in `AboutDialog.cpp` — the third-party list the app actually
ships (there is no licence file tree under `resources/`; that dialog is the list).

**Two corrections to the Phase 1 research spec**, both worth knowing before anyone touches this
again:

1. *"a `BUILD_FREE_LICENSE` CMake option swaps in an MPL2 sparse-LU solver instead."* It does not.
   The only thing that option does in the pinned tree is `add_definitions(-DEIGEN_MPL2_ONLY)`. The
   solver selection is unaffected. It is still passed `ON` — turning "no LGPL Eigen headers" into a
   compile-time failure rather than a promise is worth having — but not for the stated reason, and
   the recommendation to pin the *non*-`BUILD_FREE_LICENSE` path was therefore inverted.
2. The MapleCOMSPS SAT solver is bundled in `3rd/` but is not in any build. `src/localsat.cpp`
   shells out to an external `minisat` **binary** via `system()`; it does not link the bundled
   solver at all. That path is only reachable with `flag_aggresive_sat`, which the wrapper never
   sets — so there is no dependency on a tool the user may not have.

### Why the CMakeLists is replaced, not patched

Upstream builds exactly one thing: the `quadriflow` CLI executable. There is **no library target
and no `install()` rule anywhere**, so `ExternalProject`'s `--target install` would install nothing
and the slicer would have nothing to link. It also hard-codes `-O3`, `-fopenmp` and
`-fsanitize=address` into `CMAKE_CXX_FLAGS_RELEASE`, which MSVC rejects outright.

So `deps/QuadriFlow/CMakeLists.txt.in` replaces it, following the `deps/OpenCSG` precedent. It is a
transcription of upstream's build with four deliberate differences:

1. a static library instead of an executable (`src/main.cpp`, the CLI's `main()`, is the only
   source dropped from upstream's `quadriflow_SRC`);
2. `install()` rules plus a generated `QuadriFlowConfig.cmake`;
3. compiler flags left to the toolchain (upstream's MSVC warning-suppression block is kept
   verbatim, plus `/bigobj` — `parametrizer-int.cpp`'s Eigen expression templates exhaust the
   default object-file section limit);
4. a minimal Lemon build — `base.cc`, `color.cc`, `random.cc`, `arg_parser.cc` and a generated
   `config.h` — instead of `add_subdirectory()` on Lemon's own CMake project, which drags in docs,
   demos, tests, an hg/wget/python probe and LP-solver bindings (glpk, cplex, clp, cbc, soplex)
   whose libraries we do not have and must not link.

`EIGEN3_INCLUDE_DIR` points at the tree's own `deps_src/eigen`, the same copy libslic3r compiles
against: QuadriFlow's Eigen types cross the ABI boundary in the wrapper, so the two must agree.

### Build status

**Windows MSVC: built, installed and linked clean.** VS 2022 17.14 (MSVC 19.44), x64, cmake 3.31.8.
Both `quadriflow.lib` and `quadriflow_lemon.lib` compiled with no errors on the first attempt; a
standalone consumer that does `find_package(QuadriFlow)` and calls into `qflow::Parametrizer`
configures, compiles, links and runs against the installed prefix.

**macOS/Linux CI: unverified.** `deps/CMakeLists.txt` now `include()`s the package and lists
`dep_QuadriFlow` in `_dep_list`, so CI will pick it up on its next deps build. What to watch for,
none of which could be tested from here:

- The replacement CMakeLists sets no GCC/Clang optimisation flags, relying on `CMAKE_BUILD_TYPE`
  from the deps harness. Upstream forced `-O3`; a deps build that somehow lands on `-O0` would be
  slow rather than wrong.
- `-Wno-sign-compare -Wno-int-in-bool-context` are applied on non-MSVC. `-Wno-int-in-bool-context`
  is a GCC spelling; Clang accepts unknown `-Wno-*` with only a warning, so this should be inert
  rather than fatal on macOS, but it is the most likely first thing to need a tweak.
- Lemon's `config.h.in` is configured with `LEMON_VERSION` only; on a platform where Lemon's own
  CMake would have probed for more, something may be missing. It built clean on MSVC with just that.
- The `FindBoost` CMP0167 deprecation warning is emitted (harmless, and the same one the rest of
  the deps tree already produces).

### What was added to the shared deps prefix

The brief authorised installing into the owner's shared prefix
`C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local`, add-only. All six install roots were
confirmed non-existent beforehand, so **nothing was overwritten or removed**:

```
lib/quadriflow.lib
lib/quadriflow_lemon.lib
lib/cmake/QuadriFlow/          (Config, ConfigVersion, Targets, Targets-release)
include/quadriflow/            (QuadriFlow headers, pcg32/, lemon/)
share/quadriflow/              (LICENSE.QuadriFlow.txt, LICENSE.LEMON.txt)
```

Release configuration only — `add_debug_dep()` is wired in `QuadriFlow.cmake` for a full deps
build, but the spike installed Release alone, which is what the worktree build needs.

## 2. The CMake option

`SLIC3R_QUAD_REMESH`, default `ON`, in the top-level `CMakeLists.txt`. Unlike OpenVDB above it, a
missing package is **a warning and a disabled feature, not a fatal error**, so a tree whose deps
predate `deps/QuadriFlow` still builds:

```cmake
if (SLIC3R_QUAD_REMESH)
    find_package(QuadriFlow QUIET)
    if (NOT QuadriFlow_FOUND)
        set(SLIC3R_QUAD_REMESH OFF)
        message(STATUS "QuadriFlow not found - the quad remesher will be omitted. ...")
```

`libslic3r` gets `SLIC3R_QUAD_REMESH` as a **PUBLIC** compile definition, so the GUI and the tests
see the same answer `quad_remesh_available()` was built with.

## 3. libslic3r

`quad_remesh(its, opts, report)` → `QuadMesh` (indexed quads) and `QuadMesh::triangulate()` → an
`indexed_triangle_set` for slicing. `quad_remesh_triangulated()` is the one-call convenience the
GUI uses.

`QuadRemesh.cpp` **compiles either way**: without the dependency it provides refusing stubs and
`quad_remesh_available()` returns `false`, so GUI code needs no `#ifdef` and the disabled branch
still type-checks.

Three decisions worth recording:

**Determinism.** QuadriFlow seeds a PRNG and calls the global `srand()`. Without care, a model
quad-remeshed before slicing would give different G-code every run and take the slice determinism
gates (`2026-09-07-slice-determinism.md`) with it. So: the seed is an explicit parameter defaulting
to a fixed constant (`QUAD_REMESH_DEFAULT_SEED = 0`), never persisted and never exposed in the UI;
and calls are serialised on a mutex, because upstream's stages share global `rand()` state and two
concurrent remeshes would make the result depend on the interleaving. The test asserts
**bit-identical** output across two runs, not approximate equality.

**Non-manifold input is refused, not attempted.** QuadriFlow's `dedge` stage builds a half-edge
structure and its `hierarchy` stage walks it assuming every edge has an opposite. An open edge makes
that walk run off the surface: upstream either asserts or emits holes in unpredictable places.
`quad_remesh_accepts()` rejects open edges (`its_num_open_edges`) and multi-shell inputs
(`its_is_splittable`), each with a reason the user can act on — and the open-mesh message points at
*Repair by remeshing*, which is the tool that fixes it. The dialog asks this **before** opening, so
a bad part gets an explanation rather than a button that does nothing.

**The volume stores triangles.** `QuadMesh` exists for the one consumer that wants topology
(Sculpt/Subdivide); slicing, the convex hull and every file format keep seeing an
`indexed_triangle_set`. Per the brief, **nothing extra is stored** — no second mesh representation
on `ModelVolume`. `triangulate()` splits each quad on its 0–2 diagonal and *shares* the diagonal's
two vertices, so a watertight quad mesh stays watertight rather than merely looking so by face
count; the test checks the edge pairing, not the arithmetic.

The wrapper feeds the mesh in **memory** (filling `Parametrizer::V`/`F` then calling
`NormalizeMesh()`) rather than through upstream's `Load()`, which parses an `.obj` from disk —
avoiding both a temp-file round-trip and the precision loss of the text format. Upstream's pipeline
is then called stage by stage in `main.cpp`'s order, kept explicit so a future upstream bump shows
up as a diff here instead of silently changing behaviour. Everything is wrapped in a `try/catch`:
a failed remesh must leave the user's model alone, not take the app down.

## 4. GUI

**`QuadRemeshDialog`** — house style, a direct sibling of `RemeshDialog` (`DPIDialog`, `::Label`,
`::TextInput`, `::CheckBox`, `DialogButtons`, `UpdateDlgDarkUI`):

- **Target faces** — integer `::TextInput`, prefilled with the part's triangle count / 2 (two
  triangles make a quad, so that is roughly density-preserving), clamped to
  `[QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX]` = `[20, 2000000]` by the same constants
  libslic3r enforces.
- **Preserve sharp features** — `::CheckBox`, default on.
- **Count preview** — `Triangles: N -> about M (about K quads)`, live on every keystroke. Unlike the
  voxel dialog's estimate this relationship is exact (the stored mesh is two triangles per quad);
  it still says "about" because the solver lands *near* the target rather than on it.
- Settings persist to `AppConfig` section `quad_remesh`; the seed deliberately does not.
- When the selected part would be refused, the dialog **still opens** with OK disabled and the
  reason shown in place of the estimate, so the user learns *why* instead of pressing a dead button.

**Entry points**, both as the spec asks:

1. Object context menu, immediately after *Repair/Remesh* (`GUI_Factories.cpp`). Hidden entirely
   when `quad_remesh_available()` is false — an entry that can only report "unavailable" is worse
   than no entry.
2. The Sculpt panel, below the Subdivide row (`GLGizmoSculpt.cpp`), which is what it is *for*:
   Subdivide splits whatever triangles the part happens to have, so an uneven mesh stays uneven and
   the brush bites differently in different places. It closes the gizmo and hands over to the
   object-list action rather than remeshing in place, because the mesh swap invalidates every mask
   and cache the open session holds.

`ObjectList::quad_remesh()` mirrors `repair_by_remesh()` exactly: snapshot, `wxBusyCursor`,
`clear_before_change_mesh()` (which is what clears painted supports/seams/MMU/fuzzy-skin and fires
the existing removal notification), `set_mesh` / `set_new_unique_id` / `calculate_convex_hull`,
`ensure_on_bed`, `changed_mesh`, then a notification with before/after counts. A refusal always has
an actionable reason, so the first one is carried into the notification rather than being left in
the log. A multi-part selection where the target is untouched gets each part's *own* default, so
parts keep their relative density.

## 5. Tests

`tests/libslic3r/test_quad_remesh.cpp`, tag `[QuadRemesh]`. Seven cases, **all passing** (25424
assertions):

| Test | Asserts |
|---|---|
| default target | triangle count / 2, with the clamp floor honoured |
| unavailable build | `quad_remesh()` is safe to call and reports either way (runs in both builds) |
| **sphere @ 2000** | 1800–2200 quads; every quad has 4 distinct in-range indices; watertight after triangulation; still sphere-shaped (bbox + per-vertex radius) |
| **cube, preserve sharp** | all 12 edges still carry vertices within 0.6 mm; no vertex off the cube's surface; watertight |
| **non-manifold refusal** | open mesh → `NotManifold` + reason; two shells → `NotManifold`; empty → `EmptyInput`; no crash, no partial mesh |
| **same seed twice** | bit-identical vertices and quads, for an explicit seed *and* for the default |
| triangulate() | vertex set not duplicated, zero open edges by both the local pairing check and `its_num_open_edges`, no bridging spikes |

Phase 1's `[MeshRemesh]` suite still passes unchanged (7 cases, 3537 assertions). Both together:
14 cases, 28961 assertions, all passing.

`libslic3r_gui` also builds and links clean with all five touched GUI files compiling without
error or new warning.

**One build-environment note for whoever runs this next.** The first `libslic3r_gui` attempt died
with `error C1902: Program database manager mismatch` in an unrelated file
(`GUI/filamentsync/FilamentColorMapBox.cpp`). That is the known mspdbsrv/PDB clash, not a code
error: several agents were compiling in this tree at once and `/m:2` had two MSBuild nodes
contending for the same PDB server. Rebuilding with `/m:1` fixed it with no source change. Also
worth knowing: `TaskStop` on a foreground `cmake --build` kills the console session and the build
dies silently without writing errors — relaunch detached with
`Start-Process -FilePath <bat> -WindowStyle Hidden -PassThru` and tail the log instead.

## Deviations from the brief

- **`BUILD_FREE_LICENSE` semantics** — passed `ON` as instructed, but it does not do what the
  research spec said (see the licence audit). Documented rather than worked around.
- **Upstream ships no library** — the brief assumed a normal `add_cmake_project` would do. It
  cannot; the replacement-CMakeLists approach (`deps/OpenCSG` precedent) was needed.
- **Standalone build, not `deps/build` target** — as the brief's fallback allowed, since building
  the deps project's own target would have re-examined the whole dependency graph.
- **`quads`/`Vec4i32`** — `Point.hpp` has `Vec4i` commented out; `Vec4i32` is the live alias.
- **Licence list** — there is no third-party licence file tree under `resources/`;
  `CopyrightsDialog::fill_entries()` in `AboutDialog.cpp` is the list the app ships, so the entries
  went there.

## Open items

- macOS/Linux deps CI, per the build-status section above.
- The Sculpt/Subdivide workflow does not yet *use* the quad topology — per the brief, nothing extra
  is stored and the volume holds the triangulated mesh. Making Subdivide quad-aware is future work;
  the `QuadMesh` accessor exists for it.
- Owner click-tests (below) not run — no model was loaded into a live slicer from this session.

## Click-tests for the owner

1. Select a closed part → right-click → **Quad remesh...**; confirm the dialog prefills with about
   half the triangle count, the preview line tracks typing, and the result slices normally.
2. Quad-remesh a part with painted supports; confirm the existing "custom supports removed"
   notification still fires.
3. Select a part with holes → **Quad remesh...**; confirm the dialog opens with OK greyed and the
   reason shown, pointing at Repair by remeshing.
4. Open **Sculpt** on a mid-poly organic part → **Quad remesh...** below Subdivide; confirm the
   gizmo closes, the remesh runs, and re-opening Sculpt shows an even mesh that Subdivides evenly.
5. Quad-remesh the same part twice at the same target and confirm identical triangle counts
   (the determinism guarantee, visible without a G-code diff).
