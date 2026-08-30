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
#include <wx/dcbuffer.h>

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

// (filament_g / filament_mm) for one snapshot; 0 when there's no usable
// filament data (e.g. filament_mm == 0), signalling "fall back to mm".
double filament_ratio(const SliceCompare::Snapshot& snap)
{
    return snap.filament_mm > 0.0 ? snap.filament_g / snap.filament_mm : 0.0;
}

FeatureAmount feature_amount(const SliceCompare::FeatureRow& row,
                              const SliceCompare::Snapshot& a,
                              const SliceCompare::Snapshot& b)
{
    FeatureAmount out;
    const double ratio_a = filament_ratio(a);
    const double ratio_b = filament_ratio(b);
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

// Same grams-with-mm-fallback convention as feature_amount(), but for a
// single delta between two raw filament-mm quantities (used for the layer
// status line's "Delta e", where we only have each side's LayerRec::extrusion_mm).
wxString format_delta_grams_or_mm(double mm_a, double mm_b,
                                   const SliceCompare::Snapshot& a, const SliceCompare::Snapshot& b)
{
    const double ratio_a = filament_ratio(a);
    const double ratio_b = filament_ratio(b);
    if (ratio_a > 0.0 && ratio_b > 0.0)
        return wxString::Format("%+.2fg", mm_b * ratio_b - mm_a * ratio_a);
    return wxString::Format("%+.1fmm", mm_b - mm_a);
}

wxString format_signed_seconds(double delta_seconds)
{
    return wxString::Format("%+.1fs", delta_seconds);
}

// Thin custom-painted strip mounted beside the layer slider: a colored tick
// per row that is either "changed" (matched, but different) or "unmatched"
// (a_only/b_only -- no counterpart on the other side). Rows are laid out
// top-to-bottom in the same z-descending order as the vertical wxSlider
// (index 0 = lowest z = bottom).
class LayerTickStrip : public wxPanel
{
public:
    explicit LayerTickStrip(wxWindow* parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxSize(12, -1))
    {
        SetBackgroundStyle(wxBG_STYLE_PAINT);
        SetBackgroundColour(wxColour(0x11, 0x11, 0x11)); // keep in sync with CompareCanvas.cpp colors
        SetMinSize(wxSize(12, -1));
        Bind(wxEVT_PAINT, &LayerTickStrip::on_paint, this);
    }

    void set_rows(std::vector<SliceCompare::LayerMatch> rows)
    {
        m_rows = std::move(rows);
        Refresh();
    }

private:
    void on_paint(wxPaintEvent&)
    {
        wxAutoBufferedPaintDC dc(this);
        dc.SetBackground(wxBrush(wxColour(0x11, 0x11, 0x11))); // keep in sync with CompareCanvas.cpp colors
        dc.Clear();

        const int n = static_cast<int>(m_rows.size());
        if (n <= 0)
            return;

        const wxSize sz = GetClientSize();
        for (int i = 0; i < n; ++i) {
            const SliceCompare::LayerMatch& row = m_rows[i];

            wxColour colour;
            if (row.zkey_a >= 0 && row.zkey_b >= 0 && row.changed)
                colour = wxColour(0xC6, 0x28, 0x28); // changed -- keep in sync with CompareCanvas.cpp colors
            else if (row.zkey_a < 0 || row.zkey_b < 0)
                colour = wxColour(0x9E, 0x9E, 0x9E); // unmatched (a_only/b_only) -- keep in sync with CompareCanvas.cpp colors
            else
                continue; // matched + unchanged: no tick

            // index 0 (lowest z) at the bottom, matching the vertical slider.
            const int y = n > 1 ? (sz.GetHeight() - 1) * (n - 1 - i) / (n - 1) : sz.GetHeight() / 2;
            dc.SetPen(wxPen(colour, 2));
            dc.DrawLine(0, y, sz.GetWidth(), y);
        }
    }

    std::vector<SliceCompare::LayerMatch> m_rows;
};

} // anonymous namespace

