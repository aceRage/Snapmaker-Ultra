#include "RemeshDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION   = "remesh";
static const char *CFG_VOXEL     = "voxel_size";
static const char *CFG_FLAT      = "keep_bottom_flat";
static const char *CFG_MARGIN    = "bottom_margin";
static const char *CFG_SHARP     = "preserve_sharp_edges";
static const char *CFG_ANGLE     = "sharp_feature_angle";

static wxString num_str(double v) { return wxString::Format("%.3g", v); }

RemeshOptions RemeshDialog::load_from_config(double auto_voxel)
{
    RemeshOptions o;
    o.voxel_size    = auto_voxel;
    o.bottom_margin = 0.; // 0 = derive from the voxel size

    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        auto read_double = [cfg](const char *key, double &out) {
            if (!cfg->has(CFG_SECTION, key))
                return;
            try {
                out = std::stod(cfg->get(CFG_SECTION, key));
            } catch (...) {
                // A hand-edited or truncated config must not stop the repair.
            }
        };
        read_double(CFG_VOXEL, o.voxel_size);
        read_double(CFG_MARGIN, o.bottom_margin);
        read_double(CFG_ANGLE, o.sharp_feature_angle);
        if (cfg->has(CFG_SECTION, CFG_FLAT))
            o.keep_bottom_flat = cfg->get(CFG_SECTION, CFG_FLAT) == "true";
        if (cfg->has(CFG_SECTION, CFG_SHARP))
            o.preserve_sharp_edges = cfg->get(CFG_SECTION, CFG_SHARP) == "true";
    }

    // Clamp on load, against the same limits libslic3r enforces, so a config written
    // by a future build (or by hand) can never hand the geometry a value it rejects.
    o.voxel_size          = std::clamp(o.voxel_size, REMESH_VOXEL_MIN, REMESH_VOXEL_MAX);
    o.sharp_feature_angle = std::clamp(o.sharp_feature_angle, REMESH_ANGLE_MIN, REMESH_ANGLE_MAX);
    o.bottom_margin       = std::clamp(o.bottom_margin, 0., REMESH_BOTTOM_MARGIN_MAX);
    return o;
}

