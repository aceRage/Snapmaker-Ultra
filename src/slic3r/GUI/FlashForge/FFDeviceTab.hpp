#ifndef slic3r_FFDeviceTab_hpp_
#define slic3r_FFDeviceTab_hpp_

#include <wx/panel.h>
#include <wx/simplebook.h>

namespace Slic3r {
namespace GUI {

class DeviceListPanel;
class SingleDeviceState;

// Ultra: Flashforge Device tab - hosts Orca-Flashforge's Device List and Device
// Status pages (the pages their MonitorPanel mounts), routed by the
// EVT_SWITCH_TO_DEVICE_LIST/STATUS events posted via MainFrame::jump_to_monitor.
class FFDeviceTab : public wxPanel
{
public:
    FFDeviceTab(wxWindow* parent);

    void OnActivate();

private:
    wxSimplebook*      m_book { nullptr };
    DeviceListPanel*   m_device_list { nullptr };
    SingleDeviceState* m_device_status { nullptr };
};

} // namespace GUI
} // namespace Slic3r

#endif // slic3r_FFDeviceTab_hpp_
