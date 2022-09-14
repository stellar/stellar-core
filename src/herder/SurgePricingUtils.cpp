// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "crypto/SecretKey.h"
#include "util/Logging.h"
#include "util/numeric128.h"
#include "util/types.h"
#include <numeric>

namespace stellar
{
namespace
{

int
feeRate3WayCompare(TransactionFrameBase const& l, TransactionFrameBase const& r)
{
    return stellar::feeRate3WayCompare(l.getFeeBid(), l.getNumOperations(),
                                       r.getFeeBid(), r.getNumOperations());
}

} // namespace

int
feeRate3WayCompare(int64_t lFeeBid, uint32_t lNbOps, int64_t rFeeBid,
                   uint32_t rNbOps)
{
    // Let f1, f2 be the two fee bids, and let n1, n2 be the two
    // operation counts. We want to calculate the boolean comparison
    // "f1 / n1 < f2 / n2" but, since these are uint128s, we want to
    // avoid the truncating division or use of floating point.
    //
    // Therefore we multiply both sides by n1 * n2, and cancel:
    //
    //               f1 / n1 < f2 / n2
    //  == f1 * n1 * n2 / n1 < f2 * n1 * n2 / n2
    //  == f1 *      n2      < f2 * n1
    auto v1 = bigMultiply(lFeeBid, rNbOps);
    auto v2 = bigMultiply(rFeeBid, lNbOps);
    if (v1 < v2)
    {
        return -1;
    }
    else if (v1 > v2)
    {
        return 1;
    }
    return 0;
}

int64_t
computeBetterFee(TransactionFrameBase const& tx, int64_t refFeeBid,
                 uint32_t refNbOps)
{
    constexpr auto m = std::numeric_limits<int64_t>::max();

    int64 minFee = m;
    int64 v;
    if (bigDivide(v, refFeeBid, tx.getNumOperations(), refNbOps,
                  Rounding::ROUND_DOWN) &&
        v < m)
    {
        minFee = v + 1;
    }
    return minFee;
}

SurgePricingPriorityQueue::TxStackComparator::TxStackComparator(bool isGreater,
                                                                size_t seed)
    : mIsGreater(isGreater), mSeed(seed)
{
}

bool
SurgePricingPriorityQueue::TxStackComparator::operator()(
    TxStackPtr const& txStack1, TxStackPtr const& txStack2) const
{
    return txStackLessThan(*txStack1, *txStack2) ^ mIsGreater;
}

bool
SurgePricingPriorityQueue::TxStackComparator::compareFeeOnly(
    TransactionFrameBase const& tx1, TransactionFrameBase const& tx2) const
{
    return compareFeeOnly(tx1.getFeeBid(), tx1.getNumOperations(),
                          tx2.getFeeBid(), tx2.getNumOperations());
}

bool
SurgePricingPriorityQueue::TxStackComparator::compareFeeOnly(
    int64_t tx1Bid, uint32_t tx1Ops, int64_t tx2Bid, uint32_t tx2Ops) const
{
    bool isLess = feeRate3WayCompare(tx1Bid, tx1Ops, tx2Bid, tx2Ops) < 0;
    return isLess ^ mIsGreater;
}

bool
SurgePricingPriorityQueue::TxStackComparator::isGreater() const
{
    return mIsGreater;
}

bool
SurgePricingPriorityQueue::TxStackComparator::txStackLessThan(
    TxStack const& txStack1, TxStack const& txStack2) const
{
    if (txStack1.empty())
    {
        return !txStack2.empty();
    }
    if (txStack2.empty())
    {
        return false;
    }
    return txLessThan(txStack1.getTopTx(), txStack2.getTopTx(), true);
}

bool
SurgePricingPriorityQueue::TxStackComparator::txLessThan(
    TransactionFrameBaseConstPtr const& tx1,
    TransactionFrameBaseConstPtr const& tx2, bool breakTiesWithHash) const
{
    auto cmp3 = feeRate3WayCompare(*tx1, *tx2);

    if (cmp3 != 0 || !breakTiesWithHash)
    {
        return cmp3 < 0;
    }
    // break tie with pointer arithmetic
    auto lx = reinterpret_cast<size_t>(tx1.get()) ^ mSeed;
    auto rx = reinterpret_cast<size_t>(tx2.get()) ^ mSeed;
    return lx < rx;
}

SurgePricingPriorityQueue::SurgePricingPriorityQueue(bool isHighestPriority,
                                                     uint32_t opsLimit,
                                                     size_t comparisonSeed)
    : mComparator(isHighestPriority, comparisonSeed)
    , mOpsLimit(opsLimit)
    , mOpsCount(0)
    , mTxStackSet(mComparator)
{
}

std::vector<TransactionFrameBasePtr>
SurgePricingPriorityQueue::getMostTopTxsWithinLimit(
    std::vector<TxStackPtr> const& txStacks, uint32_t opsLimit)
{
    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true, opsLimit,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
    for (auto txStack : txStacks)
    {
        queue.add(txStack);
    }
    std::vector<TransactionFrameBasePtr> txs;
    auto visitor = [&txs](TxStack const& txStack) {
        txs.push_back(txStack.getTopTx());
        return VisitTxStackResult::TX_PROCESSED;
    };
    uint32_t opsLeftUntilLimit;
    queue.popTopTxs(/* allowGaps */ true, visitor, opsLeftUntilLimit);
    return txs;
}

void
SurgePricingPriorityQueue::visitTopTxs(
    std::vector<TxStackPtr> const& txStacks, uint32_t opsLimit,
    size_t comparisonSeed,
    std::function<VisitTxStackResult(TxStack const&)> const& visitor,
    uint32_t& opsLeftUntilLimit)
{
    SurgePricingPriorityQueue queue(/* isHighestPriority */ true, opsLimit,
                                    comparisonSeed);
    for (auto txStack : txStacks)
    {
        queue.add(txStack);
    }
    queue.popTopTxs(/* allowGaps */ false, visitor, opsLeftUntilLimit);
}

void
SurgePricingPriorityQueue::add(TxStackPtr txStack)
{
    releaseAssert(txStack != nullptr);
    bool inserted = mTxStackSet.insert(txStack).second;
    if (inserted)
    {
        mOpsCount += txStack->getNumOperations();
    }
}

void
SurgePricingPriorityQueue::erase(TxStackPtr txStack)
{
    releaseAssert(txStack != nullptr);
    auto it = mTxStackSet.find(txStack);
    if (it != mTxStackSet.end())
    {
        erase(it);
    }
}

void
SurgePricingPriorityQueue::erase(
    SurgePricingPriorityQueue::TxStackSet::iterator iter)
{
    auto ops = (*iter)->getNumOperations();
    mOpsCount -= ops;
    mTxStackSet.erase(iter);
}

void
SurgePricingPriorityQueue::popTopTxs(
    bool allowGaps,
    std::function<VisitTxStackResult(TxStack const&)> const& visitor,
    uint32_t& opsLeftUntilLimit)
{
    opsLeftUntilLimit = mOpsLimit;

    while (!mTxStackSet.empty())
    {
        auto currIt = getTop();
        auto const& currTx = *(*currIt)->getTopTx();
        auto currOps = currTx.getNumOperations();
        if (currOps > opsLeftUntilLimit)
        {
            if (allowGaps)
            {
                erase(currIt);
                continue;
            }
            else
            {
                break;
            }
        }

        // At this point, `currIt` points at the top transaction in the queue
        // that is still within operation limits, so we can visit it and remove
        // it from the queue.
        auto const& txStack = *currIt;
        auto visitRes = visitor(*txStack);
        auto ops = txStack->getTopTx()->getNumOperations();
        // Only account for operation counts when transaction was actually
        // processed by the visitor.
        if (visitRes == VisitTxStackResult::TX_PROCESSED)
        {
            opsLeftUntilLimit -= ops;
        }
        // Pop the tx from current iterator, unless the whole tx stack is
        // skipped.
        if (visitRes == VisitTxStackResult::TX_STACK_SKIPPED)
        {
            erase(currIt);
        }
        else
        {
            popTopTx(currIt);
        }
    }
}

std::pair<bool, int64_t>
SurgePricingPriorityQueue::canFitWithEviction(
    TransactionFrameBase const& tx, uint32_t txOpsDiscount,
    std::vector<TxStackPtr>& txStacksToEvict) const
{
    // This only makes sense when the lowest fee rate tx is on top.
    releaseAssert(!mComparator.isGreater());

    if (tx.getNumOperations() < txOpsDiscount)
    {
        throw std::invalid_argument(
            "Discount shouldn't be larger than transaction operations count");
    }
    auto txNewOps = tx.getNumOperations() - txOpsDiscount;
    if (txNewOps > mOpsLimit)
    {
        CLOG_WARNING(
            Herder,
            "Operation limit is lower size than transaction ops: {} < {}."
            "Node won't be able to accept all transactions.",
            mOpsLimit, txNewOps);
        return std::make_pair(false, 0ll);
    }
    uint32_t totalOps = sizeOps();
    uint32_t newTotalOps = totalOps + txNewOps;
    bool fitWithoutEviction = newTotalOps <= mOpsLimit;
    // If there is enough space, return.
    if (fitWithoutEviction)
    {
        return std::make_pair(true, 0ll);
    }
    auto iter = getTop();
    int64_t neededTotalOps =
        std::max<int64_t>(0, static_cast<int64_t>(newTotalOps) - mOpsLimit);
    // The above checks should ensure there are some operations that need to be
    // evicted.
    releaseAssert(neededTotalOps > 0);
    while (neededTotalOps > 0)
    {
        // The preconditions are ensured above that we should be able to fit
        // the transaction by evicting some transactions (given high enough
        // fee).
        releaseAssert(iter != mTxStackSet.end());
        auto const& evictTxStack = *iter;
        auto const& evictTx = *evictTxStack->getTopTx();
        auto evictOps = evictTxStack->getNumOperations();
        // Only support this for single tx stacks (eventually we should only
        // have such stacks and share this invariant across all the queue
        // operations).
        releaseAssert(evictOps == evictTx.getNumOperations());

        // Evicted tx must have a strictly lower fee than the new tx.
        if (!mComparator.compareFeeOnly(evictTx, tx))
        {
            auto minFee = computeBetterFee(tx, evictTx.getFeeBid(),
                                           evictTx.getNumOperations());
            return std::make_pair(false, minFee);
        }
        // Ensure that this transaction is not from the same account.
        if (tx.getSourceID() == evictTx.getSourceID())
        {
            return std::make_pair(false, 0ll);
        }

        txStacksToEvict.emplace_back(evictTxStack);
        neededTotalOps -= evictOps;
        if (neededTotalOps <= 0)
        {
            return std::make_pair(true, 0ll);
        }

        ++iter;
    }
    // The preconditions must guarantee that we find an evicted transaction set
    // or report that the tx fee is too low.
    releaseAssert(false);
}

SurgePricingPriorityQueue::TxStackSet::iterator
SurgePricingPriorityQueue::getTop() const
{
    return mTxStackSet.begin();
}

uint32_t
SurgePricingPriorityQueue::sizeOps() const
{
    return mOpsCount;
}

void
SurgePricingPriorityQueue::popTopTx(
    SurgePricingPriorityQueue::TxStackSet::iterator iter)
{
    auto txStack = *iter;
    erase(iter);
    txStack->popTopTx();
    if (!txStack->empty())
    {
        add(txStack);
    }
}
} // namespace stellar
