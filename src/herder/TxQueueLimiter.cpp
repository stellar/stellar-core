// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxQueueLimiter.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"

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
        return lessThanXored(l, r, mSeed);
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

TxQueueLimiter::TxQueueLimiter(uint32 multiplier, LedgerManager& lm)
    : mPoolLedgerMultiplier(multiplier), mLedgerManager(lm)
{
    mTxs = std::make_unique<QueueLimiterTxMap>();
    mMinFeeNeeded = {0, 0};
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

// compute the fee bid that `tx` should have in order to beat
// a transaction `ref` with fee bid `refFeeBid` and `refNbOps` operations
int64
computeBetterFee(TransactionFrameBasePtr const& tx, int64 refFeeBid,
                 uint32 refNbOps)
{
    constexpr auto m = std::numeric_limits<int64>::max();

    int64 minFee = m;
    int64 v;
    if (bigDivide(v, refFeeBid, tx->getNumOperations(), refNbOps,
                  Rounding::ROUND_DOWN) &&
        v < m)
    {
        minFee = v + 1;
    }
    return minFee;
}

std::pair<bool, int64>
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx) const
{
    // enforce min fee if needed
    if (mMinFeeNeeded.second != 0)
    {
        auto cmp3Min =
            feeRate3WayCompare(newTx->getFeeBid(), newTx->getNumOperations(),
                               mMinFeeNeeded.first, mMinFeeNeeded.second);
        if (cmp3Min <= 0)
        {
            auto minFee = computeBetterFee(newTx, mMinFeeNeeded.first,
                                           mMinFeeNeeded.second);
            return std::make_pair(false, minFee);
        }
    }

    auto newOps = mQueueSizeOps;
    if (oldTx)
    {
        // oldTx is currently tracked by the queue,
        // so this should always hold
        releaseAssert(oldTx->getNumOperations() <= newOps);
        newOps -= oldTx->getNumOperations();
    }
    newOps += newTx->getNumOperations();

    // if there is enough space, return
    if (newOps <= maxQueueSizeOps())
    {
        return std::make_pair(true, 0ll);
    }

    // need to see if we could be added by kicking out cheaper transactions
    // starting with the cheapest one
    auto neededOps = newOps - maxQueueSizeOps();
    auto id = newTx->getSourceID();
    for (auto it = mTxs->begin(); it != mTxs->end(); ++it)
    {
        if (feeRate3WayCompare(*it, newTx) >= 0)
        {
            auto minFee = computeBetterFee(newTx, (*it)->getFeeBid(),
                                           (*it)->getNumOperations());
            return std::make_pair(false, minFee);
        }
        // ensure that this transaction is not from the same account
        auto& tx = *it;
        if (tx->getSourceID() == id)
        {
            return std::make_pair(false, 0ll);
        }
        auto curOps = tx->getNumOperations();
        if (neededOps <= curOps)
        {
            return std::make_pair(true, 0ll);
        }
        neededOps -= curOps;
    }

    // we reach this point if the queue doesn't have capacity for that
    // transaction even when empty. Combination of multiplier and max ledger
    // size is too small for whatever reason
    static size_t lastMax = std::numeric_limits<size_t>::max();
    if (lastMax != maxQueueSizeOps())
    {
        lastMax = maxQueueSizeOps();
        CLOG_WARNING(Herder,
                     "Transaction Queue limiter configured with {} operations: "
                     "node won't be able to accept all transactions",
                     lastMax);
    }

    return std::make_pair(false, 0ll);
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
        mMinFeeNeeded = {evictTx->getFeeBid(), evictTx->getNumOperations()};
        evict(evictTx);
    }
    return true;
}

void
TxQueueLimiter::reset()
{
    mTxs = std::make_unique<QueueLimiterTxMap>();
    mQueueSizeOps = 0;
    resetMinFeeNeeded();
}

std::pair<int64, uint32>
TxQueueLimiter::getMinFeeNeeded() const
{
    return mMinFeeNeeded;
}

void
TxQueueLimiter::resetMinFeeNeeded()
{
    mMinFeeNeeded = {0ll, 0};
}

bool
lessThanXored(TransactionFrameBasePtr const& l,
              TransactionFrameBasePtr const& r, size_t seed)
{
    auto cmp3 = feeRate3WayCompare(l, r);
    if (cmp3 != 0)
    {
        return cmp3 < 0;
    }
    // break tie with pointer arithmetic
    auto lx = reinterpret_cast<size_t>(l.get()) ^ seed;
    auto rx = reinterpret_cast<size_t>(r.get()) ^ seed;
    return lx < rx;
}
}