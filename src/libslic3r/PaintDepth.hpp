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
// - pdmWalls: see below.
//
// WALLS MODE (fix-wave F3, .superpowers/sdd/2026-08-31-paint-depth/wall-count-
// investigation.md section 5). The band is
//
//     band(N) = N * perimeter_spacing                                   // N full bead pitches
//             + 2 * (ext_perimeter_width - ext_perimeter_spacing)       // undo Arachne's pre-inset
//             + 0.25 * perimeter_spacing                               // count-window margin
//
// replacing the original `ext_perimeter_width + (N-1)*perimeter_spacing`. Why each term:
//
//   - The painted claim is a SEPARATE PrintRegion (its wall_filament differs, so
//     Layer::is_perimeter_compatible - Layer.cpp:184 - never merges it into its parent) and
//     therefore gets its own perimeter generation over the clamped annulus alone
//     (Layer.cpp:257-260). "How many walls does the paint get" is literally "how many beads
//     fit across a strip this wide", so the band has to be sized in bead PITCHES, i.e.
//     N * perimeter_spacing - not "one width plus N-1 spacings".
//   - PerimeterGenerator::process_arachne (PerimeterGenerator.cpp:2231-2256) pre-shrinks the
//     surface by (ext_perimeter_width - ext_perimeter_spacing) on each side before Arachne
//     ever sees it, whenever `precise_outer_wall` is on - which it is by default
//     (PrintConfig.cpp:1222) together with wall_sequence = InnerOuter (:1888). The 2x term
//     hands that back. It is deliberately the worst case: with precise_outer_wall OFF only
//     one such inset is taken, so the band is then one h*(1-pi/4) wider than strictly needed
//     - harmless, and it keeps the band independent of a setting the segmentation stage
//     cannot see per-region.
//   - Without the last term `T = band - 2*(ext_w - ext_s)` lands EXACTLY on N*spacing, the
//     optimal thickness for N beads but only (1 - threshold)*spacing above the count-drop
//     boundary; for ODD N that boundary uses Arachne's `add_thr = min_bead_width/width`
//     (~0.76 at stock settings) rather than `split_thr`, leaving just 0.083mm at 0.45mm
//     lines / 0.1mm layers - which anything at all (precise_outer_wall, a narrower line
//     width, the interlocking notch) then eats. 0.25*spacing re-centres T in the window:
//     0.212mm of downward margin at N=3, ~2.5x today's, and parity-independent.
//
// `walls` is clamped to >= 1 (the paint_depth_walls option's own min) so a caller passing 0
// or a negative count still gets a sane one-wall-wide band rather than a negative one, and
// the result is clamped to >= 0 so degenerate flows (all-zero widths, as in the unit tests)
// collapse the band to "disabled" instead of going negative.
//
// NOT fixed here, and deliberately: LimitedBeadingStrategy caps Arachne at
// `max_bead_count = 2 * wall_loops` beads (WallToolPaths.cpp:514,
// LimitedBeadingStrategy.cpp:41-64), so at the stock `wall_loops = 2` a painted region can
// never exceed 4 loops however wide this band is. Band beyond that ceiling is not lost - it
// becomes painted solid/sparse infill inside the same painted region - but it is not extra
// LOOPS. See the fix-wave report.
float paint_depth_band_mm(PaintDepthMode mode, int walls, double mm,
                           float ext_perimeter_width, float ext_perimeter_spacing,
                           float perimeter_spacing);

