#include "CompareCanvas.hpp"

#include "libslic3r/SliceCompare/Diff.hpp"

#include "slic3r/GUI/I18N.hpp"

#include <wx/dcbuffer.h>
#include <wx/graphics.h>

#include <boost/log/trivial.hpp>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>

namespace Slic3r {
namespace GUI {

namespace {

// Colours from the Slice Compare rendering spec.
// NB: deliberately not named COLOR_* -- wingdi/WinUser.h #defines a pile of
// COLOR_* macros (COLOR_BACKGROUND et al.) for GetSysColor(), and those
// leak in transitively through the wx headers.
const wxColour CANVAS_BG_COLOUR(0x11, 0x11, 0x11);
const wxColour BOTH_COLOUR(0x9E, 0x9E, 0x9E, 89);   // ~35% alpha (89/255)
const wxColour JITTER_COLOUR(0x2E, 0x7D, 0x32);
const wxColour A_ONLY_COLOUR(0x15, 0x65, 0xC0);
const wxColour B_ONLY_COLOUR(0xC6, 0x28, 0x28);

// Side-by-side mode: each pane draws its own layer's raw segments feature-
// neutral -- same gray as BOTH_COLOUR, but full alpha since there's no
// underlying "match" to fade for (each pane shows exactly one side).
const wxColour SIDE_BY_SIDE_COLOUR(0x9E, 0x9E, 0x9E);
const wxColour PANE_CAPTION_COLOUR(0xC8, 0xC8, 0xC8);
const wxColour PANE_DIVIDER_COLOUR(0x40, 0x40, 0x40);

constexpr double PEN_WIDTH_MATCH = 1.0;
constexpr double PEN_WIDTH_DIFF  = 2.0;

// Paint-time guard (Task 10): a layer whose paint exceeds this is logged
// once (not every frame) at debug level so slow dense-layer renders show up
// in logs without spamming them.
constexpr double SLOW_PAINT_BUDGET_MS = 100.0;

} // anonymous namespace

CompareCanvas::CompareCanvas(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize, wxFULL_REPAINT_ON_RESIZE)
{
    SetBackgroundStyle(wxBG_STYLE_PAINT); // avoid flicker; we paint the whole client area ourselves
    SetBackgroundColour(CANVAS_BG_COLOUR);
    SetMinSize(wxSize(200, 200));

    Bind(wxEVT_PAINT, &CompareCanvas::on_paint, this);
    Bind(wxEVT_MOUSEWHEEL, &CompareCanvas::on_mouse_wheel, this);
    Bind(wxEVT_MOTION, &CompareCanvas::on_mouse_motion, this);
    Bind(wxEVT_LEFT_DOWN, &CompareCanvas::on_left_down, this);
    Bind(wxEVT_LEFT_UP, &CompareCanvas::on_left_up, this);
    Bind(wxEVT_MOUSE_CAPTURE_LOST, &CompareCanvas::on_capture_lost, this);
    Bind(wxEVT_SIZE, &CompareCanvas::on_size, this);
}

void CompareCanvas::set_layers(const SliceCompare::LayerRec* a, const SliceCompare::LayerRec* b)
{
    m_a = a;
    m_b = b;
    rebuild_diff();
    Refresh();
}

void CompareCanvas::set_side_by_side(bool on)
{
    if (m_side_by_side == on)
        return;
    m_side_by_side = on;
    fit_view(); // re-fit for the new layout (each pane only gets half the width)
}

void CompareCanvas::rebuild_diff()
{
    SliceCompare::SegDiff diff;
    if (m_a && m_b)
        diff = SliceCompare::diff_segments(*m_a, *m_b);
    else if (m_a)
        diff.a_only = m_a->segs;
    else if (m_b)
        diff.b_only = m_b->segs;

    m_both   = merge_collinear(diff.both);
    m_jitter = merge_collinear(diff.jitter);
    m_a_only = merge_collinear(diff.a_only);
    m_b_only = merge_collinear(diff.b_only);

    // Side-by-side mode draws each side's own raw segments independent of
    // the overlay diff above (no red/blue classification in that mode).
    m_a_all = m_a ? merge_collinear(m_a->segs) : std::vector<Polyline>();
    m_b_all = m_b ? merge_collinear(m_b->segs) : std::vector<Polyline>();
}

bool CompareCanvas::compute_content_bbox(float& bx0, float& by0, float& bx1, float& by1) const
{
    bool has = false;
    auto consider = [&](const SliceCompare::LayerRec* r) {
        if (!r)
            return;
        if (r->has_bbox) {
            if (!has) {
                bx0 = r->bx0; by0 = r->by0; bx1 = r->bx1; by1 = r->by1;
                has = true;
            } else {
                bx0 = std::min(bx0, r->bx0); by0 = std::min(by0, r->by0);
                bx1 = std::max(bx1, r->bx1); by1 = std::max(by1, r->by1);
            }
            return;
        }
        for (const auto& s : r->segs) {
            const float minx = std::min(s.x0, s.x1), maxx = std::max(s.x0, s.x1);
            const float miny = std::min(s.y0, s.y1), maxy = std::max(s.y0, s.y1);
            if (!has) {
                bx0 = minx; by0 = miny; bx1 = maxx; by1 = maxy;
                has = true;
            } else {
                bx0 = std::min(bx0, minx); by0 = std::min(by0, miny);
                bx1 = std::max(bx1, maxx); by1 = std::max(by1, maxy);
            }
        }
    };
    consider(m_a);
    consider(m_b);
    return has;
}

void CompareCanvas::fit_view()
{
    const wxSize sz = GetClientSize();
    if (sz.GetWidth() <= 0 || sz.GetHeight() <= 0)
        return;

    float bx0 = 0, by0 = 0, bx1 = 0, by1 = 0;
    const bool has_bbox = compute_content_bbox(bx0, by0, bx1, by1);

    double width  = has_bbox ? double(bx1 - bx0) : 200.0;
    double height = has_bbox ? double(by1 - by0) : 200.0;
    const double cx = has_bbox ? double(bx0 + bx1) * 0.5 : 0.0;
    const double cy = has_bbox ? double(by0 + by1) * 0.5 : 0.0;

    if (width  < 1.0) width  = 1.0;
    if (height < 1.0) height = 1.0;

    constexpr double MARGIN = 0.9; // small breathing room around the content
    // In side-by-side mode each pane only occupies about half the client
    // width (see draw_side_by_side()), so fit against that instead of the
    // full canvas width -- otherwise content sized for a full-width overlay
    // view would spill past its pane's clip rect.
    const double fit_width = m_side_by_side ? std::max(1.0, (sz.GetWidth() - 2.0) / 2.0) : double(sz.GetWidth());
    double scale = std::min(fit_width / width, sz.GetHeight() / height) * MARGIN;
    if (!(scale > 0.0) || !std::isfinite(scale))
        scale = 4.0;
    m_scale = scale;

    m_pan.m_x = sz.GetWidth()  * 0.5 - cx * m_scale;
    m_pan.m_y = sz.GetHeight() * 0.5 + cy * m_scale;

    m_view_initialized = true;
    Refresh();
}

wxPoint2DDouble CompareCanvas::world_to_screen(double x, double y) const
{
    // gcode's +Y is "up"; screen Y grows downward, so it is flipped here.
    return wxPoint2DDouble(m_pan.m_x + x * m_scale, m_pan.m_y - y * m_scale);
}

std::vector<CompareCanvas::Polyline> CompareCanvas::merge_collinear(const std::vector<SliceCompare::Seg>& segs)
{
    std::vector<Polyline> result;
    if (segs.empty())
        return result;

    auto unit_dir = [](const wxPoint2DDouble& from, const wxPoint2DDouble& to) -> wxPoint2DDouble {
        const double dx = to.m_x - from.m_x, dy = to.m_y - from.m_y;
        const double len = std::hypot(dx, dy);
        return len > 1e-9 ? wxPoint2DDouble(dx / len, dy / len) : wxPoint2DDouble(0.0, 0.0);
    };

    Polyline current;
    current.pts.emplace_back(segs[0].x0, segs[0].y0);
    current.pts.emplace_back(segs[0].x1, segs[0].y1);

    for (size_t i = 1; i < segs.size(); ++i) {
        const SliceCompare::Seg& s = segs[i];
        const wxPoint2DDouble p0(s.x0, s.y0), p1(s.x1, s.y1);
        const wxPoint2DDouble last = current.pts.back();

        bool merged = false;
        if (current.pts.size() >= 2 &&
            std::fabs(last.m_x - p0.m_x) < 1e-4 && std::fabs(last.m_y - p0.m_y) < 1e-4) {
            const wxPoint2DDouble prev = current.pts[current.pts.size() - 2];
            const wxPoint2DDouble d0 = unit_dir(prev, last);
            const wxPoint2DDouble d1 = unit_dir(p0, p1);
            const double cross = d0.m_x * d1.m_y - d0.m_y * d1.m_x;
            if (std::fabs(cross) < 1e-3) {
                current.pts.push_back(p1);
                merged = true;
            }
        }

        if (!merged) {
            result.push_back(std::move(current));
            current = Polyline();
            current.pts.emplace_back(p0);
            current.pts.emplace_back(p1);
        }
    }
    result.push_back(std::move(current));
    return result;
}

void CompareCanvas::draw_polylines(wxGraphicsContext* gc, const std::vector<Polyline>& polylines,
                                    const wxColour& colour, double pen_width) const
{
    if (polylines.empty())
        return;

    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(colour, pen_width)));
    for (const auto& pl : polylines) {
        if (pl.pts.size() < 2)
            continue;
        wxGraphicsPath path = gc->CreatePath();
        const wxPoint2DDouble p0 = world_to_screen(pl.pts[0].m_x, pl.pts[0].m_y);
        path.MoveToPoint(p0.m_x, p0.m_y);
        for (size_t i = 1; i < pl.pts.size(); ++i) {
            const wxPoint2DDouble p = world_to_screen(pl.pts[i].m_x, pl.pts[i].m_y);
            path.AddLineToPoint(p.m_x, p.m_y);
        }
        gc->StrokePath(path);
    }
}

