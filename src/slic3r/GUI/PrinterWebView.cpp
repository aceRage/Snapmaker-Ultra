#include "PrinterWebView.hpp"

#include "I18N.hpp"
#include "slic3r/GUI/PrinterWebView.hpp"
#include "slic3r/GUI/wxExtensions.hpp"
#include "slic3r/GUI/GUI_App.hpp"
#include "slic3r/GUI/MainFrame.hpp"
#include "common_func/common_func.hpp"

#include <wx/sizer.h>
#include <wx/string.h>
#include <wx/toolbar.h>
#include <wx/textdlg.h>

#include <slic3r/GUI/Widgets/WebView.hpp>
#include <wx/webview.h>
#include <boost/algorithm/string.hpp>
#include "slic3r/GUI/SSWCP.hpp"
#include "sentry_wrapper/SentryWrapper.hpp"

namespace pt = boost::property_tree;

namespace Slic3r {
namespace GUI {

PrinterWebView::PrinterWebView(wxWindow *parent)
        : wxPanel(parent, wxID_ANY, wxDefaultPosition, wxDefaultSize)
 {

    wxBoxSizer* topsizer = new wxBoxSizer(wxVERTICAL);

    wxString url      = wxString::FromUTF8(LOCALHOST_URL + std::to_string(wxGetApp().get_page_http_port()) + "/web/flutter_web/index.html?path=2");
    auto     real_url = wxGetApp().get_international_url(url);
      // Create the webview
    m_browser = WebView::CreateWebView(this, real_url);
    if (m_browser == nullptr) {
        wxLogError("Could not init m_browser");
        return;
    }

    m_browser->Bind(wxEVT_WEBVIEW_ERROR, &PrinterWebView::OnError, this);
    m_browser->Bind(wxEVT_WEBVIEW_LOADED, &PrinterWebView::OnLoaded, this);
    m_browser->Bind(wxEVT_WEBVIEW_SCRIPT_MESSAGE_RECEIVED, &PrinterWebView::OnScriptMessage, this, m_browser->GetId());

    SetSizer(topsizer);

    topsizer->Add(m_browser, wxSizerFlags().Expand().Proportion(1));

    update_mode();

    //Zoom
    m_zoomFactor = 100;

    //Connect the idle events
    Bind(wxEVT_CLOSE_WINDOW, &PrinterWebView::OnClose, this);

 }

PrinterWebView::~PrinterWebView()
{
    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " Start";
    SetEvtHandlerEnabled(false);
    SSWCP::on_webview_delete(m_browser);

    wxGetApp().fltviews().remove_printer_view(this);

    BOOST_LOG_TRIVIAL(info) << __FUNCTION__ << " End";
}


void PrinterWebView::load_url(wxString& url, wxString apikey)
{
    if (m_browser == nullptr)
        return;
    m_apikey = apikey;

    if (url.find("path=2") != std::string::npos) {
        wxGetApp().fltviews().add_printer_view(this, url, apikey);
    } else {
        wxGetApp().fltviews().remove_printer_view(this);
    }

    m_browser->Show();
    m_browser->LoadURL(url);

    UpdateState();
}

void PrinterWebView::reload()
{
    m_browser->Reload();
}

bool PrinterWebView::isSnapmakerPage()
{
    if (m_browser == nullptr)
        return false;
    auto url = m_browser->GetCurrentURL();
    return (url.find("flutter_web") != std::string::npos);
}

void PrinterWebView::sendMessage(const std::string& msg) {
    WebView::RunScript(m_browser, msg);
}

void PrinterWebView::update_mode()
{
    // m_browser->EnableAccessToDevTools(wxGetApp().app_config->get_bool("developer_mode"));
    m_browser->EnableAccessToDevTools(true);
}

/**
 * Method that retrieves the current state from the web control and updates the
 * GUI the reflect this current state.
 */
void PrinterWebView::UpdateState() {
  // SetTitle(m_browser->GetCurrentTitle());

}

void PrinterWebView::OnClose(wxCloseEvent& evt)
{
    this->Hide();
}

// High-FPS LAN camera overlay for the Snapmaker U1 device page. The Flutter app polls
// http://<printer-ip>/server/files/camera/monitor.jpg (~1 fps); this script sniffs that
// URL to learn the printer's LAN address, then offers a floating "HD" button that opens
// the printer's camera-streamer WebRTC player (http://<ip>/webcam/webrtc, ~15 fps) in a
// full-view iframe overlay. Inert on pages that never poll the monitor snapshot.
static const char* hd_camera_script()
{
    return R"JS(
    (function() {
        if (window.__snorca_hd_cam) return;
        window.__snorca_hd_cam = true;
        // Candidate printer addresses injected by the slicer; more can be sniffed at runtime.
        var candidates = __SNORCA_IPS__;
        var reachable = [];    // hosts with a responding camera-streamer
        var activeHost = null; // host the device page is currently polling (sniffed)
        var btn = null, overlay = null, frame = null, picker = null;

        function addReachable(host) {
            if (reachable.indexOf(host) === -1) {
                reachable.push(host);
                ensureBtn();
                refreshPicker();
            }
        }
        function sniff(u) {
            try {
                if (typeof u !== 'string') return;
                var m = u.match(/^https?:\/\/([^\/]+)\/server\/files\/camera\/monitor\.jpg/i);
                if (!m) return;
                var host = m[1].replace(/:\d+$/, '');
                if (candidates.indexOf(host) === -1) candidates.push(host);
                activeHost = host;
            } catch (e) {}
        }
        // Resource-timing observer sees every subresource the page loads, regardless of API.
        try {
            new PerformanceObserver(function(list) {
                list.getEntries().forEach(function(e) { sniff(e.name); });
            }).observe({ type: 'resource', buffered: true });
        } catch (e) {}
        try {
            if (window.fetch) {
                var of = window.fetch;
                window.fetch = function(input) {
                    sniff(typeof input === 'string' ? input : (input && input.url));
                    return of.apply(this, arguments);
                };
            }
        } catch (e) {}
        try {
            var oo = XMLHttpRequest.prototype.open;
            XMLHttpRequest.prototype.open = function(method, url) { sniff(url); return oo.apply(this, arguments); };
        } catch (e) {}

        function probe() {
            candidates.forEach(function(ip) {
                if (!ip || reachable.indexOf(ip) !== -1) return;
                var im = new Image();
                im.onload = function() { addReachable(ip); };
                im.src = 'http://' + ip + '/webcam/snapshot.jpg?t=' + Date.now();
            });
        }
        setTimeout(probe, 1000);
        setInterval(probe, 10000);

        function pickHost() {
            if (activeHost && reachable.indexOf(activeHost) !== -1) return activeHost;
            var last = null;
            try { last = localStorage.getItem('snorca_hd_last'); } catch (e) {}
            if (last && reachable.indexOf(last) !== -1) return last;
            return reachable.length ? reachable[0] : null;
        }
        function currentHost() {
            if (!frame) return null;
            var m = String(frame.src).match(/^https?:\/\/([^\/]+)\//);
            return m ? m[1] : null;
        }
        function setStream(host) {
            try { localStorage.setItem('snorca_hd_last', host); } catch (e) {}
            if (frame) frame.src = 'http://' + host + '/webcam/webrtc';
            refreshPicker();
        }
        function refreshPicker() {
            if (!picker) return;
            picker.innerHTML = '';
            if (reachable.length < 2) { picker.style.display = 'none'; return; }
            picker.style.display = 'flex';
            var cur = currentHost();
            reachable.forEach(function(h) {
                var b = document.createElement('div');
                b.textContent = h;
                b.style.cssText = 'padding:6px 12px;margin-right:8px;border-radius:16px;color:#fff;font:13px sans-serif;cursor:pointer;user-select:none;background:' +
                                  (h === cur ? 'rgba(0,150,80,0.85)' : 'rgba(255,255,255,0.15)') + ';';
                b.onclick = function() { setStream(h); };
                picker.appendChild(b);
            });
        }
        function ensureBtn() {
            if (btn) return;
            if (!document.body) { setTimeout(ensureBtn, 1000); return; }
            btn = document.createElement('div');
            btn.textContent = 'HD';
            btn.title = 'High-FPS camera stream (LAN)';
            btn.style.cssText = 'position:fixed;right:16px;bottom:16px;z-index:2147483645;width:44px;height:44px;border-radius:22px;background:rgba(0,0,0,0.65);color:#fff;font:bold 15px sans-serif;display:flex;align-items:center;justify-content:center;cursor:pointer;user-select:none;box-shadow:0 2px 8px rgba(0,0,0,0.4);';
            btn.onclick = toggleOverlay;
            document.body.appendChild(btn);
        }
        function toggleOverlay() {
            if (overlay) { overlay.remove(); overlay = null; frame = null; picker = null; return; }
            var host = pickHost();
            if (!host) return;
            overlay = document.createElement('div');
            overlay.style.cssText = 'position:fixed;inset:0;z-index:2147483646;background:#000;';
            frame = document.createElement('iframe');
            frame.allow = 'autoplay';
            frame.style.cssText = 'position:absolute;inset:0;width:100%;height:100%;border:0;';
            overlay.appendChild(frame);
            picker = document.createElement('div');
            picker.style.cssText = 'position:absolute;left:16px;top:16px;z-index:2;display:none;';
            overlay.appendChild(picker);
            var close = document.createElement('div');
            close.textContent = '✕';
            close.title = 'Close';
            close.style.cssText = 'position:absolute;right:16px;top:16px;z-index:2;width:36px;height:36px;border-radius:18px;background:rgba(255,255,255,0.15);color:#fff;font:bold 16px sans-serif;display:flex;align-items:center;justify-content:center;cursor:pointer;user-select:none;';
            close.onclick = toggleOverlay;
            overlay.appendChild(close);
            document.body.appendChild(overlay);
            setStream(host);
        }
    })();
)JS";
}

void PrinterWebView::InjectHdCameraScript()
{
    if (m_browser == nullptr)
        return;
    // Pass candidate printer addresses to the script; it probes them for a reachable
    // camera-streamer, so the button only shows on LAN. Sources, in priority order:
    // the manual preference, the printer preset's print host, and registered devices.
    std::vector<std::string> hosts;
    {
        // The preference accepts several printers, separated by comma/semicolon/space.
        std::vector<std::string> manual;
        boost::split(manual, wxGetApp().app_config->get("hd_camera_host"), boost::is_any_of(",; "), boost::token_compress_on);
        hosts.insert(hosts.end(), manual.begin(), manual.end());
    }
    {
        std::string host = wxGetApp().preset_bundle->printers.get_edited_preset().config.opt_string("print_host");
        if (size_t pos = host.find("//"); pos != std::string::npos)
            host = host.substr(pos + 2);
        if (size_t pos = host.find('/'); pos != std::string::npos)
            host = host.substr(0, pos);
        if (size_t pos = host.find(':'); pos != std::string::npos)
            host = host.substr(0, pos);
        hosts.emplace_back(host);
    }
    for (const DeviceInfo& dev : wxGetApp().app_config->get_devices())
        hosts.emplace_back(dev.ip);
    wxString ips = "[";
    for (const std::string& host : hosts) {
        if (host.empty())
            continue;
        // Restrict to safe hostname characters (also keeps the JS literal uninjectable).
        if (host.find_first_not_of("0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ.:-_") != std::string::npos)
            continue;
        if (ips.length() > 1)
            ips += ",";
        ips += "'" + host + "'";
    }
    ips += "]";
    wxString script = hd_camera_script();
    script.Replace("__SNORCA_IPS__", ips);
    // Inject into the current page and persist for future navigations (idempotent JS guard).
    WebView::RunScript(m_browser, script);
#ifndef __WXMAC__
    m_browser->AddUserScript(script);
#endif
}

void PrinterWebView::SendAPIKey()
{
    if (m_apikey.IsEmpty())
        return;

    // Re-inject on every document load (e.g. context-menu Reload). Idempotent
    // JS-level marker avoids stacking fetch/XHR wrappers if LOADED fires more than once.
    wxString script = wxString::Format(R"(
    (function() {
        if (window.__sm_apikey_hooked) return;
        window.__sm_apikey_hooked = true;
        var apiKey = '%s';
        // Override fetch to inject X-API-Key header
        if (window.fetch) {
            var originalFetch = window.fetch;
            window.fetch = function(input, init) {
                init = init || {};
                init.headers = init.headers || {};
                if (!init.headers['X-API-Key']) {
                    init.headers['X-API-Key'] = apiKey;
                }
                return originalFetch(input, init);
            };
        }
        // Override XMLHttpRequest to inject X-API-Key header.
        // Preserves prototype chain and static constants for compatibility
        // with libraries that check instanceof or readyState constants.
        var OrigXHR = window.XMLHttpRequest;
        var newXHR = function() {
            var xhr = new OrigXHR();
            var origOpen = xhr.open;
            var headersSet = false;
            xhr.open = function(method, url) {
                origOpen.apply(xhr, arguments);
                if (!headersSet) {
                    xhr.setRequestHeader('X-API-Key', apiKey);
                    headersSet = true;
                }
            };
            return xhr;
        };
        newXHR.prototype = OrigXHR.prototype;
        newXHR.DONE = OrigXHR.DONE;
        newXHR.UNSENT = OrigXHR.UNSENT;
        newXHR.OPENED = OrigXHR.OPENED;
        newXHR.HEADERS_RECEIVED = OrigXHR.HEADERS_RECEIVED;
        newXHR.LOADING = OrigXHR.LOADING;
        window.XMLHttpRequest = newXHR;
    })();
)",
                                       m_apikey);

