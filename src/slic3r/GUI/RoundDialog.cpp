#include "RoundDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION = "round_edges";
static const char *CFG_RADIUS  = "radius";
static const char *CFG_VOXEL   = "voxel_size";
static const char *CFG_CONCAVE = "round_concave";
static const char *CFG_FLAT    = "keep_bottom_flat";
static const char *CFG_MARGIN  = "bottom_margin";

static wxString num_str(double v) { return wxString::Format("%.3g", v); }

RoundOptions RoundDialog::load_from_config()
{
    RoundOptions o;

    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        auto read_double = [cfg](const char *key, double &out) {
            if (!cfg->has(CFG_SECTION, key))
                return;
            try {
                out = std::stod(cfg->get(CFG_SECTION, key));
            } catch (...) {
                // A hand-edited or truncated config must not stop the operation.
            }
        };
        read_double(CFG_RADIUS, o.radius);
        read_double(CFG_VOXEL, o.voxel_size);
        read_double(CFG_MARGIN, o.bottom_margin);
        if (cfg->has(CFG_SECTION, CFG_CONCAVE))
            o.round_concave = cfg->get(CFG_SECTION, CFG_CONCAVE) == "true";
        if (cfg->has(CFG_SECTION, CFG_FLAT))
            o.keep_bottom_flat = cfg->get(CFG_SECTION, CFG_FLAT) == "true";
    }

    // Clamp on load against the same limits libslic3r enforces, so a config written
    // by a future build (or by hand) can never hand the geometry a value it rejects.
    o.radius        = std::clamp(o.radius, ROUND_RADIUS_MIN, ROUND_RADIUS_MAX);
    o.bottom_margin = std::clamp(o.bottom_margin, 0., ROUND_BOTTOM_MARGIN_MAX);
    // 0 stays 0 - it means "derive it from the radius" and must survive the round trip.
    if (o.voxel_size > 0.)
        o.voxel_size = std::clamp(o.voxel_size, ROUND_VOXEL_MIN, ROUND_VOXEL_MAX);
    return o;
}

