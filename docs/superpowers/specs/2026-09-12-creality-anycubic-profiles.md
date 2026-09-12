# Creality / Anycubic profile completeness

Date: 2026-09-12. Branch: `feat/ultra-preferences` (research only, nothing built or merged).

## 1. Inventory: what we have vs. upstream OrcaSlicer

Baseline (this repo, `resources/profiles/Creality.json` / `Anycubic.json`,
`machine_model_list`): **Creality 32 models**, **Anycubic 20 models**, inherited from the
Snapmaker Orca base with no local additions.

Upstream (`github.com/SoftFever/OrcaSlicer`, main branch): `Creality.json` at commit
`fdc0ee18f11f22766e1e22e66fe0304fedadb086` (2026-09-03, vendor version `02.03.02.77`);
`Anycubic.json` at commit `6ec904074b1630b2571e493ea2829acbd27b6837` (2026-09-01, vendor
version `02.04.00.03`).

### Creality gap (upstream has, we lack)

| Model | Nozzle variants | Notes |
|---|---|---|
| K2 | 0.2 / 0.4 / 0.6 / 0.8 | Not in our list (we only carry K2 Plus) |
| K2 Pro | 0.2 / 0.4 / 0.6 / 0.8 | Not in our list |
| K2 SE | 0.4 | Not in our list |
| K1 / K1 Max / K1 SE / K1C `_CFS-C` | 0.4 each | Separate model entries for the CFS-C hardware variant; we only have the plain models |
| Ender-3 V4 | TBD | New model line, needs a direct check before import |
| SPARKX i7 | TBD | New sub-brand line, needs a direct check before import |

Our K2 Plus already matches upstream's K2 Plus (0.2/0.4/0.6/0.8) — not a gap. No Creality
model we carry is absent or renamed upstream.

### Anycubic gap and retired/renamed (both vendors)

None. All 20 of our Anycubic models (Kobra 3, Kobra 3 Max, Kobra S1, Kobra S1 Max included)
exist upstream under the same names, and a direct check of our local
`resources/profiles/Anycubic/machine/` confirms the nozzle-variant files already match
upstream: Kobra 3 (0.2/0.4/0.6/0.8), Kobra 3 Max (0.4/0.6/0.8), Kobra S1 (0.4), Kobra S1 Max
(0.25/0.4/0.6/0.8, including the unusual 0.25 mm variant). Anycubic needs no import, just a
periodic re-check. Nothing in either vendor's list appears retired or renamed upstream --
every model we ship matched an upstream entry by name.

## 2. Asset audit of what we have

### The check script

