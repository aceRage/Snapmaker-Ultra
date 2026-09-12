#ifndef slic3r_SliceBake_hpp_
#define slic3r_SliceBake_hpp_

// Slice baking, phase 1 ("Exact layers" mode).
//
// Turns a sliced PrintObject's PRINTED APPEARANCE - the outer wall as it will actually be
// extruded, fuzzy skin and all - into a watertight mesh that can be re-sliced. The driving
// scenario is texture transplanting: slice at 0.3 mm WITH fuzzy skin, bake, then re-slice the
// bake at 0.12 mm WITHOUT fuzzy skin and get the same texture printed at a finer layer height.
//
// What is baked: LayerRegion::perimeters, outer loops only (erExternalPerimeter, plus the
// overhang/over-support variants, which are the same physical outer wall split by role), from
// every region of every layer, each loop's polyline offset outward by its own width/2 and
// unioned into one filled ExPolygons per layer. The result is SOLID - the bake is a re-sliceable
// object, not a hollow replica - so top and bottom surfaces come for free from the filled layers
// and the loft's own caps.
//
// What is excluded, by construction rather than by filtering: supports and support interface
// (they live in SupportLayer, never touched), brim and skirt (separate collections on Print /
// PrintObject), the wipe tower (a separate structure on Print), and infill and inner perimeters
// (never read - everything radially inward of the outer loop is filled in anyway).
//
// The loft itself is the existing Slic3r::slices_to_mesh (SlicesToTriangleMesh.cpp), which is
// not SLA-specific despite its only current caller being Format/SL1.cpp.
//
// Deterministic: the layer loop is ordered, the per-layer union is Clipper's (itself
// deterministic for a fixed input ordering), and nothing is hashed or threaded across layers in
// a way that could reorder the output.
//
// Spec: docs/superpowers/specs/2026-09-12-slice-bake-research.md (phase 1).

#include "ExPolygon.hpp"
#include "Point.hpp"
#include "TriangleMesh.hpp"

#include <cstddef>
#include <functional>
#include <string>
#include <vector>

namespace Slic3r {

class PrintObject;
class Layer;

// A "close small gaps" radius of 0 - the default - means no closing at all, not "close by
// nothing": the morphological close is skipped entirely so the layer polygons come out of the
// union bit-for-bit as Clipper produced them.
static constexpr double SLICE_BAKE_CLOSE_GAPS_MAX = 2.0; // mm

struct SliceBakeOptions
{
    // Layer subset, as half-open [first, last) indices into PrintObject::layers(). The default
    // (0, max) means every layer. Phase 1 only ever passes the default from the GUI; the
    // parameter exists because the tests and a later range picker both want it.
    size_t layer_begin = 0;
    size_t layer_end   = std::numeric_limits<size_t>::max();

    // Optional per-layer morphological closing radius in mm (offset out then back in). Bridges
    // the hairline gaps a fuzzed wall can leave between the outward offsets of two neighbouring
    // loops, at the cost of rounding off features finer than the radius. 0 = off.
    double close_gaps_radius = 0.;

    // Undo the object's placement transform so the baked mesh sits where the ModelObject's mesh
    // sat - i.e. the bake can REPLACE the object in place. Off gives the mesh in print
    // coordinates (centred on the plate), which is what an "export exactly what was sliced"
    // consumer wants.
    bool in_object_frame = true;
};

struct SliceBakeReport
{
    size_t layers_baked    = 0;
    size_t loops_used      = 0;   // outer-wall loops that contributed
    size_t triangles       = 0;
    size_t vertices        = 0;
    size_t empty_layers    = 0;   // layers with no outer wall at all (skipped)
    double z_min           = 0.;  // in the frame of the returned mesh
    double z_max           = 0.;
    bool   watertight      = false;
    std::string note;
};

// Thrown when the caller's progress callback asks to stop. Mirrors ColorSplitCancelled.
class SliceBakeCancelled : public std::exception
{
public:
    const char *what() const noexcept override { return "Slice bake cancelled"; }
};

// percent in 0..100; return false to cancel (which raises SliceBakeCancelled out of the bake).
using SliceBakeProgress = std::function<bool(int)>;

// The per-layer outer-wall footprints, in PRINT coordinates (the object's own sliced frame,
// unscaled millimetres are recovered by unscaled()). One entry per baked layer, in Z order;
// `out_z` receives each entry's print_z and `out_bottom_z` its bottom_z, so the caller can loft
// with the object's real (possibly variable) layer heights rather than assuming a constant one.
//
// Layers whose outer wall is empty are dropped from all three vectors together.
std::vector<ExPolygons> slice_bake_layer_regions(const PrintObject      &object,
                                                 const SliceBakeOptions &opts,
                                                 std::vector<double>    *out_z,
                                                 std::vector<double>    *out_bottom_z,
                                                 SliceBakeReport        *report   = nullptr,
                                                 const SliceBakeProgress &progress = {});

// The whole phase-1 bake: outer-wall footprints -> loft -> mesh.
//
// The returned mesh is positioned per opts.in_object_frame. An empty result (no indices) means
// the object had no bakeable outer wall; `report->note` says why.
indexed_triangle_set slice_bake_to_mesh(const PrintObject       &object,
                                        const SliceBakeOptions  &opts,
                                        SliceBakeReport         *report   = nullptr,
                                        const SliceBakeProgress &progress = {});

// Does this object have anything to bake? Cheap - stops at the first outer-wall loop it finds.
// The GUI uses it to enable/disable the menu item, alongside the plate's sliced state.
bool slice_bake_available(const PrintObject &object);

// A rough triangle count for the dialog's "this will be big" line, without running the bake.
// The loft emits, per layer, two triangles per boundary point for the wall strip plus the
// free-top/overhang caps; two per point is the term that dominates, so this counts the outer-wall
// points of every layer in the subset and doubles it. Labelled as an estimate wherever shown.
size_t slice_bake_estimate_triangles(const PrintObject &object, const SliceBakeOptions &opts);

} // namespace Slic3r

#endif // slic3r_SliceBake_hpp_
