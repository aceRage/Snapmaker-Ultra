#ifndef slic3r_GUI_RoundDialog_hpp_
#define slic3r_GUI_RoundDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/MeshRound.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// The dialog shown before "Round all edges" (ObjectList::round_all_edges, and the
// Edit gizmo's button). It only collects options - every bit of geometry lives in
// libslic3r/MeshRound.
//
// Deliberately the same shape as RemeshDialog: the two operations share a voxel
// round trip, a "keep the bottom flat" option and the same painted-data warning, so
// making them look alike is the honest thing rather than a shortcut.
//
// `bbox_min_extent` is the shortest side of the part's bounding box: a rolling ball
// cannot round a feature thinner than twice its radius, so the dialog warns when the
// radius is past that rather than letting the geometry return an empty mesh.
class RoundDialog : public DPIDialog
{
public:
    RoundDialog(wxWindow *parent, double bbox_min_extent, size_t triangles_before, double surface_area);

    // Valid after ShowModal() returns wxID_OK; also written back to AppConfig then.
    const RoundOptions &options() const { return m_options; }

    // The stored options, clamped. Shared with the gizmo's panel so both start from
    // the same numbers.
    static RoundOptions load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double       read_mm(TextInput *input, double fallback) const;
    RoundOptions current_options() const;
    void         update_preview();
    void         save_to_config() const;

    RoundOptions m_options;
    double       m_bbox_min_extent  = 0.;
    size_t       m_triangles_before = 0;
    double       m_surface_area     = 0.;

    TextInput  *m_radius_input  = nullptr;
    TextInput  *m_voxel_input   = nullptr;
    ::CheckBox *m_concave_cb    = nullptr;
    ::CheckBox *m_flat_cb       = nullptr;
    TextInput  *m_margin_input  = nullptr;
    ::Label    *m_preview_text  = nullptr;
    ::Label    *m_warning_text  = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_RoundDialog_hpp_
