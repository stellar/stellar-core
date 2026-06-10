// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucketIndex.h"
#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/DiskIndex.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "xdr/Stellar-ledger-entries.h"
#include <ios>
#include <medida/meter.h>
#include <shared_mutex>
#include <vector>

namespace stellar
{

bool
LiveBucketIndex::typeNotSupported(LedgerEntryType t)
{
    return t == OFFER;
}

std::streamoff
LiveBucketIndex::getPageSize(Config const& cfg, size_t bucketSize)
{
    // Convert cfg param from MB to bytes
    if (auto cutoff = cfg.BUCKETLIST_DB_INDEX_CUTOFF * 1024 * 1024;
        bucketSize < cutoff)
    {
        return 0;
    }

    return getPageSizeFromConfig(cfg);
}

LiveBucketIndex::LiveBucketIndex(BucketManager& bm,
                                 std::filesystem::path const& filename,
                                 Hash const& hash, asio::io_context& ctx,
                                 SHA256* hasher)
    : mCacheHitMeter(bm.getCacheHitMeter())
    , mCacheMissMeter(bm.getCacheMissMeter())
    , mMarkCacheMeters(
          !bm.getConfig().DISABLE_SOROBAN_METRICS_FOR_TESTING)
{
    ZoneScoped;
    releaseAssert(!filename.empty());

    auto pageSize = getPageSize(bm.getConfig(), fs::size(filename.string()));
    if (pageSize == 0)
    {

        CLOG_DEBUG(Bucket,
                   "LiveBucketIndex::createIndex() using in-memory index for "
                   "bucket {}",
                   filename);
        mInMemoryIndex = std::make_unique<InMemoryIndex>(bm, filename, hasher);
    }
    else
    {
        CLOG_DEBUG(Bucket,
                   "LiveBucketIndex::createIndex() indexing key range with "
                   "page size {} in bucket {}",
                   pageSize, filename);
        mDiskIndex = std::make_unique<DiskIndex<LiveBucket>>(
            bm, filename, pageSize, hash, ctx, hasher);
    }
}

template <class Archive>
LiveBucketIndex::LiveBucketIndex(BucketManager const& bm, Archive& ar,
                                 std::streamoff pageSize)

