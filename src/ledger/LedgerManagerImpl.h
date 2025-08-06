#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "history/HistoryManager.h"
#include "ledger/InMemorySorobanState.h"
#include "ledger/LedgerCloseMetaFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/NetworkConfig.h"
#include "ledger/SharedModuleCacheCompiler.h"
#include "ledger/SorobanMetrics.h"
#include "main/ApplicationImpl.h"
#include "main/PersistentState.h"
#include "rust/RustBridge.h"
#include "transactions/ParallelApplyStage.h"
#include "transactions/ParallelApplyUtils.h"
#include "transactions/TransactionFrame.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
#include <filesystem>
#include <optional>
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
class ParallelLedgerInfo;

#ifdef BUILD_TESTS
namespace BucketTestUtils
{
class LedgerManagerForBucketTests;
}
#endif

class LedgerManagerImpl : public LedgerManager
{
  protected:
    Application& mApp;
    std::unique_ptr<XDROutputFileStream> mMetaStream;
    std::unique_ptr<XDROutputFileStream> mMetaDebugStream;
    std::weak_ptr<BasicWork> mFlushAndRotateMetaDebugWork;
    std::filesystem::path mMetaDebugPath;

  private:
    struct LedgerApplyMetrics
    {
        SorobanMetrics mSorobanMetrics;
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

    // LedgerManager thread model is as follows. There is a "primary apply
    // thread" responsible for applying classic transactions, orchestrating
    // parallel Soroban transaction execution, committing state, and advancing
    // the ledger header. This "primary apply thread" is either the stellar-core
    // main thread (if background ledger apply is disabled) or the "apply
    // thread."
    //
    // In addition to the primary thread, LedgerManager will spin up "Soroban
    // threads" responsible for executing Soroban transactions. These threads
    // only execute transactions based on read-only state snapshots and never
    // commit changes. Soroban thread changes are accumulated and applied by the
    // primary apply thread. The primary apply thread is long lasting between
    // ledgers. Soroban threads are short lived, spawned during ledger
    // application and joined before committing state and advancing the ledger
    // header.
    //
    // ApplyState is the encapsulation used to store any state needed by the
    // primary apply thread and Soroban execution threads. Only the primary
    // apply thread can modify ApplyState. Soroban execution threads view
    // ApplyState as a immutable snapshot and can only call const functions.
    // While Soroban execution threads are live, the primary apply thread must
    // not mutate ApplyState. This is enforced via different "phases" of state
    // application (see below).
    //
    // Any state that apply needs to access through the app connector should go
    // here, at very least just to make it clear what is being accessed by which
    // threads. We may try to further encapsulate it.
    class ApplyState
    {
      public:
        // During the ledger close process, the apply state goes through these
        // phases:
        // - SETTING_UP_STATE: LedgerManager is waiting for or setting up
        //   ApplyState. This occurs on startup and after BucketApply during
        //   catchup.
        // - READY_TO_APPLY: Apply State is ready but not actively executing
        //   transactions or committing ledger state. ApplyState is immutable.
        // - APPLYING: ApplyState is actively executing transactions.
        //   During this time, ApplyState is immutable and multiple Soroban
        //   execution threads may read ApplyState concurrently. During TX
        //   execution, apply state changes are aggregated, but will not be
        //   committed until the COMMITTING phase.
        // - COMMITTING: ApplyState is committing ledger state.
        //   During this time, ApplyState is being modified and must only be
        //   accessed by the primary apply thread. This phase applies upgrades,
        //   commits state to disk, and advances the ledger header.
        //
        //  Phase transitions:
        //  SETTING_UP_STATE -> READY_TO_APPLY  -> SETTING_UP_STATE
        //                |
        //                -> APPLYING -> COMMITTING -> READY_TO_APPLY
        //
        //  SETTING_UP_STATE is the initial phase on startup. ApplyState may
        //  also transition from READY_TO_APPLY -> SETTING_UP_STATE if a node
        //  falls out of sync and must enter catchup, which requires re-entering
        //  the SETTING_UP_STATE phase to reset lcl state.
        //
        //  APPLYING is the only phase in which Soroban execution
        //  threads are active.
        enum class Phase
        {
            SETTING_UP_STATE,
            READY_TO_APPLY,
            APPLYING,
            COMMITTING
        };

      private:
        LedgerApplyMetrics mMetrics;

        AppConnector& mAppConnector;

        // Latest Soroban config during apply (should not be used outside of
        // application, as it may be in half-valid state). Note that access to
        // this variable is not synchronized, since it should only be used by
        // one thread (main or ledger close).
        std::shared_ptr<SorobanNetworkConfig> mSorobanNetworkConfig;

