# Draw cut — phase 1, as built

Branch `feat/draw-cut`, off `feat/ultra-preferences` at `7365985cbe`. Implements phase 1 of
`2026-09-11-draw-cut-research.md`: stroke capture, the as-is directions, Extension, Depth,
the split, the gates, gizmo-local undo and a preview. No draft angle, no connectors on the
drawn surface, no line editing past Esc / Ctrl+Z.

## What was built

### The mode is a three-valued enum, as the spec recommends

`m_curved_surface` (a bool) became

```cpp
enum class CutSurfaceMode { Flat, Curved, Draw };
CutSurfaceMode m_surface_mode{ CutSurfaceMode::Flat };
```

with `is_curved_surface()` keeping its exact old meaning (`m_surface_mode == Curved &&
CutMode(m_mode) == cutPlanar`), a new `is_draw_surface()`, and a new `is_shaped_surface()`
for the many sites that meant "not the flat plane" rather than "curved specifically". Every
one of the ~26 sites the spec enumerated was converted; Curved's behaviour is unchanged and
the 50-case `[CurvedCut]` suite passes untouched.

### `cut_with_solid()`, factored out of `curved_cut_split()`

`CurvedCut.{hpp,cpp}` gained

```cpp
bool cut_with_solid(const indexed_triangle_set& mesh,
                    const indexed_triangle_set& cutter_lo,
                    const indexed_triangle_set& cutter_hi,
                    bool kerf,
                    indexed_triangle_set* upper,
                    indexed_triangle_set* lower,
                    const char* log_tag = "Curved cut");
```

carrying all four of the behaviours the spec names — the Manifold→mcut chain with multi-part
merging, the negative-signed-volume winding flip, both-sides-always-run with complement
recovery, and the kerf's two-solid shape. `curved_cut_split()` is now "build two slabs, call
it", and `draw_cut_split()` calls the same function. Nothing about the boolean exists twice.

One incidental cleanup came with it: `curved_cut_split()` used to build a whole
`TriangleMesh` copy of the input just to read its bounding box, then hand the original to the
boolean anyway. It computes the bbox from the vertices directly now, and `cut_with_solid()`
owns the one copy the boolean needs.

### `src/libslic3r/DrawCut.{hpp,cpp}`

- `DrawCutSample` — position, surface normal and facet, all in the cut plane's frame.
- `DrawCutStroke` — raw samples in, `finish(spacing, smoothing, force_closed)` out:
  longest-run repair, resample, smooth, open/closed decision, sign-continuous binormals,
  self-crossing check. `error()` reports which gate refused it.
- `draw_cut_resample()` — the 3D resampler the spec says does not exist in this codebase
  (`Polyline::equally_spaced_points` is 2D integer `Points`; `Polyline3` is a stub). Walks
  cumulative arc length, lerps position and normal, renormalises, and **does** emit the last
  point, which is the wart `equally_spaced_points` has.
- `draw_cut_smooth()` — N windowed-average passes over positions and normals, endpoints held
  for an open stroke and wrapped for a closed one.
- `draw_cut_closing_tolerance()` — `max(3 * spacing, 2 mm)`.
- `draw_cut_cutter_solid()` — the closed cutter solid.
- `draw_cut_self_crossing()` / `draw_cut_strip_folds()` — the two gates.
- `draw_cut_empty_sides()` — the cheap "does this line separate the part" test.
- `draw_cut_split()` — builds the cutter(s) and calls `cut_with_solid()`.

### `Cut::perform_with_draw_stroke()`

Alongside `perform_with_curved_sheet()`, with `process_volume_draw_cut()` and
`process_solid_part_draw_cut()` cloned from the curved pair (the split call is the only line
that differs). Flexi joints dispatch out to `perform_with_flexi_joints()` from the Draw entry
point the same way the curved path does.

The kerf reaches the cut through `DrawCutParams::thickness` rather than as its own argument,
because a drawn cut's band is offset along the **strip's own** normal, which varies along the
stroke — there is no single plane normal for a separate `thickness` argument to mean.

