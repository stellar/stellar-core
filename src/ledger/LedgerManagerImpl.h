#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "history/HistoryManager.h"
#include "ledger/LedgerCloseMetaFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "ledger/SorobanMetrics.h"
#include "main/PersistentState.h"
#include "transactions/TransactionFrame.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
#include <filesystem>
#include <string>

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace medida
{
class Timer;
class Counter;
class Histogram;
class Buckets;
}

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class Database;
class LedgerTxnHeader;
class BasicWork;

class LedgerManagerImpl : public LedgerManager
{
  protected:
    Application& mApp;
    std::unique_ptr<XDROutputFileStream> mMetaStream;
    std::unique_ptr<XDROutputFileStream> mMetaDebugStream;
    std::weak_ptr<BasicWork> mFlushAndRotateMetaDebugWork;
    std::filesystem::path mMetaDebugPath;

  private:
    // Output of the apply process, also what gets held as "LCL".
    struct LedgerState
    {
        LedgerHeaderHistoryEntry ledgerHeader;
        std::shared_ptr<SorobanNetworkConfig const> sorobanConfig;
        HistoryArchiveState has;
        std::shared_ptr<SearchableLiveBucketListSnapshot const> snapshot;
    };

    // Any state that apply needs to access through the app connector should go
    // here, at very least just to make it clear what is being accessed by which
    // threads. We may try to further encapsulate it.
    struct ApplyState
    {
        // Latest Soroban config during apply (should not be used outside of
        // application, as it may be in half-valid state). Note that access to
        // this variable is not synchronized, since it should only be used by
        // one thread (main or ledger close).
        std::shared_ptr<SorobanNetworkConfig> mSorobanNetworkConfig;
    };

    struct LedgerApplyMetrics
    {
        medida::Timer& mTransactionApply;
        medida::Histogram& mTransactionCount;
        medida::Histogram& mOperationCount;
        medida::Histogram& mPrefetchHitRate;
        medida::Timer& mLedgerClose;
        medida::Buckets& mLedgerAgeClosed;
        medida::Counter& mLedgerAge;
        medida::Counter& mTransactionApplySucceeded;
        medida::Counter& mTransactionApplyFailed;
        medida::Counter& mSorobanTransactionApplySucceeded;
        medida::Counter& mSorobanTransactionApplyFailed;
        medida::Meter& mMetaStreamBytes;
        medida::Timer& mMetaStreamWriteTime;
        LedgerApplyMetrics(medida::MetricsRegistry& registry);
    };

    // This state is private to the apply thread and holds work-in-progress
    // that gets accessed via the AppConnector, from inside transactions.
    ApplyState mApplyState;

    // Cached LCL state output from last apply (or loaded from DB on startup).
    LedgerState mLastClosedLedgerState;

    LedgerApplyMetrics mLedgerApplyMetrics;
    SorobanMetrics mSorobanMetrics;

    VirtualClock::time_point mLastClose;

    // Use mutex to guard ledger state during apply
    mutable std::recursive_mutex mLedgerStateMutex;

    medida::Timer& mCatchupDuration;

    std::unique_ptr<LedgerCloseMetaFrame> mNextMetaToEmit;

    // Use in the context of parallel ledger apply to indicate background thread
    // is currently closing a ledger or has ledgers queued to apply.
    bool mCurrentlyApplyingLedger{false};

    LedgerState& getLCLState();
    LedgerState const& getLCLState() const;

    static std::vector<MutableTxResultPtr> processFeesSeqNums(
        ApplicableTxSetFrame const& txSet, AbstractLedgerTxn& ltxOuter,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        LedgerCloseData const& ledgerData);

    TransactionResultSet applyTransactions(
        ApplicableTxSetFrame const& txSet,
        std::vector<MutableTxResultPtr> const& mutableTxResults,
        AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta);

    // initialLedgerVers must be the ledger version at the start of the ledger.
    // On the ledger in which a protocol upgrade from vN to vN + 1 occurs,
    // initialLedgerVers must be vN.
    LedgerState sealLedgerTxnAndStoreInBucketsAndDB(
        AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        uint32_t initialLedgerVers);

