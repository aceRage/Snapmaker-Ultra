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
class ModelVolume;

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

// The split's optional refinements; the dialog owns them. flat_cap gives an up- or down-facing flat group
// with a core at least three wall stacks wide the solid-shell depth instead of D (spec 3.5). crease_step adds
// spec 3.6's intermediate ring vertex to every boundary vertex of a group: at the two CONVEX crease cases it
// is a real step - a painted top stays one wall stack clear of the side faces below its surface layer, and a
// painted side keeps its full wall stack up to the top edge - and at a plain boundary it just subdivides the
// side one layer down. Spec 3.6's concave and same-state creases are always on, whatever crease_step says.
struct ColorSplitParams {
    bool   flat_cap          = true;   // spec 3.5
    bool   absorb_islands    = true;   // spec 3.8
    bool   crease_step       = true;   // spec 3.6
    double depth_override_mm = 0.;     // <= 0: use depths.D
};

// Spec 3.7 / 3.1a (Ruling 18): one closed, inward-offset shell per SMOOTH PATCH of one painted
// state - facets connect only across edges whose dihedral angle is under 30 degrees, so a boss's
// side and top cap are two shells whose claims may overlap; spec 3.8 settles the overlap.
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

// Spec 3.9: which space the split runs in. D, ws and h are WORLD millimetres, the volume's mesh is not.
// Let T = instance x volume matrix. An isotropic T (scale s, any rotation, mirror allowed) needs no transform
// at all - the split runs on the mesh as it stands with the depths divided by s, which is exact and shared by
// every instance of that scale. An anisotropic T has no single depth scale, so the mesh is carried into world
// space by `to_split` and the pieces come back through `from_split`.
struct ColorSplitSpace {
    Transform3d to_split    = Transform3d::Identity();   // mesh -> split space (identity on the mesh-space path)
    Transform3d from_split  = Transform3d::Identity();   // split space -> mesh
    double      depth_scale = 1.;                        // divide world depths by this on the mesh-space path
    bool        world_path  = false;
};
ColorSplitSpace color_split_space(const ModelObject &object, const ModelVolume &volume);
// World depths -> split-space depths: D, ws, both caps and the layer height all divide by `s`.
ColorSplitDepths scale_depths(const ColorSplitDepths &depths, double s);

// Spec 4: replace the painted source volume by the split's output, in its own slot - body first (unless it is
// empty), then one MODEL_PART per filament ascending. Every output is a NEW volume, so undo/redo sees fresh
// ObjectIDs; the source is deleted. `solid_interfaces` sets the object's `interface_shells`;
// `keep_base_sparse_infill` pins each colour part's sparse infill to the body's filament. The result is
// consumed (its meshes are moved into the model). Returns the created volumes, empty if there was nothing to
// create - in which case the object is left untouched.
std::vector<ModelVolume *> apply_color_split(ModelObject &object, size_t source_volume_idx, ColorSplitResult &&result,
                                             const ColorSplitSpace &space, bool solid_interfaces, bool keep_base_sparse_infill);

} // namespace Slic3r
