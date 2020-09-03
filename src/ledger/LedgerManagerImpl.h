#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "history/HistoryManager.h"
#include "ledger/LedgerManager.h"
#include "main/PersistentState.h"
#include "transactions/TransactionFrame.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger.h"
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

class LedgerManagerImpl : public LedgerManager
{
    LedgerHeaderHistoryEntry mLastClosedLedger;

  protected:
    Application& mApp;
    std::unique_ptr<XDROutputFileStream> mMetaStream;

  private:
    medida::Timer& mTransactionApply;
    medida::Histogram& mTransactionCount;
    medida::Histogram& mOperationCount;
    medida::Histogram& mPrefetchHitRate;
    medida::Timer& mLedgerClose;
    medida::Buckets& mLedgerAgeClosed;
    medida::Counter& mLedgerAge;
    VirtualClock::time_point mLastClose;

    std::unique_ptr<VirtualClock::time_point> mStartCatchup;
    medida::Timer& mCatchupDuration;

    void
    processFeesSeqNums(std::vector<TransactionFrameBasePtr>& txs,
                       AbstractLedgerTxn& ltxOuter, int64_t baseFee,
                       std::unique_ptr<LedgerCloseMeta> const& ledgerCloseMeta);

    void
    applyTransactions(std::vector<TransactionFrameBasePtr>& txs,
                      AbstractLedgerTxn& ltx, TransactionResultSet& txResultSet,
                      std::unique_ptr<LedgerCloseMeta> const& ledgerCloseMeta);

    void ledgerClosed(AbstractLedgerTxn& ltx);

    void storeCurrentLedger(LedgerHeader const& header);
    void prefetchTransactionData(std::vector<TransactionFrameBasePtr>& txs);
    void prefetchTxSourceIds(std::vector<TransactionFrameBasePtr>& txs);
    void closeLedgerIf(LedgerCloseData const& ledgerData);

    State mState;
    void setState(State s);

  protected:
    virtual void transferLedgerEntriesToBucketList(AbstractLedgerTxn& ltx,
                                                   uint32_t ledgerSeq,
                                                   uint32_t ledgerVers);

    void advanceLedgerPointers(LedgerHeader const& header,
                               bool debugLog = true);
    void logTxApplyMetrics(AbstractLedgerTxn& ltx, size_t numTxs,
                           size_t numOps);

  public:
    LedgerManagerImpl(Application& app);

    void moveToSynced() override;
    State getState() const override;
    std::string getStateHuman() const override;

    void valueExternalized(LedgerCloseData const& ledgerData) override;

    uint32_t getLastMaxTxSetSize() const override;
    uint32_t getLastMaxTxSetSizeOps() const override;
    int64_t getLastMinBalance(uint32_t ownerCount) const override;
    uint32_t getLastReserve() const override;
    uint32_t getLastTxFee() const override;

    uint32_t getLastClosedLedgerNum() const override;
    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader const& genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;

    HistoryArchiveState getLastClosedLedgerHAS() override;

    Database& getDatabase() override;

    void startCatchup(CatchupConfiguration configuration,
                      std::shared_ptr<HistoryArchive> archive) override;

    void closeLedger(LedgerCloseData const& ledgerData) override;
    void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                          uint32_t count) override;

    void
    setLastClosedLedger(LedgerHeaderHistoryEntry const& lastClosed) override;

    void manuallyAdvanceLedgerHeader(LedgerHeader const& header) override;

    void setupLedgerCloseMetaStream();
};
}
