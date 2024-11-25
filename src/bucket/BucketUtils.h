#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "main/Application.h"
#include "xdr/Stellar-ledger-entries.h"
#include <cstdint>
#include <list>
#include <mutex>

namespace medida
{
class Counter;
}
namespace stellar
{

class Application;

#define BUCKET_TYPE_ASSERT(BucketT) \
    static_assert(std::is_same_v<BucketT, LiveBucket> || \
                      std::is_same_v<BucketT, HotArchiveBucket>, \
                  "BucketT must be a Bucket type")

// A fine-grained merge-operation-counter structure for tracking various
// events during merges. These are not medida counters because we do not
// want or need to publish this level of granularity outside of testing, and
// we do want merges to run as quickly as possible.
struct MergeCounters
{
    uint64_t mPreInitEntryProtocolMerges{0};
    uint64_t mPostInitEntryProtocolMerges{0};

    uint64_t mRunningMergeReattachments{0};
    uint64_t mFinishedMergeReattachments{0};

    uint64_t mPreShadowRemovalProtocolMerges{0};
    uint64_t mPostShadowRemovalProtocolMerges{0};

    uint64_t mNewMetaEntries{0};
    uint64_t mNewInitEntries{0};
    uint64_t mNewLiveEntries{0};
    uint64_t mNewDeadEntries{0};
    uint64_t mOldMetaEntries{0};
    uint64_t mOldInitEntries{0};
    uint64_t mOldLiveEntries{0};
    uint64_t mOldDeadEntries{0};

    uint64_t mOldEntriesDefaultAccepted{0};
    uint64_t mNewEntriesDefaultAccepted{0};
    uint64_t mNewInitEntriesMergedWithOldDead{0};
    uint64_t mOldInitEntriesMergedWithNewLive{0};
    uint64_t mOldInitEntriesMergedWithNewDead{0};
    uint64_t mNewEntriesMergedWithOldNeitherInit{0};

    uint64_t mShadowScanSteps{0};
    uint64_t mMetaEntryShadowElisions{0};
    uint64_t mLiveEntryShadowElisions{0};
    uint64_t mInitEntryShadowElisions{0};
    uint64_t mDeadEntryShadowElisions{0};

    uint64_t mOutputIteratorTombstoneElisions{0};
    uint64_t mOutputIteratorBufferUpdates{0};
    uint64_t mOutputIteratorActualWrites{0};
    uint64_t mOutputIteratorLiveToInitRewrites{0};
    MergeCounters& operator+=(MergeCounters const& delta);
    bool operator==(MergeCounters const& other) const;
};

// Stores key that is eligible for eviction and the position of the eviction
// iterator as if that key was the last entry evicted
struct EvictionResultEntry
{
    LedgerKey key;
    EvictionIterator iter;
    uint32_t liveUntilLedger;

    EvictionResultEntry(LedgerKey const& key, EvictionIterator const& iter,
                        uint32_t liveUntilLedger)
        : key(key), iter(iter), liveUntilLedger(liveUntilLedger)
    {
    }
};

struct EvictionResult
{
    // List of keys eligible for eviction in the order in which they occur in
    // the bucket
    std::list<EvictionResultEntry> eligibleKeys{};

    // Eviction iterator at the end of the scan region
    EvictionIterator endOfRegionIterator;

    // LedgerSeq which this scan is based on
    uint32_t initialLedger{};

    // State archival settings that this scan is based on
    StateArchivalSettings initialSas;

    EvictionResult(StateArchivalSettings const& sas) : initialSas(sas)
    {
    }

    // Returns true if this is a valid archival scan for the current ledger
    // and archival settings. This is necessary because we start the scan
    // for ledger N immediately after N - 1 closes. However, ledger N may
    // contain a network upgrade changing eviction scan settings. Legacy SQL
    // scans will run based on the changes that occurred during ledger N,
    // meaning the scan we started at ledger N - 1 is invalid since it was based
    // off of older settings.
    bool isValid(uint32_t currLedger,
                 StateArchivalSettings const& currSas) const;
};

struct EvictionCounters
{
    medida::Counter& entriesEvicted;
    medida::Counter& bytesScannedForEviction;
    medida::Counter& incompleteBucketScan;
    medida::Counter& evictionCyclePeriod;
    medida::Counter& averageEvictedEntryAge;

    EvictionCounters(Application& app);
};

class EvictionStatistics
{
  private:
    std::mutex mLock{};

    // Only record metrics if we've seen a complete cycle to avoid noise
    bool mCompleteCycle{false};
    uint64_t mEvictedEntriesAgeSum{};
    uint64_t mNumEntriesEvicted{};
    uint32_t mEvictionCycleStartLedger{};

  public:
    // Evicted entry "age" is the delta between its liveUntilLedger and the
    // ledger when the entry is actually evicted
    void recordEvictedEntry(uint64_t age);

    void submitMetricsAndRestartCycle(uint32_t currLedgerSeq,
                                      EvictionCounters& counters);
};
}