// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshot.h"
#include "bucket/Bucket.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketList.h"
#include "bucket/BucketSnapshot.h"
#include "crypto/SecretKey.h" // IWYU pragma: keep
#include "ledger/LedgerTxn.h"

#include "medida/timer.h"
#include "util/GlobalChecks.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"
#include <memory>

namespace stellar
{
template <class BucketT>
BucketListSnapshot<BucketT>::BucketListSnapshot(
    BucketListBase<BucketT> const& bl, uint32_t ledgerSeq)
    : mLedgerSeq(ledgerSeq)
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
    BucketListSnapshot const& snapshot)
    : mLevels(snapshot.mLevels), mLedgerSeq(snapshot.mLedgerSeq)
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
    return mLedgerSeq;
}

template <class BucketT>
void
SearchableBucketListSnapshotBase<BucketT>::loopAllBuckets(
    std::function<bool(BucketSnapshotT const&)> f) const
{
    releaseAssert(mSnapshot);

    for (auto const& lev : mSnapshot->getLevels())
    {
        // Return true if we should exit loop early
        auto processBucket = [f](BucketSnapshotT const& b) {
            if (b.isEmpty())
            {
                return false;
            }

            return f(b);
        };

        if (processBucket(lev.curr) || processBucket(lev.snap))
        {
            return;
        }
    }
}

EvictionResult
SearchableLiveBucketListSnapshot::scanForEviction(
    uint32_t ledgerSeq, EvictionCounters& counters,
    EvictionIterator evictionIter, std::shared_ptr<EvictionStatistics> stats,
    StateArchivalSettings const& sas)
{
    releaseAssert(mSnapshot);
    releaseAssert(stats);

    auto getBucketFromIter =
        [&levels = mSnapshot->getLevels()](
            EvictionIterator const& iter) -> LiveBucketSnapshot const& {
        auto& level = levels.at(iter.bucketListLevel);
        return iter.isCurrBucket ? level.curr : level.snap;
    };

    LiveBucketList::updateStartingEvictionIterator(
        evictionIter, sas.startingEvictionScanLevel, ledgerSeq);

    EvictionResult result(sas);
    auto startIter = evictionIter;
    auto scanSize = sas.evictionScanSize;

    for (;;)
    {
        auto const& b = getBucketFromIter(evictionIter);
        LiveBucketList::checkIfEvictionScanIsStuck(
            evictionIter, sas.evictionScanSize, b.getRawBucket(), counters);

        // If we scan scanSize before hitting bucket EOF, exit early
        if (b.scanForEviction(evictionIter, scanSize, ledgerSeq,
                              result.eligibleKeys, *this))
        {
            break;
        }

        // If we return back to the Bucket we started at, exit
        if (LiveBucketList::updateEvictionIterAndRecordStats(
                evictionIter, startIter, sas.startingEvictionScanLevel,
                ledgerSeq, stats, counters))
        {
            break;
        }
    }

    result.endOfRegionIterator = evictionIter;
    result.initialLedger = ledgerSeq;
    return result;
}

template <class BucketT>
std::shared_ptr<LedgerEntry>
SearchableBucketListSnapshotBase<BucketT>::getLedgerEntry(LedgerKey const& k)
{
    ZoneScoped;
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        mSnapshotManager.maybeUpdateSnapshot(mSnapshot);
    }
    else
    {
        // TODO::
        releaseAssert(false);
    }

    if (threadIsMain())
    {
        mSnapshotManager.startPointLoadTimer();
        auto [result, bloomMiss] = getLedgerEntryInternal(k);
        mSnapshotManager.endPointLoadTimer(k.type(), bloomMiss);
        return result;
    }
    else
    {
        auto [result, bloomMiss] = getLedgerEntryInternal(k);
        return result;
    }
}

template <class BucketT>
std::pair<std::shared_ptr<LedgerEntry>, bool>
SearchableBucketListSnapshotBase<BucketT>::getLedgerEntryInternal(
    LedgerKey const& k)
{
    std::shared_ptr<LedgerEntry> result{};
    auto sawBloomMiss = false;

    auto f = [&](BucketSnapshotT const& b) {
        auto [be, bloomMiss] = b.getBucketEntry(k);
        sawBloomMiss = sawBloomMiss || bloomMiss;

        if (be.has_value())
        {
            if constexpr (std::is_same_v<BucketT, LiveBucket>)
            {
                result =
                    be.value().type() == DEADENTRY
                        ? nullptr
                        : std::make_shared<LedgerEntry>(be.value().liveEntry());
            }
            else
            {
                releaseAssert(false);
                // TODO::W
            }

            return true;
        }
        else
        {
            return false;
        }
    };

    loopAllBuckets(f);
    return {result, sawBloomMiss};
}

template <class BucketT>
std::vector<LedgerEntry>
SearchableBucketListSnapshotBase<BucketT>::loadKeysInternal(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    LedgerKeyMeter* lkMeter)
{
    std::vector<LedgerEntry> entries;

    // Make a copy of the key set, this loop is destructive
    auto keys = inKeys;
    auto f = [&](BucketSnapshotT const& b) {
        b.loadKeysWithLimits(keys, entries, lkMeter);
        return keys.empty();
    };

    loopAllBuckets(f);
    return entries;
}

std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadKeysWithLimits(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    LedgerKeyMeter* lkMeter)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(mSnapshot);

    if (threadIsMain())
    {
        auto timer =
            mSnapshotManager.recordBulkLoadMetrics("prefetch", inKeys.size())
                .TimeScope();
        return loadKeysInternal(inKeys, lkMeter);
    }
    else
    {
        return loadKeysInternal(inKeys, lkMeter);
    }
}

