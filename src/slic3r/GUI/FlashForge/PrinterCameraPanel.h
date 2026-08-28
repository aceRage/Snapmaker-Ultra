#ifndef slic3r_GUI_PrinterCameraPanel_hpp_
#define slic3r_GUI_PrinterCameraPanel_hpp_

#include <wx/event.h>
#include <wx/panel.h>
#include <wx/webview.h>
#include "MultiComDef.hpp"

namespace Slic3r { namespace GUI {

class PrinterCameraPanel : public wxPanel
{
public:
    PrinterCameraPanel(wxWindow *parent);

    void setSize(wxSize size);

    void setCurComId(com_id_t comId);

    void setStreamUrl(const std::string &streamUrl);

    void setOffline();

    void showPopup();

private:
    void onPaint(wxPaintEvent &event);

    void onScriptMessage(wxWebViewEvent &event);

private:
    com_id_t   m_curComId;
    wxWebView *m_webView;
    wxDialog  *m_popupDlg;
};

}} // namespace Slic3r::GUI

#endif
