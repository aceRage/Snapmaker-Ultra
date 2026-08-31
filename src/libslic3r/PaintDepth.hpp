#ifndef slic3r_PaintDepth_hpp_
#define slic3r_PaintDepth_hpp_

namespace Slic3r {

// Paint Depth Stage 1 (docs/superpowers/specs/2026-08-31-paint-depth-design.md,
// docs/superpowers/plans/2026-08-31-paint-depth.md Task 1 item 1): how far a painted
// color claim is allowed to bleed inward from the sliced boundary before reverting to
// the object's base filament. Feeds the existing cut_segmented_layers band clamp
// (MultiMaterialSegmentation.cpp) via paint_depth_band_mm below - that clamp used to be
// driven directly by mmu_segmented_region_max_width, which defaulted to 0 (disabled);
// this mode switch is what makes the feature bounded BY DEFAULT (spec decision 1/2).
//
// pdmUnlimited (0) is deliberately the enum's zero value so it lines up with the old
// "0 = disabled" convention mmu_segmented_region_max_width used - a default-constructed
// or legacy-zero-derived mode reads as "no clamp", matching prior behavior for anyone
// who explicitly asks for it (handle_legacy_composite in PrintConfig.cpp maps a legacy
// mmu_segmented_region_max_width of exactly 0 to defaults, i.e. pdmWalls - see that
// function's own comment for why 0 does NOT map to pdmUnlimited despite the enum-value
// coincidence: the approved bounded-by-default flip applies to every project, not just
// new ones, so a legacy zero must land on the new default, not carry "disabled" forward).
enum PaintDepthMode {
    pdmUnlimited = 0,
    pdmWalls,
    pdmMillimeters,
};

// Pure helper (plan Task 1 item 4): the band width (mm) fed to cut_segmented_layers's
// max_width parameter, where 0.f means "disabled" (mirrors the pre-existing
// mmu_segmented_region_max_width convention so callers/tests can treat the two
// consistently).
//
// - pdmUnlimited: always 0.f - the clamp stays off, byte-identical to today's default-0
//   mmu_segmented_region_max_width behavior.
// - pdmMillimeters: `mm` verbatim (cast to float) - wall count and flow widths are
//   ignored entirely, an explicit user-chosen depth.
// - pdmWalls: ext_perimeter_width + (walls-1)*perimeter_spacing - the fuzzy-skin
//   precedent (fuzzy_skin_segmentation_by_painting, MultiMaterialSegmentation.cpp:
//   2237-2253, "limit the depth ... by the maximal extrusion width of external
//   perimeters") extended to N walls: the first (outermost) wall is the full external
//   perimeter width, every additional wall after that adds one perimeter_spacing - the
//   same width/spacing relationship consecutive walls already have in Arachne/classic
//   wall generation. `walls` is clamped to >= 1 (the paint_depth_walls option's own
//   min) so a caller passing 0 or a negative count still gets a sane one-wall-wide band
//   rather than a negative/degenerate one.
float paint_depth_band_mm(PaintDepthMode mode, int walls, double mm,
                           float ext_perimeter_width, float perimeter_spacing);

} // namespace Slic3r

#endif // slic3r_PaintDepth_hpp_
