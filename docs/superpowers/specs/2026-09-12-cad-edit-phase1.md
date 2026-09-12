# CAD Edit mode — phase 1 as built

Branch `feat/cad-edit-mode`, off `feat/ultra-preferences`. Implements **phase 1** of
`docs/superpowers/specs/2026-09-11-cad-mode-research.md` (§3.1, §3.2, §7): an **Edit** gizmo with feature-edge-chain and planar-face
selection, face push/pull by translation, a gizmo-local undo stack, and the Sculpt-style commit that preserves painted data.

No bevel, chamfer, extrude, inset or loop selection — those are phases 2 and 3 and every one of them inserts geometry, so when they land
they must take the *other* commit path (`clear_before_change_mesh`), the way Subdivide and Simplify do.

## What was built

### `src/libslic3r/MeshEdit.{hpp,cpp}` — pure geometry, no GL, no wx, no Model

The whole module rests on one property, which is also the reason phase 1 is scoped this way: **`translate_region()` moves vertex
positions and nothing else.** It never touches `its.indices`, never adds or removes a vertex, and never reorders anything. Same contract
as `MeshSculpt.hpp`, and it buys the same thing — the commit may go through `Sculpt::commit_sculpted_mesh()` *without*
`Plater::clear_before_change_mesh()`, so painted supports, seams, MMU colours and fuzzy skin all survive a push.

| Piece | What it does |
|---|---|
| `MeshTopology` / `build_topology()` | Face normals, `its_face_neighbors`, `its_face_edge_ids` (with `assign_unbound_edges`), plus the **reverse map** edge id → incident facets and the per-edge facet count. The reverse map is the new part: nothing in-tree exposes edge → facets, and the chain walk needs it. |
| `edge_dihedral_deg` / `feature_edge_mask` | Dihedral angle per global edge. A boundary edge reports 180 (as "feature" as an edge gets); a non-manifold edge reports 0, so nothing selects one by accident. |
| `grow_edge_chain` / `all_edge_chains` | The spec's walk: extend from each end while **exactly one** other feature edge is incident and the turn is under a continuation threshold. Three or more incident feature edges is a *corner* and terminates the chain. Detects closure back onto the seed. |
| `grow_face_region` | Two modes. `Planar` tests every candidate against the **seed** normal only — a CAD face. `Smooth` adds a per-step test against the facet it came from; this is `Measure.cpp`'s `grow_curve_patch` re-expressed with **both thresholds exposed** instead of frozen at 8/20 deg, as the spec suggested reusing. Returns a sorted facet list plus the area-weighted normal, area and centroid. |
| `translate_region` | Moves every vertex used by the region's facets — interior *and* border — so the facets outside the region that share a border vertex **stretch to follow**. Reports `moved_vertices` and `dirty_facets` for the GPU patch. |
| `self_intersects` | Thin wrapper over `MeshBoolean::cgal::does_self_intersect`, so the tests and the gizmo have one name for the guard and `MeshEdit.cpp` owns the only include of it. |
| `snap_to_step` | Both the numeric field and the drag go through it, so typing 5.0 and dragging to 5.0 give bit-identical meshes. |
| `EditSession` | Working mesh + topology cache + the **gizmo-local undo stack** (32 entries, one per *completed* operation, redo branch cleared on a new edit — the shape `GLGizmoSculpt`'s stroke undo and `GLGizmoCut3D`'s curved-sheet undo both use). |

**Guards on a push**, cheapest first:
1. `WholeMesh` — the region is the whole mesh, so there is no ring left to stretch. Refused; that is the Move gizmo's job.
2. `NoOp` — `d == 0` returns the input bit-identical. The documented identity case, not an error.
3. `OutOfBounds` — `|d|` exceeds the mesh's own bounding-box diagonal, so it cannot be anything but a fold-through. Refused without paying for CGAL.
4. `SelfIntersects` — `does_self_intersect` on the moved mesh. **A refusal returns an empty mesh**, so a caller that ignores the status cannot accidentally install a broken one.

### `src/slic3r/GUI/Gizmos/GLGizmoEdit.{hpp,cpp}` — the gizmo

