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

// TODO:
// * Dig into flow control, make sure it holds some kind of lock to prevent ASIO
// overlay queue from growing too much
//     * Might want to put bg tx queue on its own thread with intermediate
//     priority (lower than SCP, higher than bucket maintenance). Otherwise,
//     running this on the overlay thread might delay SCP message processing as
//     incoming SCP messages need to wait for tx queue additions to occur, which
//     is bad.
//         * My note: Try both approaches and benchmark

enum class TxQueueAddResultCode
{
    ADD_STATUS_PENDING = 0,
    ADD_STATUS_DUPLICATE,
    ADD_STATUS_ERROR,
    ADD_STATUS_TRY_AGAIN_LATER,
    ADD_STATUS_FILTERED,
    ADD_STATUS_COUNT
};

struct TxQueueAddResult
{
    TxQueueAddResultCode code;
    MutableTxResultPtr txResult;

    // AddResult with no txResult
    explicit TxQueueAddResult(TxQueueAddResultCode addCode);

    // AddResult from existing transaction result
    explicit TxQueueAddResult(TxQueueAddResultCode addCode,
                              MutableTxResultPtr payload);

    // AddResult with error txResult with the specified txErrorCode
    explicit TxQueueAddResult(TxQueueAddResultCode addCode,
                              TransactionFrameBasePtr tx,
                              TransactionResultCode txErrorCode);
};

class TransactionQueue
{
  public:
    static uint64_t const FEE_MULTIPLIER;

    using AddResultCode = TxQueueAddResultCode;
    using AddResult = TxQueueAddResult;

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
    using Transactions = std::vector<TransactionFrameBasePtr>;
    struct AccountState
    {
        int64_t mTotalFees{0};
        uint32_t mAge{0};
        std::optional<TimestampedTx> mTransaction;
    };

    explicit TransactionQueue(Application& app,
                              SearchableSnapshotConstPtr bucketSnapshot,
                              uint32 pendingDepth, uint32 banDepth,
                              uint32 poolLedgerMultiplier, bool isSoroban);
    virtual ~TransactionQueue();

    static std::vector<AssetPair>
    findAllAssetPairsInvolvedInPaymentLoops(TransactionFrameBasePtr tx);

    AddResult tryAdd(TransactionFrameBasePtr tx, bool submittedFromSelf);
    // Ban transactions that are no longer valid or have insufficient fee;
    // transaction per account limit applies here, so `txs` should have no
    // duplicate source accounts
    void ban(Transactions const& txs);

    void shutdown();

    // TODO: Better docs
    // TODO: More descriptive name
    // Update internal queue structures after a ledger closes
    void update(
        Transactions const& applied, LedgerHeader const& lcl,
        SearchableSnapshotConstPtr newBucketSnapshot,
        std::function<TxFrameList(TxFrameList const&)> const& filterInvalidTxs);

    bool isBanned(Hash const& hash) const;
    TransactionFrameBaseConstPtr getTx(Hash const& hash) const;
    TxFrameList getTransactions(LedgerHeader const& lcl) const;
    bool sourceAccountPending(AccountID const& accountID) const;

    virtual size_t getMaxQueueSizeOps() const = 0;

#ifdef BUILD_TESTS
    AccountState
    getAccountTransactionQueueInfo(AccountID const& accountID) const;
    size_t countBanned(int index) const;
#endif

  protected:
    // TODO: Docs?
    // TODO: Move?
    class TxQueueLock : NonMovableOrCopyable
    {
      public:
        TxQueueLock(std::unique_lock<std::mutex>&& lock,
                    std::shared_ptr<std::condition_variable> cv)
            : mLock(std::move(lock)), mCv(cv)
        {
        }
        ~TxQueueLock()
        {
            // Wake threads on destruction
            mCv->notify_all();
        }

      private:
        std::unique_lock<std::mutex> mLock;
        std::shared_ptr<std::condition_variable> mCv;
    };

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

    uint32 const mPendingDepth;

    AccountStates mAccountStates;
    BannedTransactions mBannedTransactions;

