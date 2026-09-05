#include "SupportGroupsDialog.hpp"

#include <algorithm>

#include <boost/algorithm/string.hpp>

#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>

#include "libslic3r/Model.hpp"
#include "libslic3r/SupportSet.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "GUI_Factories.hpp"
#include "GUI_ObjectList.hpp"
#include "I18N.hpp"
#include "MsgDialog.hpp"
#include "Plater.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/Label.hpp"
#include "format.hpp"

namespace Slic3r {
namespace GUI {

// One window at a time, like the fork's own Slice Compare frame.
static SupportGroupsDialog* g_instance = nullptr;

// Prefixed, because ObjectDataViewModel.hpp already has a colName/colCount family in this
// namespace and an unprefixed one here would redefine colCount.
enum GroupColumn {
    gcGroup = 0,
    gcParts,
    gcInterfaceFilament,
    gcTopZ,
    gcInterfaceLayers,
    gcInterfacePattern,
    gcInterfaceSpacing,
    gcSet,
    gcCount
};

// A value the way the Support page would show it: the enum's own label where there is one, Yes/No
// for a checkbox, the serialised value otherwise. "-" when the group does not carry the key.
static wxString option_display(const DynamicPrintConfig& cfg, const std::string& key)
{
    const ConfigOption* opt = cfg.option(key);
    if (opt == nullptr)
        return wxString("-");
    const ConfigOptionDef* def = print_config_def.get(key);
    const std::string      raw = opt->serialize();
    if (def != nullptr) {
        if (def->enum_labels.size() == def->enum_values.size())
            for (size_t i = 0; i < def->enum_values.size(); ++ i)
                if (def->enum_values[i] == raw)
                    return _(def->enum_labels[i]);
        if (def->type == coBool)
            return opt->getBool() ? _L("Yes") : _L("No");
    }
    return from_u8(raw);
}

// The interface filament slot as the user set it: 0 is "use the support filament".
static wxString interface_filament_display(const DynamicPrintConfig& cfg)
{
    const ConfigOption* opt = cfg.option("support_interface_filament");
    if (opt == nullptr)
        return wxString("-");
    const int slot = opt->getInt();
    return slot <= 0 ? _L("Default") : wxString::Format(_L("Filament %d"), slot);
}

void open_support_groups_dialog(wxWindow* parent, ModelObject* object)
{
    if (object == nullptr)
        return;
    if (g_instance == nullptr) {
        g_instance = new SupportGroupsDialog(parent, object);
        g_instance->Show();
    } else {
        g_instance->set_object(object);
        // Closing the window only hides it, so bring it back rather than raising something the
        // user cannot see.
        g_instance->Show();
        g_instance->Raise();
        g_instance->SetFocus();
    }
}

void support_groups_dialog_refresh()
{
    if (g_instance != nullptr)
        g_instance->reload();
}

SupportGroupsDialog::SupportGroupsDialog(wxWindow* parent, ModelObject* object)
    : wxDialog(parent, wxID_ANY, _L("Support groups"), wxDefaultPosition, wxDefaultSize,
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    if (object != nullptr)
        m_object_id = object->id();
    // The rest of the slicer draws in the fork's own body font (Widgets/Label.hpp); this window
    // was left on the system default, which is both smaller and a different face. Setting it on
    // the dialog before the children are made means every one of them inherits it.
    SetFont(Label::Body_14);
    build_ui();
    // The user edits parts in the object list and in the part parameter panel while this window
    // is open; refreshing when it comes back to the front is enough to stay in step, and costs
    // nothing when nothing changed.
    Bind(wxEVT_ACTIVATE, [this](wxActivateEvent& evt) {
        evt.Skip();
        if (evt.GetActive())
            reload();
    });
    reload();
    wxGetApp().UpdateDlgDarkUI(this);
    // The table HEADER is drawn by the native header control behind the generic
    // wxDataViewListCtrl, and the recursive UpdateDarkUI pass UpdateDlgDarkUI just ran never
    // reaches it: it recolours the wxWindow, not that HWND, so the header kept whatever text
    // colour the last theme left on it - which is how it ended up white on white and unreadable.
    // GUI_App::UpdateDVCDarkUI is the fork's own answer, and every other table in the application
    // (the object list, Unsaved Changes, Print Host, Slice Compare) calls it: it applies the
    // mode-aware explorer theme to the header HWND and sets a header wxItemAttr carrying
    // NppDarkMode::GetTextColor() - 0xF0F0F0 in dark mode, the system window text in light - plus
    // the application's normal font. It also gives the rows the alternating colour and the border
    // the other tables have. It has to run AFTER UpdateDlgDarkUI, which would otherwise repaint
    // over it.
    wxGetApp().UpdateDVCDarkUI(m_table);
}

SupportGroupsDialog::~SupportGroupsDialog()
{
    if (g_instance == this)
        g_instance = nullptr;
}

ModelObject* SupportGroupsDialog::object() const
{
    if (wxGetApp().plater() == nullptr)
        return nullptr;
    for (ModelObject* mo : wxGetApp().plater()->model().objects)
        if (mo->id() == m_object_id)
            return mo;
    return nullptr;
}

void SupportGroupsDialog::set_object(ModelObject* object)
{
    if (object == nullptr)
        return;
    m_object_id = object->id();
    reload();
}

// The object-list selection, restricted to this window's object: a New group is made out of
// exactly the parts the user has highlighted, which is why the window has to be non-modal.
std::vector<ModelVolume*> SupportGroupsDialog::selection() const
{
    std::vector<ModelVolume*> out;
    const ModelObject* mo = object();
    if (mo == nullptr)
        return out;
    for (ModelVolume* volume : selected_part_volumes())
        if (volume->get_object() == mo)
            out.push_back(volume);
    return out;
}

const SupportGroupsDialog::Row* SupportGroupsDialog::selected_row() const
{
    if (m_table == nullptr)
        return nullptr;
    const int row = m_table->GetSelectedRow();
    if (row == wxNOT_FOUND || row < 0 || row >= (int) m_rows.size())
        return nullptr;
    return &m_rows[size_t(row)];
}

void SupportGroupsDialog::build_ui()
{
    wxBoxSizer* main = new wxBoxSizer(wxVERTICAL);
    const int em = wxGetApp().em_unit();

    m_hint = new wxStaticText(this, wxID_ANY, wxEmptyString);
    main->Add(m_hint, 0, wxEXPAND | wxALL, em / 2);

    m_table = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES | wxDV_SINGLE);
    // Only the name is editable, and in place: renaming a group must not raise a modal text
    // prompt (see the header).
    m_table->AppendTextColumn(_L("Group"),              wxDATAVIEW_CELL_EDITABLE, 14 * em);
    m_table->AppendTextColumn(_L("Parts"),              wxDATAVIEW_CELL_INERT,     4 * em);
    m_table->AppendTextColumn(_L("Interface filament"), wxDATAVIEW_CELL_INERT,     9 * em);
    m_table->AppendTextColumn(_L("Top Z distance"),     wxDATAVIEW_CELL_INERT,     7 * em);
    m_table->AppendTextColumn(_L("Interface layers"),   wxDATAVIEW_CELL_INERT,     7 * em);
    m_table->AppendTextColumn(_L("Interface pattern"),  wxDATAVIEW_CELL_INERT,     9 * em);
    m_table->AppendTextColumn(_L("Interface spacing"),  wxDATAVIEW_CELL_INERT,     8 * em);
    m_table->AppendTextColumn(_L("Support set"),        wxDATAVIEW_CELL_INERT,    12 * em);
    m_table->SetMinSize(wxSize(76 * em, 16 * em));
    m_table->SetFont(Label::Body_14);
    m_hint->SetFont(Label::Body_14);
    // The header is themed at the end of the constructor, after UpdateDlgDarkUI - see there.
    main->Add(m_table, 1, wxEXPAND | wxALL, em / 2);

