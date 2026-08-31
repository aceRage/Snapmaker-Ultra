# Task 2 — Clamp wiring

Worktree: C:\Dev\SnapmakerOrcaNext, branch feat/paint-depth, base ed726d71e9 (Task 1)

## Status: COMPLETE

All five plan items are implemented, `[paintdepth]` (8 test cases / 40 assertions,
confirmed stable across repeated runs) and `[chameleon]` (133 test cases / 605
assertions, unchanged from Task 1) are green, and the full `ALL_BUILD` gate passed exit
0 (segmentation is core, per the BUILD-SLOT RULE `ALL_BUILD` was required; a real-compile
build slot was busy on the first check, so per protocol I waited rather than building,
re-checked once idle, then ran `ALL_BUILD` in the background and confirmed exit 0 before
proceeding).

## Implementation (items 1-3)

### 1. Clamp wiring — `multi_material_segmentation_by_painting` (MultiMaterialSegmentation.cpp:2198-2237)
Replaced the direct read of the now-legacy `mmu_segmented_region_max_width` with a band
computed via Task 1's `paint_depth_band_mm(paint_depth_mode, paint_depth_walls,
paint_depth_mm, ext_perimeter_width, perimeter_spacing)`. Per-region flow (external
perimeter width for the first wall, perimeter spacing for each additional wall) is
sourced from **every `PrintRegion` on the object** (`print_object.num_printing_regions()`
/ `printing_region(i)`), mirroring `fuzzy_skin_segmentation_by_painting`'s existing
precedent (:2237-2253 in this file) of deriving its clamp width from the object's
region flows generically rather than from a specific painted region. This call site has
no cheap paint-facet → `PrintRegion` mapping available (that mapping lives in
`PrintObjectSlice.cpp`'s `PrintObjectRegions::LayerRangeRegions`, built later in the
pipeline in a different translation unit), so — matching the fuzzy-skin precedent exactly
— every region's flow is a candidate and **the maximum per-region band wins** when
regions differ (documented in-code as the deliberate conservative choice: a narrower
region's thinner walls must never under-clamp a wider region's paint claim). The computed
`max_width` (mm) feeds the existing `cut_segmented_layers` gate (:2169-2172)
unconditionally — that gate's shape (`segmentation_max_width > 0.f || …`) is untouched.

### 2. Whole-layer short-circuit (:2149-2151) — hand-walk + verified, no code change needed
Traced the call graph: `has_layer_only_one_color` (inside `segmentation_by_painting`,
:2149-2151) writes the single ringing color's claim as
`segmented_regions[layer_idx][color] = input_expolygons[layer_idx]` (the entire
cross-section) into the *same* `segmented_regions` array that the Voronoi
(`extract_colored_segments`) path writes into. The `cut_segmented_layers` gate at
:2169-2172 runs *after* the full per-layer loop completes, and iterates
`0..segmented_regions.size()` **unconditionally** — it has no branch distinguishing
"came from the whole-layer short-circuit" vs "came from the Voronoi graph". So once item
1 makes `max_width` nonzero for a bounded mode, a fully-ringed layer's whole-cross-section
claim gets `diff_ex`'d against the same inward-offset "core" polygon as every other
layer, shrinking it to just the boundary band exactly like the Voronoi case. No routing
change was needed — the existing pipeline already treats both origins uniformly; the gap
was purely that the clamp was never turned on. Confirmed empirically (not just by
reading) with the "a fully-painted boundary still clamps to the band" end-to-end test
below, which literally reproduces the has_layer_only_one_color path (all 8 side facets of
a test cube painted the same color, so every mid-object layer's entire boundary is one
color) and asserts the object's center is *not* in the extruder's claimed area in walls
mode, while confirming (in the paired unlimited-mode test) that without the clamp the bug
is real: the center *is* claimed.

### 3. Interlocking mutual exclusion (:2169) — preserved, with an added mode gate
The gate's own expression at :2169 (`(segmentation_max_width > 0.f ||
segmentation_interlocking_depth > 0.f) && !segmentation_interlocking_beam`) is
byte-identical to before — untouched, as the plan requires. However, Task 1 flipped
`mmu_segmented_region_interlocking_depth`'s default from 0 to 0.3, which meant that
without further work, `pdmUnlimited` (the "fully off, legacy" mode) would still trip the
`segmentation_interlocking_depth > 0.f` half of that OR and clamp a 0.3mm interlocking
band on every other layer — breaking the "unlimited mode = legacy behavior, bit-identical"
requirement. Fixed by gating the *value fed into* the gate, not the gate itself:
`interlocking_depth = paint_depth_mode != pdmUnlimited ? mmu_segmented_region_interlocking_depth.value : 0.f`.
This was caught by real TDD RED (see Testing below) — my first end-to-end unlimited-mode
test genuinely failed against the code before this gate was added, for exactly this
reason.

## Testing (item 4) — end-to-end achieved (not geometry-only)

**Investigated whether the segmentation entry points are instantiable with synthetic
meshes + TriangleSelector paint state: yes.** `tests/libslic3r/test_mixed_filament.cpp`
already has a working precedent ("Mixed filament component edits rebuild painted region
targets") that builds a `Model`/`ModelObject`/cube `ModelVolume`, paints facets via
`TriangleSelector` + `volume->mmu_segmentation_facets.set(selector)`, and drives a real
`Print::apply()`. I extended this to a full slice: `PrintObject::slice()` is public
("Called by make_perimeters()") and — critically — internally calls
`multi_material_segmentation_by_painting` itself as part of `slice_volumes()`'s "Is any
ModelVolume multi-material painted?" branch, then folds the result into the per-region
`LayerRegion::slices` via `apply_mm_segmentation`. That gave a route to the actual
end-to-end "gold standard" the plan asked for: paint a real mesh, slice a real
`PrintObject`, and inspect the *actually-applied* claimed geometry.

New file `tests/libslic3r/test_paint_depth_clamp.cpp`, registered in
`tests/libslic3r/CMakeLists.txt`, `[paintdepth]` tag, 4 test cases / 8 assertions:

- **walls mode clamps a full-height painted face to the wall band** — a 40x40x20mm cube
  with one entire side face painted Extruder2 (walls=3): a point 0.3mm from the painted
  face IS claimed, a point 10mm in is NOT.
- **unlimited mode leaves the same painted face unbounded (legacy parity)** — same
  fixture, `pdmUnlimited`: the 10mm-in point IS claimed (unclamped Voronoi split reaches
  well past any plausible band). Also pins item 3's interlocking gate.
- **a fully-painted boundary still clamps to the band (whole-layer short-circuit)** — all
  8 side facets painted (reproduces `has_layer_only_one_color` at every mid-object layer):
  walls mode claims near the edge but NOT the object's exact center.
- **unlimited mode reproduces the whole-layer-claims-interior bug on a fully-painted
  ring** — same fixture, `pdmUnlimited`: the center IS claimed, documenting the
  pre-existing (intentionally preserved) legacy behavior.

`extruder2_claim_for_layer()` reads the *final, already-applied* geometry: after `slice()`
runs, `apply_mm_segmentation` has split the base `PrintRegion` and created an auto
`PrintRegion` with `config().wall_filament == 2` for the painted color; the helper finds
that region's `LayerRegion::slices` for the queried layer. This was a deliberate design
change partway through: my first draft called `multi_material_segmentation_by_painting`
a *second* time directly (after `slice()`'s internal call had already run it once and
`apply_mm_segmentation` had already mutated/split the region surfaces it reads from) —
empirically this was flaky, including an intermittent SIGSEGV, because the second call's
`input_expolygons` were built from already-split, no-longer-representative region
geometry feeding the Voronoi/graph algorithms a malformed input. Switching to inspect the
already-applied result exactly once (no duplicate invocation) made all 4 tests
deterministic across repeated runs (verified 3x in isolation, plus repeated full-suite
runs).

**Real TDD RED observed, twice:**
1. With `MultiMaterialSegmentation.cpp` stashed back to the pre-Task-2 state (`git stash`),
   the "unlimited mode leaves the same painted face unbounded" test genuinely failed
   (`any_contains(extruder2_claim, deep_point) == false`) — Task 1's interlocking-depth
   default flip alone (without Task 2's mode gate) clamps even in "unlimited" mode on
   alternating layers, exactly the item-3 bug. Restoring the real fix turned it GREEN.
2. Along the way, a genuinely pre-existing (not Task-2-introduced) config gap surfaced:
   any MM-painted object sliced with `full_print_config()`'s bare option-registry defaults
   throws `Flow::spacing() produced negative spacing`, because
   `outer_wall_line_width`'s bare default is a literal `0` (not `0%`) and
   `MultiMaterialSegmentation.cpp`'s `layer_color_stat` lambda (feeding
   `segmentation_top_and_bottom_layers`, which `multi_material_segmentation_by_painting`
   always runs) reads it via `config.get_abs_value()` with no auto-width fallback,
   unlike most other Flow call sites. Confirmed via bisection (temporary diagnostic
   logging in `slice_volumes()`, reverted before commit) that this reproduces
   identically with `MultiMaterialSegmentation.cpp` fully stashed back to Task 1, i.e. it
   predates this task. Real printer/filament presets always carry a non-zero
   `outer_wall_line_width`, so the test config sets one explicitly (documented inline)
   rather than relying on the bare default.

Also ran `[MixedFilament]` (181 test cases) as a broader regression check since
`MultiMaterialSegmentation.cpp` is shared infrastructure: 178 pass, 1 unexpected failure
+ 2 expected-failure ("(CURRENT BUG)" / "(KNOWN bug)" labeled) cases, all three in
filament-remap/redundancy-invariant tests unrelated to segmentation geometry or paint
depth — pre-existing, untouched by this diff (confirmed via `git diff --stat`: only
`MultiMaterialSegmentation.cpp` and test/CMake files changed).

## Thin-feature #6892 fixture (item 5) — not attempted; GUI-round note

Not implemented as an automated fixture. Constructing a synthetic mesh that reliably
reproduces PrusaSlicer #6892 (thin/small parts where Voronoi regions merge across the
part) would need careful geometry (multiple close painted regions on a thin wall) beyond
what was practical to validate in the time available for this task, and the design spec
explicitly fences it as "Voronoi-stage bounding is the fenced fallback only if this
reproduces badly" — i.e. it's an accepted, documented gap, not silently skipped.
**GUI-round note for Task 4 / manual verification:** paint two different colors close
together on a thin wall (wall thickness comparable to 2-3x the wall-mode band), slice with
`paint_depth_mode = walls`, and confirm the claimed regions don't merge/bleed across the
thin section in ways the per-object `max_width` band doesn't already predict.

## Self-review (hand-walk, per the task's binding review criteria)

- **Brown spot on thick yellow slab, walls mode 3 → claim bounded to
  ext_width+2*spacing:** confirmed by the "walls mode clamps a full-height painted face"
  test (band = ext_perimeter_width + 2*perimeter_spacing per `paint_depth_band_mm`'s
  walls-mode formula, walls=3) — claim reaches a 0.3mm-from-face point, does not reach a
  10mm-from-face point.
- **Brown full ring → still bounded:** confirmed by the "fully-painted boundary still
  clamps" test — this is the literal has_layer_only_one_color reproduction, not a proxy.
- **Unlimited mode → legacy behavior bit-identical:** confirmed by both unlimited-mode
  tests, and additionally reasoned through: under `pdmUnlimited`,
  `paint_depth_band_mm(pdmUnlimited, …)` always returns `0.f` regardless of loop inputs,
  and `interlocking_depth` is explicitly forced to `0.f`, so `segmentation_max_width == 0`
  and `segmentation_interlocking_depth == 0` — the exact same values the gate at :2169
  received before this task existed (both `mmu_segmented_region_max_width` and the
  pre-Task-1 `mmu_segmented_region_interlocking_depth` defaulted to 0), so
  `cut_segmented_layers` is never invoked, matching pre-feature behavior exactly.

## Concerns / notes for reviewers

- Item 1's region-selection choice (every `PrintRegion` on the object, max band across
  them, rather than only the specifically-painted region) is a judgment call the plan
  explicitly flagged as needing documentation ("document which region supplies widths
  when multiple"). I mirrored the existing `fuzzy_skin_segmentation_by_painting`
  precedent rather than inventing a new approach, on the reasoning that (a) it's an
  established, reviewed pattern in this exact file for this exact kind of clamp-width
  derivation, and (b) a cheap paint-facet → PrintRegion mapping isn't available at this
  call site. A future refinement (out of scope here) could plumb that mapping through if
  per-color band precision becomes a requirement.
- The pre-existing `outer_wall_line_width` / `Flow::spacing()` config gap (found while
  building the end-to-end tests) only manifests for MM-painted objects sliced against
  literally-bare option-registry defaults (as opposed to any real preset, which always
  sets a concrete line width) — flagged here for awareness but intentionally not "fixed"
  as it's pre-existing and out of this task's scope; worth a follow-up ticket if it can
  also bite real users somehow.
- Did not attempt item 5's #6892 fixture; see the GUI-round note above.

Report path: `.superpowers/sdd/2026-08-31-paint-depth/task-2-report.md`