        // The current reusable / inter-ledger soroban module cache.
        ::rust::Box<rust_bridge::SorobanModuleCache> mModuleCache;

        // Manager object that (re)builds the module cache in background
        // threads. Only non-nullptr when there's a background compilation in
        // progress.
        std::unique_ptr<SharedModuleCacheCompiler> mCompiler;

        // Protocol versions to compile each contract for in the module cache.
        std::vector<uint32_t> mModuleCacheProtocols;

        // Number of threads to use for compilation (cached from config).
        size_t const mNumCompilationThreads;

        // In-memory map of live Soroban state for the current ledger.
        InMemorySorobanState mInMemorySorobanState;

        // Phase is not synchronized, should only be modified by the primary
        // apply thread.
        Phase mPhase{Phase::SETTING_UP_STATE};

        // Kicks off (on auxiliary threads) compilation of all contracts in the
        // provided snapshot, for ledger protocols starting at minLedgerVersion
        // and running through to Config::CURRENT_LEDGER_PROTOCOL_VERSION (to
        // enable upgrades).
        void startCompilingAllContracts(SearchableSnapshotConstPtr snap,
                                        uint32_t minLedgerVersion);

        // Checks if ApplyState can currently be modified. For functions that
        // are only called in ledgerClose, use the stronger
        // assertCommittingPhase. This is for functions that can be called on
        // the commit or init path.
        void assertWritablePhase() const;

      public:
        LedgerApplyMetrics& getMetrics();

        ApplyState(Application& app);

        // Asserts that calling thread is the primary apply thread.
        void threadInvariant() const;

        // The following methods are const getters, and can be accessed from any
        // thread for read-only purposes during the APPLYING phase.
        InMemorySorobanState const& getInMemorySorobanState() const;

#ifdef BUILD_TESTS
        InMemorySorobanState& getInMemorySorobanStateForTesting();
        ::rust::Box<rust_bridge::SorobanModuleCache> const&
        getModuleCacheForTesting();
        uint64_t getSorobanInMemoryStateSizeForTesting() const;
#endif

        std::shared_ptr<SorobanNetworkConfig const>
        getSorobanNetworkConfigForApply() const;

        ::rust::Box<rust_bridge::SorobanModuleCache> const&
        getModuleCache() const;

        bool isCompilationRunning() const;

        // Non-const mutating methods, must always be called from the primary
        // apply thread (either main or parallel apply thread) during the
        // COMMITTING phase.
        SorobanNetworkConfig& getSorobanNetworkConfigForCommit(
#ifdef BUILD_TESTS
            bool skipPhaseCheck = false
#endif
        );

        bool hasSorobanNetworkConfigForCommit() const;

        // Initializes an empty SorobanNetworkConfig if it's not already set.
        void maybeInitializeSorobanNetworkConfig();

        void
        updateInMemorySorobanState(std::vector<LedgerEntry> const& initEntries,
                                   std::vector<LedgerEntry> const& liveEntries,
                                   std::vector<LedgerKey> const& deadEntries,
                                   LedgerHeader const& lh);

        // Note: These are const getters, but should still only be called in the
        // COMMITTING phase.
        uint64_t getSorobanInMemoryStateSize() const;
        void reportInMemoryMetrics(SorobanMetrics& metrics) const;

        void manuallyAdvanceLedgerHeader(LedgerHeader const& lh);
        // Finishes a compilation started by `startCompilingAllContracts`.
        void finishPendingCompilation();

        // Equivalent to calling `startCompilingAllContracts` followed by
        // `finishPendingCompilation`.
        void compileAllContractsInLedger(SearchableSnapshotConstPtr snap,
                                         uint32_t minLedgerVersion);

        // Estimates the size of the arena underlying the module cache's shared
        // wasmi engine, from metrics, and rebuilds if it has likely built up a
        // lot of dead space inside of it.
        void maybeRebuildModuleCache(SearchableSnapshotConstPtr snap,
                                     uint32_t minLedgerVersion);

        // Evicts a single contract from the module cache, if it is present.
        // This should be done whenever a contract LE is evicted from the
        // live BL.
        void evictFromModuleCache(uint32_t ledgerVersion,
                                  EvictedStateVectors const& evictedState);

        // Adds all contracts in the provided set of LEs to the module cache.
        // This should be called as entries are added to the live bucketlist.
        void addAnyContractsToModuleCache(uint32_t ledgerVersion,
                                          std::vector<LedgerEntry> const& le);

        // Populates all live Soroban state into the cache from the provided
        // snapshot.
        void populateInMemorySorobanState(SearchableSnapshotConstPtr snap,
                                          uint32_t ledgerVersion);

        void handleUpgradeAffectingSorobanInMemoryStateSize(
            AbstractLedgerTxn& upgradeLtx);