void open_slice_compare_frame(wxWindow* parent, bool preselect_last_two)
{
    if (!g_instance) {
        g_instance = new SliceCompareFrame(parent);
        g_instance->Show();
    } else {
        g_instance->Raise();
        g_instance->SetFocus();
        // Snapshots sliced while the frame was already open otherwise
        // wouldn't show up in the pickers until it was closed and reopened;
        // preserves the current A/B selection by identity (see rebuild_pickers()).
        g_instance->rebuild_pickers();
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

    // Guard: nudge the user toward slicing/browsing when there aren't
    // enough session snapshots to pick two from yet. Visibility is toggled
    // in rebuild_pickers(); hidden by default until that first runs.
    m_session_hint = new wxStaticText(this, wxID_ANY, _L("Slice something (or Browse…) to compare"));
    m_session_hint->SetForegroundColour(wxColour(128, 128, 128));
    main_sizer->Add(m_session_hint, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 10);

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
    m_feat_table->AppendTextColumn(_L("Δ Time"), wxDATAVIEW_CELL_INERT, 90);
    m_feat_table->AppendTextColumn(_L("Δ Grams"), wxDATAVIEW_CELL_INERT, 90);
    m_feat_table->AppendTextColumn(_L("B Time"), wxDATAVIEW_CELL_INERT, 90);
    wxBoxSizer* feat_sizer = new wxBoxSizer(wxVERTICAL);
    feat_sizer->Add(m_feat_table, 1, wxEXPAND);
    feat_page->SetSizer(feat_sizer);

    m_notebook->AddPage(cfg_page, _L("Settings changes"));
    m_notebook->AddPage(feat_page, _L("By feature"));

    // Canvas view: overlay canvas | tick strip | vertical layer slider, with
    // the "z=" label and jump button stacked above/below the slider, and the
    // status line spanning the full width underneath.
    wxBoxSizer* canvas_col = new wxBoxSizer(wxVERTICAL);

    wxBoxSizer* canvas_row = new wxBoxSizer(wxHORIZONTAL);
    m_canvas = new CompareCanvas(this);
    canvas_row->Add(m_canvas, 1, wxEXPAND);

    wxBoxSizer* slider_col = new wxBoxSizer(wxVERTICAL);
    m_layer_z_label = new wxStaticText(this, wxID_ANY, _L("z=") + "--", wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER_HORIZONTAL);
    slider_col->Add(m_layer_z_label, 0, wxALIGN_CENTER_HORIZONTAL | wxBOTTOM, 4);

    wxBoxSizer* slider_row = new wxBoxSizer(wxHORIZONTAL);
    m_layer_tick_strip = new LayerTickStrip(this);
    slider_row->Add(m_layer_tick_strip, 0, wxEXPAND | wxRIGHT, 2);
    // wxSL_VERTICAL alone defaults to min-at-top/max-at-bottom; wxSL_INVERSE flips that so the
    // slider's minimum (row 0 == lowest z) sits at the bottom, matching LayerTickStrip's layout
    // (which paints index 0 at the bottom, "low z at bottom" like a printer bed) and the physical
    // thumb position after a jump/select.
    m_layer_slider = new wxSlider(this, wxID_ANY, 0, 0, 0, wxDefaultPosition, wxDefaultSize, wxSL_VERTICAL | wxSL_INVERSE);
    m_layer_slider->Enable(false);
    slider_row->Add(m_layer_slider, 0, wxEXPAND);
    slider_col->Add(slider_row, 1, wxEXPAND);

    m_jump_btn = new wxButton(this, wxID_ANY, _L("Jump to biggest change"));
    m_jump_btn->Enable(false);
    slider_col->Add(m_jump_btn, 0, wxEXPAND | wxTOP, 6);

    m_side_by_side_btn = new wxToggleButton(this, wxID_ANY, _L("Side by side"));
    m_side_by_side_btn->SetToolTip(_L("Show A and B in separate panes instead of overlaid"));
    slider_col->Add(m_side_by_side_btn, 0, wxEXPAND | wxTOP, 6);

    canvas_row->Add(slider_col, 0, wxEXPAND | wxLEFT, 8);
    canvas_col->Add(canvas_row, 1, wxEXPAND);

    m_status_line = new wxStaticText(this, wxID_ANY, wxEmptyString);
    canvas_col->Add(m_status_line, 0, wxEXPAND | wxTOP, 6);

    content_row->Add(m_notebook, 0, wxEXPAND | wxRIGHT, 10);
    content_row->Add(canvas_col, 1, wxEXPAND);

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
    m_layer_slider->Bind(wxEVT_SLIDER, [this](wxCommandEvent&) { select_layer_row(m_layer_slider->GetValue()); });
    m_jump_btn->Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { jump_to_biggest_change(); });
    m_side_by_side_btn->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent&) {
        m_canvas->set_side_by_side(m_side_by_side_btn->GetValue());
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

    // Re-raising an already-open frame calls this again (see
    // open_slice_compare_frame() below): keep whatever A/B was already
    // selected, matched back onto the freshly-rebuilt lists by store id/
    // pointer identity (sync_choice_selection re-inserts file-loaded
    // snapshots that aren't store-backed). A no-op when m_a/m_b are null or
    // the entry was evicted in the meantime.
    sync_choice_selection(m_pick_a, m_a, m_pick_a_prev_sel);
    sync_choice_selection(m_pick_b, m_b, m_pick_b_prev_sel);

    // Guard: with fewer than two session snapshots, the pickers can't offer
    // two to compare from the store alone -- nudge toward slicing/Browse.
    const bool few_snapshots = SliceCompare::SnapshotStore::instance().list().size() < 2;
    m_session_hint->Show(few_snapshots);
    Layout();
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
        // Defensive: drop the canvas's raw LayerRec* pointers before `target`
        // (an m_a/m_b shared_ptr) is reassigned below -- if it held the last
        // reference to the old Snapshot, that reassignment can free the
        // layers it points into before recompute() gets a chance to hand
        // the canvas fresh ones.
        m_canvas->set_layers(nullptr, nullptr);
        target = snap;
        recompute();
        return;
    }

    // Same defensive ordering as above: null out the canvas's pointers into
    // the outgoing snapshot before `target` can evict it (e.g. picking a
    // still-valid entry while the previously-selected one has since been
    // evicted from the SnapshotStore -- get() already null-guards that case
    // downstream in recompute(), but the canvas mustn't hold stale pointers
    // in between).
    m_canvas->set_layers(nullptr, nullptr);
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
    m_layer_diff = SliceCompare::LayerDiff();

    if (!m_a || !m_b) {
        m_layer_slider->SetRange(0, 0);
        m_layer_slider->Enable(false);
        m_jump_btn->Enable(false);
        static_cast<LayerTickStrip*>(m_layer_tick_strip)->set_rows({});
        m_canvas->set_layers(nullptr, nullptr);
        m_layer_z_label->SetLabel(_L("z=") + "--");
        m_status_line->SetLabel(wxEmptyString);
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

    // Layer axis: diff_layers() rows are already z-ascending and include
    // unmatched (a_only/b_only) rows, so they map directly onto slider indices.
    m_layer_diff = SliceCompare::diff_layers(*m_a, *m_b);
    static_cast<LayerTickStrip*>(m_layer_tick_strip)->set_rows(m_layer_diff.rows);

    const int row_count = static_cast<int>(m_layer_diff.rows.size());
    m_layer_slider->Enable(row_count > 0);
    m_jump_btn->Enable(m_layer_diff.biggest_zkey_a != -1);

    if (row_count > 0) {
        m_layer_slider->SetRange(0, row_count - 1);

        // Land on the biggest change (if there is one) so the interesting
        // layer is what the user sees first; otherwise the first row.
        int initial = 0;
        if (m_layer_diff.biggest_zkey_a != -1) {
            for (int i = 0; i < row_count; ++i) {
                if (m_layer_diff.rows[i].zkey_a == m_layer_diff.biggest_zkey_a) {
                    initial = i;
                    break;
                }
            }
        }
        m_layer_slider->SetValue(initial);
        select_layer_row(initial);
        m_canvas->fit_view(); // fresh content: center/zoom to it once, then leave pan/zoom to the user
    } else {
        m_layer_slider->SetRange(0, 0);
        m_canvas->set_layers(nullptr, nullptr);
        m_layer_z_label->SetLabel(_L("z=") + "--");
        m_status_line->SetLabel(wxEmptyString);
    }

    Layout();
}

