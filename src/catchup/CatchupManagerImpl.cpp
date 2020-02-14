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

    addToSyncingLedgers(ledgerData);
    assert(!mSyncingLedgers.empty());

    auto contiguous =
        lastReceivedLedgerSeq == mSyncingLedgers.back().getLedgerSeq();
    if (!mCatchupWork)
    {
        uint32_t catchupTriggerLedger =
            mApp.getHistoryManager().nextCheckpointLedger(
                lastReceivedLedgerSeq - 1) +
            1;

        if (lastReceivedLedgerSeq >= catchupTriggerLedger)
        {
            auto message =
                fmt::format("Starting catchup after ensuring checkpoint "
                            "ledger {} was closed on network",
                            catchupTriggerLedger);
            logAndUpdateCatchupStatus(contiguous, message);

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
            logAndUpdateCatchupStatus(contiguous, message);
        }
    }
    else
    {
        logAndUpdateCatchupStatus(contiguous);
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

    // mCatchupState = CatchupState::APPLYING_HISTORY;
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

void
CatchupManagerImpl::addToSyncingLedgers(LedgerCloseData const& ledgerData)
{
    switch (mSyncingLedgers.push(ledgerData))
    {
    case SyncingLedgerChainAddResult::CONTIGUOUS:
        // Normal close while catching up
        CLOG(INFO, "Ledger")
            << "Close of ledger " << ledgerData.getLedgerSeq() << " buffered";
        return;
    case SyncingLedgerChainAddResult::TOO_OLD:
        CLOG(INFO, "Ledger")
            << "Skipping close ledger: latest known is "
            << mSyncingLedgers.back().getLedgerSeq() << ", more recent than "
            << ledgerData.getLedgerSeq();
        return;
    case SyncingLedgerChainAddResult::TOO_NEW:
        // Out-of-order close while catching up; timeout / network failure?
        CLOG(WARNING, "Ledger")
            << "Out-of-order close during catchup, buffered to "
            << mSyncingLedgers.back().getLedgerSeq() << " but network closed "
            << ledgerData.getLedgerSeq();

        CLOG(WARNING, "Ledger")
            << "this round of catchup will fail and restart.";
        return;
    default:
        assert(false);
    }
}

bool
CatchupManagerImpl::hasBufferedLedger() const
{
    return !mSyncingLedgers.empty();
}

LedgerCloseData
CatchupManagerImpl::popBufferedLedger()
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "popBufferedLedger called when mSyncingLedgers is empty!");
    }

    auto lcd = mSyncingLedgers.front();
    mSyncingLedgers.pop();
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());

    return lcd;
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
    mSyncingLedgers = {};
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}
}
