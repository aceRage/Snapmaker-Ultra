# Re-editable cuts

Status: implemented on `feat/re-editable-cuts`.
Follows on from `2026-09-11-draw-cut-research.md` §5, which flagged this as a separate feature:
"a new per-object record … in a **new** metadata file, not bolted onto the per-connector one …
designed for the sheet and the stroke together. Flag, do not build." This is that build.

---

## 1. The problem

A cut is destructive. The Cut gizmo's plane (`m_plane_center`, `m_rotation_m`), curved sheet
(`m_curved_sheet`) and drawn stroke (`m_draw_stroke`) are **session state**: `on_set_state()`
(`GLGizmoCut.cpp`) flattens every one of them when the gizmo opens *and* when it closes. The cut
bakes the surface into two new meshes and the description of how they were made is gone.

The only thing that survived a cut was the connector **volumes**, through
`Metadata/cut_information.xml` — and only as baked geometry plus per-connector tolerances, keyed to
`CutObjectBase cut_id`, which links the two halves.

So: move the plane 2 mm and you re-cut from scratch, re-place every connector, and lose the sheet
you spent ten minutes bending.

---

## 2. What is persisted

### 2.1 The record

`src/libslic3r/CutRecipe.hpp` — `struct CutRecipe`, hanging off `ModelObject::cut_recipe` as a
`std::optional`. **Both halves of one cut carry an equal recipe**, so selecting either is enough to
reopen the cut. They already share `cut_id`; that is what the re-edit uses to find the *other* half.

| Field | Meaning |
|---|---|
| `version` | Schema version. `CutRecipeVersion` = 1. |
| `kind` | `Plane` / `Curved` / `Drawn` / `Groove` — the one choice that decides which `Cut::perform_*` runs. |
| `plane_center`, `rotation_m` | The cut frame, **in the object's coordinate system**. |
| `sheet` | `nx`, `ny`, `half_size_u`, `half_size_v`, `values[]` (`kind == Curved`). |
| `stroke` | The stroke's **raw** samples (pos, normal, facet), `closed`, `smoothing` (`kind == Drawn`). |
| `draw_direction`, `draw_view_dir`, `draw_extension`, `draw_angle_deg`, `draw_through_all`, `draw_depth` | The sweep parameters (`kind == Drawn`). |
| `groove` | Depth / width / flap angle / angle, their `_init` values and tolerances (`kind == Groove`). |
| `thickness`, `thickness_offset` | The kerf and which side it is taken from. |
| `keep_upper` … `rotate_lower` | The after-cut attributes, exactly as the panel's checkboxes set them. |
| `upper_visibility`, `lower_visibility` | Preview-only side visibility (0 Visible, 1 Ghost, 2 Hidden). |
| `connectors[]` | The connector **definitions** (`ModelObject::cut_connectors`), including full `FlexiJointParams`. |
| `mesh`, `mesh_hash` | The pre-cut mesh, and the SHA-256 that names its blob. |

### 2.2 Three decisions

**The stroke is stored as its RAW samples, not its finished path.** `DrawCutStroke::finish()`
documents that it always re-derives the resampled path, the smoothing and the binormals *from the
raw samples* — "always starts from the RAW samples". Storing both would let the two disagree. The
smoothing value travels with the samples because it is an **input** to that derivation, not a
property of the sweep.

**The mesh blob is not STL.** An STL round trip gives every facet its own three vertices and
re-welds on the way back, so the mesh that came out would not be the mesh that went in — and the
entire point of storing it is that re-cutting reproduces the *same* halves. The blob is the
`indexed_triangle_set` verbatim:

```
magic "ESCUTMSH" | uint32 version(1) | uint32 n_vertices | uint32 n_facets
| n_vertices × 3 × float32 | n_facets × 3 × uint32       (all little-endian)
```

