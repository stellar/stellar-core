// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/BucketListBase.h"
#include "bucket/LiveBucket.h"
#include "crypto/SecretKey.h" // IWYU pragma: keep
#include "ledger/LedgerTxn.h"
#include "main/AppConnector.h"

#include "util/GlobalChecks.h"
#include <medida/metrics_registry.h>
#include <optional>
#include <vector>

namespace stellar
{
template <class BucketT>
BucketListSnapshot<BucketT>::BucketListSnapshot(
    BucketListBase<BucketT> const& bl, LedgerHeader header)
    : mHeader(std::move(header))
{
    for (uint32_t i = 0; i < BucketListBase<BucketT>::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        mLevels.emplace_back(BucketLevelSnapshot<BucketT>(level));
    }
}

template <class BucketT>
BucketListSnapshot<BucketT>::BucketListSnapshot(
    BucketListSnapshot<BucketT> const& snapshot)
    : mLevels(snapshot.mLevels), mHeader(snapshot.mHeader)
{
}

template <class BucketT>
std::vector<BucketLevelSnapshot<BucketT>> const&
BucketListSnapshot<BucketT>::getLevels() const
{
    return mLevels;
}

template <class BucketT>
uint32_t
BucketListSnapshot<BucketT>::getLedgerSeq() const
{
    return mHeader.ledgerSeq;
}

template <class BucketT>
LedgerHeader const&
SearchableBucketListSnapshotBase<BucketT>::getLedgerHeader() const
{
    releaseAssert(mSnapshot);
    return mSnapshot->getLedgerHeader();
}

template <class BucketT>
void
SearchableBucketListSnapshotBase<BucketT>::loopAllBuckets(
    std::function<Loop(BucketSnapshotT const&)> f,
    BucketListSnapshot<BucketT> const& snapshot) const
{
    for (auto const& lev : snapshot.getLevels())
    {
        auto processBucket = [f](BucketSnapshotT const& b) {
            if (b.isEmpty())
            {
                return Loop::INCOMPLETE;
            }

            return f(b);
        };

        if (processBucket(lev.curr) == Loop::COMPLETE ||
            processBucket(lev.snap) == Loop::COMPLETE)
        {
            return;
        }
    }
}

template <class BucketT>
std::shared_ptr<typename BucketT::LoadT const>
SearchableBucketListSnapshotBase<BucketT>::load(LedgerKey const& k) const
{
    ZoneScoped;

    std::shared_ptr<typename BucketT::LoadT const> result{};
    auto timerIter = mPointTimers.find(k.type());
    releaseAssert(timerIter != mPointTimers.end());
    auto timer = timerIter->second.TimeScope();

    // Search function called on each Bucket in BucketList until we find the key
    auto loadKeyBucketLoop = [&](auto const& b) {
        auto [be, bloomMiss] = b.getBucketEntry(k);
        if (bloomMiss)
        {
            // Reset timer on bloom miss to avoid outlier metrics, since we
            // really only want to measure disk performance
            timer.Reset();
        }

        if (be)
        {
            result = BucketT::bucketEntryToLoadResult(be);
            return Loop::COMPLETE;
        }
        else
        {
            return Loop::INCOMPLETE;
        }
    };

    loopAllBuckets(loadKeyBucketLoop, *mSnapshot);
    return result;
}

template <class BucketT>
std::optional<std::vector<typename BucketT::LoadT>>
SearchableBucketListSnapshotBase<BucketT>::loadKeysFromLedger(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    uint32_t ledgerSeq) const
{
    return loadKeysInternal(inKeys, /*lkMeter=*/nullptr, ledgerSeq);
}

template <class BucketT>
BucketLevelSnapshot<BucketT>::BucketLevelSnapshot(
    BucketLevel<BucketT> const& level)
    : curr(level.getCurr()), snap(level.getSnap())
{
}

template <class BucketT>
SearchableBucketListSnapshotBase<BucketT>::SearchableBucketListSnapshotBase(
    BucketSnapshotManager const& snapshotManager, AppConnector const& app,
    SnapshotPtrT<BucketT>&& snapshot,
    std::map<uint32_t, SnapshotPtrT<BucketT>>&& historicalSnapshots)
    : mSnapshotManager(snapshotManager)
    , mSnapshot(std::move(snapshot))
    , mHistoricalSnapshots(std::move(historicalSnapshots))
    , mAppConnector(app)
    , mBulkLoadMeter(app.getMetrics().NewMeter(
          {BucketT::METRIC_STRING, "query", "loads"}, "query"))
    , mBloomMisses(app.getMetrics().NewMeter(
          {BucketT::METRIC_STRING, "bloom", "misses"}, "bloom"))
    , mBloomLookups(app.getMetrics().NewMeter(
          {BucketT::METRIC_STRING, "bloom", "lookups"}, "bloom"))
{
    // Initialize point load timers for each LedgerEntry type
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(
            static_cast<LedgerEntryType>(t));
        auto& metric =
            app.getMetrics().NewTimer({BucketT::METRIC_STRING, "point", label});
        mPointTimers.emplace(static_cast<LedgerEntryType>(t), metric);
    }
}

template <class BucketT>
SearchableBucketListSnapshotBase<BucketT>::~SearchableBucketListSnapshotBase()
{
}

template <class BucketT>
medida::Timer&
SearchableBucketListSnapshotBase<BucketT>::getBulkLoadTimer(
    std::string const& label, size_t numEntries) const
{
    if (numEntries != 0)
    {
        mBulkLoadMeter.Mark(numEntries);
    }

    auto iter = mBulkTimers.find(label);
    if (iter == mBulkTimers.end())
    {
        auto& metric = mAppConnector.getMetrics().NewTimer(
            {BucketT::METRIC_STRING, "bulk", label});
        iter = mBulkTimers.emplace(label, metric).first;
    }

    return iter->second;
}

template struct BucketLevelSnapshot<LiveBucket>;
template struct BucketLevelSnapshot<HotArchiveBucket>;
template class BucketListSnapshot<LiveBucket>;
template class BucketListSnapshot<HotArchiveBucket>;
template class SearchableBucketListSnapshotBase<LiveBucket>;
template class SearchableBucketListSnapshotBase<HotArchiveBucket>;
}