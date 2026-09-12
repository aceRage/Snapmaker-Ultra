# Housekeeping, 2026-09-12

Branch `fix/housekeeping-0912`, off `feat/ultra-preferences` (`944457b8bb`). Four items
from the owner's brief. One commit per item.

---

## Item 1 — Bed texture rendering

### The premise was wrong: bed textures already render

The brief (following `2026-09-12-creality-anycubic-profiles.md`) reports that
`m_texture` / `render_texture()` in `src/slic3r/GUI/3DBed.cpp` are commented out, and
concludes the profiles' `bed_texture` SVG/PNG "never draw". The first half is true; the
conclusion is not.

**`bed_texture` reaches the screen through `PartPlate`, not through `Bed3D`.** This fork
inherits Bambu Studio's architecture, in which the *part plate* owns the bed surface and
Bed3D draws only the model STL, the grid and the axes. The live path is:

| Step | Location |
|---|---|
| Read `bed_texture` from the selected printer preset | `Plater.cpp:24216` `Plater::set_bed_shape()`, via `PresetUtils::system_printer_bed_texture` (`Preset.cpp:4018`); `bed_custom_texture` overrides it |
| Hand it to the plate list | `Plater.cpp:17668` -> `PartPlateList::set_shapes(..., custom_texture, ...)` |
| Validate + store (the live twin of the commented-out `check_texture`) | `PartPlate.cpp:5248` `PartPlateList::update_logo_texture_filename()` |
| Rasterise SVG / load PNG, upload to GPU | `PartPlate.cpp:738` `PartPlate::render_logo()` |
| Draw with the GL3 `printbed` shader | `PartPlate.cpp:692` `PartPlate::render_logo_texture()` |

`Bed3D::render_texture()` is dead code, but it is *redundant* dead code: it is the
pre-plate Bambu implementation, superseded by `PartPlate::render_logo`. Both drew the same
texture with the same `printbed` shader onto the same quad. Reviving it as the brief
describes would have drawn every bed texture **twice**, co-planar and blended, at
`GROUND_Z` (Bed3D) versus `GROUND_Z + 0.02` (PartPlate) — z-fighting plus doubled alpha.

The commented-out code is also stale in a way that proves it has not been live in this
fork: it binds the vertex attribute `v_tex_coords` (`3DBed.cpp:561`), but both
`printbed.vs` shaders declare `v_tex_coord`, singular
(`resources/shaders/{110,140}/printbed.vs`), which is what the live `GLModel` path looks up
(`GLModel.cpp:705`). Restored verbatim it would have bound nothing and rendered an
untextured quad.

`git log -S` confirms the lines were never live here: they arrive already commented out in
`1555904bef` ("Add the full source of BambuStudio"), the initial import. The same lines are
commented out in the reference clone at `C:\Dev\BambuStudio` (`3DBed.cpp:472`). Upstream did
not "keep it working" — upstream disabled it for the same reason, when PartPlate took over.

### What the vendor gate actually does

`Tab::on_presets_changed()` (`Tab.cpp:2101`) sets the plate render mode from the vendor:

```cpp
bool is_bbl_vendor_preset = wxGetApp().preset_bundle->is_bbl_vendor();
if (is_bbl_vendor_preset) {
    wxGetApp().plater()->get_partplate_list().set_render_option(true, true);   // bed-type logos
} else {
    wxGetApp().plater()->get_partplate_list().set_render_option(false, true);  // profile bed_texture
}
```

`render_bedtype_logo` selects between two texture sources in `render_logo()`: Bambu printers
draw the built-in per-bed-type plate art (Cool Plate, Engineering Plate, ...), everything
else draws the profile's own `bed_texture`. Both branches work; this is correct, not a bug.

So Snapmaker U1, Creality, FlashForge and Anycubic (PNG and SVG alike) all render their
`bed_texture` today, and Bambu renders its bed-type art. Verified by reading the profile
data through to the files on disk — `Creality K2 Plus` -> `creality_k2plus_buildplate_texture.png`,
`Creality K2 Pro` -> `creality_k2pro_buildplate_texture.svg`, `Bambu Lab A1` -> `bbl-3dp-logo.svg`
— each present on disk and reachable by the path above.

### The Snapmaker U1's blank bed is deliberate, not a bug

The brief lists the U1 as "empty SVG now", implying breakage. It is an intentional change
the owner made the day before, in `a41f0287c8` (2026-09-11):

> Snapmaker U1 bed texture: drop the vendor wordmark (the texture was only the nine letter paths)

