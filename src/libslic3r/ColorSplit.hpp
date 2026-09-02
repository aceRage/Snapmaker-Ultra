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

} // namespace Slic3r