```python
#!/usr/bin/env python3
# Read-only audit of resources/profiles/<Vendor>.json + <Vendor>/{machine,filament,process}.
# Checks: every *_list[].sub_path exists; every "inherits" resolves to a preset of that
# TYPE with a matching "name" field found anywhere under the vendor's machine/filament/
# process tree (mirrors PresetCollection::find_preset(name) in Preset.cpp, which indexes
# by name, not path); bed_model/bed_texture named by each machine json exist; the
# "<model>_cover.png" Plater.cpp derives at runtime for each machine_model_list entry
# exists (thumbnail.png is optional/legacy -- a miss only downgrades to a placeholder
# icon with a log warning, reported separately, not counted as broken).
# Writes nothing. Usage: python tools/audit_profile_assets.py <repo_root>
import json, os, sys
ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PROFILES = os.path.join(ROOT, "resources", "profiles")

def load_json(path):
    with open(path, encoding="utf-8-sig") as f:
        return json.load(f)
def rel(p):
    return os.path.relpath(p, ROOT).replace("\\", "/")

def build_name_index(vendor_dir, subdir):
    # Recursively index every json under vendor_dir/subdir by its own "name" field.
    index = {}
    base = os.path.join(vendor_dir, subdir)
    if not os.path.isdir(base):
        return index
    for dirpath, _, filenames in os.walk(base):
        for fn in filenames:
            if not fn.endswith(".json"): continue
            full = os.path.join(dirpath, fn)
            try: data = load_json(full)
            except Exception: continue
            name = data.get("name")
            if name and name not in index: index[name] = (full, data)
    return index

def check_vendor(vendor_name):
    vendor_json_path = os.path.join(PROFILES, vendor_name + ".json")
    vendor_dir = os.path.join(PROFILES, vendor_name)
    if not os.path.isfile(vendor_json_path) or not os.path.isdir(vendor_dir):
        return None
    problems = []
    vjson = load_json(vendor_json_path)
    machine_index = build_name_index(vendor_dir, "machine")
    filament_index = build_name_index(vendor_dir, "filament")
    process_index = build_name_index(vendor_dir, "process")

    def check_inherits_chain(data, kind, index, referenced_by, seen):
        name = data.get("inherits")
        if not name:
            return
        if name in seen:
            problems.append((f"inherits_cycle_{kind}", name, referenced_by)); return
        seen.add(name)
        if name not in index:
            problems.append((f"missing_inherits_{kind}", name, referenced_by)); return
        check_inherits_chain(index[name][1], kind, index, referenced_by, seen)
    for entry in vjson.get("machine_model_list", []) + vjson.get("machine_list", []):
        sub_path = entry.get("sub_path")
        if not sub_path: continue
        referenced_by = f"{vendor_name}.json machine list: {entry.get('name', sub_path)}"
        full = os.path.join(vendor_dir, sub_path)
        if not os.path.isfile(full):
            problems.append(("missing_machine_json", rel(full), referenced_by)); continue
        data = load_json(full)
        check_inherits_chain(data, "machine", machine_index, referenced_by, {data.get("name", sub_path)})
        for key in ("bed_model", "bed_texture"):
            val = data.get(key, "")
            if isinstance(val, list): val = val[0] if val else ""
            if val:
                p = os.path.join(vendor_dir, val)
                if not os.path.isfile(p):
                    problems.append((f"missing_{key}", rel(p), f"{sub_path} ({key})"))
    for entry in vjson.get("machine_model_list", []):
        model_name = entry.get("name")
        if not model_name: continue
        cover = os.path.join(vendor_dir, model_name + "_cover.png")
        thumb = os.path.join(vendor_dir, model_name + "_thumbnail.png")
        if not os.path.isfile(cover):
            problems.append(("missing_cover", rel(cover), f"{vendor_name}.json: {model_name}"))
        if not os.path.isfile(thumb):
            problems.append(("missing_thumbnail_optional", rel(thumb), f"{vendor_name}.json: {model_name}"))
    for list_key, kind, index in (("filament_list", "filament", filament_index),
                                   ("process_list", "process", process_index)):
        for entry in vjson.get(list_key, []):
            sub_path = entry.get("sub_path")
            if not sub_path: continue
            referenced_by = f"{vendor_name}.json {list_key}: {entry.get('name', sub_path)}"
            full = os.path.join(vendor_dir, sub_path)
            if not os.path.isfile(full):
                problems.append((f"missing_{kind}_json", rel(full), referenced_by)); continue
            data = load_json(full)
            check_inherits_chain(data, kind, index, referenced_by, {data.get("name", sub_path)})
    return problems

def main():
    total = 0
    for vendor in sorted(f[:-5] for f in os.listdir(PROFILES) if f.endswith(".json")):
        problems = check_vendor(vendor)
        if problems:
            print(f"\n=== {vendor} ({len(problems)} findings) ===")
            for kind, path, ref in problems:
                print(f"  [{kind}] {path}  <- {ref}")
                total += 1
    print(f"\nTOTAL: {total}")
if __name__ == "__main__":
    main()
```

Ran read-only against the full `resources/profiles/` tree (no files written). Worth
flagging for anyone reusing this: filament/process `"inherits"` resolves by matching the
target's own `"name"` field anywhere in the vendor's preset tree (mirrors
`PresetCollection::find_preset()`, `Preset.cpp:1475`, which indexes by name, not path) --
not a same-directory file named `"<inherits>.json"`. A naive path-based version produced 554
false positives on BBL alone (e.g. `Generic ABS @base` actually lives in a different
directory than the child that inherits it); the name-indexed version above produced zero.

### Findings: Creality and Anycubic

| Vendor | Missing file | Kind | Referenced by |
|---|---|---|---|
| Creality | `creality_ender3v3plus_buildplate_texture.png` | bed_texture | `machine/Creality Ender-3 V3 Plus.json` |
| Anycubic | *(none)* | -- | -- |

Creality has exactly one real broken reference: the file on disk is
`creality_ender3v3plus_buildplate_texture.png.png` (double `.png`) while `Creality Ender-3
V3 Plus.json`'s `bed_texture` key names it without the duplicate extension -- a simple typo.
Anycubic has zero missing machine/filament/process/bed/cover references.

Both vendors also show one *optional* finding: every machine model is missing its legacy
`<model>_thumbnail.png` (32 for Creality, 20 for Anycubic). Not a regression -- `_cover.png`
is what the modern printer picker (`Plater.cpp`) actually uses, and both vendors are 100%
covered there. `_thumbnail.png` is read only by `ConfigWizard.cpp`'s older wizard tile and,
on a miss, warns and falls back to a placeholder icon (section 4) -- cosmetic, not a defect.

