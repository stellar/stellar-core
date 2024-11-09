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
    if (auto cutoff = cfg.BUCKETLIST_DB_INDEX_CUTOFF * 1'000'000;
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
{
    // Only disk indexes are serialized
    releaseAssertOrThrow(pageSize != 0);
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
    if (std::shared_lock<std::shared_mutex> lock(mCacheMutex); mCache)
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

    std::unique_lock<std::shared_mutex> lock(mCacheMutex);
    if (totalBucketListAccountsSizeBytes < maxBucketListBytesToCache)
    {
        // We can cache the entire bucket
        mCache = std::make_unique<CacheT>(accountsInThisBucket);
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

        auto accountsToCache = static_cast<size_t>(
            bytesAvailableForBucketCache / averageAccountSize);

        mCache = std::make_unique<CacheT>(accountsToCache);
    }
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

std::shared_ptr<BucketEntry const>
LiveBucketIndex::getCachedEntry(LedgerKey const& k) const
{
    if (shouldUseCache() && isCachedType(k))
    {
        std::shared_lock<std::shared_mutex> lock(mCacheMutex);
        auto cachePtr = mCache->maybeGet(k);
        if (cachePtr)
        {
            mCacheHitMeter.Mark();
            return *cachePtr;
        }

        // In the case of a bloom filter false positive, we might have a cache
        // "miss" because we're searching for something that doesn't exist. We
        // don't cache non-existent entries, so we don't meter misses here.
        // Instead, we track misses when we insert a new entry, since we always
        // insert a new entry into the cache after a miss.
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
    static const std::vector<PoolID> emptyVec = {};

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
LiveBucketIndex::getOfferRange() const
{
    if (mDiskIndex)
    {
        // Get the smallest and largest possible offer keys
        LedgerKey upperBound(OFFER);
        upperBound.offer().sellerID.ed25519().fill(
            std::numeric_limits<uint8_t>::max());
        upperBound.offer().offerID = std::numeric_limits<int64_t>::max();

        LedgerKey lowerBound(OFFER);
        lowerBound.offer().sellerID.ed25519().fill(
            std::numeric_limits<uint8_t>::min());
        lowerBound.offer().offerID = std::numeric_limits<int64_t>::min();

        return mDiskIndex->getOffsetBounds(lowerBound, upperBound);
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return mInMemoryIndex->getOfferRange();
}

std::optional<std::pair<std::streamoff, std::streamoff>>
LiveBucketIndex::getContractCodeRange() const
{
    if (mDiskIndex)
    {
        // Get the smallest and largest possible contract code keys
        LedgerKey lowerBound(CONTRACT_CODE);
        lowerBound.contractCode().hash.fill(
            std::numeric_limits<uint8_t>::min());

        LedgerKey upperBound(CONTRACT_CODE);
        upperBound.contractCode().hash.fill(
            std::numeric_limits<uint8_t>::max());

        return mDiskIndex->getOffsetBounds(lowerBound, upperBound);
    }

    releaseAssertOrThrow(mInMemoryIndex);
    return mInMemoryIndex->getContractCodeRange();
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
    if (mDiskIndex)
    {
        std::shared_lock<std::shared_mutex> lock(mCacheMutex);
        return mCache != nullptr;
    }

    return false;
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
        mCacheMissMeter.Mark();

        std::unique_lock<std::shared_mutex> lock(mCacheMutex);
        mCache->put(k, entry);
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
        std::shared_lock<std::shared_mutex> lock(mCacheMutex);
        return mCache->maxSize();
    }

    return 0;
}
#endif

template LiveBucketIndex::LiveBucketIndex(BucketManager const& bm,
                                          cereal::BinaryInputArchive& ar,
                                          std::streamoff pageSize);
}