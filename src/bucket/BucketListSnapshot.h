#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "util/SimpleTimer.h"
#include "util/UnorderedMap.h"
#include "util/UnorderedSet.h"
#include "util/XDRStream.h"
#include "xdr/Stellar-ledger-entries.h"
#include "xdr/Stellar-ledger.h"

#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace medida
{
class Counter;
class Meter;
class Timer;
}

namespace stellar
{

class BucketSnapshotManager;
struct EvictionMetrics;
struct EvictionResultCandidates;
struct EvictionResultEntry;
struct InflationWinner;
struct StateArchivalSettings;
class EvictionStatistics;
template <class BucketT> class BucketListBase;
template <class BucketT> class BucketLevel;

// BucketListSnapshotData holds the immutable bucket references that can be
// safely shared across threads. It contains only bucket pointers and no
// metadata (headers, sequence numbers, etc.) or mutable state (file streams).
template <class BucketT> struct BucketListSnapshotData
{
    BUCKET_TYPE_ASSERT(BucketT);

    struct Level
    {
        std::shared_ptr<BucketT const> const curr;
        std::shared_ptr<BucketT const> const snap;

        Level(BucketLevel<BucketT> const& level);
        Level(std::shared_ptr<BucketT const> currBucket,
              std::shared_ptr<BucketT const> snapBucket);
    };

    std::vector<Level> const levels;

    explicit BucketListSnapshotData(BucketListBase<BucketT> const& bl);
};

// SearchableBucketListSnapshot provides BucketList lookup functionality.
// Each snapshot maintains its own stream cache for file I/O and a pointer to
// immutable snapshot data (ledger header, list of referenced buckets, etc).
//
// Thread-safety:
// - A single snapshot instance must only be used by one thread at a time.
// - The underlying snapshot data (shared via shared_ptr) is immutable and
//   can be safely shared.
template <class BucketT> class SearchableBucketListSnapshot
{
    BUCKET_TYPE_ASSERT(BucketT);

  protected:
    // Shared immutable snapshot data
    std::shared_ptr<BucketListSnapshotData<BucketT> const> mData;
    std::map<uint32_t, std::shared_ptr<BucketListSnapshotData<BucketT> const>>
        mHistoricalSnapshots;

    // Ledger header associated with this snapshot. Stored separately from
    // BucketListSnapshotData so that the snapshot data only contains bucket
    // references.
    LedgerHeader const mHeader;

    // Per-snapshot mutable stream cache
    mutable UnorderedMap<BucketT const*, std::unique_ptr<XDRInputFileStream>>
        mStreams;

    MetricsRegistry& mMetrics;

    // Tracks load times for each LedgerEntryType. We use
    // SimpleTimer since medida Timer overhead is too expensive for point loads.
    UnorderedMap<LedgerEntryType, SimpleTimer&> mPointTimers;

    // Bulk load timers take significantly longer, so the timer overhead is
    // comparatively negligible.
    mutable UnorderedMap<std::string, medida::Timer&> mBulkTimers;
    medida::Meter& mBulkLoadMeter;

    // Returns (lazily-constructed) file stream for bucket file. Note
    // this might be in some random position left over from a previous read --
    // must be seek()'ed before use.
    XDRInputFileStream&
    getStream(std::shared_ptr<BucketT const> const& bucket) const;

    // Loads the bucket entry for LedgerKey k. Starts at file offset pos and
    // reads until key is found or the end of the page. Returns <BucketEntry,
    // bloomMiss>, where bloomMiss is true if a bloomMiss occurred during the
    // load.
    std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
    getEntryAtOffset(std::shared_ptr<BucketT const> const& bucket,
                     LedgerKey const& k, std::streamoff pos,
                     size_t pageSize) const;

    // Loads bucket entry for LedgerKey k. Returns <BucketEntry, bloomMiss>,
    // where bloomMiss is true if a bloomMiss occurred during the load.
    std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
    getBucketEntry(std::shared_ptr<BucketT const> const& bucket,
                   LedgerKey const& k) const;

    // Loads LedgerEntry's for given keys in the given bucket. When a key is
    // found, the entry is added to result and the key is removed from keys.
    void loadKeysFromBucket(std::shared_ptr<BucketT const> const& bucket,
                            std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                            std::vector<typename BucketT::LoadT>& result) const;

    std::optional<std::vector<typename BucketT::LoadT>>
    loadKeysInternal(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                     std::optional<uint32_t> ledgerSeq) const;

    medida::Timer& getBulkLoadTimer(std::string const& label,
                                    size_t numEntries) const;

    // Iterate over all buckets in a snapshot in order, calling f on each
    // non-empty bucket. Exits early if function returns Loop::COMPLETE.
    // The first overload operates on an explicit snapshot (used for historical
    // queries).
    template <typename Func>
    void loopAllBuckets(Func&& f,
                        BucketListSnapshotData<BucketT> const& snapshot) const;
    template <typename Func> void loopAllBuckets(Func&& f) const;

    SearchableBucketListSnapshot(
        MetricsRegistry& metrics,
        std::shared_ptr<BucketListSnapshotData<BucketT> const> data,
        std::map<uint32_t,
                 std::shared_ptr<BucketListSnapshotData<BucketT> const>>
            historicalSnapshots,
        LedgerHeader const& header);

  public:
    virtual ~SearchableBucketListSnapshot() = default;

    // Point load, returns nullptr if not found
    std::shared_ptr<typename BucketT::LoadT const>
    load(LedgerKey const& k) const;

    // Loads inKeys from the specified historical snapshot. Returns
    // load_result_vec if the snapshot for the given ledger is
    // available, std::nullopt otherwise. Note that ledgerSeq is defined
    // as the state of the BucketList at the beginning of the ledger. This means
    // that for ledger N, the maximum lastModifiedLedgerSeq of any LedgerEntry
    // in the BucketList is N - 1.
    std::optional<std::vector<typename BucketT::LoadT>>
    loadKeysFromLedger(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
                       uint32_t ledgerSeq) const;

    uint32_t getLedgerSeq() const;
    LedgerHeader const& getLedgerHeader() const;

    // Access to underlying data (for copying/refreshing)
    std::shared_ptr<BucketListSnapshotData<BucketT> const> const&
    getSnapshotData() const;

    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<BucketT> const>> const&
    getHistoricalSnapshots() const;
};

// Live bucket list snapshot with additional query methods
class SearchableLiveBucketListSnapshot
    : public SearchableBucketListSnapshot<LiveBucket>
{
    SearchableLiveBucketListSnapshot(
        MetricsRegistry& metrics,
        std::shared_ptr<BucketListSnapshotData<LiveBucket> const> data,
        std::map<uint32_t,
                 std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
            historicalSnapshots,
        LedgerHeader const& header);

    Loop scanForEvictionInBucket(
        std::shared_ptr<LiveBucket const> const& bucket, EvictionIterator& iter,
        uint32_t& bytesToScan, uint32_t ledgerSeq,
        std::list<EvictionResultEntry>& evictableEntries, uint32_t ledgerVers,
        UnorderedSet<LedgerKey>& keysInEvictableEntries) const;

  public:
    std::vector<LedgerEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
             std::string const& label) const;

