# Headless Slicer Roadmap (CLI -> plate model -> JSON-RPC daemon -> test harness)

Date: 2026-09-01 - Status: research + roadmap, nothing implemented - Scope: FFF only, Windows-first.
All paths relative to `C:\Dev\SnapmakerOrca`; line numbers are from the tree on this date.

## Summary

- A windowless code path already exists: `CLI::run` (src/Snapmaker_Orca.cpp:1042), inherited from
  BambuStudio's cloud slicer. It is windowless but not headless: it links `libslic3r_gui`
  (src/CMakeLists.txt:161), instantiates `Slic3r::GUI::PartPlateList` with a null Plater (3046),
  renders plate thumbnails through GLFW + OpenGL (88, 5471-5510), calls wx statics (1082) and reaches
  `wxGetApp()` - which has no `wxApp` behind it in CLI mode - on at least two paths (see Gaps 3).
- Windows is second-class: the progress pipe and `result.json` are compiled only for Linux
  (173-366, 405-445). On Windows the only machine-readable output is the exit code
  (src/libslic3r/Utils.hpp:24-70) plus the log.
- Recommendation, in order: (A) make the CLI a trustworthy agent surface on Windows (JSON results and
  progress, crash fix, regression tests); (B) extract a wx/GL-free `PlateSet` from `PartPlate` into
  libslic3r so plates can be built and sliced without the GUI library; (C) build `slicerd`, a
  long-running JSON-RPC-over-stdio daemon (optional localhost HTTP) on libslic3r + PlateSet + a
  wx-free PrintHost adapter; (D) a ctest/pytest harness with tolerant G-code diffing, because this
  fork's G-code is not byte-reproducible.
- A is cheap and unblocks agents now; B is the load-bearing refactor that makes C small; D rides on A
  from day one and on C later.

## Current CLI capabilities and gaps

Entry `CLI::run` (1042); no action => launches the GUI (1141-1186). `CLI::setup` parses options and
splits them into actions/transforms (6060-6154). Option definitions: src/libslic3r/PrintConfig.cpp:8526
(actions), 8687 (transforms), 8808 (misc). Exit codes: src/libslic3r/Utils.hpp:24-70; message table
Snapmaker_Orca.cpp:115-150.

What works:

| Area | Options and code |
|---|---|
| Load | STL/OBJ/3MF via `Model::read_from_file` (1413). A BBS 3MF must be the first file and brings config, plate data and project presets (1397-1404); other inputs are model-only (1408). STEP is not loadable: `read_from_file` has no STEP branch; only `Model::read_from_step` (src/libslic3r/Model.cpp:180) exists and takes GUI-style progress/mesh-parameter callbacks. |
| Presets | `--load-settings "machine.json;process.json"`, `--load-filaments "f1.json;..."` through `load_config_file` -> `ConfigBase::load_from_json` (1693-1705); `--uptodate`, `--downward-check`. |
| Objects | `--load-filament-ids` sets `extruder` on every object of the Nth input (1539-1583; STL/OBJ only, rejected with 3MF at 1398); `--clone-objects`, `--skip-objects`, `--repetitions`, `--ensure-on-bed`, `--rotate*`, `--scale`, `--convert-unit`, `--assemble`, `--load-assemble-list`. |
| Plates | Plates come from the 3MF (`load_from_3mf_structure`, 3345) or from `--arrange 1` (`arrangement::arrange` 4034/4439 + `rebuild_plates_after_arrangement` 4518/4690); `--orient` (3791). Wipe tower is estimated and placed per plate by CLI-local code (3127-3250, 4289-4387). Plain STL input forces `plate_to_slice = 0` (1652). |
| Slice | `--slice 0` (all) / `--slice N`. Per plate: `update_print_volume_state` (4900), config = plate config + extra (4989-4990), `print->apply` (4991), `validate` (5017), `process` (5116-5131), `export_gcode` (5185) to `<outputdir>/plate_N.gcode` (5180); conflict and warning gating (5135-5171); `--mtcpp/--mstpp` limits; `--export-slicedata/--load-slicedata` caches. |
| Export | `--export-3mf` (5304 -> `CLI::export_project` 6275-6311 -> `store_bbs_3mf`), `--export-stl(s)`, `--export-obj`, `--export-settings`, `--info`. |

