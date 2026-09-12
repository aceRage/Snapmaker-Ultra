# Slice baking, phase 1: what was built

Implements phase 1 of `docs/superpowers/specs/2026-09-12-slice-bake-research.md` — the
"Exact layers" bake: turn a SLICED object's outer wall, as it will actually be extruded, into a
watertight mesh that can be re-sliced.

Branch `feat/slice-bake`, from `feat/ultra-preferences` (944457b8bb).

## The driving scenario

Slice a part at 0.3 mm **with** fuzzy skin, bake the sliced appearance to a mesh, then re-slice
that baked mesh at 0.12 mm **without** fuzzy skin. The coarse 0.3 mm fuzzy pattern is now carried
by the mesh itself, so it prints at 0.12 mm quality — the same texture, 2.5× finer layers, and no
wall jitter of the slicer's own.

This is what the bake is for, and it is the acceptance test (scenario (b) below).

## What was built

### 1. `libslic3r/SliceBake.{hpp,cpp}`

From a sliced `PrintObject`, per layer:

1. Walk `LayerRegion::perimeters` of **every** region of the layer (so painted / MMU objects,
   which are split into several `PrintRegion`s at slice time, contribute all of their outer wall).
2. Keep only the outer-wall roles: `erExternalPerimeter`, plus `erOverhangPerimeter` and this
   fork's `erOverSupportPerimeter` — the same physical loop split by role where it leaves the
   layer below. Dropping those two punches holes in exactly the overhanging bands.
3. Per **path**, not per loop, offset the stored centreline polyline outward by that path's own
   `width`/2 (via `ExtrusionPath::polygons_covered_by_width`, which is the same relationship the
   rest of the tree uses for "what does this extrusion cover"). Per-path matters because a loop
   crossing an overhang has paths of differing width, and one width would be wrong on whichever
   half did not supply it. A `ClipperSafetyOffset`-sized overlap is added so neighbouring paths of
   one loop union without leaving a zero-width crack at the joint.
4. Union the layer's footprints with `pftNonZero`, which **fills** each closed loop's interior.
   This is what makes the bake a solid rather than a hollow replica — and what makes top and
   bottom surfaces implicit: a filled top layer already is the top skin, so `erTopSolidInfill` /
   `erBottomSurface` never have to be read.
5. Loft the per-layer `ExPolygons` stack with the existing `Slic3r::slices_to_mesh`.

Exclusions are **by construction**, not by filtering:

| Excluded | Why it cannot appear |
|---|---|
| Supports, support interface | live in `SupportLayer`; the bake only walks `PrintObject::layers()` |
| Brim, skirt | separate `ExtrusionEntityCollection`s on `Print` / `PrintObject` |
| Wipe tower | a separate structure on `Print` |
| Infill, inner perimeters | never read — everything radially inward of the outer loop is filled anyway |

Options (`SliceBakeOptions`): layer subset as a half-open `[layer_begin, layer_end)` (default all),
`close_gaps_radius` in mm (default 0 = the morphological close is skipped entirely, so the layer
polygons come out of the union exactly as Clipper produced them), and `in_object_frame` (default
on), which applies `trafo_centered().inverse()` so the bake lands where the `ModelVolume`'s own
mesh sits and can replace the object in place. A negative-determinant instance transform has its
winding flipped back, so a mirrored object does not bake inside-out.

Determinism: the layer loop is serial and ordered, the per-layer union is Clipper's over a
fixed-order input, and nothing is hashed. Two bakes of one slice are byte-identical — asserted.

### 2. `SlicesToTriangleMesh`: the grid overload is now declared

`slices_to_mesh(slices, zmin, grid)` already existed as the implementation the two public
overloads sit on; it was simply not in the header. It is declared now (no behaviour change), so
the bake can hand over the **real** per-layer `print_z` values.

That matters in two cases the constant-height overload gets wrong: a variable or adaptive layer
height, and a layer subset that does not start at the bed. `grid[i]` is the top of slice i's band
and `zmin` is the bottom of slice 0's, so `grid[i] = layer->print_z`, `zmin = bottom_z` of the
first baked layer reproduces the printed stack exactly.

### 3. GUI

