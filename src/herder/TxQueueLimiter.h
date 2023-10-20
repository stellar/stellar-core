#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "ledger/LedgerManager.h"
#include "transactions/TransactionFrame.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"

namespace stellar
{

class SingleTxStack : public TxStack
{
  public:
    SingleTxStack(TransactionFrameBasePtr tx) : mTx(tx)
    {
    }

    TransactionFrameBasePtr
    getTopTx() const override
    {
        releaseAssert(mTx);
        return mTx;
    }

    void
    popTopTx() override
    {
        releaseAssert(mTx);
        mTx = nullptr;
    }

    bool
    empty() const override
    {
        return mTx == nullptr;
    }

    Resource
    getResources() const override
    {
        releaseAssert(mTx);
        return Resource(mTx->getResources(/* useByteLimitInClassic */ false));
    }

  private:
    TransactionFrameBasePtr mTx;
};

class TxQueueLimiter
{
    // number of ledgers we can pool in memory
    uint32 const mPoolLedgerMultiplier;
    LedgerManager& mLedgerManager;

    UnorderedMap<TransactionFrameBasePtr, TxStackPtr> mStackForTx;

    // all known transactions
    std::unique_ptr<SurgePricingPriorityQueue> mTxs;

    // When non-nullopt, limit the number dex operations by this value
    std::optional<Resource> mMaxDexOperations;

    // Stores the maximum inclusion fee among the transactions evicted from
    // every tx lane. Inclusion fees are stored as ratios (fee_bid / num_ops).
    std::vector<std::pair<int64, uint32_t>> mLaneEvictedInclusionFee;

    // Configuration of SurgePricingPriorityQueue with the per-lane operation
    // limits.
    std::shared_ptr<SurgePricingLaneConfig> mSurgePricingLaneConfig;

    // Quick lookup of relevant account IDs, needed temporary to maintain
    // 1-tx-per-account invariance. When tx stacks are removed, we can remove
    // this logic as well.
    std::optional<std::unordered_set<AccountID>> mEnforceSingleAccounts;
    Application& mApp;
    bool const mIsSoroban;

  public:
    TxQueueLimiter(uint32 multiplier, Application& app, bool isSoroban);
    ~TxQueueLimiter();

    void addTransaction(TransactionFrameBasePtr const& tx);
    void removeTransaction(TransactionFrameBasePtr const& tx);
#ifdef BUILD_TESTS
    size_t size() const;
    std::pair<bool, int64>
    canAddTx(TransactionFrameBasePtr const& tx,
             TransactionFrameBasePtr const& oldTx,
             std::vector<std::pair<TxStackPtr, bool>>& txsToEvict);
#endif
    Resource maxScaledLedgerResources(bool isSoroban) const;

    // Evict `txsToEvict` from the limiter by calling `evict`.
    // `txsToEvict` should be provided by the `canAddTx` call.
    // Note that evict must call `removeTransaction` as to make space.
    void evictTransactions(
        std::vector<std::pair<TxStackPtr, bool>> const& txsToEvict,
        TransactionFrameBase const& txToFit,
        std::function<void(TransactionFrameBasePtr const&)> evict);

    // oldTx is set when performing a replace by fee
    // return
    // first=true if transaction can be added
    // otherwise:
    //    second=0 if caller needs to wait
    //    second=minimum fee needed for tx to pass the next round of
    //    validation
    // `txsToEvict` will contain transactions that need to be evicted in order
    // to fit the new transactions. It should be passed to `evictTransactions`
    // to perform the actual eviction.
    std::pair<bool, int64>
    canAddTx(TransactionFrameBasePtr const& tx,
             TransactionFrameBasePtr const& oldTx,
             std::vector<std::pair<TxStackPtr, bool>>& txsToEvict,
             uint32_t ledgerVersion);

    // Resets the state related to evictions (maximum evicted bid).
    void resetEvictionState();

    // Resets the internal transaction container and the eviction state.
    void reset(uint32_t ledgerVersion);
};
}
