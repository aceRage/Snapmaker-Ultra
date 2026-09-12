# Edit gizmo: "Round all edges" and the phase-2 edge bevel

Branch `feat/edit-bevel`, from `feat/ultra-preferences`. Two deliverables, built and committed in order:

1. **Round all edges** — the interim, whole-mesh fillet by the OpenVDB morphological round trip.
2. **Edit gizmo phase 2** — the real edge bevel / chamfer on a selected feature-edge chain.

They are deliberately different tools and the UI says so. Step 1 is the blunt instrument that rounds *everything* and rebuilds the part; step 2 is the CAD operation that acts on *what you selected* and inserts exact geometry. The research spec (`2026-09-11-cad-mode-research.md` §3.3) calls step 1 "the global alternative" and says to *"ship as a separate, clearly-labelled command — never as the edge-chain bevel"*. That is what was done.

---

## Step 1 — Round all edges

### What it does

A rolling-ball fillet of radius `r` applied to every edge of a part at once, via morphological opening and closing of the signed distance field:

| half | offsets | rounds |
|---|---|---|
| **OPEN** | erode by `r`, dilate by `r` | every **convex** edge |
| **CLOSE** | dilate by `r`, erode by `r` | every **concave** edge (inside of an L, a pocket, a slot) |

Doing open *then* close gives both in one pass, which is what "round all edges" means to a user. `openvdb::tools::LevelSetFilter::offset()` is the erode/dilate: it offsets the level set by a world distance and renormalises, which is exactly a morphological dilation (negative offset) / erosion (positive one).

### Files

| file | what |
|---|---|
| `src/libslic3r/MeshRound.hpp` / `.cpp` | Options, clamps, the pipeline, and the measurement helpers. **No OpenVDB include** — the rounder is injected as a callable, exactly as `MeshRemesh.hpp` injects its remesher, so libslic3r builds and the tests run without OpenVDB. |
| `src/libslic3r/OpenVDBUtils.cpp` | `round_by_voxels()` — the OpenVDB half. Also `voxel_ops_available()`. |
| `src/libslic3r/MeshRepair.hpp` | Declares both, staying OpenVDB-free so GUI code can include it cheaply. |
| `src/libslic3r/MeshRepairStub.cpp` | **New.** The no-OpenVDB side of that switch (see *Deviations*). |
| `src/slic3r/GUI/RoundDialog.hpp` / `.cpp` | The options dialog, in `RemeshDialog`'s house style. |
| `src/slic3r/GUI/GUI_ObjectList.cpp` | `ObjectList::round_all_edges()`, `repair_by_remesh()`'s twin. |
| `src/slic3r/GUI/GUI_Factories.cpp` | The **Round all edges…** menu entry, next to Repair/Remesh. |
| `src/slic3r/GUI/Gizmos/GLGizmoEdit.cpp` | The same operation as a button at the bottom of the Edit panel. |
| `tests/libslic3r/test_mesh_round.cpp` | **New**, tag `[MeshRound]`. |

### Keep the bottom flat

Rounding every edge includes the one where the part meets the bed, which lifts the outline off the plate and leaves the first layer unsupported. The option reuses **`MeshRemesh::remesh_keep_flat_bottom()` verbatim** — that function already takes an injected `VoxelRemesher`, so `round_with_options()` binds its four-argument rounder down to that two-argument signature and gets the whole cut / remesh-the-top / union-the-slab-back path for free, with no duplicated geometry code.

Two parameters had to be chosen with care, and both are commented at the call site:

* **The margin is a multiple of the RADIUS** (`ROUND_BOTTOM_MARGIN_RADII = 1.5`), not of the voxel size. A plain remesh only has to clear a voxel of rippling; a round has to clear the whole fillet, which is a radius tall. Too thin and the cut plane runs through the rounding and the union leaves a step.
* **The radius is passed in `remesh_keep_flat_bottom`'s `voxel_size` slot.** That parameter controls how far the slab is extended past the cut so the two solids overlap. The round *erodes* the upper part by up to a radius at its own cut face, so the overlap has to be a radius, not a voxel, or the union has nothing to bite on.

### Coupling the radius and the voxel size

