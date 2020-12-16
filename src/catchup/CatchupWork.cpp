// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "bucket/BucketList.h"
#include "catchup/ApplyBucketsWork.h"
#include "catchup/ApplyBufferedLedgersWork.h"
#include "catchup/ApplyCheckpointWork.h"
#include "catchup/CatchupConfiguration.h"
#include "catchup/CatchupRange.h"
#include "catchup/DownloadApplyTxsWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/DownloadBucketsWork.h"
#include "historywork/DownloadVerifyTxResultsWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

uint32_t const CatchupWork::PUBLISH_QUEUE_UNBLOCK_APPLICATION = 8;
uint32_t const CatchupWork::PUBLISH_QUEUE_MAX_SIZE = 16;

CatchupWork::CatchupWork(Application& app,
                         CatchupConfiguration catchupConfiguration,
                         std::shared_ptr<HistoryArchive> archive)
    : Work(app, "catchup", BasicWork::RETRY_NEVER)
    , mLocalState{app.getLedgerManager().getLastClosedLedgerHAS()}
    , mDownloadDir{std::make_unique<TmpDir>(
          mApp.getTmpDirManager().tmpDir(getName()))}
    , mCatchupConfiguration{catchupConfiguration}
    , mArchive{archive}
{
    if (mArchive)
    {
        CLOG_INFO(History, "CatchupWork: selected archive {}",
                  mArchive->getName());
    }
}

CatchupWork::~CatchupWork()
{
}

std::string
CatchupWork::getStatus() const
{
    std::string toLedger;
    if (mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT)
    {
        toLedger = mGetHistoryArchiveStateWork &&
                           mGetHistoryArchiveStateWork->getState() ==
                               State::WORK_SUCCESS
                       ? std::to_string(mGetHistoryArchiveStateWork
                                            ->getHistoryArchiveState()
                                            .currentLedger)
                       : "CURRENT";
    }
    else
    {
        toLedger = std::to_string(mCatchupConfiguration.toLedger());
    }

    return fmt::format("Catching up to ledger {}: {}", toLedger,
                       mCurrentWork ? mCurrentWork->getStatus()
                                    : Work::getStatus());
}

void
CatchupWork::doReset()
{
    ZoneScoped;
    mBucketsAppliedEmitted = false;
    mTransactionsVerifyEmitted = false;
    mBuckets.clear();
    mDownloadVerifyLedgersSeq.reset();
    mBucketVerifyApplySeq.reset();
    mTransactionsVerifyApplySeq.reset();
    mGetHistoryArchiveStateWork.reset();
    mApplyBufferedLedgersWork.reset();
    auto const& lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    mLastClosedLedgerHashPair =
        LedgerNumHashPair(lcl.header.ledgerSeq, make_optional<Hash>(lcl.hash));
    mCatchupSeq.reset();
    mGetBucketStateWork.reset();
    mVerifyTxResults.reset();
    mVerifyLedgers.reset();
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    mCurrentWork.reset();
}

bool
CatchupWork::hasAnyLedgersToCatchupTo() const
{
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == State::WORK_SUCCESS);

    return mLastClosedLedgerHashPair.first <=
           mGetHistoryArchiveStateWork->getHistoryArchiveState().currentLedger;
}

void
CatchupWork::downloadVerifyLedgerChain(CatchupRange const& catchupRange,
                                       LedgerNumHashPair rangeEnd)
{
    ZoneScoped;
    auto verifyRange = catchupRange.getFullRangeIncludingBucketApply();
    assert(verifyRange.mCount != 0);
    auto checkpointRange =
        CheckpointRange{verifyRange, mApp.getHistoryManager()};
    auto getLedgers = std::make_shared<BatchDownloadWork>(
        mApp, checkpointRange, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir,
        mArchive);
    mRangeEndPromise = std::promise<LedgerNumHashPair>();
    mRangeEndFuture = mRangeEndPromise.get_future().share();
    mRangeEndPromise.set_value(rangeEnd);
    mVerifyLedgers = std::make_shared<VerifyLedgerChainWork>(
        mApp, *mDownloadDir, verifyRange, mLastClosedLedgerHashPair,
        mRangeEndFuture);

    std::vector<std::shared_ptr<BasicWork>> seq{getLedgers, mVerifyLedgers};
    mDownloadVerifyLedgersSeq =
        addWork<WorkSequence>("download-verify-ledgers-seq", seq);
    mCurrentWork = mDownloadVerifyLedgersSeq;
}