### Gizmo

- Stroke capture on left-drag, using the painter base's interpolation loop and nothing else:
  seeds between the last **hit** and the current position so a fast flick leaves no chord,
  each seed raycast against a per-gesture cached raycaster over
  `curved_instance_mesh_in_plane()`. The plane-fit tail at `GLGizmoPainterBase.cpp:442-516` is
  deliberately not reused — its points are never re-projected onto the mesh.
- A miss is skipped, not terminating; a click that misses the mesh does not capture the mouse.
- The plane grabbers hide in Draw mode. Cut position / rotation inputs stay, because the
  stroke lives in the plane's frame.
- `render_clipper_cut()` returns early in Draw: MeshClipper's cap is a slice at one z, so on a
  drawn cut it would put a flat disc through the ruled surface.
- Panel: Mode radio Flat / Curved / **Draw**; Direction combo (Surface normal, View, Axis
  X/Y/Z), Extension (0–50 mm, default 5), Depth (Through all on by default, else a slider),
  Smoothing (0–1, default 0.2), Clear line. Thickness and the Visible/Ghost/Hidden side
  controls are the existing shared ones.
- The panel states whether it read the stroke as a closed loop or an open line, and warns on
  an empty side or a fold.
- Esc clears the line (mid-stroke it abandons the one being drawn and restores the previous);
  Ctrl+Z / Ctrl+Y / Ctrl+Shift+Z go through the gizmo's own stack. `on_cut_char`'s gate was
  widened from `is_curved_surface()` to `is_shaped_surface()`, which is the trap the spec
  flags — without it Draw would get no Ctrl+Z and the symptom would read as confusing rather
  than broken.
- The stroke is carried through the right-click plane flip the way the sheet is: the flip
  negates local y and z, so the samples and their normals do too. Pure arithmetic, nothing
  re-projected.
- `on_set_state()` clears the stroke on open and on close, so a new object does not open the
  gizmo with the last one's line on it.

### The preview: the cutter solid itself

**Chosen: render the cutter solid, translucent, over the model.** Neither of the spec's two
options was taken, and this is the main deviation.

