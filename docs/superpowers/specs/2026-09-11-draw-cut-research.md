# Draw cut mode — research + design

Status: research only, no code. Branch `feat/ultra-preferences`. Companion to
`2026-09-08-curved-cut-research.md` (phases 1–4), whose sheet, fit, resample, preview, connector
frame and undo stack this builds on. Section 8 is a separate, much smaller item: non-square control
grids for the Curved sheet.

**Naming note.** The brief refers to `2026-09-09-curved-cut-*.md`, `2026-09-10-curved-cut-*.md` and
`2026-09-11-curved-cut-nags`. Those files do not exist: the entire curved-cut history (research →
phase 4) lives in the single `2026-09-08-curved-cut-research.md`, which is what this spec reads
against. Flagging it so the owner knows nothing was skipped.

## Recommendation (read this first)

Draw is a **third surface mode**, a peer of Flat and Curved, not a variant of the sheet. The sheet
is a height field `z = f(u,v)` over the whole cut plane; a drawn stroke is a *curve*, and the cut
surface it generates is a **ruled strip swept along that curve**, which is not single-valued over
the plane and therefore cannot be a `CurvedCutSheet`. Trying to force it into one (rasterising a
stroke into a height field) loses exactly the thing the feature is for — undercuts and draft angles.

So: a new `DrawCutStroke` type in `src/libslic3r/DrawCut.{hpp,cpp}`, a new
`draw_cut_split()` that builds a **closed cutter solid** from the stroke and hands it to the *same*
`curved_boolean()` Manifold→mcut chain `curved_cut_split()` already uses. Everything downstream —
`process_volume_*`, `add_cut_volume`, `post_process`, kerf, side visibility — is reused unchanged.

The single most important structural finding: `is_curved_surface()` is
`m_curved_surface && CutMode(m_mode) == CutMode::cutPlanar` (`GLGizmoCut.hpp:597`), i.e. Curved is a
*sub-mode of Planar* driven by a **bool**, with only 13 uses of `m_curved_surface` in the .cpp. Draw
turns that bool into a 3-valued enum. That is a small, contained change.

---

## 1. Interaction

### 1.1 Stroke capture

Both halves already exist; neither needs inventing.

**Raycast per sample.** `MeshRaycaster::unproject_on_mesh(mouse_pos, trafo, camera, position,
normal, clipping_plane, facet_idx, sinking_limit)` (`MeshUtils.hpp:178`) returns **position, triangle
normal and facet index** — exactly what a stroke sample needs. The cut gizmo already calls it this
way (`GLGizmoCut.cpp:1891`) against a raycaster cached in `update_curved_sheet_raycaster()`
(`:1863`). Draw builds the same cached raycaster but over the **object** mesh, which
`curved_instance_mesh_in_plane()` (`:1175-1199`) already produces in the cut-plane frame — it merges
every `is_model_part()` volume through
`(translation_transform(m_plane_center) * m_rotation_m).inverse() * inst_matrix * mv->get_matrix()`.
Right frame, right volume filter, one existing call. Cache it per gesture the way
`m_curved_snap_mesh` is (built on `RightDown` `:2297`, cleared on release `:2284`); re-deriving per
motion event stalls on a heavy model.

Gotcha: `unproject_on_mesh` returns **false when the hit count is odd** (nearest hit would be from
inside the mesh), so a ray starting inside the part is refused, not clamped. `closest_hit()`
(`MeshUtils.hpp:208`) drops the sinking/parity filter if drawing into a cavity ever needs it.

**Gap filling.** `GLGizmoPainterBase::get_projected_mouse_positions()` (`GLGizmoPainterBase.cpp:403-417`)
inserts extra seeds when the mouse jumps more than `resolution` px since `m_last_mouse_click`
(callers pass `1.`). Without it a quick flick leaves a metre-long chord. Two caveats: the list is
**newest-first** (current, then backwards, then `m_last_mouse_click`) so a chronological stroke must
reverse each group; and `m_last_mouse_click` is re-set **only on an actual hit**, which is what makes
a miss non-fatal. Note `GLGizmoSculpt` does **not** interpolate — one `continue_stroke` per wx
`Dragging` — because a sphere brush closes its own gaps. A thin line has no such forgiveness, so
**Draw must take the painter-base path, not the sculpt one.** That is the key reuse decision here.

**Events**, mirroring `gizmo_event(SLAGizmoEventType, mouse_position, shift, alt, ctrl)`
(`GLGizmoPainterBase.cpp:588`; enum at `GLGizmosCommon.hpp:21` — the `GLGizmos.hpp:8` copy is stale):

- `LeftDown` on the object → clear stroke, start capture, set `m_button_down` (the painter's guard so
  a click that misses the mesh does not capture the mouse).
- `Dragging` → interpolate, raycast each seed, append `{Vec3d pos, Vec3d normal, size_t facet}`.
  Misses are **skipped, not terminating**: crossing a hole or the silhouette must not end the stroke.
- `LeftUp` → resample, smooth, decide open/closed, rebuild preview, push **one undo entry per
  completed stroke** — the granularity both `push_curved_undo()` (`GLGizmoCut.cpp:2199`) and
  Sculpt's mouse-up snapshot (`GLGizmoSculpt.cpp:486`) use.

Samples are stored in the **cut-plane frame** (`m_rotation_m.inverse() * (hit - m_plane_center)`, the
transform at `GLGizmoCut.cpp:1904`), not world — the frame the cut runs in, so the stroke survives a
plane nudge as the sheet does.

