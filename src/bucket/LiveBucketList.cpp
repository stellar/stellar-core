// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucketList.h"
#include "bucket/BucketListBase.h"
#include "ledger/LedgerManager.h"

#include <medida/counter.h>

namespace stellar
{

void
LiveBucketList::addBatch(Application& app, uint32_t currLedger,
                         uint32_t currLedgerProtocol,
                         std::vector<LedgerEntry> const& initEntries,
                         std::vector<LedgerEntry> const& liveEntries,
                         std::vector<LedgerKey> const& deadEntries)
{
    ZoneScoped;
    addBatchInternal(app, currLedger, currLedgerProtocol, initEntries,
                     liveEntries, deadEntries);
}

BucketEntryCounters
LiveBucketList::sumBucketEntryCounters() const
{
    BucketEntryCounters counters;
    for (auto const& lev : mLevels)
    {
        for (auto const& b : {lev.getCurr(), lev.getSnap()})
        {
            if (b->isIndexed())
            {
                auto c = b->getBucketEntryCounters();
                counters += c;
            }
        }
    }
    return counters;
}

void
LiveBucketList::updateStartingEvictionIterator(EvictionIterator& iter,
                                               uint32_t firstScanLevel,
                                               uint32_t ledgerSeq)
{
    // Check if an upgrade has changed the starting scan level to below the
    // current iterator level
    if (iter.bucketListLevel < firstScanLevel)
    {
        // Reset iterator to the new minimum level
        iter.bucketFileOffset = 0;
        iter.isCurrBucket = true;
        iter.bucketListLevel = firstScanLevel;
    }

    // Whenever a Bucket changes (spills or receives an incoming spill), the
    // iterator offset in that bucket is invalidated. After scanning, we
    // must write the iterator to the BucketList then close the ledger.
    // Bucket spills occur on ledger close after we've already written the
    // iterator, so the iterator may be invalidated. Because of this, we
    // must check if the Bucket the iterator currently points to changed on
    // the previous ledger, indicating the current iterator is invalid.
    if (iter.isCurrBucket)
    {
        // Check if bucket received an incoming spill
        releaseAssert(iter.bucketListLevel != 0);
        if (BucketListBase::levelShouldSpill(ledgerSeq - 1,
                                             iter.bucketListLevel - 1))
        {
            // If Bucket changed, reset to start of bucket
            iter.bucketFileOffset = 0;
        }
    }
    else
    {
        if (BucketListBase::levelShouldSpill(ledgerSeq - 1,
                                             iter.bucketListLevel))
        {
            // If Bucket changed, reset to start of bucket
            iter.bucketFileOffset = 0;
        }
    }
}

bool
LiveBucketList::updateEvictionIterAndRecordStats(
    EvictionIterator& iter, EvictionIterator startIter,
    uint32_t configFirstScanLevel, uint32_t ledgerSeq,
    std::shared_ptr<EvictionStatistics> stats, EvictionCounters& counters)
{
    releaseAssert(stats);

    // If we reached eof in curr bucket, start scanning snap.
    // Last level has no snap so cycle back to the initial level.
    if (iter.isCurrBucket && iter.bucketListLevel != kNumLevels - 1)
    {
        iter.isCurrBucket = false;
        iter.bucketFileOffset = 0;
    }
    else
    {
        // If we reached eof in snap, move to next level
        ++iter.bucketListLevel;
        iter.isCurrBucket = true;
        iter.bucketFileOffset = 0;

        // If we have scanned the last level, cycle back to initial
        // level
        if (iter.bucketListLevel == kNumLevels)
        {
            iter.bucketListLevel = configFirstScanLevel;

            // Record then reset metrics at beginning of new eviction cycle
            stats->submitMetricsAndRestartCycle(ledgerSeq, counters);
        }
    }

    // If we are back to the bucket we started at, break
    if (iter.bucketListLevel == startIter.bucketListLevel &&
        iter.isCurrBucket == startIter.isCurrBucket)
    {
        return true;
    }

    return false;
}

void
LiveBucketList::checkIfEvictionScanIsStuck(EvictionIterator const& evictionIter,
                                           uint32_t scanSize,
                                           std::shared_ptr<LiveBucket const> b,
                                           EvictionCounters& counters)
{
    // Check to see if we can finish scanning the new bucket before it
    // receives an update
    uint64_t period = bucketUpdatePeriod(evictionIter.bucketListLevel,
                                         evictionIter.isCurrBucket);
    if (period * scanSize < b->getSize())
    {
        CLOG_WARNING(Bucket,
                     "Bucket too large for current eviction scan size.");
        counters.incompleteBucketScan.inc();
    }
}
}