void CompareCanvas::draw_pane(wxGraphicsContext* gc, const std::vector<Polyline>& polylines,
                               double pane_x0, double pane_w, double x_shift, const wxString& caption) const
{
    gc->PushState();
    gc->Clip(pane_x0, 0.0, pane_w, double(GetClientSize().GetHeight()));
    gc->Translate(x_shift, 0.0);
    draw_polylines(gc, polylines, SIDE_BY_SIDE_COLOUR, PEN_WIDTH_MATCH);
    gc->PopState(); // restores both the clip and the translate

    gc->SetFont(gc->CreateFont(GetFont(), PANE_CAPTION_COLOUR));
    gc->DrawText(caption, pane_x0 + 6.0, 4.0);
}

void CompareCanvas::draw_side_by_side(wxGraphicsContext* gc) const
{
    const wxSize sz = GetClientSize();
    constexpr double DIVIDER_WIDTH = 2.0;
    const double pane_w   = std::max(1.0, (sz.GetWidth() - DIVIDER_WIDTH) / 2.0);
    const double paneA_x0 = 0.0;
    const double paneB_x0 = pane_w + DIVIDER_WIDTH;
    const double full_centre = sz.GetWidth() * 0.5;

    // Both panes share m_scale/m_pan (the world->screen transform computed
    // for the full canvas); each pane's x_shift just recenters that same
    // transform's output within its own half, so panning/zooming moves both
    // panes together and A/B stay directly comparable.
    draw_pane(gc, m_a_all, paneA_x0, pane_w, paneA_x0 + pane_w * 0.5 - full_centre, _L("A"));
    draw_pane(gc, m_b_all, paneB_x0, pane_w, paneB_x0 + pane_w * 0.5 - full_centre, _L("B"));

    gc->SetPen(gc->CreatePen(wxGraphicsPenInfo(PANE_DIVIDER_COLOUR, 1.0)));
    const double divider_x = paneA_x0 + pane_w + DIVIDER_WIDTH * 0.5;
    gc->StrokeLine(divider_x, 0.0, divider_x, double(sz.GetHeight()));
}

