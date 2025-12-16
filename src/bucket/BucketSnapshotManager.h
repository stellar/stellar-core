#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshot.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "util/NonCopyable.h"
#include "util/ThreadAnnotations.h"

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

class AppConnector;
class LiveBucketList;
class HotArchiveBucketList;

// This class serves as the boundary between non-threadsafe singleton classes
// (BucketManager, BucketList, Metrics, etc) and threadsafe, parallel BucketList
// snapshots.
class BucketSnapshotManager : NonMovableOrCopyable
{
  private:
    AppConnector& mAppConnector;

    // Lock must be held when accessing any member variables holding snapshots
    mutable SharedMutex mSnapshotMutex;

    // Snapshot that is maintained and periodically updated by BucketManager on
    // the main thread. When background threads need to generate or refresh a
    // snapshot, they will copy this snapshot.
    std::shared_ptr<BucketListSnapshotData<LiveBucket> const>
        mCurrLiveSnapshot GUARDED_BY(mSnapshotMutex){};
    std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>
        mCurrHotArchiveSnapshot GUARDED_BY(mSnapshotMutex){};

    // ledgerSeq that the snapshot is based on -> snapshot
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
        mLiveHistoricalSnapshots GUARDED_BY(mSnapshotMutex);
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
        mHotArchiveHistoricalSnapshots GUARDED_BY(mSnapshotMutex);

    uint32_t const mNumHistoricalSnapshots;

  public:
    // Called by main thread to update snapshots whenever the BucketList
    // is updated
    void updateCurrentSnapshot(LiveBucketList const& liveBL,
                               HotArchiveBucketList const& hotArchiveBL,
                               LedgerHeader const& header)
        LOCKS_EXCLUDED(mSnapshotMutex);

    // numHistoricalLedgers is the number of historical snapshots that the
    // snapshot manager will maintain. If numHistoricalLedgers is 5, snapshots
    // will be capable of querying state from ledger [lcl, lcl - 5].
    BucketSnapshotManager(AppConnector& app, LiveBucketList const& liveBL,
                          HotArchiveBucketList const& hotArchiveBL,
                          LedgerHeader const& header,
                          uint32_t numHistoricalLedgers);

    // Copy the most recent snapshot for the live bucket list
    SearchableSnapshotConstPtr copySearchableLiveBucketListSnapshot() const
        LOCKS_EXCLUDED(mSnapshotMutex);

    // Create a deep copy from an existing searchable snapshot
    static SearchableSnapshotConstPtr copySearchableLiveBucketListSnapshot(
        SearchableSnapshotConstPtr const& snapshot,
        medida::MetricsRegistry& metrics);

    // Create a deep copy from an existing searchable hot archive snapshot
    static SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot(
        SearchableHotArchiveSnapshotConstPtr const& snapshot);

    // Copy the most recent snapshot for the hot archive bucket list
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot() const
        LOCKS_EXCLUDED(mSnapshotMutex);

    // Copy the most recent snapshot for the live bucket list, while holding the
    // lock
    SearchableSnapshotConstPtr
    copySearchableLiveBucketListSnapshot(SharedLockShared const& guard) const
        REQUIRES_SHARED(mSnapshotMutex);

    // Copy the most recent snapshot for the hot archive bucket list, while
    // holding the lock
    SearchableHotArchiveSnapshotConstPtr
    copySearchableHotArchiveBucketListSnapshot(
        SharedLockShared const& guard) const REQUIRES_SHARED(mSnapshotMutex);

    // `maybeCopy` interface refreshes `snapshot` if a newer snapshot is
    // available. It's a no-op otherwise. This is useful to avoid unnecessary
    // copying.
    void
    maybeCopySearchableBucketListSnapshot(SearchableSnapshotConstPtr& snapshot)
        LOCKS_EXCLUDED(mSnapshotMutex);
    void maybeCopySearchableHotArchiveBucketListSnapshot(
        SearchableHotArchiveSnapshotConstPtr& snapshot)
        LOCKS_EXCLUDED(mSnapshotMutex);

    // This function is the same as snapshot refreshers above, but guarantees
    // that both snapshots are consistent with the same lcl. This is required
    // when querying both snapshot types as part of the same query.
    void maybeCopyLiveAndHotArchiveSnapshots(
        SearchableSnapshotConstPtr& liveSnapshot,
        SearchableHotArchiveSnapshotConstPtr& hotArchiveSnapshot)
        LOCKS_EXCLUDED(mSnapshotMutex);
};
}
