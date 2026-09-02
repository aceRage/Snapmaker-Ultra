# Colour Split — Design Spec

Date: 2026-09-01 · Rev 2.14 (after adversarial review; §3.1a/§3.4/§3.4a/§3.6/§3.9/§7 refined during planning, Tasks 3–10; §12 added at Task 11; §3.1/§3.9/§12 amended by Ruling 28, the final fix wave) · Status: implemented (v1) — GUI round pending
Research: `docs/colorsplitting_research.md` · Worktree: `C:\Dev\SnapmakerOrcaNext`, branch feat/color-split off
Snapmaker-Ultra main dff2c65eab (the paint-depth merge). A copy of this file lives in that worktree at
`docs/superpowers/specs/2026-09-01-color-split-design.md`; the worktree copy is the binding one once committed.

## 1. Goal

Add **Split → By painted colour**: convert the MMU paint of a model part into explicit solid parts, in place.
One part per painted filament — a solid of **normal thickness D** under its painted surface whose side walls
follow the surface normal at the paint boundary — plus the remaining **body** part. The parts partition the
original volume exactly (no clearance gap, no double coverage). Afterwards the object slices through the
ordinary multi-part path (own perimeters per part, later-part-wins overlap clipping), parts can carry per-part
settings, export as separate bodies (3MF), or become objects (existing Split → To objects).

This is the geometric realisation of the paint-depth intent — normal thickness D everywhere — without the
per-layer approximations of the 2D segmentation (the 24° surface-layer ceiling, the mm-mode top-surface issues).

## 2. User decisions (2026-09-01)

1. Engine: exact shells + Manifold booleans; the OpenVDB voxel engine is the documented fallback (§10).
2. Depth mirrors the paint-depth settings (walls/mm → D via `paint_depth_band_mm`, classic floor); flat
   tops/bottoms capped at solid-shell depth (default on); unlimited available.
3. Output: parts in the same object, body first then colour parts; the source part and its paint are consumed.
4. The feasibility spike (§9) runs first, in the worktree.

## 3. Geometry

Notation: M = the source part's mesh (closed and 2-manifold after exact vertex welding). Paint states
s = 1..K with state value == 1-based filament id; S_s = the strict patch of state s; S_0 = the unpainted
remainder; F = S_0 ∪ … ∪ S_K, a conforming T-joint-free retriangulation of M's surface. h = layer height,
ws = wall stack = outer-wall width + outer-wall spacing (§3.3).

**3.1 Patches.** One `TriangleSelector sel(M)`; `sel.deserialize(volume.mmu_segmentation_facets.get_data(), false)`;
`S_s = sel.get_facets_strict(EnforcerBlockerType(s))` for every s in `get_data().used_states` and s = 0. All
calls share one vertex pool (TriangleSelector.cpp:1478-1502), so F is the concatenation of the index lists;
compactify per piece only. Adjacency = `its_face_neighbors(F)`; F must have zero open edges, else refuse (§7).
States above the printer's filament count: physical overflow is skipped with a warning (those facets count as
unpainted); mixed-filament virtual ids are kept and become the part's extruder, as the 2D path does
(PrintApply.cpp:1885-1893) — the plan verifies the extruder clamp in `region_config_from_model_volume` accepts them.
**Rev 2.14 (Ruling 28(2)), implemented:** the limit travels as `ColorSplitParams::max_state` (0 = no limit, for
a caller with no printer in hand). `split_volume_by_paint` re-labels every facet of a state above it to state 0
— the facets stay in F, they simply join the body — and pushes ONE warning per dropped state, "Filament _k_ is
not available on this printer; its paint stays in the body colour.", ahead of the shell warnings; if that empties
the state list the split raises the same `nothing_to_split` kind carrying those notes as its message. The GUI
sets `max_state = fff_print().mixed_filament_manager().total_filaments(num_extruders)` with `num_extruders =
filament_diameter.size()`, i.e. exactly Print::apply's `num_total_filaments` (PrintApply.cpp:1379, 1484), so the
mixed VIRTUAL ids stay valid and only real overflow is dropped. Without this a part would carry `extruder = k`,
which slice time clamps back to filament 1 while the object list shows a chip for a filament that does not exist.

