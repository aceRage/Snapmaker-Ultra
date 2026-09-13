# Draw cut — the chain, and the halves that follow it

Branch `fix/draw-cut-loop`, off `feat/ultra-preferences` at `ceb9ffc22c`. Fixes the four
things the owner found click-testing phases 1 and 2 on a tall box next to a cube
(2026-09-12). The phase 1 and phase 2 specs' deviations are taken as the truth
throughout — in particular that Extension runs along the cut direction, that an open
stroke's cutter is a swept slab, that the fold test's sign is the opposite of the obvious
reading, and that an empty side disables the cut.

## The four items, and what each turned into

### 1 and 2. The line is a CHAIN of strokes, and nothing is lofted until it closes

**What was wrong.** The line was ONE stroke: a press, a drag, a release, and whatever came
out of that one gesture was the whole cut line. You cannot get all the way round a tall
box without rotating the view, so the line could never be a loop round a large part — and
because the partial stroke was lofted anyway, what the preview showed was a wild ruled
surface swept off half a line, which meant nothing.

The two items are one fix, because the second is a consequence of the first: once the line
is only lofted when it is a closed loop, there is no partial surface left to preview.

**`DrawCutChain`, in `src/libslic3r/DrawCut.{hpp,cpp}`.** Ordered raw samples with two
endpoints, plus the per-stroke ranges undo works off:

- `end_for_start(p, r)` → `DrawChainEnd::{None, Front, Back}`: which endpoint a stroke
  beginning at `p` continues. An empty chain accepts anything; a **closed** chain accepts
  nothing.
- `append(stroke, r)` → the end it joined, or `None` for a refusal. A **Front** append
  **reverses** the stroke before prepending it: the stroke was drawn *away* from the
  chain's front, so prepending it as captured makes the sequence double back at the join
  and every tangent there is wrong. That reversal is the one piece of the chain that is
  not bookkeeping.
- `undo_last_stroke()` takes back the last appended stroke whole, **and the closure it
  made** — a chain cannot stay closed after losing the span that closed it.
- `force_close()` for the panel's explicit **Close loop**.
- `finish(out, spacing, smoothing)` produces a `DrawCutStroke` **only for a closed
  chain**; for an open one it leaves `out` cleared with the new
  `DrawCutError::NotClosed`.

That last line is what makes items 1 and 2 hold everywhere at once, and it is the reason
the gizmo change is small. Every "is there a cut surface" site in `GLGizmoCut.cpp` already
asked `m_draw_stroke.valid()`; `m_draw_stroke` is now **derived from the chain and left
empty while the chain is open**, so an open chain lofts nothing, previews no cutter shell,
places no connectors, classifies nothing and greys out the Cut button — without any of
those sites knowing that chains exist.

**The snap radius** is `clamp(0.02 * bbox diagonal, 1 mm, 6 mm)`, computed by
`draw_cut_chain_snap_radius()`. Scaled because a 2 mm radius that is comfortable on a
200 mm print is most of a 10 mm trinket; clamped because a radius larger than the line is
long would close every chain the moment it started. An undefined bbox gives the minimum,
which is the safe end (too small means the user has to be accurate; too large means a
chain closes behind their back).

Closure keeps the sample that closed it — it is on the model, it was drawn, and dropping
it would leave a visible notch — and does **not** snap it onto the far endpoint. The
closing span is then a real span within one snap radius of zero, which on any real part is
under the 1 mm resample spacing.

`DrawCutChain::finish()` passes `force_closed = true` to `DrawCutStroke::finish()`,
deliberately: the **chain** decided it is closed, by a snap radius that on a large part is
wider than `finish()`'s own `max(3 * spacing, 2 mm)` closing tolerance. Letting `finish()`
re-decide would silently reopen a chain the user watched snap shut.

**In the gizmo.** A stroke is captured into `m_draw_capture` and appended to the chain on
**LeftUp**, not as it is drawn. That is what lets a gesture be abandoned — Esc, or a press
that turns out not to continue the chain — with the line never having changed. The
polyline the user watches while drawing is the chain's samples with the in-flight stroke
spliced on at the end it continues (reversed for a Front continuation, so what they see
is what `append()` will do).

