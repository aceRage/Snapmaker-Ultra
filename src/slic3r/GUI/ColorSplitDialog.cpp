#include "ColorSplitDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "format.hpp"

#include <wx/checkbox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <string>

namespace Slic3r { namespace GUI {

static wxString mm_str(double v) { return wxString::Format("%.2f", v) + " mm"; }

ColorSplitDialog::ColorSplitDialog(wxWindow               *parent,
                                   const ColorSplitDepths &depths,
                                   const std::vector<int> &filaments,
                                   size_t                  triangle_count,
                                   size_t                  part_count,
                                   bool                    keep_base_sparse_infill_default)
    : DPIDialog(parent, wxID_ANY, _L("Split by painted colour"), wxDefaultPosition, wxDefaultSize,
                wxDEFAULT_DIALOG_STYLE)
{
    wxBoxSizer *root = new wxBoxSizer(wxVERTICAL);

    // ---- summary -------------------------------------------------------------------------------------
    wxString filament_list;
    for (size_t i = 0; i < filaments.size(); ++i)
        filament_list += (i == 0 ? "" : ", ") + wxString::Format("%d", filaments[i]);
    wxString summary = _L("Filaments") + ": " + filament_list + "\n";
    summary += _L("Triangles") + ": " + from_u8(std::to_string(triangle_count)) + "\n";
    // Ruling 27(3): every depth below is the first painted part's, but the settings behind them are per part.
    if (part_count > 1)
        summary += format_wxstr(_L("%1% painted parts (the depths below are the first part's)"), part_count) + "\n";
    summary += _L("Computed depth") + ": " +
               (depths.unlimited ? _L("unlimited (the colour goes all the way through)") : mm_str(depths.D)) + "\n";
    summary += _L("Wall stack") + ": " + mm_str(depths.ws) + "\n";
    summary += _L("Flat caps (top / bottom)") + ": " + mm_str(depths.cap_top) + " / " + mm_str(depths.cap_bottom) + "\n";
    summary += _L("Layer height") + ": " + mm_str(depths.layer_height);
    wxStaticText *summary_text = new wxStaticText(this, wxID_ANY, summary);
    root->Add(summary_text, 0, wxEXPAND | wxALL, FromDIP(10));

    // ---- depth ---------------------------------------------------------------------------------------
    wxBoxSizer *depth_sizer = new wxBoxSizer(wxHORIZONTAL);
    depth_sizer->Add(new wxStaticText(this, wxID_ANY, _L("Depth override (mm)") + ":"), 0,
                     wxALIGN_CENTER_VERTICAL | wxRIGHT, FromDIP(8));
    m_depth_ctrl = new wxTextCtrl(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxSize(FromDIP(90), -1));
    m_depth_ctrl->SetHint(_L("computed"));
    m_depth_ctrl->SetToolTip(_L("Leave empty to use the depth computed from the print settings."));
    depth_sizer->Add(m_depth_ctrl, 0, wxALIGN_CENTER_VERTICAL);
    root->Add(depth_sizer, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(10));

    // ---- options -------------------------------------------------------------------------------------
    auto add_check = [this, root](const wxString &label, bool value, const wxString &tooltip) {
        wxCheckBox *cb = new wxCheckBox(this, wxID_ANY, label);
        cb->SetValue(value);
        if (! tooltip.IsEmpty())
            cb->SetToolTip(tooltip);
        root->Add(cb, 0, wxEXPAND | wxLEFT | wxRIGHT | wxTOP, FromDIP(10));
        return cb;
    };

    m_unlimited_cb = add_check(_L("Unlimited depth"), depths.unlimited,
                               _L("Give each colour the full thickness of the part instead of a depth."));
    m_flat_cap_cb  = add_check(_L("Cap flat tops/bottoms at solid shell depth"), true,
                               _L("A wide painted top or bottom is made as deep as the solid shell instead of the wall depth."));
    m_absorb_islands_cb = add_check(_L("Absorb enclosed islands"), true,
                                    _L("Fold a fragment fully enclosed by one colour into that colour instead of leaving it separate."));
    m_keep_base_sparse_infill_cb = add_check(_L("Keep base-colour sparse infill"), keep_base_sparse_infill_default,
                                             _L("Only the walls of each colour part change filament; its sparse infill stays the body's."));
    m_solid_interfaces_cb = add_check(_L("Solid colour interfaces (interface_shells)"), true,
                                      _L("Slice a solid skin where two colours meet, so the boundary is not sparse infill."));

    // The override is what "unlimited" would ignore, so make that plain in the UI (ColorSplit.cpp:128).
    auto sync_depth_enabled = [this]() { m_depth_ctrl->Enable(! m_unlimited_cb->GetValue()); };
    m_unlimited_cb->Bind(wxEVT_CHECKBOX, [sync_depth_enabled](wxCommandEvent &evt) { sync_depth_enabled(); evt.Skip(); });
    sync_depth_enabled();

    // ---- buttons -------------------------------------------------------------------------------------
    wxSizer *buttons = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    if (buttons != nullptr)
        root->Add(buttons, 0, wxEXPAND | wxALL, FromDIP(10));
    Bind(wxEVT_BUTTON, [this](wxCommandEvent &evt) {
        if (validate())
            EndModal(wxID_OK);
    }, wxID_OK);

    SetSizer(root);
    root->SetSizeHints(this);
    Layout();
    root->Fit(this);
    wxGetApp().UpdateDlgDarkUI(this);
}

bool ColorSplitDialog::validate()
{
    m_depth_override = 0.;
    // "Unlimited depth" greys the override out and the split ignores it (ColorSplit.cpp:128), so whatever the
    // field still holds is not an answer the user gave and must not stop them.
    if (m_unlimited_cb->GetValue())
        return true;
    wxString text = m_depth_ctrl->GetValue();
    text.Trim(true).Trim(false);
    if (! text.IsEmpty()) {
        double value = 0.;
        if ((! text.ToDouble(&value) && ! text.ToCDouble(&value)) || value <= 0.) {
            show_error(this, _L("Enter a positive depth in millimetres, or leave the field empty to use the computed depth."));
            return false;
        }
        m_depth_override = value;
    }
    // paint_depth_mode "unlimited" computes no depth at all (PaintDepth.cpp:12-13), so unticking the box
    // without giving one would cut nothing - that is checked per target in Plater::split_by_color
    // (Ruling 27(3)), the only place that knows the painted parts this dialog does not show.
    return true;
}

ColorSplitParams ColorSplitDialog::params() const
{
    ColorSplitParams params;
    params.flat_cap          = m_flat_cap_cb->GetValue();
    params.absorb_islands    = m_absorb_islands_cb->GetValue();
    params.crease_step       = true;    // spec 3.6: always on, no user knob
    params.depth_override_mm = m_unlimited_cb->GetValue() ? 0. : m_depth_override;
    return params;
}

bool ColorSplitDialog::unlimited() const { return m_unlimited_cb->GetValue(); }
bool ColorSplitDialog::solid_interfaces() const { return m_solid_interfaces_cb->GetValue(); }
bool ColorSplitDialog::keep_base_sparse_infill() const { return m_keep_base_sparse_infill_cb->GetValue(); }

}} // namespace Slic3r::GUI