**A trap.** `get_projected_mouse_positions()` does not stop at hits: at `:442-516` it fits a plane,
projects to 2D, runs `Polyline::simplify()`, projects back and re-attaches via `get_closest_facet()`.
That is the codebase's closest thing to a surface polyline and it is **not** what Draw wants — points
come back as *plane* points never re-projected onto the mesh (only the facet is re-derived), and the
plane fit degenerates on exactly the curved strokes Draw exists for. **Reuse the interpolation loop
(`:406-429`), not the plane-fit tail.** Otherwise nothing builds a click-by-click surface polyline;
the nearest kin is `GLGizmoText` (`GLGizmoText.hpp:71-72`), which walks a surface re-raycasting each
glyph and keeping the normal — the right kernel, driven by one click rather than a drag.

### 1.2 Resample, smooth, closing

- **Resample** to fixed arc length (default **1.0 mm**). `Polyline::equally_spaced_points` and
  `simplify` (Douglas-Peucker) are **2D integer `Points`** (`Polyline.hpp:118-119`), and `Polyline3`
  (`:290`) is a **stub declaring only `lines()`**. So there is **no 3D resampler in this codebase**:
  write `draw_cut_resample(samples, spacing)` (~30 lines, walk arc length, lerp position and normal,
  renormalise) in `DrawCut.cpp` where a test reaches it. Do not inherit
  `equally_spaced_points`' wart — it emits the first point but not reliably the last; append the
  endpoint explicitly.
- Likewise **no Catmull-Rom over a space curve** exists: the only spline is
  `CurvedCutSheet::evaluate()`, a scalar height field, its basis unfactored inside `CurvedCut.cpp`.
  If phase 2 control points want a spline through 3D points, extract it or write one — budget it.
- **Smooth**: N windowed-average passes over positions and normals (panel 0–10, default 2). A raw
  stroke is faceted — it picks up every triangle normal — and an unsmoothed normal field visibly
  kinks the ruled surface. Re-project after each pass with
  `MeshRaycaster::get_closest_point(point, normal*)` (`MeshUtils.hpp:221`), which refreshes the
  normal cheaply and keeps samples on the surface.
- **Closed vs open**: closed when the last sample is within `max(3 × spacing, 2 mm)` of the first;
  snap it onto the first so the ring has no seam. The panel states which it chose and offers
  **Force closed** for a stroke that nearly met.

### 1.3 Editing the line afterwards

Phase 2. Hundreds of samples are not handles: keep a sparse **control point** set (every k-th sample,
or a DP reduction), draw those, and reuse the cut gizmo's own idiom — the curved sheet's handles sit
**outside `GrabberID` entirely**: `curved_pick_control()` (`GLGizmoCut.cpp:2090-2114`) is a
screen-space nearest-point test (`CameraUtils::project`, 18 px radius) returning a flat index, and
`render_curved_control_points()` (`:1970`) does its own `glClear(GL_DEPTH_BUFFER_BIT)` so handles
float above the surface. Mirror `m_curved_hover_ctl` / `m_curved_drag_ctl` (`GLGizmoCut.hpp:168-170`),
decoding `i = id % n`. Also copy the **absolute-drag** contract (`:173`, `m_curved_drag_grid`): each
tick restores the gesture-start state then re-applies once, so returning the cursor undoes the edit
instead of accumulating.

Dragging a control point re-raycasts onto the mesh (the point stays **on the surface**); the stroke is
then re-fit and re-smoothed. Insert = ctrl-click on the ribbon; delete = right-click a control. Esc
cancels an in-progress stroke, or clears a finished one (pushing undo).

Ctrl+Z/Ctrl+Y go through `on_cut_char()` (`GLGizmoCut.cpp:2234-2256`, wired from
`GLGizmosManager.cpp:830`), which returns false to let the plater's undo through. **It gates on
`is_curved_surface()` at `:2241`, so Draw gets no Ctrl+Z until that widens** — easy to miss, and the
symptom (Ctrl+Z undoing a *model* action instead of a stroke) reads as confusing, not broken. Copy the
no-op suppression too (`:2280`, `:2335`): pop the entry if values are unchanged at release.

### 1.4 Rendering the stroke

A polyline exactly on the mesh z-fights. The codebase solved this twice; **use the first, do not
invent a third**:

- **`mm_contour` shader bias (preferred).** `TriangleSelectorGUI::render_paint_contour()`
  (`GLGizmoPainterBase.cpp:1775`) draws a `Lines` GLModel of on-surface edges, biased **in clip space
  in the vertex shader** — nothing here uses `glPolygonOffset` for this.
  `resources/shaders/140/mm_contour.vs`:

  ```glsl
  vec4 clip_position = projection_matrix * view_model_matrix * vec4(v_position, 1.0);
  clip_position.z -= offset * abs(clip_position.w);
  ```

  `abs(w)` makes the nudge depth-independent; `offset` is `is_mesa() ? 0.0005 : 0.00001`. Copy the
  shader-swap dance (`:1777-1779`, `:1798-1799`): stash the current shader, `stop_using()`, restart.
