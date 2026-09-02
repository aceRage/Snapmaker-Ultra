# N1/N2/N3 fix-wave report

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`. Base HEAD `530e2f52d2`
("fix(paint-depth): correct vertical paint-depth fix-wave findings C1/I1/I2/I3"). Fix commit
`3448111acd`. Against: `vertical-depth-fixwave-rereview.md`.

## Status: all three findings closed, all gates green.

---

## N1 (Critical) — I2's guard also zeroed every painted colour's extrusion stats

**Root cause.** `layer_color_stat` (`MultiMaterialSegmentation.cpp`) looped over
`layer.regions()` with a single `if (region->slices.empty()) continue;` guard placed above
*both* the shell-depth max (`out.top_shell_layers` / `out.bottom_shell_layers`) *and* the
per-colour extrusion-stat block (`out.num_regions`, `out.extrusion_width`,
`out.extrusion_spacing`, `out.small_region_threshold`). For a painted colour `c`, the only
region with `wall_filament == c` is the auto-created painted region
(`PrintApply.cpp:1088-1090`), and that region's `slices` are empty at `layer_color_stat` time
on every layer — `apply_mm_segmentation` (`PrintObjectSlice.cpp:5275`), which actually
populates them, runs *after* segmentation (`:5267`). So the per-colour block was skipped
unconditionally for every painted colour on every layer: `num_regions` stayed 0,
`extrusion_width`/`extrusion_spacing`/`small_region_threshold` stayed `0.f`.

Consequences, both invisible to the pre-existing suite because every prior probe was
dead-centre on a uniform-cross-section slab:
- The lateral inward taper (`offset -= stat.extrusion_spacing + stat.extrusion_width` in both
  descent loops) went to zero, so a painted shell claim stopped narrowing with depth and
  stayed the full surface-layer silhouette all the way down — a full-width prism instead of a
  truncated pyramid.
- `small_region_threshold == 0` also disabled the `opening_ex()` thin-projection filter (the
  #7104 fix).
- `assert(out.num_regions > 0)` was re-armed for every painted colour (masked in Release by
  `/DNDEBUG`).

**Fix.** Moved `const PrintRegionConfig &config = region->region().config();` above the guard
(now needed by both branches) and narrowed `if (! region->slices.empty())` to wrap only the
two shell-depth-max assignments. The per-colour block is unconditional again, exactly matching
its pre-I2 (and structurally pre-fix-wave) behavior. The shell-depth max loses nothing: a
painted region's config always mirrors its parent volume region's
`top_shell_layers`/`thickness`/bottom counterparts (only the filament ids differ,
`PrintApply.cpp:1088-1090`), and the parent *does* have non-empty slices on every layer it
occupies, so the max already sees that depth through the parent.

**Self-review hand-walk.** Painted colour `c` whose only `wall_filament == c` region has empty
slices this layer: the per-colour block now runs unconditionally for that region (config reads
are geometry-independent — `outer_wall_line_width`, `gap_infill_speed`, `layer.height` — so
valid regardless of `slices` state), so `num_regions`, `extrusion_width`, `extrusion_spacing`,
`small_region_threshold` are populated correctly → taper alive, #7104 filter alive, assert
satisfied. The shell-count max is computed from a *different* region (the parent, or any other
region on the layer with real geometry) inside the now-narrower `if`, so a geometry-less
region (painted-but-unsegmented, or a modifier/Z-stacked volume elsewhere per the existing I2
test) still cannot inflate it — I2's fix is fully preserved.

**RED test** (`test_paint_depth_clamp.cpp`, "the painted top claim's lateral inward taper
survives at the deepest claimed layer (not a full-width prism)"): single-region 40×40×4mm
capped slab, `layer_height=0.1`, `top_shell_layers=4`, `top_shell_thickness=0.6` → effective
6-layer shell. Probes 2mm inside the silhouette edge (18mm from a 20mm half-width centre) at
the deepest claimed layer (depth 5). Hand-derived taper: `extrusion_spacing + extrusion_width
= (0.45 - 0.1·(1-π/4)) + 0.45 ≈ 0.8785mm/layer`; at depth 5 that's `≈4.39mm` of accumulated
inward erosion, comfortably past the 2mm-from-edge probe (tapered ⇒ unclaimed) but the
untapered pre-fix claim still reaches the full silhouette (⇒ wrongly claimed).

Captured RED (pre-fix, HEAD `530e2f52d2` + new tests only, no production change):
```
multi_material_segmentation_by_painting: the painted top claim's lateral inward
taper survives at the deepest claimed layer (not a full-width prism)
FAILED: CHECK_FALSE( any_contains(extruder2_claim_for_layer(*object, deepest_claimed_layer), near_edge_probe) )
with expansion: !true
```
Post-fix: passes. Full run at that stage: 25 cases, 23 passed / 2 failed (this test + N2's),
173 assertions, 171 passed / 2 failed — i.e. exactly the two new bugs failed, nothing else
perturbed.

**Assert-check feasibility — done, not just claimed feasible.** The project already has a
native, cheap pattern for this: several existing files (`PrintObject.cpp:64-70`,
`GCode.cpp:82`, `EdgeGrid.cpp:21`, `Slicing.cpp:13`, others) use
`#undef NDEBUG` / `#undef assert` / `#include <cassert>` to force `assert()` active in one
Release-config translation unit, with no dependency rebuild (confirmed the shared deps prefix
at `C:/Dev/SnapmakerOrca/deps/build/OrcaSlicer_dep/usr/local` is Release-only — e.g.
`TBBTargets-release.cmake` with no debug counterpart — so a real Debug config here would mean
rebuilding the entire dependency chain, not cheap). I applied that exact pattern temporarily to
`MultiMaterialSegmentation.cpp`, rebuilt (single-file recompile + relink, no deps touched), and
ran the N1 test case alone against the pre-fix code:
```
Assertion failed: out.num_regions > 0, file C:\Dev\SnapmakerOrcaNext\src\libslic3r\MultiMaterialSegmentation.cpp, line 1525
```
(process aborted, non-zero exit). This is direct runtime confirmation of the exact invariant
violation the rereview predicted, not an inference. The shim was reverted (`git diff` on the
committed file shows no trace of it) before the real fix and final GREEN build — it was never
part of the shipped change.

