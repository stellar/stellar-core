#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManagerImpl.h"
#include "util/NonCopyable.h"
#include "util/UnorderedMap.h"

#include <map>
#include <memory>
#include <shared_mutex>

namespace medida
{
class Meter;
class MetricsRegistry;
class Timer;
}

namespace stellar
{

class Application;
class LiveBucketList;
template <class BucketT> class BucketListSnapshot;
class SearchableLiveBucketListSnapshot;

// This class serves as the boundary between non-threadsafe singleton classes
// (BucketManager, BucketList, Metrics, etc) and threadsafe, parallel BucketList
// snapshots.
class BucketSnapshotManager : NonMovableOrCopyable
{
  private:
    Application& mApp;

    // Snapshot that is maintained and periodically updated by BucketManager on
    // the main thread. When background threads need to generate or refresh a
    // snapshot, they will copy this snapshot.
    std::unique_ptr<BucketListSnapshot<LiveBucket> const> mCurrentSnapshot{};

    // ledgerSeq that the snapshot is based on -> snapshot
    std::map<uint32_t, std::unique_ptr<BucketListSnapshot<LiveBucket> const>>
        mHistoricalSnapshots;

    uint32_t const mNumHistoricalSnapshots;

    // Lock must be held when accessing mCurrentSnapshot and
    // mHistoricalSnapshots
    mutable std::shared_mutex mSnapshotMutex;

    mutable UnorderedMap<LedgerEntryType, medida::Timer&> mPointTimers{};
    mutable UnorderedMap<std::string, medida::Timer&> mBulkTimers{};

    medida::Meter& mBulkLoadMeter;
    medida::Meter& mBloomMisses;
    medida::Meter& mBloomLookups;

    mutable std::optional<VirtualClock::time_point> mTimerStart;

  public:
    // Called by main thread to update mCurrentSnapshot whenever the BucketList
    // is updated
    void updateCurrentSnapshot(
        std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& newSnapshot);

    // numHistoricalLedgers is the number of historical snapshots that the
    // snapshot manager will maintain. If numHistoricalLedgers is 5, snapshots
    // will be capable of querying state from ledger [lcl, lcl - 5].
    BucketSnapshotManager(Application& app,
                          std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& snapshot,
                          uint32_t numHistoricalLedgers);

    std::shared_ptr<SearchableLiveBucketListSnapshot>
    copySearchableBucketListSnapshot() const;

    // Checks if snapshot is out of date with mCurrentSnapshot and updates
    // it accordingly
    void maybeUpdateSnapshot(
        std::unique_ptr<BucketListSnapshot<LiveBucket> const>& snapshot,
        std::map<uint32_t, std::unique_ptr<BucketListSnapshot<LiveBucket> const>>&
            historicalSnapshots) const;

    // All metric recording functions must only be called by the main thread
    void startPointLoadTimer() const;
    void endPointLoadTimer(LedgerEntryType t, bool bloomMiss) const;
    medida::Timer& recordBulkLoadMetrics(std::string const& label,
                                         size_t numEntries) const;
};
}