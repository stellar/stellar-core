// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "catchup/CatchupManagerImpl.h"
#include "catchup/CatchupConfiguration.h"
#include "history/FileTransferInfo.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/StatusManager.h"
#include "work/WorkScheduler.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

CatchupManagerImpl::CatchupMetrics::CatchupMetrics()
    : mHistoryArchiveStatesDownloaded{0}
    , mCheckpointsDownloaded{0}
    , mLedgersVerified{0}
    , mLedgerChainsVerificationFailed{0}
    , mBucketsDownloaded{0}
    , mBucketsApplied{0}
    , mTxSetsDownloaded{0}
    , mTxSetsApplied{0}
{
}

CatchupManagerImpl::CatchupMetrics::CatchupMetrics(
    uint64_t historyArchiveStatesDownloaded, uint64_t checkpointsDownloaded,
    uint64_t ledgersVerified, uint64_t ledgerChainsVerificationFailed,
    uint64_t bucketsDownloaded, uint64_t bucketsApplied,
    uint64_t txSetsDownloaded, uint64_t txSetsApplied)
    : mHistoryArchiveStatesDownloaded{historyArchiveStatesDownloaded}
    , mCheckpointsDownloaded{checkpointsDownloaded}
    , mLedgersVerified{ledgersVerified}
    , mLedgerChainsVerificationFailed{ledgerChainsVerificationFailed}
    , mBucketsDownloaded{bucketsDownloaded}
    , mBucketsApplied{bucketsApplied}
    , mTxSetsDownloaded{txSetsDownloaded}
    , mTxSetsApplied{txSetsApplied}
{
}

CatchupManagerImpl::CatchupMetrics
operator-(CatchupManager::CatchupMetrics const& x,
          CatchupManager::CatchupMetrics const& y)
{
    return CatchupManager::CatchupMetrics{
        x.mHistoryArchiveStatesDownloaded - y.mHistoryArchiveStatesDownloaded,
        x.mCheckpointsDownloaded - y.mCheckpointsDownloaded,
        x.mLedgersVerified - y.mLedgersVerified,
        x.mLedgerChainsVerificationFailed - y.mLedgerChainsVerificationFailed,
        x.mBucketsDownloaded - y.mBucketsDownloaded,
        x.mBucketsApplied - y.mBucketsApplied,
        x.mTxSetsDownloaded - y.mTxSetsDownloaded,
        x.mTxSetsApplied - y.mTxSetsApplied};
}

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
    , mLargestLedgerSeqHeard(0)
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
    if (catchupWorkIsDone())
    {
        if (mCatchupWork->getState() == BasicWork::State::WORK_FAILURE &&
            mCatchupWork->fatalFailure())
        {
            CLOG_FATAL(History, "Catchup failed and cannot recover");
            mCatchupFatalFailure = true;
        }
        mCatchupWork.reset();
        logAndUpdateCatchupStatus(true);
    }

    uint32_t lastReceivedLedgerSeq = ledgerData.getLedgerSeq();
    mLargestLedgerSeqHeard =
        std::max(mLargestLedgerSeqHeard, lastReceivedLedgerSeq);

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

        addAndTrimSyncingLedgers(ledgerData);
        logAndUpdateCatchupStatus(true);
        return;
    }

    // Next, we buffer every out of sync ledger to allow us to get back in sync
    // in case the ledgers we're missing are received.
    addAndTrimSyncingLedgers(ledgerData);

    // Finally we wait some number of ledgers beyond the smallest buffered
    // checkpoint ledger before we trigger the CatchupWork. This could be any
    // number, for the sake of simplicity at the moment it's set to one ledger
    // after the first buffered one. Since we can receive out of order ledgers,
    // we just check for any ledger larger than the checkpoint

    auto& hm = mApp.getHistoryManager();

    std::string message;
    uint32_t firstLedgerInBuffer = mSyncingLedgers.begin()->first;
    uint32_t lastLedgerInBuffer = mSyncingLedgers.crbegin()->first;
    if (mApp.getConfig().modeDoesCatchupWithBucketList() &&
        hm.isFirstLedgerInCheckpoint(firstLedgerInBuffer) &&
        firstLedgerInBuffer < lastLedgerInBuffer)
    {
        // No point in processing ledgers as catchup won't ever be able to
        // succeed
        if (!mCatchupFatalFailure)
        {
            message = fmt::format(
                FMT_STRING("Starting catchup after ensuring checkpoint "
                           "ledger {:d} was closed on network"),
                lastLedgerInBuffer);
            startOnlineCatchup();
        }
        else
        {
            CLOG_FATAL(History, "Skipping catchup: incompatible core version "
                                "or invalid local state");
        }
    }
    else
    {
        // get the smallest checkpoint we need to start catchup
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
            message = fmt::format(
                FMT_STRING("Waiting for trigger ledger: {:d}/{:d}, ETA: {:d}s"),
                lastLedgerInBuffer, catchupTriggerLedger, eta.count());
        }
        else
        {
            message = fmt::format(
                FMT_STRING(
                    "Waiting for out-of-order ledger(s). Trigger ledger: {:d}"),
                catchupTriggerLedger);
        }
    }
    logAndUpdateCatchupStatus(true, message);
}