`resources/profiles/Snapmaker/Snapmaker U1_texture.svg` is a valid 349-byte SVG with a
correct `viewBox` and an empty `<g>`, carrying a comment saying the wordmark was removed on
purpose and that the plate outline comes from the bed model. It rasterises fine and draws a
fully transparent quad. **Not reverted** — reinstating the wordmark would undo a deliberate
decision, and the brief gives no authority to.

### Degradation on a missing or unreadable texture

Already correct, and checked on every branch:

- `update_logo_texture_filename()` (`PartPlate.cpp:5248`) tests extension and existence with
  a `boost::system::error_code` overload, so a permission error returns false instead of
  throwing; a bad path logs at `error` and clears the filename.
- An empty filename resets the texture and returns before any GL call (`PartPlate.cpp:742`).
- A rasterisation or PNG-decode failure logs at `warning` and returns, leaving the plate
  untextured (`PartPlate.cpp:768`, `:784`).
- An unsupported extension logs at `warning` and returns (`:789`).
- The draw is additionally guarded by `m_logo_triangles.is_initialized()` (`:804`).

No path is fatal. Nothing to fix.

### Decision

**No code change.** The feature works; the dead code is superseded, not lost; the one
apparent regression is an intentional commit from the day before. Deleting the commented-out
`Bed3D` block was considered and rejected for this branch: it is inert, it documents the
pre-plate design, and removing ~120 lines of commented code in `3DBed.cpp` risks colliding
with the Edit-gizmo and GLGizmoCut agents working nearby. The PC render path is therefore
trivially bit-identical, textured or not.

This item is documentation plus the click-test below, which is what the brief asks for in
the "otherwise" case: the rasterisation is *not* factored into libslic3r (it lives in
`GLTexture::load_from_svg_file`, GUI-side, and needs a GL context), so no unit-level test is
possible and the click-test is the verification.

### Click-test

Run the slicer and, on the Prepare tab with the plate visible from above:

1. **Third-party PNG.** Select printer *Creality K2 Plus*. The plate shows the Creality
   build-plate artwork (PNG). Confirms the non-SVG branch and `svg_source=0` in the shader.
2. **Third-party SVG.** Switch to *Creality K2 Pro*. Artwork changes to the SVG plate.
   Confirms nanosvg rasterisation and the radial-gradient `svg_source=1` branch.
3. **Bambu bed-type art.** Switch to *Bambu Lab A1*. The plate shows Bambu's bed-type
   artwork, and changing Bed Type in the sidebar (Cool / Engineering / High Temp) swaps it.
   Confirms `render_bedtype_logo=true` and that the vendor gate flips.
4. **No texture.** Switch to *Snapmaker U1*. The plate is clean — grid, bed model, no
   wordmark. This is the expected post-`a41f0287c8` look, not a failure.
5. **Missing file (degradation).** Temporarily rename
   `resources/profiles/Creality/creality_k2plus_buildplate_texture.png`, restart, select
   K2 Plus. Expect an untextured plate, no crash, and one `Unable to load bed texture:`
   line in the log. Restore the file.
6. **Dark/light.** Toggle the app theme with a textured plate selected, then again with the
   U1. The texture is unaffected by theme (it is not tinted); the bed model and grid recolour
   via `DEFAULT_MODEL_COLOR_DARK`. Check the plate stays legible in both.
7. **Bottom view.** Orbit below the plate on a textured printer. The texture renders
   back-facing with `transparent_background=1` (front face flipped to `GL_CW`), i.e. visible
   but translucent — not black, and not z-fighting.

Step 7 is the one that would have caught the double-draw had `Bed3D::render_texture` been
revived as briefed.

---

## Item 2 — Profile validator errors (26 -> 10 -> 0)

> **Status update, 2026-09-12 — done.** The 10 remaining errors are resolved on
> `fix/snapmaker-preset-names`; the tree now validates at **0 errors** and the CI gate is
> green. The owner took the decision this section left to them. The shape differs from the
> one sketched in "What is left, and why" below on one important point — the alias — see
> **Resolution** at the end of this item.

### How it was reproduced

`.github/workflows/check_profiles.yml` does not build a validator: it **downloads a prebuilt
binary**, `SoftFever/Orca_tools` release `1`, and runs
`./OrcaSlicer_profile_validator -p resources/profiles -l 2`. So these errors gate real PRs.
The same binary was fetched and run under WSL Ubuntu against this worktree — no build needed,
and no approximation of the checks.

**A method note that matters.** In validation mode the loader rethrows on the first bad vendor
(`PresetBundle.cpp:1472`), so a whole-tree run *aborts early* and the headline count is a
floor, not a total. Fixing the first class of error made the count go **up** before it came
down, because the validator finally reached vendors it had never loaded. Anyone re-running this
should expect that.

