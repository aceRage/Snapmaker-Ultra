#ifndef slic3r_ImageRowWalls_hpp_
#define slic3r_ImageRowWalls_hpp_

// ImageMap Phase 4, step 1: side-wall dithering.
//
// Spec: docs/superpowers/specs/2026-09-12-imagemap-phase4-walls.md.
// Companion: docs/superpowers/specs/2026-09-07-imagemap-phase3-imagerow.md (phase 3, which did
// the same thing for TOP SOLID INFILL and built every primitive this file reuses).
//
// WHAT THIS ADDS. Phase 3 sampled the image along a top surface's fill lines and cut them into
// short per-filament runs. An image mapped on a VERTICAL face (a box or wrapped projection) had
// nowhere to go: walls kept the region's nominal filament, so the picture existed on the top and
// stopped at the edge. This file extends the same sampling, the same dither and the same
// per-run override to the OUTER PERIMETER loop (and, when the row asks for it, the first inner
// perimeter, so a colour run is two lines wide and actually opaque from outside).
//
// WHY THIS LIVES AT G-CODE TIME, NOT IN PerimeterGenerator. A wall is a CLOSED LOOP whose start
// point (the seam) is chosen at G-code time by SeamPlacer::place_seam() (GCode.cpp:7599), long
// after PerimeterGenerator has run. Splitting the loop earlier would pre-empt that choice and
// make the first run boundary the de facto seam - visible as a scar on every layer, and the one
// thing the brief explicitly forbids ("keep the loop's seam position; runs are just colour
// boundaries"). So the split happens inside GCode.cpp's existing per-island PERIMETERS block,
// AFTER the seam placer has rotated the loop, exactly the way the local-Z clipper already does it
// (clip_extrusion_collection_for_local_z + LocalZLoopSeamPlacer, GCode.cpp:4345/6068). Sampling
// after PerimeterGenerator also means sampling AFTER fuzzy skin, which is applied inside the
// generator (Feature/FuzzySkin/FuzzySkin.cpp, called from PerimeterGenerator.cpp:234 and :498) -
// so run boundaries are placed on the jittered geometry the nozzle actually follows, which is
// what "run boundaries must survive fuzzy jitter" requires.
//
// COORDINATE SPACES. Identical to phase 3's (see Fill.cpp's own header comment): perimeter
// polylines live in the PrintObject's working space; image_fill_project() wants the volume's own
// MESH space, so a point is mapped through (trafo_centered() * volume->get_matrix()).inverse().
// The one genuinely NEW piece of geometry here is the normal: a top surface hands
// image_fill_project() a synthesised "up", but a wall's Box projection must pick the box FACE the
// wall belongs to, so each sample point needs the wall's OWN outward normal. That is computed
// per sample from the loop's local tangent (perpendicular in XY, oriented outward using the
// loop's counter-clockwise winding, which PerimeterGenerator guarantees via
// make_counter_clockwise()), then carried into mesh space by the same transform.
//
// WHAT IS NOT COVERED. Same uniform-scale caveat as phase 3: arc length is treated as equal in
// mesh and print space. Inner perimeters beyond the first are never split (they are invisible).
// A wall whose projection declines a colour everywhere (a cylindrical projection exactly on the
// axis) is left whole, printing with the region's nominal filament exactly as before.

#include <memory>
#include <vector>

#include "ExtrusionEntity.hpp"
#include "ExtrusionEntityCollection.hpp"
#include "ImageFill.hpp"
#include "Point.hpp"

