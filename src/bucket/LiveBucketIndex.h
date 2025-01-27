#pragma once

// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketUtils.h"
#include "bucket/DiskIndex.h"
#include "bucket/InMemoryIndex.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "util/NonCopyable.h"
#include "util/XDROperators.h" // IWYU pragma: keep
#include "xdr/Stellar-ledger-entries.h"
#include <filesystem>
#include <optional>

#include <cereal/archives/binary.hpp>

namespace asio
{
class io_context;
}

namespace medida
{
class Meter;
}

namespace stellar
{

/**
 * BucketIndex is an in-memory mapping of LedgerKey's to the file offset
 * of the associated LedgerEntry in a given Bucket file. Because the set of
 * LedgerKeys is too large to keep in memory, BucketIndex can either index
 * individual keys or key ranges.
 *
 * For small buckets, an individual index is used for faster index lookup. For
 * larger buckets, the range index is used. The range index cannot give an exact
 * position for a given LedgerEntry or tell if it exists in the bucket, but can
 * give an offset range for where the entry would be if it exists. Config flags
 * determine the size of the range index, as well as what bucket size should use
 * the individual index vs range index.
 */

class BucketManager;

class LiveBucketIndex : public NonMovableOrCopyable
{
  public:
    using IterT =
        std::variant<InMemoryIndex::IterT, DiskIndex<LiveBucket>::IterT>;

  private:
    std::unique_ptr<DiskIndex<LiveBucket> const> mDiskIndex;
    std::unique_ptr<InMemoryIndex const> mInMemoryIndex;

    static inline DiskIndex<LiveBucket>::IterT
    getDiskIter(IterT const& iter)
    {
        return std::get<DiskIndex<LiveBucket>::IterT>(iter);
    }

    static inline InMemoryIndex::IterT
    getInMemoryIter(IterT const& iter)
    {
        return std::get<InMemoryIndex::IterT>(iter);
    }

  public:
    inline static const std::string DB_BACKEND_STATE = "bl";
    inline static const uint32_t BUCKET_INDEX_VERSION = 5;

    // Constructor for creating new index from Bucketfile
    LiveBucketIndex(BucketManager& bm, std::filesystem::path const& filename,
                    Hash const& hash, asio::io_context& ctx);

    // Constructor for loading pre-existing index from disk
    template <class Archive>
    LiveBucketIndex(BucketManager const& bm, Archive& ar,
                    std::streamoff pageSize);

    // Returns true if LedgerEntryType not supported by BucketListDB
    static bool typeNotSupported(LedgerEntryType t);

    // Returns pagesize for given index based on config parameters and bucket
    // size, in bytes
    static std::streamoff getPageSize(Config const& cfg, size_t bucketSize);

    IndexReturnT lookup(LedgerKey const& k) const;

    std::pair<IndexReturnT, IterT> scan(IterT start, LedgerKey const& k) const;

    std::vector<PoolID> const& getPoolIDsByAsset(Asset const& asset) const;

    std::optional<std::pair<std::streamoff, std::streamoff>>
    getOfferRange() const;

    BucketEntryCounters const& getBucketEntryCounters() const;
    uint32_t getPageSize() const;

    IterT begin() const;
    IterT end() const;
    void markBloomMiss() const;
#ifdef BUILD_TESTS
    bool operator==(LiveBucketIndex const& in) const;
#endif
};
}