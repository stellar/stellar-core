#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketManager.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "main/AppConnector.h"
#include "util/NonCopyable.h"

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
class SearchableHotArchiveBucketListSnapshot;

template <class BucketT>
using SnapshotPtrT = std::unique_ptr<BucketListSnapshot<BucketT> const>;
using SearchableSnapshotConstPtr =
    std::shared_ptr<SearchableLiveBucketListSnapshot const>;
using SearchableHotArchiveSnapshotConstPtr =
    std::shared_ptr<SearchableHotArchiveBucketListSnapshot const>;

// This class serves as the boundary between non-threadsafe singleton classes
// (BucketManager, BucketList, Metrics, etc) and threadsafe, parallel BucketList
// snapshots.
class BucketSnapshotManager : NonMovableOrCopyable
{
  private:
    AppConnector mAppConnector;

    // Snapshot that is maintained and periodically updated by BucketManager on
    // the main thread. When background threads need to generate or refresh a
    // snapshot, they will copy this snapshot.
    SnapshotPtrT<LiveBucket> mCurrLiveSnapshot{};
    SnapshotPtrT<HotArchiveBucket> mCurrHotArchiveSnapshot{};

    // ledgerSeq that the snapshot is based on -> snapshot
    std::map<uint32_t, SnapshotPtrT<LiveBucket>> mLiveHistoricalSnapshots;
    std::map<uint32_t, SnapshotPtrT<HotArchiveBucket>>
        mHotArchiveHistoricalSnapshots;

    uint32_t const mNumHistoricalSnapshots;

    // Lock must be held when accessing any member variables holding snapshots
    mutable std::shared_mutex mSnapshotMutex;

  public:
    // Called by main thread to update snapshots whenever the BucketList
    // is updated
    void
    updateCurrentSnapshot(SnapshotPtrT<LiveBucket>&& liveSnapshot,
                          SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot);

    // numHistoricalLedgers is the number of historical snapshots that the
    // snapshot manager will maintain. If numHistoricalLedgers is 5, snapshots
    // will be capable of querying state from ledger [lcl, lcl - 5].
    BucketSnapshotManager(Application& app, SnapshotPtrT<LiveBucket>&& snapshot,
                          SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot,
                          uint32_t numHistoricalLedgers);

    // Copy the most recent snapshot for the live bucket list
    SearchableSnapshotConstPtr copySearchableLiveBucketListSnapshot() const;

    // Copy the most recent snapshot for the hot archive bucket list
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot() const;

    // `maybeCopy` interface refreshes `snapshot` if a newer snapshot is
    // available. It's a no-op otherwise. This is useful to avoid unnecessary
    // copying.
    void
    maybeCopySearchableBucketListSnapshot(SearchableSnapshotConstPtr& snapshot);
    void maybeCopySearchableHotArchiveBucketListSnapshot(
        SearchableHotArchiveSnapshotConstPtr& snapshot);
};
}