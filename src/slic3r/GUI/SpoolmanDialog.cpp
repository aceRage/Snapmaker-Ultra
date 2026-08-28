#include "SpoolmanDialog.hpp"

#include "I18N.hpp"
#include "GUI_App.hpp"
#include "Plater.hpp"
#include "PartPlate.hpp"
#include "NotificationManager.hpp"
#include "libslic3r/AppConfig.hpp"
#include "libslic3r/PresetBundle.hpp"
#include "libslic3r/GCode/GCodeProcessor.hpp"

#include <wx/dataview.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/menu.h>
#include <wx/msgdlg.h>

#include <boost/format.hpp>
#include <thread>

namespace Slic3r {
namespace GUI {

static std::string slot_key(int slot) { return "slot_" + std::to_string(slot); }

int SpoolmanDialog::bound_spool_for_slot(int slot)
{
    const std::string v = wxGetApp().app_config->get("spoolman_bindings", slot_key(slot));
    return v.empty() ? 0 : std::atoi(v.c_str());
}

void SpoolmanDialog::bind_slot(int slot, int spool_id)
{
    wxGetApp().app_config->set("spoolman_bindings", slot_key(slot), std::to_string(spool_id));
}

SpoolmanDialog::SpoolmanDialog(wxWindow* parent)
    : wxDialog(parent, wxID_ANY, _L("Spool Manager"), wxDefaultPosition, wxSize(760, 420),
               wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER)
{
    auto* sizer = new wxBoxSizer(wxVERTICAL);

    auto* toolbar = new wxBoxSizer(wxHORIZONTAL);
    m_status = new wxStaticText(this, wxID_ANY, "");
    toolbar->Add(m_status, 1, wxALIGN_CENTER_VERTICAL | wxLEFT, 8);
    auto* deduct_btn = new wxButton(this, wxID_ANY, _L("Deduct last slice"));
    deduct_btn->SetToolTip(_L("Subtract the current plate's sliced filament usage from the spools bound to each slot"));
    deduct_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { deduct_last_slice(); });
    toolbar->Add(deduct_btn, 0, wxALL, 6);
    auto* refresh_btn = new wxButton(this, wxID_ANY, _L("Refresh"));
    refresh_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { refresh_spools(); });
    toolbar->Add(refresh_btn, 0, wxALL, 6);
    sizer->Add(toolbar, 0, wxEXPAND);

    m_list = new wxDataViewListCtrl(this, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES);
    m_list->AppendTextColumn(_L("Spool"),     wxDATAVIEW_CELL_INERT, 230);
    m_list->AppendTextColumn(_L("Material"),  wxDATAVIEW_CELL_INERT, 70);
    m_list->AppendTextColumn(_L("Color"),     wxDATAVIEW_CELL_INERT, 70);
    m_list->AppendTextColumn(_L("Remaining"), wxDATAVIEW_CELL_INERT, 90);
    m_list->AppendTextColumn(_L("Location"),  wxDATAVIEW_CELL_INERT, 110);
    m_list->AppendTextColumn(_L("Bound to"),  wxDATAVIEW_CELL_INERT, 90);
    // Left double click on a row: pick a slot to bind this spool to.
    m_list->Bind(wxEVT_DATAVIEW_ITEM_ACTIVATED, [this](wxDataViewEvent& evt) {
        const int row = m_list->ItemToRow(evt.GetItem());
        if (row >= 0)
            on_use_in_slot(row);
    });
    sizer->Add(m_list, 1, wxEXPAND | wxALL, 6);

    auto* hint = new wxStaticText(this, wxID_ANY, _L("Double-click a spool to bind it to a filament slot. Bindings are used when deducting usage."));
    sizer->Add(hint, 0, wxLEFT | wxBOTTOM, 10);

    SetSizer(sizer);
    refresh_spools();
}

void SpoolmanDialog::update_status(const wxString& text, bool error)
{
    m_status->SetLabel(text);
    m_status->SetForegroundColour(error ? wxColour(217, 83, 79) : wxColour(10, 158, 80));
}

