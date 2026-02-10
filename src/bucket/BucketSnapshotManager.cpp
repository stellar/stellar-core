// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketListSnapshot.h"
#include "bucket/HotArchiveBucketList.h"
#include "bucket/LiveBucketList.h"
#include "main/AppConnector.h"
#include "util/GlobalChecks.h"
#include "util/MetricsRegistry.h"

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    AppConnector& app, LiveBucketList const& liveBL,
    HotArchiveBucketList const& hotArchiveBL, LedgerHeader const& header,
    uint32_t numHistoricalLedgers)
    : mAppConnector(app)
    , mCurrLiveSnapshot(
          std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL, header))
    , mCurrHotArchiveSnapshot(
          std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(
              hotArchiveBL, header))
    , mNumHistoricalSnapshots(numHistoricalLedgers)
{
    releaseAssert(threadIsMain());
    releaseAssert(mCurrLiveSnapshot);
    releaseAssert(mCurrHotArchiveSnapshot);
}

SearchableSnapshotConstPtr
BucketSnapshotManager::copySearchableLiveBucketListSnapshot() const
{
    SharedLockShared guard(mSnapshotMutex);
    // Can't use std::make_shared due to private constructor
    return copySearchableLiveBucketListSnapshot(guard);
}

SearchableHotArchiveSnapshotConstPtr
BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot() const
{
    SharedLockShared guard(mSnapshotMutex);
    releaseAssert(mCurrHotArchiveSnapshot);
    // Can't use std::make_shared due to private constructor
    return copySearchableHotArchiveBucketListSnapshot(guard);
}

SearchableSnapshotConstPtr
BucketSnapshotManager::copySearchableLiveBucketListSnapshot(
    SharedLockShared const& guard) const
{
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableLiveBucketListSnapshot>(
        new SearchableLiveBucketListSnapshot(mAppConnector.getMetrics(),
                                             mCurrLiveSnapshot,
                                             mLiveHistoricalSnapshots));
}

SearchableSnapshotConstPtr
BucketSnapshotManager::copySearchableLiveBucketListSnapshot(
    SearchableSnapshotConstPtr const& snapshot, MetricsRegistry& metrics)
{
    // Can't use std::make_shared due to private constructor
    releaseAssert(snapshot);
    return std::shared_ptr<SearchableLiveBucketListSnapshot>(
        new SearchableLiveBucketListSnapshot(
            metrics, snapshot->getSnapshotData(),
            snapshot->getHistoricalSnapshots()));
}

SearchableHotArchiveSnapshotConstPtr
BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot(
    SearchableHotArchiveSnapshotConstPtr const& snapshot,
    MetricsRegistry& metrics)
{
    releaseAssert(snapshot);
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableHotArchiveBucketListSnapshot>(
        new SearchableHotArchiveBucketListSnapshot(
            metrics, snapshot->getSnapshotData(),
            snapshot->getHistoricalSnapshots()));
}

SearchableHotArchiveSnapshotConstPtr
BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot(
    SharedLockShared const& guard) const
{
    releaseAssert(mCurrHotArchiveSnapshot);
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableHotArchiveBucketListSnapshot>(
        new SearchableHotArchiveBucketListSnapshot(
            mAppConnector.getMetrics(), mCurrHotArchiveSnapshot,
            mHotArchiveHistoricalSnapshots));
}

namespace
{
template <typename SnapshotT, typename DataT>
bool
needsUpdate(std::shared_ptr<SnapshotT const> const& snapshot,
            std::shared_ptr<DataT const> const& currData)
{
    return !snapshot || snapshot->getLedgerSeq() < currData->getLedgerSeq();
}
}

void
BucketSnapshotManager::maybeCopySearchableBucketListSnapshot(
    SearchableSnapshotConstPtr& snapshot)
{
    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock.
    SharedLockShared guard(mSnapshotMutex);
    if (needsUpdate(snapshot, mCurrLiveSnapshot))
    {
        snapshot = copySearchableLiveBucketListSnapshot(guard);
    }
}

void
BucketSnapshotManager::maybeCopySearchableHotArchiveBucketListSnapshot(
    SearchableHotArchiveSnapshotConstPtr& snapshot)
{
    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock.
    SharedLockShared guard(mSnapshotMutex);
    if (needsUpdate(snapshot, mCurrHotArchiveSnapshot))
    {
        snapshot = copySearchableHotArchiveBucketListSnapshot(guard);
    }
}

void
BucketSnapshotManager::maybeCopyLiveAndHotArchiveSnapshots(
    SearchableSnapshotConstPtr& liveSnapshot,
    SearchableHotArchiveSnapshotConstPtr& hotArchiveSnapshot)
{
    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock. For consistency we hold the lock while
    // updating both snapshots.
    SharedLockShared guard(mSnapshotMutex);
    if (needsUpdate(liveSnapshot, mCurrLiveSnapshot))
    {
        liveSnapshot = copySearchableLiveBucketListSnapshot(guard);
    }

    if (needsUpdate(hotArchiveSnapshot, mCurrHotArchiveSnapshot))
    {
        hotArchiveSnapshot = copySearchableHotArchiveBucketListSnapshot(guard);
    }
}

std::pair<SearchableSnapshotConstPtr, SearchableHotArchiveSnapshotConstPtr>
BucketSnapshotManager::copySearchableBucketListSnapshots() const
{
    SharedLockShared guard(mSnapshotMutex);
    return {copySearchableLiveBucketListSnapshot(guard),
            copySearchableHotArchiveBucketListSnapshot(guard)};
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    LiveBucketList const& liveBL, HotArchiveBucketList const& hotArchiveBL,
    LedgerHeader const& header)
{
    auto updateSnapshot = [numHistoricalSnapshots = mNumHistoricalSnapshots](
                              auto& currentSnapshot, auto& historicalSnapshots,
                              auto newSnapshot) {
        releaseAssert(newSnapshot);
        releaseAssert(!currentSnapshot || newSnapshot->getLedgerSeq() >=
                                              currentSnapshot->getLedgerSeq());

        // First update historical snapshots
        if (numHistoricalSnapshots != 0)
        {
            // If historical snapshots are full, delete the oldest one
            if (historicalSnapshots.size() == numHistoricalSnapshots)
            {
                historicalSnapshots.erase(historicalSnapshots.begin());
            }

            historicalSnapshots.emplace(currentSnapshot->getLedgerSeq(),
                                        std::move(currentSnapshot));
        }

        currentSnapshot = std::move(newSnapshot);
    };

    auto newLiveSnapshot =
        std::make_shared<BucketListSnapshotData<LiveBucket>>(liveBL, header);
    auto newHotArchiveSnapshot =
        std::make_shared<BucketListSnapshotData<HotArchiveBucket>>(hotArchiveBL,
                                                                   header);

    // Updating canonical snapshots requires exclusive write access
    SharedLockExclusive guard(mSnapshotMutex);
    updateSnapshot(mCurrLiveSnapshot, mLiveHistoricalSnapshots,
                   std::move(newLiveSnapshot));
    updateSnapshot(mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots,
                   std::move(newHotArchiveSnapshot));
}
}