    // counters
    struct QueueMetrics
    {
        QueueMetrics(std::vector<medida::Counter*> sizeByAge,
                     medida::Counter& bannedTransactionsCounter,
                     medida::Timer& transactionsDelay,
                     medida::Timer& transactionsSelfDelay)
            : mSizeByAge(std::move(sizeByAge))
            , mBannedTransactionsCounter(bannedTransactionsCounter)
            , mTransactionsDelay(transactionsDelay)
            , mTransactionsSelfDelay(transactionsSelfDelay)
        {
        }
        std::vector<medida::Counter*> mSizeByAge;
        medida::Counter& mBannedTransactionsCounter;
        medida::Timer& mTransactionsDelay;
        medida::Timer& mTransactionsSelfDelay;
    };

    std::unique_ptr<QueueMetrics> mQueueMetrics;

    UnorderedSet<OperationType> mFilteredTypes;

    bool mShutdown{false};
    bool mWaiting{false};
    bool mPendingMainThreadBroadcast{false}; // TODO: I don't love this solution
    // TODO: VirtualTimer is not thread-safe. Right now it's only used in
    // functions that are called from the main thread. However, if I move
    // broadcasting to the background I will need to be careful with this.
    VirtualTimer mBroadcastTimer;

    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const = 0;
    virtual bool broadcastSome() = 0;
    virtual bool allowTxBroadcast(TimestampedTx const& tx) = 0;

    // TODO: Explain that there's an overload that takes a guard because this
    // function is called internally, and also scheduled on a timer. Any async
    // call should call the first overload (which grabs a lock), and any
    // internal call should call the second overload (which enforces that the
    // lock is already held).
    void broadcast(bool fromCallback);
    void broadcast(bool fromCallback, TxQueueLock const& guard);
    // broadcasts a single transaction
    enum class BroadcastStatus
    {
        BROADCAST_STATUS_ALREADY,
        BROADCAST_STATUS_SUCCESS,
        BROADCAST_STATUS_SKIPPED
    };
    BroadcastStatus broadcastTx(TimestampedTx& tx);

    TransactionQueue::AddResult
    canAdd(TransactionFrameBasePtr tx, AccountStates::iterator& stateIter,
           std::vector<std::pair<TransactionFrameBasePtr, bool>>& txsToEvict);

    void releaseFeeMaybeEraseAccountState(TransactionFrameBasePtr tx);

    void prepareDropTransaction(AccountState& as);
    void dropTransaction(AccountStates::iterator stateIter);

    bool isFiltered(TransactionFrameBasePtr tx) const;

    // TODO: Docs
    // Protected versions of public functions that contain the actual
    // implementation so they can be called internally when the lock is already
    // held.
    void banInternal(Transactions const& banTxs);

    TxQueueLock lock() const;

    // Snapshots to use for transaction validation
    ImmutableValidationSnapshotPtr mValidationSnapshot;
    SearchableSnapshotConstPtr mBucketSnapshot;

    TxQueueLimiter mTxQueueLimiter;
    UnorderedMap<AssetPair, uint32_t, AssetPairHash> mArbitrageFloodDamping;

    UnorderedMap<Hash, TransactionFrameBasePtr> mKnownTxHashes;

    size_t mBroadcastSeed;

  private:
    AppConnector& mAppConn;

    mutable std::mutex mTxQueueMutex;
    mutable std::shared_ptr<std::condition_variable> mTxQueueCv =
        std::make_shared<std::condition_variable>();
    mutable std::atomic<bool> mMainThreadWaiting{false};

    void removeApplied(Transactions const& txs);

    /**
     * Increase age of each AccountState that has at least one transaction in
     * mTransactions. Also increments the age for each banned transaction, and
     * unbans transactions for which age equals banDepth.
     */
    void shift();

    // TODO: Explain that this takes a lock guard due to the `broadcast` call
    // that it makes.
    void rebroadcast(TxQueueLock const& guard);

    // TODO: Docs
    // Private versions of public functions that contain the actual
    // implementation so they can be called internally when the lock is already
    // held.
    bool isBannedInternal(Hash const& hash) const;
    TxFrameList getTransactionsInternal(LedgerHeader const& lcl) const;

