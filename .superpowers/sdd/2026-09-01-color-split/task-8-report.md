# Task 8 report — End-to-end slicing parity and the S4 wedge measurement

Branch `feat/color-split` in `C:\Dev\SnapmakerOrcaNext`. Commit **`13967943cc`** —
`test(color-split): end-to-end slice parity and S4 wedge measurement`. Test-only; no production code touched.

## What I implemented

`C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp`, one new section at the end plus one rebuilt
fixture. Five file-static helpers and two `TEST_CASE`s:

| Helper | What it does |
|---|---|
| `e2e_config()` | `split_test_config()` with `layer_height` and `initial_layer_print_height` both pinned to 0.2, so the layer grid is a plain uniform stack (100 layers on the 20 mm cube) and "the layer just below the surface layer" is exactly print_z 19.8. |
| `filament_area(const Layer *, int filament)` | Area in mm² a filament claims on a layer: sum over `layer->regions()` whose `region().config().wall_filament.value` matches, over `lr->slices.surfaces`, of `unscaled(unscaled(s.expolygon.area()))`. The brief's `filament2_area` generalised, because S4 needs the same read for filament 1 — `wall_filament` is the discriminator on **both** paths (`apply_mm_segmentation` stamps it on a 2D paint claim; `region_config_from_model_volume` derives it from a volume's own `extruder` key, PrintObject.cpp:3244-3257). |
| `layer_area(const Layer *)` | Same sum over every region — the whole sliced cross-section, used to check the split parts still tile the cube. |
| `slice_one(Print&, Model&, const DynamicPrintConfig&)` | `set_status_silent()` / `apply` / `objects_mutable().front()->slice()`, the sequence `slice_painted_cube` uses (test_paint_depth_clamp.cpp:97-119). |
| `split_in_place(ModelObject&, const DynamicPrintConfig&, const ColorSplitParams&)` | The split **in the exact call shape the GUI will use** (Task 9): `color_split_depths(config, {1, 2})` from the same config the slice uses, `color_split_space(object, *volumes.front())`, `scale_depths` / `scale_params` against `space.depth_scale`, and the **raw** mesh + paint with `space.to_split` carrying the world path (Ruling 23 — identity here, but the shape is the point). Requires no warnings (a skipped component would make the areas meaningless) and exactly two created volumes. |

**Case 1 — `colorsplit e2e: split parts slice like the 2D paint-depth claim on a painted side face`**
(`[colorsplit]`). A 40×40×20 cube with the +X face painted Extruder2 is sliced twice through the real `Print`
pipeline: unsplit (the 2D paint-depth segmentation) and split into body + one part. Layer counts must match;
then, over the middle half of the stack (layers 25-74), the filament-2 area on each layer must agree within
one outer wall line over the 40 mm painted edge (16.8 mm²). Also asserts both areas are non-zero (so the
comparison cannot pass vacuously on two empty reads) and that the split object's whole cross-section is still
1600 mm².

The top and bottom quarters are excluded exactly as the brief specifies, because there the two paths differ by
design: the 3D shell tapers along the cube's corner bisector where the painted face meets the caps (spec 3.6),
which the 2D segmentation — working one layer at a time with no knowledge of the faces above or below — cannot
express.

**Case 2 — `colorsplit e2e S4: painted cube top keeps a body outer wall on the side faces`**
(`[colorsplit_spike]`). Same cube painted on top, split twice — `crease_step` off and on, `flat_cap` off both
times so the step alone decides the shape — sliced, and the body's area read on layer 98 (print_z 19.8, the
layer just below the surface layer). `WARN`s both numbers; `REQUIRE`s ≥ 0.9 × 4 × 40 × `depths.ws` when the
step is on, and (added) that the step case exceeds the no-step case, so the measurement is not inert.

**Folded follow-up (Task 7 re-review).** `colorsplit: a rotated anisotropic scale is not mistaken for an
isotropic one` used `R = AngleAxisd(0.25π, UnitZ())`, which leaves the third column norm at 1 — so
`color_split_space`'s `|sx − sz|` test alone already rejected it and the Gram off-diagonal test the case exists
for was never exercised. Rebuilt with `R = Quaterniond::FromTwoVectors(UnitX(), (1,1,1)/√3)`, i.e. a stretch of
2 along the body diagonal: `L = I + a aᵀ`, all three columns √2 long, `LᵀL = I + ones`. The case now asserts all
three column norms coincide, that the columns are *not* orthogonal (`|col0 · col1| = 1`), and `space.world_path`.