* **Three pick modes** on a radio row: `Face` (planar, tolerance slider), `Curved face` (smooth, per-step + total-curvature sliders), `Edge chain` (dihedral-threshold slider). Hover re-grows on every mouse move and caches on the seed facet / seed edge, so a hover that stays put rebuilds nothing.
* **Highlights** through `init_plane_data(its, facets, MEASURE_PLNE_NORMAL_OFFSET)` — the same builder and the same z-fight offset `GLGizmoMeasure::init_plane_glmodel` uses, drawn with `gouraud_light`. Hover is a pale blue, selection a solid amber, the same hover-vs-selected convention `GLGizmoMeasure::render_glmodel(..., hover)` follows. A chain draws as a 6-sided tube per segment, radius a fraction of the part's bounding-box diagonal so it reads the same on a 5 mm part and a 300 mm one.
* **Push/pull**: a cylinder along the region normal at its centroid is the drag handle. A drag is **absolute** — every tick re-applies the whole displacement to the pre-drag mesh — so the gesture is exactly reversible and a drag back to the start returns the start. Distance comes from the closest approach of the mouse ray to the push axis (the Move gizmo's solve), which stays stable when the axis points near the camera; a camera-facing plane would not.
* **Preview vs commit**: a drag tick previews *in the render volume only* — no self-intersection test (too slow per tick), no undo entry, no Model touch. `end_drag()` puts the working mesh back to the pre-drag state and applies **once**, guarded: that path takes the `TakeSnapshot(_u8L("Edit"), GizmoAction)`, runs the self-intersection test and commits. This is the spec's "the drag preview must not snapshot per tick".
* **Undo**: Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z against the session stack, plus Undo/Redo buttons. Routed through `GLGizmosManager::on_char`'s first-refusal hook, added right after the Cut gizmo's, so Ctrl+Z reaches the gizmo's stack *before* the canvas turns it into an `EVT_GLCANVAS_UNDO`. Esc drops the selection (or cancels a live drag) rather than closing the gizmo, and only claims the key when there is something to drop.
* **Commit**: `Sculpt::commit_sculpted_mesh(*m_volume, std::move(edited), true)` with **no** `clear_before_change_mesh()`. That function refuses outright unless `indices_match()`, so this can never silently misalign existing paint; the gizmo checks the return and does nothing on a refusal rather than committing anyway.
* **Panel** states the refusal reason rather than doing nothing silently, and a self-intersection refusal also raises a notification.

### Registration, kept minimal and additive for a clean merge

* `EType::Edit` **appended after `Sculpt`** in `GLGizmosManager.hpp`, so no existing enum value moves and the `m_gizmos` order they index is untouched.
* Four one-hunk additions in `GLGizmosManager.cpp`: the include, the dark-icon `case`, the `emplace_back` (last in the list), and the `get_name` `case` — each immediately after the matching Sculpt line. Plus the `on_char` hook, appended after the Cut gizmo's.
* `resources/images/toolbar_edit.svg` + `toolbar_edit_dark.svg`, in the Sculpt icons' grammar: grey workpiece outline (isometric box) plus the teal `#009688` accent showing the tool action (the top face tinted, a push arrow above it). Icons need no registration anywhere else.
* No change to `GLGizmoCut*`, `DrawCut*`, `MeshRemesh*`, `RemeshDialog` or any Sculpt file — the files the parallel agents own.

## Deviations from the research spec

1. **`EType::Edit` is appended after `Sculpt`, not inserted.** The spec pointed at `GLGizmosManager.hpp:93`; inserting there would renumber every later enum value. Appending keeps the diff to one hunk and cannot collide with the parallel gizmo work.
2. **`grow_curve_patch` is re-expressed, not called.** `MeasuringImpl::grow_curve_patch` is private to `Measure.cpp`, hard-codes 8/20 deg and is only reachable through `Measuring::get_feature(..., pick_kind = 2)` wrapped in a `SurfaceFeature`. Phase 1 needs the two thresholds on sliders and needs the facet list directly, so the same DFS lives in `grow_face_region(RegionMode::Smooth)` with the tuning handed to the caller. `Measuring` is not constructed at all, which also keeps the Edit session cheap (no border-loop walk, no `tbb::parallel_for` on attach).
3. **Planar selection does not go through `MeasuringImpl::update_planes` either**, for the same reason plus one more: `update_planes` builds *every* region on construction, which is wasted work when a session touches two or three faces. `grow_face_region(RegionMode::Planar)` is the same strict seed test, computed on demand.
4. **`nearest_edge_of_facet` replaces the snap-radius cap for edge picking.** The spec pointed at `Measure.cpp`'s `facet_snap_extent` bug. Phase 1 sidesteps it structurally: edge picking is a separate *mode*, so an edge pick and a face pick never compete for the same hover and there is nothing for a corner to steal. If the two modes are ever merged into one auto-picking mode, `facet_snap_extent`'s cap is the thing to reuse.
5. **Edge chains are selection and highlight only.** Nothing in phase 1 acts on a chain — it is the substrate phase 2's bevel will consume. The panel says so rather than offering a control that does nothing.
6. **`ImGui::BeginDisabled` is not available** in this bundled imgui; the Undo/Redo buttons grey through `ImGuiWrapper::button(label, size, enable)`, which is what the rest of the application uses.
7. **Undo/redo re-commit through `commit_sculpted_mesh` too**, and take their own plater snapshot. A session undo restores a mesh that differs only in vertex positions, so the same no-`clear_before_change_mesh` path applies and paint survives an undo as well as a push.
9. **A cube cannot be pushed into a self-intersection**, which the research spec's §6 assumed it could ("a push far enough to invert makes `does_self_intersect` true"). It does not: driving a cube's top face down through its bottom leaves the four walls on exactly the same footprint, so the solid merely **inverts** — no two triangles ever cross, and CGAL is right to report no self-intersection. The test was written that way first and failed for exactly this reason. A genuine self-intersection needs a face whose travel runs into *other* geometry, hence the L-prism. Worth knowing for phase 2: an inverted-but-not-crossing result is the `OutOfBounds` pre-check's job, and `does_self_intersect` will never catch it.
10. **The tapered-wall shear is not addressed** — it is explicitly phase 3 in the spec (§3.2 caveat) and the caveat is recorded verbatim in `MeshEdit.hpp` above `translate_region`.

## Tests — `tests/libslic3r/test_mesh_edit.cpp`, tag `[MeshEdit]`

Registered in `tests/libslic3r/CMakeLists.txt` beside `test_mesh_sculpt.cpp`.

| Test | Asserts |
|---|---|
| cube feature edges | Exactly **12** feature edges at 45 deg (the 6 coplanar face diagonals do not count) in exactly **12 chains of one edge each** — every cube vertex has three incident feature edges, so every one is a corner and no chain can extend. Every feature edge claimed by exactly one chain. |
| cylinder rim | A 32-segment cylinder at 30 deg gives exactly `2 * segs` feature edges; a seed on the top rim closes into the **whole 32-edge loop** (`closed == true`); `all_edge_chains` finds exactly the two rims. The wall seams (11.25 deg) are *not* features. |
| cube face region | The +Z face is exactly **2 triangles**, normal +Z, area 100 mm², sorted and unique. Smooth mode at the same tolerance agrees exactly. |
| chamfered box | A hand-built 8-sided prism with a 4 mm chamfer on the +X/+Z edge. The top region is **exactly** its 2 coplanar triangles and area `16 × 10` — it does **not** leak across the 45 deg chamfer; the chamfer flat and the +X wall are two further 2-facet regions, disjoint from it. Smooth mode crosses the chamfer where Planar will not, and the test pins **both** of its guards independently: a 50 deg seed cap admits the chamfer (45 deg from the seed) but refuses the +X wall (90 deg) → 4 facets; opening the cap to 100 deg admits all three → 6 facets; a 100 deg cap with a 10 deg *per-step* limit still stops at the top face → 2 facets. |
| **push a cube face 5 mm** | Volume up by **exactly `area × 5` = 500 mm³ within 1e-6 × 500**; mesh stays **watertight** (`its_num_open_edges == 0`) and **manifold** (every edge has exactly 2 incident facets); **vertex and facet counts unchanged and every index identical** — the assertion that actually protects the painted-data contract; the 4 face vertices moved by exactly +5 z and every other vertex is bit-identical; 10 dirty facets (2 top + 8 stretched wall). |
| pull / identity / whole mesh | `-5` drops the volume 500 mm³ and stays watertight; `d = 0` returns `NoOp` and a bit-identical mesh; selecting every facet returns `WholeMesh`. |
| **self-intersection** | On an **L-prism** (20 × 20 with a 10 × 10 notch), pushing the notch's inner +X wall **15 mm** drives it clean through the outer +X wall: returns `SelfIntersects` and an **empty** mesh. The same move with the guard off does produce a mesh, and `self_intersects()` confirms it really is broken — so the test measures the guard, not a tautology. A 5 mm push that stays inside the notch is `Ok` and watertight; a 1000 mm push returns `OutOfBounds` without reaching CGAL. |
| session undo | One stack entry per applied operation; two pushes then two undos restores the original **vertex for vertex**; redo works; a new operation clears the redo branch; **a refused operation pushes nothing**. |
| snap step | Rounds to the nearest multiple, both signs; a non-positive step is the identity; an already-snapped value is unchanged. |

## Owner click-tests

These are the phase-1 subset of §6 of the research spec. The bevel and extrude ones are deliberately not here — those features do not exist yet.

1. **Push keeps paint (the headline).** Load a cube STL. Paint support blockers on a *side wall* with the Support painting gizmo. Open **Edit**, mode `Face`, hover the top face — the whole face highlights amber on click. Drag the arrow up 5 mm, or type `5` and press Apply. The box is taller, the four walls followed, **and the painted blockers are still there and still on the same wall**. Repeat with MMU colours and with a seam: all three survive.
2. **Face hover.** Hover across the top face — the whole face stays highlighted, it does not flicker to one triangle. Move onto a side wall: the highlight jumps to that whole wall.
3. **Curved face.** Load a cylinder (or any part with a filleted face). Mode `Face` on the curved wall gives one or two triangles; switch to `Curved face` and it grows across the curvature. Raise `Total curvature` and watch it swallow more of the cylinder; lower it and watch it shrink.
4. **Edge chain.** Mode `Edge chain` on a cube: hover a top edge — **exactly that one edge** lights up (every cube corner is a valence-3 corner, so the chain cannot extend). On a cylinder, hover the top rim: **the whole rim lights up as one closed chain**. Drop `Edge angle` toward 1 deg on the cylinder and the wall seams start joining in; raise it past 90 and the rim stops being a feature.
5. **Snap.** With `Snap` on and `Step` 0.5 mm, drag the top face slowly: the distance ticks in 0.5 mm steps. Turn Snap off and it moves continuously. Type `5` in the field with Snap on at step 0.5 and press Apply — the result is the same mesh as dragging to 5.00.
6. **Refusals.** On a 10 mm cube, select the top face and type `-1000`, Apply: **nothing changes** and the panel says the distance is larger than the part. For the self-intersection guard you need a part with something to push *into* — an L-bracket or any part with a pocket: select the pocket's inner wall and push it past the outer wall. Nothing changes, and the panel says the push would make the part intersect itself. (Pushing a plain cube's top face straight down will *not* trigger it — that only inverts the box, it does not cross any triangles. See deviation 9.)
7. **Whole-part refusal.** Switch to `Curved face`, raise `Total curvature` to 90 and click a cube face until the region covers everything — Apply says to use the Move gizmo.
8. **Undo.** Push the top face 5 mm, then another 5 mm, then Ctrl+Z twice: the cube is back to 10 mm exactly. Ctrl+Y once: back to 15 mm. Push again: the Redo button greys out. Do ten mixed pushes and undo them all — **one stack entry per operation**, no double steps, no skipped ones.
9. **Esc.** With a face selected, Esc clears the selection and the gizmo stays open. Press Esc again with nothing selected and the gizmo closes as usual. Mid-drag, right-click or Esc puts the face back where it started.
10. **Slice.** After a push, slice the part: no errors, and the walls that stretched are printed at their new height.
11. **Dark mode.** Toggle the dark theme and confirm the toolbar icon swaps to `toolbar_edit_dark.svg` and stays legible.
