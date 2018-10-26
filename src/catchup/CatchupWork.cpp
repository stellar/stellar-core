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

CatchupWork::CatchupWork(Application& app,
                         CatchupConfiguration catchupConfiguration,
                         bool manualCatchup, ProgressHandler progressHandler,
                         size_t maxRetries)
    : Work(app, "catchup", maxRetries)
    , mLocalState{app.getHistoryManager().getLastClosedHistoryArchiveState()}
    , mDownloadDir{std::make_unique<TmpDir>(
          mApp.getTmpDirManager().tmpDir(getName()))}
    , mCatchupConfiguration{catchupConfiguration}
    , mManualCatchup{manualCatchup}
    , mProgressHandler{progressHandler}
{
}

CatchupWork::~CatchupWork()
{
}

void
CatchupWork::doReset()
{
    mBucketsAppliedEmitted = false;
    mBucketsProcessing = false;
    mBuckets.clear();
    mDownloadVerifyLedgersSeq.reset();
    mBucketVerifyApplySeq.reset();
    mTransactionsVerifyApplySeq.reset();
    mGetHistoryArchiveStateWork.reset();
    mGetBucketStateWork.reset();
    mLastClosedLedgerAtReset = mApp.getLedgerManager().getLastClosedLedgerNum();
    mRemoteState = {};
    mApplyBucketsRemoteState = {};
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
}

std::string
CatchupWork::getStatus() const
{
    if (mTransactionsVerifyApplySeq)
    {
        return mTransactionsVerifyApplySeq->getStatus();
    }
    else if (mBucketVerifyApplySeq)
    {
        return mBucketVerifyApplySeq->getStatus();
    }
    else if (mDownloadVerifyLedgersSeq)
    {
        return mDownloadVerifyLedgersSeq->getStatus();
    }

    return Work::getStatus();
}

bool
CatchupWork::hasAnyLedgersToCatchupTo() const
{
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == State::WORK_SUCCESS);

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
CatchupWork::alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const
{
    return atCheckpoint == mRemoteState.currentLedger;
}

BasicWork::State
CatchupWork::doWork()
{
    // Step 1: Get history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        auto toCheckpoint =
            mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT
                ? CatchupConfiguration::CURRENT
                : mApp.getHistoryManager().nextCheckpointLedger(
                      mCatchupConfiguration.toLedger() + 1) -
                      1;
        CLOG(INFO, "History")
            << "Catchup downloading history archive state at checkpoint "
            << toCheckpoint;
        mGetHistoryArchiveStateWork =
            addWork<GetHistoryArchiveStateWork>(mRemoteState, toCheckpoint);
        return State::WORK_RUNNING;
    }
    else if (mGetHistoryArchiveStateWork->getState() != State::WORK_SUCCESS)
    {
        return mGetHistoryArchiveStateWork->getState();
    }

    // Step 2: Compare local and remote states
    if (!hasAnyLedgersToCatchupTo())
    {
        mApp.getCatchupManager().historyCaughtup();
        asio::error_code ec = std::make_error_code(std::errc::invalid_argument);
        mProgressHandler(ec, ProgressState::FINISHED,
                         LedgerHeaderHistoryEntry{});
        return State::WORK_SUCCESS;
    }

    auto resolvedConfiguration =
        mCatchupConfiguration.resolve(mRemoteState.currentLedger);
    auto catchupRange =
        makeCatchupRange(mLastClosedLedgerAtReset, resolvedConfiguration,
                         mApp.getHistoryManager());

    // Step 3: If needed, download archive state for buckets
    if (catchupRange.second)
    {
        auto checkpointRange =
            CheckpointRange{catchupRange.first, mApp.getHistoryManager()};
        if (!alreadyHaveBucketsHistoryArchiveState(checkpointRange.first()))
        {
            if (!mGetBucketStateWork)
            {
                mGetBucketStateWork = addWork<GetHistoryArchiveStateWork>(
                    mApplyBucketsRemoteState, checkpointRange.first());
            }
            if (mGetBucketStateWork->getState() != State::WORK_SUCCESS)
            {
                return mGetBucketStateWork->getState();
            }
        }
        else
        {
            mApplyBucketsRemoteState = mRemoteState;
        }
    }

    // Step 4: Perform catchup in 3 phases
    if (mTransactionsVerifyApplySeq)
    {
        auto state = mTransactionsVerifyApplySeq->getState();
        if (state == State::WORK_SUCCESS)
        {
            mProgressHandler({}, ProgressState::APPLIED_TRANSACTIONS,
                             mLastApplied);
            mProgressHandler({}, ProgressState::FINISHED, mLastApplied);
            mApp.getCatchupManager().historyCaughtup();
        }
        return state;
    }

    if (mBucketsProcessing)
    {
        if (bucketProcessingState(catchupRange) == State::WORK_SUCCESS)
        {
            if (catchupRange.second && !mBucketsAppliedEmitted)
            {
                mProgressHandler({}, ProgressState::APPLIED_BUCKETS,
                                 mFirstVerified);
                mBucketsAppliedEmitted = true;
            }
        }
        else
        {
            return bucketProcessingState(catchupRange);
        }

        // Phase 3: Download and Apply transactions
        downloadApplyTransactions(catchupRange);
        return State::WORK_RUNNING;
    }

    if (mDownloadVerifyLedgersSeq)
    {
        auto state = mDownloadVerifyLedgersSeq->getState();

        // Phase 2: Download, verify and apply buckets if needed
        if (state == State::WORK_SUCCESS)
        {
            assert(!mBucketsProcessing);
            if (catchupRange.second)
            {
                downloadApplyBuckets(catchupRange);
            }

            mBucketsProcessing = true;
            return State::WORK_RUNNING;
        }
        return state;
    }

    auto toLedger = mCatchupConfiguration.toLedger() == 0
                        ? "CURRENT"
                        : std::to_string(mCatchupConfiguration.toLedger());
    CLOG(INFO, "History") << "Starting catchup with configuration:\n"
                          << "  lastClosedLedger: "
                          << mApp.getLedgerManager().getLastClosedLedgerNum()
                          << "\n"
                          << "  toLedger: " << toLedger << "\n"
                          << "  count: " << mCatchupConfiguration.count();

    // Phase 1: Download and verify ledger chain
    downloadVerifyLedgerChain(catchupRange);
    return State::WORK_RUNNING;
}