RoundDialog::RoundDialog(wxWindow *parent, double bbox_min_extent, size_t triangles_before, double surface_area)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Round all edges"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_bbox_min_extent(bbox_min_extent)
    , m_triangles_before(triangles_before)
    , m_surface_area(surface_area)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_options = load_from_config();

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(20));

    const wxSize input_size(FromDIP(120), -1);

    auto make_input = [this, input_size](double value, const wxString &unit) {
        auto *in = new ::TextInput(this, num_str(value), unit, wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
        in->GetTextCtrl()->SetFont(Label::Body_14);
        in->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
        in->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
            e.Skip();
            update_preview();
        });
        return in;
    };

    // Every label is a ::Label rather than a bare wxStaticText, for the dark-mode
    // reason spelled out in FillBedDialog.cpp.

    // ---- radius -------------------------------------------------------------------------------
    auto radius_label = new ::Label(this, Label::Body_14, _L("Radius") + ":");
    m_radius_input    = make_input(m_options.radius, _L("mm"));
    m_radius_input->SetToolTip(_L("The radius of the fillet put on every edge - think of it as a "
                                  "ball of this size rolled over the whole part. A feature thinner "
                                  "than twice the radius cannot survive it, so keep the radius well "
                                  "under the smallest wall you want to keep."));
    m_radius_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(radius_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_radius_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- voxel size ---------------------------------------------------------------------------
    auto voxel_label = new ::Label(this, Label::Body_14, _L("Voxel size") + ":");
    m_voxel_input    = make_input(m_options.voxel_size > 0. ? m_options.voxel_size
                                                            : round_auto_voxel_size(m_options.radius),
                                  _L("mm"));
    m_voxel_input->SetToolTip(_L("How finely the rounding is computed. The part is rebuilt as the "
                                 "surface of its distance field on a grid of this spacing, so detail "
                                 "smaller than one voxel is lost and a smaller value means more "
                                 "triangles and a slower operation.\n\nLeave it alone and it follows "
                                 "the radius, which is almost always what you want."));
    f_sizer->Add(voxel_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_voxel_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- concave edges ------------------------------------------------------------------------
    const wxString concave_tip = _L("With this on, inside corners are filleted as well as outside "
                                    "ones - the inside of an L, a pocket, a slot. Turn it off to "
                                    "round only the outside edges and leave inside corners crisp.");

    auto concave_label = new ::Label(this, Label::Body_14, _L("Round inside corners too") + ":");
    concave_label->Wrap(FromDIP(300));
    concave_label->SetToolTip(concave_tip);
    m_concave_cb = new ::CheckBox(this);
    m_concave_cb->SetValue(m_options.round_concave);
    m_concave_cb->SetToolTip(concave_tip);
    f_sizer->Add(concave_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_concave_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    // ---- keep the bottom flat -----------------------------------------------------------------
    const wxString flat_tip = _L("Rounding every edge includes the one where the part meets the "
                                 "bed, which lifts the outline off the plate and leaves the first "
                                 "layer unsupported. With this on, the part is cut just above its "
                                 "base, only the upper piece is rounded, and the untouched bottom "
                                 "slab is joined back on - so the base comes through exactly as it "
                                 "went in and only the edges above it are filleted.\n\n"
                                 "Parts with no flat base are rounded whole, as if it were off.");

    auto flat_label = new ::Label(this, Label::Body_14, _L("Keep the bottom flat") + ":");
    flat_label->Wrap(FromDIP(300));
    flat_label->SetToolTip(flat_tip);
    m_flat_cb = new ::CheckBox(this);
    m_flat_cb->SetValue(m_options.keep_bottom_flat);
    m_flat_cb->SetToolTip(flat_tip);
    f_sizer->Add(flat_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_flat_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto margin_label = new ::Label(this, Label::Body_14, _L("Bottom slab height") + ":");
    margin_label->Wrap(FromDIP(300));
    m_margin_input = make_input(m_options.bottom_margin > 0. ? m_options.bottom_margin
                                                             : ROUND_BOTTOM_MARGIN_RADII * m_options.radius,
                                _L("mm"));
    m_margin_input->SetToolTip(_L("How much of the original base is kept untouched. It has to clear "
                                  "the fillet itself, so leave it at about one and a half times the "
                                  "radius: any thinner and the cut runs through the rounding, any "
                                  "thicker and a sloped wall picks up a visible seam where the two "
                                  "pieces meet."));
    m_margin_input->Enable(m_options.keep_bottom_flat);
    f_sizer->Add(margin_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_margin_input, 0, wxALIGN_CENTER_VERTICAL);

    m_flat_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_margin_input->Enable(m_flat_cb->GetValue());
        update_preview();
    });
    m_concave_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        update_preview();
    });

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- triangle count -----------------------------------------------------------------------
    m_preview_text = new ::Label(this, Label::Head_14, wxEmptyString);
    m_preview_text->Wrap(FromDIP(320));
    v_sizer->Add(m_preview_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- the "radius does not fit" warning ----------------------------------------------------
    m_warning_text = new ::Label(this, Label::Body_14, wxEmptyString);
    m_warning_text->Wrap(FromDIP(320));
    m_warning_text->SetForegroundColour(wxColour("#D9534F"));
    v_sizer->Add(m_warning_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto note_text = new ::Label(this, Label::Body_14,
                                 _L("The part is rebuilt, so painted supports, seams and colours are cleared."));
    note_text->Wrap(FromDIP(320));
    note_text->SetForegroundColour(wxColour("#6B6B6B"));
    v_sizer->Add(note_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons ------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Round"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_options = current_options();
        save_to_config();
        EndModal(wxID_OK);
    });
    dlg_btns->GetCANCEL()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) { EndModal(wxID_CANCEL); });
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    update_preview();

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

