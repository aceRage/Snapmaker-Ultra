# Per-Part Support Settings — Research Report

Date: 2026-09-02 · Tree: `C:\Dev\SnapmakerOrca` on `feat/ultra-preferences` @ 06e7d39b7d · Read-only study,
no code changed. All line numbers below were read in this tree on this date; they will drift.

## Question

> Is it viable to allow support settings to be adjusted at a per-PART level instead of only per-OBJECT?
> Sometimes many parts are grouped into one object as an assembly, but some parts of the assembly need
> tighter supports or different support interfaces (interface layer counts, top z distance, support style,
> interface pattern, tree branch settings) than the rest.

## Verdict in one paragraph

**Partial yes, and the answer differs sharply by key.** There is no fundamental blocker in the data model —
`ModelVolume::config` already exists, is already serialised into the 3MF with *no* allow-list, and the
overhang detector already runs a per-`LayerRegion` loop. The blocker is that every support parameter lives
in `PrintObjectConfig` and is baked once per `PrintObject` into a single `SupportParameters` struct
(`SupportParameters.hpp:9-190`) that ~123 call sites read as a scalar, and support geometry
(`SupportGeneratorLayer::polygons`) carries no provenance tag at all. Cheapest genuinely-useful slice is
**per-part support-settings *groups*** — resolve each part's support override, group parts by resolved
config, and run the *existing, unmodified* generator once per group over a per-group overhang mask, merging
the results (this is exactly Cura's architecture, and it is what `TreeSupport3D.cpp:125` `group_meshes`
already gestures at). A full per-region rewrite of `SupportParameters` is ~55-80 engineer-days and high
risk; the group approach is ~32-50 days; an interface-only first slice is ~19-25 days for normal supports.
And the zero-day answer that already works today is **Split → To objects**, which is functionally
per-part supports minus the grouping ergonomics — at the cost of losing cross-part slice clipping.

---

## 1. How it works today

### 1.1 The two config scopes, and why supports are in the wrong one

Every FFF setting belongs to exactly one static config class (`PrintConfig.hpp:10-13` names the hierarchy):

| Class | Scope | Where it comes from |
|---|---|---|
| `PrintObjectConfig` (`PrintConfig.hpp:898-1073`) | one per `PrintObject` | print preset ← `ModelObject::config` |
| `PrintRegionConfig` (`PrintConfig.hpp:1077-1225`) | one per `PrintRegion` (a *set* of volumes) | print preset ← `ModelObject::config` ← `ModelVolume::config` ← material ← layer range |

**Every support key is in `PrintObjectConfig`.** Grepping `PrintRegionConfig`'s member list for `support`
returns nothing; the only overhang-adjacent region keys are wall/speed/bridge ones
(`detect_overhang_wall`, `overhang_*_speed`, `bridge_flow`, `make_overhang_printable`,
`extra_perimeters_on_overhangs`). The support block in `PrintObjectConfig` spans
`PrintConfig.hpp:952-1023` and includes exactly the keys the question asks about:

```
enable_support (953), support_type (955), support_angle (957), support_on_build_plate_only (958),
support_top_z_distance (961), support_bottom_z_distance (962), support_filament (964),
support_line_width (965), support_interface_filament (968), support_interface_top_layers (973),
support_interface_bottom_layers (974), support_interface_spacing (976), support_base_pattern (978),
support_interface_pattern (979), support_base_pattern_spacing (981), support_expansion (982),
support_style (984), support_threshold_angle (995), support_object_xy_distance (997),
tree_support_branch_diameter (1012), tree_support_branch_angle (1013), tree_support_wall_count (1016),
support_bottom_interface_spacing (1022)
```

### 1.2 How object config and volume config reach the back end

`PrintObject::object_config_from_model_object` (`PrintObject.cpp:3230-3242`) is the whole story for objects:
start from the print preset's `PrintObjectConfig`, `apply` the `ModelObject`'s own `DynamicPrintConfig` on
top, clamp extruder ids. It never looks at `model_object.volumes`.

`region_config_from_model_volume` (`PrintObject.cpp:3274-3305`) is the whole story for parts. It funnels
everything through `apply_to_print_region_config` (`PrintObject.cpp:3245-3271`), whose key line is:

```cpp
if (ConfigOption* my_opt = out.option(it->first, false); my_opt != nullptr)   // PrintObject.cpp:3266
```

`out` is a `PrintRegionConfig`. **A support key on a `ModelVolume` is silently dropped right here** — no
warning, no error, no effect. This is the single load-bearing line for the whole question.

### 1.3 Regions are deduplicated, so `LayerRegion` ≠ part

`PrintApply.cpp:1025-1035` (`get_create_region`) hashes each resolved `PrintRegionConfig` and returns an
existing `PrintRegion*` when the config matches. Two parts of an assembly with identical settings therefore
share one `PrintRegion`, and `slices_to_regions` (`PrintObjectSlice.cpp:441-467`) then *merges and closes*
their slices into one `LayerRegion`. Volume identity survives only in
`PrintObjectRegions::LayerRangeRegions::volume_regions` (`Print.hpp:325-340`), whose entries keep a
`const ModelVolume *model_volume` (`Print.hpp:286`) alongside the `PrintRegion*`. That mapping is
volume → region, **not** region → volume, and it is many-to-one.

Consequence for any design that wants "which part is this overhang from": you cannot read it off the
`LayerRegion` today. Three ways out (§3.2).

### 1.4 What the UI lets you set on a part

Two independent gates, both trivially widenable:

1. `SettingsFactory::get_options(bool is_part)` — `GUI_Factories.cpp:160-176`:
   ```cpp
   PrintRegionConfig reg_config;
   auto options = reg_config.keys();
   if (!is_part) { PrintObjectConfig obj_config; ... append obj_config.keys(); }
   ```
   This feeds `get_full_settings_hierarchy` (`GUI_Factories.cpp:376-401`) which builds the right-click
   **Add settings** menu, and `SettingsFactory::get_bundle` (`:258-290`) which decides what the settings
   node under a part shows. A part therefore offers only `PrintRegionConfig` keys.
2. `TabPrintPart::TabPrintPart` — `Tab.cpp:3459-3463` — is constructed with `PrintRegionConfig().keys()`,
   versus `TabPrintObject` (`Tab.cpp:3445-3449`) with `concat(PrintObjectConfig().keys(), PrintRegionConfig().keys())`.
   So even if a key reached a volume's `ModelConfig`, the part parameter panel would not render it.

