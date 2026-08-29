#ifndef slic3r_GUI_SliceCompareFrame_hpp_
#define slic3r_GUI_SliceCompareFrame_hpp_

#include "libslic3r/SliceCompare/Snapshot.hpp"

#include <wx/wx.h>
#include <wx/notebook.h>
#include <wx/dataview.h>

#include <memory>

namespace Slic3r {
namespace GUI {

// Opens the (singleton) Slice Compare frame, raising the existing instance
// instead of creating a second one if it is already open.
// preselect_last_two: when true, the two newest SnapshotStore entries are
// selected as A/B (older -> A, newer -> B).
void open_slice_compare_frame(wxWindow* parent, bool preselect_last_two);

class SliceCompareFrame : public wxFrame
{
public:
    explicit SliceCompareFrame(wxWindow* parent);
    ~SliceCompareFrame() override;

    // Assigns both snapshots, syncs the pickers to match where possible, and
    // recomputes the header stats and tables.
    void set_snapshots(std::shared_ptr<const SliceCompare::Snapshot> a,
                        std::shared_ptr<const SliceCompare::Snapshot> b);

    // Selects the two newest SnapshotStore entries as A/B, if at least two
    // exist. No-op otherwise.
    void preselect_last_two();

private:
    void build_ui();
    void rebuild_pickers();  // SnapshotStore list + "Browse..." entries
    void recompute();        // runs diff_configs/diff_features, fills header+tables
    void update_header();

    // Handles a wxEVT_CHOICE on `choice`: resolves the selected entry (store
    // item / previously-loaded file / "Browse..."), updating `target` and
    // `prev_selection` (used to revert the choice on a cancelled/failed browse).
    void on_choice_changed(wxChoice* choice, std::shared_ptr<const SliceCompare::Snapshot>& target, int& prev_selection);

    // Finds (or, failing that, inserts) the picker entry matching `snap` and
    // selects it.
    void sync_choice_selection(wxChoice* choice, const std::shared_ptr<const SliceCompare::Snapshot>& snap, int& prev_selection);

    wxChoice* m_pick_a = nullptr;
    wxChoice* m_pick_b = nullptr;
    wxButton* m_swap_btn = nullptr;

    wxStaticText* m_header_time = nullptr;
    wxStaticText* m_header_filament = nullptr;
    wxStaticText* m_header_layers = nullptr;
    wxStaticText* m_header_speed = nullptr;

    wxNotebook*         m_notebook = nullptr;
    wxDataViewListCtrl* m_cfg_table = nullptr;
    wxDataViewListCtrl* m_feat_table = nullptr;
    wxPanel*            m_canvas_placeholder = nullptr; // canvas view lands in Task 9

    int m_pick_a_prev_sel = wxNOT_FOUND;
    int m_pick_b_prev_sel = wxNOT_FOUND;

    std::shared_ptr<const SliceCompare::Snapshot> m_a;
    std::shared_ptr<const SliceCompare::Snapshot> m_b;
};

} // namespace GUI
} // namespace Slic3r

#endif
