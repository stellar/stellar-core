#pragma once

// copyright 2024 stellar development foundation and contributors. licensed
// under the apache license, version 2.0. see the copying file at the root
// of this distribution or at http://www.apache.org/licenses/license-2.0

#include "bucket/BucketBase.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucketIndex.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

class HotArchiveBucket;
template <typename T> class BucketOutputIterator;
template <typename T> class BucketInputIterator;

typedef BucketInputIterator<HotArchiveBucket> HotArchiveBucketInputIterator;
typedef BucketOutputIterator<HotArchiveBucket> HotArchiveBucketOutputIterator;

/*
 * Hot Archive Buckets are used by the HotBucketList to store recently evicted
 * entries. They contain entries of type HotArchiveBucketEntry.
 */
class HotArchiveBucket
    : public BucketBase<HotArchiveBucket, HotArchiveBucketIndex>,
      public std::enable_shared_from_this<HotArchiveBucket>
{
    static std::vector<HotArchiveBucketEntry>
    convertToBucketEntry(std::vector<LedgerEntry> const& archivedEntries,
                         std::vector<LedgerKey> const& restoredEntries,
                         std::vector<LedgerKey> const& deletedEntries);

  public:
    // Entry type that this bucket stores
    using EntryT = HotArchiveBucketEntry;

    // Entry type returned by loadKeys
    using LoadT = HotArchiveBucketEntry;

    using IndexT = HotArchiveBucketIndex;

    static inline constexpr char const* METRIC_STRING =
        "bucketlistDB-hotArchive";

    HotArchiveBucket();
    virtual ~HotArchiveBucket()
    {
    }
    HotArchiveBucket(std::string const& filename, Hash const& hash,
                     std::unique_ptr<HotArchiveBucketIndex const>&& index);
    uint32_t getBucketVersion() const;

    static std::shared_ptr<HotArchiveBucket>
    fresh(BucketManager& bucketManager, uint32_t protocolVersion,
          std::vector<LedgerEntry> const& archivedEntries,
          std::vector<LedgerKey> const& restoredEntries,
          std::vector<LedgerKey> const& deletedEntries, bool countMergeEvents,
          asio::io_context& ctx, bool doFsync);

    // Returns true if the given BucketEntry should be dropped in the bottom
    // level bucket (i.e. HOT_ARCHIVE_LIVE)
    static bool isTombstoneEntry(HotArchiveBucketEntry const& e);

    // Note: this functions is called maybePut for interoperability with
    // LiveBucket. This function always writes te given entry to the output
    // iterator.
    static void
    maybePut(HotArchiveBucketOutputIterator& out,
             HotArchiveBucketEntry const& entry,
             std::vector<HotArchiveBucketInputIterator>& shadowIterators,
             bool keepShadowedLifecycleEntries, MergeCounters& mc);

    // For now, we only count LiveBucket merge events
    static void
    countOldEntryType(MergeCounters& mc, HotArchiveBucketEntry const& e)
    {
    }

    static void
    countNewEntryType(MergeCounters& mc, HotArchiveBucketEntry const& e)
    {
    }

    static void
    checkProtocolLegality(HotArchiveBucketEntry const& entry,
                          uint32_t protocolVersion)
    {
    }

    static void mergeCasesWithEqualKeys(
        MergeCounters& mc, HotArchiveBucketInputIterator& oi,
        HotArchiveBucketInputIterator& ni, HotArchiveBucketOutputIterator& out,
        std::vector<HotArchiveBucketInputIterator>& shadowIterators,
        uint32_t protocolVersion, bool keepShadowedLifecycleEntries);

    static std::shared_ptr<LoadT const>
    bucketEntryToLoadResult(std::shared_ptr<EntryT const> const& be);

    friend class HotArchiveBucketSnapshot;
};
}