- **Geometric normal offset** for a ribbon with width: `GLGizmoSculpt::render_mask_overlay()`
  (`GLGizmoSculpt.cpp:913`) displaces geometry `0.02f` mm along the face normal, `gouraud`,
  `GL_BLEND` on, `GL_CULL_FACE` off. Gotcha noted there: `gouraud` reads `clipping_plane`, `z_range`,
  `slope.actived`, `volume_world_matrix`, `volume_mirrored` **unconditionally**, so a gizmo without
  `ObjectClipper` must still feed dummies.
- Draw the stroke again with `glDisable(GL_DEPTH_TEST)` in a dim colour (the
  `render_cursor_circle()` treatment, `:187`) so a closed loop's hidden half still reads as a ring.

The **cut-surface preview** matters more than the stroke: render the ruled surface with the existing
per-side alpha uniforms (`curved_cut_side_alpha`, Visible/Ghost/Hidden). `render_clipper_cut()`
(`GLGizmoCut.cpp:3529`) already bypasses `MeshClipper` for a bent sheet (it slices at a single z) via
`render_curved_cap()`; a drawn cut needs the same bypass, its cap being the ruled surface clipped to
the object.

**Never rebuild the cut preview mid-stroke.** Adopt both existing debounces:
`m_curved_fit_pending` / `request_curved_fit()` (`GLGizmoCut.hpp:243-252`) defers expensive work to
gesture end, and `m_curved_cap_stale` (`:236-238`) shows "Cut face updating…" instead of recomputing.
Draw is cheaper than Sculpt here: it never moves vertices, so the raycaster and its AABB tree stay
valid all stroke — none of Sculpt's stale-tree caveats (`2026-09-07-sculpt-mode.md:141`: a 48–213 ms
rebuild paid once at mouse-up) apply. Rebuild the light ribbon every tick; rebuild the ruled surface
only on `LeftUp` and on a parameter change.

### 1.5 Panel

Added to `render_curved_surface_inputs()` (`GLGizmoCut.cpp:2419`), whose Flat/Curved
`bbl_radio_button` pair (`:2429-2451`) becomes three:

| Control | Range / values | Default | Notes |
|---|---|---|---|
| **Surface** | Flat / Curved / **Draw** | Flat | third radio; see §1.6 |
| **Direction** | Surface normal / View / Axis X / Y / Z | Surface normal | combo |
| **Angle** | −60°…+60°, signed | 0° | Direction = Surface normal only; 0 = straight in |
| **Extension** | 0…50 mm | 5 mm | how far the surface reaches *past* the stroke |
| **Depth** | Through all (checkbox) or 0…bbox mm | Through all | how far it reaches *in* |
| **Smoothing** | 0…10 passes | 2 | |
| **Thickness** | 0…20 mm (`CutThicknessMin/Max`) | 0 | existing kerf, rendered by `render_cut_thickness_input()` (`:2558`, called `:4930`) — already outside the surface block because it applies to every mode |
| **Show halves** | Visible / Ghost / Hidden per side | Visible | `render_side_visibility_inputs()` (`:2379`), already called before the `!m_curved_surface` early-out at `:2457` so it serves all modes |
| Clear stroke | button | | pushes undo |

Control points / Bend radius / Falloff / Smooth (sheet controls) hide in Draw mode.

### 1.6 The mode flag

`m_curved_surface` is a **bool** (`GLGizmoCut.hpp:156`) and `is_curved_surface()` is
`m_curved_surface && CutMode(m_mode) == CutMode::cutPlanar` (`:597`) — Curved is a *sub-mode of
Planar*, not a `CutMode` value. Adding Draw as a `CutMode` would drag in the groove/connector
branching everywhere; adding it as a third radio does not. So:

```cpp
enum class CutSurfaceMode { Flat, Curved, Draw };
CutSurfaceMode m_surface_mode{ CutSurfaceMode::Flat };
```

keeping `is_curved_surface()` as `m_surface_mode == Curved && CutMode(m_mode) == cutPlanar` and adding
`is_draw_surface()`. The ~26 affected sites are mechanical: cpp 1203, 1302, 1521, 1696, 1838, 1854,
1880, 1947, 2201, 2212, 2225, 2241, 2260, 2432–2457, 2853, 3292, 3529, 3843, 3848, 4611, 4630, 5721,
5894. Most are `is_curved_surface() && !is_flat()` guards Draw wants to share. Curved's behaviour
stays bit-identical, and this is the whole of the mode plumbing.

Two session-state hooks to extend alongside: `invalidate_curved_sheet()` (`GLGizmoCut.hpp:608`) sets
all four dirty flags in one call — give Draw the same helper; and `on_set_state()` (`:2848-2880`)
resets everything on gizmo open *and* close, which is where a stroke must also be cleared.
## 2. Geometry

### 2.1 Per-sample ray direction

For sample `i` with position `p_i` and smoothed surface normal `n_i`, and stroke tangent
`t_i = normalize(p_{i+1} − p_{i−1})`:

- **Direction = View**: `d_i = −view_dir` (constant). **Axis X/Y/Z**: `d_i = ±axis` (constant).
  Constant-direction modes are the "as-is" extrusion: a ruled surface through the stroke, and they
  are the phase-1 scope.
- **Direction = Surface normal, Angle = 0**: `d_i = −n_i` (straight in along the inward normal).
- **Angle ≠ 0**: rotate the inward normal **toward the stroke's outward binormal**
  `b_i = normalize(t_i × n_i)` (sign fixed so `b_i` points *away* from the enclosed region for a
  closed stroke — see §2.5):

