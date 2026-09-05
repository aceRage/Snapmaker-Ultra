# Support sets and per-part support groups — phased implementation plan

Date: 2026-09-02 · Branch: `feat/ultra-preferences` @ `85041c4460` · Status: draft for review
Implements: `docs/superpowers/specs/2026-09-02-per-part-supports-research.md` (option **b′**, first slice **d**)
Read-only research pass; every file:line below was re-read in this tree on this date and will drift.

> The research doc was written at `06e7d39b7d`; the tree has moved. Where this plan and the research
> disagree on a line number, this plan's number is the one that was verified.

## 1. Goal

Two features, one spine.

1. **Support sets** — reusable, named snippets of the Support-category process settings, saved as JSON
   next to the user's presets. Not a preset type: applying a set copies its values into the current
   process settings, which then show as modified exactly like a hand edit. Sets are portable across
   projects and printers because the interface filament travels as a *type* ("same as part", "soluble",
   or an explicit filament type string) and is resolved to a slot at apply time.
2. **Named support groups per object** — any `MODEL_PART` volume can be assigned to a named group; a
   group references a support set and the **resolved** values are written onto the parts' own
   `ModelVolume::config`, so a 3MF stays self-contained and reopens anywhere (including in stock Orca,
   which drops the unknown keys silently). Ungrouped parts fall through to the object's own Support
   settings — the *default group*. The support generator then produces a different interface under
   each group.

Not in scope: per-part support **base** geometry (style, XY distance, expansion, base pattern), mixing
normal and tree supports inside one object, per-part support *base* filament, per-part Z gap (see §3.6
for the rule that replaces it), and support modifier volumes.

## 2. Stages and gates

Each stage builds, ships and is testable on its own. Do not start the next stage until its gate holds.

| Stage | Delivers | Gate (must all pass) |
|---|---|---|
| 1 — Support sets | `SupportSet` model + on-disk store under `<datadir>/user/<id>/support_set/`, "Support set" row on the process tab's Support page (pick / Apply / Save current as… / Rename / Delete), interface-filament type resolution | `test_support_set.cpp` passes (JSON round trip, name sanitising, enumeration, filament-type resolution incl. every fallback); Apply marks the process preset modified and the changed values show in the tab; **no back-end change at all** → the byte-identity script (§3.7) is green on the corpus with an empty diff, trivially |
| 2 — Groups on parts | `support_group` key, Object-List "Support group ▸" menu on a part and on a multi-selection, the badge, the Support-groups panel, part-level support keys in the settings menu/part tab, the `is_improper_category` fix, 3MF round trip, invalidation plumbing | `test_3mf.cpp` group round trip passes; `test_support_groups.cpp` group-resolution tests pass (K==1 for equal overrides); assigning a group + re-slice invalidates only `posSupportMaterial` (or `posSlice` when §3.6 flips); **byte identity holds for every project in the corpus, including one that carries groups** — the generator still ignores them |
| 3 — Interface groups, normal supports **(done, §2c)** | per-group masks, per-group `SupportParameters`, per-group `generate_interface_layers`, per-group interface toolpaths + interface filament | K==1 corpus within tolerance (§3.7 as replaced by §2b's gate); the two-part fixture in `test_support_material.cpp` shows different interface extrusion length above part B and unchanged above part A; a soluble-interface group emits its own extruder via `interface_by_extruder`; support generation time within 1.6× of baseline at K=2 on the corpus. Stage 3 added a third gate — `--gate groups` — for the ON-mode cases, which must differ; see §2c |
| 4 — Tree parity | organic tree: full parity through the same seam; classic tree: per-group interface *fill* only (documented limit) | K==1 corpus byte-identical with `support_type = tree(auto)` and both styles; organic-tree two-part fixture behaves like Stage 3's; classic tree shows per-group roof pattern/spacing/filament and an explicit "interface layer count is object-wide for classic tree supports" notice |
| 5 — Polish | preview colouring by filament check, empty-group and conflict warnings, docs, `PART_CATEGORY_SETTINGS` "Support" group, tooltip copy | Preview colours the per-group interface with its resolved filament; a group with no parts, a group whose set no longer exists, and a group whose interface filament type cannot be resolved each raise a visible, non-fatal warning; `docs/superpowers/specs/` page written |

## 2a. Stage 1 status (2026-09-03): DONE, gate held

Shipped on `feat/support-sets-stage1` (cut from `feat/ultra-preferences` @ `d6e020124a`): the
`SupportSet` model, the `<datadir>/user/<preset_folder>/support_set/*.json` store with the
read-only `default` fallback of §5 item 5, the pure helpers (`support_set_keys`,
`support_set_from_config`, `support_set_apply_to`, `resolve_interface_filament`,
`support_set_interface_filament_type`, `sanitize_support_set_filename`, the JSON round trip); a
"Support set" opt-group at the top of the process tab's Support page carrying a combo of the saved
sets plus Apply / "Save current as…" / Rename / Delete and a note line for the
filament-resolution message; `tests/libslic3r/test_support_set.cpp`. **No back-end change**: no
file that participates in slicing was touched, so §3.7's byte identity holds by construction.

Deviations from the plan, all deliberate:

1. **The code lives in `src/libslic3r/SupportSet.{hpp,cpp}`, not `src/slic3r/GUI/`** (§1.1). The
   plan's own gate note sanctions this ("move the pure helpers into `src/libslic3r/SupportSet.cpp`
   if the link is awkward. Prefer the latter if it comes up") and it did come up:
   `libslic3r_tests` links `test_common libslic3r OpenSSL::Crypto` and nothing wx, so a
   `slic3r/GUI` TU cannot join it. Every helper *and* the store turn out to be wx-free —
   `data_dir()`, `AppConfig` and `PresetBundle` are all libslic3r — so the only GUI-side input is
   which per-account preset folder is live, handed over by `SupportSetStore::set_preset_folder()`.
   Stage 2 gets the same names, in `Slic3r::` rather than `Slic3r::GUI::`.
2. **`to_json` / `from_json` are named `support_set_to_json` / `support_set_from_json`**, to keep
   them out of nlohmann's ADL serialisation lookup. String forms
   (`support_set_{to,from}_json_string`) are what the store and the tests use; the header takes
   only `nlohmann/json_fwd.hpp`.
3. **The excluded-key list grew by seven.** The plan's mechanical rule (a `PrintObjectConfig`
   member whose `print_config_def` category is `"Support"`) admits `brim_width`, `brim_type`,
   `brim_object_gap`, `brim_ears_max_angle`, `brim_ears_detection_length`, `bridge_no_support`
   and `max_bridge_length` — 54 such keys in this tree, not the 58 the plan counted. Applying a
   support set must not silently rewrite the user's brim or bridging, so those seven joined
   `support_set_excluded_keys()` next to the filament slots, `raft_*`, `enforce_support_layers`
   and `independent_support_layer_height` (which is not a `PrintObjectConfig` member here, so it
   is belt-and-braces). **39 keys remain.** The plan designates that list as the place such
   adjustments live, so Stages 2 and 5 must read it from there rather than recomputing the rule.
4. **`SupportSet::read_only` was added to the struct.** A set that came from the `default`
   fallback while another account is logged in can be applied but not renamed or deleted, and
   saving over it writes a shadowing copy into the account's own folder rather than editing the
   shared one. The combo shows those as "*(shared)*".
5. **Enumeration is not driven from `update()`.** The folder is scanned when the Support page is
   built and when it *becomes* the active page, never from `TabPrint::update()` — `update()` runs
   inside settings-change scopes, a phone `POST /api/settings/process` among them, and "Things
   that must NOT change" forbids blocking file I/O there.

Gate: `libslic3r_tests "[SupportSet]"` — 411 assertions in 8 test cases, all pass; the whole
`libslic3r_tests` suite still passes (592 cases, 53 023 assertions, the 2 pre-existing
`[!shouldfail]` cases unchanged). `build_main.bat` clean (0 `error C`, 0 `error LNK`). Runtime
gate `test_support_sets.py` (snorca_hubtest) against a side install on an isolated data dir: the
Support page's 51 option keys are unchanged in the same order, the widget-only group correctly
does **not** reach the remote API or the phone Settings sheet, no `support_set` folder is created
until the user saves one, and `POST /api/settings/process` + `/revert` still behave.
`test_security.py` and `test_hidden.py` re-run on the new build.

Findings:

1. **A widget-only opt-group is invisible to the remote API by design.** `api_process_settings`
   (`src/slic3r/GUI/RemoteAccess.cpp:1324-1328`) drops lines with no config options and then
   groups with no lines, and the phone page applies the same filter
   (`resources/web/orca/stream_center.html:1426`). So the Support-set row cannot be asserted
   through the API — and, more usefully, it cannot leak into the phone Settings sheet either.
   The row itself is only constructed when the Support page is first activated
   (`OptionsGroup::activate` returns early once `sizer` exists), and no API route activates a
   page, so **clicking it is manual-checklist work**; the automated gate proves only that the row
   is compiled in and that nothing else on the page moved.
2. **`Utils::iso_utc_timestamp()` is the basic ISO form** (`%Y%m%dT%H%M%SZ`,
   `src/libslic3r/Time.cpp:24`), not the extended form the plan's JSON example shows.
   `SupportSet::created` is written with a local `strftime` so the on-disk shape matches the plan;
   it is informational metadata and is never parsed back.
3. **`ConfigOptionBools{ 0, 1, 0 }` is ambiguous** between `initializer_list<bool>` and
   `initializer_list<unsigned char>`; tests must spell `std::vector<unsigned char>{…}`.
4. **The store creates nothing until a set is saved.** `reload()` returns early when the
   directory is absent, so an existing profile tree is untouched by the feature until the user
   uses it — worth keeping when Stage 2 adds its own reads.

Not done in this stage: `scripts/support_group_identity.py` and `tests/data/support_corpus/`.
The plan's task table puts them on the separate `feat/support-identity-harness` branch (T3), and
Stage 1's gate calls them green "trivially"; that claim holds here by construction (no slicing
file changed) but the script does not exist yet, so **T3 remains a prerequisite for Stage 2**.

## 2b. Stage 2 status (2026-09-03): DONE — T3-T7 shipped, tolerance gate green

> **Hardware pass (2026-09-04):** the user ran the manual checklist on the live build. Steps 1-3 passed
> on the first install; step 4 (reopen) crashed, traced to the #560 port's plate-name icon (see
> `fix/support-group-reopen`, merged as 36cb3d3757), and the whole list passed on the fixed build.


Stage 2 first stopped at T3 because the plan's §5.1 experiment showed byte identity is
unachievable on this tree. The reviewer chose option 2 from that finding: **§3.7's byte identity is
replaced by a tolerance gate built on the fork's own Slice Compare engine**, and the
nondeterminism itself is being chased separately on `fix/slice-determinism`. This section records
the new gate, its calibration, and what Stage 2 delivered.

### The gate (plan deviation, §3.7)

`scripts/support_group_identity.py --gate tolerance` (the default) runs each pair of G-code files
through `tests/slice_compare_cli`, a **test-only** executable that calls
`SliceCompare::load_snapshot_from_file` and the `diff_*` functions directly. No hook was added to
the shipped application - the engine was already headless libslic3r - and the tool is never
installed. `--gate bytes` keeps the original comparison for the day the determinism fix lands.

**Criteria:** zero changed config rows, zero changed layers, zero layers present on only one side,
and at least `--segment-tolerance` per cent of segments matching (default 99.0).

The plan asked for segments at 100 %. **The baseline cannot meet that against itself.** Measured
on the untouched tree, one executable on both sides:

| measurement | result |
|---|---|
| structural criteria (config rows, changed layers, one-sided layers) | stable at zero on every case, every run |
| segments, baseline vs baseline | roughly one case per run loses 10-18 segments of several thousand, on 4-7 layers of a 41-66 layer print (99.4-99.95 %) |
| `tree_classic`, baseline vs baseline | 6.6 % matching segments, 8 changed layers, 49 dirty layers - **every run** |

So the structural criteria are required exactly and the segment count is thresholded. A
support-group regression would move layers or a large fraction of segments, not 0.4 % of them.
`tree_classic` was **removed from the corpus**: classic tree support is not self-reproducible at
all, while organic tree, normal supports and rafts are all stable through this gate. §5.1's own
instruction is to drop such a case and say so.

**Gate result: green after every task.** The baseline is a build of the branch's pre-T4 commit
(`aa5bda6932`), installed to a scratch prefix; the candidate is each task's build, likewise
installed to a scratch prefix - the user's own `build/Snapmaker_Orca` tree is never written to.

| after | corpus | passes | result |
|---|---|---|---|
| T4 | 8 cases | 3 | all within tolerance |
| T6 | 8 cases | 3 | all within tolerance |
| T7 | 8 cases | 3 | all within tolerance |
| T5 | 9 cases (incl. `group_parts`) | 3 | all within tolerance |

The `group_parts` case is the one the plan's gate item 4 asks for: **a project whose parts carry
`support_group` and part-level support keys, sliced identically by a baseline that does not even
know the key** (it drops it on load, §3.1) **and by the new build, which knows it and ignores it.**
That is the whole off-mode claim, measured rather than asserted.

### What shipped

- **T3** - the harness and corpus, plus `tests/slice_compare_cli`. Corpus cases: normal grid /
  snug / ledge, a dense interface, a soluble interface, organic tree, a raft, a no-support
  control, and (T5) `group_parts`.
- **T4** - the `support_group` key (in `print_config_def`, a member of no static config class and
  not a print option, so it never reaches the process preset, the project config or the G-code
  `CONFIG_BLOCK`); `part_support_keys()`; `PrintObject::support_groups()`; the soluble rule of
  §3.6 as one line in `object_config_from_model_object`; and
  `model_support_group_data_changed()` extending the `PrintApply` condition.
  `tests/fff_print/test_support_groups.cpp` - 8 cases, 44 assertions.
- **T6** - `is_improper_category` made key-aware at both call sites,
  `SettingsFactory::get_options` / `part_support_keys` / `is_part_support_key` /
  `part_support_group`, a `"Support"` group in `PART_CATEGORY_SETTINGS`, the Object-List
  **"Support group ▸"** submenu on a part and on a multi-part selection, `TabPrintPart` widened to
  render the curated keys, and the group badge in the object list's name column.
- **T7** - `src/slic3r/GUI/SupportGroupsDialog.{hpp,cpp}`: the non-modal per-object Support-groups
  window (groups, their part counts, the resolved tier values, the support set they reference;
  New / Rename / Delete / Re-apply set / Select parts), opened from the object context menu and
  from the tail of the part submenu.
- **T5** - the project-3MF round trip, **proven** rather than skipped, plus the
  `group_parts` corpus case and `make_group_fixture.py`: see below.

### Deviations

1. **`part_support_keys()` lives in `PrintConfig`, not on `PrintObject`** (§3.5 implied the
   latter). `Model.cpp` needs the list for the invalidation predicate and cannot include
   `Print.hpp`, which includes `Model.hpp`. The list is config metadata, so `PrintConfig` is where
   it belongs; `Model.hpp` already includes `PrintConfig.hpp`. `SettingsFactory::part_support_keys()`
   (§2.2) exists and simply returns it, so the GUI spelling the plan asks for is there with one
   source of truth behind it.
2. **The gate's segment criterion is a threshold, not 100 %** - forced, with the measurements
   above.
3. **Group 0 always exists, so the plan's gate item 4 counts differently.** §3.4 step 4 says the
   default group is always present even when empty of parts, so "two differently named parts with
   identical values" gives `size() == 2` (the empty default plus one shared group), not the
   `size() == 1` that gate line quotes. The substantive claim - identical values collapse
   regardless of the labels, and the survivor takes the first non-empty name - is asserted.
4. **The group name rule applies to group 0 too**, per §3.4 step 3.
5. **No modal text prompt asks for a group name.** §2.2 sketches "New group… opens a small dialog:
   name + a set picker + four knobs". Instead the submenu offers **"New group from support set ▸
   <set>"** (which names the group after the set and writes its *resolved* values) and **"New
   group"** (auto-named `Group`, `Group 2`, …, seeded with the object's own values), and renaming
   happens **in place** in the Support-groups window's name column. The reason is the fork's own
   constraint in "Things that must NOT change": a raw `wxTextEntryDialog` on a path a phone
   request can reach would block a GUI nobody is watching. One click assigns; the name is editable
   afterwards. Every prompt this feature does raise is a `MessageDialog`, whose `ShowModal`
   override auto-answers outside Interactive mode.
6. **A group references its support set by name.** §3.1 allows exactly one new key, so there is no
   second key holding a set id: the group's *label* is the reference, which is why "New group from
   support set" names the group after the set. `SupportGroupsDialog` looks the set up by that name,
   shows it in the "Support set" column, marks the group *modified* when any key the set defines no
   longer matches on the parts, and offers "Re-apply set". A group whose set was renamed or deleted
   simply stops showing one; its values are untouched, because the values live on the parts.
7. **`get_visible_options` / `get_all_visible_options` strip the new part-level Support rows for an
   *object*.** `PART_CATEGORY_SETTINGS` is the *base* both object and part tables are built from, so
   adding a `"Support"` group there would have shown eight rows twice in the object settings table
   and reordered the rest. Objects keep exactly the `OBJECT_CATEGORY_SETTINGS` list they had.
8. **`support_interface_loop_pattern` is stored and resolved but never rendered.** Its
   `append_single_option_line` is commented out on the process tab (`Tab.cpp`, "Advanced" group), so
   `TabPrintModel::build()` has no line to keep for the part panel either. Stage 5 can uncomment it
   in one place and both panels gain it.

### T5: what IS proven now, and what still is not

**The round trip is verified twice: once in a unit test, once by the application itself.**

