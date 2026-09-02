#pragma once
// Declarations shared between the ColorSplit translation units - ColorSplit.cpp (API and pipeline),
// ColorSplitShell.cpp (groups and shell construction) and ColorSplitPartition.cpp (Manifold). Everything the
// GUI and the tests may call lives in ColorSplit.hpp instead; nothing here is public API.
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md
#include "ColorSplit.hpp"

namespace Slic3r {
namespace ColorSplitDetail {

// The dialog's depth override wins over both the depth AND the unlimited flag. Both entry points have to
// apply it identically - build_color_shells to cut with, split_volume_by_paint to report back - so neither
// gets to spell the rule out for itself.
ColorSplitDepths effective_depths(const ColorSplitDepths &depths, const ColorSplitParams &params);

} // namespace ColorSplitDetail
} // namespace Slic3r
