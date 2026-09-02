# Absorb-tail review: `10559ee391..9277c13e9b` (feat/paint-depth, worktree `C:\Dev\SnapmakerOrcaNext`)

Scope: 520d05d7ff (inter-claim absorb + Item 2 filter move), 5789f2d560 (`paint_depth_solid_interfaces`),
9277c13e9b (gap-fill-off kill-width tracking). Read-only; every number below was produced on this
worktree's binary in this session, every code claim was hand-traced at the cited line.

**Verdict: FIX FIRST.** 0 Critical, 2 Important, 9 Minor.

Binary faithfulness (the failure mode that bit the absorb commit itself): an incremental
`cmake --build --target libslic3r_tests` compiled **zero** sources and left
`build/tests/libslic3r/Release/libslic3r_tests.exe` at its 14:45:13 timestamp; `git status` shows no
modified source. The binary tested below IS HEAD's source.

---

## One-line results per mandatory check

| # | Check | Result |
|---|---|---|
| 1 | Absorb correctness (a-d) | **PASS** - real containment, deterministic winner, gates skip unlimited + fuzzy; caveats in M1/M3/M5 |
| 2 | No leftover debug code | **PASS** - nothing in the diff; the `[.absorbdiag]` harness is gone; see M8 for untracked artefacts |
| 3 | Threshold correction | **PASS** - `t = 0.225` is an opening delta, kill width `2t = 0.45 = min_claim_width`; 0.225 < 0.34 < 0.45 is consistent |
| 4 | Item 2 filter move | **FAIL (I1)** - additive for the colour itself and inert in unlimited, ceiling claim honest, BUT the legacy shadow bypasses the moved filter entirely |
| 5 | `paint_depth_solid_interfaces` | **PASS on code / FAIL on evidence (I2)** - all four sites gated, no fifth consumer; the RED proves exactly one of the four |
| 6 | gap_infill_speed==0 tracking | **PASS** - 0.7 is upstream's own constant, inert when gap fill is on everywhere; object-global widening can over-absorb on mixed objects (M4) |
| 7 | Determinism + regression | **PASS** - `[paintdepth]` 67/1014 identical under `--order rand --rng-seed 1` and `2`; `[chameleon]` 133/605; `verify_paintdepth.sh` 17/17 twice; full suite **495 \| 493 passed \| 2 failed-as-expected, 51093 assertions, exit 0** (see M6 for the "2 failed" in the previous report) |

---

## Important

### I1. Item 2's band-level opening is bypassed by the legacy shadow: un-opened descent rings reach the final claim

**Where.** `src/libslic3r/MultiMaterialSegmentation.cpp`
- `:2110-2111` / `:2179-2180` - the per-step `opening_ex(last, small_region_threshold)` is skipped when `normal_shell`.
- `:2123` / `:2190` - the now-RAW `last` is copied into `legacy_shell_triangles_by_color_top/bottom` (the legacy shadow) whenever the step is within `top_shell_layers`.
- `:2217-2227` - the shadow is unioned into `legacy_top_and_bottom_layers_out[c][L]` and **never opened** (no `opening_ex` anywhere in that loop).
- `:2266-2270` - the band-level opening is applied only to `top_band`/`bottom_band`, i.e. only to `triangles_by_color_merged` (= `top_and_bottom_layers`, the `full` claim).
- `:2741-2753` (wave-b-review Important-2 clip in `merge_segmented_layers`): `excess = full \ legacy; full = legacy U (excess \ other_painted_laterals)`. `full` is rebuilt **from the raw `legacy` verbatim**.

**Failure scenario.** Bounded mode (the default), any layer that (i) receives descent contributions from an origin beyond the solid-shell depth (so `excess` is non-empty - every sub-surface layer 5..15 under a painted slope at stock 4-layer/0.6mm shells) and (ii) carries another painted colour's lateral claim (so `other_painted_laterals` is non-empty - every two-colour boundary, the sphere fixture included). On those layers the final claim is `legacy_raw U ...`, so every thin fragment (< 0.225mm) of every ring deposited within the shell depth survives - the #7104 thin-projection filter no longer applies to them at all. Before Item 2 those fragments were per-step opened away. Consequences: thin painted fragments interior to the shell (Arachne widens each into a 0.34mm bead of the wrong colour, Classic prints nothing - a sub-bead void), extra region-footprint churn (the symptom-3 driver), and - because the merge loop's trim (`segmented_regions_trimmed = diff_ex(lateral, top_and_bottom_by_extruder)`) uses the rebuilt `full` - those raw fragments also **remove geometry from the neighbouring colour's lateral band**, which the un-clipped legacy part is exempt from the Important-2 guard for. So "claim-only-additive, never removes previously-claimed geometry" (check 4) is false across colours. Everything stays >= one wall stack inside the contour (both ring terms are F1-inset), so this is not exterior bleed - it is the interior sliver class this branch has been chasing, reintroduced on the painted side where `has_interclaim_sliver` (base-only) cannot see it. The report's sentence "this is the only point these contributions are filtered at all - band-level, once" (`:2263-2265`, interclaim-absorb-report.md section 4) is wrong for the legacy-depth portion. Behaviour is also inconsistent: layers where `excess` or `other_painted_laterals` is empty keep the band-opened `full`, layers where both are non-empty get the raw legacy.