Curiosities worth knowing:

- `is_improper_category` (`GUI_Factories.cpp:84-89`) tries to hide support settings from parts with
  `(!is_object_settings && category == "Support material")` — but the actual category string on all 58
  support options is `"Support"` (`PrintConfig.cpp:5924, 6023, 6103, 6149`, and 54 more). **That guard is a
  dead no-op**; the real gate is `get_options`. If you widen `get_options` without fixing this, you get the
  behaviour "free", but you also lose the deliberate hiding it was meant to provide.
- The frequent-settings popup (`create_freq_settings_popupmenu`, `GUI_Factories.cpp:447-481`) *would* offer
  the `"Support"` bundle (`FREQ_SETTINGS_BUNDLE_FFF`, `:101-105`) on a part, and
  `ObjectList::add_category_to_settings_from_frequent` (`GUI_ObjectList.cpp:1980-2014`) does **no**
  part-legality filtering before writing keys into the volume's `ModelConfig`. That path is currently dead —
  the call is commented out at `GUI_Factories.cpp:794` — but it is a latent hazard if re-enabled and a ready
  hook if you want the feature.
- Clipboard paste *is* filtered: `ObjectList::paste_settings_into_list` (`GUI_ObjectList.cpp:1294-1302`)
  drops any pasted key not in `get_options(true)` when the target is a volume or layer range.
- Object-level support settings are curated in `SettingsFactory::OBJECT_CATEGORY_SETTINGS` under `"Support"`
  (`GUI_Factories.cpp:128-137`, ~28 keys). `PART_CATEGORY_SETTINGS` (`:143-158`) has Quality/Strength/Speed
  and **no** Support group. That map is the natural place to add a curated part-level support list.

### 1.5 Where support parameters are actually read

`SupportParameters::SupportParameters(const PrintObject& object)` (`SupportParameters.hpp:11-190`) reads
~40 `object_config.*` fields **once**, into a flat struct of scalars: `num_top_interface_layers`,
`interface_spacing`, `interface_density`, `contact_fill_pattern`, `base_fill_pattern`, `gap_xy`,
`support_style`, flows, angles. It is constructed once per generator instance
(`SupportMaterial.cpp:331-338`, `TreeSupport.cpp:609-612`, `TreeSupport3D.cpp:3433`).

Read-site counts (a proxy for the size of a per-region rewrite):

| File | lines | `object_config`/`m_config.support_*` reads | `support_params.` reads | `.polygons` touches |
|---|---|---|---|---|
| `Support/SupportMaterial.cpp` | 3305 | 32 | 22 | 49 |
| `Support/TreeSupport.cpp` | 3531 | 52 | 19 | — |
| `Support/TreeSupport3D.cpp` | 4028 | 41 | 11 | 19 |
| `Support/SupportCommon.cpp` | 2069 | 10 | 71 | 68 |
| `Support/SupportParameters.hpp` | ~260 | 40 | — | — |
| `Support/TreeSupportCommon.hpp` | 760 | 18 | — | — |

Dispatch is `PrintObject::_generate_support_material` (`PrintObject.cpp:4436-4447`): tree vs normal, one
generator instance for the whole object. Normal-support pipeline (`SupportMaterial.cpp:380-560`):

```
top_contact_layers ─▶ bottom_contact_layers_and_layer_support_areas ─▶ raft_and_intermediate_support_layers
   ─▶ trim_support_layers_by_object ─▶ generate_base_layers ─▶ trim_top_contacts_by_bottom_contacts
   ─▶ generate_interface_layers(*m_object_config, m_support_params, …)      [SupportCommon.cpp:126]
   ─▶ generate_raft_base                                                    [SupportCommon.cpp:315]
   ─▶ generate_support_toolpaths(support_layers, config, support_params, …) [SupportCommon.cpp:1482]
```

The last three take `const PrintObjectConfig&` + `const SupportParameters&` by reference — a **single**
config for the whole object.

### 1.6 Where the per-part signal already exists and is thrown away

`detect_overhangs` (`SupportMaterial.cpp:1368-1546`) **already loops per region**:

```cpp
for (LayerRegion *layerm : layer.regions()) {          // SupportMaterial.cpp:1429
    ...
    polygons_append(overhang_polygons, diff_polygons);  // :1518
}
ExPolygons overhang_areas = union_ex(overhang_polygons); // :1522  ← provenance destroyed here
```

So the loop structure for per-region overhang handling is *already in place*; line 1522 is where the
attribution is discarded. (With region dedup from §1.3, that attribution would be per-region, not per-part,
unless you also break the dedup or re-slice — see §3.2.)

Same pattern for painted supports: `PrintObject::project_and_append_custom_facets`
(`PrintObject.cpp:4793-4820`) walks `model_object()->volumes`, projects each part's `supported_facets`, and
`append`s them into one flat `std::vector<Polygons>`. And `PrintObject::slice_support_volumes`
(`PrintObjectSlice.cpp:5607-5657`) unions every enforcer volume (or every blocker volume) into one
per-layer `Polygons`. `SupportAnnotations` (`SupportMaterial.cpp:1324-1348`) then holds two flat
`std::vector<Polygons>`. **Per-volume paint data exists in the model** (`mv->supported_facets`, written per
volume at `GLGizmoFdmSupports.cpp:584`) **and is flattened before the generator sees it.**

### 1.7 Keys that are structurally harder than the rest

`support_top_z_distance` is not just a support-generator input:

- `Slicing.cpp:78` — `soluble_interface = (support_top_z_distance == 0)`, stored on `SlicingParameters`.
- `Slicing.cpp:117-127` — sets `gap_support_object` / `gap_object_support`, and rounds them to a multiple of
  layer height when `!print_config.independent_support_layer_height`.
- `PrintObject.cpp:1360-1372` — `detect_surfaces_type` decides `bottom_is_fully_supported`, i.e. whether a
  bottom surface is `stBottom` or `stBottomBridge`, from `m_config.support_top_z_distance == 0 &&
  is_auto(support_type)` (plus `bridge_no_support` / interface layers / `max_bridge_length` for tree). This
  changes **object** extrusions, not just support ones.