namespace Slic3r {

class PrintObject;
class PrintRegion;
class MixedFilament;

// Everything one region's wall split needs, built once per (layer, region) rather than per loop.
// Mirrors Fill.cpp's ImageRowContext field for field, plus `split_first_inner`.
struct ImageRowWallContext
{
    const MixedFilament             *row = nullptr;
    ImageFillParams                  params;
    std::vector<int>                 candidate_ids;     // physical, 1-based
    std::vector<std::array<float,3>> candidate_colors;  // parallel to candidate_ids, sRGB 0..1
    BoundingBoxf3                    mesh_box;
    Transform3d                      mesh_from_print = Transform3d::Identity();
    float                            min_run_len_mm    = 0.4f;
    float                            sample_spacing_mm = 0.4f;
    // Whether the first INNER perimeter (inset_idx == 1) is split too. A single outer line is
    // often translucent enough that the layer below shows through and the dither reads muddy;
    // backing it with the same colour doubles the opacity for one extra tool-path's worth of
    // travel and no extra tool changes (the runs are grouped per filament per LAYER, so an inner
    // run of filament F costs nothing once F is already being visited for the outer runs).
    bool                             split_first_inner = false;
    // Phase 4, step 4 (Arachne/Concentric TOP surfaces reusing this cutter): when set, every
    // sample uses THIS normal instead of the per-segment wall normal computed from the loop's
    // tangent. A top surface is one plane, so its facet normal does not vary along a path - and
    // the wall normal (horizontal, perpendicular to travel) would be flatly wrong there, making a
    // Box projection pick a side face for a surface that faces up. Left unset (all zero) for
    // walls, which is what selects the per-segment computation.
    Vec3f                            fixed_normal_mesh{0.f, 0.f, 0.f};
};

// Does this region's WALL filament name an enabled ImageWeighted row with a usable image?
// Returns the configured (pre-resolution) 1-based virtual id, or 0. Reads outer_wall_filament
// when the region sets one (> 0) and wall_filament otherwise - the same precedence
// PrintRegion::extruder() itself applies (PrintRegion.cpp:131).
//
// Returns 0 for every region of every print that does not use this feature, which is what keeps
// the whole of this file off the hot path and Bar A byte-identical.
unsigned int image_row_wall_configured_virtual_id(const PrintObject &object, const PrintRegion &region);

// The set of PHYSICAL 1-based filaments an image-row wall on this region can possibly print
// with - i.e. the row's candidate list, already filtered to ids that exist and have a colour.
// Empty when this region is not an image-row wall at all.
//
// ToolOrdering needs this because the wall split happens at G-CODE time, after ToolOrdering has
// already built each layer's tool list: it cannot read the per-run tags (they do not exist yet),
// so it registers the whole candidate set instead. See the call site's own comment in
// ToolOrdering.cpp for why over-registering is harmless.
std::vector<unsigned int> image_row_wall_candidate_filaments(const PrintObject &object, const PrintRegion &region,
                                                             size_t num_physical);

// Builds the full context. false means "not an image row here" (row missing/disabled/undecodable,
// fewer than two usable candidate filaments, or no owning model-part volume) and leaves `ctx`
// untouched; the caller then leaves the walls exactly as they were.
// `flow_width_mm` is the external perimeter's extrusion width - it sets both the sample spacing
// and the minimum run length, so the resolution is nozzle-bounded rather than arbitrary.
bool image_row_wall_context_for_region(const PrintObject &object, const PrintRegion &region,
                                       float flow_width_mm, ImageRowWallContext &ctx);

// Phase 4, step 3 (IRONING). Builds a context for an IRONING pass over an image row. Ironing
// follows the region's solid_infill_filament (Layer::make_ironing sets ironing_params.extruder
// from it), so unlike the wall context this one reads that key, not wall_filament - it is the
// SAME row the top solid infill beneath the ironed skin is using, which is the point: the cover
// pass must reproduce the picture it covers rather than hide it.
//
// The normal is the surface's own "up" (ironing is always a top surface), supplied via
// fixed_normal_mesh, so a Box projection picks the top face.
//
// false means "this region's ironing is not an image row" and leaves `ctx` untouched.
bool image_row_wall_iron_context(const PrintObject &object, const PrintRegion &region,
                                 float flow_width_mm, ImageRowWallContext &ctx);

// Splits ONE already-seam-placed perimeter entity into per-run pieces.
//
// `entity` must be an ExtrusionLoop, ExtrusionMultiPath or ExtrusionPath whose geometry is final
// (fuzz applied, seam rotated into place). The loop is walked from its current start point - i.e.
// from the seam - so run boundaries fall wherever the image says and the seam stays where
// SeamPlacer put it.
//
// Returns one entry per run, in print order, each a heap ExtrusionEntityCollection holding that
// run's geometry and tagged with `image_row_extruder_1based`. Returns EMPTY when the entity
// should be left alone (not splittable, fewer than two runs, or the projection declined a colour
// along the whole loop) - the caller then keeps the original entity untouched.
//
// A split loop necessarily stops being a closed ExtrusionLoop: each run becomes an
// ExtrusionMultiPath, because there is no such thing as a partially-coloured loop. The pieces
// still describe exactly the original geometry end to end, in order, starting at the seam, so
// the nozzle follows the same path it would have followed - it just changes filament part-way
// round. This is the same trade the local-Z clipper already makes (GCode.cpp:4375-4385) and it
// costs the loop's seam-gap clipping and scarf joint, which only apply on the ExtrusionLoop
// branch of GCode::extrude_loop().
std::vector<std::unique_ptr<ExtrusionEntityCollection>> image_row_split_wall_entity(
    const ImageAssetStore &assets, const ImageRowWallContext &ctx, const ExtrusionEntity &entity, double print_z);

// True when this entity is a wall the split should claim: the outer loop always, the first inner
// perimeter only when `ctx.split_first_inner`. Uses `inset_idx` (PerimeterGenerator sets it on
// every loop it emits) and falls back to the erExternalPerimeter role for an entity whose
// inset_idx was never set, exactly as GCode.cpp's own outer/inner wall splitter does.
bool image_row_wall_entity_is_claimed(const ExtrusionEntity &entity, bool split_first_inner);

} // namespace Slic3r

#endif // slic3r_ImageRowWalls_hpp_
