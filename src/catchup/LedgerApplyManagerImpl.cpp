// Copyright 2014-2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"
#include "catchup/LedgerApplyManagerImpl.h"
#include "catchup/CatchupConfiguration.h"
#include "herder/Herder.h"
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

const uint32_t LedgerApplyManagerImpl::MAX_EXTERNALIZE_LEDGER_APPLY_DRIFT = 12;

LedgerApplyManagerImpl::CatchupMetrics::CatchupMetrics()
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

LedgerApplyManagerImpl::CatchupMetrics::CatchupMetrics(
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

LedgerApplyManagerImpl::CatchupMetrics
operator-(LedgerApplyManager::CatchupMetrics const& x,
          LedgerApplyManager::CatchupMetrics const& y)
{
    return LedgerApplyManager::CatchupMetrics{
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
                            return HistoryManager::isFirstLedgerInCheckpoint(
                                kvp.first, hm.getConfig());
                        });
}

std::unique_ptr<LedgerApplyManager>
LedgerApplyManager::create(Application& app)
{
    return std::make_unique<LedgerApplyManagerImpl>(app);
}

LedgerApplyManagerImpl::LedgerApplyManagerImpl(Application& app)
    : mApp(app)
    , mCatchupWork(nullptr)
    , mSyncingLedgersSize(
          app.getMetrics().NewCounter({"ledger", "memory", "queued-ledgers"}))
    , mLargestLedgerSeqHeard(0)
{
    releaseAssert(threadIsMain());
}

LedgerApplyManagerImpl::~LedgerApplyManagerImpl()
{
}

uint32_t
LedgerApplyManagerImpl::getCatchupCount()
{
    releaseAssert(threadIsMain());
    return mApp.getConfig().CATCHUP_COMPLETE
               ? std::numeric_limits<uint32_t>::max()
               : mApp.getConfig().CATCHUP_RECENT;
}

