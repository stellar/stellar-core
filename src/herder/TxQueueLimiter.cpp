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
namespace
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

    uint32_t
    getNumOperations() const override
    {
        releaseAssert(mTx);
        return mTx->getNumOperations();
    }

  private:
    TransactionFrameBasePtr mTx;
};

int64_t
computeBetterFee(std::pair<int64, uint32_t> const& evictedBid,
                 TransactionFrameBase const& tx)
{
    if (evictedBid.second != 0 &&
        feeRate3WayCompare(evictedBid.first, evictedBid.second, tx.getFeeBid(),
                           tx.getNumOperations()) >= 0)
    {
        return computeBetterFee(tx, evictedBid.first, evictedBid.second);
    }
    return 0;
}

}

TxQueueLimiter::TxQueueLimiter(uint32 multiplier, LedgerManager& lm)
    : mPoolLedgerMultiplier(multiplier), mLedgerManager(lm)
{
    mEvictedFeeBid = {0, 0};
}

TxQueueLimiter::~TxQueueLimiter()
{
    // empty destructor allows deleting TxQueueLimiter from other source files
}

#ifdef BUILD_TESTS
size_t
TxQueueLimiter::size() const
{
    return mTxs->sizeOps();
}
#endif

uint32_t
TxQueueLimiter::maxQueueSizeOps() const
{
    uint32_t maxOpsLedger = mLedgerManager.getLastMaxTxSetSizeOps();
    maxOpsLedger *= mPoolLedgerMultiplier;
    return maxOpsLedger;
}

void
TxQueueLimiter::addTransaction(TransactionFrameBasePtr const& tx)
{
    auto txStack = std::make_shared<SingleTxStack>(tx);
    mStackForTx[tx] = txStack;
    mTxs->add(txStack);
}

void
TxQueueLimiter::removeTransaction(TransactionFrameBasePtr const& tx)
{
    auto txStackIt = mStackForTx.find(tx);
    if (txStackIt == mStackForTx.end())
    {
        throw std::logic_error(
            "invalid state (missing tx) while removing tx in TxQueueLimiter");
    }
    mTxs->erase(txStackIt->second);
}

std::pair<bool, int64>
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx,
                         std::vector<TxStackPtr>& txsToEvict)
{
    // We cannot normally initialize transaction queue in the constructor
    // because `maxQueueSizeOps()` may not be initialized. Hence we initialize
    // lazily during the add/reset.
    if (mTxs == nullptr)
    {
        resetTxs();
    }

    int64_t minFeeToBeatEvicted = computeBetterFee(mEvictedFeeBid, *newTx);
    if (minFeeToBeatEvicted > 0)
    {
        return std::make_pair(false, minFeeToBeatEvicted);
    }

    uint32_t txOpsDiscount = 0;
    if (oldTx)
    {
        auto newTxOps = newTx->getNumOperations();
        auto oldTxOps = oldTx->getNumOperations();
        // Bump transactions must have at least the old amount of operations.
        releaseAssert(oldTxOps <= newTxOps);
        txOpsDiscount = newTxOps - oldTxOps;
    }
    // Update the operation limits in case upgrade happened. This is cheap
    // enough to happen unconditionally without relying on upgrade triggers.
    mTxs->updateOpsLimit(maxQueueSizeOps());
    return mTxs->canFitWithEviction(*newTx, txOpsDiscount, txsToEvict);
}

void
TxQueueLimiter::evictTransactions(
    std::vector<TxStackPtr> const& txsToEvict, uint32_t opsToFit,
    std::function<void(TransactionFrameBasePtr const&)> evict)
{
    uint32_t maxOps = maxQueueSizeOps();
    for (auto const& evictedStack : txsToEvict)
    {
        auto tx = evictedStack->getTopTx();
        mEvictedFeeBid = {tx->getFeeBid(), tx->getNumOperations()};
        evict(tx);
        // While we guarantee `txsToEvict` to have enough operations to fit new
        // operations, the eviction itself may remove transactions with high seq
        // nums and hence make space sooner than expected.
        if (mTxs->sizeOps() + opsToFit <= maxOps)
        {
            break;
        }
    }
    // It should be guaranteed to fit the required operations after the
    // eviction.
    releaseAssert(mTxs->sizeOps() + opsToFit <= maxOps);
}

void
TxQueueLimiter::reset()
{
    resetTxs();
    resetEvictionState();
}

void
TxQueueLimiter::resetEvictionState()
{
    mEvictedFeeBid = {0, 0};
}

void
TxQueueLimiter::resetTxs()
{
    mTxs = std::make_unique<SurgePricingPriorityQueue>(
        /* isHighestPriority */ false, maxQueueSizeOps(),
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
}
}