void SliceCompareFrame::select_layer_row(int row_index)
{
    if (row_index < 0 || row_index >= static_cast<int>(m_layer_diff.rows.size())) {
        m_canvas->set_layers(nullptr, nullptr);
        m_layer_z_label->SetLabel(_L("z=") + "--");
        m_status_line->SetLabel(wxEmptyString);
        return;
    }

    const SliceCompare::LayerMatch& row = m_layer_diff.rows[row_index];

    const SliceCompare::LayerRec* la = nullptr;
    const SliceCompare::LayerRec* lb = nullptr;
    if (m_a && row.zkey_a >= 0) {
        auto it = m_a->layers.find(row.zkey_a);
        if (it != m_a->layers.end())
            la = &it->second;
    }
    if (m_b && row.zkey_b >= 0) {
        auto it = m_b->layers.find(row.zkey_b);
        if (it != m_b->layers.end())
            lb = &it->second;
    }

    m_canvas->set_layers(la, lb);

    const double z = la ? la->z : (lb ? lb->z : 0.0);
    m_layer_z_label->SetLabel(_L("z=") + wxString::Format("%.2f", z));

    update_status_line(row, la, lb);
}

void SliceCompareFrame::update_status_line(const SliceCompare::LayerMatch& row,
                                            const SliceCompare::LayerRec* la, const SliceCompare::LayerRec* lb)
{
    if (m_layer_diff.matched == 0 && (m_layer_diff.a_only + m_layer_diff.b_only) > 0) {
        m_status_line->SetLabel(_L("Layer heights differ — showing coincident layers only; see Settings/Feature tabs"));
        return;
    }

    if (!la) {
        m_status_line->SetLabel(_L("Layer only in B"));
        return;
    }
    if (!lb) {
        m_status_line->SetLabel(_L("Layer only in A"));
        return;
    }

    wxString flags_part;
    if (!row.flags.empty()) {
        wxString joined;
        for (size_t i = 0; i < row.flags.size(); ++i) {
            if (i > 0)
                joined += ", ";
            joined += wxString::FromUTF8(row.flags[i]);
        }
        flags_part = "  [" + joined + "]";
    }

    const wxString delta_e = format_delta_grams_or_mm(la->extrusion_mm, lb->extrusion_mm, *m_a, *m_b);

    m_status_line->SetLabel(_L("Δt=") + format_signed_seconds(row.d_seconds) + "  " +
                             _L("Δe=") + delta_e + "  " +
                             _L("overlap=") + wxString::Format("%.2f", row.overlap) +
                             flags_part);
}

void SliceCompareFrame::jump_to_biggest_change()
{
    if (m_layer_diff.biggest_zkey_a == -1)
        return;

    for (int i = 0; i < static_cast<int>(m_layer_diff.rows.size()); ++i) {
        if (m_layer_diff.rows[i].zkey_a == m_layer_diff.biggest_zkey_a) {
            m_layer_slider->SetValue(i);
            select_layer_row(i);
            m_canvas->fit_view(); // recenter on the layer we just jumped to
            break;
        }
    }
}

} // namespace GUI
} // namespace Slic3r