- **Menu**: object right-click → "Bake slice to mesh…", in `create_extra_object_menu` beside
  Repair/Remesh. Enabled only when exactly one object is selected, the background slicing process
  is idle, that object's `PrintObject` has `posPerimeters` done, and `slice_bake_available()` finds
  an outer-wall loop. `posPerimeters` rather than a full G-code export, because perimeters are
  precisely what the bake reads.
- **Dialog** (`SliceBakeDialog`, built the way `RemeshDialog` is — it collects options and nothing
  else): **Result** = Replace object / Add as new object / Export STL…, a **Close small gaps**
  checkbox with a radius, and the **size line**, which states the layer count and an estimated
  triangle count **before** the run. That line is the reason the action has a dialog at all: a
  lofted 100 mm part at 0.2 mm is 500 stair-stepped bands.
- **Job** (`Jobs/SliceBakeJob`), through the existing Jobs system like `FillBedJob` /
  `ColorSplitJob`: progress via `ctl.update_status`, cancel via `ctl.was_canceled()` threaded into
  the library's progress callback (which raises `SliceBakeCancelled`). Everything that touches the
  `Model` happens in `finalize()`, on the main thread.
- **Replace** is one `Plater::TakeSnapshot`, so one Undo puts the original back. It calls
  `clear_before_change_mesh()` first — the bake's vertex indices bear no relation to the source's,
  so painted supports, seams, fuzzy skin and MMU colour are dropped (and the existing
  "custom supports removed" notification fires). The object keeps its position (the bake is
  returned in the object's frame and the volume's transform is reset to identity) and its print
  settings (the `ModelObject` and its config are untouched). Modifiers, negative volumes and
  support blockers are left alone; only model **parts** are replaced, and only by one volume,
  since the bake is a single solid covering all of them.
- After replacing, `changed_mesh()` schedules the background process and the plate's
  `update_slice_result_valid_state(false)` makes the stale slice visible in the UI, so re-slicing
  happens against the new mesh.
- The STL path asks for the filename on the main thread, before the job starts.

## Building here — the silent exit-255 trap

**Never run the MSVC link in the foreground of a Bash/PowerShell tool call.** Linking
`fff_print_tests.exe` or `libslic3r_gui` takes several minutes and overruns the tool's own
timeout. When that fires it kills the process tree, and `cmake --build` reports it as **exit 255
with no `error LNK`, no `Build FAILED`, and no executable** — which reads exactly like a link
failure in your own code and is not one. A 32 MB `/v:diagnostic` MSBuild log of one such run
contained zero compiler or linker diagnostics; it simply truncated mid-compile.

Launch detached instead, and poll for a sentinel:

```powershell
# run_tests.bat (CRLF): vcvars64, then PATH, then
#   cmake --build C:/Dev/wt_bake/build --config Release --target fff_print_tests -- /nodeReuse:false /m:1
#   echo BUILD_EXIT=%ERRORLEVEL%
Start-Process -FilePath "$S\run_tests.bat" -RedirectStandardOutput "$S\build.log" `
              -RedirectStandardError "$S\build.err" -WindowStyle Hidden -PassThru