- `PrintObject.cpp:1002-1020` — changing it invalidates `posSlice` (full re-slice), alongside
  `support_bottom_z_distance`, `support_type`, `support_interface_top_layers`, `bridge_no_support`,
  `max_bridge_length`, `support_critical_regions_only`.
- `PrintObject.cpp:1197-1204` — `bridge_flow` only invalidates perimeters/infill when
  `support_top_z_distance > 0`.
- `GCode.cpp:1472, 1532` — read at G-code time for object-level decisions.
- `TreeSupport3D.cpp:132-136` — sets the **static** `TreeSupportSettings::soluble`
  (`TreeSupportCommon.hpp:361`, `inline static bool soluble = false`) from *any* object with
  `support_top_z_distance < EPSILON`. A process-global.

Good news: it does **not** change the object's layer Z grid (`Slicing.cpp:79-115` computes
`layer_height`/`min`/`max` independently of `soluble_interface`), and the contact-layer print_z is already
computed per contact layer (`SupportMaterial.cpp:1751, 1755, 1791`), so per-part Z gaps are *conceivable*.
But `detect_surfaces_type` is inside a `region_id` loop already (`PrintObject.cpp:1355-1372`), which is a
lucky break: that one *could* go per-region without restructuring.

Similarly `support_type` (normal vs tree) selects the whole generator at `PrintObject.cpp:4437`, so
"normal here, tree there" — the exact ask in OrcaSlicer #3658 — means running **both** generators on one
object and merging.

### 1.8 3MF serialisation — no allow-list

Export (`bbs_3mf.cpp:7631-7634`):
```cpp
for (const std::string& key : volume->config.keys())
    stream << "<metadata key=\"" << key << "\" value=\"" << volume->config.opt_serialize(key) << "\"/>";
```
Import (`bbs_3mf.cpp:4899` and `:5047`): every metadata key not matched by the known-attribute chain goes to
`volume->config.set_deserialize(metadata.key, metadata.value, config_substitutions)`.

**There is no per-volume key allow-list in either direction.** New part-level support keys therefore need
zero format work, and an older Orca/Bambu Studio reading such a project would deserialise them into the
volume config (they are valid `PrintConfigDef` keys) and then silently drop them in
`apply_to_print_region_config` — graceful degradation, not a crash. Object config takes the same untyped
path at `bbs_3mf.cpp:2057`.

---

## 2. Existing partial mechanisms — what a user can and cannot do today

### Can do

| Mechanism | Granularity | What it gives you |
|---|---|---|
| **Support painting** (FDM support gizmo, `mv->supported_facets`, `GLGizmoFdmSupports.cpp:580-594`) | per triangle, stored per volume | Force support under / block support under a painted area. **Binary only** — no parameters. Flattened at `PrintObject.cpp:4793-4820`. |
| **Support enforcer / blocker volumes** (`ModelVolumeType::SUPPORT_ENFORCER` / `SUPPORT_BLOCKER`, added via `ADD_VOLUME_MENU_ITEMS`, `GUI_Factories.cpp:330-336`) | per volume | Same binary semantics, as a mesh. Unioned per type at `PrintObjectSlice.cpp:5607-5657`. Applied at `SupportMaterial.cpp:1501-1504` (blockers subtract) and `:1650-1666` (enforcers add). |
| **Per-part region settings** (`PrintRegionConfig` keys on a `ModelVolume`) | per part / per modifier | Walls, infill, speeds, ironing, fuzzy skin, bridge flow, filament assignment. **No support keys.** |
| **Split → To parts / To objects** (`ObjectList::split`, `GUI_ObjectList.cpp:2762`; `Plater::priv::split_object`, `Plater.cpp:13292-13330`; `ModelObject::split`, `Model.cpp:2094-2210`) | promotes a part to a whole object | **This is the only way today to get per-part supports.** `Model.cpp:2168-2169` does `new_object->config.assign_config(this->config); new_object->config.apply(volume->config, true);` and `Model.cpp:2191-2201` shifts each instance so placement is preserved. Every support key then works, per piece, immediately. |
| **Height-range settings** (`ModelObject::layer_config_ranges`, `TabPrintLayer`, `Tab.cpp:3472-3476`) | per Z band of an object | `layer_height` + `PrintRegionConfig` keys. No support keys. |
| **Support-filament matching (Chameleon, this fork)** — `Print.cpp:2990-3120` `chameleon_assign_support_interfaces`, resolvers in `BrimFilament.hpp/.cpp` | per support extrusion sample | Repaints support interface/base/ironing extrusions to match the model surface above / wall beside them. Already resolves a support point to a `(layer, region)` pair via `chameleon_pick_projection_region` (`BrimFilament.hpp:393`). **Attributes to an extruder, not to a part**, and runs *after* generation — geometry is already fixed. |
| **Tree support mesh grouping** — `TreeSupport3D.cpp:125-176` `group_meshes` | per `PrintObject` | Groups objects with equal `TreeSupportSettings` for shared tree computation. The multi-object-per-group branch is `#if 0`'d (`:148-155`), so it is one group per object today. Conceptually the right shape for a per-part group design. |

### Cannot do

- Set **any** support parameter on a part, a modifier, or a height range. Not exposed
  (`GUI_Factories.cpp:160-176`, `Tab.cpp:3459-3463`) and inert if forced (`PrintObject.cpp:3266`).
- Get different interface layer counts, interface pattern/spacing, Z gap, base pattern, style, expansion,
  XY distance, or tree branch parameters under different parts of one object.
- Mix normal and tree supports within one object (`PrintObject.cpp:4437` is an either/or).
- Give a support-enforcer volume its own parameters — it is a pure boolean mask.
- Assign different support *filaments* to different parts of one object.

### The practical workaround, and its real costs

Split → To objects gives all of the above today. What you give up, grounded:

- **No cross-part slice clipping.** `PrintObject::clip_multipart_objects = true`
  (`PrintObjectSlice.cpp:30`) clips overlapping *volumes of one object* (later volume wins,
  `PrintObjectSlice.cpp:420-437`). Separate objects are never clipped against each other, so parts that
  touch or interlock get double-extruded shared surfaces.
- **No cross-part support trimming.** `trim_support_layers_by_object` (`SupportMaterial.cpp:3102-3132`)
  trims only against its own `object`; `buildplate_covered` (`SupportMaterial.cpp:1297-1320`) likewise.
  Object A's support columns can collide with object B's geometry, and `support_on_build_plate_only` is
  computed blind to B.