---

## N2 (Minor) — C1 residual: zero-shell region's surface layer still claimed

**Root cause.** The surface-layer `append(triangles_by_color_top[color_idx][...], top_ex)` was
gated only on `top_ex` being non-empty, never on `stat.top_shell_layers > 0`. `top_raw` being
populated at all is decided by the *object-wide* `max_top_layers` gate (max over every
`PrintRegion` on the object, regardless of per-layer presence) — so in a mixed-region object, a
layer whose only present (non-empty-sliced) region has `top_shell_layers == 0` can still have
`top_raw` geometry (from some *other* region's nonzero shell elsewhere), and the surface claim
fired anyway. That violates C1's contract: a zero shell count claims nothing at all, not even
the immediately-painted surface facet (`LayerRegion.cpp:1025-1036` demotes it away from
`stTop`).

**Fix.** Added `&& stat.top_shell_layers > 0` to the `if (! top_ex.empty())` guard wrapping
both the surface append and the descent loop. No behavior change for any nonzero-shell case
(confirmed: all 22 pre-existing `[paintdepth]` cases and all 133 `[chameleon]` cases pass
byte-for-byte unchanged). The descent loop was already a no-op in the zero case (its own bound
`last_idx > max(layer_idx - 0, 0) = layer_idx` is never satisfied by a loop starting at
`layer_idx - 1`), so this is a pure surface-claim fix.

**Scope decision — bottom counterpart (`:1556`, now `:1587`) deliberately not touched.** The
identical residual gap exists on the bottom side, but the fix-wave's cited anchor is `:1536`
(top) only, and the rereview's own N2 section discusses top exclusively. Rather than fold in an
unrequested, unreviewed change, I left it as a noted asymmetry (comment added at the top site;
also called out in the new N2 test's comment) — the same "record, defer, don't silently fix"
pattern this file already uses for I1's provably-unobservable top analogue and C1's top-only
pinning. Hand-walked (not automated-tested): the bottom fix would be the mechanically identical
one-line change at the bottom claim block; I'm not spawning a follow-up task for it since it's
a trivial fix, cited nowhere in my instructions, and flagging it here is sufficient for
whoever scopes the next pass.

**RED test** (`test_paint_depth_clamp.cpp`, "a zero-shell region's painted top is not claimed
even when another region makes the object-wide gate nonzero"): two Z-stacked model-part
volumes — "lower" (z 0-4mm, stock `top_shell_layers=4` default, purely to keep the object-wide
gate open) and "upper" (z 4-20mm, explicit `top_shell_layers=0`, `top_shell_thickness=0.6`,
top cap painted). At the object's top layer, "lower" has no geometry (empty slices), so the
per-layer `stat.top_shell_layers` comes back 0 from "upper" alone — this also incidentally
exercises Site A's (helper's) zero-count early return, since a nonzero `top_shell_thickness`
would otherwise resurrect a claim if that early return were ever reverted.

Captured RED (pre-fix):
```
multi_material_segmentation_by_painting: a zero-shell region's painted top is
not claimed even when another region makes the object-wide gate nonzero
FAILED: CHECK_FALSE( any_contains(extruder2_claim_for_layer(*out_object, top_index), probe) )
with expansion: !true
```
Post-fix: passes.

---

## N3 (Minor) — added coverage, bottom-direction zero-shell case

The existing C1 inverted test only exercises `top_layers_eff` (`:1319-1322`); there was no
`bottom_shell_layers = 0` counterpart even though the bottom gate (`:1321-1322`) is already
symmetric and correct. Added the exact bottom mirror of that test (bottom cap painted,
`bottom_shell_layers=0`, `bottom_shell_thickness=0.6`) to `test_paint_depth_clamp.cpp`. This is
coverage only — it passed on first build with no production change, confirming Site B's bottom
half was never actually broken, only untested.

---

## Verification

All commands run from `C:\Dev\SnapmakerOrcaNext` against fix commit `3448111acd` (post N1+N2
fix, post assert-shim revert):

- `libslic3r_tests.exe "[paintdepth]"` → **All tests passed (173 assertions in 25 test cases)**
  (baseline was 22 cases / 156 assertions; +3 cases / +17 assertions from N1/N2/N3, all green).
- `libslic3r_tests.exe "[chameleon]"` → **All tests passed (605 assertions in 133 test cases)**
  — unchanged from baseline, confirming zero regression outside the touched function.
- ALL_BUILD via the scratchpad `build_next_wt.bat` wrapper → **exit code 0**, zero `error`
  occurrences in the build log (GUI app, all four test binaries, all deps-linked targets built
  clean).
- `spike/verify_paintdepth.sh` × 2 → **17/17 checks passed** both runs, `RESULT: ALL PASS`
  both times, including the unpainted-determinism/byte-identical-parity checks (this fix is
  entirely inside the MM-painted code path, so unpainted objects are provably unaffected).

Build-slot discipline: checked `Get-Process cl,link,MSBuild` before every build; the first
attempt found ~20 active `cl.exe` processes with substantial accumulated CPU (real compile
work elsewhere) and the turn was stopped with "WAITING FOR BUILD SLOT" until the coordinator
confirmed the slot was free. All five subsequent builds (RED, assert-shim add, GREEN,
assert-shim-revert-is-part-of-GREEN, ALL_BUILD) were preceded by a clean `Get-Process` check.

## Files changed (commit `3448111acd`)

- `src/libslic3r/MultiMaterialSegmentation.cpp` — N1 (guard rescope) + N2 (surface-claim gate).
- `tests/libslic3r/test_paint_depth_clamp.cpp` — 3 new `[paintdepth]` test cases (N1, N2, N3).

Not committed (untouched, pre-existing or out of scope): `.superpowers/sdd/2026-08-31-paint-depth/progress.md`
(modified by something other than this session — not authored by me, left alone), the other
untracked review/report docs in the same directory, and `spike/out/*.gcode` fixture outputs.