Endpoint handles are spheres at the chain's two ends, drawn after a depth-buffer clear so
an endpoint on the far side of a tall part is still findable — it is where the next stroke
has to start. The one a release would close on turns **bright green and grows** while the
cursor is inside the snap radius, so the closure is visible before it happens. Measured
against the last **captured sample** rather than against the mouse ray, so the feedback
agrees exactly with the test `append()` will make.

Nothing is drawn for a closed chain: it has no free endpoints, and marking two arbitrary
samples as special would be a lie about what clicking there does.

### 3. The halves classification follows the drawn surface

**What was wrong.** The cyan/magenta colouring, the Visible/Ghost/Hidden side display and
the connectors all read the same per-fragment "which side" the colour-clip shader
computes, and in Draw mode that was still the **flat plane's** dot product. The colours ran
straight through the drawn surface.

**Why the curved cut's mechanism cannot be reused.** The sheet is a height field
`z = f(u,v)` in a 2D texture. A ruled strip swept along a curve is **not single-valued over
the plane** — that is precisely what the mode exists for — so there is no `(u,v) -> z` to
put in a 2D texture. Phase 1's spec already answered this question ("if the ruled solid can
be expressed as a height field": it cannot) and left properly coloured halves as a phase 2
item that phase 2 did not build.

**What is available instead** is the question the split itself asks: **is the point inside
the cutter solid**. Exact by construction — it is the same solid `draw_cut_split()` hands
to the boolean — and it needs no assumption about the surface being a graph over anything.

- `draw_cut_classify_upper(cutter, closed, p)` is that question, exactly, for one point:
  one parity ray, with `draw_cut_split()`'s own convention (for a **closed** stroke the
  plug — inside the cutter — is the upper half; for an **open** one the cutter is the swept
  slab on the lower side, so inside is lower). This is what connectors and the tests use.
- `draw_cut_inside_field(...)` bakes it over a grid as a **sign field**: `-1` upper, `+1`
  lower, matching `m_color_clip_plane`'s own convention (`side < 0` is side 1, which
  `apply_color_clip_plane_colors()` feeds with the upper colour).

The field is uploaded as a **3D texture** and the volume shader samples it per fragment.
`sampler3D` is core GL 1.2 / GLSL 110, so this works on the 2.1 fallback path too — the
plumbing mirrors the sheet's exactly, one dimension wider:
`GLVolumeCollection::set_draw_color_clip(tex, world_to_plane, origin, size)`, uniforms
`draw_field_{active,tex,matrix,origin,size}` on texture unit 4, and a branch in both
`resources/shaders/{110,140}/gouraud.fs` that takes precedence over the sheet's (a cut is
Curved or Draw, never both, so giving the newer one precedence means a stale sheet texture
left over from a mode switch cannot colour a drawn cut).

Two things about the field are worth recording because a test caught each:

**One parity ray per COLUMN, not per voxel.** For each `(i, j)` the crossings of the +Z
line with the cutter's triangles are collected once, sorted, and the whole column of 48
voxels filled by walking them — the parity between two consecutive crossings is constant.
That is what makes 48³ = 110k voxels affordable on a mouse-up (2304 rays against a few
thousand faces, tens of milliseconds) instead of 110k full mesh passes.

**The columns are jittered.** `point_in_solid()` avoids axis-aligned degeneracies by using
an irrational **ray direction**; a column ray is fixed along +Z, so the same trick has to
be applied to the sample **points** instead. Each column's `(x, y)` is offset by a fixed
sub-voxel irrational fraction of the cell, which moves every column off the axis-aligned
grid the cutter's faces are built on (a stroke on a cube's top face gives faces parallel to
+Z everywhere) without moving any column more than a fraction of a voxel.

The texture is **NEAREST**-filtered, not linear. The field is a two-valued sign, and
interpolating between −1 and +1 puts a band of intermediate values across the boundary
whose sign depends on which side of 0 the interpolation lands — which reads as a ragged
edge that moves as the camera moves.

The GL 2.1 fallback stores 0 for upper and 1 for lower rather than an encode/decode of a
range (GL_LUMINANCE is fixed point on [0,1] and cannot hold −1), and the shader tests
`v - 0.5`, which is correct for both encodings: −1 and 0 are both below 0.5.

**With a kerf** there are two surfaces and the band between them belongs to neither side,
so the colouring picks the **upper half's own boundary** — `face_hi` for a closed stroke,
`face_lo` for an open one — which is what makes "cyan is what you keep on top" true.