Baseline on this branch's parent: **26 `[error]` log lines**, matching the Creality/Anycubic
spec's §6.7 headline.

**"26" counts lines, not distinct errors**, and the distinction matters. Each dangling
`inherits` logs two to four lines, so the 26 are 9 inherits + 7 `printer_variant` + 1 parse
failure = **17 distinct errors**, with 10 more hidden behind the abort. An independent
re-derivation using per-vendor sweeps (which avoid the abort entirely) puts the parent tree at
**27 distinct errors** and the pre-import baseline at **17** — so **yesterday's Creality import
introduced 10 of them**, the `renamed_from` collisions in class 3 below. The Creality/Anycubic
spec's §6.7 claim that the 26 were "identical to the 26 on the baseline — no regression" is
wrong on both halves; that spec still wants the correction, which is left to its owner rather
than edited from this branch.

### Cause and fix, by class

**1. Nine `can not find inherits` (18 of the 26 lines) — an empty manifest.**


Every missing parent (`fdm_filament_abs`, `fdm_filament_tpu`, `fdm_filament_common`,
`Generic ABS @System`, `AliZ PA-CF @base`) exists on disk under
`resources/profiles/OrcaFilamentLibrary/filament/`. But `OrcaFilamentLibrary.json` ships an
**empty `filament_list`**, so none of its 274 presets is registered, and every cross-vendor
`inherits` into the library dangles.

The app does not notice because this fork carries a workaround: when the library's manifest is
empty, `PresetBundle::load_vendor_configs_from_json` (`PresetBundle.cpp:3211`, added in
`ac3dafe08a`, 2026-05-26) discovers the files by scanning the directory. Upstream's binary has
no such fallback. So the profiles were fine at runtime and broken for CI — which is exactly the
kind of divergence worth closing rather than annotating.

**Fix:** populate `filament_list` with all 274 presets, in the same order the disk scan uses
(`filament/base` recursively, then the top level, then each vendor subdirectory sorted), and
then **topologically sort** it so every parent precedes its children. Plain filename order is
not enough: alphabetically `fdm_filament_abs` comes before its own parent
`fdm_filament_common`, and the loader resolves `inherits` in list order. This is the same
ordering contract §6.6 of the Creality/Anycubic spec discovered the hard way.

**2. Seven `printer_variant` mismatches — which turned out to be thirteen.**

The rule: `printer_variant` must *begin with* the preset's own `nozzle_diameter`, optionally
plus a non-numeric suffix. Six more appeared once the early abort was gone. Three distinct
causes, all fixed in the metadata, never in the nozzle geometry a user prints with:

| Cause | Presets | Fix |
|---|---|---|
| `printer_variant` absent, so it inherits `"0.4"` from the vendor's common base | Creality CR-10 V3 0.6, Ender-3 V3 KE 0.2 / 0.6 / 0.8; Sovol SV07 0.6 / 0.8 / 1.0 | set it explicitly, as every sibling of the same family already does |
| `nozzle_diameter` is the typo — the name, `printer_variant` and all siblings agree, and only the diameter disagrees | Prusa MK3S 0.25 (`0.2`), RatRig V-Core 4 HYBRID 500 0.5 (`0.4`) and 0.8 (`0.6`) | correct the diameter, not the variant (every other MK3S/HYBRID sibling has nd == pv) |
| a suffix variant claiming its plain sibling's value | the six Flashforge Guider4 / Guider4 Pro HF presets | give each its own `0.4HF` / `0.6HF` / `0.8HF` |

The Flashforge HF case was the most interesting: `Guider4` and `Guider4 Pro` **declare**
`0.4HF;0.6HF;0.8HF` in their model `nozzle_diameter` lists, but no preset claimed any of them —
all six HF presets claimed `0.4` or `0.6` instead, colliding with their plain siblings. So
three declared variants per model were unreachable and three variants were double-claimed.
After the fix every declared variant resolves to exactly one preset, verified for both models.

`Flashforge Adventurer 4 Series HS` was handled differently: its suffix-only `"HS"` is
*self-consistent and reachable today*, because the model declares `HS` too. Changing the preset
alone would have made it unreachable, so **both files moved together**, `HS` -> `0.4HS`.

One more, found on the way: **`Sovol SV07`'s model still declared only `"0.4"`**, so the
0.6/0.8/1.0 variants imported the day before were unreachable. §6.3 of the Creality/Anycubic
spec states this bump was made; it was not. Fixed here.

**3. Two more the validator could never reach, both ours.**

