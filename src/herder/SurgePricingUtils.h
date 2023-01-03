#pragma once

// Copyright 2021 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include <set>

#include "transactions/TransactionFrameBase.h"

namespace stellar
{
// Compare fee/numOps between `l` and `r`.
// Returns -1 if `l` is strictly less than `r`, `1` if it's strictly greater and
// 0 when both are equal.
int feeRate3WayCompare(int64_t lFeeBid, uint32_t lNbOps, int64_t rFeeBid,
                       uint32_t rNbOps);

// Compute the fee bid that `tx` should have in order to beat
// a transaction `ref` with fee bid `refFeeBid` and `refNbOps` operations.
int64_t computeBetterFee(TransactionFrameBase const& tx, int64_t refFeeBid,
                         uint32_t refNbOps);

// Interface for a stack-like container holding transactions.
// This needs to be implemented by any user of `SurgePricingPriorityQueue`, as
// it operates on stacks and not bare transactions.
//
// This interface is not strictly a stack as it just defines the 'pop' operation
// and leaves the element additions up to the implementer. So the naming here is
// mostly to be in contrast with `SurgePricingPriorityQueue`.
class TxStack
{
  public:
    // Gets the transaction on top of the stack.
    virtual TransactionFrameBasePtr getTopTx() const = 0;
    // Pops the transaction from top of the stack.
    virtual void popTopTx() = 0;
    // Returns the total number of operations in the stack.
    virtual uint32_t getNumOperations() const = 0;
    // Returns whether this stack is empty.
    virtual bool empty() const = 0;

    virtual ~TxStack() = default;
};

using TxStackPtr = std::shared_ptr<TxStack>;

// Configuration for multi-lane transaction limiting and surge pricing.
//
// This configuration defines how many 'lanes' are there to compare and limit
// transactions.
//
// The configuration has the following semantics:
// - There exists at least one 'lane' for transactions.
// - Each transaction belongs to exactly one lane.
// - Every lane has an operation limit associated with it.
// - The lane '0' is always considered to be 'generic' lane. It defines the
//   maximum total number of operations allowed, i.e. transactions from *all*
//   the lanes must fit into it. Transactions from this lane can take place of
//   transactions from any other lane.
// - Lanes '1' and following are considered to be 'limited'. Besides fitting
//   into 'generic' lane, transactions from these lanes must also fit into
//   their lane's limit. Also transactions from 'limited' lanes may have their
//   own base fee that is not lower than the base fee for 'generic' lane.
//
// To summarize, this config defines the following invariants for some set of
// transactions:
// - `sum(tx.operations) <= laneOpsLimits[0]`
// - `for each lane >= 1: sum(tx[lane].operations) <= laneOpsLimits[lane]`
class SurgePricingLaneConfig
{
  public:
    using TxLaneClassifier =
        std::function<size_t(TransactionFrameBase const& tx)>;

    // Returns a a lane the transaction belongs to.
    virtual size_t getLane(TransactionFrameBase const& tx) const = 0;
    // Returns per-lane operation limits.
    virtual std::vector<uint32_t> const& getLaneOpsLimits() const = 0;
    // Updates the limit of the generic lane. This is needed due to
    // protocol upgrades (other lane limits are currently updated via
    // configuration).
    virtual void updateGenericLaneLimit(uint32_t limit) = 0;

    virtual ~SurgePricingLaneConfig() = default;
};

// Lane configuration that optionally defines a limited lane for transactions
// that have DEX operations (manager offer, path payments).
class DexLimitingLaneConfig : public SurgePricingLaneConfig
{
  public:
    // Index of the DEX limited lane.
    static constexpr size_t DEX_LANE = 1;

    // Creates the config. `opsLimit` is the total number of operations allowed
    // (lane '0'). `dexOpsLimit` when non-`nullopt` defines a limited lane for
    // transactions with DEX operations (otherwise only one lane is created).
    DexLimitingLaneConfig(uint32_t opsLimit,
                          std::optional<uint32_t> dexOpsLimit);

