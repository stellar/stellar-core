// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketListSnapshot.h"
#include "main/Application.h"
#include "util/XDRStream.h" // IWYU pragma: keep

#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include <shared_mutex>

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    Application& app, std::unique_ptr<BucketListSnapshot const>&& snapshot,
    uint32_t numHistoricalSnapshots)
    : mApp(app)
    , mCurrentSnapshot(std::move(snapshot))
    , mHistoricalSnapshots()
    , mNumHistoricalSnapshots(numHistoricalSnapshots)
    , mBulkLoadMeter(app.getMetrics().NewMeter(
          {"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "lookups"}, "bloom"))
{
    releaseAssert(threadIsMain());
}

std::unique_ptr<SearchableBucketListSnapshot>
BucketSnapshotManager::copySearchableBucketListSnapshot() const
{
    // Can't use std::make_unique due to private constructor
    return std::unique_ptr<SearchableBucketListSnapshot>(
        new SearchableBucketListSnapshot(*this));
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

void
BucketSnapshotManager::maybeUpdateSnapshot(
    std::unique_ptr<BucketListSnapshot const>& snapshot,
    std::map<uint32_t, std::unique_ptr<BucketListSnapshot const>>&
        historicalSnapshots) const
{
    // The canonical snapshot held by the BucketSnapshotManager is not being
    // modified. Rather, a thread is checking it's copy against the canonical
    // snapshot, so use a shared lock.
    std::shared_lock<std::shared_mutex> lock(mSnapshotMutex);

    // First update current snapshot
    if (!snapshot ||
        snapshot->getLedgerSeq() != mCurrentSnapshot->getLedgerSeq())
    {
        // Should only update with a newer snapshot
        releaseAssert(!snapshot || snapshot->getLedgerSeq() <
                                       mCurrentSnapshot->getLedgerSeq());
        snapshot = std::make_unique<BucketListSnapshot>(*mCurrentSnapshot);
    }

    // Then update historical snapshots (if any exist)
    if (mHistoricalSnapshots.empty())
    {
        return;
    }

    // If size of manager's history map is different, or if the oldest snapshot
    // ledger seq is different, we need to update.
    if (mHistoricalSnapshots.size() != historicalSnapshots.size() ||
        mHistoricalSnapshots.begin()->first !=
            historicalSnapshots.begin()->first)
    {
        // Copy current snapshot map into historicalSnapshots
        historicalSnapshots.clear();
        for (auto const& [ledgerSeq, snapshot] : mHistoricalSnapshots)
        {
            historicalSnapshots.emplace(
                ledgerSeq, std::make_unique<BucketListSnapshot>(*snapshot));
        }
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    std::unique_ptr<BucketListSnapshot const>&& newSnapshot)
{
    releaseAssert(newSnapshot);
    releaseAssert(threadIsMain());

    // Updating the BucketSnapshotManager canonical snapshot, must lock
    // exclusively for write access.
    std::unique_lock<std::shared_mutex> lock(mSnapshotMutex);
    releaseAssert(!mCurrentSnapshot || newSnapshot->getLedgerSeq() >=
                                           mCurrentSnapshot->getLedgerSeq());

    // First update historical snapshots
    if (mNumHistoricalSnapshots != 0)
    {
        // If historical snapshots are full, delete the oldest one
        if (mHistoricalSnapshots.size() == mNumHistoricalSnapshots)
        {
            mHistoricalSnapshots.erase(mHistoricalSnapshots.begin());
        }

        mHistoricalSnapshots.emplace(mCurrentSnapshot->getLedgerSeq(),
                                     std::move(mCurrentSnapshot));
        mCurrentSnapshot = nullptr;
    }

    mCurrentSnapshot.swap(newSnapshot);
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
}