void
CatchupWork::downloadVerifyLedgerChain(CatchupRange catchupRange)
{
    assert(!mBucketsProcessing);
    assert(!mDownloadVerifyLedgersSeq);
    assert(!mTransactionsVerifyApplySeq);

    auto ledgerRange = catchupRange.first;
    auto checkpointRange =
        CheckpointRange{ledgerRange, mApp.getHistoryManager()};
    CLOG(INFO, "History")
        << "Catchup downloading ledger chain for checkpointRange ["
        << checkpointRange.first() << ".." << checkpointRange.last() << "]";
    auto getLedgers = std::make_shared<BatchDownloadWork>(
        mApp, checkpointRange, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);

    CLOG(INFO, "History")
        << "Catchup verifying ledger chain for checkpointRange ["
        << checkpointRange.first() << ".." << checkpointRange.last() << "]";
    auto verifyLedgers = std::make_shared<VerifyLedgerChainWork>(
        mApp, *mDownloadDir, ledgerRange, mManualCatchup, mFirstVerified,
        mLastVerified);

    std::vector<std::shared_ptr<BasicWork>> seq{getLedgers, verifyLedgers};
    mDownloadVerifyLedgersSeq =
        addWork<WorkSequence>("download-verify-ledgers-headers-seq", seq);
}

void
CatchupWork::downloadApplyBuckets(CatchupRange catchupRange)
{
    assert(mDownloadVerifyLedgersSeq);
    assert(mDownloadVerifyLedgersSeq->getState() == State::WORK_SUCCESS);

    auto range = catchupRange.first;
    CLOG(INFO, "History") << "Catchup downloading and verifying buckets";

    std::vector<std::string> hashes =
        mApplyBucketsRemoteState.differingBuckets(mLocalState);
    auto getBuckets = std::make_shared<DownloadBucketsWork>(
        mApp, mBuckets, hashes, *mDownloadDir);

    // Consistency check: mRemoteState and mFirstVerified should
    // point to the same ledger and the same BucketList.
    assert(mApplyBucketsRemoteState.currentLedger ==
           mFirstVerified.header.ledgerSeq);
    assert(mApplyBucketsRemoteState.getBucketListHash() ==
           mFirstVerified.header.bucketListHash);

    // Consistency check: LCL should be in the _past_ from
    // firstVerified, since we're about to clobber a bunch of DB
    // state with new buckets held in firstVerified's state.
    auto lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
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
    auto applyBuckets = std::make_shared<ApplyBucketsWork>(
        mApp, mBuckets, mApplyBucketsRemoteState);

    std::vector<std::shared_ptr<BasicWork>> seq{getBuckets, applyBuckets};
    mBucketVerifyApplySeq = addWork<WorkSequence>(
        "download-verify-apply-buckets", seq, RETRY_NEVER);
}

void
CatchupWork::downloadApplyTransactions(CatchupRange catchupRange)
{
    assert(!mTransactionsVerifyApplySeq);
    assert(mDownloadVerifyLedgersSeq);
    assert(mDownloadVerifyLedgersSeq->getState() == State::WORK_SUCCESS);
    assert(bucketProcessingState(catchupRange) == State::WORK_SUCCESS);

    auto range = catchupRange.first;
    auto checkpointRange = CheckpointRange{range, mApp.getHistoryManager()};

    CLOG(INFO, "History") << "Catchup downloading transactions for range ["
                          << checkpointRange.first() << ".."
                          << checkpointRange.last() << "]";

    auto getTxs = std::make_shared<BatchDownloadWork>(
        mApp, checkpointRange, HISTORY_FILE_TYPE_TRANSACTIONS, *mDownloadDir);

    CLOG(INFO, "History") << "Catchup applying transactions for range ["
                          << range.first() << ".." << range.last() << "]";
    auto applyLedgers = std::make_shared<ApplyLedgerChainWork>(
        mApp, *mDownloadDir, range, mLastApplied);

    std::vector<std::shared_ptr<BasicWork>> seq{getTxs, applyLedgers};
    mTransactionsVerifyApplySeq =
        addWork<WorkSequence>("download-apply-transactions", seq, RETRY_NEVER);
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

BasicWork::State
CatchupWork::bucketProcessingState(CatchupRange catchupRange) const
{
    assert(mBucketsProcessing);
    if (catchupRange.second)
    {
        if (mBucketVerifyApplySeq)
        {
            return mBucketVerifyApplySeq->getState();
        }
    }
    return State::WORK_SUCCESS;
}
}