        // Throws if current state is not READY_TO_APPLY, advances to APPLYING
        void markStartOfApplying();

        // Throws if current state is not APPLYING, advances to
        // COMMITTING
        void markStartOfCommitting();

        // Throws if current state is not COMMITTING, advances to READY_TO_APPLY
        void markEndOfCommitting();

        // Throws if current state is not SETTING_UP_STATE, advances to
        // READY_TO_APPLY
        void markEndOfSetupPhase();

        // Throws if current state is not READY_TO_APPLY, advances to
        // SETTING_UP_STATE.
        void resetToSetupPhase();

        void assertSetupPhase() const;
        void assertCommittingPhase() const;
    };

    // This state is private to the apply thread and holds work-in-progress
    // that gets accessed via the AppConnector, from inside transactions.
    ApplyState mApplyState;

    // Cached LCL state output from last apply (or loaded from DB on startup).
    CompleteConstLedgerStatePtr mLastClosedLedgerState;

    VirtualClock::time_point mLastClose;

    // Use mutex to guard ledger state during apply
    mutable std::recursive_mutex mLedgerStateMutex;

    medida::Timer& mCatchupDuration;

    std::unique_ptr<LedgerCloseMetaFrame> mNextMetaToEmit;

    // Use in the context of parallel ledger apply to indicate background thread
    // is currently closing a ledger or has ledgers queued to apply.
    bool mCurrentlyApplyingLedger{false};

    static std::vector<MutableTxResultPtr> processFeesSeqNums(
        ApplicableTxSetFrame const& txSet, AbstractLedgerTxn& ltxOuter,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        LedgerCloseData const& ledgerData);

    void processResultAndMeta(
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        uint32_t txIndex, TransactionMetaBuilder& txMetaBuilder,
        TransactionFrameBase const& tx,
        MutableTransactionResultBase const& result,
        TransactionResultSet& txResultSet);

    TransactionResultSet applyTransactions(
        ApplicableTxSetFrame const& txSet,
        std::vector<MutableTxResultPtr> const& mutableTxResults,
        AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta);

    void
    applyParallelPhase(TxSetPhaseFrame const& phase,
                       std::vector<ApplyStage>& applyStages,
                       std::vector<MutableTxResultPtr> const& mutableTxResults,
                       uint32_t& index, AbstractLedgerTxn& ltx,
                       bool enableTxMeta, Hash const& sorobanBasePrngSeed);

    void applySequentialPhase(
        TxSetPhaseFrame const& phase,
        std::vector<MutableTxResultPtr> const& mutableTxResults,
        uint32_t& index, AbstractLedgerTxn& ltx, bool enableTxMeta,
        Hash const& sorobanBasePrngSeed,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        TransactionResultSet& txResultSet);

    void processPostTxSetApply(
        std::vector<TxSetPhaseFrame> const& phases,
        std::vector<ApplyStage> const& applyStages, AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        TransactionResultSet& txResultSet);

    std::unique_ptr<ThreadParallelApplyLedgerState>
    applyThread(AppConnector& app,
                std::unique_ptr<ThreadParallelApplyLedgerState> threadState,
                Cluster const& cluster, Config const& config,
                SorobanNetworkConfig const& sorobanConfig,
                ParallelLedgerInfo ledgerInfo, Hash sorobanBasePrngSeed);

    std::vector<std::unique_ptr<ThreadParallelApplyLedgerState>>
    applySorobanStageClustersInParallel(
        AppConnector& app, ApplyStage const& stage,
        GlobalParallelApplyLedgerState const& globalState,
        Hash const& sorobanBasePrngSeed, Config const& config,
        SorobanNetworkConfig const& sorobanConfig,
        ParallelLedgerInfo const& ledgerInfo);

    void checkAllTxBundleInvariants(AppConnector& app, ApplyStage const& stage,
                                    Config const& config,
                                    ParallelLedgerInfo const& ledgerInfo,
                                    LedgerHeader const& header);

    void applySorobanStage(AppConnector& app, LedgerHeader const& header,
                           GlobalParallelApplyLedgerState& globalParState,
                           ApplyStage const& stage,
                           Hash const& sorobanBasePrngSeed);

    void applySorobanStages(AppConnector& app, AbstractLedgerTxn& ltx,
                            std::vector<ApplyStage> const& stages,
                            Hash const& sorobanBasePrngSeed);

