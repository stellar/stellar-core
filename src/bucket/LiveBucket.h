// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketBase.h"
#include "bucket/BucketUtils.h"
#include "bucket/LiveBucketIndex.h"

namespace medida
{
class Counter;
}

namespace stellar
{
class AbstractLedgerTxn;
class Application;
class EvictionStatistics;
class LiveBucket;
template <IsBucketType T> class BucketOutputIterator;
template <IsBucketType T> class BucketInputIterator;

typedef BucketOutputIterator<LiveBucket> LiveBucketOutputIterator;
typedef BucketInputIterator<LiveBucket> LiveBucketInputIterator;

/*
 * Live Buckets are used by the LiveBucketList to store the current canonical
 * state of the ledger. They contain entries of type BucketEntry.
 */
class LiveBucket : public BucketBase<LiveBucket, LiveBucketIndex>,
                   public std::enable_shared_from_this<LiveBucket>
{
    // Stores all BucketEntries (except METAENTRY) in the same order that they
    // appear in the bucket file for level 0 entries. Because level 0 merges
    // block the main thread when we write to the BucketList, we use the
    // in-memory entries to produce the new bucket instead of file IO.
    std::unique_ptr<std::vector<BucketEntry>> mEntries{};

  public:
    // Entry type that this bucket stores
    using EntryT = BucketEntry;

    // Entry type returned by loadKeys
    using LoadT = LedgerEntry;

    using IndexT = LiveBucketIndex;

    static inline constexpr char const* METRIC_STRING = "bucketlistDB-live";

    LiveBucket();

    // Creates an in-memory only bucket with the given entries (no file or
    // index). This should only be used as a temporary bucket for level 0
    // merges.
    LiveBucket(std::unique_ptr<std::vector<BucketEntry>> entries);

    virtual ~LiveBucket()
    {
    }
    LiveBucket(
        std::string const& filename, Hash const& hash,
        std::shared_ptr<LiveBucketIndex const>&& index,
        std::unique_ptr<std::vector<BucketEntry>> inMemoryState = nullptr);

    // Returns true if a BucketEntry that is key-wise identical to the given
    // BucketEntry exists in the bucket. For testing.
    bool containsBucketIdentity(BucketEntry const& id) const;

    // Returns the current cache size for this bucket's index
    size_t getIndexCacheSize() const;

    // At version 11, we added support for INITENTRY and METAENTRY. Before this
    // we were only supporting LIVEENTRY and DEADENTRY.
    static constexpr ProtocolVersion
        FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY =
            ProtocolVersion::V_11;
    static constexpr ProtocolVersion FIRST_PROTOCOL_SHADOWS_REMOVED =
        ProtocolVersion::V_12;

    static void checkProtocolLegality(BucketEntry const& entry,
                                      uint32_t protocolVersion);

    static std::vector<BucketEntry>
    convertToBucketEntry(bool useInit,
                         std::vector<LedgerEntry> const& initEntries,
                         std::vector<LedgerEntry> const& liveEntries,
                         std::vector<LedgerKey> const& deadEntries);

    template <typename InputSource>
    static void mergeCasesWithEqualKeys(
        MergeCounters& mc, InputSource& inputSource,
        std::function<void(BucketEntry const&)> putFunc,
        uint32_t protocolVersion,
        std::vector<LiveBucketInputIterator>& shadowIterators,
        bool keepShadowedLifecycleEntries);

#ifdef BUILD_TESTS
    // "Applies" the bucket to the database. For each entry in the bucket,
    // if the entry is init or live, creates or updates the corresponding
    // entry in the database (respectively; if the entry is dead (a
    // tombstone), deletes the corresponding entry in the database.
    void apply(Application& app) const;
    size_t getMaxCacheSize() const;
#endif

    // Returns [lowerBound, upperBound) of file offsets for all entries of the
    // given type in the bucket, or std::nullopt if no entries of this type
    // exist. Note that if the underlying index is a page based index, this is a
    // rough bound such that entries of another type may also be present in the
    // range.
    std::optional<std::pair<std::streamoff, std::streamoff>>
    getRangeForType(LedgerEntryType type) const;

    // Create a fresh bucket from given vectors of init (created) and live
    // (updated) LedgerEntries, and dead LedgerEntryKeys. The bucket will
    // be sorted, hashed, and adopted in the provided BucketManager.
    static std::shared_ptr<LiveBucket>
    fresh(BucketManager& bucketManager, uint32_t protocolVersion,
          std::vector<LedgerEntry> const& initEntries,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries, bool countMergeEvents,
          asio::io_context& ctx, bool doFsync);

    // Create a fresh bucket that exists only in memory, without writing to
    // disk, calculating a hash, or indexing. This should only be used for
    // "level -1" snap buckets that are immediately merged into level 0.
    static std::shared_ptr<LiveBucket>
    freshInMemoryOnly(BucketManager& bucketManager, uint32_t protocolVersion,
                      std::vector<LedgerEntry> const& initEntries,
                      std::vector<LedgerEntry> const& liveEntries,
                      std::vector<LedgerKey> const& deadEntries,
                      bool countMergeEvents);

    // Returns true if the given BucketEntry should be dropped in the bottom
    // level bucket (i.e. DEADENTRY)
    static bool isTombstoneEntry(BucketEntry const& e);

    static std::shared_ptr<LoadT const>
    bucketEntryToLoadResult(std::shared_ptr<EntryT const> const& be);

    // Whenever a given BucketEntry is "eligible" to be written as the merge
    // result in the output bucket, this function writes the entry to the output
    // iterator if the entry is not shadowed.
    // putFunc will be called to actually write the result of maybePut.
    static void maybePut(std::function<void(BucketEntry const&)> putFunc,
                         BucketEntry const& entry, MergeCounters& mc,
                         std::vector<LiveBucketInputIterator>& shadowIterators,
                         bool keepShadowedLifecycleEntries);

    // Merge two buckets in memory without using FutureBucket.
    // This is used only for level 0 merges. Note that the resulting Bucket is
    // still written to disk.
    static std::shared_ptr<LiveBucket>
    mergeInMemory(BucketManager& bucketManager, uint32_t maxProtocolVersion,
                  std::shared_ptr<LiveBucket> const& oldBucket,
                  std::shared_ptr<LiveBucket> const& newBucket,
                  bool countMergeEvents, asio::io_context& ctx, bool doFsync);

    static void countOldEntryType(MergeCounters& mc, BucketEntry const& e);
    static void countNewEntryType(MergeCounters& mc, BucketEntry const& e);

    // Returns whether shadowed lifecycle entries should be kept
    static bool updateMergeCountersForProtocolVersion(
        MergeCounters& mc, uint32_t protocolVersion,
        std::vector<LiveBucketInputIterator> const& shadowIterators);

    uint32_t getBucketVersion() const;

    bool
    hasInMemoryEntries() const
    {
        return static_cast<bool>(mEntries);
    }

    std::vector<BucketEntry> const&
    getInMemoryEntries() const
    {
        releaseAssertOrThrow(mEntries);
        return *mEntries;
    }

    // Initializes the random eviction cache if it has not already been
    // initialized. totalBucketListAccountsSizeBytes is the total size, in
    // bytes, of all BucketEntries in the BucketList that hold ACCOUNT entries,
    // including INIT, LIVE and DEAD entries.
    void maybeInitializeCache(size_t totalBucketListAccountsSizeBytes,
                              Config const& cfg) const;

    BucketEntryCounters const& getBucketEntryCounters() const;

    friend class SearchableLiveBucketListSnapshot;
};
}
