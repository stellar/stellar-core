#pragma once

// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/LedgerApplyManager.h"
#include "history/HistoryManager.h"
#include "ledger/NetworkConfig.h"
#include "rust/RustBridge.h"
#include <memory>

namespace stellar
{

class LedgerCloseData;
class Database;
class SorobanMetrics;

// This diagram provides a schematic of the flow of (logical) ledgers coming in
// from the SCP-and-Herder consensus complex, passing through the
// LedgerApplyManager (LAM) and LedgerManager (LM), and finally coming to rest
// in the "last closed ledger" (LCL) variables in the LM. These will
// subsequently be published to history but that's not relevant to this diagram.
//
// A "logical" ledger takes on more structure as it flows through the system. It
// begins as LedgerCloseData, which is just a txset and hash reference to a
// ledger, coming out of SCP. But during apply it is transformed into an
// ApplyLedgerOutput, which carries with it a full snapshot of the bucketlist.
//
// The tricky part of this flow involves the fact that work is being done by
// _two threads_: on the left there is work by the main thread, and on the right
// the apply thread. Logical ledgers begin on the main thread, are transferred
// over to the apply thread for the computationally intensive act of applying,
// and are then transferred _back_ to the main thread (with a new bucketlist
// snapshot) in order to update the LCL variables in the LM.
//
//                      ┌────────────────────┐
//             ┌───┐    │      Herder        │   ┌─────┐
//             │   │    │                    ├──►│     │
//             │   │    │  Feeds SCP module, │◄──┤ SCP │
//             │   │    │  emits consensus   │   │     │
//             │   │    └──┬─────────────────┘   └─────┘
//             │   │       │
//             │   │       │
//             │   │       │
//        │    │   │    ┌──┼───────────────────────────┐
//        │    │   │    │  │   LedgerApplyManager      │
//        │    │   │    │  │                           │
//        │    │   │    │  │  Assembles contiguous     │
//      T │    │ M │    │  │  ledger sequence and      │
//      i │    │ a │    │  │  feeds to apply thread    │
//      m │    │ i │    │  │  (or triggers catchup)    │
//      e │    │ n │    │  │                           │
//        │    │   │    │  │ ┌───────────────────┐     │    ┌───┐
//      p │    │ T │    │  ▼ │  mSyncingLedgers  │─────┼───►│   │
//      a │    │ h │    │    └───────────────────┘     │    │   │
//      s │    │ r │    │   H                     Q    │    │   │
//      s │    │ e │    └──────────────────────────────┘    │   │
//      i │    │ a │                                        │ A │
//      n │    │ d │                                        │ p │
//      g │    │   │    ┌──────────────────────┐            │ p │
//        │    │   │    │                      │            │ l │
//        │    │   │    │                      │            │ y │
//        │    │   │    │                      │            │   │
//        │    │   │    │    LedgerManager     │         A  │ T │
//        │    │   │    │                      │            │ h │
//        │    │   │    │ State and logic for  │            │ r │
//        ▼    │   │    │ apply thread and     │            │ e │
//             │   │    │ close, including     │            │ a │
//             │   │    │ storing snapshot of  │            │ d │
//             │   │    │ "last closed ledger" │            │   │
//             │   │    │                      │            │   │
//             │   │    │             LCL ◄────┼────────────┤   │
//             │   │    │                      │            └──┬┘
//             │   │    │                      │             ▲ ▼
//             └───┘    │ mLastClosedLedger    │      ┌──────┴───┐
//                      │ mLastClosedLedgerHAS │      │ DB       │
//                      │ mLastClosedSnapshot ─┼─────►│ &        │
//                      │                      │      │ Buckets  │
//                      └──────────────────────┘      └──────────┘
//
//
// The apply thread uses the buckets and database, as well as some state
// variables in the LM. The remaining state variables of the LM, including the
// LCL variables, are for the main thread. One of the LCL state variables is a
// snapshot of the bucketlist. The bucketlist snapshots are immutable and
// therefore trivially threadsafe, but may be lagging behind the newest
// bucketlist formed by the apply thread.
//
// In more precise terms, 4 points on the diagram are labeled: L, Q, A, and LCL.
// These are points where we can identify and relate some state variables and an
// invariant order between them:
//
//     H -- LedgerApplyManagerImpl::mLargestLedgerSeqHeard tracks this, it is
//          the ledger sequence number of the most recent ledger added to
//          mSyncingLedgers (whether or not any ledgers are still _in_
//          mSyncingLedgers, it may have been emptied by the apply thread).
//
//     Q -- LedgerApplyManagerImpl::mLastQueuedToApply tracks this, it is the
//          ledger sequence number of the most recent ledger _dequeued_ from
//          mSyncingLedgers and posted over to the apply thread. This does not
//          mean it has been applied! Just posted to the apply thread.
//
//     A -- The LedgerCloseData::mLedgerSeq of the argument passed to
//          LedgerManagerImpl::applyLedger tracks this, as well as the live
//          state of the database and live bucketlist: they are updated
//          synchronously from A-1 to A by applyLedger (and its callees).
//          In other words A is the ledger _currently being applied_ by the
//          apply thread, which might be behind Q if multiple ledgers have
//          been posted to the apply thread but also moves ahead of LCL when
//          the apply thread is running.
//
//   LCL -- LedgerManagerImpl::mLastClosedLedger.header.ledgerSeq tracks this.
//          It is the ledger sequence number of the most recent ledger that has
//          been posted back to the main thread _and received there and copied
//          into the LCL variables_. It may be more than 1 ledger behind A if
//          A is closing quickly and the main thread is busy: results are posted
//          back to the main thread using a queue.
//
// The invariant is that LCL <= A <= Q <= H. Each can get arbitrarily far ahead
// of the previous (for example of the network votes ahead of the current node,
// or the current node is doing bulk catchup), and they can also all be equal
// (for example if the current node just booted up, or is idle between ledgers);
// but they must always be in this order.
//
// Note also that SCP nominates a value containing a txset which contains, by
// hash reference, the LCL; so voting cannot actually start on ledger N until
// LCL has arrived at N-1. This limits the maximum potential parallelism between
// voting and applying. But we can still at least overlap the apply thread's big
// synchronous CPU-and-IO-consuming activity with "other stuff the main thread
// has to do" such as queueing and validating transactions, as well as stepping
// SCP state machines through incoming SCP messages. In particular: if a _fast
// supermajority_ of nodes votes to adopt ledger N, then a slower minority will
// accept the externalize without voting themselves, and can do so even before
// they've managed to catch their _own_ LCL up to ledger N-1. So there is some
// benefit to the complexity here.
//
// To complicate matters a little more: applyLedger _can_ be called
// synchronously from the main thread. It detects this condition and
// synchronously completes ledger close, rather than posting back to itself.
// This happens in the testsuite, as well as if the user disables parallel
// ledger apply.
//
// Finally, a terminology note: the LedgerManager was originally only accessed
// by a single thread (the main thread) but now has state variables used by both
// main and apply threads, which can be confusing. Moreover from the perspective
// of the protocol -- SCP messages and XDR structs -- the action of "closing a
// ledger" is fairly atomic, and there is no distinction made between "apply"
// and "close". But now that we have two threads, at least _within these
// classes_ we endeavour to use the term "close" and "last closed" to refer to
// the actions taken and variables updated by the main thread _after_ apply, and
// the term "apply" to refer to actions taken and variables updated by the apply
// thread.
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

