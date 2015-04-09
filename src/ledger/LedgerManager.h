#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxSetFrame.h"
#include "history/HistoryManager.h"
#include <memory>

namespace stellar
{

class TxSetFrame;
typedef std::shared_ptr<TxSetFrame> TxSetFramePtr;
class LedgerHeaderFrame;
class Database;

/**
 * Helper class that describes a single ledger-to-close -- a set of transactions
 * and auxiliary values -- as decided by the Herder (and ultimately: SCP). This
 * does not include the effects of _performing_ any transactions, merely the
 * values that the network has agreed _to apply_ to the current ledger,
 * atomically, in order to produce the next ledger.
 */
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

/**
 * LedgerManager maintains, in memory, a logical pair of ledgers:
 *
 *     1. The "current ledger", to which the transactions currently accumulating
 *        in memory (in the Herder) will be applied on the next ledger-close.
 *
 *     2. The "last-closed ledger" (LCL), which is the result of the most recent
 *        ledger-close. This ledger is the state most recently committed to the
 *        SQL store, is immutable, and is the basis for the transactions applied
 *        in "current". In other words, "current" begins as a copy of LCL and
 *        then is mutated by the transactions applied to it.
 *
 * LedgerManager also coordinates the incremental advance of these ledgers: the
 * process of closing ledgers, turning the current ledger into the next LCL, and
 * producing a new current ledger.
 *
 * Finally, LedgerManager triggers and responds to boundary conditions in the
 * life cycle of the application that "suddenly" alter the notions of "current"
 * and "LCL": process startup, desynchronization and resynchronization of the
 * current process with the rest of the network.
 */
class LedgerManager
{
  public:
    enum State
    {
        // Loading state from database, not yet active
        LM_BOOTING_STATE,

        // local state is in sync with view of consensus coming from herder
        // desynchronization will cause transition to CATCHING_UP_STATE.
        LM_SYNCED_STATE,

        // local state doesn't match view of consensus from herder
        // catchup is in progress
        LM_CATCHING_UP_STATE,

        LM_NUM_STATE
    };

    virtual void setState(State s) = 0;
    virtual State getState() const = 0;
    virtual std::string getStateHuman() const = 0;

    bool isSynced() const
    {
        return getState() == LM_SYNCED_STATE;
    }

    // Logging helpers, return strings describing the provided ledgers.
    static std::string ledgerAbbrev(uint32_t seq, uint256 const& hash);
    static std::string ledgerAbbrev(LedgerHeader const& header,
                                    uint256 const& hash);
    static std::string ledgerAbbrev(std::shared_ptr<LedgerHeaderFrame> p);
    static std::string ledgerAbbrev(LedgerHeaderHistoryEntry he);

    // Factory
    static std::unique_ptr<LedgerManager> create(Application& app);

    // Called by Herder to inform LedgerManager that a SCP has agreed on a new
    // close event. This is the most common cause of LedgerManager advancing
    // from one ledger to the next: the network reached consensus on
    // `ledgerData`.
    virtual void externalizeValue(LedgerCloseData ledgerData) = 0;

    // Return the current ledger header.
    virtual LedgerHeader const& getCurrentLedgerHeader() const = 0;

    // Return the LCL header and (complete, immutable) hash.
    virtual LedgerHeaderHistoryEntry const&
    getLastClosedLedgerHeader() const = 0;

    // Return the sequence number of the current ledger.
    virtual uint32_t getLedgerNum() const = 0;

    // Return the sequence number of the LCL.
    virtual uint32_t getLastClosedLedgerNum() const = 0;

    // Return the minimum balance required to establish, in the current ledger,
    // a new ledger entry with `ownerCount` owned objects.  Derived from the
    // current ledger's `baseReserve` value.
    virtual int64_t getMinBalance(uint32_t ownerCount) const = 0;

    // Return the close time of the current ledger, in seconds since the POSIX
    // epoch.
    virtual uint64_t getCloseTime() const = 0;

    // Return the fee required to apply a transaction to the current ledger. The
    // current ledger's baseFee is a 32bit value in stroops, but it is returned
    // as a 64bit value here to minimize the chance of overflow in a subsequent
    // arithmetic operation.
    virtual int64_t getTxFee() const = 0;

    // Return the (changing) number of seconds since the LCL closed.
    virtual uint64_t secondsSinceLastLedgerClose() const = 0;

    // Return a mutable reference to the current ledger header; this is used
    // solely by LedgerDelta to _modify_ the current ledger-in-progress.
    virtual LedgerHeader& getCurrentLedgerHeader() = 0;

    virtual Database& getDatabase() = 0;

    // Called by application lifecycle events, system startup.
    virtual void startNewLedger() = 0;
    virtual void loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) = 0;

    // Forcibly switch the application into catchup mode, treating `initLedger`
    // as the current ledger number (to begin catchup from). Normally this
    // happens automatically when LedgerManager detects it is desynchronized
    // from SCP's consensus ledger; this methos is present in the public
    // interface
    // to permit testing.
    virtual void startCatchUp(uint32_t initLedger,
                              HistoryManager::CatchupMode resume) = 0;

    // Called by the history subsystem during catchup: this method asks the
    // LedgerManager whether or not the HistoryManager should trust (thus: begin
    // applying history that terminates in) a candidate LCL.
    //
    // The LedgerManager consults a local buffer in which it accumulates SCP
    // consensus results during catchup, and returns VERIFY_HASH_OK if the
    // proposed ledger is a (trusted) member of that consensus buffer;
    // VERIFY_HASH_BAD if a buffered consensus ledger exists with the same
    // sequence number but _different_ hash; and VERIFY_HASH_UNKNOWN if no
    // ledger has been received from SCP yet with the proposed ledger sequence
    // number.
    virtual HistoryManager::VerifyHashStatus
    verifyCatchupCandidate(LedgerHeaderHistoryEntry const&) const = 0;

    // Forcibly close the current ledger, applying `ledgerData` as the consensus
    // changes.  This is normally done automatically as part of
    // `externalizeValue()`; this method is present in the public interface to
    // permit testing.
    virtual void closeLedger(LedgerCloseData ledgerData) = 0;

    virtual ~LedgerManager()
    {
    }
};
}
