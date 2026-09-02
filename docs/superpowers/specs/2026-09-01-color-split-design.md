# Colour Split — Design Spec

Date: 2026-09-01 · Rev 2 (after adversarial review) · Status: awaiting user review, spike pending
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

**3.4 Per-vertex depth d(v).** d(v) = min(D, t(v)/2), where t(v) is the thickness of M along −n(v) from v
(`AABBMesh::query_ray_hit` from v − ε·n(v); no hit → t = ∞). The half-thickness clamp reproduces the 2D
Voronoi split: a wall thinner than 2D is shared at mid-thickness with whatever faces it (another colour or the
unpainted far side), and Unlimited = "everything up to mid-thickness", exactly what the 2D unlimited mode gives.
Fold guard: for every group triangle compare the reversed bottom triangle's normal with the top's; where the
dot product ≤ 0 or the bottom area < 10⁻³ of the top's, halve d at that triangle's vertices and repeat (≤ 8
rounds, floor d = h). After construction each shell is checked with `MeshBoolean::cgal::does_self_intersect`;
a still self-intersecting component halves its d uniformly and rebuilds; if it fails at d = h the split is
refused with an error naming the filament (§7). Small convex features (fillets, bosses, spheres with r < D) thus
get a thin skin instead of a folded shell; their interiors come back through §3.8.

**3.5 Depth groups (flat cap).** Transliteration of the live 2D rule (`flat_cap_component_ex`,
MultiMaterialSegmentation.cpp:1493-1578; tests test_paint_depth_clamp.cpp:5107-5200): a facet is flat iff
tan θ < h / (3·ws) (≈2.2° at h = 0.1 mm; a 3° face is NOT flat); within each S_s, edge-connected flat components
whose XY projection survives an inward offset of 1.5·ws form **capped groups**; everything else forms uncapped
groups at d(v). Cap depth: N_eff = `effective_shell_layers_by_thickness` semantics — `top_shell_layers` (or the
number of layers spanning `top_shell_thickness` when that is deeper), 0 when the layer count is 0 —
d_cap = max(h, N_eff·h), i.e. a zero-shell object keeps only its surface layer; bottom-facing groups
(n_z < 0) use the bottom_* keys. The cap applies only when D ≥ ws and d_cap < D (the 2D gates,
MultiMaterialSegmentation.cpp:1874-1877, 2228); capped groups use min(d(v), d_cap). Flat cap off ⇒ one group per patch.

**3.6 Side walls and the wall-stack step.** For a boundary edge (a,b) of a group (neighbour facet outside the
group): if the painted facet P is more horizontal than its outside neighbour Q (|n_P·z| > |n_Q·z|) — a painted top
meeting a side face, a painted floor meeting a wall — the wall first steps one wall stack into the painted face
while dropping one layer: a₁ = a + ws·t_in(a) − h·n_P, then continues a′ = a₁ − (d(a) − h)·n_P, where t_in is the
unit inward tangent of P at the boundary. This is the 2D F1 contour inset: the surface layer claims to the edge,
every layer below stays one wall stack away from the side surface, so the side face keeps a body-coloured
outer wall (without it a zero-width body wedge grows at 45° from the edge and the painted colour shows on the
side face for the top ≈ws). Otherwise (paint boundary inside a smooth face, or a painted side face meeting a
more horizontal neighbour) the wall is the plain bisector projection a′ = a − d(a)·n(a). Known approximation:
for side surfaces that lean inward going down, the inset shrinks below ws at depth (2D re-insets per layer).

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
isotropic (scale s, any rotation, mirror allowed) the split runs in mesh space with D/s, ws/s, h/s — exact for
every instance sharing that scale. Otherwise the mesh copy is transformed by T of the first instance
(`TriangleMesh::transform(T, fix_left_handed=true)`), split, and the pieces transformed back by T⁻¹ (same
flag); other instances with different anisotropic scale get an approximate depth (documented limit).

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
| Source mesh has open edges after welding | Refused before the job: "The part is not watertight; repair it first." |
| Part not painted / paint resolves to nothing | Menu item disabled / job ends with a notification, no change |
| A painting gizmo is open or a job is running | Menu action refused with a notification |
| Shell self-intersects even at d = h | Error naming the filament, model untouched |
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
