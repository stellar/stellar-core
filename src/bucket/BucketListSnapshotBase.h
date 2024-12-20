#pragma once

// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListBase.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketSnapshot.h"
#include "bucket/BucketSnapshotManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "history/HistoryArchive.h"
#include "ledger/NetworkConfig.h"

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

// Complete state of last closed ledger: ledger header, soroban network config,
// bucketlist
struct LastClosedLedger
{
    std::optional<SorobanNetworkConfig> sorobanNetworkConfig;
    LedgerHeaderHistoryEntry lhhe;
    HistoryArchiveState has;
    LastClosedLedger() = default;
    explicit LastClosedLedger(
        LedgerHeaderHistoryEntry const& lhhe, HistoryArchiveState const& has,
        std::optional<SorobanNetworkConfig> const& sorobanNetworkConfig)
        : sorobanNetworkConfig(sorobanNetworkConfig), lhhe(lhhe), has(has)
    {
    }
    // Add getters
    uint32_t
    getProtocolVersion() const
    {
        return lhhe.header.ledgerVersion;
    }
    uint32_t
    getLastTxFee() const
    {
        return lhhe.header.baseFee;
    }
    uint32_t
    getLastReserve() const
    {
        return lhhe.header.baseReserve;
    }
    uint32_t
    getMaxTxSetSize() const
    {
        return lhhe.header.maxTxSetSize;
    }
    uint32_t
    getLastMaxTxSetSizeOps() const
    {
        auto n = getMaxTxSetSize();
        return protocolVersionStartsFrom(getProtocolVersion(),
                                         ProtocolVersion::V_11)
                   ? n
                   : (n * MAX_OPS_PER_TX);
    }
    uint32_t
    getLedgerSeq() const
    {
        return lhhe.header.ledgerSeq;
    }
    LedgerHeaderHistoryEntry const&
    getLedgerHeaderHistoryEntry() const
    {
        return lhhe;
    }
    std::optional<SorobanNetworkConfig> const&
    getSorobanNetworkConfig() const
    {
        return sorobanNetworkConfig;
    }
};

template <class BucketT> class BucketListSnapshot : public NonMovable
{

    BUCKET_TYPE_ASSERT(BucketT);
    using BucketSnapshotT =
        std::conditional_t<std::is_same_v<BucketT, LiveBucket>,
                           LiveBucketSnapshot, HotArchiveBucketSnapshot>;

  private:
    std::vector<BucketLevelSnapshot<BucketT>> mLevels;

    // Last closed ledger associated with this snapshot
    LastClosedLedger const mLastClosedLedger;

  public:
    BucketListSnapshot(BucketListBase<BucketT> const& bl,
                       LastClosedLedger lastClosed);

    // Only allow copies via constructor
    BucketListSnapshot(BucketListSnapshot const& snapshot);
    BucketListSnapshot& operator=(BucketListSnapshot const&) = delete;

    std::vector<BucketLevelSnapshot<BucketT>> const& getLevels() const;
    uint32_t getLedgerSeq() const;
    LedgerHeader const&
    getLedgerHeader() const
    {
        return mLastClosedLedger.lhhe.header;
    }
    LastClosedLedger const&
    getLastClosedLedger() const
    {
        return mLastClosedLedger;
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
    bool const mAutoUpdate;

    // Loops through all buckets, starting with curr at level 0, then snap at
    // level 0, etc. Calls f on each bucket. Exits early if function
    // returns Loop::COMPLETE.
    void loopAllBuckets(std::function<Loop(BucketSnapshotT const&)> f,
                        BucketListSnapshot<BucketT> const& snapshot) const;

    // If `autoUpdate` is true, the snapshot will keep itself consistent with
    // LCL automatically. If not, callers are expected to refresh the snapshot
    // manually (this is useful to use cases that a consistent view of the state
    // for some arbitrary time)
    SearchableBucketListSnapshotBase(
        BucketSnapshotManager const& snapshotManager, bool autoUpdate);

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
    LastClosedLedger const& getLastClosedLedger();

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
}