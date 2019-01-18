#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupManager.h"
#include "history/HistoryManager.h"
#include <memory>

namespace stellar
{

class LedgerCloseData;
class Database;

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
    static const uint32_t GENESIS_LEDGER_SEQ;
    static const uint32_t GENESIS_LEDGER_VERSION;
    static const uint32_t GENESIS_LEDGER_BASE_FEE;
    static const uint32_t GENESIS_LEDGER_BASE_RESERVE;
    static const uint32_t GENESIS_LEDGER_MAX_TX_SIZE;
    static const int64_t GENESIS_LEDGER_TOTAL_COINS;

    enum State
    {
        // Loading state from database, not yet active
        LM_BOOTING_STATE,

        // local state is in sync with view of consensus coming from herder
        // desynchronization will cause transition to LM_BOOTING_STATE.
        LM_SYNCED_STATE,

        // local state doesn't match view of consensus from herder
        // catchup is in progress
        LM_CATCHING_UP_STATE,

        LM_NUM_STATE
    };

    enum class CatchupState
    {
        NONE,
        WAITING_FOR_TRIGGER_LEDGER,
        APPLYING_HISTORY,
        APPLYING_BUFFERED_LEDGERS,
        WAITING_FOR_CLOSING_LEDGER
    };

    virtual void bootstrap() = 0;
    virtual State getState() const = 0;
    virtual CatchupState getCatchupState() const = 0;
    virtual std::string getStateHuman() const = 0;

    bool
    isSynced() const
    {
        return getState() == LM_SYNCED_STATE;
    }

    // Logging helpers, return strings describing the provided ledgers.
    static std::string ledgerAbbrev(uint32_t seq, uint256 const& hash);
    static std::string ledgerAbbrev(LedgerHeader const& header);
    static std::string ledgerAbbrev(LedgerHeader const& header,
                                    uint256 const& hash);
    static std::string ledgerAbbrev(LedgerHeaderHistoryEntry he);

    // Factory
    static std::unique_ptr<LedgerManager> create(Application& app);

    // Genesis ledger
    static LedgerHeader genesisLedger();

    // Called by Herder to inform LedgerManager that a SCP has agreed on a new
    // close event. This is the most common cause of LedgerManager advancing
    // from one ledger to the next: the network reached consensus on
    // `ledgerData`.
    virtual void valueExternalized(LedgerCloseData const& ledgerData) = 0;

    // Return the LCL header and (complete, immutable) hash.
    virtual LedgerHeaderHistoryEntry const&
    getLastClosedLedgerHeader() const = 0;

    // return the HAS that corresponds to the last closed ledger as persisted in
    // the database
    virtual HistoryArchiveState getLastClosedLedgerHAS() = 0;

    // Return the sequence number of the LCL.
    virtual uint32_t getLastClosedLedgerNum() const = 0;

    // Return the minimum balance required to establish, in the current ledger,
    // a new ledger entry with `ownerCount` owned objects.  Derived from the
    // current ledger's `baseReserve` value.
    virtual int64_t getLastMinBalance(uint32_t ownerCount) const = 0;

    virtual uint32_t getLastReserve() const = 0;

    // Return the fee required to apply a transaction to the current ledger.
    virtual uint32_t getLastTxFee() const = 0;

    // return the maximum size of a transaction set to apply to the current
    // ledger
    virtual uint32_t getLastMaxTxSetSize() const = 0;

    // Return the (changing) number of seconds since the LCL closed.
    virtual uint64_t secondsSinceLastLedgerClose() const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual Database& getDatabase() = 0;

    // Called by application lifecycle events, system startup.
    virtual void startNewLedger() = 0;

    // loads the last ledger information from the database
    // if handler is set, also loads bucket information and invokes handler.
    virtual void loadLastKnownLedger(
        std::function<void(asio::error_code const& ec)> handler) = 0;

    // Forcibly switch the application into catchup mode, treating `toLedger`
    // as the destination ledger number and count as the number of past ledgers
    // that should be replayed. Normally this happens automatically when
    // LedgerManager detects it is desynchronized from SCP's consensus ledger.
    // This method is present in the public interface to permit testing and
    // command line catchups.
    virtual void startCatchup(CatchupConfiguration configuration,
                              bool manualCatchup) = 0;

    // Forcibly close the current ledger, applying `ledgerData` as the consensus
    // changes.  This is normally done automatically as part of
    // `valueExternalized()`; this method is present in the public interface to
    // permit testing.
    virtual void closeLedger(LedgerCloseData const& ledgerData) = 0;

    // deletes old entries stored in the database
    virtual void deleteOldEntries(Database& db, uint32_t ledgerSeq,
                                  uint32_t count) = 0;

    virtual ~LedgerManager()
    {
    }
};
}
