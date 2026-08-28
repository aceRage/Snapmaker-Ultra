#ifndef slic3r_GUI_PrinterModelPanel_hpp_
#define slic3r_GUI_PrinterModelPanel_hpp_

#include <set>
#include <wx/panel.h>
#include <wx/bitmap.h>
#include "slic3r/GUI/PresetComboBoxes.hpp"
#include "slic3r/GUI/wxExtensions.hpp"

namespace Slic3r { namespace GUI {

class PrinterModelPanel : public wxPanel
{
public:
    PrinterModelPanel(wxWindow *parent);

    void setup(PlaterPresetComboBox *printerCmb, ScalableButton *editBtn, ScalableButton *connectionBtn);

    void updatePrinterIcon();

private:
    void onPaint(wxPaintEvent &event);

    void onLeave(wxMouseEvent &event);

    void onMotion(wxMouseEvent &event);

    void onLeftDown(wxMouseEvent &event);

    void onPrintCmbSetFocus(wxFocusEvent &event);

    void onPrintCmbKillFocus(wxFocusEvent &event);

    void onButtonClicked(wxCommandEvent &event);

    void onActivateApp(wxActivateEvent &event);

private:
    std::string           m_iconPath;
    wxBitmap              m_iconBmp;
    PlaterPresetComboBox *m_printerCmb;
    ScalableButton       *m_editBtn;
    ScalableButton       *m_connectionBtn;
    bool                  m_hover;
    std::set<wxObject*>   m_focusObjSet;
};

}} // namespace Slic3r::GUI

#endif
