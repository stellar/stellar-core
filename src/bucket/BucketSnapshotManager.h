#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshot.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "ledger/LedgerStateSnapshot.h"
#include "util/NonCopyable.h"
#include "util/ThreadAnnotations.h"

#include <map>
#include <memory>

namespace medida
{
class MetricsRegistry;
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

    // LedgerHeader corresponding to the current snapshots. Stored separately
    // because BucketListSnapshotData no longer contains the header.
    LedgerHeader mCurrHeader GUARDED_BY(mSnapshotMutex){};

    // ledgerSeq that the snapshot is based on -> snapshot
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
        mLiveHistoricalSnapshots GUARDED_BY(mSnapshotMutex);
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
        mHotArchiveHistoricalSnapshots GUARDED_BY(mSnapshotMutex);

    uint32_t const mNumHistoricalSnapshots;

    // The current complete ledger state, set by LedgerManager after each
    // ledger close. Used by copyLedgerStateSnapshot() to construct
    // LedgerStateSnapshot instances.
    CompleteConstLedgerStatePtr mCompleteState GUARDED_BY(mSnapshotMutex){};

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

    // Store the complete ledger state for use by copyLedgerStateSnapshot().
    // Called by LedgerManager after each ledger close.
    void setCompleteState(CompleteConstLedgerStatePtr state)
        LOCKS_EXCLUDED(mSnapshotMutex);

    // Access raw snapshot data and historical snapshots. Used by
    // CompleteConstLedgerState construction.
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
    getLiveHistoricalSnapshots() const LOCKS_EXCLUDED(mSnapshotMutex);
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
    getHotArchiveHistoricalSnapshots() const LOCKS_EXCLUDED(mSnapshotMutex);

    // Create a LedgerStateSnapshot containing both live and hot archive
    // snapshots plus the current header. Thread-safe.
    LedgerStateSnapshot copyLedgerStateSnapshot() const
        LOCKS_EXCLUDED(mSnapshotMutex);

    // Refresh `snapshot` if its ledger seq differs from the current snapshot.
    // No-op otherwise.
    void maybeUpdateLedgerStateSnapshot(LedgerStateSnapshot& snapshot) const
        LOCKS_EXCLUDED(mSnapshotMutex);
};
}
