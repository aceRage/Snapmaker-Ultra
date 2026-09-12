#ifndef slic3r_GUI_SliceBakeDialog_hpp_
#define slic3r_GUI_SliceBakeDialog_hpp_

// The options dialog shown before "Bake slice to mesh..." (ObjectList::bake_slice_to_mesh).
//
// Built the same way RemeshDialog is, and for the same reason: it collects options and nothing
// else - every bit of geometry lives in libslic3r/SliceBake, and the run itself is a background
// job (Jobs/SliceBakeJob).
//
// Spec: docs/superpowers/specs/2026-09-12-slice-bake-research.md (phase 1, section 4).

#include "GUI_Utils.hpp"

#include "libslic3r/SliceBake.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/ComboBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// What to do with the mesh once it exists.
enum class SliceBakeResultMode {
    Replace = 0,  // the object becomes the bake, in place
    AddNew,       // the bake arrives as a second object beside the original
    ExportSTL,    // nothing in the scene changes; the mesh goes to a file
};

struct SliceBakeSettings
{
    SliceBakeResultMode result = SliceBakeResultMode::Replace;
    SliceBakeOptions    options;   // layer subset (all in phase 1), close-gaps radius
};

class SliceBakeDialog : public DPIDialog
{
public:
    // `layers` and `estimated_triangles` are the figures for the object that will be baked; they
    // drive the size line, which is the whole point of showing a dialog for a one-option action.
    SliceBakeDialog(wxWindow *parent, const wxString &object_name, size_t layers, size_t estimated_triangles);

    // Valid after ShowModal() returns wxID_OK; also written back to AppConfig then.
    const SliceBakeSettings &settings() const { return m_settings; }

    static SliceBakeSettings load_from_config();

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    double            read_mm(TextInput *input, double fallback) const;
    SliceBakeSettings current_settings() const;
    void              update_preview();
    void              save_to_config() const;

    SliceBakeSettings m_settings;
    wxString          m_object_name;
    size_t            m_layers     = 0;
    size_t            m_estimate   = 0;

    ::ComboBox *m_result_choice = nullptr;
    ::CheckBox *m_close_cb      = nullptr;
    TextInput  *m_close_input   = nullptr;
    ::Label    *m_size_text     = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_SliceBakeDialog_hpp_