    m_table->Bind(wxEVT_DATAVIEW_SELECTION_CHANGED, [this](wxDataViewEvent& evt) {
        evt.Skip();
        update_buttons();
    });
    m_table->Bind(wxEVT_DATAVIEW_ITEM_EDITING_DONE, [this](wxDataViewEvent& evt) {
        evt.Skip();
        if (m_reloading || evt.IsEditCancelled())
            return;
        // The store commits the edited text after this handler returns, so read it afterwards
        // rather than racing it.
        CallAfter([this]() { on_rename(); });
    });

    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    auto add_button = [this, buttons, em](Button** btn, const wxString& label, const wxString& tip,
                                          ButtonStyle style, void (SupportGroupsDialog::*handler)()) {
        *btn = new Button(this, label);
        (*btn)->SetStyle(style, ButtonType::Compact);
        (*btn)->SetToolTip(tip);
        (*btn)->Bind(wxEVT_BUTTON, [this, handler](wxCommandEvent&) { (this->*handler)(); });
        buttons->Add(*btn, 0, wxRIGHT, em / 2);
    };
    add_button(&m_btn_new, _L("New"),
               _L("Make a new group out of the parts selected in the object list"),
               ButtonStyle::Confirm, &SupportGroupsDialog::on_new);
    add_button(&m_btn_rename, _L("Rename"),
               _L("Rename the selected group; you can also edit the name in the list"),
               ButtonStyle::Regular, &SupportGroupsDialog::on_rename_start);
    add_button(&m_btn_delete, _L("Delete"),
               _L("Delete the group and put its parts back on the object's own support settings"),
               ButtonStyle::Regular, &SupportGroupsDialog::on_delete);
    add_button(&m_btn_reapply, _L("Re-apply set"),
               _L("Write the saved support set of the same name onto this group's parts again"),
               ButtonStyle::Regular, &SupportGroupsDialog::on_reapply_set);
    add_button(&m_btn_select, _L("Select parts"),
               _L("Select this group's parts in the object list"),
               ButtonStyle::Regular, &SupportGroupsDialog::on_select_parts);
    buttons->AddStretchSpacer();
    Button* close = new Button(this, _L("Close"));
    close->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    close->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { Hide(); });
    buttons->Add(close, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, em / 2);

