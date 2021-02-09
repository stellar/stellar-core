#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "ledger/LedgerManager.h"
#include "transactions/TransactionFrame.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"

namespace stellar
{
class QueueLimiterTxMap;
class TxQueueLimiter
{
    // size of the transaction queue, in operations
    size_t mQueueSizeOps{0};
    // number of ledgers we can pool in memory
    uint32 const mPoolLedgerMultiplier;
    LedgerManager& mLedgerManager;
    // minimum fee needed for a transaction
    // stored as a pair (fee bid, nb operations)
    std::pair<int64, uint32> mMinFeeNeeded;

    // all known transactions
    std::unique_ptr<QueueLimiterTxMap> mTxs;
    TransactionFrameBasePtr getWorstTransaction();

  public:
    TxQueueLimiter(uint32 multiplier, LedgerManager& lm);
    ~TxQueueLimiter();

    size_t
    size() const
    {
        return mQueueSizeOps;
    }
    size_t maxQueueSizeOps() const;

    void addTransaction(TransactionFrameBasePtr const& tx);
    void removeTransaction(TransactionFrameBasePtr const& tx);

    // evict the worst transactions until there
    // is enough capacity to insert ops operations
    // by calling `evict` - note that evict must call `removeTransaction`
    // as to make space
    // returns false if it ran out of transactions before
    // reaching its goal
    bool evictTransactions(
        size_t ops, std::function<void(TransactionFrameBasePtr const&)> evict);

    // oldTx is set when performing a replace by fee
    // return
    // first=true if transaction can be added
    // otherwise:
    //    second=0 if caller needs to wait
    //    second=minimum fee needed for tx to pass the next round of
    //    validation
    std::pair<bool, int64> canAddTx(TransactionFrameBasePtr const& tx,
                                    TransactionFrameBasePtr const& oldTx) const;

    void reset();

    // txs must have strictly more base fee bid than this
    std::pair<int64, uint32> getMinFeeNeeded() const;

    void resetMinFeeNeeded();
};

bool lessThanXored(TransactionFrameBasePtr const& l,
                   TransactionFrameBasePtr const& r, size_t seed);
}
