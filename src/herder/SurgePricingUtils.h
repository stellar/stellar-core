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

// Priority queue-like data structure that allows to store transactions ordered
// by fee rate and perform operations that respect the operation count limits.
//
// The queue operates on transaction stacks and only the top transactions of the
// stacks are compared. This allows to e.g. order transactions while preserving
// the seq num order.
class SurgePricingPriorityQueue
{
  public:
    // Helper that uses `SurgePricingPriorityQueue` to greedily select the
    // maximum subset of transactions from `txStacks` with maximum fee ratios
    // such that the total operation count doesn't exceed `opsLimit`.
    // The greedy ordering optimizes for the maximal fee ratio first, then for
    // the output operation count.
    // Transactions will be popped from the input `txStacks`, so after this call
    // `txStacks` will contain all the remaining transactions.
    static std::vector<TransactionFrameBasePtr>
    getMostTopTxsWithinLimit(std::vector<TxStackPtr> const& txStacks,
                             uint32_t opsLimit);

    // Result of visiting the transaction stack in the `visitTopTxs`.
    // This serves as a callback output to let the queue know how to process the
    // visited stack.
    enum class VisitTxStackResult
    {
        // Top transaction of the stack should be popped, but not counted
        // towards the operation limit.
        TX_SKIPPED,
        // Top transaction of the stack should be popped and counted towards the
        // operation limits.
        TX_PROCESSED,
        // The whole stack should be skipped (no transactions are popped or
        // counted towards the operation limit).
        TX_STACK_SKIPPED
    };

    // Helper that uses `SurgePricingPriorityQueue` to visits the top (by fee
    // rate) transactions in the `txStacks` until `opsLimit` is reached.
    // The visiting process will end for a lane as soon as there is a
    // transaction that causes the limit to be exceeded.
    // `comparisonSeed` is used to break the comparison ties.
    // `visitor` should process the `TxStack` and provide an action to do with
    // that stack.
    // `opsLeftUntilLimit` is an output parameter that will contain the number
    // of operations left until the limit is reached.
    static void visitTopTxs(
        std::vector<TxStackPtr> const& txStacks, uint32_t opsLimit,
        size_t comparisonSeed,
        std::function<VisitTxStackResult(TxStack const&)> const& visitor,
        uint32_t& opsLeftUntilLimit);

    // Creates a `SurgePricingPriorityQueue` with the provided `opsLimit`.
    // `isHighestPriority` defines the comparison order: when it's `true` the
    // highest fee rate transactions are considered to be at the top, otherwise
    // the lowest fee rate transaction is at the top.
    SurgePricingPriorityQueue(bool isHighestPriority, uint32_t opsLimit,
                              size_t comparisonSeed);

    // Adds a `TxStack` to this queue. The queue has ownership of the stack and
    // may pop transactions from it.
    void add(TxStackPtr txStack);
    // Erases a `TxStack` from this queue.
    void erase(TxStackPtr txStack);

    // Checks whether a provided transaction could fit into this queue without
    // violating the ops limit while evicting some lower fee rate `TxStacks`
    // from the queue.
    // Returns whether transaction can be fit and if not, returns the minimum
    // required fee to possibly fit. `txOpsDiscount` is a number of operations
    // to subtract from tx's operations when estimating the total operation
    // counts. `txStacksToEvict` is an output parameter that will contain all
    // the stacks that need to be evicted in order to fit `tx`.
    std::pair<bool, int64_t>
    canFitWithEviction(TransactionFrameBase const& tx, uint32_t txOpsDiscount,
                       std::vector<TxStackPtr>& txStacksToEvict) const;

    // Returns total number of operations in all the stacks in this queue.
    uint32_t sizeOps() const;

    // Sets the queue ops limit to `newLimit`.
    void updateOpsLimit(uint32_t newLimit);

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

    // Generalized method for visiting and popping the top transactions in the
    // queue until the ops limit is reached.
    // This may leave the queue empty (when `allowGaps` is `true`) and hence is
    // only used from the helper methods that don't expose the queue at all.
    void
    popTopTxs(bool allowGaps,
              std::function<VisitTxStackResult(TxStack const&)> const& visitor,
              uint32_t& opsLeftUntilLimit);

    void erase(SurgePricingPriorityQueue::TxStackSet::iterator iter);
    void popTopTx(SurgePricingPriorityQueue::TxStackSet::iterator iter);
    SurgePricingPriorityQueue::TxStackSet::iterator getTop() const;

    TxStackComparator const mComparator;
    uint32_t mOpsLimit;
    uint32_t mOpsCount;

    TxStackSet mTxStackSet;
};

} // namespace stellar