- **`resources/profiles/Snapmaker.json`** named
  `process/0.10mm Color Mixing @Snapmaker U1 (0.4 nozzle).json` where the file on disk is
  `(0.4 **N**ozzle).json`. Case-insensitive on Windows, **fatal on Linux and macOS**: the read
  comes back empty and the entire vendor json fails to parse. A one-character fix, and the
  whole tree was audited for this class — it is the only one.
- **The ten `Generic X @Creality K2-all` filaments imported the day before** each declared
  `renamed_from: "Creality Generic X @K2-all;Creality Generic X K2-all"`. The second clause
  collides with the old name the pre-existing `Creality Generic X @K2-all` preset *auto-derives*
  by `@`-stripping (`PresetBundle.cpp:3562`), so two presets claimed one old name. Dropped the
  redundant clause; the first is the real rename and is uncontested. This was a regression from
  yesterday's import, not a pre-existing error — the spec's "no regression" claim was wrong on
  this point.

### What is left, and why

**10 errors remain**, all one kind: `Found duplicated preset: Generic <type> in vendor:
Snapmaker`. BBL and Snapmaker each ship a filament named literally `Generic PLA`, `Generic ABS`
and so on — genuinely different presets (different `setting_id`s, different
`compatible_printers`) that collide only on the display name.

This clash **predates this branch** — those Snapmaker files date to the 2.1.2 merge — and it is
not caused by populating the library manifest. It was simply never visible, because the
validator aborted at the dangling inherits long before it reached Snapmaker. Registering the
library removed the abort, not added the clash. Measured both ways: 54 cross-vendor duplicate
names exist without the library registered, 130 with it.

The runtime tolerates it deliberately: `PresetCollection::merge_presets` (`Preset.cpp:3238`)
special-cases `SM_BUNDLE == "Snapmaker"` — a clashing **Snapmaker** preset is kept alongside
BBL's, where any other vendor's would be dropped. So nothing is lost at runtime, but which copy
`find_preset` returns depends on load order, i.e. on the filesystem.

Resolving it means renaming user-selectable presets, which changes what users see in their
filament list and risks their saved selections — a decision that belongs to the owner, not to a
housekeeping branch. If it is taken up, the shape is: rename the ten Snapmaker files, their
`name` fields and their `Snapmaker.json` entries to `Generic X @Snapmaker U1`, which keeps the
UI label via `@`-derivation. Do **not** add `renamed_from: "Generic X"` — that bare name is
still BBL's live preset, so the rename map would be ambiguous. Existing U1 projects referencing
the bare name need their own compatibility call. Left alone and recorded here; until it is
done, this one CI gate stays red.

### Verification

| What | Result |
|---|---|
| CI's own `OrcaSlicer_profile_validator -l 2`, baseline (branch parent) | 26 `[error]` lines = 17 distinct, +10 hidden by the abort |
| same, per-vendor sweep (avoids the abort), branch parent vs pre-import | 27 vs 17 distinct — the import added 10 |
| same, after the fixes | **10 errors**, all the pre-existing Snapmaker/BBL name clash |
| same, with only the `printer_variant` fixes (library manifest left empty) | 8 errors — isolates the two classes |
| `orca_extra_profile_check.py` (assets, default) | **0 errors** |
| `orca_extra_profile_check.py --check-materials --no-check-assets` | **0 errors** |
| Flashforge Guider4 / Guider4 Pro / Adventurer 4: every declared variant claimed exactly once | no missing, no collisions |
| Diff shape | surgical — one value per file; the 1126 insertions are almost entirely the 274-entry library manifest |

A caution for anyone repeating this: the validator calls `set_data_dir()` on the profiles
folder and leaves a `resources/profiles/user/` directory behind. It is untracked scratch —
delete it, and do not commit it.

### Resolution: the ten name clashes (2026-09-12)

Branch `fix/snapmaker-preset-names`. The owner's call was to rename the Snapmaker presets and
keep an alias so existing projects still resolve.

**The rename needed no new convention — the vendor already had one, and these ten were simply
the presets that never got it.** Every other U1 0.4-nozzle filament in
`resources/profiles/Snapmaker/filament/` is already suffixed: `Generic ASA @U1 0.4 nozzle`,
`Snapmaker ABS @U1 0.4 nozzle`, `Snapmaker ASA @U1 0.4 nozzle`. All ten clashing presets carry
`compatible_printers: ["Snapmaker U1 (0.4 nozzle)"]`, exactly like those siblings. So the
rename is `Generic X` -> `Generic X @U1 0.4 nozzle`, which is what the files should have been
called all along — and `Generic ASA @U1 0.4 nozzle` is the standing proof the pattern does not
clash.

