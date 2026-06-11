// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucketList.h"
#include "bucket/BucketListBase.h"
#include "bucket/BucketManager.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/ProtocolVersion.h"

#include <algorithm>
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
    bool useInit = protocolVersionStartsFrom(
        currLedgerProtocol,
        LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);
    auto entries = LiveBucket::convertToBucketEntry(useInit, initEntries,
                                                    liveEntries, deadEntries);
    std::vector<std::shared_ptr<LiveBucket>> shards;
    if (!entries.empty())
    {
        bool doFsync = !app.getConfig().DISABLE_XDR_FSYNC;
        shards.push_back(LiveBucket::freshShard(
            app.getBucketManager(), currLedgerProtocol, std::move(entries),
            app.getClock().getIOContext(), doFsync));
    }
    addBatchShards(app, currLedger, currLedgerProtocol, std::move(shards));
}

void
LiveBucketList::addBatchShards(
    Application& app, uint32_t currLedger, uint32_t currLedgerProtocol,
    std::vector<std::shared_ptr<LiveBucket>>&& newShards)
{
    ZoneScoped;
    // Match the legacy level-0 merge behavior where every ledger's (possibly
    // empty) batch re-stamped curr with the current protocol's metadata: a
    // ledger that produced no shards at a META-supporting protocol
    // contributes a meta-only shard, so curr is never version-less above
    // non-empty lower levels. Pre-METAENTRY protocols genuinely leave curr
    // empty, as before.
    bool anyShard =
        std::any_of(newShards.begin(), newShards.end(),
                    [](std::shared_ptr<LiveBucket> const& s) {
                        return s && !s->isEmpty();
                    });
    if (!anyShard &&
        protocolVersionStartsFrom(
            currLedgerProtocol,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
    {
        newShards.push_back(LiveBucket::freshShard(
            app.getBucketManager(), currLedgerProtocol, {},
            app.getClock().getIOContext(),
            !app.getConfig().DISABLE_XDR_FSYNC));
    }
    spillLevels(app, currLedger, currLedgerProtocol);
    mLevels[0].prepareFirstLevelFromShards(app, currLedger,
                                           std::move(newShards));
    mLevels[0].commit();

    // Try to resolve completed merges to single buckets (see the comment in
    // addBatchInternal). Nonblocking.
    if (!app.getConfig().ARTIFICIALLY_PESSIMIZE_MERGES_FOR_TESTING)
    {
        resolveAnyReadyFutures();
    }

    // Initialize caches for any new buckets we might have added
    maybeInitializeCaches(app.getConfig());
}

BucketEntryCounters
LiveBucketList::sumBucketEntryCounters() const
{
    BucketEntryCounters counters;
    for (auto const& lev : mLevels)
    {
        for (auto const& b : {lev.getCurr(), lev.getSnap()})
        {
            if (!b->isEmpty())
            {
                auto c = b->getBucketEntryCounters();
                counters += c;
            }
        }
    }
    return counters;
}

void
LiveBucketList::maybeInitializeCaches(Config const& cfg) const
{
    auto blCounters = sumBucketEntryCounters();
    size_t totalAccountsSize =
        blCounters.entryTypeSizes.at(LedgerEntryTypeAndDurability::ACCOUNT);

    for (uint32_t i = 0; i < kNumLevels; ++i)
    {
        auto curr = mLevels[i].getCurr();
        if (!curr->isEmpty())
        {
            curr->maybeInitializeCache(totalAccountsSize, cfg);
        }

        auto snap = mLevels[i].getSnap();
        if (!snap->isEmpty())
        {
            snap->maybeInitializeCache(totalAccountsSize, cfg);
        }
    }
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
    std::shared_ptr<EvictionStatistics> stats, EvictionMetrics& metrics)
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
            stats->submitMetricsAndRestartCycle(ledgerSeq, metrics);
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
                                           EvictionMetrics& metrics)
{
    // Check to see if we can finish scanning the new bucket before it
    // receives an update
    uint64_t period = bucketUpdatePeriod(evictionIter.bucketListLevel,
                                         evictionIter.isCurrBucket);
    if (period * scanSize < b->getSize())
    {
        CLOG_WARNING(Bucket,
                     "Bucket too large for current eviction scan size.");
        metrics.incompleteBucketScan.inc();
    }
}
}
