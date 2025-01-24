// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/DiskIndex.h"
#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "crypto/Hex.h"
#include "crypto/ShortHash.h"
#include "util/BufferedAsioCerealOutputArchive.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include <medida/meter.h>

#include <cereal/archives/binary.hpp>

namespace stellar
{

namespace
{
// Returns pagesize for given index based on config parameters and bucket size,
// in bytes
std::streamoff
effectivePageSize(Config const& cfg, size_t bucketSize)
{
    // TODO: Individual index
    return 1UL << cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;
    // Convert cfg param from MB to bytes
    if (auto cutoff = cfg.BUCKETLIST_DB_INDEX_CUTOFF * 1'000'000;
        bucketSize < cutoff)
    {
        return 0;
    }

    auto pageSizeExp = cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;
    releaseAssertOrThrow(pageSizeExp < 32);
    return pageSizeExp == 0 ? 0 : 1UL << pageSizeExp;
}

// Returns true if the key is not contained within the given IndexEntry.
// Range index: check if key is outside range of indexEntry
// Individual index: check if key does not match indexEntry key
bool
keyNotInIndexEntry(LedgerKey const& key, RangeEntry const& indexEntry)
{
    return key < indexEntry.lowerBound || indexEntry.upperBound < key;
}

// std::lower_bound predicate. Returns true if index comes "before" key and does
// not contain it
// If key is too small for indexEntry bounds: return false
// If key is contained within indexEntry bounds: return false
// If key is too large for indexEntry bounds: return true
bool
lower_bound_pred(RangeIndex::value_type const& indexEntry, LedgerKey const& key)
{
    return indexEntry.first.upperBound < key;
}

// std::upper_bound predicate. Returns true if key comes "before" and is not
// contained within the indexEntry.
// If key is too small for indexEntry bounds: return true
// If key is contained within indexEntry bounds: return false
// If key is too large for indexEntry bounds: return false
bool
upper_bound_pred(LedgerKey const& key, RangeIndex::value_type const& indexEntry)
{
    return key < indexEntry.first.lowerBound;
}
}

template <class BucketT>
std::pair<std::optional<std::streamoff>, RangeIndex::const_iterator>
DiskIndex<BucketT>::scan(RangeIndex::const_iterator start,
                         LedgerKey const& k) const
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(mData.keysToOffset.size()));

    // Search for the key in the index before checking the bloom filter so we
    // return the correct iterator to the caller. This may be slightly less
    // effecient then checking the bloom filter first, but the filter's primary
    // purpose is to avoid disk lookups, not to avoid in-memory index search.
    auto keyIter =
        std::lower_bound(start, mData.keysToOffset.end(), k, lower_bound_pred);

    // If the key is not in the bloom filter or in the lower bounded index
    // entry, return nullopt
    mBloomLookupMeter.Mark();
    if ((mData.filter && !mData.filter->contains(k)) ||
        keyIter == mData.keysToOffset.end() ||
        keyNotInIndexEntry(k, keyIter->first))
    {
        return {std::nullopt, keyIter};
    }
    else
    {
        return {keyIter->second, keyIter};
    }
}

template <class BucketT>
std::optional<std::pair<std::streamoff, std::streamoff>>
DiskIndex<BucketT>::getOffsetBounds(LedgerKey const& lowerBound,
                                    LedgerKey const& upperBound) const
{
    // Get the index iterators for the bounds
    auto startIter =
        std::lower_bound(mData.keysToOffset.begin(), mData.keysToOffset.end(),
                         lowerBound, lower_bound_pred);
    if (startIter == mData.keysToOffset.end())
    {
        return std::nullopt;
    }

    auto endIter =
        std::upper_bound(std::next(startIter), mData.keysToOffset.end(),
                         upperBound, upper_bound_pred);

    // Get file offsets based on lower and upper bound iterators
    std::streamoff startOff = startIter->second;
    std::streamoff endOff = std::numeric_limits<std::streamoff>::max();

    // If we hit the end of the index then upper bound should be EOF
    if (endIter != mData.keysToOffset.end())
    {
        endOff = endIter->second;
    }

    return std::make_pair(startOff, endOff);
}