| Old name | New name |
|---|---|
| `Generic ABS` | `Generic ABS @U1 0.4 nozzle` |
| `Generic PA` | `Generic PA @U1 0.4 nozzle` |
| `Generic PA-CF` | `Generic PA-CF @U1 0.4 nozzle` |
| `Generic PC` | `Generic PC @U1 0.4 nozzle` |
| `Generic PETG` | `Generic PETG @U1 0.4 nozzle` |
| `Generic PLA` | `Generic PLA @U1 0.4 nozzle` |
| `Generic PLA Silk` | `Generic PLA Silk @U1 0.4 nozzle` |
| `Generic PLA-CF` | `Generic PLA-CF @U1 0.4 nozzle` |
| `Generic PVA` | `Generic PVA @U1 0.4 nozzle` |
| `Generic TPU` | `Generic TPU @U1 0.4 nozzle` |

File renamed, `name` field updated, `Snapmaker.json` `filament_list` entry updated (name and
`sub_path`) for each. Nothing inherits from any of the ten, no machine's `default_materials` /
`default_filament_profile` names them (the U1 machines use `Snapmaker PLA`-style names), and
`filament_hot_bed_nozzles.json` is keyed by material *type*, not preset name. The bare names
that remain in the tree — `BBL.json`, `resources/web/guide/*/test.js` — are BBL's own and are
correctly left alone.

#### The alias: no `renamed_from` is added, and that is the point

"What is left, and why" above already warned against `renamed_from: "Generic X"`, and was
right. It is worth stating *why* precisely, because the reason also shows why no explicit
`renamed_from` is needed at all.

Every lookup in the preset system tries the live-name map first and the rename map only as a
fallback — `find_preset_internal(name)` then `find_preset_renamed(name)`, the same order in
`load_external_preset` (`Preset.cpp:2138-2144`), `validate_preset` (`Preset.cpp:2053-2056`) and
`find_preset2` (`Preset.cpp:2813`). BBL's `Generic PLA` is still live. So a
`renamed_from: "Generic PLA"` could never fire — and worse, `update_map_system_profile_renamed`
(`Preset.cpp:3309`) would log an error the moment any other preset claimed the same old name.
That is exactly the trap the Creality import fell into.

**The alias that does the work is derived automatically.** `PresetBundle.cpp:3560-3570` splits
a preset name on `@` and, when no explicit alias is given, uses the left-hand part:

```cpp
size_t end_pos = preset_name.find_first_of("@");
if (end_pos != std::string::npos) {
    alias_name = preset_name.substr(0, end_pos);
    if (renamed_from.empty())
        renamed_from.emplace_back(alias_name + preset_name.substr(end_pos + 1));
    boost::trim_right(alias_name);
}
```

So `Generic PLA @U1 0.4 nozzle` gets `alias = "Generic PLA"`, and `Preset::label()`
(`Preset.cpp:655`) returns the alias — **the filament list still reads "Generic PLA"**. The
rename is invisible in the UI, which is the property that protects users' saved selections.
The auto-derived `renamed_from` is the junk string `"Generic PLA U1 0.4 nozzle"`; it is inert
(nothing else claims it, checked across every rename claim in the tree) and is deliberately
left alone rather than overridden, because supplying an explicit `renamed_from` would suppress
this derivation — the `if (renamed_from.empty())` guard — and the derivation is what we want.

#### How an old project resolves

An old U1 project stores `filament_settings_id = "Generic PLA"`. On load,
`PresetBundle::load_config_model` -> `load_config_file_config` (`PresetBundle.cpp:2953`) hands
that string to `PresetCollection::load_external_preset` as `original_name`, which finds it as a
live system preset — **BBL's `Generic PLA`** — and selects it, applying the project's own saved
values on top. No "preset not found" prompt: the "Customized Preset" dialog
(`Plater.cpp:12290`) fires only when `validate_preset` resolves neither the name nor its
`inherits`, and the name resolves.

Be precise about what this does and does not give you. The project does not re-bind to the
Snapmaker preset; it binds to BBL's same-named one, carrying its own stored settings, so the
print comes out as the project specified. That is not a new behaviour introduced here — it is
what already happened whenever BBL's copy sorted first, which is exactly the load-order
dependence this clash caused (`find_preset_internal` does a `lower_bound` over a sorted deque
and returns the first of two equal names). **The rename removes the ambiguity rather than
preserving it**: before, which preset a bare `Generic PLA` reached depended on the filesystem;
now `Generic PLA` unambiguously means BBL's and `Generic PLA @U1 0.4 nozzle` unambiguously
means Snapmaker's, while the U1 user still sees "Generic PLA" in the picker because of the
alias.

#### Verification

