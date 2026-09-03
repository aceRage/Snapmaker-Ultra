#ifndef slic3r_GUI_SupportGroupsDialog_hpp_
#define slic3r_GUI_SupportGroupsDialog_hpp_

// Ultra (support groups): the Support-groups window for one object.
//
// It is NON-MODAL on purpose, and for two reasons. The user picks the parts a new group is made
// from in the object list while the window is open, which a modal window makes impossible; and a
// modal window on a hub-managed (hidden) instance would block a GUI nobody is watching - the
// fork's own rule in "Things that must NOT change". Every prompt it does raise is a
// MessageDialog, whose ShowModal override auto-answers outside Interactive mode; renaming a group
// is an in-place edit in the list, not a wxTextEntryDialog.
//
// docs/superpowers/plans/2026-09-02-support-sets-and-groups.md 2.5.

#include <string>
#include <vector>

#include <wx/dialog.h>

#include "libslic3r/ObjectID.hpp"
#include "libslic3r/PrintConfig.hpp"

class wxDataViewListCtrl;
class wxDataViewEvent;
class wxStaticText;
// The fork's themed button lives in the global namespace (Widgets/Button.hpp).
class Button;

namespace Slic3r {

class ModelObject;
class ModelVolume;

namespace GUI {

// Open (or raise and retarget) the singleton Support-groups window for `object`.
void open_support_groups_dialog(wxWindow* parent, ModelObject* object);
// Rebuild the open window's table; no-op when none is open. Called after a group assignment made
// somewhere else - the object-list menu, an edit in the part parameter panel, undo/redo.
void support_groups_dialog_refresh();

class SupportGroupsDialog : public wxDialog
{
public:
    SupportGroupsDialog(wxWindow* parent, ModelObject* object);
    ~SupportGroupsDialog() override;

    // Point the window at another object (the user right-clicked a different one).
    void set_object(ModelObject* object);
    // Re-read the model and repaint the table, keeping the selected row where possible.
    void reload();

private:
    struct Row {
        // "" for row 0, the default group.
        std::string                 name;
        std::vector<ModelVolume*>   volumes;
        // The object's config with this group's part-level overrides applied - what
        // PrintObject::support_groups() would resolve for it.
        DynamicPrintConfig          values;
        // A saved support set of the same name exists...
        bool                        has_set   = false;
        // ...and one of the keys it defines no longer matches on the parts.
        bool                        modified  = false;
    };

    ModelObject*        object() const;
    const Row*          selected_row() const;
    // The selected object-list parts that belong to this window's object.
    std::vector<ModelVolume*> selection() const;
    void                build_ui();
    void                update_buttons();
    // Starts the in-place edit of the name cell; on_rename() commits it.
    void                on_rename_start();
    void                on_rename();
    void                on_new();
    void                on_delete();
    void                on_reapply_set();
    void                on_select_parts();

    // Resolved by id on every use: the window outlives any particular ModelObject pointer.
    ObjectID            m_object_id;
    std::vector<Row>    m_rows;

    wxDataViewListCtrl* m_table          = nullptr;
    wxStaticText*       m_hint           = nullptr;
    Button*             m_btn_new        = nullptr;
    Button*             m_btn_rename     = nullptr;
    Button*             m_btn_delete     = nullptr;
    Button*             m_btn_reapply    = nullptr;
    Button*             m_btn_select     = nullptr;
    // Guards the in-place rename handler against the DeleteAllItems/AppendItem churn of reload().
    bool                m_reloading      = false;
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_GUI_SupportGroupsDialog_hpp_
