#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#include "util/asio.h"

#include "history/HistoryManager.h"
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
class LedgerHeaderReference;

class LedgerManagerImpl : public LedgerManager
{
    LedgerHeaderHistoryEntry mLastClosedLedger;

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
                            LedgerState& ls);
    void applyTransactions(std::vector<TransactionFramePtr>& txs,
                           LedgerState& ls,
                           TransactionResultSet& txResultSet);

    void ledgerClosed(LedgerState& ls);
    void storeCurrentLedger(std::shared_ptr<LedgerHeaderReference> header);
    void advanceLedgerPointers(std::shared_ptr<LedgerHeaderReference> header);

    void storeHeaderInDatabase(LedgerHeader const& header);

    State mState;

  public:
    LedgerManagerImpl(Application& app);

    void setState(State s) override;
    State getState() const override;
    std::string getStateHuman() const override;

    void valueExternalized(LedgerCloseData const& ledgerData) override;

    uint32_t getLastClosedLedgerNum() const override;
    uint64_t secondsSinceLastLedgerClose() const override;
    void syncMetrics() override;

    void startNewLedger(LedgerHeader genesisLedger);
    void startNewLedger() override;
    void loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;

    Database& getDatabase() override;

    void startCatchUp(CatchupConfiguration configuration,
                      bool manualCatchup) override;

    HistoryManager::LedgerVerificationStatus
    verifyCatchupCandidate(LedgerHeaderHistoryEntry const&,
                           bool manualCatchup) const override;
    void closeLedger(LedgerCloseData const& ledgerData) override;
    void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                          uint32_t count) override;
};
}
