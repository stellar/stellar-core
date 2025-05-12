#pragma once

// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupWork.h"
#include "catchup/LedgerApplyManager.h"
#include <memory>

namespace medida
{
class Meter;
class Counter;
}

namespace stellar
{

class Application;
class Work;

class LedgerApplyManagerImpl : public LedgerApplyManager
{
    // Maximum number of ledgers that can be queued to apply (this only applies
    // when Config.parallelLedgerClose() == true). If this number if exceeded,
    // core stops scheduling new ledgers to apply, and goes into catchup mode.
    static uint32_t const MAX_EXTERNALIZE_LEDGER_APPLY_DRIFT;

    Config const& mConfig;
    Application& mApp;

    std::shared_ptr<CatchupWork> mCatchupWork;

    // key is ledgerSeq
    // mSyncingLedgers has the following invariants:
    // (1) It is empty, or
    // (2) It starts with LCL + 1 (the next ledger to apply), or
    // (3) It contains at most 64 + 1 = 65 ledgers.
    //
    // In the third case, it is always a subset of
    // {L, L + 1, ..., L + 63, L + 64} where L is the first ledger of a
    // checkpoint. {L, L + 1, ..., L + 63} is meant to be used to apply after
    // downloading previous checkpoints. L + 64 is buffered so that when we hear
    // L + 65 (or any ledger after that), we can delete {L, ..., L + 63} as it
    // indicates that the checkpoint containing L + 64 has been published.
    //
    // There are two methods that modify mSyncingLedgers
    // (trimSyncingLedgers, tryApplySyncingLedgers) and they both
    // maintain the invariants above.
    std::map<uint32_t, LedgerCloseData> mSyncingLedgers;
    medida::Counter& mSyncingLedgersSize;

    // These state variables track the flow of ledgers through mSyncingLedgers,
    // they are the variables Q and L in the diagram in LedgerManager.h. See
    // that diagram for details and discussion of the threading model and
    // flow of ledgers between LedgerApplyManager and LedgerManager.
    std::optional<uint32_t> mLastQueuedToApply;
    uint32_t mLargestLedgerSeqHeard;
    // Use in the context of parallel ledger apply to indicate background thread
    // is currently closing a ledger or has ledgers queued to apply.
    bool mCurrentlyApplyingLedger{false};

    void updateLastQueuedToApply();
    void startOnlineCatchup();
    void trimSyncingLedgers();
    void tryApplySyncingLedgers();
    uint32_t getCatchupCount();
    CatchupMetrics mMetrics;

    // Check if catchup can't be performed due to local version incompatibility
    // or state corruption. Once this flag is set, core won't attempt catchup as
    // it will never succeed.
    bool mCatchupFatalFailure{false};
    State mState;
    void setState(State s);
    LedgerManager& mLedgerManager;

  public:
    LedgerApplyManagerImpl(Application& app);
    ~LedgerApplyManagerImpl() override;

    void valueExternalized(LedgerCloseData const& ledgerData,
                           bool isLatestSlot) override;
    ProcessLedgerResult processLedger(LedgerCloseData const& ledgerData,
                                      bool isLatestSlot) override;
    void startCatchup(CatchupConfiguration configuration,
                      std::shared_ptr<HistoryArchive> archive) override;

    std::string getStatus() const override;

    BasicWork::State getCatchupWorkState() const override;
    bool catchupWorkIsDone() const override;
    bool isCatchupInitialized() const override;

    void logAndUpdateCatchupStatus(bool contiguous,
                                   std::string const& message) override;
    void logAndUpdateCatchupStatus(bool contiguous) override;

    std::optional<LedgerCloseData> maybeGetNextBufferedLedgerToApply() override;
    std::optional<LedgerCloseData> maybeGetLargestBufferedLedger() override;
    uint32_t getLargestLedgerSeqHeard() const override;
    uint32_t getMaxQueuedToApply() override;

    void syncMetrics() override;

    CatchupMetrics const&
    getCatchupMetrics() override
    {
        return mMetrics;
    }

    void historyArchiveStatesDownloaded(uint32_t num) override;
    void ledgersVerified(uint32_t num) override;
    void ledgerChainsVerificationFailed(uint32_t num) override;
    void bucketsApplied(uint32_t num) override;
    void txSetsApplied(uint32_t num) override;
    void fileDownloaded(FileType type, uint32_t num) override;

#ifdef BUILD_TESTS
    std::map<uint32_t, LedgerCloseData> const&
    getBufferedLedgers() const
    {
        return mSyncingLedgers;
    }

    std::shared_ptr<CatchupWork>
    getCatchupWork() const
    {
        return mCatchupWork;
    }

    bool
    getCatchupFatalFailure() const
    {
        return mCatchupFatalFailure;
    }

    std::optional<uint32_t> mMaxExternalizeApplyBuffer;
    uint32_t
    getMaxExternalizeApplyBuffer()
    {
        return mMaxExternalizeApplyBuffer ? *mMaxExternalizeApplyBuffer
                                          : MAX_EXTERNALIZE_LEDGER_APPLY_DRIFT;
    }
#endif

    void moveToSynced() override;
    State getState() const override;
    std::string getStateHuman() const override;

    void ledgerCloseComplete(uint32_t lcl, bool calledViaExternalize,
                             LedgerCloseData const& ledgerData) override;

    virtual bool
    isApplying() const override
    {
        releaseAssert(threadIsMain());
        return mCurrentlyApplyingLedger;
    }
};
}
