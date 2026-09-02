# Task 10 report — GUI: Plater action, dialog, job, menu

Commit: `b2945879f4` — `feat(color-split): Split by painted colour action, dialog and job`
Branch: `feat/color-split` in `C:\Dev\SnapmakerOrcaNext` (base `3516081b4f`)

## What was implemented

### 1. `src/slic3r/GUI/Jobs/ColorSplitJob.hpp/.cpp`

`class ColorSplitJob : public Job` with a `Target` struct per painted `MODEL_PART`.

`process(Ctl&)`
- Loops the targets, calling `split_volume_by_paint(t.mesh, t.paint, t.depths, t.params, progress, t.space.to_split)`.
  Mesh and paint are always mesh-space copies (Ruling 23); `to_split` is passed through.
- Progress `(100 * i + percent) / n`; the callback returns `!ctl.was_canceled()`.
- `ColorSplitCancelled` → return immediately. Any other `std::exception` (including `ColorSplitError` and
  whatever Manifold/CGAL throws) is recorded in `t.error` and the remaining targets still run (spec 7).
- Extra cancel check at the top of each iteration so a cancel between targets does not start the next one.
- The mesh/paint copies are released after each target (they can be large).

`finalize(bool canceled, std::exception_ptr&)`
- Returns immediately on cancel or a leftover exception (`PlaterWorker` surfaces the latter).
- Re-finds object + volume by `ObjectID` **per target** and aborts that target with a notification when
  `mmu_segmentation_facets.timestamp()` differs, or when the mesh's vertex/index counts differ (cheap
  "the mesh was replaced" guard), or when the ids are gone. Per Task 7's note, nothing is cached across
  `apply_color_split` — every target is looked up afresh.
- `Plater::TakeSnapshot("Split by painted colour")` is taken **lazily**, immediately before the first
  `apply_color_split`, so a run in which every target fails leaves no empty undo step.
- `apply_color_split(*object, vol_idx, std::move(t.result), t.space, m_solid_interfaces, m_keep_base_sparse_infill)`.
- After every target has been applied, one list rebuild per touched object:
  `add_volumes_to_object_in_list(obj_idx, predicate)` (the predicate picks the created volumes and returns their
  `wxDataViewItem`s), `input_file.clear()`, `changed_object(int)`, `notify_instance_updated(int)`,
  `update_info_items(size_t)`, then `select_items(...)` for the new parts.
  Doing the rebuild once at the end (rather than per target) matters: an earlier rebuild's `wxDataViewItem`s
  would be stale after the next target replaced volumes in the same object.
- Warnings → `wxGetApp().notification_manager()->push_plater_warning_notification(...)`;
  errors → `show_error(m_plater, from_u8(...))`.

### 2. `src/slic3r/GUI/ColorSplitDialog.hpp/.cpp`

`class ColorSplitDialog : public DPIDialog` (= `DPIAware<wxDialog>`), title "Split by painted colour".
- Summary in **world** mm: Filaments, Triangles, Computed depth (or "unlimited …"), Wall stack,
  Flat caps (top / bottom), Layer height.
- `wxTextCtrl` "Depth override (mm)", empty = computed (hint text "computed").
- Checkboxes: "Unlimited depth" (initialised from the computed `ColorSplitDepths::unlimited`),
  "Cap flat tops/bottoms at solid shell depth" (on), "Absorb enclosed islands" (on),
  "Keep base-colour sparse infill" (default `!paint_infill_override`),
  "Solid colour interfaces (interface_shells)" (on).
- Ticking "Unlimited depth" disables the override field (the library ignores the flag when an override is
  given — `ColorSplit.cpp:128`).
- `CreateStdDialogButtonSizer(wxOK | wxCANCEL)`; the OK button is intercepted and `validate()` runs first:
  a non-empty override must parse as a positive number, and unticking "Unlimited depth" when the print
  settings computed no depth at all (`paint_depth_mode = unlimited` → `D = 0`, `PaintDepth.cpp:12-13`) is
  refused with an explanation. `wxGetApp().UpdateDlgDarkUI(this)` at the end.