**The plane is in the object frame, not the world.** The halves get moved, rotated and re-arranged
on the bed after a cut. A world-frame plane would then describe a cut through empty space the next
time the project opened. The gizmo converts with the instance offset.

### 2.3 What the pre-cut mesh actually is

`GLGizmoCut3D::object_part_mesh()`: every `MODEL_PART` volume that is **not** a cut connector,
merged and taken into the object frame. Connector volumes are excluded deliberately — they are
generated *by* the cut from the connector definitions the recipe already stores, so keeping them
would bake one generation of connectors into the next re-cut's input.

---

## 3. The 3MF schema

Two new archive entries, both under `Metadata/`, **neither referenced from the main `<resources>`
model mesh list**. This is the same contract `Metadata/cut_information.xml` and
`Metadata/image_fill/` already keep, and it is what keeps the file loadable by upstream slicers
(§7).

### 3.1 `Metadata/cut_recipe.xml`

```xml
<cut_recipes>
 <recipe object_id="1" version="1" kind="1" mesh="9f86d081…"
         center_x="0" center_y="0" center_z="0"
         rotation="1 0 0 0  0 1 0 0  0 0 1 0  0 0 0 1"
         thickness="0.8" thickness_offset="1"
         keep_upper="1" keep_lower="1" keep_as_parts="0"
         place_on_cut_upper="1" place_on_cut_lower="0"
         rotate_upper="0" rotate_lower="1"
         upper_visibility="0" lower_visibility="0"
         draw_direction="0" draw_view_x="0" draw_view_y="0" draw_view_z="-1"
         draw_extension="5" draw_angle="0" draw_through_all="1" draw_depth="10">
  <sheet nx="5" ny="5" half_size_u="30" half_size_v="30" values="0 0 0 … 6 … 0"/>
  <stroke closed="0" smoothing="0.2">
   <s px="-20" py="0" pz="20" nx="0" ny="0" nz="1" f="0"/>
   …
  </stroke>
  <groove depth="3.5" width="5.5" …/>
  <connectors>
   <connector x="4" y="-3" z="0" rotation="1 0 0 0 …" radius="2.5" height="6"
              r_tolerance="0.05" h_tolerance="0.15" z_angle="0.75"
              type="0" style="1" shape="2"
              flexi_kind="…" …/>
  </connectors>
 </recipe>
</cut_recipes>
```

- `object_id` is the **1-based** 3MF object index, the same key `cut_information.xml` uses.
- `rotation` is 16 doubles, **row-major**, at `setprecision(17)`. A matrix rather than Euler angles
  so no convention has to be agreed between writer and reader, and so it round trips exactly.
- `sheet/@values` is `nx*ny` doubles, row-major, at `setprecision(17)`.
- `<sheet>`, `<stroke>` and `<groove>` appear only for their own `kind`.
- The `flexi_*` attributes mirror `cut_information.xml`'s, field for field, with the same
  default-on-absence rule.

### 3.2 `Metadata/cut_recipe/<sha256>.bin`

One file per **distinct** pre-cut mesh, named by the SHA-256 of its own blob bytes. Both halves of
one cut name the same hash, so the mesh is stored **once** — asserted by the round-trip test, which
counts the entries in the written archive.

### 3.3 Versioning

`version` is on each `<recipe>`. A reader that does not recognise the value **drops that recipe**
and logs it; the object still loads, it simply has no "Edit cut…". Guessing at fields would be
worse than refusing: the user would get a *different* cut from the one their halves were made with,
silently.

### 3.4 Untrusted input

Every value read from the archive is file content and is treated as such, following the discipline
`_extract_cut_information_from_archive` already established (and the BambuStudio #5829aa45f fix
that put it there):

- Every enum (`kind`, connector `type`/`style`/`shape`, `thickness_offset`, `draw_direction`,
  `flexi_kind`) is range-checked **before** the cast.