**Fix.** Keep the clip's intent without re-importing raw geometry: at `:2751-2753` replace `combined = legacy U excess_clipped` with `full = diff_ex(full, intersection_ex(excess, other_painted_laterals))` (identical when `legacy` is a subset of `full`, which was the pre-Item-2 invariant; differs only by the un-opened fragments). Alternatively apply the same `stat.normal_shell`-gated `opening_ex(reach, stat.small_region_threshold)` at `:2227`. Add a pin: on the sphere fixture, assert no painted-colour component in `claim_for_layer(., 2|3)` is empty under `opening_ex(., scaled(0.1125))` while lying inside `offset_ex(lslices, -0.87854)` (the painted twin of `has_interclaim_sliver`). Given the absorb report's own admission that Item 2 is not load-bearing, reverting Item 2 is also acceptable.

### I2. The `paint_depth_solid_interfaces` RED evidences one of the four gated sites; three could be ungated and the suite would still pass

**Where.** `tests/libslic3r/test_paint_depth_clamp.cpp:572-579` asserts `!extruder2_layer_has_solid_skin(...)` (`:285-300`), which inspects `layerm->slices.surfaces` surface types - the output of `detect_surfaces_type()` only, i.e. site 1 (`PrintObject.cpp:1337-1338`).
- Site 2 `PrintObject.cpp:1772-1773` (`discover_vertical_shells`, `top_bottom_surfaces_all_regions`) changes `fill_surfaces` (internal-solid additions), never `slices.surfaces` types - invisible to the assertion.
- Sites 3/4 `PerimeterGenerator.cpp:622` and `:2273` are reached only under `only_one_wall_top` (`:1450`, `:2246-2249`), whose default is `false` (`PrintConfig.cpp:1228`) and which the fixture never sets - the gated branches do not execute in the test at all.

So "reverting 4 gates => exactly 1 failure" is exactly what "reverting gate 1 alone" produces; it does not distinguish the two. The code IS gated at all four sites (hand-verified above; `has_bounded_paint_depth` consumers = `LayerRegion.cpp:227` plumbing + the four sites, nothing else in `src/` or `tests/`), so this is a coverage gap, not a defect - but on a feature where the previous fix broke the thing it fixed, a gate with no test is a gate that will silently regress.

**Fix.** (a) Site 2: on the Z-interface cube after `process()`, compare stInternalSolid area in `fill_surfaces` of the base region at the interface layer with the option on vs off (`ensure_vertical_shell_thickness` at its default). (b) Sites 3/4: run the same fixture with `only_one_wall_top = true` and compare the Extruder2 region's `fill_surfaces` top-fill area (or `perimeters` count) on vs off - the `upper_slices_same_region` branch produces a top surface at the colour boundary that the `*upper_slices` branch does not.

---

## Minor

### M1. Winner comparison is a double shoelace, not an "exact integer" one (comment and report are wrong; behaviour deterministic)
`MultiMaterialSegmentation.cpp:2653-2657`, `:2672` and interclaim-absorb-report.md section 3 say `ExPolygon::area()` is an integer shoelace. It is `double` (`Polygon.cpp:47-59`: `Vec2d` casts, `cross2`, `0.5 * a`; `ExPolygon.cpp:50-56`). Products are exact only while |coordinate| < ~2^26.5 (about 95mm from the object centre) and the sum is order-dependent. For a fixed binary the result is still deterministic (same input, same order), and the absorb's operands (eps = 200 units by island-rim length) are tiny, so ties on exactly mirror-symmetric geometry compare equal (the unit test at `:3223` passes for that reason). Near-ties on real geometry are decided by rounding, not by index. Fix the comment; optionally accumulate `int64_t` cross products.