**The connectors half of item 3, which was a separate bug.**
`put_connectors_on_cut_plane()` snaps every connector onto the flat plane, and it runs from
`update_clipper()` — on every plane nudge, every rotation and every gizmo open. On a drawn
cut that walks a connector straight off the cut surface, because a drawn connector's
position came from a raycast against that surface and there is no "same parameters, new
surface" relationship to restore (phase 2, deviation #4). It is now a no-op in Draw mode,
which is also what keeps `connector_rotation_m()`'s frame — derived from the surface at the
connector's own `(s, w)` — describing the place the connector actually is.

### 4. Only one chain, and a disjoint stroke is rejected

`end_for_start()` returns `None` for a stroke that starts within the snap radius of
neither endpoint, and the gizmo checks it **on the press**, so the refusal costs the user a
click rather than a whole stroke they then watch disappear. The panel says which of the two
reasons it was, because "nothing happened" is the worst possible feedback for a gesture the
user thought they made.

**A closed chain is not replaced implicitly at all** — this is the branch of the brief's
"(or, if the chain is closed, replaces it after confirmation via the existing gizmo undo
stack, your call; document it)" that was taken. A closed chain's `end_for_start()` says
`None` at both endpoints, including *at* an endpoint, and the way to get a different line is
**Clear line** or **Ctrl+Z** — deliberate gestures, both of which already go through the
gizmo-local undo stack. Reopening a closed chain would have to pick a place to reopen it,
and there is no gesture that says which.

**A closed loop entirely on one face is a valid chain** and cuts a plug out of that face,
drawn in one gesture (the phase 1 case, unchanged) or in several (tested both ways).

### Phase 1's open cut, kept but made explicit

An open line right across a part splits it in two with no inside or outside, which is a
perfectly good cut and one phase 1 supported. The chain cannot loft it *automatically* any
more, because **"the line is not finished yet" and "the line is finished and is not a loop"
look identical from the samples alone** — and lofting every partial line is exactly the
wild-shape preview items 1 and 2 exist to remove.

So the user says which: **Cut along the line** sets `DrawCutChain::finish_open()`, and a
chain with that set produces an open stroke from `finish()` the way phase 1 did. **Any
append clears it**, because a chain that has just grown is one the user is still drawing —
and so do undo and Close loop. A closed chain refuses it, and a chain shorter than
`MinChainSamples` refuses both verdicts.

Its endpoint handles stay up, because a finished-open chain still has two free ends and
carrying on from either is meaningful (it un-finishes the line). Only a **closed** chain
hides them.

The recipe carries the verdict (`CutRecipeStroke::finished_open`, version 2). A **version 1**
recipe has no such field, and its line **was cut with** — so an unclosed version 1 stroke is
treated as finished-open. Without that, every open cut saved before this change would reopen
with no surface and no visible reason why.

## What was kept, and what it now means

- **Ctrl+Z per stroke segment.** The undo entry carries the **whole chain** rather than a
  stroke, because a stroke is a *range of* the chain and undo has to restore the chain's
  open/closed state too — which a stroke-only entry could not express. One entry per
  appended stroke, pushed on the press and popped again on LeftUp if nothing was appended
  (otherwise the first Ctrl+Z would appear to do nothing, taking back a link the user never
  got). Ctrl+Z after the stroke that closed the loop gives back the open chain with the
  surface preview gone.
- **The recipe stores the raw samples of the whole chain**, and now the per-stroke ranges
  as well, so a reopened cut's Ctrl+Z takes back one stroke rather than the whole line.
  Recipe **version 2**; version 1 still loads (`cut_recipe_version_supported()`), because a
  version 1 recipe's samples and closed flag were always the whole description of the line
  — it re-cuts to exactly the same halves, as a chain of one stroke.
- **Flip / side switching** carries the chain. The flip negates local y and z of every
  sample, which is pure arithmetic as before, but the chain is rebuilt through the recipe
  converters rather than by `set_samples()`: the samples keep their order and only their
  coordinates change, so collapsing the chain to one stroke would needlessly throw away the
  per-stroke undo the user has built up.
- **Connectors on the drawn surface** are unchanged in mechanism and now correct in two
  ways they were not: they are only reachable on a closed chain (the stroke is invalid
  until then), and they are no longer dragged onto the flat plane behind the user's back.

## Deviations and judgement calls