    size_t getLane(TransactionFrameBase const& tx) const override;
    std::vector<uint32_t> const& getLaneOpsLimits() const override;
    virtual void updateGenericLaneLimit(uint32_t limit) override;

  private:
    size_t getTxLane(TransactionFrameBase const& tx) const;

    std::vector<uint32_t> mLaneOpsLimits;
};

// Priority queue-like data structure that allows to store transactions ordered
// by fee rate and perform operations that respect the lane limits specified by
// `SurgePricingLaneConfig`.
//
// The queue operates on transaction stacks and only the top transactions of the
// stacks are compared. This allows to e.g. order transactions while preserving
// the seq num order.
class SurgePricingPriorityQueue
{
  public:
    // Index of the 'generic' lane. This has to be '0' and is used for clarity.
    static constexpr size_t GENERIC_LANE = 0;

    // Helper that uses `SurgePricingPriorityQueue` to greedily select the
    // maximum subset of transactions from `txStacks` with maximum fee ratios
    // within the limits specified by `laneConfig`.
    // The greedy ordering optimizes for the maximal fee ratio first, then for
    // the output operation count.
    // Transactions will be popped from the input `txStacks`, so after this call
    // `txStacks` will contain all the remaining transactions.
    // `hadTxNotFittingLane` is an output parameter that for every lane will
    // identify whether there was a transaction that didn't fit into that lane's
    // limit.
    static std::vector<TransactionFrameBasePtr> getMostTopTxsWithinLimits(
        std::vector<TxStackPtr> const& txStacks,
        std::shared_ptr<SurgePricingLaneConfig> laneConfig,
        std::vector<bool>& hadTxNotFittingLane);

    // Result of visiting the transaction stack in the `visitTopTxs`.
    // This serves as a callback output to let the queue know how to process the
    // visited stack.
    enum class VisitTxStackResult
    {
        // Top transaction of the stack should be popped, but not counted
        // towards the lane limits.
        TX_SKIPPED,
        // Top transaction of the stack should be popped and counted towards the
        // lane limits.
        TX_PROCESSED,
        // The whole stack should be skipped (no transactions are popped or
        // counted towards the lane limits).
        TX_STACK_SKIPPED
    };

    // Helper that uses `SurgePricingPriorityQueue` to visit the top (by fee
    // rate) transactions in the `txStacks` until `laneConfig` limits are
    // reached. The visiting process will end for a lane as soon as there is a
    // transaction that causes the limit to be exceeded.
    // `comparisonSeed` is used to break the comparison ties.
    // `visitor` should process the `TxStack` and provide an action to do with
    // that stack.
    // `laneOpsLeftUntilLimit` is an output parameter that for each lane will
    // contain the number of operations left until lane's limit is reached.
    static void visitTopTxs(
        std::vector<TxStackPtr> const& txStacks,
        std::shared_ptr<SurgePricingLaneConfig> laneConfig,
        size_t comparisonSeed,
        std::function<VisitTxStackResult(TxStack const&)> const& visitor,
        std::vector<uint32_t>& laneOpsLeftUntilLimit);

    // Creates a `SurgePricingPriorityQueue` for the provided lane
    // configuration.
    // `isHighestPriority` defines the comparison order: when it's `true` the
    // highest fee rate transactions are considered to be at the top, otherwise
    // the lowest fee rate transaction is at the top.
    SurgePricingPriorityQueue(
        bool isHighestPriority,
        std::shared_ptr<SurgePricingLaneConfig> laneConfig,
        size_t comparisonSeed);

    // Adds a `TxStack` to this queue. The queue has ownership of the stack and
    // may pop transactions from it.
    void add(TxStackPtr txStack);
    // Erases a `TxStack` from this queue.
    void erase(TxStackPtr txStack);

