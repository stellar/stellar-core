#include "overlay/OverlayAppConnector.h"
#include "herder/Herder.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "util/Timer.h"

namespace stellar
{

Herder&
OverlayAppConnector::getHerder()
{
    releaseAssert(threadIsMain());
    return mApp.getHerder();
}

LedgerManager&
OverlayAppConnector::getLedgerManager()
{
    releaseAssert(threadIsMain());
    return mApp.getLedgerManager();
}

OverlayManager&
OverlayAppConnector::getOverlayManager()
{
    releaseAssert(threadIsMain());
    return mApp.getOverlayManager();
}

BanManager&
OverlayAppConnector::getBanManager()
{
    releaseAssert(threadIsMain());
    return mApp.getBanManager();
}

void
OverlayAppConnector::postOnMainThread(std::function<void()>&& f,
                                      std::string&& message,
                                      Scheduler::ActionType type)
{
    releaseAssert(threadIsMain());
    mApp.postOnMainThread(std::move(f), std::move(message), type);
}

Config const&
OverlayAppConnector::getConfig() const
{
    releaseAssert(threadIsMain());
    return mApp.getConfig();
}

bool
OverlayAppConnector::overlayShuttingDown() const
{
    releaseAssert(threadIsMain());
    return mApp.getOverlayManager().isShuttingDown();
}

VirtualClock::time_point
OverlayAppConnector::now() const
{
    releaseAssert(threadIsMain());
    return mApp.getClock().now();
}

bool
OverlayAppConnector::shouldYield() const
{
    releaseAssert(threadIsMain());
    return mApp.getClock().shouldYield();
}
}