`tests/libslic3r/test_3mf.cpp` gained two `[3mf][SupportGroups]` scenarios. The first builds a
two-volume `ModelObject`, puts one override on part A and `support_group = "B"` plus four tier A/B
values on part B, and goes through **`store_bbs_3mf` + `load_bbs_3mf`** - the pair the application
uses for a project, not the legacy `store_3mf`, which never writes a volume's `ModelConfig` at all
and so could not have proven anything. It asserts both volumes' configs come back with the same
keys and the same values, that `support_group` and every tier value survived, that nothing leaked
onto part A, and that the mesh is unchanged.

Three obstacles were in the way, and none needed a work-around:

1. `StoreParams::config` is dereferenced unconditionally (`_add_model_config_file_to_archive` takes
   a `const DynamicPrintConfig&`), so it must be a real config rather than `nullptr`. An empty
   `plate_data_list` is fine - every loop over it is size-driven.
2. Without `SaveStrategy::Silence` the exporter writes an `origin.txt` under the model's backup
   path; and that path is rooted at `temporary_dir()`, which the application sets at startup and a
   test must set too, or it resolves to the root of the current drive.
3. **A libslic3r bug, and the real reason the earlier attempt "segfaulted".**
   `ConfigOptionEnumsGenericTempl::set()` copies only the values, never `keys_map`, so a `coEnums`
   member of a **static** config class - `FullPrintConfig`, i.e. what
   `DynamicPrintConfig::full_print_config()` is built from - keeps `keys_map == nullptr`, and
   `serialize_single_value()` dereferences it with no check. `save_to_json()` of a full print
   config therefore crashes on `default_nozzle_volume_type`, and the exporter serialises the whole
   config (`_add_project_config_file_to_archive`). A project config that came from a `PresetBundle`
   has the maps, which is why the application never hits it. The test rebuilds those options from
   `print_config_def` (whose default value carries the map) rather than dropping them. **Worth
   fixing separately, in `Config.hpp`.**

**The corpus fixture is produced by the slicer, and the application is what proves it loads.**
`tests/data/support_corpus/make_group_fixture.py` (new) exports a project from
`twopart_bridge.3mf` with `--export-3mf` - the importer turns that file's three `<component>`s
into **one object with three MODEL_PART volumes**, contrary to the earlier note - injects the
group metadata into one `<part>` of the project the application wrote, and then re-exports the
result and requires every injected key to come back. That last step is the end-to-end proof: the
slicer read the group and wrote it out again.

The case deliberately carries **no `support_top_z_distance`**. That is the one part-level key that
already *acts* in Stage 2, through the soluble rule of §3.6, so a corpus case carrying it would
rightly change the G-code and the gate would rightly go red. `test_support_groups.cpp` covers the
soluble rule instead.

**Stock-Orca degradation is now measured, not just asserted.** The pre-T4 baseline is a build of
this very tree that does not define `support_group` - functionally what stock Orca is for this key.
Run against the fixture:

```
sg_base/snapmaker-orca.exe --export-3mf out.3mf tests/data/support_corpus/twopart_groups.3mf
  support_group                      dropped
  support_interface_top_layers       PRESENT
  support_interface_bottom_layers    PRESENT
  support_interface_spacing          PRESENT
  support_interface_filament         PRESENT
  parts: 3
```

It loads the project, silently drops exactly the key it does not know, keeps every other support
value and all three parts, and saves again - no exception, no crash, geometry untouched. The
mechanism behind it is pinned by the second unit scenario: `PrintConfigDef::handle_legacy` clears
an opt_key it does not recognise, which is how `set_deserialize` drops it. A real stock
Orca / Bambu Studio binary was still not run.

**Undo/redo** is manual-checklist work (below). What the code guarantees: every write goes through
one `take_snapshot`, `ModelConfig::set_key_value` bumps the timestamp the undo stack keys on, and
`ObjectList::update_after_undo_redo` rebuilds the whole tree - where the badge is set as each row
is created, so badge, part panel and `support_groups()` all follow the model back.

### Decisions for the reviewer (the plan's risks)

- **R2.1 - the settings node appears where it did not before.** Accepted. A part carrying group
  values now gets a `"Support"` bundle from `get_bundle`, so `object_config_options_changed` adds a
  settings child under it. That is the intended behaviour, and it also fires for anyone who was
  already forcing support keys onto volumes by hand-editing a 3MF - those keys were silently
  invisible before and are visible now.
- **R2.2 - clipboard paste copies the values but not the label.** Accepted as the plan proposes.
  `paste_settings_into_list` filters against `get_options(true)`, which now contains the curated
  support keys but deliberately not `support_group`. So pasting a grouped part's settings gives the
  target part the same *values* - and the resolver, which keys on values and not on names, folds it
  into that same group, which then keeps its name from its first labelled member. The pasted part
  shows no badge. Coherent, but it must not be an accident: it is documented here and in the code.
- **R2.3 - the dead frequent-settings path.** Left commented out
  (`GUI_Factories.cpp`, the only call site), with a comment saying that re-enabling it must filter
  through `get_options(is_part)` first, because `add_category_to_settings_from_frequent` does no
  legality check. The new key-aware `is_improper_category` makes that path *safe* rather than
  dangerous in the meantime: a caller with no option key in hand blocks the whole `"Support"`
  category on a part.
- **R2.4 - undo/redo.** One `take_snapshot` per write, as above. Verified by construction and by
  the manual checklist; no automated GUI test exists in this fork to assert it.

### Manual checklist

Run against a side install (`cmake --install <build> --config Release --prefix <scratch>`), never
over `build/Snapmaker_Orca` while the user's slicer is running.

1. Load a multi-part object. Right-click a part → **Support group ▸ New group**. The row gains a
   `[Group]` badge, a settings node appears under the part, and the part parameter panel's Support
   page shows the curated keys.
2. Change one of those values on the part. The Support-groups window (object context menu →
   **Support groups…**) shows the group with its own resolved values and the object row unchanged.
3. Save a support set on the process tab's Support page, then **Support group ▸ New group from
   support set ▸ <name>**. The group takes the set's name; the window's "Support set" column shows
   it, and shows "(modified)" only after you edit one of the group's values.
4. Save the project, close it, reopen it: badge, settings node, part panel and the window all agree.
5. **Ctrl+Z** after an assignment: the badge disappears, the part panel loses the rows, and the
   settings node goes. Ctrl+Y brings all three back.
6. Part settings never show a non-support key from the Support category: the part's "Add settings"
   menu offers exactly the 14 curated keys under "Support" and nothing else (no `enable_support`,
   no `support_type`, no `raft_*`, no `brim_*`).
7. Rename a part that has a badge: the editor opens with the part's own name, not the badge.

### Suites

`libslic3r_tests` - 594 cases / 53 051 assertions, the 2 `[!shouldfail]` cases unchanged;
`[SupportSet]` 8 cases / 411 assertions; the new `[SupportGroups]` 3MF scenarios 2 cases /
28 assertions. `fff_print_tests "[SupportGroups]"` - 8 cases / 44 assertions.

**Pre-existing, and not reachable from this work:** the *whole* `fff_print_tests` suite has 9
failing legacy PrusaSlicer-era scenarios (`test_data`, `test_flow`, `test_gcodewriter`,
`test_model`, `test_print`, `test_printgcode`, `test_skirt_brim`, the last with a SIGSEGV). They
fail on `boost::filesystem::create_directory: The system cannot find the path specified` and on a
config `.ini` the loader calls an unsupported format - an environment/test-data gap, and they fail
the same way from the build directory. Neither test binary links `libslic3r_gui`
(`tests/*/CMakeLists.txt` link `test_common libslic3r OpenSSL::Crypto`), so no Stage 2 GUI change
can reach them. `test_mixed_filament.cpp` also has two order-dependent failures that predate this
work.

Runtime gate `test_support_groups_ui.py` (snorca_hubtest, new), against a side install on an
isolated data dir, driving a **hidden** instance - ALL PASS:

- the DLL under test carries the new menu and window strings (the Object-List menu and the
  Support-groups window are GUI-only, so no API route can open them; that is the most an automated
  check can say about them, and clicking them is the manual checklist's job);
- `GET /api/settings/process` returns the **same 51 Support-page option keys, in the same order**,
  as the pre-change build (`ss_baseline_keys.txt`) - the part panel widened, the process tab did
  not move;
- neither the widget-only "Support set" row nor anything support-group shaped reaches the remote
  sheet, and `support_group` itself never appears in it;
- `POST /api/settings/process` and `/settings/process/revert` behave exactly as before.

Nothing in `RemoteAccess`, `RemoteHub`, `StreamPanel` or `resources/web/orca/` was touched.

## 2c. Stage 3 status (2026-09-04): DONE — the interface follows the group, both gates green

> **Hardware pass (2026-09-05):** the user ran the Stage 3 checklist on the live build fdd5d5ac47 (after
> `fix/support-group-single-part` and `fix/support-group-filament`): every step passed. UI notes that came out
> of it: the groups dialog needed more width for the standard header font (0f056be6d4), and the Support set row
> is being reworked on `feat/support-set-ui` (Groups button, icon buttons, a pop-out set editor).

The first stage that changes what is printed. The shared geometry pipeline — contacts, bases,
columns, rafts — runs **once and unchanged**; only the interface stage and the interface toolpaths
became per group, exactly as §3.5/§3.6 of the Stage 3 section describe. Branch
`feat/support-sets-stage3`, cut from `feat/ultra-preferences` @ `eadc7ec8ed`.

### What shipped

- **`src/libslic3r/Flow.{hpp,cpp}`** — `support_material_flow`, `support_material_1st_layer_flow` and
  `support_material_interface_flow` gained a `const PrintObjectConfig&` form; the object-only forms
  forward with `object->config()`, so all existing call sites are byte-identical.
- **`src/libslic3r/Support/SupportParameters.hpp`** — `SupportParameters(object, object_config)`,
  with `SupportParameters(object)` delegating to it. The body moved unchanged; `object_config` was
  already a local reference, so the diff is three lines plus the flow calls.
- **`src/libslic3r/Support/SupportLayer.hpp`** — the `uint16_t support_group` tag on
  `SupportGeneratorLayer`, 0 for everything a single-group object produces.
- **`src/libslic3r/PrintObjectSlice.cpp`** — `PrintObject::slice_volumes_at_layers(volumes)`, the
  verbatim body `slice_support_volumes(type)` used to hold; the latter is now a two-liner over it, so
  enforcer / blocker behaviour cannot have moved.
- **`src/libslic3r/PrintObject.cpp` / `Print.hpp`** — `PrintObject::support_group_masks()` (groups
  1..K-1 slice their own volumes; group 0 gets the **complement**, so the masks partition the
  footprint with no gap) and `PrintObject::has_support_group_interface_filament()`, the predicate the
  Chameleon pass and `WipingExtrusions` both stand down on. Both are no-ops at K==1 —
  `support_group_masks()` returns before slicing anything.
- **`src/libslic3r/Support/SupportMaterial.{hpp,cpp}`** — the per-group interface stage:
  `support_shared_config()` (the object config with the interface layer counts raised to the max over
  all groups, §3.5), `support_group_claims()` (per-group footprints expanded by
  `gap_xy + interface flow width` and cut against every lower group, so the claims are disjoint),
  `clone_contacts_masked()` (a sibling of each contact layer holding only one group's share, fed to
  `generate_interface_layers` and **never** to the layer graph), and the K-way loop that replaces the
  single `generate_interface_layers` call, carving `intermediate_layers` in group order and tagging
  every layer it produces.
- **`src/libslic3r/Support/SupportCommon.{hpp,cpp}`** — `SupportGroupToolpaths`,
  `support_group_object_layer_index()` (with the nearest-`print_z` fallback R3.2 asks for),
  `support_group_piece()`, and four `groups == nullptr`-guarded edits inside
  `generate_support_toolpaths`: `LayerCache::nonempty` became a `small_vector`, the sibling interface
  layers of the other groups are collected at each print_z, `extrude_interface` splits a shared
  contact layer by claim and fills each piece with its group's pattern / density / flow / angle, and
  a group that pins its own interface filament emits into its own bucket, which becomes
  `SupportLayer::interface_by_extruder` **after** the height modulation.
- **`src/libslic3r/Print.cpp`** — the Chameleon interlock of §3.7, plus
  `PrintObject::add_support_group_chameleon_warning()` because `active_step_add_warning` is protected
  and the Chameleon pass is a free function.
- **`src/libslic3r/GCode/ToolOrdering.cpp`** — R3.5: `is_support_overriddable` returns false for an
  object with a group interface filament, so `flush_into_support` cannot repaint it.
- **`tests/fff_print/test_support_material.cpp`** — the two Stage 3 gate fixtures (below).
- **`tests/data/support_corpus/`** — `twopart_groups_geom.3mf` (new, one filament) and a second
  profile in `make_group_fixture.py` that builds it; `corpus.json` reclassifies `group_parts` and adds
  `group_parts_geom` as **ON-mode** cases.
- **`scripts/support_group_identity.py`** — `--gate groups`, the ON-mode gate.

### The gates

**Off mode — the hard gate.** `scripts/support_group_identity.py` (tolerance, the default) with the
baseline installed from `feat/ultra-preferences`'s build tree and the candidate from this branch,
both to scratch prefixes, on an isolated `--datadir`. **Green on all 8 off-mode cases, three passes:**

| case | pass 1 | pass 2 | pass 3 |
|---|---|---|---|
| `normal_grid` (4466 segments) | ok | ok | ok |
| `normal_snug` (2255) | ok | ok | ok |
| `normal_ledge` (2592) | ok | ok | ok |
| `soluble_interface` (35647) | ok | ok | ok |
| `dense_interface` (4964) | ok | ok | ok |
| `tree_organic` (36339) | ok | ok | ok |
| `raft` (4726) | ok | ok | ok |
| `no_support` (1726) | ok | ok | ok |

Every one of them satisfies the criteria exactly: zero changed config rows, zero changed layers, zero
one-sided layers, segments above the threshold. Note what `tree_organic` being green means — the
organic tree path calls the very functions this stage rewrote (`generate_interface_layers`,
`generate_support_toolpaths`) and is untouched by them, which is the foundation Stage 4a builds on.

**On mode — the new gate.** `--gate groups` runs only the cases whose parts carry `support_group` and
inverts the question: they must **differ**, and only in geometry. Green, three passes, identical
numbers every time:

| case | verdict |
|---|---|
| `group_parts` | `config_rows=0  layers matched=61 identical=55 changed=6  a_only=0 b_only=0  segments=88.40%  dirty_layers=6` — plus the `expect_tool` criterion: **3 `T1` tool changes in the candidate, 0 in the baseline** |
| `group_parts_geom` | the same six layers and 88.40 % of segments, on **one** filament and with **no** tool change anywhere |

Read together those two lines are the whole claim. `config_rows=0` says the settings did not move —
the one new key never reaches `full_print_config` (§3.1), so the G-code config block is identical.
`changed=6` of 61 layers says the geometry did move, and only on the layers carrying part B's support
interface. `group_parts_geom` proves that happens with a single filament, so it cannot be an artefact
of filament ordering; `group_parts` adds the tool change on top, which is the interface filament being
scheduled and emitted end to end.

**Unit gates.** `tests/fff_print/test_support_material.cpp`, `[SupportMaterial][support_groups]`:

1. *per-group interface* — the two-part fixture of the plan's gate item 2. A leg on the bed plus two
   20 mm cubes floating 10 mm above it, three `MODEL_PART` volumes of one object, 20 mm apart in X.
   A control run with no overrides asserts `support_groups().size() == 1`; then part B asks for five
   dense interface layers where the object asks for two sparse ones. Measured over every support
   layer, by summing `erSupportMaterialInterface` path length either side of the gap: **B's interface
   grows by more than 30 %, A's stays within 1 % of the control.** The split X is read off the
   object's own slices rather than assumed, so the test does not depend on where `trafo_centered()`
   puts the origin.
2. *per-group interface filament* — the same fixture with `support_interface_filament = 2` on part B:
   `interface_by_extruder[1]` is non-empty on the layers under B, every entity in it is
   `erSupportMaterialInterface`, no other extruder is ever registered, and
   `has_support_group_interface_filament()` is true.

**Suites.** `libslic3r_tests` — 599 cases / 53 725 assertions, the 2 `[!shouldfail]` cases unchanged.
`fff_print_tests "[SupportGroups]"` (Stage 2's) — 8 cases / 44 assertions, pass.
`fff_print_tests "[support_groups]"` (new) — 2 cases / 20 assertions, pass. Build clean: 0 `error C`,
0 `error LNK`.

**Pre-existing and not reachable from this work:** `fff_print_tests` still carries the 9 legacy
PrusaSlicer-era failures §2b lists, and one of them is in this very file —
`SCENARIO("SupportMaterial: support_layers_z and contact_distance")` SIGSEGVs at its
`WHEN("First layer height = 0.4")` branch. It was **verified to crash identically on the
`feat/ultra-preferences` baseline binary** (same scenario, same branch, same signal), so it predates
Stage 3. §2b's list should have named it; it does now.

### Deviations from the plan

1. **`group_parts` changed sides.** The plan's Stage 2 gate had it as an off-mode case: the fixture
   carries `support_interface_top_layers`, `_bottom_layers`, `_spacing` and `_filament`, all tier A,
   and Stage 2's generator ignored them. Stage 3 makes tier A act, so the case must now differ. It is
   marked `"groups": true` in `corpus.json`, the default gate skips it by name and says so in its
   header line, and `--gate groups` claims it. `group_parts_geom` was added so the ON-mode evidence
   does not rest on a two-filament case alone.
2. **The ON-mode criterion is a third gate, not a hand-checked diff.** §3.7's "second, cheaper
   control" suggests using Slice Compare for diagnosis at K==2. It is used as an *assertion* instead:
   differ, with zero config rows, plus the named tool change present in the candidate and absent from
   the baseline. That is reproducible and it fails loudly if the feature silently stops working.
3. **Gate item 3's ToolOrdering half is asserted by the corpus, not by the unit test.** The plan asks
   the unit test to check that `ToolOrdering` lists the group's extruder on those layers. On the
   synthetic two-entry filament config a unit fixture can assemble, `ToolOrdering` does not even keep
   one `LayerTools` per layer (layers go missing from its table and the reorder pass leaves
   "don't care" entries in place), so such an assertion would be testing the fixture. The claim is
   instead proven on a real printer profile by the `expect_tool` criterion above — 3 `T1` tool changes
   in the candidate's G-code, 0 in the baseline's — which is strictly stronger evidence, and the test
   file says so at the point where the assertion would have been. **Nothing in `ToolOrdering` was
   changed by this stage**; `interface_by_extruder` is storage it already read for Chameleon.
4. **Gate item 4 — the two `#if 0` blocks — was not done, and should be struck from the plan.** They
   are not "API drift": `SupportMaterial: forced support is generated` calls
   `SupportMaterial::support_layers_z()`, and neither that function nor that class exists anywhere in
   this tree (`grep -rn support_layers_z src/libslic3r/` is empty); it also uses
   `support_material_enforce_layers`, which survives only as a legacy alias in `handle_legacy`, plus
   `print.objects` and `print.default_object_config` as public members. Reviving them means writing
   two new tests against a different generator, not fixing two compile errors. The suite's missing
   geometry assertion — the actual reason the plan wanted them — is supplied by the two new fixtures,
   which measure extrusion length rather than layer Zs.
5. **Gate item 5 was run with `assert()` re-enabled in one translation unit, not on a Debug build.**
   No Debug dependencies exist in this tree. See §5 answer 2 for the method and for the positive
   control that makes the result meaningful rather than vacuous.
6. **Ironing stays object-wide in this stage.** `support_ironing*` are tier A keys and are stored and
   resolved per group, but `generate_support_toolpaths` takes its ironing parameters from the shared
   `SupportParameters`, which is the object's. The plan's §3.6 change list does not cover ironing, and
   splitting `polys_to_iron` per group is Stage 5 polish; today a group's ironing keys are inert,
   which is why off-mode is unaffected.
7. **`interface_filament` is compared against the object's value, and a group matching the object is
   not routed.** `SupportGroupToolpaths::interface_filament` is the group's raw
   `support_interface_filament`; the bucket is only used when it is non-zero **and** differs from the
   object's. A group that merely repeats the object's choice therefore keeps extruding into
   `support_fills`, which is what the object was going to print with anyway.
8. **The lowest group's interface layer is still merged into the shared top contact layer.** §3.6
   item 2 sanctions this explicitly ("keeping the lowest-group one in `layer_cache.interface_layer` so
   the existing merge logic behaves as before"), and it is why `interface_layers` is `stable_sort`ed
   after the group loop rather than `sort`ed: at equal `print_z` the groups must stay in group order.
   The merged polygons are then re-split by claim at extrusion time and land back in their own group.

### Decisions for the reviewer (the plan's risks)

- **R3.1 — the seam at group boundaries.** Accepted, and made explicit. `support_group_claims()`
  expands each group's footprint by `scale_(gap_xy) + interface flow scaled width` and then subtracts
  every *lower* group's claim, so the claims are disjoint by construction and the split is a hard clip
  along that boundary. Group 0 takes the remainder, so nothing is ever dropped. The two knobs R3.1
  names — the expansion and the group order — are both in that one function, with the reason in a
  comment. A screenshot of the seam is Stage 5 documentation work.
- **R3.2 — merged contact layers with no object-layer index.** Handled by
  `support_group_object_layer_index()`: it uses `idx_object_layer_above` / `_below` when they are set
  and otherwise binary-searches the nearest object layer `print_z`. Not asserted as "rare" — a Debug
  counter needs a Debug build (see above) — but the fallback is exact for any contact layer, since a
  contact sits against the object by definition.
- **R3.3 — K is not capped at 8.** `LayerCache::nonempty` is now a `small_vector`, which grows, so the
  capacity argument for the cap is gone. Groups are keyed by resolved config, so K is bounded by the
  number of *distinct* support configurations a user actually creates. If a cap is still wanted it
  belongs in the resolver with a UI notice — Stage 5 — not here.
- **R3.4 — dual-nozzle interface flow.** Not addressed in this stage; a group whose interface filament
  sits on a different nozzle gets a different interface flow width, which is correct and may not tile
  with the object's. Stage 5's warning is the mitigation the plan already assigns.
- **R3.5 — `flush_into_support` repainting the interface.** Done, one clause next to the Chameleon one
  in `is_support_overriddable`.
- **R3.6 — group order determinism.** Group order comes from `ModelObject::volumes` order, the
  interface layers are `stable_sort`ed so equal keys keep it, and the claims are cut against lower
  groups in that same order. Reordering an object's volumes therefore changes the output at K>1 — the
  same class of dependency the slicer already has via `clip_multipart_objects`. Worth one line in the
  Stage 5 docs.
- **New — the Chameleon interlock raises a warning the user will see.** `chameleon_assign_support_
  interfaces` now skips an object whose group picks its own interface filament and posts a
  NON_CRITICAL warning saying so. Both features write `interface_by_extruder`; running both would make
  the result depend on ordering. The group wins, as §4 decided.

### Manual checklist

Run against a side install (`cmake --install <build> --config Release --prefix <scratch>`), never over
`build/Snapmaker_Orca` while the user's slicer is running.

1. Load a multi-part object with an overhang over one part. Assign that part a support group
   (**Support group ▸ New group**) and set the group's *interface layers* to 5 and *interface spacing*
   to 0 in the part parameter panel. Slice. In the preview, the support interface under that part is
   visibly denser and deeper than under its neighbour, and the neighbour's is unchanged.
2. Undo the assignment (**Ctrl+Z**) and re-slice: the preview goes back to a single uniform interface.
3. On a two-filament printer, give the group a different **interface filament**. Slice: the preview
   colours that group's support interface with the second filament, the tool changes appear in the
   G-code preview, and the other part's interface keeps the first filament.
4. With that group still holding its own interface filament, turn **support filament matching**
   (Chameleon) on for the object. Slice: a non-critical warning appears saying Chameleon is off for
   this object because a support group picks its own interface filament, and the group's colour is the
   one that survives.
5. Set **flush into support** on as well. The group's interface must keep its colour — no purge
   material repainted into it.
6. Give two different parts groups with *identical* values. They collapse into one group (the
   Support-groups window shows one row), and the output is the same as assigning one group to both.
7. Set a group's `support_top_z_distance` to 0 on an object whose own value is non-zero. The whole
   object becomes soluble-interface (§3.6) — this is Stage 2 behaviour, re-check it still holds.
8. Slice an ordinary project with supports and no groups at all, before and after installing this
   build, and compare the G-code: it must be the same print. (The automated form of this is the
   off-mode gate above.)

### Hardware pass (2026-09-05): the claim did not reach the contacts

Shipped Stage 3 looked right on the corpus and was wrong on a real print. Branch
`fix/support-group-single-part`, cut from `feat/ultra-preferences` @ `1c895674ef`.

**The report.** Two slanted cylinders with normal support; one of them put in a support group made
from a set with more interface layers and interface ironing. Sliced, that part's support interface
looked exactly like the ungrouped one's and like the global settings.

**What the project actually contained.** `tests/supportgroup_test.3mf` is **one** object ("Assembly",
Snapmaker U1, 8 filaments, 0.16 mm layers) with **two** `MODEL_PART` volumes, not two objects - the
GUI's Support-group submenu only appears on a part row, so it could not have been two. The volume
named "back tongue" (the FRONT one on the plate; the names come from the source model and do not
match plate position) carries `support_group = "Normal - With Iron"` and all fourteen curated keys.
Only three of them differ from the object: `support_interface_top_layers` 2 -> 5,
`support_interface_filament` 0 -> 6, `support_ironing` 0 -> 1. Ironing is inert this stage
(deviation 6) and filament 6 is the object's own extruder, so the whole visible claim rested on the
interface layer count.

**The measurement.** Sliced with this branch's build on an isolated data dir, support-interface
extrusion measured per part (the two parts separate cleanly at y = 95):

| run | grouped part | ungrouped part |
|---|---|---|
| group stripped from the 3MF (control) | 12 976.5 mm, 66 layers, 6 780 segments | 10 548.3 mm, 58 layers, 5 188 segments |
| the user's project, group = 5 layers | 13 133.0 mm, 72 layers, 16 345 segments | **identical** |
| no group, 5 layers set OBJECT-WIDE | **17 417.9 mm**, 75 layers, 8 453 segments | 13 707.8 mm |

The group delivered **+1.2 %** where the same value object-wide delivered **+34 %** - about a
thirtieth of the effect - and drew the interface it did produce in 2.4x as many, much shorter
segments. The ungrouped part was byte-identical, so the group machinery was engaging (the log says
`2 support groups`) and staying off its neighbour; it simply was not getting the geometry.

**Root cause.** `PrintObjectSupportMaterial::generate` built the per-group claims by expanding each
group's part footprint by `gap_xy + one interface extrusion width` - about 0.6 mm
(`src/libslic3r/Support/SupportMaterial.cpp:517-520`, as shipped). But a top contact layer is not
the part's footprint: `fill_contact_layer` runs every contact through `SupportGridPattern`
(`SupportMaterial.cpp:2049,2074`), which **stretches each island onto a grid** of
`support_base_pattern_spacing + support_material_flow.spacing()` - 5.4 mm on stock settings
(`SupportGridParams`, `SupportMaterial.cpp:811-820`). The narrow crescent a slanted wall sheds on
each layer therefore becomes a band of 5.4 mm grid cells straddling the part outline, most of it
outside a 0.6 mm claim. Instrumented on the user's project:

```
group 0 (default): claimed 6 691 mm2 of 7 506 mm2 of top-contact area -> 1 847 mm2 of interface
group 1 (the user's group): claimed   814 mm2 (10.9 %)                 -> 294 mm2 of interface
```

`support_group_piece()` gives group 0 whatever the other claims leave, so the 89 % went to the
default group and was filled with the OBJECT's two interface layers. The corpus never caught it
because `twopart_groups*.3mf` overhangs are flat 20 x 16 mm decks - the grid fringe is a small
fraction of a large contact - and because both fixtures are multi-part, where the neighbour's
interface at least changes shape. On a **single-part** object nothing in the output moves at all.

**The fix.** Three edits, all inside the group path (K == 1 still returns before any of it):

1. `src/libslic3r/Support/SupportMaterial.cpp` - the claim reach now includes one grid cell for
   `smsGrid`: `gap_xy + interface width + (support_base_pattern_spacing + support flow spacing)`.
   The comment carries the measured numbers so the constant is not mysterious.
2. `support_group_claims()` gained a second cut: a claim is refused any area sitting over **another
   group's part** (its footprint plus `gap_xy`, which no support may enter anyway). Reaching 5.4 mm
   is only safe with that guard - without it a wide claim would cross to a neighbour and take the
   contacts belonging to it.
3. `PrintObject::support_group_masks()` now gives **every** group its own parts' footprint, group 0
   included, which is what `SupportGroup::mask` was documented to be. Group 0's mask used to be the
   complement of the others; nothing ever read it (group 0 is the complement at *claim* level, in
   `support_group_piece`), and its own footprint is exactly what edit 2 needs. This also drops one
   full-object slice and K boolean ops per layer.

An `info` line per group now records how many top contact layers it claimed of how many, and how
many interface layers it produced - the number that would have made this a five-minute diagnosis.

**Evidence after the fix.** Same project, same build machine, same isolated data dir. Support
interface on the GROUPED part:

| run | interface length | layers | segments | share of the yardstick |
|---|---|---|---|---|
| control (group stripped) | 12 976.5 mm | 66 | 6 780 | - |
| before the fix | 13 133.0 mm | 72 | 16 345 | **3.5 %** |
| **after the fix** | **15 706.9 mm** | 72 | 9 374 | **61 %** |
| yardstick: 5 layers set object-wide | 17 417.9 mm | 75 | 8 453 | 100 % |

The ungrouped part is **10 548.3 mm / 58 layers / 5 188 segments in every one of those runs** -
identical to the last digit, so widening the claim took nothing from the neighbour. The
fragmentation is largely gone with it: 16 345 segments for 13 133 mm (0.80 mm per segment) became
9 374 for 15 707 mm (1.68 mm), against the yardstick's 2.06 mm. The new log line reads
`group 1 ("Normal - With Iron", 1 parts, 5 top interface layers) claimed 61 of 86 top contact
layers, produced 53 interface layers`.

**The residual, stated honestly.** 61 %, not 100 %. What is left is the outermost grid fringe: a
contact cell further from the part than one grid cell still goes to the default group, which fills
it with the object's two layers where an object-wide setting would have given it five. `reach` is
the lever and it is one line; it was not pushed further because a larger radius can only be paid for
by taking a neighbour's fringe, and on a flat overhang - the unit fixture below - one grid cell
already delivers more than 90 %. Widening it further, or replacing the fixed radius with a true
nearest-part split, is Stage 5 work with a measurement to aim at.

**Gates, all on this branch's own before / after pair (both Stage 3, installed to scratch prefixes,
isolated `--datadir`).**

- **Off mode, the hard gate** - `scripts/support_group_identity.py`, tolerance: **green on all 8
  off-mode cases** (`normal_grid` 4466 segments, `normal_snug` 2255, `normal_ledge` 2592,
  `soluble_interface` 35647, `dense_interface` 4964, `tree_organic` 36339, `raft` 4726,
  `no_support` 1726). Expected by construction - every line of this fix sits behind `K > 1` - and
  measured anyway.
- **On mode** - `--gate groups`: `group_parts_geom` **ok**, `group_single_part` **ok**
  (`config_rows=0 layers matched=41 identical=35 changed=6 a_only=0 b_only=0 segments=55.36 %
  dirty_layers=6`). `group_parts` reports `DIFFER ... the baseline already has a T1 tool change -
  the case proves nothing`: its `expect_tool` criterion asks for a tool change **absent from the
  baseline**, which only holds when the baseline predates Stage 3. Here both sides are Stage 3 and
  both emit exactly 3 `T1`, as they should - this fix does not touch filament routing. The
  criterion is fine; the pairing is not the one it was written for.
- **Unit** - `fff_print_tests "[support_groups]"`: 3 cases / 35 assertions pass with the fix, and
  the new one **fails without it** (`grouped > global * 0.90` -> `2469.67 > 2783.20` is false, i.e.
  79.9 % of the yardstick). `"[SupportGroups]"` (Stage 2's): 8 cases / 44 assertions pass.
  `libslic3r_tests`: 599 cases / 53 725 assertions, the same 2 `[!shouldfail]` cases as §2c.
- **Unchanged pre-existing failure** - `SCENARIO("SupportMaterial: support_layers_z and contact
  distance")` still SIGSEGVs at `WHEN("First layer height = 0.4")`, as §2c records.

**New regression cover.**

- `tests/fff_print/test_support_material.cpp` - `"a group on a single-part object acts like the
  object's own settings"`. A leg plus a floating cube merged into ONE `MODEL_PART`, so the default
  group owns no parts. Three runs: control, five interface layers object-wide, five asked for by a
  group. Asserts the object-wide change really deepens the interface (> 30 %), that the group does
  too, and that the group delivers > 90 % of what object-wide delivers. Before the fix the third
  assertion fails by a wide margin.
- `tests/data/support_corpus/` - `onepart_ledge.3mf` (one volume, written by `make_fixtures.py`) and
  `onepart_group.3mf` (the `singlepart` profile of `make_group_fixture.py`), plus the
  `group_single_part` ON-mode case in `corpus.json`.

**What this says about the earlier gates.** Nothing they asserted was wrong: off-mode identity holds
(the whole group path is behind `K > 1`), and `group_parts` / `group_parts_geom` really did differ
from the baseline. They were just not sensitive to *how much* of the group's own contact area the
group actually got - "differs" is a weak claim. The unit test above is the first one that measures
the magnitude against a yardstick, and it is the assertion to keep.

### Hardware pass 2 (2026-09-05): the group's filament under both parts

Branch `fix/support-group-filament`, cut from `feat/ultra-preferences` @ `3adbc031c6` (which
carries Stage 3 and the claim-reach fix above). Two reports from the same print.

**A. The group's interface filament appeared under BOTH parts.** With one part in a group whose
interface filament is 8 (PVA, cyan), the cyan interface was drawn under the ungrouped part too;
giving the other part a second group with a PETG interface (red) then mixed red into the PVA
part's support as well.

**B. The Support-groups window's table header was white on white**, and the window drew in the
system font rather than the fork's.

**What the project contains.** `tests/supportgroup_test.3mf` is the same object as the first
hardware pass, re-made: "Assembly" on a Snapmaker U1 with 9 filament slots, object extruder 6, two
`MODEL_PART` volumes 26 mm apart in Y (both named "front tongue" - two placements of one source
volume). Two things in it decide everything below:

- the object carries `support_filament_matching = 1` - **Chameleon is on** - while the process
  leaves `support_filament` and `support_interface_filament` at 0, i.e. *"don't care"*;
- as saved, **both** volumes carry `support_group = "PVA Interface"` with identical values
  (`support_interface_filament` 8, 5/2 interface layers, spacing 0, `rectilinear_interlaced`,
  ironing on, `support_top_z_distance` 0). The file therefore cannot show symptom A by itself -
  with both parts in the group, cyan under both parts is the correct answer. The measurements
  below use a variant with the group left on ONE part, which is the situation the report
  describes, built by rewriting `Metadata/model_settings.config` and nothing else. Whether the
  user assigned the group to both parts or the GUI did is not answered here: the assignment path
  (`selected_part_volumes` / `assign_support_group`, `GUI_Factories.cpp:1314,1445`) writes only
  the volumes the object list actually has selected, and re-reading it turned up no way for one
  click to reach two parts. Worth one question to the user.

**Root cause of A: `GCode.cpp`, the Chameleon "mechanism B" dominant-bucket pin** (`GCode.cpp`
:5590-5607 as shipped; `process_layer`'s support-extruder block). It fires for any object with
`support_filament_matching` on whose support layer has a non-empty
`SupportLayer::interface_by_extruder`, and pins **both** still-"don't care" slots - the support
BASE and the support INTERFACE - to that layer's largest bucket's extruder. It was written when
that map held only Chameleon's own matched partitions. Since Stage 3 a support group's own
interface filament writes the very same map (`SupportCommon.cpp:2104`) - and for exactly such an
object the Chameleon pass has already stood down (`Print.cpp:3197`), so every bucket there is a
GROUP's, not a match. The pin therefore handed the whole layer's don't-care support - the base and
the *other* parts' interface, everything still in `support_fills` - to the group's filament.

There is a second, smaller path to the same place, and it does not need Chameleon at all: with
`support_interface_filament = 0` the don't-care resolution falls through to the first/active
extruder of the layer (`GCode.cpp`:5638 as shipped), and once a group has forced its filament onto a layer
that extruder is often the group's. Measured with matching off, the ungrouped part's interface
came out **49 % T7** that way.

**The fix**, three edits, all no-ops for an object with no support group:

1. `src/libslic3r/PrintObject.cpp` - `PrintObject::support_group_interface_extruders()`, the
   0-based extruders a group pins for its own interface (sorted, unique, empty otherwise).
   `has_support_group_interface_filament()` is now `! ...empty()`, so the two cannot drift.
2. `src/libslic3r/GCode.cpp` - the dominant-bucket pin stands down when that list is non-empty,
   the same interlock the Chameleon pass, `WipingExtrusions` and `is_support_overriddable` already
   use; and those extruders are excluded from the "don't care" candidate search in both of its
   branches. A group's filament is for its own part's interface and for nothing else.
3. `src/libslic3r/PrintObject.cpp` / `Print.cpp` - the Chameleon stand-down **warning now
   appears**. See below.

**The Chameleon warning: it never fired, and could not have.** `chameleon_assign_support_
interfaces` posted it through `PrintObject::active_step_add_warning`, but that pass runs at the
end of `Print::process` (`Print.cpp:4290`), *after* every PrintObject step has finished. There
`m_step_active` is -1 - `active_step_add_warning`'s own `assert(m_step_active != -1)` says that
must not happen, and a Release build has no asserts, so the call indexed `m_state[-1]` and the
warning went nowhere. Measured: **0 occurrences on the baseline, on every variant.** It is now
raised from `PrintObject::generate_support_material()`, inside the `posSupportMaterial` step,
under the same condition plus `support_filament_matching`; `add_support_group_chameleon_warning()`
- the public door that existed only for the old call - is gone. It also has a
`SlicingNotificationType` of its own (`SlicingSupportGroupChameleonOff`), because the CLI's
`result.json` filter drops any warning left on the default id - which is precisely why nothing
automated could have caught its absence. Measured after: the warning appears exactly on the
variants with Chameleon ON *and* a group interface filament (as-saved, one-part, two-group), and
not on the matching-off or no-group variants, in both `result.json` and the progress stream.

**Evidence: support-interface extrusion per part, per tool, on the user's project.** Sliced with
the CLI on an isolated data dir; the two parts separate at y = 121.5. T5 is filament 6 (the
object's own), T7 is filament 8 (PVA, the group's), T8 is filament 9 (PETG).

*Group on ONE part (the front one), Chameleon on - the reported case:*

| run | grouped part (front) | ungrouped part (back) |
|---|---|---|
| control, group stripped | 8 233.4 mm, all T5 | 9 443.2 mm, all T5 |
| **before** | 10 092.9 mm, **all T7** | 9 504.9 mm, **all T7** |
| **after** | 9 944.4 mm T7 + 148.5 mm T5 (**98.5 % T7**) | 9 504.9 mm, **all T5** |

The ungrouped part's interface is **9 504.9 mm in 4 909 segments before and after** - identical to
the last digit. Nothing moved but the tool, which is the whole claim. The support BASE and the
ironing tell the same story: before, 5 957 mm of base and 4 272 mm of ironing on the ungrouped
part were drawn in PVA; after, **neither part has a single millimetre of T7 outside the group's own
interface**.

*Two groups, PVA on the front part and PETG on the back:*

| run | front part interface | back part interface | base support |
|---|---|---|---|
| before | 9 990.1 T7 + 109.7 T8 | 10 459.4 T8 + 17.6 T7 | T7 7 015 mm + T8 5 869 mm scattered over both parts |
| after | 9 920.9 T7 + 178.8 T5 | 10 391.9 T8 + 85.2 T5 | **0 mm of T7 or T8** - all T5 |

*The file as saved (both parts grouped):* both parts keep their PVA interface, correctly, before
and after; what changes is that 12 623 mm of PVA base support and 8 008 mm of PVA ironing become
the object's own filament.

The ~1.5 % of the grouped part's interface still drawn in T5 (148.5 mm of 10 092.9) is the R3.1
claim seam, not a filament bug: it is the same fringe the claim-reach section measures, and it is
present in the control too.

**Root cause of B, and the fix.** `SupportGroupsDialog` themed itself with `UpdateDlgDarkUI`,
which walks the child windows and recolours them. The table header is not one: behind the generic
`wxDataViewListCtrl` it is a native header HWND, so `SetForegroundColour` on the control never
reaches it and it kept whatever text colour the theme last left there. `GUI_App::UpdateDVCDarkUI`
(`GUI_App.cpp:3789`) is the fork's own answer and every other table in the application calls it -
the object list, Unsaved Changes, Print Host, Slice Compare - but this window did not. It applies
the mode-aware explorer theme to the header HWND and sets a header `wxItemAttr` carrying
`NppDarkMode::GetTextColor()`, which is `0xF0F0F0` in dark mode and the system window text in
light, so one call fixes both modes; it also gives the rows the alternating colour and the border
the other tables have. It has to run *after* `UpdateDlgDarkUI`, which would otherwise paint over
it. The fonts are the second half: the dialog now sets `Label::Body_14` on itself before its
children are built, and on the table and the hint line, so the window reads in the same face and
size as the rest of the slicer instead of the system default.

**Gates.**

- **Off mode, the hard gate** - `scripts/support_group_identity.py`, tolerance, baseline
  `feat/ultra-preferences` @ `3adbc031c6` installed to a scratch prefix: **green on all 8 cases,
  three passes**, with exactly the segment counts 2c records (`normal_grid` 4466, `normal_snug`
  2255, `normal_ledge` 2592, `soluble_interface` 35647, `dense_interface` 4964, `tree_organic`
  36339, `raft` 4726, `no_support` 1726). Expected by construction: every branch of the fix is
  behind a non-empty `support_group_interface_extruders()`.
- **On mode** - `--gate groups --baseline-has-groups`: **green on all 4 cases, three passes.**
- **Unit** - `fff_print_tests "[SupportGroups]"` 10 cases / 50 assertions, `"[support_groups]"`
  3 cases / 35 assertions, `libslic3r_tests` 599 cases / 53 725 assertions with the same 2
  `[!shouldfail]` cases. Build clean: 0 `error C`, 0 `error LNK`.

**The ON-mode gate had to be taught about a baseline that already has groups**, and this is the
more useful half of the harness change. Its requirements 1 and 3 - "the two G-codes must DIFFER"
and "the tool change must be ABSENT from the baseline" - only say anything while the baseline
*predates* the feature. Every branch cut after Stage 3 pairs two builds that both honour groups,
so both requirements are unsatisfiable by construction; 2c hit this for `group_parts` and called
it a pairing problem, and this branch would have hit it for all four cases. `--baseline-has-groups`
flips requirement 1 to "the support GEOMETRY must be IDENTICAL" and drops the second half of
requirement 3. That is not a weaker gate for a fix like this one, it is the right one: the
comparison engine reads coordinates, not tools, so `config_rows=0 changed=0 a_only=0 b_only=0
segments=100.0000%` on all four cases is a *positive* statement that this change moved no support
geometry at all.

**New regression cover.**

- `corpus.json` - `expect_tool_part` on `group_parts`, and a new ON-mode case
  `group_parts_matching`: the same fixture and args plus `--support-filament-matching=1`, which is
  what the user's project had. The criterion splits the named feature's extrusions at the widest
  gap along an axis and requires the named tool to draw >= 95 % of the group's own part and <= 5 %
  of the other part's. On `twopart_groups.3mf`, measured across the parts' split at x = 138.4, the
  baseline gives the neighbouring part **100.0 % T1 with matching on** and **34.0 % T1 with it
  off**; the candidate gives **1.0 %** in both. `expect_tool` - "a T1 exists somewhere in the
  file" - is true on every one of those runs, which is exactly why it could not see this.
- `tests/fff_print/test_support_groups.cpp` - two cases for
  `support_group_interface_extruders()`: a group pinning slot 2 is reported as extruder 1, and a
  group merely repeating the object's own slot pins nothing. (They need a two-filament config:
  `PrintObject` clamps an out-of-range support filament slot to 1, which would otherwise turn the
  first case into the second.)

**Not fixed here, and worth a decision.** The predicate compares a volume's RAW
`support_interface_filament` slot against the object's CLAMPED one
(`object_config_from_model_object` runs `clamp_exturder_to_default`, the volume value does not).
On a printer with fewer filaments than the group asks for, the group's slot is used unclamped
through the whole Stage 3 path - `support_groups()` copies it raw as well - so this is
pre-existing Stage 3 behaviour, consistent within itself, and left alone. Stage 5's "a group whose
interface filament cannot be resolved raises a warning" is where it belongs.

### What Stage 4 and Stage 5 inherit

Stage 4a is now a small stage: `TreeSupport3D::generate_support_areas` calls the *same* two functions
this stage made group-aware, so organic-tree parity is (a) building its `SupportParameters` from
`support_shared_config`, (b) the same per-group `generate_interface_layers` loop with
`clone_contacts_masked`, and (c) passing the `groups` vector to `generate_support_toolpaths`. R4.1 —
whether organic-tree contacts carry `idx_object_layer_above` — is already answered defensively by
`support_group_object_layer_index()`'s nearest-`print_z` fallback. Stage 4b (classic tree roof fill)
is untouched by this stage and remains as planned. Stage 5 inherits: the preview-colour check, every
warning in its list, the R3.1 seam screenshot, the R3.6 volume-order note, the classic-tree notice,
and per-group ironing (deviation 6).

## 3. Shared contract

The stages are meant to be built by different agents on different branches. These names, shapes and
rules are the contract between them; change them only by editing this section first.

### 3.1 New config keys

Exactly **one** new key. It is defined in `PrintConfigDef` and is a member of **no** static config class
— the same shape as `extruder` (`src/libslic3r/PrintConfig.cpp:1981`, which is in `print_config_def` but
in neither `PrintObjectConfig` nor `PrintRegionConfig`, and is special-cased by hand at
`src/libslic3r/PrintObject.cpp:3251-3262`).

```cpp
// src/libslic3r/PrintConfig.cpp, next to the support block (after support_interface_filament, :5997-6005)
// Ultra (support groups): the name of the support group this part belongs to. Empty / absent = the
// object's own Support settings (the default group). Stored per ModelVolume only; deliberately not a
// member of PrintObjectConfig or PrintRegionConfig and not in Preset::print_options(), so it never
// reaches the process preset, the project config or the G-code CONFIG_BLOCK. Label/category left empty
// so is_improper_category (GUI_Factories.cpp:84) hides it from the "Add settings" menu.
def = this->add("support_group", coString);
def->tooltip = L("Name of the support group this part belongs to. Empty means the object's own support settings.");
def->mode = comDevelop;
def->set_default_value(new ConfigOptionString(""));
```

Consequences, all verified:

- `ModelConfig` wraps a `DynamicPrintConfig` (`src/libslic3r/PrintConfig.hpp:2034-2093`) whose `def()` is
  `print_config_def`, so a def-only key can be stored on a `ModelVolume` with no further plumbing.
- 3MF export writes every `volume->config` key with no allow-list
  (`src/libslic3r/Format/bbs_3mf.cpp:7631-7634`); import feeds every unmatched metadata key to
  `volume->config.set_deserialize` (`:4899` and `:5047`).
- A **stock Orca/Bambu Studio** reading such a project hits
  `PrintConfigDef::handle_legacy` (`src/libslic3r/PrintConfig.cpp:7841`), whose tail
  (`:8014-8017`) does `if (! print_config_def.has(opt_key)) { opt_key = ""; return; }` — the key is
  dropped silently, no `UnknownOptionException` (`src/libslic3r/Config.cpp:597`), no crash. Graceful
  degradation confirmed.
- Because the key is not in `s_Preset_print_options` (`src/libslic3r/Preset.cpp:871`) it never appears
  in `Print::full_print_config()` and therefore never in the G-code config block
  (`src/libslic3r/GCode.cpp:2332`, `:3264`). This is what makes §3.7's byte identity *exact* rather than
  "identical modulo new keys".

Everything else a group carries — `support_interface_top_layers`, `support_interface_spacing`,
`support_interface_pattern`, `support_interface_filament`, … — is an **existing**
`PrintObjectConfig` key written onto the volume's `ModelConfig`. No schema change.

### 3.2 Support set: JSON shape, location, enumeration

**Location.** `<datadir>/user/<preset_folder>/support_set/*.json`, where `<preset_folder>` is
`AppConfig::get("preset_folder")` or `DEFAULT_USER_FOLDER_NAME` ("default",
`src/libslic3r/PresetBundle.hpp:16`) — the same rule `PresetBundle::load_user_presets` uses for
`process/`, `filament/`, `machine/` (`src/libslic3r/PresetBundle.cpp:794-836`, dir constants at
`src/libslic3r/Preset.hpp:21,23`).

`PresetCollection::load_presets` (`src/libslic3r/Preset.cpp:1189-1215`) only ever walks the one subdir it
is handed (plus a nested `base/`), so a sibling `support_set/` directory is invisible to the preset
machinery and cannot collide with it. `PresetBundle::save_user_presets`
(`src/libslic3r/PresetBundle.cpp:1106-1128`) likewise only writes the three known subdirs.

**Enumeration** mirrors `load_presets`: `boost::filesystem::directory_iterator` over the directory,
`Slic3r::is_json_file(name)`, skip and log anything that fails to parse. Sort by the in-file `name`,
case-insensitive.

**Identity.** The file's `name` field is authoritative and is what the group references. The filename is
`sanitize(name) + ".json"` where `sanitize` replaces every character outside `[A-Za-z0-9 _.-]` with `_`
and trims; on a collision append ` (2)`, ` (3)`, … to the *file* name only. Rename rewrites `name` and
moves the file; delete removes the file.

**Shape** (version 1):

```json
{
  "version": 1,
  "type": "support_set",
  "name": "Soluble interface",
  "description": "PVA interface, zero gap, 3 dense layers",
  "created": "2026-09-02T14:05:11Z",
  "app_version": "2.1.0",
  "interface_filament_type": "soluble",
  "values": {
    "support_interface_top_layers": "3",
    "support_interface_bottom_layers": "3",
    "support_interface_spacing": "0",
    "support_bottom_interface_spacing": "0",
    "support_interface_pattern": "auto",
    "support_interface_loop_pattern": "0",
    "support_top_z_distance": "0",
    "support_style": "grid",
    "support_threshold_angle": "30"
  }
}
```

- `values` are `ConfigBase::opt_serialize` strings of `print_config_def` keys, exactly like a preset
  `.json`, so reading back is `set_deserialize` and forward compatibility comes free from
  `handle_legacy` (`src/libslic3r/PrintConfig.cpp:7841`).
- **Allowed keys** = every key that is (a) a member of `PrintObjectConfig`
  (`src/libslic3r/PrintConfig.hpp:953-1023` for the support block) **and** (b) whose
  `print_config_def` category is `"Support"` — `PrintConfig.cpp:6000` and 57 more. Loading a file with a
  key outside that set drops the key and logs a warning; it does not fail the load.
- **Excluded by construction**: `support_filament` and `support_interface_filament` (slot indices are
  not portable — they travel as `interface_filament_type` instead), every `raft_*` key,
  `enforce_support_layers`, and `independent_support_layer_height` (a `PrintConfig`, i.e. printer-wide,
  key). The excluded list is a named constant so Stages 1, 2 and 5 agree.
- `interface_filament_type` is one of `"same"`, `"soluble"`, or an exact `filament_type` enum string
  (`src/libslic3r/PrintConfig.cpp:2566-2604`: `"PVA"`, `"BVOH"`, `"PLA"`, `"PETG"`, …).

### 3.3 Interface-filament resolution rule

`int resolve_interface_filament(const std::string &type, const DynamicPrintConfig &full_config, std::string *warning)`
returns a 1-based `support_interface_filament` slot, or 0 for "Default / use the current filament".
It reads `filament_type` (`ConfigOptionStrings`, `PrintConfig.hpp:1289`) and `filament_soluble`
(`ConfigOptionBools`, `:1291`).

| `interface_filament_type` | Rule |
|---|---|
| `"same"` (or missing) | return **0** — `support_interface_filament = 0` already means "no specific filament, use the current one" (`PrintConfig.cpp:5997-6005`) |
| `"soluble"` | lowest 1-based `i` with `filament_soluble[i-1] == true`; else lowest `i` with `filament_type[i-1] ∈ {"PVA","BVOH"}`; else **0** + warning `"no soluble filament is loaded; the support interface will use the current filament"` |
| any other string `T` | lowest `i` with `filament_type[i-1] == T` (case-insensitive, exact); else lowest `i` with `filament_type[i-1]` starting with `T + "-"` (so `"PLA"` matches `"PLA-CF"` only when no plain PLA is loaded); else **0** + warning `"no <T> filament is loaded; …"` |

Ties always break to the lowest slot, so the rule is deterministic and printer-independent. The warning
is surfaced (Stage 1: next to the Support-set row; Stage 5: in the Support-groups panel), never as a
modal.

The reverse map, used by "Save current as…": if the current `support_interface_filament` is 0 →
`"same"`; else if `filament_soluble[slot-1]` → `"soluble"`; else `filament_type[slot-1]`.

### 3.4 Group model and resolution

```cpp
// src/libslic3r/Print.hpp, in PrintObject's public section next to slice_support_volumes (:524-526)
struct SupportGroup {
    // Display name. "" = the default group (parts with no support_group key).
    std::string                      name;
    // The object's PrintObjectConfig with this group's part-level support overrides applied.
    PrintObjectConfig                config;
    // The MODEL_PART volumes resolving to this group, in ModelObject::volumes order.
    std::vector<const ModelVolume*>  volumes;
    // Per object layer (this->layers()), the union of `volumes` sliced at that layer's slice_z.
    // Empty until PrintObject::support_group_masks() fills it.
    std::vector<Polygons>            mask;
};
std::vector<SupportGroup> support_groups() const;
```

Resolution, in `PrintObject::support_groups()`:

1. Start from `m_config` (the object's own `PrintObjectConfig`, produced by
   `object_config_from_model_object`, `src/libslic3r/PrintObject.cpp:3230-3242`).
2. For each `MODEL_PART` volume in `model_object()->volumes` order, build
   `cfg = m_config`, then apply the volume's values for the **part-level support key set** only
   (§3.5). Read the display name from the volume's `support_group` key.
3. **Group by resolved `cfg`, not by name**: two parts land in the same group iff
   `cfg_a.diff(cfg_b).empty()` over the part-level key set. The group's `name` is the first
   non-empty name seen among its members, else `""`.
4. Group 0 is always the group whose config equals `m_config` (the default group), even if empty of
   parts, so downstream code can address it. Groups 1..K-1 follow in first-volume order.
5. **If K == 1 the caller must take today's code path unchanged.** This is the whole of the off-mode
   guarantee, and it is the same discipline the fork already applies to Chameleon
   (`src/libslic3r/Print.cpp:3078-3080`).

A part whose override happens to equal the object value therefore yields K==1 — the research's test #1
(`§ "Test coverage today"` item 1) is exactly this.

`mask` is filled by re-slicing the group's volumes at the object's layer Zs, reusing the machinery of
`PrintObject::slice_support_volumes` (`src/libslic3r/PrintObjectSlice.cpp:5607-5657`) — see Stage 3 for
the factoring. This is the research's attribution "route 2": no region-dedup break, no perimeter side
effects, one extra mesh slice per group.

### 3.5 The part-level support key set

> **Addendum 2026-09-05 (over-support surfaces).** `over_support_surfaces`, `over_support_flow` and
> `over_support_speed` (`docs/superpowers/specs/2026-09-05-over-support-surfaces.md`) are `Support`-
> category `PrintObjectConfig` keys, so they join `support_set_keys()` by the derived rule of §2a
> item 3 with no edit, and they were added to the curated `part_support_keys()` list below by hand.
> They are **tier B**: set-eligible and part-eligible - a set carries them, a part may carry them, the
> 3MF round trip and the group resolver handle them - but object-wide in behaviour, because
> `detect_surfaces_type` reads the object's own `PrintObject::m_config`. **Stage 5 wires the per-part
> path**; the same promotion `support_style` and `support_threshold_angle` are waiting for.


Curated, not mechanical (research open question 7). Three tiers; Stage 2 exposes tier A + B on parts,
Stage 3 makes tier A act, tier B is display-only until Stage 4/later, tier C is never per-part.

**Tier A — acts in Stage 3 (normal) and Stage 4 (tree):**
`support_interface_top_layers`, `support_interface_bottom_layers`, `support_interface_spacing`,
`support_bottom_interface_spacing`, `support_interface_pattern`, `support_interface_loop_pattern`,
`support_interface_filament`, `support_ironing`, `support_ironing_pattern`, `support_ironing_flow`,
`support_ironing_spacing`.

**Tier B — stored, resolved, shown, but object-wide in behaviour (Stage 3/4):**
`support_top_z_distance` (see §3.6 — it *does* act, but only via the strictest rule),
`support_style`, `support_threshold_angle`. A future stage can promote these by running the whole
generator per group; nothing in this plan's data model has to change for that.

**Tier C — never per-part:** `raft_*` (a raft is an object-wide substructure,
`src/libslic3r/Slicing.cpp:130-150`), `enforce_support_layers` (object layer indices),
`independent_support_layer_height` (a printer-wide `PrintConfig` key), `support_type`,
`enable_support`, `support_filament`.

### 3.6 The soluble rule — what happens to `support_top_z_distance`

`support_top_z_distance` is not only a support-generator input. It sets
`SlicingParameters::soluble_interface` (`src/libslic3r/Slicing.cpp:78`, stored at `:90`, gaps at
`:117-127`), drives `bottom_is_fully_supported` in `detect_surfaces_type`
(`src/libslic3r/PrintObject.cpp:1368`, which changes **object** extrusions, not support ones), forces a
full `posSlice` invalidation (`src/libslic3r/PrintObject.cpp:1005`), and sets the process-global static
`TreeSupportSettings::soluble` (`src/libslic3r/Support/TreeSupport3D.cpp:132-135` →
`src/libslic3r/Support/TreeSupportCommon.hpp:361`, `inline static bool soluble = false`).

**Rule: object-wide soluble behaviour follows the strictest group.** If **any** support group on an
object resolves to `support_top_z_distance == 0`, the whole object behaves as soluble-interface: zero
gap, `stBottom` instead of `stBottomBridge`, `TreeSupportSettings::soluble = true`. Groups asking for a
larger gap do **not** get their own gap; the UI says so (Stage 5 warning: *"Group <X> asks for a soluble
interface, so this whole object uses a 0 mm top Z distance."*).

**Where it is applied — one place:** `PrintObject::object_config_from_model_object`
(`src/libslic3r/PrintObject.cpp:3230-3242`). It already receives the `ModelObject`; today it never looks
at `object.volumes` (research §1.2). Add:

```cpp
    // Ultra (support groups): support_top_z_distance is object-wide behaviour - it decides
    // SlicingParameters::soluble_interface (Slicing.cpp:78), bottom surface classification
    // (PrintObject.cpp:1368) and the organic-tree static (TreeSupport3D.cpp:132). A per-part
    // value cannot be honoured, so the strictest group wins: any group asking for a soluble
    // interface makes the whole object soluble. No group -> no change.
    if (support_groups_want_soluble(object))
        config.support_top_z_distance.value = 0.;
```

This one line buys four things at once:

1. It feeds `PrintObject::m_config`, hence `SlicingParameters`, `detect_surfaces_type` and the tree
   static, with no changes at any of those sites.
2. It routes through the **existing** invalidation path: `PrintApply.cpp:1671` recomputes
   `object_config_from_model_object` and diffs it against the live `PrintObject::config()`
   (`:1672-1680`), so flipping a group's Z gap correctly forces `posSlice` via the
   `support_top_z_distance` entry at `PrintObject.cpp:1005`.
3. With no groups it is a no-op → byte identity by construction.
4. It makes "soluble interface under part B only" *work* (B gets its dense PVA interface; A simply gets
   the same zero gap it would have had if the user had set it object-wide), which is the actual user
   need behind the request.

### 3.7 Off-mode byte identity — definition and measurement

**Definition.** For a fixed project + preset + build machine, the G-code produced by the new build is
byte-identical to the G-code produced by the baseline build after normalising exactly one line: the
`; generated by <version> on <local timestamp>` line inside `HEADER_BLOCK`
(`src/libslic3r/GCode.cpp:2275-2276`). Nothing else may differ — not the config block, not layer
counts, not a single extrusion coordinate. Because the only new config key (§3.1) never reaches
`full_print_config`, the `CONFIG_BLOCK` (`GCode.cpp:2332`, `:3264`) is unchanged too.

"Off mode" means: **K == 1** for every object on the plate. That covers every existing project (no
`support_group` keys anywhere) and any new project where all parts resolve to the same support config.

**Measurement — `scripts/support_group_identity.py`** (new, Stage 1):

```
python scripts/support_group_identity.py --baseline <old exe> --candidate <new exe> \
       --corpus tests/data/support_corpus --out <dir> [--datadir <isolated dd>]
```

It drives both executables through `scripts/orca_cli.py`'s `OrcaCli.slice(...)`
(`--slice 0 --allow-newer-file --no-thumbnails --outputdir <dir>`, see `scripts/orca_cli.py:65-91`),
then for each produced `.gcode` strips the `; generated by ` line from both sides and compares SHA-256.
It prints one line per project and exits non-zero on the first difference, naming the first differing
line number. Use `--datadir` so the run cannot touch the real profile tree.

**Corpus.** At minimum: one single-part object with normal supports; one multi-part assembly with
normal supports; one object with a soluble interface (`support_top_z_distance = 0`,
`support_interface_filament` set); one organic-tree object; one classic-tree object; one raft; one
multi-plate project; one project with support enforcer + blocker volumes and painted supports. Store
them under `tests/data/support_corpus/` (3MF, small). `tests/h2d_persist.3mf` and `tests/testload.3mf`
already in the tree are reasonable seeds.

**Second, cheaper control**: the fork's own Slice Compare
(`docs/superpowers/specs/2026-08-29-slice-compare-design.md`, engine at
`src/libslic3r/SliceCompare/{Snapshot,Diff}.{hpp,cpp}`, UI at `src/slic3r/GUI/SliceCompare/`, tests at
`tests/libslic3r/test_slice_compare.cpp`). `load_snapshot_from_file(path)`
(`src/libslic3r/SliceCompare/Snapshot.hpp:56-62`) reads a `.gcode` or `gcode.3mf` headlessly, so
"self-diff of the two builds' outputs ⇒ 0 config rows, all layers identical, segments 100 % both" is a
one-screen assertion. Use it in the GUI for *diagnosis* when the byte compare fails — it tells you which
layers and which feature moved — and for the K==2 stages, where byte identity is not expected and you
instead want "support-interface seconds changed above part B, everything else identical".

### 3.8 Invalidation contract

A per-volume support key change reaches **nothing** today. Verified: the only volume-config-driven
invalidation is `verify_update_print_object_regions` (`src/libslic3r/PrintApply.cpp:747-826`), which
diffs `region_config_from_model_volume` output — and that funnels through
`apply_to_print_region_config` (`src/libslic3r/PrintObject.cpp:3247-3271`), whose
`if (ConfigOption* my_opt = out.option(it->first, false); my_opt != nullptr)` at `:3264` silently drops
every support key. So the model would change and the print would not re-slice.

Two hooks, both required:

1. **Soluble flip → `posSlice`**: free, via §3.6 (the object config genuinely changes, and
   `PrintApply.cpp:1669-1682` already diffs it and calls
   `PrintObject::invalidate_state_by_config_options`, whose `support_top_z_distance` entry at
   `PrintObject.cpp:1005` maps to `posSlice`).
2. **Everything else → `posSupportMaterial`**: add a `model_support_group_data_changed(model_object,
   model_object_new)` predicate next to `model_custom_supports_data_changed` and extend the condition
   at `src/libslic3r/PrintApply.cpp:1646`:

   ```cpp
   if (supports_differ || model_custom_supports_data_changed(model_object, model_object_new) ||
       model_support_group_data_changed(model_object, model_object_new)) {
   ```

   The predicate compares, pairwise over `MODEL_PART` volumes matched by id, the serialised values of
   the part-level support key set (§3.5) plus `support_group`. It runs **before**
   `model_volume_list_copy_configs` copies the new configs over (`PrintApply.cpp:1684`, helper at
   `:58-89`), so both sides are still available. It must not set `supports_differ` — that one also stops
   background processing and shuffles volumes (`:1648-1651`), which is not needed here.

Both paths are no-ops when no part carries a support override, which is the Stage 2 gate.

## 4. Decisions taken

- **A support set is not a preset.** No `Preset::Type`, no `PresetCollection`, no compatibility
  conditions, no inheritance, no dirty tracking. It is a file with a name and a `values` map, and
  applying it is a `Tab::load_config` call (`src/slic3r/GUI/Tab.cpp:1217-1230`) that diffs, sets, marks
  the tab dirty and reloads the fields — the same state a hand edit produces.
- **The values live on the parts, the set name is a label.** A group can drift from its set (the user
  edits one part directly); the Support-groups panel shows the group as *modified* and offers
  "re-apply the set". This mirrors how presets behave here and keeps the 3MF self-contained.
- **Groups are keyed by resolved config, named for display.** Two differently named groups with
  identical settings collapse to one generator run, and — crucially — a group whose values equal the
  object's collapses into the default group so K stays 1.
- **Per-part Z gap is refused, not faked** (§3.6). The alternative (per-region
  `bottom_is_fully_supported` + per-contact gaps) is the expensive half of research option (b) and is
  explicitly out of scope.
- **Per-part `support_type` (normal vs tree) is out of scope.** It is an either/or at
  `src/libslic3r/PrintObject.cpp:4437-4446`, i.e. two generators on one object, and it roughly doubles
  the merge work (research §1.7, §3(b′)).
- **Per-part interface *filament* is in scope** and reuses the fork's own Chameleon storage:
  `SupportLayer::interface_by_extruder` (`src/libslic3r/Layer.hpp:285`), registered by
  `ToolOrdering::collect_extruders` (`src/libslic3r/GCode/ToolOrdering.cpp:754-760`) and emitted by
  `GCode::process_layer` (`src/libslic3r/GCode.cpp:6627-6690`). Nothing new in `ToolOrdering`, the wipe
  tower or the G-code writer.
- **Chameleon and support groups are mutually exclusive per object.** If
  `support_filament_matching` is on *and* any group specifies an interface filament different from the
  object's, the group assignment wins and Chameleon is skipped for that object with a warning. Both
  write `interface_by_extruder`; letting them both run would make the result depend on ordering.
  Implemented as an extra `continue` in the per-object loop of `chameleon_assign_support_interfaces`
  (`src/libslic3r/Print.cpp:3077-3120`), next to the existing shared-object skip.
- **The "Support groups" panel is a non-modal dialog, not a page in the object parameter tab.**
  `TabPrintModel::build()` prunes every `Line` with no options and every `OptGroup` with no lines
  (`src/slic3r/GUI/Tab.cpp:2994-3006`), so a widget-only row inserted into the shared Support page would
  be deleted for object/part tabs. A dialog dodges that entirely and is where the multi-row list, the
  per-group set picker and the part counts belong anyway.
- **The badge is a suffix in the name column, not a new column.** `ObjectDataViewModel::GetValue`
  renders `case colName: variant << DataViewBitmapText(node->m_name, node->m_bmp);`
  (`src/slic3r/GUI/ObjectDataViewModel.cpp:1783-1784`); appending `"  [" + group + "]"` there costs one
  line and does not disturb rename (`SetName` still writes `m_name`), column widths or the fork's own
  `colVisibility` column (`ObjectDataViewModel.hpp:38-49`).

## 5. Cheap experiments to settle before Stage 3

1. **Does the corpus reproduce byte-for-byte at all?** Run
   `scripts/support_group_identity.py --baseline X --candidate X` (same exe twice) before writing any
   code. If a project is not self-reproducible (thread-order nondeterminism), drop it from the corpus
   and say so. Half a day; it is the foundation of every later gate.
2. **`assert(num_top_contacts <= 1)`** (`src/libslic3r/Support/SupportCommon.cpp:1450`). Confirms that
   splitting *contact layers* into K sibling `SupportGeneratorLayer`s trips a Debug assert in
   `generate_support_layers`. This is why Stage 3 splits contacts **at extrusion time** and not in the
   layer graph. Verify by a one-line experiment on a Debug build before committing to the design.
3. **`boost::container::static_vector<LayerCacheItem, 5>`**
   (`src/libslic3r/Support/SupportCommon.cpp:1606`) has capacity 5. K groups need up to `3 + 2*K`
   entries. Confirm that raising it (or switching to `boost::container::small_vector<…, 5>`) does not
   change the Release output at K==1 — the byte-identity script answers this in one run.
4. **Mask cost.** Time `slice_support_volumes`-equivalent slicing of one group's volumes on the largest
   corpus model. If it is more than ~10 % of support-generation time, cache the masks on the
   `PrintObject` next to `m_support_layers` and clear them in `PrintObject::clear_support_layers`
   (`src/libslic3r/PrintObject.cpp:831`).
5. **`<datadir>/user/<id>/` lifetime.** `PresetBundle::remove_user_presets_directory`
   (`src/libslic3r/PresetBundle.cpp:1152-1165`) does `fs::remove_all` on the whole per-account folder;
   it is currently **commented out** at its only GUI call site
   (`src/slic3r/GUI/GUI_App.cpp:5534`), but `update_user_presets_directory(DEFAULT_USER_FOLDER_NAME)`
   at `GUI_App.cpp:4938` does switch the folder on logout. Decide in Stage 1: either accept that sets
   are per-account (and document it), or read `<datadir>/user/default/support_set/` as a read-only
   fallback when `preset_folder != "default"`. Recommendation: the fallback — one extra directory scan,
   and it makes sets behave the way users will expect.

### Answers — all five settled

1. **Settled in Stage 2 (§2b): no, the corpus does not reproduce byte-for-byte.** Same executable on
   both sides differs on about a third of runs. §3.7's byte identity was replaced by the tolerance
   gate; `tree_classic` was dropped from the corpus because it is not self-reproducible at all.
2. **Settled in Stage 3, empirically, and the answer is yes — it trips.** A Debug build is
   impossible in this tree (`deps/build/OrcaSlicer_dep/usr/local/lib` holds Release import libraries
   only, 1 debug lib out of 54), so the experiment was run by re-enabling `assert()` in the single
   translation unit that holds it: `#undef NDEBUG` + `#include <assert.h>` at the top of
   `SupportCommon.cpp`, in an otherwise ordinary Release build. Undefining `NDEBUG` is ABI-neutral
   (`_ITERATOR_DEBUG_LEVEL` / `_DEBUG` are untouched), so the object still links against the Release
   dependencies. Two runs:
   - **positive control** — feed the per-group contact clones into `top_contacts` before
     `generate_support_layers`, i.e. do exactly the thing the design refuses to do, and run the K==2
     fixture: `Assertion failed: num_top_contacts <= 1, ... SupportCommon.cpp, line 1458`. The
     assumption behind "split contacts at extrusion time" is therefore **measured, not assumed**.
   - **the shipped design** — same assert-enabled build, no probe, both K==2 fixtures plus the whole
     `[SupportGroups]` suite: all pass. `Test::verify_nonempty` (the other `#ifndef NDEBUG` block, at
     the end of `generate_support_toolpaths`) is active in that build too and does not fire. That is
     **Stage 3 gate item 5**, run the only way this tree allows.
3. **Settled: `boost::container::small_vector<LayerCacheItem, 5>`, no output change at K==1.** The
   same inline capacity, so a single-group object still allocates nothing, and it spills to the heap
   beyond it. The off-mode tolerance gate is the proof: green on all 8 cases, three passes.
4. **Settled: the masks are cheap — no caching needed.** On the largest corpus model, with the
   slicer's own `--debug 3` timestamps around `"Support generator - Start"` … `" - End"`, the
   mask + claim prologue measures **1 ms** against a 11–28 ms support-generation window, i.e. well
   under §5.4's 10 % trigger, and the whole window at K==2 is 0.58–0.99× the K==1 baseline over three
   repetitions (**Stage 3 gate item 6** wanted < 1.6×). Caveat, stated plainly: at 20 ms the corpus
   models are too small for the ratio to mean much — run-to-run noise dominates and that is why it
   sometimes lands below 1.0. The number that is solid is the prologue's 1 ms, which is what §5.4
   actually asked about, and `PrintObject::support_group_masks()` returns before slicing anything at
   all when K==1, so off-mode pays exactly zero.
5. **Settled in Stage 1 (§2a deviation 4): the read-only `default` fallback**, as recommended.

---

## Stage 1 — Support sets (UI + storage), no back-end change

Usable on its own: a user can capture the Support page's current values as a named set and re-apply it
to any project on any printer.

### Files and functions

#### 1.1 New: `src/slic3r/GUI/SupportSet.hpp` / `.cpp`

Deliberately in `slic3r/GUI` (not `libslic3r`): it needs `data_dir()`, `AppConfig` and the preset bundle,
and nothing in the back end reads it.

```cpp
struct SupportSet {
    std::string name, description, created, app_version, file;
    std::string interface_filament_type = "same";   // "same" | "soluble" | <filament_type string>
    std::map<std::string, std::string> values;      // opt_serialize strings, §3.2
};

class SupportSetStore {
public:
    static SupportSetStore& instance();
    static std::string dir();                       // <datadir>/user/<preset_folder>/support_set
    static std::string fallback_dir();              // <datadir>/user/default/support_set (read-only)
    void   reload();                                // enumerate both dirs, dedupe by name
    const std::vector<SupportSet>& list() const;
    const SupportSet* find(const std::string& name) const;
    bool   save(const SupportSet& set, std::string* err);          // create or overwrite by name
    bool   rename(const std::string& from, const std::string& to, std::string* err);
    bool   remove(const std::string& name, std::string* err);
};

// Pure, testable helpers (no wx, no data_dir) - these are what tests exercise:
SupportSet   support_set_from_config(const DynamicPrintConfig& cfg, const std::string& name);
void         support_set_apply_to(const SupportSet& set, DynamicPrintConfig& out,
                                  const DynamicPrintConfig& full_config, std::string* warning);
int          resolve_interface_filament(const std::string& type,
                                        const DynamicPrintConfig& full_config, std::string* warning);
std::string  sanitize_support_set_filename(const std::string& name);
const std::vector<std::string>& support_set_keys();   // the allowed-key list of §3.2
nlohmann::json to_json(const SupportSet&);
bool         from_json(const nlohmann::json&, SupportSet&, std::string* err);
```

`support_set_keys()` is built once from `print_config_def`: every key whose
`ConfigOptionDef::category == "Support"` that is also in `PrintObjectConfig().keys()`, minus the
excluded list of §3.2. Building it from the def rather than hard-coding it means a future support key
joins sets automatically.

#### 1.2 `src/slic3r/GUI/Tab.cpp` — the Support-set row

Insert at the top of the Support page, i.e. immediately after
`page = add_options_page(L("Support"), "custom-gcode_support");` (`:2508`) and before
`optgroup = page->new_optgroup(L("Support"), L"param_support");` (`:2509`), a new optgroup carrying one
full-width widget line. The idiom is `Tab::build_preset_description_line`
(`src/slic3r/GUI/Tab.cpp:2123-2169`): `Line line = Line{ "", "" }; line.full_width = 1;
line.append_widget(w); optgroup->append_line(line);` with `widget_t = std::function<wxSizer*(wxWindow*)>`
(`src/slic3r/GUI/OptionsGroup.hpp:32`).

The widget is a horizontal sizer: a `ComboBox`/`wxBitmapComboBox` of set names (plus a leading
"— none —"), then `Apply`, `Save current as…`, `Rename`, `Delete` as `ScalableButton`s (same class the
detach button uses, `Tab.cpp:2130`), then a warning `wxStaticText` for the filament-resolution message.

- **Apply**: `DynamicPrintConfig delta; support_set_apply_to(set, delta, *m_config, &warn);` then
  `this->load_config(delta);` (`Tab.cpp:1217`). `load_config` diffs, sets, calls `update_dirty()`,
  `reload_config()` and `update()` — the preset shows as modified exactly like a hand edit. Nothing else
  to do.
- **Save current as…**: name prompt (`MessageDialog`/`wxTextEntryDialog`), then
  `support_set_from_config(*m_config, name)` and `SupportSetStore::save`.
- **Rename / Delete**: store calls + `reload()` + repopulate the combo. Delete confirms.

Guard the whole row with `if (m_type == Preset::TYPE_PRINT)` so `TabPrintObject`/`TabPrintPart` do not
inherit it (they would prune it anyway, `Tab.cpp:2994-3006`, but be explicit).

#### 1.3 `src/slic3r/CMakeLists.txt`

Add `GUI/SupportSet.cpp` / `GUI/SupportSet.hpp` next to the other `GUI/` sources.

### Data flow

```
<datadir>/user/<id>/support_set/*.json
   └─ SupportSetStore::reload()  ── list ──▶  combo on the Support page
                                   Apply ──▶  support_set_apply_to(set, delta, full_config)
                                                 ├─ values  → delta.set_deserialize(key, value)
                                                 └─ type    → resolve_interface_filament → delta["support_interface_filament"]
                                              Tab::load_config(delta) → m_config, dirty, reload_config
   ◀── Save current as ── support_set_from_config(*m_config, name)
```

### Gate

- New `tests/libslic3r/test_support_set.cpp` (add to `tests/libslic3r/CMakeLists.txt:3-35`) asserting:
  1. `to_json` → `from_json` round trip preserves name, description, type and every value;
  2. `support_set_from_config` captures exactly `support_set_keys()` and never `support_filament` /
     `support_interface_filament` / a `raft_*` key;
  3. `support_set_apply_to` writes exactly the set's keys into `out` and leaves the rest untouched;
  4. `resolve_interface_filament` for each row of §3.3's table including all three fallbacks and the
     `"PLA"` → `"PLA-CF"` prefix case, on a synthetic `DynamicPrintConfig` carrying `filament_type` and
     `filament_soluble` vectors;
  5. `sanitize_support_set_filename` for spaces, slashes, non-ASCII, and the collision suffix;
  6. loading a JSON with an unknown/forbidden key drops that key, keeps the rest, and reports it.
  (These helpers are wx-free, so they belong in the `libslic3r` suite even though the file lives in
  `slic3r/GUI` — link the .cpp into the test target, or move the pure helpers into
  `src/libslic3r/SupportSet.cpp` if the link is awkward. Prefer the latter if it comes up.)
- Manual: save a set on a Snapmaker profile, switch to a Bambu profile with a different filament
  ordering, apply — the interface filament lands on the right slot and the tab shows modified.
- `scripts/support_group_identity.py` green (trivially: no back-end code changed).

### Risks

- **R1.1 — the per-account folder** (§5 item 5). Mitigation: the `default` fallback dir.
- **R1.2 — `Tab::load_config` only fires `update()` when something changed.** Applying a set that
  matches the current values is a silent no-op; that is correct but looks broken. Show a transient
  "already applied" note.
- **R1.3 — `ConfigManipulation::toggle_print_fff_options`** greys out
  `support_interface_spacing` when support ironing is on (`src/slic3r/GUI/ConfigManipulation.cpp:753`)
  and `support_interface_filament` when there are no interface layers (`:744-746`). A set that turns
  ironing on *and* sets a spacing will have the spacing applied but the field greyed. Apply in one
  `load_config` call so the toggles settle once, and accept the greying.

### Effort

**4–6 engineer-days** (store + helpers 2 d, tab row 1.5 d, tests 1 d, the identity script 0.5–1.5 d).

---

## Stage 2 — Group assignment, per-volume keys, 3MF round trip (generator still blind)

After this stage the model, the UI and the file format all know about groups, and the slicer output is
still byte-identical to today. That separation is the point: it lets the 3MF/undo/UI risk land under a
gate that a single script can prove.

### Files and functions

#### 2.1 `src/libslic3r/PrintConfig.cpp` / `.hpp` — the `support_group` key

Exactly §3.1. Nothing goes into `PrintConfig.hpp`'s macro lists and nothing goes into
`s_Preset_print_options` (`src/libslic3r/Preset.cpp:871`).

#### 2.2 `src/slic3r/GUI/GUI_Factories.cpp` — open parts to support settings, fix the dead guard

**`is_improper_category` (`:84-89`)** — today's third clause tests `category == "Support material"`,
a string no option carries (every support key's category is `"Support"`,
`src/libslic3r/PrintConfig.cpp:6000` and 57 more), so it never fires. Make it key-aware:

```cpp
// Ultra: the guard used to test category == "Support material" - a string no option carries
// (every support key's category is "Support", PrintConfig.cpp:6000 and 57 more), so the clause
// was a dead no-op and the real gate was SettingsFactory::get_options. It is now key-aware: a
// part may carry the curated support-group keys and nothing else from the "Support" category.
static bool is_improper_category(const std::string& category, const int filaments_cnt,
                                 const bool is_object_settings = true,
                                 const std::string& opt_key = std::string())
{
    return  category.empty() ||
        (filaments_cnt == 1 && (category == "Extruders" || category == "Wipe options")) ||
        (!is_object_settings && category == "Support" && !SettingsFactory::is_part_support_key(opt_key));
}
```

Both call sites have the key in hand: `SettingsFactory::get_bundle` (`:281`, inside the
`for (auto& opt_key : opt_keys)` loop) and `get_full_settings_hierarchy` (`:385`, inside
`for (auto& option : options)`).

**`SettingsFactory::get_options(bool is_part)` (`:160-176`)** — append the curated tier A+B list for
parts:

```cpp
    PrintRegionConfig reg_config;
    auto options = reg_config.keys();
    if (!is_part) {
        PrintObjectConfig obj_config;
        std::vector<std::string> obj_options = obj_config.keys();
        options.insert(options.end(), obj_options.begin(), obj_options.end());
    } else {
        // Ultra (support groups): a part may carry the curated part-level support keys
        // (docs/superpowers/plans/2026-09-02-support-sets-and-groups.md §3.5). They are
        // PrintObjectConfig keys, so apply_to_print_region_config (PrintObject.cpp:3264)
        // still drops them - PrintObject::support_groups() reads them from the volume config
        // directly instead.
        const auto& sup = SettingsFactory::part_support_keys();
        options.insert(options.end(), sup.begin(), sup.end());
    }
```

`part_support_keys()` / `is_part_support_key()` are new statics on `SettingsFactory` holding §3.5's
tier A + tier B lists.

**`PART_CATEGORY_SETTINGS` (`:143-158`)** — add a `L("Support")` group listing the same keys with sort
orders, so the curated part list shows in the same table style as Quality/Strength/Speed. (Stage 5 can
polish the ordering; the group must exist here for the settings node to look right.)

**New `MenuFactory::append_menu_items_support_group(wxMenu*)`** — model it line by line on
`append_menu_items_visibility` (`:1162-1213`), which is the fork's own precedent for a per-part submenu
that works on a multi-selection:

- collect the selected `ModelVolume*`s the same way `selected_object_ids` does (`:1164-1185`), but keep
  the volumes, not ids, and only `MODEL_PART` ones;
- build a submenu: `Default` (clears `support_group` and the resolved keys), then one radio-ish item per
  existing group on that object, then a separator, then `New group…`;
- `New group…` opens a small dialog: name + a `SupportSetStore` picker + the four required knobs
  (interface filament type, top Z distance, interface layers, interface pattern/spacing) prefilled from
  the chosen set;
- applying writes, on every selected volume's `ModelConfig`: `support_group` = name, plus the resolved
  tier A/B values from the set (resolved through `resolve_interface_filament` against the *current*
  full config, §3.3);
- wrap in `take_snapshot(_L("Support group assigned"))` (the idiom at
  `src/slic3r/GUI/GUI_ObjectList.cpp:1952` and `:1993`) and finish with
  `ObjectList::object_config_options_changed({volume->get_object(), volume})`
  (`GUI_ObjectList.cpp:784-812`) and `ObjectList::changed_object()` (`:3505-3508`), which is what
  triggers the re-slice.

Register it in `create_bbl_part_menu` (`:1591-1625`, next to `append_menu_items_visibility` at `:1603`),
in `part_menu()` (`:1911-1916`) and in `multi_selection_menu()`'s `multi_volume` branch (`:1994-2000`,
next to the visibility call at `:1998`). Add an object-level `Support groups…` entry that opens the panel
in `create_extra_object_menu` / `object_menu()` (`:1448`, `:1889`).

#### 2.3 `src/slic3r/GUI/Tab.cpp` — the part parameter panel

`TabPrintPart::TabPrintPart` is constructed with `PrintRegionConfig().keys()`
(`src/slic3r/GUI/Tab.cpp:3459-3463`). Widen it:

```cpp
TabPrintPart::TabPrintPart(ParamsPanel* parent) :
    // Ultra (support groups): parts also carry the curated part-level support keys, so the
    // part parameter panel must render them - see GUI_Factories.cpp's part_support_keys().
    TabPrintModel(parent, concat(PrintRegionConfig().keys(), SettingsFactory::part_support_keys()))
```

`TabPrintObject` (`:3445-3449`) already has all `PrintObjectConfig` keys and needs no change.
`TabPrintLayer` (`:3472-3476`) must **not** be widened — support settings on a height range are not in
scope.

#### 2.4 `src/slic3r/GUI/ObjectDataViewModel.{hpp,cpp}` — the badge

- `ObjectDataViewModelNode`: add `wxString m_support_group;` next to
  `wxString m_extruder = wxEmptyString;` (`src/slic3r/GUI/ObjectDataViewModel.hpp:89`), and copy it in
  the node-copy block at `:265` alongside `m_extruder = from_node.m_extruder;`.
- `ObjectDataViewModel::GetValue`, `case colName` (`src/slic3r/GUI/ObjectDataViewModel.cpp:1783-1784`):

  ```cpp
  case colName:
      // Ultra (support groups): show the part's support group as a suffix badge. m_name is
      // untouched, so rename/SetName and every other consumer are unaffected.
      variant << DataViewBitmapText(node->m_support_group.IsEmpty() ? node->m_name
                                    : node->m_name + "  [" + node->m_support_group + "]", node->m_bmp);
      break;
  ```
- New `ObjectList::update_support_group_badges()` — a near-copy of
  `ObjectList::update_visibility_icons` (`src/slic3r/GUI/GUI_ObjectList.cpp:5672-5694`): walk objects and
  volumes, read each volume's `support_group`, set the node field, `m_objects_model->ItemChanged(item)`.
  Call it from the menu handler and from `ObjectList::load_config`/project-load paths.

#### 2.5 New: `src/slic3r/GUI/SupportGroupsDialog.{hpp,cpp}` — the panel

Non-modal dialog for one `ModelObject` (opened from the object context menu). Rows: group name, the
support set it references, part count, and the four required knobs inline (interface filament type, top
Z distance, interface layers, interface pattern + spacing). Buttons: `New`, `Rename`, `Delete`
(delete puts its parts back in the default group), `Re-apply set` (rewrites the resolved values on the
group's parts), `Select parts`. A group whose parts' values differ from its set is shown as *modified*.

Row 0 is always the default group ("Object settings"), read-only, with the object's own values and the
count of ungrouped parts.

#### 2.6 `src/libslic3r/PrintApply.cpp` — invalidation

Add `model_support_group_data_changed` and extend the condition at `:1646`, exactly as §3.8 specifies.
Place the helper next to `model_custom_supports_data_changed` (same file, near `:58-100`).

#### 2.7 `src/libslic3r/Print.hpp` / `PrintObject.cpp` — the resolver (no consumer yet)

Add `struct SupportGroup` and `std::vector<SupportGroup> PrintObject::support_groups() const`
(§3.4), plus `static bool support_groups_want_soluble(const ModelObject&)` used by
`object_config_from_model_object` (`src/libslic3r/PrintObject.cpp:3230-3242`, §3.6). Fill only `name`,
`config` and `volumes`; leave `mask` empty until Stage 3. Nothing calls `support_groups()` from the
generator yet — this stage only proves the resolution is right and that K==1 for the whole corpus.

### Data flow

```
Object-List menu / SupportGroupsDialog
   └─ writes ModelVolume::config: support_group + resolved tier A/B keys  (take_snapshot → undo/redo)
        ├─ ObjectDataViewModel badge         (display)
        ├─ SettingsFactory::get_bundle       → settings node under the part shows the resolved values
        ├─ TabPrintPart                      → part parameter panel renders them
        ├─ bbs_3mf export :7631-7634         → <metadata key="support_group" value="B"/> + the values
        │   bbs_3mf import :4899 / :5047     → volume->config.set_deserialize (unknown in stock Orca → dropped)
        └─ PrintApply.cpp:1646               → invalidate_step(posSupportMaterial)
             PrintApply.cpp:1671             → object_config_from_model_object → soluble rule → posSlice when it flips
                  PrintObject::support_groups()  ── computed, unused (Stage 3 consumes it)
```

### Gate

- `tests/libslic3r/test_3mf.cpp`: a new SCENARIO builds a two-volume `ModelObject`, sets
  `support_group = "B"` plus two tier-A values on volume 1, `store_3mf` + `load_3mf`, asserts both
  volumes' configs come back identical (the file already has the `store_3mf`/`load_3mf` idiom at
  `:52-60`; `test_color_split.cpp:1454-1456` and `test_mixed_filament.cpp:4677-4678` are the
  multi-volume fixture precedents).
- New `tests/fff_print/test_support_groups.cpp` (add to `tests/fff_print/CMakeLists.txt:3-18`):
  1. two parts, no overrides → `support_groups().size() == 1`;
  2. one part overrides `support_interface_top_layers` → `size() == 2`, the default group first, the
     override group carries the right volume;
  3. one part overrides a key to the *same value the object already has* → `size() == 1`
     (this is the off-mode pin);
  4. two parts with different names but identical values → `size() == 2`? **No** — assert `size() == 1`
     and that the surviving group's `name` is the first non-empty one (§3.4 step 3);
  5. a part with `support_top_z_distance = 0` on an object with a non-zero value →
     `print.objects().front()->config().support_top_z_distance == 0` (the soluble rule, §3.6);
  6. `support_group` on a **modifier** or an enforcer volume is ignored (`is_model_part()` only).
- Manual: assign a group, save the project, reopen — badge, panel and part panel all agree.
- `scripts/support_group_identity.py` green on the whole corpus **plus** a project carrying groups
  (the generator must still ignore them at this stage).

### Risks

- **R2.1 — the settings node appears where it did not before.** `get_bundle` now returns a `"Support"`
  bundle for a part carrying group values (`GUI_Factories.cpp:258-292`), so `object_config_options_changed`
  (`GUI_ObjectList.cpp:784-812`) adds a settings child under the part. That is the intended behaviour but
  it changes the tree for anyone who was already forcing support keys onto volumes by hand-editing a
  3MF. Acceptable; mention it in the docs.
- **R2.2 — clipboard paste.** `ObjectList::paste_settings_into_list`
  (`GUI_ObjectList.cpp:1279-1308`) filters against `get_options(true)`, which now includes the support
  keys — so pasting a part's settings copies its group's values but **not** `support_group` (which is
  deliberately not in the list). Decide and document: the pasted part gets the values but no group
  label, and the group resolver folds it into whichever group has the same values. That is coherent;
  just do not let it be an accident.
- **R2.3 — the dead frequent-settings path.** `create_freq_settings_popupmenu`
  (`GUI_Factories.cpp:447-481`) would offer the whole `"Support"` bundle
  (`FREQ_SETTINGS_BUNDLE_FFF`, `:101-105`) on a part, and
  `add_category_to_settings_from_frequent` (`GUI_ObjectList.cpp:1980-2014`) does **no** legality
  filtering. The call is commented out at `GUI_Factories.cpp:794`. Leave it commented; if anyone
  re-enables it, it must filter through `get_options(is_part)` first. Add a comment saying so.
- **R2.4 — undo/redo.** `ModelConfig::set_key_value` bumps the timestamp
  (`PrintConfig.hpp:2064`), and `ModelVolume`'s config is already part of the undo/redo snapshot
  (`Model.hpp:72-102`), so a `take_snapshot` around the write is all that is needed. Verify by
  assigning a group, Ctrl+Z, and checking the badge, the part panel and `support_groups()` all revert.

### Effort

**7–10 engineer-days** (key + resolver + invalidation 2 d, menus 2 d, badge 0.5 d, dialog 2–3 d, tests
1.5–2 d).

---

## Stage 3 — Interface-only support groups in the normal support generator

> **Shipped 2026-09-04 on `feat/support-sets-stage3`. Read §2c for what was actually built, the
> deviations, and both gates' results. The design below is what was implemented, with the deviations
> §2c lists.**

The shared geometry pipeline runs **once, unchanged** (so contacts, bases and columns are bit-for-bit
what they are today); only the *interface* stage and the *interface toolpaths* become per-group. This is
research option (d) built on option (b′)'s spine: per-group resolved configs, per-group
`SupportParameters`, per-group masks. Escalating later to K whole-generator runs needs no data-model
change.

### Why not K full generator runs in this stage

`generate_support_layers` asserts `num_top_contacts <= 1` per print_z
(`src/libslic3r/Support/SupportCommon.cpp:1450`), so K sibling `TopContact` layers at one Z trip a Debug
assert. K full runs also produce K overlapping base column sets that must be arbitrated by trimming
group *n* against groups `< n` — real work, real risk, and none of it is needed for interface keys,
which are read in exactly two places (verified: `support_interface_*` appears **nowhere** in
`SupportMaterial.cpp`; it is consumed only by `SupportParameters`' constructor
(`src/libslic3r/Support/SupportParameters.hpp:27-45, 103-133`), by `generate_interface_layers`
(`SupportCommon.cpp:126-313`) and by `generate_support_toolpaths`
(`SupportCommon.cpp:1496, 1638, 1658, 1690, 1713, 1771-1775`)).

### Changes

#### 3.1 `src/libslic3r/PrintObjectSlice.cpp` — factor out the slicing helper

`PrintObject::slice_support_volumes(ModelVolumeType)` (`:5607-5657`) already does exactly the needed
work: slice a set of volumes at `zs_from_layers(this->layers())` with `trafo_centered()`, then merge and
union per layer. Split it:

```cpp
// New, in Print.hpp next to slice_support_volumes (:524-526):
std::vector<Polygons> slice_volumes_at_layers(const std::vector<const ModelVolume*> &volumes) const;
```

and rewrite `slice_support_volumes(type)` as a two-liner that collects the volumes of `type` and calls
it. The body moves verbatim, so enforcer/blocker behaviour is unchanged by construction — the corpus
byte-identity run is the proof.

#### 3.2 `src/libslic3r/PrintObject.cpp` — masks

Fill `SupportGroup::mask` in a new `PrintObject::support_group_masks(std::vector<SupportGroup>&) const`
called from `_generate_support_material` (`:4436-4447`). Group 0 (the default group) gets the
**complement**: `diff(all model-part slices at layer i, union of groups 1..K-1 masks at layer i)`, so the
K masks partition the object footprint with no overlap and no gap. Cache on the `PrintObject` next to
`m_support_layers` and clear in `clear_support_layers` (`:831`) if experiment §5.4 says the slicing is
expensive.

#### 3.3 `src/libslic3r/Support/SupportParameters.hpp` — a config-taking constructor

Today the only constructor is `SupportParameters(const PrintObject& object)` (`:11`), which reads
`object.config()` for ~40 fields and calls `Slic3r::support_material_flow(&object, …)` /
`support_material_interface_flow(&object, …)` (`src/libslic3r/Flow.cpp:214, 232, 244`), which read
`object->config()` again. Add:

```cpp
    SupportParameters(const PrintObject& object) : SupportParameters(object, object.config()) {}
    SupportParameters(const PrintObject& object, const PrintObjectConfig& object_config);
```

The body moves to the two-argument form with `object_config` as a parameter instead of a local; the
three flow helpers gain a `const PrintObjectConfig&` overload in `Flow.hpp:142-145` / `Flow.cpp:214-252`
(the existing ones forward with `object->config()`). Mechanical, and the delegating constructor keeps
every one of the ~123 existing call sites byte-identical.

#### 3.4 `src/libslic3r/Support/SupportLayer.hpp` — one scalar tag

```cpp
	// Ultra (support groups): index into PrintObject::support_groups() whose interface parameters
	// this layer was generated with. 0 for every layer of a single-group object, so nothing about a
	// single-group object changes. Only interface / base-interface layers ever carry a non-zero
	// value; contacts, bases and rafts are shared and stay 0.
	uint16_t support_group { 0 };
```

Next to `bridging` (`:100`). `merge()` (`:66-79`) needs no change: interface layers of different groups
live in different vectors and are never merged into one another. `reset()` (`:41-43`) restores 0 via
`*this = SupportGeneratorLayer()`.

#### 3.5 `src/libslic3r/Support/SupportMaterial.cpp` — the per-group interface call

`PrintObjectSupportMaterial::generate` (`:373-560`). Replace the single call at `:481`:

```cpp
    auto [interface_layers, base_interface_layers] = generate_interface_layers(
        *m_object_config, m_support_params, bottom_contacts, top_contacts, empty_layers, empty_layers,
        intermediate_layers, layer_storage);
```

with:

```cpp
    // Ultra (support groups): the shared pipeline above ran once and is untouched. Only the
    // interface stage is per group: each group projects ONLY the contacts over its own parts,
    // with its own SupportParameters, and carves its interface out of what previous groups left
    // in intermediate_layers (generate_interface_layers already subtracts at SupportCommon.cpp:185).
    // With a single group this is literally the call it replaced - see the plan's §3.7.
    SupportGeneratorLayersPtr interface_layers, base_interface_layers;
    if (m_groups.size() == 1) {
        std::tie(interface_layers, base_interface_layers) = generate_interface_layers(
            *m_object_config, m_support_params, bottom_contacts, top_contacts,
            empty_layers, empty_layers, intermediate_layers, layer_storage);
    } else {
        for (size_t g = 0; g < m_groups.size(); ++ g) {
            const SupportParameters &params_g = m_group_params[g];
            if (! params_g.has_interfaces())
                continue;
            SupportGeneratorLayersPtr top_g    = clone_contacts_masked(top_contacts,    m_groups[g], layer_storage, /*use_above*/ true);
            SupportGeneratorLayersPtr bottom_g = clone_contacts_masked(bottom_contacts, m_groups[g], layer_storage, /*use_above*/ false);
            auto [iface_g, base_iface_g] = generate_interface_layers(
                m_groups[g].config, params_g, bottom_g, top_g, empty_layers, empty_layers,
                intermediate_layers, layer_storage);
            for (SupportGeneratorLayer *l : iface_g)      l->support_group = uint16_t(g);
            for (SupportGeneratorLayer *l : base_iface_g) l->support_group = uint16_t(g);
            append(interface_layers, iface_g);
            append(base_interface_layers, base_iface_g);
        }
        std::sort(interface_layers.begin(), interface_layers.end(),
                  [](auto *l, auto *r) { return *l < *r; });
        std::sort(base_interface_layers.begin(), base_interface_layers.end(),
                  [](auto *l, auto *r) { return *l < *r; });
    }
```

`clone_contacts_masked` is a new static in the same file. For each contact layer it allocates a sibling in
`layer_storage`, copies `print_z / bottom_z / height / bridging / layer_type / idx_object_layer_*`, and
intersects `polygons`, `contact_polygons`, `overhang_polygons` and `enforcer_polygons` with
`expand(group.mask[idx], e)`, where `idx` is `idx_object_layer_above` for top contacts and
`idx_object_layer_below` for bottom contacts (**both already exist and are already set** —
`SupportLayer.hpp:95-99`, written at `SupportMaterial.cpp:1801, 1819` for top contacts and `:2437` for
bottom contacts), falling back to a nearest-`print_z` lookup when the index is `size_t(-1)` (which
happens for layers produced by `merge_contact_layers`, `:2363`). `e = scale_(m_support_params.gap_xy) +
m_support_params.support_material_interface_flow.scaled_width()`. Layers that come out empty are
dropped.

`m_groups` / `m_group_params` are members filled at the top of `generate` from
`object.support_groups()`. The object-level `m_support_params` (built in the constructor at `:331-338`)
must be constructed from a **shared config** = the object config with
`support_interface_top_layers` and `support_interface_bottom_layers` replaced by the **max over all
groups**. Reason: `m_support_params.support_material_bottom_interface_flow` decides bottom-contact layer
heights (`:2427`), and `SupportParameters` collapses `support_material_interface_flow` to
`support_material_flow` when `support_interface_top_layers == 0`
(`SupportParameters.hpp:63-66` and `:110-113`). Taking the max keeps the interface flow alive whenever any
group wants interfaces, and equals the object's own value when K==1 → byte identity.

`support_interface_filament` in the shared config stays the **object's** value, so bottom-contact layer
heights never move because of a group's filament choice.

#### 3.6 `src/libslic3r/Support/SupportCommon.{hpp,cpp}` — per-group interface toolpaths

`generate_support_toolpaths` (`SupportCommon.hpp:70-80`, `SupportCommon.cpp:1482-1966`) gains one
trailing defaulted parameter:

```cpp
// Ultra (support groups): per-group interface parameters + per-object-layer masks. nullptr (the
// default) is exactly today's single-config behaviour; every call site that does not pass it is
// unchanged.
struct SupportGroupToolpaths {
    const PrintObjectConfig *config;
    const SupportParameters *params;
    const std::vector<Polygons> *mask;      // per object layer
    int                      interface_filament;  // 1-based; 0 = same as the object
};
void generate_support_toolpaths(..., const std::vector<SupportGroupToolpaths> *groups = nullptr,
                                const std::vector<coordf_t> *object_layer_zs = nullptr);
```

Four edits inside the function, all `groups == nullptr`-guarded:

1. **`LayerCache`** (`:1600-1618`): `boost::container::static_vector<LayerCacheItem, 5> nonempty;`
   has capacity 5 but K groups need up to `3 + 2*K`. Change to
   `boost::container::small_vector<LayerCacheItem, 5>` (same inline capacity, heap spill beyond) and
   add `std::vector<SupportGeneratorLayerExtruded> extra_interface_layers;` used only in group mode.
   `add_nonempty_and_sort` (`:1611-1617`) also walks `extra_interface_layers`.
2. **Interface-layer lookup** (`:1673-1682`): today `idx_layer_interface` picks the single interface
   layer at this print_z. In group mode, collect **all** interface (and base-interface) layers within
   `EPSILON` of `support_layer.print_z` into `extra_interface_layers`, keeping the lowest-group one in
   `layer_cache.interface_layer` so the existing merge logic at `:1707-1719` behaves as before.
3. **`extrude_interface`** (the lambda at `:1737-1770` and its three calls at `:1773-1775`): in group
   mode, wrap each call in a loop over groups. For an interface layer, the group is
   `layer_ex.layer->support_group` and the polygons are used whole. For a **contact** layer (shared, tag
   0) the polygons are split at extrusion time: `intersection(layer_ex.polygons_to_extrude(),
   group.mask[idx])` with `idx` from `idx_object_layer_above` / `idx_object_layer_below`, group 0 taking
   the remainder. Each piece uses its group's `filler_interface` (built from
   `params_g.contact_fill_pattern`), `params_g.interface_density`,
   `params_g.support_material_interface_flow` and `support_interface_angle` computed from
   `cfg_g.support_interface_pattern` (`:1658-1659`). With `groups == nullptr` the loop is skipped and
   the original single call runs — byte identity.
4. **Per-group interface filament**: when `group.interface_filament != 0` and differs from the object's,
   the group's freshly generated entities go into a local collection which is then moved into
   `support_layer.interface_by_extruder[interface_filament - 1]`
   (`src/libslic3r/Layer.hpp:285`) instead of `layer_ex.extrusions`. From there
   `ToolOrdering::collect_extruders` registers the extruder on that layer
   (`src/libslic3r/GCode/ToolOrdering.cpp:754-760`) and `GCode` emits it after the toolchange
   (`src/libslic3r/GCode.cpp:6627-6690`). Nothing new is needed in `ToolOrdering`, the wipe tower or
   `GCode`. Do this **after** `modulate_extrusion_by_overlapping_layers` at `:1906-1907`, so the
   height modulation still applies.

The `config.support_interface_top_layers == 0` / `support_interface_bottom_layers` branches at
`:1690-1719` and `:1771-1772` keep reading the **object** config: those decide *merging* of contacts into
base layers, which is shared geometry. If a group sets 0 while the object does not, that group's
interface simply has zero layers and its contacts extrude as interface with the group's pattern — a
coherent, documented outcome.

#### 3.7 `src/libslic3r/Print.cpp` — the Chameleon interlock

In `chameleon_assign_support_interfaces` (`:3077-3120`), next to the shared-object skip at `:3113-3114`:

```cpp
        // Ultra (support groups): a group that pins its own support-interface filament writes the same
        // SupportLayer::interface_by_extruder map this pass owns. Letting both run makes the result
        // depend on ordering, so the explicit per-part assignment wins and Chameleon skips the object.
        if (object_has_support_group_interface_filament(*object)) {
            object->active_step_add_warning(PrintStateBase::WarningLevel::NON_CRITICAL,
                _u8L("Support filament matching is off for this object because one of its support groups picks its own interface filament."));
            continue;
        }
```

### Data flow

```
PrintObject::_generate_support_material (PrintObject.cpp:4436)
  └─ support_groups() (§3.4) + support_group_masks() (slice_volumes_at_layers)
       PrintObjectSupportMaterial::generate (SupportMaterial.cpp:373)
         ├─ [SHARED, unchanged] buildplate_covered, top_contact_layers, bottom_contacts,
         │   raft_and_intermediate, trim_support_layers_by_object, generate_base_layers,
         │   trim_top_contacts_by_bottom_contacts
         ├─ [PER GROUP] clone_contacts_masked → generate_interface_layers(cfg_g, params_g, …)
         │      carving intermediate_layers in group order; result tagged support_group = g
         ├─ generate_raft_base, generate_support_layers          [SHARED, unchanged]
         └─ generate_support_toolpaths(..., groups)
              ├─ interface layers: filled with their own group's filler/flow/angle/density
              ├─ contact layers:   split by mask at extrusion time, same per-group parameters
              └─ group interface filament ≠ object's → SupportLayer::interface_by_extruder[e]
                    → ToolOrdering::collect_extruders (:754) → GCode::process_layer (:6627)
```

### Gate

1. `scripts/support_group_identity.py` green on the whole corpus (K==1 everywhere) — the hard gate.
2. `tests/fff_print/test_support_material.cpp`, new `TEST_CASE("SupportMaterial: per-group interface",
   "[SupportMaterial][support_groups]")`: build a `ModelObject` with two side-by-side part volumes over
   an overhang (the `test_color_split.cpp:1454-1456` / `test_mixed_filament.cpp:4677-4678` fixture
   shape), give part B `support_interface_top_layers = 5` and `support_interface_spacing = 0` while the
   object has `2` and `0.5`, slice, then sum `erSupportMaterialInterface` path length per
   `SupportLayer::support_fills` restricted to each part's XY bbox. Assert: B's interface length grows
   by >30 %, A's is within 1 % of a control run with no groups. (The role-filtering idiom is the
   existing `[SupportMaterial][chameleon]` test at `test_support_material.cpp:51-98`.)
3. Same fixture with `support_interface_filament` differing on B: assert
   `support_layers()[i]->interface_by_extruder` is non-empty for the layers under B and empty elsewhere,
   and that `ToolOrdering` lists that extruder on those layers.
4. Re-enable `tests/fff_print/test_support_material.cpp`'s `#if 0` blocks: the "forced support is
   generated" scenario at `:201-260` and "Checking bridge speed" at `:262-317`. They do not compile
   today (`init_print` API drift, `Slic3r::Test::` helpers). Fixing them is ~0.5 d each and gives the
   suite an actual geometry assertion, which it currently lacks entirely.
5. `SLIC3R_DEBUG`/Debug build of the K==2 fixture must not trip
   `assert(num_top_contacts <= 1)` (`SupportCommon.cpp:1450`) or the
   `Test::verify_nonempty` assertion at `SupportCommon.cpp:1945-1964`.
6. Timing: support generation on the largest corpus model at K==2 within 1.6× the K==1 baseline.

### Risks

- **R3.1 — contact-layer attribution at group boundaries.** Where two parts' overhangs merge into one
  contact polygon, the split is a hard clip along the mask boundary expanded by `e`. Expect a visible
  seam in the interface pattern there. Mitigation: the expansion `e` and the group order are the two
  knobs; make `e` a named constant with a comment, and take the screenshot into the docs so the
  behaviour is documented rather than surprising.
- **R3.2 — `merge_contact_layers`** (`SupportMaterial.cpp:2363`) merges contacts closer than
  `support_layer_height_min`, and `SupportGeneratorLayer::merge` (`SupportLayer.hpp:66-79`) does not
  merge `idx_object_layer_above`. A merged layer keeps the surviving one's index. The nearest-`print_z`
  fallback in `clone_contacts_masked` covers it; assert in Debug that the fallback fires rarely.
- **R3.3 — `LayerCache::nonempty` capacity** (§5 item 3). Cap K at 8 groups per object in the resolver
  and say so in the UI; beyond that, merge the smallest groups into the default group with a warning.
- **R3.4 — flows across a dual-nozzle printer.** `support_material_interface_flow` reads
  `nozzle_diameter.get_at(support_interface_filament - 1)` (`src/libslic3r/Flow.cpp:244-251`). A group
  whose interface filament sits on a different nozzle gets a different interface flow *width* — correct,
  but it means the group's interface may not tile with the object's. Warn in the Support-groups panel
  when the resolved slot's nozzle diameter differs from the object's.
- **R3.5 — `interface_by_extruder` interacts with `flush_into_support`.** `WipingExtrusions` can claim
  support extrusions (`src/libslic3r/GCode.cpp:5532-5548`); the fork's own
  `WipingExtrusions::is_support_overriddable` (`src/libslic3r/GCode/ToolOrdering.cpp:1859`) already
  pins this off for Chameleon-active objects at `:1882-1883`
  (`if (object.config().support_filament_matching.value) return false;`). Extend the same predicate to
  "this object has a support group with its own interface filament" so a purge cannot repaint a
  deliberately-chosen interface colour. The existing test at `test_support_material.cpp:51-98` is the
  template.
- **R3.6 — group order determinism.** Group order comes from `ModelObject::volumes` order (§3.4). Any
  operation that reorders volumes changes which group carves `intermediate_layers` first, which changes
  output. Document it; it is the same class of dependency the slicer already has on volume order via
  `clip_multipart_objects` (`src/libslic3r/PrintObjectSlice.cpp:30, 420-437`).

### Effort

**12–18 engineer-days** (masks + `slice_volumes_at_layers` 1.5 d, `SupportParameters` config ctor +
flow overloads 1.5 d, `clone_contacts_masked` + per-group interface call 3 d,
`generate_support_toolpaths` per-group interface + contact split 4–6 d, interface filament +
interlock 2 d, tests and the re-enabled `#if 0` blocks 2–3 d, byte-identity chasing 1–2 d).

---

## Stage 4 — Tree support parity

Two different code paths, two different answers. Be explicit about both in the UI.

### 4a. Organic trees (`smsTreeOrganic`, Orca's default) — full parity, same seam

`TreeSupport3D::generate_support_areas` builds `SupportParameters support_params(print_object);`
(`src/libslic3r/Support/TreeSupport3D.cpp:3433`), calls
`generate_interface_layers(print_object.config(), support_params, …)` (`:3516-3517`) and
`generate_support_toolpaths(print_object.support_layers(), print_object.config(), support_params, …)`
(`:3554-3555`) — **the same two functions Stage 3 already made group-aware.** So Stage 4a is:

- construct the shared `support_params` from the max-interface-layers shared config (§3.5 of Stage 3);
- replace the single `generate_interface_layers` call with the same per-group loop, masking the
  contacts with `clone_contacts_masked` (organic trees set `top_contacts` through
  `interface_placer`/`organic_draw_branches`, so `idx_object_layer_above` may be unset — rely on the
  nearest-`print_z` fallback and verify);
- pass the `groups` vector to `generate_support_toolpaths`.

Also handle the soluble static: `group_meshes` sets `TreeSupportSettings::soluble = true` from any
object with `support_top_z_distance < EPSILON` (`TreeSupport3D.cpp:130-135` →
`TreeSupportCommon.hpp:361`). §3.6 already forces the object's own `support_top_z_distance` to 0 when
any group is soluble, so this needs **no change** — but add a comment at `TreeSupport3D.cpp:132` saying
that the object config it reads is already group-aware, or the next person will "fix" it.

### 4b. Classic trees (`smsTreeSlim` / `TreeStrong` / `TreeHybrid`) — interface *fill* only

`TreeSupport` (`src/libslic3r/Support/TreeSupport.cpp:608-620`) does not use
`generate_interface_layers`. It computes roof geometry itself in `draw_circles`
(`:1912-2130`, `roof_areas` / `roof_1st_layer` on the `SupportLayer`) and fills it in
`generate_toolpaths` (`:1318-1560`, interface fills at `:1422, 1479, 1509, 1521`).

Scope for this stage:

- **In:** per-group interface *pattern*, *spacing*, *density*, *ironing* and *filament*, by intersecting
  `ts_layer->roof_areas` / `roof_1st_layer` with the group masks inside `generate_toolpaths` and filling
  each piece with that group's `SupportParameters` (the per-group `SupportParameters` copy idiom already
  exists there at `:1551-1554`).