    HistoryArchiveState
    storePersistentStateAndLedgerHeaderInDB(LedgerHeader const& header,
                                            bool appendToCheckpoint);
    static void prefetchTransactionData(AbstractLedgerTxnParent& rootLtx,
                                        ApplicableTxSetFrame const& txSet,
                                        Config const& config);
    static void prefetchTxSourceIds(AbstractLedgerTxnParent& rootLtx,
                                    ApplicableTxSetFrame const& txSet,
                                    Config const& config);

    State mState;

#ifdef BUILD_TESTS
    std::vector<TransactionMetaFrame> mLastLedgerTxMeta;
#endif

    void setState(State s);

    void emitNextMeta();

    // Publishes soroban metrics, including select network config limits as well
    // as the actual ledger usage.
    void publishSorobanMetrics();

    // Update cached last closed ledger state values managed by this class.
    void advanceLastClosedLedgerState(LedgerState const& output);

  protected:
    // initialLedgerVers must be the ledger version at the start of the ledger
    // and currLedgerVers is the ledger version in the current ltx header. These
    // values are the same except on the ledger in which a protocol upgrade from
    // vN to vN + 1 occurs. initialLedgerVers must be vN and currLedgerVers must
    // be vN + 1.

    // NB: LedgerHeader is a copy here to prevent footguns in case ltx
    // invalidates any header references
    virtual void sealLedgerTxnAndTransferEntriesToBucketList(
        AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        LedgerHeader lh, uint32_t initialLedgerVers);

    // Update bucket list snapshot, and construct LedgerState return
    // value, which contains all information relevant to ledger state (HAS,
    // ledger header, network config, bucketlist snapshot).
    LedgerState
    advanceBucketListSnapshotAndMakeLedgerState(LedgerHeader const& header,
                                                HistoryArchiveState const& has);
    void logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                           size_t numOps);

  public:
    LedgerManagerImpl(Application& app);

    // Reloads the network configuration from the ledger.
    // This needs to be called every time a ledger is closed.
    // This call is read-only and hence `ltx` can be read-only.
    void updateSorobanNetworkConfigForApply(AbstractLedgerTxn& ltx) override;
    void moveToSynced() override;
    void beginApply() override;
    State getState() const override;
    std::string getStateHuman() const override;

    void valueExternalized(LedgerCloseData const& ledgerData,
                           bool isLatestSlot) override;

    uint32_t getLastMaxTxSetSize() const override;
    uint32_t getLastMaxTxSetSizeOps() const override;
    Resource maxLedgerResources(bool isSoroban) override;
    Resource maxSorobanTransactionResources() override;
    int64_t getLastMinBalance(uint32_t ownerCount) const override;
    uint32_t getLastReserve() const override;
    uint32_t getLastTxFee() const override;
    uint32_t getLastClosedLedgerNum() const override;
    SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() override;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() override;

    bool hasLastClosedSorobanNetworkConfig() const override;

#ifdef BUILD_TESTS
    SorobanNetworkConfig& getMutableSorobanNetworkConfigForApply() override;
    std::vector<TransactionMetaFrame> const&
    getLastClosedLedgerTxMeta() override;
    TransactionResultSet mLatestTxResultSet{};
    void storeCurrentLedgerForTest(LedgerHeader const& header) override;
#endif

    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader const& genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(bool restoreBucketlist) override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;

    HistoryArchiveState getLastClosedLedgerHAS() override;

    Database& getDatabase() override;

    void startCatchup(
        CatchupConfiguration configuration,
        std::shared_ptr<HistoryArchive> archive,
        std::set<std::shared_ptr<LiveBucket>> bucketsToRetain) override;

    void applyLedger(LedgerCloseData const& ledgerData,
                     bool calledViaExternalize) override;
    void ledgerCloseComplete(uint32_t lcl, bool calledViaExternalize,
                             LedgerCloseData const& ledgerData);
    void
    setLastClosedLedger(LedgerHeaderHistoryEntry const& lastClosed) override;

    void manuallyAdvanceLedgerHeader(LedgerHeader const& header) override;

    void setupLedgerCloseMetaStream();
    void maybeResetLedgerCloseMetaDebugStream(uint32_t ledgerSeq);

    SorobanMetrics& getSorobanMetrics() override;
    SearchableSnapshotConstPtr getLastClosedSnaphot() override;
    virtual bool
    isApplying() const override
    {
        return mCurrentlyApplyingLedger;
    }
};
}
