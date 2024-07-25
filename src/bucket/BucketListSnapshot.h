#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketList.h"
#include "bucket/BucketManagerImpl.h"
#include "bucket/BucketSnapshot.h"
#include "bucket/BucketSnapshotManager.h"

namespace medida
{
class Timer;
}

namespace stellar
{

template <class BucketT> struct BucketLevelSnapshot
{
    static_assert(std::is_same_v<BucketT, LiveBucket> ||
                  std::is_same_v<BucketT, HotArchiveBucket>);

    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

    BucketSnapshotT curr;
    BucketSnapshotT snap;

    BucketLevelSnapshot(BucketLevel<BucketT> const& level);
};

template <class BucketT> class BucketListSnapshot : public NonMovable
{
    static_assert(std::is_same_v<BucketT, LiveBucket> ||
                  std::is_same_v<BucketT, HotArchiveBucket>);
    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

  private:
    std::vector<BucketLevelSnapshot<BucketT>> mLevels;

    // ledgerSeq that this BucketList snapshot is based off of
    uint32_t mLedgerSeq;

  public:
    BucketListSnapshot(BucketListBase<BucketT> const& bl, uint32_t ledgerSeq);

    // Only allow copies via constructor
    BucketListSnapshot(BucketListSnapshot const& snapshot);
    BucketListSnapshot& operator=(BucketListSnapshot const&) = delete;

    std::vector<BucketLevelSnapshot<BucketT>> const& getLevels() const;
    uint32_t getLedgerSeq() const;
};

// A lightweight wrapper around BucketListSnapshot for thread safe BucketListDB
// lookups.
//
// Any thread that needs to perform BucketList lookups should retrieve
// a single SearchableBucketListSnapshot instance from
// BucketListSnapshotManager. On each lookup, the SearchableBucketListSnapshot
// instance will check that the current snapshot is up to date via the
// BucketListSnapshotManager and will be refreshed accordingly. Callers can
// assume SearchableBucketListSnapshot is always up to date.
template <class BucketT>
class SearchableBucketListSnapshotBase : public NonMovableOrCopyable
{
    static_assert(std::is_same_v<BucketT, LiveBucket> ||
                  std::is_same_v<BucketT, HotArchiveBucket>);

    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

  protected:
    virtual ~SearchableBucketListSnapshotBase() = 0;

    BucketSnapshotManager const& mSnapshotManager;

    // Snapshot managed by SnapshotManager
    std::unique_ptr<BucketListSnapshot<BucketT> const> mSnapshot{};

    // Loops through all buckets, starting with curr at level 0, then snap at
    // level 0, etc. Calls f on each bucket. Exits early if function
    // returns true
    void loopAllBuckets(std::function<bool(BucketSnapshotT const&)> f) const;

    std::vector<LedgerEntry>
    loadKeysInternal(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                     LedgerKeyMeter* lkMeter);

    // Loads bucket entry for LedgerKey k. Returns <LedgerEntry, bloomMiss>,
    // where bloomMiss is true if a bloom miss occurred during the load.
    std::pair<std::shared_ptr<LedgerEntry>, bool>
    getLedgerEntryInternal(LedgerKey const& k);

    SearchableBucketListSnapshotBase(
        BucketSnapshotManager const& snapshotManager);

  public:
    std::shared_ptr<LedgerEntry> getLedgerEntry(LedgerKey const& k);
};

class SearchableLiveBucketListSnapshot
    : public SearchableBucketListSnapshotBase<LiveBucket>
{
    SearchableLiveBucketListSnapshot(
        BucketSnapshotManager const& snapshotManager);

  public:
    std::vector<LedgerEntry>
    loadKeysWithLimits(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                       LedgerKeyMeter* lkMeter = nullptr);

    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset);

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance);

    EvictionResult scanForEviction(uint32_t ledgerSeq,
                                   EvictionCounters& counters,
                                   EvictionIterator evictionIter,
                                   std::shared_ptr<EvictionStatistics> stats,
                                   StateArchivalSettings const& sas);

    friend std::shared_ptr<SearchableLiveBucketListSnapshot>
    BucketSnapshotManager::getSearchableBucketListSnapshot() const;
};
}