void
CatchupWork::downloadVerifyTxResults(CatchupRange const& catchupRange)
{
    ZoneScoped;
    auto range = catchupRange.getReplayRange();
    auto checkpointRange = CheckpointRange{range, mApp.getHistoryManager()};
    mVerifyTxResults = std::make_shared<DownloadVerifyTxResultsWork>(
        mApp, checkpointRange, *mDownloadDir);
}

bool
CatchupWork::alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const
{
    return atCheckpoint ==
           mGetHistoryArchiveStateWork->getHistoryArchiveState().currentLedger;
}

WorkSeqPtr
CatchupWork::downloadApplyBuckets()
{
    ZoneScoped;
    auto const& has = mGetBucketStateWork->getHistoryArchiveState();
    std::vector<std::string> hashes = has.differingBuckets(mLocalState);
    auto getBuckets = std::make_shared<DownloadBucketsWork>(
        mApp, mBuckets, hashes, *mDownloadDir, mArchive);

    auto applyBuckets = std::make_shared<ApplyBucketsWork>(
        mApp, mBuckets, has, mVerifiedLedgerRangeStart.header.ledgerVersion);

    std::vector<std::shared_ptr<BasicWork>> seq{getBuckets, applyBuckets};
    return std::make_shared<WorkSequence>(mApp, "download-verify-apply-buckets",
                                          seq, RETRY_NEVER);
}

void
CatchupWork::assertBucketState()
{
    auto const& has = mGetBucketStateWork->getHistoryArchiveState();

    // Consistency check: remote state and mVerifiedLedgerRangeStart should
    // point to the same ledger and the same BucketList.
    if (has.currentLedger != mVerifiedLedgerRangeStart.header.ledgerSeq)
    {
        CLOG_ERROR(History, "Caught up to wrong ledger: wanted {}, got {}",
                   has.currentLedger,
                   mVerifiedLedgerRangeStart.header.ledgerSeq);
    }
    assert(has.currentLedger == mVerifiedLedgerRangeStart.header.ledgerSeq);
    assert(has.getBucketListHash() ==
           mVerifiedLedgerRangeStart.header.bucketListHash);

    // Consistency check: LCL should be in the _past_ from
    // firstVerified, since we're about to clobber a bunch of DB
    // state with new buckets held in firstVerified's state.
    auto lcl = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (mVerifiedLedgerRangeStart.header.ledgerSeq < lcl.header.ledgerSeq)
    {
        throw std::runtime_error(
            fmt::format("Catchup MINIMAL applying ledger earlier than local "
                        "LCL: {:s} < {:s}",
                        LedgerManager::ledgerAbbrev(mVerifiedLedgerRangeStart),
                        LedgerManager::ledgerAbbrev(lcl)));
    }
}

void
CatchupWork::downloadApplyTransactions(CatchupRange const& catchupRange)
{
    ZoneScoped;
    auto waitForPublish = mCatchupConfiguration.offline();
    auto range = catchupRange.getReplayRange();
    mTransactionsVerifyApplySeq = std::make_shared<DownloadApplyTxsWork>(
        mApp, *mDownloadDir, range, mLastApplied, waitForPublish, mArchive);
}

