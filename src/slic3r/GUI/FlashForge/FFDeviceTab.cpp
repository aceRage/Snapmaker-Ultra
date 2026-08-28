#include "FFDeviceTab.hpp"

#include "slic3r/GUI/Monitor.hpp"
#include "slic3r/GUI/FlashForge/DeviceListPanel.hpp"
#include "slic3r/GUI/FlashForge/SingleDeviceState.hpp"

#include <wx/sizer.h>

namespace Slic3r {
namespace GUI {

FFDeviceTab::FFDeviceTab(wxWindow* parent)
    : wxPanel(parent, wxID_ANY)
{
    m_book = new wxSimplebook(this, wxID_ANY);
    m_device_list   = new DeviceListPanel(m_book);
    m_device_status = new SingleDeviceState(m_book);
    m_book->AddPage(m_device_list, wxEmptyString, true);
    m_book->AddPage(m_device_status, wxEmptyString, false);

    auto* sizer = new wxBoxSizer(wxVERTICAL);
    sizer->Add(m_book, wxSizerFlags().Expand().Proportion(1));
    SetSizer(sizer);

    // Same routing as Orca-Flashforge's MonitorPanel.
    Bind(EVT_SWITCH_TO_DEVICE_STATUS, [this](wxCommandEvent& event) {
        m_device_status->setCurId(event.GetInt());
        m_book->SetSelection(1);
        m_device_status->checkPrinterStatus();
    });
    Bind(EVT_SWITCH_TO_DEVICE_LIST, [this](wxCommandEvent&) {
        m_book->SetSelection(0);
    });
}

void FFDeviceTab::OnActivate()
{
    if (m_device_list != nullptr)
        m_device_list->OnActivate();
}

} // namespace GUI
} // namespace Slic3r
