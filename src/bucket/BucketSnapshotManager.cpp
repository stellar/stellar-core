// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketListSnapshot.h"
#include "main/Application.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h" // IWYU pragma: keep

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    Application& app,
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& snapshot,
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&&
        hotArchiveSnapshot)
    : mApp(app)
    , mCurrentSnapshot(std::move(snapshot))
    , mCurrentHotArchiveSnapshot(std::move(hotArchiveSnapshot))
    , mBulkLoadMeter(app.getMetrics().NewMeter(
          {"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "lookups"}, "bloom"))
{
    releaseAssert(threadIsMain());
    releaseAssert(mCurrentSnapshot);
    releaseAssert(mCurrentHotArchiveSnapshot);
}

std::shared_ptr<SearchableLiveBucketListSnapshot>
BucketSnapshotManager::getSearchableBucketListSnapshot() const
{
    // Can't use std::make_shared due to private constructor
    return std::shared_ptr<SearchableLiveBucketListSnapshot>(
        new SearchableLiveBucketListSnapshot(*this));
}

std::shared_ptr<SearchableHotArchiveBucketListSnapshot>
BucketSnapshotManager::getSearchableHotArchiveBucketListSnapshot() const
{
    releaseAssert(mCurrentHotArchiveSnapshot);
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

void
BucketSnapshotManager::maybeUpdateSnapshot(
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>& snapshot) const
{
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    if (!snapshot ||
        snapshot->getLedgerSeq() != mCurrentSnapshot->getLedgerSeq())
    {
        // Should only update with a newer snapshot
        releaseAssert(!snapshot || snapshot->getLedgerSeq() <
                                       mCurrentSnapshot->getLedgerSeq());
        snapshot =
            std::make_unique<BucketListSnapshot<LiveBucket>>(*mCurrentSnapshot);
    }
}

void
BucketSnapshotManager::maybeUpdateHotArchiveSnapshot(
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>& snapshot) const
{
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    if (!snapshot ||
        snapshot->getLedgerSeq() != mCurrentHotArchiveSnapshot->getLedgerSeq())
    {
        // Should only update with a newer snapshot
        releaseAssert(!snapshot ||
                      snapshot->getLedgerSeq() <
                          mCurrentHotArchiveSnapshot->getLedgerSeq());
        snapshot = std::make_unique<BucketListSnapshot<HotArchiveBucket>>(
            *mCurrentHotArchiveSnapshot);
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    std::unique_ptr<BucketListSnapshot<LiveBucket> const>&& newSnapshot)
{
    releaseAssert(newSnapshot);
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    releaseAssert(!mCurrentSnapshot || newSnapshot->getLedgerSeq() >=
                                           mCurrentSnapshot->getLedgerSeq());
    mCurrentSnapshot.swap(newSnapshot);
}

void
BucketSnapshotManager::updateCurrentHotArchiveSnapshot(
    std::unique_ptr<BucketListSnapshot<HotArchiveBucket> const>&& newSnapshot)
{
    releaseAssert(newSnapshot);
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    releaseAssert(!mCurrentHotArchiveSnapshot ||
                  newSnapshot->getLedgerSeq() >=
                      mCurrentHotArchiveSnapshot->getLedgerSeq());
    mCurrentHotArchiveSnapshot.swap(newSnapshot);
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