#ifndef slic3r_CutUtils_hpp_
#define slic3r_CutUtils_hpp_

#include "enum_bitmask.hpp"
#include "Point.hpp"
#include "Model.hpp"
#include "CurvedCut.hpp"
#include "DrawCut.hpp"

#include <vector>

namespace Slic3r {

using ModelObjectPtrs = std::vector<ModelObject*>;

// Append the NEGATIVE_VOLUME that carries a flexi joint into `mo` before the cut runs.
// The volume's mesh is the male body; Cut regenerates both bodies from cut_info.flexi, so
// the mesh is only there for previews and for "does it intersect the contour" checks.
// Lives in libslic3r (not in the gizmo) so tests can drive the whole apply path headless.
ModelVolume* add_flexi_joint_volume(ModelObject* mo, const CutConnector& connector, const std::string& name);

// True when this object carries at least one unprocessed flexi joint connector.
bool has_flexi_joint(const ModelObject* mo);

enum class ModelObjectCutAttribute : int { KeepUpper, KeepLower, KeepAsParts, FlipUpper, FlipLower, PlaceOnCutUpper, PlaceOnCutLower, CreateDowels, InvalidateCutInfo };
using ModelObjectCutAttributes = enum_bitmask<ModelObjectCutAttribute>;
ENABLE_ENUM_BITMASK_OPERATORS(ModelObjectCutAttribute);


class Cut {

    Model                       m_model;
    int                         m_instance;
    const Transform3d           m_cut_matrix;
    ModelObjectCutAttributes    m_attributes;
    // The cut thickness ("kerf") the current perform_with_plane() was asked for,
    // stashed so perform_with_flexi_joints() - which it dispatches into without
    // arguments - can widen the joint's gap by it. Zero for every other entry
    // point, which is the no-kerf behaviour.
    double                      m_kerf{ 0.0 };

    void post_process(ModelObject* object, ModelObjectPtrs& objects, bool keep, bool place_on_cut, bool flip);
    void post_process(ModelObject* upper_object, ModelObject* lower_object, ModelObjectPtrs& objects);
    void finalize(const ModelObjectPtrs& objects);

public:

    Cut(const ModelObject* object, int instance, const Transform3d& cut_matrix, 
        ModelObjectCutAttributes attributes = ModelObjectCutAttribute::KeepUpper |
                                              ModelObjectCutAttribute::KeepLower |
                                              ModelObjectCutAttribute::KeepAsParts );
    ~Cut() { m_model.clear_objects(); }

    struct Groove
    {
        float depth{ 0.f };
        float width{ 0.f };
        float flaps_angle{ 0.f };
        float angle{ 0.f };
        float depth_init{ 0.f };
        float width_init{ 0.f };
        float flaps_angle_init{ 0.f };
        float angle_init{ 0.f };
        float depth_tolerance{ 0.1f };
        float width_tolerance{ 0.1f };
    };

    struct Part
    {
        bool selected;
        bool is_modifier;
    };

    // `thickness` is the CUT THICKNESS ("kerf"), in mm along the cut normal: a
    // band of material centred on the cut surface is removed, so the upper half
    // keeps what is above +t/2 and the lower half what is below -t/2. Zero (the
    // default) is the original cut, bit-for-bit - the offset slice is only taken
    // when t > 0. See CurvedCut.hpp for the shared range and offset enum.
    const ModelObjectPtrs& perform_with_plane(double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // Curved cut, phase 1: split by a height field z = f(u,v) over the cut plane
    // instead of by the plane itself. A sheet with every control point at zero IS
    // the plane, and this routes straight into perform_with_plane() in that case, so
    // a zero-displacement curved cut runs the same code path as today's flat cut and
    // its output is bit-identical. No connectors on a curved cut in phase 1.
    const ModelObjectPtrs& perform_with_curved_sheet(const CurvedCutSheet& sheet, double thickness = 0.0, CutThicknessOffset offset = CutThicknessOffset::Centred);
    // Draw cut, phase 1: split by a RULED STRIP swept along a stroke the user drew
    // on the model's surface, instead of by the plane or by a height field. The
    // stroke is in the cut plane's own frame, the same frame the sheet lives in.
    //
    // The kerf rides in through `params.thickness` rather than as its own argument:
    // a drawn cut's band is offset along the STRIP's own normal (which varies along
    // the stroke), not along a single plane normal, so there is nothing for a
    // separate `thickness` argument to mean here.
    //
    // No connectors on a drawn cut in phase 1 (phase 2 adds the surface frame the
    // connector path would stand on). A flexi joint dispatches to
    // perform_with_flexi_joints() exactly as it does on a curved cut, for the same
    // reason: the joint's two segments are separated by its own gap between two
    // flat faces, so a flexi cut cannot also be a drawn one.
    const ModelObjectPtrs& perform_with_draw_stroke(const DrawCutStroke& stroke, const DrawCutParams& params);
    // Flexi joint cut: one object, two watertight model parts, a real Manifold boolean.
    // perform_with_plane() dispatches here automatically when a flexi connector is present.
    const ModelObjectPtrs& perform_with_flexi_joints();
    const ModelObjectPtrs& perform_by_contour(std::vector<Part> parts, int dowels_count);
    const ModelObjectPtrs& perform_with_groove(const Groove& groove, const Transform3d& rotation_m, bool keep_as_parts = false);

}; // namespace Cut

} // namespace Slic3r

#endif /* slic3r_CutUtils_hpp_ */