void SpoolmanDialog::refresh_spools()
{
    m_list->DeleteAllItems();
    m_spools.clear();
    std::string error;
    if (!Spoolman::enabled()) {
        update_status(_L("Spoolman is not configured (Preferences > Ultra)"), true);
        return;
    }
    if (!Spoolman::get_spools(m_spools, error)) {
        update_status(wxString::Format(_L("Connection failed: %s"), error), true);
        return;
    }
    // slot bindings reverse map for the "Bound to" column
    const size_t slot_count = wxGetApp().preset_bundle != nullptr ? wxGetApp().preset_bundle->filament_presets.size() : 0;
    for (const SpoolmanSpool& s : m_spools) {
        wxVector<wxVariant> row;
        std::string label = s.vendor.empty() ? s.name : s.vendor + " " + s.name;
        row.push_back(wxVariant(wxString::FromUTF8(label + " #" + std::to_string(s.id))));
        row.push_back(wxVariant(wxString::FromUTF8(s.material)));
        row.push_back(wxVariant(wxString::FromUTF8(s.color_hex.empty() ? std::string("-") : "#" + s.color_hex)));
        row.push_back(wxVariant(s.remaining_weight >= 0. ? wxString::Format("%.0f g", s.remaining_weight) : wxString("-")));
        row.push_back(wxVariant(wxString::FromUTF8(s.location)));
        wxString bound = "-";
        for (size_t slot = 0; slot < slot_count; ++slot)
            if (bound_spool_for_slot(int(slot)) == s.id)
                bound = wxString::Format(_L("Slot %d"), int(slot) + 1);
        row.push_back(wxVariant(bound));
        m_list->AppendItem(row);
    }
    update_status(wxString::Format(_L("Connected - %d spools"), int(m_spools.size())));
}

void SpoolmanDialog::on_use_in_slot(int row)
{
    if (row < 0 || row >= int(m_spools.size()) || wxGetApp().preset_bundle == nullptr)
        return;
    const int slot_count = int(wxGetApp().preset_bundle->filament_presets.size());
    wxMenu menu;
    for (int slot = 0; slot < slot_count; ++slot) {
        wxMenuItem* item = menu.Append(wxID_ANY, wxString::Format(_L("Bind to slot %d"), slot + 1));
        const int spool_id = m_spools[size_t(row)].id;
        menu.Bind(wxEVT_MENU, [this, slot, spool_id](wxCommandEvent&) {
            // one spool per slot: clear any previous binding of this spool
            for (int s = 0; s < 64; ++s)
                if (bound_spool_for_slot(s) == spool_id)
                    bind_slot(s, 0);
            bind_slot(slot, spool_id);
            refresh_spools();
        }, item->GetId());
    }
    PopupMenu(&menu);
}

// UI-thread only: (spool_id, grams) pairs for the current plate's sliced usage.
static std::vector<std::pair<int, double>> collect_plate_usage()
{
    std::vector<std::pair<int, double>> usage;
    Plater* plater = wxGetApp().plater();
    if (plater == nullptr)
        return usage;
    PartPlate* plate = plater->get_partplate_list().get_curr_plate();
    if (plate == nullptr || !plate->is_slice_result_valid())
        return usage;
    GCodeProcessorResult* result = plate->get_slice_result();
    if (result == nullptr)
        return usage;
    const auto* density_opt = wxGetApp().preset_bundle->full_config().option<ConfigOptionFloats>("filament_density");
    for (const auto& [extruder, volume_mm3] : result->print_statistics.total_volumes_per_extruder) {
        const int spool_id = SpoolmanDialog::bound_spool_for_slot(int(extruder));
        if (spool_id <= 0 || volume_mm3 <= 0.)
            continue;
        const double density = (density_opt != nullptr && extruder < density_opt->values.size()) ? density_opt->values[extruder] : 1.24; // g/cm3
        usage.emplace_back(spool_id, volume_mm3 / 1000. * density);
    }
    return usage;
}

static std::string deduct_usage(const std::vector<std::pair<int, double>>& usage)
{
    std::string summary;
    for (const auto& [spool_id, grams] : usage) {
        std::string error;
        if (Spoolman::use_weight(spool_id, grams, error)) {
            if (!summary.empty())
                summary += ", ";
            summary += (boost::format("%.1f g -> spool #%d") % grams % spool_id).str();
        }
    }
    return summary;
}

std::string SpoolmanDialog::deduct_current_plate_usage()
{
    if (!Spoolman::enabled())
        return {};
    return deduct_usage(collect_plate_usage());
}

void SpoolmanDialog::deduct_after_send_async()
{
    if (!Spoolman::enabled() || wxGetApp().app_config->get("spoolman_deduct") != "true")
        return;
    auto usage = collect_plate_usage();
    if (usage.empty())
        return;
    std::thread([usage = std::move(usage)]() {
        const std::string summary = deduct_usage(usage);
        if (summary.empty())
            return;
        wxGetApp().CallAfter([summary]() {
            if (Plater* plater = wxGetApp().plater(); plater != nullptr)
                plater->get_notification_manager()->push_notification("Spool Manager: deducted " + summary);
        });
    }).detach();
}

void SpoolmanDialog::deduct_last_slice()
{
    const std::string summary = deduct_current_plate_usage();
    if (summary.empty()) {
        wxMessageBox(_L("Nothing deducted. Slice a plate and bind spools to slots first."), _L("Spool Manager"), wxICON_INFORMATION);
        return;
    }
    refresh_spools();
    update_status(wxString::FromUTF8("Deducted: " + summary));
}

} // namespace GUI
} // namespace Slic3r
