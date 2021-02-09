// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxQueueLimiter.h"
#include "herder/TxSetFrame.h"
#include "util/GlobalChecks.h"

namespace stellar
{

// comparator for TransactionFrameBasePtr
// that sorts by base fee, and breaks ties using pointers
struct QueueLimiterTxComparator
{
    size_t mSeed;
    QueueLimiterTxComparator(size_t seed) : mSeed(seed)
    {
    }

    bool
    operator()(TransactionFrameBasePtr const& l,
               TransactionFrameBasePtr const& r) const
    {
        auto cmp3 = feeRate3WayCompare(l, r);
        if (cmp3 != 0)
        {
            return cmp3 < 0;
        }
        // break tie with pointer arithmetic
        auto lx = reinterpret_cast<size_t>(l.get()) ^ mSeed;
        auto rx = reinterpret_cast<size_t>(r.get()) ^ mSeed;
        return lx < rx;
    }
};

class QueueLimiterTxMap
    : public std::set<TransactionFrameBasePtr, QueueLimiterTxComparator>,
      NonMovableOrCopyable
{
  public:
    QueueLimiterTxMap()
        : std::set<TransactionFrameBasePtr, QueueLimiterTxComparator>(
              QueueLimiterTxComparator(
                  rand_uniform<uint64>(0, std::numeric_limits<uint64>::max())))
    {
    }
};

TxQueueLimiter::TxQueueLimiter(int multiplier, LedgerManager& lm)
    : mPoolLedgerMultiplier(multiplier), mLedgerManager(lm)
{
    mTxs = std::make_unique<QueueLimiterTxMap>();
}

TxQueueLimiter::~TxQueueLimiter()
{
    // empty destructor allows deleting TxQueueLimiter from other source files
}

size_t
TxQueueLimiter::maxQueueSizeOps() const
{
    size_t maxOpsLedger = mLedgerManager.getLastMaxTxSetSizeOps();
    maxOpsLedger *= mPoolLedgerMultiplier;
    return maxOpsLedger;
}

void
TxQueueLimiter::addTransaction(TransactionFrameBasePtr const& tx)
{
    auto newTotOps = mQueueSizeOps;
    auto txOps = tx->getNumOperations();
    newTotOps += txOps;
    if (newTotOps > maxQueueSizeOps())
    {
        throw std::logic_error("invalid state adding tx in TxQueueLimiter");
    }
    mTxs->emplace(tx);
    mQueueSizeOps = newTotOps;
}

void
TxQueueLimiter::removeTransaction(TransactionFrameBasePtr const& tx)
{
    auto txOps = tx->getNumOperations();
    if (mQueueSizeOps < txOps)
    {
        throw std::logic_error(
            "invalid state (queue size) removing tx in TxQueueLimiter");
    }
    if (mTxs->erase(tx) == 0)
    {
        throw std::logic_error(
            "invalid state (missing tx) removing tx in TxQueueLimiter");
    }
    mQueueSizeOps -= txOps;
}

bool
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx) const
{
    auto newOps = mQueueSizeOps;
    if (oldTx)
    {
        // oldTx is currently tracked by the queue,
        // so this should always hold
        releaseAssert(oldTx->getNumOperations() <= newOps);
        newOps -= oldTx->getNumOperations();
    }
    newOps += newTx->getNumOperations();
    if (newOps <= maxQueueSizeOps())
    {
        return true;
    }
    // need to see if we could be added by kicking out cheaper transactions
    // starting with the cheapest one
    auto neededOps = newOps - maxQueueSizeOps();
    auto id = newTx->getSourceID();
    for (auto it = mTxs->begin(); it != mTxs->end(); ++it)
    {
        if (feeRate3WayCompare(*it, newTx) >= 0)
        {
            return false;
        }
        // ensure that this transaction is not from the same account
        auto& tx = *it;
        if (tx->getSourceID() == id)
        {
            return false;
        }
        auto curOps = tx->getNumOperations();
        if (neededOps <= curOps)
        {
            return true;
        }
        neededOps -= curOps;
    }
    // we reach this point if the queue doesn't have capacity for that
    // transaction even when empty. Combination of multiplier and max ledger
    // size is too small for whatever reason
    return false;
}

TransactionFrameBasePtr
TxQueueLimiter::getWorstTransaction()
{
    auto it = mTxs->begin();
    if (it == mTxs->end())
    {
        throw std::logic_error(
            "invalid state getting worst tx in TxQueueLimiter");
    }
    return *it;
}

bool
TxQueueLimiter::evictTransactions(
    size_t ops, std::function<void(TransactionFrameBasePtr const&)> evict)
{
    while (size() + ops > maxQueueSizeOps())
    {
        if (size() == 0)
        {
            return false;
        }
        auto evictTx = getWorstTransaction();
        evict(evictTx);
    }
    return true;
}

void
TxQueueLimiter::reset()
{
    mTxs = std::make_unique<QueueLimiterTxMap>();
    mQueueSizeOps = 0;
}
}