- Getters: `params()` (`ColorSplitParams`), `unlimited()`, `solid_interfaces()`, `keep_base_sparse_infill()`.
  `crease_step` is hard-wired `true` (spec 3.6 — no user knob was specified).

### 3. `Plater::split_by_color()` / `Plater::can_split_by_color()`

`Plater.hpp:735` (next to `split_volume()`) and `Plater.hpp:767` (next to `can_split(bool)`);
implementations in `Plater.cpp` just before `Plater::optimize_rotation()` and just after
`Plater::can_split(bool)`.

- Refuses with a plater warning notification while a painting gizmo is open
  (`canvas3D()->get_gizmos_manager().get_current_type()` in `{FdmSupports, Seam, FuzzySkin, MmSegmentation}`)
  and while `get_ui_job_worker().is_idle()` is false.
- Effective config: `wxGetApp().preset_bundle->full_config()` → `apply(object.config.get(), true)` →
  per volume `apply(v->config.get(), true)`.
- Per painted `MODEL_PART`: `color_split_space(object, *v)`, `color_split_depths(cfg, v->get_extruders())`,
  and on the **mesh-space** path `scale_depths(world, space.depth_scale)`; the world path passes through.
  `color_split_space`/`color_split_depths` throwing `ColorSplitError` is caught and shown via `show_error`
  before anything is queued.
- The dialog is shown with the **world** depths, the union of the painted filaments (sorted) and the total
  triangle count. On OK, `scale_params(params, depth_scale)` on the mesh-space path (the world path passes
  through), `t.depths.unlimited = dlg.unlimited()`, and
  `replace_job(worker, std::make_unique<ColorSplitJob>(...))`.

### 4. Menus — `src/slic3r/GUI/GUI_Factories.cpp`

"By painted colour" added to the Split submenu in all four blocks and "Split by painted colour" as a sibling
in the plain part menu:

| Location | Function | Item |
|---|---|---|
| :1434-1436 | `create_object_menu()` | submenu "By painted colour" |
| :1475-1477 | `create_extra_object_menu()` | submenu "By painted colour" |
| :1549-1551 | `create_part_menu()` | sibling "Split by painted colour" |
| :1614-1616 | part submenu (`menu`-local `split_menu`) | submenu "By painted colour" |
| :2012-2014 | multi-selection menu | submenu "By painted colour" |

Each item is enabled by `plater()->can_split_by_color()`; every enclosing `append_submenu` enable lambda now
also ORs in `can_split_by_color()` so the Split submenu is reachable on a painted object that is not
otherwise splittable.

### 5. `src/slic3r/CMakeLists.txt`

`GUI/ColorSplitDialog.cpp/.hpp` (:81-82) and `GUI/Jobs/ColorSplitJob.cpp/.hpp` (:263-264).

## Where I deviated from the brief's sketch (and why)

| Brief sketch | What I did | Why |
|---|---|---|
| `ColorSplitJob(Plater*, targets, ColorSplitParams params, bool, bool)` | dropped the `params` argument | The sketch never read the member — every target already carries its own space-scaled `params`. |
| `Target::paint_timestamp` typed `uint64_t` | `ObjectBase::Timestamp` | `Timestamp` is a member typedef of `ObjectBase`, not a free typedef in `Slic3r`. |
| `Plater::TakeSnapshot` at the top of `finalize` | `std::optional<Plater::TakeSnapshot>`, emplaced before the first apply | An all-failed run must not push an empty undo step. |
| list refresh inside the per-target loop | one refresh per touched object after the loop | Items from an earlier rebuild go stale when the next target of the same object replaces volumes. |
| `wxGetApp().notification_manager()` / plain `_u8L` strings | same, plus `boost::format` for the messages with a part name | Matches the surrounding GUI code (`Auxiliary.cpp:378` etc.). |
| paint-timestamp check only | timestamp **and** vertex/index counts | Spec 5 says "if the paint timestamp **or mesh** changed"; the counts are the cheap mesh guard. |
| — (not in the sketch) | mesh/paint copies released per target; cancel check at the loop top; `validate()` on OK | Self-review additions, see below. |
| plain `wxDialog` | `DPIDialog` (`DPIAware<wxDialog>`) with plain `wxCheckBox`/`wxTextCtrl` + `CreateStdDialogButtonSizer` | `DPIDialog` is the fork's dialog base and gives `FromDIP`; the widgets stay plain wx as the brief asked. |
| "Split by painted colour" refusal path for a non-watertight mesh done before the job | left to the job's error path | `extract_color_patches` (`ColorSplit.cpp:30`) is the only place that knows, and it runs inside the split. The user-visible outcome is identical: `show_error` with "The part is not watertight; repair it before splitting by colour." and no model change. |

