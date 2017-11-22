#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "history/HistoryManager.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "ledger/SyncingLedgerChain.h"
#include "main/PersistentState.h"
#include "transactions/TransactionFrame.h"
#include "util/Timer.h"
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
}

namespace stellar
{
class Application;
class Database;
class LedgerDelta;

class LedgerManagerImpl : public LedgerManager
{
    LedgerHeaderHistoryEntry mLastClosedLedger;
    LedgerHeaderFrame::pointer mCurrentLedger;

    Application& mApp;
    medida::Timer& mTransactionApply;
    medida::Timer& mLedgerClose;
    medida::Timer& mLedgerAgeClosed;
    medida::Counter& mLedgerAge;
    medida::Counter& mLedgerStateCurrent;
    medida::Timer& mLedgerStateChanges;
    VirtualClock::time_point mLastClose;
    VirtualClock::time_point mLastStateChange;

    medida::Counter& mSyncingLedgersSize;

    SyncingLedgerChain mSyncingLedgers;

    void historyCaughtup(asio::error_code const& ec,
                         CatchupWork::ProgressState progressState,
                         LedgerHeaderHistoryEntry const& lastClosed);

    void processFeesSeqNums(std::vector<TransactionFramePtr>& txs,
                            LedgerDelta& delta);
    void applyTransactions(std::vector<TransactionFramePtr>& txs,
                           LedgerDelta& ledgerDelta,
                           TransactionResultSet& txResultSet);

    void ledgerClosed(LedgerDelta const& delta);
    void storeCurrentLedger();
    void advanceLedgerPointers();

    State mState;

  public:
    LedgerManagerImpl(Application& app);

    void setState(State s) override;
    State getState() const override;
    std::string getStateHuman() const override;

    void valueExternalized(LedgerCloseData const& ledgerData) override;

    uint32_t getLedgerNum() const override;
    uint32_t getLastClosedLedgerNum() const override;
    int64_t getMinBalance(uint32_t ownerCount) const override;
    uint32_t getTxFee() const override;
    uint32_t getMaxTxSetSize() const override;
    uint64_t getCloseTime() const override;
    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;
    LedgerHeader const& getCurrentLedgerHeader() const override;
    LedgerHeader& getCurrentLedgerHeader() override;
    uint32_t getCurrentLedgerVersion() const override;

    Database& getDatabase() override;

    void startCatchUp(CatchupConfiguration configuration,
                      bool manualCatchup) override;

    HistoryManager::VerifyHashStatus
    verifyCatchupCandidate(LedgerHeaderHistoryEntry const&,
                           bool manualCatchup) const override;
    void closeLedger(LedgerCloseData const& ledgerData) override;
    void deleteOldEntries(Database& db, uint32_t ledgerSeq) override;
    void checkDbState() override;
};
}