void
CatchupManagerImpl::startCatchup(
    CatchupConfiguration configuration, std::shared_ptr<HistoryArchive> archive,
    std::set<std::shared_ptr<Bucket>> bucketsToRetain)
{
    ZoneScoped;
    auto lastClosedLedger = mApp.getLedgerManager().getLastClosedLedgerNum();
    if ((configuration.toLedger() != CatchupConfiguration::CURRENT) &&
        (configuration.toLedger() <= lastClosedLedger))
    {
        throw std::invalid_argument(fmt::format(
            FMT_STRING("Target ledger({:d}) is not newer than LCL({:d})"),
            configuration.toLedger(), lastClosedLedger));
    }

    if (configuration.localBucketsOnly() !=
        mApp.getLedgerManager().rebuildingInMemoryState())
    {
        throw std::invalid_argument(
            "Local catchup is only valid when rebuilding ledger state");
    }

    // Offline and local catchup types aren't triggered by buffered ledgers
    auto offlineCatchup =
        configuration.offline() || configuration.localBucketsOnly();
    releaseAssert(offlineCatchup == mSyncingLedgers.empty());

    releaseAssert(!mCatchupWork);

    // NB: if WorkScheduler is aborting this returns nullptr,
    // which means we don't "really" start catchup.
    mCatchupWork = mApp.getWorkScheduler().scheduleWork<CatchupWork>(
        configuration, bucketsToRetain, archive);
}

std::string
CatchupManagerImpl::getStatus() const
{
    return mCatchupWork ? mCatchupWork->getStatus() : std::string{};
}