1. **The classification is a voxel field, not an analytic test in the shader.** An exact
   per-fragment point-in-solid test would mean shipping the cutter's triangles to the
   shader and marching them, which is a different order of complexity for a preview. The
   field is exact everywhere except within a voxel of the boundary, and the boundary is
   where the cut surface is drawn translucent over it anyway. 48³ was chosen as the
   resolution at which the boundary is finer than the eye resolves on a translucent
   preview; there is a test pinning the field against the exact classification everywhere
   outside a 1.5-voxel band.

2. **An edit through Edit points collapses the chain to one stroke.** `commit_draw_points()`
   rewrites the chain's samples from the edited points, and an insert or delete changes the
   sample count, so the original per-stroke ranges no longer describe the line. Pretending
   they do would corrupt undo. Undo itself still works — the entry pushed before the edit
   holds the chain as it was, ranges and all — it just makes the *edited* line one link.

3. **The recipe's stroke ranges are stored sorted by POSITION, not in append order.**
   `DrawCutChain::stroke_bounds()` is in append order, because that is the order undo
   consumes it in — and a front append puts its samples at index 0 while its range goes to
   the end of the list. The replay in `cut_recipe_stroke_to_chain()` rebuilds the chain by
   walking the sample list forwards, which is the only replay that reproduces the stored
   sample *order*, and that order is what the cut is made from. What is lost is the sequence
   the strokes were drawn in, so a reopened cut's Ctrl+Z takes back the last stroke *along
   the line* rather than the last one drawn. Nobody remembers the drawing order of a line
   from a previous session, and storing both orders would let the two disagree about the
   same line.

4. **Malformed stored ranges fall back to one stroke rather than being half-applied.**
   Ranges that do not tile `[0, samples.size())` exactly — a gap, an overlap, a range past
   the end, a first range not starting at 0 — are discarded. A chain whose ranges disagree
   with its samples corrupts undo in a way the user cannot see coming.

5. **A "Close loop" button as well as the snap.** The snap covers the gesture the owner
   asked for; the button covers the line whose two ends cannot comfortably be brought
   within the radius — round a feature that comes back to within a few millimetres of
   itself. The closing span is then whatever gap is left, which the resampler walks like any
   other span.

## Two bugs found by the tests, recorded because the reasoning that produces them looks sound

**`m_bounds` is in APPEND order, not positional order.** The first version inserted a front
append's range at the *start* of `m_bounds`, to keep the list in the same order as the
samples. `undo_last_stroke()` then took back `m_bounds.back()`, which after any front append
is the **oldest** stroke — so Ctrl+Z on the near end of the line silently removed its far
end. The list is in append order now, because that is the order undo consumes it in; where
each range sits in `m_samples` is irrelevant to it.

**The replay's snap radius must be TINY, not huge.** `cut_recipe_stroke_to_chain()` replays
the stored ranges through `append()`, and the obvious radius to pass is one that cannot
refuse anything. That is exactly backwards: the same radius is what `append()` measures the
**closure** with, so the very first replayed stroke was judged to have closed the chain on
its far endpoint, and a closed chain refuses every later append — the whole replay fell back
to one stroke, silently. The joins are exact by construction, so an epsilon radius passes
every start test and fails every closure test, which is what is wanted since the closure is
restored explicitly from the stored flag.

## Tests

`tests/libslic3r/test_draw_cut.cpp`, tag `[DrawCut]`: **48 cases, 3023 assertions, all
passing** — phase 1's 18 and phase 2's 20 untouched, plus 10 new:

- **continue from either end**: a Back continuation, then a Front one, with the chain's
  sequence asserted continuous (no span longer than 1.5× the sampling step — a stroke
  prepended unreversed would leave a 10 mm span at the join), and a disjoint stroke
  changing nothing;
- **the disjoint rejection**: the chain untouched, and both sides of the radius boundary
  (0.95 r taken, 1.05 r refused);
- **the snap closes within the radius and not outside**: a square drawn in four strokes
  ending 1 mm short (closed) and 5 mm short (open), the open one producing
  `DrawCutError::NotClosed` and an empty path; plus the radius formula's scaling and both
  clamps;
- **undo per stroke**: a Front-appended stroke taken back (the case the `m_bounds`-order
  bug broke), the closure taken back with the stroke that made it, and the chain emptying
  and refusing to go further;