- **Prime-tower constraints tighten.** `Print::validate` (`Print.cpp:1785-1805`) requires equal layer
  heights / raft layers / layering across all objects when the prime tower is on. (The old
  `gap_support_object` equality check is `#if 0`'d at `:1798-1802`, so differing Z gaps are allowed.)
- **Ergonomics.** No group; move/rotate/arrange must be done via multi-select
  (`Selection::MultipleFullObject`, `Selection.hpp:55-57, 262`). Nothing keeps the pieces together across
  an arrange.
- Colour paint is preserved on the multi-volume path (`Model.cpp:2180-2182` copies
  `mmu_segmentation_facets`) but `supported_facets` / `seam_facets` are **not** copied by `split`, and
  `new_vol->config.reset()` (`Model.cpp:2186`) plus `new_vol->source = ModelVolume::Source()`
  (`Model.cpp:2206`) discard the volume's own config and its reload-from-disk link. (The volume config is
  not lost outright — it was already merged into the new object's config at `Model.cpp:2169` — but any
  key that is object-scope-only now applies to the whole new object.)

---

## 3. Design options

Effort figures are rough single-engineer days for someone already fluent in this code base, including tests
and a GUI pass but **not** including a long field-validation cycle. Treat ±40% as the honest band.

### (a) Split into objects + a "group" concept

**What it is.** Keep the existing per-object support path untouched. Add a first-class *group* to the model
so several `ModelObject`s move/rotate/arrange/delete/select as one, and make "Split for per-part settings"
a one-click operation that leaves a group behind.

**Viability: yes, for the slicing half — it already works.** Everything downstream is unchanged: each new
`ModelObject` gets its own `PrintObjectConfig` (`PrintObject.cpp:3230`), `SlicingParameters`
(`PrintObject::update_slicing_parameters`, `PrintObject.cpp:3331-3339`), `SupportParameters`, and generator
instance. `ModelObject::split` already promotes volume config onto the new object (`Model.cpp:2168-2169`)
and preserves placement (`Model.cpp:2191-2201`).

**What the codebase has for grouping: essentially nothing.** Searching `Model.hpp` for group/assembly
concepts turns up only `ModelInstance::m_assemble_transformation` / `m_offset_to_assembly`
(`Model.hpp:1253-1301`) and `CalculateAssemblyBoundingBox` (`Model.hpp:448-451`) — these drive the
**assembly (exploded) view** in `GLCanvas3D`/`GUI_Preview`/`Selection`, a *visualisation* transform, not a
slicing or selection grouping. There is no `group_id` on `ModelObject`, no group node in
`ObjectDataViewModel`, and no group handling in arrange. `Selection` supports `MultipleFullObject` /
`MultipleFullInstance` (`Selection.hpp:53-57`) so multi-object transforms already work; a group is mostly
"a persisted, named multi-selection".

**Effort.**
- Zero-day variant (document the workaround, polish `split`: carry `supported_facets`/`seam_facets` across,
  keep names, add a "Split for support settings" menu entry): **2-4 d**.
- Real grouping: `group_id` on `ModelObject` + undo/redo + 3MF round-trip **3-5 d**; ObjectList tree
  nesting **4-6 d**; Selection/gizmo/arrange/delete/plate-assignment honouring groups **8-15 d**;
  interactions and polish **3-5 d** → **18-30 d**.

**Risk to existing prints: near zero** for the back end (no libslic3r change). Medium risk of GUI
regressions in selection/arrange, which are historically fiddly here.

**Interactions.** Tree vs normal: perfect — each object picks its own generator, so "normal here, tree
there" works. Support painting: perfect — per volume already. Multi-material / soluble interface: each
object gets its own `support_interface_filament`; the prime tower gets more tool changes and
`Print::validate`'s layer-height equality rules (`Print.cpp:1785-1805`) start to bite. Variable layer
height: each object gets its own `layer_height_profile`, which is a *feature* but also means an assembly can
end up with mismatched Z grids — and the prime tower then refuses (`Print.cpp:1801-1803`,
`equal_layering`). 3MF: `group_id` is one new object-level metadata key, no allow-list to update
(`bbs_3mf.cpp:2057`).

**Verdict.** Best value-per-day by a wide margin, and it is the only option that gives *all* support keys
(including `support_type` and `support_filament`) at part granularity. Its real defect is the loss of
cross-part clipping and support trimming for assemblies whose parts touch — which is exactly the
print-in-place case.

### (b) True per-region support parameters inside the generators

**What it is.** Make `SupportParameters` per-region, tag support geometry with the region it serves, and
have contact/interface/base generation and toolpath generation consult the tag.

**Viability: partial, and expensive.** The pieces that favour it:
- `detect_overhangs` already iterates `layer.regions()` (`SupportMaterial.cpp:1429`); only the `union_ex` at
  `:1522` erases the tag.
- `detect_surfaces_type`'s support-dependent bridging decision is already inside a `region_id` loop
  (`PrintObject.cpp:1355-1372`).
- `generate_interface_layers` (`SupportCommon.cpp:126-313`) uses `num_top_interface_layers` only as a
  *vertical window* (`top_z` at `:218`) to select which contact layers project into an intermediate layer.
  Sharding that by group and unioning is mechanically straightforward.

The pieces that fight it:
- `SupportGeneratorLayer` (`SupportLayer.hpp:40-113`) holds one flat `Polygons polygons` plus three
  optional `Polygons`; `merge()` (`:66-79`) unions them. Adding a tag means either a
  `std::vector<std::pair<Polygons, GroupId>>` (touching ~136 `.polygons` sites across
  `SupportMaterial.cpp`/`SupportCommon.cpp`/`TreeSupport3D.cpp`) or K parallel layer sets.
- `SupportParameters` is consumed ~123 times as a scalar bundle.
- Tree supports are the hard half. `TreeSupport.cpp` (3531 lines) reads `m_object_config` 52 times and
  computes influence areas as one object-wide problem; `TreeSupport3D.cpp` (4028 lines) threads a single
  `const TreeSupportSettings& config` through 20+ functions, and `TreeSupportSettings::soluble` is a
  **static** (`TreeSupportCommon.hpp:361`). Per-region branch angle / tip diameter / wall count means either
  per-group tree runs (→ option (b′)) or a deep rewrite of influence-area propagation.