LedgerApplyManager::ProcessLedgerResult
LedgerApplyManagerImpl::processLedger(LedgerCloseData const& ledgerData,
                                      bool isLatestSlot)
{
    releaseAssert(threadIsMain());
    updateLastQueuedToApply();

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

    // Always skip old ledgers
    uint32_t lastReceivedLedgerSeq = ledgerData.getLedgerSeq();
    if (lastReceivedLedgerSeq <= *mLastQueuedToApply)
    {
        // If last queued to apply is already at-or-ahead of the ledger we just
        // received from the network, we're up to date. Return early, nothing to
        // do.
        CLOG_INFO(
            Ledger,
            "Skipping close ledger: local state is {}, more recent than {}",
            *mLastQueuedToApply, ledgerData.getLedgerSeq());
        return ProcessLedgerResult::PROCESSED_ALL_LEDGERS_SEQUENTIALLY;
    }

    // Always add a newer ledger, maybe apply
    mSyncingLedgers.emplace(lastReceivedLedgerSeq, ledgerData);
    mLargestLedgerSeqHeard =
        std::max(mLargestLedgerSeqHeard, lastReceivedLedgerSeq);

    // 1. CatchupWork is not running yet
    // 2. LedgerApplyManager received  ledger that should be immediately applied
    // by LedgerManager: check if we have any sequential ledgers. If so, attempt
    // to apply mSyncingLedgers and possibly get back in sync
    if (!mCatchupWork && lastReceivedLedgerSeq == *mLastQueuedToApply + 1)
    {
        tryApplySyncingLedgers();
        return ProcessLedgerResult::PROCESSED_ALL_LEDGERS_SEQUENTIALLY;
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
    CLOG_INFO(Ledger,
              "Close of ledger {} buffered. mSyncingLedgers has {} ledgers",
              ledgerData.getLedgerSeq(), mSyncingLedgers.size());

    // First: if CatchupWork has started, just buffer and return early.
    if (mCatchupWork)
    {
        // we don't want to buffer any ledgers at or below where we are catching
        // up to
        auto const& config = mCatchupWork->getCatchupConfiguration();
        if (ledgerData.getLedgerSeq() <= config.toLedger())
        {
            return ProcessLedgerResult::WAIT_TO_APPLY_BUFFERED_OR_CATCHUP;
        }

        trimSyncingLedgers();
        logAndUpdateCatchupStatus(true);
        return ProcessLedgerResult::WAIT_TO_APPLY_BUFFERED_OR_CATCHUP;
    }

    // Next, we buffer every out of sync ledger to allow us to get back in sync
    // in case the ledgers we're missing are received.
    trimSyncingLedgers();

    // Finally we wait some number of ledgers beyond the smallest buffered
    // checkpoint ledger before we trigger the CatchupWork. This could be any
    // number, for the sake of simplicity at the moment it's set to one ledger
    // after the first buffered one. Since we can receive out of order ledgers,
    // we just check for any ledger larger than the checkpoint

    std::string message;
    uint32_t firstLedgerInBuffer = mSyncingLedgers.begin()->first;
    uint32_t lastLedgerInBuffer = mSyncingLedgers.crbegin()->first;
    if (mApp.getConfig().modeDoesCatchupWithBucketList() &&
        HistoryManager::isFirstLedgerInCheckpoint(firstLedgerInBuffer,
                                                  mApp.getConfig()) &&
        firstLedgerInBuffer < lastLedgerInBuffer &&
        !mApp.getLedgerManager().isApplying())
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
            HistoryManager::isFirstLedgerInCheckpoint(firstLedgerInBuffer,
                                                      mApp.getConfig())
                ? firstLedgerInBuffer
                : HistoryManager::firstLedgerAfterCheckpointContaining(
                      firstLedgerInBuffer, mApp.getConfig());

        uint32_t catchupTriggerLedger = HistoryManager::ledgerToTriggerCatchup(
            requiredFirstLedgerInCheckpoint, mApp.getConfig());

        if (mApp.getLedgerManager().isApplying())
        {
            message =
                fmt::format(FMT_STRING("Waiting for ledger {:d} application to "
                                       "complete before starting catchup"),
                            getMaxQueuedToApply());
        }
        // If the trigger ledger is behind the last ledger, that means we're
        // waiting for out of order ledgers, which should arrive quickly
        else if (catchupTriggerLedger > lastLedgerInBuffer)
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
    return ProcessLedgerResult::WAIT_TO_APPLY_BUFFERED_OR_CATCHUP;
}

void
LedgerApplyManagerImpl::startCatchup(
    CatchupConfiguration configuration, std::shared_ptr<HistoryArchive> archive,
    std::set<std::shared_ptr<LiveBucket>> bucketsToRetain)
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    updateLastQueuedToApply();

    auto lastClosedLedger = *mLastQueuedToApply;
    if ((configuration.toLedger() != CatchupConfiguration::CURRENT) &&
        (configuration.toLedger() <= lastClosedLedger))
    {
        throw std::invalid_argument(fmt::format(
            FMT_STRING("Target ledger({:d}) is not newer than LCL({:d})"),
            configuration.toLedger(), lastClosedLedger));
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
LedgerApplyManagerImpl::getStatus() const
{
    releaseAssert(threadIsMain());
    return mCatchupWork ? mCatchupWork->getStatus() : std::string{};
}

BasicWork::State
LedgerApplyManagerImpl::getCatchupWorkState() const
{
    releaseAssert(threadIsMain());
    releaseAssert(mCatchupWork);
    return mCatchupWork->getState();
}

bool
LedgerApplyManagerImpl::catchupWorkIsDone() const
{
    releaseAssert(threadIsMain());
    return mCatchupWork && mCatchupWork->isDone();
}

bool
LedgerApplyManagerImpl::isCatchupInitialized() const
{
    releaseAssert(threadIsMain());
    return mCatchupWork != nullptr;
}

void
LedgerApplyManagerImpl::logAndUpdateCatchupStatus(bool contiguous,
                                                  std::string const& message)
{
    releaseAssert(threadIsMain());
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
LedgerApplyManagerImpl::logAndUpdateCatchupStatus(bool contiguous)
{
    releaseAssert(threadIsMain());
    logAndUpdateCatchupStatus(contiguous, getStatus());
}

std::optional<LedgerCloseData>
LedgerApplyManagerImpl::maybeGetNextBufferedLedgerToApply()
{
    releaseAssert(threadIsMain());
    // Since we just applied a ledger, refresh mLastQueuedToApply
    updateLastQueuedToApply();

    trimSyncingLedgers();
    if (!mSyncingLedgers.empty() &&
        mSyncingLedgers.begin()->first == *mLastQueuedToApply + 1)
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
LedgerApplyManagerImpl::maybeGetLargestBufferedLedger()
{
    releaseAssert(threadIsMain());
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
LedgerApplyManagerImpl::getLargestLedgerSeqHeard() const
{
    releaseAssert(threadIsMain());
    return mLargestLedgerSeqHeard;
}

uint32_t
LedgerApplyManagerImpl::getMaxQueuedToApply()
{
    releaseAssert(threadIsMain());
    updateLastQueuedToApply();
    return *mLastQueuedToApply;
}

void
LedgerApplyManagerImpl::syncMetrics()
{
    releaseAssert(threadIsMain());
    mSyncingLedgersSize.set_count(mSyncingLedgers.size());
}

void
LedgerApplyManagerImpl::updateLastQueuedToApply()
{
    releaseAssert(threadIsMain());
    if (!mLastQueuedToApply)
    {
        mLastQueuedToApply = mApp.getLedgerManager().getLastClosedLedgerNum();
    }
    else
    {
        mLastQueuedToApply =
            std::max(*mLastQueuedToApply,
                     mApp.getLedgerManager().getLastClosedLedgerNum());
    }
}

void
LedgerApplyManagerImpl::startOnlineCatchup()
{
    releaseAssert(threadIsMain());
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
LedgerApplyManagerImpl::trimSyncingLedgers()
{
    releaseAssert(threadIsMain());
    auto removeLedgersLessThan = [&](uint32_t ledger) {
        // lower_bound returns an iterator pointing to the first element whose
        // key is not considered to go before k. Thus we get the iterator to
        // `ledger` if exists, or the first one after `ledger`.
        auto it = mSyncingLedgers.lower_bound(ledger);
        // This erases [begin, it).
        mSyncingLedgers.erase(mSyncingLedgers.begin(), it);
    };
    removeLedgersLessThan(*mLastQueuedToApply + 1);
    if (!mSyncingLedgers.empty())
    {
        auto const lastBufferedLedger = mSyncingLedgers.rbegin()->first;
        if (HistoryManager::isFirstLedgerInCheckpoint(lastBufferedLedger,
                                                      mApp.getConfig()))
        {
            // The last ledger is the first ledger in the checkpoint.
            // This means that nodes may not have started publishing
            // the checkpoint of lastBufferedLedger.
            // We should only keep lastBufferedLedger _and_ the checkpoint
            // before that.
            removeLedgersLessThan(
                HistoryManager::firstLedgerInCheckpointContaining(
                    lastBufferedLedger - 1, mApp.getConfig()));
        }
        else
        {
            // The last ledger isn't the first ledger in the checkpoint.
            // This means that nodes must have started publishing
            // the checkpoint of lastBufferedLedger.
            // Therefore, we will delete all ledgers before the checkpoint.
            removeLedgersLessThan(
                HistoryManager::firstLedgerInCheckpointContaining(
                    lastBufferedLedger, mApp.getConfig()));
        }
    }
}

void
LedgerApplyManagerImpl::tryApplySyncingLedgers()
{
    ZoneScoped;
    releaseAssert(threadIsMain());
    uint32_t nextToClose = *mLastQueuedToApply + 1;
    auto lcl = mApp.getLedgerManager().getLastClosedLedgerNum();

    // We can apply multiple ledgers here, which might be slow. This is a rare
    // occurrence so we should be fine.
    auto it = mSyncingLedgers.cbegin();
    while (it != mSyncingLedgers.cend())
    {
        auto const& lcd = it->second;

        // we still have a missing ledger
        if (nextToClose != lcd.getLedgerSeq())
        {
            break;
        }

        // If we have too many ledgers queued to apply, just stop scheduling
        // more and let the node gracefully go into catchup.
        releaseAssert(mLastQueuedToApply >= lcl);
        if (nextToClose - lcl >= MAX_EXTERNALIZE_LEDGER_APPLY_DRIFT)
        {
            CLOG_INFO(History,
                      "Next ledger to apply is {}, but LCL {} is too far "
                      "behind, waiting",
                      nextToClose, lcl);
            break;
        }

        if (mApp.getConfig().parallelLedgerClose())
        {
            // Notify LM that application has started
            mApp.getLedgerManager().beginApply();
            mApp.postOnLedgerCloseThread(
                [&app = mApp, lcd]() {
                    // No-op if app is shutting down
                    if (app.isStopping())
                    {
                        return;
                    }
                    app.getLedgerManager().closeLedger(lcd,
                                                       /* externalize */ true);
                },
                "closeLedger queue");
        }
        else
        {
            mApp.getLedgerManager().closeLedger(lcd, /* externalize */ true);
        }
        mLastQueuedToApply = lcd.getLedgerSeq();

        ++it;
        ++nextToClose;
    }

    mSyncingLedgers.erase(mSyncingLedgers.cbegin(), it);
}

void
LedgerApplyManagerImpl::historyArchiveStatesDownloaded(uint32_t num)
{
    releaseAssert(threadIsMain());
    mMetrics.mHistoryArchiveStatesDownloaded += num;
}

void
LedgerApplyManagerImpl::ledgersVerified(uint32_t num)
{
    releaseAssert(threadIsMain());
    mMetrics.mLedgersVerified += num;
}

void
LedgerApplyManagerImpl::ledgerChainsVerificationFailed(uint32_t num)
{
    releaseAssert(threadIsMain());
    mMetrics.mLedgerChainsVerificationFailed += num;
}

void
LedgerApplyManagerImpl::bucketsApplied(uint32_t num)
{
    releaseAssert(threadIsMain());
    mMetrics.mBucketsApplied += num;
}
void
LedgerApplyManagerImpl::txSetsApplied(uint32_t num)
{
    releaseAssert(threadIsMain());
    mMetrics.mTxSetsApplied += num;
}

void
LedgerApplyManagerImpl::fileDownloaded(FileType type, uint32_t num)
{
    releaseAssert(threadIsMain());
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
                "LedgerApplyManagerImpl::fileDownloaded unknown file type {}"),
            typeString(type)));
    }
}

}