Everything else in the brief matched the real API exactly (`Plater::TakeSnapshot(Plater*, const std::string&)`,
`ObjectList::changed_object(int) const`, `show_error(wxWindow*, const wxString&)`, `ModelObject::input_file`,
`GLGizmosManager::EType`, `append_menu_item`'s 7th parameter being the `wxEvtHandler*`).

## Build

```
cmd /c C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad\build_next_wt.bat
  (cmake --build . --config Release --target ALL_BUILD, from C:\Dev\SnapmakerOrcaNext\build)
```

Two runs (build slot verified free before each — `Get-Process cl,link,MSBuild` empty):

| Run | Log | Exit | `error C####` / `error LNK` | Warnings in my files |
|---|---|---|---|---|
| full (first compile of the new sources) | `scratchpad/build_task10.log` | **0** | 0 | none |
| final (after the two self-review fixes to `ColorSplitJob.cpp`) | `scratchpad/build_task10b.log` | **0** | 0 | none |

`grep -Ei "warning" <log> | grep -Ei "ColorSplit|Plater\.cpp|GUI_Factories"` is empty in both logs.
`build/src/Release/snapmaker-orca.exe` relinked at 09:23:12; `build/tests/libslic3r/Release/libslic3r_tests.exe`
was rebuilt in the first run (unchanged by the second, which touched GUI sources only).

## Tests

```
build\tests\libslic3r\Release\libslic3r_tests.exe "[colorsplit]"
  All tests passed (912 assertions in 56 test cases)
build\tests\libslic3r\Release\libslic3r_tests.exe "[paintdepth]"
  All tests passed (1568 assertions in 94 test cases)
```

(The `[colorsplit]` e2e case prints its usual informational parity WARN — it is a `WARN`, not a failure.)

## Files changed

Created:
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\Jobs\ColorSplitJob.hpp`
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\Jobs\ColorSplitJob.cpp`
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\ColorSplitDialog.hpp`
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\ColorSplitDialog.cpp`

Modified:
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\Plater.hpp` (2 declarations)
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\Plater.cpp` (3 includes + 2 implementations)
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\GUI\GUI_Factories.cpp` (5 menu items + 4 submenu enable lambdas)
- `C:\Dev\SnapmakerOrcaNext\src\slic3r\CMakeLists.txt` (4 source lines)

`.superpowers/`, the untracked worktree junk (`one_vertex_touch.svg`, `twospheres.obj`, `spike/out/*.gcode`)
and all build artefacts were left out of the commit.

## Self-review findings (fixed before the final build)

1. **Cancel between targets.** `process` only noticed a cancel through the progress callback. With several
   painted parts a cancel arriving between two targets would start the next split. Added
   `if (ctl.was_canceled()) return;` at the top of each iteration. (Behaviourally this was already safe —
   `finalize` discards everything on cancel — but it wasted work.)
2. **Warnings from a discarded split.** `t.result.warnings` were pushed before the id/timestamp guards, so a
   target that was abandoned because the paint changed would still report "component skipped for filament 2"
   alongside "it was left as it was". The push now happens after that guard (but still before the
   "produced no separate part" case, where the notes are the explanation).

Both fixes are in `ColorSplitJob.cpp` and are covered by the final build.

Checked and found correct: menu items in all five places; enablement lambdas; gizmo and worker-idle guards;
every dialog option wired through to `ColorSplitParams` / `apply_color_split`; progress bounded 0..100;
`finalize` re-lookup by ObjectID with no pointer cached across `apply_color_split`; undo snapshot before the
first model change; no UI call from `process` (all wx work is in `finalize`, which runs on the main thread);
`m_plater` is the only pointer the job holds and the Plater outlives every job.

## Concerns

1. **No GUI test coverage exists**, so nothing here has been exercised at runtime — the acceptance is the
   build plus the unchanged libslic3r suites. The GUI round below is the real verification.
2. **`create_part_menu()` appends "Split" twice** (pre-existing: `menu` and `&m_part_menu` are the same
   object, and both halves of the function append the item). I added my item once, after the first "Split".
   The duplicate is not mine to fix in this task, but the part menu will show two "Split" entries and one
   "Split by painted colour".
3. **Watertightness is reported after the job starts**, not before it (see the deviations table). The user
   sees the busy cursor and a progress notification for a moment before the error dialog.
4. **`changed_object` calls `ensure_on_bed`**, so an object floating above the bed will drop when the split is
   applied. That is exactly what `ObjectList::split()` does, so it matches the existing "Split to parts"
   behaviour, but it is a visible side effect worth confirming in the GUI round.
5. **Multi-selection menu**: the item is enabled only when `Selection::get_object_idx()` resolves to one
   object, so selecting two painted objects greys it out. The action is single-object by design (spec 5 says
   several painted *parts* of one object are split one after another).

## GUI round — instructions for the user

Binary: `C:\Dev\SnapmakerOrcaNext\build\src\Release\snapmaker-orca.exe`

1. **Happy path.** Open (or build) a project with a multi-filament printer selected, paint a face of an object
   with filament 2 using the colour-painting gizmo, then close the gizmo.
   Right-click the object (3D view or the object list) → **Split → By painted colour**.
   Expect the dialog titled "Split by painted colour" showing the filaments, the triangle count, and the
   computed depth / wall stack / flat caps / layer height in mm. Accept the defaults → OK.
   Expect a brief progress notification, then in the object list: the object now holds `<name>` (the body)
   plus `<name> F2`, both MODEL_PARTs, with the filament chips set to 1 and 2, and the new parts selected.
   The painted-colour info item under the object should be gone (there is no paint left).
2. **Slice and check the depth.** Slice and step through the preview. The `F2` part should appear as a real
   solid region of filament 2 about as deep as the "Computed depth" the dialog showed — cross-check against
   the same model *before* splitting (the 2D paint-depth claim) by undoing first.
3. **Undo.** Ctrl+Z once. Expect the single painted part back, with its paint intact, and the object list
   restored. Ctrl+Y should redo the split.
4. **Gizmo guard.** Open the colour-painting gizmo, then use the menu item (from the object list, which stays
   reachable). Expect the plater warning "Close the painting tool before splitting by colour." and no change.
5. **Depth override / unlimited.** Repeat step 1 but type e.g. `2.5` into "Depth override (mm)" — the parts
   should be visibly deeper. Then repeat with "Unlimited depth" ticked (note the override field greys out):
   the painted colour should go all the way through the part.
   Type `abc` or `-1` into the override to confirm the dialog refuses to close with an explanation.
6. **Options.** Untick "Solid colour interfaces" and confirm the object's `interface_shells` is *not* set
   (Object settings). Untick "Keep base-colour sparse infill" and confirm the colour part's
   `sparse_infill_filament` is not pinned to the body's filament.
7. **Several painted parts.** An object with two painted MODEL_PARTs should split both in one action, with
   one undo step covering both.
8. **3MF round-trip.** Save the split project as 3MF, close, reopen. The body + `F<n>` parts and their
   filament assignments must come back, and slicing must produce the same result.
9. **Busy guard (optional).** Start an arrange or another long job and immediately trigger the split — expect
   "Another operation is still running; try splitting by colour again when it finishes."

Report anything that differs, especially: a part that lands in the wrong place after the split (a transform
bug in the space handling), a colour depth that does not match the dialog's number, or an undo that does not
restore the paint.
