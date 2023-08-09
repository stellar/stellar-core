// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "herder/TxQueueLimiter.h"
#include "herder/SurgePricingUtils.h"
#include "herder/TxSetFrame.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"

namespace stellar
{
namespace
{

int64_t
computeBetterFee(std::pair<int64, uint32_t> const& evictedBid,
                 TransactionFrameBase const& tx)
{
    if (evictedBid.second != 0 &&
        feeRate3WayCompare(evictedBid.first, evictedBid.second,
                           tx.getInclusionFee(), tx.getNumOperations()) >= 0)
    {
        return computeBetterFee(tx, evictedBid.first, evictedBid.second);
    }
    return 0;
}

}

TxQueueLimiter::TxQueueLimiter(uint32 multiplier, Application& app,
                               bool isSoroban)
    : mPoolLedgerMultiplier(multiplier)
    , mLedgerManager(app.getLedgerManager())
    , mApp(app)
    , mIsSoroban(isSoroban)
{
    auto maxDexOps = app.getConfig().MAX_DEX_TX_OPERATIONS_IN_TX_SET;
    if (maxDexOps && !mIsSoroban)
    {
        mMaxDexOperations =
            std::make_optional<Resource>(*maxDexOps * multiplier);
    }

    if (app.getConfig().LIMIT_TX_QUEUE_SOURCE_ACCOUNT)
    {
        mEnforceSingleAccounts =
            std::make_optional<std::unordered_set<AccountID>>();
    }
}

TxQueueLimiter::~TxQueueLimiter()
{
    // empty destructor allows deleting TxQueueLimiter from other source files
}

#ifdef BUILD_TESTS
size_t
TxQueueLimiter::size() const
{
    releaseAssert(!mIsSoroban);
    return mTxs->totalResources().getVal(Resource::Type::OPERATIONS);
}
#endif

Resource
TxQueueLimiter::maxScaledLedgerResources(bool isSoroban,
                                         AbstractLedgerTxn& ltxOuter) const
{
    return multiplyByDouble(
        mLedgerManager.maxLedgerResources(isSoroban, ltxOuter),
        mPoolLedgerMultiplier);
}

void
TxQueueLimiter::addTransaction(TransactionFrameBasePtr const& tx)
{
    releaseAssert(tx->isSoroban() == mIsSoroban);
    // First check invariance
    if (mEnforceSingleAccounts)
    {
        auto& accts = *mEnforceSingleAccounts;
        auto res = accts.emplace(tx->getSourceID());
        // Must be first time insertion
        if (!res.second)
        {
            throw std::logic_error(
                "invalid state (duplicate account) while adding tx in "
                "TxQueueLimiter");
        }
    }
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
    mStackForTx.erase(txStackIt);
    if (mEnforceSingleAccounts)
    {
        auto& accts = *mEnforceSingleAccounts;
        auto res = accts.erase(tx->getSourceID());
        // Must be present
        if (res == 0)
        {
            throw std::logic_error(
                "invalid state (missing account) while removing tx in "
                "TxQueueLimiter");
        }
    }
}

std::pair<bool, int64>
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx,
                         std::vector<std::pair<TxStackPtr, bool>>& txsToEvict)
{

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), /* shouldUpdateLastModified */ true,
                  TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);
    return canAddTx(newTx, oldTx, txsToEvict, ltx);
}

