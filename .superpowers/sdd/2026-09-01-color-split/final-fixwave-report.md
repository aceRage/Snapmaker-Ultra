# Colour split — final fix wave (Ruling 28)

Date: 2026-09-02 · Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/color-split` off `faaf626985`
Commits: `f443fbd33f` fix(color-split): Z-preserving mesh path, state overflow, job gizmo guard ·
`92ca134e1b` docs(color-split): spec rev 2.14 — Z-preserving mesh path, overflow rule, partition cost

Status: **DONE**. All three findings implemented, three new tests seen RED then GREEN, ALL_BUILD exit 0,
four suites green, spec amended to rev 2.14.

---

## Critical 1 — Z-dependent rules ran in mesh space for rotated/mirrored parts

**What changed.** `color_split_space` now takes the mesh-space path only when T = instance × volume is
isotropic **and** Z-preserving.

- `src/libslic3r/ColorSplit.cpp:269` — `const bool z_preserving = std::abs(L(0, 2)) < 1e-6 * sx && std::abs(L(1, 2)) < 1e-6 * sx && L(2, 2) > 0.;`
- `src/libslic3r/ColorSplit.cpp:271` — `z_preserving` added to the existing isotropy conjunction (equal
  column norms + the Gram off-diagonals); everything else falls through to the unchanged world path at
  `ColorSplit.cpp:275-277` (`world_path = true; to_split = T; from_split = T.inverse()`).
- `src/libslic3r/ColorSplit.cpp:261-268` — the reason recorded in place: the flat test
  (`ColorSplitShell.cpp:155` region, `unit_face_normal(p, f).z()`), the flat-core projection
  (`flat_core_survives` → `projection_survives(..., Vec3f::UnitZ(), 1.5*ws)`, `ColorSplitShell.cpp:117`),
  the Case A/B choice (`n_p.z()` vs `n_q.z()`, `ColorSplitShell.cpp:421`) and the group mean-normal
  `UnitZ` fallback (`ColorSplitShell.cpp:128`) all read the split space's z, while spec §3.5/§3.6 define
  "flat" and "more horizontal" in the PRINT frame.
- `src/libslic3r/ColorSplit.hpp:117-126` — `ColorSplitSpace`'s header comment rewritten: mesh path = an
  isotropic **and Z-preserving** T (a turn about z, an x/y mirror, a uniform scale), exact for every
  instance sharing that transform; anisotropic **or tilted** goes to the world path of the FIRST instance,
  which now carries an approximate up-axis as well as an approximate depth for other instances.

Paint is still read in mesh space (Ruling 23) — `to_split` is applied to the retriangulated surface only,
so the world path was already exact for a pure rotation ("a rotated volume on the world path stays in
place", `test_color_split.cpp:1550` region) and no new geometry code was needed.

**Note on the test fixture.** The finding's sketch said `all_with(CUBE_TOP, …)` with the instance rotated
90° about x. Those two cannot both hold: R_x(90°) maps mesh +z to world −y, so a painted mesh-TOP becomes a
world SIDE face, not the world top the finding's prose asks for ("a cube rotated 90° about X with its
**world-top** painted"). The mesh face that R_x(90°) carries onto world +z is +Y, so the test paints
`CUBE_PLUS_Y` ( = facets {10, 11} , a new constant beside the existing `CUBE_PLUS_X`) and uses a 40 mm cube
so the world footprint is the 40 × 40 the finding's `[ws, 40 − ws]` assertion assumes. Everything else in
the sketch is implemented verbatim.

**New tests.**

- `tests/libslic3r/test_color_split.cpp:1633` — *a tilted isotropic instance takes the world path and caps
  the WORLD top*. `painted_model(make_cube(40,40,40), all_with(CUBE_PLUS_Y, Extruder2))`, instance
  `set_rotation(Vec3d(PI/2, 0, 0))`, `cap_and_step()`, depths from
  `color_split_depths(split_test_config(), {1,2})` (D = 1.40885, ws = 0.79708, cap_top = 0.8, h = 0.2).
  Asserts `space.world_path`; runs the split with `space.to_split` and `apply_color_split`; then, in WORLD
  space (every piece vertex through `instance->get_matrix() * piece->get_matrix()`):
  * the object's world bbox is unchanged by the split (1e-3);
  * `wb.size().z() == cap_top` within 1e-3, and `wb.max.z()` is the object's world top — i.e. the painted
    face is capped at the top-shell depth, not cut at D;
  * the vertices below the surface layer (`world z < top − h − 1e-4`, which is the bottom ring, since the
    ring copies sit exactly one layer down) are inset by exactly `ws` from all four world side faces
    (1e-3 on each of min.x, max.x, min.y, max.y) — spec §3.6 case A on the world top.
- `tests/libslic3r/test_color_split.cpp:1690` — *a z-mirrored instance takes the world path*.
  `set_mirror(Vec3d(1,1,-1))` on the instance → `world_path` true and `det(to_split.linear()) < 0`.

**Existing test updated.** `test_color_split.cpp:1342`, formerly "a rotated, scaled and mirrored PART stays
in place", asserted `!space.world_path` for a volume rotation of `(0.3, 0.2, 0.7)` — precisely the
behaviour this finding removes, and it would have failed on the `from_split` round trip. It is now
"a **z-rotated**, scaled and mirrored PART stays in place" with `set_rotation(Vec3d(0., 0., 0.7))` (scale
1.5 and mirror (−1,1,1) unchanged), which is the positive control for the new rule; its tilted half is
covered by the new world-path test, and a comment says so.

## Important 2 — spec §3.1's state-overflow rule

**What changed.**

- `src/libslic3r/ColorSplit.hpp:75-81` — `int max_state = 0; // 0 = unlimited` added to
  `ColorSplitParams`, documented as "physical extruders plus the ENABLED mixed (virtual) filaments".
