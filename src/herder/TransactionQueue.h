#pragma once

// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "crypto/SecretKey.h"
#include "herder/TxQueueLimiter.h"
#include "herder/TxSetFrame.h"
#include "ledger/LedgerTxn.h"
#include "transactions/TransactionFrame.h"
#include "util/HashOfHash.h"
#include "util/Timer.h"
#include "util/XDROperators.h"
#include "xdr/Stellar-transaction.h"

#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include <chrono>
#include <deque>
#include <memory>
#include <vector>

namespace medida
{
class Counter;
class Timer;
}

namespace stellar
{

class Application;

/**
 * TransactionQueue keeps received transactions that are valid and have not yet
 * been included in a transaction set.
 *
 * An accountID is in mAccountStates if and only if it is the fee-source or
 * sequence-number-source for at least one transaction in the TransactionQueue.
 * This invariant is maintained by releaseFeeMaybeEraseAccountState.
 *
 * Transactions received from the HTTP "tx" endpoint and the overlay network
 * should be added by calling tryAdd. If that succeeds, the transaction may be
 * removed later in three ways:
 * - removeApplied() should be called after transactions are applied. It removes
 *   the specified transactions, but leaves transactions with subsequent
 *   sequence numbers in the TransactionQueue. It also resets the age for the
 *   sequence-number-source of each specified transaction.
 * - ban() should be called after transactions become invalid for any reason.
 *   Banned transactions cannot be added to the TransactionQueue again for a
 *   banDepth ledgers.
 * - shift() should be called after each ledger close, after removeApplied. It
 *   increases the age for every account that is the sequence-number-source for
 *   at least one transaction. If the age becomes greater than or equal to
 *   pendingDepth, all transactions for that source account are banned. It also
 *   unbans any transactions that have been banned for more than banDepth
 *   ledgers.
 */
class TransactionQueue
{
  public:
    static uint64_t const FEE_MULTIPLIER;

    enum class AddResult
    {
        ADD_STATUS_PENDING = 0,
        ADD_STATUS_DUPLICATE,
        ADD_STATUS_ERROR,
        ADD_STATUS_TRY_AGAIN_LATER,
        ADD_STATUS_FILTERED,
        ADD_STATUS_COUNT
    };

    /*
     * Information about queue of transaction for given account. mAge and
     * mTotalFees are stored in queue, but mMaxSeq must be computed each
     * time (its O(1) anyway).
     */
    struct AccountTxQueueInfo
    {
        SequenceNumber mMaxSeq{0};
        int64_t mTotalFees{0};
        size_t mQueueSizeOps{0};
        size_t mBroadcastQueueOps{0};
        uint32_t mAge{0};

        friend bool operator==(AccountTxQueueInfo const& x,
                               AccountTxQueueInfo const& y);
    };

    /**
     * AccountState stores the following information:
     * - mTotalFees: the sum of feeBid() over every transaction for which this
     *   account is the fee-source (this may include transactions that are not
     *   in mTransactions)
     * - mAge: the number of ledgers that have closed since the last ledger in
     *   which a transaction in mTransactions was included. This is always 0 if
     *   mTransactions is empty
     * - mTransactions: the list of transactions for which this account is the
     *   sequence-number-source, ordered by sequence number
     */

    struct TimestampedTx
    {
        TransactionFrameBasePtr mTx;
        bool mBroadcasted;
        VirtualClock::time_point mInsertionTime;
        bool mSubmittedFromSelf;
    };
    using TimestampedTransactions = std::vector<TimestampedTx>;
    using Transactions = std::vector<TransactionFrameBasePtr>;
    struct AccountState
    {
        int64_t mTotalFees{0};
        size_t mQueueSizeOps{0};
        size_t mBroadcastQueueOps{0};
        uint32_t mAge{0};
        TimestampedTransactions mTransactions;
    };

    explicit TransactionQueue(Application& app, uint32 pendingDepth,
                              uint32 banDepth, uint32 poolLedgerMultiplier);
    ~TransactionQueue();

    static std::vector<AssetPair>
    findAllAssetPairsInvolvedInPaymentLoops(TransactionFrameBasePtr tx);

