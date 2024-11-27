// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupConfiguration.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/HistoryArchive.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "util/Thread.h"
#include "work/Work.h"
#include "work/WorkSequence.h"

namespace stellar
{

class HistoryManager;
class Bucket;
class TmpDir;
class CatchupRange;

using WorkSeqPtr = std::shared_ptr<WorkSequence>;

// CatchupWork does all the necessary work to perform any type of catchup.
// It accepts CatchupConfiguration structure to know from which ledger to which
// one to do the catchup and if it involves only applying ledgers or ledgers and
// buckets.
//
// First, it gets a history state, which allows it to calculate a
// proper destination ledger (in case CatchupConfiguration::CURRENT)
// and get a list of buckets that should be in the database on that ledger.
//
// Next, it downloads and verifies ledgers (if verifyMode is set to
// VERIFY_BUFFERED_LEDGERS, it can also verify against ledgers currently
// buffered in LedgerManager).
//
// Then, depending on configuration, it can download, verify and apply buckets
// (as in MINIMAL and RECENT catchups), and then download and apply
// transactions (as in COMPLETE and RECENT catchups).
//
// After that, catchup is done and the node can replay buffered ledgers and take
// part in consensus protocol.

class CatchupWork : public Work
{
  protected:
    HistoryArchiveState mLocalState;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::map<std::string, std::shared_ptr<LiveBucket>> mBuckets;

    void doReset() override;
    BasicWork::State doWork() override;
    void onFailureRaise() override;
    void onSuccess() override;

  public:
    // Resume application when publish queue shrinks down to this many
    // checkpoints
    static uint32_t const PUBLISH_QUEUE_UNBLOCK_APPLICATION;

    // Allow at most this many checkpoints in the publish queue while catching
    // up. If the queue grows too big, ApplyCheckpointWork will wait until
    // enough snapshots were published, and unblock itself.
    static uint32_t const PUBLISH_QUEUE_MAX_SIZE;

    CatchupWork(Application& app, CatchupConfiguration catchupConfiguration,
                std::set<std::shared_ptr<LiveBucket>> bucketsToRetain,
                std::shared_ptr<HistoryArchive> archive = nullptr);
    virtual ~CatchupWork();
    std::string getStatus() const override;

    CatchupConfiguration const&
    getCatchupConfiguration() const
    {
        return mCatchupConfiguration;
    }

    bool
    fatalFailure()
    {
        if (futureIsReady(mFatalFailureFuture))
        {
            return mFatalFailureFuture.get();
        }
        return false;
    }

  private:
    LedgerNumHashPair mLastClosedLedgerHashPair;
    CatchupConfiguration const mCatchupConfiguration;
    LedgerHeaderHistoryEntry mVerifiedLedgerRangeStart;
    LedgerHeaderHistoryEntry mLastApplied;
    std::shared_ptr<HistoryArchive> mArchive;
    bool mBucketsAppliedEmitted{false};
    bool mTransactionsVerifyEmitted{false};

    std::shared_ptr<GetHistoryArchiveStateWork> mGetHistoryArchiveStateWork;
    std::shared_ptr<GetHistoryArchiveStateWork> mGetBucketStateWork;

    WorkSeqPtr mDownloadVerifyLedgersSeq;
    std::promise<LedgerNumHashPair> mRangeEndPromise;
    std::shared_future<LedgerNumHashPair> mRangeEndFuture;
    std::shared_ptr<VerifyLedgerChainWork> mVerifyLedgers;
    std::shared_ptr<Work> mVerifyTxResults;
    WorkSeqPtr mBucketVerifyApplySeq;
    std::shared_ptr<Work> mTransactionsVerifyApplySeq;
    std::shared_ptr<BasicWork> mApplyBufferedLedgersWork;
    WorkSeqPtr mCatchupSeq;

    std::shared_ptr<BasicWork> mCurrentWork;

    std::shared_future<bool> mFatalFailureFuture;

    bool alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const;
    void assertBucketState();

    void downloadVerifyLedgerChain(CatchupRange const& catchupRange,
                                   LedgerNumHashPair rangeEnd);
    WorkSeqPtr downloadApplyBuckets();
    void downloadApplyTransactions(CatchupRange const& catchupRange);
    void downloadVerifyTxResults(CatchupRange const& catchupRange);
    BasicWork::State runCatchupStep();

    BasicWork::State getAndMaybeSetHistoryArchiveState();
    BasicWork::State
    getAndMaybeSetBucketHistoryArchiveState(uint32_t applyBucketsAt);

    std::optional<HistoryArchiveState> mHAS;
    std::optional<HistoryArchiveState> mBucketHAS;
    std::set<std::shared_ptr<LiveBucket>> mRetainedBuckets;
};
}