| What | Result |
|---|---|
| CI's own `OrcaSlicer_profile_validator -p resources/profiles -l 2` (same binary, WSL) | **0 errors** (was 10) |
| `orca_extra_profile_check.py` (assets, default) | **0 errors**, 352 warnings — unchanged |
| `orca_extra_profile_check.py --check-materials --no-check-assets` | **0 errors**, 0 warnings |
| `scripts/check_preset_name_clashes.py` (new) | **0 errors** — no duplicate names, no ambiguous rename claims |
| Negative test: rename one preset back to `Generic PLA` | correctly reported as duplicated across BBL/Snapmaker; restored |
| `Snapmaker.json` parses; every `filament_list` `sub_path` exists | 328 entries, 0 missing |

`ctest -R profile` was **not** run: there is no build tree in this worktree and the brief said
not to build one. The new check is registered as the `profile_names` test in
`tests/CMakeLists.txt` (label `profile`), alongside `profile_assets` and `profile_materials`,
and was run directly as the script it wraps.

#### Owner's click-test

1. Select printer **Snapmaker U1 (0.4 nozzle)**. Open the filament dropdown. It reads
   **"Generic PLA"**, exactly as before — not "Generic PLA @U1 0.4 nozzle". Same for the other
   nine. This is the alias doing its job; if a suffix is visible, the alias derivation broke.
2. Open a **project saved before this change** that used the U1 with Generic PLA. It loads with
   no "Customized Preset" prompt and no missing-preset warning, the filament still shows
   "Generic PLA", and the print settings are the ones saved in the project.
3. Slice it and compare against a slice from before the branch — the G-code should be
   unchanged, since no slicing value was touched, only names.
4. Switch to a **Bambu** printer and confirm its own "Generic PLA" is still there and still
   selects normally; the two no longer compete for the same name.

---

## Item 3 — Material check errors (112 -> 0)

### Cause

`--check-materials` (`scripts/orca_extra_profile_check.py:158`,
`check_machine_default_materials`) takes each machine json's `default_materials` — or, when
that key is absent, `default_filament_profile` — and resolves every name against the set of
`"name"` fields of all presets under `<vendor>/filament/**` plus
`OrcaFilamentLibrary/filament/**`. A name that does not resolve is silently dropped from the
material picker at runtime, so the visible symptom was a short filament list, never an error.

Note what the check does *not* look at: `compatible_printers`. The brief's suggested remedy
("prefer adding the missing compatible_printers entries") therefore does not apply to most of
these — the references named presets that do not exist under that name at all, and adding a
`compatible_printers` entry to some other preset would not make the name resolve. The one
place the preferred remedy did apply is Flashforge, below.

The spec's §6.8 attribution was also too narrow. Anycubic Kobra 2 / Afinia / Z-Bolt / SV08 MAX
account for roughly 45 of the 112; BBL, Flashforge, Snapmaker, TwoTrees, Lulzbot, Cubicon,
CoLiDo, MagicMaker, Tiertime and Wanhao France make up the rest.

### Fix

36 machine jsons, grouped by root cause:

| Cause | Count | What was done |
|---|---|---|
| Missing nozzle/machine qualifier — the preset existed all along | 19 | BBL H2D / H2C / H2D Pro / A2L gained ` 0.4 nozzle`; Sovol SV08 MAX gained ` @Sovol SV08 MAX` |
| Preset genuinely absent | 47 | Repointed at what the vendor actually ships: Anycubic Kobra 2 Pro/Plus/Max keep their one real PLA preset; Kobra 3 Max moves to the `@System` generics; Kobra S1 keeps its PLA; TwoTrees moves to its two real `@SK1` presets |
| Wrong name for an existing preset | 16 | `Z-Bolt Generic PLA` -> `Z-Bolt PLA @0.4 nozzle`; `Cubicon PLA @base` -> `... @Cubicon xCeler-I 0.4 nozzle`; CoLiDo's placeholder `My Generic ABS` -> `CoLiDo Generic ABS @CoLiDo X16`; `MM Generic PLA` -> `MM Generic PEEK` (BoneKing is a PEEK machine); Wanhao France's truncated `Direct Drive` -> `YUMI PLA Direct Drive`; `Snapmaker J1 PLA/PETG` -> `Snapmaker PLA/PETG @J1` |
| Separator bugs | 2 (16 names) | `Anycubic Kobra X` packed 13 names into a single array element, which the checker compares whole; split into 13 entries and fixed the `Generetic` typo. `Lulzbot Taz Pro S` used commas where the loader wants semicolons |
| Empty `default_filament_profile: [""]` | 2 | Afinia and Tiertime common bases now name a real preset |
| Flashforge Creator 5 / 5 Pro | 4 | `Flashforge TPU-95A` -> the real `Flashforge TPU 95A`; bare `Generic PLA` -> `Generic PLA @FF C5` / `@FF C5P` |

