#pragma once
// Split by painted colour: MMU paint -> in-place solid parts.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "libslic3r.h"
#include "TriangleMesh.hpp"
#include "TriangleSelector.hpp"
#include "PaintDepth.hpp"
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

namespace Slic3r {

class DynamicPrintConfig;
class ModelObject;

class ColorSplitError : public std::runtime_error { public: using std::runtime_error::runtime_error; };
class ColorSplitCancelled : public ColorSplitError { public: ColorSplitCancelled() : ColorSplitError("cancelled") {} };

// Progress callback: percent 0..100; return false to cancel.
using ColorSplitProgress = std::function<bool(int)>;

// Spec 3.1: the conforming, T-joint-free retriangulation F of the volume surface with a state per facet.
struct ColorPatches {
    indexed_triangle_set surface;     // welded; zero open edges
    std::vector<int>     facet_state; // per triangle of `surface`: 0 = unpainted, else 1-based filament id
    std::vector<int>     states;      // ascending painted states present (>= 1)
};
ColorPatches extract_color_patches(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint);

// Spec 3.3/3.5: world-mm depth model derived from the part's effective config.
struct ColorSplitDepths {
    double D            = 0.;   // normal depth; ignored when unlimited
    double ws           = 0.;   // wall stack = external width + external spacing
    double cap_top      = 0.;   // capped-group depth for up-facing flats (>= layer_height)
    double cap_bottom   = 0.;   // same for down-facing flats
    double layer_height = 0.;
    bool   unlimited    = false;
};
// `filaments` = 1-based ids whose nozzle/flow take part (body extruder + painted filaments); the widest wins.
ColorSplitDepths color_split_depths(const DynamicPrintConfig &effective, const std::vector<int> &filaments);

// Spec 3.2: angle-weighted vertex normals of the full surface F.
std::vector<Vec3f> color_split_normals(const indexed_triangle_set &surface);
// Spec 3.4 (rev 2.2): d(v) = min(D, t(v)/2 - delta), delta = 0.002 mm, t(v) = thickness along -n(v). D may be +inf (unlimited).
std::vector<float> compute_vertex_depths(const ColorPatches &patches, const std::vector<Vec3f> &normals, double D);

// The split's optional refinements; the dialog owns them. Spec 3.5 (flat_cap) and 3.6 (crease_step) are not
// built yet - the shell builder currently behaves as if both were off.
struct ColorSplitParams {
    bool   flat_cap          = true;   // spec 3.5
    bool   absorb_islands    = true;   // spec 3.8
    bool   crease_step       = true;   // spec 3.6
    double depth_override_mm = 0.;     // <= 0: use depths.D
};

// Spec 3.1a (Ruling 13): a flat (linear) refinement of F so that no edge exceeds `max_edge_mm`; each refined
// facet takes the state of the original facet it lies on. STL cylinders, pins and bosses carry only two
// vertex rings, so without interior vertices every side normal is a junction bisector and no inward offset is
// ever radial - a painted boss came out as a cup. Returns `patches` untouched when no edge is longer.
ColorPatches refine_color_patches(const ColorPatches &patches, double max_edge_mm);
// The length that pre-pass refines to: max(ws, min(D_eff, mesh diagonal / 20)) - fine enough that a feature
// gets interior vertices, never finer than one wall stack, and never more than a twentieth of the part.
double color_split_refine_length(const ColorSplitDepths &depths, const ColorSplitParams &params, const BoundingBoxf3 &mesh_bbox);

// Spec 3.7: one closed, inward-offset shell per edge-connected component of one painted state.
struct ColorShell { int state = 0; bool capped = false; indexed_triangle_set mesh; };
// Validity of a shell: closed (no open edge), free of self-intersections, and its signed volume.
struct ShellCheck { bool closed = false; bool self_intersects = true; double volume = 0.; };
ShellCheck check_shell(const indexed_triangle_set &shell);
// Spec 7 (rev 2.3): a component that cannot carry a valid shell even at its floor depth is skipped, not an
// error - the body keeps that feature in its own colour. `warnings`, when given, collects one note per skip.
std::vector<ColorShell> build_color_shells(const ColorPatches &patches, const ColorSplitDepths &depths,
                                           const ColorSplitParams &params, const ColorSplitProgress &progress,
                                           std::vector<std::string> *warnings = nullptr);

// Spec 3.8: what one split produces. `body` is the unpainted remainder (may be empty); `pieces` holds one
// merged mesh per painted filament, ascending; `warnings` are the user-facing notes collected on the way.
struct ColorSplitResult {
    indexed_triangle_set                              body;      // may be empty
    std::vector<std::pair<int, indexed_triangle_set>> pieces;    // (filament, mesh), ascending filament
    std::vector<std::string>                          warnings;
    ColorSplitDepths                                  depths;
};
// Spec 3.8: rest <- mesh; each shell is cut out of `rest` in turn, so pieces and body are complementary by
// construction and overlaps go to the lower filament (the shell order build_color_shells returns).
ColorSplitResult partition_by_shells(const indexed_triangle_set &mesh, const std::vector<ColorShell> &shells,
                                     bool absorb_islands, const ColorSplitProgress &progress);
// The one-shot entry point: paint -> patches -> shells -> partition.
ColorSplitResult split_volume_by_paint(const indexed_triangle_set &mesh, const TriangleSelector::TriangleSplittingData &paint,
                                       const ColorSplitDepths &depths, const ColorSplitParams &params, const ColorSplitProgress &progress);

} // namespace Slic3r
