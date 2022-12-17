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

class TxQueueLimiter
{
    // number of ledgers we can pool in memory
    uint32 const mPoolLedgerMultiplier;
    LedgerManager& mLedgerManager;

    UnorderedMap<TransactionFrameBasePtr, TxStackPtr> mStackForTx;

    // all known transactions
    std::unique_ptr<SurgePricingPriorityQueue> mTxs;

    // When non-nullopt, limit the number dex operations by this value
    std::optional<uint32_t> mMaxDexOperations;

    // Stores the maximum bid among the transactions evicted from every tx lane.
    // Bids are stored as ratios (fee_bid / num_ops).
    std::vector<std::pair<int64, uint32_t>> mLaneEvictedFeeBid;

    // Configuration of SurgePricingPriorityQueue with the per-lane operation
    // limits.
    std::shared_ptr<SurgePricingLaneConfig> mSurgePricingLaneConfig;

  public:
    TxQueueLimiter(uint32 multiplier, Application& app);
    ~TxQueueLimiter();

    void addTransaction(TransactionFrameBasePtr const& tx);
    void removeTransaction(TransactionFrameBasePtr const& tx);
#ifdef BUILD_TESTS
    size_t size() const;
#endif
    uint32_t maxQueueSizeOps() const;

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
             std::vector<std::pair<TxStackPtr, bool>>& txsToEvict);

    // Resets the state related to evictions (maximum evicted bid).
    void resetEvictionState();

    // Resets the internal transaction container and the eviction state.
    void reset();
};
}
