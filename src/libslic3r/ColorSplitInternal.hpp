#pragma once
// Declarations shared between the ColorSplit translation units - ColorSplit.cpp (API and pipeline),
// ColorSplitShell.cpp (groups and shell construction) and ColorSplitPartition.cpp (Manifold). Everything the
// GUI and the tests may call lives in ColorSplit.hpp instead; nothing here is public API.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "ColorSplit.hpp"

namespace Slic3r {

class AABBMesh;

namespace ColorSplitDetail {

// The dialog's depth override wins over both the depth AND the unlimited flag. Both entry points have to
// apply it identically - build_color_shells to cut with, split_volume_by_paint to report back - so neither
// gets to spell the rule out for itself.
ColorSplitDepths effective_depths(const ColorSplitDepths &depths, const ColorSplitParams &params);

// Spec 3.4's probe: half the thickness of the part along -dir from `v`, less the 0.002 mm sliver; +inf when
// the probe finds no far side. Shared because spec 3.6's crease rule offsets a boundary vertex along a
// direction of its own, and a depth measured along n(v) says nothing about how much material lies along that
// other direction - it has to re-measure along the one its wall actually travels.
float half_thickness_along(const AABBMesh &aabb, const Vec3f &v, const Vec3f &dir);
// compute_vertex_depths on a tree the caller already holds, so the shell builder and the depth model share one.
// `half_thickness`, when given, receives the RAW t(v)/2 - delta of every vertex, before D is applied: spec
// 3.4a's mitre lengthens a bisector segment past d(v) and needs the clamp on its own to bound it again.
std::vector<float> vertex_depths(const AABBMesh &aabb, const ColorPatches &patches, const std::vector<Vec3f> &normals, double D,
                                 std::vector<float> *half_thickness = nullptr);

} // namespace ColorSplitDetail
} // namespace Slic3r
