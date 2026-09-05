#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

// Ultra: the client identity this build presents to a third-party sign-in page. It names
// THIS product, and CreateWebView pairs it with this build's real SLIC3R_VERSION.
//
// The Bambu account login used to pass "BBL-Slicer" here, which (together with a faked
// version, 02.03.00.01) introduced the fork to bambulab.com as official Bambu Studio 2.3.0.1.
// That is the precise behaviour Bambu Lab objected to publicly in April 2026, so we do not do
// it any more. Measured 2026-09-04: bambulab.com/en/sign-in serves the page to a UA without
// the "BBL-Slicer" token perfectly well, but classifies the client "platform":"browser"
// instead of "platform":"studio", so it offers the ordinary web login rather than the
// in-slicer flow that posts the token back (see WebUserLoginDialog). Restoring the old
// behaviour is a one-token change here - and a deliberate decision to impersonate.
#define ULTRA_CLIENT_UA_TAG "Snapmaker-Orca-Ultra"

class WebView
{
public:
    // Ultra: brand_tag prefixes the User-Agent. It stays "SM-Slicer" by default because the
    // bundled pages under resources/web look for that token in navigator.userAgent to tell
    // whether they are running inside the slicer (globalapi.js, IsInSlicer()).
    static wxWebView *CreateWebView(wxWindow *parent, wxString const &url, wxString const &brand_tag = "SM-Slicer");
#if wxUSE_WEBVIEW_EDGE
    static bool CheckWebViewRuntime();
    static bool DownloadAndInstallWebViewRuntime();
#endif
    static void LoadUrl(wxWebView * webView, wxString const &url);

    static bool RunScript(wxWebView * webView, wxString const & msg);

    static void RecreateAll();
};

#endif // !slic3r_GUI_WebView_hpp_