    // initialLedgerVers must be the ledger version at the start of the ledger.
    // On the ledger in which a protocol upgrade from vN to vN + 1 occurs,
    // initialLedgerVers must be vN.
    CompleteConstLedgerStatePtr sealLedgerTxnAndStoreInBucketsAndDB(
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
    std::optional<LedgerCloseMetaFrame> mLastLedgerCloseMeta;
#endif

    void setState(State s);

    void emitNextMeta();

    // Publishes soroban metrics, including select network config limits as well
    // as the actual ledger usage.
    void publishSorobanMetrics();

    // Update cached last closed ledger state values managed by this class.
    void
    advanceLastClosedLedgerState(CompleteConstLedgerStatePtr newLedgerState);

    // Helper function to conditionally load Soroban network config based on
    // protocol version. Populates both the apply state and the LCL state.
    void maybeLoadSorobanNetworkConfig(uint32_t ledgerVersion);

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
    CompleteConstLedgerStatePtr
    advanceBucketListSnapshotAndMakeLedgerState(LedgerHeader const& header,
                                                HistoryArchiveState const& has);
    void logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                           size_t numOps);

    // Friend declaration for App LedgerTxnRoot initialization
    friend void ApplicationImpl::initialize(bool createNewDB,
                                            bool forceRebuild);

    std::unique_ptr<LedgerTxnRoot>
    createLedgerTxnRoot(Application& app, size_t entryCacheSize,
                        size_t prefetchBatchSize
#ifdef BEST_OFFER_DEBUGGING
                        ,
                        bool bestOfferDebuggingEnabled
#endif
                        ) override;

  public:
    LedgerManagerImpl(Application& app);

    // Reloads the network configuration from the ledger.
    // This needs to be called every time a ledger is closed.
    // This call is read-only and hence `ltx` can be read-only.
    void updateSorobanNetworkConfigForCommit(AbstractLedgerTxn& ltx) override;
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
    SorobanNetworkConfig const&
    getLastClosedSorobanNetworkConfig() const override;
    SorobanNetworkConfig const& getSorobanNetworkConfigForApply() override;

    bool hasLastClosedSorobanNetworkConfig() const override;
    std::chrono::milliseconds getExpectedLedgerCloseTime() const override;

#ifdef BUILD_TESTS
    SorobanNetworkConfig& getMutableSorobanNetworkConfigForApply() override;
    void mutateSorobanNetworkConfigForApply(
        std::function<void(SorobanNetworkConfig&)> const& f) override;
    std::vector<TransactionMetaFrame> const&
    getLastClosedLedgerTxMeta() override;
    std::optional<LedgerCloseMetaFrame> const&
    getLastClosedLedgerCloseMeta() override;
    TransactionResultSet mLatestTxResultSet{};
    void storeCurrentLedgerForTest(LedgerHeader const& header) override;
    std::function<void()> mAdvanceLedgerStateAndPublishOverride;
    InMemorySorobanState const& getInMemorySorobanStateForTesting() override;
    ::rust::Box<rust_bridge::SorobanModuleCache>
    getModuleCacheForTesting() override;
    void rebuildInMemorySorobanStateForTesting(uint32_t ledgerVersion) override;
    uint64_t getSorobanInMemoryStateSizeForTesting() override;
#endif

    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader const& genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(bool restoreBucketlist) override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;

    HistoryArchiveState getLastClosedLedgerHAS() const override;

    Database& getDatabase() override;

    void startCatchup(CatchupConfiguration configuration,
                      std::shared_ptr<HistoryArchive> archive) override;

    void applyLedger(LedgerCloseData const& ledgerData,
                     bool calledViaExternalize) override;
    void
    advanceLedgerStateAndPublish(uint32_t ledgerSeq, bool calledViaExternalize,
                                 LedgerCloseData const& ledgerData,
                                 CompleteConstLedgerStatePtr newLedgerState,
                                 bool queueRebuildNeeded) override;
    void ledgerCloseComplete(uint32_t lcl, bool calledViaExternalize,
                             LedgerCloseData const& ledgerData,
                             bool queueRebuildNeeded);
    void
    setLastClosedLedger(LedgerHeaderHistoryEntry const& lastClosed) override;

    void manuallyAdvanceLedgerHeader(LedgerHeader const& header) override;

    void setupLedgerCloseMetaStream();
    void maybeResetLedgerCloseMetaDebugStream(uint32_t ledgerSeq);

    SorobanMetrics& getSorobanMetrics() override;
    SearchableSnapshotConstPtr getLastClosedSnaphot() const override;
    virtual bool
    isApplying() const override
    {
        return mCurrentlyApplyingLedger;
    }
    void markApplyStateReset() override;
    ::rust::Box<rust_bridge::SorobanModuleCache> getModuleCache() override;

    void handleUpgradeAffectingSorobanInMemoryStateSize(
        AbstractLedgerTxn& upgradeLtx) override;

    void
    assertSetupPhase() const override
    {
        mApplyState.assertSetupPhase();
    }

#ifdef BUILD_TESTS
    friend class BucketTestUtils::LedgerManagerForBucketTests;
#endif
};
}