- **a closed chain refuses a continuation**, including one starting exactly at an endpoint;
- **a closed circle on one face**, drawn in **two** strokes: a plug of `π r² h` within 5%
  and a body, summing to the cube within 1e-3;
- **the tall box looped in three strokes from two view directions**: two watertight halves
  partitioning the box within 1e-3, each about half, and asserted **stacked** (one half's z
  range above the other's) rather than nested — the geometric claim that the loop cut the
  box in two;
- **the halves classification follows the drawn surface**: points inside the plug above
  *and below* the cut plane both classify upper, and points outside it above and below both
  classify lower — which is the pair the flat plane gets wrong. Then every vertex of each
  produced half, nudged radially off the cylinder wall into its own half, classified
  correctly (all of them, not a majority);
- **the voxel field agrees with the exact classification** at every one of >10000 grid
  points outside a 1.5-voxel band, with both halves represented and the sign convention
  pinned at the plug's centre;
- **an open stroke's classification is the other way round**, checked against the centroids
  of the halves the split actually produced rather than against a guess.

`tests/libslic3r/test_cut_recipe.cpp`, tag `[CutRecipe]`: **9 cases, 542 assertions** — 6
untouched plus 3 new: a three-stroke chain (including a front append) round-tripping with
its samples, their order and its stroke split intact; a version 1 stroke loading as a chain
of one stroke and a version 1 recipe still `valid()`; and five malformed range sets each
falling back to one stroke with a well-formed pair as the negative control.

`[CurvedCut]`: **50 cases, 54911 assertions**, unchanged — `CurvedCut.cpp` was not touched.

## Click-tests for the owner

Nobody has clicked any of this.

1. **The tall box, three strokes, two view directions.** Draw along one face; let go; the
   two ends should be marked with orange spheres. Rotate; start the next stroke **on one of
   those spheres** and carry on. There should be **no surface preview** at any point until
   the loop closes — only the orange line and the two endpoint balls.
2. **The snap.** On the third stroke, bring the cursor near the far endpoint: that ball
   should turn **green and grow** before you release. Let go inside it and the loop closes —
   the balls disappear, the translucent cutter shell appears, and the panel says "Closed".
3. **The colours.** With the loop closed, check the cyan/magenta split **follows the drawn
   surface** rather than a flat plane: on a loop round the box with Direction = Axis Z, the
   colour boundary should be the drawn line itself, all the way round, at every height.
   Then set one side to **Ghost** and to **Hidden** and confirm the right half goes.
4. **Cut**, and check the two halves mate.
5. **A circle on one face** (the cube next to the box): draw a closed loop in one gesture,
   confirm it snaps and cuts a plug; then draw one in two strokes and confirm the same.
6. **Undo.** Ctrl+Z should take back **one stroke** each time, and the Ctrl+Z that undoes
   the closing stroke should make the surface preview disappear again. Ctrl+Y back. Then Esc
   mid-stroke (the stroke being drawn vanishes, the chain stands) and Esc after (the line
   goes).

Also worth a look, in falling order of likely surprise:

7. **A stroke that starts nowhere near an endpoint** — the press should do nothing and the
   panel should say why. Same for a stroke on an already-closed chain.
8. **Close loop** on a chain whose ends are, say, 15 mm apart: the closing span is that
   whole gap, so the cut surface takes a straight-ish shortcut there. Confirm that reads as
   intended rather than as a bug.
9. **Connectors** on a drawn cut, then **nudge the cut plane's position input**. They must
   stay on the drawn surface (before this change they were snapped onto the flat plane every
   time the clipper updated).
10. **Edit points** on a closed chain, then Ctrl+Z: the edit is one step, and the line it
    comes back to is the chain as it was.
11. **A heavy model**: the field is rebuilt on every mouse-up and every parameter change, so
    watch for a hitch when dragging Extension on a few-hundred-thousand-triangle part.
12. **The GL 2.1 path**, if there is a machine for it: the field is a `sampler3D` in the 110
    shader too, and the encoding differs there.

## Still open

The fold **clamp** with tapering (detection only, as in phases 1 and 2). Sub-loop splitting
for a self-crossing stroke.

The classification field is rebuilt synchronously on every parameter change, which on a very
heavy part is the one place this change could be felt as a hitch. The obvious next step is
the background job the phase 1 research spec describes for the split — the field is a far
cheaper thing to put on a worker than a boolean, and it already has the dirty flag a job
would key off.
