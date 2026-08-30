#ifndef slic3r_GUI_SliceCompareFrame_hpp_
#define slic3r_GUI_SliceCompareFrame_hpp_

#include "libslic3r/SliceCompare/Snapshot.hpp"
#include "libslic3r/SliceCompare/Diff.hpp"
#include "CompareCanvas.hpp"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/dataview.h>
#include <wx/slider.h>
#include <wx/tglbtn.h>

#include <memory>

namespace Slic3r {
namespace GUI {

// Opens the (singleton) Slice Compare frame, raising the existing instance
// instead of creating a second one if it is already open.
// preselect_last_two: when true, the two newest SnapshotStore entries (of
// the current plate, falling back to the whole session) are selected as
// A/B (older -> A, newer -> B). See SliceCompareFrame::preselect_last_two().
void open_slice_compare_frame(wxWindow* parent, bool preselect_last_two);

// Refresh the open compare frame's snapshot pickers (no-op when no frame is open).
// Called from the Plater slice-capture hook so snapshots created while the frame
// is open appear without reopening it. UI thread only.
void slice_compare_notify_snapshots_changed();

class SliceCompareFrame : public wxFrame
{
public:
    explicit SliceCompareFrame(wxWindow* parent);
    ~SliceCompareFrame() override;

    // Assigns both snapshots, syncs the pickers to match where possible, and
    // recomputes the header stats and tables.
    void set_snapshots(std::shared_ptr<const SliceCompare::Snapshot> a,
                        std::shared_ptr<const SliceCompare::Snapshot> b);

    // Selects the two newest SnapshotStore entries belonging to the current
    // plate as A/B (older -> A, newer -> B), falling back to the two newest
    // of the whole session when fewer than two of the current plate exist.
    // No-op when fewer than two session snapshots exist at all.
    void preselect_last_two();

    // Rebuilds the SnapshotStore-backed picker entries ("Browse..." entry
    // always trails) and re-syncs the current A/B selection back onto the
    // freshly-built lists by identity where it still exists. Public so
    // open_slice_compare_frame() can refresh an already-open frame's
    // pickers when raising it (snapshots sliced while it was open otherwise
    // wouldn't show up until it was closed and reopened).
    void rebuild_pickers();

private:
    void build_ui();
    void recompute();        // runs diff_configs/diff_features, fills header+tables
    void update_header();

    // Handles a wxEVT_CHOICE on `choice`: resolves the selected entry (store
    // item / previously-loaded file / "Browse..."), updating `target` and
    // `prev_selection` (used to revert the choice on a cancelled/failed browse).
    void on_choice_changed(wxChoice* choice, std::shared_ptr<const SliceCompare::Snapshot>& target, int& prev_selection);

    // Finds (or, failing that, inserts) the picker entry matching `snap` and
    // selects it.
    void sync_choice_selection(wxChoice* choice, const std::shared_ptr<const SliceCompare::Snapshot>& snap, int& prev_selection);

    // Selects m_layer_diff.rows[row_index] (clamped/no-op if out of range):
    // resolves the row's zkey_a/zkey_b to LayerRec pointers (nullptr for -1
    // keys), pushes them to the canvas, and refreshes the "z=" label and
    // status line.
    void select_layer_row(int row_index);

    // Fills the status line for the currently selected row (or the
    // layer-height-mismatch banner, when the whole diff has no matched
    // layers at all).
    void update_status_line(const SliceCompare::LayerMatch& row,
                             const SliceCompare::LayerRec* la, const SliceCompare::LayerRec* lb);

    // Moves the slider/canvas to the row whose zkey_a == m_layer_diff.biggest_zkey_a.
    void jump_to_biggest_change();

    wxChoice* m_pick_a = nullptr;
    wxChoice* m_pick_b = nullptr;
    wxButton* m_swap_btn = nullptr;

    // Shown only while the SnapshotStore has fewer than two session
    // snapshots to pick from (see rebuild_pickers()).
    wxStaticText* m_session_hint = nullptr;

    wxStaticText* m_header_time = nullptr;
    wxStaticText* m_header_filament = nullptr;
    wxStaticText* m_header_layers = nullptr;
    wxStaticText* m_header_speed = nullptr;

    wxNotebook*         m_notebook = nullptr;
    wxDataViewListCtrl* m_cfg_table = nullptr;
    wxDataViewListCtrl* m_feat_table = nullptr;

    CompareCanvas* m_canvas = nullptr;
    wxSlider*      m_layer_slider = nullptr;
    wxPanel*       m_layer_tick_strip = nullptr; // custom-painted; see LayerTickStrip in the .cpp
    wxStaticText*  m_layer_z_label = nullptr;
    wxButton*      m_jump_btn = nullptr;
    wxToggleButton* m_side_by_side_btn = nullptr;
    wxStaticText*  m_status_line = nullptr;

    SliceCompare::LayerDiff m_layer_diff;

    int m_pick_a_prev_sel = wxNOT_FOUND;
    int m_pick_b_prev_sel = wxNOT_FOUND;

    std::shared_ptr<const SliceCompare::Snapshot> m_a;
    std::shared_ptr<const SliceCompare::Snapshot> m_b;
};

} // namespace GUI
} // namespace Slic3r

#endif