```
d_i = cos(θ)·(−n_i) + sin(θ)·b_i
```

A positive θ flares the cut outward (a draft/taper, the plug gets wider going in); negative
undercuts. `b_i` is well-defined wherever `t_i` and `n_i` are not parallel, which is everywhere a
stroke is drawn *on* a surface rather than *along its own normal*.

The strip must be **frame-continuous**: `b_i` computed pointwise flips sign across an inflection.
Parallel-transport `b` along the stroke from `b_0` (pick the branch that keeps `b_i · b_{i−1} > 0`)
and, for a closed stroke, check the holonomy after the loop — if the transported frame comes back
flipped, the stroke is non-orientable on this surface (a Möbius path); warn and fall back to θ = 0.

### 2.2 The ruled strip and the closed cutter

Per sample, two rail points:

```
out_i = p_i + E · b_i          (outward by Extension, E)
in_i  = p_i + D · d_i          (inward by Depth D, or through-all)
```

Through-all means `D = 1.05 · bbox.diagonal()` — large enough to exit any side, which is the same
"reach past the object" slack `curved_cut_split()` uses (`CurvedCut.cpp:~680`, `1.05 × extent + 1`).
The outward rail is also lifted **outward along `n_i`** by E so the surface starts *above* the
surface it was drawn on; otherwise a stroke in a concavity has its rim buried in material and the
boolean leaves a skin.

Triangulate the strip as a quad grid between the two rails (two triangles per span), exactly the way
`curved_cut_lower_slab()` stitches its rim (`CurvedCut.cpp:606-623`).

**This strip is an open surface, and a boolean needs a closed solid.** Close it:

- **Closed stroke**: the strip is a tube-like band. Cap the **inner** rail with a fan/triangulation
  of the `in_i` ring and the **outer** rail likewise, giving a closed solid whose interior is
  "everything the plug occupies". Intersection with the object → the plug; A_NOT_B → the rest.
- **Open stroke**: extend both ends by **Extension along the stroke tangent** (`p_0 − E·t_0`,
  `p_N + E·t_N`, with their rails), then close the two end quads and the outer/inner caps. The
  extended ends are what let the surface reach past the silhouette so the boolean actually
  separates.

**Kerf (Thickness > 0)**: build the cutter **twice**, offset by `face_lo` and `face_hi` from
`curved_cut_thickness_faces(thickness, offset, lo, hi)` (`CurvedCut.hpp`), offsetting each rail
along its own `b_i`/normal rather than along a global Z — the same "measure along the local frame"
rule phase 4 established for connectors. The lower half is cut by the `face_lo` solid, the upper by
the `face_hi` one, so the band between belongs to neither. `t == 0` builds one solid and runs
exactly the phase-2 boolean pair.

### 2.3 Which path: `curved_cut_split` or the plane path

**Neither, directly — but reuse the core.** `curved_cut_split()` is bound to a `CurvedCutSheet`
(it calls `curved_cut_lower_slab(sheet, …)` and `sheet.max_displacement()`); the plane path
(`cut_mesh()`) is a half-space slice and cannot express a drawn boundary at all. So:

```cpp
bool draw_cut_split(const indexed_triangle_set& mesh,
                    const DrawCutStroke& stroke, const DrawCutParams& params,
                    indexed_triangle_set* upper, indexed_triangle_set* lower,
                    double thickness = 0.0,
                    CutThicknessOffset offset = CutThicknessOffset::Centred);
```

which **reuses, verbatim**, the four hard-won behaviours of `curved_cut_split()`
(`CurvedCut.cpp:654-760`):

1. the `curved_boolean()` Manifold→mcut chain with the merge of multi-part results;
2. the **negative-signed-volume winding flip** (`its_volume(object.its) < 0` → swap indices) —
   without it an inward-wound import silently returns one half;
3. **both sides always run**, and a side that failed is recovered as the complement
   (`object A_NOT_B kept`), disabled when a kerf is in play;
4. the kerf's two-solid construction.

Cleanest implementation: factor those into a shared `cut_with_solid(mesh, cutter_lo, cutter_hi, …)`
in `CurvedCut.cpp` and have both `curved_cut_split()` and `draw_cut_split()` call it. That is a pure
refactor with the existing `[CurvedCut]` suite as its regression net.

Plumbing mirrors the curved path exactly: `Cut::perform_with_draw_stroke(...)` alongside
`perform_with_curved_sheet()` (`CutUtils.cpp:584`), and `process_volume_draw_cut()` /
`process_solid_part_draw_cut()` cloned from `process_volume_curved_cut()` (the only line that
changes is the `curved_cut_split` call). Flexi joints dispatch out to `perform_with_flexi_joints()`
from the Draw entry point the same way (`CutUtils.cpp:608`).

### 2.4 Self-intersection

The ruled surface self-intersects where the offset rails fold — a **concave** stroke turning with
radius `r` less than the inward reach, or a large |θ| doing the same. Two defences:

- **Detect**: the strip folds at sample `i` when the local turning radius in the ruled direction is
  smaller than the offset. Compute the in-plane curvature `κ_i` of the stroke projected onto the
  plane spanned by `b_i`/`d_i`; a fold occurs when `E · κ_i > 1` (outward rail) or when adjacent
  `in_i` segments reverse orientation (inward rail, the θ-driven case). Both are one pass.
