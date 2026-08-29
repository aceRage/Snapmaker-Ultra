#ifndef slic3r_GUI_WebView_hpp_
#define slic3r_GUI_WebView_hpp_

#include <wx/webview.h>

class WebView
{
public:
    // Ultra: brand_tag prefixes the User-Agent ("SM-Slicer" by default). The Bambu account
    // login must use "BBL-Slicer" or bambulab.com's sign-in won't run the slicer login flow
    // (it just redirects to the marketing home and never posts the token back).
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