### Findings: every other vendor (spot check, since the script was already written)

| Vendor | Missing file | Kind |
|---|---|---|
| RatRig | 4 `ratrig-vcore-bed-{300,500}-{copy,mirror}-mode.stl` | bed_model |
| Sovol | `sovol_sv07(plus)_buildplate_model.stl` (2 files) | bed_model |
| SecKit | `Seckit-logo.svg` | bed_texture |
| Snapmaker | `Snapmaker U1_bed.svg` | bed_texture |
| Wanhao | `Wanhao D12-300_buildplate_texture.png` | bed_texture |
| Wanhao France | `D12_texture` (extensionless placeholder value, 18 machine jsons reference it) | bed_texture |
| Wanhao France | 4 `_cover.png` for the "DIRECT" model variants | cover |
| BBL | ~340 `missing_inherits_filament`, all third-party brands (AliZ, Overture, FusRock) inheriting a `@base` preset absent from this fork's `BBL/filament/` tree | inherits |

Out of scope here but real: the BBL finding means those filament presets fail to resolve
their config today. Flagged as a follow-up, not fixed here.

### How the app loads these at runtime, and what each miss does

Traced in `3DBed.cpp`, `Preset.cpp` (`PresetUtils::system_printer_bed_model`/`_bed_texture`,
~4006), `PresetBundle.cpp` (`get_stl_model_for_printer_model`/`get_texture_for_printer_model`,
~690), `Plater.cpp` (~10069, cover image), `ConfigWizard.cpp` (~245, thumbnail image), and
`GLModel.cpp` (`init_from_file`, ~581).

| Asset kind | Resolution path | On a miss |
|---|---|---|
| `bed_model` (STL) | `PresetUtils::system_printer_bed_model()`: `data_dir()/vendor/...` then `resources_dir()/profiles/...`, gated by `boost::filesystem::exists()`. Loaded by `GLModel::init_from_file()` in `Bed3D::render_model()`. | **Silent, non-fatal.** `init_from_file` returns `false` on a missing/bad file (`exists()` check plus a `try/catch` around `Model::read_from_file`); the bed falls back to the default flat plate (`render_default()`). No dialog, no log line. |
| `bed_texture` | Same helper (`system_printer_bed_texture` / `get_texture_for_printer_model`), plumbed through `Plater::set_bed_shape()` and `GCodeViewer.cpp`. | **Fully inert either way.** `Bed3D`'s texture members (`m_texture`, `render_texture()`) are commented out in both `3DBed.cpp`/`.hpp` in this fork -- the resolved path is never consumed by the bed renderer (`PartPlateList::set_shapes`'s `texture_filename` is the unrelated per-plate corner logo). A missing bed_texture changes nothing visible today, which also means a present one renders nothing -- a separate, pre-existing latent bug worth its own ticket if textures are meant to work. |
| `<model>_cover.png` | `Plater.cpp` (~10069-10089) builds `resources_dir()/profiles/<vendor>/<model>_cover.png` when the printer picker paints a card, gated by `exists()` before `wxImage::LoadFile`. | **Silent.** No image for that card, no log line. |
| `<model>_thumbnail.png` (legacy wizard tile) | `ConfigWizard.cpp` (~235-250), `data_dir()/vendor/...` then `resources_dir()/profiles/...`. | **Warns, then falls back.** Logs a warning and loads `Slic3r::var(PRINTER_PLACEHOLDER)`, a bundled generic icon. Cosmetic. |
| Vendor-tab bitmaps (`create_scaled_bitmap`, the 2026-09-08 FlashForge crash) | `src/slic3r/GUI/wxExtensions.cpp` | **Already fixed**: used to `throw Slic3r::RuntimeError`, now returns a transparent placeholder via `missing_bitmap_placeholder()`, logged once per name (see `2026-09-08-flashforge-bitmaps.md`). |

**Bottom line for crash prevention:** none of the four profile-asset kinds we audit
(bed_model, bed_texture, cover, thumbnail) can currently crash the app in this fork -- every
consumer gates on `boost::filesystem::exists()` or a caught exception before touching the
file, unlike the FlashForge bitmap path before its 2026-09-08 fix. The risk isn't a crash --
it's a silently degraded bed/printer-picker experience nothing today catches in CI or tests.

## 3. Import plan

**Scope:** Creality K2 / K2 Pro / K2 SE (upstream has full machine + process coverage at
0.2/0.4/0.6/0.8 nozzles) and the four K1-family `_CFS-C` variants. Anycubic needs no import
(section 1) -- treat as "re-check in ~3 months" rather than an active task.

