// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/ApplyLedgerChainWork.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/DownloadBucketsWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "test/TestPrinter.h"
#include "util/Logging.h"
#include <lib/util/format.h>

namespace stellar
{

CatchupWork::CatchupWork(Application& app, WorkParent& parent,
                         CatchupConfiguration catchupConfiguration,
                         bool manualCatchup, ProgressHandler progressHandler,
                         size_t maxRetries)
    : BucketDownloadWork(
          app, parent, "catchup",
          app.getHistoryManager().getLastClosedHistoryArchiveState(),
          maxRetries)
    , mCatchupConfiguration{catchupConfiguration}
    , mManualCatchup{manualCatchup}
    , mProgressHandler{progressHandler}
{
}

CatchupWork::~CatchupWork()
{
    clearChildren();
}

std::string
CatchupWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyTransactionsWork)
        {
            return mApplyTransactionsWork->getStatus();
        }
        else if (mDownloadTransactionsWork)
        {
            return mDownloadTransactionsWork->getStatus();
        }
        else if (mApplyBucketsWork)
        {
            return mApplyBucketsWork->getStatus();
        }
        else if (mDownloadBucketsWork)
        {
            return mDownloadBucketsWork->getStatus();
        }
        else if (mGetBucketsHistoryArchiveStateWork)
        {
            return mGetBucketsHistoryArchiveStateWork->getStatus();
        }
        else if (mVerifyLedgersWork)
        {
            return mVerifyLedgersWork->getStatus();
        }
        else if (mDownloadLedgersWork)
        {
            return mDownloadLedgersWork->getStatus();
        }
        else if (mGetHistoryArchiveStateWork)
        {
            return mGetHistoryArchiveStateWork->getStatus();
        }
    }
    return BucketDownloadWork::getStatus();
}

void
CatchupWork::onReset()
{
    auto toLedger = mCatchupConfiguration.toLedger() == 0
                        ? "CURRENT"
                        : std::to_string(mCatchupConfiguration.toLedger());
    CLOG(INFO, "History") << "Starting catchup with configuration:\n"
                          << "  lastClosedLedger: "
                          << mApp.getLedgerManager().getLastClosedLedgerNum()
                          << "\n"
                          << "  toLedger: " << toLedger << "\n"
                          << "  count: " << mCatchupConfiguration.count();

    auto toCheckpoint =
        mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT
            ? CatchupConfiguration::CURRENT
            : mApp.getHistoryManager().nextCheckpointLedger(
                  mCatchupConfiguration.toLedger() + 1) -
                  1;
    CLOG(INFO, "History")
        << "Catchup downloading history archive state at checkpoint "
        << toCheckpoint;

    clearChildren();
    mBucketsAppliedEmitted = false;
    BucketDownloadWork::onReset();
    mGetHistoryArchiveStateWork.reset();
    mDownloadLedgersWork.reset();
    mVerifyLedgersWork.reset();
    mGetBucketsHistoryArchiveStateWork.reset();
    mDownloadBucketsWork.reset();
    mApplyBucketsWork.reset();
    mDownloadTransactionsWork.reset();
    mApplyTransactionsWork.reset();

    uint64_t sleepSeconds =
        mManualCatchup || (toCheckpoint == CatchupConfiguration::CURRENT)
            ? 0
            : mApp.getHistoryManager().nextCheckpointCatchupProbe(toCheckpoint);

    mLastClosedLedgerAtReset = mApp.getLedgerManager().getLastClosedLedgerNum();
    mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
        "get-history-archive-state", mRemoteState, toCheckpoint,
        std::chrono::seconds(sleepSeconds));
}

bool
CatchupWork::hasAnyLedgersToCatchupTo() const
{
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    if (mLastClosedLedgerAtReset <= mRemoteState.currentLedger)
    {
        return true;
    }

    CLOG(INFO, "History")
        << "Last closed ledger is later than current checkpoint: "
        << mLastClosedLedgerAtReset << " > " << mRemoteState.currentLedger;
    CLOG(INFO, "History") << "Wait until next checkpoint before retrying ";
    CLOG(ERROR, "History") << "Nothing to catchup to ";
    return false;
}