The offset walks the narrow band by `radius / voxel` voxels, so the two are not independent:

* `round_auto_voxel_size(r) = clamp(r / 6, 0.02, 5.0)` — six voxels per radius, below about four the fillet is visibly faceted.
* The narrow band is requested as `r_grid + 4` voxels on each side, or `LevelSetFilter::offset()` clips and the result is garbage.
* A voxel size coarser than `r / 2` is **clamped down**, not refused: the user asked for a radius, and the radius is the thing they will measure.

### Tests — `[MeshRound]`, 10 cases

The injected rounder in the tests is **not a fake**: it builds an analytically exact rounded box (the Minkowski sum of a shrunk box with a ball, tessellated as a sphere whose eight octants are pulled apart to the corners). That is precisely the surface the OpenVDB round trip approximates, so every assertion checks the same property the real path must satisfy.

| test | pins |
|---|---|
| the rounded-box volume formula agrees with a tessellation | `rounded_box_volume()` against a 32×64 tessellation within 0.2%; the `2r` doesn't-fit and `r = 0` cases |
| the auto voxel size tracks the radius and stays clamped | `r/6`, clamped both ends |
| **a 20 mm cube at r = 2 loses close to the analytic volume** | the brief's headline: volume loss **within 10%** of `rounded_box_volume_loss`, **no dihedral above 30°**, **watertight**, and the envelope still 20 mm |
| a bigger radius removes more material, monotonically | `r` ∈ {0.5, 1, 2, 4}: strictly decreasing volume, each within 2% of the closed form |
| **keep the bottom flat leaves the base planar** | base plane exact to 1e-4, every vertex in the base band exactly on it, footprint still 20 × 20 |
| with keep-bottom-flat off the base is rounded too | the complement: the bottom footprint shrinks below 19 mm |
| a mesh with no flat bottom falls back | a sphere: `fell_back` set, `note` non-empty, plain round still runs |
| the radius and voxel size are clamped | radius past max clamped; voxel past `r/2` pulled to `r/2`; `0` derives from the radius |
| an empty mesh or a missing rounder gives an empty result | and a *failing* rounder is a failure, not a silent identity |
| the measurement helpers agree with hand-checked meshes | cube = 90°, sphere < 20°, an open cube is not watertight |
| rounding the same mesh twice gives an identical result | determinism |

**Result: 11/11 pass, 13,166 assertions.** `[MeshRemesh]` still passes 7/7, so the `remesh_keep_flat_bottom()` reuse did not disturb its owner.

Three of these failed on the first run and all three were faults in the *test scaffolding*, not the production code — worth recording because each was a real lesson about the fixture:

* the stub's octant-split sphere was geometrically closed but **topologically full of holes** (poles, the longitude seam and every octant boundary duplicate positions under different indices); welding coincident vertices *before* dropping degenerate facets fixes it, and the two sides of a seam need a coordinate **snap** first because they are computed by different trigonometry and land a few ULPs apart;
* the default tessellation was too coarse — the measured quantity is a *small difference of large numbers*, so a 0.6% shortfall in arc area became a **33% error in the volume loss**; at 64 × 128 it lands inside the brief's 10%;
* "a sphere has no flat bottom" is **false** for a UV sphere, whose bottom pole is a fan of bed-facing facets within a fraction of a voxel of `z_min`; the fallback test now uses a cube standing on one corner, which genuinely has none.

---

## Step 2 — Edge bevel / chamfer (phase 2)