```

then `until grep -q "BUILD_EXIT=" build.log; do sleep 30; done`. The log stops growing well
before the process exits, so "no new output" is **not** a finished signal — only the sentinel is.

Three further traps hit on this branch:

- **Concurrent MSBuilds corrupt the PDBs.** Three runs (`/m:4`, `/m:2`, `/m:1`) were alive against
  one build tree because the wrapper commands returned while MSBuild kept running. The result was
  `error C1090: PDB API call failed, error code '23'` scattered across unrelated files. Kill every
  stray `cl`/`MSBuild`/`link` before starting a build, and confirm the count is zero.
- **`/m:1` does not serialize compilation.** The generated command line carries `/MP`, so MSVC
  still forks parallel compilers inside the single node.
- Running the test exe needs its own directory on `PATH` (31 DLLs sit beside it); invoke it by
  full path with `cd /d` to that directory, or it exits 9009.

## Tests — `tests/fff_print/test_slice_bake.cpp`, tag `[slice_bake]`

Measured 2026-09-12 on this PC (Release, `fff_print_tests.exe "[slice_bake]"`):
**7 cases, 4 passed, 3 failed; 7708 assertions, 7702 passed, 6 failed.**

### Passing, and what they establish

| Case | Result |
|---|---|
| (a) 20 mm cube @ 0.2 mm | **watertight (0 open edges)**, 101 distinct Z levels / 100 layers, volume **8000.02 mm³ vs 8000 nominal (+0.0002%)**, bbox 20×20×20. 0.300 s |
| (d) supports/brim/skirt excluded | source had **49 support layers** plus a 3 mm brim and a skirt; bake volume **960.003 mm³ vs the T's exact 960**, bbox 20×20×12. 0.160 s |
| layer subset | `[10,30)` → 20 layers, bbox z 2.0 .. 6.0 exactly. 0.090 s |
| determinism | two bakes of one fuzzy slice byte-identical (vertices and indices) |

(c) cylinder is *geometrically* right — 60 layers, 61 Z levels, **radius fitted 7.998 mm vs source
8 mm** (min 7.975, max 8.016) — but fails its watertightness assertion, see below.

### The owner's scenario (b): the texture transfer works, the test's band check does not

The transfer itself is demonstrated. Source at 0.3 mm with fuzzy skin: wall deviation mean 0.323,
**max 0.587 mm**. Bake: 67 layers, 25,584 triangles. Re-sliced at **0.12 mm with fuzzy skin OFF**
(168 layers): the re-sliced outer walls still deviate by **max 0.630 mm, mean per-layer max
0.579 mm**. The coarse fuzzy texture is carried by the mesh and reproduced by the fine layers,
which is what the feature is for.

Two assertions in that case are nevertheless wrong and fail:

- `max_dev < 0.45` — measured **0.630**. The 0.15–0.45 window was written assuming the deviation
  would be bounded by `fuzzy_skin_thickness`. It is not: the fuzz displaces a wall centreline that
  the bake then offsets outward by width/2, and the measurement is against the wall's own bounding
  box, so the bound needs re-deriving from the geometry rather than widening to fit the number.
- `in_mean < 0.05` and `cross_mean > 3·in_mean` — measured in-band **0.546** vs cross-band
  **0.554**: indistinguishable, so the test cannot see the 0.3 mm banding at all. The fault is in
  `radial_profile`: on a *square* cross-section the centroid-to-wall radius swings by ~±2 mm
  between edge and corner, which swamps the ~0.3 mm signal. A band check has to measure deviation
  from the local wall line (as `wall_deviation` does), not from a centroid.

### Open defect: the loft leaves cracks on non-prismatic geometry

Open edges: **0** on the plain cube, **20,264** on the fuzzy cube, **15,245** on the cylinder.

The cause is in `slices_to_mesh` itself, and its own comment says so —
*"FIXME: these repairs do not fix the mesh entirely. There will be cracks in the output."*
`triangulate_expolygons_3d` tesselates each layer's free-top/overhang delta independently of the
`wall_strip` vertices, so T-junctions appear wherever consecutive layers differ. A prismatic cube
never differs layer to layer, which is exactly why it alone comes out watertight, and why the
defect is invisible in the SLA path this loft was written for.

Phase 1 is not complete until the bake closes these, since the spec requires a watertight result.
The fix belongs in `SliceBake` (welding the cap and wall vertex sets, or a tolerance-based stitch
after the loft), not in a blanket repair pass.

### Timings

Not yet measured for a 100 mm benchy-class part at 0.2 mm — that benchmark waits on the
watertightness fix, since a cracked mesh's triangle count is not the count the finished bake will
produce. The figures above are the 20 mm cases: 0.30 s for the 100-layer cube bake, 0.58 s for the
full scenario (b) round trip including two slices.

## Click-tests

Not yet performed — the library defect above should be fixed first, so the GUI is exercised
against a bake that is actually watertight. `libslic3r_gui` **does build clean** with the dialog,
job, menu item and `ObjectList` changes in it (verified 2026-09-12), so the remaining work is
behavioural: the scenario end to end, Replace vs Add vs Export, and one-step Undo of a replace.

## Not in phase 1

Per the spec: the G-code route (Z contouring, seams), Smooth mode (OpenVDB level set), and
colour/paint carry-over. Also deferred: the layer-range picker in the dialog (the library takes
the subset; the dialog always passes "all").
