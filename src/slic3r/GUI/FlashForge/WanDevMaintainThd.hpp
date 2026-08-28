#ifndef slic3r_GUI_WanDevMaintainThd_hpp_
#define slic3r_GUI_WanDevMaintainThd_hpp_

#include <cstdint>
#include <atomic>
#include <boost/thread/mutex.hpp>
#include <boost/thread/thread.hpp>
#include <wx/event.h>
#include "FlashNetworkIntfc.h"
#include "MultiComDef.hpp"
#include "WaitEvent.hpp"
#include "WanDevTokenMgr.hpp"

namespace Slic3r { namespace GUI {

struct ReloginHttpEvent : public wxCommandEvent {
    ComErrno ret;
    std::string clientId;
    std::string accessToken;
    com_user_profile_t userProfile;
    fnet_wan_dev_info_t *devInfos;
    int devCnt;
};

struct GetWanDevEvent : public wxCommandEvent {
    ComErrno ret;
    std::string clientId;
    fnet_wan_dev_info_t *devInfos;
    int devCnt;
};

wxDECLARE_EVENT(RELOGIN_HTTP_EVENT, ReloginHttpEvent);
wxDECLARE_EVENT(GET_WAN_DEV_EVENT, GetWanDevEvent);

class WanDevMaintainThd : public wxEvtHandler
{
public:
    WanDevMaintainThd(fnet::FlashNetworkIntfc *networkIntfc);

    void exit();

    void setReqHeaders(const std::string &clientId, int64_t appId, int64_t platId);

    void setReloginHttp();

    void setUpdateWanDev();

    void setUpdateUserProfile();

    void stop();

private:
    void run();

    void getReqHeaders(std::string &clientId, int64_t &appId, int64_t &platId);

    bool reloginHttp(const std::string &clientId, int64_t &appId, int64_t &platId,
        ScopedWanDevToken &scopedToken);

    void updateWanDev(const std::string &clientId, int64_t &appId, int64_t &platId,
        const std::string &accessToken);

    void updateUserProfile(const std::string &accessToken);

private:
    std::string             m_clientId;
    int64_t                 m_appId;
    int64_t                 m_platId;
    boost::mutex            m_reqHeadersMutex;
    WaitEvent               m_loopWaitEvent;
    std::atomic_bool        m_reloginHttp;
    std::atomic_bool        m_updateWanDev;
    std::atomic_bool        m_updateUserProfile;
    std::atomic_bool        m_exitThread;
    boost::thread           m_thread;
    fnet::FlashNetworkIntfc*m_networkIntfc;
};

}} // namespace Slic3r::GUI

#endif
