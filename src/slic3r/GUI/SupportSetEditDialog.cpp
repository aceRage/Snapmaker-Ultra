#include "SupportSetEditDialog.hpp"

#include <algorithm>

#include <wx/sizer.h>
#include <wx/stattext.h>

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "OptionsGroup.hpp"
#include "Widgets/Button.hpp"
#include "Widgets/ComboBox.hpp"
#include "format.hpp"

namespace Slic3r {
namespace GUI {

// The rows, in the order the process tab's Support page shows them, grouped under the same
// headings. This is the curated part-level key set of PrintConfig.cpp's part_support_keys() minus
// support_interface_filament, which a set never stores as a slot index: it travels as the
// portable interface_filament_type and gets its own row below. Thirteen option lines plus that
// one, so every value a set can carry is on this window.
static const std::vector<std::string>& keys_support()
{
    static const std::vector<std::string> s_keys = { "support_style", "support_threshold_angle" };
    return s_keys;
}

static const std::vector<std::string>& keys_ironing()
{
    static const std::vector<std::string> s_keys = {
        "support_ironing", "support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing"
    };
    return s_keys;
}

static const std::vector<std::string>& keys_advanced()
{
    static const std::vector<std::string> s_keys = {
        "support_top_z_distance",
        "support_interface_top_layers", "support_interface_bottom_layers",
        "support_interface_pattern", "support_interface_spacing", "support_bottom_interface_spacing",
        // Commented out on the process tab (Tab.cpp, "Advanced"), but a set stores it and a group
        // resolves it, so the editor is the one place it can be seen and changed.
        "support_interface_loop_pattern",
    };
    return s_keys;
}

SupportSetEditDialog::SupportSetEditDialog(wxWindow                 *parent,
                                           const SupportSet         &set,
                                           const DynamicPrintConfig &filament_config)
    : wxDialog(parent, wxID_ANY, format_wxstr(_L("Edit support set - %1%"), from_u8(set.name)),
               wxDefaultPosition, wxDefaultSize, wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
    , m_set(set)
    , m_filament_config(filament_config)
{
    // The editor's own config: the set's values where it has them, the option's own default
    // otherwise, so every row has something to show. Nothing here is connected to the project -
    // the whole point of the window is that changing a set does not change what is being sliced.
    ConfigSubstitutionContext substitutions(ForwardCompatibilitySubstitutionRule::Enable);
    for (const std::vector<std::string>* list : { &keys_support(), &keys_ironing(), &keys_advanced() })
        for (const std::string& key : *list) {
            const ConfigOptionDef* def = print_config_def.get(key);
            if (def == nullptr || def->default_value.get() == nullptr || !support_set_is_allowed_key(key))
                continue;
            m_config.set_key_value(key, def->default_value->clone());
            if (auto it = m_set.values.find(key); it != m_set.values.end())
                m_config.set_deserialize(key, it->second, substitutions);
        }

    const int em = wxGetApp().em_unit();
    wxBoxSizer* main = new wxBoxSizer(wxVERTICAL);

    wxStaticText* head = new wxStaticText(this, wxID_ANY,
        _L("These values belong to the support set. Changing them here does not change the "
           "current project - use Apply on the Support page, or Re-apply set in the Support "
           "groups window, to put them to work."));
    head->Wrap(52 * em);
    main->Add(head, 0, wxEXPAND | wxALL, em / 2);

    add_group(main, _L("Support"),  keys_support());
    add_interface_filament_row(main);
    add_group(main, _L("Ironing"),  keys_ironing());
    add_group(main, _L("Advanced"), keys_advanced());

    wxBoxSizer* buttons = new wxBoxSizer(wxHORIZONTAL);
    buttons->AddStretchSpacer();
    Button* save = new Button(this, _L("Save"));
    save->SetStyle(ButtonStyle::Confirm, ButtonType::Compact);
    save->SetToolTip(_L("Write these values back to this support set"));
    save->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        collect();
        EndModal(wxID_OK);
    });
    Button* cancel = new Button(this, _L("Cancel"));
    cancel->SetStyle(ButtonStyle::Regular, ButtonType::Compact);
    cancel->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CANCEL); });
    buttons->Add(save, 0, wxRIGHT, em / 2);
    buttons->Add(cancel, 0);
    main->Add(buttons, 0, wxEXPAND | wxALL, em / 2);

    Bind(wxEVT_CLOSE_WINDOW, [this](wxCloseEvent&) { EndModal(wxID_CANCEL); });

    toggle_fields();
    SetSizerAndFit(main);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

void SupportSetEditDialog::add_group(wxSizer *parent_sizer, const wxString &title, const std::vector<std::string> &keys)
{
    // is_tab_opt = false, so the group draws itself in a static box with its title - the same
    // shape PhysicalPrinterDialog's option group has, and it keeps the tab's searcher out of it.
    auto og = std::make_shared<ConfigOptionsGroup>(this, title, &m_config);
    og->m_on_change = [this](t_config_option_key opt_key, boost::any) {
        if (opt_key == "support_ironing")
            toggle_fields();
    };
    for (const std::string& key : keys)
        if (m_config.has(key))
            og->append_single_option_line(key);
    og->activate();
    m_groups.push_back(og);
    parent_sizer->Add(og->sizer, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, wxGetApp().em_unit() / 2);
}

