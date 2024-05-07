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
    LedgerHeaderHistoryEntry mLastClosedLedger;
    std::optional<SorobanNetworkConfig> mSorobanNetworkConfig;

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
    VirtualClock::time_point mLastClose;
    bool mRebuildInMemoryState{false};

    std::unique_ptr<VirtualClock::time_point> mStartCatchup;
    medida::Timer& mCatchupDuration;

    std::unique_ptr<LedgerCloseMetaFrame> mNextMetaToEmit;

    std::vector<TransactionResultPayloadPtr> processFeesSeqNums(
        std::vector<TransactionFrameBasePtr> const& txs,
        AbstractLedgerTxn& ltxOuter, ApplicableTxSetFrame const& txSet,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta);

    void applyTransactions(
        ApplicableTxSetFrame const& txSet,
        std::vector<TransactionFrameBasePtr> const& txs,
        std::vector<TransactionResultPayloadPtr> const& txResults,
        AbstractLedgerTxn& ltx, TransactionResultSet& txResultSet,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta);

    // initialLedgerVers must be the ledger version at the start of the ledger.
    // On the ledger in which a protocol upgrade from vN to vN + 1 occurs,
    // initialLedgerVers must be vN.
    void
    ledgerClosed(AbstractLedgerTxn& ltx,
                 std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
                 uint32_t initialLedgerVers);

    void storeCurrentLedger(LedgerHeader const& header, bool storeHeader);
    void
    prefetchTransactionData(std::vector<TransactionFrameBasePtr> const& txs);
    void prefetchTxSourceIds(std::vector<TransactionFrameBasePtr> const& txs);
    void closeLedgerIf(LedgerCloseData const& ledgerData);

    State mState;
    void setState(State s);

    void emitNextMeta();

    SorobanNetworkConfig& getSorobanNetworkConfigInternal();

    // Publishes soroban metrics, including select network config limits as well
    // as the actual ledger usage.
    void publishSorobanMetrics();

  protected:
    // initialLedgerVers must be the ledger version at the start of the ledger
    // and currLedgerVers is the ledger version in the current ltx header. These
    // values are the same except on the ledger in which a protocol upgrade from
    // vN to vN + 1 occurs. initialLedgerVers must be vN and currLedgerVers must
    // be vN + 1.
    virtual void transferLedgerEntriesToBucketList(
        AbstractLedgerTxn& ltx,
        std::unique_ptr<LedgerCloseMetaFrame> const& ledgerCloseMeta,
        uint32_t ledgerSeq, uint32_t currLedgerVers,
        uint32_t initialLedgerVers);

    void advanceLedgerPointers(LedgerHeader const& header,
                               bool debugLog = true);
    void logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                           size_t numOps);

  public:
    LedgerManagerImpl(Application& app);

    // Reloads the network configuration from the ledger.
    // This needs to be called every time a ledger is closed.
    // This call is read-only and hence `ltx` can be read-only.
    void updateNetworkConfig(AbstractLedgerTxn& ltx) override;
    void moveToSynced() override;
    State getState() const override;
    std::string getStateHuman() const override;

    void valueExternalized(LedgerCloseData const& ledgerData) override;

    uint32_t getLastMaxTxSetSize() const override;
    uint32_t getLastMaxTxSetSizeOps() const override;
    Resource maxLedgerResources(bool isSoroban) override;
    Resource maxSorobanTransactionResources() override;
    int64_t getLastMinBalance(uint32_t ownerCount) const override;
    uint32_t getLastReserve() const override;
    uint32_t getLastTxFee() const override;
    uint32_t getLastClosedLedgerNum() const override;
    SorobanNetworkConfig const& getSorobanNetworkConfig() override;
    bool hasSorobanNetworkConfig() const override;

#ifdef BUILD_TESTS
    SorobanNetworkConfig& getMutableSorobanNetworkConfig() override;
#endif

    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader const& genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(bool restoreBucketlist,
                             bool isLedgerStateReady) override;
    virtual bool rebuildingInMemoryState() override;
    virtual void setupInMemoryStateRebuild() override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;

    HistoryArchiveState getLastClosedLedgerHAS() override;

    Database& getDatabase() override;

    void
    startCatchup(CatchupConfiguration configuration,
                 std::shared_ptr<HistoryArchive> archive,
                 std::set<std::shared_ptr<Bucket>> bucketsToRetain) override;

    void closeLedger(LedgerCloseData const& ledgerData) override;
    void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                          uint32_t count) override;

    void deleteNewerEntries(Database& db, uint32_t ledgerSeq) override;

    void setLastClosedLedger(LedgerHeaderHistoryEntry const& lastClosed,
                             bool storeInDB) override;

    void manuallyAdvanceLedgerHeader(LedgerHeader const& header) override;

    void setupLedgerCloseMetaStream();
    void maybeResetLedgerCloseMetaDebugStream(uint32_t ledgerSeq);

    SorobanMetrics& getSorobanMetrics() override;
};
}