void CompareCanvas::on_paint(wxPaintEvent& /*evt*/)
{
    wxAutoBufferedPaintDC dc(this); // unbuffered DC would flicker on repeated Refresh()
    dc.SetBackground(wxBrush(CANVAS_BG_COLOUR));
    dc.Clear();

    if (!m_view_initialized)
        fit_view();

    std::unique_ptr<wxGraphicsContext> gc(wxGraphicsContext::Create(dc));
    if (!gc)
        return; // no GC backend available; leave the cleared background as-is

    gc->SetAntialiasMode(wxANTIALIAS_DEFAULT);

    const auto paint_start = std::chrono::steady_clock::now();

    if (m_side_by_side) {
        draw_side_by_side(gc.get());
    } else {
        // Draw order: both -> jitter -> a_only -> b_only, so the diffs that
        // matter most stay on top of the (mostly unchanged) shared geometry.
        draw_polylines(gc.get(), m_both,   BOTH_COLOUR,   PEN_WIDTH_MATCH);
        draw_polylines(gc.get(), m_jitter, JITTER_COLOUR, PEN_WIDTH_MATCH);
        draw_polylines(gc.get(), m_a_only, A_ONLY_COLOUR, PEN_WIDTH_DIFF);
        draw_polylines(gc.get(), m_b_only, B_ONLY_COLOUR, PEN_WIDTH_DIFF);
    }

    const double paint_ms = std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now() - paint_start).count();
    if (paint_ms > SLOW_PAINT_BUDGET_MS && !m_logged_slow_paint) {
        m_logged_slow_paint = true;
        BOOST_LOG_TRIVIAL(debug) << "CompareCanvas: layer paint took " << paint_ms
                                  << "ms, exceeding the " << SLOW_PAINT_BUDGET_MS << "ms budget"
                                  << " (dense layer? logged once)";
    }
}

