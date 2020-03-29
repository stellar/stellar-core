// Copyright 2020 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "catchup/CatchupConfiguration.h"
#include "ledger/LedgerRange.h"
#include <stdexcept>

namespace stellar
{

class HistoryManager;
struct CheckpointRange;

// Range required to do a catchup.
//
// Ranges originate in CatchupConfigurations which have "last" ledger L to catch
// up to, and a count C of ledgers "before" L to replay.
//
// We transform this (along with knowledge of the current LCL) into a pair of at
// least one -- possibly both -- of a checkpoint bucket-apply and a half-open
// LedgerRange to replay.
//
// Such a replay range, if nonempty, will start immediately following the LCL or
// the bucket-apply. There are thus five possible cases:
//
//   1. LCL is not genesis, we replay from LCL.
//   2. Requested count C exceeds L, we replay from LCL.
//   3. C is zero and L is on a checkpoint, we only apply buckets.
//   4. L is within the first checkpoint, we replay from LCL.
//   5. Otherwise we apply buckets at previous checkpoint, then replay.

class CatchupRange
{
    // Whether to apply buckets.
    bool const mApplyBuckets;

    // Which ledger to apply buckets at, or zero if !mApplyBuckets.
    uint32_t const mApplyBucketsAtLedger;

    // After possibly applying buckets, replay a half-open range of ledgers.
    // In other words if mReplayRange.mCount is zero, no replay happens.
    LedgerRange const mReplayRange;

    void checkInvariants();

  public:
    // Return a LedgerRange spanning both the apply-buckets phase (if it
    // exists) and the replay phase.
    LedgerRange
    getFullRangeIncludingBucketApply() const
    {
        if (mApplyBuckets)
        {
            return LedgerRange(mApplyBucketsAtLedger, mReplayRange.mCount + 1);
        }
        else
        {
            return mReplayRange;
        }
    }

    uint32_t
    count() const
    {
        if (mApplyBuckets)
        {
            return mReplayRange.mCount + 1;
        }
        return mReplayRange.mCount;
    }

    uint32_t
    first() const
    {
        if (mApplyBuckets)
        {
            return mApplyBucketsAtLedger;
        }
        else
        {
            return mReplayRange.mFirst;
        }
    }

    uint32_t
    last() const
    {
        if (mReplayRange.mCount != 0)
        {
            return mReplayRange.last();
        }
        else
        {
            // If we're not doing any ledger replay, we should at least be
            // applying buckets.
            assert(mApplyBuckets);
            return mApplyBucketsAtLedger;
        }
    }

    // Return the LedgerRange that covers the ledger-replay part of this
    // catchup.
    LedgerRange
    getReplayRange() const
    {
        return mReplayRange;
    }

    bool
    replayLedgers() const
    {
        return mReplayRange.mCount > 0;
    }

    uint32_t
    getReplayFirst() const
    {
        return mReplayRange.mFirst;
    }

    uint32_t
    getReplayCount() const
    {
        return mReplayRange.mCount;
    }

    // Return first+count, which is one-past-the-last ledger to replay.
    // Best to use this in a loop header to properly handle count=0.
    uint32_t
    getReplayLimit() const
    {
        return mReplayRange.limit();
    }

    // Return first+count-1 which is the last ledger to replay, but
    // throw if count==0. Use only in rare "inclusive range" cases.
    uint32_t
    getReplayLast() const
    {
        return mReplayRange.last();
    }

    bool
    applyBuckets() const
    {
        return mApplyBuckets;
    }

    uint32_t
    getBucketApplyLedger() const
    {
        if (!mApplyBuckets)
        {
            throw std::logic_error("getBucketApplyLedger() cannot be called on "
                                   "CatchupRange when mApplyBuckets == false");
        }
        return mApplyBucketsAtLedger;
    }

    /**
     * Preconditions:
     * * lastClosedLedger > 0
     * * configuration.toLedger() > LedgerManager::GENESIS_LEDGER_SEQ
     * * configuration.toLedger() > lastClosedLedger
     * * configuration.toLedger() != CatchupConfiguration::CURRENT
     */
    explicit CatchupRange(uint32_t lastClosedLedger,
                          CatchupConfiguration const& configuration,
                          HistoryManager const& historyManager);

    // Apply buckets only, no replay.
    explicit CatchupRange(uint32_t applyBucketsAtLedger);

    // Replay only, no buckets.
    explicit CatchupRange(LedgerRange const& replayRange);

    // Apply buckets then replay.
    explicit CatchupRange(uint32_t applyBucketsAtLedger,
                          LedgerRange const& replayRange);
};
}