void SupportSetEditDialog::add_interface_filament_row(wxSizer *parent_sizer)
{
    const int em = wxGetApp().em_unit();

    wxStaticBoxSizer* box = new wxStaticBoxSizer(wxVERTICAL, this, _L("Filament"));
    wxBoxSizer*       row = new wxBoxSizer(wxHORIZONTAL);

    wxStaticText* label = new wxStaticText(box->GetStaticBox(), wxID_ANY, _L("Support/raft interface") + ":");
    row->Add(label, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, em);

    m_filament_combo = new ComboBox(box->GetStaticBox(), wxID_ANY, wxEmptyString, wxDefaultPosition,
                                    wxSize(18 * em, -1), 0, nullptr, wxCB_READONLY);
    m_filament_combo->SetToolTip(_L("A set travels between printers, so it stores the interface filament as a "
                                    "TYPE rather than a slot number. The slot is worked out when the set is "
                                    "applied, on whatever printer that happens to be."));

    // "same" and "soluble" always; then every filament type loaded on this printer, so the usual
    // choice is one click away. An existing value that is none of those is kept as its own entry
    // rather than silently rewritten.
    auto add_entry = [this](const wxString& label, const std::string& value) {
        if (std::find(m_filament_type_values.begin(), m_filament_type_values.end(), value) != m_filament_type_values.end())
            return;
        m_filament_combo->Append(label);
        m_filament_type_values.push_back(value);
    };
    add_entry(_L("Same as the part"), "same");
    add_entry(_L("Soluble"), "soluble");
    if (const ConfigOptionStrings* types = m_filament_config.option<ConfigOptionStrings>("filament_type"); types != nullptr)
        for (const std::string& type : types->values)
            if (!type.empty())
                add_entry(from_u8(type), type);
    if (!m_set.interface_filament_type.empty())
        add_entry(from_u8(m_set.interface_filament_type), m_set.interface_filament_type);

    int sel = 0;
    for (size_t i = 0; i < m_filament_type_values.size(); ++ i)
        if (m_filament_type_values[i] == m_set.interface_filament_type)
            sel = int(i);
    m_filament_combo->SetSelection(sel);
    m_filament_combo->Bind(wxEVT_COMBOBOX, [this](wxCommandEvent& evt) {
        evt.Skip();
        update_filament_note();
    });
    row->Add(m_filament_combo, 0, wxALIGN_CENTER_VERTICAL);

    box->Add(row, 0, wxEXPAND | wxALL, em / 2);

    m_filament_note = new wxStaticText(box->GetStaticBox(), wxID_ANY, wxEmptyString);
    box->Add(m_filament_note, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);

    parent_sizer->Add(box, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, em / 2);
    update_filament_note();
}

void SupportSetEditDialog::update_filament_note()
{
    if (m_filament_note == nullptr || m_filament_combo == nullptr)
        return;
    const int sel = m_filament_combo->GetSelection();
    if (sel < 0 || size_t(sel) >= m_filament_type_values.size())
        return;
    const std::string& type = m_filament_type_values[size_t(sel)];

    std::string warning;
    const int   slot = resolve_interface_filament(type, m_filament_config, &warning);
    wxString    text;
    bool        is_warning = !warning.empty();
    if (is_warning)
        text = from_u8(warning);
    else if (slot > 0)
        text = format_wxstr(_L("On this printer that is filament %1%."), slot);
    else
        text = _L("The support interface uses the same filament as the part.");
    m_filament_note->SetLabel(text);
    m_filament_note->SetForegroundColour(is_warning ? wxColour("#ED6B21")
                                                    : wxSystemSettings::GetColour(wxSYS_COLOUR_GRAYTEXT));
    m_filament_note->GetParent()->Layout();
}

void SupportSetEditDialog::toggle_fields()
{
    const ConfigOption* ironing = m_config.option("support_ironing");
    const bool          on      = ironing != nullptr && ironing->getBool();
    for (const std::string& key : { "support_ironing_pattern", "support_ironing_flow", "support_ironing_spacing" })
        for (const std::shared_ptr<ConfigOptionsGroup>& og : m_groups)
            if (Field* field = og->get_field(key); field != nullptr)
                field->toggle(on);
}

void SupportSetEditDialog::collect()
{
    // Only the keys this window shows are written back. A set carries more than the curated
    // fourteen (support_set_keys() is the whole Support category minus the excluded list), and
    // those values are none of the editor's business - they stay exactly as they were on disk.
    for (const std::string& key : m_config.keys())
        m_set.values[key] = m_config.opt_serialize(key);
    if (m_filament_combo != nullptr) {
        const int sel = m_filament_combo->GetSelection();
        if (sel >= 0 && size_t(sel) < m_filament_type_values.size())
            m_set.interface_filament_type = m_filament_type_values[size_t(sel)];
    }
}

} // namespace GUI
} // namespace Slic3r