- `src/libslic3r/ColorSplit.cpp:181-200` — new file-static `drop_overflow_states(patches, max_state,
  notes)`: pushes one note per dropped state, re-labels every `facet_state > max_state` to 0 (the facets
  stay in the surface F and simply join the body — they are not removed, so F keeps its zero open edges and
  the partition still sums to the whole part) and erases those states from `patches.states`. `states` is
  ascending, so the common case costs one comparison and the whole helper is a no-op.
- `src/libslic3r/ColorSplit.cpp:212-225` — applied in `split_volume_by_paint` right after
  `extract_color_patches`, so `extract_color_patches` itself is untouched and its tests stay green (the
  smaller change of the two the finding offered). The notes are collected into the same vector
  `build_color_shells` later appends to (`ColorSplit.cpp:230`), so they lead the result's warnings through
  the existing merge at `ColorSplit.cpp:232`.
- **Edge case not in the finding, decided here:** when the filter empties the state list, the split raises
  `ColorSplitError(<the notes, joined>, ColorSplitErrorKind::nothing_to_split)` instead of the generic
  "The part has no painted colours." Otherwise the job would replace the source volume with an identical
  body and say nothing about why. `nothing_to_split` is Ruling 27(2)'s warning kind, so the GUI shows it as
  a plater warning naming the unavailable filament.
- `src/slic3r/GUI/Plater.cpp:24074-24084` — the GUI sets it:
  `params.max_state = int(fff_print().mixed_filament_manager().total_filaments(num_extruders));` with
  `num_extruders = base.option<ConfigOptionFloats>("filament_diameter")->values.size()`. That is exactly
  `Print::apply`'s pair (`PrintApply.cpp:1379` for the physical count, `:1484` for `num_total_filaments`),
  so mixed VIRTUAL ids stay valid and only real overflow is dropped. **The accessor was found**:
  `Print::mixed_filament_manager()` is public (`src/libslic3r/Print.hpp:1032-1033`) and `Plater::fff_print()`
  is a member (`src/slic3r/GUI/Plater.hpp:326-327`), so no fallback was needed; the
  `preset_bundle->filament_presets.size()` fallback is wired only for the impossible null
  `filament_diameter` option. `params` changed from `const` to non-const; `scale_params` copies the field
  through unchanged.

**New test.** `tests/libslic3r/test_color_split.cpp:1141` — *a paint state above the printer's filament
count stays in the body colour*. A 40×40×20 cube with state 4 on the top and state 2 on the +X face:
`max_state = 0` → two pieces (filaments 2 and 4), no warnings; `max_state = 2` → one piece (filament 2),
exactly one warning containing "Filament 4", body + piece still equals the whole block, the body is larger
than in the two-colour run (the state-4 slab went back to it), and the surviving piece's volume is
unchanged within 1e-3.

## Minor 3 — the job's `finalize` did not re-check the painting gizmo

`src/slic3r/GUI/Jobs/ColorSplitJob.cpp:97-108` — the same guard as `Plater::split_by_color`
(`Plater.cpp:23993-24000`) now runs at the top of `finalize`, after the `canceled || eptr` early-out and
**before** any mutation (before the snapshot, the id/timestamp/triangle-count re-checks and
`apply_color_split`), aborting with the existing `_u8L("Close the painting tool before splitting by
colour.")` plater warning. Includes added: `slic3r/GUI/GLCanvas3D.hpp`, `slic3r/GUI/Gizmos/GLGizmosManager.hpp`.

## Docs

`docs/superpowers/specs/2026-09-01-color-split-design.md` → **rev 2.14** (header line 3).