The curved cut's coloured halves come from the volume shader sampling the sheet's height field
out of a texture. That needs the cut surface to *be* a height field over the plane,
single-valued in `(u,v)` — which a ruled strip swept along a curve is precisely not, so the
colour-clip approach cannot be reused at all (the spec's "if the ruled solid can be expressed
as a height field" is answered: it cannot).

The spec's fallback was to run the real split in a background job on stroke end and preview
the resulting half meshes with a spinner. Rendering the cutter instead is more robust and
strictly cheaper:

- It **is** the surface the boolean will use — the same `draw_cut_cutter_solid()` call, not a
  re-derivation — so what the user sees is what they get.
- No boolean, therefore no job, no spinner, no stale-preview state, and no window in which the
  preview disagrees with the panel.
- It updates the instant any parameter changes, rather than after a few hundred milliseconds
  of Manifold per change.

What is lost: the two halves are not separately coloured, because without running the split
the shader cannot tell them apart. The Visible / Ghost / Hidden side controls still work
against the plane's own split, and ghosting the near half is what lets the shell be seen
inside the part. Colouring the halves properly is a phase-2 item and would want the background
job the spec describes.

The stroke itself is drawn as a ribbon of quads lying on the surface, biased in **clip space**
by the `mm_contour` shader (`clip_position.z -= offset * abs(clip_position.w)`), which is the
first of the codebase's two solutions to this and the one the spec says to use — no
`glPolygonOffset`, no geometric normal offset. It is drawn twice: once with the depth test off
in a dim colour so a closed loop's hidden half still reads as a ring, then on the surface in
full colour.

The ribbon rebuilds every drag tick (it is a light strip, and Draw never moves a vertex so the
raycaster's AABB tree stays valid all stroke — none of Sculpt's stale-tree caveats apply); the
cutter shell rebuilds on mouse-up and on a parameter change.

## Deviations from the spec

1. **The preview** — as above. The spec offered a colour-clip reuse or a background-job
   split; this renders the cutter solid instead.

2. **Extension is measured along the cut direction, not sideways.** The spec's §2.2 has
   `out_i = p_i + E * b_i` (outward along the binormal) plus a lift along `n_i`. Implemented
   that way first, and it silently turned every closed cut into a draft: the strip then runs
   from radius `r + E` at the top to radius `r` at the bottom, so a 12 mm circle with 5 mm
   extension cut a truncated cone of 1.65x the intended volume instead of a cylinder. At
   phase 1's angle of zero there is no draft, so both rails lie on the ruling line through
   `p_i` along `d_i`:

   ```
   out_i = p_i - E * d_i     (back out along the cut direction, clear of the face)
   in_i  = p_i + D * d_i
   ```

   E still does both of the jobs the spec wanted from it — it lifts the rim clear of the face
   (so a stroke in a concavity is not left with its rim buried), and for an open stroke it is
   the tangent extension past each end that reaches past the silhouette. Phase 2's draft angle
   tilts `d` itself toward the binormal, which is where the binormal earns its keep.

3. **An open stroke's cutter is a swept slab, not a capped strip.** The spec says to close the
   open strip with "the two end quads and the outer/inner caps". Capping the strip's single
   boundary loop gives a solid of **zero volume** — a flattened bag, watertight by every edge
   count and empty inside — so the INTERSECTION returns nothing and the caller gets one half.
   The fix is the one `curved_cut_lower_slab()` already uses for the sheet: sweep the surface
   to a far boundary outside the part (1.05 × bbox diagonal, along one direction for the whole
   strip), so the solid is "everything on one side of the drawn surface". A closed stroke needs
   no sweep: its band is already a tube with a real interior.

4. **The fold test's sign is the opposite of the obvious reading.** §2.4 says a fold occurs
   when `E * κ_i > 1`. That is only true where the stroke's turn centre is on the **outward**
   side — a concave corner. At a convex corner (which is every point of a circle drawn as a
   loop) the outward offset moves *away* from the centre and the rail simply gets longer; it
   cannot fold however tight the curve. Without the sign test a 3 mm circle at 5 mm extension
   would be reported as a fold, which is wrong. Phase 1 detects and warns; the clamp is phase 2
   as scheduled.

5. **An empty side disables the cut, rather than warning and proceeding.** The curved cut warns
   and produces the one half that has material, which is sensible for a plane that misses part
   of an object. A stroke that does not separate the part gives back the whole part and nothing
   else, which is not a cut at all — so `can_perform_cut()` returns false and the panel says
   "the stroke does not separate the part", the wording the brief asks for.

6. **Smoothing is a 0..1 panel value, not 0..10 passes.** §1.5 has "Smoothing 0..10 passes,
   default 2"; the brief has "Smoothing (0..1)". The brief wins: the slider is 0..1 and
   `draw_cut_smooth_passes()` maps it onto 0..10 passes, so the default 0.2 is the spec's 2
   passes.

7. **No re-projection onto the mesh after smoothing.** §1.2 suggests re-projecting each pass
   with `MeshRaycaster::get_closest_point()`. That needs a raycaster, which lives in the GUI,
   and `draw_cut_smooth()` is in libslic3r where the tests reach it. For the couple of passes
   the default asks for, the displacement is within the chord sag of the surface anyway. Worth
   revisiting in phase 2 if a heavily smoothed stroke on a small feature visibly leaves the
   surface.

## Tests

`tests/libslic3r/test_draw_cut.cpp`, tag `[DrawCut]`, registered in
`tests/libslic3r/CMakeLists.txt` next to `test_curved_cut.cpp`. 18 cases:

- the resampler: even spacing, arc length within 0.5%, the endpoint reached, a ring left open
  and evenly wrapped;
- the closing tolerance, both directions plus Force closed;
- smoothing: bounded, settling, endpoints held for an open stroke, wrapped for a closed one,
  normals unit, 0 passes a no-op, the 0..1 → passes mapping;
- the gates: too short (one sample, and a 1 mm dab), a stroke that jumps across empty space
  keeping its longest run, a figure-of-eight refused as self-crossing (with a plain circle as
  the negative control);
- folds: a concave flower folds, a plain circle never does at any extension, an open wave
  folds when tight and not when gentle;
- **the headline case**: a closed 12 mm circle on a 40 mm cube's top face, Surface normal,
  through all → two watertight parts, the plug a cylinder of `π r² h` within 5%, volumes
  summing to the cube within 1e-3, the plug spanning the full height with radius R ± 0.2;
- winding independence: the same circle clockwise and counter-clockwise give the same plug;
- an open line across the box: two watertight halves, volumes summing to the original, each
  about half, on opposite sides of the line;
- the negative control: the same line too short with Extension 0 reports a side empty, and
  raising Extension clears it;
- the kerf: 1 mm thickness costs the pair the band's volume within 2%, and leaves a 1 mm gap
  between the halves;
- Axis Z over a sloped stroke gives a prism whose section is constant in z (the centre of the
  plug's cross-section does not move between two height bands), plus the structural check that
  every inward rail point is the same translation of its stroke point;
- the cutter solid is watertight and outward-wound for both open and closed strokes, and empty
  for an invalid stroke;
- Depth stops the cut short of through-all (a 10 mm pocket in a 40 mm cube);
- `[.demo]` OBJ exports behind `SLIC3R_DRAW_CUT_DEMO`.

The whole existing `[CurvedCut]` suite passes unchanged, which is the `cut_with_solid()`
refactor's regression net.

## Click-tests for the owner

Nobody has clicked any of this. In particular:

1. **Draw on a curved surface** (a benchy hull, not a cube): does the ribbon follow the
   surface without z-fighting, and does a closed loop's hidden half still read as a ring?
2. **A fast flick** across the model: no gap in the stroke (the interpolation), and no chord
   through air where it crossed a hole or the silhouette.
3. **Cut a closed loop** and check the plug and the hole fit back together by eye. Set one
   half to Ghost / Hidden and check the mating face.
4. **An open stroke drawn not quite across the part**: the panel says "the stroke does not
   separate the part", the Cut button is disabled, and raising Extension enables it.
5. **Direction = View** with the camera at an angle, and **Axis X / Y / Z**: does the cut go
   the way the label says? The View direction is *latched* when you pick View and when you
   finish a stroke — so orbiting afterwards and then nudging Extension must **not** re-aim the
   cut. Worth confirming, because the alternative (re-reading the camera on every parameter
   change) is what the code does if the latch is ever removed, and the symptom is a preview
   that swings round for no reason the user asked for.
6. **Esc mid-stroke** and **Esc after a stroke**; **Ctrl+Z** after a stroke and after Clear
   line; **Ctrl+Y** to put it back.
7. **The right-click plane flip** with a stroke drawn: the line should stay exactly where it
   is on the model, with only which half counts as upper swapping.
8. **Thickness > 0** on both a closed loop and an open line: a real gap, of the width asked
   for, on the drawn surface rather than on a plane.
9. **Switch Flat → Curved → Draw → Curved**: the sheet should still be where it was; the
   stroke should be gone when Draw is re-entered.
10. **A heavy model** (a few hundred thousand triangles): the drag should stay at frame rate.
    The raycaster is built once per gesture, but the first press pays for it.

## Phase 2, unchanged from the research spec

The draft angle with parallel transport and the holonomy check; the fold **clamp** with
tapering; `draw_cut_surface_{point,normal,frame}()` and the connector `(s,w)` contour test;
sparse control points with drag / insert / delete; and properly coloured preview halves, which
is where the background-job split the spec describes would earn its keep.
