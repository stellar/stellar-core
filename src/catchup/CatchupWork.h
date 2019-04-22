// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupConfiguration.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/HistoryArchive.h"
#include "ledger/LedgerRange.h"
#include "work/Work.h"
#include "work/WorkSequence.h"

namespace stellar
{

class LedgerRange;
class CheckpointRange;
class HistoryManager;
class Bucket;
class TmpDir;

// Range required to do a catchup.
//
// If second is true, this catchup requires downloading and applying buckets
// for the first.first() and then downloading and applying transactions.
//
// If second is false, this catchup requires downloading and applying
// transactions for whole range of ledgers.
//
// When (first.first() == first.last()) && second this is equivalent to old
// CATCHUP_MIMIMAL.
//
// For old CATCHUP_COMPLETE second is always false, as CATCHUP_COMPLETE only
// applies transactions.
//
// For old CATCHUP_RECENT value of second depends on value of last closed
// ledger - if CATCHUP_RECENT would go before it, it behaves exactly as
// CATCHUP_COMPLETE. If not, second is true and miminal catchup will be done
// first.
using CatchupRange = std::pair<LedgerRange, bool>;
using WorkSeqPtr = std::shared_ptr<WorkSequence>;

// CatchupWork does all the neccessary work to perform any type of catchup.
// It accepts CatchupConfiguration structure to know from which ledger to which
// one do the catchup and if it involves only applying ledgers or ledgers and
// buckets.
//
// First thing it does is to get a history state which allows to calculate
// proper destination ledger (in case CatchupConfiguration::CURRENT) was used
// and to get list of buckets that should be in database on that ledger.
//
// Next step is downloading and verifying ledgers (if verifyMode is set to
// VERIFY_BUFFERED_LEDGERS it can also verify against ledgers currently
// buffered in LedgerManager).
//
// Then, depending on configuration, it can download, verify and apply buckets
// (as in MINIMAL and RECENT catchups), and then download and apply
// transactions (as in COMPLETE and RECENT catchups).
//
// After that, catchup is done and node can replay buffered ledgers and take
// part in consensus protocol.

class CatchupWork : public Work
{
  protected:
    HistoryArchiveState mLocalState;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

    void doReset() override;
    BasicWork::State doWork() override;
    void onFailureRaise() override;
    void onSuccess() override;

  public:
    enum class ProgressState
    {
        APPLIED_BUCKETS,
        APPLIED_TRANSACTIONS,
        FINISHED
    };

    // ProgressHandler is called in different phases of catchup with following
    // values of ProgressState argument:
    // - APPLIED_BUCKETS - called after buckets had been applied at lastClosed
    // ledger
    // - APPLIED_TRANSACTIONS - called after transactions had been applied,
    // last one at lastClosed ledger
    // - FINISHED - called after buckets and transaction had been applied,
    // lastClosed is the same as value from previous call
    //
    // Different types of catchup causes different sequence of calls:
    // - CATCHUP_MINIMAL calls APPLIED_BUCKETS then FINISHED
    // - CATCHUP_COMPLETE calls APPLIED_TRANSACTIONS then FINISHED
    // - CATCHUP_RECENT calls APPLIED_BUCKETS, APPLIED_TRANSACTIONS then
    // FINISHED
    //
    // In case of error this callback is called with non-zero ec parameter and
    // the rest of them does not matter.
    using ProgressHandler = std::function<void(
        asio::error_code const& ec, ProgressState progressState,
        LedgerHeaderHistoryEntry const& lastClosed,
        CatchupConfiguration::Mode catchupMode)>;

    /**
     * Preconditions:
     * * lastClosedLedger > 0
     * * configuration.toLedger() >= lastClosedLedger
     * * configuration.toLedger() != CatchupConfiguration::CURRENT
     */

    static CatchupRange
    makeCatchupRange(uint32_t lastClosedLedger,
                     CatchupConfiguration const& configuration,
                     HistoryManager const& historyManager);

    CatchupWork(Application& app, CatchupConfiguration catchupConfiguration,
                ProgressHandler progressHandler, size_t maxRetries);
    ~CatchupWork();
    std::string getStatus() const override;

  private:
    HistoryArchiveState mRemoteState;
    HistoryArchiveState mApplyBucketsRemoteState;
    LedgerNumHashPair mLastClosedLedgerHashPair;
    CatchupConfiguration const mCatchupConfiguration;
    LedgerHeaderHistoryEntry mVerifiedLedgerRangeStart;
    LedgerHeaderHistoryEntry mLastApplied;
    ProgressHandler mProgressHandler;
    bool mBucketsAppliedEmitted{false};

    std::shared_ptr<BasicWork> mGetHistoryArchiveStateWork;
    std::shared_ptr<BasicWork> mGetBucketStateWork;

    WorkSeqPtr mDownloadVerifyLedgersSeq;
    std::shared_ptr<VerifyLedgerChainWork> mVerifyLedgers;
    WorkSeqPtr mBucketVerifyApplySeq;
    WorkSeqPtr mTransactionsVerifyApplySeq;
    WorkSeqPtr mCatchupSeq;

    bool hasAnyLedgersToCatchupTo() const;
    bool alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const;
    void assertBucketState();

    void downloadVerifyLedgerChain(CatchupRange catchupRange,
                                   LedgerNumHashPair rangeEnd);
    WorkSeqPtr downloadApplyBuckets();
    WorkSeqPtr downloadApplyTransactions(CatchupRange catchupRange);
};
}