> **STATUS: NOT FINISHED — 18 / 21. Two real defects fixed; one geometric error in the rail placement remains, and it is now derived exactly. Do not ship this half.**
>
> **`[MeshEdit]` 18 / 21**, `[MeshRound]` 11 / 11 (13,166 assertions).
>
> #### Fixed here (1): rails are shared by position
>
> `rail_of()` keyed its inserted points by `(edge, side, vertex)`. Two different
> `(edge, side)` pairs at one bevelled vertex land on the **same point** — at the cube
> corner where the X, Y and Z edges meet, the rail of the X edge measured in face XY and
> the rail of the Z edge measured in face YZ are both `v + w·Y` — so every corner
> produced three twin pairs and the all-12 cube twenty-four. The filler then stitched the
> zero-area slivers *between* the twins, got the right patch count (8) for that topology,
> and `its_merge_vertices()` welded the twins and collapsed every patch it had just made.
>
> Rails are now deduped **at insertion**, keyed by original vertex plus a position
> quantised to 1e-4 mm (the two computations run through different normals and cross
> products, so they agree to a few ULPs rather than bitwise). Measured:
> `coincident_dups` **24 → 0** on the all-12 cube, and `verts` no longer drops across the
> merge. Welding afterwards cannot work and the spec used to say so; the reason is now
> concrete — by then the topology has been built around the duplicates.
>
> #### Fixed here (2): winding is settled globally, once
>
> The surface is assembled by three independent producers — the rewritten sides
> (ear-clipped against their own face normal), the strips (`emit_quad()`, against the mean
> of the two incident face normals) and the caps/patches. Each is internally correct and
> **they do not agree with each other**: the two facets meeting on a strip's end edge came
> out traversing it in the *same* direction. Undirected, the edge has its two facets and
> `is_closed_manifold()` is satisfied; wound, it is inconsistent, and the winding-aware
> `its_num_open_edges()` behind `watertight()` counts it as open.
>
> The probe named it outright, at a chamfer corner: `(8,9)=2/0 (8,24)=0/2 (9,24)=2/0` —
> two facets on each edge and not one of them opposing.
>
> Two consequences, both now fixed:
>
> * a new `orient_consistently()` floods one orientation across the finished mesh and
>   flips whatever disagrees, then flips the whole shell if the signed volume says it
>   faces inward. Chasing the disagreement back into the producers is the wrong shape of
>   fix — each derives its winding from a normal, and at a corner every such reference is
>   near-degenerate, which is the same trap the end caps fell into. Mutual agreement is a
>   property of the assembled surface, so it is settled on the assembled surface.
> * the **hole test** is now undirected (an edge carried by exactly one facet) in both the
>   corner walk and `fill_open_loops()`. The directed test reports an already-closed but
>   inconsistently wound edge as open, which invented holes that were then patched — and
>   the patch did not fix the winding either, so the next pass found the same "hole" and
>   patched it again: two identical triangles, four facets on the edge, `nonmanifold=24`.
>
> #### Corner patches: built per corner, from what the strips recorded
>
> `strip_end_ring` records, as each strip is emitted, which points it left open at which
> vertex. The corner pass then works on **one corner at a time**, over only that corner's
> own points, and fills every open loop it finds there, counting the whole corner as one
> corner patch. That replaces hunting boundary runs globally, which could not work in
> either order: with duplicate rails it filled the slivers between twins, and after
> welding it saw one corner's several runs as separate loops (20 where 8 were wanted,
> `open=84`). The information it needed — which run belongs to which corner — is not in
> the edge counts at all; it is in which strip left which end open.
>
> A loop is taken as the **minimal cycle through a chosen open edge** (remove the edge,
> breadth-first shortest path between its endpoints) rather than by a free walk, because
> at a rail of a rounded corner four open edges meet and a free walk cuts out of one hole
> into the next — measured, 9 points of a 12-point ring. Strip edges are preferred as the
> seed over chords, so a rounded corner's lenses are filled before the chord triangle
> between them; the other order consumes all three chords at once and the remaining cycle
> then covers the chord triangle a second time, sealing a sliver of void inside a mesh
> that is still closed and still manifold but has the wrong volume.
>
> #### What is left, derived exactly: the rail is placed on the wrong point
>
> The three still failing are the all-12 round, the all-12 chamfer against the closed
> form, and the chain on a chamfered box. The chamfer case now reports
> `corner_patches=0`, `is_closed_manifold` **true**, and a volume that is wildly wrong —
> `removed 5626` where 432 is right. That combination is the whole diagnosis: the surface
> closes by count while pinching, so the enclosed volume is not the part.
>
> Worked through on the 20 mm cube at `w = 2`, corner `(20,20,0)`:
>
> * `rail_of()` returns `v + w·t`, `t` the in-plane perpendicular. For face `z = 0` that
>   puts the two rails at **(20,18,0)** and **(18,20,0)** — both *on the cube's own
>   edges*, 2 mm from the corner.
> * The side rewrite then drops `v` (both its boundary edges are bevelled) and **chords
>   between those two rails**.
> * But the *vertical* edge's strip, running up from `(20,20,0)`, has its bottom end pair
>   at exactly `((20,18,0), (18,20,0))` as well. So that edge is claimed by the face chord
>   and by a strip end — two facets, no hole, and the corner triangle has nowhere to go.
>   Hence `corner_patches=0` and the pinch.
>
> The correct geometry says what the rail should have been. A face's polygon corner is the
> intersection of **its own two offset lines**: on `z = 0` the line `y = 18` (the offset of
> the −x edge) and `x = 18` (the offset of the −y edge) meet at the single point
> **(18,18,0)**. A chamfered cube is 6 octagons + 12 rectangles + 8 triangles, and each
> octagon's corner is that one point — not a two-rail chord. The rail endpoints
> `(20,18,0)` and `(18,20,0)` are the right points for the *corner triangle*, but they are
> not the right points for the *face polygon*.
>
> So the remaining work is a change to the rail model, not another patch on the filler:
> each face needs its own inset corner point, computed as the intersection of the two
> offset lines in that face's plane, with the per-edge rail endpoints kept for the strips
> and the corner triangles. That is the piece to do next, and the stage probe (inert
> behind `MESHEDIT_BEVEL_DIAG`) reports `coincident_dups`, the per-corner
> `open_edges` count and the loop sizes, which is what these three measurements came from.

