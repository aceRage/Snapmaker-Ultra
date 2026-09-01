#ifndef slic3r_MultiMaterialSegmentation_hpp_
#define slic3r_MultiMaterialSegmentation_hpp_

#include <utility>
#include <vector>

namespace Slic3r {

class ExPolygon;
class ModelVolume;
class PrintObject;
class FacetsAnnotation;

using ExPolygons = std::vector<ExPolygon>;

struct ColoredLine
{
    Line line;
    int  color;
    int  poly_idx       = -1;
    int  local_line_idx = -1;
};

using ColoredLines = std::vector<ColoredLine>;

enum class IncludeTopAndBottomLayers {
    Yes,
    No
};

struct ModelVolumeFacetsInfo {
    const FacetsAnnotation &facets_annotation;
    // Indicate if model volume is painted.
    const bool              is_painted;
    // Indicate if the default extruder (TriangleStateType::NONE) should be replaced with the volume extruder.
    const bool              replace_default_extruder;
};

// Returns segmentation based on painting in segmentation gizmos.
std::vector<std::vector<ExPolygons>> segmentation_by_painting(const PrintObject                                               &print_object,
                                                              const std::function<ModelVolumeFacetsInfo(const ModelVolume &)> &extract_facets_info,
                                                              size_t                                                           num_facets_states,
                                                              float                                                            segmentation_max_width,
                                                              float                                                            segmentation_interlocking_depth,
                                                              // Wave A / C-1: the narrowest painted claim the band clamp's
                                                              // degradation ladder may emit (mm) - one external extrusion.
                                                              // Below it the clamp reverts to its pre-degradation no-op rather
                                                              // than cutting a sub-extrusion strip no toolpath can honour. See
                                                              // paint_depth_clamp_keep_core in the .cpp.
                                                              float                                                            segmentation_min_claim_width,
                                                              // WAVE B / Option N (.superpowers/sdd/2026-08-31-paint-depth/
                                                              // curved-gap-design.md): the painted claim's thickness measured
                                                              // NORMAL to the painted surface (mm), which bounds the top/bottom
                                                              // descent instead of top_shell_layers / bottom_shell_layers. 0
                                                              // means "no normal-thickness shell" - unlimited mode, and the fuzzy
                                                              // skin path, which has no top/bottom claim at all. See
                                                              // segmentation_top_and_bottom_layers in the .cpp.
                                                              float                                                            segmentation_normal_depth,
                                                              // ITEM 1 (.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-
                                                              // report.md, interclaim-sliver-investigation.md section 5 Option 1):
                                                              // the F1 wall-stack band width (mm, ext_perimeter_width +
                                                              // ext_perimeter_spacing, max across the object's printing regions),
                                                              // plumbed through to merge_segmented_layers's interior inter-claim
                                                              // absorb as its F1 guard. 0.f (paired with segmentation_normal_depth
                                                              // == 0.f) disables the absorb entirely.
                                                              float                                                            segmentation_wall_stack,
                                                              bool                                                             segmentation_interlocking_beam,
                                                              IncludeTopAndBottomLayers                                        include_top_and_bottom_layers,
                                                              const std::function<void()>                                     &throw_on_cancel_callback);

// ITEM 1 (.superpowers/sdd/2026-08-31-paint-depth/interclaim-absorb-report.md): given a thin,
// fully-interior base island and the final per-colour claims on its layer (index 0 = base,
// ignored; index >= 1 = painted colours, the only eligible neighbours), returns the 1-based
// colour index of the claim with the largest shared area against the island dilated by `eps` -
// ties broken by the LOWEST colour index. Returns 0 if no painted claim touches the (dilated)
// island at all. Exposed (not static) so it is directly unit-testable: the tie-break rule is
// exact-integer-area-based and must be pinned deterministically, independent of mesh/Clipper
// floating-point geometry.
size_t interclaim_absorb_winner(const ExPolygons &island, const std::vector<ExPolygons> &painted_claims, float eps);

// Returns multi-material segmentation based on painting in multi-material segmentation gizmo
std::vector<std::vector<ExPolygons>> multi_material_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

// Returns fuzzy skin segmentation based on painting in fuzzy skin segmentation gizmo
std::vector<std::vector<ExPolygons>> fuzzy_skin_segmentation_by_painting(const PrintObject &print_object, const std::function<void()> &throw_on_cancel_callback);

} // namespace Slic3r

namespace boost::polygon {
template<> struct geometry_concept<Slic3r::ColoredLine>
{
    typedef segment_concept type;
};

template<> struct segment_traits<Slic3r::ColoredLine>
{
    typedef coord_t       coordinate_type;
    typedef Slic3r::Point point_type;

    static inline point_type get(const Slic3r::ColoredLine &line, const direction_1d &dir)
    {
        return dir.to_int() ? line.line.b : line.line.a;
    }
};
} // namespace boost::polygon

#endif // slic3r_MultiMaterialSegmentation_hpp_