- Region ≠ part (§1.3). You would additionally need to break region dedup (see below) or attribute by
  re-slicing.

**Getting part attribution — three routes.**
1. *Break the dedup.* Add a hidden `support_group_id` int to `PrintRegionConfig`, set from the part's
   support override, so `get_create_region` (`PrintApply.cpp:1025`) cannot merge two parts with different
   support settings. Then `LayerRegion → PrintRegion → group` is available everywhere for free.
   **Cost:** distinct regions are *not* merged/closed at `PrintObjectSlice.cpp:441-467`, so touching parts
   get separate perimeter loops and a visible seam where they meet. Acceptable for assemblies with
   clearances; a regression for parts that share a surface.
2. *Re-slice the group's volumes* at the object's layer Z's and intersect. `slice_support_volumes`
   (`PrintObjectSlice.cpp:5607-5657`) is exactly this code, already written and used for enforcers. Costs
   one extra mesh slice per group; **no** region/perimeter side effects. This is the safe route.
3. *Persist per-volume slices.* `VolumeSlices` (`Print.hpp:50-54`) already exists but only the first layer
   survives (`firstLayerObjSliceByVolume`, `Print.hpp:639`); the rest are consumed by `slices_to_regions`.
   Retaining them costs memory on large models.

**Effort.** `SupportParameters` per group + threading: **10-15 d**. Tagging support layers through
`SupportMaterial.cpp`/`SupportCommon.cpp`: **15-25 d**. Tree parity: **20-30 d**. Tests/validation:
**10 d**. → **55-80 d**.

**Risk: high.** Every code path in the file with the most subtle geometry in the slicer changes shape.

**Interactions.** Painting: enforcer/blocker masks would need per-group splitting too. Soluble interface:
`soluble_interface_non_soluble_base` (`SupportParameters.hpp:17-24`) and `can_merge_support_regions`
(`:86-95`) become per-group, which multiplies support extrusion roles and tool changes. Variable layer
height: contact layers already carry their own print_z, so per-group Z gaps produce more distinct support
layers — interacts with `independent_support_layer_height` (`Slicing.cpp:122`). 3MF: no format work.

#### (b′) Per-part support **groups** — the pragmatic middle (recommended core)

Not in the original list, but it dominates (b) and deserves to be named.

**What it is.** Resolve each `MODEL_PART`'s support config (object config + the part's override). Group
parts by resolved config — in practice K = 2, rarely 3. For each group build a per-layer mask by slicing
that group's volumes (route 2 above, reusing `slice_support_volumes`'s machinery). Then run the **existing,
unmodified** generator once per group:

- construct `SupportParameters` from a per-group `PrintObjectConfig` (a `PrintObjectConfig` copy with the
  group's overrides applied — same shape as `object_config_from_model_object`, `PrintObject.cpp:3230`);
- restrict overhang sources to the group's mask (one intersection right after
  `SupportMaterial.cpp:1522`, or a filter on the `layer.regions()` loop at `:1429`);
- keep trimming/collision against the **whole** object (`trim_support_layers_by_object`,
  `buildplate_covered` are already whole-object) — this is the property option (a) loses;
- merge group results into the object's `SupportLayer`s, arbitrating overlaps by group order
  (trim group *n* by the union of groups < *n*), then run `generate_support_toolpaths` per group.

This is structurally identical to Cura (§4) and to what `group_meshes` (`TreeSupport3D.cpp:125-176`) was
built for. It also gives "normal here, tree there" almost free, because the generator choice at
`PrintObject.cpp:4437` becomes per group.

**Viability: yes.** No change to `SupportParameters`, `SupportGeneratorLayer`, or the tree internals.

**Effort.** Config plumbing + group resolution + per-group masks **5-8 d**; normal-support per-group run +
merge/arbitration **8-12 d**; tree per-group run + merge **10-15 d**; UI **3-5 d**; 3MF/undo/redo
**1-2 d**; tests **5-8 d** → **32-50 d**.

**Risk: medium.** Off-mode purity is easy to guarantee — if every part resolves to the same config, K=1 and
the code takes the byte-identical existing path (the same discipline this fork already applies to Chameleon,
`Print.cpp:3101-3108`). Main new risks: K× support-generation time on big models, and the merge/arbitration
rule at group boundaries.

**Interactions.** Tree vs normal: works, per group. Painting: enforcer/blocker masks must be split by group
(or applied to all groups — a design decision). Soluble/multi-material: each group can carry its own
`support_interface_filament`, which multiplies tool changes; gate it behind an explicit opt-in. Variable
layer height: unchanged (one Z grid per object). 3MF: no format work.

### (c) Support modifier volumes

**What it is.** A modifier mesh (new `ModelVolumeType`, or `PARAMETER_MODIFIER` carrying support keys) whose
volume overrides a subset of support parameters for support geometry inside it.

**Viability: partial.** The mask half is easy and already written — `slice_support_volumes`
(`PrintObjectSlice.cpp:5607-5657`) produces exactly the per-layer `Polygons` needed, and
`SupportAnnotations` (`SupportMaterial.cpp:1324-1348`) is where it would land. Applying the override is
where it gets murky.

**The support-column problem is the design, not a detail.** A support column is generated by *downward
propagation* from a contact layer: `bottom_contact_layers_and_layer_support_areas` produces
`layer_support_areas` and `generate_base_layers` (`SupportMaterial.cpp:456`) fills the intermediate layers
from them. So a column crossing a modifier box has a *contact* outside the box and *body* inside it. Three
coherent semantics, and you must pick one and document it:

1. **Origin semantics** — a support region takes the modifier's settings if its *contact* (the overhang it
   supports) lies inside the modifier. Matches "the interface under this part should be denser". Requires
   tagging at contact time and propagating the tag down — which is (b)/(b′)'s tagging problem again.
2. **Containment semantics** — support *at a given Z* takes the modifier's settings if the support polygon
   at that Z is inside the modifier. Easy to implement (per-layer intersection) but produces settings that
   change mid-column: interface density switching at an invisible box boundary, and for
   `support_top_z_distance` it is meaningless (the gap is a property of the contact, not of the column).
3. **Hybrid** — geometric parameters (Z gap, interface layer count, style, branch angle) use origin
   semantics; purely-local fill parameters (interface pattern/spacing, base pattern/spacing, ironing) use
   containment. Most useful, most explaining to do.

