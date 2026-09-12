#include "QuadRemeshDialog.hpp"

#include "GUI_App.hpp"
#include "I18N.hpp"
#include "MainFrame.hpp"

#include "libslic3r/AppConfig.hpp"

#include <wx/sizer.h>
#include <wx/valtext.h>

#include <algorithm>

namespace Slic3r { namespace GUI {

static const char *CFG_SECTION = "quad_remesh";
static const char *CFG_TARGET  = "target_faces";
static const char *CFG_SHARP   = "preserve_sharp";

QuadRemeshOptions QuadRemeshDialog::load_from_config(int default_target)
{
    QuadRemeshOptions o;
    o.target_faces = default_target;

    AppConfig *cfg = wxGetApp().app_config;
    if (cfg != nullptr) {
        if (cfg->has(CFG_SECTION, CFG_TARGET)) {
            try {
                o.target_faces = std::stoi(cfg->get(CFG_SECTION, CFG_TARGET));
            } catch (...) {
                // A hand-edited or truncated config must not stop the remesh.
            }
        }
        if (cfg->has(CFG_SECTION, CFG_SHARP))
            o.preserve_sharp = cfg->get(CFG_SECTION, CFG_SHARP) == "true";
    }

    // Clamp on load against the same limits libslic3r enforces, so a config written by
    // a future build (or by hand) can never hand the geometry a value it rejects.
    o.target_faces = std::clamp(o.target_faces, QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX);
    // The seed is deliberately NOT persisted or exposed: it exists so the result is
    // reproducible, and a user-visible knob would only be a way to break that.
    o.seed = QUAD_REMESH_DEFAULT_SEED;
    return o;
}

QuadRemeshDialog::QuadRemeshDialog(wxWindow          *parent,
                                   int                default_target,
                                   size_t             triangles_before,
                                   const std::string &refusal)
    : DPIDialog(parent ? parent : static_cast<wxWindow *>(wxGetApp().mainframe),
                wxID_ANY,
                _L("Quad remesh"),
                wxDefaultPosition,
                wxDefaultSize,
                wxCAPTION | wxCLOSE_BOX)
    , m_default_target(default_target > 0 ? default_target : QUAD_REMESH_TARGET_MIN)
    , m_triangles_before(triangles_before)
    , m_refusal(refusal)
{
    SetBackgroundColour(*wxWHITE);
    SetFont(Label::Body_14);

    m_options = load_from_config(m_default_target);

    auto v_sizer = new wxBoxSizer(wxVERTICAL);
    auto f_sizer = new wxFlexGridSizer(2, 2, FromDIP(4), FromDIP(20));

    const wxSize input_size(FromDIP(120), -1);

    // Every label is a ::Label rather than a bare wxStaticText, for the dark-mode
    // reason spelled out in FillBedDialog.cpp: a wxStaticText reports the system
    // button face as its background and UpdateDlgDarkUI() then paints a lighter grey
    // band behind it.

    // ---- target faces -------------------------------------------------------------------------
    auto target_label = new ::Label(this, Label::Body_14, _L("Target faces") + ":");
    m_target_input    = new ::TextInput(this, wxString::Format("%d", m_options.target_faces),
                                        wxEmptyString, wxEmptyString, wxDefaultPosition, input_size,
                                        wxTE_PROCESS_ENTER);
    m_target_input->GetTextCtrl()->SetFont(Label::Body_14);
    m_target_input->GetTextCtrl()->SetValidator(wxTextValidator(wxFILTER_DIGITS));
    m_target_input->GetTextCtrl()->Bind(wxEVT_TEXT, [this](wxCommandEvent &e) {
        e.Skip();
        update_preview();
    });
    m_target_input->SetToolTip(wxString::Format(
        _L("Roughly how many quads to produce. The remesher places its irregular points by "
           "solving over the whole surface, so it comes close to this number rather than "
           "hitting it exactly.\n\nThe default, %d, is half this part's triangle count - two "
           "triangles make a quad, so it keeps the part at about its present density."),
        m_default_target));
    m_target_input->GetTextCtrl()->SetFocus();
    f_sizer->Add(target_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_target_input, 0, wxALIGN_CENTER_VERTICAL);

    // ---- preserve sharp features --------------------------------------------------------------
    const wxString sharp_tip = _L("Line the quads up with the part's hard edges, so a corner stays "
                                  "a corner instead of being smoothed across. Worth leaving on for "
                                  "anything mechanical; it makes little difference on an organic "
                                  "shape that has no hard edges to find.");

    auto sharp_label = new ::Label(this, Label::Body_14, _L("Preserve sharp features") + ":");
    sharp_label->Wrap(FromDIP(300));
    sharp_label->SetToolTip(sharp_tip);
    m_sharp_cb = new ::CheckBox(this);
    m_sharp_cb->SetValue(m_options.preserve_sharp);
    m_sharp_cb->SetToolTip(sharp_tip);
    m_sharp_cb->Bind(wxEVT_TOGGLEBUTTON, [this](wxCommandEvent &e) {
        e.Skip();
        update_preview();
    });
    f_sizer->Add(sharp_label, 0, wxEXPAND | wxALIGN_CENTER_VERTICAL);
    f_sizer->Add(m_sharp_cb, 0, wxALIGN_CENTER_VERTICAL | wxTOP | wxBOTTOM, FromDIP(5));

    v_sizer->Add(f_sizer, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- triangle / quad count ----------------------------------------------------------------
    m_preview_text = new ::Label(this, Label::Head_14, wxEmptyString);
    m_preview_text->Wrap(FromDIP(320));
    v_sizer->Add(m_preview_text, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, FromDIP(10));

    auto note_text = new ::Label(this, Label::Body_14,
                                 _L("The part is stored as triangles, two per quad. Painted "
                                    "supports, seams and colours are cleared by a remesh."));
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
    // A part the remesher will refuse: the controls stay readable but there is nothing
    // to press. update_preview() puts the reason where the estimate would be.
    if (!m_refusal.empty()) {
        dlg_btns->GetOK()->Disable();
        m_target_input->Disable();
        m_sharp_cb->Disable();
    }
    v_sizer->Add(dlg_btns, 0, wxEXPAND);

    update_preview();

    this->SetSizer(v_sizer);
    this->Layout();
    v_sizer->Fit(this);
    CenterOnParent();
    wxGetApp().UpdateDlgDarkUI(this);
}

QuadRemeshOptions QuadRemeshDialog::current_options() const
{
    QuadRemeshOptions o = m_options;
    o.target_faces  = std::clamp(read_int(m_target_input, m_options.target_faces),
                                 QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX);
    o.preserve_sharp = m_sharp_cb != nullptr ? m_sharp_cb->GetValue() : m_options.preserve_sharp;
    o.seed           = QUAD_REMESH_DEFAULT_SEED;
    return o;
}

int QuadRemeshDialog::read_int(TextInput *input, int fallback) const
{
    if (input == nullptr)
        return fallback;
    long     v   = 0;
    wxString txt = input->GetTextCtrl()->GetValue();
    if (txt.empty() || !txt.ToLong(&v))
        return fallback;
    return int(std::clamp<long>(v, QUAD_REMESH_TARGET_MIN, QUAD_REMESH_TARGET_MAX));
}

void QuadRemeshDialog::update_preview()
{
    if (m_preview_text == nullptr)
        return;

    if (!m_refusal.empty()) {
        // Say why the button is dead, in the place the user is already looking.
        m_preview_text->SetLabel(_L("This part cannot be quad remeshed: ") + from_u8(m_refusal));
    } else {
        // Unlike the voxel dialog, the relationship here is exact rather than
        // estimated: the target IS the quad count the remesher aims for, and the
        // stored mesh is two triangles per quad. Still worded as "about", because
        // the solver lands near the target rather than on it.
        const int target = read_int(m_target_input, m_options.target_faces);
        m_preview_text->SetLabel(wxString::Format(
            _L("Triangles: %llu -> about %llu (about %d quads)"),
            (unsigned long long) m_triangles_before,
            (unsigned long long) (long long) target * 2,
            target));
    }
    m_preview_text->Wrap(FromDIP(320));

    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
}

void QuadRemeshDialog::save_to_config() const
{
    AppConfig *cfg = wxGetApp().app_config;
    if (cfg == nullptr)
        return;
    cfg->set(CFG_SECTION, CFG_TARGET, std::to_string(m_options.target_faces));
    cfg->set(CFG_SECTION, CFG_SHARP, m_options.preserve_sharp);
}

void QuadRemeshDialog::on_dpi_changed(const wxRect &suggested_rect)
{
    Layout();
    if (GetSizer() != nullptr)
        GetSizer()->Fit(this);
    Refresh();
}

}} // namespace Slic3r::GUI
