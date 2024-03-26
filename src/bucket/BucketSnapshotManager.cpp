// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketListSnapshot.h"
#include "main/Application.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

BucketSnapshotManager::BucketSnapshotManager(
    medida::MetricsRegistry& metrics, BucketList const& bl,
    std::recursive_mutex& bucketListMutex)
    : mMetrics(metrics)
    , mBucketList(bl)
    , mBucketListMutex(bucketListMutex)
    , mBulkLoadMeter(
          mMetrics.NewMeter({"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(
          mMetrics.NewMeter({"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(
          mMetrics.NewMeter({"bucketlistDB", "bloom", "lookups"}, "bloom"))
{
}

std::unique_ptr<SearchableBucketListSnapshot>
BucketSnapshotManager::getSearchableBucketListSnapshot() const
{
    std::lock_guard<std::recursive_mutex> lock(mBucketListMutex);

    // Note: cannot use make_unique due to private constructor
    return std::unique_ptr<SearchableBucketListSnapshot>(
        new SearchableBucketListSnapshot(*this, mBucketList));
}

medida::Timer&
BucketSnapshotManager::recordBulkLoadMetrics(std::string const& label,
                                             size_t numEntries) const
{
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
    SearchableBucketListSnapshot& snapshot) const
{
    std::lock_guard<std::recursive_mutex> lock(mBucketListMutex);
    uint32_t currLedgerSeq = mBucketList.getLedgerSeq();

    if (currLedgerSeq != snapshot.mLedgerSeq)
    {
        snapshot.mLedgerSeq = currLedgerSeq;
        snapshot.mLevels.clear();

        for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
        {
            auto const& level = mBucketList.getLevel(i);
            snapshot.mLevels.emplace_back(SearchableBucketLevelSnapshot(level));
        }
    }
}

}