### M2. Unlimited-mode "legacy parity" test cannot detect a behaviour change
`test_paint_depth_clamp.cpp:3467` slices the same fixture twice with the same binary and compares - that pins run-to-run determinism only. Unlimited byte-identity rests on the gate trace (`paint_depth_normal_mm = 0` => `normal_shell == false` at `:1912-1914` => per-step opening kept at `:2110`/`:2179`, band-level skipped at `:2267`, absorb skipped at `:2801`), which I verified, and on the fuzzy path (`IncludeTopAndBottomLayers::No` => `segmentation_top_and_bottom_layers` never runs; `bounded_mode == false`). Rename the test, or compare against a frozen pre-change claim snapshot.

### M3. The clamp's own keep-core is absorbed on parts 2.87-3.32mm thick (stock walls=3)
For a wall painted on both faces with thickness `w` in `(2*band, 2*band + min_claim_width]` = (2.871, 3.321] mm, `paint_depth_clamp_keep_core` (`:1239-1270`) leaves a hairline-to-0.45mm base core (`core_full = offset(layer, -1.435675)`, no ladder because `thin` is empty), which is thin, fully inside `offset(layer, -0.87854)` and painted-neighboured on both sides - so the absorb hands it to the lowest-index (or noise-decided, see M1) neighbour. Pre-absorb that core printed as nothing (Classic) or a 0.34mm base bead (Arachne), so the change is benign and hidden, but it is an undocumented policy change ("the interior stays base") and on even layers the notch shifts the window to (2.671, 3.121], so a 3.12-3.32mm wall now alternates absorbed/kept per layer - region churn that `paint_depth_solid_interfaces` (default on) turns into internal-solid patches. Document it at `:2810` and consider exempting islands whose winner margin is below eps-noise.

### M4. Gap-fill-off widening is object-global and can over-absorb 0.45-0.75mm genuine base on mixed objects
`:3347-3349` takes the MAX over every region with `gap_infill_speed <= 0`; `:2832` applies it to every island on every layer. One modifier volume with gap fill off raises the kill width to ~0.75mm for the whole object, so a deliberate 0.45-0.75mm unpainted stripe between two painted claims in a gap-fill-ON region (printable there as gap fill / one Arachne bead) is absorbed. The "does not over-absorb" pin (`:3688`) probes a many-mm-wide base region and cannot see this band. Fix: derive the threshold per layer from the regions with non-empty `slices` on that layer (the N1-style guard `layer_color_stat` already uses), or from the two neighbouring claims' own `small_region_threshold`. Also note `:3349` uses nominal `layer_height` while `layer_color_stat` (`:1855-1858`) uses `layer.height`: first-layer/adaptive heights drift the real kill width by <= ~0.015mm - negligible, but the "exact same formula" claim is not quite exact.

### M5. Kill-width boundary is inclusive
A strip of width exactly `2t` collapses under a `-t` erosion (Clipper drops the degenerate result), so a base strip AT 0.45mm is absorbed; `:2811-2812` says "narrower than". Measure-zero; fix the wording.

### M6. The previous report's "2 failed" are deterministic `--warn NoAssertions` artefacts, not environment flakes
Run alone with `--warn NoAssertions`: `Hollow two overlapping spheres` (`test_hollowing.cpp:7-20`, writes an OBJ, asserts nothing) and `Voronoi missing edges - points 12067` (`test_voronoi.cpp:33-74`, its only `REQUIRE` is commented out at `:73`) each report `1 failed`, exit 1; without the flag each passes (`assertions: - none -`). The absorb report ran without the flag (491 passed), the shell report with it (2 failed) - same code, different flag. shell-setting-and-gapfill-report.md's "environment/timing-sensitive flake in mesh-boolean/Voronoi code" is wrong. True plain number after these commits: **495 test cases \| 493 passed \| 2 failed as expected; 51093 assertions \| 51091 passed \| 2 failed as expected; exit 0** (the 51095 in the shell report = 51093 + the two warning-as-assertion entries).

### M7. `paint_depth_solid_interfaces` invalidates `posSlice`
`PrintObject.cpp:963` puts the key in the layer_height/paint_depth group, so toggling it re-runs slicing and MMU segmentation although only `posPerimeters`/`posPrepareInfill` consume it. Over-invalidation is safe and was the brief's instruction; noting it because it makes the GUI round-trip on this option a full reslice.

