// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupConfiguration.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/HistoryArchive.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerRange.h"
#include "work/Work.h"
#include "work/WorkSequence.h"

namespace stellar
{

class HistoryManager;
class Bucket;
class TmpDir;
struct LedgerRange;
struct CheckpointRange;

// Range required to do a catchup.
//
// Ranges originate in CatchupConfigurations which have "last" ledger L to catch
// up to, and a count C of ledgers "before" L to replay. The devil is in the
// details:
//
//  - When the database already has LCL != LedgerManager.GENESIS_LEDGER_SEQ we
//    ignore C and just replay from LCL-to-L to avoid the possibility of
//    introducing gaps into our replay.
//
//  - This leaves two sub-cases, both with LCL == GENESIS_LEDGER_SEQ:
//
//    - When LCL + C > L, the user asked for something nonsensical but again we
//      just interpret this as meaning "replay from LCL-to-L".
//
//    - Otherwise the user is implicitly asking for the range of size C given by
//      the interval [K, L] to be replayed, where K=(L-C)+1. We then round down
//      to the last ledger B of the checkpoint _strictly before_ K, apply
//      buckets at B (eg. 63, 127, etc.), and do replay from the ledger _after_
//      B (eg. 64, 128, etc.) The "strictly before" part here ensures that if
//      the user asks for C ledgers to be replayed, we will always emit
//      transaction metadata for >= C ledgers -- and thereby always replay
//      ledger K -- even if K happens to be on a checkpoint boundary (and
//      therefore could have been restored from buckets).
//
// All this is calculated when CatchupRange is constructed and stored in its
// member structure CatchupRange::Ledgers, where mFirst is the first ledger
// that _will be replayed_ and, if there's bucket-applying to do, it'll happen
// at ledger mFirst-1.

struct CatchupRange final
{
    struct Ledgers
    {
        uint32_t const mFirst;
        uint32_t const mCount;
    };

    Ledgers mLedgers;
    bool const mApplyBuckets;

    /**
     * Preconditions:
     * * lastClosedLedger > 0
     * * configuration.toLedger() > lastClosedLedger
     * * configuration.toLedger() != CatchupConfiguration::CURRENT
     */
    explicit CatchupRange(uint32_t lastClosedLedger,
                          CatchupConfiguration const& configuration,
                          HistoryManager const& historyManager);

    bool
    applyLedgers() const
    {
        return mLedgers.mCount > 0;
    }

    uint32_t getLast() const;
    uint32_t getBucketApplyLedger() const;
};
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
    // Resume application when publish queue shrinks down to this many
    // checkpoints
    static uint32_t const PUBLISH_QUEUE_UNBLOCK_APPLICATION;

    // Allow at most this many checkpoints in the publish queue while catching
    // up. If the queue grows too big, ApplyCheckpointWork will wait until
    // enough snapshots were published, and unblock itself.
    static uint32_t const PUBLISH_QUEUE_MAX_SIZE;

    CatchupWork(Application& app, CatchupConfiguration catchupConfiguration,
                std::shared_ptr<HistoryArchive> archive = nullptr);
    virtual ~CatchupWork();
    std::string getStatus() const override;

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
    std::shared_ptr<VerifyLedgerChainWork> mVerifyLedgers;
    std::shared_ptr<Work> mVerifyTxResults;
    WorkSeqPtr mBucketVerifyApplySeq;
    std::shared_ptr<Work> mTransactionsVerifyApplySeq;
    std::shared_ptr<BasicWork> mApplyBufferedLedgersWork;
    WorkSeqPtr mCatchupSeq;

    std::shared_ptr<BasicWork> mCurrentWork;

    bool hasAnyLedgersToCatchupTo() const;
    bool alreadyHaveBucketsHistoryArchiveState(uint32_t atCheckpoint) const;
    void assertBucketState();

    void downloadVerifyLedgerChain(CatchupRange const& catchupRange,
                                   LedgerNumHashPair rangeEnd);
    WorkSeqPtr downloadApplyBuckets();
    void downloadApplyTransactions(CatchupRange const& catchupRange);
    void downloadVerifyTxResults(CatchupRange const& catchupRange);
    BasicWork::State runCatchupStep();
};
}