template <class BucketT>
DiskIndex<BucketT>::DiskIndex(BucketManager& bm,
                              std::filesystem::path const& filename,
                              std::streamoff pageSize, Hash const& hash,
                              asio::io_context& ctx)
    : mBloomLookupMeter(bm.getBloomLookupMeter<BucketT>())
    , mBloomMissMeter(bm.getBloomMissMeter<BucketT>())
{
    ZoneScoped;
    mData.pageSize = pageSize;

    // Only LiveBucket needs an asset to poolID mapping
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        mData.assetToPoolID = std::make_unique<AssetPoolIDMap>();
    }

    auto fileSize = fs::size(filename);
    auto estimatedIndexEntries = fileSize / pageSize;
    mData.keysToOffset.reserve(estimatedIndexEntries);

    XDRInputFileStream in;
    in.open(filename.string());
    std::streamoff pos = 0;
    std::streamoff pageUpperBound = 0;
    typename BucketT::EntryT be;
    size_t iter = 0;
    [[maybe_unused]] size_t count = 0;

    std::vector<uint64_t> keyHashes;
    auto seed = shortHash::getShortHashInitKey();

    while (in && in.readOne(be))
    {
        // peridocially check if bucket manager is exiting to stop indexing
        // gracefully
        if (++iter >= 1000)
        {
            iter = 0;
            if (bm.isShutdown())
            {
                throw std::runtime_error("Incomplete bucket index due to "
                                         "BucketManager shutdown");
            }
        }

        if (!isBucketMetaEntry<BucketT>(be))
        {
            ++count;
            LedgerKey key = getBucketLedgerKey(be);

            if constexpr (std::is_same_v<BucketT, LiveBucket>)
            {
                // We need an asset to poolID mapping for
                // loadPoolshareTrustlineByAccountAndAsset queries. For this
                // query, we only need to index INIT entries because:
                // 1. PoolID is the hash of the Assets it refers to, so this
                //    index cannot be invalidated by newer LIVEENTRY updates
                // 2. We do a join over all bucket indexes so we avoid
                // storing
                //    multiple redundant index entries (i.e. LIVEENTRY
                //    updates)
                // 3. We only use this index to collect the possible set of
                //    Trustline keys, then we load those keys. This means
                //    that we don't need to keep track of DEADENTRY. Even if
                //    a given INITENTRY has been deleted by a newer
                //    DEADENTRY, the trustline load will not return deleted
                //    trustlines, so the load result is still correct even
                //    if the index has a few deleted mappings.
                if (be.type() == INITENTRY && key.type() == LIQUIDITY_POOL)
                {
                    auto const& poolParams = be.liveEntry()
                                                 .data.liquidityPool()
                                                 .body.constantProduct()
                                                 .params;
                    (*mData.assetToPoolID)[poolParams.assetA].emplace_back(
                        key.liquidityPool().liquidityPoolID);
                    (*mData.assetToPoolID)[poolParams.assetB].emplace_back(
                        key.liquidityPool().liquidityPoolID);
                }
            }
            else
            {
                static_assert(std::is_same_v<BucketT, HotArchiveBucket>);
            }

            auto keyBuf = xdr::xdr_to_opaque(key);
            SipHash24 hasher(seed.data());
            hasher.update(keyBuf.data(), keyBuf.size());
            keyHashes.emplace_back(hasher.digest());

            if (pos >= pageUpperBound)
            {
                pageUpperBound =
                    roundDown(pos, mData.pageSize) + mData.pageSize;
                mData.keysToOffset.emplace_back(RangeEntry(key, key), pos);
            }
            else
            {
                auto& rangeEntry = mData.keysToOffset.back().first;
                releaseAssert(rangeEntry.upperBound < key);
                rangeEntry.upperBound = key;
            }

            mData.counters.template count<BucketT>(be);
        }

        pos = in.pos();
    }

    // Binary Fuse filter requires at least 2 elements
    if (keyHashes.size() > 1)
    {
        // There is currently an access error that occurs very rarely
        // for some random seed values. If this occurs, simply rotate
        // the seed and try again.
        for (int i = 0; i < 10; ++i)
        {
            try
            {
                mData.filter =
                    std::make_unique<BinaryFuseFilter16>(keyHashes, seed);
            }
            catch (std::out_of_range& e)
            {
                auto seedToStr = [](auto seed) {
                    std::string result;
                    for (auto b : seed)
                    {
                        fmt::format_to(std::back_inserter(result), "{:02x}", b);
                    }
                    return result;
                };

                CLOG_ERROR(Bucket,
                           "Bad memory access in BinaryFuseFilter with "
                           "seed {}, retrying",
                           seedToStr(seed));
                seed[0]++;
            }
        }
    }

    CLOG_DEBUG(Bucket, "Indexed {} positions in {}", mData.keysToOffset.size(),
               filename.filename());
    ZoneValue(static_cast<int64_t>(count));

    if (bm.getConfig().BUCKETLIST_DB_PERSIST_INDEX)
    {
        saveToDisk(bm, hash, ctx);
    }
}

