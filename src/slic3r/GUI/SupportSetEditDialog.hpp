#ifndef slic3r_GUI_SupportSetEditDialog_hpp_
#define slic3r_GUI_SupportSetEditDialog_hpp_

// Ultra (support sets): the pop-out editor for one saved support set.
//
// It is MODAL, unlike the Support-groups window, and deliberately so: it edits a file, not the
// model, so there is nothing in the object list to pick while it is open, and Save/Cancel is the
// whole of its contract. The one rule it must not break is that editing a set NEVER touches the
// current project's process settings - so it drives its own DynamicPrintConfig, seeded from the
// set's values, and hands the edited set back to the caller. Applying a set to the project stays
// where it was: the row's "Apply" button and the Support-groups window's "Re-apply set".
//
// The fields are the fork's own ConfigOptionsGroup over print_config_def, so the controls, the
// enum labels and the tooltips are the ones the process tab's Support page shows.
//
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 3.2 / 3.5.

#include <memory>
#include <string>
#include <vector>

#include <wx/dialog.h>

#include "libslic3r/PrintConfig.hpp"
#include "libslic3r/SupportSet.hpp"

class wxSizer;
class wxStaticText;
// The fork's themed widgets live in the global namespace (Widgets/ComboBox.hpp, Widgets/Button.hpp).
class ComboBox;
class Button;

namespace Slic3r {
namespace GUI {

class ConfigOptionsGroup;

class SupportSetEditDialog : public wxDialog
{
public:
    // `filament_config` carries filament_type / filament_soluble, for the interface-filament
    // resolution note. It may be empty; the note is then simply not shown.
    SupportSetEditDialog(wxWindow                 *parent,
                         const SupportSet         &set,
                         const DynamicPrintConfig &filament_config);

    // Valid after ShowModal() == wxID_OK: the input set with the edited values written back.
    // Every key the editor does not show is carried over untouched.
    const SupportSet& edited_set() const { return m_set; }

private:
    void add_group(wxSizer *parent_sizer, const wxString &title, const std::vector<std::string> &keys);
    void add_interface_filament_row(wxSizer *parent_sizer);
    // Grey out the ironing sub-options while ironing is off, the way the process tab does.
    void toggle_fields();
    void update_filament_note();
    void collect();

    SupportSet         m_set;
    DynamicPrintConfig m_filament_config;
    // The editor's own copy of the values. Nothing outside this dialog ever sees it.
    DynamicPrintConfig m_config;

    std::vector<std::shared_ptr<ConfigOptionsGroup>> m_groups;
    // Combo entries in order; index into m_filament_type_values gives the stored type string.
    ComboBox                        *m_filament_combo = nullptr;
    std::vector<std::string>         m_filament_type_values;
    wxStaticText                    *m_filament_note  = nullptr;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SupportSetEditDialog_hpp_