    virtual void moveToSynced() = 0;
    virtual void beginApply() = 0;
    virtual State getState() const = 0;
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
    static std::string ledgerAbbrev(LedgerHeaderHistoryEntry const& he);

    // Factory
    static std::unique_ptr<LedgerManager> create(Application& app);

    // Genesis ledger
    static LedgerHeader genesisLedger();

    // Called by Herder to inform LedgerManager that a SCP has agreed on a new
    // close event. This is the most common cause of LedgerManager advancing
    // from one ledger to the next: the network reached consensus on
    // `ledgerData`.
    virtual void valueExternalized(LedgerCloseData const& ledgerData,
                                   bool isLatestSlot) = 0;

    // Return the LCL header and (complete, immutable) hash.
    virtual LedgerHeaderHistoryEntry const&
    getLastClosedLedgerHeader() const = 0;

    // Get bucketlist snapshot of LCL
    virtual SearchableSnapshotConstPtr getLastClosedSnaphot() = 0;

    // return the HAS that corresponds to the last closed ledger as persisted in
    // the database
    // This function return of copy of latest HAS, so it's thread-safe.
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

    // return the maximum size of a transaction set to apply to the current
    // ledger expressed in number of operations
    virtual uint32_t getLastMaxTxSetSizeOps() const = 0;

