#include "SliceBakeDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>
#include <cmath>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION    = "slice_bake";
static const char *CFG_RESULT     = "result";
static const char *CFG_CLOSE_ON   = "close_gaps";
static const char *CFG_CLOSE_R    = "close_gaps_radius";

static const double DEFAULT_CLOSE_RADIUS = 0.05; // mm, the value offered when the box is ticked

static wxString num_str(double v) { return wxString::Format("%.3g", v); }

SliceBakeSettings SliceBakeDialog::load_from_config()
{
    SliceBakeSettings s;
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        if (cfg->has(CFG_SECTION, CFG_RESULT)) {
            const std::string r = cfg->get(CFG_SECTION, CFG_RESULT);
            if (r == "add")         s.result = SliceBakeResultMode::AddNew;
            else if (r == "export") s.result = SliceBakeResultMode::ExportSTL;
            else                    s.result = SliceBakeResultMode::Replace;
        }
        bool close_on = cfg->has(CFG_SECTION, CFG_CLOSE_ON) && cfg->get(CFG_SECTION, CFG_CLOSE_ON) == "true";
        double radius = DEFAULT_CLOSE_RADIUS;
        if (cfg->has(CFG_SECTION, CFG_CLOSE_R)) {
            try {
                radius = std::stod(cfg->get(CFG_SECTION, CFG_CLOSE_R));
            } catch (...) {
                // A hand-edited config must not stop the bake.
            }
        }
        s.options.close_gaps_radius = close_on ? std::clamp(radius, 0., SLICE_BAKE_CLOSE_GAPS_MAX) : 0.;
    }
    return s;
}

