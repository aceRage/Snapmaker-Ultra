#pragma once

#include <wx/panel.h>

class wxWebView;
class wxWebViewEvent;

namespace Slic3r {
namespace GUI {

// "Stream" tab: grid of LAN camera streams (see resources/web/orca/stream_center.html).
class StreamPanel : public wxPanel
{
public:
    StreamPanel(wxWindow* parent);

private:
    void OnScriptMessage(wxWebViewEvent& evt);

    wxWebView* m_browser { nullptr };
};

} // namespace GUI
} // namespace Slic3r
