# Draw cut — phase 2, as built

Branch `feat/draw-cut-phase2`, off `feat/ultra-preferences` at `7614df6381` (which is phase 1
merged). Implements phase 2 of `2026-09-11-draw-cut-research.md` on top of
`2026-09-11-draw-cut-phase1.md`, whose seven deviations are taken as the truth throughout —
in particular that Extension runs along the cut direction rather than sideways, that an open
stroke's cutter is a swept slab, that the fold test's sign is the opposite of the obvious
reading, that the preview is the translucent cutter solid, and that an empty side disables the
cut.

Four things were added: the **draft Angle**, **line editing**, **connectors on the drawn
surface**, and the panel that drives them. One phase 1 deviation was closed on the way
(deviation #7, the missing re-projection after smoothing) because phase 2 made it load-bearing.

## 1. The draft Angle

`DrawCutParams::angle_deg`, signed, in degrees, clamped to `DrawCutMaxAngleDeg` (60) at every
use. It tilts the ruling away from the local surface normal toward the stroke's **outward**
binormal:

```
d_i = cos(theta) * (-n_i) + sin(theta) * b_i
```

Positive **flares outward** — the ruling leans away from the loop's interior as it goes in, so a
closed stroke's plug widens with depth and lifts out the way a moulding draft does. Negative
**undercuts** — the plug narrows with depth, so it is a dovetail that cannot be pulled straight
out. The sign rides on the same winding-independent binormal `DrawCutStroke::binormal()` already
defined, so a loop drawn clockwise and one drawn anticlockwise draft the same way.

Phase 1's file-static `inward_dir()` became the exported **`draw_cut_inward_dir()`**, because the
fold guard, the surface frame and the tests all have to ask the cutter builder's own question
rather than a re-derivation of it. At `theta == 0` it short-circuits and returns `-n_i` bit for
bit, which is what keeps every phase 1 case (and every existing user's cut) unchanged; there is a
test asserting exactly that, to 1e-15.

**Only `DrawCutDirection::SurfaceNormal` uses it.** The constant-direction modes are one
direction at every sample by definition — that is what "as-is extrusion" means — and a per-sample
tilt is precisely what they are not, so `draw_cut_inward_dir()` ignores the angle for them and
the panel greys the slider out.

### The fold guard now accounts for it

This is the part of the angle work that is not just arithmetic. Phase 1's guard is

```
E * kappa_i > 1,  scored only where the turn centre is on the OUTWARD side
```

and that is right *at angle 0 and only there*, for the reason phase 1 gives: the ruling is then
the inward normal, so pushing the outward rail back by `E` along it moves the rail straight out
of the surface and not a millimetre sideways. The only lateral reach is `E` itself.

At angle `theta` the ruling **leans sideways by `sin(theta)`**, and two things change:

1. The **inward** rail is now displaced laterally by `D * |sin theta|`, where `D` is the depth —
   which for a through-all cut is the bounding-box diagonal. That is routinely an order of
   magnitude more than `E` has ever been, and it is the reach that actually folds things.
2. It leans the **other way** from the outward rail, so the dangerous corners are the ones phase
   1 correctly ignored: a **convex** corner, where nothing reached before, now has a rail walking
   toward a centre `1/kappa` away with `D * |sin theta|` of reach to do it in. A loop drawn round
   a small boss and drafted through a thick part folds exactly there.

So `draw_cut_strip_folds()` gained two parameters (`angle_deg`, `depth`), tests against
`max(E, D * |sin theta|)`, and scores the inward side too once the ruling leans. Both defaults
are zero, so the existing calls are the phase 1 behaviour unchanged.

Detection only, as in phase 1 — the clamp-and-taper is still not built. Manifold tolerates a
slightly self-intersecting cutter and mcut is the fallback, so a fold degrades to "boolean failed
→ complement recovery" rather than to a wrong cut, and the panel says which knob to turn.

### The holonomy check

`draw_cut_frame_holonomy_flips()`. The research spec asks for a parallel-transport holonomy test
after going round a closed loop, and warns that a frame that comes back flipped means the stroke
is non-orientable on that surface. `compute_binormals()` does **not** transport a closed loop's
field — it orients it from the centroid, which is what makes it winding-independent — so the
literal test has nothing to measure. What can still go wrong is the same thing by a different
route: a loop that is not star-shaped about its centroid, or one running over a surface whose
tangent plane turns right over, ends up with **adjacent binormals of opposite sign**, and a draft
applied across that seam flares one way on one arc of the loop and the other way on the rest.

That is what is detected: a sign flip between neighbours, all the way round including the closing
span. The gizmo forces the angle to 0 and says so — the spec's "warn and fall back to theta = 0".

## 2. Line editing

An **Edit points** toggle in the panel. While it is on, the stroke's resampled points are
draggable handles with the curved sheet's gesture vocabulary: hover highlights, left-drag moves,
right-click deletes, Shift+click on a segment inserts.

A toggle rather than a modifier key, because the two gestures genuinely collide — a left-drag on
the model is "draw a new line" in one mode and "move this point" in the other, and there is
nothing in the event to tell them apart. `draw_on_mouse()` claims the editing branch **before**
the capture branch for the same reason. A click on nothing while editing does *not* start a new
stroke: that would throw away the line the user is working on for a stray click.

Three things make this different from the sheet's control points, and each is a line of code:

1. **The sheet's grid is fixed-size** — dragging a control point changes a height and nothing
   else. A stroke's points are a **list**, and insert and delete change its length, so the whole
   line goes back through `finish()` after every edit (`commit_draw_points()`). That is what
   keeps it resampled: moving a point otherwise leaves one long span and one short one either
   side of it, and the ruling density follows the spans. A closed stroke has its closing span
   re-expressed before the re-finish, or every edit would silently open the loop.

2. **A sheet control point moves along one axis.** A stroke point moves **on the surface**, so
   the drag is a raycast (`draw_point_reproject()`), not a plane projection — which is also what
   keeps the surface normal it carries correct, and the normal is what the draft angle is
   measured from. A miss does not move the point, the same "a miss is skipped, not fatal" rule
   capture follows.

3. **The preview updates live under the drag.** The stroke is *not* re-finished mid-drag (that
   would resample and walk the point out from under the cursor), so both the ribbon and the
   cutter shell read `m_draw_points` directly while `m_draw_drag_pt >= 0`; the shell is built from
   a copy of the stroke with `set_path()` applied. One `draw_cut_cutter_solid()` call per motion
   event, which is what a slider drag already costs.

Each edit is one undo entry, pushed before the change, through the same gizmo-local stack and the
same `on_cut_char()` hook the stroke and the sheet use. Esc leaves editing mode before it clears
the line — the line is the thing the user has just spent effort on, so the escalation is "stop
editing" then "throw it away".

### Re-projection after smoothing (phase 1 deviation #7, closed)

`reproject_draw_stroke_on_mesh()`, called from `refresh_draw_stroke()`:
`MeshRaycaster::get_closest_point()` over the cached instance mesh, which is already in the plane
frame, so nothing is transformed either way. Phase 1 could leave this because nothing read the
normals for anything but the cut direction at angle 0; phase 2 reads them for the draft angle and
for every connector frame, and Edit points lets a user drag a point anywhere, so the path has to
be back on the surface first.

`DrawCutStroke::set_path()` is the new accessor it needs: replace the finished path in place,
recompute the binormals, keep the open/closed decision, and **do not resample** — the path
already is one, and running the resampler over its own output would walk the samples a little
further every time a slider moved. A path of a different length is refused rather than
half-applied.

## 3. Connectors on the drawn surface

**Plug, Dowel and Snap work; the Flexi kinds are allowed and gated by the existing per-kind
footprint check rather than refused.** The reason is worth stating because it cuts the other way
from what one might guess about a swept surface:

A ruled strip is **developable along its rules** — a straight ruling has exactly zero curvature
that way — so only *one* of its two directions can curve at all. A connector on a straight
stretch of stroke is therefore sitting on a genuinely flat patch, flatter than the same connector
on a dome would be. So Hinge and Thread keep the curved cut's own advisory
(`CurvedConnectorFlatPatchFactor`, 3×) computed from the **cross-rule** curvature only, and a
stroke drawn straight enough passes it. Nothing is refused here that the curved cut would allow.
Flexi joints still dispatch out to `perform_with_flexi_joints()` from the Draw entry point, as
they did in phase 1 and as they do on a curved cut, taking only the frame from the drawn surface.

### The geometry, in libslic3r

The strip's parameters are `(s, w)`: `s` arc length along the stroke from the first path sample
(wrapping for a closed one), `w` millimetres along the ruling from the stroke itself, positive
into the part — exactly the parameter the cutter's rails use, so a point built from them lies on
the surface the boolean will use.

- `draw_cut_surface_point(stroke, params, s, w)`
- `draw_cut_surface_normal(...)` — `normalize(t × d)`, **sign-pinned to the local outward
  binormal**. A raw `t × d` flips with the drag direction and across an inflection, so two
  connectors a millimetre apart would point opposite ways.

  The first version pinned it to the same summed, one-direction-for-the-whole-strip field
  `draw_cut_cutter_solid()` derives for its sweep, and that is **wrong here** — a mistake worth
  recording because the reasoning that produces it is superficially sound. It is right for the
  cutter's sweep, which needs one direction for one slab. It is wrong for a frame, because on a
  **closed** loop the strip's normal genuinely rotates through a full turn (on a circular plug it
  *is* the radial direction, pointing a different way at every sample), so the sum cancels to
  nearly nothing and whatever survives is noise. Pinning to it flipped the frame on roughly half
  the loop — precisely the discontinuity the pinning existed to prevent. The frame-continuity test
  caught it at `-0.996`.

  The reference has to be **local**, and there is already a local field with a consistent sign:
  the outward binormal, which `compute_binormals()` orients away from the interior for a closed
  stroke and transports for an open one. Pinning to it makes the frame's +Z "out of the plug",
  continuously, all the way round.
- `draw_cut_surface_frame(...)` — local Z that normal, local X the plane's X projected onto the
  tangent plane with the same degenerate guard `curved_cut_sheet_frame()` uses (fall back to the
  plane's Y), plus the connector's own `z_angle`. Keeping the construction identical is what makes
  a connector's Rotation mean the same thing in both modes.
- `draw_cut_surface_project(...)` — the inverse, turning a raycast hit into `(s, w)`. A sweep over
  the samples for the nearest ruling, then a refinement over the two neighbouring spans.
- `draw_cut_surface_contains(...)` — the Draw analogue of the curved cut's `(u,v)`-in-contour
  test: `w` inside `[-extension, +depth]` with the connector's own radius of margin at each rim,
  and for an open stroke `s` likewise clear of both ends.
- `draw_cut_surface_curvature_radius(...)` / `draw_cut_patch_is_flat_enough(...)` /
  `draw_cut_surface_tilt_deg(...)` — the advisory trio, matching the curved cut's constants so the
  panel's wording does not have to change between modes.

  The curvature's finite-difference step must **straddle several samples**, which is the second
  thing a test caught. The path is a polygon — a 1 mm resample of whatever was drawn — so three
  points a fraction of a millimetre apart land on one or two of its straight facets and the
  circumradius through them measures the *faceting*, not the shape. At the first step size a 6 mm
  ring read 3.7 mm, wrong by a third and wrong in the direction that matters (it would warn about
  connectors that are fine). The step is now at least four resample spacings.

### The plumbing, in the gizmo

Small, because phase 4 of the curved work already generalised the connector path from "one shared
`m_rotation_m`" to "a rotation built from the local surface normal", and did it *in the connector
path* rather than in the sheet. Three existing functions grew a Draw branch:

- **`connector_rotation_m()`** → `m_rotation_m * draw_cut_surface_frame(...)` at the connector's
  `(s, w)`, exactly the shape the sheet's branch has.
- **`unproject_on_curved_sheet()`** → `unproject_on_draw_surface()`, a raycast against the
  **cutter shell** — which is what the user can see, since the translucent preview is that same
  solid, so a click lands where the eye says it will. A miss does **not** fall back to the plane,
  unlike the sheet's version: the sheet spans the plane and a click past its rim still means
  something, whereas the drawn surface *is* the cut and a click that misses it would put a
  connector where the cut is not.
- **`connector_pos_on_sheet()`** → identity. The sheet's version exists to restore "same (u,v),
  new height" when the sheet is bent afterwards; a drawn connector's position comes from a raycast
  against the cut surface and is already on it, and there is no analogous relationship to restore.

No contour test on the Draw path, deliberately: the drawn cutter *is* the cut surface and nothing
else, so every point of it is on the cut. What replaces it is the `(s, w)` domain check, which
asks the question that actually matters — is the connector clear of the strip's own rims.

### Why they survive the split and the kerf

Worth stating because it is not obvious from the code. `process_connector_cut()` works off the
connector's own `pos` and `rotation_m` and knows nothing about the cut surface. Both halves are
produced by **one** boolean against **one** cutter, so the faces they present to each other are
the same surface — and a connector standing perpendicular to that surface therefore meets both
squarely. The kerf moves both faces along the **same** strip normal (`draw_cut_cutter_solid()`
offsets along `-binormal` for a closed stroke and `-sweep` for an open one, one field for the
whole strip), so the hole and the plug stay coaxial: the gap opens along the connector's own axis,
which is the direction it comes apart in anyway. There is a test asserting the frame does not
depend on the thickness, and another asserting the two halves' shares of a connector body sum to
the whole.

`perform_with_draw_stroke()` needed no change: phase 1 already kept the curved path's loop shape,
so a connector volume reaching it is processed rather than dropped. The stale comment saying
phase 1 ships connector-free was left in place as a note of when it stopped being true.

## 4. Panel

In Draw mode, in order: Direction, **Angle** (−60..60, greyed unless Direction is Surface normal,
with the tooltip that says which way the sign goes — the one thing about the feature that is not
self-evident on screen), Extension, Depth / Through all, Smoothing, **Edit points**, Clear line.
Thickness and the Visible/Ghost/Hidden side controls are the existing shared ones, unchanged.
"Add connectors" was already reachable in Draw mode — the gate at `:5843` is the flat cut's own
condition with nothing surface-specific in it — so nothing had to be opened up.

New messages: the fold warning now names the Angle and the Depth when an angle is set (at
through-all depth the angle is much the likelier cause, since the sideways reach is
`depth * sin(angle)`); the holonomy fallback; and three connector advisories matching the curved
cut's wording — off the surface's edge, tilted past `CurvedConnectorTiltWarnDeg`, and a
Hinge/Thread on too tight a patch.

`m_draw_angle` is reset on gizmo open and close with the rest of the session state: it is a
property of one cut, not a preference, and leaving it set would silently draft the next object's.
Edit points is cleared alongside — a mode with no line in it has no way out by the obvious
gesture, since editing mode refuses to start a new stroke.

## Deviations from the research spec

1. **The holonomy check measures neighbour sign flips, not transported-frame holonomy.** The spec
   assumes the closed-loop frame is built by parallel transport, and phase 1 deliberately does not
   build it that way (the centroid orientation is what makes it winding-independent). The failure
   the spec is guarding against is still detected, by the symptom rather than the mechanism. See
   §1.

2. **The fold guard clamps nothing.** Detection and a warning naming the three knobs, as in phase
   1; the taper toward `1/kappa` is still unbuilt. The angle made the *detection* materially
   better, which is what the brief asked for; the clamp remains the open item.

3. **`draw_cut_surface_normal()` takes a `w` it does not use.** The ruling is straight, so the
   strip's normal does not vary along it. The parameter is taken so callers need not know that,
   and so a future twisted ruling would not change the signature.

4. **Connector frames are derived, not stored, but positions are not re-derived.** The curved cut
   re-derives a connector's height from its `(u,v)` on every use, so a later sheet edit moves the
   connector with the surface. Draw derives the *frame* the same way but leaves the *position*
   where the user put it: there is no "same parameters, new surface" relationship to restore,
   because on a drawn cut the stroke moving **is** the surface moving, and re-projecting every
   connector onto an edited stroke would drag them around for reasons the user did not ask for.

5. **Line editing does not re-project the whole path per edit.** `commit_draw_points()` re-runs
   `finish()`, which re-runs `reproject_draw_stroke_on_mesh()` — so the projection happens once per
   edit rather than per point per drag tick.

## Tests

`tests/libslic3r/test_draw_cut.cpp`, tag `[DrawCut]`. **38 cases, all passing** (2859 assertions):
phase 1's 18 untouched, plus 20 new. The `[CurvedCut]` suite passes unchanged — **50 cases, 54911
assertions** — which it should, since `CurvedCut.cpp`, `CurvedCut.hpp` and `CutUtils.cpp` were not
touched at all.

The 20 new cases:

**The angle (7)** — the +30° frustum against the closed form
`V = pi h/3 (r1^2 + r1 r2 + r2^2)` with `r2 = R + h tan(theta)` within 5%, plus the geometry the
volume stands for (wider at the bottom by `h tan theta`, the right height, volume conserved); the
−30° undercut plug, narrower at the bottom and smaller than the straight cut; angle 0 identical to
phase 1 to 1e-15; the angle ignored by the constant directions; the rotation read back off `d`
component-wise at six angles; **the fold case** — a six-lobed flower that phase 1's guard passes
at E = 1 and that folds at 45° through 30 mm, with the depth-only negative control; and a plain
circle that never folds until the lateral reach passes its radius.

Note the units trap the angle cases document: `DrawCutParams::depth` is measured **along the
ruling**, so cutting `h` deep in z at `theta` means asking for `h / cos(theta)`.

**Line editing (5)** — move, insert and delete on a closed ring each leave it valid, closed and
resampled, with the edit visibly taking (length and reach change the right way); an open line
edited stays open with its endpoints unmoved; and `set_path()`'s own contract — same length in and
out, binormals recomputed and still outward, a wrong-length path refused.

"Resampled" is asserted as *no span longer than the spacing, and none degenerate*, not as equal
chords. The resampler emits a point every `spacing` along the **input polyline's arc length**, so
the straight-line distance between two output points equals the spacing only where the line is
locally straight; across the apex an edit creates, the two neighbouring chords measure about 0.71
and 0.85 mm on a 12 mm ring. That is the resampler working. The assertion that still discriminates
is the one about long spans: a line not re-finished after an edit carries one span of several
millimetres where the point was dragged.

The open-line case likewise asserts that the last point is within **half a spacing** of where it
was, not that it is unmoved. `draw_cut_resample()` appends the final input sample only when it is
more than half a spacing from the one already emitted — otherwise it would leave a span the
central-difference tangent reads as a near-zero direction — and lengthening the middle of a line
moves where the last multiple of the spacing falls, so the end can legitimately be dropped and the
path stop up to half a spacing short. The property that matters is that the end does not **creep**:
it stays within that half spacing and never overshoots the point the user drew to, which is what
the test now checks. The first point is exact, because the resampler always emits it.

**The surface and connectors (4+)** — the surface point at `w == 0` is the stroke sample, `w > 0`
goes in and `w < 0` comes out; the frame's Z is radial on a circular plug wall, orthonormal,
right-handed, and continuous all the way round; the projection round-trips within 0.2 mm; the
domain test rejects both rims and an open stroke's two ends and wraps for a closed one; a straight
line swept straight down is flat in both directions while a 6 mm ring reads its own radius across
the rules; the tilt reads 90° on a vertical plug wall; **the connector case** — a dowel body on
the surface frame, split against both halves, whose two shares sum to the whole within 3% and are
balanced within 25%, which is the assertion that the frame really is across the surface rather
than along it; and the frame's independence from the kerf.

## Click-tests for the owner

Nobody has clicked any of this either. In particular:

1. **The Angle on a closed loop**, both signs, on a cube and on something curved: does +30 give a
   plug that lifts out and −30 one that locks? The translucent shell should visibly splay or pinch
   as the slider moves.
2. **Angle with Direction = Axis or View**: the slider must be greyed out, and switching Direction
   to Surface normal must bring it back with the value it had.
3. **Edit points**: drag a handle across a curved face and watch the shell follow live; check the
   handle stays *on* the surface rather than sliding on a plane. Drag one off the silhouette — the
   point should not move.
4. **Right-click a handle** to delete, **Shift+click a segment** to insert. Then Ctrl+Z each of
   them, several times, and Ctrl+Y back. Every edit should be its own step.
5. **Edit a closed loop until its two ends would separate** — the line must stay a loop (the panel
   keeps saying "Closed loop"), because `commit_draw_points()` re-expresses the closing span.
6. **Esc while editing**: first Esc should leave editing mode, second should clear the line.
7. **Place a Plug on the drawn surface** of a closed-loop cut, cut, and check the hole and the plug
   actually mate. Then the same with **Thickness > 0** — the gap should open along the connector's
   axis, not skew it.
8. **A connector placed near the rim** of the cut surface: the panel should say it is not clear of
   the edge.
9. **A Hinge on a straight stretch** of stroke (should be fine) and **on a tight curve** (should
   warn). This is the claim that a ruled strip is flatter than a dome, tested by hand.
10. **Smoothing on a small feature**: with the re-projection in, a heavily smoothed line should now
    stay on the surface rather than cutting the corner. Compare with phase 1's behaviour if it is
    still to hand.
11. **Switch Draw → Curved → Draw with connectors placed**: the mode combo should be locked while
    connectors exist, which is the reverse gate phase 1 inherited.

## Still open

The fold **clamp** with tapering (detection only, as above). Properly coloured preview halves,
which would want the background-job split the research spec describes. Sub-loop splitting for a
self-crossing stroke. Serialization is unchanged and needs to stay that way: the cut is
destructive and the stroke is session state.
