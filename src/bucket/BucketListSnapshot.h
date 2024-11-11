#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshot.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"

namespace medida
{
class Timer;
}

namespace stellar
{

template <class BucketT> struct BucketLevelSnapshot
{
    BUCKET_TYPE_ASSERT(BucketT);

    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

    BucketSnapshotT curr;
    BucketSnapshotT snap;

    BucketLevelSnapshot(BucketLevel<BucketT> const& level);
};

template <class BucketT> class BucketListSnapshot : public NonMovable
{
    BUCKET_TYPE_ASSERT(BucketT);
    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

  private:
    std::vector<BucketLevelSnapshot<BucketT>> mLevels;

    // LedgerHeader associated with this ledger state snapshot
    LedgerHeader const mHeader;

  public:
    BucketListSnapshot(BucketListBase<BucketT> const& bl, LedgerHeader hhe);

    // Only allow copies via constructor
    BucketListSnapshot(BucketListSnapshot const& snapshot);
    BucketListSnapshot& operator=(BucketListSnapshot const&) = delete;

    std::vector<BucketLevelSnapshot<BucketT>> const& getLevels() const;
    uint32_t getLedgerSeq() const;
    LedgerHeader const&
    getLedgerHeader() const
    {
        return mHeader;
    }
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
    BUCKET_TYPE_ASSERT(BucketT);

    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

  protected:
    virtual ~SearchableBucketListSnapshotBase() = 0;

    BucketSnapshotManager const& mSnapshotManager;

    // Snapshot managed by SnapshotManager
    SnapshotPtrT<BucketT> mSnapshot{};
    std::map<uint32_t, SnapshotPtrT<BucketT>> mHistoricalSnapshots;

    // Loops through all buckets, starting with curr at level 0, then snap at
    // level 0, etc. Calls f on each bucket. Exits early if function
    // returns Loop::COMPLETE.
    void loopAllBuckets(std::function<Loop(BucketSnapshotT const&)> f,
                        BucketListSnapshot<BucketT> const& snapshot) const;

    SearchableBucketListSnapshotBase(
        BucketSnapshotManager const& snapshotManager);

    std::optional<std::vector<typename BucketT::LoadT>>
    loadKeysInternal(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                     LedgerKeyMeter* lkMeter,
                     std::optional<uint32_t> ledgerSeq);

  public:
    uint32_t
    getLedgerSeq() const
    {
        return mSnapshot->getLedgerSeq();
    }

    LedgerHeader const& getLedgerHeader();

    // Loads inKeys from the specified historical snapshot. Returns
    // load_result_vec if the snapshot for the given ledger is
    // available, std::nullopt otherwise. Note that ledgerSeq is defined
    // as the state of the BucketList at the beginning of the ledger. This means
    // that for ledger N, the maximum lastModifiedLedgerSeq of any LedgerEntry
    // in the BucketList is N - 1.
    std::optional<std::vector<typename BucketT::LoadT>>
    loadKeysFromLedger(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                       uint32_t ledgerSeq);

    std::shared_ptr<typename BucketT::LoadT> load(LedgerKey const& k);
};

class SearchableLiveBucketListSnapshot
    : public SearchableBucketListSnapshotBase<LiveBucket>
{
    SearchableLiveBucketListSnapshot(
        BucketSnapshotManager const& snapshotManager);

  public:
    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset);

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance);

    std::vector<LedgerEntry>
    loadKeysWithLimits(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                       LedgerKeyMeter* lkMeter);

    EvictionResult scanForEviction(uint32_t ledgerSeq,
                                   EvictionCounters& counters,
                                   EvictionIterator evictionIter,
                                   std::shared_ptr<EvictionStatistics> stats,
                                   StateArchivalSettings const& sas);

    friend std::shared_ptr<SearchableLiveBucketListSnapshot>
    BucketSnapshotManager::copySearchableLiveBucketListSnapshot() const;
};

class SearchableHotArchiveBucketListSnapshot
    : public SearchableBucketListSnapshotBase<HotArchiveBucket>
{
    SearchableHotArchiveBucketListSnapshot(
        BucketSnapshotManager const& snapshotManager);

  public:
    std::vector<HotArchiveBucketEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys);

    friend std::shared_ptr<SearchableHotArchiveBucketListSnapshot>
    BucketSnapshotManager::copySearchableHotArchiveBucketListSnapshot() const;
};
}