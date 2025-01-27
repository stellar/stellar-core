// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/SearchableBucketList.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketListSnapshotBase.h"
#include "bucket/LiveBucketList.h"
#include "ledger/LedgerTxn.h"
#include "util/GlobalChecks.h"

#include <medida/timer.h>

namespace stellar
{
EvictionResultCandidates
SearchableLiveBucketListSnapshot::scanForEviction(
    uint32_t ledgerSeq, EvictionCounters& counters,
    EvictionIterator evictionIter, std::shared_ptr<EvictionStatistics> stats,
    StateArchivalSettings const& sas, uint32_t ledgerVers) const
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

    EvictionResultCandidates result(sas);
    auto startIter = evictionIter;
    auto scanSize = sas.evictionScanSize;

    for (;;)
    {
        auto const& b = getBucketFromIter(evictionIter);
        LiveBucketList::checkIfEvictionScanIsStuck(
            evictionIter, sas.evictionScanSize, b.getRawBucket(), counters);

        // If we scan scanSize before hitting bucket EOF, exit early
        if (b.scanForEviction(evictionIter, scanSize, ledgerSeq,
                              result.eligibleEntries, *this,
                              ledgerVers) == Loop::COMPLETE)
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
std::optional<std::vector<typename BucketT::LoadT>>
SearchableBucketListSnapshotBase<BucketT>::loadKeysInternal(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    LedgerKeyMeter* lkMeter, std::optional<uint32_t> ledgerSeq) const
{
    ZoneScoped;

    // Make a copy of the key set, this loop is destructive
    auto keys = inKeys;
    std::vector<typename BucketT::LoadT> entries;
    auto loadKeysLoop = [&](auto const& b) {
        b.loadKeys(keys, entries, lkMeter);
        return keys.empty() ? Loop::COMPLETE : Loop::INCOMPLETE;
    };

    if (!ledgerSeq || *ledgerSeq == mSnapshot->getLedgerSeq())
    {
        loopAllBuckets(loadKeysLoop, *mSnapshot);
    }
    else
    {
        auto iter = mHistoricalSnapshots.find(*ledgerSeq);
        if (iter == mHistoricalSnapshots.end())
        {
            return std::nullopt;
        }

        releaseAssert(iter->second);
        loopAllBuckets(loadKeysLoop, *iter->second);
    }

    return entries;
}

// This query has two steps:
//  1. For each bucket, determine what PoolIDs contain the target asset via the
//     assetToPoolID index
//  2. Perform a bulk lookup for all possible trustline keys, that is, all
//     trustlines with the given accountID and poolID from step 1
std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    ZoneScoped;

    // This query should only be called during TX apply
    releaseAssert(mSnapshot);

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

        return Loop::INCOMPLETE; // continue
    };

    loopAllBuckets(trustLineLoop, *mSnapshot);

    auto timer =
        getBulkLoadTimer("poolshareTrustlines", trustlinesToLoad.size())
            .TimeScope();

    std::vector<LedgerEntry> result;
    auto loadKeysLoop = [&](auto const& b) {
        b.loadKeys(trustlinesToLoad, result, /*lkMeter=*/nullptr);
        return trustlinesToLoad.empty() ? Loop::COMPLETE : Loop::INCOMPLETE;
    };

    loopAllBuckets(loadKeysLoop, *mSnapshot);
    return result;
}

std::vector<InflationWinner>
SearchableLiveBucketListSnapshot::loadInflationWinners(size_t maxWinners,
                                                       int64_t minBalance) const
{
    ZoneScoped;
    releaseAssert(mSnapshot);

    // This is a legacy query, should only be called by main thread during
    // catchup
    auto timer = getBulkLoadTimer("inflationWinners", 0).TimeScope();

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

        return Loop::INCOMPLETE;
    };

    loopAllBuckets(countVotesInBucket, *mSnapshot);
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
            winners.push_back(
                InflationWinner{iter->second->first, iter->first});
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

std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadKeysWithLimits(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    LedgerKeyMeter* lkMeter) const
{
    auto timer = getBulkLoadTimer("prefetch", inKeys.size()).TimeScope();
    auto op = loadKeysInternal(inKeys, lkMeter, std::nullopt);
    releaseAssertOrThrow(op);
    return std::move(*op);
}

SearchableLiveBucketListSnapshot::SearchableLiveBucketListSnapshot(
    BucketSnapshotManager const& snapshotManager,
    AppConnector const& appConnector, SnapshotPtrT<LiveBucket>&& snapshot,
    std::map<uint32_t, SnapshotPtrT<LiveBucket>>&& historicalSnapshots)
    : SearchableBucketListSnapshotBase<LiveBucket>(
          snapshotManager, appConnector, std::move(snapshot),
          std::move(historicalSnapshots))
{
}

SearchableHotArchiveBucketListSnapshot::SearchableHotArchiveBucketListSnapshot(
    BucketSnapshotManager const& snapshotManager,
    AppConnector const& appConnector, SnapshotPtrT<HotArchiveBucket>&& snapshot,
    std::map<uint32_t, SnapshotPtrT<HotArchiveBucket>>&& historicalSnapshots)
    : SearchableBucketListSnapshotBase<HotArchiveBucket>(
          snapshotManager, appConnector, std::move(snapshot),
          std::move(historicalSnapshots))
{
}

std::vector<HotArchiveBucketEntry>
SearchableHotArchiveBucketListSnapshot::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const
{
    auto op = loadKeysInternal(inKeys, /*lkMeter=*/nullptr, std::nullopt);
    releaseAssertOrThrow(op);
    return std::move(*op);
}
}
