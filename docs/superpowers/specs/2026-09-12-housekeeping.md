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

`gate_all.sh` itself was not run, per the brief.
