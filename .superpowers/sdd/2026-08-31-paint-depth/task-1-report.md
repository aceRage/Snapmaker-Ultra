# Task 1 — Config surface + walls math + build setup

Worktree: C:\Dev\SnapmakerOrcaNext, branch feat/paint-depth, base 4fb5cca015
Commit: ed726d71e9 "feat(paint-depth): config surface + walls math for perimeter-aware paint depth"

## Status: COMPLETE

All six plan items are implemented, `[paintdepth]` + `[chameleon]` are green, and the
full `ALL_BUILD` gate (required because PrintConfig was touched) passed exit 0. Work is
committed as `ed726d71e9` on `feat/paint-depth`.

Note on sequencing: when I first reached the `ALL_BUILD` gate, `Get-Process
cl,link,MSBuild` showed 21 active `cl.exe` + 17 `MSBuild.exe` processes (real compile
work elsewhere on the machine, not idle node-reuse workers — cl.exe only exists while
actively compiling), so per the BUILD-SLOT RULE I stopped without building or
committing and reported "WAITING FOR BUILD SLOT". The coordinator then confirmed the
slot was free; I re-checked (`Get-Process` returned no matches), ran `ALL_BUILD` via
the wrapper bat, confirmed exit 0, and committed.

## Setup (item 6)

- No prior build tree existed in the worktree. Configured a fresh one:
  `"C:\Dev\tools\cmake-3.31.8-windows-x86_64\bin\cmake.exe" -G "Visual Studio 17 2022" -A x64
  -DCMAKE_PREFIX_PATH="C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local"
  -DCMAKE_BUILD_TYPE=Release ..` into `C:\Dev\SnapmakerOrcaNext\build` — matches
  `C:\Dev\SnapmakerOrca\build`'s CMakeCache.txt (generator "Visual Studio 17 2022",
  platform x64, no explicit toolset, same CMAKE_PREFIX_PATH). Configure succeeded
  (exit 0); only the usual upstream CGAL/CMP01xx dev warnings.
- Discovered the top-level `CMakeLists.txt` has `option(BUILD_TESTS "Build unit tests" OFF)`
  — the initial configure did not build tests. Re-ran cmake in the existing build dir
  with `-DBUILD_TESTS=ON` to add the `tests/` subdirectory (needed for
  `libslic3r_tests`/`[paintdepth]`/`[chameleon]`).
- Wrapper bat written to
  `C:\Users\acesa\AppData\Local\Temp\claude\C--Dev\85fd2715-89f2-41bc-8877-2c5d67ab52c5\scratchpad\build_next_wt.bat`
  (vcvars64 + `cmake --build . --config Release --target ALL_BUILD`, run from inside
  the build dir — `cmake --build <dir> --target <T>` from OUTSIDE the build dir fails
  with `MSB1009: Project file does not exist` on this cmake/generator combo, so the
  wrapper `cd /d`s into the build dir first). A second, narrower wrapper
  (`build_libslic3r_tests.bat`, same directory) targets just `libslic3r_tests` and was
  used for the TDD RED/GREEN cycle below without paying the full `ALL_BUILD` cost twice.

## Implementation (items 1-5)

### 1. New options (`src/libslic3r/PrintConfig.hpp` / `.cpp`)
- `paint_depth_mode` — new `coEnum` `PaintDepthMode { pdmUnlimited, pdmWalls, pdmMillimeters }`
  (declared in the new `src/libslic3r/PaintDepth.hpp`, wired into the config-enum
  static-map machinery the same way `BrimType`/`BrimFilamentSource` are). Default
  `pdmWalls` — the approved bounded-by-default flip.
- `paint_depth_walls` — `coInt`, min 1, default 3.
- `paint_depth_mm` — `coFloat`, min 0, default 1.5.
- All three added to `PrintObjectConfig` beside `mmu_segmented_region_max_width` /
  `mmu_segmented_region_interlocking_depth`, with bleed-rationale tooltips.
- `mmu_segmented_region_interlocking_depth` default flipped 0. -> 0.3 (spec decision 3).

### 2. Legacy handling — decision: full replacement
`mmu_segmented_region_max_width` remains a defined `ConfigOptionDef` (so old
project/preset files still deserialize it without error) but is now **legacy-parse-only**:
it is no longer read by any GUI/config-manipulation code path, and its Tab.cpp row was
removed in favor of the three new options. The actual migration lives in
`PrintConfigDef::handle_legacy_composite` (PrintConfig.cpp, alongside the existing
`wiping_volumes_matrix` migration, since a single legacy key needs to set *two* new
keys — `handle_legacy`'s single-key-to-single-key signature can't do that):
- Guarded on `config.has("mmu_segmented_region_max_width") && !config.has("paint_depth_mode")`
  (never overwrites a config that already carries the new key).
- Nonzero legacy value → `{paint_depth_mode = millimeters, paint_depth_mm = <value>}`.
- Zero (or absent) legacy value → **left alone**, falling through to `paint_depth_mode`'s
  own default (`pdmWalls`) — this is deliberate, not an oversight: the bounded-by-default
  flip (spec decision 1) must apply to every reslice of an old project, not just brand
  new ones, so legacy "disabled" does NOT carry forward as `pdmUnlimited`.
