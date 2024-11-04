// Copyright 2014 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/SurgePricingUtils.h"
#include "crypto/SecretKey.h"
#include "util/Logging.h"
#include "util/numeric128.h"
#include "util/types.h"
#include <Tracy.hpp>
#include <numeric>

namespace stellar
{
namespace
{

// Use _inclusion_ fee to order transactions
int
feeRate3WayCompare(TransactionFrameBase const& l, TransactionFrameBase const& r)
{
    return stellar::feeRate3WayCompare(
        l.getInclusionFee(), l.getNumOperations(), r.getInclusionFee(),
        r.getNumOperations());
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

SurgePricingPriorityQueue::TxComparator::TxComparator(bool isGreater,
                                                      size_t seed)
    : mIsGreater(isGreater), mSeed(seed)
{
}

bool
SurgePricingPriorityQueue::TxComparator::operator()(
    TransactionFrameBasePtr const& tx1,
    TransactionFrameBasePtr const& tx2) const
{
    return txLessThan(tx1, tx2) ^ mIsGreater;
}

bool
SurgePricingPriorityQueue::TxComparator::compareFeeOnly(
    TransactionFrameBase const& tx1, TransactionFrameBase const& tx2) const
{
    return compareFeeOnly(tx1.getInclusionFee(), tx1.getNumOperations(),
                          tx2.getInclusionFee(), tx2.getNumOperations());
}

bool
SurgePricingPriorityQueue::TxComparator::compareFeeOnly(int64_t tx1Bid,
                                                        uint32_t tx1Ops,
                                                        int64_t tx2Bid,
                                                        uint32_t tx2Ops) const
{
    bool isLess = feeRate3WayCompare(tx1Bid, tx1Ops, tx2Bid, tx2Ops) < 0;
    return isLess ^ mIsGreater;
}

bool
SurgePricingPriorityQueue::TxComparator::isGreater() const
{
    return mIsGreater;
}

bool
SurgePricingPriorityQueue::TxComparator::txLessThan(
    TransactionFrameBasePtr const& tx1,
    TransactionFrameBasePtr const& tx2) const
{
    auto cmp3 = feeRate3WayCompare(*tx1, *tx2);

    if (cmp3 != 0)
    {
        return cmp3 < 0;
    }
#ifndef BUILD_TESTS
    // break tie with pointer arithmetic
    auto lx = reinterpret_cast<size_t>(tx1.get()) ^ mSeed;
    auto rx = reinterpret_cast<size_t>(tx2.get()) ^ mSeed;
#else
    // Sort transactions deterministically in tests in order to ensure
    // reproducibility.
    auto lx = tx1->getFullHash();
    auto rx = tx2->getFullHash();
#endif
    return lx < rx;
}

SurgePricingPriorityQueue::SurgePricingPriorityQueue(
    bool isHighestPriority, std::shared_ptr<SurgePricingLaneConfig> settings,
    size_t comparisonSeed)
    : mComparator(isHighestPriority, comparisonSeed)
    , mLaneConfig(settings)
    , mLaneLimits(mLaneConfig->getLaneLimits())
    , mTxSortedSets(mLaneLimits.size(), TxSortedSet(mComparator))
{
    releaseAssert(!mLaneLimits.empty());
    mLaneCurrentCount = std::vector<Resource>(
        mLaneLimits.size(),
        Resource(std::vector<int64_t>(mLaneLimits[0].size(), 0)));
}

std::vector<TransactionFrameBasePtr>
SurgePricingPriorityQueue::getMostTopTxsWithinLimits(
    std::vector<TransactionFrameBasePtr> const& txs,
    std::shared_ptr<SurgePricingLaneConfig> laneConfig,
    std::vector<bool>& hadTxNotFittingLane)
{
    ZoneScoped;

    SurgePricingPriorityQueue queue(
        /* isHighestPriority */ true, laneConfig,
        stellar::rand_uniform<size_t>(0, std::numeric_limits<size_t>::max()));
    for (auto const& tx : txs)
    {
        queue.add(tx);
    }
    std::vector<TransactionFrameBasePtr> outTxs;
    auto visitor = [&outTxs](TransactionFrameBasePtr const& tx) {
        outTxs.push_back(tx);
        return VisitTxResult::PROCESSED;
    };
    std::vector<Resource> laneLeftUntilLimit;
    queue.popTopTxs(/* allowGaps */ true, visitor, laneLeftUntilLimit,
                    hadTxNotFittingLane);
    return outTxs;
}

void
SurgePricingPriorityQueue::visitTopTxs(
    std::vector<TransactionFrameBasePtr> const& txs,
    std::function<VisitTxResult(TransactionFrameBasePtr const&)> const& visitor,
    std::vector<Resource>& laneLeftUntilLimit)
{
    ZoneScoped;

    for (auto const& tx : txs)
    {
        add(tx);
    }
    std::vector<bool> hadTxNotFittingLane;
    popTopTxs(/* allowGaps */ false, visitor, laneLeftUntilLimit,
              hadTxNotFittingLane);
}

void
SurgePricingPriorityQueue::add(TransactionFrameBasePtr tx)
{
    releaseAssert(tx != nullptr);
    auto lane = mLaneConfig->getLane(*tx);
    bool inserted = mTxSortedSets[lane].insert(tx).second;
    if (inserted)
    {
        mLaneCurrentCount[lane] += mLaneConfig->getTxResources(*tx);
    }
}

void
SurgePricingPriorityQueue::erase(TransactionFrameBasePtr tx)
{
    releaseAssert(tx != nullptr);
    auto lane = mLaneConfig->getLane(*tx);
    auto it = mTxSortedSets[lane].find(tx);
    if (it != mTxSortedSets[lane].end())
    {
        erase(lane, it);
    }
}

void
SurgePricingPriorityQueue::erase(Iterator const& it)
{
    auto innerIt = it.getInnerIter();
    erase(innerIt.first, innerIt.second);
}

void
SurgePricingPriorityQueue::erase(
    size_t lane, SurgePricingPriorityQueue::TxSortedSet::iterator iter)
{
    auto res = mLaneConfig->getTxResources(*(*iter));
    releaseAssert(res <= mLaneCurrentCount[lane]);
    mLaneCurrentCount[lane] -= res;
    mTxSortedSets[lane].erase(iter);
}

void
SurgePricingPriorityQueue::popTopTxs(
    bool allowGaps,
    std::function<VisitTxResult(TransactionFrameBasePtr const&)> const& visitor,
    std::vector<Resource>& laneLeftUntilLimit,
    std::vector<bool>& hadTxNotFittingLane)
{
    ZoneScoped;

    laneLeftUntilLimit = mLaneLimits;
    hadTxNotFittingLane.assign(mLaneLimits.size(), false);
    while (true)
    {
        auto currIt = getTop();
        bool gapSkipped = false;
        bool canPopSomeTx = true;

        // Try to find a lane in the top iterator that hasn't reached limit yet.
        while (!currIt.isEnd())
        {
            auto const& currTx = *(*currIt);
            auto curr = mLaneConfig->getTxResources(currTx);
            auto lane = mLaneConfig->getLane(currTx);
            if (anyGreater(curr, laneLeftUntilLimit[lane]) ||
                anyGreater(curr, laneLeftUntilLimit[GENERIC_LANE]))
            {
                if (allowGaps)
                {
                    // If gaps are allowed we just erase the iterator and
                    // continue in the main loop.
                    gapSkipped = true;
                    erase(currIt);
                    if (anyGreater(curr, laneLeftUntilLimit[lane]))
                    {
                        hadTxNotFittingLane[lane] = true;
                    }
                    else
                    {
                        hadTxNotFittingLane[GENERIC_LANE] = true;
                    }
                    break;
                }
                else
                {
                    // If gaps are not allowed, but we only reach the limit for
                    // the 'limited' lane, then we still can find some
                    // transactions in the other lanes that would fit into
                    // 'generic' and their own lanes.
                    // We don't break the 'no gaps' invariant by doing so -
                    // every lane stops being iterated over as soon as
                    // non-fitting tx is found.
                    if (lane != GENERIC_LANE &&
                        anyGreater(curr, laneLeftUntilLimit[lane]))
                    {
                        currIt.dropLane();
                    }
                    else
                    {
                        canPopSomeTx = false;
                        break;
                    }
                }
            }
            else
            {
                break;
            }
        }
        if (gapSkipped)
        {
            continue;
        }
        if (!canPopSomeTx || currIt.isEnd())
        {
            break;
        }
        // At this point, `currIt` points at the top transaction in the queue
        // (within the allowed lanes) that is still within resource limits, so
        // we can visit it and remove it from the queue.
        auto const& tx = *currIt;
        auto visitRes = visitor(tx);
        auto res = mLaneConfig->getTxResources(*tx);
        auto lane = mLaneConfig->getLane(*tx);
        // Only account for resource counts when transaction was actually
        // processed by the visitor.
        if (visitRes == VisitTxResult::PROCESSED)
        {
            // 'Generic' lane's limit is shared between all the transactions.
            // Resources left must be greater than or equal to the tx subtracted
            // (enforced by -= operator)
            laneLeftUntilLimit[GENERIC_LANE] -= res;
            if (lane != GENERIC_LANE)
            {
                laneLeftUntilLimit[lane] -= res;
            }
        }
        erase(currIt);
    }
}

std::pair<bool, int64_t>
SurgePricingPriorityQueue::canFitWithEviction(
    TransactionFrameBase const& tx, std::optional<Resource> txDiscount,
    std::vector<std::pair<TransactionFrameBasePtr, bool>>& txsToEvict) const
{
    ZoneScoped;

    // This only makes sense when the lowest fee rate tx is on top.
    releaseAssert(!mComparator.isGreater());

    auto lane = mLaneConfig->getLane(tx);
    auto txNewResources = mLaneConfig->getTxResources(tx);
    if (txDiscount)
    {
        txNewResources = subtractNonNegative(txNewResources, *txDiscount);
    }

    if (anyGreater(txNewResources, mLaneLimits[GENERIC_LANE]) ||
        anyGreater(txNewResources, mLaneLimits[lane]))
    {
        CLOG_WARNING(
            Herder,
            "Transaction fee lane has lower size than transaction : {} < "
            "{}. Node won't be able to accept all transactions.",
            txNewResources > mLaneLimits[GENERIC_LANE]
                ? mLaneLimits[GENERIC_LANE].toString()
                : mLaneLimits[lane].toString(),
            txNewResources.toString());
        return std::make_pair(false, 0ll);
    }
    Resource total = totalResources();

    if (!total.canAdd(txNewResources) ||
        !(mLaneCurrentCount[lane].canAdd(txNewResources)))
    {
        return std::make_pair(false, 0ll);
    }

    Resource newTotalResources = total + txNewResources;
    Resource newLaneResources = mLaneCurrentCount[lane] + txNewResources;
    // To fit the eviction, tx has to both fit into 'generic' lane's limit
    // (shared among all the txs) and its own lane's limit.
    releaseAssert(mLaneCurrentCount[lane] <= total);
    releaseAssert(mLaneLimits[lane] <= mLaneLimits[GENERIC_LANE]);
    bool fitWithoutEviction = newTotalResources <= mLaneLimits[GENERIC_LANE] &&
                              newLaneResources <= mLaneLimits[lane];
    // If there is enough space, return.
    if (fitWithoutEviction)
    {
        return std::make_pair(true, 0ll);
    }
    auto iter = getTop();

    Resource neededTotal =
        subtractNonNegative(newTotalResources, mLaneLimits[GENERIC_LANE]);
    Resource neededLane =
        subtractNonNegative(newLaneResources, mLaneLimits[lane]);

    // The above checks should ensure there are some operations that need to be
    // evicted.
    releaseAssert(neededTotal.anyPositive() || neededLane.anyPositive());
    while (neededTotal.anyPositive() || neededLane.anyPositive())
    {
        bool evictedDueToLaneLimit = false;
        while (!iter.isEnd())
        {
            // This is the cheapest transaction
            auto const& evictTx = *(*iter);
            auto evictLane = mLaneConfig->getLane(evictTx);
            bool canEvict = false;
            // Check if it makes sense to evict the current top tx.
            //
            // Simple cases: generic lane txs can evict anything; same lane txs
            // can evict each other; any transaction can be evicted if per-lane
            // limit hasn't been reached; any transaction can be evicted if
            // there is not enough resources to evict from limited lane to fit
            // the current transaction
            if (lane == GENERIC_LANE || lane == evictLane ||
                anyGreater(neededTotal, neededLane))
            {
                canEvict = true;
            }
            else
            {
                // When we need to evict more  from the tx's lane than from
                // the generic lane, then we can only evict from tx's lane (any
                // other evictions are pointless).
                evictedDueToLaneLimit = true;
            }
            if (!canEvict)
            {
                // If we couldn't evict a cheaper transaction from a lane, then
                // we shouldn't evict more expensive ones (even though some of
                // them could be evicted following the `canEvict` logic above).
                iter.dropLane();
            }
            else
            {
                break;
            }
        }
        // The preconditions are ensured above that we should be able to fit
        // the transaction by evicting some transactions.
        releaseAssert(!iter.isEnd());
        auto const& evictTx = *(*iter);
        auto evict = mLaneConfig->getTxResources(evictTx);
        auto evictLane = mLaneConfig->getLane(evictTx);

        // Evicted tx must have a strictly lower fee than the new tx.
        // NB: this logic works with properly with Soroban transactions as well,
        // since those are guaranteed to contain 1 op, therefore fee-per-op
        // computation is a no-op.
        if (!mComparator.compareFeeOnly(evictTx, tx))
        {
            auto minFee = computeBetterFee(tx, evictTx.getInclusionFee(),
                                           evictTx.getNumOperations());
            return std::make_pair(
                false, minFee + (tx.getFullFee() - tx.getInclusionFee()));
        }
        // Ensure that this transaction is not from the same account.
        if (tx.getSourceID() == evictTx.getSourceID())
        {
            return std::make_pair(false, 0ll);
        }

        txsToEvict.emplace_back(*iter, evictedDueToLaneLimit);
        neededTotal = subtractNonNegative(neededTotal, evict);
        // Only reduce the needed lane when we evict from tx's lane or when
        // we're trying to fit a 'generic' lane tx (as it can evict any other
        // tx).
        if (lane == GENERIC_LANE || lane == evictLane)
        {
            neededLane = subtractNonNegative(neededLane, evict);
        }
        if (!neededTotal.anyPositive() && !neededLane.anyPositive())
        {
            return std::make_pair(true, 0ll);
        }

        iter.advance();
    }
    releaseAssert(false);
}

SurgePricingPriorityQueue::Iterator
SurgePricingPriorityQueue::getTop() const
{
    std::vector<LaneIter> iters;
    for (size_t lane = 0; lane < mTxSortedSets.size(); ++lane)
    {
        auto const& txSet = mTxSortedSets[lane];
        if (txSet.empty())
        {
            continue;
        }
        iters.emplace_back(lane, txSet.begin());
    }
    return SurgePricingPriorityQueue::Iterator(*this, iters);
}

Resource
SurgePricingPriorityQueue::totalResources() const
{
    releaseAssert(!mLaneCurrentCount.empty());
    auto resourceCount = mLaneCurrentCount.begin()->size();
    Resource res(std::vector<int64_t>(resourceCount, 0));
    return std::accumulate(mLaneCurrentCount.begin(), mLaneCurrentCount.end(),
                           res);
}

Resource
SurgePricingPriorityQueue::laneResources(size_t lane) const
{
    releaseAssert(lane < mLaneCurrentCount.size());
    return mLaneCurrentCount[lane];
}

SurgePricingPriorityQueue::Iterator::Iterator(
    SurgePricingPriorityQueue const& parent, std::vector<LaneIter> const& iters)
    : mParent(parent), mIters(iters)
{
}

std::vector<SurgePricingPriorityQueue::LaneIter>::iterator
SurgePricingPriorityQueue::Iterator::getMutableInnerIter() const
{
    releaseAssert(!isEnd());
    auto best = mIters.begin();
    for (auto groupIt = std::next(mIters.begin()); groupIt != mIters.end();
         ++groupIt)
    {
        auto& [group, it] = *groupIt;
        if (mParent.mComparator(*it, *best->second))
        {
            best = groupIt;
        }
    }
    return best;
}

TransactionFrameBasePtr
SurgePricingPriorityQueue::Iterator::operator*() const
{
    return *getMutableInnerIter()->second;
}

SurgePricingPriorityQueue::LaneIter
SurgePricingPriorityQueue::Iterator::getInnerIter() const
{
    return *getMutableInnerIter();
}

bool
SurgePricingPriorityQueue::Iterator::isEnd() const
{
    return mIters.empty();
}

void
SurgePricingPriorityQueue::Iterator::advance()
{
    auto it = getMutableInnerIter();
    ++it->second;
    if (it->second == mParent.mTxSortedSets[it->first].end())
    {
        mIters.erase(it);
    }
}

void
SurgePricingPriorityQueue::Iterator::dropLane()
{
    mIters.erase(getMutableInnerIter());
}

DexLimitingLaneConfig::DexLimitingLaneConfig(Resource Limit,
                                             std::optional<Resource> dexLimit)
    : mUseByteLimit(Limit.size() == NUM_CLASSIC_TX_BYTES_RESOURCES)
{
    mLaneLimits.push_back(Limit);
    if (dexLimit)
    {
        mLaneLimits.push_back(*dexLimit);
    }
}

std::vector<Resource> const&
DexLimitingLaneConfig::getLaneLimits() const
{
    return mLaneLimits;
}

void
DexLimitingLaneConfig::updateGenericLaneLimit(Resource const& limit)
{
    mLaneLimits[0] = limit;
}

Resource
DexLimitingLaneConfig::getTxResources(TransactionFrameBase const& tx)
{
    releaseAssert(!tx.isSoroban());
    return tx.getResources(mUseByteLimit);
}

size_t
DexLimitingLaneConfig::getLane(TransactionFrameBase const& tx) const
{
    if (mLaneLimits.size() > DexLimitingLaneConfig::DEX_LANE &&
        tx.hasDexOperations())
    {
        return DexLimitingLaneConfig::DEX_LANE;
    }
    else
    {
        return SurgePricingPriorityQueue::GENERIC_LANE;
    }
}

SorobanGenericLaneConfig::SorobanGenericLaneConfig(Resource limit)
{
    mLaneLimits.push_back(limit);
}

size_t
SorobanGenericLaneConfig::getLane(TransactionFrameBase const& tx) const
{
    if (!tx.isSoroban())
    {
        throw std::runtime_error("Invalid tx: non-Soroban");
    }

    return SurgePricingPriorityQueue::GENERIC_LANE;
}

std::vector<Resource> const&
SorobanGenericLaneConfig::getLaneLimits() const
{
    return mLaneLimits;
}

void
SorobanGenericLaneConfig::updateGenericLaneLimit(Resource const& limit)
{
    mLaneLimits[0] = limit;
}

Resource
SorobanGenericLaneConfig::getTxResources(TransactionFrameBase const& tx)
{
    releaseAssert(tx.isSoroban());
    return tx.getResources(/* useByteLimitInClassic */ false);
}
} // namespace stellar