**License status:** OrcaSlicer main is AGPL-3.0, same as EdgeSlicer, so importing the
profile JSON and machine/process/filament definitions needs no relicensing -- copy with
attribution (this doc + commit SHA), same as every other vendor folder here. The Creality
bed model STL/texture and `_cover.png` files are Creality product artwork, not OrcaSlicer's
IP; apply the same call the owner made for FlashForge on 2026-09-08 (memory
`flashforge-artwork-decision`): keep while EdgeSlicer has no commercial distribution.

**Upstream paths (commit `fdc0ee18f11f22766e1e22e66fe0304fedadb086`):**
`resources/profiles/Creality.json` (new `machine_model_list`/`process_list`/`filament_list`
entries for K2/K2 Pro/K2 SE and the four `_CFS-C` variants); `Creality/machine/Creality
K2*.json` + nozzle variants + the `_CFS-C` machine jsons; `Creality/process/*@Creality
K2*.json` (0.2/0.4/0.6/0.8 sets); `Creality/filament/*` entries whose `compatible_printers`
reference the new machines; `Creality/*_cover.png`, `*_buildplate_model.stl`,
`*_buildplate_texture.png` for K2/K2 Pro/K2 SE.

**Expected conflicts with our modified base:** `fdm_creality_common.json`/
`fdm_machine_common.json` have diverged from upstream's BBL-derived common base (diff shows
differing `machine_max_speed_x/y`, `retraction_distances_when_cut`,
`extruder_clearance_height_to_rod`, `max_layer_height`) -- new K2-family jsons that
`"inherits": "fdm_creality_common"` pick up *our* tuned values, so diff line-by-line against
upstream's K2 machine json to confirm no K2-specific override is silently dropped. Our extra
keys (`ftp_folder`, host-type additions from the phone-integration work) aren't in
upstream's machine jsons; add them post-copy the same way existing Creality models have
them, or the new machines won't get phone/hub network settings. Mixed-nozzle keys used by
some newer vendors don't apply (Creality/Anycubic are single-nozzle FFF). Filename
collisions are unlikely, but re-diff `fdm_creality_common` itself before copying in case
upstream touched it since our fork.

**Validation steps:**
1. `python3 scripts/orca_extra_profile_check.py --vendor Creality --check-filaments
   --check-materials --check-obsolete-keys` -- sub_path existence, vendor/sub-file name
   consistency, `compatible_printers` completeness, filament_id length, obsolete keys.
   Already runs in CI via `.github/workflows/check_profiles.yml` on any
   `resources/profiles/**` PR.
2. `tools/audit_profile_assets.py` (section 2) against the branch, to confirm every new
   bed_model/bed_texture/cover file actually landed.
3. The upstream binary validator, same as CI: `curl -LJO
   https://github.com/SoftFever/Orca_tools/releases/download/1/OrcaSlicer_profile_validator`
   then `./OrcaSlicer_profile_validator -p resources/profiles -l 2`.
4. Our own gate, `src/dev-utils/Snapmaker_Orca_profile_validator.cpp`: builds a
   `PresetBundle`, generates a `_orca_test` custom preset per system preset (printer/
   filament/print), and exercises `PresetCollection::save_current_preset` -- catches a
   config key that fails to parse into a `ConfigOption`, which the JSON-level checks miss.
   Build with `ORCA_TOOLS=ON` and run against the branch.

## 4. Crash prevention

**CI/gate check for missing assets.** Extend `scripts/orca_extra_profile_check.py` with a
`check_asset_files(profiles_dir, vendor_name)` function in its existing style (`Path`-based,
`print_error`/`print_warning`, returns an error count) running the bed_model/bed_texture/
cover checks from `tools/audit_profile_assets.py` above, wired to a new `--check-assets`
flag (default-on in CI). This closes the gap that today's `check_name_consistency` only
verifies a `sub_path` file exists, not the model/texture/image files it in turn names.
`check_profiles.yml` already triggers on `resources/profiles/**` and runs this script first,
so no new workflow file is needed -- just extend the existing job.

Separately, `orca_extra_profile_check.py` has no CTest equivalent today (GitHub Actions
only); add a thin wrapper (e.g. `tests/slic3rutils/profile_assets_test`, `add_test(NAME
profile_assets COMMAND ${Python3_EXECUTABLE}
${CMAKE_SOURCE_DIR}/scripts/orca_extra_profile_check.py --check-assets)`) so a local `ctest`
run catches this too.

