#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/SearchableBucketSnapshot.h"

namespace medida
{
class Timer;
}

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
class SearchableBucketListSnapshot : public NonMovableOrCopyable
{
    std::vector<SearchableBucketLevelSnapshot> mLevels;
    BucketSnapshotManager const& mSnapshotManager;

    // ledgerSeq that this BucketList snapshot is based off of
    uint32_t mLedgerSeq;

    // Loops through all buckets, starting with curr at level 0, then snap at
    // level 0, etc. Calls f on each bucket. Exits early if function
    // returns true
    void loopAllBuckets(
        std::function<bool(SearchableBucketSnapshot const&)> f) const;

    SearchableBucketListSnapshot(BucketSnapshotManager const& snapshotManager,
                                 BucketList const& bl);

  public:
    std::vector<LedgerEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys);

    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset);

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance);

    std::shared_ptr<LedgerEntry> getLedgerEntry(LedgerKey const& k);

    friend class BucketSnapshotManager;
};
}