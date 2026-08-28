#include "WanDevMaintainThd.hpp"
#include <boost/bind/bind.hpp>
#include "FreeInDestructor.h"
#include "MultiComEvent.hpp"
#include "MultiComUtils.hpp"
#include "WanDevTokenMgr.hpp"

namespace Slic3r { namespace GUI {

wxDEFINE_EVENT(RELOGIN_HTTP_EVENT, ReloginHttpEvent);
wxDEFINE_EVENT(GET_WAN_DEV_EVENT, GetWanDevEvent);

WanDevMaintainThd::WanDevMaintainThd(fnet::FlashNetworkIntfc *networkIntfc)
    : m_reloginHttp(false)
    , m_updateWanDev(false)
    , m_updateUserProfile(false)
    , m_exitThread(false)
    , m_thread(boost::bind(&WanDevMaintainThd::run, this))
    , m_networkIntfc(networkIntfc)
{
}

void WanDevMaintainThd::exit()
{
    stop();
    m_exitThread = true;
    m_loopWaitEvent.set(true);
    m_thread.join();
}

void WanDevMaintainThd::setReqHeaders(const std::string &clientId, int64_t appId, int64_t platId)
{
    boost::mutex::scoped_lock lock(m_reqHeadersMutex);
    m_clientId = clientId;
    m_appId = appId;
    m_platId = platId;
}

void WanDevMaintainThd::setReloginHttp()
{
    m_reloginHttp = true;
    m_loopWaitEvent.set(true);
}

void WanDevMaintainThd::setUpdateUserProfile()
{
    m_updateUserProfile = true;
    m_loopWaitEvent.set(true);
}

void WanDevMaintainThd::setUpdateWanDev()
{
    m_updateWanDev = true;
    m_loopWaitEvent.set(true);
}

void WanDevMaintainThd::stop()
{
    m_reloginHttp = false;
    m_updateWanDev = false;
    m_updateUserProfile = false;
}

void WanDevMaintainThd::run()
{
    while (!m_exitThread) {
        m_loopWaitEvent.waitTrue(5000);
        m_loopWaitEvent.set(false);
        if (!m_reloginHttp && !m_updateWanDev && !m_updateUserProfile) {
            continue;
        }
        std::string clientId;
        int64_t appId, platId;
        getReqHeaders(clientId, appId, platId);
        if (m_reloginHttp) {
            ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
            if (reloginHttp(clientId, appId, platId, token)) {
                m_reloginHttp = false;
            }
        } else {
            ScopedWanDevToken token = WanDevTokenMgr::inst()->getScopedToken();
            if (m_updateWanDev) {
                m_updateWanDev = false;
                updateWanDev(clientId, appId, platId, token.accessToken());
            }
            if (m_updateUserProfile) {
                m_updateUserProfile = false;
                updateUserProfile(token.accessToken());
            }
        }
    }
}

void WanDevMaintainThd::getReqHeaders(std::string &clientId, int64_t &appId, int64_t &platId)
{
    boost::mutex::scoped_lock lock(m_reqHeadersMutex);
    clientId = m_clientId;
    appId = m_appId;
    platId = m_platId;
}

bool WanDevMaintainThd::reloginHttp(const std::string &clientId, int64_t &appId, int64_t &platId,
    ScopedWanDevToken &scopedToken)
{
    com_user_profile_t userProfile;
    ComErrno ret = MultiComUtils::getUserProfile(scopedToken.accessToken(), userProfile, ComTimeoutWanB);
    if (m_reloginHttp && ret == COM_UNAUTHORIZED) {
        ret = WanDevTokenMgr::inst()->refreshToken(scopedToken);
        if (m_reloginHttp && ret == COM_OK) {
            ret = MultiComUtils::getUserProfile(scopedToken.accessToken(), userProfile, ComTimeoutWanB);
        }
    }
    fnet_wan_dev_info_t *devInfos = nullptr;
    int devCnt = 0;
    if (m_reloginHttp && ret == COM_OK) {
        ret = MultiComUtils::fnetRet2ComErrno(m_networkIntfc->getWanDevList(clientId.c_str(),
            scopedToken.accessToken().c_str(), appId, platId, &devInfos, &devCnt, ComTimeoutWanB));
    }
    if (m_reloginHttp) {
        ReloginHttpEvent *event = new ReloginHttpEvent;
        event->SetEventType(RELOGIN_HTTP_EVENT);
        event->ret = ret;
        event->clientId = clientId;
        event->accessToken = scopedToken.accessToken();
        event->userProfile = userProfile;
        event->devInfos = devInfos;
        event->devCnt = devCnt;
        QueueEvent(event);
        return ret == COM_OK;
    } else {
        m_networkIntfc->freeWanDevList(devInfos, devCnt);
        return false;
    }
}

void WanDevMaintainThd::updateWanDev(const std::string &clientId, int64_t &appId, int64_t &platId,
    const std::string &accessToken)
{
    int tryCnt = 3;
    int fnetRet = FNET_OK;
    fnet_wan_dev_info_t *devInfos = nullptr;
    int devCnt = 0;
    for (int i = 0; i < tryCnt && !m_exitThread; ++i) {
        auto getWanDevList =  m_networkIntfc->getWanDevList;
        fnetRet = getWanDevList(clientId.c_str(), accessToken.c_str(), appId, platId, &devInfos, &devCnt,
            ComTimeoutWanB);
        if (fnetRet == FNET_OK || fnetRet == FNET_UNAUTHORIZED || m_exitThread) {
            break;
        } else if (i + 1 < tryCnt) {
            int sleepTimes[] = {1, 3, 5};
            boost::this_thread::sleep_for(boost::chrono::seconds(sleepTimes[i < 3 ? i : 2]));
        }
    }
    if (!m_exitThread) {
        GetWanDevEvent *event = new GetWanDevEvent;
        event->SetEventType(GET_WAN_DEV_EVENT);
        event->ret = MultiComUtils::fnetRet2ComErrno(fnetRet);
        event->clientId = clientId;
        event->devInfos = devInfos;
        event->devCnt = devCnt;
        QueueEvent(event);
    } else {
        m_networkIntfc->freeWanDevList(devInfos, devCnt);
    }
}

void WanDevMaintainThd::updateUserProfile(const std::string &accessToken)
{
    int tryCnt = 3;
    ComErrno ret = COM_OK;
    com_user_profile_t userProfile;
    for (int i = 0; i < tryCnt && !m_exitThread; ++i) {
        ret = MultiComUtils::getUserProfile(accessToken, userProfile, ComTimeoutWanB);
        if (ret == COM_OK || ret == COM_UNAUTHORIZED || m_exitThread) {
            break;
        } else if (i + 1 < tryCnt) {
            int sleepTimes[] = {1, 3, 5};
            boost::this_thread::sleep_for(boost::chrono::seconds(sleepTimes[i < 3 ? i : 2]));
        }
    }
    if (!m_exitThread) {
        QueueEvent(new ComGetUserProfileEvent(COM_GET_USER_PROFILE_EVENT, userProfile, ret));
    }
}

}} // namespace Slic3r::GUI
