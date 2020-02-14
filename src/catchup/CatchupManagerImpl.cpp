// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "catchup/CatchupManagerImpl.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "util/format.h"
#include "work/WorkScheduler.h"

namespace stellar
{

std::unique_ptr<CatchupManager>
CatchupManager::create(Application& app)
{
    return std::make_unique<CatchupManagerImpl>(app);
}

CatchupManagerImpl::CatchupManagerImpl(Application& app)
    : mApp(app)
    , mCatchupWork(nullptr)
    , mSyncingLedgersSize(
          app.getMetrics().NewCounter({"ledger", "memory", "queued-ledgers"}))
{
}

CatchupManagerImpl::~CatchupManagerImpl()
{
}

uint32_t
getCatchupCount(Application& app)
{
    return app.getConfig().CATCHUP_COMPLETE
               ? std::numeric_limits<uint32_t>::max()
               : app.getConfig().CATCHUP_RECENT;
}

void
CatchupManagerImpl::processLedger(LedgerCloseData const& ledgerData)
{
    uint32_t lastReceivedLedgerSeq = ledgerData.getLedgerSeq();

    // mSyncingLedgers can be non-empty after mCatchupWork finishes if there was
    // a failure during catchup
    if (mCatchupWork && mCatchupWork->isDone())
    {
        if (!mSyncingLedgers.empty())
        {
            resetSyncingLedgers();
        }
        mCatchupWork.reset();
    }

    auto addLedgerData = [&]() { mSyncingLedgers.push_back(ledgerData); };

    if (!mCatchupWork)
    {
        uint32_t checkpointLedger =
            mApp.getHistoryManager().nextCheckpointLedger(
                lastReceivedLedgerSeq - 1);

        // We wait until checkpoint + 1 ledger is closed before we start
        // catchup, but we want to catchup to checkpoint - 1 so we don't have to
        // wait for the next checkpoint snapshot. We need to buffer the
        // checkpoint ledger because of this.
        if (lastReceivedLedgerSeq == checkpointLedger)
        {
            addLedgerData();
        }

        uint32_t catchupTriggerLedger = checkpointLedger + 1;
        if (lastReceivedLedgerSeq >= catchupTriggerLedger)
        {
            addLedgerData();

            auto message =
                fmt::format("Starting catchup after ensuring checkpoint "
                            "ledger {} was closed on network",
                            catchupTriggerLedger);
            logAndUpdateCatchupStatus(true, message);

            // catchup just before first buffered ledger that way we will have a
            // way to verify history consistency - compare previousLedgerHash of
            // buffered ledger with last one downloaded from history
            auto firstBufferedLedgerSeq =
                mSyncingLedgers.front().getLedgerSeq();
            auto hash = make_optional<Hash>(
                mSyncingLedgers.front().getTxSet()->previousLedgerHash());
            startCatchup({LedgerNumHashPair(firstBufferedLedgerSeq - 1, hash),
                          getCatchupCount(mApp),
                          CatchupConfiguration::Mode::ONLINE},
                         nullptr);
        }
        else
        {
            auto eta = (catchupTriggerLedger - lastReceivedLedgerSeq) *
                       mApp.getConfig().getExpectedLedgerCloseTime();
            auto message = fmt::format(
                "Waiting for trigger ledger: {}/{}, ETA: {}s",
                lastReceivedLedgerSeq, catchupTriggerLedger, eta.count());
            logAndUpdateCatchupStatus(true, message);
        }
    }
    else
    {
        addLedgerData();
        logAndUpdateCatchupStatus(true);
    }
}

void
CatchupManagerImpl::startCatchup(CatchupConfiguration configuration,
                                 std::shared_ptr<HistoryArchive> archive)
{
    auto lastClosedLedger = mApp.getLedgerManager().getLastClosedLedgerNum();
    if ((configuration.toLedger() != CatchupConfiguration::CURRENT) &&
        (configuration.toLedger() <= lastClosedLedger))
    {
        throw std::invalid_argument("Target ledger is not newer than LCL");
    }

    auto offlineCatchup = configuration.offline();
    assert(offlineCatchup == mSyncingLedgers.empty());

    assert(!mCatchupWork || mCatchupWork->isDone());

    mCatchupWork = mApp.getWorkScheduler().scheduleWork<CatchupWork>(
        configuration, archive);
}

std::string
CatchupManagerImpl::getStatus() const
{
    return mCatchupWork ? mCatchupWork->getStatus() : std::string{};
}

BasicWork::State
CatchupManagerImpl::getCatchupWorkState() const
{
    assert(mCatchupWork);
    return mCatchupWork->getState();
}

bool
CatchupManagerImpl::catchupWorkIsDone() const
{
    return mCatchupWork && mCatchupWork->isDone();
}

bool
CatchupManagerImpl::isCatchupInitialized() const
{
    return mCatchupWork != nullptr;
}

void
CatchupManagerImpl::logAndUpdateCatchupStatus(bool contiguous,
                                              std::string const& message)
{
    if (!message.empty())
    {
        auto contiguousString =
            contiguous ? "" : " (discontiguous; will fail and restart)";
        auto state =
            fmt::format("Catching up{}: {}", contiguousString, message);
        auto existing = mApp.getStatusManager().getStatusMessage(
            StatusCategory::HISTORY_CATCHUP);
        if (existing != state)
        {
            CLOG(INFO, "History") << state;
            mApp.getStatusManager().setStatusMessage(
                StatusCategory::HISTORY_CATCHUP, state);
        }
    }
    else
    {
        mApp.getStatusManager().removeStatusMessage(
            StatusCategory::HISTORY_CATCHUP);
    }
}

void
CatchupManagerImpl::logAndUpdateCatchupStatus(bool contiguous)
{
    logAndUpdateCatchupStatus(contiguous, getStatus());
}

bool
CatchupManagerImpl::hasBufferedLedger() const
{
    return !mSyncingLedgers.empty();
}

LedgerCloseData const&
CatchupManagerImpl::getBufferedLedger() const
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "getBufferedLedger called when mSyncingLedgers is empty!");
    }

    return mSyncingLedgers.front();
}

void
CatchupManagerImpl::popBufferedLedger()
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "popBufferedLedger called when mSyncingLedgers is empty!");
    }

    mSyncingLedgers.pop_front();
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}

void
CatchupManagerImpl::syncMetrics()
{
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}

void
CatchupManagerImpl::reset()
{
    mCatchupWork.reset();
    resetSyncingLedgers();
}

void
CatchupManagerImpl::resetSyncingLedgers()
{
    mSyncingLedgers.clear();
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}
}
