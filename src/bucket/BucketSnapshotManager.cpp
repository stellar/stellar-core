// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/SearchableBucketList.h"
#include "main/AppConnector.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h" // IWYU pragma: keep

#include <shared_mutex>
#include <xdrpp/types.h>

namespace stellar
{

namespace
{
template <class BucketT>
std::map<uint32_t, SnapshotPtrT<BucketT>>
copyHistoricalSnapshots(
    std::map<uint32_t, SnapshotPtrT<BucketT>> const& snapshots)
{
    std::map<uint32_t, SnapshotPtrT<BucketT>> copiedSnapshots;
    for (auto const& [ledgerSeq, snap] : snapshots)
    {
        copiedSnapshots.emplace(
            ledgerSeq,
            std::make_unique<BucketListSnapshot<BucketT> const>(*snap));
    }
    return copiedSnapshots;
}
}

BucketSnapshotManager::BucketSnapshotManager(
    Application& app, SnapshotPtrT<LiveBucket>&& snapshot,
    SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot,
    uint32_t numLiveHistoricalSnapshots)
    : mAppConnector(app)
    , mCurrLiveSnapshot(std::move(snapshot))
    , mCurrHotArchiveSnapshot(std::move(hotArchiveSnapshot))
    , mLiveHistoricalSnapshots()
    , mHotArchiveHistoricalSnapshots()
    , mNumHistoricalSnapshots(numLiveHistoricalSnapshots)
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
        new SearchableLiveBucketListSnapshot(
            *this, mAppConnector,
            std::make_unique<BucketListSnapshot<LiveBucket>>(
                *mCurrLiveSnapshot),
            copyHistoricalSnapshots(mLiveHistoricalSnapshots)));
}

SearchableHotArchiveSnapshotConstPtr
BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot(
    SharedLockShared const& guard) const
{
    releaseAssert(mCurrHotArchiveSnapshot);
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableHotArchiveBucketListSnapshot>(
        new SearchableHotArchiveBucketListSnapshot(
            *this, mAppConnector,
            std::make_unique<BucketListSnapshot<HotArchiveBucket>>(
                *mCurrHotArchiveSnapshot),
            copyHistoricalSnapshots(mHotArchiveHistoricalSnapshots)));
}

namespace
{
template <typename T, typename U>
bool
needsUpdate(std::shared_ptr<T const> const& snapshot,
            SnapshotPtrT<U> const& curr)
{
    return !snapshot || snapshot->getLedgerSeq() < curr->getLedgerSeq();
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
        liveSnapshot = copySearchableLiveBucketListSnapshot();
    }

    if (needsUpdate(hotArchiveSnapshot, mCurrHotArchiveSnapshot))
    {
        hotArchiveSnapshot = copySearchableHotArchiveBucketListSnapshot();
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    SnapshotPtrT<LiveBucket>&& liveSnapshot,
    SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot)
{
    auto updateSnapshot = [numHistoricalSnapshots = mNumHistoricalSnapshots](
                              auto& currentSnapshot, auto& historicalSnapshots,
                              auto&& newSnapshot) {
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
            currentSnapshot = nullptr;
        }

        currentSnapshot.swap(newSnapshot);
    };

    // Updating the BucketSnapshotManager canonical snapshot, must lock
    // exclusively for write access.
    SharedLockExclusive guard(mSnapshotMutex);
    updateSnapshot(mCurrLiveSnapshot, mLiveHistoricalSnapshots, liveSnapshot);
    updateSnapshot(mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots,
                   hotArchiveSnapshot);
}
}