    AddResult tryAdd(TransactionFrameBasePtr tx, bool submittedFromSelf);
    void removeApplied(Transactions const& txs);
    void ban(Transactions const& txs);

    /**
     * Increase age of each AccountState that has at least one transaction in
     * mTransactions. Also increments the age for each banned transaction, and
     * unbans transactions for which age equals banDepth.
     */
    void shift();

    AccountTxQueueInfo
    getAccountTransactionQueueInfo(AccountID const& accountID) const;

    size_t countBanned(int index) const;
    bool isBanned(Hash const& hash) const;
    TransactionFrameBaseConstPtr getTx(Hash const& hash) const;

    TxSetFrame::Transactions getTransactions(LedgerHeader const& lcl) const;

    struct ReplacedTransaction
    {
        TransactionFrameBasePtr mOld;
        TransactionFrameBasePtr mNew;
    };

    void maybeVersionUpgraded();

    void rebroadcast();

    void shutdown();
    size_t getMaxQueueSizeOps() const;

  private:
    /**
     * The AccountState for every account. As noted above, an AccountID is in
     * AccountStates iff at least one of the following is true for the
     * corresponding AccountState
     * - AccountState.mTotalFees > 0
     * - !AccountState.mTransactions.empty()
     */
    using AccountStates = UnorderedMap<AccountID, AccountState>;

    /**
     * Banned transactions are stored in deque of depth banDepth, so it is easy
     * to unban all transactions that were banned for long enough.
     */
    using BannedTransactions = std::deque<UnorderedSet<Hash>>;

    Application& mApp;
    uint32 const mPendingDepth;

    AccountStates mAccountStates;
    BannedTransactions mBannedTransactions;
    uint32_t mLedgerVersion;

    // counters
    std::vector<medida::Counter*> mSizeByAge;
    medida::Counter& mBannedTransactionsCounter;
    medida::Counter& mArbTxSeenCounter;
    medida::Counter& mArbTxDroppedCounter;
    medida::Timer& mTransactionsDelay;
    medida::Timer& mTransactionsSelfDelay;

    UnorderedSet<OperationType> mFilteredTypes;

    bool mShutdown{false};
    bool mWaiting{false};
    std::vector<uint32_t> mBroadcastOpCarryover;
    VirtualTimer mBroadcastTimer;

    std::pair<uint32_t, std::optional<uint32_t>>
    getMaxOpsToFloodThisPeriod() const;
    bool broadcastSome();
    void broadcast(bool fromCallback);
    // broadcasts a single transaction
    enum class BroadcastStatus
    {
        BROADCAST_STATUS_ALREADY,
        BROADCAST_STATUS_SUCCESS,
        BROADCAST_STATUS_SKIPPED
    };
    BroadcastStatus broadcastTx(AccountState& state, TimestampedTx& tx);

    AddResult canAdd(TransactionFrameBasePtr tx,
                     AccountStates::iterator& stateIter,
                     TimestampedTransactions::iterator& oldTxIter,
                     std::vector<std::pair<TxStackPtr, bool>>& txsToEvict);

    void releaseFeeMaybeEraseAccountState(TransactionFrameBasePtr tx);

    void prepareDropTransaction(AccountState& as, TimestampedTx& tstx);
    void dropTransactions(AccountStates::iterator stateIter,
                          TimestampedTransactions::iterator begin,
                          TimestampedTransactions::iterator end);

    void clearAll();

    bool isFiltered(TransactionFrameBasePtr tx) const;

    std::unique_ptr<TxQueueLimiter> mTxQueueLimiter;
    UnorderedMap<AssetPair, uint32_t, AssetPairHash> mArbitrageFloodDamping;

    UnorderedMap<Hash, TransactionFrameBasePtr> mKnownTxHashes;

    size_t mBroadcastSeed;

    friend class TxQueueTracker;

#ifdef BUILD_TESTS
  public:
    size_t getQueueSizeOps() const;
    std::optional<int64_t> getInQueueSeqNum(AccountID const& account) const;
    std::function<void(TransactionFrameBasePtr&)> mTxBroadcastedEvent;
#endif
};

extern std::array<const char*,
                  static_cast<int>(
                      TransactionQueue::AddResult::ADD_STATUS_COUNT)>
    TX_STATUS_STRING;

}
