// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "catchup/CatchupManagerImpl.h"
#include "catchup/CatchupConfiguration.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "work/WorkScheduler.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

template <typename T>
T
findFirstCheckpoint(T begin, T end, HistoryManager const& hm)
{
    return std::find_if(begin, end,
                        [&hm](std::pair<uint32_t, LedgerCloseData> const& kvp) {
                            return hm.isFirstLedgerInCheckpoint(kvp.first);
                        });
}

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
    ZoneScoped;
    if (mCatchupWork && mCatchupWork->isDone())
    {
        trimAndReset();
    }

    uint32_t lastReceivedLedgerSeq = ledgerData.getLedgerSeq();

    // 1. CatchupWork is not running yet
    // 2. CatchupManager received  ledger that was immediately applied by
    // LedgerManager: check if we have any sequential ledgers.
    // If so, attempt to apply mSyncingLedgers and possibly get back in sync
    if (!mCatchupWork && lastReceivedLedgerSeq ==
                             mApp.getLedgerManager().getLastClosedLedgerNum())
    {
        tryApplySyncingLedgers();
        return;
    }
    else if (lastReceivedLedgerSeq <=
             mApp.getLedgerManager().getLastClosedLedgerNum())
    {
        // If LCL is already at-or-ahead of the ledger we just received from the
        // network, we're up to date. Return early, nothing to do.
        return;
    }

    // For the rest of this method: we know LCL has fallen behind the network
    // and we must buffer this ledger, we only need to decide whether to start
    // the CatchupWork state machine.
    //
    // Assuming we fell out of sync at ledger K, we wait for the first ledger L
    // of the checkpoint following K to start catchup. When we
    // reach L+1 we assume the checkpoint covering K has probably been published
    // to history and commence catchup, running the (checkpoint-driven) catchup
    // state machine to ledger L-1 (the end of the checkpoint covering K) and
    // then replay buffered ledgers from L onwards.

    // First: if CatchupWork has started, just buffer and return early.
    if (mCatchupWork)
    {
        // we don't want to buffer any ledgers at or below where we are catching
        // up to
        auto const& config = mCatchupWork->getCatchupConfiguration();
        if (ledgerData.getLedgerSeq() <= config.toLedger())
        {
            return;
        }

        addToSyncingLedgers(ledgerData);
        logAndUpdateCatchupStatus(true);
        return;
    }

    // Next, we buffer every out of sync ledger to allow us to get back in sync
    // in case the ledgers we're missing are received.
    addToSyncingLedgers(ledgerData);

    // Finally we wait some number of ledgers beyond the smallest buffered
    // checkpoint ledger before we trigger the CatchupWork. This could be any
    // number, for the sake of simplicity at the moment it's set to one ledger
    // after the first buffered one. Since we can receive out of order ledgers,
    // we just check for any ledger larger than the checkpoint

    auto& hm = mApp.getHistoryManager();
    auto it =
        findFirstCheckpoint(mSyncingLedgers.begin(), mSyncingLedgers.end(), hm);

    std::string message;
    uint32_t lastLedgerInBuffer = mSyncingLedgers.crbegin()->first;
    if (mApp.getConfig().MODE_DOES_CATCHUP && it != mSyncingLedgers.end() &&
        it->first < lastLedgerInBuffer)
    {
        message = fmt::format("Starting catchup after ensuring checkpoint "
                              "ledger {} was closed on network",
                              lastLedgerInBuffer);

        // We only need ledgers starting from the checkpoint. We can
        // remove all ledgers before this
        mSyncingLedgers.erase(mSyncingLedgers.begin(), it);

        startOnlineCatchup();
    }
    else
    {
        // get the smallest checkpoint we need to start catchup
        uint32_t firstLedgerInBuffer = mSyncingLedgers.cbegin()->first;
        uint32_t requiredFirstLedgerInCheckpoint =
            hm.isFirstLedgerInCheckpoint(firstLedgerInBuffer)
                ? firstLedgerInBuffer
                : hm.firstLedgerAfterCheckpointContaining(firstLedgerInBuffer);

        uint32_t catchupTriggerLedger =
            hm.ledgerToTriggerCatchup(requiredFirstLedgerInCheckpoint);

        // If the trigger ledger is behind the last ledger, that means we're
        // waiting for out of order ledgers, which should arrive quickly
        if (catchupTriggerLedger > lastLedgerInBuffer)
        {
            auto eta = (catchupTriggerLedger - lastLedgerInBuffer) *
                       mApp.getConfig().getExpectedLedgerCloseTime();
            message = fmt::format("Waiting for trigger ledger: {}/{}, ETA: {}s",
                                  lastLedgerInBuffer, catchupTriggerLedger,
                                  eta.count());
        }
        else
        {
            message = fmt::format(
                "Waiting for out-of-order ledger(s). Trigger ledger: {}",
                catchupTriggerLedger);
        }
    }
    logAndUpdateCatchupStatus(true, message);
}

