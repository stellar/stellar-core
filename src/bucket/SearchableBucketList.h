#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshotBase.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"

namespace stellar
{
class SearchableLiveBucketListSnapshot
    : public SearchableBucketListSnapshotBase<LiveBucket>
{
    SearchableLiveBucketListSnapshot(
        BucketSnapshotManager const& snapshotManager,
        AppConnector const& appConnector, SnapshotPtrT<LiveBucket>&& snapshot,
        std::map<uint32_t, SnapshotPtrT<LiveBucket>>&& historicalSnapshots);

  public:
    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset) const;

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;

    std::vector<LedgerEntry>
    loadKeysWithLimits(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                       LedgerKeyMeter* lkMeter) const;

    EvictionResultCandidates scanForEviction(
        uint32_t ledgerSeq, EvictionCounters& counters, EvictionIterator iter,
        std::shared_ptr<EvictionStatistics> stats,
        StateArchivalSettings const& sas, uint32_t ledgerVers) const;

    friend SearchableSnapshotConstPtr
    BucketSnapshotManager::copySearchableLiveBucketListSnapshot() const;
};

class SearchableHotArchiveBucketListSnapshot
    : public SearchableBucketListSnapshotBase<HotArchiveBucket>
{
    SearchableHotArchiveBucketListSnapshot(
        BucketSnapshotManager const& snapshotManager,
        AppConnector const& appConnector,
        SnapshotPtrT<HotArchiveBucket>&& snapshot,
        std::map<uint32_t, SnapshotPtrT<HotArchiveBucket>>&&
            historicalSnapshots);

  public:
    std::vector<HotArchiveBucketEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const;

    friend SearchableHotArchiveSnapshotConstPtr
    BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot() const;
};
}