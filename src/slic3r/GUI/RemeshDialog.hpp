#ifndef slic3r_GUI_RemeshDialog_hpp_
#define slic3r_GUI_RemeshDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/MeshRemesh.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// The dialog shown before "Repair by remeshing" (ObjectList::repair_by_remesh).
// It only collects options - every bit of geometry lives in libslic3r/MeshRemesh.
//
// `auto_voxel` is the value the menu item used to pick silently (bbox diagonal / 300,
// clamped); it is shown as the prefilled voxel size so the user can see what they are
// overriding rather than guessing. `triangles_before` drives the "Triangles" line.
class RemeshDialog : public DPIDialog
{
public:
    RemeshDialog(wxWindow *parent, double auto_voxel, size_t triangles_before, double surface_area);

    // Valid after ShowModal() returns wxID_OK; also written back to AppConfig then.
    const RemeshOptions &options() const { return m_options; }

    // The stored options, clamped, with the voxel size defaulted to `auto_voxel`
    // when nothing has been saved yet.
    static RemeshOptions load_from_config(double auto_voxel);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double        read_mm(TextInput *input, double fallback) const;
    RemeshOptions current_options() const;
    void          update_preview();
    void          save_to_config() const;

    RemeshOptions m_options;
    double        m_auto_voxel      = 0.1;
    size_t        m_triangles_before = 0;
    double        m_surface_area     = 0.;

    TextInput  *m_voxel_input  = nullptr;
    ::CheckBox *m_flat_cb      = nullptr;
    TextInput  *m_margin_input = nullptr;
    ::CheckBox *m_sharp_cb     = nullptr;
    TextInput  *m_angle_input  = nullptr;
    ::Label    *m_preview_text = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_RemeshDialog_hpp_