    SetSizerAndFit(main);
}

void SupportGroupsDialog::reload()
{
    if (m_table == nullptr)
        return;
    ModelObject* mo = object();
    if (mo == nullptr) {
        // The object was deleted while the window was open.
        m_rows.clear();
        m_reloading = true;
        m_table->DeleteAllItems();
        m_reloading = false;
        m_hint->SetLabel(_L("This object is gone. Close this window and open it again from another object."));
        update_buttons();
        return;
    }

    const std::string selected = selected_row() != nullptr ? selected_row()->name : std::string();
    // Opening or refocusing the window is an explicit user action, so the one directory scan the
    // set store needs is allowed here - never from a settings-change scope, which a phone request
    // can reach.
    SupportSetStore::instance().reload();

    // Row 0 is always the default group, even with no ungrouped parts, so it can be addressed
    // unconditionally - the same rule PrintObject::support_groups() follows.
    m_rows.clear();
    m_rows.push_back(Row{ std::string(), {}, object_support_values(mo), false, false });
    for (const std::string& name : object_support_group_names(mo)) {
        DynamicPrintConfig values = object_support_values(mo);
        values.apply(support_group_values(mo, name), true);
        Row row{ name, {}, values, false, false };
        // A group "references" the support set of the same name - that is the whole reference,
        // and it is why the New-group-from-set menu names the group after the set. There is no
        // second config key holding a set id; the plan's 3.1 allows exactly one new key.
        if (const SupportSet* set = SupportSetStore::instance().find(name); set != nullptr) {
            row.has_set = true;
            std::string warning;
            const DynamicPrintConfig resolved = support_set_values_by_name(name, &warning);
            for (const std::string& key : resolved.keys()) {
                const ConfigOption* want = resolved.option(key);
                const ConfigOption* have = row.values.option(key);
                if (want != nullptr && (have == nullptr || *have != *want)) {
                    row.modified = true;
                    break;
                }
            }
        }
        m_rows.push_back(std::move(row));
    }
    for (ModelVolume* volume : mo->volumes) {
        if (!volume->is_model_part())
            continue;
        const std::string name = SettingsFactory::part_support_group(volume);
        for (Row& row : m_rows)
            if (row.name == name) {
                row.volumes.push_back(volume);
                break;
            }
    }

    m_reloading = true;
    m_table->DeleteAllItems();
    int select_row = 0;
    for (size_t i = 0; i < m_rows.size(); ++ i) {
        const Row& row = m_rows[i];
        wxVector<wxVariant> data;
        data.push_back(wxVariant(i == 0 ? _L("Object settings") : from_u8(row.name)));
        data.push_back(wxVariant(wxString::Format("%d", int(row.volumes.size()))));
        data.push_back(wxVariant(interface_filament_display(row.values)));
        data.push_back(wxVariant(option_display(row.values, "support_top_z_distance")));
        data.push_back(wxVariant(option_display(row.values, "support_interface_top_layers") + " / " +
                                 option_display(row.values, "support_interface_bottom_layers")));
        data.push_back(wxVariant(option_display(row.values, "support_interface_pattern")));
        data.push_back(wxVariant(option_display(row.values, "support_interface_spacing") + " / " +
                                 option_display(row.values, "support_bottom_interface_spacing")));
        data.push_back(wxVariant(i == 0 ? wxString("-")
                                        : row.has_set ? (row.modified ? format_wxstr(_L("%1% (modified)"), from_u8(row.name))
                                                                      : from_u8(row.name))
                                                      : wxString("-")));
        m_table->AppendItem(data);
        if (!selected.empty() && row.name == selected)
            select_row = int(i);
    }
    m_reloading = false;
    if (!m_rows.empty())
        m_table->SelectRow(select_row);

    m_hint->SetLabel(format_wxstr(_L("Support groups of \"%1%\". A group's values live on its parts, "
                                     "so the project keeps them; the name is only a label."),
                                  from_u8(mo->name)));
    update_buttons();
    Layout();
}