- Counts that drive loops are clamped (`flexi_hinge_knuckles`, `thread_starts`, `bayonet_lugs`).
- The sheet grid is bounded by `CurvedCutSheet::MaxResolution` before `nx*ny` doubles are reserved,
  and the value count must match the grid exactly or the recipe is dropped.
- The mesh blob is capped at 256 MB, its declared lengths must match the file's actual size
  exactly, and **every facet index is checked against the vertex count** — an unchecked index would
  be read out of bounds by every consumer downstream.
- The blob is re-hashed on the way in; a file whose name and content disagree is dropped rather
  than filed under a name the recipe would then match against the wrong mesh.
- A recipe whose mesh blob is absent is dropped, not half-applied.

---

## 4. UI

### 4.1 "Keep cut editable"

A checkbox in the Cut panel's "After cut" block, **default on** (`m_keep_cut_editable`). Tooltip:

> Remember this cut so it can be edited later: select either half and choose "Edit cut…" to reopen
> this panel with the same surface and settings.
>
> The original, uncut shape is stored in the project file, so a cut object takes about twice the
> space. Turn this off for a one-off cut.

With it off, `perform_cut()` writes no recipe and **everything behaves exactly as it did before this
feature existed** — asserted by a test that saves two cut halves with no recipe and checks that
neither `Metadata/cut_recipe.xml` nor any blob is in the archive.

Deliberately **per-cut, not a preference**: the cost is per-object and the decision is
per-object. (The same reasoning `m_cut_thickness` already follows — see the phase-3 comment in
`on_set_state()` about sticky state silently affecting the *next* object.)

### 4.2 "Edit cut…"

Object right-click → "Edit cut…", appended by `MenuFactory::append_menu_item_edit_cut()`. Dynamic
(destroy-then-re-append on each menu open), exactly like the "Invalidate cut info" item beside it,
so it is **absent** rather than greyed out on objects that carry no recipe. Enable condition:
`ObjectList::has_selected_editable_cut()` → any selected object with `has_cut_recipe()`.

`ObjectList::edit_cut()`:

1. Refuses while another gizmo is open (`check_gizmos_closed_except`), the rule
   `ObjectList::simplify()` follows.
2. Finds the selected half carrying a recipe, then collects **every object of the same cut** by
   `cut_id` — not by recipe, which is deliberately equal on both halves and so cannot tell two
   different cuts apart.
3. Takes **one** `Plater::TakeSnapshot("Edit cut")` that brackets the whole re-edit.
4. `GLGizmoCut3D::arm_reedit(recipe, ids)`; on refusal (no stored mesh, unknown schema) shows a
   dialog saying why, rather than opening a gizmo that would cut the wrong thing.
5. Opens the Cut gizmo (close-then-open, the Emboss/SVG two-step, because `open_gizmo()` toggles).

### 4.3 The re-edit session

`arm_reedit()` **parks** the recipe rather than applying it: `open_gizmo()` runs `on_set_state()`,
which flattens every piece of session state, so a recipe applied before that would be wiped.
`begin_reedit()` is called at the end of `on_set_state()`'s `On` branch, after the reset.

`begin_reedit()`:

- Removes the halves from the model and adds a **stand-in** object carrying the recipe's pre-cut
  mesh, at the first surviving half's instance transform. Removing rather than merely hiding is
  what lets the selection — and so every raycaster, the clipper and the cut itself — address the
  pre-cut mesh as an ordinary object, so the **whole gizmo works unchanged** during a re-edit.
- Puts the recipe's connector definitions on the stand-in.
- Selects it, then `apply_recipe_to_gizmo()`.

`apply_recipe_to_gizmo()` restores the mode, the plane (object frame + instance offset), the
surface, the parameters and the connectors. Two things worth noting:

- **The curved fit is suppressed** (`m_curved_res_user_set`, `m_curved_fit_valid`,
  `m_curved_fit_center/rotation` pre-seeded). `fit_curved_sheet_to_section()` would resize the
  sheet's domain and resample the values the recipe just restored — a *different* surface from the
  one the halves were cut with.