**3.1a Smooth-patch decomposition (rev 2.6, Ruling 18; replaces the refinement pre-pass of rev 2.4/2.5).**
A single offset surface cannot represent two claims that overlap inside the part: on a painted boss the top
cap's bottom disc crosses the side wall's bottom tube near the axis, the shell self-intersects, the fold guard
shortens it and a hidden core survives. Therefore a state's facets are grouped by edge connectivity only across
edges whose dihedral angle is below 30° (n₁·n₂ > cos 30°): each **smooth patch** becomes its own shell, and the
sequential Split (§3.8) takes the overlapping patch shells one after another. At a boundary edge whose outside
facet carries the SAME state (a crease inside the painted region) the wall follows the patch's own mean normal
n_P with no step and no bisector, so the top slab's walls go straight down and the side tube's walls straight
inward; a coarse two-ring STL cylinder therefore needs no extra vertices (its ring vertices' patch normals are
radial). Uniform (Manifold `RefineToLength`) and edge-selective refinement were both measured in Task 5 and
rejected: neither helps, and chord-interior vertices disorder the bottom ring near the axis.

**3.2 Normals.** n(v) = `NormalUtils::create_normals(F, VertexNormalType::AngleWeighted)` — angle weighting is
triangulation-independent, so a cube edge gets the exact 45° bisector; always computed on the FULL surface F,
never on a patch alone, so boundary vertices carry the true surface normal (the bisector at a crease — the
taper the user asked for, and the same bisector the 2D Voronoi segmentation produces).

**3.3 Depth D and wall stack ws.** Both come from the source part's effective config
`full_config ⊕ object.config ⊕ volume.config` (the overlay `region_config_from_model_volume` applies,
PrintObject.cpp:3275-3290). Per filament f in {body extruder} ∪ {painted filaments}:
width = `outer_wall_line_width` (0 → `line_width`; percentages over `nozzle_diameter[f−1]`), spacing likewise
for `inner_wall_line_width`, at `layer_height` (`Flow::new_from_config_width`, Flow.cpp:111-125, the resolution
PrintRegion::flow uses, PrintRegion.cpp:72-101); D_f = `paint_depth_band_mm(paint_depth_mode, paint_depth_walls,
paint_depth_mm, ext_w, ext_s, spacing)` with `paint_depth_band_classic_floor_mm` when `wall_generator` is classic.
D = max_f D_f and ws = max_f (ext_w + ext_s) — the same "widest region wins" rule as MultiMaterialSegmentation.cpp:3903-3925.
Unlimited mode → D = ∞ (the per-vertex clamp below then bounds every depth). A public helper
`color_split_depths(config) → {D, ws, cap_top, cap_bottom}` feeds the dialog; the user may override D.

**3.4 Per-vertex depth d(v) (rev 2.2).** d(v) = min(D, t(v)/2 − δ) with δ = 0.002 mm, where t(v) is the
thickness of M along −n(v) from v (`AABBMesh::query_ray_hits` from v − ε·n(v), first hit farther than 5ε; no
hit → t = ∞). The half-thickness clamp reproduces the 2D Voronoi split: a wall thinner than 2D is shared at
mid-thickness with whatever faces it (another colour or the unpainted far side), and Unlimited = "everything up
to mid-thickness", exactly what the 2D unlimited mode gives. Because no bottom ever crosses the mid-surface,
shells never nest or invert, so Manifold only ever sees ordinary solids. A feature painted the same colour on
both sides (a pin, a boss, a thin plate) becomes two half-shells whose bottoms stop δ short of each other; the
2δ sliver between them is an enclosed body island (absorbed into the colour by §3.8) or, where it connects to
the body, a sub-resolution sliver the slicer drops — the pin still prints entirely in its colour.
Fold guard: for every group triangle compare the reversed bottom triangle's normal with the top's; where the
dot product ≤ 0 or the bottom area < 10⁻⁶ of the top's, halve d at that triangle's vertices and repeat (≤ 8
rounds, floor d = h). After construction each shell is checked with `MeshBoolean::cgal::does_self_intersect`;
a still self-intersecting component halves its d uniformly and rebuilds; if it still fails at its floor depth the
component is skipped with a warning naming the filament (§7, rev 2.3) so one micro-feature cannot block the split. Small convex features (fillets, bosses, spheres with r < D) thus
get a thin skin instead of a folded shell; their interiors come back through §3.8.

