#include "PrinterErrorMsgDlg.hpp"
#include <wx/uri.h>
#include <wx/utils.h>
#include "slic3r/GUI/I18N.hpp"
#include "slic3r/GUI/FlashForge/MultiComMgr.hpp"
#include "slic3r/GUI/Widgets/Label.hpp"

namespace Slic3r { namespace GUI {

PrinterErrorMsgDlg::error_code_data_map_t PrinterErrorMsgDlg::s_errorCodeDataMap;

PrinterErrorMsgDlg::PrinterErrorMsgDlg(wxWindow *parent, com_id_t comId, const std::string &errorCode)
    : wxDialog(parent, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxCAPTION | wxSYSTEM_MENU)
    , m_comId(comId)
    , m_errorCode(errorCode)
{
    if (s_errorCodeDataMap.empty()) {
        initErrorCodeDataMap();
    }
    SetBackgroundColour(*wxWHITE);
    SetDoubleBuffered(true);

    m_titleLbl = new wxStaticText(this, wxID_ANY, _L("Error"), wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_titleLbl->SetFont(::Label::Body_14);
    m_titleLbl->SetForegroundColour("#333333");

    m_msgLbl = new wxStaticText(this, wxID_ANY, wxEmptyString, wxDefaultPosition, wxDefaultSize, wxALIGN_CENTER);
    m_msgLbl->SetMinSize(wxSize(FromDIP(361), -1));
    m_msgLbl->SetForegroundColour("#333333");

    m_operator1Btn = new FFButton(this, wxID_ANY, wxEmptyString);
    m_operator1Btn->SetFontColor("#419488");
    m_operator1Btn->SetBorderColor("#419488");
    m_operator1Btn->SetFontHoverColor("#65A79E");
    m_operator1Btn->SetBorderHoverColor("#65A79E");
    m_operator1Btn->SetFontPressColor("#1A8676");
    m_operator1Btn->SetBorderPressColor("#1A8676");

    m_operator2Btn = new FFButton(this, wxID_ANY, wxEmptyString);
    m_operator2Btn->SetFontColor("#419488");
    m_operator2Btn->SetBorderColor("#419488");
    m_operator2Btn->SetFontHoverColor("#65A79E");
    m_operator2Btn->SetBorderHoverColor("#65A79E");
    m_operator2Btn->SetFontPressColor("#1A8676");
    m_operator2Btn->SetBorderPressColor("#1A8676");

    wxSizer *sizerBtn = new wxBoxSizer(wxHORIZONTAL);
    sizerBtn->AddStretchSpacer(1);
    sizerBtn->Add(m_operator1Btn, 0, wxRIGHT, FromDIP(23));
    sizerBtn->Add(m_operator2Btn);
    sizerBtn->AddStretchSpacer(1);

    wxSizer *sizer = new wxBoxSizer(wxVERTICAL);
    sizer->AddSpacer(FromDIP(34));
    sizer->Add(m_titleLbl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    sizer->AddSpacer(FromDIP(15));
    sizer->Add(m_msgLbl, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    sizer->AddSpacer(FromDIP(34));
    sizer->Add(sizerBtn, 0, wxEXPAND | wxLEFT | wxRIGHT, FromDIP(15));
    sizer->AddSpacer(FromDIP(47));
    SetSizer(sizer);

    setupErrorCode(errorCode);
    m_operator1Btn->Bind(wxEVT_BUTTON, &PrinterErrorMsgDlg::onOperator1, this);
    m_operator2Btn->Bind(wxEVT_BUTTON, &PrinterErrorMsgDlg::onOperator2, this);
    MultiComMgr::inst()->Bind(COM_CONNECTION_EXIT_EVENT, &PrinterErrorMsgDlg::onConnectionExit, this);
    MultiComMgr::inst()->Bind(COM_DEV_DETAIL_UPDATE_EVENT, &PrinterErrorMsgDlg::onDevDetailUpdate, this);

    Layout();
    Fit();
    CenterOnScreen();
}

bool PrinterErrorMsgDlg::isErrorCodeHandled(const std::string &errorCode)
{
    if (s_errorCodeDataMap.empty()) {
        initErrorCodeDataMap();
    }
    return s_errorCodeDataMap.find(errorCode) != s_errorCodeDataMap.end()
        || errorCode == "E0088" || errorCode == "E0089";
}

void PrinterErrorMsgDlg::setupErrorCode(const std::string &errorCode)
{
    auto it = s_errorCodeDataMap.find(errorCode);
    if (it != s_errorCodeDataMap.end()) {
        m_titleLbl->SetLabelText(_CTX("Error", "Flashforge"));
        m_msgLbl->SetLabelText(_L(it->second.message));
        m_operator1Btn->Show(!it->second.wikiUrl.empty());
        m_operator1Btn->SetLabel(_CTX("View Guide", "Flashforge"), FromDIP(165), FromDIP(36));
        m_operator2Btn->SetLabel(_CTX("Close", "Flashforge"), FromDIP(165), FromDIP(36));
    } else if (errorCode == "E0088") {
        m_titleLbl->SetLabelText(_L("Error"));
        m_msgLbl->SetLabelText(_L("Non-Flashforge build plate detected. Print quality may not be guaranteed."));
        m_operator1Btn->SetLabel(_L("Continue printing"), FromDIP(165), FromDIP(36));
        m_operator2Btn->SetLabel(_L("Stop printing (replace the build plate)"), FromDIP(165), FromDIP(36));
    } else if (errorCode == "E0089") {
        m_titleLbl->SetLabelText(_L("Error"));
        m_msgLbl->SetLabelText(_L("Lidar detected first-layer defects. Please check and decide whether to continue printing."));
        m_operator1Btn->SetLabel(_L("Continue printing (defects acceptable)"), FromDIP(165), FromDIP(36));
        m_operator2Btn->SetLabel(_L("Stop printing"), FromDIP(165), FromDIP(36));
    }
    if (!m_msgLbl->GetLabelText().empty()) {
        Layout();
        Fit();
        m_msgLbl->Wrap(m_msgLbl->GetSize().x);
    }
}

void PrinterErrorMsgDlg::onOperator1(wxCommandEvent &event)
{
    event.Skip();
    auto it = s_errorCodeDataMap.find(m_errorCode);
    if (s_errorCodeDataMap.find(m_errorCode) != s_errorCodeDataMap.end()) {
        wxURI uri(_L(it->second.wikiUrl));
        wxLaunchDefaultBrowser(uri.BuildURI());
        return;
    } else if (m_errorCode == "E0088") {
        MultiComMgr::inst()->putCommand(m_comId, new ComPlateDetectCtrl("continue"));
    } else if (m_errorCode == "E0089") {
        MultiComMgr::inst()->putCommand(m_comId, new ComFirstLayerDetectCtrl("continue"));
    }
    EndModal(wxOK);
}

void PrinterErrorMsgDlg::onOperator2(wxCommandEvent &event)
{
    event.Skip();
    auto it = s_errorCodeDataMap.find(m_errorCode);
    if (it != s_errorCodeDataMap.end()) {
        MultiComMgr::inst()->putCommand(m_comId, new ComErrorCodeCtrl("clearErrorCode", m_errorCode));
    } else if (m_errorCode == "E0088") {
        MultiComMgr::inst()->putCommand(m_comId, new ComPlateDetectCtrl("stop"));
    } else if (m_errorCode == "E0089") {
        MultiComMgr::inst()->putCommand(m_comId, new ComFirstLayerDetectCtrl("stop"));
    }
    EndModal(wxOK);
}

void PrinterErrorMsgDlg::onConnectionExit(ComConnectionExitEvent &event)
{
    event.Skip();
    if (event.id != m_comId) {
        return;
    }
    EndModal(wxCANCEL);
}

void PrinterErrorMsgDlg::onDevDetailUpdate(ComDevDetailUpdateEvent &event)
{
    event.Skip();
    if (event.id != m_comId) {
        return;
    }
    if (strcmp(event.devDetail->status, "error") != 0 || event.devDetail->errorCode != m_errorCode) {
        EndModal(wxCANCEL);
    }
}

void PrinterErrorMsgDlg::initErrorCodeDataMap()
{
    auto pair = s_errorCodeDataMap.emplace("E0001", error_code_data_t());
    pair.first->second.message = "Printer out of range. Please home again!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0001-printer-out-of-range-please-home-again";

    pair = s_errorCodeDataMap.emplace("E0002", error_code_data_t());
    pair.first->second.message = "Communication with MCU interrupted!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0002-communication-with-mcu-interrupted";

    pair = s_errorCodeDataMap.emplace("E0003", error_code_data_t());
    pair.first->second.message = "X/Y/Z/E TMC error: GSTAT!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0003-xyze-tmc-error-gstat";

    pair = s_errorCodeDataMap.emplace("E0004", error_code_data_t());
    pair.first->second.message = "X/Y/Z/E motor TMC chip communication error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0004-xyze-motor-tmc-chip-communication-error";

    pair = s_errorCodeDataMap.emplace("E0005", error_code_data_t());
    pair.first->second.message = "MCU: Unable to connect!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0005-mcu-unable-to-connect";

    pair = s_errorCodeDataMap.emplace("E0006", error_code_data_t());
    pair.first->second.message = "Nozzle temperature below minimum temperature!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0006-nozzle-temperature-below-minimum-temperature";

    pair = s_errorCodeDataMap.emplace("E0007", error_code_data_t());
    pair.first->second.message = "Extruder T0 not heating as expected!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0007-extruder-t0-not-heating-as-expected";

    pair = s_errorCodeDataMap.emplace("E0008", error_code_data_t());
    pair.first->second.message = "Extruder T1 not heating as expected!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0008-extruder-t1-not-heating-as-expected";

    pair = s_errorCodeDataMap.emplace("E0009", error_code_data_t());
    pair.first->second.message = "Heated bed not heating as expected!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0009-heated-bed-not-heating-as-expected";

    pair = s_errorCodeDataMap.emplace("E0010", error_code_data_t());
    pair.first->second.message = "Chamber not heating as expected!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0010-chamber-not-heating-as-expected";

    pair = s_errorCodeDataMap.emplace("E0011", error_code_data_t());
    pair.first->second.message = "Host error. Please restart!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0011-host-error-please-restart";

    pair = s_errorCodeDataMap.emplace("E0012", error_code_data_t());
    pair.first->second.message = "X-axis homing error. X-axis sensor not triggered!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0012-x-axis-homing-error-x-axis-sensor-not-triggered";

    pair = s_errorCodeDataMap.emplace("E0013", error_code_data_t());
    pair.first->second.message = "Y-axis homing error. Y-axis sensor not triggered!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0013-y-axis-homing-error-y-axis-sensor-not-triggered";

    pair = s_errorCodeDataMap.emplace("E0014", error_code_data_t());
    pair.first->second.message = "Z-axis homing error. Z-axis sensor not triggered!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0014-z-axis-homing-error-z-axis-sensor-not-triggered";

    pair = s_errorCodeDataMap.emplace("E0015", error_code_data_t());
    pair.first->second.message = "Extruder temperature < -10!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0015-extruder-temperature-10";

    pair = s_errorCodeDataMap.emplace("E0016", error_code_data_t());
    pair.first->second.message = "Heated bed temperature < -10!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0016-heated-bed-temperature-10";

    pair = s_errorCodeDataMap.emplace("E0017", error_code_data_t());
    pair.first->second.message = "Command exceeds queue limit!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0017-command-exceeds-queue-limit";

    pair = s_errorCodeDataMap.emplace("E0018", error_code_data_t());
    pair.first->second.message = "Language initialization failed. Please retry!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0018-language-initialization-failed-please-retry";

    pair = s_errorCodeDataMap.emplace("E0019", error_code_data_t());
    pair.first->second.message = "USB flash drive read error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0019-usb-flash-drive-read-error";

    pair = s_errorCodeDataMap.emplace("E0020", error_code_data_t());
    pair.first->second.message = "Real-time video service failure!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0020-real-time-video-service-failure";

    pair = s_errorCodeDataMap.emplace("E0021", error_code_data_t());
    pair.first->second.message = "Failed to enable the camera. Please check!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0021-failed-to-enable-the-camera-please-check";

    pair = s_errorCodeDataMap.emplace("E0022", error_code_data_t());
    pair.first->second.message = "Insufficient storage space!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0022-insufficient-storage-space";

    pair = s_errorCodeDataMap.emplace("E0028", error_code_data_t());
    pair.first->second.message = "File copy error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0028-file-copy-error";

    pair = s_errorCodeDataMap.emplace("E0029", error_code_data_t());
    pair.first->second.message = "Model download failed!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0029-model-download-failed";

    pair = s_errorCodeDataMap.emplace("E0041", error_code_data_t());
    pair.first->second.message = "Leveling sensor data cannot be cleared. Please check!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0041-leveling-sensor-data-cannot-be-cleared-please-check";

    pair = s_errorCodeDataMap.emplace("E0042", error_code_data_t());
    pair.first->second.message = "Leveling sensor not triggered. Please check!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0042-leveling-sensor-not-triggered-please-check";

    pair = s_errorCodeDataMap.emplace("E0043", error_code_data_t());
    pair.first->second.message = "Leveling sensor triggered early. Please check!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0043-leveling-sensor-triggered-early-please-check";

    pair = s_errorCodeDataMap.emplace("E0046", error_code_data_t());
    pair.first->second.message = "Heated bed temperature control error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0046-heated-bed-temperature-control-error";

    pair = s_errorCodeDataMap.emplace("E0047", error_code_data_t());
    pair.first->second.message = "Heated bed heating timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0047-heated-bed-heating-timeout";

    pair = s_errorCodeDataMap.emplace("E0048", error_code_data_t());
    pair.first->second.message = "Extruder temperature control error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0048-extruder-temperature-control-error";

    pair = s_errorCodeDataMap.emplace("E0075", error_code_data_t());
    pair.first->second.message = "Nozzle too low!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0075-nozzle-too-low";

    pair = s_errorCodeDataMap.emplace("E0076", error_code_data_t());
    pair.first->second.message = "Nozzle too high!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0076-nozzle-too-high";

    pair = s_errorCodeDataMap.emplace("E0077", error_code_data_t());
    pair.first->second.message = "Chamber heating timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0077-chamber-heating-timeout";

    pair = s_errorCodeDataMap.emplace("E0078", error_code_data_t());
    pair.first->second.message = "Chamber heating failed. Left heating fan speed too low!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0078-chamber-heating-failed-left-heating-fan-speed-too-low";

    pair = s_errorCodeDataMap.emplace("E0079", error_code_data_t());
    pair.first->second.message = "Chamber heating failed. Right heating fan speed too low!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0079-chamber-heating-failed-right-heating-fan-speed-too-low";

    pair = s_errorCodeDataMap.emplace("E0080", error_code_data_t());
    pair.first->second.message = "Abnormal chamber temperature. Air outlet temperature sensor may be damaged!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0080-abnormal-chamber-temperature-air-outlet-temperature-sensor-may-be-damaged";

    pair = s_errorCodeDataMap.emplace("E0083", error_code_data_t());
    pair.first->second.message = "Air filter fan speed too low or stopped!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0083-air-filter-fan-speed-too-low-or-stopped";

    pair = s_errorCodeDataMap.emplace("E0100", error_code_data_t());
    pair.first->second.message = "Channel 1 feeding timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0100-channel-1-feeding-timeout";

    pair = s_errorCodeDataMap.emplace("E0101", error_code_data_t());
    pair.first->second.message = "Channel 2 feeding timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0101-channel-2-feeding-timeout";

    pair = s_errorCodeDataMap.emplace("E0102", error_code_data_t());
    pair.first->second.message = "Channel 3 feeding timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0102-channel-3-feeding-timeout";

    pair = s_errorCodeDataMap.emplace("E0103", error_code_data_t());
    pair.first->second.message = "Channel 4 feeding timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0103-channel-4-feeding-timeout";

    pair = s_errorCodeDataMap.emplace("E0104", error_code_data_t());
    pair.first->second.message = "Channel 1 retracting timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0104-channel-1-retracting-timeout";

    pair = s_errorCodeDataMap.emplace("E0105", error_code_data_t());
    pair.first->second.message = "Channel 2 retracting timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0105-channel-2-retracting-timeout";

    pair = s_errorCodeDataMap.emplace("E0106", error_code_data_t());
    pair.first->second.message = "Channel 3 retracting timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0106-channel-3-retracting-timeout";

    pair = s_errorCodeDataMap.emplace("E0107", error_code_data_t());
    pair.first->second.message = "Channel 4 retracting timeout!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0107-channel-4-retracting-timeout";

    pair = s_errorCodeDataMap.emplace("E0108", error_code_data_t());
    pair.first->second.message = "Failed to feed filament to the extruder!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0108-failed-to-feed-filament-to-the-extruder";

    pair = s_errorCodeDataMap.emplace("E0109", error_code_data_t());
    pair.first->second.message = "IFS odometer roller 1/2/3/4 not moving!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0109-ifs-odometer-roller-1234-not-moving";

    pair = s_errorCodeDataMap.emplace("E0110", error_code_data_t());
    pair.first->second.message = "Filament type mismatch!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0110-filament-type-mismatch";

    pair = s_errorCodeDataMap.emplace("E0111", error_code_data_t());
    pair.first->second.message = "Abnormal leveling data!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0111-abnormal-leveling-data";

    pair = s_errorCodeDataMap.emplace("E0112", error_code_data_t());
    pair.first->second.message = "Z-axis print height exceeded";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0112-leveling-triggered-early";

    pair = s_errorCodeDataMap.emplace("E0113", error_code_data_t());
    pair.first->second.message = "Extruder filament sensor error";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0113-filament-is-detected-in-the-extruder-please-check-the-feeding-position-and-clean-it-manually-or-check-the-condition-of-the-nozzle-sensor";

    pair = s_errorCodeDataMap.emplace("E0114", error_code_data_t());
    pair.first->second.message = "IFS homing error";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0114-ifs-homing-error";

    pair = s_errorCodeDataMap.emplace("E0115", error_code_data_t());
    pair.first->second.message = "Lidar focus failure detected. Please check the Lidar.";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0115-the-laser-radar-has-failed-to-focus-please-check-the-laser-radar";

    pair = s_errorCodeDataMap.emplace("E0116", error_code_data_t());
    pair.first->second.message = "Exception in priming_handler, please copy the logs!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0116-exception-in-priming_handler-please-copy-the-logs";

    pair = s_errorCodeDataMap.emplace("E0117", error_code_data_t());
    pair.first->second.message = "Exception in flush_handler, please copy the logs!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0117-exception-in-flush_handler-please-copy-the-logs";

    pair = s_errorCodeDataMap.emplace("E0118", error_code_data_t());
    pair.first->second.message = "Internal error on command, please copy the logs!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0118-internal-error-on-command-please-copy-the-logs";

    pair = s_errorCodeDataMap.emplace("E0119", error_code_data_t());
    pair.first->second.message = "Timing error, please copy the logs!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0119-timing-error-please-copy-the-logs";

    pair = s_errorCodeDataMap.emplace("E0120", error_code_data_t());
    pair.first->second.message = "System restarted, please copy the logs!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0120-system-restarted-please-copy-the-logs";

    pair = s_errorCodeDataMap.emplace("E0121", error_code_data_t());
    pair.first->second.message = "The \"spi_transfer_response\" cannot be obtained.!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0121-the-spi_transfer_response-cannot-be-obtained";

    pair = s_errorCodeDataMap.emplace("E0122", error_code_data_t());
    pair.first->second.message = "Extruder temperature error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0122-extruder-temperature-error";

    pair = s_errorCodeDataMap.emplace("E0123", error_code_data_t());
    pair.first->second.message = "platform temperature error!";
    pair.first->second.wikiUrl = "https://wiki.flashforge.com/en/ad5x/error_code_list_ad5x#e0123-platform-temperature-error";
}

}} // namespace Slic3r::GUI