**No filament was removed from any vendor's library and no slicing value changed** — only
preset *references*, so any filament a user has already selected still resolves.

Where a preset existed but was simply not offered on that machine, the brief's preferred
remedy was used: `resources/profiles/Flashforge/filament/Flashforge TPU 95A.json` gains the
six `Flashforge Creator 5 {0.4,0.6,0.8} nozzle` and `Creator 5 Pro {...}` entries in
`compatible_printers`, following the `@FF C5` / `@FF C5P` convention of the sibling presets
(0.25 excluded, matching every other Flashforge TPU).

A first pass rewrote the files with `json.dump` and reformatted five of them wholesale
(991 insertions / 827 deletions) — these files vary in indent width, tabs vs spaces, CRLF vs
LF, and some have no trailing newline. It was reverted and redone with a surgical rewrite of
just the one value per file: **53 insertions, 51 deletions across 36 files**, every changed
line a materials line.

### Verification

| What | Result |
|---|---|
| `orca_extra_profile_check.py --check-materials --no-check-assets` | **0 errors** (was 112), 56 vendors |
| `orca_extra_profile_check.py` (default, assets) | **0 errors**, 352 warnings — unchanged from baseline, no regression |
| Every replacement name checked against the 6669-name filament index before editing | all resolve |
| `git diff` shape | 53+/51-, no reformatting |
| `ctest -R profile` | see the build section below |

`tests/CMakeLists.txt` gains a `profile_materials` test running
`--check-materials --no-check-assets`, label `profile`. It is registered as a test of its own
rather than folded into `profile_assets` so the two failure modes stay distinguishable, and
`--no-check-assets` is needed because `--check-assets` defaults to on.

---

## Item 4 — Hardening tests pinned to old data dirs

### Cause

Both gates were written against whatever hub happened to be running on a fixed path, and both
paths had since moved:

- `test_hardening.py:26` `DATADIR = C:\Users\acesa\AppData\Roaming\Snapmaker_Orca` — the
  pre-rebrand data dir. The live one is `%APPDATA%\EdgeSlicer`.
- `test_hardening.py:28` `EXE = C:\Dev\SnapmakerOrca\build\Snapmaker_Orca\EdgeSlicer.exe` — a
  build tree, not an install; the gates install to `snorca_hubtest/inst_*`.
- `test_security_next.py:10` `DATADIR = ...\snorca_hubtest\dd_next` — a scratch dir belonging
  to a hub no script starts any more.
- `test_security_next.py:11` `LAN_IP = "10.0.0.131"` — hard-wired.

`test_hardening.py`'s docstring also told the operator to run `wait_for_hub.py`, which no
longer exists.

A second, worse problem: sections A-C/F-G of `test_hardening.py` assumed *the user's own hub*.
Section B opens up to 140 sockets to drive a hub past its connection cap and section E fills
its instance table — against the owner's live hub that is disruptive, not merely wrong.

A third, latent one: `test_security_next.py` predates the Phase 0b admin-port split and sent
every `/hub/*` probe to `hub["port"]`, the phone-facing listener. On a current build those all
404 for the wrong reason, so the gate would have failed wholesale even once repointed.

### Fix

Both now follow the `[install_dir] [datadir]` convention of `test_webrtc.py` /
`test_app_push.py` / `test_phone_notify.py`:

```python
INSTALL = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else os.path.join(HERE, "inst_all")
DATADIR = os.path.abspath(sys.argv[2]) if len(sys.argv) > 2 else os.path.join(HERE, "dd_hard")
EXE = os.path.join(INSTALL, "EdgeSlicer.exe")
if not os.path.exists(EXE):
    EXE = os.path.join(INSTALL, "snapmaker-orca.exe")
```

and each:

- bails with `no install at <dir> - run cmake --install first` when the exe is absent;
- **refuses to run against a real data dir**, using `test_hub_persist.py`'s `REAL_DDS` guard
  over `%APPDATA%\EdgeSlicer` and `%APPDATA%\Snapmaker_Orca` — the safety property that keeps
  section B away from the owner's live hub;
- wipes and recreates its scratch dir (`hub/`, `log/`), quits any leftover hub first, starts
  its own with `--hub --datadir <dd> --hub-phone`, polls `hub/hub.json` for an `admin_port`
  and a live pid, and quits it in teardown (`/hub/quit`, then `taskkill /F /T`);
