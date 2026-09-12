#ifndef slic3r_GUI_QuadRemeshDialog_hpp_
#define slic3r_GUI_QuadRemeshDialog_hpp_

#include "GUI_Utils.hpp"

#include "libslic3r/QuadRemesh.hpp"

#include "Widgets/CheckBox.hpp"
#include "Widgets/DialogButtons.hpp"
#include "Widgets/Label.hpp"
#include "Widgets/TextInput.hpp"

namespace Slic3r { namespace GUI {

// The dialog shown before "Quad remesh..." (ObjectList::quad_remesh). Sibling of
// RemeshDialog and deliberately built the same way: it only collects options, and
// every bit of geometry lives in libslic3r/QuadRemesh.
//
// `default_target` is the prefilled target - the part's triangle count / 2, i.e.
// roughly face-count-preserving. `triangles_before` drives the count line.
//
// `refusal` is non-empty when the selected part is one quad_remesh() will refuse (an
// open mesh, or several shells). The dialog still opens - so the user can read WHY
// rather than wondering why nothing happened - but the OK button is disabled and the
// reason is shown in place of the estimate.
class QuadRemeshDialog : public DPIDialog
{
public:
    QuadRemeshDialog(wxWindow          *parent,
                     int                default_target,
                     size_t             triangles_before,
                     const std::string &refusal);

    // Valid after ShowModal() returns wxID_OK; also written back to AppConfig then.
    const QuadRemeshOptions &options() const { return m_options; }

    // The stored options, clamped, with the target defaulted to `default_target` when
    // nothing has been saved yet.
    static QuadRemeshOptions load_from_config(int default_target);

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override;

private:
    int               read_int(TextInput *input, int fallback) const;
    QuadRemeshOptions current_options() const;
    void              update_preview();
    void              save_to_config() const;

    QuadRemeshOptions m_options;
    int               m_default_target   = 0;
    size_t            m_triangles_before = 0;
    std::string       m_refusal;

    TextInput  *m_target_input = nullptr;
    ::CheckBox *m_sharp_cb     = nullptr;
    ::Label    *m_preview_text = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_QuadRemeshDialog_hpp_
