// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/SearchableBucketListSnapshot.h"
#include "bucket/BucketInputIterator.h"
#include "crypto/SecretKey.h"
#include "ledger/LedgerTxn.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

void
SearchableBucketListSnapshot::loopAllBuckets(
    std::function<bool(SearchableBucketSnapshot const&)> f) const
{
    for (auto const& lev : mLevels)
    {
        // Return true if we should exit loop early
        auto processBucket = [f](SearchableBucketSnapshot const& b) {
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

std::shared_ptr<LedgerEntry>
SearchableBucketListSnapshot::getLedgerEntry(LedgerKey const& k)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(*this);

    // TODO: Metrics only on main thread
    auto timer = mSnapshotManager.getPointLoadTimer(k.type()).TimeScope();

    std::shared_ptr<LedgerEntry> result{};

    auto f = [&](SearchableBucketSnapshot const& b) {
        auto be = b.getBucketEntry(k);
        if (be.has_value())
        {
            result =
                be.value().type() == DEADENTRY
                    ? nullptr
                    : std::make_shared<LedgerEntry>(be.value().liveEntry());
            return true;
        }
        else
        {
            return false;
        }
    };

    loopAllBuckets(f);
    return result;
}

std::vector<LedgerEntry>
SearchableBucketListSnapshot::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(*this);

    auto timer =
        mSnapshotManager.recordBulkLoadMetrics("prefetch", inKeys.size())
            .TimeScope();

    std::vector<LedgerEntry> entries;

    // Make a copy of the key set, this loop is destructive
    auto keys = inKeys;
    auto f = [&](SearchableBucketSnapshot const& b) {
        b.loadKeys(keys, entries);
        return keys.empty();
    };

    loopAllBuckets(f);
    return entries;
}

// This query has two steps:
//  1. For each bucket, determine what PoolIDs contain the target asset via the
//     assetToPoolID index
//  2. Perform a bulk lookup for all possible trustline keys, that is, all
//     trustlines with the given accountID and poolID from step 1
std::vector<LedgerEntry>
SearchableBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(*this);

    LedgerKeySet trustlinesToLoad;

    auto trustLineLoop = [&](SearchableBucketSnapshot const& b) {
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
    return loadKeys(trustlinesToLoad);
}

std::vector<InflationWinner>
SearchableBucketListSnapshot::loadInflationWinners(size_t maxWinners,
                                                   int64_t minBalance)
{
    ZoneScoped;
    mSnapshotManager.maybeUpdateSnapshot(*this);

    auto timer = mSnapshotManager.recordBulkLoadMetrics("inflationWinners", 0)
                     .TimeScope();

    UnorderedMap<AccountID, int64_t> voteCount;
    UnorderedSet<AccountID> seen;

    auto countVotesInBucket = [&](SearchableBucketSnapshot const& b) {
        for (BucketInputIterator in(b.getRawBucket()); in; ++in)
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

SearchableBucketLevelSnapshot::SearchableBucketLevelSnapshot(
    BucketLevel const& level)
    : curr(level.getCurr()), snap(level.getSnap())
{
}

// This is not thread safe, must call while holding
// BucketManager::mBucketListMutex.
SearchableBucketListSnapshot::SearchableBucketListSnapshot(
    BucketSnapshotManager const& snapshotManager, BucketList const& bl)
    : mSnapshotManager(snapshotManager)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        mLevels.emplace_back(SearchableBucketLevelSnapshot(level));
    }
}
}