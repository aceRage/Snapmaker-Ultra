# Shell-setting and gap-fill sliver follow-ups — implementation report (Item 1 + Item 2)

Worktree `C:\Dev\SnapmakerOrcaNext`, branch `feat/paint-depth`, base `520d05d7ff`.
Spec: this task's brief, referencing `interclaim-absorb-report.md`'s "still open" section
(item 1 = its symptom-3 pointer, item 2 = its loose end 3) and
`interclaim-sliver-investigation.md` (symptom 3 mechanism, loose end 3 numbers).

Two commits, each independently green:

| commit | subject |
|---|---|
| `5789f2d560` | `feat(paint-depth): add paint_depth_solid_interfaces to gate Stage 2's forced solid shells (default ON)` |
| `9277c13e9b` | `fix(paint-depth): track the absorb's kill width to the gap-fill-off sliver population (loose end 3)` |

---

## Item 1 — `paint_depth_solid_interfaces`

### The four read sites

`PrintObject::has_bounded_paint_depth()` (`Print.hpp:514`, unchanged — kept purely factual:
`is_mm_painted() && paint_depth_mode != unlimited`) was OR'd unconditionally into
`interface_shells`'s effective value at exactly four sites, located by grepping
`has_bounded_paint_depth`:

1. `PrintObject.cpp:1338` — `detect_surfaces_type()`'s `interface_shells` local.
2. `PrintObject.cpp:1773` — `discover_vertical_shells()`'s `top_bottom_surfaces_all_regions` local.
3. `PerimeterGenerator.cpp:622` — `split_top_surfaces()`'s top-surface-clip branch.
4. `PerimeterGenerator.cpp:2273` — `process_arachne()`'s equivalent branch.

(`LayerRegion.cpp:227`, which assigns the raw fact into `PerimeterGenerator::has_bounded_paint_
depth`, is plumbing, not a read site, and was deliberately left alone — each of the two
PerimeterGenerator.cpp sites now individually ANDs that raw field with `object_config->
paint_depth_solid_interfaces`, so the field itself stays a pure fact.)

All four now read `(has_bounded_paint_depth-ish) && paint_depth_solid_interfaces` instead of the
raw fact alone. `has_bounded_paint_depth()` itself is untouched — it is used nowhere else in the
codebase (confirmed by grep), so this keeps it honest as a factual query while the new option is
a separate policy layered on top at each consuming site.

### The option

`paint_depth_solid_interfaces`, `coBool`, default `true`. Defined in `PrintConfig.cpp` right
after `paint_infill_override` (same Multimaterial/Advanced group as the other `paint_depth_*`
keys), field added to `PrintObjectConfig` in `PrintConfig.hpp` in the same position.

- **Greyed** in `ConfigManipulation.cpp::toggle_print_fff_options` whenever `paint_depth_mode ==
  unlimited` — identical condition to the neighbouring `paint_infill_override` toggle, since
  `has_bounded_paint_depth()` is false for the whole object in that mode regardless of this
  option's value.
- **GUI row** added in `Tab.cpp`'s Multimaterial → Advanced group, directly after
  `paint_infill_override`'s row.
- **Preset registration**: added to `s_Preset_print_options` (`Preset.cpp`, returned by
  `Preset::print_options()`) next to the other `paint_depth_*` keys.
- **Invalidation**: added to `PrintObject::invalidate_state_by_config_options`'s `paint_depth_*`
  group (`posSlice`, which cascades forward through `posPerimeters`/`posPrepareInfill`/everything
  after) — the same group the other four `paint_depth_*` keys already invalidate on, per the
  task's instruction to keep it in that group rather than deriving a separate (narrower) step.

### Tests / RED evidence

