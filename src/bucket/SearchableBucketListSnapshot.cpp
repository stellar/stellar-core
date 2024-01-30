// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/SearchableBucketListSnapshot.h"
#include "bucket/BucketInputIterator.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"

#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

medida::Timer&
SearchableBucketListSnapshot::recordBulkLoadMetrics(std::string const& label,
                                                    size_t numEntries) const
{
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

medida::Timer&
SearchableBucketListSnapshot::getPointLoadTimer(LedgerEntryType t) const
{
    auto iter = mPointTimers.find(t);
    if (iter == mPointTimers.end())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(t);
        auto& metric =
            mApp.getMetrics().NewTimer({"bucketlistDB", "point", label});
        iter = mPointTimers.emplace(t, metric).first;
    }

    return iter->second;
}

bool
SearchableBucketListSnapshot::isWithinAllowedLedgerDrift(
    uint32_t allowedLedgerDrift) const
{
    auto currLCL = mApp.getLedgerManager().getLastClosedLedgerNum();

    // Edge case: genesis ledger
    auto minimumLCL =
        allowedLedgerDrift > currLCL ? 0 : currLCL - allowedLedgerDrift;

    return mLCL >= minimumLCL;
}

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
SearchableBucketListSnapshot::getLedgerEntry(LedgerKey const& k) const
{
    ZoneScoped;
    auto timer = getPointLoadTimer(k.type()).TimeScope();

    // Snapshots not currently supported, all access must be up to date
    releaseAssert(isWithinAllowedLedgerDrift(0));

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
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const
{
    ZoneScoped;
    auto timer = recordBulkLoadMetrics("prefetch", inKeys.size()).TimeScope();

    // Snapshots not currently supported, all access must be up to date
    releaseAssert(isWithinAllowedLedgerDrift(0));

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

std::vector<LedgerEntry>
SearchableBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    ZoneScoped;
    auto timer = recordBulkLoadMetrics("poolshareTrustlines", 0).TimeScope();

    // Snapshots not currently supported, all access must be up to date
    releaseAssert(isWithinAllowedLedgerDrift(0));

    UnorderedMap<LedgerKey, LedgerEntry> liquidityPoolToTrustline;
    UnorderedSet<LedgerKey> deadTrustlines;
    LedgerKeySet liquidityPoolKeysToSearch;

    // First get all the poolshare trustlines for the given account
    auto trustLineLoop = [&](SearchableBucketSnapshot const& b) {
        b.loadPoolShareTrustLinessByAccount(accountID, deadTrustlines,
                                            liquidityPoolToTrustline,
                                            liquidityPoolKeysToSearch);
        return false; // continue
    };
    loopAllBuckets(trustLineLoop);

    // Load all the LiquidityPool entries that the account has a trustline for.
    auto liquidityPoolEntries = loadKeys(liquidityPoolKeysToSearch);
    // pools always exist when there are trustlines
    releaseAssertOrThrow(liquidityPoolEntries.size() ==
                         liquidityPoolKeysToSearch.size());
    // Filter out liquidity pools that don't match the asset we're looking for
    std::vector<LedgerEntry> result;
    result.reserve(liquidityPoolEntries.size());
    for (const auto& e : liquidityPoolEntries)
    {
        releaseAssert(e.data.type() == LIQUIDITY_POOL);
        auto const& params =
            e.data.liquidityPool().body.constantProduct().params;
        if (compareAsset(params.assetA, asset) ||
            compareAsset(params.assetB, asset))
        {
            auto trustlineIter =
                liquidityPoolToTrustline.find(LedgerEntryKey(e));
            releaseAssert(trustlineIter != liquidityPoolToTrustline.end());
            result.emplace_back(trustlineIter->second);
        }
    }

    return result;
}

std::vector<InflationWinner>
SearchableBucketListSnapshot::loadInflationWinners(size_t maxWinners,
                                                   int64_t minBalance) const
{
    ZoneScoped;
    auto timer = recordBulkLoadMetrics("inflationWinners", 0).TimeScope();

    // Snapshots not currently supported, all access must be up to date
    releaseAssert(isWithinAllowedLedgerDrift(0));

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
// BucketManager::mBucketSnapshotMutex.
SearchableBucketListSnapshot::SearchableBucketListSnapshot(Application& app,
                                                           BucketList const& bl)
    : mApp(app)
    , mLCL(app.getLedgerManager().getLastClosedLedgerNum())
    , mBulkLoadMeter(app.getMetrics().NewMeter(
          {"bucketlistDB", "query", "loads"}, "query"))
    , mBloomMisses(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "misses"}, "bloom"))
    , mBloomLookups(app.getMetrics().NewMeter(
          {"bucketlistDB", "bloom", "lookups"}, "bloom"))
    , mEvictionCounters(app)
{
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto const& level = bl.getLevel(i);
        mLevels.emplace_back(SearchableBucketLevelSnapshot(level));
    }
}
}