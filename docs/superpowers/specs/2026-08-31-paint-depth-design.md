# Paint Depth — Design Spec

Date: 2026-08-31 · Branch: feat/paint-depth (off ultra-main @ cfa3c43713) · Status: approved
Goal: a general paint-depth scheme so a multicolor object's primary body shows no visible
color bleed from small dark painted features (brown spot embedding brown filament to the
core of a yellow body).

## Root cause (research-verified, code-anchored)

Painted color has NO inward depth bound by default: paint lands on the slice boundary
(PaintedLineVisitor, MultiMaterialSegmentation.cpp:523-589) but extract_colored_segments
(:412-481) hands each painted stretch its entire Voronoi face — to the medial axis /
object core (whole-layer short-circuit at :2149-2151 when one color rings the boundary).
The existing inward clamp cut_segmented_layers (:1145-1169, band diff at :1163) is gated
on mmu_segmented_region_max_width > 0, which DEFAULTS TO 0 = disabled
(PrintConfig.cpp:3863-3870). The claim then overrides wall_filament + solid_infill_filament
+ sparse_infill_filament (PrintApply.cpp:1076-1078) — dark walls AND dark infill to the
center. Bleed paths: (a) dark mass behind ≤wall_loops light perimeters; (b) dark solid
infill within top_shell_layers of light top faces; (c) bare dark/light Z interfaces
(interface_shells defaults false, PrintObject.cpp:1373-1376). Painted/base regions never
share perimeters (is_perimeter_compatible splits on wall_filament, Layer.cpp:179-214).
Precedent for perimeter-aware bounding: fuzzy_skin_segmentation_by_painting passes
max_external_perimeter_width as the bound (:2237-2253).

## User decisions (all approved 2026-08-31)

1. **Bounded BY DEFAULT** — new projects and reslices get depth-bounded paint; legacy
   "unlimited" selectable.
2. **Semantics: mode switch [unlimited | walls | millimeters], global in v1** —
   default "walls: 3"; per-color table is a clean later add.
3. **Interlocking band ON** where depth is bounded — Prusa-style interlocking sub-band
   (existing knob), default ~0.3mm; 0 = plain revert.
4. **Stage 2 IN SCOPE** — vertical-bleed fixes ship with the feature.

## Stage 1 — perimeter-aware depth clamp

- Config (Print Settings > Multimaterial): `paint_depth_mode` coEnum
  [unlimited|walls|millimeters] default walls; `paint_depth_walls` coInt default 3
  (min 1); `paint_depth_mm` coFloat default 1.5. mmu_segmented_region_max_width becomes
  the internal/legacy carrier (walls mode computes mm per object =
  external_perimeter_width + (N-1)*perimeter_spacing from the painted region's flow —
  fuzzy-skin precedent) or is deprecated-aliased via handle_legacy (nonzero legacy value
  → millimeters mode with that value); pick the cleaner form and document.
- Wire into multi_material_segmentation_by_painting (:2197-2234) → existing
  cut_segmented_layers path (:2169-2172). Whole-layer short-circuit (:2149-2151) must
  respect the bound (a fully-ringed layer still clamps to the band).
- Interlocking: mmu_segmented_region_interlocking_depth default 0 → 0.3 (only active
  when depth bounded, as upstream); beam-interlocking mutual exclusion (:2169) unchanged.
- Invalidation joins the existing posSlice keys (PrintObject.cpp:957-972).
- Known upstream caveat to test: thin/small parts where regions merge (PrusaSlicer
  #6892) — cover with a thin-feature fixture; Voronoi-stage bounding is the fenced
  fallback only if this reproduces badly.

## Stage 2 — vertical bleed

- (a) Color Z-interfaces get solid skin: when any bounded painted region exists,
  auto-enable interface_shells behavior for the affected objects OR implement a scoped
  "solid interface at color boundaries" (prefer the smallest correct form; document).
- (b) `paint_infill_override` coBool default true (= today): when false, bounded claims
  keep BASE-color sparse infill (conditional around PrintApply.cpp:1078); solid infill
  stays painted (it can be visible on top surfaces).

## Deferred (fenced)

Per-color depth table; per-stroke depth (gizmo/3MF project — name options so a
per-stroke value can later override); opacity-aware depth; Voronoi-stage bounding.

## Testing

Unit: mode→mm computation (walls math from flow, fuzzy-skin parity); clamp band
geometry via cut_segmented_layers-level tests if instantiable; config
defaults/legacy mapping. Integration: painted-cube fixtures (spot paint on a thick
body) sliced unlimited vs walls:3 — claimed region area shrinks to the band, interior
reverts to base filament in walls AND infill; thin-feature fixture for #6892. GUI:
user paints a dark spot on a light body, checks preview cross-section shows ≤3 dark
walls + base-color interior; then bleed check on a real print.