- **The drawn stroke is rebuilt from raw samples and re-`finish()`ed** with the recipe's own
  smoothing and closed flag, which is what makes the reproduced path identical to the one that was
  cut with.

**Cancel** (closing the gizmo without cutting) → `cancel_reedit()` → `plater->undo()`, which rolls
back to before `begin_reedit()`. The halves come back and the stand-in goes, in one step — and
because it is the *same* snapshot, the user's own Ctrl+Z does exactly the same thing (design
point 2).

**Perform cut** in a re-edit: `perform_cut()` clears `m_reedit_active` *before* closing the gizmo
(so the committing path does not reach `cancel_reedit()`), cuts the stand-in normally, and then
removes the old halves by **ObjectID** — after `apply_cut_object_to_model()`, because every index is
invalidated by it, and by id because indices are meaningless across it.

**One half deleted.** `begin_reedit()` notices when fewer objects than expected survive and sets
`m_reedit_one_half_missing`; the panel warns "One half of this cut is missing from the project.
Cutting again will produce both halves." The re-cut still runs and produces both.

### 4.4 Undo / redo

The gizmo-local sheet and stroke stacks (`m_curved_undo` / `m_draw_undo`) are **seeded** from the
recipe in `apply_recipe_to_gizmo()`: `clear_*_undo()` then `push_*_undo()`, so the first Ctrl+Z in a
re-edit returns to the surface as loaded rather than to a flat sheet or an empty stroke (design
point 5).

The plater's own undo covers the whole re-edit as one snapshot, per §4.2 step 3.

---

## 5. Tests

`tests/libslic3r/test_cut_recipe.cpp`, tag `[CutRecipe]`.

| Test | What it proves |
|---|---|
| the pre-cut mesh blob round trips exactly | Vertices bit-for-bit and facet indexing unchanged — not "close". An STL round trip would fail this. Plus: the hash is a function of content. |
| a corrupt mesh blob is refused rather than trusted | Truncated, bad magic, empty, and **a facet index past the end of the vertex array**. |
| a cut recipe survives a 3MF round trip for every surface kind | Plane / Curved / Drawn / Groove, each with a fully populated recipe, through `store_bbs_3mf` + `load_bbs_3mf`. `operator==` compares every stored field; the mesh is compared vertex by vertex on top. Also asserts **one** blob in the archive for two halves. |
| a cut re-performed from a loaded recipe reproduces the same halves | Cut a cube (plane, and curved with a bent sheet), record each half's volume and bbox; put the recipe through a real 3MF; cut again from what came back; compare within 1e-6. **This is the feature's actual promise** — the others only show the bytes survive, this shows they are enough. |
| with no recipe nothing is written | The opt-out: no `cut_recipe.xml`, no blobs, objects still `is_cut()`. |
| a recipe with no stored mesh is not offered for editing | `valid()` is false with no mesh, with an unknown version, and with a sheet whose value count does not match its grid. |

Run: `libslic3r_tests.exe "[CutRecipe]"`. `[CurvedCut]` and `[DrawCut]` are run alongside to show
nothing regressed.

Set `SNORCA_RECIPE_KEEP=1` to keep the generated 3MFs for inspection.

---

## 6. Click-tests

Manual, in the built GUI. Not yet executed — see §8.

1. **Round trip.** Load a cube. Cut gizmo → move the plane off centre → Perform cut. Save the
   project. Reopen it. Right-click either half → **Edit cut…**. The gizmo opens showing the *whole
   uncut cube* with the plane where it was. Drag the plane 5 mm. Perform cut. → Two new halves at
   the new position; the old two are gone; the object count is unchanged.
2. **Connectors survive.** Same, but place two Plug connectors before the first cut. On Edit cut…,
   both connectors are back on the plane in their original positions. Re-cut → the connectors are
   in the new halves.