### M8. Untracked artefacts in the worktree root (hygiene, not debug code)
`twospheres.obj` and `one_vertex_touch.svg` (written by `test_hollowing.cpp:19` / `test_geometry.cpp:540` whenever the suite is run with cwd = repo root), plus `spike/out/paintdepth_*.gcode` from verify runs. None are committed; add to `.gitignore` or delete. The diff itself is clean: added lines contain only the two `BOOST_LOG_TRIVIAL(debug)` Begin/End markers (`:2802`, `:2898`) that match the file's existing stage-marker convention; no `if (false`, `TEMP`, `#if 0`, `printf`/`cout`, `getenv`, or `[.absorbdiag]` anywhere in `src/` or `tests/`.

### M9. Dead wiki anchor
`Tab.cpp:2642` links `multimaterial_settings_advanced#paint-depth-solid-interfaces`, which does not exist (same as the sibling `#paint-sparse-infill`). No-op in the UI.

---

## Check details (what was actually executed / traced)

**1a.** `:2880` `diff_ex(single, interior).empty()` - true geometric containment against `offset_ex(input_expolygons[L], -wall_stack)` (`:2860`), holes handled because `offset_ex` grows holes outward. Boundary-touching islands produce non-empty diff residue and are skipped - conservative.
**1b.** `:2673` strict `>` on an ascending scan => lowest index wins exact ties; empty claims skipped (`:2664`), zero-overlap skipped (`:2668`); see M1 for the "integer" claim.
**1c.** Per-connected-component test (`:2876-2879`) protects anything attached to a printable core (notch tooth pinned at `:3427`). Thin parts wholly inside the wall-stack band (the C-1 "keeps its whole cross-section" case) fail the interior test because `offset(part, -0.87854)` is empty for parts under 1.757mm. The one genuine-base class that IS absorbed is the keep-core hairline of M3; boundary semantics in M5.
**1d.** Unlimited: `paint_depth_normal_mm = 0` (`:3429`) => `normal_shell` false everywhere => legacy per-step opening retained, band opening and absorb skipped. Fuzzy skin (`:3452-3458`): `segmentation_normal_depth = 0`, `segmentation_wall_stack = 0`, `gapfill_off = 0`, `IncludeTopAndBottomLayers::No` => `segmentation_top_and_bottom_layers` never runs, `bounded_mode == false`. The merge loop's new `layer_color_stat` call (`:2266`) adds no new `assert(num_regions > 0)` exposure - the descent loop already calls it unconditionally for every (layer, colour) at `:1956`.
**3.** `t = scaled(0.45 * 0.5)` (`:2833`); opening delta `t` annihilates width `2t`. Two independent 0.225mm erosions bound the inter-claim gap at 0.45mm plus miter-corner truncations (which stay narrow, so width-keyed absorption still catches them). The reported 0.34mm annuli lie between the investigation's 0.225 bound and the 0.45 kill width - consistent; a 0.225 kill would have missed them.
**4.** For the colour's own claim: `opening(A U B)` contains `opening(A) U opening(B)` (monotone), and termination on the raw ring runs at least as deep as on the opened ring => additive. Inert in unlimited (1d). Report section 4 states plainly the ceiling stays 23.96 deg and does not claim otherwise - honest. Cross-colour: see I1.
**5.** Default `true` at `PrintConfig.cpp:4010`; four sites read `(bounded && option)` so default is a no-op by identity; `false` collapses each site to `object_config->interface_shells` / `m_config.interface_shells.value`. Greying `ConfigManipulation.cpp:921` mirrors `paint_infill_override`; `Preset.cpp:929`; `PrintConfig.hpp:921`; `Tab.cpp:2642`; `verify_paintdepth.sh:190` strip. Registration parity with `paint_infill_override` confirmed by grep (same file set). Evidence gap: I2.
**6.** `0.7f * Flow::rounded_rectangle_extrusion_spacing` is the upstream "two lines slightly overlapping" arm - present verbatim at merge-base `10559ee391:1855` and in the unmodified main fork `C:\Dev\SnapmakerOrca\...:1360`; not tuned here. `max_claim_width_absorb_gapfill_off` stays `0.f` when no region has `gap_infill_speed <= 0`, so `std::max(min_claim_width, 0.f)` is exactly the pre-commit threshold - byte-identical, and the gap-on sphere pin (`:3675`) plus verify parity agree. Over-absorb on mixed objects: M4.
**7.** Logs in scratchpad `review_paintdepth_seed1.log` / `_seed2.log` (orders differ, case sets and all non-timing output identical, `All tests passed (1014 assertions in 67 test cases)` both), `review_chameleon.log` (605/133), `review_fullsuite_plain.log`, `review_noassert_hollow.log` / `_voronoi.log`, `review_verify1.log` / `_verify2.log` (17/17, unpainted byte-parity vs the frozen baseline intact, run1 == run2).