    virtual Resource maxLedgerResources(bool isSoroban) = 0;
    virtual Resource maxSorobanTransactionResources() = 0;
    virtual void updateSorobanNetworkConfigForApply(AbstractLedgerTxn& ltx) = 0;
    // Return the network config for Soroban.
    // The config is automatically refreshed on protocol upgrades.
    // Ledger txn here is needed for the sake of lazy load; it won't be
    // used most of the time.
    virtual SorobanNetworkConfig const& getLastClosedSorobanNetworkConfig() = 0;
    virtual SorobanNetworkConfig const& getSorobanNetworkConfigForApply() = 0;

    virtual bool hasLastClosedSorobanNetworkConfig() const = 0;

#ifdef BUILD_TESTS
    virtual SorobanNetworkConfig& getMutableSorobanNetworkConfigForApply() = 0;
    virtual std::vector<TransactionMetaFrame> const&
    getLastClosedLedgerTxMeta() = 0;
    virtual void storeCurrentLedgerForTest(LedgerHeader const& header) = 0;
#endif

    // Return the (changing) number of seconds since the LCL closed.
    virtual uint64_t secondsSinceLastLedgerClose() const = 0;

    // Ensure any metrics that are "current state" gauge-like counters reflect
    // the current reality as best as possible.
    virtual void syncMetrics() = 0;

    virtual Database& getDatabase() = 0;

    // Called by application lifecycle events, system startup.
    virtual void startNewLedger() = 0;

    // loads the last ledger information from the database with the following
    // parameter:
    //  * restoreBucketlist indicates whether to restore the bucket list fully,
    //  and restart merges
    virtual void loadLastKnownLedger(bool restoreBucketlist) = 0;

    // Forcibly switch the application into catchup mode, treating `toLedger`
    // as the destination ledger number and count as the number of past ledgers
    // that should be replayed. Normally this happens automatically when
    // LedgerManager detects it is desynchronized from SCP's consensus ledger.
    // This method is present in the public interface to permit testing and
    // offline catchups.
    virtual void startCatchup(CatchupConfiguration configuration,
                              std::shared_ptr<HistoryArchive> archive) = 0;

    // Forcibly apply `ledgerData` to the current ledger, causing it to close.
    // This is normally done automatically as part of `valueExternalized()`
    // during normal operation (in which case `calledViaExternalize` should be
    // set to true), but can also be called directly by catchup (with
    // `calledViaExternalize` false in this case).
    virtual void applyLedger(LedgerCloseData const& ledgerData,
                             bool calledViaExternalize) = 0;
#ifdef BUILD_TESTS
    void
    applyLedger(LedgerCloseData const& ledgerData)
    {
        applyLedger(ledgerData, /* externalize */ false);
    }
#endif

    virtual void
    setLastClosedLedger(LedgerHeaderHistoryEntry const& lastClosed) = 0;

    virtual void manuallyAdvanceLedgerHeader(LedgerHeader const& header) = 0;

    virtual SorobanMetrics& getSorobanMetrics() = 0;
    virtual ::rust::Box<rust_bridge::SorobanModuleCache> getModuleCache() = 0;

    virtual ~LedgerManager()
    {
    }

    virtual bool isApplying() const = 0;
};
}