- **Out, and it must be said in the UI:** per-group interface *layer count*. The number of roof layers
  is decided during influence-area propagation in `draw_circles`, object-wide. A group setting
  `support_interface_top_layers` on a classic-tree object shows an inline notice:
  *"Interface layer count is object-wide for classic tree supports; use organic trees or normal supports
  for per-group interface layers."*

### Gate

- `scripts/support_group_identity.py` green on the corpus with `support_type = tree(auto)` and both
  `support_style = organic` and `support_style = tree slim`.
- Organic-tree version of Stage 3's two-part fixture: same assertions.
- Classic-tree fixture: per-group roof pattern/spacing visible in `support_fills`
  (`erSupportMaterialInterface` path spacing differs above B), interface layer count identical above
  both parts, and the notice present.
- Debug build: `TreeSupportSettings::soluble` is `true` exactly when some object has
  `support_top_z_distance == 0` after the §3.6 rule.

### Risks

- **R4.1 — `idx_object_layer_above` on organic-tree contacts.** Contacts made by
  `organic_draw_branches` / `interface_placer` may leave it unset. Settle with a one-hour experiment
  (a Debug counter on the fallback path) before writing the masking code.
- **R4.2 — `TreeSupportSettings::soluble` is a process-global static.** Two objects on one plate, one
  soluble, one not, already share it today; groups do not make it worse but do make it easier to hit.
  Add a `BOOST_LOG_TRIVIAL(warning)` when it is set from one object while another has a non-zero gap.
