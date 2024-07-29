#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManagerImpl.h"
#include "util/NonCopyable.h"
#include "util/UnorderedMap.h"

#include <memory>
#include <mutex>

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
class SearchableHotArchiveBucketListSnapshot;

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
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>
        mCurrentHotArchiveSnapshot{};

    // Lock must be held when accessing mCurrentSnapshot
    mutable std::recursive_mutex mSnapshotMutex;

    mutable UnorderedMap<LedgerEntryType, medida::Timer&> mPointTimers{};
    mutable UnorderedMap<std::string, medida::Timer&> mBulkTimers{};

    medida::Meter& mBulkLoadMeter;
    medida::Meter& mBloomMisses;
    medida::Meter& mBloomLookups;

    mutable std::optional<VirtualClock::time_point> mTimerStart;

    // Called by main thread to update mCurrentSnapshot whenever the BucketList
    // is updated
    void updateCurrentSnapshot(
        std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& newSnapshot);

    void updateCurrentHotArchiveSnapshot(
        std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&&
            newSnapshot);

    friend void BucketManagerImpl::addHotArchiveBatch(
        Application& app, uint32_t currLedger, uint32_t currLedgerProtocol,
        std::vector<LedgerEntry> const& initEntries,
        std::vector<LedgerKey> const& deadEntries);
    friend void
    BucketManagerImpl::addLiveBatch(Application& app, uint32_t currLedger,
                                    uint32_t currLedgerProtocol,
                                    std::vector<LedgerEntry> const& initEntries,
                                    std::vector<LedgerEntry> const& liveEntries,
                                    std::vector<LedgerKey> const& deadEntries);
    friend void BucketManagerImpl::assumeState(HistoryArchiveState const& has,
                                               uint32_t maxProtocolVersion,
                                               bool restartMerges);

  public:
    BucketSnapshotManager(
        Application& app,
        std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& snapshot,
        std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&&
            hotArchiveSnapshot);

    std::shared_ptr<SearchableLiveBucketListSnapshot>
    getSearchableBucketListSnapshot() const;

    std::shared_ptr<SearchableHotArchiveBucketListSnapshot>
    getSearchableHotArchiveBucketListSnapshot() const;

    // Checks if snapshot is out of date with mCurrentSnapshot and updates
    // it accordingly
    void maybeUpdateSnapshot(
        std::unique_ptr<BucketListSnapshot<LiveBucket> const>& snapshot) const;

    void maybeUpdateHotArchiveSnapshot(
        std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>& snapshot)
        const;

    // All metric recording functions must only be called by the main thread
    void startPointLoadTimer() const;
    void endPointLoadTimer(LedgerEntryType t, bool bloomMiss) const;
    medida::Timer& recordBulkLoadMetrics(std::string const& label,
                                         size_t numEntries) const;
};
}