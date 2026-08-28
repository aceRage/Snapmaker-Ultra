#ifndef _Slic3r_GUI_FFTextCtrl_hpp_
#define _Slic3r_GUI_FFTextCtrl_hpp_

#include <vector>
#include <wx/string.h>
#include <wx/textctrl.h>
#include "slic3r/GUI/Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

class FFTextCtrl : public wxTextCtrl
{
public:
    FFTextCtrl(wxWindow* parent = nullptr, wxString text = "", wxSize size = wxDefaultSize, int style = 0, wxString hint = "");
    void SetTextHint(const wxString& hint);
    void SetMaxLength(int max_length);

private:
    void OnPaint(wxPaintEvent& event);

private:
    wxString m_hint;
    wxString m_old_text;
    int m_max_length;
    Label* m_length_label{ nullptr };
    std::vector<std::string> m_vs;
};

}} // namespace Slic3r::GUI

#endif