    // Inject immediately into the current page on all platforms.
    WebView::RunScript(m_browser, script);

#ifndef __WXMAC__
    // On Windows/Linux: also install a persistent user script so the
    // API key is injected at document start on future navigations.
    // AddUserScript works correctly on these platforms (Edge WebView2, WebKitGTK).
    // Do NOT call Reload() — the current page is already handled by RunScript above.
    m_browser->RemoveAllUserScripts();
    m_browser->AddUserScript(script);
    // Note: RemoveAllUserScripts also drops the HD camera script; OnLoaded calls
    // InjectHdCameraScript() right after SendAPIKey(), which re-adds it substituted.
#endif
}

void PrinterWebView::OnError(wxWebViewEvent &evt)
{
    auto e = "unknown error";
    switch (evt.GetInt()) {
      case wxWEBVIEW_NAV_ERR_CONNECTION:
        e = "wxWEBVIEW_NAV_ERR_CONNECTION";
        break;
      case wxWEBVIEW_NAV_ERR_CERTIFICATE:
        e = "wxWEBVIEW_NAV_ERR_CERTIFICATE";
        break;
      case wxWEBVIEW_NAV_ERR_AUTH:
        e = "wxWEBVIEW_NAV_ERR_AUTH";
        break;
      case wxWEBVIEW_NAV_ERR_SECURITY:
        e = "wxWEBVIEW_NAV_ERR_SECURITY";
        break;
      case wxWEBVIEW_NAV_ERR_NOT_FOUND:
        e = "wxWEBVIEW_NAV_ERR_NOT_FOUND";
        break;
      case wxWEBVIEW_NAV_ERR_REQUEST:
        e = "wxWEBVIEW_NAV_ERR_REQUEST";
        break;
      case wxWEBVIEW_NAV_ERR_USER_CANCELLED:
        e = "wxWEBVIEW_NAV_ERR_USER_CANCELLED";
        break;
      case wxWEBVIEW_NAV_ERR_OTHER:
        e = "wxWEBVIEW_NAV_ERR_OTHER";
        break;
      }
    BOOST_LOG_TRIVIAL(fatal) << __FUNCTION__<< boost::format(":PrinterWebView error loading page %1% %2% %3% %4%") %evt.GetURL() %evt.GetTarget() %e %evt.GetString();
}

void PrinterWebView::OnLoaded(wxWebViewEvent &evt)
{
    if (evt.GetURL().IsEmpty())
        return;
    if (evt.GetURL() != m_browser->GetCurrentURL())
        return;
    SendAPIKey();
    InjectHdCameraScript();
}

void PrinterWebView::OnScriptMessage(wxWebViewEvent& evt) {
    // BOOST_LOG_TRIVIAL(trace) << __FUNCTION__ << ": " << evt.GetString().ToUTF8().data();

    // if (wxGetApp().get_mode() == comDevelop)
    //     wxLogMessage("Script message received; value = %s, handler = %s", evt.GetString(), evt.GetMessageHandler());

    // test
    SSWCP::handle_web_message(evt.GetString().ToUTF8().data(), m_browser);
}


} // GUI
} // Slic3r