Gaps that matter to an agent:

1. No machine-readable result on Windows: `record_exit_reson` writes `result.json` only under
   `__linux__` (405-445); `--pipe` progress is Linux-only (173-366, 1289-1293).
2. No estimates in any output except inside the exported 3MF (`PlateData::gcode_prediction` /
   `gcode_weight`, src/slic3r/GUI/PartPlate.cpp:5485-5497) and the G-code header. The data exists:
   `GCodeProcessorResult::print_statistics` (src/libslic3r/GCode/GCodeProcessor.hpp:232, `modes[]` :98),
   `Print::print_statistics()` (src/libslic3r/Print.hpp:819-832), warnings in `g_slicing_warnings`
   (171, 368-376) and `GCodeProcessorResult::warnings` (GCodeProcessor.hpp:236). None is serialized.
3. `--export-3mf` after slicing crashes (spike/FINDINGS.md:38-39). Most likely mechanism, unverified by
   debugger: thumbnails are regenerated whenever objects were arranged (5344; STL input always arranges,
   1537); the render path `render_thumbnail_internal` (src/slic3r/GUI/GLCanvas3D.cpp:6181) ->
   `GLVolume::simple_render` (src/slic3r/GUI/3DScene.cpp:514, `model.render()` at 602) ->
   `GLModel::render` -> `wxGetApp().get_current_shader()` (src/slic3r/GUI/GLModel.cpp:602, also 671).
   With no `wxApp`, `wxGetApp()` is a null reference. When no GL context can be made the stage is
   skipped (5511-5513) and the export "works" without thumbnails, which is why it is environment
   dependent. Second unguarded site: `PartPlateList::select_plate` calls `wxGetApp().plater()`
   (PartPlate.cpp:4070, fork-added `notify_filament_usage_changed`; also 4547), reachable from the CLI
   at 3922 and 4251 (`--slice N --arrange 1`).
4. `--load-filament-ids` did not map STL objects in the spike (FINDINGS.md:40-41) although 1583 sets
   `extruder`; needs a regression test before anyone trusts it.
5. System presets by name resolve only from `resources/profiles/BBL/{machine_full,process_full,
   filament_full}` and `BBL/cli_config.json` (1793, 1993, 2054, 2074, 2095, 2165, 2204, 2226, 2485,
   3375-3378). No vendor in this fork ships a `machine_full` directory, so `--uptodate` and
   machine-switch flows are dead for every vendor; only explicit, inherits-flattened JSON files work.
6. No `PresetBundle`: the CLI never loads vendor bundles or user presets, so "select printer X /
   filament Y by name" is impossible.
7. CLI G-code has no embedded thumbnails: `export_gcode(outfile, gcode_result, nullptr)` (5185) passes
   no callback (U1/Moonraker previews lose out). Plate thumbnails exist only via the GLFW path.
8. Single-shot process: models and presets reload on every call; no cancel; no per-object settings other
   than `extruder`; no plate create/assign commands; no upload/send.
9. The "CLI" is the 68 MB `Snapmaker_Orca.dll` (src/CMakeLists.txt:124; stub exe :193); every run pays
   GUI-library static initialisation.

## GUI-coupled pieces