- §3.1 gains the implemented overflow rule: `max_state`, the re-labelling, the one-note-per-state wording,
  the `nothing_to_split` fallout, and the GUI's `total_filaments(filament_diameter.size())`.
- §3.9 rewritten: mesh path iff isotropic **and** Z-preserving, with the numeric test spelled out
  (|L(0,2)| < 10⁻⁶·s, |L(1,2)| < 10⁻⁶·s, L(2,2) > 0), which transforms qualify, and why isotropy alone is
  not enough (the 90°-about-x cube and the z mirror, named).
- §12 "Documented limits" gains: the partition cost is **O(shells × mesh)** — one Manifold `Split` per
  shell against the whole remainder, so painted text with hundreds of patches can take minutes, and a
  per-filament `BatchBoolean` union is the planned improvement; the multi-instance limit now covers
  ORIENTATION as well as scale; and the rotation rule itself. §12's "Tests" line updated to the rev 2.14
  counts, with the rev 2.13 ones kept for comparison.

---

## Evidence

**RED (before the fix, tests built at the same commit with only `ColorSplitParams::max_state` declared).**

```
colorsplit: a tilted isotropic instance takes the world path and caps the WORLD top
  test_color_split.cpp(1651): FAILED: REQUIRE( space.world_path ) with expansion: false
  test cases: 1 | 1 failed ; assertions: 2 | 1 passed | 1 failed

colorsplit: a z-mirrored instance takes the world path
  test_color_split.cpp(1700): FAILED: REQUIRE( space.world_path ) with expansion: false
  test cases: 1 | 1 failed ; assertions: 2 | 1 passed | 1 failed

colorsplit: a paint state above the printer's filament count stays in the body colour
  test_color_split.cpp(1163): FAILED: REQUIRE( r.pieces.size() == 1 ) with expansion: 2 == 1
  test cases: 1 | 1 failed ; assertions: 5 | 4 passed | 1 failed
```

**Build.** `build_next_wt.bat` (ALL_BUILD, Release x64) — **exit 0**, zero `error C/LNK/MSB` lines; log
`scratchpad\all_build_fixwave28.log`. `ColorSplitJob.cpp` and `Plater.cpp` both recompiled;
`snapmaker-orca.exe`, `Snapmaker_Orca.dll` and all four test binaries relinked. Slot was checked free
(`Get-Process cl,link,MSBuild` empty) before each of the three builds; no source was edited after the final
ALL_BUILD.

**GREEN** (`build\tests\libslic3r\Release\libslic3r_tests.exe`, run after the final ALL_BUILD):

```
[colorsplit]         All tests passed (941 assertions in 59 test cases)
[colorsplit_spike]   All tests passed (24 assertions in 3 test cases)
[paintdepth]         All tests passed (1568 assertions in 94 test cases)
[chameleon]          All tests passed (605 assertions in 133 test cases)
```

Whole binary, for the §12 record: `test cases: 584 | 582 passed | 2 failed as expected` ·
`assertions: 52612 | 52610 passed | 2 failed as expected` (the two are the pre-existing expected failures
recorded at rev 2.13). The `[colorsplit]` end-to-end parity WARN is unchanged: worst diff 1.61063 of 4,
matching the case-B corner hold 1.61059.

## Concerns / follow-ups

1. **The up-axis is still the FIRST instance's.** `color_split_space` reads `object.instances.front()`
   only, so an object whose instances have *different* orientations (instance 0 upright, instance 1 laid
   on its side) gets instance 0's flat caps and crease cases for all of them. That is the same class of
   approximation §3.9 already documents for anisotropic scales, and §12's limit now names orientation
   explicitly — but it is a real limit, not a fixed bug. A per-instance split (or refusing mixed
   orientations) is the only exact answer and was out of scope here.
2. **The overflow warning is not translated.** It is built in `libslic3r`, like `too_small_warning`
   (`ColorSplitShell.cpp:32-45`), so it goes to the user in English while the GUI's own notifications use
   `_u8L`. Consistent with the rest of the library's warnings, but worth a pass if the feature is localised.
3. **`max_state` is read once, when the dialog is dismissed.** If the user removes a filament while the
   job runs, the split still uses the count it started with. The `finalize` re-checks cover paint and mesh
   changes, not printer changes; a part could therefore land with an extruder that has just become invalid,
   which slice time then clamps exactly as it does today.
4. **The GUI round is still unwalked** (§12 "Not yet measured"): the gizmo guard in `finalize`, the
   overflow warning's appearance in the plater notification, and the world path for a tilted instance have
   only library-level and compile-level coverage. The three behaviours are cheap to confirm by hand: rotate
   a painted part 90° about X and split; paint with a filament, delete it from the printer, split; open the
   MMU gizmo while a large split runs.