- **R4.3 — tree base geometry is *not* group-aware**, so a group's interface can sit on a branch grown
  under the object's own parameters. That is intended for this stage; say so.

### Effort

**8–12 engineer-days** (organic 3–5 d, classic-tree roof fill 3–5 d, tests 2 d).

---

## Stage 5 — Polish

- **Preview colouring by filament.** The per-group interface already extrudes on its own extruder via
  `interface_by_extruder`, so the G-code viewer colours it correctly once the toolchange is scheduled —
  verify on the two-part fixture and fix if the preview's support-role colouring overrides the tool
  colour. This is a *check*, not necessarily a change; budget for a small fix.
- **Warnings** (all `active_step_add_warning(NON_CRITICAL, …)` or inline panel text, never modal):
  a group with no parts; a group whose referenced support set no longer exists on this machine (the
  values still apply — say so); an unresolvable interface filament type (§3.3); a group asking for a
  non-zero Z gap on an object made soluble by another group (§3.6); a group whose interface filament
  sits on a different nozzle (R3.4); K capped at 8 (R3.3); classic-tree interface-layer-count notice
  (Stage 4b).
- **`PART_CATEGORY_SETTINGS` "Support" group** ordering and labels (`GUI_Factories.cpp:143-158`),
  tooltips for `support_group`, and the "Support groups…" object menu entry icon.
