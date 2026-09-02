# Vertical paint-depth fix wave — scoped re-review

Reviewed: `530e2f52d2` ("fix(paint-depth): correct vertical paint-depth fix-wave findings
C1/I1/I2/I3") on `feat/paint-depth`, worktree `C:\Dev\SnapmakerOrcaNext`.
Against: `vertical-depth-fix-review.md` (the findings) and `vertical-depth-fixwave-report.md`
(the implementer's account). Read-only. Every claim below was hand-executed against the
current source, not taken from either document.

**Verdict: ISSUES** — the four findings themselves are fixed, but the I2 predicate has an
unintended second effect that is a new Critical defect.

| Finding | Verdict |
|---|---|
| C1 — `*_shell_layers == 0` claims depth into a nonexistent shell | **RESOLVED** (see N2 for a narrow residual) |
| I1 — `++m` one short on loop exhaustion | **RESOLVED** |
| I2 — max-over-ALL-regions is object-wide | **PARTIAL** — the depth inflation is fixed and pinned, but the chosen predicate introduces **N1** |
| I3 — no non-uniform-layer-height coverage | **RESOLVED** |
| Regression sweep / erosion taper | Taper is **REAL product behavior**, not a fixture artifact — but N1 would silently disable it |

---

## C1 — RESOLVED

Three sites traced, all correct.

**Site A — helper early return** (`MultiMaterialSegmentation.cpp:1230-1231`).
`if (n_layers <= 0) return n_layers;` sits before the `thickness > 0.` block, so the walk can
no longer raise `effective` above 0. Mirrors the generators exactly:
`PrintObject.cpp:1965` (`if (int n_top_layers = region_config.top_shell_layers.value;
n_top_layers > 0)`) and `:1994` (bottom counterpart) — both re-read this session, both wrap the
*entire* gather including the `< top_shell_thickness - EPSILON` term.
Nit only: it returns `n_layers`, not `0`. For a corrupt negative count that yields a negative,
but `std::max(out.top_shell_layers /*0*/, ...)` at `:1495` and the ternary at `:1319` both clamp
it, and `def->min = 0` makes it unreachable from the GUI. Harmless.

**Site B — sizing/gate bound** (`:1319-1325`). `top_layers_eff` / `bottom_layers_eff` gate on
`config.*_shell_layers.value > 0` before consulting `layers_for_thickness()`. Correct.

**Site C — per-layer stat** (`:1495-1498`). Unchanged, but now inherits Site A's early return,
so a zero-count region contributes 0 to the max. Correct.

**Zero claim, not even one surface layer — traced end to end.** With every region at
`top_shell_layers = 0`: `max_top_layers == 0` → `:1355`/`:1366` pass `nullptr` for `top` to
`slice_mesh_slabs` → `top` stays empty → `merge()` at `:1367-1385` does `find_if` over an empty
`src`, hits `end()`, leaves `top_raw[extruder_idx]` empty → `:1531`'s `! top.empty()` is false →
neither the surface-layer `append` at `:1536` nor the descent at `:1539` runs. **Zero claim,
surface layer included.** ✔

**Ordinary `layers > 0` path untouched.** Hand-walked the first vertical test's config
(0.1 mm layers, `N = 4`, `T = 0.6`): `n_layers = 4 > 0` so no early return; walk breaks at
`m = 6`; `effective = max(4, 6) = 6`; bound `last_idx > max(int(S) - 6, 0)` claims `S..S-5`, not
`S-6`. Matches the test's assertions exactly, and matches the pre-fix-wave behavior of
`41394ce2b4`. Site B's ternary is a pure no-op whenever the count is nonzero. ✔

**Is the inverted test asserting the right thing, or just the negation of a stale assertion?**
It asserts the right thing, and it discriminates — but it pins only one of the three sites.
- Config is `top_shell_layers=0, top_shell_thickness=0.6, layer_height=0.2, bottom_shell_layers=3,
  bottom_shell_thickness=0.0`, top cap painted, single `CHECK_FALSE` at the surface layer.
- The surface layer is the correct sentinel: `:1536` appends it *unconditionally* whenever
  `top_raw` is non-empty, so "surface layer unclaimed" ⟺ "no top claim at all". A single
  assertion is genuinely sufficient here, not laziness.
- Revert Site B alone and `max_top_layers = layers_for_thickness(0.6) = int(0.6/0.2)+1 = 3` → the
  gate at `:1338` opens, `top_raw` is populated, the surface layer is appended → test fails.
  So it pins Site B specifically. The report's captured RED (`!true`) is consistent.
- It does **not** pin Site A or Site C: with Site B in place, `max_top_layers == 0` short-circuits
  everything upstream regardless of what the helper returns. Reverting only the helper's early
  return would keep this test green. See **N3**.
- No bottom-direction counterpart (`bottom_shell_layers=0` + `bottom_shell_thickness>0`) exists.
  Also N3.

---

## I1 — RESOLVED

**The review's live counterexample, hand-executed against the current code.** 0.5 mm plate,
`layer_height = initial_layer_print_height = 0.1` → 5 layers, `bottom_z = 0, .1, .2, .3, .4`;
`bottom_shell_layers = 3`, `bottom_shell_thickness = 0.6`; bottom cap painted, `S = 0`.

- Helper (`:1254-1262`): `idx = 1..4`, gaps `.1/.2/.3/.4`, never `>= 0.5999`; loop exits with
  `idx == 5`; `idx >= num_layers` → `++m` → **`m = 5`**; `effective = max(3, 5) = 5`.
- Bound `:1559`: `last_idx < min(0 + 5, 5) = 5` → `last_idx = 1..4`, plus the surface layer 0
  appended at `:1556` → **layers 0–4, the whole object.**
- Generator `PrintObject.cpp:2001`, re-read this session
  (`bottom_z - m_layers[i]->bottom_z() < region_config.bottom_shell_thickness - EPSILON`): for
  `i = 4`, `0.4 < 0.5999` → true → the bottom shell reaches layer 4.

Claim now covers every layer the generator reaches. ✔

**No over-count in the break path** — verified structurally, not by example. The `++m` fires
*before* the break test, so the layer that triggers the break is counted; that increment is
exactly the stand-in for the surface layer. For a bottom surface at `S` whose deepest
thickness-satisfying layer is `S+k`: the loop visits `S+1..S+k` without breaking (`m = k`), then
visits `S+k+1` and breaks (`m = k+1`) = the correct total depth `k+1`. `idx = S+k+1 < num_layers`,
so the new `if (idx >= num_layers) ++m` does **not** fire. Symmetric for top. Spot-checked with
`T = 0.25`, `h = 0.1`, `S = 0`, 10 layers: `m = 3`, claim = layers 0,1,2; generator solidifies
0,1,2. Exact. ✔ No off-by-one in the other direction.

**Degenerate edges.** Bottom with `S = num_layers-1`: loop body never runs, `m = 0`, `++m` → 1
(depth 1 = the surface layer alone). Top with `S = 0`: same, `m = 1`. Both correct, and both
were already `>= 1` pre-fix via `max(n_layers, ...)` for any nonzero count.

**Sizing bound still holds** (this is the part the report does not check, and it matters — it
feeds the TBB double-buffer parity trick). Exhaustion implies every gap is `< T - EPSILON`, so
`(num_layers-1-S)·mlh < T`, hence `m = num_layers - S <= int(T/mlh) + 1`, and `m <= num_layers`;
therefore `m <= layers_for_thickness(T) <= bottom_layers_eff` whenever the count is nonzero (and
when it is zero the helper early-returns 0, matching `bottom_layers_eff = 0`). Same argument for
top with `S·mlh < T`. So `effective - 1 <= granularity` still holds after the `++m`. ✔

**Top analogue.** Confirmed masked, and confirmed unchanged. With `effective = S+1`, the bound at
`:1539` is `std::max(int(layer_idx - stat.top_shell_layers), int(0))`; `layer_idx` is `size_t` so
`S - (S+1)` wraps to `SIZE_MAX`, `int(SIZE_MAX) = -1`, `max(-1, 0) = 0` → `last_idx > 0` → layer 0
excluded. Pre-fix (`effective = S`) gives `max(0, 0) = 0` → the identical bound. So the top `++m`
is provably unobservable, the report's decision to skip a non-discriminating top test is right,
and the residual "top claim can never reach layer 0" is pre-existing, unchanged, and recorded.
(It is still real: a painted top whose thickness spans the object leaves layer 0 base-coloured.
Out of scope, but it is now recorded in two places, which was the review's ask.)

**Test.** 40×40×4 mm slab, `layer_height = 0.5` → 8 layers, `bottom_z = 0, .5, …, 3.5`;
`bottom_shell_thickness = 4.0` > max gap 3.5, so the walk is guaranteed to exhaust. Post-fix
`m = 8`, bound `min(0+8, 8) = 8` → layers 0–7. Pre-fix `m = 7` → layers 0–6, failing at exactly
`idx = 7` — one assertion, matching the report's RED. Discriminates. ✔

---

## I2 — PARTIAL (fix is right in intent; predicate has a second, unintended effect → N1)

**The depth-inflation defect is fixed.** `if (region->slices.empty()) continue;` at `:1479-1480`.
`slices` is populated at `PrintObjectSlice.cpp:5230-5234` and segmentation runs at `:5267`, so the
data is there — confirmed by re-reading both sites.

**Is `slices` the right member?** Among the candidates, yes:
- `slices.empty()` is literally `surfaces.empty()` (`SurfaceCollection.hpp:50`), so
  `slices` and `slices.surfaces` are the same test.
- `fill_surfaces` would be **wrong**: it is filled at `posPrepareInfill`, long after `posSlice`, so
  it is empty for *every* region here — the guard would skip everything, zeroing both the shell
  max and the extrusion stats.

**Could a legitimately-contributing region be skipped (empty slices but real extrusions)? YES —
and this is the problem.** Auto-created MM *painted* regions are exactly that case:
- They are created up front in `generate_print_object_regions`
  (`PrintApply.cpp:1088-1090` sets `cfg.wall_filament = painted_extruder_id`, `:1102`
  `get_create_region`, `:1116` `layer_range.painted_regions.push_back`) and therefore live in
  `all_regions`, so `PrintObjectSlice.cpp:5206-5208` gives them a `LayerRegion` on every layer.
- `slices_to_regions` (`PrintObjectSlice.cpp:262-327`) only ever writes slots reached through
  `layer_range.volume_regions` (`:290`, `:321`, and the complex path's `:335-336`). **Painted
  regions are never in `volume_regions`, so they receive no geometry there.**
- Their slices are first written by `apply_mm_segmentation` (`PrintObjectSlice.cpp:4653`), called
  at `:5275` — *after* segmentation at `:5267`.

So at `layer_color_stat` time every painted region has empty `slices` on **every** layer, and the
new guard skips it everywhere.

For the *shell-depth max* this is harmless: a painted region's config is its parent's with only
`wall_filament`/`solid_infill_filament`(/`sparse_infill_filament`) changed
(`PrintApply.cpp:1088-1090`, `:836-841`), so its `top_shell_layers`/`top_shell_thickness`/bottom
counterparts are identical to the parent volume region's — and the parent *does* have slices on
those layers. No depth is lost. ✔

For the *extrusion stats* it is **not** harmless — see N1.

**Does the new two-volume test discriminate? Yes.** Hand-executed: 20 layers at 1.0 mm,
`top_index = 19`, upper's top cap painted. Post-fix `layer_color_stat(19, 2)` skips "lower"
(z 0–4, empty slices at layer 19) → `stat.top_shell_layers = max(4, walk)`; the top walk at
`S = 19` breaks immediately (`print_z[19] - print_z[18] = 1.0 >= 0.6 - EPSILON`, `m = 1`) →
`effective = 4` → bound `max(19-4, 0) = 15` → layers 16–19 claimed, layer 9 not. Pre-fix "lower"'s
`top_shell_layers = 30` entered the max → bound `max(int(19-30), 0) = 0` → layers 1–19, and the
lateral taper over 10 steps (~0.685 mm/layer at 1.0 mm layers, 0.45 mm outer wall) is only
~6.85 mm on a 20 mm half-width, so the dead-centre probe is still inside → `CHECK_FALSE` fails.
Exactly one assertion, as the report's RED shows. ✔

---

## I3 — RESOLVED

**Fixture arithmetic, done independently.** `layer_height = 0.1`,
`initial_layer_print_height = 0.2`, 4 mm slab → 39 layers; real
`bottom_z = 0, 0.2, 0.3, 0.4, 0.5, 0.6, …`. Bottom cap painted, `S = 0`,
`bottom_shell_thickness = 0.6`, `bottom_shell_layers = 3`:
gaps `0.2 / 0.3 / 0.4 / 0.5 / 0.6`; the break test `>= 0.6 - 1e-4 = 0.5999` first fires at
`idx = 5` → `m = 5` (no exhaust increment, `5 < 39`) → `effective = max(3, 5) = 5` → bound
`min(0+5, 39) = 5` → **layers 0–4 claimed, layer 5 not.**

**It genuinely distinguishes the real walk from `ceil(T/h)`.** A uniform-0.1 assumption gives
`ceil(0.6/0.1) = 6` → layers 0–5 → the test's `CHECK_FALSE` at layer 5 fails. The report's actual
swap used `layers_for_thickness`'s own formula with `layers[1]->height = 0.1`:
`int(0.6/0.1) + 1`; in IEEE-754 `0.6/0.1 = 5.999999999999999…` so `int(...) = 5`, `+1 = 6` — same
wrong answer, same single failing assertion at line 826. Consistent with the captured RED. ✔
The break is also FP-robust: `bottom_z[5]` is ~0.6 (accumulated), and `0.6 >= 0.5999` holds with
huge margin either side of rounding.

**Cross-check against the generator**: `PrintObject.cpp:2001` for `i = 5` gives
`0.6 - 0 < 0.5999` → false, and the count half `5 - 0 < 3` → false. Layer 5 is genuinely outside
the bottom shell. The test asserts the truth, not just a discriminating number. ✔

**No perturbation of the other fixtures.** The new parameter is trailing with default `0.`, and
the body is `initial_layer_print_height > 0. ? initial_layer_print_height : layer_height` — for
every pre-existing call site (all of which omit it) that is byte-identical to the previous
unconditional `= layer_height`. All four pre-existing vertical cases keep their original expected
depths and still pass. The parameter sits after `Print &print`, so no call site's positional
arguments shift. ✔

---

## Check 5 — the erosion taper: REAL product behavior, not a fixture artifact

The taper is `offset -= (stat.extrusion_spacing + stat.extrusion_width)` in both descent loops
(`:1542`, `:1562`), pre-existing upstream BBS code, untouched by this commit and by `41394ce2b4`.
It is intentional: it keeps the painted claim retreating inside the walls as it descends so the
colour does not surface through the perimeter, and `if (last.empty()) break;` (`:1545`/`:1565`)
terminates the descent when the claim erodes away.

**It is real, and it bounds the benefit of this whole fix wave.** At a 0.45 mm outer wall the
claim narrows ~0.79 mm/layer at 0.5 mm layers and ~0.87 mm/layer at 0.1 mm layers. User-visible
effect: a painted top face is coloured to its full extent only at the surface layer; below that
the colour is a truncated pyramid retreating from the silhouette. The deeper claim this fix wave
buys (e.g. 6 layers instead of 4 at 0.1 mm / `T = 0.6`) is therefore realised only in the interior
of a *wide* painted face — roughly, a face narrower than `2 × 0.87 × depth` (≈10 mm at 6 layers)
never gets the deep claim at all, and the extra solid layers under its painted skin stay
base-coloured near the edges. That is the same "safe direction" over/under-coverage the review's
M5 describes on the generator side; it is **not a defect**, but the fix-wave report treats it
purely as a test-fixture nuisance ("a confound independent of the I1 bug"), which undersells it.

The implementer's numbers check out and are not an artifact: 40×40 slab, 20 mm half-width,
0.87 mm/layer → the dead-centre probe is lost at ~layer 23 of a 40-layer exhausted claim →
40−23+1 = 18 failures, exactly the "18 spurious failures" reported. The redesign to 0.5 mm/8
layers (~5.5 mm cumulative erosion vs. 20 mm margin) is the right fix for the test.

**But see N1: post-fix, this taper is very likely dead for painted colours.**

---

## NEW ISSUES

### N1 (Critical) — the I2 guard also zeroes every painted colour's extrusion stats, disabling the lateral taper

`src/libslic3r/MultiMaterialSegmentation.cpp:1479-1480` vs. `:1499-1514`.

The guard is placed at the *top* of the region loop, so it suppresses the whole body — not just
the two shell-depth `max`es it was meant to bound, but also the per-colour block:

```cpp
if (color_idx == 0 || config.wall_filament == int(color_idx)) {
    out.extrusion_width        = std::max<float>(out.extrusion_width, outer_wall_line_width);
    out.small_region_threshold = ...;
    out.extrusion_spacing      = Flow::rounded_rectangle_extrusion_spacing(...);
    ++ out.num_regions;
}
```

For a painted colour `c`, the **only** region with `wall_filament == c` is the auto-created
painted region (`PrintApply.cpp:1088-1090`) — and, per the I2 section above, that region has empty
`slices` on every layer at this point in the pipeline. It is now skipped everywhere, so for every
painted colour, on every layer:

- `num_regions == 0` → `assert(out.num_regions > 0)` at `:1516` now fires in any assert-enabled
  build. (The shipped test binary is Release, `/DNDEBUG` per `build/CMakeCache.txt`, so the green
  suite proves nothing here.)
- `extrusion_width == extrusion_spacing == small_region_threshold == 0` →
  `offset -= (0 + 0)` at `:1542`/`:1562`: **the lateral inward taper of check 5 is gone**, and
  `if (last.empty()) break;` can no longer fire, so the descent always runs to its full depth at
  the full silhouette; and `opening_ex(top_ex, 0)` at `:1534`/`:1554` no longer prunes thin
  projections (the `#7104` dimple filter).

Net user-visible effect: every MM-painted top/bottom shell claim becomes a full-width prism
instead of a truncated pyramid — painted colour pushed right out to the outer wall on every
claimed layer, extra filament, extra tool changes, and a silent divergence from stock BBS/Orca on
every painted print. This is a strictly larger blast radius than the I2 defect it was fixing.

The only painted colour that escapes is one equal to its parent region's own `wall_filament`
(`get_create_region` then aliases the painted region to the parent, `PrintApply.cpp:1100-1102`),
i.e. painting a volume with the colour it already had — not the normal case.

**Confidence / status.** Traced mechanically through four independent sites (region creation,
`slices_to_regions`' coverage, the `:5230→:5267→:5275` ordering, and `num_regions`' sole consumer
being the assert). **Not runtime-confirmed**: the corroborating assert is compiled out in Release,
and no test in the suite discriminates the taper (every probe is dead-centre on a 40 mm slab,
where both tapered and untapered claims contain the probe). Corroborating circumstantial evidence:
upstream's `assert(out.num_regions > 0)` only ever held *because* the empty-slices painted
LayerRegion was counted.

**Suggested fix** — keep I2's intent, scope the guard to the two `max`es only:

```cpp
for (const LayerRegion *region : layer.regions()) {
    const PrintRegionConfig &config = region->region().config();
    if (! region->slices.empty()) {                 // I2: shell depth is layer-local ...
        out.top_shell_layers    = std::max(...);
        out.bottom_shell_layers = std::max(...);
    }
    if (color_idx == 0 || config.wall_filament == int(color_idx)) {
        ... // ... but the per-colour extrusion stats must still come from the painted
            // region, whose slices are empty until apply_mm_segmentation runs.
    }
}
```

A discriminating test: painted top cap on a slab, probe a point ~1–2 mm inside the silhouette at
the deepest claimed layer — tapered ⇒ unclaimed, untapered ⇒ claimed.

### N2 (Minor) — C1 residual: in a mixed-region object the zero-shell region's surface layer is still claimed

The surface-layer `append` at `:1536` is not gated on `stat.top_shell_layers > 0`. If *any* region
on the object has `top_shell_layers > 0`, `max_top_layers > 0`, `top_raw` is populated, and on a
layer where only a zero-shell region has geometry (a modifier setting `top_shell_layers = 0` over
part of the top, with that top painted) the surface layer is claimed even though
`LayerRegion.cpp:1025-1036` has demoted that surface to `stInternal`/`stInternalVoid`. Bounded to
exactly one layer and strictly smaller than the pre-fix-wave over-claim; the uniform case that C1
was written about is fully correct. Worth a comment or a `stat.top_shell_layers > 0` guard.

### N3 (Minor) — C1's coverage pins only one of its three sites

The inverted test goes red only for the `top_layers_eff` gate (`:1319-1322`); with that gate in
place, `max_top_layers == 0` short-circuits before the helper's early return (`:1230-1231`) or the
per-layer stat (`:1495-1498`) can be observed. Reverting the helper's early return alone keeps the
suite green. There is also no `bottom_shell_layers = 0` case. Both gaps are only reachable in
mixed-region objects (the same shape as N2), so one mixed-region test would cover N2, the helper's
early return, and the per-layer stat at once.

---

## Gates, re-run this session

Binary `build/tests/libslic3r/Release/libslic3r_tests.exe` (mtime 19:29), newer than both touched
sources (`MultiMaterialSegmentation.cpp` 19:28, `test_paint_depth_clamp.cpp` 19:21); `git status`
shows no modified tracked files, so the binary matches `530e2f52d2`.

```
[paintdepth]                 All tests passed (156 assertions in 22 test cases)
[chameleon]                  All tests passed (605 assertions in 133 test cases)
spike/verify_paintdepth.sh   17/17 checks passed.  RESULT: ALL PASS
```

All three reproduce the report's numbers exactly. None of them can catch N1 (no probe is placed
where the taper would matter) or N2/N3 (no mixed-region fixture exists).

## Deferred minors, unchanged

M4/M5/M6/M7/M8 from the original review remain open, as the report states. M6 (the walk recomputed
once per colour) is marginally cheaper now that empty regions are skipped. M8's `size_t`/`int`
wraparound at `:1539` is now load-bearing for the I1 top analysis above — it is what makes the top
`++m` unobservable — so cleaning it up would change behavior and should be done deliberately, not
as a tidy-up.