template <class BucketT>
template <class Archive>
DiskIndex<BucketT>::DiskIndex(Archive& ar, BucketManager const& bm,
                              std::streamoff pageSize)
    : mBloomLookupMeter(bm.getBloomLookupMeter<BucketT>())
    , mBloomMissMeter(bm.getBloomMissMeter<BucketT>())
{
    releaseAssertOrThrow(pageSize != 0);
    mData.pageSize = pageSize;
    ar(mData);
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        releaseAssertOrThrow(mData.assetToPoolID);
    }
    else
    {
        static_assert(std::is_same_v<BucketT, HotArchiveBucket>);
        releaseAssertOrThrow(!mData.assetToPoolID);
    }
}

template <class BucketT>
void
DiskIndex<BucketT>::saveToDisk(BucketManager& bm, Hash const& hash,
                               asio::io_context& ctx) const
{
    ZoneScoped;
    releaseAssert(bm.getConfig().BUCKETLIST_DB_PERSIST_INDEX);
    if constexpr (std::is_same_v<BucketT, LiveBucket>)
    {
        releaseAssertOrThrow(mData.assetToPoolID);
    }
    else
    {
        static_assert(std::is_same_v<BucketT, HotArchiveBucket>);
        releaseAssertOrThrow(!mData.assetToPoolID);
    }

    auto timer =
        LogSlowExecution("Saving index", LogSlowExecution::Mode::AUTOMATIC_RAII,
                         "took", std::chrono::milliseconds(100));

    std::filesystem::path tmpFilename =
        BucketT::randomBucketIndexName(bm.getTmpDir());
    CLOG_DEBUG(Bucket, "Saving bucket index for {}: {}", hexAbbrev(hash),
               tmpFilename);

    {
        OutputFileStream out(ctx, !bm.getConfig().DISABLE_XDR_FSYNC);
        out.open(tmpFilename.string());
        cereal::BufferedAsioOutputArchive ar(out);
        ar(mData);
    }

    std::filesystem::path canonicalName = bm.bucketIndexFilename(hash);
    CLOG_DEBUG(Bucket, "Adopting bucket index file {} as {}", tmpFilename,
               canonicalName);
    if (!bm.renameBucketDirFile(tmpFilename, canonicalName))
    {
        std::string err("Failed to rename bucket index :");
        err += strerror(errno);
        // it seems there is a race condition with external systems
        // retry after sleeping for a second works around the problem
        std::this_thread::sleep_for(std::chrono::seconds(1));
        if (!bm.renameBucketDirFile(tmpFilename, canonicalName))
        {
            // if rename fails again, surface the original error
            throw std::runtime_error(err);
        }
    }
}

template <class BucketT>
void
DiskIndex<BucketT>::markBloomMiss() const
{
    mBloomMissMeter.Mark();
}