- **Docs**: `docs/superpowers/specs/2026-09-02-support-sets-and-groups.md` — what a set is and where it
  lives, the interface-filament resolution table, the group model, the soluble rule, the classic-tree
  limitation, and the "opens in stock Orca, the keys are dropped" note.

### Effort

**3–5 engineer-days.**

---

## Things that must NOT change

None of this work touches the fork's remote/LAN/phone/hub stack. If a diff in these files appears in a
support-groups branch, it is a mistake:

- `src/slic3r/GUI/RemoteAccess.{hpp,cpp}` — the per-instance loopback API, the dialog policy hook, the
  attention machinery (`docs/superpowers/plans/2026-09-02-hidden-service-mode.md` §2c).
- `src/slic3r/GUI/RemoteHub.{hpp,cpp}` — the hub server, tray icon, instance registry,
  `spawn_slicer`, the Tailscale relay.
- `src/slic3r/GUI/StreamPanel.{hpp,cpp}` and `resources/web/orca/*` (`hub.html`,
  `stream_center.html`, `player.html`, the go2rtc plumbing).
- Hidden-instance behaviour: `GUI_App::is_hub_managed()` / `start_remote_access()`
  (`src/slic3r/GUI/GUI_App.cpp`), `MainFrame::request_quit` and the close-to-hide branch
  (`src/slic3r/GUI/MainFrame.cpp`), `GLCanvas3D::ensure_gl_ready()`, the `--hidden` /
  `SNORCA_HIDDEN` / `start_hidden` precedence chain.