    virtual int getFloodPeriod() const = 0;

#ifdef BUILD_TESTS
  public:
    // TODO: These tests invoke protected/private functions directly that assume
    // things are properly locked. I need to make sure these tests operate in a
    // thread-safe manner or change them to not require private member access.
    friend class TransactionQueueTest;

    // TODO: Docs
    void updateSnapshots(SearchableSnapshotConstPtr const& newBucketSnapshot);

    size_t getQueueSizeOps() const;
    std::optional<int64_t> getInQueueSeqNum(AccountID const& account) const;
    std::function<void(TransactionFrameBasePtr&)> mTxBroadcastedEvent;

#endif
};

class SorobanTransactionQueue : public TransactionQueue
{
  public:
    SorobanTransactionQueue(Application& app,
                            SearchableSnapshotConstPtr bucketSnapshot,
                            uint32 pendingDepth, uint32 banDepth,
                            uint32 poolLedgerMultiplier);

    size_t getMaxQueueSizeOps() const override;
#ifdef BUILD_TESTS
    void
    clearBroadcastCarryover()
    {
        TxQueueLock lock = TransactionQueue::lock();
        mBroadcastOpCarryover.clear();
        mBroadcastOpCarryover.resize(1, Resource::makeEmptySoroban());
    }
#endif

  private:
    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const override;
    virtual bool broadcastSome() override;
    std::vector<Resource> mBroadcastOpCarryover;
    // No special flooding rules for Soroban
    virtual bool
    allowTxBroadcast(TimestampedTx const& tx) override
    {
        return true;
    }

    int
    getFloodPeriod() const override
    {
        return mValidationSnapshot->getConfig().FLOOD_SOROBAN_TX_PERIOD_MS;
    }
};

class ClassicTransactionQueue : public TransactionQueue
{
  public:
    ClassicTransactionQueue(Application& app,
                            SearchableSnapshotConstPtr bucketSnapshot,
                            uint32 pendingDepth, uint32 banDepth,
                            uint32 poolLedgerMultiplier);

    size_t getMaxQueueSizeOps() const override;

  private:
    medida::Counter& mArbTxSeenCounter;
    medida::Counter& mArbTxDroppedCounter;

    virtual std::pair<Resource, std::optional<Resource>>
    getMaxResourcesToFloodThisPeriod() const override;
    virtual bool broadcastSome() override;
    std::vector<Resource> mBroadcastOpCarryover;
    virtual bool allowTxBroadcast(TimestampedTx const& tx) override;

    int
    getFloodPeriod() const override
    {
        return mValidationSnapshot->getConfig().FLOOD_TX_PERIOD_MS;
    }
};

// TODO: Rename?
// TODO: Docs. A thread-safe container for transaction queues that allows for
// delayed-initialization of the queues.
// TODO: Doc comments on methods.
class TransactionQueues : public NonMovableOrCopyable
{
  public:
    TransactionQueues() = default;

    void setClassicTransactionQueue(
        std::unique_ptr<ClassicTransactionQueue> classicTransactionQueue);
    void setSorobanTransactionQueue(
        std::unique_ptr<SorobanTransactionQueue> sorobanTransactionQueue);

    bool hasClassicTransactionQueue() const;
    bool hasSorobanTransactionQueue() const;

    ClassicTransactionQueue& getClassicTransactionQueue() const;
    SorobanTransactionQueue& getSorobanTransactionQueue() const;

    // Convenience functions that operate on both queues (if they exist)
    void shutdown();
    bool sourceAccountPending(AccountID const& accountID) const;
    bool isBanned(Hash const& hash) const;
    TransactionFrameBaseConstPtr getTx(Hash const& hash) const;

  private:
    mutable std::mutex mMutex;
    std::unique_ptr<ClassicTransactionQueue> mClassicTransactionQueue = nullptr;
    std::unique_ptr<SorobanTransactionQueue> mSorobanTransactionQueue = nullptr;
};
using TransactionQueuesPtr = std::shared_ptr<TransactionQueues>;

extern std::array<const char*,
                  static_cast<int>(
                      TransactionQueue::AddResultCode::ADD_STATUS_COUNT)>
    TX_STATUS_STRING;
}