| Piece | Where | Coupling | Headless status |
|---|---|---|---|
| Model, Print, PrintObject, PrintConfig, PrintBase status/cancel | src/libslic3r: Print.hpp:922 `export_gcode`, :979 `validate`, :1060 `set_plate_origin`; PrintBase.hpp:404 `ApplyStatus`, :437 `SlicingStatus`, :482 `set_status_callback`, :490/:518 cancel | none | usable now; tests/fff_print/test_data.cpp:198-221 is the minimal recipe (arrange, `apply`, `validate`, `process`) |
| 3MF plate metadata | src/libslic3r/Format/bbs_3mf.hpp:52-108 `PlateData`, :239 `load_bbs_3mf`, :266 `store_bbs_3mf(StoreParams)` | none | usable |
| GCodeProcessor (estimates, warnings, `process_file` :809 for existing G-code) | libslic3r | none | usable |
| PresetBundle / Preset / AppConfig | src/libslic3r/PresetBundle.hpp:71 `load_presets(AppConfig&, ...)`, :254 `load_vendor_configs_from_json`, :461 `full_fff_config`; src/libslic3r/AppConfig.cpp | wx-free (only comment hits), but instantiated only by GUI_App (src/slic3r/GUI/GUI_App.cpp:2226, 2758) and expects the `data_dir()/system` vendor cache | usable with a prepared data dir |
| Arrange / Orient engines | src/libslic3r/Arrange.hpp, Orient.hpp | none | usable; plate-aware pre/post-processing lives in PartPlateList (PartPlate.hpp:782-794) |
| PartPlate / PartPlateList | src/slic3r/GUI/PartPlate.hpp:77, :528 | plate data (93-116, 154: instance set, origin/size, per-plate `DynamicPrintConfig`, Print*/GCodeResult*, slice state) interleaved with GL state (129-160); 47 `wxGetApp()` calls (e.g. .cpp:205, 302, 1351-1370, 3569, 4070, 4547) and 26 `m_plater` guards; a CLI-only twin `get_extruders_under_cli` (.cpp:1476) shows the pattern | CLI uses it with `plater=NULL` (3046); works only where guarded |
| Plater | src/slic3r/GUI/Plater.cpp: `update_background_process` 13539, `export_gcode` 14212, `on_process_completed` 15690, `export_3mf` 21367, `send_to_printer` 20860, `arrange` 23798, `select_plate` 24170 | wx everywhere | not reusable; its call order is the spec |
| BackgroundSlicingProcess | src/slic3r/GUI/BackgroundSlicingProcess.hpp:80; `process_fff` .cpp:195-257: `is_BBL_printer` from `wxGetApp().preset_bundle` (198), `process()` (229), `export_gcode` with thumbnail callback (240), post-process scripts (242), finalize/export/upload (247-255) | wx events, PartPlate | the sequence to replicate, incl. SEH wrappers on Windows (hpp:198-205) |
| Thumbnails | GLCanvas3D.cpp:6181 internal, :6426 FBO variant; CLI: hidden GLFW window (5471-5504; OSMesa on Linux 5495), `OpenGLManager::init_gl` (5510; needs GL >= 2.0 and FBO, OpenGLManager.cpp:256-269), shader via `opengl_mgr.get_shader("thumbnail")` (5562) | GL context + `wxGetApp()` in GLModel.cpp:602 | needs the shader-lookup fix or a CPU renderer |
| Wipe tower placement | per-plate default from `wxGetApp().preset_bundle->project_config` + `printer_structure` (PartPlate.cpp:3567-3585, guarded at 3288); `estimate_wipe_tower_size/polygon` (PartPlate.hpp:306-307) | preset bundle | CLI re-implements placement (3127-3250, 4289-4387) |
| Filament / AMS mapping | GUI SelectMachine dialog; plugin `PrintParams::ams_mapping/ams_mapping2` (src/slic3r/Utils/bambu_networking.hpp:247-249) | GUI | not headless |
| PrintHost upload | src/slic3r/Utils/PrintHost.hpp:47-77 (`wxString` at :10, :53-60); factory PrintHost.cpp:41-72; OctoPrint multipart to `api/files/local` (OctoPrint.cpp:383-407); `Http` (Utils/Http.hpp) is wx-free; Snapmaker U1 profiles use `host_type: octoprint` (resources/profiles/Snapmaker/machine/*.json:30) | `wxString` callbacks; `PrintHostJobQueue` needs a dialog (hpp:247); compiled into libslic3r_gui (src/slic3r/CMakeLists.txt:721-766) | thin adapter needed |
| Bambu network plugin | src/slic3r/Utils/NetworkAgent.hpp (function-pointer table, `start_print` :188); DLLs loaded from `data_dir/plugins` (NetworkAgent.cpp:232-262, 534-553); init in GUI_App.cpp:3298-3347 | GUI_App owns agent + DeviceManager | loadable headless (same toolchain/ABI), never redistribute the DLLs |
| Localhost HTTP | src/slic3r/GUI/HttpServer.hpp:74 (boost.beast/asio, port 13618 at :17) | GUI namespace by placement only | reusable skeleton for a daemon transport |

Prior art worth mirroring (from memory): PrusaSlicer CLI (`--export-gcode --load cfg.ini`, transforms,
`--info`; one Print, no plates); BambuStudio/OrcaSlicer CLI = this one (`--slice N`, `--export-3mf`,
`--load-settings`, Linux `--pipe` + `result.json` because it runs in their cloud); CuraEngine's
`slice`/`connect` model - a warm engine process driven over a socket with streamed progress and layer
data, which is the shape Phase C should take; "slic3r as a library" - libslic3r was designed to be
embedded, and tests/fff_print/test_data.cpp is the proof that Model + config -> Print works with no GUI.

## Proposed architecture

Layers, bottom-up:

1. libslic3r, surface unchanged: Model, Print, PrintConfig, PresetBundle, AppConfig, bbs_3mf,
   GCodeProcessor, Arrange, Orient.
2. New `src/libslic3r/PlateSet.{hpp,cpp}`: the headless plate model extracted from PartPlate(List):
   geometry (origin, w/d/h, shape, exclude areas, stride/cols), instance membership, per-plate config
   (bed type, print sequence, name, lock, filament map, layer sequences), owned `Print` +
   `GCodeProcessorResult` per plate, slice-state flags, `PlateData` (de)serialisation, wipe-tower
   default/estimate parameterised by config instead of `wxGetApp()`, arrange pre/post helpers.
   `GUI::PartPlate` becomes a view over `PlateSet::Plate` (GL, hover, textures only).
3. New `src/slicerd/`: `SlicerSession` = the BackgroundSlicingProcess sequence without wx: load ->
   presets -> edits -> arrange/orient -> apply/validate/process on a worker thread with
   `set_status_callback` and cancel -> results -> export -> upload. PrintHost through a wx-free
   adapter (std::string error/info callbacks).
4. Transport: JSON-RPC 2.0, newline-delimited on stdin/stdout (subprocess-native for agents and CI, no
   ports, no auth); optional `--http 127.0.0.1:<port>` reusing the boost.beast server with a token.
   Notifications: `job.progress {job, plate, percent, text, warnings[]}`.
5. Executable `snapmaker-orca-slicerd`: new `add_executable` linking `libslic3r` + a small `slic3r_net`
   library (Http, PrintHost adapters); no wx, no GL, no libslic3r_gui. Precedent:
   `Snapmaker_Orca_profile_validator` links only libslic3r (src/CMakeLists.txt:98-114). Thumbnails, if
   required, via an optional GL helper process or a CPU rasteriser (AGG is in-tree:
   src/libslic3r/SLA/AGGRaster.hpp).
6. Clients: `tools/slicerd/client.py` (subprocess + JSON lines) shared by agents, pytest and CI.

Method surface v1: `session.open/close/reset`; `project.load(files)`, `project.export3mf(plate?)`;
`presets.list(type)`, `presets.select(printer, filaments[], process)`; `config.get/set(scope:
global|plate|object|volume, key, value)` validated against `print_config_def`;
`objects.list/transform/clone/remove`; `plates.list/create/delete/assign/arrange/orient`;
`slice.start(plate|all) -> job`, `job.status/cancel`; `result.get(plate)` (time per mode, filament
mm/g/cost per extruder, layer count, conflicts, `toolpath_outside`, warnings); `gcode.export(plate,
path)`; `printer.test/upload(host, plate, start)`; later `printer.bambu.*` behind a flag.

## Phases (A-D)

### A. Harden the CLI as the first agent surface - S/M

Files: src/Snapmaker_Orca.cpp; src/slic3r/GUI/GLModel.cpp; src/slic3r/GUI/PartPlate.cpp;
src/libslic3r/PrintConfig.cpp (CLI defs); new scripts/orca_cli.py; tests/.

- Cross-platform machine output: compile `record_exit_reson` (405-445) on all platforms; replace the
  Linux named pipe with `--progress-json` JSON lines on stdout using the existing message shape
  (219-229).
- Add per-plate estimates and warnings to `result.json` from GCodeProcessor.hpp:98/232/236,
  Print.hpp:819-832 and `g_slicing_warnings`.
- Fix the post-slice crash: let `GLModel::render` take the current shader from `OpenGLManager`
  (`GLShadersManager::get_current_shader`, GLShadersManager.cpp:93) instead of `wxGetApp()`
  (GLModel.cpp:602/671); guard PartPlate.cpp:4070/4547 with `m_plater`; add `--no-thumbnails`.
  Confirm with a debugger run before changing anything.
- Pass a thumbnail callback at 5185 (or `--no-thumbnails` explicitly) so U1 G-code can carry previews
  once thumbnails are headless.
- Preset by name: replace the `BBL/*_full` lookups (1793-2226, 3375-3378) with
  `PresetBundle::load_vendor_configs_from_json` over `resources/profiles/<vendor>` plus
  `--printer/--filament/--process <name>`; or document "flattened JSON only" and ship a flattener.
- Regression tests: `--load-filament-ids` on two STLs, `--slice 2`, `--export-3mf` round trip, exit
  codes.

Risks: the CLI file is 6.4k lines of BBS cloud logic - keep edits additive. After A: an agent can slice
any 3MF/STL with given presets on Windows, read time/filament/warnings, get G-code and a re-loadable
3MF, and diff runs.

### B. Headless plate model (PlateSet) - L

Files: new src/libslic3r/PlateSet.{hpp,cpp}; src/slic3r/GUI/PartPlate.{hpp,cpp} (5k lines -> view);
Plater.cpp call sites of `get_partplate_list()`; src/slic3r/GUI/Jobs/ArrangeJob.cpp;
src/Snapmaker_Orca.cpp (switch 3046 to PlateSet); tests/libslic3r/test_plateset.cpp.

Minimal API: `PlateSet(bed shape, exclude areas, height, printer_structure)`; `create/delete/select`;
`add_instance/remove_instance/find_instance_belongs`; `plate(i).config()`;
`plate(i).print()/result()`; arrange helpers; `to_plate_data()/from_plate_data()`;
`default_wipe_tower_pos(i)`; `used_extruders(i, full_config)`.

Approach: move logic, do not rewrite. Keep `PartPlate` cereal undo/redo (hpp:495-518) by serialising the
embedded `PlateSet::Plate`; keep `m_plates_mutex` semantics; GUI-only behaviours (spiral-vase dialog at
.cpp:344/4433, `obj_list` refreshes, filament-usage notify) stay in the view.

Risks: GUI regressions in plate selection and filament usage; large diff colliding with in-flight
feature branches (support-match, paint-depth). After B: plates can be built, populated and sliced with
no GUI library; the CLI drops libslic3r_gui except for thumbnails.

### C. Long-running daemon (slicerd) - M/L (M once B has landed)

Files: new src/slicerd/{main.cpp, Session.{hpp,cpp}, Rpc.{hpp,cpp}, Jobs.cpp};
src/slic3r/Utils/PrintHost.hpp (wx-free base with std::string callbacks, wx adapters stay in GUI);
src/CMakeLists.txt (target); tools/slicerd/client.py; docs.

Behaviour: one session per process because libslic3r carries process-global state
(`Model::setExtruderParams/setPrintSpeedTable` statics, Model.hpp:1614-1615, used by the CLI at
5108-5109; `resources_dir()`; PlaceholderParser); one job at a time; the worker thread mirrors
`BackgroundSlicingProcess::thread_proc` including SEH wrapping on Windows; `apply()` never runs
concurrently with `process()`; cancel via `PrintBase::cancel` + `CanceledException`.

Presets: `AppConfig` + `PresetBundle::load_presets` from `--datadir` (default the GUI's
`%APPDATA%\Snapmaker_Orca`, opened read-only) so the agent sees the same user presets; fall back to
`resources/profiles` when the `system` cache is absent.

Send: OctoPrint/Moonraker through `Http` (U1 uses the octoprint host type); Bambu through NetworkAgent
only when the plugin is already present in `data_dir/plugins`, never bundled.

Risks: PrintHost signature churn across ~15 host classes (adapter avoids most of it); memory growth
(`session.reset` frees `Print`); Windows stdio buffering (binary mode, flush per message). After C: a
chat agent or CI drives load -> inspect -> edit -> slice -> read estimates -> export -> upload
interactively with progress and cancel.

### D. Test harness and tolerant G-code diffing - M

Files: tests/headless/ (Catch2, links libslic3r + PlateSet); tools/gcode_diff.py; tests/data fixtures;
.github/workflows.

- Determinism knobs: `--threads N` via `tbb::global_control` (precedent src/libslic3r/utils.cpp:189);
  fixed `datadir`; pinned preset files; bundle versions logged in `result.json`.
- Diff policy - this fork is not byte-reproducible (address-layout knife edge in solid-infill slivers,
  independent of threading): strip header/footer comments, `M73`, `; filament used`,
  `; model printing time`, timestamps; drop travel-bounded extrusion blocks with XY extent < 0.2 mm;
  then compare per-layer feature stats and quantised segment sets. Reuse the SliceDiff engine design
  from docs/superpowers/specs/2026-08-29-slice-compare-design.md (10 um segments, 10 mm cells,
  Z-matched layers, proximity rescue).
- Assertions: layer count exact; estimated time within 2 %; filament within 1 %; warning set equal;
  conflict and `toolpath_outside` flags equal.
- CI: Windows (MSVC) and Linux (OSMesa for optional thumbnails) running orca_cli.py / client.py
  against golden fixtures.

After D: every feature branch gets automatic slice regression checks; agents get a stable oracle.

## Risks

- Fork-specific `wxGetApp()` leaks into shared code (PartPlate.cpp:4070/4547) will recur; add a CI
  grep forbidding `wxGetApp` in PlateSet/slicerd and in CLI-reachable PartPlate paths.
- Thumbnails on Windows services or disconnected RDP sessions: the hidden GLFW window needs a real
  GL 2.0+ context. Options: a helper process on an interactive session; Mesa llvmpipe placed next to
  slicerd only (MIT-licensed, not a vendor DLL); the CPU rasteriser. Decide before promising thumbnails.
- Vendor DLLs (`bambu_networking.dll`/`BambuSource.dll`, `FlashNetwork.dll`) must never ship with
  slicerd; load from the user data dir or not at all.
- Preset-cache dependence (`data_dir/system`) can make headless results differ from the GUI when caches
  diverge; log bundle versions and fail loudly on mismatch.
- Non-determinism makes golden byte diffs impossible; metrics plus tolerant diff only.
- The PlateSet refactor collides with in-flight branches; land B in a quiet window with a GUI round.
- std::string across the plugin DLL boundary requires the same MSVC runtime as the plugin; one
  toolchain only.
- Unverified crash hypothesis (Gaps 3): if the debugger shows a different site, Phase A's fix changes
  but not its scope.

## Open questions

1. Transport for v1: stdio JSON-RPC only, or HTTP as well (needed by non-subprocess clients)?
2. Are plate thumbnails required in agent-produced 3MFs (Bambu printers display them; the U1 uses
   G-code previews), or is `--no-thumbnails` acceptable for CI?
3. Should slicerd share the GUI's data dir (user presets, physical printers with credentials) or run
   isolated with its own?
4. Per-object settings API: raw key/value validated by `print_config_def`, or a curated subset?
5. Does A need preset-by-name, or is flattened-JSON-only enough until C?
6. Multiple sessions per daemon (requires de-globalising the Model statics) or one session per process?
7. Python bindings (pybind11 over libslic3r) instead of, or in addition to, the RPC daemon - later or never?