RemeshDialog::RemeshDialog(wxWindow *parent, double auto_voxel, size_t triangles_before, double surface_area)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Repair by remeshing"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_auto_voxel(auto_voxel > 0. ? auto_voxel : 0.1)
    , m_triangles_before(triangles_before)
    , m_surface_area(surface_area)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_options = load_from_config(m_auto_voxel);

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
    // reason spelled out in FillBedDialog.cpp: a wxStaticText reports the system
    // button face as its background and UpdateDlgDarkUI() then paints a lighter grey
    // band behind it.

    // ---- voxel size ---------------------------------------------------------------------------
    auto voxel_label = new ::Label(this, Label::Body_14, _L("Voxel size") + ":");
    m_voxel_input    = make_input(m_options.voxel_size, _L("mm"));
    m_voxel_input->SetToolTip(wxString::Format(
        _L("How coarse the rebuild is. The mesh is re-created as the surface of its distance "
           "field on a grid of this spacing, so detail smaller than one voxel is lost and a "
           "smaller value means more triangles and a slower repair.\n\nThe value for this part "
           "chosen automatically is %s mm."),
        num_str(m_auto_voxel)));
    m_voxel_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(voxel_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_voxel_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- keep the bottom flat -----------------------------------------------------------------
    const wxString flat_tip = _L("Voxel remeshing reconstructs the surface on a fixed grid, which "
                                 "leaves a flat base rippled and rounds off its outline by up to a "
                                 "voxel - the first layer is then no longer flush with the bed. "
                                 "With this on, the part is cut just above its base, only the "
                                 "upper piece is remeshed, and the untouched bottom slab is joined "
                                 "back on, so the base comes through exactly as it went in.\n\n"
                                 "Parts with no flat base are remeshed whole, as before.");

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
                                                             : REMESH_BOTTOM_MARGIN_VOXELS * m_options.voxel_size,
                                _L("mm"));
    m_margin_input->SetToolTip(_L("How much of the original base is kept untouched. Leave it at a "
                                  "couple of voxels: too thin and the cut runs through the "
                                  "rounding the remesh would have added anyway, too thick and a "
                                  "sloped wall picks up a visible seam where the two pieces meet."));
    m_margin_input->Enable(m_options.keep_bottom_flat);
    f_sizer->Add(margin_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_margin_input, 0, wxALIGN_CENTER_VERTICAL);

    m_flat_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_margin_input->Enable(m_flat_cb->GetValue());
        update_preview();
    });

    // ---- preserve sharp edges -----------------------------------------------------------------
    const wxString sharp_tip = _L("The distance field averages both faces together within about a "
                                  "voxel of a hard edge, so corners come back filleted. With this "
                                  "on, the edges of the original part are found first, and any "
                                  "rebuilt vertex that lands within a voxel of one is pulled back "
                                  "onto it.");

    auto sharp_label = new ::Label(this, Label::Body_14, _L("Preserve sharp edges") + ":");
    sharp_label->Wrap(FromDIP(300));
    sharp_label->SetToolTip(sharp_tip);
    m_sharp_cb = new ::CheckBox(this);
    m_sharp_cb->SetValue(m_options.preserve_sharp_edges);
    m_sharp_cb->SetToolTip(sharp_tip);
    f_sizer->Add(sharp_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_sharp_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto angle_label = new ::Label(this, Label::Body_14, _L("Feature angle") + ":");
    angle_label->Wrap(FromDIP(300));
    m_angle_input = make_input(m_options.sharp_feature_angle, _L("deg"));
    m_angle_input->SetToolTip(_L("Two faces meeting at more than this angle count as a hard edge. "
                                 "Lower values protect gentler creases as well; higher values "
                                 "protect only the crispest corners."));
    m_angle_input->Enable(m_options.preserve_sharp_edges);
    f_sizer->Add(angle_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_angle_input, 0, wxALIGN_CENTER_VERTICAL);

    m_sharp_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_angle_input->Enable(m_sharp_cb->GetValue());
        update_preview();
    });

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- triangle count -----------------------------------------------------------------------
    m_preview_text = new ::Label(this, Label::Head_14, wxEmptyString);
    m_preview_text->Wrap(FromDIP(320));
    v_sizer->Add(m_preview_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto note_text = new ::Label(this, Label::Body_14,
                                 _L("Painted supports, seams and colours are cleared by a remesh."));
    note_text->Wrap(FromDIP(320));
    // Light-mode tone only; UpdateDlgDarkUI() maps it for dark mode, as in FillBedDialog.
    note_text->SetForegroundColour(wxColour("#6B6B6B"));
    v_sizer->Add(note_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons ------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Remesh"));
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

RemeshOptions RemeshDialog::current_options() const
{
    RemeshOptions o = m_options;
    o.voxel_size = std::clamp(read_mm(m_voxel_input, m_options.voxel_size), REMESH_VOXEL_MIN, REMESH_VOXEL_MAX);
    o.keep_bottom_flat = m_flat_cb != nullptr ? m_flat_cb->GetValue() : m_options.keep_bottom_flat;
    o.bottom_margin = std::clamp(read_mm(m_margin_input, m_options.bottom_margin), 0., REMESH_BOTTOM_MARGIN_MAX);
    o.preserve_sharp_edges = m_sharp_cb != nullptr ? m_sharp_cb->GetValue() : m_options.preserve_sharp_edges;
    o.sharp_feature_angle = std::clamp(read_mm(m_angle_input, m_options.sharp_feature_angle), REMESH_ANGLE_MIN, REMESH_ANGLE_MAX);
    return o;
}

double RemeshDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void RemeshDialog::update_preview()
{
    if (m_preview_text == nullptr)
        return;

    // Only the "before" count is known for certain - an exact "after" would mean
    // running the whole remesh just to draw a label, which the spec explicitly rules
    // out. What IS cheap is the relationship the marching-cubes extraction obeys:
    // the output has roughly two triangles per voxel-sized patch of surface, so
    // area / voxel^2 * 2 is a usable order-of-magnitude figure. It is labelled as an
    // estimate and rounded hard, so it never reads as a promise.
    const double voxel = std::clamp(read_mm(m_voxel_input, m_options.voxel_size), REMESH_VOXEL_MIN, REMESH_VOXEL_MAX);
    if (m_triangles_before == 0 || m_surface_area <= 0. || voxel <= 0.) {
        m_preview_text->SetLabel(wxString::Format(_L("Triangles: %llu"), (unsigned long long) m_triangles_before));
    } else {
        double est = 2. * m_surface_area / (voxel * voxel);
        // Round to two significant figures - anything finer would be false precision.
        if (est >= 10.) {
            const double mag = std::pow(10., std::floor(std::log10(est)) - 1.);
            est = std::round(est / mag) * mag;
        }
        m_preview_text->SetLabel(wxString::Format(_L("Triangles: %llu -> about %llu"),
                                                  (unsigned long long) m_triangles_before,
                                                  (unsigned long long) std::llround(est)));
    }
    m_preview_text->Wrap(FromDIP(320));

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void RemeshDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(CFG_SECTION, CFG_VOXEL, num_str(m_options.voxel_size).ToStdString());
    cfg->set(CFG_SECTION, CFG_FLAT, m_options.keep_bottom_flat);
    cfg->set(CFG_SECTION, CFG_MARGIN, num_str(m_options.bottom_margin).ToStdString());
    cfg->set(CFG_SECTION, CFG_SHARP, m_options.preserve_sharp_edges);
    cfg->set(CFG_SECTION, CFG_ANGLE, num_str(m_options.sharp_feature_angle).ToStdString());
}

void RemeshDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
