#pragma once

// copyright 2024 stellar development foundation and contributors. licensed
// under the apache license, version 2.0. see the copying file at the root
// of this distribution or at http://www.apache.org/licenses/license-2.0

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
template <typename T> class BucketOutputIterator;
template <typename T> class BucketInputIterator;

typedef BucketOutputIterator<LiveBucket> LiveBucketOutputIterator;
typedef BucketInputIterator<LiveBucket> LiveBucketInputIterator;

/*
 * Live Buckets are used by the LiveBucketList to store the current canonical
 * state of the ledger. They contain entries of type BucketEntry.
 */
class LiveBucket : public BucketBase<LiveBucket, LiveBucketIndex>,
                   public std::enable_shared_from_this<LiveBucket>
{
  public:
    // Entry type that this bucket stores
    using EntryT = BucketEntry;

    // Entry type returned by loadKeys
    using LoadT = LedgerEntry;

    using IndexT = LiveBucketIndex;

    static inline constexpr char const* METRIC_STRING = "bucketlistDB-live";

    LiveBucket();
    virtual ~LiveBucket()
    {
    }
    LiveBucket(std::string const& filename, Hash const& hash,
               std::unique_ptr<LiveBucketIndex const>&& index);

    // Returns true if a BucketEntry that is key-wise identical to the given
    // BucketEntry exists in the bucket. For testing.
    bool containsBucketIdentity(BucketEntry const& id) const;

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

    static void mergeCasesWithEqualKeys(
        MergeCounters& mc, LiveBucketInputIterator& oi,
        LiveBucketInputIterator& ni, LiveBucketOutputIterator& out,
        std::vector<LiveBucketInputIterator>& shadowIterators,
        uint32_t protocolVersion, bool keepShadowedLifecycleEntries);

#ifdef BUILD_TESTS
    // "Applies" the bucket to the database. For each entry in the bucket,
    // if the entry is init or live, creates or updates the corresponding
    // entry in the database (respectively; if the entry is dead (a
    // tombstone), deletes the corresponding entry in the database.
    void apply(Application& app) const;
#endif

    // Returns [lowerBound, upperBound) of file offsets for all offers in the
    // bucket, or std::nullopt if no offers exist
    std::optional<std::pair<std::streamoff, std::streamoff>>
    getOfferRange() const;

    // Create a fresh bucket from given vectors of init (created) and live
    // (updated) LedgerEntries, and dead LedgerEntryKeys. The bucket will
    // be sorted, hashed, and adopted in the provided BucketManager.
    static std::shared_ptr<LiveBucket>
    fresh(BucketManager& bucketManager, uint32_t protocolVersion,
          std::vector<LedgerEntry> const& initEntries,
          std::vector<LedgerEntry> const& liveEntries,
          std::vector<LedgerKey> const& deadEntries, bool countMergeEvents,
          asio::io_context& ctx, bool doFsync);

    // Returns true if the given BucketEntry should be dropped in the bottom
    // level bucket (i.e. DEADENTRY)
    static bool isTombstoneEntry(BucketEntry const& e);

    static std::shared_ptr<LoadT const>
    bucketEntryToLoadResult(std::shared_ptr<EntryT const> const& be);

    // Whenever a given BucketEntry is "eligible" to be written as the merge
    // result in the output bucket, this function writes the entry to the output
    // iterator if the entry is not shadowed.
    static void maybePut(LiveBucketOutputIterator& out,
                         BucketEntry const& entry,
                         std::vector<LiveBucketInputIterator>& shadowIterators,
                         bool keepShadowedLifecycleEntries, MergeCounters& mc);

    static void countOldEntryType(MergeCounters& mc, BucketEntry const& e);
    static void countNewEntryType(MergeCounters& mc, BucketEntry const& e);

    uint32_t getBucketVersion() const;

    BucketEntryCounters const& getBucketEntryCounters() const;

    friend class LiveBucketSnapshot;
};
}