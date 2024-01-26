#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "bucket/SearchableBucketSnapshot.h"

namespace stellar
{

struct SearchableBucketLevelSnapshot
{
    SearchableBucketSnapshot curr;
    SearchableBucketSnapshot snap;

    SearchableBucketLevelSnapshot(BucketLevel const& level);
};

// A lightweight wrapper around the BucketList for thread safe BucketListDB
// lookups
class SearchableBucketListSnapshot : public NonMovable
{
    Application& mApp;
    std::vector<SearchableBucketLevelSnapshot> mLevels;

    // Last closed ledger sequence that this BucketList snapshot is based off of
    uint32_t mLCL;

    mutable UnorderedMap<LedgerEntryType, medida::Timer&> mPointTimers{};
    mutable UnorderedMap<std::string, medida::Timer&> mBulkTimers{};

    medida::Meter& mBulkLoadMeter;
    medida::Meter& mBloomMisses;
    medida::Meter& mBloomLookups;

    EvictionCounters mEvictionCounters;

    // Loops through all buckets, starting with curr at level 0, then snap at
    // level 0, etc. Calls f on each bucket. Exits early if function
    // returns true
    void loopAllBuckets(
        std::function<bool(SearchableBucketSnapshot const&)> f) const;

    medida::Timer& recordBulkLoadMetrics(std::string const& label,
                                         size_t numEntries) const;

    medida::Timer& getPointLoadTimer(LedgerEntryType t) const;

  public:
    // TODO: Private constructor so only BucketManager can create this class
    // with a mutex check
    SearchableBucketListSnapshot(Application& app, BucketList const& bl);

    std::vector<LedgerEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const;

    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset) const;

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;

    std::shared_ptr<LedgerEntry> getLedgerEntry(LedgerKey const& k) const;
};
}