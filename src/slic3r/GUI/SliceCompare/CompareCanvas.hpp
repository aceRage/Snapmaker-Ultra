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

    // Task 10: side-by-side split view. The flag is stored now; rendering
    // still overlays A/B in the same viewport regardless of its value until
    // Task 10 implements the split.
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

    static std::vector<Polyline> merge_collinear(const std::vector<SliceCompare::Seg>& segs);

    const SliceCompare::LayerRec* m_a = nullptr; // not owned
    const SliceCompare::LayerRec* m_b = nullptr; // not owned

    std::vector<Polyline> m_both, m_jitter, m_a_only, m_b_only;

    bool m_side_by_side    = false;
    bool m_view_initialized = false;

    double          m_scale = 4.0;      // px per mm
    wxPoint2DDouble m_pan{0.0, 0.0};    // screen-space offset of the world origin

    bool    m_dragging = false;
    wxPoint m_drag_last;
};

} // namespace GUI
} // namespace Slic3r

#endif
