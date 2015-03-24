#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC
#include "util/asio.h"

#include <string>
#include "ledger/LedgerManager.h"
#include "ledger/LedgerHeaderFrame.h"
#include "main/PersistentState.h"
#include "history/HistoryManager.h"
#include "generated/Stellar-ledger.h"

/*
Holds the current ledger
Applies the tx set to the last ledger to get the next one
Hands the old ledger off to the history
*/

namespace medida
{
class Timer;
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

    uint64_t mLastCloseTime;

    std::vector<LedgerCloseData> mSyncingLedgers;

    void historyCaughtup(asio::error_code const& ec,
                         HistoryManager::ResumeMode mode,
                         LedgerHeaderHistoryEntry const& lastClosed);

    void closeLedgerHelper(LedgerDelta const& delta);
    void advanceLedgerPointers();

  public:

    LedgerManagerImpl(Application& app);

    void externalizeValue(LedgerCloseData ledgerData) override;

    uint32_t getLedgerNum() const override;
    uint32_t getLastClosedLedgerNum() const override;
    int64_t getMinBalance(uint32_t ownerCount) const override;
    int64_t getTxFee() const override;
    uint64_t getCloseTime() const override;
    uint64_t secondsSinceLastLedgerClose() const override;

    void startNewLedger() override;
    void loadLastKnownLedger() override;

    LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const override;
    LedgerHeader const& getCurrentLedgerHeader() const override;
    LedgerHeader& getCurrentLedgerHeader() override;

    Database& getDatabase() override;

    void startCatchUp(uint32_t initLedger, HistoryManager::ResumeMode resume) override;
    HistoryManager::VerifyHashStatus verifyCatchupCandidate(LedgerHeaderHistoryEntry const&) const override;
    void closeLedger(LedgerCloseData ledgerData) override;
};
}