**3.4a Mitred offsets (rev 2.9, Ruling 24).** d(v) is a depth measured perpendicular to the patch. A segment
that travels along the vertex bisector n(v) is therefore given the length d / max(n(v)·n_P, 0.5) — the classic
mitre, limited at a 60° half-angle — and only then clamped by the half-thickness probe along that direction.
Without the mitre a face whose only vertices are its corners (a plain cube face) received d/√3 of depth: the
Task 8 parity measurement showed 45.9 mm²/layer against the 2D claim's 54.4 (= 40·D − D²). With it the corner
offsets land at the 45° Voronoi diagonal the 2D segmentation produces, and the two agree to the interlocking-notch
amplitude. Segments along n_P (Case A, Case B's first segment, concave and same-state walls) are unaffected.

**3.5 Depth groups (flat cap).** Transliteration of the live 2D rule (`flat_cap_component_ex`,
MultiMaterialSegmentation.cpp:1493-1578; tests test_paint_depth_clamp.cpp:5107-5200): a facet is flat iff
tan θ < h / (3·ws) (≈2.2° at h = 0.1 mm; a 3° face is NOT flat); within each S_s, edge-connected flat components
whose XY projection survives an inward offset of 1.5·ws form **capped groups**; everything else forms uncapped
groups at d(v). Cap depth: N_eff = `effective_shell_layers_by_thickness` semantics — `top_shell_layers` (or the
number of layers spanning `top_shell_thickness` when that is deeper), 0 when the layer count is 0 —
d_cap = max(h, N_eff·h), i.e. a zero-shell object keeps only its surface layer; bottom-facing groups
(n_z < 0) use the bottom_* keys. The cap applies only when D ≥ ws and d_cap < D (the 2D gates,
MultiMaterialSegmentation.cpp:1874-1877, 2228); capped groups use min(d(v), d_cap). Flat cap off ⇒ one group per patch.

**3.6 Side walls at creases (rev 2.1).** Every boundary vertex a of a group gets an intermediate ring vertex a₁
and a bottom vertex a′; the side of the shell is two strips (b,a,a₁,b₁) and (b₁,a₁,a′,b′). Let n_P be the mean
normal of the group's facets at a and n_Q the mean normal of the outside facets across a's boundary edges. A
boundary is a **crease** when n_P·n_Q < cos 15°.
- Plain boundary (no crease — a paint edge inside a smooth face): a₁ = a − h·n(a), a′ = a − d(a)·n(a).
- Crease, P more horizontal than Q (|n_P·z| > |n_Q·z|: painted top meets a side face, painted floor meets a
  wall): a₁ = a + ws·t_in(a) − h·n_P, a′ = a₁ − (d(a) − h)·n_P, with t_in the unit inward tangent of P at the
  boundary (t_in = normalize(n_P × (b − a)) for the group's CCW boundary edge a→b). This is the 2D F1 contour
  inset: the surface layer claims to the edge, every layer below stays one wall stack away from the side
  surface, so the side face keeps a body-coloured outer wall (without it a zero-width body wedge grows at 45°
  from the edge and the painted colour shows on the side face for the top ≈ws).
- Crease, P less horizontal (painted side face meets a more horizontal neighbour, e.g. the top): a₁ = a − ws·n_P,
  a′ = a₁ − (d(a) − ws)·n(a). The painted side keeps its full outer wall stack right up to the edge (a plain
  bisector would thin the piece to a sliver there and show body colour on the side face's top ≈ws); after that
  the bisector taper limits the painted rim on the neighbouring top face to one wall stack, where the 2D band
  would paint a D-wide rim — a deliberate, documented deviation.
- Concave crease (n_Q · t_in > 0: the outside neighbour rises over the painted face, e.g. a painted boss side
  meeting the block top, Ruling 14): a′ = a − d(a)·n_P with no step and no bisector, so the piece never leaves the
  painted feature's own footprint (a bisector would carry a hidden painted skirt into the neighbouring body and cost
  toolchanges on layers that carry no paint). This rule is always on, independent of the crease-step option.
- Ties (rev 2.10, Ruling 25): the convex cases need a STRICT ordering — Case A iff |n_P·z| > |n_Q·z| + 10⁻³,
  Case B iff |n_P·z| < |n_Q·z| − 10⁻³. Two vertical faces (or two equally sloped ones) have no layer asymmetry,
  so their crease takes the plain mitred bisector: the 45° Voronoi diagonal the 2D segmentation produces. Holding
  a wall stack there (Case B) would claim ws·(2D − ws) per layer more than the 2D path (Task 9 measurement).
- Per-vertex classification (rev 2.11, Ruling 26, known limit): the crease case is chosen once per boundary
  vertex (one ring copy). A vertex where a Case-B edge meets a tie edge — every corner of a four-vertex face —
  is Case B, so the tie edges inherit the wall-stack hold: on a plain cube face the piece claims ws·(2D − ws)
  per layer more than the 2D Voronoi diagonal (≈1.6 mm² at stock settings, invisible, ≤ one wall stack along
  the vertical creases). Per-edge classification would remove it and is deferred. The end-to-end parity test
  compares against the 2D path with its interlocking notch disabled (a 2D-only artefact).
- Width guard (rev 2.7, Ruling 22): the Case A inset is applied to a group only if the group's projection onto
  the plane perpendicular to its mean normal survives an inward offset of ws (the flat cap's own yardstick);
  otherwise the group's Case A vertices use the plain bisector rule. A painted stroke narrower than 2·ws
  (embossed text) therefore keeps a valid tapered shell instead of inverting its ring and being dropped — a
  deliberate deviation from 2D parity, which would claim only the surface layer under such a stroke.
- Mid-thickness on every path (Rulings 20/21): the depth of each segment is re-probed along the direction it
  actually travels (Case A along n_P from the ring's surface position projected back onto the surface, Case B's
  first segment along n_P from the vertex, concave and same-state walls along n_P), so no wall ever passes the
  mid-thickness whatever the vertex's bisector normal says.
When d(a) leaves no room for the second strip (d(a) − first segment ≤ h) a′ collapses onto a₁ and only the first
strip is emitted. Known approximation: for side surfaces that lean inward going down, the inset shrinks below
ws at depth (2D re-insets per layer).

**3.7 Shell construction.** For every edge-connected component C of every depth group: top = C's triangles;
bottom = the same triangles on the offset vertices with reversed winding; sides = for each boundary edge (a,b)
the strip (b, a, a′, b′) (with the intermediate ring a₁,b₁ when §3.6 applies), oriented outward. Boundary
vertices with more than two incident boundary edges (figure-8 loops, an annulus whose hole touches the rim)
are duplicated per wedge in the shell mesh so every edge has exactly two faces; `MeshGL::Merge()` only merges
along open edges (mesh.h:167-173) and leaves them alone. A patch covering the whole closed surface has no
boundary: its shell is top ∪ bottom (a hollow solid) — valid as long as the bottom does not fold (§3.4).
Each component is its own Manifold (built from `MeshGL64` with an explicit tolerance); components are never
unioned.

**3.8 Partition by sequential Split.** rest ← M. For filaments in ascending order and, within a filament,
capped groups before uncapped, component by component: `(piece, rest) = rest.Split(shell)` (manifold.h:232);
piece is appended to that filament's mesh (several closed shells in one `TriangleMesh` slice fine). Body = final
rest. This makes piece/body complementary from one evaluation (no ε mismatch), never unions touching or
coincident solids, and settles overlaps by order (lower filament index wins; component order is irrelevant).
Then **enclosed body islands** (default on): `Decompose()` the body; a component none of whose faces originate
from M (`runOriginalID` provenance) is fully enclosed by colour pieces and invisible; it is merged into the
piece whose shell contributed most of its surface (ties → lowest filament). This restores whole small features
(a painted 1 mm boss is entirely its colour, as the 2D path claims it) while thin walls keep the mid-split.
Validation before touching the model: every Manifold `Status() == NoError`; `Volume()` of body + Σ pieces
within 10⁻⁴·Volume(M) of Volume(M); empty pieces are dropped with a warning; an empty body is allowed (§4).

**3.9 Coordinate space.** D, ws, h are world millimetres. Let T = instance × volume matrix. If T's scale is
isotropic (scale s) **and T preserves Z** (rev 2.14, Ruling 28(1)) the split runs in mesh space with D/s, ws/s,
h/s — exact for every instance sharing that transform. Z-preserving means mesh +z maps to world +z with no tilt:
with L = T.linear(), |L(0,2)| < 10⁻⁶·s, |L(1,2)| < 10⁻⁶·s and L(2,2) > 0. A turn about z, an x or y mirror and
any uniform scale all satisfy it and keep the cheap exact path; a rotation about x or y, or a z mirror, does not.
Isotropy alone is **not** enough, because §3.5 and §3.6 are print-frame rules ("flat", "more horizontal than its
neighbour", up-facing vs down-facing caps) that the shell stage reads off the z of the space it actually runs in
— the flat test, the flat-core projection, the case A/B choice and the group mean-normal fallback all compare
against that z. A cube turned 90° about x with its world top painted would otherwise be classified as a painted
side face: case B at every corner, the wall stack held to what the printer sees as the side faces (painted colour
on them for the top ≈ws) and the flat cap taken on the wrong faces; a z mirror would swap cap_top for cap_bottom.
Otherwise — anisotropic (no single depth scale) or tilted — (rev 2.8, Ruling 23) the paint is still read in mesh space — `extract_color_patches` runs on the
untransformed mesh — and only the extracted patch surface F is transformed by T of the first instance (with the
left-handed fix; a per-triangle vertex swap leaves per-facet states intact, whereas transforming the raw mesh
first would mirror the sub-facet paint of partially painted facets); the pieces are transformed back by T⁻¹
(same flag). `depth_override_mm` is a world length like D and is scaled on the mesh-space path too
(`scale_params`); `ColorSplitResult::depths` is reported in split space. Other instances with a different
anisotropic scale get an approximate depth (documented limit).

**3.10 Not done by the split.** No interlocking notch (the 2D `mmu_segmented_region_interlocking_depth` has no
geometric counterpart; a dovetail is future work). No transfer of seam/support/fuzzy-skin paint (the source
mesh is replaced). Modifiers, negative volumes and layer-range settings of the object are untouched.

## 4. Model changes

Library: `src/libslic3r/ColorSplit.hpp/.cpp` (may include `manifold.h`; libslic3r links Manifold PRIVATE):

```
struct ColorSplitParams { PaintDepthMode mode; int walls; double mm; double depth_override_mm; // <= 0: computed
                          bool flat_cap = true; bool absorb_islands = true; bool solid_interfaces = true;
                          bool paint_infill_override = true; };
struct ColorSplitDepths { double D, ws, cap_top, cap_bottom, layer_height; };
ColorSplitDepths color_split_depths(const DynamicPrintConfig &effective, const std::vector<int> &filaments);
struct ColorSplitResult { indexed_triangle_set body; /* may be empty */
                          std::vector<std::pair<int, indexed_triangle_set>> pieces; // filament, mesh (ascending)
                          std::vector<std::string> warnings; ColorSplitDepths depths; };
ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &, const ColorSplitParams &,
                                       const std::function<bool(int)> &progress /* false = cancel */);
void apply_color_split(ModelObject &, const ObjectID &source_volume_id, ColorSplitResult &&, const ColorSplitParams &);
```

`apply_color_split` (UI thread, after `take_snapshot`): every output is a NEW volume through the public
`ModelObject::add_volume(const ModelVolume &source, TriangleMesh &&)` (copies name/config/transformation, centres
the mesh — the `(other, mesh&&)` constructor is private, so nothing is inserted in place); placement is then
corrected for rotation/scale with `v->set_offset(src.get_offset() + src.get_transformation().get_matrix_no_offset() * v->mesh().get_init_shift())`
(the plain `ModelVolume::split` recipe misplaces rotated parts); `text_configuration`, `emboss_shape` and
`cut_info` are reset on every new volume (the constructor copies them; a text part must not be regenerable);
annotations are empty by construction, so no stale `used_states` reach `PrintApply`. Names: body keeps the
source name, colour parts `<name> F<filament>`. Extruders: colour parts `config.set("extruder", filament)`;
the body gets an explicit `extruder` only if the source had the key (else it inherits the object's, and the
Ultra `outer_wall_filament` reset at PrintObject.cpp:3257-3259 is not re-triggered). With
`paint_infill_override == false`, colour parts additionally get `sparse_infill_filament` = body extruder (the
2D behaviour at PrintApply.cpp:1096-1103). The source volume is deleted by index; the new volumes are rotated
into its former position (body first, then parts ascending) so modifiers/negatives keep their relative order
and the body-first slice-time clipping order holds. Empty body ⇒ no body volume; the first piece takes the
position. `solid_interfaces` ⇒ object `interface_shells = true` (needed explicitly: `has_bounded_paint_depth()`
requires `is_mm_painted()`, Print.hpp:514). `Print::apply` sees the change through `model_volume_list_changed`.

## 5. GUI

- Menu: **Split → By painted colour** next to "To objects / To parts" in GUI_Factories.cpp (object menu :1424-1436
  and :1462-1474, part submenu :1595-1607, multi-selection :1987-2002; the plain part menu :1540-1549 has a
  single Split item and gets a sibling). Enabled when the selected object/part has a MODEL_PART with
  `is_mm_painted()`; several painted parts are split one after another.
- Dialog (modal): depth mode/value prefilled from the effective paint-depth settings with computed D, ws and
  cap depths shown; D override; "Cap flat tops/bottoms at solid shell depth" (on); "Absorb enclosed islands"
  (on); "Keep base-colour sparse infill" (default = the object's `paint_infill_override` inverted); "Solid
  colour interfaces (interface_shells)" (on); summary "K painted filaments, N triangles". OK starts the job.
- Job (`ColorSplitJob : Job`): refused while a painting gizmo is open (`get_current_type()`, the pattern at
  GUI_ObjectList.cpp:2554-2577 resets gizmo states) and while `m_worker` is not idle. It stores object and
  volume `ObjectID`s plus the paint timestamp; `process` runs `split_volume_by_paint` on copies off-thread with
  progress via `Ctl::update_status` and cancellation wired to Manifold's `ExecutionContext::Cancel`; `finalize`
  re-finds the object/volume by id, aborts with a notification if the paint timestamp or mesh changed, then
  `take_snapshot("Split by painted colour")`, `apply_color_split`, `ObjectList::add_volumes_to_object_in_list`
  (public rebuild), `changed_object`, `notify_instance_updated`, `update_info_items`, `input_file.clear()`, and
  selects the new parts. Errors surface through `show_error` with the reason; warnings as a notification.

## 6. Slicing consequences (verified)

Colour parts get their own perimeters (`wall_filament` differs, Layer.cpp:179-184); their interior beyond the
walls is sparse infill in their colour unless "Keep base-colour sparse infill" is chosen (§4); the body first /
parts later order makes parts win overlap clipping (PrintObjectSlice.cpp:30, 420-437); `interface_shells`
governs solid skins at part interfaces (PrintObject.cpp:1337-1338, 1772-1773); flush rules are per object and
unchanged; the Ultra `outer_wall_filament` reset lets each part's outer wall follow its own extruder.
After the split the object's paint-depth settings are inert (no paint left) unless other parts are painted.

## 7. Errors

| Condition | Behaviour |
|---|---|
| Source mesh has open edges after welding | Refused by the job before any model change (rev 2.12): "The part is not watertight; repair it before splitting by colour." |
| Part not painted / paint resolves to nothing | Menu item disabled / job ends with a notification, no change |
| A painting gizmo is open or a job is running | Menu action refused with a notification |
| A component's shell self-intersects even at its floor depth (a painted feature smaller than about two layer heights) | That component is skipped with a warning naming the filament and its size; the rest of the split proceeds (rev 2.3) |
| Manifold status ≠ NoError, or the volume check fails | Error "Could not split <part> by colour (reason)", model untouched |
| Piece for filament k empty | Dropped, warning listed in the result notification |
| Paint or mesh changed while the job ran | Finalize aborts with a notification, model untouched |
| Cancel | Job stops (Manifold context cancelled), model untouched |

## 8. Testing

Tag `[colorsplit]` in `tests/libslic3r/test_color_split.cpp`, reusing the paint-depth harness recipe
(`TriangleSelector` on a synthetic mesh → `mmu_segmentation_facets.set`), `its_make_*` fixtures and the sliced-
region readers from `test_paint_depth_clamp.cpp`:

1. Patch extraction: two states on a cube share boundary vertex indices; F has zero open edges.
2. Shell validity: flat, convex cap, concave groove, convex fillet r < D, 2 mm boss, sphere r < D, embossed
   text, a fully painted closed patch, thin wall (t < 2D, one side and both sides painted): every shell has
   Status NoError, zero open edges, no CGAL self-intersection, Volume within tolerance of the expectation.
3. Pinch boundary (annulus touching the rim, figure-8) builds a valid shell.
4. Flat cap: 40×40 cap → d_cap; 3°/4°/5° and 10°/20° slopes → uncapped; a ledge beside a riser capped whole;
   `top_shell_layers = 0` → surface-layer-only claim.
5. Wall-stack step: painted cube top → below the surface layer the piece stays ws from the side faces; painted
   side face meeting the top → no step.
6. Partition: Σ Volume = Volume(M) within 10⁻⁴; pairwise piece intersections empty; body ∪ pieces watertight;
   enclosed-island absorption (painted sphere r < D ends up wholly its colour).
7. End-to-end: paint a spot on a cube, split, `Print::apply` + slice the multi-part object. On the vertical-wall
   fixture the per-layer painted region equals the 2D paint-depth segmentation of the unsplit object (walls
   mode) within one line width; on slope/top fixtures the piece thickness is checked against the intent
   (normal thickness D), not against the 2D output, which differs by design on 24–45° slopes.
8. Transform round trip: a rotated and a mirrored PART, and an anisotropically scaled instance, stay in place
   (world bbox unchanged) with D in world mm.
9. Unlimited mode; multi-patch same colour; three adjacent colours; paint cutting through original triangles
   (T-joint path); mixed-filament virtual id.
10. Performance guard: 100k-triangle painted sphere in bounded time (value set by the spike).
GUI round: paint, split, inspect list/filaments, slice, undo/redo, 3MF round trip (parts persist as separate
bodies with their extruders), try with the painting gizmo open (refused).

## 9. Spike (first plan task; throwaway code)

S1 Manifold and shells: the §8.2 fixture set — record Status, Volume vs expectation, and validity of
`rest.Split(shell)`; include coincident side walls of two adjacent colours. S2 half-thickness clamp on thin
walls (one side / both sides painted) and the fold guard on r < D features. S3 timing: `Refine`d sphere to
~100k triangles, one and three colours, with and without the CGAL self-intersection check. S4 slice
comparison (§8.7) and the painted-cube-top wedge with and without the wall-stack step (§3.6). Outcome selects:
engine A as designed / A with a reduced check set / engine B.

## 10. Fallback engine B (documented, built only if the spike fails)

OpenVDB (8.2, linked into libslic3r): level set of M with `meshToVolume(..., polygonIndexGrid)` over the
colour-labelled F gives, for every interior voxel within depth D, the closest surface triangle and hence its
colour — a depth-bounded 3D Voronoi partition. Each label region is meshed (`volumeToMesh`), dilated by one
voxel and intersected with M (Manifold) to restore the exact outer skin. Robust to any geometry; costs:
voxel-resolution internal walls, ~1-voxel wobble of the visible colour edge, heavier meshes and memory
(0.1 mm voxels on a 100 mm part ≈ 10⁷–10⁸ band voxels).

## 11. Deferred

Per-colour depth table; geometric interlock (dovetail) at colour walls; re-projection of seam/support paint
onto the new parts; a "split and keep the original" variant (duplicate first); batch "split all painted
objects"; SLA.

## 12. Measured

Implemented on `feat/color-split` in `C:\Dev\SnapmakerOrcaNext` (Tasks 1–11, 2026-09-01/02). Every number
below comes from the committed test suite, Release x64, Manifold 3.5.2 with `MANIFOLD_PAR=OFF`; the full
record is `.superpowers/sdd/2026-09-01-color-split/spike-report.md`.

**Tests** (rev 2.14, after the Ruling 28 fix wave). `libslic3r_tests.exe "[colorsplit]"` 941 assertions in 59
cases; `"[colorsplit_spike]"` 24 in 3; `"[paintdepth]"` 1568 in 94; `"[chameleon]"` 605 in 133; the whole binary
52 612 assertions in 584 cases, 2 of which are the pre-existing cases that fail as expected. (At rev 2.13 the
`[colorsplit]` figure was 912 in 56 and the binary 52 583 in 581.)

**S1 — engine (§9): 2 mm boss on a block, painted whole.** The painted region is two smooth patches (§3.1a),
so two shells: side tube 9.37426 mm³, top slab 4.43006 mm³. The piece after the partition is 9.37566 mm³ =
99.48 % of the 9.42478 mm³ of boss standing above the block, with z_min = 10.000 — nothing of it hides inside
the body. The 1 mm of cylinder buried in the block bounds no painted facet and was never reachable; what stays
body-coloured is an interior core, never a visible surface (§3.8).

**S3 — timing.** 99 224-triangle sphere, r = 20 mm, D = 1.5 mm, default params: **one colour 1.013 s, three
colours 6.53 s**. Stage breakdown at 69 520 shell triangles: `extract_color_patches` 0.020 s,
`build_color_shells` 0.230 s — of which the CGAL self-intersection check 0.104 s, 45 % of that stage and
10.3 % of the whole split — and `partition_by_shells` 0.756 s. The check earns its 10 %: engine A as designed,
no reduced check set, no engine B (§9, §10).

**S4a — slice parity on a vertical wall (§8.7).** 40×40×20 cube, +X face painted, sliced both ways with
D = 1.40885 mm and ws = 0.79708 mm, layers 25–74. The 2D claim is 54.3691 mm² on every layer (odd and even
alike, with the 2D-only interlocking notch disabled, Ruling 26); the 3D piece is 55.9797 mm² on every layer,
the derived 40·D − (D − ws)². **Worst difference 1.61063 mm² of the 4.0 mm² bound (40 %)**, and it matches the
case-B corner hold ws·(2D − ws) = 1.61059 to 4·10⁻⁵ — the residual is §3.6's geometry, not error.

**S4b — painted cube top, the wall-stack step (§3.6).** Body area on layer 98 (print_z 19.8) of the same cube
with its top painted: **47.6398 mm² with the step off, 124.991 mm² with it on**, against the exact one-stack
ring 4·40·ws − 4ws² = 124.99 mm². With the step the piece sits exactly one wall stack in from all four side
faces; without it the body keeps a 0.30 mm ring, under one outer wall line, and the top colour would print out
onto the side faces.

**Documented limits.**
- **Per-vertex crease classification** (§3.6, Ruling 26): the crease case is chosen once per boundary vertex,
  because a vertex has one ring copy. Per-edge classification is deferred.
- **A four-vertex face therefore holds one wall stack along its vertical creases**: every corner of a plain
  cube face is case B and the tie edges between them inherit the hold, so the piece claims
  ws·(2D − ws) ≈ 1.61 mm² per layer more than the 2D Voronoi diagonal at stock settings — the whole of the
  S4a residual above, and ≤ one wall stack wide.
- **Partition cost is O(shells × mesh)** (§3.8): every shell is a separate Manifold `Split` against the whole
  remainder, so the work grows with the number of smooth patches (§3.1a) times the size of the part. The S3
  timing above is the smooth case — a sphere is a handful of patches. Painted TEXT is the bad case: hundreds
  of tiny patches, hundreds of full-mesh Splits, and the split can take minutes. The planned improvement is a
  per-filament `BatchBoolean` union of that filament's shells before a single Split per filament; it is not
  built, and nothing below depends on it.
- **Anisotropic or tilted multi-instance objects** (§3.9): the world path uses the FIRST instance's transform,
  so another instance with a different scale gets an approximate depth and another instance with a different
  ORIENTATION gets an approximate up-axis (its flat caps and §3.6 crease cases are the first instance's).
- **The mesh-space path is only for Z-preserving isotropic transforms** (§3.9, rev 2.14, Ruling 28(1)): a turn
  about z, an x/y mirror and a uniform scale keep it; a rotation about x or y and a z mirror take the world
  path instead, since §3.5/§3.6 are print-frame rules read off the split space's own z.
- **Strokes narrower than 2·ws** (§3.6 width guard, Ruling 22) fall back to the plain mitred bisector instead
  of the case-A inset; that is what keeps embossed text splittable at all, at the cost of 2D parity there.
- §3.10's exclusions stand as written: no interlocking notch, no seam/support/fuzzy-skin paint transfer.

**Not yet measured.** The plan's GUI round — paint, split, inspect the list and filaments, slice, undo/redo,
3MF round trip, painting gizmo open (refused) — has not been walked through by hand; the GUI compiles and
links into `snapmaker-orca.exe` and its logic is covered only by the library tests behind it.