bool
CatchupWork::downloadLedgers(CheckpointRange const& range)
{
    if (mDownloadLedgersWork)
    {
        assert(mDownloadLedgersWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History")
        << "Catchup downloading ledger chain for checkpointRange ["
        << range.first() << ".." << range.last() << "]";
    mDownloadLedgersWork = addWork<BatchDownloadWork>(
        range, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);

    return true;
}

bool
CatchupWork::verifyLedgers(LedgerRange const& range)
{
    if (mVerifyLedgersWork)
    {
        assert(mVerifyLedgersWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History")
        << "Catchup verifying ledger chain for checkpointRange ["
        << range.first() << ".." << range.last() << "]";
    mVerifyLedgersWork = addWork<VerifyLedgerChainWork>(
        *mDownloadDir, range, mManualCatchup, mFirstVerified, mLastVerified);

    return true;
}

bool
CatchupWork::alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const
{
    return atCheckpoint == mRemoteState.currentLedger;
}

bool
CatchupWork::downloadBucketsHistoryArchiveState(uint32_t atCheckpoint)
{
    if (mGetBucketsHistoryArchiveStateWork)
    {
        assert(mGetBucketsHistoryArchiveStateWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History") << "Catchup downloading history archive "
                             "state for applying buckets at checkpoint "
                          << atCheckpoint;

    mGetBucketsHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
        "get-buckets-history-archive-state", mApplyBucketsRemoteState,
        atCheckpoint, std::chrono::seconds(0));

    return true;
}

bool
CatchupWork::downloadBuckets()
{
    if (mDownloadBucketsWork)
    {
        assert(mDownloadBucketsWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History") << "Catchup downloading and verifying buckets";
    std::vector<std::string> hashes =
        mApplyBucketsRemoteState.differingBuckets(mLocalState);
    mDownloadBucketsWork =
        addWork<DownloadBucketsWork>(mBuckets, hashes, *mDownloadDir);
    return true;
}

bool
CatchupWork::applyBuckets()
{
    if (mApplyBucketsWork)
    {
        assert(mApplyBucketsWork->getState() == WORK_SUCCESS);
        return false;
    }

    // Consistency check: mRemoteState and mFirstVerified should point to
    // the same ledger and the same BucketList.
    assert(mApplyBucketsRemoteState.currentLedger ==
           mFirstVerified.header.ledgerSeq);
    assert(mApplyBucketsRemoteState.getBucketListHash() ==
           mFirstVerified.header.bucketListHash);

    // Consistency check: LCL should be in the _past_ from firstVerified,
    // since we're about to clobber a bunch of DB state with new buckets
    // held in firstVerified's state.
    auto lcl = app().getLedgerManager().getLastClosedLedgerHeader();
    if (mFirstVerified.header.ledgerSeq < lcl.header.ledgerSeq)
    {
        throw std::runtime_error(
            fmt::format("Catchup MINIMAL applying ledger earlier than local "
                        "LCL: {:s} < {:s}",
                        LedgerManager::ledgerAbbrev(mFirstVerified),
                        LedgerManager::ledgerAbbrev(lcl)));
    }

    CLOG(INFO, "History") << "Catchup applying buckets for state "
                          << LedgerManager::ledgerAbbrev(mFirstVerified);
    mApplyBucketsWork =
        addWork<ApplyBucketsWork>(mBuckets, mApplyBucketsRemoteState);

    return true;
}

bool
CatchupWork::downloadTransactions(CheckpointRange const& range)
{
    if (mDownloadTransactionsWork)
    {
        assert(mDownloadTransactionsWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History") << "Catchup downloading transactions for range ["
                          << range.first() << ".." << range.last() << "]";

    mDownloadTransactionsWork = addWork<BatchDownloadWork>(
        range, HISTORY_FILE_TYPE_TRANSACTIONS, *mDownloadDir);

    return true;
}

bool
CatchupWork::applyTransactions(LedgerRange const& range)
{
    if (mApplyTransactionsWork)
    {
        assert(mApplyTransactionsWork->getState() == WORK_SUCCESS);
        return false;
    }

    CLOG(INFO, "History") << "Catchup applying transactions for range ["
                          << range.first() << ".." << range.last() << "]";

    mApplyTransactionsWork =
        addWork<ApplyLedgerChainWork>(*mDownloadDir, range, mLastApplied);

    return true;
}

Work::State
CatchupWork::onSuccess()
{
    if (!hasAnyLedgersToCatchupTo())
    {
        mApp.getCatchupManager().historyCaughtup();
        asio::error_code ec = std::make_error_code(std::errc::invalid_argument);
        mProgressHandler(ec, ProgressState::FINISHED,
                         LedgerHeaderHistoryEntry{});
        return WORK_SUCCESS;
    }

    auto resolvedConfiguration =
        mCatchupConfiguration.resolve(mRemoteState.currentLedger);
    auto catchupRange =
        makeCatchupRange(mLastClosedLedgerAtReset, resolvedConfiguration,
                         mApp.getHistoryManager());
    auto ledgerRange = catchupRange.first;
    auto checkpointRange =
        CheckpointRange{ledgerRange, mApp.getHistoryManager()};

    if (downloadLedgers(checkpointRange))
    {
        return WORK_PENDING;
    }

    if (verifyLedgers(ledgerRange))
    {
        return WORK_PENDING;
    }

    if (catchupRange.second)
    {
        if (!alreadyHaveBucketsHistoryArchiveState(checkpointRange.first()))
        {
            if (downloadBucketsHistoryArchiveState(checkpointRange.first()))
            {
                return WORK_PENDING;
            }
        }
        else
        {
            mApplyBucketsRemoteState = mRemoteState;
        }

        if (downloadBuckets())
        {
            return WORK_PENDING;
        }

        if (applyBuckets())
        {
            return WORK_PENDING;
        }

        if (!mBucketsAppliedEmitted)
        {
            mProgressHandler({}, ProgressState::APPLIED_BUCKETS,
                             mFirstVerified);
            mBucketsAppliedEmitted = true;
        }
    }
    else
    {
        CLOG(INFO, "History") << "Catchup downloading history archive "
                                 "state for applying buckets at checkpoint "
                              << checkpointRange.first() << " not needed";
    }

    if (downloadTransactions(checkpointRange))
    {
        return WORK_PENDING;
    }

    if (applyTransactions(ledgerRange))
    {
        return WORK_PENDING;
    }

    mProgressHandler({}, ProgressState::APPLIED_TRANSACTIONS, mLastApplied);
    mProgressHandler({}, ProgressState::FINISHED, mLastApplied);
    mApp.getCatchupManager().historyCaughtup();
    return WORK_SUCCESS;
}

void
CatchupWork::onFailureRaise()
{
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mProgressHandler(ec, ProgressState::FINISHED, LedgerHeaderHistoryEntry{});
}

namespace
{

// compute first checkpoint that is not 100% finished
// if lastClosedLedger is not last ledger in checkpoint, then first not finished
// checkpoint is checkpoint containing lastClosedLedger
// if lastClosedLedger is last ledger in checkpoint, then first not finished
// checkpoint is next checkpoint (with 0 ledgers applied)
uint32_t
firstNotFinishedCheckpoint(uint32_t lastClosedLedger,
                           HistoryManager const& historyManager)
{
    auto result = historyManager.checkpointContainingLedger(lastClosedLedger);
    if (lastClosedLedger < result)
    {
        return result;
    }
    else
    {
        return result + historyManager.getCheckpointFrequency();
    }
}

// return first ledger that should be applied so at least count history entries
// are stored in database (count 0 is changed to 1, because even doing only
// bucket apply gives one entry)
//
// if it is impossible, returns smallest possible ledger number -
// LedgerManager::GENESIS_LEDGER_SEQ
uint32_t
firstNeededLedger(CatchupConfiguration const& configuration)
{
    auto neededCount = std::max(configuration.count(), 1u);
    return configuration.toLedger() > neededCount
               ? configuration.toLedger() - neededCount + 1
               : LedgerManager::GENESIS_LEDGER_SEQ;
}

std::pair<bool, uint32_t>
computeCatchupStart(uint32_t smallestLedgerToApply,
                    uint32_t smallestBucketApplyCheckpoint,
                    HistoryManager const& historyManager)
{
    // checkpoint that contains smallestLedgerToApply - it is first one than
    // can be applied, it is always greater than LCL
    auto smallestCheckpointToApply =
        historyManager.checkpointContainingLedger(smallestLedgerToApply);

    // if first ledger that should be applied is on checkpoint boundary then
    // we do an bucket-apply
    if (smallestLedgerToApply == smallestCheckpointToApply)
    {
        return std::make_pair(true, smallestCheckpointToApply);
    }

    // we are before first checkpoint - applying buckets is not possible, so
    // just apply transactions
    if (smallestLedgerToApply < historyManager.getCheckpointFrequency() - 1)
    {
        return std::make_pair(false, smallestLedgerToApply);
    }

    // we need to apply on previous checkpoint (if possible), so we are sure
    // that we get required number of history entries in database
    smallestCheckpointToApply -= historyManager.getCheckpointFrequency();
    if (smallestCheckpointToApply >= smallestBucketApplyCheckpoint)
    {
        return std::make_pair(true, smallestCheckpointToApply);
    }
    else
    {
        // in that case we would apply before LCL, so there is no need to
        // apply buckets
        return std::make_pair(false, smallestLedgerToApply);
    }
}
}

CatchupRange
CatchupWork::makeCatchupRange(uint32_t lastClosedLedger,
                              CatchupConfiguration const& configuration,
                              HistoryManager const& historyManager)
{
    assert(lastClosedLedger > 0);
    assert(configuration.toLedger() >= lastClosedLedger);
    assert(configuration.toLedger() != CatchupConfiguration::CURRENT);

    // maximum ledger number that we should do "transaction apply" on in order
    // to replay enough of transaction history
    auto smallestLedgerToApply =
        std::max(firstNeededLedger(configuration), lastClosedLedger);

    // smallest checkpoint value that can be bucket-applied on local ledger -
    // all lower checkpoints are already fully applied
    auto smallestBucketApplyCheckpoint =
        firstNotFinishedCheckpoint(lastClosedLedger, historyManager);

    // check if catchup should start with bucket apply or not and if so,
    // which checkpoint should it start at
    auto catchupStart = computeCatchupStart(
        smallestLedgerToApply, smallestBucketApplyCheckpoint, historyManager);

    // if we are about to apply buckets just after LCL, we can as well apply
    // transactions
    if (catchupStart.first && catchupStart.second <= lastClosedLedger + 1)
    {
        return {{catchupStart.second, configuration.toLedger()}, false};
    }

    return {{catchupStart.second, configuration.toLedger()},
            catchupStart.first};
}
}
