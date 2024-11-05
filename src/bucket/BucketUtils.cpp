// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include <medida/metrics_registry.h>

namespace stellar
{

MergeCounters&
MergeCounters::operator+=(MergeCounters const& delta)
{
    mPreInitEntryProtocolMerges += delta.mPreInitEntryProtocolMerges;
    mPostInitEntryProtocolMerges += delta.mPostInitEntryProtocolMerges;

    mRunningMergeReattachments += delta.mRunningMergeReattachments;
    mFinishedMergeReattachments += delta.mFinishedMergeReattachments;

    mPreShadowRemovalProtocolMerges += delta.mPreShadowRemovalProtocolMerges;
    mPostShadowRemovalProtocolMerges += delta.mPostShadowRemovalProtocolMerges;

    mNewMetaEntries += delta.mNewMetaEntries;
    mNewInitEntries += delta.mNewInitEntries;
    mNewLiveEntries += delta.mNewLiveEntries;
    mNewDeadEntries += delta.mNewDeadEntries;
    mOldMetaEntries += delta.mOldMetaEntries;
    mOldInitEntries += delta.mOldInitEntries;
    mOldLiveEntries += delta.mOldLiveEntries;
    mOldDeadEntries += delta.mOldDeadEntries;

    mOldEntriesDefaultAccepted += delta.mOldEntriesDefaultAccepted;
    mNewEntriesDefaultAccepted += delta.mNewEntriesDefaultAccepted;
    mNewInitEntriesMergedWithOldDead += delta.mNewInitEntriesMergedWithOldDead;
    mOldInitEntriesMergedWithNewLive += delta.mOldInitEntriesMergedWithNewLive;
    mOldInitEntriesMergedWithNewDead += delta.mOldInitEntriesMergedWithNewDead;
    mNewEntriesMergedWithOldNeitherInit +=
        delta.mNewEntriesMergedWithOldNeitherInit;

    mShadowScanSteps += delta.mShadowScanSteps;
    mMetaEntryShadowElisions += delta.mMetaEntryShadowElisions;
    mLiveEntryShadowElisions += delta.mLiveEntryShadowElisions;
    mInitEntryShadowElisions += delta.mInitEntryShadowElisions;
    mDeadEntryShadowElisions += delta.mDeadEntryShadowElisions;

    mOutputIteratorTombstoneElisions += delta.mOutputIteratorTombstoneElisions;
    mOutputIteratorBufferUpdates += delta.mOutputIteratorBufferUpdates;
    mOutputIteratorActualWrites += delta.mOutputIteratorActualWrites;
    return *this;
}

bool
MergeCounters::operator==(MergeCounters const& other) const
{
    return (
        mPreInitEntryProtocolMerges == other.mPreInitEntryProtocolMerges &&
        mPostInitEntryProtocolMerges == other.mPostInitEntryProtocolMerges &&

        mRunningMergeReattachments == other.mRunningMergeReattachments &&
        mFinishedMergeReattachments == other.mFinishedMergeReattachments &&

        mNewMetaEntries == other.mNewMetaEntries &&
        mNewInitEntries == other.mNewInitEntries &&
        mNewLiveEntries == other.mNewLiveEntries &&
        mNewDeadEntries == other.mNewDeadEntries &&
        mOldMetaEntries == other.mOldMetaEntries &&
        mOldInitEntries == other.mOldInitEntries &&
        mOldLiveEntries == other.mOldLiveEntries &&
        mOldDeadEntries == other.mOldDeadEntries &&

        mOldEntriesDefaultAccepted == other.mOldEntriesDefaultAccepted &&
        mNewEntriesDefaultAccepted == other.mNewEntriesDefaultAccepted &&
        mNewInitEntriesMergedWithOldDead ==
            other.mNewInitEntriesMergedWithOldDead &&
        mOldInitEntriesMergedWithNewLive ==
            other.mOldInitEntriesMergedWithNewLive &&
        mOldInitEntriesMergedWithNewDead ==
            other.mOldInitEntriesMergedWithNewDead &&
        mNewEntriesMergedWithOldNeitherInit ==
            other.mNewEntriesMergedWithOldNeitherInit &&

        mShadowScanSteps == other.mShadowScanSteps &&
        mMetaEntryShadowElisions == other.mMetaEntryShadowElisions &&
        mLiveEntryShadowElisions == other.mLiveEntryShadowElisions &&
        mInitEntryShadowElisions == other.mInitEntryShadowElisions &&
        mDeadEntryShadowElisions == other.mDeadEntryShadowElisions &&

        mOutputIteratorTombstoneElisions ==
            other.mOutputIteratorTombstoneElisions &&
        mOutputIteratorBufferUpdates == other.mOutputIteratorBufferUpdates &&
        mOutputIteratorActualWrites == other.mOutputIteratorActualWrites);
}

// Check that eviction scan is based off of current ledger snapshot and that
// archival settings have not changed
bool
EvictionResult::isValid(uint32_t currLedger,
                        StateArchivalSettings const& currSas) const
{
    return initialLedger == currLedger &&
           initialSas.maxEntriesToArchive == currSas.maxEntriesToArchive &&
           initialSas.evictionScanSize == currSas.evictionScanSize &&
           initialSas.startingEvictionScanLevel ==
               currSas.startingEvictionScanLevel;
}

EvictionCounters::EvictionCounters(Application& app)
    : entriesEvicted(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "entries-evicted"}))
    , bytesScannedForEviction(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "bytes-scanned"}))
    , incompleteBucketScan(app.getMetrics().NewCounter(
          {"state-archival", "eviction", "incomplete-scan"}))
    , evictionCyclePeriod(
          app.getMetrics().NewCounter({"state-archival", "eviction", "period"}))
    , averageEvictedEntryAge(
          app.getMetrics().NewCounter({"state-archival", "eviction", "age"}))
{
}

void
EvictionStatistics::recordEvictedEntry(uint64_t age)
{
    std::lock_guard l(mLock);
    ++mNumEntriesEvicted;
    mEvictedEntriesAgeSum += age;
}

void
EvictionStatistics::submitMetricsAndRestartCycle(uint32_t currLedgerSeq,
                                                 EvictionCounters& counters)
{
    std::lock_guard l(mLock);

    // Only record metrics if we've seen a complete cycle to avoid noise
    if (mCompleteCycle)
    {
        counters.evictionCyclePeriod.set_count(currLedgerSeq -
                                               mEvictionCycleStartLedger);

        auto averageAge = mNumEntriesEvicted == 0
                              ? 0
                              : mEvictedEntriesAgeSum / mNumEntriesEvicted;
        counters.averageEvictedEntryAge.set_count(averageAge);
    }

    // Reset to start new cycle
    mCompleteCycle = true;
    mEvictedEntriesAgeSum = 0;
    mNumEntriesEvicted = 0;
    mEvictionCycleStartLedger = currLedgerSeq;
}
}
