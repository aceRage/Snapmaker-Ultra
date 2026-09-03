#ifndef slic3r_ObjColorMatch_hpp_
#define slic3r_ObjColorMatch_hpp_

// Headless colour import: decide which filament slot every imported colour belongs to when there
// is nobody at the PC to answer ObjColorDialog.
//
// This is the pure half - clustering plus the mapping policy - deliberately free of wxWidgets so
// it can be unit tested. The GUI half (adding the new slots to the sidebar) lives next to the
// panel's own algorithm in src/slic3r/GUI/ObjColorDialog.cpp.

#include "Color.hpp"

#include <cstddef>
#include <functional>
#include <vector>

namespace Slic3r {

// CONST_FILAMENTS (Model.cpp) has 16 usable slots, and the colour dialog enforces the same limit.
constexpr size_t OBJ_COLOR_MAX_SLOTS = 16;

// How close an imported colour has to be to a loaded filament before we print it with that spool
// instead of asking for a new one. CIE76 dE, so the number means something: two colours a user
// would never load side by side sit under 10 (two oranges: 9.7), a pure red against a loaded dark
// red is 15.9, and genuinely different hues are far above (green vs yellow: 92.7).
constexpr float OBJ_COLOR_MATCH_TOLERANCE = 20.0f;

// CIE76 colour difference between two sRGB colours, components in 0..1.
// NOTE: ObjColorPanel::deal_approximate_match_btn computes the same formula but feeds 0..255 into
// a curve that expects 0..1, so its numbers are not CIE units. That is pre-existing and only ever
// used for "which is nearest", where the distortion is monotonic enough not to matter; this
// function is the correctly scaled one, because a tolerance needs real units to be defensible.
float obj_color_distance(const RGBA &a, const RGBA &b);

struct ObjColorMatchResult
{
    // One 1-based filament slot per input colour, exactly what ObjColorPanel::update_filament_ids
    // would have written had somebody clicked OK.
    std::vector<unsigned char> filament_ids;
    unsigned char              first_extruder_id{1};
    // Slots that do not exist yet, in the order they must be created. The caller adds them.
    std::vector<RGBA>          added_colors;

    size_t input{0};      // input colours
    size_t clusters{0};   // clusters the k-means found
    size_t reused{0};     // clusters that matched a slot within the tolerance
    size_t added{0};      // clusters that got a new slot
    size_t merged{0};     // clusters folded into their nearest slot because all 16 were taken
};

using ObjColorDistanceFn = std::function<float(const RGBA &, const RGBA &)>;

// Cluster `input_colors` the way ObjColorPanel does and map every cluster to a filament slot.
// `existing_colors` are the slots already loaded, in slot order (slot n is existing_colors[n-1]).
// Returns false only when there is nothing to do - no input, or the clustering failed.
bool obj_color_auto_match(const std::vector<RGBA> &input_colors,
                          bool                     is_single_color,
                          const std::vector<RGBA> &existing_colors,
                          ObjColorMatchResult &    out,
                          size_t                   max_slots = OBJ_COLOR_MAX_SLOTS,
                          float                    tolerance = OBJ_COLOR_MATCH_TOLERANCE,
                          ObjColorDistanceFn       distance  = nullptr);

} // namespace Slic3r

#endif /* slic3r_ObjColorMatch_hpp_ */