## RED / GREEN evidence

**RED 1 — the cases did not exist.** The binary built at 07:10 (before this task's edits) listed
`[colorsplit]` 53 cases, `[colorsplit_spike]` 2, `[paintdepth]` 94, with no case matching `e2e`. After the
change: 54 / 3 / 94.

**RED 2 — the parity bound bites (measured, then reverted).** With the parity case's split temporarily built
by `no_cap_no_step()` instead of `ColorSplitParams{}` — i.e. spec 3.6's crease step switched off, so the side
shell is a pure corner-bisector taper of D/√3 = 0.813 mm — the case fails on the first compared layer:

```
test_color_split.cpp(1673): FAILED:
  REQUIRE( std::abs(a2 - a3) <= one_line )
with expansion:
  22.4947240552 <= 16.8
with message:
  layer 25 print_z 5.2: 2D 54.3691 mm^2, 3D 31.8744 mm^2, diff 22.4947 mm^2 (bound 16.8)
```

So the 16.8 mm² budget is tight enough to catch a regression of the crease step and loose enough for the step
as specified. The probe was reverted and the tree rebuilt before the final run; the committed file has
`ColorSplitParams{}`.

**RED 3 — the S4 requirement bites, from the passing run itself.** The `crease_step=off` arm measures a body
area of 47.6398 mm², far below the 0.9 × 127.533 = 114.78 mm² the `on` arm has to clear. The same assertion
applied to the no-step geometry would fail.

**GREEN — final run** (after the last build, no source edited since):

```
[colorsplit]        All tests passed (800 assertions in 54 test cases)
[colorsplit_spike]  All tests passed (24 assertions in 3 test cases)
[paintdepth]        All tests passed (1568 assertions in 94 test cases)
```

`[colorsplit]` output is pristine — the only `warning:` line in that run is the pre-existing one at
test_color_split.cpp:704 (the "feature too small to carry a shell is skipped with a warning" case). The two new
`WARN`s live in `[colorsplit_spike]`, where the brief puts them.

## Measured numbers

Config resolves to **D = 1.40885 mm, ws = 0.79708 mm** on both paths (the 2D band goes through the same
`paint_depth_band_mm` + `paint_depth_band_classic_floor_mm` pair as `color_split_depths`).

### Per-layer 2D vs 3D (parity case, layers 25-74, 50 layers)

| | 2D, odd layers (25 of them) | 2D, even layers (25) | 3D, every layer |
|---|---|---|---|
| filament-2 area | **54.3691 mm²** | **50.6409 mm²** | **45.8867 mm²** |
| \|2D − 3D\| | **8.48242 mm²** | **4.75419 mm²** | — |

Bound 16.8 mm²; worst case 8.48 mm² = **50.5 % of budget**. The 3D area is constant to the last printed digit
on all 50 layers. The 2D alternation is the interlocking notch: `paint_depth_classic_notch_cap_mm` leaves the
configured 0.1 mm intact here (band slack 1.40885 − 0.79708 = 0.612 mm), so even layers cut at 1.30885 mm.
**Both parities are inside the bound, so nothing was loosened and `mmu_segmented_region_interlocking_depth` was
left at its 0.1 default** rather than zeroed as the brief's fallback allowed.

The residual 8.48 mm² is spec 3.6's own geometry: case B gives the side patch a ws-deep ring at the surface and
then tapers along the corner bisector, so the piece is ws + (D − ws)/√3 = 1.15028 mm deep where the 2D band is
1.40885 mm. 40 × 1.15028 = 46.011 mm² against the measured 45.887 (the 0.124 mm² gap is the taper at the two
±Y ends); the 2D claim tapers at its ends too, at 45°, where the +X edge's Voronoi cell meets the ±Y edges' —
D × (40 − D) = 54.365 against the measured 54.369.

### S4 body areas on layer 98 (print_z 19.8)

| crease_step | body (filament 1) | piece (filament 2) | body / (4 × 40 × ws) |
|---|---|---|---|
| off | **47.6398 mm²** | 1552.36 mm² | 0.374 |
| on | **124.991 mm²** | 1475.01 mm² | **0.980** |

One wall stack ring = 4 × 40 × ws = 127.533 mm². The exact ring of a 40 mm square inset by ws is
4 × 40 × ws − 4ws² = 124.99 mm², which is what "on" measures to five digits — the step lands the piece exactly
one wall stack in from all four side faces. Without it the wedge leaves the body a 0.30 mm ring, under one
outer wall line (0.42 mm), so the top piece's colour would print right out to the side faces on that layer.

Both tables are appended to `.superpowers/sdd/2026-09-01-color-split/spike-report.md` as
"S4 — slice comparison (spec 8.7) and the painted-cube-top wedge (spec 3.6), 2026-09-02" (not committed).

## Files changed

- `C:\Dev\SnapmakerOrcaNext\tests\libslic3r\test_color_split.cpp` — +141 / −6 (committed, `13967943cc`).
- `C:\Dev\SnapmakerOrcaNext\.superpowers\sdd\2026-09-01-color-split\spike-report.md` — S4 section appended (not committed).
- `C:\Dev\SnapmakerOrcaNext\.superpowers\sdd\2026-09-01-color-split\task-8-report.md` — this file (not committed).

No production source was touched; no defect was found that would have called for one.

## Self-review findings (fixed before reporting)

1. **The brief's `surfaces_to_expolygons()` does not exist.** `SurfaceCollection` exposes a plain
   `Surfaces surfaces` member; switched to `for (const Surface &s : lr->slices.surfaces)`, which is what
   `extruder2_claim_for_layer` (test_paint_depth_clamp.cpp:200-215) does.
2. **A vacuous pass was possible.** If the `wall_filament` read were wrong, both areas would be 0 and the
   difference would trivially satisfy the bound. Added `REQUIRE(a2 > 0.)` / `REQUIRE(a3 > 0.)` and the
   `layer_area == 1600 mm²` tiling check.
3. **The S4 case had no assertion tying the two arms together**, so a regression that made both arms behave
   like the step arm would still pass. Added `REQUIRE(body[1] > body[0])`.
4. **`split_in_place` swallowed split warnings.** Added the `INFO` that names them before the
   `REQUIRE(r.warnings.empty())`, so a skipped component reports itself instead of a bare boolean.
5. **The layer grid was not pinned.** `split_test_config` sets `layer_height` but not
   `initial_layer_print_height` (registry default 0.2 — same value, but not guaranteed by the fixture). Pinned
   it in `e2e_config()` so "layer_count − 2" provably means print_z 19.8.
6. **The rebuilt anisotropic fixture had to actually be off-axis in the right way.** The brief's suggested
   `AngleAxisd(1.0, (1,1,1)/√3)` does *not* equalise the column norms (R·e₁ comes out
   (0.694, 0.639, −0.333), giving norms 1.563 / 1.492 / 1.154, so the norm test would still reject it and the
   Gram test still would not be exercised). Equal norms need R·e₁ = (±1,±1,±1)/√3, which
   `FromTwoVectors(UnitX(), (1,1,1)/√3)` gives exactly; verified by the two `WithinRel` assertions in the case.
7. **Spike-report wording.** First draft attributed the 3D piece's end taper to Voronoi bevels; that term
   belongs to the 2D claim. Corrected.

## Concerns

- **None blocking.** The parity bound passes with half its budget spare, on both interlocking parities, so the
  measurement is not sitting on a knife edge.
- The parity case is a single fixture (one vertical wall). Spec 8.7 only claims parity on the vertical-wall
  fixture — slope and top fixtures are checked against the intent, not against the 2D output, which the
  existing `[colorsplit]` geometry cases already do — so this is the specified coverage, not a gap I chose.
- `one_line = 40. * 0.42` restates `split_test_config`'s `outer_wall_line_width` as a literal rather than
  reading it back from the config. That is the brief's own formulation and it is commented; if the config's
  width ever changes, the bound would need updating with it.
- The build slot was shared with another build for the first ~7 minutes; the protocol was followed (checked
  `cl`/`link`/`MSBuild` before every build, waited for the slot to clear, no source edited after the final
  build).