- Machine definitions and profiles under `resources/profiles/` — support sets are a *user* artefact and
  must never be shipped as vendor JSON.

Two soft constraints that are easy to trip:

- **The Support-groups dialog must be answerable while hidden.** Stage 3's dialog policy
  (`RemoteAccess::dialog_mode()`) auto-answers modals on a hub-managed instance. Make the new dialog
  non-modal and make the "New group…" name prompt use the fork's `MessageDialog` family so the existing
  override (`MessageDialog::ShowModal`, `src/slic3r/GUI/MsgDialog.cpp:362-376`, which consults
  `RemoteAccess::dialog_mode()` and auto-answers outside Interactive mode) handles it. Never add a raw
  `wxTextEntryDialog` on a path a phone request can reach.
- **No new blocking file I/O on the GUI thread inside a request scope.** `SupportSetStore::reload()`
  scans a directory; call it on tab activation and on explicit user action, not from
  `Plater::priv::update` or any `run_on_main` body.

## Agent-sized tasks and branches

All branches cut from `feat/ultra-preferences`. Each is independently buildable, testable and
mergeable; the dependency edges are strict.

| # | Branch | Scope | Depends on | Days |
|---|---|---|---|---|
| T1 | `feat/support-set-store` | §1.1 store + pure helpers + `test_support_set.cpp` + CMake | — | 2.5 |
| T2 | `feat/support-set-ui` | §1.2 Support-page row (Apply / Save as / Rename / Delete), warning text | T1 | 1.5 |
| T3 | `feat/support-identity-harness` | `scripts/support_group_identity.py`, `tests/data/support_corpus/`, experiment §5.1 | — | 1.5 |
| T4 | `feat/support-group-key` | §3.1 key, §2.7 `SupportGroup` + `support_groups()` + soluble rule, §2.6 invalidation, `test_support_groups.cpp` | T3 | 3 |
| T5 | `feat/support-group-3mf` | 3MF round-trip test, stock-Orca degradation check, undo/redo verification | T4 | 1 |
| T6 | `feat/support-group-menus` | §2.2 `is_improper_category` fix, `get_options`, `PART_CATEGORY_SETTINGS`, the "Support group ▸" submenu, §2.3 `TabPrintPart`, §2.4 badge | T4 | 3 |
| T7 | `feat/support-groups-panel` | §2.5 `SupportGroupsDialog` | T6, T1 | 2.5 |
| T8 | `feat/support-params-config-ctor` | §3.3 `SupportParameters(object, config)` + `Flow` overloads — pure refactor, gated by T3 | T3 | 1.5 |
| T9 | `feat/support-group-masks` | §3.1–3.2 `slice_volumes_at_layers`, `support_group_masks`, `SupportLayer.hpp` tag | T4, T8 | 2 |
| T10 | `feat/support-group-interface` | §3.5 `clone_contacts_masked` + per-group `generate_interface_layers` | T9 | 3 |
| T11 | `feat/support-group-toolpaths` | §3.6 `generate_support_toolpaths` per-group interface + contact split + `LayerCache` | T10 | 5 |
| T12 | `feat/support-group-filament` | §3.6 item 4 `interface_by_extruder`, §3.7 Chameleon interlock, R3.5 wiping pin | T11 | 2 |
| T13 | `feat/support-material-tests` | re-enable `test_support_material.cpp` `#if 0` blocks (`:201-260`, `:262-317`), add the two-part interface fixtures | T11 | 2.5 |
| T14 | `feat/support-group-tree-organic` | Stage 4a | T12 | 4 |
| T15 | `feat/support-group-tree-classic` | Stage 4b | T12 | 4 |
| T16 | `feat/support-group-polish` | Stage 5 | T14, T15 | 4 |

Parallelism: T1/T3 start together; T2 and T4 follow; T6/T7 and T8/T9 run in parallel; T10→T11→T12 is
the critical path; T14 and T15 are independent of each other. Two agents finish in roughly the wall
time of the critical path plus the tail (~26–30 days); one agent takes the sum.

**Every branch's definition of done includes running `scripts/support_group_identity.py` on the
corpus.** For T1–T9 and T13 it must be green (byte-identical). For T10–T16 it must be green with K==1
and the K==2 fixture must show change only where the plan says it should.

## Effort summary

| Stage | Days |
|---|---|
| 1 — Support sets | 4–6 |
| 2 — Groups on parts, 3MF, invalidation | 7–10 |
| 3 — Interface groups, normal supports | 12–18 |
| 4 — Tree parity (organic + classic) | 8–12 |
| 5 — Polish, warnings, docs | 3–5 |
| **Total** | **34–51** |

Consistent with the research's 32–50 d for option (b′) and above its 19–30 d for (d) alone — the extra
comes from support sets (a feature (d) did not include), per-group interface *filament*, and tree
parity. Treat ±40 % as the honest band; the widest single uncertainty is T11
(`generate_support_toolpaths`), which is where byte identity is easiest to lose and hardest to get back.