RoundOptions RoundDialog::current_options() const
{
    RoundOptions o = m_options;
    o.radius = std::clamp(read_mm(m_radius_input, m_options.radius), ROUND_RADIUS_MIN, ROUND_RADIUS_MAX);
    // An empty or unreadable voxel field means "auto", which libslic3r spells 0.
    const double voxel = read_mm(m_voxel_input, 0.);
    o.voxel_size = voxel > 0. ? std::clamp(voxel, ROUND_VOXEL_MIN, ROUND_VOXEL_MAX) : 0.;
    o.round_concave    = m_concave_cb != nullptr ? m_concave_cb->GetValue() : m_options.round_concave;
    o.keep_bottom_flat = m_flat_cb != nullptr ? m_flat_cb->GetValue() : m_options.keep_bottom_flat;
    o.bottom_margin    = std::clamp(read_mm(m_margin_input, m_options.bottom_margin), 0., ROUND_BOTTOM_MARGIN_MAX);
    return o;
}

double RoundDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void RoundDialog::update_preview()
{
    const double radius = std::clamp(read_mm(m_radius_input, m_options.radius), ROUND_RADIUS_MIN, ROUND_RADIUS_MAX);
    double       voxel  = read_mm(m_voxel_input, 0.);
    if (voxel <= 0.)
        voxel = round_auto_voxel_size(radius);
    voxel = std::clamp(voxel, ROUND_VOXEL_MIN, std::min(ROUND_VOXEL_MAX, radius / 2.));

    if (m_preview_text != nullptr) {
        // The same order-of-magnitude estimate RemeshDialog shows, and for the same
        // reason: an exact figure would mean running the whole operation to draw a
        // label. Marching cubes emits roughly two triangles per voxel-sized patch.
        if (m_triangles_before == 0 || m_surface_area <= 0. || voxel <= 0.) {
            m_preview_text->SetLabel(wxString::Format(_L("Triangles: %llu"), (unsigned long long) m_triangles_before));
        } else {
            double est = 2. * m_surface_area / (voxel * voxel);
            if (est >= 10.) {
                const double mag = std::pow(10., std::floor(std::log10(est)) - 1.);
                est = std::round(est / mag) * mag;
            }
            m_preview_text->SetLabel(wxString::Format(_L("Triangles: %llu -> about %llu"),
                                                      (unsigned long long) m_triangles_before,
                                                      (unsigned long long) std::llround(est)));
        }
        m_preview_text->Wrap(FromDIP(320));
    }

    if (m_warning_text != nullptr) {
        // A rolling ball of radius r cannot round a part whose shortest side is under
        // 2r - it would consume the whole thing. Say so before the operation runs
        // rather than reporting a failure afterwards.
        wxString warn;
        if (m_bbox_min_extent > 0. && radius * 2. >= m_bbox_min_extent)
            warn = wxString::Format(_L("A radius of %s mm is too large for this part: its shortest "
                                       "side is %s mm and the rounding needs twice the radius to fit."),
                                    num_str(radius), num_str(m_bbox_min_extent));
        m_warning_text->SetLabel(warn);
        m_warning_text->Show(!warn.empty());
        m_warning_text->Wrap(FromDIP(320));
    }

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void RoundDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(CFG_SECTION, CFG_RADIUS, num_str(m_options.radius).ToStdString());
    cfg->set(CFG_SECTION, CFG_VOXEL, num_str(m_options.voxel_size).ToStdString());
    cfg->set(CFG_SECTION, CFG_CONCAVE, m_options.round_concave);
    cfg->set(CFG_SECTION, CFG_FLAT, m_options.keep_bottom_flat);
    cfg->set(CFG_SECTION, CFG_MARGIN, num_str(m_options.bottom_margin).ToStdString());
}

void RoundDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