// WAVE A / item 8 (.superpowers/sdd/2026-08-31-paint-depth/classic-generator-investigation.md
// sections 2b, 2c, 3 and 6): the CLASSIC wall generator's floor for the band above. Apply it only
// when wall_generator == Classic; the caller owns that branch.
//
// paint_depth_band_mm is shaped for Arachne's bead-count windows. Classic tiles whatever width it
// is given, so the 0.25*spacing margin and the 2*(ext_w - ext_s) pre-inset are harmless there -
// they only widen the middle gap-fill line. The one thing classic needs that Arachne does not is a
// MINIMUM, because PerimeterGenerator::process_classic's onion offset on a strip always returns
// BOTH of the strip's boundaries: a painted band can only ever hold an even number of
// external-width loops (plus at most one gap-fill line), never one. The narrowest band classic can
// render honestly is therefore two properly-spaced lines,
//
//     ext_perimeter_width + ext_perimeter_spacing        (= 0.878540 at 0.45mm lines / 0.1mm layers)
//
// which is exactly the `wall_stack` fix-wave F1 insets its top/bottom claim by
// (MultiMaterialSegmentation.cpp). Below that, three separate real defects appear at
// paint_depth_walls = 1 (band 0.578595, the only band under the floor at stock flows):
//
//   1. The two depth-0 external loops sit 0.128595mm apart centre-to-centre while each is 0.45mm
//      wide - 30% of normal packing, i.e. +48% over-extrusion along the whole painted boundary,
//      knowingly uncompensated (PerimeterGenerator.cpp:1419's "FIXME evaluate the overlaps").
//   2. band(1) = 1.25*s + 2h(1-pi/4) is independent of ext_perimeter_width while classic's
//      survival threshold IS ext_perimeter_width, so any profile with a wide enough outer wall
//      (e.g. outer 0.6 / inner 0.42 at 0.1mm => band(1) = 0.541 < 0.6) loses N=1 paint entirely:
//      zero extrusions on every layer. Arachne rescues the same strip with WideningBeadingStrategy.
//   3. Worst: F1 insets the top/bottom claim by one wall_stack, and that claim is unioned with -
//      not clamped by - the lateral band. Wherever band < wall_stack the union leaves the BASE
//      region holding a closed ring of width wall_stack - band (0.299945mm at N=1) on every
//      sub-surface shell layer, and classic prints that ring as NOTHING: offset_ex comes back
//      empty at i = 0, `last` is cleared, and gaps are only collected from i >= 1. A genuine void
//      ring under every painted top/bottom face. Flooring band(1) AT wall_stack closes it by
//      construction - the lateral band and the F1 inset become the same number and meet exactly.
//
// Why it must NOT be unconditional: Arachne's 1 -> 2 bead boundary is T > (1 + split_thr)*ext_s =
// 0.647570 (RedistributeBeadingStrategy.cpp:42-48, split_thr = 2*min_bead_width/ext_w - 1).
// Today's band(1) gives T = 0.535675 -> 1 bead; a floored band(1) gives T = 0.835620 -> 2 beads,
// breaking the "1 wall means 1 loop" contract F3 established. Hence the caller's branch.
//
// A non-positive band means "disabled" (unlimited mode, or paint_depth_mm = 0) and is returned
// untouched - flooring it would silently switch the clamp back on.
float paint_depth_band_classic_floor_mm(float band, float ext_perimeter_width, float ext_perimeter_spacing);

// Fix-wave F4 (.superpowers/sdd/2026-08-31-paint-depth/wall-count-investigation.md section 3):
// the EFFECTIVE interlocking depth cut_segmented_layers may use, given the region flow the
// band above was sized against.
//
// cut_segmented_layers narrows the clamp band by mmu_segmented_region_interlocking_depth on
// every even-indexed layer (MultiMaterialSegmentation.cpp:1164, :1169) to carve a mechanical
// interlocking tooth. That is a wanted feature - but the shipped 0.3mm default is ~0.70 *
// perimeter_spacing, far more than the band's whole count-window margin, so it dropped the
// painted region from N loops to N-1 on every even layer: the user-visible "clearly only 1-2
// walls" 3/2/3/2 alternation. Widening the band to compensate cannot work (the investigation
// shows it just turns 3/2 into 4/3, because 0.3mm is most of a full count window).
//
// So the notch is clamped here to a quarter of one perimeter spacing - exactly the
// count-window margin band() above builds in, so the notch is guaranteed to fit INSIDE that
// margin and can never move T across a bead-count boundary. Both parities then land in the
// same count window: N walls means N walls on every layer. The config default is lowered to
// 0.1mm to match (PrintConfig.cpp), so the clamp is normally inert and only bites for users
// who raise it by hand.
//
// WAVE A / I-3 (.superpowers/sdd/2026-08-31-paint-depth/bleed-and-walls-fixwave-review.md):
// the cap applies in WALLS MODE ONLY, and takes the mode as an argument so that contract lives
// in one place. The whole derivation above is "the notch must fit inside the count-window margin
// that band(N) builds in, so Arachne still delivers N loops on both parities". In
// pdmMillimeters there is no N: the band is the user's literal paint_depth_mm (see
// paint_depth_band_mm above - "wall count and flow widths are ignored entirely, an explicit
// user-chosen depth"), it is not sized to a bead count, and no wall-count contract is being
// protected. Capping there contradicted the mode's own documented contract and silently cut a
// hand-set 0.5mm mechanical key down to 0.107mm; PrintConfig.cpp's tooltip already justifies the
// cap by "Paint depth walls" alone. pdmUnlimited never reaches this helper (the call site gates
// on it) but is handled the same way for totality.
//
// The shipped default (0.1mm) is below the cap at any realistic flow, so this changes nothing
// for anyone who has not deliberately raised the notch by hand.
//
// A non-positive perimeter_spacing carries no information to clamp against (degenerate flow),
// so the configured value is returned verbatim; a non-positive configured depth is returned
// as-is too (0 = feature disabled, the option's own documented convention).
float paint_depth_interlocking_depth_mm(PaintDepthMode mode, double configured_depth, float perimeter_spacing);

} // namespace Slic3r

#endif // slic3r_PaintDepth_hpp_