#ifdef BUILD_TESTS
template <class BucketT>
bool
DiskIndex<BucketT>::operator==(DiskIndex<BucketT> const& in) const
{
    if (getPageSize() != in.getPageSize())
    {
        return false;
    }

    if (mData.keysToOffset != in.mData.keysToOffset)
    {
        return false;
    }

    // If both indexes have a filter, check if they are equal
    if (mData.filter && in.mData.filter)
    {
        if (!(*(mData.filter) == *(in.mData.filter)))
        {
            return false;
        }
    }
    else
    {
        // If both indexes don't fave a filter, check that each filter is
        // null
        if (mData.filter || in.mData.filter)
        {
            return false;
        }
    }

    if (mData.assetToPoolID && in.mData.assetToPoolID)
    {
        if (!(*(mData.assetToPoolID) == *(in.mData.assetToPoolID)))
        {
            return false;
        }
    }
    else
    {
        if (mData.assetToPoolID || in.mData.assetToPoolID)
        {
            return false;
        }
    }

    if (mData.counters != in.mData.counters)
    {
        return false;
    }

    return true;
}
#endif

template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
createIndex(BucketManager& bm, std::filesystem::path const& filename,
            Hash const& hash, asio::io_context& ctx)
{
    BUCKET_TYPE_ASSERT(BucketT);

    ZoneScoped;
    auto const& cfg = bm.getConfig();
    releaseAssertOrThrow(!filename.empty());
    // TODO: Individual index
    auto pageSize = effectivePageSize(cfg, fs::size(filename.string()));
    if (pageSize == 0)
    {
        CLOG_DEBUG(Bucket,
                   "BucketIndex::createIndex() indexing individual keys in "
                   "bucket {}",
                   filename);
    }
    else
    {
        CLOG_DEBUG(Bucket,
                   "BucketIndex::createIndex() indexing key range with "
                   "page size {} in bucket {}",
                   pageSize, filename);
    }

    try
    {
        return std::unique_ptr<typename BucketT::IndexT const>(
            new typename BucketT::IndexT(bm, filename, pageSize, hash, ctx));
    }
    // BucketIndex throws if BucketManager shuts down before index finishes,
    // so return empty index instead of partial index
    catch (std::runtime_error&)
    {
        return {};
    }
}

template <class BucketT>
std::unique_ptr<typename BucketT::IndexT const>
loadIndex(BucketManager const& bm, std::filesystem::path const& filename,
          size_t bucketFileSize)
{
    std::ifstream in(filename, std::ios::binary);
    if (!in)
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("Error opening file {}"), filename));
    }

    std::streamoff pageSize;
    uint32_t version;
    cereal::BinaryInputArchive ar(in);
    DiskIndex<BucketT>::preLoad(ar, version, pageSize);

    // Make sure on-disk index was built with correct version and config
    // parameters before deserializing whole file
    if (version != BucketT::IndexT::BUCKET_INDEX_VERSION ||
        pageSize != effectivePageSize(bm.getConfig(), bucketFileSize) ||
        pageSize == 0)
    {
        return {};
    }

    return std::unique_ptr<typename BucketT::IndexT const>(
        new typename BucketT::IndexT(bm, ar, pageSize));
}

template std::unique_ptr<typename HotArchiveBucket::IndexT const>
createIndex<HotArchiveBucket>(BucketManager& bm,
                              std::filesystem::path const& filename,
                              Hash const& hash, asio::io_context& ctx);
template std::unique_ptr<typename LiveBucket::IndexT const>
createIndex<LiveBucket>(BucketManager& bm,
                        std::filesystem::path const& filename, Hash const& hash,
                        asio::io_context& ctx);
template std::unique_ptr<typename HotArchiveBucket::IndexT const>
loadIndex<HotArchiveBucket>(BucketManager const& bm,
                            std::filesystem::path const& filename,
                            size_t bucketFileSize);
template std::unique_ptr<typename LiveBucket::IndexT const>
loadIndex<LiveBucket>(BucketManager const& bm,
                      std::filesystem::path const& filename,
                      size_t bucketFileSize);

template class DiskIndex<HotArchiveBucket>;
template class DiskIndex<LiveBucket>;

template DiskIndex<HotArchiveBucket>::DiskIndex(cereal::BinaryInputArchive& ar,
                                                BucketManager const& bm,
                                                std::streamoff pageSize);
template DiskIndex<LiveBucket>::DiskIndex(cereal::BinaryInputArchive& ar,
                                          BucketManager const& bm,
                                          std::streamoff pageSize);
}