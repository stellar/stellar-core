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
CatchupManagerImpl::getCatchupCount()
{
    return mApp.getConfig().CATCHUP_COMPLETE
               ? std::numeric_limits<uint32_t>::max()
               : mApp.getConfig().CATCHUP_RECENT;
}

void
CatchupManagerImpl::processLedger(LedgerCloseData const& ledgerData)
{
    if (mCatchupWork && mCatchupWork->isDone())
    {
        trimAndReset();
    }

    uint32_t lastReceivedLedgerSeq = ledgerData.getLedgerSeq();

    // If LCL is already at-or-ahead of the ledger we just received from the
    // network, we're up to date. Return early, nothing to do.
    if (lastReceivedLedgerSeq <=
        mApp.getLedgerManager().getLastClosedLedgerNum())
    {
        return;
    }

    // For the rest of this method: we know LCL has fallen behind the network,
    // we only need to decide whether to buffer the newly received ledger and/or
    // start the CatchupWork state machine.
    //
    // Assuming we fell out of sync at ledger K, we wait for the first ledger L
    // of the checkpoint following K, and begin buffering from there. When we
    // reach L+1 we assume the checkpoint covering K has probably been published
    // to history and commence catchup, running the (checkpoint-driven) catchup
    // state machine to ledger L-1 (the end of the checkpoint covering K) and
    // then replay buffered ledgers from L onwards.

    // First: if CatchupWork has started, just buffer and return early.
    if (mCatchupWork)
    {
        addToSyncingLedgers(ledgerData);
        logAndUpdateCatchupStatus(true);
        return;
    }

    // Next switch between 3 cases: starting buffering, waiting to
    // buffer, and already buffering.
    auto& hm = mApp.getHistoryManager();
    uint32_t firstLedgerInBuffer;
    if (mSyncingLedgers.empty())
    {
        if (hm.isFirstLedgerInCheckpoint(lastReceivedLedgerSeq))
        {
            // Case 1: At the ledger where we should start buffering.
            addToSyncingLedgers(ledgerData);
            firstLedgerInBuffer = lastReceivedLedgerSeq;
        }
        else
        {
            // Case 2: Still waiting to start buffering.
            firstLedgerInBuffer =
                hm.firstLedgerAfterCheckpointContaining(lastReceivedLedgerSeq);
        }
    }
    else
    {
        // Case 3: Already buffering, keep buffering.
        addToSyncingLedgers(ledgerData);
        firstLedgerInBuffer = mSyncingLedgers.front().getLedgerSeq();
    }
    assert(hm.isFirstLedgerInCheckpoint(firstLedgerInBuffer));

    // Finally we wait some number of ledgers beyond the first buffered
    // ledger before we trigger the CatchupWork. This could be any number,
    // for the sake of simplicity at the moment it's set to one ledger
    // after the first buffered one.
    std::string message;
    uint32_t catchupTriggerLedger =
        hm.ledgerToTriggerCatchup(firstLedgerInBuffer);
    if (lastReceivedLedgerSeq >= catchupTriggerLedger)
    {
        message = fmt::format("Starting catchup after ensuring checkpoint "
                              "ledger {} was closed on network",
                              catchupTriggerLedger);
        startOnlineCatchup();
    }
    else
    {
        auto eta = (catchupTriggerLedger - lastReceivedLedgerSeq) *
                   mApp.getConfig().getExpectedLedgerCloseTime();
        message = fmt::format("Waiting for trigger ledger: {}/{}, ETA: {}s",
                              lastReceivedLedgerSeq, catchupTriggerLedger,
                              eta.count());
    }
    logAndUpdateCatchupStatus(true, message);
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

    assert(!mCatchupWork);

    // NB: if WorkScheduler is aborting this returns nullptr, but that
    // which means we don't "really" start catchup.
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
            contiguous ? "" : " (discontiguous; will fail and restart): ";
        auto state = fmt::format("{}{}", contiguousString, message);
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
}

void
CatchupManagerImpl::syncMetrics()
{
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}

void
CatchupManagerImpl::trimAndReset()
{
    assert(mCatchupWork);
    mCatchupWork.reset();

    logAndUpdateCatchupStatus(true);
    trimSyncingLedgers();
}

void
CatchupManagerImpl::addToSyncingLedgers(LedgerCloseData const& ledgerData)
{
    mSyncingLedgers.push_back(ledgerData);

    CLOG(INFO, "Ledger") << "Close of ledger " << ledgerData.getLedgerSeq()
                         << " buffered";
}

void
CatchupManagerImpl::startOnlineCatchup()
{
    assert(mSyncingLedgers.size() > 1);

    // catchup just before first buffered ledger that way we will have a
    // way to verify history consistency - compare previousLedgerHash of
    // buffered ledger with last one downloaded from history
    auto firstBufferedLedgerSeq = mSyncingLedgers.front().getLedgerSeq();
    auto hash = make_optional<Hash>(
        mSyncingLedgers.front().getTxSet()->previousLedgerHash());
    startCatchup({LedgerNumHashPair(firstBufferedLedgerSeq - 1, hash),
                  getCatchupCount(), CatchupConfiguration::Mode::ONLINE},
                 nullptr);
}

void
CatchupManagerImpl::trimSyncingLedgers()
{
    // look for the latest checkpoint
    auto rit = mSyncingLedgers.rbegin();
    auto rend = mSyncingLedgers.rend();
    auto& hm = mApp.getHistoryManager();
    while (rit != rend)
    {
        if (hm.isFirstLedgerInCheckpoint(rit->getLedgerSeq()))
        {
            break;
        }
        ++rit;
    }

    // only keep ledgers after start of the latest checkpoint (if it exists)
    if (rit != rend)
    {
        // rit points to a ledger that's the first in a checkpoint, like 64 or
        // 128; rit.base() is the underlying forward iterator one _past_ (in
        // forward-order) the value pointed-to by rit. In other words
        // (++rit).base() is the forward iterator pointing to the same ledger 64
        // or 128 or such.
        //
        // We then erase the half-open range [begin, (++rit).base()) which is
        // like [..., 64) or [..., 128), which leaves us with the checkpoint
        // range [64, ...) or [128, ...) in mSyncingLedgers.
        mSyncingLedgers.erase(mSyncingLedgers.begin(), (++rit).base());
    }
    else
    {
        mSyncingLedgers.clear();
    }
}
}