void SupportGroupsDialog::update_buttons()
{
    const Row*  row       = selected_row();
    const bool  real      = row != nullptr && !row->name.empty();
    const bool  have_sel  = !selection().empty();
    if (m_btn_new     != nullptr) m_btn_new->Enable(have_sel);
    if (m_btn_rename  != nullptr) m_btn_rename->Enable(real);
    if (m_btn_delete  != nullptr) m_btn_delete->Enable(real);
    if (m_btn_reapply != nullptr) m_btn_reapply->Enable(real && row->has_set);
    if (m_btn_select  != nullptr) m_btn_select->Enable(row != nullptr && !row->volumes.empty());
}

void SupportGroupsDialog::on_rename_start()
{
    const int row = m_table != nullptr ? m_table->GetSelectedRow() : wxNOT_FOUND;
    if (row == wxNOT_FOUND || row == 0)
        return;
    m_table->EditItem(m_table->RowToItem(row), m_table->GetColumn(gcGroup));
}

void SupportGroupsDialog::on_rename()
{
    ModelObject* mo = object();
    const int    row = m_table != nullptr ? m_table->GetSelectedRow() : wxNOT_FOUND;
    if (mo == nullptr || row <= 0 || row >= (int) m_rows.size())
        return;
    const std::string from = m_rows[size_t(row)].name;
    std::string       to   = into_u8(m_table->GetTextValue(unsigned(row), gcGroup));
    boost::trim(to);
    if (to.empty() || to == from) {
        reload();       // put the old text back
        return;
    }
    const std::vector<std::string> used = object_support_group_names(mo);
    if (std::find(used.begin(), used.end(), to) != used.end()) {
        MessageDialog(this, format_wxstr(_L("This object already has a group called \"%1%\"."), from_u8(to)),
                      _L("Support groups"), wxICON_WARNING | wxOK).ShowModal();
        reload();
        return;
    }
    // Only the label moves; the values already on the parts stay exactly as they are.
    wxGetApp().plater()->take_snapshot("Support group renamed");
    for (ModelVolume* volume : m_rows[size_t(row)].volumes)
        volume->config.set_key_value("support_group", new ConfigOptionString(to));
    wxGetApp().obj_list()->update_support_group_badges();
    reload();
}

void SupportGroupsDialog::on_new()
{
    ModelObject* mo = object();
    const std::vector<ModelVolume*> parts = selection();
    if (mo == nullptr || parts.empty())
        return;
    const DynamicPrintConfig values = object_support_values(mo);
    // Not translated: the label goes into the 3MF and is read back on any machine.
    assign_support_group(parts, unique_support_group_name(mo, "Group"), &values, "Support group assigned");
    reload();
}

void SupportGroupsDialog::on_delete()
{
    const Row* row = selected_row();
    if (row == nullptr || row->name.empty())
        return;
    assign_support_group(row->volumes, std::string(), nullptr, "Support group deleted");
    reload();
}

void SupportGroupsDialog::on_reapply_set()
{
    const Row* row = selected_row();
    if (row == nullptr || row->name.empty() || !row->has_set)
        return;
    std::string warning;
    const DynamicPrintConfig values = support_set_values_by_name(row->name, &warning);
    assign_support_group(row->volumes, row->name, &values, "Support set re-applied");
    if (!warning.empty())
        MessageDialog(this, from_u8(warning), _L("Support groups"), wxICON_INFORMATION | wxOK).ShowModal();
    reload();
}

void SupportGroupsDialog::on_select_parts()
{
    ModelObject* mo  = object();
    const Row*   row = selected_row();
    if (mo == nullptr || row == nullptr || row->volumes.empty())
        return;
    std::vector<ObjectVolumeID> ids;
    ids.reserve(row->volumes.size());
    for (ModelVolume* volume : row->volumes)
        ids.push_back({ mo, volume });
    wxGetApp().obj_list()->select_items(ids);
    update_buttons();
}

} // namespace GUI
} // namespace Slic3r
