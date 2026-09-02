#include "StreamPanel.hpp"

#include "RemoteAccess.hpp"
#include "RemoteHub.hpp"
#include "GUI_App.hpp"
#include "HttpServer.hpp"
#include "slic3r/GUI/Widgets/WebView.hpp"
#include "slic3r/Utils/Http.hpp"

#include <nlohmann/json.hpp>
#include <wx/sizer.h>
#include <wx/webview.h>
#include <wx/uri.h>
#include <wx/weakref.h>

#include <thread>

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

    // The page talks to us over the app-wide "wx" script channel that CreateWebView already
    // injects; a second named handler is never injected by the Edge backend, so don't add one.
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &StreamPanel::OnScriptMessage, this, m_browser->GetId());

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // This instance's loopback API: the hub lists and proxies it (phone Prepare/Devices tabs).
    RemoteAccess::get().start();

    // Phone access left on last time: bring the hub up with the same link. SNORCA_PHONE_ACCESS=<token>
    // in the environment does the same without touching the saved settings (headless/agent use).
    wxString env_token;
    std::string token;
    bool        phone = false;
    if (wxGetEnv("SNORCA_PHONE_ACCESS", &env_token) && !env_token.empty()) {
        token = env_token.ToStdString();
        phone = true;
    } else if (wxGetApp().app_config->get("stream_phone_access") == "1") {
        token = wxGetApp().app_config->get("stream_phone_token");
        phone = true;
    }
    if (phone) {
        // Off the GUI thread: spawning and waiting for the hub takes a moment.
        std::thread([token]() { RemoteHub::ensure_running(token, true); }).detach();
    }
}

void StreamPanel::OnScriptMessage(wxWebViewEvent& evt)
{
    const wxString msg = evt.GetString();
    if (msg == "hub_start") {
        // The page needs a relay (go2rtc for RTSP/ONVIF, MJPEG for Bambu P1): make sure the
        // hub runs and hand back its ports.
        wxWeakRef<StreamPanel> weak(this);
        std::thread([weak]() {
            RemoteHub::Info info = RemoteHub::ensure_running("", false);
            wxGetApp().CallAfter([weak, info]() {
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__hubReady) window.__hubReady(%d, %d);", info.go2rtc_port, info.relay_port));
            });
        }).detach();
    } else if (msg.StartsWith("stream_state:")) {
        // The page's full camera list (with credentials): the hub keeps it for the phone and
        // for streams that outlive this window. Remembered here if no hub runs yet.
        const std::string state = msg.Mid(13).ToStdString(wxConvUTF8);
        std::thread([state]() { RemoteHub::post_state(state); }).detach();
    } else if (msg == "remote_on" || msg == "remote_off" || msg == "remote_info") {
        // The toggle is remembered (with its token, so a scanned link survives restarts).
        wxWeakRef<StreamPanel> weak(this);
        const std::string      saved_token = wxGetApp().app_config->get("stream_phone_token");
        const std::string      what        = msg.ToStdString();
        std::thread([weak, what, saved_token]() {
            RemoteHub::Info info;
            if (what == "remote_on")
                info = RemoteHub::ensure_running(saved_token, true);
            else if (what == "remote_off")
                info = RemoteHub::set_phone(false);
            else
                info = RemoteHub::query();
            wxGetApp().CallAfter([weak, what, info]() {
                auto* cfg = wxGetApp().app_config;
                if (what == "remote_on") {
                    cfg->set("stream_phone_access", info.alive && info.phone ? "1" : "0");
                    cfg->set("stream_phone_token", info.token);
                } else if (what == "remote_off") {
                    cfg->set("stream_phone_access", "0");
                    cfg->set("stream_phone_token", "");
                }
                if (weak == nullptr || weak->m_browser == nullptr)
                    return;
                WebView::RunScript(weak->m_browser, wxString::Format("if (window.__remoteInfo) window.__remoteInfo(%s);", wxString::FromUTF8(info.json())));
            });
        }).detach();
    } else if (msg.StartsWith("ff_detail:")) {
        // Flashforge new-gen LAN API: POST http://<ip>:8898/detail returns device
        // detail JSON including cameraStreamUrl (rtsp:// on Creator 5, MJPEG http://
        // on the Adventurer 5M family). The page cannot POST there itself (no CORS
        // headers from the printer), so probe from here and hand the URL back.
        const std::string ip = msg.Mid(10).ToStdString();
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