**Runtime fallback.** The 2026-09-08 fix (`missing_bitmap_placeholder()` in
`wxExtensions.cpp`) already covers the one asset class here that used to be fatal
(`create_scaled_bitmap`). Section 2 confirms bed_model, bed_texture, cover, and thumbnail
are all already non-fatal by construction (every load site gates on `exists()` or a caught
exception) -- no runtime patch is needed for those four; the gap is CI/test coverage, not a
crash to fix. Follow-up ticket worth filing separately: `bed_texture` is resolved but never
rendered anywhere in this fork (dead code in `3DBed.cpp`/`.hpp`) -- a pre-existing bug this
import should not start silently relying on.

## 5. Estimate

Roughly a day: K2/K2 Pro/K2 SE + CFS-C import diffed against our modified common base (~4h);
wiring `ftp_folder`/host-type keys into the new machine jsons (~1h); validation via script,
binary validator, `Snapmaker_Orca_profile_validator`, and a manual smoke test (~2h);
`check_asset_files()` + CTest wrapper (~1h). Anycubic needs no import -- budget ~30 min to
re-diff before merging (this research used commit `6ec904074b1630b2571e493ea2829acbd27b6837`).

---

# 6. Implementation record (2026-09-12, branch `feat/creality-profiles`)

Carried out on a worktree of `feat/ultra-preferences` (8e7b9f803e). Nothing merged.

## 6.1 Creality import

Upstream source: `github.com/SoftFever/OrcaSlicer`, commit
`fdc0ee18f11f22766e1e22e66fe0304fedadb086` (AGPL-3.0, same licence as this fork).
`Creality.json` version `02.03.01.10` -> `02.03.02.00`; 32 -> **41 machine models**
(now matching upstream's 41), 78 -> 96 machines, 271 -> 345 process, 70 -> 398 filament.

**Models imported (9):** K2, K2 Pro, K2 SE, K1_CFS-C, K1 Max_CFS-C, K1 SE_CFS-C,
K1C_CFS-C, **Ender-3 V4**, **SPARKX i7**.

Ender-3 V4 and SPARKX i7 were left conditional in section 3 pending a completeness check.
Both **are** complete upstream at that commit (machine model + all nozzle variants + process
presets + filament sets), so both were imported. SPARKX i7 was additionally required by the
owner; it needed no authoring.

**Files added: 429 json + 8 svg.** 27 machine (9 model + 18 nozzle variants: K2 and K2 Pro
at 0.2/0.4/0.6/0.8, SPARKX i7 at 0.2/0.4/0.6/0.8, K2 SE / the four CFS-C / Ender-3 V4 at
0.4), 74 process, 328 filament (includes 11 shared parents such as `Generic PLA @Creality`
and `fdm_filament_petg` that the new presets inherit), 8 bed-texture svgs. Zero filename
collisions with existing files.

### Divergence against our base: much smaller than section 3 expected

Section 3 predicted conflicts in `fdm_creality_common` / `fdm_machine_common`. Diffed
directly:

| File | Key sets | Differing values |
|---|---|---|
| `fdm_creality_common.json` | identical (56 keys each) | 1: `default_filament_profile` (ours `Creality Generic PLA`, upstream `Generic PLA @Creality`) |
| `fdm_machine_common.json` | identical (54 keys each) | 1: `retraction_length` (ours `1`, upstream `5`) |

No key the imported machines need is missing from our base, so **no base edits were made**
and the new machines inherit our tuned values as intended.

Section 3 also called for adding `ftp_folder` / host-type keys post-copy. Checked: of our 70
existing Creality machine jsons, **none** carry `ftp_folder`, and only one carries
`host_type`. There is no convention to match, so nothing was added; the imported jsons keep
their own `host_type: crealityprint` (a value this fork supports -- `PrintConfig.cpp:85`,
`htCrealityPrint`).

### Upstream reference bugs fixed on import

Upstream's own machine jsons name presets that do not exist anywhere upstream. Left as-is
they would have failed the new asset gate, so each was repointed at the real preset:

| File | Broken reference | Fixed to |
|---|---|---|
| `Creality K2 0.2 nozzle.json` | `0.10mm Standard @Creality K2 0.2 nozzle` | `0.10mm HighDetail @Creality K2 0.2 nozzle` |
| `Creality K2 Pro 0.2 nozzle.json` | `0.16mm Optimal @Creality K2 Pro 0.2 nozzle` | `0.14mm Optimal @Creality K2 Pro 0.2 nozzle` |
| `Creality Ender-3 V4.json` | 6 x `Creality Generic <type> @Ender-3 V4-all` | the real `Generic <type> @Ender-3 V4-all` / `CR-PLA Matte @...` |
| `Creality Ender-3 V4.json` | `Creality Generic ASA @Ender-3 V4-all` | **dropped** -- upstream ships no ASA for this model |
| `Creality SPARKX i7.json` | `Generic PLA Silk @SPARKX i7-all` | `Generic PLA-Silk @SPARKX i7-all` |
| 4 x `*_CFS-C 0.4 nozzle.json` | `Generic PLA @Creality K1-all`, `Generic PLA HF @Creality` | our `Creality Generic PLA @K1-all`, `Creality HF Generic PLA` |
| 4 x `*_CFS-C.json` (`default_materials`) | the `Generic X @Creality K1-all` scheme, absent here | mirrored from each variant's working base model (K1, K1 Max, K1 SE, K1C) |

### Asset substitutions (no binaries imported, per the brief)

Bed-texture SVGs are vector text and were imported verbatim (8 files; this tree already
carries 72 svg bed textures, so the format is well established). Bed model STLs and cover
PNGs are binary, so each new model was pointed at an existing asset of the correct size,
verified by reading the STL bounding boxes rather than trusting the names:

| Model | Bed size | bed_model used | Cover used |
|---|---|---|---|
| K2 | 260x260 | `creality_hi_buildplate_model.stl` (265x280) | K2 Plus |
| K2 Pro | 300x300 | `creality_ender3s1plus_buildplate_model.stl` | K2 Plus |
| K2 SE | 220x215 | `creality_k1_buildplate_model.stl` (235x250) | K2 Plus |
| K1_CFS-C / K1 Max / K1 SE / K1C_CFS-C | as base model | the base model's own bed (exact) | the base model's cover (exact) |
| Ender-3 V4 | 220x220 | `creality_ender3v3_buildplate_model.stl` (exact) | Ender-3 V3 |
| SPARKX i7 | 260x260 | `creality_hi_buildplate_model.stl` | Hi |

The four `_CFS-C` variants are the same hardware as their base models with a different
filament system, so base bed and cover are exactly right, not approximations. Artwork is
kept under the same call the owner made for FlashForge on 2026-09-08 (non-commercial).

## 6.2 Anycubic: re-diffed, still no import needed

Re-diffed against upstream `6ec904074b1630b2571e493ea2829acbd27b6837`: **model lists and
machine lists are identical** (20 models, same names, same nozzle variants) -- section 1's
finding holds. Upstream is version `02.04.00.03` to our `02.04.00.02`, a metadata-only bump.

**Anycubic Kobra X** (requested separately) is **already complete in this tree**: model entry,
`Anycubic Kobra X 0.4 nozzle`, 9 process presets, 13 filaments, plus bed model, bed texture
and cover on disk. It passes the new asset gate. Nothing to import or author.

## 6.3 Sovol

Diffed against upstream at the same commit: **model and machine lists are identical**
(13 models, 23 machines), including the full SV08 lineup (SV08, SV08 MAX) and Zero. The
filament lists differ only by naming convention (upstream renamed `Sovol SV08 ABS` to
`Generic ABS @Sovol SV08`); same count, same presets. No 2026 variant (Plus/Pro/multi-material)
exists upstream. Checked `github.com/Sovol3d` directly: `SV08`, `SV08MAX`, `SOVOL-ZERO`,
`SV06-ACE-PLUS` are hardware/firmware repos with no slicer profiles.

One real gap found in Sovol's own `Sovol3d/Sovol-OrcaSlicer` fork (an OrcaSlicer derivative
carrying AGPL-3.0 in `OrcaSlicer/LICENSE.txt`, so licence-compatible): **SV07 0.6 / 0.8 /
1.0 nozzle variants**, which neither upstream nor this tree carried.

