#ifndef slic3r_GUI_SliceCompare_CompareCanvas_hpp_
#define slic3r_GUI_SliceCompare_CompareCanvas_hpp_

#include "libslic3r/SliceCompare/Snapshot.hpp"

#include <wx/panel.h>
#include <wx/geometry.h>
#include <wx/colour.h>

#include <vector>

class wxGraphicsContext;

namespace Slic3r {
namespace GUI {

// Overlays the toolpath segments of two layers (one from each compared
// snapshot): unchanged geometry in gray, small rescued/jittered matches in
// green, and A-only/B-only differences in blue/red. Recomputes the
// SliceCompare::SegDiff lazily whenever the layer pair changes.
//
// Neither LayerRec pointer passed to set_layers() is owned by the canvas;
// they must stay valid until the next call (in practice: as long as
// SliceCompareFrame keeps the owning Snapshot's shared_ptr alive).
class CompareCanvas : public wxPanel
{
public:
    explicit CompareCanvas(wxWindow* parent);

    // Either pointer may be null (e.g. an unmatched a_only/b_only row).
    // Recomputes the SegDiff for the new pair and repaints.
    void set_layers(const SliceCompare::LayerRec* a, const SliceCompare::LayerRec* b);

    // Toggles between the overlay view (A/B superimposed, diff-classified
    // colors) and the side-by-side split view (A left / B right, sharing
    // scale/pan, each pane drawing only its own layer's raw geometry).
    // Re-fits the view on toggle so the new layout starts well-framed.
    void set_side_by_side(bool on);

    // Rescales/recenters the view so the current layer(s)' content fills the
    // canvas (zoom-to-content).
    void fit_view();

private:
    // A run of consecutive same-class segments merged into a single
    // polyline point list -- a performance guard so dense layers (lots of
    // short, collinear toolpath segments) don't need one draw call per
    // segment.
    struct Polyline { std::vector<wxPoint2DDouble> pts; };

    void on_paint(wxPaintEvent& evt);
    void on_mouse_wheel(wxMouseEvent& evt);
    void on_mouse_motion(wxMouseEvent& evt);
    void on_left_down(wxMouseEvent& evt);
    void on_left_up(wxMouseEvent& evt);
    void on_capture_lost(wxMouseCaptureLostEvent& evt);
    void on_size(wxSizeEvent& evt);

    // Recomputes m_both/m_jitter/m_a_only/m_b_only from m_a/m_b.
    void rebuild_diff();

    // Union bounding box of whatever LayerRec(s) are currently set, using
    // each layer's precomputed bbox when available and falling back to a
    // scan of its segments otherwise. Returns false if there's no content.
    bool compute_content_bbox(float& bx0, float& by0, float& bx1, float& by1) const;

    // World (mm, gcode +Y-up) -> screen (px, Y-down) conversion.
    wxPoint2DDouble world_to_screen(double x, double y) const;

    void draw_polylines(wxGraphicsContext* gc, const std::vector<Polyline>& polylines,
                         const wxColour& colour, double pen_width) const;

    // Side-by-side mode: draws one pane's classified paths (shared/jitter in
    // neutral colors, this side's exclusive paths in its diff colour) clipped
    // to the pane rect [pane_x0, pane_x0+pane_w) x [0, client height), shifted
    // horizontally by x_shift screen px so the shared world->screen transform
    // recenters this pane's content within its own half. Caption is drawn
    // last, unclipped/untranslated, at the pane's top-left.
    void draw_pane(wxGraphicsContext* gc, const std::vector<Polyline>& shared,
                   const std::vector<Polyline>& jitter,
                   const std::vector<Polyline>& only, const wxColour& only_colour,
                   double pane_x0, double pane_w, double x_shift, const wxString& caption) const;

    // Splits the client rect into A-left/B-right panes, each drawn with the
    // overlay's diff classification split per side (A pane: both + a_only;
    // B pane: both + b_only), plus a divider between them.
    void draw_side_by_side(wxGraphicsContext* gc) const;

    static std::vector<Polyline> merge_collinear(const std::vector<SliceCompare::Seg>& segs);

    const SliceCompare::LayerRec* m_a = nullptr; // not owned
    const SliceCompare::LayerRec* m_b = nullptr; // not owned

    std::vector<Polyline> m_both, m_jitter, m_a_only, m_b_only;

    bool m_side_by_side    = false;
    bool m_view_initialized = false;

    // Set once a paint takes longer than the 100ms budget, so the slow-paint
    // warning below is logged only the first time it happens (not every frame).
    bool m_logged_slow_paint = false;

    double          m_scale = 4.0;      // px per mm
    wxPoint2DDouble m_pan{0.0, 0.0};    // screen-space offset of the world origin

    bool    m_dragging = false;
    wxPoint m_drag_last;
};

} // namespace GUI
} // namespace Slic3r

#endif