void
CatchupManagerImpl::startCatchup(CatchupConfiguration configuration,
                                 std::shared_ptr<HistoryArchive> archive)
{
    ZoneScoped;
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
            CLOG_INFO(History, "{}", state);
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
CatchupManagerImpl::getFirstBufferedLedger() const
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "getFirstBufferedLedger called when mSyncingLedgers is empty!");
    }

    return mSyncingLedgers.cbegin()->second;
}

LedgerCloseData const&
CatchupManagerImpl::getLastBufferedLedger() const
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "getLastBufferedLedger called when mSyncingLedgers is empty!");
    }

    return mSyncingLedgers.crbegin()->second;
}

void
CatchupManagerImpl::popBufferedLedger()
{
    if (!hasBufferedLedger())
    {
        throw std::runtime_error(
            "popBufferedLedger called when mSyncingLedgers is empty!");
    }

    mSyncingLedgers.erase(mSyncingLedgers.cbegin());
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
    mSyncingLedgers.emplace(ledgerData.getLedgerSeq(), ledgerData);

    CLOG_INFO(Ledger, "Close of ledger {} buffered", ledgerData.getLedgerSeq());
}

void
CatchupManagerImpl::startOnlineCatchup()
{
    assert(mSyncingLedgers.size() > 1);

    // catchup just before first buffered ledger that way we will have a
    // way to verify history consistency - compare previousLedgerHash of
    // buffered ledger with last one downloaded from history
    auto const& lcd = mSyncingLedgers.begin()->second;
    auto firstBufferedLedgerSeq = lcd.getLedgerSeq();
    auto hash = make_optional<Hash>(lcd.getTxSet()->previousLedgerHash());
    startCatchup({LedgerNumHashPair(firstBufferedLedgerSeq - 1, hash),
                  getCatchupCount(), CatchupConfiguration::Mode::ONLINE},
                 nullptr);
}

void
CatchupManagerImpl::trimSyncingLedgers()
{
    // Look for newest checkpoint ledger by using a reverse iterator
    auto rit =
        findFirstCheckpoint(mSyncingLedgers.rbegin(), mSyncingLedgers.rend(),
                            mApp.getHistoryManager());

    // only keep ledgers after start of the latest checkpoint. If no checkpoint
    // exists, then do nothing. We don't want to erase mSyncingLedgers in case
    // we receive the missing ledgers and recover
    if (rit != mSyncingLedgers.rend())
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
}

void
CatchupManagerImpl::tryApplySyncingLedgers()
{
    ZoneScoped;
    auto const& ledgerHeader =
        mApp.getLedgerManager().getLastClosedLedgerHeader();

    // We can apply multiple ledgers here, which might be slow. This is a rare
    // occurrence so we should be fine.
    auto it = mSyncingLedgers.cbegin();
    while (it != mSyncingLedgers.cend())
    {
        auto const& lcd = it->second;

        // we still have a missing ledger
        if (ledgerHeader.header.ledgerSeq + 1 != lcd.getLedgerSeq())
        {
            break;
        }

        mApp.getLedgerManager().closeLedger(lcd);
        CLOG_INFO(History, "Closed buffered ledger: {}",
                  LedgerManager::ledgerAbbrev(ledgerHeader));

        ++it;
    }

    mSyncingLedgers.erase(mSyncingLedgers.cbegin(), it);
}
}
