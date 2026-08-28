#include "StreamPanel.hpp"

#include "BambuCamRelay.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/uri.h>
#include <wx/weakref.h>

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
    } else if (evt.GetString().StartsWith("ff_detail:")) {
        // Flashforge new-gen LAN API: POST http://<ip>:8898/detail returns device
        // detail JSON including cameraStreamUrl (rtsp:// on Creator 5, MJPEG http://
        // on the Adventurer 5M family). The page cannot POST there itself (no CORS
        // headers from the printer), so probe from here and hand the URL back.
        const std::string ip = evt.GetString().Mid(10).ToStdString();
        if (ip.empty() || ip.find_first_of("\"'\\<>") != std::string::npos)
            return;
        wxWeakRef<StreamPanel> weak(this);
        auto reply = [weak, ip](const std::string& url) {
            wxGetApp().CallAfter([weak, ip, url]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                nlohmann::json esc_ip = ip, esc_url = url; // JSON-escape for JS literals
                WebView::RunScript(weak->m_browser,
                    wxString::Format("if (window.__ffDetail) window.__ffDetail(%s, %s);",
                                     wxString::FromUTF8(esc_ip.dump()), wxString::FromUTF8(esc_url.dump())));
            });
        };
        auto http = Http::post("http://" + ip + ":8898/detail");
        http.timeout_connect(4)
            .timeout_max(8)
            .header("Content-Type", "application/json")
            .set_post_body(std::string("{\"serialNumber\":\"\",\"checkCode\":\"\"}"))
            .on_error([reply](std::string, std::string, unsigned) { reply(""); })
            .on_complete([reply](std::string body, unsigned) {
                std::string url;
                try {
                    nlohmann::json j = nlohmann::json::parse(body);
                    if (j.contains("detail") && j["detail"].is_object() && j["detail"].contains("cameraStreamUrl"))
                        url = j["detail"]["cameraStreamUrl"].get<std::string>();
                    else if (j.contains("cameraStreamUrl"))
                        url = j["cameraStreamUrl"].get<std::string>();
                } catch (...) {}
                reply(url);
            })
            .perform();
    }
}

} // namespace GUI
} // namespace Slic3r
