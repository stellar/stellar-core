// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/SearchableBucketList.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h" // IWYU pragma: keep

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "xdr/Stellar-ledger-entries.h"
#include <shared_mutex>
#include <xdrpp/types.h>

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    Application& app, SnapshotPtrT<LiveBucket>&& snapshot,
    SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot,
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

    // Initialize point load timers for each LedgerEntry type
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(
            static_cast<LedgerEntryType>(t));
        auto& metric =
            mApp.getMetrics().NewTimer({"bucketlistDB", "point", label});
        mPointTimers.emplace(static_cast<LedgerEntryType>(t), metric);
    }
}

std::shared_ptr<SearchableLiveBucketListSnapshot const>
BucketSnapshotManager::copySearchableLiveBucketListSnapshot() const
{
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableLiveBucketListSnapshot>(
        new SearchableLiveBucketListSnapshot(*this));
}

std::shared_ptr<SearchableHotArchiveBucketListSnapshot const>
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

template <>
void
BucketSnapshotManager::maybeUpdateSnapshot<LiveBucket>(
    SnapshotPtrT<LiveBucket>& snapshot,
    std::map<uint32_t, SnapshotPtrT<LiveBucket>>& historicalSnapshots,
    bool forceUpdate) const
{
    maybeUpdateSnapshotInternal(snapshot, historicalSnapshots,
                                mCurrLiveSnapshot, mLiveHistoricalSnapshots,
                                forceUpdate);
}

template <>
void
BucketSnapshotManager::maybeUpdateSnapshot<HotArchiveBucket>(
    SnapshotPtrT<HotArchiveBucket>& snapshot,
    std::map<uint32_t, SnapshotPtrT<HotArchiveBucket>>& historicalSnapshots,
    bool forceUpdate) const
{
    maybeUpdateSnapshotInternal(snapshot, historicalSnapshots,
                                mCurrHotArchiveSnapshot,
                                mHotArchiveHistoricalSnapshots, forceUpdate);
}

template <class BucketT>
void
BucketSnapshotManager::maybeUpdateSnapshotInternal(
    SnapshotPtrT<BucketT>& snapshot,
    std::map<uint32_t, SnapshotPtrT<BucketT>>& historicalSnapshots,
    SnapshotPtrT<BucketT> const& managerSnapshot,
    std::map<uint32_t, SnapshotPtrT<BucketT>> const& managerHistoricalSnapshots,
    bool forceUpdate) const
{
    BUCKET_TYPE_ASSERT(BucketT);

    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock.
    std::shared_lock<std::shared_mutex> lock(mSnapshotMutex);

    // Should only update on snapshot creation
    releaseAssert(!snapshot);
    snapshot =
        std::make_unique<BucketListSnapshot<BucketT> const>(*managerSnapshot);

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
            historicalSnapshots.emplace(
                ledgerSeq,
                std::make_unique<BucketListSnapshot<BucketT> const>(*snap));
        }
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    SnapshotPtrT<LiveBucket>&& liveSnapshot,
    SnapshotPtrT<HotArchiveBucket>&& hotArchiveSnapshot)
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
        releaseAssert(iter != mPointTimers.end());
        iter->second.Update(duration);
    }
}

uint32_t
BucketSnapshotManager::getCurrentLedgerSeq() const
{
    std::shared_lock<std::shared_mutex> lock(mSnapshotMutex);
    return mCurrLiveSnapshot->getLedgerSeq();
}
}