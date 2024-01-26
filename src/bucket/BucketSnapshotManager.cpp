// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketListSnapshot.h"
#include "main/Application.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    medida::MetricsRegistry& metrics,
    std::unique_ptr<BucketListSnapshot const>&& snapshot)
    : mMetrics(metrics)
    , mCurrentSnapshot(std::move(snapshot))
    , mBulkLoadMeter(
          mMetrics.NewMeter({"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(
          mMetrics.NewMeter({"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(
          mMetrics.NewMeter({"bucketlistDB", "bloom", "lookups"}, "bloom"))
{
    releaseAssert(threadIsMain());
}

std::unique_ptr<SearchableBucketListSnapshot>
BucketSnapshotManager::getSearchableBucketListSnapshot() const
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
        auto& metric = mMetrics.NewTimer({"bucketlistDB", "bulk", label});
        iter = mBulkTimers.emplace(label, metric).first;
    }

    return iter->second;
}

medida::Timer&
BucketSnapshotManager::getPointLoadTimer(LedgerEntryType t) const
{
    // For now, only keep metrics for the main thread. We can decide on what
    // metrics make sense when more background services are added later.
    releaseAssert(threadIsMain());

    auto iter = mPointTimers.find(t);
    if (iter == mPointTimers.end())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(t);
        auto& metric = mMetrics.NewTimer({"bucketlistDB", "point", label});
        iter = mPointTimers.emplace(t, metric).first;
    }

    return iter->second;
}

void
BucketSnapshotManager::maybeUpdateSnapshot(
    std::unique_ptr<BucketListSnapshot const>& snapshot) const
{
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    if (!snapshot ||
        snapshot->getLedgerSeq() != mCurrentSnapshot->getLedgerSeq())
    {
        // Should only update with a newer snapshot
        releaseAssert(!snapshot || snapshot->getLedgerSeq() <
                                       mCurrentSnapshot->getLedgerSeq());
        snapshot = std::make_unique<BucketListSnapshot>(*mCurrentSnapshot);
    }
}

void
BucketSnapshotManager::updateCurrentSnapshot(
    std::unique_ptr<BucketListSnapshot const>&& newSnapshot)
{
    releaseAssert(newSnapshot);
    releaseAssert(threadIsMain());
    std::lock_guard<std::recursive_mutex> lock(mSnapshotMutex);
    releaseAssert(!mCurrentSnapshot || newSnapshot->getLedgerSeq() >=
                                           mCurrentSnapshot->getLedgerSeq());
    mCurrentSnapshot.swap(newSnapshot);
}
}