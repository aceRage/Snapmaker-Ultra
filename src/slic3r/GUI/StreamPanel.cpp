#include "StreamPanel.hpp"

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
    // Seed the page's host list from the legacy camera-address preference, if set.
    const std::string seed = wxGetApp().app_config->get("hd_camera_host");
    if (!seed.empty())
        url += "?seed=" + wxURI(wxString::FromUTF8(seed)).BuildURI();

    m_browser = WebView::CreateWebView(this, url);
    if (m_browser == nullptr)
        return;

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);
}

} // namespace GUI
} // namespace Slic3r
