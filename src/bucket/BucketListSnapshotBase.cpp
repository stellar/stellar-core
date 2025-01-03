// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/BucketListBase.h"
#include "bucket/LiveBucket.h"
#include "crypto/SecretKey.h" // IWYU pragma: keep
#include "ledger/LedgerTxn.h"

#include "util/GlobalChecks.h"
#include <optional>
#include <vector>

namespace stellar
{
template <class BucketT>
BucketListSnapshot<BucketT>::BucketListSnapshot(
    BucketListBase<BucketT> const& bl, LedgerHeader header)
    : mHeader(std::move(header))
{
    releaseAssert(threadIsMain());

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
std::shared_ptr<typename BucketT::LoadT>
SearchableBucketListSnapshotBase<BucketT>::load(LedgerKey const& k) const
{
    ZoneScoped;

    std::shared_ptr<typename BucketT::LoadT> result{};
    auto sawBloomMiss = false;

    // Search function called on each Bucket in BucketList until we find the key
    auto loadKeyBucketLoop = [&](auto const& b) {
        auto [be, bloomMiss] = b.getBucketEntry(k);
        sawBloomMiss = sawBloomMiss || bloomMiss;

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

    if (threadIsMain())
    {
        mSnapshotManager.startPointLoadTimer();
        loopAllBuckets(loadKeyBucketLoop, *mSnapshot);
        mSnapshotManager.endPointLoadTimer(k.type(), sawBloomMiss);
        return result;
    }
    else
    {
        // TODO: Background metrics
        loopAllBuckets(loadKeyBucketLoop, *mSnapshot);
        return result;
    }
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
    BucketSnapshotManager const& snapshotManager,
    SnapshotPtrT<BucketT>&& snapshot,
    std::map<uint32_t, SnapshotPtrT<BucketT>>&& historicalSnapshots)
    : mSnapshotManager(snapshotManager)
    , mSnapshot(std::move(snapshot))
    , mHistoricalSnapshots(std::move(historicalSnapshots))
{
}

template <class BucketT>
SearchableBucketListSnapshotBase<BucketT>::~SearchableBucketListSnapshotBase()
{
}

template struct BucketLevelSnapshot<LiveBucket>;
template struct BucketLevelSnapshot<HotArchiveBucket>;
template class BucketListSnapshot<LiveBucket>;
template class BucketListSnapshot<HotArchiveBucket>;
template class SearchableBucketListSnapshotBase<LiveBucket>;
template class SearchableBucketListSnapshotBase<HotArchiveBucket>;
}