SliceBakeDialog::SliceBakeDialog(wxWindow *parent, const wxString &object_name, size_t layers, size_t estimated_triangles)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Bake slice to mesh"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_object_name(object_name)
    , m_layers(layers)
    , m_estimate(estimated_triangles)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_settings = load_from_config();

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(20));

    const wxSize input_size(FromDIP(140), -1);

    // Every label is a ::Label rather than a bare wxStaticText, for the dark-mode reason spelled
    // out in FillBedDialog.cpp: a wxStaticText reports the system button face as its background
    // and UpdateDlgDarkUI() then paints a lighter grey band behind it.

    // ---- what to do with the result -----------------------------------------------------------
    auto result_label = new ::Label(this, Label::Body_14, _L("Result") + ":");
    m_result_choice = new ::ComboBox(this, wxID_ANY, wxEmptyString, wxDefaultPosition, input_size, 0, nullptr, wxCB_READONLY);
    m_result_choice->Append(_L("Replace object"));
    m_result_choice->Append(_L("Add as new object"));
    m_result_choice->Append(_L("Export STL..."));
    m_result_choice->SetSelection(int(m_settings.result));
    m_result_choice->SetToolTip(_L("Replace object: the part becomes its own baked surface, keeping its position "
                                   "and its print settings.\n\n"
                                   "Add as new object: the bake arrives beside the original, which is left alone.\n\n"
                                   "Export STL: nothing in the scene changes; the mesh is written to a file."));
    f_sizer->Add(result_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_result_choice, 0, wxALIGN_CENTER_VERTICAL);

    // ---- close small gaps ---------------------------------------------------------------------
    const wxString close_tip = _L("A fuzzed or steeply curved wall can leave hairline gaps between the printed "
                                  "widths of neighbouring loops. With this on, each layer's outline is grown by "
                                  "the radius and shrunk back again, which bridges those gaps - at the cost of "
                                  "rounding off any detail finer than the radius.");

    auto close_label = new ::Label(this, Label::Body_14, _L("Close small gaps") + ":");
    close_label->Wrap(FromDIP(300));
    close_label->SetToolTip(close_tip);
    m_close_cb = new ::CheckBox(this);
    m_close_cb->SetValue(m_settings.options.close_gaps_radius > 0.);
    m_close_cb->SetToolTip(close_tip);
    f_sizer->Add(close_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_close_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    auto radius_label = new ::Label(this, Label::Body_14, _L("Gap radius") + ":");
    radius_label->Wrap(FromDIP(300));
    m_close_input = new ::TextInput(this,
                                    num_str(m_settings.options.close_gaps_radius > 0. ? m_settings.options.close_gaps_radius
                                                                                      : DEFAULT_CLOSE_RADIUS),
                                    _L("mm"), wxEmptyString, wxDefaultPosition, input_size, wxTE_PROCESS_ENTER);
    m_close_input->GetTextCtrl()->SetFont(Label::Body_14);
    m_close_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_NUMERIC));
    m_close_input->Enable(m_close_cb->GetValue());
    f_sizer->Add(radius_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_close_input, 0, wxALIGN_CENTER_VERTICAL);

    m_close_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        m_close_input->Enable(m_close_cb->GetValue());
    });

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- the size line --------------------------------------------------------------------------
    // This is the reason the action has a dialog at all: a lofted bake is one stair-stepped band
    // per layer, so a 100 mm part at 0.2 mm is 500 bands of tens of thousands of triangles each.
    // The user gets the number BEFORE the job starts, not after it has eaten the memory.
    m_size_text = new ::Label(this, Label::Head_14, wxEmptyString);
    m_size_text->Wrap(FromDIP(340));
    v_sizer->Add(m_size_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto note_text = new ::Label(this, Label::Body_14,
                                 _L("The bake is the outer wall as it will be printed - fuzzy skin included. "
                                    "Supports, brim, skirt, the wipe tower and the infill are not baked. "
                                    "Replacing the object clears any painted supports, seams and colours, "
                                    "and the plate has to be sliced again afterwards."));
    note_text->Wrap(FromDIP(340));
    // Light-mode tone only; UpdateDlgDarkUI() maps it for dark mode, as in FillBedDialog.
    note_text->SetForegroundColour(wxColour("#6B6B6B"));
    v_sizer->Add(note_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    // ---- buttons --------------------------------------------------------------------------------
    auto dlg_btns = new DialogButtons(this, {"OK", "Cancel"});
    dlg_btns->GetOK()->SetLabel(_L("Bake"));
    dlg_btns->GetOK()->Bind(wxEVT_BUTTON, [this](wxCommandEvent &e) {
        m_settings = current_settings();
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

SliceBakeSettings SliceBakeDialog::current_settings() const
{
    SliceBakeSettings s = m_settings;
    if (m_result_choice != nullptr) {
        const int sel = m_result_choice->GetSelection();
        s.result = sel == 1 ? SliceBakeResultMode::AddNew
                            : (sel == 2 ? SliceBakeResultMode::ExportSTL : SliceBakeResultMode::Replace);
    }
    const bool close_on = m_close_cb != nullptr && m_close_cb->GetValue();
    s.options.close_gaps_radius =
        close_on ? std::clamp(read_mm(m_close_input, DEFAULT_CLOSE_RADIUS), 0., SLICE_BAKE_CLOSE_GAPS_MAX) : 0.;
    // Phase 1 bakes every layer; the range picker is deferred (spec section 4).
    s.options.layer_begin = 0;
    s.options.layer_end   = std::numeric_limits<size_t>::max();
    return s;
}

double SliceBakeDialog::read_mm(TextInput *input, double fallback) const
{
    if (input == nullptr)
        return fallback;
    double   v   = 0.;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToDouble(&v))
        return fallback;
    return v;
}

void SliceBakeDialog::update_preview()
{
    if (m_size_text == nullptr)
        return;

    // Rounded to two significant figures: the count depends on the tesselation of every layer's
    // free-top and overhang deltas, which is not knowable without running the bake, so anything
    // finer would be false precision.
    double est = double(m_estimate);
    if (est >= 10.) {
        const double mag = std::pow(10., std::floor(std::log10(est)) - 1.);
        est = std::round(est / mag) * mag;
    }
    m_size_text->SetLabel(wxString::Format(_L("%llu layers, about %llu triangles"),
                                           (unsigned long long) m_layers,
                                           (unsigned long long) std::llround(est)));
    m_size_text->Wrap(FromDIP(340));

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void SliceBakeDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    const char *r = m_settings.result == SliceBakeResultMode::AddNew ? "add"
                  : (m_settings.result == SliceBakeResultMode::ExportSTL ? "export" : "replace");
    cfg->set(CFG_SECTION, CFG_RESULT, std::string(r));
    cfg->set(CFG_SECTION, CFG_CLOSE_ON, m_settings.options.close_gaps_radius > 0.);
    if (m_settings.options.close_gaps_radius > 0.)
        cfg->set(CFG_SECTION, CFG_CLOSE_R, num_str(m_settings.options.close_gaps_radius).ToStdString());
}

void SliceBakeDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
