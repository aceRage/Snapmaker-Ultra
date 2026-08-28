#include "StreamPanel.hpp"

#include "BambuCamRelay.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"

#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/uri.h>

namespace Slic3r {
namespace GUI {

StreamPanel::StreamPanel(wxWindow* parent)
    : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
{
    wxString url = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) +
                                      "/web/orca/stream_center.html");
    // Local MJPEG relay for Bambu cameras (see BambuCamRelay).
    url += wxString::Format("?relay=%d", BambuCamRelay::get().port());
    // Seed the page's host list from the legacy camera-address preference, if set.
    const std::string seed = wxGetApp().app_config->get("hd_camera_host");
    if (!seed.empty())
        url += "&seed=" + wxURI(wxString::FromUTF8(seed)).BuildURI();

    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr)
        return;

    // The page asks for the go2rtc relay (RTSPS cameras) on demand via
    // window.snorca.postMessage('start_go2rtc'), so the process only runs when needed.
    m_browser->AddScriptMessageHandler("snorca");
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &StreamPanel::OnScriptMessage, this, m_browser->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);
}

void StreamPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    if (evt.GetString() == "start_go2rtc") {
        const int port = Go2RtcLauncher::get().port();
        WebView::RunScript(m_browser, wxString::Format("if (window.__go2rtcReady) window.__go2rtcReady(%d);", port));
    }
}

} // namespace GUI
} // namespace Slic3r
