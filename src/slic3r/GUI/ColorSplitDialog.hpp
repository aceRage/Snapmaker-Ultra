#ifndef slic3r_GUI_ColorSplitDialog_hpp_
#define slic3r_GUI_ColorSplitDialog_hpp_

// "Split by painted colour": the modal that owns the split's optional refinements (spec 5).
// Spec: docs/superpowers/specs/2026-09-01-color-split-design.md

#include "GUI_Utils.hpp"

#include "libslic3r/ColorSplit.hpp"

#include <cstddef>
#include <vector>

class wxCheckBox;
class wxTextCtrl;

namespace Slic3r { namespace GUI {

class ColorSplitDialog : public DPIDialog
{
public:
    // `depths` are the computed WORLD-millimetre depths of the FIRST painted part, shown as they are;
    // `part_count` is how many painted parts the action found (Ruling 27(3): the dialog says so when there is
    // more than one, and Plater::split_by_color checks the depths of the ones not shown); `filaments` are the
    // 1-based ids the split will produce a part for; `keep_base_sparse_infill_default` is the object's
    // paint_infill_override inverted.
    ColorSplitDialog(wxWindow                *parent,
                     const ColorSplitDepths  &depths,
                     const std::vector<int>  &filaments,
                     size_t                   triangle_count,
                     size_t                   part_count,
                     bool                     keep_base_sparse_infill_default);

    // Valid once ShowModal() returned wxID_OK.
    ColorSplitParams params() const;
    // The dialog's "Unlimited depth": overrides ColorSplitDepths::unlimited on every target.
    bool unlimited() const;
    bool solid_interfaces() const;
    bool keep_base_sparse_infill() const;

protected:
    void on_dpi_changed(const wxRect &suggested_rect) override {}

private:
    bool validate();

    double m_depth_override = 0.;

    wxTextCtrl *m_depth_ctrl                 = nullptr;
    wxCheckBox *m_unlimited_cb               = nullptr;
    wxCheckBox *m_flat_cap_cb                = nullptr;
    wxCheckBox *m_absorb_islands_cb          = nullptr;
    wxCheckBox *m_keep_base_sparse_infill_cb = nullptr;
    wxCheckBox *m_solid_interfaces_cb        = nullptr;
};

}} // namespace Slic3r::GUI

#endif // slic3r_GUI_ColorSplitDialog_hpp_
