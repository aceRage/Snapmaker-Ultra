#include "SliceCompareFrame.hpp"

#include "libslic3r/SliceCompare/Diff.hpp"
#include "libslic3r/ExtrusionEntity.hpp"

#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/GUI_App.hpp"

#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/button.h>
#include <wx/choice.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>

#include <cmath>
#include <utility>

namespace Slic3r {
namespace GUI {

namespace {

// The one open SliceCompareFrame, if any; nulled out by the destructor so
// open_slice_compare_frame() knows to create a fresh one.
SliceCompareFrame* g_instance = nullptr;

// Client data attached to each wxChoice item.
//   store_id > 0 : SnapshotStore id, fetched fresh on selection (may have
//                  been evicted, in which case the picker falls back to null).
//   store_id == 0: a snapshot loaded from disk (held directly, since it
//                  lives outside the store).
//   store_id == -1: the trailing "Browse..." entry.
class PickerItemData : public wxClientData
{
public:
    int store_id = -1;
    std::shared_ptr<const SliceCompare::Snapshot> file_snapshot;
};

wxString snapshot_label(const SliceCompare::Snapshot& snap)
{
    if (!snap.label.empty())
        return wxString::FromUTF8(snap.label);
    if (!snap.source.empty())
        return wxString::FromUTF8(snap.source);
    return _L("Untitled snapshot");
}

wxString format_duration_hm(double seconds)
{
    if (seconds < 0.0)
        seconds = 0.0;
    const long total = std::lround(seconds);
    const long h = total / 3600;
    const long m = (total % 3600) / 60;
    const long s = total % 60;
    if (h > 0)
        return wxString::Format("%ldh %ldm", h, m);
    if (m > 0)
        return wxString::Format("%ldm %lds", m, s);
    return wxString::Format("%lds", s);
}

wxString format_signed_duration(double delta_seconds)
{
    const wxString sign = delta_seconds > 0.0 ? wxString("+") : (delta_seconds < 0.0 ? wxString("-") : wxString());
    return sign + format_duration_hm(std::fabs(delta_seconds));
}

wxString format_stat_row(const wxString& a_val, const wxString& b_val, const wxString& delta)
{
    return a_val + " -> " + b_val + "  (" + delta + ")";
}

wxString role_display_name(uint8_t role)
{
    if (role >= static_cast<uint8_t>(erCount))
        return _L("Unknown");
    return wxString::FromUTF8(ExtrusionEntity::role_to_string(static_cast<ExtrusionRole>(role)));
}

// mm values on a FeatureRow are filament mm; convert to grams via each
// snapshot's own (filament_g / filament_mm) ratio. Falls back to mm on both
// sides if either snapshot has no usable ratio, so the two values stay in
// the same unit.
struct FeatureAmount
{
    double a = 0.0, b = 0.0;
    bool grams = true;
};

FeatureAmount feature_amount(const SliceCompare::FeatureRow& row,
                              const SliceCompare::Snapshot& a,
                              const SliceCompare::Snapshot& b)
{
    FeatureAmount out;
    const double ratio_a = a.filament_mm > 0.0 ? a.filament_g / a.filament_mm : 0.0;
    const double ratio_b = b.filament_mm > 0.0 ? b.filament_g / b.filament_mm : 0.0;
    if (ratio_a > 0.0 && ratio_b > 0.0) {
        out.grams = true;
        out.a = row.mm_a * ratio_a;
        out.b = row.mm_b * ratio_b;
    } else {
        out.grams = false;
        out.a = row.mm_a;
        out.b = row.mm_b;
    }
    return out;
}

} // anonymous namespace

void open_slice_compare_frame(wxWindow* parent, bool preselect_last_two)
{
    if (!g_instance) {
        g_instance = new SliceCompareFrame(parent);
        g_instance->Show();
    } else {
        g_instance->Raise();
        g_instance->SetFocus();
    }

    if (preselect_last_two)
        g_instance->preselect_last_two();
}

SliceCompareFrame::SliceCompareFrame(wxWindow* parent)
    : wxFrame(parent, wxID_ANY, _L("Compare Slices"), wxDefaultPosition, wxSize(1200, 800), wxDEFAULT_FRAME_STYLE)
{
    build_ui();
    rebuild_pickers();
    recompute();

    wxGetApp().UpdateFrameDarkUI(this);
}

SliceCompareFrame::~SliceCompareFrame()
{
    if (g_instance == this)
        g_instance = nullptr;
}

void SliceCompareFrame::build_ui()
{
    wxBoxSizer* main_sizer = new wxBoxSizer(wxVERTICAL);

    // Top row: A picker | swap | B picker.
    wxBoxSizer* top_row = new wxBoxSizer(wxHORIZONTAL);
    top_row->Add(new wxStaticText(this, wxID_ANY, _L("A:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_pick_a = new wxChoice(this, wxID_ANY);
    top_row->Add(m_pick_a, 1, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    m_swap_btn = new wxButton(this, wxID_ANY, wxString::FromUTF8("\xE2\x87\x84"), wxDefaultPosition, wxDefaultSize, wxBU_EXACTFIT);
    m_swap_btn->SetToolTip(_L("Swap A and B"));
    top_row->Add(m_swap_btn, 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 10);

    top_row->Add(new wxStaticText(this, wxID_ANY, _L("B:")), 0, wxALIGN_CENTER_VERTICAL | wxRIGHT, 5);
    m_pick_b = new wxChoice(this, wxID_ANY);
    top_row->Add(m_pick_b, 1, wxALIGN_CENTER_VERTICAL);

    main_sizer->Add(top_row, 0, wxEXPAND | wxALL, 10);

    // Header strip: est time / filament / layers / max speed, "A -> B (delta)".
    wxFlexGridSizer* header_grid = new wxFlexGridSizer(4, 2, 4, 10);
    header_grid->AddGrowableCol(1, 1);
    auto add_header_row = [&](const wxString& label, wxStaticText*& target) {
        header_grid->Add(new wxStaticText(this, wxID_ANY, label), 0, wxALIGN_LEFT | wxALIGN_CENTER_VERTICAL);
        target = new wxStaticText(this, wxID_ANY, wxEmptyString);
        header_grid->Add(target, 1, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    };
    add_header_row(_L("Estimated time:"), m_header_time);
    add_header_row(_L("Filament:"), m_header_filament);
    add_header_row(_L("Layers:"), m_header_layers);
    add_header_row(_L("Max speed:"), m_header_speed);

    main_sizer->Add(header_grid, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    // Notebook (settings / feature tables) on the left, canvas placeholder on the right.
    wxBoxSizer* content_row = new wxBoxSizer(wxHORIZONTAL);

    m_notebook = new wxNotebook(this, wxID_ANY);
    m_notebook->SetMinSize(wxSize(380, -1));

    wxPanel* cfg_page = new wxPanel(m_notebook);
    m_cfg_table = new wxDataViewListCtrl(cfg_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES);
    m_cfg_table->AppendTextColumn(_L("Key"), wxDATAVIEW_CELL_INERT, 160);
    m_cfg_table->AppendTextColumn(_L("A"), wxDATAVIEW_CELL_INERT, 100);
    m_cfg_table->AppendTextColumn(_L("B"), wxDATAVIEW_CELL_INERT, 100);
    wxBoxSizer* cfg_sizer = new wxBoxSizer(wxVERTICAL);
    cfg_sizer->Add(m_cfg_table, 1, wxEXPAND);
    cfg_page->SetSizer(cfg_sizer);

    wxPanel* feat_page = new wxPanel(m_notebook);
    m_feat_table = new wxDataViewListCtrl(feat_page, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxDV_ROW_LINES);
    m_feat_table->AppendTextColumn(_L("Feature"), wxDATAVIEW_CELL_INERT, 140);
    m_feat_table->AppendTextColumn(wxString::FromUTF8("\xCE\x94 Time"), wxDATAVIEW_CELL_INERT, 90);
    m_feat_table->AppendTextColumn(wxString::FromUTF8("\xCE\x94 Grams"), wxDATAVIEW_CELL_INERT, 90);
    m_feat_table->AppendTextColumn(_L("B Time"), wxDATAVIEW_CELL_INERT, 90);
    wxBoxSizer* feat_sizer = new wxBoxSizer(wxVERTICAL);
    feat_sizer->Add(m_feat_table, 1, wxEXPAND);
    feat_page->SetSizer(feat_sizer);

    m_notebook->AddPage(cfg_page, _L("Settings changes"));
    m_notebook->AddPage(feat_page, _L("By feature"));

    // Placeholder for the Task 9 canvas view.
    m_canvas_placeholder = new wxPanel(this);
    m_canvas_placeholder->SetBackgroundColour(wxSystemSettings::GetColour(wxSYS_COLOUR_BTNSHADOW));

    content_row->Add(m_notebook, 0, wxEXPAND | wxRIGHT, 10);
    content_row->Add(m_canvas_placeholder, 1, wxEXPAND);

    main_sizer->Add(content_row, 1, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

    SetSizer(main_sizer);

    m_pick_a->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { on_choice_changed(m_pick_a, m_a, m_pick_a_prev_sel); });
    m_pick_b->Bind(wxEVT_CHOICE, [this](wxCommandEvent&) { on_choice_changed(m_pick_b, m_b, m_pick_b_prev_sel); });
    m_swap_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) {
        std::swap(m_a, m_b);
        sync_choice_selection(m_pick_a, m_a, m_pick_a_prev_sel);
        sync_choice_selection(m_pick_b, m_b, m_pick_b_prev_sel);
        recompute();
    });

    wxGetApp().UpdateDVCDarkUI(m_cfg_table);
    wxGetApp().UpdateDVCDarkUI(m_feat_table);
}

void SliceCompareFrame::rebuild_pickers()
{
    auto fill = [](wxChoice* choice) {
        choice->Clear();
        for (const auto& kv : SliceCompare::SnapshotStore::instance().list()) {
            auto* data = new PickerItemData();
            data->store_id = kv.first;
            choice->Append(wxString::FromUTF8(kv.second), data);
        }
        auto* browse_data = new PickerItemData();
        browse_data->store_id = -1;
        choice->Append(_L("Browse...") , browse_data);
    };
    fill(m_pick_a);
    fill(m_pick_b);
    m_pick_a_prev_sel = wxNOT_FOUND;
    m_pick_b_prev_sel = wxNOT_FOUND;
}

void SliceCompareFrame::on_choice_changed(wxChoice* choice, std::shared_ptr<const SliceCompare::Snapshot>& target, int& prev_selection)
{
    const int sel = choice->GetSelection();
    if (sel == wxNOT_FOUND)
        return;

    auto* data = dynamic_cast<PickerItemData*>(choice->GetClientObject(sel));
    if (!data)
        return;

    if (data->store_id == -1) {
        // "Browse..." - load a snapshot from a .gcode/.3mf file on disk.
        wxFileDialog dlg(this, _L("Select a G-code or 3MF file"), wxEmptyString, wxEmptyString,
            _L("G-code/3MF files") + " (*.gcode;*.3mf)|*.gcode;*.3mf|" + _L("All files") + " (*.*)|*.*",
            wxFD_OPEN | wxFD_FILE_MUST_EXIST);
        if (dlg.ShowModal() != wxID_OK) {
            choice->SetSelection(prev_selection);
            return;
        }

        const std::string path = dlg.GetPath().ToUTF8().data();
        SliceCompare::FileLoadResult result = SliceCompare::load_snapshot_from_file(path);
        if (!result.snapshot) {
            wxMessageBox(result.error.empty() ? _L("Failed to load the selected file.") : wxString::FromUTF8(result.error),
                _L("Slice Compare"), wxOK | wxICON_ERROR, this);
            choice->SetSelection(prev_selection);
            return;
        }

        auto snap = std::make_shared<const SliceCompare::Snapshot>(std::move(*result.snapshot));
        auto* new_data = new PickerItemData();
        new_data->store_id = 0;
        new_data->file_snapshot = snap;
        const unsigned int insert_pos = static_cast<unsigned int>(sel); // Browse... currently sits here
        choice->Insert(snapshot_label(*snap), insert_pos, new_data);
        choice->SetSelection(static_cast<int>(insert_pos));
        prev_selection = static_cast<int>(insert_pos);
        target = snap;
        recompute();
        return;
    }

    if (data->store_id == 0)
        target = data->file_snapshot;
    else
        target = SliceCompare::SnapshotStore::instance().get(data->store_id);

    prev_selection = sel;
    recompute();
}

void SliceCompareFrame::sync_choice_selection(wxChoice* choice, const std::shared_ptr<const SliceCompare::Snapshot>& snap, int& prev_selection)
{
    if (!snap)
        return;

    for (unsigned int i = 0; i < choice->GetCount(); ++i) {
        auto* data = dynamic_cast<PickerItemData*>(choice->GetClientObject(i));
        if (!data)
            continue;
        std::shared_ptr<const SliceCompare::Snapshot> item_snap;
        if (data->store_id == 0)
            item_snap = data->file_snapshot;
        else if (data->store_id > 0)
            item_snap = SliceCompare::SnapshotStore::instance().get(data->store_id);
        if (item_snap == snap) {
            choice->SetSelection(static_cast<int>(i));
            prev_selection = static_cast<int>(i);
            return;
        }
    }

    // Not present in this picker yet (e.g. supplied externally, or loaded
    // into the other picker) - insert it just before "Browse..." so the
    // control reflects the current selection.
    const unsigned int insert_pos = choice->GetCount() > 0 ? choice->GetCount() - 1 : 0;
    auto* new_data = new PickerItemData();
    new_data->store_id = 0;
    new_data->file_snapshot = snap;
    choice->Insert(snapshot_label(*snap), insert_pos, new_data);
    choice->SetSelection(static_cast<int>(insert_pos));
    prev_selection = static_cast<int>(insert_pos);
}

void SliceCompareFrame::set_snapshots(std::shared_ptr<const SliceCompare::Snapshot> a,
                                       std::shared_ptr<const SliceCompare::Snapshot> b)
{
    m_a = std::move(a);
    m_b = std::move(b);
    sync_choice_selection(m_pick_a, m_a, m_pick_a_prev_sel);
    sync_choice_selection(m_pick_b, m_b, m_pick_b_prev_sel);
    recompute();
}

void SliceCompareFrame::preselect_last_two()
{
    const auto list = SliceCompare::SnapshotStore::instance().list(); // newest first
    if (list.size() < 2)
        return;
    std::shared_ptr<const SliceCompare::Snapshot> newer = SliceCompare::SnapshotStore::instance().get(list[0].first);
    std::shared_ptr<const SliceCompare::Snapshot> older = SliceCompare::SnapshotStore::instance().get(list[1].first);
    if (older && newer)
        set_snapshots(older, newer);
}

void SliceCompareFrame::update_header()
{
    if (!m_a || !m_b) {
        m_header_time->SetLabel(_L("Select two snapshots to compare."));
        m_header_filament->SetLabel(wxEmptyString);
        m_header_layers->SetLabel(wxEmptyString);
        m_header_speed->SetLabel(wxEmptyString);
        return;
    }

    m_header_time->SetLabel(format_stat_row(
        format_duration_hm(m_a->est_seconds), format_duration_hm(m_b->est_seconds),
        format_signed_duration(m_b->est_seconds - m_a->est_seconds)));

    m_header_filament->SetLabel(format_stat_row(
        wxString::Format("%.1fg", m_a->filament_g), wxString::Format("%.1fg", m_b->filament_g),
        wxString::Format("%+.1fg", m_b->filament_g - m_a->filament_g)));

    m_header_layers->SetLabel(format_stat_row(
        wxString::Format("%d", m_a->layer_count), wxString::Format("%d", m_b->layer_count),
        wxString::Format("%+d", m_b->layer_count - m_a->layer_count)));

    m_header_speed->SetLabel(format_stat_row(
        wxString::Format("%.0f mm/s", m_a->max_speed), wxString::Format("%.0f mm/s", m_b->max_speed),
        wxString::Format("%+.0f mm/s", m_b->max_speed - m_a->max_speed)));
}

void SliceCompareFrame::recompute()
{
    update_header();

    m_cfg_table->DeleteAllItems();
    m_feat_table->DeleteAllItems();

    if (!m_a || !m_b) {
        Layout();
        return;
    }

    for (const auto& row : SliceCompare::diff_configs(*m_a, *m_b)) {
        wxVector<wxVariant> data;
        data.push_back(wxVariant(wxString::FromUTF8(row.key)));
        data.push_back(wxVariant(row.a.empty() ? _L("(none)") : wxString::FromUTF8(row.a)));
        data.push_back(wxVariant(row.b.empty() ? _L("(none)") : wxString::FromUTF8(row.b)));
        m_cfg_table->AppendItem(data);
    }

    for (const auto& row : SliceCompare::diff_features(*m_a, *m_b)) {
        const FeatureAmount amt = feature_amount(row, *m_a, *m_b);
        wxVector<wxVariant> data;
        data.push_back(wxVariant(role_display_name(row.role)));
        data.push_back(wxVariant(format_signed_duration(row.sec_b - row.sec_a)));
        data.push_back(wxVariant(amt.grams ? wxString::Format("%+.2fg", amt.b - amt.a)
                                            : wxString::Format("%+.1fmm", amt.b - amt.a)));
        data.push_back(wxVariant(format_duration_hm(row.sec_b)));
        m_feat_table->AppendItem(data);
    }

    Layout();
}

} // namespace GUI
} // namespace Slic3r
