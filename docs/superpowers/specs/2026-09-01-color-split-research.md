# Colour Split — Research Report

Date: 2026-09-01 · Input: `docs/colorsplitting_topics.md` (brainstorm notes) · Code verified against the
Snapmaker-Ultra fork at `C:\Dev\SnapmakerOrcaNext` when it was on feat/paint-depth @ 35d0345b81; main is now
dff2c65eab (that branch merged) and the worktree is on feat/color-split off it — the cited subsystems are
identical on both. Companion: `docs/colorsplitting_design.md` (the design spec, rev 2 after an adversarial
review) — its §3.4–3.8 supersede the simpler shell description in §4 below.

## 1. What was asked

"Split by painted colour": convert a part's MMU paint into separate solid parts directly in the slicer,
keeping them in place, so that a painted colour behaves like its own object — without a tolerance gap,
with a bounded inward depth and normal projection (the user's mental model from the paint-depth work).

## 2. Verdict on the topics doc

The doc's API archaeology is mostly right for this fork, but its **core geometric method is unsuitable**
and several of its claims are wrong. Summary (details and citations in §5):

| Topic | Doc says | Reality |
|---|---|---|
| Method: cap the painted patch with a sloppy planar cap, boolean-intersect with the original, "leaving exactly the true curved interface" | works generally | **Wrong.** There is no "true interface" inside a solid; the intersection is bounded by the cap itself. Worse, for paint on a FLAT face the patch and its cap are coplanar → a zero-volume piece. The construction that matches the user's model is a solid **shell of normal thickness D** under the patch (design spec §3). |
| "MVP: promote each volume to its own object" | needs writing | **Already exists**: Split → To objects on a multi-volume object makes one object per volume with placement preserved (`ModelObject::split`, Model.cpp:2113-2131, 2188-2204). |
| `get_facets_strict` triangulates T-joints so state boundaries share vertices | claim | **True** (TriangleSelector.cpp:1478-1594); nuance: every call returns the *entire* live vertex pool, only indices differ — compactify per piece, and build ONE selector for all states. |
| Painted state → filament: `int(state) + 1` / "verify off-by-one" | uncertain | **State value == 1-based filament id**; NONE (0) = the volume's own extruder. The slicer uses `int(state)` verbatim (MultiMaterialSegmentation.cpp:3674, PrintObjectSlice.cpp:892-897). |
| `FacetsAnnotation::serialize()` public; `extract_used_facet_states(sel.serialize())` | API | `serialize` on FacetsAnnotation is a private cereal template; use `get_data().used_states` (Model.hpp:737). |
| `config.assign_config_if_not_present`, `vol->extruder_id = n` member | API | Neither exists; extruder is `config.set("extruder", n)` (ModelConfig, PrintConfig.hpp:2007-2075). |
| `MeshBoolean::mfd::make_boolean` primary, mcut fallback, CGAL `does_self_intersect` available | API | **True** (MeshBoolean.hpp:100-109; gizmo GLGizmoMeshBoolean.cpp:392-405). Manifold 3.5.2 is linked, single-threaded (MANIFOLD_PAR=OFF), no face provenance read back by the wrapper. |
| Overlapping split pieces need manual z-order | concern | Slicer clips overlapping parts unconditionally, **later volume wins** (`clip_multipart_objects = true`, PrintObjectSlice.cpp:30, 420-437). |
| N=2 "steal the neighbour's facets" fast path | idea | Not needed once pieces are shells; and it would still give zero-volume pieces on flat faces. |
| Non-manifold input "run repair first" | advice | There is no `TriangleMesh::repair()` member; only import-time admesh repair. A non-manifold source must be refused (Manifold refuses it anyway). |

## 3. Prior art (external tools)

Split3MF, PlainMesh Fill & Split, Obloid Colour 3MF Splitter and ColorSplit3mf all separate painted 3MF
regions by **cutting the surface patch and capping it** (Split3MF lists cap methods "Soap film, CDT boundary,
Winding fill, Projected normal, Centroid cap" and a "Projected plane depth" for interlocking). None documents
a depth-bounded solid construction or a body remainder; their outputs are meant for printing pieces
separately, not for in-place multi-material slicing. Bambu/Orca/Prusa have no paint→parts feature; the
nearest in-tree operations are Split → To parts/objects (connectivity only, paint reset on the part path)
and the Mesh Boolean gizmo. So the in-slicer feature is not a port of anything; it is new.

Sources: https://split3mf.com/ · https://www.plainmesh.com/tools/fill-and-split · https://obloid.app/tools/color-3mf-splitter · https://github.com/mocsy/ColorSplit3mf · https://forum.bambulab.com/t/loosing-colors-when-splitting-to-parts-or-objects/55213

## 4. Design outcome (decided with the user 2026-09-01)

1. **Engine: exact shells + Manifold** (voxel/OpenVDB engine documented as fallback). Each painted patch
   (strict, T-joint-resolved) is extruded inward along the full-surface vertex normals by D and closed;
   piece_c = original ∩ shell_c, body = original − ∪ shells, pieces tiled exactly. Risk to prove by a spike:
   the offset surface self-intersects at concave creases sharper than D — Manifold's handling of that
   (winding-number semantics) is undocumented in its header and must be measured.
2. **Depth mirrors paint-depth**: defaults from the object's print settings through `paint_depth_band_mm`
   (walls/mm → D, classic floor), editable; flat tops/bottoms capped at solid-shell depth (default on);
   unlimited available.
3. **Output: parts in the same object**, body first then colour parts (later wins overlaps at slice time),
   source part and its paint consumed (undoable). Split → To objects then yields separate objects.
4. **Spike after the new worktree exists** (first task of the plan).

Why this realises the paint-depth intent better than the 2D segmentation: the shell has normal thickness D
everywhere by construction (no 24° surface-layer ceiling, no per-layer descent approximation, no mm-mode
top-surface issues), the parts are ordinary volumes (per-part settings, 3MF export as separate bodies),
and the visible colour edge stays exact (it is the patch boundary itself).

## 5. Verified facts (three read-only code audits, condensed)

The three audits below are the evidence for everything above and for the design spec's file/line anchors.

### 5.1 Paint data API


## Claims table (topics doc vs code)
- FacetsAnnotation API get_facets/get_facets_strict/has_facets/empty/set/reset/set_enforcer_block_type_limit/remap_enforcer_block_types/get_data: VERIFIED (Model.hpp:732-792). `serialize()` on FacetsAnnotation is a PRIVATE cereal template (:787-790); public data = get_data() (:737); TriangleSelector::serialize() IS public (TriangleSelector.hpp:357).
- TriangleSelector::get_facets(EBT) / get_facets_strict(EBT) / batch get_facets(std::vector<its>&): VERIFIED (TS.hpp:333,335,340).
- "strict triangulates T-joints so state boundaries share vertices": VERIFIED (TS.cpp:1478-1594).
- get_facets_strict(NONE) returns the unpainted surface: VERIFIED (slicer itself calls it with state 0 at MMS.cpp:1990-1993).
- Vertices in mesh space, index-welded: VERIFIED; NUANCE: get_facets_strict returns the ENTIRE live vertex pool for every state (identical array), only indices differ → its_compactify_vertices per piece (TriangleMesh.hpp:215).
- EnforcerBlockerType: NONE=0, ENFORCER=1, BLOCKER=2, Extruder1=ENFORCER(=1), Extruder2=BLOCKER, ... Extruder16, ExtruderMax=255, int16_t (TS.hpp:13-40). State value == 1-based filament id; NONE == the volume's own extruder.
- "slicer converts with state-1": WRONG — slicer uses int(state) directly (MMS.cpp:3674, PrintApply.cpp:1889, PrintObjectSlice.cpp:892-897 segmentation_channel_filament_id). Only ModelVolume::get_extruders_from_multi_material_painting() is 0-based (Model.cpp:3016-3029).
- extruder_id() returns 0 when unset, never -1 (stale header comment) (Model.cpp:2569-2580).
- Paint stored in MESH space, pre-transform (TS.cpp:1291-1298; MMS.cpp:3600 applies print_object.trafo()*mv->get_matrix(); PainterBase.cpp:729-730). Persistence keyed by original facet index (bbs_3mf.cpp:7098-7104) → any mesh replacement invalidates it; ModelVolume::split resets all five annotations (Model.cpp:2866-2870).
- 3MF: BBS attribute "paint_color" (bbs_3mf.cpp:281; read :3675-3678/:5317-5320; rebuild :4836-4861/:5000-5017; write :7080-7113); Prusa path "slic3rpe:mmu_segmentation" (3mf.cpp:114). Volume extruder rides the generic per-volume <metadata> loop (:7632-7634), read back set_deserialize :4899/:5047, legacy fix-ups :2085, :2161-2181.
- NO existing paint→parts feature (searched GUI + libslic3r). Split to parts = connectivity + paint reset; Split to objects keeps paint only for multi-volume objects (assign :2178-2183). merge_volumes (Model.cpp:2251-2287) carries paint onto a new mesh by per-face strings when the face mapping is known.
- ModelVolumeFacetsInfo::replace_default_extruder (MMS.hpp:31-37) is a DEAD field (never read; both callers pass false, MMS.cpp:3999,:4025).

## get_facets_strict mechanics (TS.cpp)
- Selector ctor copies mesh vertices verbatim (:1291-1293), one Triangle per original face; neighbours from its_face_neighbors (index adjacency, :1278).
- get_facets_strict(state) (:1478-1502): emits ALL vertices with ref_cnt>0 (originals + every midpoint), then for each ORIGINAL triangle get_facets_strict_recursive (:1504-1518): split → recurse with child_neighbors; leaf in `state` → get_facets_split_by_tjoints (:1520-1594): asks each edge neighbour at the same depth for an existing midpoint (triangle_midpoint :667-703); 0/1/2/3 midpoints → 1/2/3/4 triangles, recursing with neighbor_child for deeper T-joints.
- Midpoints allocated ONCE per edge (triangle_midpoint_or_allocate :705-744; perform_split :1341-1397 reuses neighbours' midpoints) → the union of indices over all states is a conforming, T-joint-free re-triangulation of the original surface; a state-A boundary edge references exactly the vertex indices its state-B neighbour uses.
- Each FacetsAnnotation::get_facets_strict(mv, s) call rebuilds a selector (Model.cpp:3568-3573: TriangleSelector selector(mv.mesh()); selector.deserialize(m_data,false); ...) deterministically, but SAFER: build ONE `TriangleSelector sel(mv.mesh()); sel.deserialize(mv.mmu_segmentation_facets.get_data(), false);` and call sel.get_facets_strict(s) for every state so vertex arrays are identical by construction.
- Sharing is by index adjacency: an unwelded source mesh (duplicate vertices along an edge, neighbour -1) gets separate midpoints per side → T-joints NOT resolved across such seams → pre-weld (its_merge_vertices, TriangleMesh.hpp:209) before building the selector? NO — the annotation is keyed by ORIGINAL facet index, and welding changes only vertices not facet order (its_merge_vertices keeps face order; verify) — treat as an implementation check.
- deserialize clamps state > max_ebt → NONE (:1814-1817); FacetsAnnotation wrappers pass ExtruderMax → nothing dropped.
- FacetsAnnotation::reset (Model.cpp:3618-3623) clears triangles_to_split + bitstream but NOT used_states → stale states still reach PrintApply.cpp:1879-1885 (ORs used_states over all volumes). Relevant if the source volume is kept and only its paint cleared.

## Gizmo mapping (GLGizmoMmuSegmentation)
- get_left_button_state_type() = EnforcerBlockerType(m_display_filament_ids[m_selected_extruder_idx]) (Gizmo.hpp:89-94); m_display_filament_ids = actual filament numbers in sidebar order (Gizmo.cpp:126-148). m_extruder_remap is slot→slot; remap_filament_assignments (:1361-1400) maps states via state_map[src_state]=dst_state and ALSO state_map[0]=dst when src_state==1 (:1385-1386).
- Per-volume selector: ebt_colors[0] = the volume's own extruder colour (:1025-1028); deserialize(mv->mmu_segmentation_facets.get_data(), false, max_ebt) :1035-1036; write-back mmu_segmentation_facets.set(*selector) :971.
- ModelVolume::remap_extruder_ids(extruder_count, state_map) Model.cpp:2667-2679; update_extruder_count :2624-2645; update_extruder_count_when_delete_filament :2647-2665.

## Slicer consumption
- multi_material_segmentation_by_painting MMS.cpp:3817-4017: num_facets_states = num_total_filaments+1 (:3818-3820); extract_facets_info lambda (:3998-4000) → segmentation_by_painting. Projection loop :3591-3683: for extruder_idx in [1,num_facets_states): get_facets(*mv, EBT(extruder_idx)) (:3596), transform print_object.trafo()*mv->get_matrix() (:3600), PaintedLineVisitor colour = int(extruder_idx) (:3674). Top/bottom shells use get_facets_strict for states [0,num_facets_states) (:1990-1993).
- PrintApply.cpp painted regions :1082-1116: cfg.wall_filament = solid_infill_filament = painted id; sparse only if paint_sparse_infill (= paint_infill_override || pdmUnlimited, :1862-1863). PaintedRegion{extruder_id 1-based, parent, region} (Print.hpp:296-304). painting_extruders from used_states (:1889).
- Base region: region_config_from_model_volume (PrintObject.cpp:3275+): object config then volume config (:3285), clamp_exturder_to_default (:3293-3299).

## Test harness (tests/libslic3r/test_paint_depth_clamp.cpp, 5305 lines; CMakeLists.txt:9)
- Cube facet table :39-52 (PLUS_X_FACE {4,5}, ALL_SIDE_FACE {4..11}, TOP_CAP_FACE {2,3}, BOTTOM_CAP_FACE {0,1}).
- paint_depth_test_config(mode, walls, paint_infill_override=true, paint_depth_solid_interfaces=true) :57-93 (2-filament full_print_config with width pins; outer_wall_line_width=0 trap documented :66-84).
- slice_painted_cube(painted_facets, mode, walls, Print&, ...) :97-119 — canonical recipe:
  ModelVolume *volume = object->add_volume(make_cube(40.,40.,20.)); TriangleSelector selector(volume->mesh()); for (f : painted) selector.set_facet(f, EnforcerBlockerType::Extruder2); REQUIRE(volume->mmu_segmentation_facets.set(selector)); print.set_status_silent(); print.apply(model, cfg); out_object->slice();
- slice_painted_box(...) :141-181; extruder2_region_config :189-199; extruder2_claim_for_layer :204-222; any_contains :224-230; process_z_interface_cube :253-285; process_z_sandwich_cube :315-358; slice_capped_slab/ledge/prism :1023-1224; make_square_frustum :1242-1259.
- Other painters: test_mixed_filament.cpp :623-625, :4531-4535, :4581-4602, :4777-4798 (set_facet+set+has_facets+remap_extruder_ids); test_triangle_selector.cpp :8-39. No test uses get_facets_strict.

## Building blocks (TriangleMesh.hpp)
its_compactify_vertices :215, its_merge_vertices :209, its_merge :324, its_face_neighbors :200, its_number_of_patches :225, its_volume :321, its_flip_triangles :204, its_split :221-222.

### 5.2 Model / plater / slicing plumbing


Nothing named "split by colour/paint" exists in the fork.

## add_volume (Model.hpp:414-418, Model.cpp:1339-1405)
- (const TriangleMesh&, bool centre=true); (TriangleMesh&&, ModelVolumeType=MODEL_PART, bool centre=true): new-mesh ctor; if centre: center_geometry_after_creation() + invalidate_bounding_box(); then save_object_mesh (BBS backup).
- (const ModelVolume& other, type): copies IDs, shares mesh, no centring. (const ModelVolume&, TriangleMesh&&) :1382: new-mesh ctor + UNCONDITIONAL centring.
- center_geometry_after_creation (Model.cpp:2681-2697): shift = mesh bbox centre; mesh translated by -shift; m_mesh->set_init_shift(shift); volume offset += shift (world placement unchanged; composes with existing offset, so ModelVolume::split does set_offset(Zero) first then re-adds the old offset).
- New-mesh ctor (Model.hpp:1134-1166) copies name/source/config/type/transformation, config.set_new_unique_id(), recomputes hull, and ASSERTS all four facet annotations EMPTY (supported/seam/mmu_segmentation/fuzzy_skin). Copy ctor keeps IDs and copies facets.

## ModelVolume::split (Model.cpp:2834-2899) / ModelObject::split (Model.cpp:2094-2208)
- ModelVolume::split: meshes = mesh().split() (its_split connectivity); first patch written back into this (new id, ALL PAINT RESET :2866-2870), others inserted right after via (other, mesh&&) ctor; each: set_offset(Zero); center_geometry_after_creation(); translate(old offset); name_"idx"; config.set("extruder", this->extruder_id()) (round-robin commented out; max_extruders unused); m_is_splittable=0; degenerate-hull volumes deleted.
- ModelObject::split: only MODEL_PART volumes; modifiers/negatives DROPPED (:2104). Single-volume → connectivity split; MULTI-VOLUME → each volume becomes ONE object, no further splitting (:2113-2131). Per piece: model->add_object(); config = object config + volume config applied (volume extruder becomes object extruder, :2172-2173); instances copied; add_volume(*volume, std::move(mesh)) (centres; paint asserted empty) then paint restored for the multi-volume case via mmu_segmentation_facets.assign (:2178-2183); new_vol->config.reset() (:2186). Placement: per instance offset += instance.get_matrix_no_offset() * new_vol->get_offset(); new_vol->set_offset(Zero) (:2188-2204). No Model::split; GUI clones the Model first.

## GUI wiring
- Plater::priv::split_object() (Plater.cpp:13250-13291): Model new_model = model; split on the clone; TakeSnapshot("Split to Objects"); remove(obj_idx); load_model_objects(new_objects,false,true) (:12627-12776: new_clone; extruder forced 0 if absent; sort_volumes(true); ensure_on_bed gated by auto_drop_on_import :12694; obj_list add; update(); update_info_items; object_list_changed(); schedule_background_process()).
- ObjectList::split() (GUI_ObjectList.cpp:2762-2818): get_volume_by_item (:3277-3297); is_splittable check; take_snapshot("Split to parts"); volume->split(filament_cnt); DeleteVolumeChildren; AddVolumeChild(parent,name,type,is_text,is_svg,warning_icon, has("extruder")?extruder():0,false) + add_settings_item; changed_object(obj_idx); notify_instance_updated; update_info_items.
- Plater::changed_object(ModelObject&) :23451: invalidate_bounding_box; ensure_on_bed(!SLA); view3D->reload_scene(false); schedule_background_process; requires_check_outside_state.
- Enablement: priv::can_split(bool to_objects) :16613 → obj_list()->is_splittable(to_objects) :3299-3325. Toolbar GLCanvas3D.cpp:6856,6866; Plater.cpp:10889-10890 → on_action_split_* :15897-15905.
- Menus live in slic3r/GUI/GUI_Factories.cpp (NO MenuFactory.cpp, NO append_menu_item_split): create_bbl_part_menu :1595-1607, create_object_menu :1424-1436 and :1463-1474, multi_selection_menu :1987-1997. Pattern:
  append_menu_item(split_menu, wxID_ANY, _L("To objects"), ..., [](wxCommandEvent&){ plater()->split_object(); }, "menu_split_objects", menu, [](){ return plater()->can_split(true); }, m_parent);
  append_submenu(menu, split_menu, wxID_ANY, _L("Split"), ..., "", [](){ return plater()->can_split(true); }, m_parent);
  append_menu_item (wxExtensions.cpp:67-96) binds wxEVT_MENU + wxEVT_UPDATE_UI condition. "Change type" = append_menu_item_change_type :806-814 → ObjectList::change_part_type :5350-5396. No "convert part to object"; nearest: append_menu_item_merge_to_multipart_object :1154, ObjectList::merge :2820.
- List refresh: add_object_to_list :3970-4023; add_volumes_to_object_in_list :4044-4102 (rows only when volumes.size()>1 :4026); reorder_volumes_and_get_selection :6264-6278; update_info_items :3812; changed_object :3505.

## Per-volume extruder
- ModelConfigObject : ObjectBase, ModelConfig (Model.hpp:72). ModelConfig API (PrintConfig.hpp:2007-2075): reset, assign_config, apply(cfg, ignore_nonexistent), apply_only, set_key_value, set<T>, set_deserialize, erase, get, has, option, opt_int, extruder(){opt_int("extruder")} (throws if absent → idiom has("extruder")?extruder():0). NO assign_config_if_not_present.
- ModelVolume::extruder_id() (Model.cpp:2569-2580): volume extruder if present & non-zero, else object's, else 0. get_extruders() :2593-2622 enumerates painted states via batch get_facets (indices 1..N == filament numbers) cached by facet timestamp, plus volume extruder (0→1).
- Slice mapping PrintObject.cpp:3247-3272 apply_to_print_region_config: non-zero extruder → sparse_infill_filament, solid_infill_filament, wall_filament; outer_wall_filament reset to 0 (Ultra). region_config_from_model_volume :3275-3310: default → object config (parts only) → volume → material → layer range; clamps to num_extruders.
- Object list extruder column: AddVolumeChild (:2794,:4083); edit ItemValueChanged :5959 → update_filament_in_config :1075-1118 (take_snapshot("Change Filament"); set_key_value("extruder", new ConfigOptionInt); object item erases extruder from every volume; plater()->update(); notify_filament_usage_changed()). Context menu append_menu_item_change_extruder GUI_Factories.cpp:940-996 → set_extruder_for_selected_items :6096-6175. load_generic_subobject :2382-2385 sets a new part's extruder to the object's value explicitly.

## Overlapping parts at slice time
- `bool PrintObject::clip_multipart_objects = true;` PrintObjectSlice.cpp:30 (static, Print.hpp:657) — constant, no option.
- slices_to_regions :262-478: per Z, XY-bbox-overlapping volumes = "complex"; temp_slices follow layer_range.volume_regions order (= ModelObject::volumes order, PrintApply.cpp:1038-1050); for each MODEL_PART/negative region: "Clip every non-zero region preceding it" — diff_ex of every earlier non-negative overlapping region (:420-437). ⇒ THE LATER VOLUME WINS THE OVERLAP, unconditionally. Modifiers intersect/diff parent (:404-419). Same-region slices appended + closing_ex(EPSILON) (:454-464). slice_volumes_inner :141-234 with extra_offset=0 (:180).
- Perimeters: Layer::make_perimeters (Layer.cpp:220-296) groups regions by is_perimeter_compatible (:179-214): compares wall_filament, outer_wall_filament, wall_loops, wall_sequence, is_infill_first, speeds, gap-fill, overhang/thin-wall/wall-direction, line widths, infill-wall overlaps, seam-slope/scarf keys ⇒ DIFFERENT wall_filament ⇒ own perimeters; boundary = wall/wall interface, exact diff_ex, no gap/overlap. interface_shells (PrintObject.cpp:1337-1338; forced by has_bounded_paint_depth && paint_depth_solid_interfaces) decides top/bottom detection at region boundaries.
- Painted regions (Print.hpp:282-336 VolumeRegion/PaintedRegion/LayerRangeRegions): generate_print_object_regions PrintApply.cpp:976-1123 — volume regions first, then per painting_extruder × part/modifier region whose root part is_mm_painted() (:732) a PrintRegion cloned from parent with wall/solid filament = painted id, sparse only if paint_sparse_infill (:1088-1116); interned :1025-1036; sorted :1119-1122. painting_extruders = OR of volumes' used_states (:1871-1898). PrintObject::slice() :810-828 → slice_volumes: volume clipping first (:5226-5234), then multi_material_segmentation_by_painting (MultiMaterialSegmentation.cpp:3817) and apply_mm_segmentation :4121 (steals intersection_ex(parent.slices, segmented) :4485-4535; parent trimmed diff + opening(5·EPSILON) :4548-4590). Painting is applied AFTER part clipping, only inside the parent's clipped area.

## Multi-part/multi-extruder settings (per-object only, none per-volume)
interface_shells PrintConfig.cpp:3861 / PrintObject.cpp:1337-1529,1772 / PerimeterGenerator.cpp:622,2273. flush_into_objects/infill/support PrintConfig.cpp:6714-6734; ToolOrdering.cpp:1478-1500 is_overriddable (soluble never; flush_into_objects → everything; else only erInternalInfill when flush_into_infill), :1549-1556, :1583-1640. enable_prime_tower :6533, wipe_tower_no_sparse_layers :5722.

## Export
- bbs_3mf.cpp: each ModelVolume = its own <object type="model"> with own <mesh> (_add_mesh_to_object_stream :6910-7135; vertices untransformed :7021-7024; per-triangle paint_color :281, :7098-7104). Parent = <components> referencing volume ids with transform = volume->get_matrix() (:6843-6890, :6754-6757); build items reference parent with instance matrix (:6742). Per-volume config → Metadata/model_settings.config <part> <metadata key="extruder"> (:7576-7653). Import: set_deserialize :4900; extruder normalisation :2159-2184.
- STL export Plater.cpp:20585-20800 (combine_mesh_fff :20518) = ONE merged body per object/instance. OBJ.cpp:237-246 = raw_mesh merge (Model.cpp:1621-1647). External tools get separate bodies ONLY via 3MF.

## Undo/redo + invariants
- Plater::take_snapshot :21951 → priv::take_snapshot :17056-17110; TakeSnapshot RAII Plater.hpp:893-910. UndoRedo.cpp:917-961 serialises the Model; ObjectBase pointers stored by object.id() with timestamp dedup. New volumes with fresh ObjectIDs need no registration; IDs must be unique/distinct from config/facet IDs. load_snapshot :1000-1041; update_after_undo_redo :17357.
- Invariants: invalidate_bounding_box (Model.hpp:457-463); ensure_on_bed (Model.cpp:1825-1846) translates instances only; center_around_origin :1812-1823; sort_volumes(true) :1464-1478 stable-sorts by type (parts, negatives, modifiers, blockers, enforcers) = region clipping order. Print::apply detects volume add/remove/reorder/transform via model_volume_list_changed (Model.cpp:3710-3756; PrintApply.cpp:1605-1612) and regenerates regions (:657-686, :1993-2005).

### 5.3 Boolean / voxel / job infrastructure


## MeshBoolean (src/libslic3r/MeshBoolean.hpp/.cpp) — all three backends compiled unconditionally
- Top-level `MeshBoolean::minus/self_union` (igl+CGAL): NO callers anywhere.
- `cgal::` (hpp:25-73): triangle_mesh_to_cgal, cgal_to_triangle_mesh, minus/plus/intersect (TriangleMesh / its / CGALMesh), does_self_intersect, does_bound_a_volume, empty, segment, merge. Booleans use throw_on_self_intersection(true) (cpp:259-272). `_cgal_do` (275-294) clobbers CGALMesh A before checking (284); hw fault → HardCrash; failure → RuntimeError("CGAL mesh boolean operation failed."); TriangleMesh/its overloads assign A only after success (406-428). CGAL 5.4 (libslic3r_cgal static lib, cpp in that lib, 506-512).
- `mcut::` (hpp:75-98): do_boolean_single (returns false, src untouched EXCEPT "UNION" → concatenation fallback returns true, 671-674/684-687); do_boolean splits both inputs with its_split and runs pairwise (763-806); make_boolean(src, cut, dst, opts) pushes only non-empty; MC_DISPATCH_ENFORCE_GENERAL_POSITION (perturbs); output re-triangulated. Vendored deps_src/mcut 1.2.
- `mfd::` (Ultra, hpp:100-109): `bool make_boolean(const TriangleMesh &src, const TriangleMesh &cut, std::vector<TriangleMesh> &dst, const std::string &opts /* "UNION" | "A_NOT_B" | "INTERSECTION" */)` — returns false and leaves dst untouched when an input is not a valid manifold (a.Status()!=NoError → log + false, 861-865) or op unknown; exceptions → warning + false (881-884); empty result → true with dst untouched (867-868). Uses MeshGL numProp=3 float + m.Merge() (824-844); does NOT read faceID/runOriginalID (866-879). Manifold v3.5.2, deps/Manifold/Manifold.cmake: MANIFOLD_PAR=OFF (single-threaded), static; linked PRIVATE into libslic3r (src/libslic3r/CMakeLists.txt:599); find_package(manifold CONFIG REQUIRED) CMakeLists.txt:729-730. Header C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local/include/manifold/manifold.h has: BatchBoolean, MinkowskiSum, MinkowskiDifference, TrimByPlane, SplitByPlane, Warp/WarpBatch, Refine*, SmoothOut, Hull, Simplify, SetTolerance.
- Usage today: GLGizmoMeshBoolean.cpp (370-426): mesh baked with volume matrix (m_src.trafo = volume get_matrix(), instance matrix NOT included), mfd::make_boolean → mcut fallback, no CGAL, synchronous on UI thread, no try/catch; generate_new_volume (457-523): take_snapshot("Mesh Boolean"); add_volume(std::move(mesh)) (centring on); copies name/config/type/material; set_offset(old offset) (486) — drops rotation/scale of the source volume; paint transfer commented out (487-491); source deleted by swap+delete_volume; plater()->update(); reorder_volumes_and_get_selection. ModelObject::make_boolean (Model.cpp:1318-1337) mcut only. SLAPrintSteps.cpp:379-450 drain holes (CGAL plus/minus, does_self_intersect, does_bound_a_volume). CSGMesh/PerformCSGMeshBooleans.hpp check_csgmesh_booleans (MeshEmpty/NotBoundAVolume/SelfIntersect) used by Plater::combine_mesh_fff (20518-20560) and export lambda (20620-20642).

## OpenVDB (8.2.0, tamasmeszaros fork a68fd58, static, Blosc on) — linked PRIVATE into libslic3r, effectively unconditional (CMakeLists.txt:738-746 FATAL_ERROR if missing; OpenVDBUtils.cpp compiled if TARGET OpenVDB::openvdb, src/libslic3r/CMakeLists.txt:14-17, 626-628). GUI cannot include openvdb headers → expose via libslic3r wrappers (pattern: MeshRepair.hpp "stays free of OpenVDB includes").
- OpenVDBUtils.hpp: `mesh_to_grid(const its&, const openvdb::math::Transform& = {}, float voxel_scale = 1.f, float exteriorBandWidth = 3.f, float interiorBandWidth = 3.f, int flags = 0)` (its_split; drops parts with its_volume < EPSILON; meshToVolume per part + csgUnion; levelSetRebuild; stores "voxel_scale" meta; band widths in VOXEL units; interior can be numeric_limits<float>::max() to fill). `grid_to_mesh(grid, isovalue=0, adaptivity=0, relaxDisorientedTriangles=true)` (volumeToMesh; divides by voxel_scale; quads → 2 tris; OUTPUT WOUND INWARD — flip for exterior, cpp:100-102). `redistance_grid(grid, iso, ext_range=3, int_range=3)` (levelSetRebuild). Ultra `remesh_by_voxels(its, voxel_size)` (MeshRepair.hpp:9-14; voxel_scale=1/voxel_size, bands 3/3, flips, {} on exception). No dilate/erode/unsigned/label wrappers.
- SLA/Hollowing.cpp generate_interior (60-148), HollowingConfig{min_thickness=2, quality=0.5, closing_distance=0.5}: voxel_scale = 3.5 + 4.5*quality (voxels per mm); offset = scale*min_thickness; D = scale*closing; out_range = 0.1*offset; in_range = 1.1*(offset+D); grid = mesh_to_grid(its, {}, scale, out_range, in_range); redistance_grid(grid, -(offset+D), in_range, in_range); mesh = grid_to_mesh(grid, iso=D, adaptivity 0) → erosion by offset+D then re-expansion by D (opening); post: swap_normals, its_quadric_edge_collapse lossless (2*FLT_EPSILON), its_compactify_vertices, its_merge_vertices, swap_normals; progress via sla::JobController at 0/30/70/100. hollow_mesh does NO boolean (merges cavity mesh; optional remove_inside_triangles using grid distance). Interior::get_distance (358-362) mm distance to cavity via ConstAccessor (not thread safe).
- Deps headers present (openvdb/tools/): MeshToVolume.h — `meshToVolume(mesh, xform, extBW, intBW, flags, Int32 polygonIndexGrid* = nullptr)` (110-118) "optional grid output that will contain the closest-polygon index for each voxel in the narrow band" (108-109); flags UNSIGNED_DISTANCE_FIELD (open meshes OK), DISABLE_INTERSECTING_VOXEL_REMOVAL, DISABLE_RENORMALIZATION, DISABLE_NARROW_BAND_TRIMMING; meshToUnsignedDistanceField (376-383); meshToLevelSet (231-237, closed but not necessarily manifold); Interrupter overload (136-145). Composite.h csgUnion/Intersection/Difference(+Copy), compMax/Min/Sum/Mul/Div/Replace. VolumeToMesh.h volumeToMesh + class VolumeToMesh (setRefGrid, setSurfaceMask(mask, invert), setAdaptivityMask). LevelSetFilter.h offset(world distance), mean/gaussian/median/laplacian/meanCurvature. LevelSetUtil.h sdfInteriorMask, extractEnclosedRegion, extractIsosurfaceMask, segmentActiveVoxels/segmentSDF. FastSweeping.h fogToSdf/sdfToSdf/sdfToExt/dilateSdf/maskSdf. TopologyToLevelSet.h. Morphology.h dilate/erodeActiveValues. Interpolation.h BoxSampler/GridSampler. ValueTransformer.h foreach/transformValues. GridOperators.h gradient/cpt. Grid typedefs Bool/Float/Int32/Mask/Vec3S.

## Closest point / distance
- AABBTreeIndirect.hpp: build_aabb_tree_over_indexed_triangle_set(vertices, faces, eps=0) (671-717); squared_distance_to_indexed_triangle_set(vertices, faces, tree, point, hit_idx_out, hit_point_out) (795-815, -1 if empty); is_any_triangle_in_radius (821-849); all_triangles_in_radius (852-876); intersect_ray_first_hit/all_hits; traverse. Queries const/thread-safe (per-call local distancer).
- AABBMesh.hpp: AABBMesh(const its&|TriangleMesh&, calc_eps=false) (stores RAW POINTER to caller's its); query_ray_hit(s); `double squared_distance(const Vec3d& p, int& i, Vec3d& c) const` (123); normal_by_face_id; vertex_face_index; face_neighbor_index. SLA/IndexedMesh.hpp near-duplicate. libigl present unused: AABB.h, point_mesh_squared_distance.h, signed_distance.h, fast_winding_number.h, per_vertex_normals.h, boundary_loop.h, remove_unreferenced.h, is_edge_manifold.h, copyleft/offset_surface.h (needs signed_distance).

## Mesh utilities (TriangleMesh.hpp)
- EXISTS: its_face_neighbors(_par) (200-201, -1 = open edge), its_face_edge_ids (193-197), its_num_open_edges (232-233), VertexFaceIndex (166-188), its_number_of_patches (225), its_is_splittable (228), its_split (221-222), its_merge_vertices (206-209; exact-coordinate weld; face ORDER PRESERVED — verified impl TriangleMesh.cpp:674+), its_remove_degenerate_faces (212), its_compactify_vertices (215), its_shrink_to_fit (236), its_flip_triangles (204), its_reverse_all_facets (cpp:1426), its_quadric_edge_collapse (QuadricEdgeCollapse.hpp:21), its_mask(its, std::vector<bool>) (CutSurface.hpp:69), its_volume (321, signed), its_average_edge_length, its_merge (324-326), its_face_normals (328), its_face_normal (331-334), its_unnormalized_normal, its_convex_hull; NormalUtils::create_normals(its, VertexNormalType=NelsonMaxWeighted) (NormalUtils.hpp:17,49-54); its_translate/its_transform (admesh stl.h 344-373, fix_left_handed), its_make_cube/prism/cylinder/cone/frustum/torus/pyramid/sphere (336-346); cut_mesh(its, z, upper, lower, triangulate_caps) (TriangleMeshSlicer.hpp:127-132). TriangleMesh: stats().manifold() = open_edges==0; transform(Transform3d, fix_left_handed) flips if det<0; split() flips negative-volume parts; merge; convex_hull_3d; set_init_shift.
- DOES NOT EXIST: its_is_manifold/its_is_watertight, TriangleMesh::repair() member (only file-static trianglemesh_repair_on_import), its_boundary_edges (derive from face_neighbors == -1), its_extrude, its_vertex_normals, TriangleMeshSlicer class.
- CGAL 5.4 PMP headers present: corefinement, clip, extrude, border, triangulate_hole, self_intersections, distance, compute_normal, manifoldness, connected_components, orientation, repair, stitch_borders, remesh, smooth_shape, fair, refine, locate, intersection, merge_border_vertices, repair_self_intersections, repair_degeneracies, measure; AABB_tree, Side_of_triangle_mesh, Polygon_mesh_slicer. Missing: alpha_wrap_3.

## Jobs / progress
- Job (src/slic3r/GUI/Jobs/Job.hpp:17-64): Ctl{update_status(int,msg), was_canceled(), clear_percent(), show_error_info(...), call_on_main_thread(fn)→future}; process(Ctl&) on worker thread; finalize(bool canceled, std::exception_ptr&) on UI thread. Worker.hpp: push/is_idle/cancel/cancel_all/process_events/wait_*; queue_job, replace_job(Worker&, Args...) (= cancel_all + queue), stop_current_job. BoostThreadWorker (dedicated thread, catches exceptions into eptr; Status → ProgressIndicator; Finalize rethrows unhandled). PlaterWorker<W> adds wxWakeUpIdle, busy cursor RAII (BusyCursorJob.hpp), converts leftover exception into show_error("An unexpected error occurred: ..."). Plater owns PlaterWorker<BoostThreadWorker> m_worker (Plater.cpp:10127, NotificationProgressIndicator 10649); Worker& Plater::get_ui_job_worker() (19946; example Plater.hpp:401-431). Users: FillBedJob, ArrangeJob, OrientJob, SLAImportJob; Emboss CreateVolumeJob/CreateObjectJob (process computes mesh, finalize take_snapshot + add_volume + set_transformation, 341-372; JobException surfaces via show_error). No mesh-processing/boolean Job exists.
- Synchronous pattern: wxBusyCursor + TakeSnapshot (Plater::priv::split_object 13250-13291; ObjectList::split 2762-2816). ProgressDialog (Widgets/ProgressDialog.hpp) for modal work (Plater.cpp:11533-11534, 17810-17817). libslic3r parallelism: execution::for_each(ex_tbb, ...) (Execution/ExecutionTBB.hpp:19); TBB linked.