New test `test_paint_depth_clamp.cpp:578` ("`paint_depth_solid_interfaces=false` falls back to
plain `interface_shells` (no solid skin) at a bounded color Z-interface"): reuses the existing
`process_z_interface_cube` Z-interface harness (extended with a trailing defaulted
`paint_depth_solid_interfaces` parameter, and `paint_depth_test_config` likewise), slices with
the option explicitly `false` and `interface_shells` at its own plain default (`false`), asserts
`CHECK_FALSE(extruder2_layer_has_solid_skin(...))`.

**Real RED, obtained by temporarily reverting the fix, not by flipping the compiled default**
(the originally-planned "flip the registry default" technique turned out inert: this test file's
own helper — mirroring the pre-existing `paint_infill_override` pattern — always sets the option
explicitly via a C++-level default parameter, so it never reads the `PrintConfigDef` registry
default at all; documented in-code as a discovered footgun, not silently worked around).
Temporarily reverted all four gates back to the raw `has_bounded_paint_depth()` OR (removing the
`&& paint_depth_solid_interfaces` conjunct), rebuilt, ran `[paintdepth]`:

```
test_paint_depth_clamp.cpp(578): failed: !(extruder2_layer_has_solid_skin(*object, first_painted_layer)) for: !true
Failed 1 test case, failed 1 assertion.
```

Exactly the new test failed, all 63 others (including the unmodified "a bounded color
Z-interface gets solid skin" default-true pin) still passed. Restored the real gating, rebuilt,
confirmed green again.

`[paintdepth]`: 63/994 → **64/998** (the one new test's 4 assertions). `[chameleon]`: 133/605,
unchanged/exact.

### `spike/verify_paintdepth.sh` fix-forward

The new key appears in every sliced object's `CONFIG_BLOCK` dump (it is a real, registered
option), which broke the script's byte-parity check against the frozen pre-feature baseline —
exactly the same situation the script's `normalize()` strip list already handles for
`paint_depth_mode`/`walls`/`mm`/`paint_infill_override`/`mmu_segmented_region_interlocking_depth`
(documented in its own header: "new or changed-default as of this feature ... config-dump-only").
Extended that established strip list (and its comment) with `paint_depth_solid_interfaces`, same
justification: it is read only inside `has_bounded_paint_depth()`'s effect, which is always false
for `!is_mm_painted()`, so an unpainted object never consults it — config-dump-only, no toolpath
difference.

---

## Item 2 — `gap_infill_speed == 0` widens slivers past the absorb's kill width

### Mechanism

`layer_color_stat` (`MultiMaterialSegmentation.cpp`, inside `segmentation_top_and_bottom_
layers`) computes `small_region_threshold` per (layer, colour):

```cpp
out.small_region_threshold = config.gap_infill_speed.value > 0 ?
                             0.5f * outer_wall_line_width :                                    // gap fill ON
                             outer_wall_line_width + 0.7f * Flow::rounded_rectangle_extrusion_spacing(outer_wall_line_width, layer.height); // gap fill OFF
out.small_region_threshold = scaled<float>(out.small_region_threshold * 0.5f);
```

This is the `opening_ex(·, small_region_threshold)` delta applied to each colour's OWN
top/bottom claim (the #7104 thin-projection filter) — kill width = `2 × small_region_threshold`.
At stock flows (0.45mm outer wall, 0.1mm layers): gap fill ON → kill width 0.225mm; gap fill OFF
→ kill width **~0.75mm** (measured in the new test: `0.74998mm`, matching the investigation's
hand computation almost exactly). Nothing re-unions the two neighbouring colours' independently-
eroded claims, so the deleted strip between them falls through to the base residue — this is the
same upstream mechanism `interclaim-sliver-investigation.md` traced for the original (gap-fill-ON)
defect Item 1 fixed; disabling gap fill simply triples its kill width.

Item 1's absorb reclaims exactly this class of strip, but its own threshold (`t = min_claim_
width / 2`, kill width = `min_claim_width` = 0.45mm at stock) does **not** track the wider
gap-fill-OFF population: a sliver in the 0.45–0.75mm range has a "printable core" under the
absorb's own `opening_ex(single, t).empty()` test, so the absorb (wrongly) treats it as genuine
base and leaves it — it survives and prints in the base filament.

### Both generators affected

The sliver is created upstream, in segmentation, identically regardless of which perimeter
generator later processes the claim geometry — the mechanism is generator-agnostic. What differs
is only whether the *resulting* strip actually prints once it survives to `apply_mm_segmentation`:
Classic emits nothing for a strip too thin to admit a first external loop, but a 0.45–0.75mm
strip easily clears that; Arachne widens any strip past `min_feature_size` (0.1mm) into a
`min_bead_width` (0.34mm) bead. At ~0.75mm both generators clear their own practical threshold
and print the sliver, so **both Classic and Arachne are affected** by the pre-fix defect and both
benefit equally from the fix (which operates upstream of generator choice).

### The conditional threshold

Added a second, absorb-specific object-level scalar in `multi_material_segmentation_by_painting`
(`max_claim_width_absorb_gapfill_off`): **MAX across the object's printing regions where
`gap_infill_speed.value <= 0`**, of `ext_perimeter_width + 0.7 × Flow::rounded_rectangle_
extrusion_spacing(ext_perimeter_width, layer_height)` — the exact same formula `layer_color_
stat`'s "gap fill disabled" arm uses (so it doesn't drift from what the generators actually
consume). Zero if no region has gap fill disabled.

Plumbed as a new parameter alongside the existing `segmentation_wall_stack` through
`segmentation_by_painting` into `merge_segmented_layers` (new param `min_claim_width_gapfill_
off`). Inside the absorb: `effective_claim_width = std::max(min_claim_width, min_claim_width_
gapfill_off); t = scaled(effective_claim_width * 0.5f)`.

**Deliberately a separate parameter, not folded into `min_claim_width` itself** — `min_claim_
width` is *also* the degradation ladder's floor (`paint_depth_clamp_keep_core`, called from
`cut_segmented_layers`), an unrelated site; widening that floor was never asked for and is not
touched. **Conditional, not unconditional**: an object with gap fill enabled everywhere
(`max_claim_width_absorb_gapfill_off == 0`) is byte-identical to before this fix — the `std::max`
degrades to `min_claim_width` alone.

### Tests / RED evidence

Three new tests, all `[paintdepth]`:

1. **RED pin** — `slice_bounded_sphere_two_colours(8.0, pdmWalls, 3, print, /*gap_infill_speed=*/0.0)`
   (the Item 1 curved sphere fixture, extracted into a reusable helper with a trailing
   `gap_infill_speed` override, sentinel `< 0` = leave at registry default, mirroring
   `slice_painted_box`'s existing convention). `has_interclaim_sliver` extended with a third
   `kill_width_mm` parameter (default 0.45, so every existing caller is unaffected); this test
   computes the expected gap-off kill width from the object's own real per-region flow (not
   hardcoded) and asserts `CHECK_FALSE(has_interclaim_sliver(*object, 0.878540, kill_width_mm))`.
2. **Gap-fill-on unchanged** — same sphere fixture, registry-default `gap_infill_speed`,
   `CHECK_FALSE(has_interclaim_sliver(*object))` (defaults, i.e. the pre-existing 0.45mm probe) —
   explicitly pins that an object with gap fill on everywhere sees no change from this fix.
3. **No over-absorption** — the Item 1 "does not over-absorb" opposite-walls frustum fixture
   (`slice_bounded_frustum_two_colours`, extended with the same trailing `gap_infill_speed`
   sentinel parameter), now with `gap_infill_speed=0.0`: the multi-mm-wide genuine base region
   between the two claims must still read as base, proving the conditional widening doesn't
   swallow real base area even when it IS active for that object.

**Real RED**, obtained by temporarily neutralizing just the fix's effect
(`effective_claim_width = min_claim_width` in place of the `std::max`, all plumbing/signatures
left intact), rebuilt, ran `[paintdepth]`:

```
test_paint_depth_clamp.cpp(3672): failed: !(has_interclaim_sliver(*object, 0.878540, double(kill_width_mm))) for: !true
with 1 message: 'kill_width_mm := 0.74998f'
Failed 1 test case, failed 1 assertion.
```

Exactly the new RED-pin test failed (the other two new tests and all 64 pre-existing ones stayed
green, as designed). Restored the real `std::max`, rebuilt, confirmed green again.

`[paintdepth]`: 64/998 → **67/1014** (three new tests). `[chameleon]`: 133/605, unchanged/exact.

---

## Full-suite number (after both commits, `libslic3r_tests`, no tag filter)

```
test cases:   495 |   491 passed | 2 failed | 2 failed as expected
assertions: 51095 | 51091 passed | 2 failed | 2 failed as expected
exit code: 2
```

**Not the brief's cited baseline (491/489/2/exit 0) — reported honestly, not smoothed over,**
per this feature's own established precedent (`interclaim-absorb-report.md` section 6 already
records the same kind of drift). The 2 unexpected failures are **not** the shouldfail pair; they
are `--warn NoAssertions` flags on:

- `Hollow two overlapping spheres` (`tests/libslic3r/test_hollowing.cpp`)
- `Voronoi missing edges - points 12067` (`tests/libslic3r/test_voronoi.cpp`)

Both files are outside this task's scope, untouched by either commit, and unrelated to paint
depth, `MultiMaterialSegmentation`, `PrintConfig`, `PrintObject`, or `PerimeterGenerator`.
`interclaim-absorb-report.md` itself already named these exact two test cases as having been
observed to intermittently produce zero assertions in a prior session ("The two cases that were
genuinely failing in that earlier log ... pass here in both runs. They are neither touched nor
explained by this change"). In this session's environment they reproduced consistently (3/3
repeat runs, same two names, same "No assertions in test case" message each time) — a pre-existing,
environment/timing-sensitive flake in mesh-boolean/Voronoi code, not a regression from this work.
The 2 *failed-as-expected* cases are the same known `[!shouldfail]` pair as always
(`test_mixed_filament.cpp:3483`, `:4429`).

492 → 495 test cases across the two commits (492 baseline + 1 item-1 test + 3 item-2 tests, minus
the pre-existing baseline count already reflecting the absorb's own prior test additions).

---

## Verify results

`spike/verify_paintdepth.sh`, run twice after **each** commit (4 runs total): **17/17, ALL PASS**,
unpainted byte-parity intact, every run. `ALL_BUILD` (scratchpad wrapper): exit 0 after each
commit's final (fix-restored) state.

---

## Files touched

**Item 1** (commit `5789f2d560`): `src/libslic3r/PrintConfig.hpp`, `src/libslic3r/PrintConfig.cpp`,
`src/libslic3r/PrintObject.cpp`, `src/libslic3r/PerimeterGenerator.cpp`,
`src/libslic3r/PerimeterGenerator.hpp`, `src/libslic3r/Preset.cpp`,
`src/slic3r/GUI/ConfigManipulation.cpp`, `src/slic3r/GUI/Tab.cpp`,
`tests/libslic3r/test_paint_depth_clamp.cpp`, `spike/verify_paintdepth.sh`.

**Item 2** (commit `9277c13e9b`): `src/libslic3r/MultiMaterialSegmentation.cpp`,
`src/libslic3r/MultiMaterialSegmentation.hpp`, `tests/libslic3r/test_paint_depth_clamp.cpp`.

`src/libslic3r/Print.hpp` (`has_bounded_paint_depth()`'s own definition) was read but
deliberately **not** modified — it stays a pure fact; the new option is layered on at each
consuming site instead, per Item 1's design reasoning above.

---

Report path: `.superpowers/sdd/2026-08-31-paint-depth/shell-setting-and-gapfill-report.md`
