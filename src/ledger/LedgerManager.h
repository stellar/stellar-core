#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the ISC License. See the COPYING file at the top-level directory of
// this distribution or at http://opensource.org/licenses/ISC

#include "herder/TxSetFrame.h"
#include "history/HistoryManager.h"
#include <memory>

namespace stellar
{

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;
class LedgerHeaderFrame;
class Database;

class LedgerCloseData
{
  public:
    uint32_t mLedgerSeq;
    TxSetFramePtr mTxSet;
    uint64_t mCloseTime;
    int32_t mBaseFee;

    LedgerCloseData(uint32_t ledgerSeq, TxSetFramePtr txSet, uint64_t closeTime,
                    int32_t baseFee)
        : mLedgerSeq(ledgerSeq)
        , mTxSet(txSet)
        , mCloseTime(closeTime)
        , mBaseFee(baseFee)
    {
    }
};

/*
 * Public Interface to the Ledger Module
 */
class LedgerManager
{
  public:

    // Logging helpers
    static std::string ledgerAbbrev(LedgerHeader const& header,
                                    uint256 const& hash);
    static std::string ledgerAbbrev(std::shared_ptr<LedgerHeaderFrame> p);
    static std::string ledgerAbbrev(LedgerHeaderHistoryEntry he);

    // Factory
    static std::unique_ptr<LedgerManager> create(Application& app);

    // Called by txherder to inform LM that a SCP has agreed on a new close event.
    virtual void externalizeValue(LedgerCloseData ledgerData) = 0;

    // Query methods called by several subsystems tracking state of LM.
    virtual LedgerHeader const& getCurrentLedgerHeader() const = 0;
    virtual LedgerHeaderHistoryEntry const& getLastClosedLedgerHeader() const = 0;
    virtual uint32_t getLedgerNum() const = 0;
    virtual uint32_t getLastClosedLedgerNum() const = 0;
    virtual int64_t getMinBalance(uint32_t ownerCount) const = 0;
    virtual uint64_t getCloseTime() const = 0;
    virtual int64_t getTxFee() const = 0; // i32 promoted to i64 to avoid overflow
    virtual uint64_t secondsSinceLastLedgerClose() const = 0;

    // Used solely by LedgerDelta to _modify_ the current ledger-in-progress.
    virtual LedgerHeader& getCurrentLedgerHeader() = 0;

    virtual Database& getDatabase() = 0;

    // Called by application lifecycle events, system startup.
    virtual void startNewLedger() = 0;
    virtual void loadLastKnownLedger() = 0;

    // Called by history modules that coordinate closely with LedgerManager when replaying.
    virtual void startCatchUp(uint32_t initLedger, HistoryManager::ResumeMode resume) = 0;
    virtual HistoryManager::VerifyHashStatus verifyCatchupCandidate(LedgerHeaderHistoryEntry const&) const = 0;
    virtual void closeLedger(LedgerCloseData ledgerData) = 0;

    virtual ~LedgerManager() {}
};
}