    // Checks whether a provided transaction could fit into this queue without
    // violating the `laneConfig` limits while evicting some lower fee rate
    // `TxStacks` from the queue.
    // Returns whether transaction can be fit and if not, returns the minimum
    // required fee to possibly fit.
    // `txOpsDiscount` is a number of operations to subtract from tx's
    // operations when estimating the total operation counts.
    // `txStacksToEvict` is an output parameter that will contain all the stacks
    // that need to be evicted in order to fit `tx`. The `bool` argument
    // indicates whether this `TxStack` has been evicted due to lane's limit (as
    // opposed to 'generic' lane's limit).
    std::pair<bool, int64_t> canFitWithEviction(
        TransactionFrameBase const& tx, uint32_t txOpsDiscount,
        std::vector<std::pair<TxStackPtr, bool>>& txStacksToEvict) const;

    // Returns total number of operations in all the stacks in this queue.
    uint32_t sizeOps() const;

    // Returns total number of operations in the provided lane of the queue.
    uint32_t laneOps(size_t lane) const;

  private:
    class TxStackComparator
    {
      public:
        TxStackComparator(bool isGreater, size_t seed);

        bool operator()(TxStackPtr const& txStack1,
                        TxStackPtr const& txStack2) const;

        bool compareFeeOnly(TransactionFrameBase const& tx1,
                            TransactionFrameBase const& tx2) const;
        bool compareFeeOnly(int64_t tx1Bid, uint32_t tx1Ops, int64_t tx2Bid,
                            uint32_t tx2Ops) const;
        bool isGreater() const;

      private:
        bool txStackLessThan(TxStack const& txStack1,
                             TxStack const& txStack2) const;

        bool txLessThan(TransactionFrameBaseConstPtr const& tx1,
                        TransactionFrameBaseConstPtr const& tx2,
                        bool breakTiesWithHash) const;

        bool const mIsGreater;
        size_t mSeed;
    };

    using TxStackSet = std::set<TxStackPtr, TxStackComparator>;
    using LaneIter = std::pair<size_t, TxStackSet::iterator>;

    // Iterator for walking the queue from top to bottom, possibly restricted
    // only to some lanes. The actual ordering is defined by
    // `isHighestPriority`.
    class Iterator
    {
      public:
        Iterator(SurgePricingPriorityQueue const& parent,
                 std::vector<LaneIter> const& iters);

        TxStackPtr operator*() const;
        // Gets the iterator of the `TxStackSet` corresponding to the current
        // value.
        LaneIter getInnerIter() const;
        bool isEnd() const;
        // Advances this iterator to the next value.
        void advance();
        // Removes a lane corresponding to the current value of the iterator.
        void dropLane();

      private:
        std::vector<LaneIter>::iterator getMutableInnerIter() const;

        SurgePricingPriorityQueue const& mParent;
        std::vector<LaneIter> mutable mIters;
    };

    // Generalized method for visiting and popping the top transactions in the
    // queue until the lane limits are reached.
    // This leaves the queue empty (when `allowGaps` is `true`) and hence is
    // only used from the helper methods that don't expose the queue at all.
    void
    popTopTxs(bool allowGaps,
              std::function<VisitTxStackResult(TxStack const&)> const& visitor,
              std::vector<uint32_t>& laneOpsLeftUntilLimit,
              std::vector<bool>& hadTxNotFittingLane);

    void erase(Iterator const& it);
    void erase(size_t lane,
               SurgePricingPriorityQueue::TxStackSet::iterator iter);
    void popTopTx(Iterator iter);

    Iterator getTop() const;

    TxStackComparator const mComparator;
    std::shared_ptr<SurgePricingLaneConfig> mLaneConfig;
    std::vector<uint32_t> const& mLaneOpsLimits;

    std::vector<uint32_t> mLaneOpsCount;

    std::vector<TxStackSet> mTxStackSets;
};

} // namespace stellar