Realistically only a *subset* of keys is meaningful under containment: `support_base_pattern`,
`support_base_pattern_spacing`, `support_interface_spacing`, `support_interface_pattern`,
`support_ironing*`, and arguably `tree_support_wall_count`. `support_top_z_distance`, `support_style`,
`support_type`, and interface *layer counts* are origin-semantics keys.

**Effort.** New volume type / config-on-modifier + gizmo/ObjectList/3MF **7-11 d**; mask + per-layer
override for the containment subset **10-15 d**; origin-semantics subset (needs tagging) **+10-15 d**;
docs/UX for the semantics **3-5 d** → **22-34 d** for containment-only, **32-49 d** with origin semantics.

**Risk: medium-high — mostly user-comprehension risk.** Silent, hard-to-predict behaviour at box boundaries
is how this feature earns bug reports. Also interacts confusingly with support painting (two overlapping
mask systems with different semantics).

**Verdict.** Strong as a *second* feature once (b′) exists (a modifier is then just another way to assign a
support group), weak as the first one.

### (d) Per-part interface settings only

**What it is.** Expose only interface-side keys at part level, applied to the contact/interface layers under
each part; base support stays shared and object-wide.

**Viability: yes, and it is the smallest coherent slice** — with one correction to the framing:
`support_top_z_distance` is **not** an interface-only key (§1.7). It sets `SlicingParameters::soluble_interface`
(`Slicing.cpp:78`), drives object surface classification at `PrintObject.cpp:1368`, forces `posSlice`
invalidation (`PrintObject.cpp:1005`), and sets a process-global static for organic trees
(`TreeSupport3D.cpp:133` → `TreeSupportCommon.hpp:361`). Include it in phase 1 only if you are prepared to
(i) make `detect_surfaces_type`'s `bottom_is_fully_supported` per-region — feasible, it is already in a
`region_id` loop — and (ii) forbid mixing 0 and non-zero within one object (i.e. soluble-interface mode
stays object-wide). I would ship the rest first and treat per-part Z gap as a phase-2 item.

**Implementation sketch.** Per-group masks (route 2). Split `top_contacts` — each `SupportGeneratorLayer`'s
`polygons`/`overhang_polygons` intersected with each group's mask, giving K contact sets. Call
`generate_interface_layers` (`SupportCommon.cpp:126`) once per group with the group's `SupportParameters`,
passing that group's contacts and the shared `intermediate_layers`; union the resulting interface layers,
and subtract each group's interface from `intermediate_layer.polygons` in group order (the function already
does this subtraction at `SupportCommon.cpp:185`). Then `generate_support_toolpaths`
(`SupportCommon.cpp:1482`) fills each group's interface layers with that group's
`contact_fill_pattern`/`interface_density`/`interface_angle`.

**Effort.** Normal supports only, keys `support_interface_top_layers`, `support_interface_bottom_layers`,
`support_interface_spacing`, `support_bottom_interface_spacing`, `support_interface_pattern`,
`support_interface_loop_pattern`: config + UI **4-6 d**; masks + contact split **4-6 d**; per-group
interface generation + merge **5-8 d**; toolpaths **3-5 d**; tests **3-5 d** → **19-30 d**. Tree-support
parity (roof areas in `TreeSupport.cpp:1749, 1888-1920`, `TreeSupport3D.cpp:3516`) **+6-10 d**.

**Risk: low-medium.** Confined to the interface stage. Off-mode purity is trivially provable (K=1 → the
existing call).

**Interactions.** Painting: unaffected (masks are independent). Multi-material/soluble: leave
`support_interface_filament` object-wide in phase 1 — making it per-part reaches `ToolOrdering` /
`WipingExtrusions` (`ToolOrdering.cpp:1865-1890`) and this fork's Chameleon pass
(`Print.cpp:2990-3120`), which is a separate design conversation. Variable layer height: none. 3MF: none.

### Side-by-side

| | (a) split+group | (b) per-region | (b′) support groups | (c) support modifiers | (d) interface-only |
|---|---|---|---|---|---|
| Viability | yes (works today) | partial, painful | **yes** | partial | yes |
| Effort (d) | 2-4 / 18-30 | 55-80 | 32-50 | 22-49 | 19-30 (+6-10 tree) |
| Risk to existing prints | ~none | high | medium | medium | low-med |
| Covers `support_type` normal/tree mix | ✅ | ✅ | ✅ | ⚠️ | ❌ |
| Covers interface layers/pattern/spacing | ✅ | ✅ | ✅ | ✅ | ✅ |
| Covers `support_top_z_distance` | ✅ | ⚠️ | ⚠️ | ❌ | ⚠️ phase 2 |
| Covers tree branch settings | ✅ | ⚠️ hard | ✅ | ⚠️ partial | ❌ |
| Keeps cross-part clipping & trimming | ❌ | ✅ | ✅ | ✅ | ✅ |
| 3MF work | 1 key | none | none | volume type | none |

---

## 4. How other slicers do it

Checked 2026-09-02.