BasicWork::State
CatchupWork::runCatchupStep()
{
    ZoneScoped;
    // Step 1: Get history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        auto toLedger = mCatchupConfiguration.toLedger() == 0
                            ? "CURRENT"
                            : std::to_string(mCatchupConfiguration.toLedger());
        CLOG_INFO(History,
                  "Starting catchup with configuration:\n  lastClosedLedger: "
                  "{}\n  toLedger: {}\n  count: {}",
                  mApp.getLedgerManager().getLastClosedLedgerNum(), toLedger,
                  mCatchupConfiguration.count());

        auto toCheckpoint =
            mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT
                ? CatchupConfiguration::CURRENT
                : mApp.getHistoryManager().checkpointContainingLedger(
                      mCatchupConfiguration.toLedger());
        mGetHistoryArchiveStateWork =
            addWork<GetHistoryArchiveStateWork>(toCheckpoint, mArchive);
        mCurrentWork = mGetHistoryArchiveStateWork;
        return State::WORK_RUNNING;
    }
    else if (mGetHistoryArchiveStateWork->getState() != State::WORK_SUCCESS)
    {
        return mGetHistoryArchiveStateWork->getState();
    }

    auto const& has = mGetHistoryArchiveStateWork->getHistoryArchiveState();
    // If the HAS is a newer version and contains networkPassphrase,
    // we should make sure that it matches the config's networkPassphrase.
    if (!has.networkPassphrase.empty() &&
        has.networkPassphrase != mApp.getConfig().NETWORK_PASSPHRASE)
    {
        CLOG_ERROR(History, "The network passphrase of the "
                            "application does not match that of the "
                            "history archive state");
        return State::WORK_FAILURE;
    }

    // Step 2: Compare local and remote states
    if (!hasAnyLedgersToCatchupTo())
    {
        CLOG_INFO(History, "*");
        CLOG_INFO(
            History,
            "* Target ledger {} is not newer than last closed ledger {} - "
            "nothing to do",
            has.currentLedger, mLastClosedLedgerHashPair.first);

        if (mCatchupConfiguration.toLedger() == CatchupConfiguration::CURRENT)
        {
            CLOG_INFO(History, "* Wait until next checkpoint before retrying");
        }
        else
        {
            CLOG_INFO(
                History,
                "* If you really want to catchup to {} run stellar-core new-db",
                mCatchupConfiguration.toLedger());
        }

        CLOG_INFO(History, "*");

        CLOG_ERROR(History, "Nothing to catchup to ");

        return State::WORK_FAILURE;
    }

    auto resolvedConfiguration =
        mCatchupConfiguration.resolve(has.currentLedger);
    auto catchupRange =
        CatchupRange{mLastClosedLedgerHashPair.first, resolvedConfiguration,
                     mApp.getHistoryManager()};

    // Step 3: If needed, download archive state for buckets
    if (catchupRange.applyBuckets())
    {
        auto applyBucketsAt = catchupRange.getBucketApplyLedger();
        if (!alreadyHaveBucketsHistoryArchiveState(applyBucketsAt))
        {
            if (!mGetBucketStateWork)
            {
                mGetBucketStateWork = addWork<GetHistoryArchiveStateWork>(
                    applyBucketsAt, mArchive);
                mCurrentWork = mGetBucketStateWork;
            }
            if (mGetBucketStateWork->getState() != State::WORK_SUCCESS)
            {
                return mGetBucketStateWork->getState();
            }
        }
        else
        {
            mGetBucketStateWork = mGetHistoryArchiveStateWork;
        }
    }

    // Step 4: Download, verify and apply ledgers, buckets and transactions

    // Bucket and transaction processing has started
    if (mCatchupSeq)
    {
        assert(mDownloadVerifyLedgersSeq);
        assert(mTransactionsVerifyApplySeq || !catchupRange.replayLedgers());

        if (mCatchupSeq->getState() == State::WORK_SUCCESS)
        {
            // Step 4.4: Apply buffered ledgers
            if (mApplyBufferedLedgersWork)
            {
                if (mApplyBufferedLedgersWork->getState() ==
                    State::WORK_SUCCESS)
                {
                    mApplyBufferedLedgersWork.reset();
                }
                else
                {
                    // waiting for mApplyBufferedLedgersWork to complete/error
                    // out
                    return mApplyBufferedLedgersWork->getState();
                }
            }
            // see if we need to apply buffered ledgers
            if (mApp.getCatchupManager().hasBufferedLedger())
            {
                mApplyBufferedLedgersWork = addWork<ApplyBufferedLedgersWork>();
                mCurrentWork = mApplyBufferedLedgersWork;
                return State::WORK_RUNNING;
            }

            return State::WORK_SUCCESS;
        }
        else if (mBucketVerifyApplySeq)
        {
            if (mBucketVerifyApplySeq->getState() == State::WORK_SUCCESS &&
                !mBucketsAppliedEmitted)
            {
                mApp.getLedgerManager().setLastClosedLedger(
                    mVerifiedLedgerRangeStart);
                mBucketsAppliedEmitted = true;
                mBuckets.clear();
                mLastApplied =
                    mApp.getLedgerManager().getLastClosedLedgerHeader();
            }
        }
        else if (mTransactionsVerifyApplySeq)
        {
            if (mTransactionsVerifyApplySeq->getState() ==
                    State::WORK_SUCCESS &&
                !mTransactionsVerifyEmitted)
            {
                mTransactionsVerifyEmitted = true;

                // In this case we should actually have been caught-up during
                // the replay process and, if judged successful, our LCL should
                // be the one provided as well.
                auto& lastClosed =
                    mApp.getLedgerManager().getLastClosedLedgerHeader();
                assert(mLastApplied.hash == lastClosed.hash);
                assert(mLastApplied.header == lastClosed.header);
            }
        }
        return mCatchupSeq->getState();
    }
    // Still waiting for ledger headers
    else if (mDownloadVerifyLedgersSeq)
    {
        if (mDownloadVerifyLedgersSeq->getState() == State::WORK_SUCCESS)
        {
            mVerifiedLedgerRangeStart =
                mVerifyLedgers->getMaxVerifiedLedgerOfMinCheckpoint();
            if (catchupRange.applyBuckets() && !mBucketsAppliedEmitted)
            {
                assertBucketState();
            }

            std::vector<std::shared_ptr<BasicWork>> seq;
            if (mCatchupConfiguration.mode() ==
                CatchupConfiguration::Mode::OFFLINE_COMPLETE)
            {
                downloadVerifyTxResults(catchupRange);
                seq.push_back(mVerifyTxResults);
            }

            if (catchupRange.applyBuckets())
            {
                // Step 4.2: Download, verify and apply buckets
                mBucketVerifyApplySeq = downloadApplyBuckets();
                seq.push_back(mBucketVerifyApplySeq);
            }

            if (catchupRange.replayLedgers())
            {
                // Step 4.3: Download and apply ledger chain
                downloadApplyTransactions(catchupRange);
                seq.push_back(mTransactionsVerifyApplySeq);
            }

            mCatchupSeq =
                addWork<WorkSequence>("catchup-seq", seq, RETRY_NEVER);
            mCurrentWork = mCatchupSeq;
            return State::WORK_RUNNING;
        }
        return mDownloadVerifyLedgersSeq->getState();
    }

    // Step 4.1: Download and verify ledger chain
    downloadVerifyLedgerChain(
        catchupRange,
        LedgerNumHashPair(catchupRange.last(), mCatchupConfiguration.hash()));

    return State::WORK_RUNNING;
}

BasicWork::State
CatchupWork::doWork()
{
    ZoneScoped;
    auto nextState = runCatchupStep();
    auto& cm = mApp.getCatchupManager();

    if (nextState == BasicWork::State::WORK_SUCCESS)
    {
        assert(!cm.hasBufferedLedger());
    }

    cm.logAndUpdateCatchupStatus(true);
    return nextState;
}

void
CatchupWork::onFailureRaise()
{
    CLOG_WARNING(History, "Catchup failed");
    Work::onFailureRaise();
}

void
CatchupWork::onSuccess()
{
    CLOG_INFO(History, "Catchup finished");
    Work::onSuccess();
}
}