- keeps the shared `check()` helper and the `RESULT: PASS|FAIL` last line, exit 0/1.

Gate-specific:

- `test_hardening.py`'s `QUOTA_DIR` is now `DATADIR + "_quota"` rather than a fixed
  `dd_quota`, so two concurrent runs cannot collide. Section F needs a live instance listed
  under the hub, which no `$INST $DD` gate previously provided; the gate now starts a hidden
  one itself the way `run_control_app.py` does (`SNORCA_PHONE_ACCESS`, `SNORCA_NEW_INSTANCE`,
  `SNORCA_HIDDEN`) and waits for it to register under `hub/instances`. `kill_children()` was
  extended to `taskkill /F /T` the tree, since a hidden slicer ignores `terminate()` and
  leaves children behind — and a stray instance trips the chain scripts' "another gate run is
  active" guard.
- `test_security_next.py` gained the `lan_ip()` helper the other gates share, and its
  `/hub/*` probes (14 of them, plus the LAN-refusal probe) moved to the admin listener while
  the player surface — `/stream.html`, `/video-stream.js`, `/video-rtc.js`, `/api/ws` — stays
  on the main port, which is exactly the Phase 0b split the gate is meant to be testing.

Originals kept beside them as `test_*.py.pre0912`.

### Wiring

`gate_all.sh` gains a `hardening` section, inserted before `profiles` (the source-tree-only
gate that is deliberately last), running both scripts and OR-ing their failures into `rc`, in
the shape of the neighbouring `webrtc` / `quality` sections. `hardening` was added to the gate
name list in the header comment.

`gate_smart.sh` maps `RemoteHub|HttpServer` -> `hardening`, as a block of its own next to the
existing `RemoteHub` blocks. The pattern is an unanchored stem like every neighbouring block,
so it catches the `.hpp` headers too.

Both files were edited by writing a temp file and `os.replace`-ing it (atomic), after
confirming no `gate_all.sh` / `gate_smart.sh` process was running. Both had in fact been
changed by another agent minutes earlier (a new `quality` gate), so the patch was applied
against their current content rather than the version first read — worth repeating for anyone
editing these shared scripts.

### Verification

| What | Result |
|---|---|
| `python -m ast` parse of both gates | OK |
| `sh -n gate_all.sh`, `sh -n gate_smart.sh` | OK |
| Real-data-dir guard, both gates, pointed at `%APPDATA%\EdgeSlicer` | `refusing to run against the real data dir (...)`, exit 1 |
| Missing-install bail | `no install at ... - run cmake --install first`, exit 1 |
| `gate_smart.sh` mapping, simulated | `RemoteHub.cpp` -> `... webrtc quality hardening`; `HttpServer.cpp` -> `lan ctl hardening`; `Print.cpp` -> `lan ctl reopen` (unchanged) |
| **`test_hardening.py` against a scratch install built from this worktree** | **RESULT: PASS — 74 checks, 0 failures** |
| **`test_security_next.py`, same install** | **RESULT: PASS — 35 checks, 0 failures** |
| Teardown | 0 leftover `EdgeSlicer.exe` on `dd_hard` / `dd_secnext`; the owner's live hub on `%APPDATA%\EdgeSlicer` untouched throughout |

`gate_all.sh` itself was not run, per the brief.

### Two real bugs the first end-to-end run caught

Neither was visible from static checks, and both were in the port rather than the product.

1. **Section F had no instance to test against.** The hidden slicer was launched as
   `EXE --datadir <dd>` with no model. Launched that way it exits immediately and never
   registers under `hub/instances`, so section F failed with "0 listed". `run_control_app.py`
   always passes a file; the gate now does the same (`h2d_copy.3mf`, overridable with
   `SNORCA_TEST_MODEL`, omitted if absent). With a model the instance stays up, registers, and
   section F passes.
2. **The LAN-refusal probe expected the wrong refusal.** Phase 0a refused a LAN-origin
   `/hub/*` with a 404 on the single listener. Phase 0b's admin listener is loopback-only, so
   a LAN peer cannot connect at all — status 0, a *strictly stronger* result that the ported
   assertion read as a failure. It now accepts unreachable-or-404 and still rejects 200.
   (`test_hardening.py` already asserted the stronger form, so the two gates now agree.)

One further assertion was relaxed as a genuine test bug, not a product one:
`same stamp, different random suffix` compared two uploads' timestamps, which fails whenever
they straddle a second boundary. The property that matters is the random suffix keeping folder
names collision-proof, so the check now asserts distinct suffixes and merely notes a straddled
boundary. Observed failing once on a boundary and passing on the re-run with the same code
path — the product behaviour was correct both times.