std::pair<bool, int64>
TxQueueLimiter::canAddTx(TransactionFrameBasePtr const& newTx,
                         TransactionFrameBasePtr const& oldTx,
                         std::vector<std::pair<TxStackPtr, bool>>& txsToEvict,
                         AbstractLedgerTxn& ltxOuter)
{
    releaseAssert(newTx);
    releaseAssert(newTx->isSoroban() == mIsSoroban);

    if (oldTx)
    {
        releaseAssert(oldTx->isSoroban() == newTx->isSoroban());
    }

    // We cannot normally initialize transaction queue in the constructor
    // because `maxQueueSizeOps()` may not be initialized. Hence we initialize
    // lazily during the add/reset.
    // Resetting both is fine here, as we always reset at the same time
    if (mTxs == nullptr)
    {
        reset(ltxOuter);
    }

    // If some transactions were evicted from this or generic lane, make sure
    // that the new transaction is better (even if it fits otherwise). This
    // guarantees that we don't replace transactions with higher bids with
    // transactions with lower bids and less operations.
    int64_t minInclusionFeeToBeatEvicted = std::max(
        computeBetterFee(
            mLaneEvictedInclusionFee[mSurgePricingLaneConfig->getLane(*newTx)],
            *newTx),
        computeBetterFee(
            mLaneEvictedInclusionFee[SurgePricingPriorityQueue::GENERIC_LANE],
            *newTx));
    // minInclusionFeeToBeatEvicted is the minimum _inclusion_ fee to evict txs.
    // For reporting, return _full_ minimum fee
    if (minInclusionFeeToBeatEvicted > 0)
    {
        return std::make_pair(
            false, minInclusionFeeToBeatEvicted +
                       (newTx->getFullFee() - newTx->getInclusionFee()));
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

    auto discount = newTx->isSoroban()
                        ? std::nullopt
                        : std::make_optional<Resource>(txOpsDiscount);
    // Update the operation limit in case upgrade happened. This is cheap
    // enough to happen unconditionally without relying on upgrade triggers.
    mSurgePricingLaneConfig->updateGenericLaneLimit(
        Resource(maxScaledLedgerResources(newTx->isSoroban(), ltxOuter)));
    return mTxs->canFitWithEviction(*newTx, discount, txsToEvict);
}

void
TxQueueLimiter::evictTransactions(
    std::vector<std::pair<TxStackPtr, bool>> const& txsToEvict,
    TransactionFrameBase const& txToFit,
    std::function<void(TransactionFrameBasePtr const&)> evict)
{
    auto resourcesToFit = txToFit.getResources();

    auto txToFitLane = mSurgePricingLaneConfig->getLane(txToFit);

    LedgerTxn ltx(mApp.getLedgerTxnRoot(), /* shouldUpdateLastModified */ true,
                  TransactionMode::READ_ONLY_WITHOUT_SQL_TXN);

    auto maxLimits = maxScaledLedgerResources(txToFit.isSoroban(), ltx);

    for (auto const& [evictedStack, evictedDueToLaneLimit] : txsToEvict)
    {
        auto tx = evictedStack->getTopTx();
        if (evictedDueToLaneLimit)
        {
            // If tx has been evicted due to lane limit, then all the following
            // txs in this lane have to beat it. However, other txs could still
            // fit with a lower fee.
            mLaneEvictedInclusionFee[mSurgePricingLaneConfig->getLane(*tx)] = {
                tx->getInclusionFee(), tx->getNumOperations()};
        }
        else
        {
            // If tx has been evicted before reaching the lane limit, we just
            // add it to generic lane, so that every new tx has to beat it.
            mLaneEvictedInclusionFee[SurgePricingPriorityQueue::GENERIC_LANE] =
                {tx->getInclusionFee(), tx->getNumOperations()};
        }

        evict(tx);
        // While we guarantee `txsToEvict` to have enough operations to fit new
        // operations, the eviction itself may remove transactions with high seq
        // nums and hence make space sooner than expected.
        if (mTxs->totalResources() + resourcesToFit <= maxLimits)
        {
            // If the tx is not in generic lane, then we need to make sure that
            // there is enough space in the respective limited lane.
            if (txToFitLane == SurgePricingPriorityQueue::GENERIC_LANE ||
                mTxs->laneResources(txToFitLane) + resourcesToFit <=
                    mSurgePricingLaneConfig->getLaneLimits()[txToFitLane])
            {
                break;
            }
        }
    }
    // It should be guaranteed to fit the required operations after the
    // eviction.
    releaseAssert(mTxs->totalResources() + resourcesToFit <= maxLimits);
}

void
TxQueueLimiter::reset(AbstractLedgerTxn& ltxOuter)
{
    if (mIsSoroban)
    {
        if (protocolVersionStartsFrom(
                ltxOuter.loadHeader().current().ledgerVersion,
                ProtocolVersion::V_20))
        {
            mSurgePricingLaneConfig =
                std::make_shared<SorobanGenericLaneConfig>(
                    maxScaledLedgerResources(mIsSoroban, ltxOuter));
        }
        else
        {
            releaseAssert(!mSurgePricingLaneConfig);
        }
    }
    else
    {
        mSurgePricingLaneConfig = std::make_shared<DexLimitingLaneConfig>(
            maxScaledLedgerResources(mIsSoroban, ltxOuter), mMaxDexOperations);
    }

    if (mSurgePricingLaneConfig)
    {
        mTxs = std::make_unique<SurgePricingPriorityQueue>(
            /* isHighestPriority */ false, mSurgePricingLaneConfig,
            stellar::rand_uniform<size_t>(0,
                                          std::numeric_limits<size_t>::max()));
    }

    mStackForTx.clear();
    if (mEnforceSingleAccounts)
    {
        mEnforceSingleAccounts->clear();
    }
    resetEvictionState();
}

void
TxQueueLimiter::resetEvictionState()
{
    if (mSurgePricingLaneConfig != nullptr)
    {
        mLaneEvictedInclusionFee.assign(
            mSurgePricingLaneConfig->getLaneLimits().size(), {0, 0});
    }
    else
    {
        releaseAssert(mLaneEvictedInclusionFee.empty());
    }
}
}
