// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/Bucket.h"
#include "bucket/BucketListSnapshot.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h" // IWYU pragma: keep

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include <shared_mutex>

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    Application& app,
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& snapshot,
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&&
        hotArchiveSnapshot,
    uint32_t numLiveHistoricalSnapshots)
    : mApp(app)
    , mCurrLiveSnapshot(std::move(snapshot))
    , mCurrHotArchiveSnapshot(std::move(hotArchiveSnapshot))
    , mLiveHistoricalSnapshots()
    , mHotArchiveHistoricalSnapshots()
    , mNumHistoricalSnapshots(numLiveHistoricalSnapshots)
    , mBulkLoadMeter(app.getMetrics().NewMeter(
          {"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "lookups"}, "bloom"))
{
    releaseAssert(threadIsMain());
    releaseAssert(mCurrLiveSnapshot);
    releaseAssert(mCurrHotArchiveSnapshot);
}

std::shared_ptr<SearchableLiveBucketListSnapshot>
BucketSnapshotManager::copySearchableLiveBucketListSnapshot() const
{
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableLiveBucketListSnapshot>(
        new SearchableLiveBucketListSnapshot(*this));
}

std::shared_ptr<SearchableHotArchiveBucketListSnapshot>
BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot() const
{
    releaseAssert(mCurrHotArchiveSnapshot);
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableHotArchiveBucketListSnapshot>(
        new SearchableHotArchiveBucketListSnapshot(*this));
}

medida::Timer&
BucketSnapshotManager::recordBulkLoadMetrics(std::string const& label,
                                             size_t numEntries) const
{
    // For now, only keep metrics for the main thread. We can decide on what
    // metrics make sense when more background services are added later.
    releaseAssert(threadIsMain());

    if (numEntries != 0)
    {
        mBulkLoadMeter.Mark(numEntries);
    }

    auto iter = mBulkTimers.find(label);
    if (iter == mBulkTimers.end())
    {
        auto& metric =
            mApp.getMetrics().NewTimer({"bucketlistDB", "bulk", label});
        iter = mBulkTimers.emplace(label, metric).first;
    }

    return iter->second;
}

template <class SnapshotT>
void
BucketSnapshotManager::maybeUpdateSnapshot(
    std::unique_ptr<SnapshotT const>& snapshot,
    std::map<uint32_t, std::unique_ptr<SnapshotT const>>& historicalSnapshots)
    const
{
    static_assert(
        std::is_same_v<SnapshotT, BucketListSnapshot<LiveBucket>> ||
        std::is_same_v<SnapshotT, BucketListSnapshot<HotArchiveBucket>>);

    auto const& managerSnapshot = [&]() -> auto const&
    {
        if constexpr (std::is_same_v<SnapshotT, BucketListSnapshot<LiveBucket>>)
        {
            return mCurrLiveSnapshot;
        }
        else
        {
            return mCurrHotArchiveSnapshot;
        }
    }
    ();

    auto const& managerHistoricalSnapshots = [&]() -> auto const&
    {
        if constexpr (std::is_same_v<SnapshotT, BucketListSnapshot<LiveBucket>>)
        {
            return mLiveHistoricalSnapshots;
        }
        else
        {
            return mHotArchiveHistoricalSnapshots;
        }
    }
    ();

    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock.
    std::shared_lock<std::shared_mutex> lock(mSnapshotMutex);

    // First update current snapshot
    if (!snapshot ||
        snapshot->getLedgerSeq() != managerSnapshot->getLedgerSeq())
    {
        // Should only update with a newer snapshot
        releaseAssert(!snapshot || snapshot->getLedgerSeq() <
                                       managerSnapshot->getLedgerSeq());
        snapshot = std::make_unique<SnapshotT>(*managerSnapshot);
    }

    // Then update historical snapshots (if any exist)
    if (managerHistoricalSnapshots.empty())
    {
        return;
    }

    // If size of manager's history map is different, or if the oldest snapshot
    // ledger seq is different, we need to update.
    if (managerHistoricalSnapshots.size() != historicalSnapshots.size() ||
        managerHistoricalSnapshots.begin()->first !=
            historicalSnapshots.begin()->first)
    {
        // Copy current snapshot map into historicalSnapshots
        historicalSnapshots.clear();
        for (auto const& [ledgerSeq, snap] : managerHistoricalSnapshots)
        {
            historicalSnapshots.emplace(ledgerSeq,
                                        std::make_unique<SnapshotT>(*snap));
        }
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& liveSnapshot,
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&&
        hotArchiveSnapshot)
{
    releaseAssert(threadIsMain());

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
    std::unique_lock<std::shared_mutex> lock(mSnapshotMutex);
    updateSnapshot(mCurrLiveSnapshot, mLiveHistoricalSnapshots, liveSnapshot);
    updateSnapshot(mCurrHotArchiveSnapshot, mHotArchiveHistoricalSnapshots,
                   hotArchiveSnapshot);
}

void
BucketSnapshotManager::startPointLoadTimer() const
{
    releaseAssert(threadIsMain());
    releaseAssert(!mTimerStart);
    mTimerStart = mApp.getClock().now();
}

void
BucketSnapshotManager::endPointLoadTimer(LedgerEntryType t,
                                         bool bloomMiss) const
{
    releaseAssert(threadIsMain());
    releaseAssert(mTimerStart);
    auto duration = mApp.getClock().now() - *mTimerStart;
    mTimerStart.reset();

    // We expect about 0.1% of lookups to encounter a bloom miss. To avoid noise
    // in disk performance metrics, we only track metrics for entries that did
    // not encounter a bloom miss.
    if (!bloomMiss)
    {
        auto iter = mPointTimers.find(t);
        if (iter == mPointTimers.end())
        {
            auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(t);
            auto& metric =
                mApp.getMetrics().NewTimer({"bucketlistDB", "point", label});
            iter = mPointTimers.emplace(t, metric).first;
        }

        iter->second.Update(duration);
    }
}

template void
BucketSnapshotManager::maybeUpdateSnapshot<BucketListSnapshot<LiveBucket>>(
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>& snapshot,
    std::map<uint32_t, std::unique_ptr<BucketListSnapshot<LiveBucket> const>>&
        historicalSnapshots) const;
template void BucketSnapshotManager::maybeUpdateSnapshot<
    BucketListSnapshot<HotArchiveBucket>>(
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>& snapshot,
    std::map<uint32_t,
             std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>>&
        historicalSnapshots) const;
}