3. **Curved.** Cut with a bent sheet. Edit cut… → the sheet comes back bent, with the same control
   grid, and the handles are where they were. Ctrl+Z once → returns to the loaded sheet, not to a
   flat one.
4. **Drawn.** Cut with a painted stroke. Edit cut… → the stroke is back on the model with the same
   extension/depth/angle. Edit a point, re-cut.
5. **Keep-cut-editable OFF.** Uncheck it, cut, save, reopen → "Edit cut…" does not appear in the
   right-click menu, and the 3MF has no `Metadata/cut_recipe*` entries (check with any zip tool).
6. **Cancel.** Edit cut… → move the plane → close the gizmo without cutting. Both halves are back,
   unchanged, and the stand-in is gone.
7. **Undo.** Edit cut… → re-cut → Ctrl+Z once. The two *original* halves are back.
8. **One half deleted.** Cut, delete one half, Edit cut… on the survivor. The panel warns that one
   half is missing; re-cut produces both.
9. **Upstream compatibility.** See §7.

---

## 7. Upstream compatibility

The design intent is that a 3MF written by this build opens in upstream OrcaSlicer / BambuStudio
without errors, with the two cut halves present and the recipe simply ignored.

**Why it should hold.** Neither `Metadata/cut_recipe.xml` nor `Metadata/cut_recipe/*.bin` is
referenced from the main `<resources>` model: no `<object>`, no `<component>`, no relationship
entry. An upstream reader walking the archive hits them in its `else if` chain of known
`Metadata/` names, matches none, and falls through — the same fate any unknown `Metadata/` entry
already has. This is exactly the contract `Metadata/image_fill/` relies on in this fork today.

**Limitation — this has NOT been verified against an upstream binary.** No upstream OrcaSlicer or
BambuStudio build is installed on this machine, and the brief forbids touching the owner's live
install. The claim above is from reading upstream's archive-walk structure (which this fork's
`_load_model_from_file` is a direct descendant of), not from loading a file. **Before merging, load
a 3MF written by this build in a real upstream OrcaSlicer and confirm it opens without warnings.**
The reference clone at `C:\Dev\BambuStudio` (noted in memory for the H2C 3MF schema work) is the
obvious place to build one.

---

## 8. Deviations and what is left

1. **§7 is unverified.** As above.
2. **Click-tests are unexecuted.** The GUI was not launched: `libslic3r_gui` was built to prove
   the gizmo, menu and object-list code compiles, but driving the app was out of scope for this
   pass. §6 is the script to run.
3. **`Groove` is persisted but not re-editable end to end.** The recipe carries the groove
   parameters and round trips them, and `apply_recipe_to_gizmo()` restores them, but a
   tongue-and-groove cut's `perform_with_groove()` path was not exercised by the re-cut test —
   only Plane and Curved were. The groove's own `m_groove_editing` interaction with a re-edit is
   untested.
4. **Dowel objects carry no recipe.** `perform_cut()` attaches the recipe only to objects with
   `is_cut()`, which excludes generated dowels. Re-editing "from a dowel" is meaningless, so this
   is deliberate, but it means a project whose halves were both deleted and whose dowels remain has
   no way back.
5. **The recipe is not carried through `ModelObject::merge()` / `split()`.** It is copied by
   `assign_copy` and cleared by `invalidate_cut()`, which covers the paths that matter, but an
   object that has been split after a cut keeps a recipe describing a mesh it no longer is. The
   `valid()` gate does not catch that — the mesh is still there, it just no longer corresponds.
   Re-cutting would replace the split parts with the original two halves. Worth a follow-up.
6. **Storage cost is as advertised and not mitigated.** No de-duplication across *different* cuts
   of the same source mesh (each cut stores its own pre-cut state, which is correct), and no
   quantisation of the stored vertices. A 3MF of a cut object is roughly twice the size.