- Task 2 (not this task) is responsible for pointing `multi_material_segmentation_by_painting`
  at the new options instead of `mmu_segmented_region_max_width` directly.

### 3. Interlocking default
Done above (0. -> 0.3), gating logic in the segmentation call (Task 2) and
`ConfigManipulation.cpp`'s beam-interlocking mutual-exclusion toggle are unchanged —
both already only activate the interlocking depth when the width clamp is non-zero /
depth is bounded.

### 4. Pure helper — new files `src/libslic3r/PaintDepth.hpp` / `.cpp`
`float paint_depth_band_mm(PaintDepthMode mode, int walls, double mm, float ext_perimeter_width, float perimeter_spacing)`:
- `pdmUnlimited` → `0.f` (matches the pre-existing "0 = disabled" `cut_segmented_layers`
  convention).
- `pdmMillimeters` → `mm` verbatim, ignoring wall count/flow widths.
- `pdmWalls` → `ext_perimeter_width + (walls-1)*perimeter_spacing`, the
  `fuzzy_skin_segmentation_by_painting` precedent
  (`MultiMaterialSegmentation.cpp:2237-2253`) extended to N walls; `walls` clamped to
  >= 1 internally so 0/negative input degrades to a one-wall-wide band instead of
  going negative.
- Added to `src/libslic3r/CMakeLists.txt` beside `BrimFilament.cpp/.hpp`.
- Test file `tests/libslic3r/test_paint_depth.cpp`, new `[paintdepth]` tag, added to
  `tests/libslic3r/CMakeLists.txt` beside `test_chameleon_brim.cpp`. 4 test cases / 10
  assertions: unlimited-mode-always-0, millimeters-verbatim, walls-mode arithmetic
  (walls=3 and walls=1), and edge cases (walls clamped to >=1 for 0/negative input,
  zero flow widths collapse the band to 0).
- **Real TDD RED observed**: built the `libslic3r_tests` target with the test file
  present but `PaintDepth.hpp`/`.cpp` not yet written — build failed (link error, no
  `paint_depth_band_mm` symbol). Implemented the header/source, rebuilt — GREEN (see
  Testing section below).

### 5. UI wiring
- `src/slic3r/GUI/Tab.cpp` (Multimaterial page, Advanced optgroup): replaced the
  `mmu_segmented_region_max_width` row with `paint_depth_mode` / `paint_depth_walls` /
  `paint_depth_mm` rows, immediately before the (unchanged)
  `mmu_segmented_region_interlocking_depth` row.
- `src/slic3r/GUI/ConfigManipulation.cpp` (`update_print_fff_config`): added
  `toggle_line("paint_depth_walls", mode == pdmWalls)` /
  `toggle_line("paint_depth_mm", mode == pdmMillimeters)` greying, placed right before
  the existing beam-interlocking toggle block.
- `src/libslic3r/PrintObject.cpp` (`~line 957`, posSlice invalidation group): added
  `paint_depth_mode` / `paint_depth_walls` / `paint_depth_mm` alongside
  `mmu_segmented_region_max_width` / `mmu_segmented_region_interlocking_depth`.
- `src/libslic3r/Preset.cpp` (`s_Preset_print_options`, ~line 928): added the three new
  keys beside the existing mmu keys (this list feeds `Preset::print_options()`, used for
  preset-diff/options-panel machinery elsewhere).

## Testing

- `[paintdepth]`: **4 test cases / 10 assertions, all passed.**
- `[chameleon]`: **133 test cases / 605 assertions, all passed** — byte-identical count
  to the pre-existing baseline, confirming no regression from the PrintConfig/Tab/
  ConfigManipulation/PrintObject changes shared with that feature.
- `libslic3r_tests` (target, not full app) built clean, exit 0, after the helper was
  implemented.
- **Full `ALL_BUILD` (config Release): exit 0, 0 errors.** This is the task's actual
  completion gate (PrintConfig was touched); it built the full GUI app plus every test
  binary (`fff_print_tests`, `libnest2d_tests`, `slic3rutils_tests`, etc.) with the new
  options in place, only the usual pre-existing `LNK4098`/upstream warnings.

## Concerns / notes for reviewers

- The choice to fully remove `mmu_segmented_region_max_width` from the Tab.cpp UI
  (rather than leaving it visible "beside" the new rows) was a judgment call the plan
  explicitly left open ("decide whether the old key remains the internal carrier or is
  fully replaced; prefer full replacement"). I judged that leaving a now-inert raw-float
  control next to the new mode-driven controls would be confusing (two ways to set
  "depth" in the UI, only one of which does anything post-Task-2), and the plan's stated
  preference is full replacement. The option definition, Preset-list entry, and
  handle_legacy_composite migration all still treat it as a real, serializable
  (if hidden) key.
- Task 2 (not implemented here) still needs to point
  `multi_material_segmentation_by_painting` at the new options via
  `paint_depth_band_mm` — until then, the new options exist and are fully wired
  through config/UI/invalidation but have no runtime effect on segmentation (an
  intentional Task-1/Task-2 boundary per the plan).
- Help-doc anchors used in the new `append_single_option_line` calls
  (`multimaterial_settings_advanced#paint-depth-mode` etc.) are placeholders following
  the existing anchor-naming convention; the actual doc page/anchors were not created
  (out of scope for this task).