// This query has two steps:
//  1. For each bucket, determine what PoolIDs contain the target asset via the
//     assetToPoolID index
//  2. Perform a bulk lookup for all possible trustline keys, that is, all
//     trustlines with the given accountID and poolID from step 1
std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset)
{
    ZoneScoped;

    // This query should only be called during TX apply
    releaseAssert(threadIsMain());
    mSnapshotManager.maybeUpdateSnapshot(mSnapshot);

    LedgerKeySet trustlinesToLoad;

    auto trustLineLoop = [&](auto const& rawB) {
        auto const& b = static_cast<LiveBucketSnapshot const&>(rawB);
        for (auto const& poolID : b.getPoolIDsByAsset(asset))
        {
            LedgerKey trustlineKey(TRUSTLINE);
            trustlineKey.trustLine().accountID = accountID;
            trustlineKey.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
            trustlineKey.trustLine().asset.liquidityPoolID() = poolID;
            trustlinesToLoad.emplace(trustlineKey);
        }

        return false; // continue
    };

    loopAllBuckets(trustLineLoop);

    auto timer = mSnapshotManager
                     .recordBulkLoadMetrics("poolshareTrustlines",
                                            trustlinesToLoad.size())
                     .TimeScope();
    return loadKeysInternal(trustlinesToLoad, nullptr);
}

std::vector<InflationWinner>
SearchableLiveBucketListSnapshot::loadInflationWinners(size_t maxWinners,
                                                       int64_t minBalance)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(mSnapshot);

    // This is a legacy query, should only be called by main thread during
    // catchup
    releaseAssert(threadIsMain());
    auto timer = mSnapshotManager.recordBulkLoadMetrics("inflationWinners", 0)
                     .TimeScope();

    UnorderedMap<AccountID, int64_t> voteCount;
    UnorderedSet<AccountID> seen;

    auto countVotesInBucket = [&](LiveBucketSnapshot const& b) {
        for (LiveBucketInputIterator in(b.getRawBucket()); in; ++in)
        {
            BucketEntry const& be = *in;
            if (be.type() == DEADENTRY)
            {
                if (be.deadEntry().type() == ACCOUNT)
                {
                    seen.insert(be.deadEntry().account().accountID);
                }
                continue;
            }

            // Account are ordered first, so once we see a non-account entry, no
            // other accounts are left in the bucket
            LedgerEntry const& le = be.liveEntry();
            if (le.data.type() != ACCOUNT)
            {
                break;
            }

            // Don't double count AccountEntry's seen in earlier levels
            AccountEntry const& ae = le.data.account();
            AccountID const& id = ae.accountID;
            if (!seen.insert(id).second)
            {
                continue;
            }

            if (ae.inflationDest && ae.balance >= 1000000000)
            {
                voteCount[*ae.inflationDest] += ae.balance;
            }
        }

        return false;
    };

    loopAllBuckets(countVotesInBucket);
    std::vector<InflationWinner> winners;

    // Check if we need to sort the voteCount by number of votes
    if (voteCount.size() > maxWinners)
    {

        // Sort Inflation winners by vote count in descending order
        std::map<int64_t, UnorderedMap<AccountID, int64_t>::const_iterator,
                 std::greater<int64_t>>
            voteCountSortedByCount;
        for (auto iter = voteCount.cbegin(); iter != voteCount.cend(); ++iter)
        {
            voteCountSortedByCount[iter->second] = iter;
        }

        // Insert first maxWinners entries that are larger thanminBalance
        for (auto iter = voteCountSortedByCount.cbegin();
             winners.size() < maxWinners && iter->first >= minBalance; ++iter)
        {
            // push back {AccountID, voteCount}
            winners.push_back({iter->second->first, iter->first});
        }
    }
    else
    {
        for (auto const& [id, count] : voteCount)
        {
            if (count >= minBalance)
            {
                winners.push_back({id, count});
            }
        }
    }

    return winners;
}

template <class BucketT>
BucketLevelSnapshot<BucketT>::BucketLevelSnapshot(
    BucketLevel<BucketT> const& level)
    : curr(level.getCurr()), snap(level.getSnap())
{
}

template <class BucketT>
SearchableBucketListSnapshotBase<BucketT>::SearchableBucketListSnapshotBase(
    BucketSnapshotManager const& snapshotManager)
    : mSnapshotManager(snapshotManager)
{
    // Initialize snapshot from SnapshotManager
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        mSnapshotManager.maybeUpdateSnapshot(mSnapshot);
    }
    else
    {
        // TODO:
        releaseAssert(false);
    }
}

template <class BucketT>
SearchableBucketListSnapshotBase<BucketT>::~SearchableBucketListSnapshotBase()
{
}

SearchableLiveBucketListSnapshot::SearchableLiveBucketListSnapshot(
    BucketSnapshotManager const& snapshotManager)
    : SearchableBucketListSnapshotBase<LiveBucket>(snapshotManager)
{
}

template struct BucketLevelSnapshot<LiveBucket>;
template struct BucketLevelSnapshot<HotArchiveBucket>;
template class BucketListSnapshot<LiveBucket>;
template class BucketListSnapshot<HotArchiveBucket>;
template class SearchableBucketListSnapshotBase<LiveBucket>;
template class SearchableBucketListSnapshotBase<HotArchiveBucket>;
}