### The contract, stated once

Phase 1's push/pull moves vertex positions and **nothing else**, which is why it keeps painted data. **A bevel inserts geometry**: vertices and facets are added and every index is renumbered. So it takes the *other* commit path — `Plater::clear_before_change_mesh()` first, the way Subdivide, Simplify and Remesh do — and painted supports, seams, MMU colours and fuzzy skin **are cleared**. The panel says so at the point the decision is taken, not only in the note at the bottom (which is about a push and says the opposite).

### Global width solve — the one decision that mattered

Taken straight from the research spec's reading of Blender's `bmesh_bevel`: **solve every width globally before building any geometry.** Blender's first bevel solved widths greedily and depth-first and was *scrapped* because it was order-dependent — two topologically identical parts came out with different widths depending on which edge the walk started from. This repo has determinism gates, so that failure mode is not acceptable here either.

The pass order in `bevel_edges()` is therefore:

1. **Validate** — every requested edge manifold and non-degenerate; a non-manifold edge is refused outright rather than guessed at.
2. **Solve widths** — `solve_bevel_widths()`, one width per edge, as the minimum of the request and three limits: (a) the incident facets' height over the edge, (b) the shortest edge at each endpoint, discounted further when three or more bevelled edges meet there, and (c) the sharpness of the edge itself. Every limit is computed from quantities that don't depend on visit order, and *the minimum of a set is order-independent*.
3. **Build** — only now is geometry emitted.
4. **Clean** — `its_merge_vertices` / `its_remove_degenerate_faces` / `its_compactify_vertices`, then assert closedness.

Steps 3 and 4 cannot change a width, so no reordering can change the result. `solve_bevel_widths()` is exposed publicly so the tests pin order-independence **directly** rather than inferring it from the output mesh.

### The abandoned first approach, and why

*(Superseded. The shipped construction is the insert-and-re-triangulate one described above; this section records the dead end so the next attempt does not walk back into it.)*

The first approach was **corner displacement**: split each vertex into one copy per side and move each copy inward along its own face. Three rounds of fixes went into it, each correcting a real defect, and the third exposed a flaw in the premise itself.

| # | what was wrong | what it broke | fix |
|---|---|---|---|
| 1 | corners keyed **per facet** | a flat face tore along its own diagonal, because only the one triangle containing the bevelled edge moved | group facets into **sides** (below) |
| 2 | sides split only at **bevelled** edges | bevelling a *single* edge does not disconnect a closed surface, so the whole cube became one side and the chamfer collapsed | also split at **creases** |
| 3 | corner patches **predicted** from the original one-ring | at a vertex where a bevelled edge meets an unbevelled crease nothing is torn, so the predicted patch added a *third* facet to edges that already had two | derive patches from the **genuinely open edges** of the assembled mesh |