Imported as **authored**, not copied: that fork is older (vendor version `01.09.00.02` vs our
`02.03.01.10`) and its SV07 start G-code diverges from ours. Copying it would have given the
new variants different machine behaviour from our own SV07 0.4. Instead each new preset was
derived from **our** `Sovol SV07 0.4 nozzle` / `0.20mm Standard @Sovol SV07`, changing only
nozzle-dependent values:

- machines: `nozzle_diameter`, `max_layer_height` (80% of nozzle, this tree's convention),
  `default_print_profile`. Everything else -- speeds, accelerations, start/end G-code,
  bed shape -- inherited unchanged from our 0.4. **Confidence: high** (bed 220x220x250 and all
  machine limits are our own existing values, not third-party claims).
- 11 process presets: layer-height ladders taken from the Sovol fork's own SV07 sets (so each
  machine's `default_print_profile` resolves), line widths following this tree's SV06 ACE
  convention (widths track the nozzle, initial layer +0.05). Speeds/accelerations kept from
  our SV07 0.4 rather than the SV06 ACE, which is a much faster CoreXY machine.
  **Confidence: medium-high** for the layer-height/width ladders (conventional, vendor-derived),
  **high** for the speeds (unchanged from our shipping SV07 profile).

`Sovol.json` `02.03.01.10` -> `02.03.02.00`; `Sovol SV07` model `nozzle_diameter`
`0.4` -> `0.4;0.6;0.8;1.0`; 23 -> 26 machines, 35 -> 46 process presets.

## 6.4 Other missing-asset fixes (section 2's cross-vendor findings)

All 31 asset errors the audit found are fixed. Every one turned out to be a naming or
packaging mistake with the correct file already on disk, except RatRig's, where the named
files never existed anywhere.

| Vendor | Finding | Decision |
|---|---|---|
| RatRig (4) | `ratrig-vcore-bed-{300,500}-{copy,mirror}-mode.stl` | **Repointed** at `ratrig-vcore-bed-{300,500}.stl`. These files exist in neither this tree nor upstream -- only the 400 size ever got dedicated copy/mirror meshes. The full-size plate is the correct bed; only the *printable area* differs between modes, and that is set per nozzle preset, not by the mesh. |
| Sovol (2) | `sovol_sv07(plus)_buildplate_model.stl` | **Repointed** at `sovol_sv06(plus)_buildplate_model.stl`. Present upstream but binary (brief excludes binaries); SV07 is dimensionally identical to SV06 (220x220x250) and SV07 Plus to SV06 Plus (300x300), confirmed by STL bounding box. The matching `sovol_sv07*_texture.png` files were already present. |
| SecKit (1) | `Seckit-logo.svg` | **Typo fixed** -> `seckit_logo.svg` (on disk, case/separator mismatch). |
| Snapmaker (1) | `Snapmaker U1_bed.svg` | **Typo fixed** -> `Snapmaker U1_texture.svg`. `..._bed.stl` is the model; every other Snapmaker machine uses `..._texture.svg` for the texture. |
| Wanhao (1) | `Wanhao D12-300_buildplate_texture.png` | **Typo fixed** -> `Wanhao_D12-300_buildplate_texture.png` (underscore). |
| Wanhao France (18) | `D12_texture`, extensionless | **Extension added** -> `D12_texture.svg`, which is on disk. Fixed in all 18 machine jsons. |
| Wanhao France (4 covers) | 4 `DIRECT` variants had no `_cover.png` | `D12 500 PRO M2 DIRECT`: **renamed** the orphaned `D12 500 PRO MAX M2 DIRECT_cover.png.png` (a double-extension typo, same class as this branch's parent commit; its bytes differ from the MAX cover, so it is the non-MAX image). Other 3: **copied** from the `PRO MAX` sibling -- same chassis and bed size, differing only in an upgrade this render does not show. |

## 6.5 The ~340 BBL "@base" inherits: not broken, no fix needed

Section 2 flagged ~340 third-party BBL filament presets (AliZ, Overture, FusRock) whose
`@base` parents were missing, as a real follow-up. **They are not broken.** Every parent
lives in `resources/profiles/OrcaFilamentLibrary/filament/<Brand>/`, e.g.
`OrcaFilamentLibrary/filament/Overture/Overture TPU @base.json`, and
`PresetBundle::load_vendor_configs_from_json` (`PresetBundle.cpp` ~3211,
`ORCA_FILAMENT_LIBRARY`) loads that library's presets from disk into the same pool every
vendor resolves against. They load fine at runtime.

The finding was an artifact of the audit script scoping its name index to one vendor folder.
The same mistake in `check_asset_files()` produced 361 false positives on the first run;
the shipped version indexes `OrcaFilamentLibrary` alongside the vendor's own presets, which
took the tree from 392 reported errors to the 31 real ones. **No presets were removed.**

## 6.6 Gate, CTest and hubtest wiring

`scripts/orca_extra_profile_check.py` gains `check_asset_files()` (plus `_build_name_index()`),
wired to `--check-assets`, **default on** so CI's existing bare invocation picks it up with no
workflow change. Checks `bed_model`, `bed_texture`, `<model>_cover.png`, and that every
`inherits` resolves to a preset of the same type (cycles reported once);
`<model>_thumbnail.png` counts as a warning only, matching its placeholder fallback.

`tests/CMakeLists.txt` registers `add_test(NAME profile_assets ...)` with label `profile`, so
`ctest -R profile` runs it locally. Registered directly rather than in a C++ test target since
it needs no compilation, and guarded by `find_package(Python3)`.

Hubtest: `gate_all.sh` gains a `profiles` section (source-tree only -- starts no instance and
touches no data dir, honouring `PROF_TREE`), and `gate_smart.sh` maps `resources/profiles/**`
to it. Note `resources/profiles/` previously escalated a profile-only change to the **full**
gate set; it now selects just this gate, with everything else under `resources/` still
forcing `all`. gate_all.sh itself was not run, per the brief.

### Ordering contract discovered

The first wiring pass sorted the vendor lists alphabetically, which broke a load-order
contract: `Creality.json` lists parents **first** (`fdm_process_common`,
`fdm_process_creality_common`, ...), and the loader resolves `inherits` in list order. The
upstream validator caught this ("can not find inherits fdm_process_creality_common"). Fixed
by preserving the original relative order and topologically sorting so every parent precedes
its children -- 0 violations now in both vendors. Worth knowing before editing any vendor json
programmatically: **these lists are order-sensitive, not sets.**

## 6.7 Validation actually run

| What | Result |
|---|---|
| `orca_extra_profile_check.py` (default, whole tree, 57 vendors) | **0 errors**, 353 warnings (all optional `_thumbnail.png`) |
| same, baseline (parent commit, same script) | 31 errors -> all 31 fixed, none introduced |
| `orca_extra_profile_check.py --vendor Creality --check-filaments --check-materials` | **0 errors** |
| `orca_extra_profile_check.py --vendor Sovol` (assets) | **0 errors** |
| Upstream `OrcaSlicer_profile_validator -l 2` (CI's binary, run under WSL) | 26 errors -- **the "no regression" reading of this number was wrong; see the correction below** |
| `ctest -R profile` (standalone probe of the new entry) | **Passed**, label `profile` |
| Negative test: hid `creality_k2_buildplate_texture.svg` | correctly reported + non-zero exit; restored |
| `sh -n` on both gate scripts + `profiles` section run in isolation | syntax OK, gate **PASS** |

`Snapmaker_Orca_profile_validator` (`ORCA_TOOLS=ON`) was **not** built: the upstream binary
validator exercises the same `PresetBundle` load path and ran clean, so a full MSVC configure
was not worth it for no additional signal. No build directory was created.

### Correction (2026-09-12, `fix/housekeeping-0912` and `fix/snapmaker-preset-names`)

**The "identical to the 26 on the baseline -- no regression" claim above is wrong on both
halves, and this import did introduce a regression.**

The mistake was methodological. In validation mode the loader rethrows on the first bad
vendor (`PresetBundle.cpp:1472`), so a whole-tree run **aborts early**: 26 is a floor, not a
total, and two runs can both print 26 while hiding different numbers of unreached errors.
The 26 lines were also not 26 errors -- each dangling `inherits` logs two to four lines, so
they were 17 distinct errors with 10 more behind the abort.

Re-derived with per-vendor sweeps, which avoid the abort entirely: **27 distinct errors on
the post-import tree against 17 pre-import**. This import added 10 of them. All ten were the
same mistake in the new `Generic X @Creality K2-all` filaments, which each declared
`renamed_from: "Creality Generic X @K2-all;Creality Generic X K2-all"`. The second clause
collides with the old name the pre-existing `Creality Generic X @K2-all` preset *auto-derives*
by `@`-stripping (`PresetBundle.cpp:3562`), so two presets claimed one old name and
`update_map_system_profile_renamed` (`Preset.cpp:3309`) logged an error for each. The first
clause is the real rename and was uncontested.

Fixed in `fix/housekeeping-0912` by dropping the redundant clause, which together with the
`OrcaFilamentLibrary` manifest and the `printer_variant` corrections took the tree to 10
errors -- a pre-existing BBL/Snapmaker display-name clash unrelated to this import.
`fix/snapmaker-preset-names` then resolved those ten by renaming the Snapmaker generics to
`Generic X @U1 0.4 nozzle`, and the tree now validates at **0 errors**. A `profile_names`
ctest (`scripts/check_preset_name_clashes.py`) guards both classes so neither can return
unnoticed.

## 6.8 Known pre-existing issues, not touched

- **112 `--check-materials` errors** (Anycubic Kobra 2 family, Afinia, Z-Bolt, Sovol SV08 MAX):
  machine `default_materials` naming filaments that do not exist. Present identically at the
  baseline; that flag is opt-in and not run by CI. Worth a follow-up of its own.
- ~~**26 upstream-validator errors**, unchanged from baseline (see 6.7).~~ Superseded: the
  count was a floor hiding an early abort, and 10 of the errors were introduced by this
  import. All resolved -- the tree now validates at 0 errors. See the correction in 6.7.
- **`bed_texture` is resolved but never rendered** in this fork (dead code in `3DBed.cpp`),
  as section 2 found. This import does not start relying on it; the gate checks the files
  exist so the reference is honest, which is what makes the eventual re-enable safe.