- **Clamp**: reduce E (and |θ|) locally toward the fold-free limit `1/κ_i`, tapering over a few
  samples so the surface stays smooth, and warn in the panel ("Extension/Angle reduced near a tight
  corner"). Advisory, matching the curved panel's habit of warning rather than refusing.

Manifold is fairly tolerant of a slightly self-intersecting cutter, and mcut is the fallback, so a
missed fold degrades to "boolean failed → complement recovery" rather than to a wrong cut. But the
clamp is what keeps the common case (a hand-drawn wobbly loop with 5 mm extension) clean.

### 2.5 Which side is "upper"

Define it from the **cutter solid's own orientation**, not from the plane:

- **Closed stroke**: the cutter solid encloses a plug. `upper` = the **plug** (the
  `INTERSECTION` result), `lower` = the remainder (`A_NOT_B`). "Upper" = the piece the stroke drew
  around. The outward binormal `b_i` is oriented so it points *away* from the loop's interior:
  compute the loop's signed area in its best-fit plane (Newell normal `N`), and set
  `b_i = normalize(t_i × n_i)` flipped so `b_i · (p_i − centroid) > 0`. Winding decides, so a loop
  drawn clockwise and one drawn counter-clockwise give the **same** plug — the user should not have
  to care about drag direction.
- **Open stroke**: the strip cuts the part in two with no inside/outside. Use the plane's own
  convention: the half on the `+Z` side of the cut plane frame is `upper`, matching flat and curved.
  A **Swap sides** button (and the existing right-click flip gesture) covers the rest.

The **sign of Angle** is tied to the same `b`: positive θ tilts toward `b`, i.e. the plug gets
*wider* as it goes in (an undercut that locks), negative makes it a draft that lifts out. Say this
in the tooltip, because it is the only thing about the feature that is not self-evident on screen.

---

## 3. Degenerate and edge cases

| Case | Behaviour |
|---|---|
| **Stroke crosses itself** | Detect with a segment-intersection pass over the projected stroke. A self-crossing closed loop has no well-defined interior → refuse the cut, warn ("The line crosses itself"). Phase 2 could split it into sub-loops; phase 1 refuses. |
| **Stroke leaves the mesh** | Misses are skipped, not fatal (§1.1). If the gap between consecutive *hits* exceeds a few spacings, the stroke has jumped across empty space: keep the longest contiguous run and warn, rather than bridging a chord through air. |
| **Very short stroke** | Fewer than ~4 samples after resampling, or arc length below ~2 mm → no cut surface; `can_perform_cut()` returns false and the panel says "Draw a line on the model". Same gate shape as `has_valid_groove()` (`GLGizmoCut.cpp:5506`). |
| **Multiple objects** | Only the selected instance is drawn on and cut, exactly as Curved does — the pick raycaster is built from the same instance mesh `curved_instance_mesh_in_plane()` (`GLGizmoCut.hpp:255`) supplies. Strokes landing on another object are ignored (the raycaster does not contain it). |
| **Open stroke that does not reach an edge** | The cutter does not separate the part; the boolean returns the whole object on one side and nothing on the other. Detect **before** cutting, cheaply: the empty-side sign test, the analogue of `curved_cut_empty_sides()` (`CurvedCut.hpp`) — if one side has no vertices, warn ("This line does not reach across the part — increase Extension or extend the line") and disable the cut. This is the phase-3 empty-side warning, reused. |
| **Depth exits through a hole** | Legitimate and works: the cutter is a solid and the boolean handles the topology. The result may be more than two connected components (a ring cut through a torus). `curved_boolean()` already merges multi-part boolean output (`CurvedCut.cpp:643-646`), and `post_process` handles the rest; note it in the panel only if the piece count surprises. |
| **Stroke on a steep silhouette** | Samples near the silhouette have normals nearly perpendicular to the view; the inward ray can graze. Mitigated by smoothing the normal field; no special case. |
| **Flat sheet analogue** | There is no "zero" stroke that degenerates to the flat cut — an empty stroke is simply "no cut". Draw has no bit-identity invariant to preserve, which is why it is a peer mode rather than a sheet variant. |

---

## 4. Connectors

**Phase 2, and mostly free.** Phase 4 of the curved work already generalised the connector frame
from "one shared `m_rotation_m`" to "a rotation built from the local surface normal", and it did so
*in the connector path*, not in the sheet: `curved_cut_sheet_frame()` produces a rotation, and
`perform_with_curved_sheet()` notes that "nothing in the connector path itself has to know about the
sheet" because the frame rides in through the volume's transform (`CutUtils.cpp:~630`). Draw needs
the same three functions against the **ruled surface** instead of the height field:

- `draw_cut_surface_point(stroke, params, s, w)` — the point at arc-length `s`, ruled parameter `w`.
- `draw_cut_surface_normal(...)` — `normalize(t × d)`, the strip's own normal; continuous because
  the frame is parallel-transported (§2.1).
- `draw_cut_surface_frame(...)` — local Z = that normal, local X = the plane's X projected onto the
  tangent plane, same construction and same degenerate guard as `curved_cut_sheet_frame()`.

Then **Plug / Dowel / Snap / DoubleRing / BallSocket / ChainLink** work unchanged — they are solids
of revolution about the local normal. The existing advisories transfer directly:
`CurvedConnectorTiltWarnDeg` (60°) for a connector printing at an angle, and the flat-patch test.
For **Hinge and Thread**, the existing code does not refuse — it *warns* below
`CurvedConnectorFlatPatchFactor` (3×) the connector's extent
(`curved_cut_patch_is_flat_enough()`), and a ruled surface is **developable along the rule
direction** (zero curvature that way), so a hinge aligned with the rules sits flatter than it would
on a dome. Keep the same warning, computed from the strip's cross-rule curvature only.

**Flexi joints**: same rule as the curved cut — a flexi cut cannot also be a drawn one, because the
joint's two segments are separated by its own gap between two flat faces. It dispatches to
`perform_with_flexi_joints()` and takes only the frame from the drawn surface.

The out-of-contour test needs the Draw analogue of the `(u,v)`-domain check: a connector must sit
within the strip's own `(s,w)` domain, and for a closed stroke also inside the plug. Phase 2.

**Phase 1 ships connector-free**, matching how the curved cut shipped its phase 1. (Connectors are
*enabled* on a curved cut today — phase 4 turned them on; the gate at `GLGizmoCut.cpp:4944` is the
flat cut's own `!m_keep_upper || !m_keep_lower || m_keep_as_parts || …` condition with nothing
curved-specific in it. Note the **reverse** gate that Draw inherits: once connectors exist,
`has_connectors` disables the surface controls (`:4933`) and the mode combo (`:4903`), so you
cannot switch Flat↔Curved↔Draw with connectors placed.)

Stale-comment warning for whoever implements this: `CutUtils.hpp:83` still says "No connectors on a
curved cut in phase 1", which is no longer true. Do not take it as the contract.

---

## 5. Serialization

**The stroke does not need to reach the 3MF, and the existing per-kind path has no slot for it.**

`Metadata/cut_information.xml` (`bbs_3mf.cpp:190`; written by `_add_cut_information_file_to_archive`
`:7508`, read by `_extract_cut_information_from_archive` `:2624`) is a **per-connector-volume** file:
it walks `mo->volumes` and writes only for `volume->is_cut_connector() || cut_info.is_flexi_joint()`
(`:7539-7541`), carrying `cut_id`/`check_sum`/`connectors_cnt` plus per-connector type and
tolerances. There is **no per-object cut-geometry record at all**, and `grep -rn "curved"` over
`src/libslic3r/Format/` returns nothing. Phase 4 confirmed the same for connectors: what persists is
the baked **volume** and its transform, not `cut_connectors`. The cut plane has never persisted;
neither does the sheet. `on_set_state()` (`GLGizmoCut.cpp:2848-2880`) resets it all on open and close.

Draw inherits that contract: **the cut is destructive, the stroke is session state.** A reopened
project has two solid halves with the drawn surface baked into their meshes and connectors standing
on their baked frames — no new 3MF fields.

A re-editable drawn cut is a *separate* feature: it would need a new per-object record (`draw_stroke`
with mode, angle, extension, depth, smoothing and the samples in the cut-plane frame) in a **new**
metadata file, not bolted onto the per-connector one. It would be the first persisted cut geometry in
the tool's history and should be designed for the sheet and the stroke together. Flag, do not build.

**Undo** extends the gizmo-local stack. `CurvedSheetState` (`GLGizmoCut.hpp:195-200`) gains stroke
fields or becomes a variant: one entry per completed stroke, per control-point drag, per Clear, per
lossy parameter change; pushed *before* the change, consumed by the same `on_cut_char()` hook so
Ctrl+Z reaches the plater only when the gizmo stack is empty. `CurvedUndoLimit` (64) is fine if the
entry stores the *resampled* stroke rather than raw samples. Also check `on_load`/`on_save`
(`GLGizmoCut.hpp:503-504`) — the gizmo's cereal hooks — when adding Draw state.

---

## 6. Tests

Pure geometry, headless, in `tests/libslic3r/test_draw_cut.cpp` (register in
`tests/libslic3r/CMakeLists.txt` next to `test_curved_cut.cpp:20`), tag `[DrawCut]`. The
`[CurvedCut]` suite (3219 lines, ~45 cases) is the model: volume conservation, closedness, and an
analytic property per feature.

1. **Closed circle on a cube's top face, Direction = Surface normal, Angle 0** → the plug is a
   cylinder: `upper` volume ≈ `π r² h` within 2%, its cross-section at several heights is a circle
   of radius `r ± 0.05 mm`, and `vol(upper) + vol(lower) ≈ vol(cube)` within 1e-3 relative.
2. **Angle = 30° gives a taper.** Same circle: the plug's radius at depth `d` is `r + d·tan(30°)`
   within tolerance — measure at two depths and check the slope. Sign check: −30° gives the
   opposite slope. This is the test that proves the binormal rotation is right.
3. **Open line across a box splits it in two**, with Extension large enough to clear both sides:
   both halves non-empty, both closed (`its_num_open_edges == 0`), volumes sum to the original.
   And the **negative control**: the same line *too short*, with Extension 0 → the empty-side test
   reports a side empty and the cut is refused.
4. **Thickness leaves a gap.** `t = 2 mm` on the closed-circle case: `vol(upper) + vol(lower)` is
   less than the cube by the band's volume (within 2%), and the minimum distance between the two
   halves' facing surfaces is `2 mm ± 0.05`.
5. **Resample and smooth are sane.** A synthetic stroke: resampling to 1 mm gives equal spacing
   within 1e-6 and preserves arc length within 0.5%; smoothing is idempotent-ish (N passes then N
   more changes the result by less than the first N did) and never moves a sample more than a bound.
6. **Winding independence.** The same circle traversed clockwise and counter-clockwise produces the
   same plug (volumes equal within 1e-6, bounding boxes equal).
7. **Self-intersection clamp.** A tight concave corner with a large Extension: the clamp fires, the
   generated cutter has no self-intersecting faces, and the cut still produces two closed halves.
8. **Constant-direction modes.** Direction = Axis Z on a closed stroke over a sloped face gives a
   prism whose cross-section is constant in Z (measure at two heights).
9. **Refactor regression.** After factoring `cut_with_solid()` out, the whole existing `[CurvedCut]`
   suite must pass unchanged — that is the proof the shared boolean core did not shift.

**Demo** (`[.demo]`, matching the curved suite's habit): export `draw_plug_upper.stl` /
`_lower.stl` for the circle-on-a-cube and the 30° taper, behind an env var.

**Owner click-tests** (nobody has clicked any of this; the curved spec's "Unverified" section is the
cautionary precedent):

- Draw a closed loop on a curved surface (a benchy hull, not a cube) and confirm the ribbon follows
  the surface without z-fighting, and the loop's hidden half still reads.
- Confirm a fast flick leaves no gap in the stroke (the interpolation).
- Cut, then check the two halves fit back together by eye; check Ghost/Hidden show the mating face.
- An open stroke drawn *not quite* across the part: confirm the warning appears and the cut is
  disabled, and that raising Extension enables it.
- Angle ±30° on the same loop: confirm the plug visibly tapers the expected way.
- Esc mid-stroke and Ctrl+Z after a stroke.

---

## 7. Phases and estimates

**Phase 1 — stroke capture + as-is direction + Extension + split + undo.**
Mode enum (13 mechanical sites) and the third radio; stroke capture with painter-style
interpolation; resample/smooth; ribbon rendering + ruled-surface preview; Direction = Surface
normal (θ = 0) / View / Axis X,Y,Z; Extension; Depth (through-all default); Thickness reused;
side visibility reused; `DrawCut.{hpp,cpp}` with the cutter builder; `cut_with_solid()` refactor out
of `curved_cut_split()`; `Cut::perform_with_draw_stroke()` + the two `process_*` clones; the
empty-side / too-short / self-crossing gates; gizmo-local undo; tests 1, 3, 4, 5, 6, 8, 9.
No connectors, no line editing, no angle.

**Estimate: 3–4 days.** The geometry is genuinely new (cutter construction, closing, orientation,
fold clamping) and the interaction is new, but every hard part it touches — the boolean chain, the
winding flip, the complement recovery, the kerf, the preview shader, the undo stack, the raycast
idiom — already exists and is proven. Call it **3 days if the `cut_with_solid()` refactor is clean,
4 if the ruled-surface preview and the open-stroke closing need the iteration they probably need.**

**Phase 2 — angle/draft + connectors on the drawn surface + editing the line.**
The binormal frame with parallel transport and the holonomy check; the fold detect/clamp with
tapering; `draw_cut_surface_{point,normal,frame}()` and the connector `(s,w)` contour test; the
tilt and flat-patch advisories; sparse control points with drag/insert/delete. Tests 2 and 7 plus
connector cases mirroring the curved suite's.
**Estimate: 3–4 days.**

---

## 8. Non-square control grids for the Curved sheet

Separate, much smaller item. **This is well under a day** — call it **3–5 hours including tests**.
The sheet is already half-rectangular: the *domain* has independent `m_half_size_u` /
`m_half_size_v` (phase 2), and only the *grid count* is still one number.

### What changes in `CurvedCutSheet`

`m_resolution` (one int) becomes `m_nx, m_ny`. Keep `resolution()` returning `max(nx, ny)` (or nx)
and `set_resolution(int)` setting both, so every existing caller compiles and keeps its meaning —
the same compatibility trick `half_size()` already plays for the rectangular domain.

Everything that assumes square, found by auditing all 17 `m_resolution` uses in `CurvedCut.cpp`:

| Site | Line(s) | Change |
|---|---|---|
| `reset()` | 35-36 | `m_z.assign(nx*ny, 0.0)`, clamp both |
| indexing `at(i,j)` | hpp | `m_z[j*m_nx + i]` — **already** `j*res + i`, so it is correct once the stride is `nx` |
| `flip_about_u()` | 53-62 | mirror **j over ny**; stride `nx` |
| `flip_about_v()` | 68-75 | mirror **i over nx** |
| `control_u(i)` | hpp | split into `control_u(i)` over `nx` and **`control_v(j)` over `ny`** — today `control_xy()` calls `control_u(j)`, which is the core square assumption |
| `control_xy(i,j)` | 77-81 | use `control_v(j)` for the v axis |
| `set_half_size()` resample | 128-145 | loop `j < ny`, `i < nx`; use `control_v(j)` |
| `evaluate()` | 207-241 | `fu = u·(nx−1)`, `fv = v·(ny−1)`; clamp `iu ≤ nx−2`, `iv ≤ ny−2`; `z_at` clamps i to nx−1, j to ny−1. **The Catmull-Rom evaluation itself is already separable in u and v** — it does 4 row interpolations then 1 column — so it needs only the right extents, no new maths. |
| `set_resolution()` | 251-273 | gains `set_grid(nx, ny)`; resample loops over the new counts, reading the old surface through `evaluate(u,v)` — which already works for any target grid |
| `grab()` / `smooth()` | 285-320 | loops `j < ny`, `i < nx`; `smooth()`'s neighbour clamp uses nx/ny separately; the `m_resolution < 3` guard becomes `min(nx,ny) < 3`… **careful**: a 10×2 sheet has ny = 2, so the Laplacian guard must be `nx < 3 && ny < 3` or smoothing along u must still work at ny = 2 |
| `sample_sheet()` | — | unaffected: it samples by `(u,v)` at `samples×samples`, not by grid count |
| `publish_reference()` / `m_ref_z` | — | reference must carry nx, ny too, and the staleness compare already compares sizes |

`MinResolution`/`MaxResolution` (3/15) apply per axis, **except** the minimum must drop to **2** for
the ruled case the owner wants: 10 × 2 means ny = 2. A 2-row grid cannot bend along v — it tilts,
which is exactly the "ruled bend, each column controllable from either end" the owner described.
The header's own comment already says "the maths works for any n >= 2". So: `MinResolution = 2`,
and note that 2×2 is a tilted plane.

### Panel

`render_curved_surface_inputs()` (`GLGizmoCut.cpp:2461-2483`): today one `SliderInt("##curved_res",
&res, Min, Max, res_fmt)` with `snprintf(res_fmt, "%d x %d", res, res)` — note that format string
already *displays* "N x N" and must become "%d x %d" of nx and ny. Replace with:

- a **Uniform** checkbox (default on, preserving today's behaviour exactly);
- uniform on → the single slider, both counts together, unchanged;
- uniform off → **two** sliders, "Columns (X)" and "Rows (Y)", each 2…15.

Both push `push_curved_undo()` before changing (a grid change is a lossy resample — already the
rule at `:2476`) and both set `m_curved_res_user_set = true` so the auto-fit stops choosing
(`:2472`, and `curved_cut_default_resolution()` at `CurvedCut.cpp:1252`). `default_curved_bend_radius()`
is recomputed from the *smaller* spacing.

### Serialization, fit, snapping, flip, undo

- **Serialization**: nothing reaches the 3MF (§5 — the sheet is session state), so there is nothing
  to version. If a future re-editable-cut record stores the grid, it stores **two** counts, and a
  file with one count reads `ny = nx`. State that rule now so the later feature inherits it.
- **Fit**: `curved_cut_fit_extent()` and `curved_cut_fit_projection_extent()` already produce
  independent `half_size_u/v` — untouched. `curved_cut_default_resolution(hs_u, hs_v, spacing,
  min_res)` should gain a two-output form returning nx and ny from the two extents at the same
  target spacing, which is strictly better than today's single answer for a long thin part.
- **Snapping**: `curved_cut_snap_distance()` is per control point and grid-agnostic; the gizmo's
  `m_curved_snap_ctl` / `m_curved_hover_ctl` are flat indices — they must decode as
  `i = id % nx, j = id / nx`. Audit every `% res` / `/ res` in `GLGizmoCut.cpp`.
- **The flip fix**: `flip_about_u()` mirrors **along v, so it must use ny**, and `flip_about_v()`
  mirrors along u with nx. Getting these backwards on a 10×2 grid is the most likely bug in the
  whole item, and it is silent on a square grid — which is why test 3 below matters.
- **Undo**: `CurvedSheetState` (`GLGizmoCut.hpp:195`) carries `resolution`; it becomes `nx, ny`.
  `apply_curved_sheet_state()` must restore both before `set_values()`, or the values land on the
  wrong grid — the header already warns about exactly this ("restoring values against the wrong grid
  would be meaningless").

### Tests

Add to `test_curved_cut.cpp`:

1. **A 10×2 sheet is a ruled surface.** Set the two rows to different constant heights per column;
   evaluate at several v between them and confirm the height is (within Catmull-Rom's clamped-
   boundary behaviour at ny = 2, which reduces to linear) the linear blend of the two rows, within
   1e-9. Along u it interpolates its control points exactly.
2. **Resample 10×2 → 5×5 preserves shape.** Evaluate the surface at a dense set of `(u,v)` before
   and after `set_grid(5,5)`; max deviation within a stated tolerance (the u direction coarsens from
   10 to 5, so the tolerance is the Catmull-Rom resample error, not zero — measure it and pin it).
   And 10×2 → 19×3 (a refinement in both axes whose nodes include the originals) reproduces the
   control values **exactly** at the shared nodes, the property `set_resolution()` already claims.
3. **Flip on a non-square grid.** A 10×2 sheet with an asymmetric surface: `flip_about_u()` twice is
   the identity bit-for-bit, and once it satisfies `f_new(x, −y) == −f_old(x, y)` at sample points.
   Same for `flip_about_v()`. This is the test that catches the nx/ny swap.
4. **Square is unchanged.** Every existing `[CurvedCut]` case passes untouched — the compatibility
   proof for `resolution()` / `set_resolution(int)`.
5. **A 10×2 sheet cuts.** End-to-end `curved_cut_split()` on a cube with a ruled 10×2 bend: both
   halves closed, volumes sum, and the cut face is ruled (straight lines along v).

### Estimate

**3–5 hours.** It is a mechanical stride-and-extent change across ~17 sites in one file plus ~6 in
the gizmo, with the existing suite as the net. The two places that will actually bite are the
`smooth()` guard at ny = 2 and the flip axes; both have a test above aimed straight at them.
