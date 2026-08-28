#ifndef GUIDE_WEB_PANEL_H
#define GUIDE_WEB_PANEL_H

#include <wx/wx.h>
#include <wx/dcclient.h>
#include "slic3r/GUI/GUI.hpp"
#include "slic3r/GUI/I18N.hpp"
#include <slic3r/GUI/wxExtensions.hpp>
#include <slic3r/GUI/Widgets/WebView.hpp>
#include <slic3r/GUI/Widgets/FFButton.hpp>

namespace Slic3r { namespace GUI {
    
wxDECLARE_EVENT(EVT_LOADING_TIMEOUT, wxCommandEvent);

class LoadingWebPage : public wxPanel
{
public:
    LoadingWebPage(wxPanel* parent);
    void OnPaint(wxPaintEvent& event);
    void OnTimer(wxTimerEvent& event);
    void Loading();
    void End();
    ~LoadingWebPage();

private:
    int      m_loadTime = 0;
    wxTimer* m_prepareTimer;
    int                         m_loadingIdx = 0;
    std::vector<ScalableBitmap> m_loadingIcons;
};

class GuideWebPanel : public wxPanel
{
public:
    GuideWebPanel(wxWindow* parent, wxWindowID id);
    ~GuideWebPanel();

private:
    enum WebState { NORMAL, PREPARE, NG };
    wxWebView* m_web_view;
    LoadingWebPage* m_loading_page;
    std::string                 m_url;
    wxPanel*   m_error_panel;
    WebState   m_status = NORMAL;
};

}} // namespace Slic3r::GUI

#endif
