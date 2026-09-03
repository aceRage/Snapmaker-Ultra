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
| 3 — Interface groups, normal supports | per-group masks, per-group `SupportParameters`, per-group `generate_interface_layers`, per-group interface toolpaths + interface filament | K==1 corpus byte-identical (§3.7); the two-part fixture in `test_support_material.cpp` shows different interface extrusion length above part B and unchanged above part A; a soluble-interface group emits its own extruder via `interface_by_extruder`; support generation time within 1.6× of baseline at K=2 on the corpus |
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

## 2b. Stage 2 status (2026-09-03): BLOCKED at T3 - the byte-identity gate does not hold

Stage 2 stopped at its first task. **T3's own experiment (§5.1) says the gate every later stage
depends on cannot be met on this codebase**, so T4-T7 were not started: shipping the
`support_group` key, the resolver, the menus and the dialog under an acceptance criterion that
cannot be evaluated would be worse than stopping.

### What shipped

- **`scripts/support_group_identity.py`** - the §3.7 harness, complete and working. It slices every
  corpus case with two builds, normalises the `; generated by` header and the `id:<N>` token of the
  object-exclusion comments, compares SHA-256, names the first differing line, and takes `--datadir`
  so a run can never touch a real profile tree. `--continue-on-diff` screens the whole corpus;
  `--all-cores` disables the single-CPU pin. Passing one exe twice is the §5.1 experiment.
- **`tests/data/support_corpus/`** - `corpus.json` (nine cases: normal grid/snug, a dense
  interface, a soluble interface, organic and classic tree, a raft, and a no-support control),
  `make_fixtures.py`, and the two ~1.2 KB fixtures it generates. No bulky binary data was added.

### The blocker, with the evidence

With a fixed binary, fixed inputs and fixed settings, **the slicer's G-code is not reliably
reproducible run to run**. Measured on the untouched tree (`feat/support-sets-stage1` @ `a71c3a342c`),
same executable on both sides:

| configuration | result |
|---|---|
| all cores, 3 passes over 9 cases | 9 of 27 case-comparisons differed (33 %) |
| pinned to one CPU, 2 passes | 6 of 18 differed (33 %) |
| one CPU + `--wall-generator=classic`, 2 passes | 6 of 18 differed (33 %) |
| `no_support` alone, 8 consecutive runs, one CPU | 8/8 identical |
| `no_support` alone, 6 runs, alternating output dirs | flips between two stable outputs, 4641 vs 4678 lines |

The differences are always the same shape: a block of roughly 40 extrusion lines present in one run
and absent in the other, e.g.

```
652a653,657
> G1 X141.144 Y142.576 F30000
> ;WIDTH:0.439282
> G1 F9267.966
```

Findings that narrow it down, none of which fix it:

1. **It is not support-specific.** The `no_support` case - a plain legged ledge with
   `enable_support=0` - is one of the cases that differs. Whatever this is, it sits under the whole
   pipeline, not under the support generator.
2. **It is not (only) thread ordering.** Pinning the process to one CPU, which makes TBB run a
   single worker (`Thread.cpp:222` sizes the arena from `tbb::this_task_arena::max_concurrency()`,
   which honours the affinity mask), makes an isolated case stable 8/8 but does not clear the
   corpus.
3. **It is not Arachne alone.** `--wall-generator=classic` does not clear it either, though the
   differing blocks carry variable widths (`;WIDTH:0.439282`).
4. **It is not the output path and not data-dir state.** Alternating output directories reproduces
   both outputs at either path, and the CLI does not write to `<datadir>/Snapmaker_Orca.conf`.
5. **It is bistable, not random drift.** A case flips between two specific outputs (4641 vs 4678
   lines), not a spread of many.

Three side findings, each worth its own fix:

- **Every `.3mf` under `resources/handy_models/` segfaults the CLI on `--slice`** on an untouched
  tree, with or without preset overrides (`3DBenchy`, `OrcaCube_v2`, `Orca_stringhell`,
  `Stanford_Bunny`, `Voron_Design_Cube_v7`, `ksr_fdmtest_v4`). `OrcaToleranceTest.stl` and plain
  3MFs load fine. This is why the corpus uses generated fixtures.
- `OrcaToleranceTest.stl` generates **no support at all** with these presets, so it would have been
  a corpus case that silently tested nothing - worth remembering when extending the corpus.
- A plate holding **more than one object** is never reproducible: the objects' print order varies.
  Multi-*part* objects (one object, several `MODEL_PART` volumes - what support groups are about)
  are fine.

### What Stage 3 (and a resumed Stage 2) inherits

The harness is the useful output of this attempt and is worth keeping whatever is decided: it is
the instrument that found all of the above, and it will be the instrument that proves any fix.

The decision the reviewer has to take before Stage 2 can resume - the plan cannot answer it,
because the plan assumed the answer was yes:

1. **Find and fix the nondeterminism**, then keep §3.7 exactly as written. Cleanest, and it is a
   real bug worth fixing on its own merits - users cannot reproduce their own prints today. Cost is
   unknown until the cause is found; the bistable, single-thread-surviving character points at a
   floating-point tie or an address-ordered container in a shared pipeline stage, not at TBB.
2. **Replace byte identity with a tolerance gate.** The plan already names the fork's own Slice
   Compare engine (`src/libslic3r/SliceCompare/`) as a diagnosis tool; promote it to the gate -
   "0 config rows, all layers identical, segments 100 %" - and accept that a ~40-line block may
   move. Weaker, but it still catches every change support groups could cause, and it is available
   now.
3. **Gate on support-specific measurements instead**: extrusion length per role per object bbox,
   which is what the Stage 3 gate already measures for the A/B fixture. Narrow, but immune to the
   wobble.

Recommendation: (2) as the working gate so Stage 2 can proceed, with (1) raised as a separate bug.
Do not adopt (1) as a prerequisite without scoping it first - it is an open-ended investigation
sitting in front of a five-stage plan.

### Manual checklist

None - no user-visible change shipped in this stage. To reproduce the finding:

```
python scripts/support_group_identity.py --baseline <exe> --candidate <same exe> \
       --out <scratch> --datadir <isolated dd> --continue-on-diff
```

Run it two or three times; different cases differ each time.

### Risks R2.1-R2.4

Not reached. They belong to T6/T7 (the Object-List menu, the part panel, the badge and the
dialog), none of which was started, so there is nothing to record as decided.

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