**Cura — the outlier, and the proof the architecture is possible.** CuraEngine generates support
**per mesh** and unions afterwards. `AreaSupport::generateSupportAreas` loops
`for (mesh_idx …) { if (!mesh.isModelMesh()) continue; generateSupportAreasForMesh(storage, *infill_settings,
*roof_settings, *bottom_settings, mesh_idx, …, mesh_support_areas_per_layer); }`, accumulating into
`global_support_areas_per_layer`, then `support_areas = support_areas.unionPolygons()` and
`splitGlobalSupportAreasIntoSupportInfillParts(...)`. Per-mesh settings read there include
`support_top_distance`, `support_angle`, `support_enable`, `support_roof_enable`, `support_roof_height`,
`support_bottom_enable`, `support_bottom_height`, `support_use_towers`, `support_mesh`,
`support_mesh_drop_down`. Cura also has `support_mesh` (print this model *as* support, inheriting the
support settings) and `anti_overhang_mesh` (blocker).
[CuraEngine/src/support.cpp](https://raw.githubusercontent.com/Ultimaker/CuraEngine/main/src/support.cpp) —
checked 2026-09-02.
Cura's *groups* are a scene-graph/UI concept: you Ctrl-click a model inside a group and edit its Per Model
Settings without ungrouping.
[Ultimaker community — per-model settings & groups](https://community.ultimaker.com/topic/45684-custom-supports/),
[MakerBot — how to adjust print settings per model in Cura](https://support.makerbot.com/s/article/1667411288884)
— checked 2026-09-02. **This is precisely option (a)+(b′) combined**, and it is why Cura users take
per-model support settings for granted.
*Caveat:* I could not fetch the `settable_per_mesh` flags for individual support settings out of
`fdmprinter.def.json` (the file is too large for the fetch tool and the summariser produced values I do not
trust). The engine-side evidence above is the solid part; treat "every Cura support setting is per-mesh" as
unverified.

**PrusaSlicer — same limitation as us, same code lineage.** Per-model settings offer "Infill", "Layers and
perimeters" and "Support material" in Advanced mode, plus more categories in Expert mode; the article
describes overrides that "affect only the relevant object and its instances" and does not distinguish
part/modifier scope.
[Prusa KB — Per model settings](https://help.prusa3d.com/article/per-model-settings_1674) — checked
2026-09-02. Our `SettingsFactory::get_options` / `TabPrintPart` split is inherited PrusaSlicer code, and
PrusaSlicer's `PrintObjectConfig`/`PrintRegionConfig` division puts support keys object-only in exactly the
same way, so "Support material" there is an **object**-level category. Modifier meshes in PrusaSlicer are
documented for infill/perimeters/speed/ironing/fuzzy skin/extrusion width — not support parameters.
[Prusa KB — Modifiers](https://help.prusa3d.com/article/modifiers_1767) — checked 2026-09-02.

**Bambu Studio — same as us (we are its fork).** The object list supports per-object overrides; not all
parameters are available per object or per modifier. I could not retrieve the Bambu wiki modifier page
(`wiki.bambulab.com` returned HTTP 402 to the fetcher), so this rests on secondary sources and on our own
inherited code (`GUI_Factories.cpp:160-176` is Bambu's).
[Bambu Studio modifier guide (not retrievable)](https://wiki.bambulab.com/en/software/bambu-studio/modifier)
— attempted 2026-09-02.

**OrcaSlicer upstream — requested, never implemented.**
- [#3658 "Set support type per modifier object"](https://github.com/OrcaSlicer/OrcaSlicer/issues/3658) —
  opened 2024-01-13, **closed as not planned**, labelled stale, no maintainer design response. Asks for
  exactly the assembly case: normal supports globally, tree supports inside a specific modifier region.
- [#9701](https://github.com/OrcaSlicer/OrcaSlicer/issues/9701) — opened 2025-05-22, **open**, labelled
  Enhancement, explicitly a re-file of #3658 after the bot closed it; no maintainer reply.
- [#6470 "Support Filament Painting / Support Modifiers"](https://github.com/OrcaSlicer/OrcaSlicer/issues/6470)
  — users describe difficulty with varying support settings across object assemblies.
- [#3421 "Part modifier modifications not being applied in correct order"](https://github.com/OrcaSlicer/OrcaSlicer/issues/3421)
  — evidence that modifier-override ordering is already a live source of bugs, relevant to option (c).
- [Discussion #5330 "Complete support block (no supports in a volume)"](https://github.com/OrcaSlicer/OrcaSlicer/discussions/5330).
All checked 2026-09-02.

**Read of the landscape.** Every PrusaSlicer-lineage slicer (Prusa, Bambu, Orca, us) has the same
object/region config split and therefore the same limitation; Cura, which never had that split, solved it by
generating support per mesh. The feature is a long-standing, unaddressed upstream request — which is both an
opportunity to differentiate and a warning that nobody in this lineage has found it cheap.

---

## 5. Recommendation

### Build (b′) — per-part support groups — with (d) as its first shipped phase

Reasoning, grounded:

- It is the only option that gets the user's stated needs (interface layer counts, interface pattern, Z gap,
  style, tree branch settings) **without** losing cross-part clipping and support trimming, which option (a)
  gives up precisely for print-in-place assemblies.
- It requires **no** change to `SupportParameters`, `SupportGeneratorLayer`, or the ~7,500 lines of tree
  support internals — the expensive parts of option (b).
- Attribution via re-slicing the group's volumes reuses `slice_support_volumes`
  (`PrintObjectSlice.cpp:5607-5657`) verbatim and has no perimeter/region side effects.
- Off-mode byte-identical output is provable by construction (K==1 → today's call path), matching this
  fork's existing discipline for Chameleon (`Print.cpp:3101-3108`).
- It generalises: option (c)'s support modifiers become "a modifier assigns a support group", and option
  (a)'s grouping remains a useful, independent UX improvement.

### Config keys to expose at part level

**Phase 1 (interface only — safe, all keys are consumed downstream of contact generation):**

| Key | `PrintConfig.hpp` | Read at |
|---|---|---|
| `support_interface_top_layers` | 973 | `SupportParameters.hpp:27`, `SupportCommon.cpp:216-227` |
| `support_interface_bottom_layers` | 974 | `SupportParameters.hpp:28-29`, `SupportCommon.cpp:239-252` |
| `support_interface_spacing` | 976 | `SupportParameters.hpp:103-107` |
| `support_bottom_interface_spacing` | 1022 | `TreeSupport.cpp:1333` |
| `support_interface_pattern` | 979 | `SupportParameters.hpp:123-132` |
| `support_interface_loop_pattern` | 967 | `SupportCommon.cpp:1496` |
| `support_ironing`, `support_ironing_pattern`, `support_ironing_flow`, `support_ironing_spacing` | 999-1002 | `SupportParameters.hpp:50-53` |

**Phase 2 (adds base/geometry — needs per-group generator runs):**
`support_base_pattern` (978), `support_base_pattern_spacing` (981), `support_expansion` (982),
`support_style` (984), `support_threshold_angle` (995), `support_threshold_overlap` (996),
`support_object_xy_distance` (997), `support_on_build_plate_only` (958), `support_angle` (957),
`tree_support_branch_angle` (1013), `tree_support_branch_angle_organic`, `tree_support_branch_diameter`
(1012), `tree_support_branch_distance`, `tree_support_wall_count` (1016),
`tree_support_branch_diameter_angle`, `tree_support_top_rate`, `tree_support_brim_width`.

**Phase 3 (structurally entangled — each needs its own decision):**
`support_top_z_distance` (961) and `support_bottom_z_distance` (962) — see §1.7; requires per-region
`bottom_is_fully_supported` (`PrintObject.cpp:1368`) and a rule forbidding mixed 0/non-zero within an
object. `support_type` (955) — needs both generators on one object. `enable_support` (953) — mostly
achievable already with a blocker volume. `support_filament` (964) / `support_interface_filament` (968) —
reaches `ToolOrdering`, the prime tower, and this fork's Chameleon pass; deliberately out of scope.

**Explicitly never per-part:** `raft_*` (a raft is an object-wide substructure, `Slicing.cpp:130-150`),
`enforce_support_layers` (963 → object layer indices), `independent_support_layer_height` (a `PrintConfig`,
i.e. printer-wide, key).

### Phased plan with tests

**Phase 0 — de-risk and ship the workaround (2-4 d).**
Polish `ModelObject::split` (`Model.cpp:2094-2210`): carry `supported_facets` and `seam_facets` to the new
volumes (today only `mmu_segmentation_facets` survives, `:2180-2182`), keep part names, and add a menu entry
that names the use case. Document the caveats from §2 in the tooltip. This immediately answers most users.

**Phase 1 — part-level interface settings, normal supports (19-30 d).**
1. Config plumbing: add the phase-1 keys to `SettingsFactory::get_options` for parts (widen
   `GUI_Factories.cpp:160-176` with a curated list rather than all of `PrintObjectConfig`), add a `"Support"`
   group to `PART_CATEGORY_SETTINGS` (`GUI_Factories.cpp:143-158`), widen `TabPrintPart`'s key list
   (`Tab.cpp:3459-3463`), and fix the dead `is_improper_category` guard (`GUI_Factories.cpp:88`) so it
   filters the *not*-allowed support keys rather than a category string that never occurs.
2. `PrintObject`: resolve per-part support configs and compute groups; K==1 → early-out to today's path.
3. Per-group masks via the `slice_support_volumes` pattern (`PrintObjectSlice.cpp:5607`).
4. Split `top_contacts` by mask; call `generate_interface_layers` (`SupportCommon.cpp:126`) per group;
   merge; per-group interface fill in `generate_support_toolpaths` (`SupportCommon.cpp:1482`).
5. Invalidation: `PrintObject::invalidate_state_by_config_options` already routes ~45 support keys to
   `posSupportMaterial` only (`PrintObject.cpp:1029-1084`) while a smaller set forces a full `posSlice`
   (`PrintObject.cpp:1002-1020`). Part-level keys must be added to the *first* list and must never land in
   the second — that is the boundary that keeps phase 1 cheap and phase 3 expensive.

**Phase 2 — full per-group generation (13-22 d on top).**
Run the whole `PrintObjectSupportMaterial::generate` / `TreeSupport::generate` per group with a masked
overhang source and whole-object trimming; arbitrate overlaps in group order. Unlocks base pattern, style,
XY distance, tree branch settings, and normal/tree mixing.

**Phase 3 — Z gap and filament (scoped separately).**

### Test coverage today, and what to add

`tests/fff_print/test_support_material.cpp` (317 lines) has only **three live tests**:
- `:12` "Three raft layers created" — asserts `support_layers().size() == 3`.
- `:51` `WipingExtrusions::is_support_overriddable` — this fork's Chameleon residual-pin predicate.
- `:100` SCENARIO "support_layers_z and contact_distance" — first support layer height, and min/max layer
  height bounds across support layers (its interior top-spacing check is `#if 0` at `:126-149`).
- `:201-317` — the "forced support is generated" and "Checking bridge speed" scenarios are inside `#if 0`
  and do not compile.

So **support geometry is effectively untested**. Additions to make with this work:
1. `[SupportMaterial]` unit tests on the group-resolution function itself (parts with equal config → K==1;
   one override → K==2; override equal to the object value → K==1). Cheap and pins off-mode purity.
2. A two-part object where part B overrides `support_interface_top_layers`; assert the interface extrusion
   length above B differs and above A does not. Expressible via `init_and_process_print`
   (`tests/fff_print/test_data.hpp`) plus `SupportLayer::support_fills` role filtering, in the style of the
   existing Chameleon test at `:51`.
3. Reuse the Slice Compare harness (`docs/superpowers/specs/2026-08-29-slice-compare-design.md`,
   `tests/libslic3r/test_slice_compare.cpp`) for the byte-identical off-mode guarantee across a corpus of
   existing projects — the strongest single regression control available here.
4. A 3MF round-trip test (`tests/libslic3r/test_3mf.cpp`) for part-level support keys, plus a manual check
   that a project written with them still loads in stock Orca (expected: keys deserialise into the volume
   config and are inertly dropped at `PrintObject.cpp:3266`).

### Open questions for the user

1. **What is the real shape of your assemblies?** If the parts have printing clearances and never touch,
   option (a) + a group concept is dramatically cheaper and gives *all* keys. If they touch or interlock
   (print-in-place), the loss of `clip_multipart_objects` makes (a) wrong and (b′) necessary. This single
   answer changes the recommendation.
2. **Which keys actually matter?** The question lists interface layer counts, top z distance, style,
   interface pattern, and tree branch settings. `support_top_z_distance` is by far the most expensive of
   those (§1.7) and `support_style`/tree branch settings need phase 2. If interface layers + interface
   pattern/spacing carry most of the value, phase 1 alone is ~4-6 weeks.
3. **Mixing normal and tree in one object** — is that in scope, or is per-object generator choice enough?
   It is the headline ask in upstream #3658 but it roughly doubles the merge/arbitration work.
4. **Per-part support *filament*** — in or out? It is the one key that reaches the wipe tower, `ToolOrdering`
   and this fork's Chameleon pass, and it would materially change the risk profile.
5. **UI model** — do you want per-part settings in the object tree (consistent with today), a support-group
   picker ("Support profile: A / B"), or modifier volumes? A named-group picker scales better past two
   groups and makes the K-runs cost legible to the user.
6. **Runtime budget** — per-group generation is roughly K× support-generation time on the affected object.
   Acceptable, or does it need a cap on K?
7. Should the part-level list be **curated** (a hand-picked `PART_CATEGORY_SETTINGS` "Support" group) or
   **mechanical** (everything in `PrintObjectConfig` whose category is `"Support"`)? Curated is safer given
   how many of those keys are structurally object-wide.