void CompareCanvas::on_mouse_wheel(wxMouseEvent& evt)
{
    const int rotation = evt.GetWheelRotation();
    if (rotation == 0)
        return;

    const wxPoint pos = evt.GetPosition();
    const double world_x = (pos.x - m_pan.m_x) / m_scale;
    const double world_y = (m_pan.m_y - pos.y) / m_scale;

    const double factor = std::pow(1.0015, double(rotation)); // ~1.2x per notch (rotation +-120)
    m_scale = std::min(500.0, std::max(0.01, m_scale * factor));

    // Re-derive the pan so the world point under the cursor stays put.
    m_pan.m_x = pos.x - world_x * m_scale;
    m_pan.m_y = pos.y + world_y * m_scale;

    Refresh();
}

void CompareCanvas::on_mouse_motion(wxMouseEvent& evt)
{
    if (m_dragging && evt.Dragging() && evt.LeftIsDown()) {
        const wxPoint pos = evt.GetPosition();
        m_pan.m_x += (pos.x - m_drag_last.x);
        m_pan.m_y += (pos.y - m_drag_last.y);
        m_drag_last = pos;
        Refresh();
    }
}

void CompareCanvas::on_left_down(wxMouseEvent& evt)
{
    m_dragging  = true;
    m_drag_last = evt.GetPosition();
    if (!HasCapture())
        CaptureMouse();
}

void CompareCanvas::on_left_up(wxMouseEvent& /*evt*/)
{
    m_dragging = false;
    if (HasCapture())
        ReleaseMouse();
}

void CompareCanvas::on_capture_lost(wxMouseCaptureLostEvent& /*evt*/)
{
    m_dragging = false;
}

void CompareCanvas::on_size(wxSizeEvent& evt)
{
    Refresh();
    evt.Skip();
}

} // namespace GUI
} // namespace Slic3r
