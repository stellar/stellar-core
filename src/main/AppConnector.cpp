#include "main/AppConnector.h"
#include "herder/Herder.h"
#include "invariant/InvariantManager.h"
#include "ledger/LedgerManager.h"
#include "ledger/LedgerTxn.h"
#include "main/Application.h"
#include "overlay/BanManager.h"
#include "overlay/OverlayManager.h"
#include "overlay/OverlayMetrics.h"
#include "overlay/Peer.h"
#include "util/Timer.h"

namespace stellar
{

AppConnector::AppConnector(Application& app)
    : mApp(app), mConfig(app.getConfig())
{
}

Herder&
AppConnector::getHerder()
{
    releaseAssert(threadIsMain());
    return mApp.getHerder();
}

LedgerManager&
AppConnector::getLedgerManager()
{
    releaseAssert(threadIsMain());
    return mApp.getLedgerManager();
}

LedgerApplyManager&
AppConnector::getLedgerApplyManager()
{
    releaseAssert(threadIsMain());
    return mApp.getLedgerApplyManager();
}

OverlayManager&
AppConnector::getOverlayManager()
{
    releaseAssert(threadIsMain());
    return mApp.getOverlayManager();
}

BanManager&
AppConnector::getBanManager()
{
    releaseAssert(threadIsMain());
    return mApp.getBanManager();
}

SorobanNetworkConfig const&
AppConnector::getLastClosedSorobanNetworkConfig() const
{
    releaseAssert(threadIsMain());
    return mApp.getLedgerManager().getLastClosedSorobanNetworkConfig();
}

SorobanNetworkConfig const&
AppConnector::getSorobanNetworkConfigForApply() const
{
    return mApp.getLedgerManager().getSorobanNetworkConfigForApply();
}

medida::MetricsRegistry&
AppConnector::getMetrics() const
{
    return mApp.getMetrics();
}

SorobanMetrics&
AppConnector::getSorobanMetrics() const
{
    return mApp.getLedgerManager().getSorobanMetrics();
}

void
AppConnector::checkOnOperationApply(Operation const& operation,
                                    OperationResult const& opres,
                                    LedgerTxnDelta const& ltxDelta,
                                    std::vector<ContractEvent> const& events)
{
    mApp.getInvariantManager().checkOnOperationApply(operation, opres, ltxDelta,
                                                     events);
}

Hash const&
AppConnector::getNetworkID() const
{
    // NetworkID is a const
    return mApp.getNetworkID();
}

void
AppConnector::postOnMainThread(std::function<void()>&& f, std::string&& message,
                               Scheduler::ActionType type)
{
    mApp.postOnMainThread(std::move(f), std::move(message), type);
}

void
AppConnector::postOnOverlayThread(std::function<void()>&& f,
                                  std::string const& message)
{
    mApp.postOnOverlayThread(std::move(f), message);
}

Config const&
AppConnector::getConfig() const
{
    return mConfig;
}

rust::Box<rust_bridge::SorobanModuleCache>
AppConnector::getModuleCache()
{
    return mApp.getLedgerManager().getModuleCache();
}

bool
AppConnector::overlayShuttingDown() const
{
    return mApp.getOverlayManager().isShuttingDown();
}

VirtualClock::time_point
AppConnector::now() const
{
    return mApp.getClock().now();
}

bool
AppConnector::shouldYield() const
{
    releaseAssert(threadIsMain());
    return mApp.getClock().shouldYield();
}

OverlayMetrics&
AppConnector::getOverlayMetrics()
{
    // OverlayMetrics class is thread-safe
    return mApp.getOverlayManager().getOverlayMetrics();
}

bool
AppConnector::checkScheduledAndCache(
    std::shared_ptr<CapacityTrackedMessage> msgTracker)
{
    return mApp.getOverlayManager().checkScheduledAndCache(msgTracker);
}

bool
AppConnector::threadIsType(Application::ThreadType type) const
{
    return mApp.threadIsType(type);
}

SearchableHotArchiveSnapshotConstPtr
AppConnector::copySearchableHotArchiveBucketListSnapshot()
{
    return mApp.getBucketManager()
        .getBucketSnapshotManager()
        .copySearchableHotArchiveBucketListSnapshot();
}
}