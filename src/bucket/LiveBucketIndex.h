// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "bucket/BucketUtils.h"
#include "bucket/DiskIndex.h"
#include "bucket/InMemoryIndex.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "ledger/LedgerHashUtils.h" // IWYU pragma: keep
#include "util/NonCopyable.h"
#include "util/RandomEvictionCache.h"
#include "util/ThreadAnnotations.h"
#include "util/XDROperators.h" // IWYU pragma: keep
#include "xdr/Stellar-ledger-entries.h"
#include <atomic>
#include <filesystem>
#include <optional>
#include <vector>

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
class SHA256;
class LiveBucketIndex : public NonMovableOrCopyable
{
  public:
    using IterT =
        std::variant<InMemoryIndex::IterT, DiskIndex<LiveBucket>::IterT>;

    using CacheT =
        RandomEvictionCache<LedgerKey, std::shared_ptr<BucketEntry const>>;

  private:
    std::unique_ptr<DiskIndex<LiveBucket> const> mDiskIndex{};
    std::unique_ptr<InMemoryIndex const> mInMemoryIndex{};

    // The indexes themselves are thread safe, as they are immutable after
    // construction. The cache is sharded by key hash, with a mutex per
    // shard: a single cache-wide mutex serializes all the concurrently
    // running apply threads (each entry load takes it in exclusive mode),
    // while uniformly-distributed keys across many shards make lock
    // collisions rare. The cached values themselves are immutable (a
    // bucket's contents never change), so sharding does not change any
    // observable behavior except eviction being per-shard.
    struct CacheShard
    {
        mutable ANNOTATED_MUTEX(mMutex);
        std::unique_ptr<CacheT> mCache GUARDED_BY(mMutex);
    };
    static constexpr size_t kNumCacheShards = 64;

    // The shard vector is created by maybeInitializeCache and published via
    // the release-store to mCacheEnabled; it is never modified afterwards
    // (only the per-shard caches are, under their shard's mutex), so readers
    // that observe mCacheEnabled == true may index it freely.
    mutable std::vector<CacheShard> mCacheShards;
    mutable std::atomic<bool> mCacheEnabled{false};
    // Serializes cache initialization only.
    mutable ANNOTATED_MUTEX(mCacheInitMutex);

    // The entry capacity is enforced globally rather than per shard (a
    // per-shard capacity would cause spurious evictions on hash imbalance
    // even when the whole bucket is meant to fit in the cache): inserts
    // count entries via mCacheEntryCount, and the inserting thread evicts
    // from its own shard once the total exceeds mCacheMaxEntries. Since
    // inserts are uniformly distributed across shards, so are the evictions.
    // mCacheMaxEntries is written once before the cache is published via
    // mCacheEnabled and read-only afterwards.
    mutable size_t mCacheMaxEntries{0};
    mutable std::atomic<size_t> mCacheEntryCount{0};

    medida::Meter& mCacheHitMeter;
    medida::Meter& mCacheMissMeter;
    // Marking the (process-wide shared) cache meters takes a per-meter mutex
    // on every cached-entry load, which serializes the parallel apply
    // threads; benchmarking configs disable this via
    // DISABLE_SOROBAN_METRICS_FOR_TESTING.
    bool const mMarkCacheMeters;

    CacheShard& getCacheShard(LedgerKey const& k) const;

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

    bool shouldUseCache() const;

    // Because we already have an LTX level cache at apply time, we only need to
    // cache entry types that are used during TX validation and flooding.
    // Currently, that is only ACCOUNT.
    static bool isCachedType(LedgerKey const& lk);

    // Returns nullptr if cache is not enabled or entry not found
    std::shared_ptr<BucketEntry const> getCachedEntry(LedgerKey const& k) const;

  public:
    inline static std::string const DB_BACKEND_STATE = "bl";
    inline static uint32_t const BUCKET_INDEX_VERSION = 6;

    // Constructor for creating new index from Bucketfile
    // Note: Constructor does not initialize the cache
    LiveBucketIndex(BucketManager& bm, std::filesystem::path const& filename,
                    Hash const& hash, asio::io_context& ctx, SHA256* hasher);

    // Constructor for loading pre-existing index from disk
    // Note: Constructor does not initialize the cache
    template <class Archive>
    LiveBucketIndex(BucketManager const& bm, Archive& ar,
                    std::streamoff pageSize);

    // Constructor for creating new index from in-memory state
    LiveBucketIndex(BucketManager& bm,
                    std::vector<BucketEntry> const& inMemoryState,
                    BucketMetadata const& metadata);

    // Initializes the random eviction cache if it has not already been
    // initialized. The random eviction cache itself has an entry limit, but we
    // expose a memory limit in the validator config. To account for this, we
    // check what percentage of the total number of accounts are in this bucket
    // and allocate space accordingly.
    //
    // bucketListTotalAccountSizeBytes is the total size, in bytes, of all
    // BucketEntries in the BucketList that hold ACCOUNT entries, including
    // INIT, LIVE and DEAD entries.
    void maybeInitializeCache(size_t bucketListTotalAccountSizeBytes,
                              Config const& cfg) const;

    // Returns true if LedgerEntryType not supported by BucketListDB
    static bool typeNotSupported(LedgerEntryType t);

    // Returns pagesize for given index based on config parameters and bucket
    // size, in bytes
    static std::streamoff getPageSize(Config const& cfg, size_t bucketSize);

    IndexReturnT lookup(LedgerKey const& k) const;

    std::pair<IndexReturnT, IterT> scan(IterT start, LedgerKey const& k) const;

    std::vector<PoolID> const& getPoolIDsByAsset(Asset const& asset) const;

    void maybeAddToCache(std::shared_ptr<BucketEntry const> const& entry) const;

    std::optional<std::pair<std::streamoff, std::streamoff>>
    getRangeForType(LedgerEntryType type) const;

    BucketEntryCounters const& getBucketEntryCounters() const;
    uint32_t getPageSize() const;

    IterT begin() const;
    IterT end() const;
    void markBloomMiss() const;
#ifdef BUILD_TESTS
    bool operator==(LiveBucketIndex const& in) const;
    size_t getMaxCacheSize() const;
#endif
    size_t getCurrentCacheSize() const;
};
}