    std::vector<LedgerEntry>
    loadPoolShareTrustLinesByAccountAndAsset(AccountID const& accountID,
                                             Asset const& asset) const;

    std::vector<InflationWinner> loadInflationWinners(size_t maxWinners,
                                                      int64_t minBalance) const;

    std::unique_ptr<EvictionResultCandidates> scanForEviction(
        uint32_t ledgerSeq, EvictionMetrics& metrics, EvictionIterator iter,
        std::shared_ptr<EvictionStatistics> stats,
        StateArchivalSettings const& sas, uint32_t ledgerVers) const;

    // Iterate over all entries of a given type. Note this iterates over all
    // BucketEntry, so some may be shadowed and outdated.
    void scanForEntriesOfType(
        LedgerEntryType type,
        std::function<Loop(BucketEntry const&)> callback) const;

    friend class BucketSnapshotManager;
};

// Hot archive bucket list snapshot
class SearchableHotArchiveBucketListSnapshot
    : public SearchableBucketListSnapshot<HotArchiveBucket>
{
    SearchableHotArchiveBucketListSnapshot(
        MetricsRegistry& metrics,
        std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const> data,
        std::map<uint32_t, std::shared_ptr<
                               BucketListSnapshotData<HotArchiveBucket> const>>
            historicalSnapshots,
        LedgerHeader const& header);

  public:
    std::vector<HotArchiveBucketEntry>
    loadKeys(std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const;

    // Iterate over all entries in all buckets. Note this iterates over all
    // HotArchiveBucketEntry, so some may be shadowed and outdated.
    void scanAllEntries(
        std::function<Loop(HotArchiveBucketEntry const&)> callback) const;

    friend class BucketSnapshotManager;
};

extern template struct BucketListSnapshotData<LiveBucket>;
extern template struct BucketListSnapshotData<HotArchiveBucket>;
extern template class SearchableBucketListSnapshot<LiveBucket>;
extern template class SearchableBucketListSnapshot<HotArchiveBucket>;

} // namespace stellar