BasicWork::State
CatchupManagerImpl::getCatchupWorkState() const
{
    releaseAssert(mCatchupWork);
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
        auto state = fmt::format(FMT_STRING("{}{}"), contiguousString, message);
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

std::optional<LedgerCloseData>
CatchupManagerImpl::maybeGetNextBufferedLedgerToApply()
{
    trimSyncingLedgers();
    if (!mSyncingLedgers.empty() &&
        mSyncingLedgers.begin()->first ==
            mApp.getLedgerManager().getLastClosedLedgerNum() + 1)
    {
        return std::make_optional<LedgerCloseData>(
            mSyncingLedgers.begin()->second);
    }
    else
    {
        return {};
    }
}

std::optional<LedgerCloseData>
CatchupManagerImpl::maybeGetLargestBufferedLedger()
{
    if (!mSyncingLedgers.empty())
    {
        return std::make_optional<LedgerCloseData>(
            mSyncingLedgers.crbegin()->second);
    }
    else
    {
        return std::nullopt;
    }
}

uint32_t
CatchupManagerImpl::getLargestLedgerSeqHeard() const
{
    return mLargestLedgerSeqHeard;
}

void
CatchupManagerImpl::syncMetrics()
{
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}

void
CatchupManagerImpl::addAndTrimSyncingLedgers(LedgerCloseData const& ledgerData)
{
    mSyncingLedgers.emplace(ledgerData.getLedgerSeq(), ledgerData);
    trimSyncingLedgers();

    CLOG_INFO(Ledger,
              "Close of ledger {} buffered. mSyncingLedgers has {} ledgers",
              ledgerData.getLedgerSeq(), mSyncingLedgers.size());
}

void
CatchupManagerImpl::startOnlineCatchup()
{
    releaseAssert(mSyncingLedgers.size() > 1);

    // catchup just before first buffered ledger that way we will have a
    // way to verify history consistency - compare previousLedgerHash of
    // buffered ledger with last one downloaded from history
    auto const& lcd = mSyncingLedgers.begin()->second;
    auto firstBufferedLedgerSeq = lcd.getLedgerSeq();
    auto hash = std::make_optional<Hash>(lcd.getTxSet()->previousLedgerHash());
    startCatchup({LedgerNumHashPair(firstBufferedLedgerSeq - 1, hash),
                  getCatchupCount(), CatchupConfiguration::Mode::ONLINE},
                 nullptr, {});
}

void
CatchupManagerImpl::trimSyncingLedgers()
{

    auto removeLedgersLessThan = [&](uint32_t ledger) {
        // lower_bound returns an iterator pointing to the first element whose
        // key is not considered to go before k. Thus we get the iterator to
        // `ledger` if exists, or the first one after `ledger`.
        auto it = mSyncingLedgers.lower_bound(ledger);
        // This erases [begin, it).
        mSyncingLedgers.erase(mSyncingLedgers.begin(), it);
    };
    removeLedgersLessThan(mApp.getLedgerManager().getLastClosedLedgerNum() + 1);
    auto& hm = mApp.getHistoryManager();
    if (!mSyncingLedgers.empty())
    {
        auto const lastBufferedLedger = mSyncingLedgers.rbegin()->first;
        if (hm.isFirstLedgerInCheckpoint(lastBufferedLedger))
        {
            // The last ledger is the first ledger in the checkpoint.
            // This means that nodes may not have started publishing
            // the checkpoint of lastBufferedLedger.
            // We should only keep lastBufferedLedger _and_ the checkpoint
            // before that.
            removeLedgersLessThan(
                hm.firstLedgerInCheckpointContaining(lastBufferedLedger - 1));
        }
        else
        {
            // The last ledger isn't the first ledger in the checkpoint.
            // This means that nodes must have started publishing
            // the checkpoint of lastBufferedLedger.
            // Therefore, we will delete all ledgers before the checkpoint.
            removeLedgersLessThan(
                hm.firstLedgerInCheckpointContaining(lastBufferedLedger));
        }
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

void
CatchupManagerImpl::historyArchiveStatesDownloaded(uint32_t num)
{
    mMetrics.mHistoryArchiveStatesDownloaded += num;
}

void
CatchupManagerImpl::ledgersVerified(uint32_t num)
{
    mMetrics.mLedgersVerified += num;
}

void
CatchupManagerImpl::ledgerChainsVerificationFailed(uint32_t num)
{
    mMetrics.mLedgerChainsVerificationFailed += num;
}

void
CatchupManagerImpl::bucketsApplied(uint32_t num)
{
    mMetrics.mBucketsApplied += num;
}
void
CatchupManagerImpl::txSetsApplied(uint32_t num)
{
    mMetrics.mTxSetsApplied += num;
}

void
CatchupManagerImpl::fileDownloaded(FileType type, uint32_t num)
{
    if (type == FileType::HISTORY_FILE_TYPE_BUCKET)
    {
        mMetrics.mBucketsDownloaded += num;
    }
    else if (type == FileType::HISTORY_FILE_TYPE_LEDGER)
    {
        mMetrics.mCheckpointsDownloaded += num;
    }
    else if (type == FileType::HISTORY_FILE_TYPE_TRANSACTIONS)
    {
        mMetrics.mTxSetsDownloaded += num;
    }
    else if (type != FileType::HISTORY_FILE_TYPE_RESULTS &&
             type != FileType::HISTORY_FILE_TYPE_SCP)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING(
                "CatchupManagerImpl::fileDownloaded unknown file type {}"),
            typeString(type)));
    }
}

}
