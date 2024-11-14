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
    : mApp(app), mConfig(std::make_shared<const Config>(app.getConfig()))
{
}

Herder&
AppConnector::getHerder() const
{
    releaseAssert(threadIsMain());
    return mApp.getHerder();
}

LedgerManager&
AppConnector::getLedgerManager() const
{
    releaseAssert(threadIsMain());
    return mApp.getLedgerManager();
}

OverlayManager&
AppConnector::getOverlayManager() const
{
    releaseAssert(threadIsMain());
    return mApp.getOverlayManager();
}

BanManager&
AppConnector::getBanManager() const
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

std::optional<SorobanNetworkConfig>
AppConnector::maybeGetSorobanNetworkConfigReadOnly() const
{
    releaseAssert(threadIsMain());
    if (mApp.getLedgerManager().hasSorobanNetworkConfig())
    {
        return mApp.getLedgerManager().getSorobanNetworkConfigReadOnly();
    }
    return std::nullopt;
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
                                    LedgerTxnDelta const& ltxDelta)
{
    mApp.getInvariantManager().checkOnOperationApply(operation, opres,
                                                     ltxDelta);
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
    return *mConfig;
}

std::shared_ptr<Config const>
AppConnector::getConfigPtr() const
{
    return mConfig;
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

VirtualClock::system_time_point
AppConnector::system_now() const
{
    // TODO: Is this thread safe? It looks like it is when in REAL_TIME mode,
    // but I'm not so sure about VIRTUAL_TIME mode as that mode has a
    // `mVirtualNow` that looks like it can change during access? The same is
    // true for `AppConnector::now`, which is marked "thread safe" in the header
    // file. Maybe both of these need some hardening though?
    return mApp.getClock().system_now();
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