After all three the all-edges cases were close (the 12-edge chamfer's volume matched the closed form) but the partial-selection cases still failed. The diagnostic run that settled it printed, for the single-edge chamfer, `open=6 nonmanifold=16` — and the offending vertices were the corner-fill centroids, sitting at points like `(7.80, 10.00, 7.80)`, far out in the middle of a face rather than near the bevel.

**The premise is the problem.** A side's copy of a vertex is shared by that side's *whole* boundary at that vertex. Moving it therefore detaches the side from its neighbour along the **entire shared edge**, not merely near the bevel — so a single-edge chamfer on a cube opens a sliver running the full length of every edge adjacent to a moved corner. The all-12 case masked this exactly because there *every* neighbouring side moves too, and the slivers close against each other.

**What replaced it** (and what is in the tree now): do not move the shared corner at all. **Insert** the two rail vertices into the incident faces' boundaries and re-triangulate those faces around them, leaving every original vertex where it is. That is the research spec's own step (3) — *"re-cut the incident facets against the offset line in their own plane"* — which the first attempt deviated from precisely to avoid the re-triangulation. The deviation is what cost the correctness; the re-cut was the work, and there was no shortcut around it.

Two further defects surfaced and were fixed during that rewrite, both found from probe data rather than reasoning:

* **Strip ends were not capped.** The generic hole-filler was left to close them and instead centroid-filled a polygon spanning a whole face, putting a stray vertex at `(7.8, y, 7.8)` in the middle of the part and removing a *pyramid* `(1/3)(w²/2)L` where the *prism* `(w²/2)L` was right — the measured 1.667 against 5.0, a clean factor of three. Fixed by emitting explicit end caps.
* **The filler then double-covered those caps**, putting three facets on each of the cap's edges. Fixed by recording capped vertices and skipping any loop that touches one — per loop, so real multi-bevel corners are still filled.

Both are settled; what remains is the single winding bug named at the top of this section.

### The build — corner split rather than facet clipping

The spec's step (3) is "re-cut the incident facets against the offset line in their own plane, discarding the sliver". Implemented **per corner rather than per facet**, which is the one real design deviation and the thing that keeps the whole operation robust:

> Instead of clipping facets against lines — which needs robust 2D boolean work and produces slivers — each original vertex `v` is **split into one new vertex per *side*** that touches it, displaced inward by the bevels of the edges meeting at `v`. A facet then simply re-indexes its three corners to their split copies and **keeps its original shape**. No clipping at all.

**"Side", not "facet", and that distinction is the whole correctness of the pass.** A CAD face is generally several triangles — a cube's square face is two — and only *one* of them contains any given bevelled edge. Keyed per facet, that triangle's corner moves while its coplanar neighbour's stays put, and the mesh **tears along the diagonal between them**. That is not hypothetical: the first implementation was keyed per facet, and *every* bevel came back `Failed` at the closedness assertion.

Facets are therefore grouped into **sides**: maximal sets connected through edges that are **neither bevelled nor a crease**. Both halves of that condition are load-bearing, and the second is the subtle one:

* stopping at **bevelled** edges makes the bevel a cut between sides — but on its own it is not enough, because cutting a *single* edge does not disconnect a closed surface. A flood fill would put the whole cube in one side, the two faces meeting at that edge would share one vertex copy, and the chamfer would **collapse instead of opening**;
* stopping at **creases** as well means a side never spans two differently-oriented faces, so each face of the edge gets its own copy — while coplanar diagonals *within* a flat face are still crossed freely, which is what prevents the tear.

The crease threshold is the same `min_dihedral_deg` the solve uses to decide an edge is too flat to bevel, so the two agree by construction. A cube is then exactly 6 sides.

The holes this opens are exactly the pieces that have to be filled, and their boundaries are already known:

* along a bevelled edge → the **strip** (the four corner copies `(a,f0) (b,f0) (b,f1) (a,f1)` bound it),
* around a bevelled vertex → the **corner patch**.

Filling holes with known boundaries is robust in a way that clipping is not.

### Strips

* **Chamfer** — two triangles across the hole, the straight chord between the rails.
* **Round** — `N` bands whose intermediate rings follow the arc tangent to both faces, built by **spherical interpolation about the arc centre** (`centre_dist = w / cos(θ/2)` along the inward bisector). Slerp is exact for a circular cross-section and degenerates cleanly.
* **A chamfer *is* the one-segment case.** `rings` collapses to 1 for `Chamfer`, so there is exactly one geometry path and "N = 1 round equals a chamfer" is true **by construction** rather than by luck — and the test asserts it bit-for-bit.
* **Winding is self-correcting.** Both profiles emit their quads through one `emit_quad()` helper that checks the quad's own normal against the mean of the two incident face normals and flips the pair together if it got it wrong. The first draft hard-coded a winding derived by hand and it was **inverted** (caught by working the cube corner through on paper before the build finished); deriving it from the mesh instead means the two profiles cannot disagree and no orientation assumption is baked in.

The arc construction was checked numerically against the cube case before it was trusted: for a 90° edge at `w = 1` the centre lands at √2 along the bisector and both rails are exactly radius 1 from it, so the arc is a true tangent quarter-circle.

### Corner patches

Corner patches are found **from the mesh that has actually been built**, not predicted from the original vertex's one-ring. That is the third version of this step and the first correct one; the two that preceded it are worth recording because they failed in instructive ways:

1. *Per-facet corner split* — tore every flat face along its own diagonal (fixed by the side grouping above).
2. *Per-side corner prediction* — still wrong at a vertex where a bevelled edge meets an **unbevelled crease**: the two sides there resolve to the very same original vertex index, so nothing is torn, and emitting a patch anyway put a **third facet** on edges that already had two — non-manifold rather than closed. The all-12-edges cube passed throughout both attempts, because there every side transition *is* a bevel; the **single-edge chamfer** is what exposed it.

The working version collects the edges that are **genuinely open** (used by one facet, found by directed-edge counting over the assembled mesh), stitches them into loops, and fills each loop. An edge that was never torn is never open, so it is never touched; a hole that does exist is filled exactly once, whatever produced it. Robust by construction rather than by case analysis.

* **Three-sided hole → one triangle.** That is the ordinary cube corner, emitted as the single triangle it is rather than a centroid plus three slivers.
* **Four or more → a fan from the polygon's own centroid**, not from one of its vertices, which keeps it valid for the non-planar (often saddle-shaped) polygon a higher-valence patch is.

The boundary comes out **already oriented** (the unmatched direction keeps the solid on the correct side), and the winding is checked once per patch against the bordering facets' normals.

On a cube with all 12 edges bevelled this produces the **eight classic three-sided corner patches**, which the tests count explicitly.

The brief mentions the research spec's "refuse valence ≥ 4" first implementation. That restriction was **not needed**: the centroid fan over the cycle-ordered boundary handles any valence, so no valence limit is imposed. A vertex whose one-ring is *not* a clean cycle (a pinched or non-manifold vertex) is skipped, and the closedness assertion at the end catches the consequence rather than shipping a hole.

### Files

| file | what |
|---|---|
| `src/libslic3r/MeshEdit.hpp` / `.cpp` | `BevelProfile`, `BevelParams`, `BevelStatus`, `BevelResult`, `bevel_edges()`, `bevel_chain()`, `solve_bevel_widths()`, `chamfered_box_volume_loss()`, `is_closed_manifold()`; `EditSession::apply_bevel()` / `preview_bevel()`. |
| `src/slic3r/GUI/Gizmos/GLGizmoEdit.hpp` / `.cpp` | Width / Segments / Profile / Apply, live preview, and `commit_bevelled_mesh()` — the `clear_before_change_mesh()` path. |
| `tests/libslic3r/test_mesh_edit.cpp` | 11 new `[MeshEdit]` cases appended. |

### Panel

Offered **only for a chain selection** — a bevel acts on edges and a face region has none, so the panel shows the tool that applies to what is actually selected rather than a disabled control.

* **Width** (mm, world units — pulled back through the volume scale like the push distance), **Segments** (1…32, `BBLSliderScalar` as in GLGizmoCut), **Profile** (Chamfer / Round radio), **Apply bevel**.
* **Live preview of the result mesh**, not the strip. The brief allows either; the result mesh was chosen because the bevel is linear in the *selected edges* (not in the mesh) and so is fast enough to re-run on a slider tick, and because showing the actual result cannot disagree with what Apply will produce. The preview rebuilds only when something it depends on actually moved, and it skips the self-intersection guard — Apply pays for that once.
* The panel shows the **clamped width** and the **corner-patch count** *before* Apply is pressed, and names every refusal.
* Undo goes through the gizmo's existing stack (whole meshes, so renumbering is irrelevant to it).

### Tests — 11 new `[MeshEdit]` cases

| test | pins |
|---|---|
| **chamfering one cube edge** | watertight, manifold, **≥ +2 facets**, volume down by the prism `0.5 w² L` within 2%, and a facet with the exact 45° band normal exists |
| **a round bevel with one segment is exactly the chamfer** | bit-for-bit identical meshes — the documented degeneracy |
| a rounded bevel removes less than the chamfer | monotone in `N` over {2,4,8,16}, strictly between chamfer and cube, and at `N=16` within 10% of `(1 − π/4) w² L` |
| **all 12 cube edges at N = 4** | watertight, manifold, **exactly 8 corner patches**, volume between the round and chamfer bounds, flats still 20 mm |
| **all 12 cube edges chamfered** | volume loss within **5%** of the closed form `chamfered_box_volume_loss` — the tightest assertion, and the one that catches a corner patch removing the wrong wedge |

**Both closed forms were verified numerically before being trusted as yardsticks**, by Monte-Carlo integration of the solid they describe, over several sizes and widths:

* `rounded_box_volume()` — agrees to **0.01%**.
* `chamfered_box_volume_loss()` — the first draft was **wrong by 6%**, which would have failed its own 5% test. Summing the twelve prisms over full side lengths over-counts each corner, and the correction is not the naive "twice the three-prism intersection": the removed region at a corner is the *union* of three wedges, and sum-minus-union works out to **¾w³ per corner**, giving `loss = 2w²(a+b+c) − 6w³`. That now agrees with Monte-Carlo to ~0.1%.
| **a chain on a chamfered box** | the top-face rim bevels whole; watertight |
| **the too-small-face clamp** | a 10 mm chamfer on a 2 mm cube clamps to ≤ 2 mm, `clamped` set, still watertight, part not collapsed |
| **the solve is order-independent** | reversed *and* rotated permutations give permuted-identical widths |
| **the same bevel twice is bit-identical** | and a permuted edge list gives the same mesh, not merely the same widths |
| refusals are reported | `width = 0` → `NoOp` with the mesh unchanged; empty list → `EmptyChain`; a coplanar edge → dropped as flat, counted |
| the session applies and undoes a bevel | undo depth, topology follows the new mesh, redo, and a preview changes nothing |

**Result: 14 of 21 `[MeshEdit]` pass, 7 fail** (the 9 phase-1 cases still pass, so nothing was regressed). Every failure is `BevelStatus::Failed` — the mesh is built but not closed — and they are exactly the cases the construction flaw predicts:

| | |
|---|---|
| **passing** | the width solve and its order-independence; the same-bevel-twice determinism; every refusal path (`width = 0` identity, empty list, too-flat drop); all 9 phase-1 cases |
| **failing** | chamfer one cube edge; round vs chamfer over segment counts; all-12 at N = 4; all-12 chamfered; chain on a chamfered box; the too-small-face clamp; the concave drop's convex control |

The all-12 chamfer *did* match the closed-form volume at one point mid-debugging (after the side grouping, before the corner rewrite), which is the clue that led to the diagnosis: the all-edges cases can close by accident because every neighbouring side moves too.

---

## Deviations from the brief

1. **`MeshRepairStub.cpp` is new.** Before this, `remesh_by_voxels()` was simply *unresolved* in an OpenVDB-less build. That was survivable only because every shipping configuration has OpenVDB — but it also meant there was no way for a caller to *ask* whether the voxel operations exist. Both menu entries need that answer to hide themselves, so the stub supplies the symbols and `voxel_ops_available()` returns false. It is never compiled alongside `OpenVDBUtils.cpp`.

2. **The bevel re-cuts per corner, not per facet.** The spec says "re-cut the incident facets against the offset line"; the corner split achieves the same result without any clipping. Rationale above.

3. **No valence ≥ 4 refusal.** The spec suggested refusing those in a first implementation. The centroid fan over a cycle-ordered boundary handles them, so the restriction would have been an artificial limit.

3a. **Concave edges are dropped, and this is a real limitation — the one thing in step 2 that is not finished.** The corner split pulls each face-corner back *along its own face*, which removes material. That is right for a convex (outside) edge; a concave (inside) edge needs the strip to *add* material into the valley, and pulling its corners back instead makes the two rails cross and the strip fold through the solid. So `solve_bevel_widths()` drops concave edges, `BevelResult::dropped_concave` counts them, and the panel names them specifically and points at **Round all edges**, which *does* fillet inside corners via the level-set close. A mixed selection still bevels the convex edges it can and reports what it dropped. Building a concave strip is the obvious next piece of work.

4. **The preview shows the result mesh, not the strip.** Explicitly allowed by the brief ("else the result mesh as preview"), and chosen for the reasons above.

5. **`remesh_keep_flat_bottom()` is reused rather than reimplemented**, with the radius passed in its `voxel_size` slot. That is a deliberate use of an existing parameter for the overlap distance it controls, commented at the call site so it does not read as a mistake.

6. **`/m:1`.** `/m:2` hit `C3859` / `C1076` (PCH virtual memory exhausted) across most of libslic3r on this machine. Single-threaded is the documented remedy.

---

## Click-tests

> **These have NOT been run.** No GUI build was produced — the worktree built and ran `libslic3r_tests` only, and `libslic3r_gui` was never linked, so nothing below has been clicked. They are written as the script to follow, not as a record of results. The bevel half (8–15) cannot pass as things stand in any case: the geometry is unfinished and Apply will report a failure.
>
> The GUI code *has* been compiled as far as the test target requires — which is to say not at all for `GLGizmoEdit.cpp` and `RoundDialog.cpp`. **Those two files are unverified even at the compiler level.** Building `libslic3r_gui` is the first thing the next session should do.

Intended script, against a Release build of the worktree, on a 20 mm cube unless noted.

### Round all edges

1. Select a cube → right-click → **Round all edges…** sits directly under **Repair/Remesh**.
2. Radius 2, leave the voxel field alone → it shows `0.333` (r/6). **Round**. The part comes back with every edge filleted and the flats still square. Triangle count rises to roughly the dialog's estimate.
3. With **Keep the bottom flat** on, the base is dead flat on the plate and its outline is unchanged; slicing shows a full first layer. Turn it off, round again: the bottom edge is filleted and the first layer is a smaller square.
4. Type a radius of 12 on the 20 mm cube → the red warning appears ("its shortest side is 20 mm and the rounding needs twice the radius to fit") before **Round** is pressed.
5. Paint a support blocker first, then round → the notification reports the triangle change and the blocker is gone, as the dialog's grey note warns.
6. Open the Edit gizmo, press **Round all edges…** at the bottom of the panel → the gizmo closes, the dialog opens, and the result is the same as from the menu.
7. Undo → the original part returns.

### Bevel

8. Edit gizmo → **Edge chain** mode → hover a top edge of the cube; the chain highlights. Click it.
9. The **Bevel** section appears with Width / Segments / Profile / Apply. The face push/pull section is *not* shown (nothing is selected that can be pushed).
10. Profile **Chamfer**, width 1 → the preview shows the chamfered edge immediately; the segment slider is greyed by the hint line. **Apply bevel** → flat 45° band, part still watertight, slices clean.
11. Profile **Round**, segments 6, width 1 → the preview updates as the slider moves. Apply → a smooth fillet on that edge.
12. Select each of the 12 edges in turn and bevel → the corners where three bevels meet are closed by a patch; the panel reports the corner count as they accumulate.
13. Set width 30 on the 20 mm cube → the panel warns "Width clamped to …" with the value it will actually use, before Apply.
14. Apply, then Ctrl+Z inside the gizmo → the bevel is undone; Ctrl+Y redoes it.
15. Paint an MMU colour, then bevel → the colour is cleared, as the panel's line states.