    : mDiskIndex(std::make_unique<DiskIndex<LiveBucket>>(ar, bm, pageSize))
    , mCacheHitMeter(bm.getCacheHitMeter())
    , mCacheMissMeter(bm.getCacheMissMeter())
    , mMarkCacheMeters(
          !bm.getConfig().DISABLE_SOROBAN_METRICS_FOR_TESTING)
{
    // Only disk indexes are serialized
    releaseAssertOrThrow(pageSize != 0);
}

LiveBucketIndex::LiveBucketIndex(BucketManager& bm,
                                 std::vector<BucketEntry> const& inMemoryState,
                                 BucketMetadata const& metadata)
    : mInMemoryIndex(
          std::make_unique<InMemoryIndex>(bm, inMemoryState, metadata))
    , mCacheHitMeter(bm.getCacheHitMeter())
    , mCacheMissMeter(bm.getCacheMissMeter())
    , mMarkCacheMeters(
          !bm.getConfig().DISABLE_SOROBAN_METRICS_FOR_TESTING)
{
}

void
LiveBucketIndex::maybeInitializeCache(size_t totalBucketListAccountsSizeBytes,
                                      Config const& cfg) const
{
    // Everything is already in memory, no need for a redundant cache.
    if (mInMemoryIndex)
    {
        return;
    }

    // Cache is already initialized
    if (mCacheEnabled.load(std::memory_order_acquire))
    {
        return;
    }

    releaseAssert(mDiskIndex);

    auto accountsInThisBucket =
        mDiskIndex->getBucketEntryCounters().entryTypeCounts.at(
            LedgerEntryTypeAndDurability::ACCOUNT);

    // Convert from MB to bytes, max size for entire BucketList cache
    auto maxBucketListBytesToCache =
        cfg.BUCKETLIST_DB_MEMORY_FOR_CACHING * 1024 * 1024;

    // Nothing to cache. or cache is disabled
    if (accountsInThisBucket == 0 || maxBucketListBytesToCache == 0)
    {
        return;
    }

    size_t accountsToCache = 0;
    if (totalBucketListAccountsSizeBytes < maxBucketListBytesToCache)
    {
        // We can cache the entire bucket
        accountsToCache = accountsInThisBucket;
    }
    else
    {
        // The random eviction cache has an entry limit, but we expose a memory
        // limit in the validator config. We can't do an exact 1 to 1 mapping
        // because account entries have different sizes.
        //
        // First we take the fraction of the total BucketList size that this
        // bucket occupies to figure out how much memory to allocate. Then we
        // use the average account size to convert that to an entry count for
        // the cache.

        auto accountBytesInThisBucket =
            mDiskIndex->getBucketEntryCounters().entryTypeSizes.at(
                LedgerEntryTypeAndDurability::ACCOUNT);

        double fractionOfTotalBucketListBytes =
            static_cast<double>(accountBytesInThisBucket) /
            totalBucketListAccountsSizeBytes;

        size_t bytesAvailableForBucketCache = static_cast<size_t>(
            maxBucketListBytesToCache * fractionOfTotalBucketListBytes);

        double averageAccountSize =
            static_cast<double>(accountBytesInThisBucket) /
            accountsInThisBucket;

        accountsToCache = static_cast<size_t>(bytesAvailableForBucketCache /
                                              averageAccountSize);
    }

    MutexLocker initGuard(mCacheInitMutex);
    if (mCacheEnabled.load(std::memory_order_relaxed))
    {
        // Lost an initialization race.
        return;
    }

    // The entry capacity (accountsToCache) is enforced globally by
    // maybeAddToCache, so each shard is given the full capacity (under which
    // it never self-evicts) but only pre-reserves space for its expected
    // share of the entries.
    mCacheMaxEntries = accountsToCache;
    auto const perShardReserve =
        (accountsToCache + kNumCacheShards - 1) / kNumCacheShards;
    mCacheShards = std::vector<CacheShard>(kNumCacheShards);
    for (auto& shard : mCacheShards)
    {
        MutexLocker shardGuard(shard.mMutex);
        shard.mCache =
            std::make_unique<CacheT>(accountsToCache, perShardReserve);
    }
    mCacheEnabled.store(true, std::memory_order_release);
}

LiveBucketIndex::IterT
LiveBucketIndex::begin() const
{
    if (mDiskIndex)
    {
        return mDiskIndex->begin();
    }
    else
    {
        releaseAssertOrThrow(mInMemoryIndex);
        return mInMemoryIndex->begin();
    }
}

LiveBucketIndex::IterT
LiveBucketIndex::end() const
{
    if (mDiskIndex)
    {
        return mDiskIndex->end();
    }
    else
    {
        releaseAssertOrThrow(mInMemoryIndex);
        return mInMemoryIndex->end();
    }
}

void
LiveBucketIndex::markBloomMiss() const
{
    releaseAssertOrThrow(mDiskIndex);
    mDiskIndex->markBloomMiss();
}

LiveBucketIndex::CacheShard&
LiveBucketIndex::getCacheShard(LedgerKey const& k) const
{
    return mCacheShards[std::hash<LedgerKey>()(k) % kNumCacheShards];
}

std::shared_ptr<BucketEntry const>
LiveBucketIndex::getCachedEntry(LedgerKey const& k) const
{
    if (shouldUseCache() && isCachedType(k))
    {
        std::shared_ptr<BucketEntry const> result{};
        auto& shard = getCacheShard(k);
        {
            MutexLocker lock(shard.mMutex);
            auto cachePtr = shard.mCache->maybeGet(k);
            if (cachePtr)
            {
                result = *cachePtr;
            }
        }
        if (result && mMarkCacheMeters)
        {
            // NB: marked outside the shard lock; Meter::Mark takes the
            // (process-wide shared) meter's own mutex.
            mCacheHitMeter.Mark();
        }

        // In the case of a bloom filter false positive, we might have a cache
        // "miss" because we're searching for something that doesn't exist. We
        // don't cache non-existent entries, so we don't meter misses here.
        // Instead, we track misses when we insert a new entry, since we always
        // insert a new entry into the cache after a miss.
        return result;
    }

    return nullptr;
}

IndexReturnT
LiveBucketIndex::lookup(LedgerKey const& k) const
{
    if (mDiskIndex)
    {
        if (auto cached = getCachedEntry(k); cached)
        {
            return IndexReturnT(cached);
        }

        return mDiskIndex->scan(mDiskIndex->begin(), k).first;
    }
    else
    {
        releaseAssertOrThrow(mInMemoryIndex);
        return mInMemoryIndex->scan(mInMemoryIndex->begin(), k).first;
    }
}

std::pair<IndexReturnT, LiveBucketIndex::IterT>
LiveBucketIndex::scan(IterT start, LedgerKey const& k) const
{
    if (mDiskIndex)
    {
        if (auto cached = getCachedEntry(k); cached)
        {
            return {IndexReturnT(cached), start};
        }

        return mDiskIndex->scan(getDiskIter(start), k);
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return mInMemoryIndex->scan(getInMemoryIter(start), k);
}

std::vector<PoolID> const&
LiveBucketIndex::getPoolIDsByAsset(Asset const& asset) const
{
    static std::vector<PoolID> const emptyVec = {};

    if (mDiskIndex)
    {
        auto iter = mDiskIndex->getAssetPoolIDMap().find(asset);
        if (iter == mDiskIndex->getAssetPoolIDMap().end())
        {
            return emptyVec;
        }
        return iter->second;
    }
    else
    {
        releaseAssertOrThrow(mInMemoryIndex);
        auto iter = mInMemoryIndex->getAssetPoolIDMap().find(asset);
        if (iter == mInMemoryIndex->getAssetPoolIDMap().end())
        {
            return emptyVec;
        }
        return iter->second;
    }
}

std::optional<std::pair<std::streamoff, std::streamoff>>
LiveBucketIndex::getRangeForType(LedgerEntryType type) const
{
    if (mDiskIndex)
    {
        return mDiskIndex->getRangeForType(type);
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return mInMemoryIndex->getRangeForType(type);
}

uint32_t
LiveBucketIndex::getPageSize() const
{
    if (mDiskIndex)
    {
        return mDiskIndex->getPageSize();
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return 0;
}

BucketEntryCounters const&
LiveBucketIndex::getBucketEntryCounters() const
{
    if (mDiskIndex)
    {
        return mDiskIndex->getBucketEntryCounters();
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return mInMemoryIndex->getBucketEntryCounters();
}

bool
LiveBucketIndex::shouldUseCache() const
{
    return mDiskIndex && mCacheEnabled.load(std::memory_order_acquire);
}

bool
LiveBucketIndex::isCachedType(LedgerKey const& lk)
{
    return lk.type() == ACCOUNT;
}

void
LiveBucketIndex::maybeAddToCache(
    std::shared_ptr<BucketEntry const> const& entry) const
{
    if (shouldUseCache())
    {
        releaseAssertOrThrow(entry);
        auto k = getBucketLedgerKey(*entry);
        if (!isCachedType(k))
        {
            return;
        }

        // If we are adding an entry to the cache, we must have missed it
        // earlier.
        if (mMarkCacheMeters)
        {
            mCacheMissMeter.Mark();
        }

        auto& shard = getCacheShard(k);
        MutexLocker lock(shard.mMutex);
        if (shard.mCache->put(k, entry))
        {
            // Capacity is enforced globally (see the header): on insertion,
            // evict from this shard if the total entry count exceeds the
            // budget. Concurrent inserters may transiently overshoot by at
            // most one entry each.
            auto count =
                mCacheEntryCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (count > mCacheMaxEntries)
            {
                shard.mCache->evictOne();
                mCacheEntryCount.fetch_sub(1, std::memory_order_relaxed);
            }
        }
    }
}

#ifdef BUILD_TESTS
bool
LiveBucketIndex::operator==(LiveBucketIndex const& in) const
{
    if (mDiskIndex)
    {
        if (!in.mDiskIndex)
        {
            return false;
        }

        if (!(*mDiskIndex == *in.mDiskIndex))
        {
            return false;
        }
    }
    else
    {
        if (in.mDiskIndex)
        {
            return false;
        }

        return *mInMemoryIndex == *in.mInMemoryIndex;
    }

    return true;
}

size_t
LiveBucketIndex::getMaxCacheSize() const
{
    if (shouldUseCache())
    {
        return mCacheMaxEntries;
    }

    return 0;
}
#endif

size_t
LiveBucketIndex::getCurrentCacheSize() const
{
    if (shouldUseCache())
    {
        return mCacheEntryCount.load(std::memory_order_relaxed);
    }

    return 0;
}

template LiveBucketIndex::LiveBucketIndex(BucketManager const& bm,
                                          cereal::BinaryInputArchive& ar,
                                          std::streamoff pageSize);
}
