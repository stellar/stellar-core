// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndexImpl.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "crypto/Hex.h"
#include "crypto/ShortHash.h"
#include "ledger/LedgerTypeUtils.h"
#include "main/Config.h"
#include "util/BinaryFuseFilter.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/XDRStream.h"

#include <Tracy.hpp>
#include <cereal/archives/binary.hpp>
#include <cereal/types/memory.hpp>
#include <cereal/types/utility.hpp>
#include <cereal/types/vector.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>

#include <memory>
#include <thread>
#include <type_traits>
#include <xdrpp/marshal.h>

namespace stellar
{

// Returns a poolshare trustline key with the given accountID and a PoolID
// filled with the fill byte
static LedgerKey
getDummyPoolShareTrustlineKey(AccountID const& accountID, uint8_t fill)
{
    LedgerKey key(TRUSTLINE);
    key.trustLine().accountID = accountID;
    key.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
    key.trustLine().asset.liquidityPoolID().fill(fill);
    return key;
}

// Returns pagesize for given index based on config parameters and bucket size,
// in bytes
static inline std::streamoff
effectivePageSize(Config const& cfg, size_t bucketSize)
{
    // Convert cfg param from MB to bytes
    if (auto cutoff = cfg.BUCKETLIST_DB_INDEX_CUTOFF * 1000000;
        bucketSize < cutoff)
    {
        return 0;
    }

    auto pageSizeExp = cfg.BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;
    releaseAssertOrThrow(pageSizeExp < 32);
    return pageSizeExp == 0 ? 0 : 1UL << pageSizeExp;
}

bool
BucketIndex::typeNotSupported(LedgerEntryType t)
{
    return t == OFFER;
}

template <class IndexT>
template <class BucketEntryT>
BucketIndexImpl<IndexT>::BucketIndexImpl(BucketManager& bm,
                                         std::filesystem::path const& filename,
                                         std::streamoff pageSize,
                                         Hash const& hash,
                                         BucketEntryT const& typeTag)
    : mBloomMissMeter(bm.getBloomMissMeter())
    , mBloomLookupMeter(bm.getBloomLookupMeter())
{
    static_assert(std::is_same_v<BucketEntryT, BucketEntry> ||
                  std::is_same_v<BucketEntryT, HotArchiveBucketEntry>);

    ZoneScoped;
    releaseAssert(!filename.empty());

    {
        auto timer = LogSlowExecution("Indexing bucket");
        mData.pageSize = pageSize;

        // We don't have a good way of estimating IndividualIndex size since
        // keys are variable size, so only reserve range indexes since we know
        // the page size ahead of time
        if constexpr (std::is_same<IndexT, RangeIndex>::value)
        {
            auto fileSize = std::filesystem::file_size(filename);
            auto estimatedIndexEntries = fileSize / mData.pageSize;
            mData.keysToOffset.reserve(estimatedIndexEntries);
        }

        XDRInputFileStream in;
        in.open(filename.string());
        std::streamoff pos = 0;
        std::streamoff pageUpperBound = 0;
        BucketEntryT be;
        size_t iter = 0;
        size_t count = 0;

        std::vector<uint64_t> keyHashes;
        auto seed = shortHash::getShortHashInitKey();

        auto countEntry = [&](BucketEntry const& be) {
            if (be.type() == METAENTRY)
            {
                // Do not count meta entries.
                return;
            }
            auto ledt = bucketEntryToLedgerEntryAndDurabilityType(be);
            mData.counters.entryTypeCounts[ledt]++;
            mData.counters.entryTypeSizes[ledt] += xdr::xdr_size(be);
        };

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

            auto isMeta = [](auto const& be) {
                if constexpr (std::is_same_v<BucketEntryT, LiveBucket::EntryT>)
                {
                    return be.type() == METAENTRY;
                }
                else
                {
                    return be.type() == HOT_ARCHIVE_METAENTRY;
                }
            };

            if (!isMeta(be))
            {
                ++count;
                LedgerKey key = getBucketLedgerKey(be);

                if constexpr (std::is_same_v<BucketEntryT, BucketEntry>)
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
                        mData.assetToPoolID[poolParams.assetA].emplace_back(
                            key.liquidityPool().liquidityPoolID);
                        mData.assetToPoolID[poolParams.assetB].emplace_back(
                            key.liquidityPool().liquidityPoolID);
                    }
                }

                if constexpr (std::is_same<IndexT, RangeIndex>::value)
                {
                    auto keyBuf = xdr::xdr_to_opaque(key);
                    SipHash24 hasher(seed.data());
                    hasher.update(keyBuf.data(), keyBuf.size());
                    keyHashes.emplace_back(hasher.digest());

                    if (pos >= pageUpperBound)
                    {
                        pageUpperBound =
                            roundDown(pos, mData.pageSize) + mData.pageSize;
                        mData.keysToOffset.emplace_back(RangeEntry(key, key),
                                                        pos);
                    }
                    else
                    {
                        auto& rangeEntry = mData.keysToOffset.back().first;
                        releaseAssert(rangeEntry.upperBound < key);
                        rangeEntry.upperBound = key;
                    }
                }
                else
                {
                    mData.keysToOffset.emplace_back(key, pos);
                }

                if constexpr (std::is_same_v<BucketEntryT, LiveBucket::EntryT>)
                {
                    countEntry(be);
                }
            }

            pos = in.pos();
        }

        if constexpr (std::is_same<IndexT, RangeIndex>::value)
        {
            // Binary Fuse filter requires at least 2 elements
            if (keyHashes.size() > 1)
            {
                mData.filter =
                    std::make_unique<BinaryFuseFilter16>(keyHashes, seed);
            }
        }

        CLOG_DEBUG(Bucket, "Indexed {} positions in {}",
                   mData.keysToOffset.size(), filename.filename());
        ZoneValue(static_cast<int64_t>(count));
    }

    if (bm.getConfig().isPersistingBucketListDBIndexes())
    {
        saveToDisk(bm, hash);
    }
}

// Individual indexes are associated with small buckets, so it's more efficient
// to just always recreate them instead of serializing to disk
template <>
void
BucketIndexImpl<BucketIndex::IndividualIndex>::saveToDisk(
    BucketManager& bm, Hash const& hash) const
{
}

template <>
void
BucketIndexImpl<BucketIndex::RangeIndex>::saveToDisk(BucketManager& bm,
                                                     Hash const& hash) const
{
    ZoneScoped;
    releaseAssert(bm.getConfig().isPersistingBucketListDBIndexes());
    auto timer =
        LogSlowExecution("Saving index", LogSlowExecution::Mode::AUTOMATIC_RAII,
                         "took", std::chrono::milliseconds(100));

    std::filesystem::path tmpFilename =
        Bucket::randomBucketIndexName(bm.getTmpDir());
    CLOG_DEBUG(Bucket, "Saving bucket index for {}: {}", hexAbbrev(hash),
               tmpFilename);

    {
        std::ofstream out;
        out.exceptions(std::ios::failbit | std::ios::badbit);
        out.open(tmpFilename, std::ios_base::binary | std::ios_base::trunc);
        cereal::BinaryOutputArchive ar(out);
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

template <class IndexT>
template <class Archive>
BucketIndexImpl<IndexT>::BucketIndexImpl(BucketManager const& bm, Archive& ar,
                                         std::streamoff pageSize)
    : mBloomMissMeter(bm.getBloomMissMeter())
    , mBloomLookupMeter(bm.getBloomLookupMeter())
{
    mData.pageSize = pageSize;
    ar(mData);
}

// Returns true if the key is not contained within the given IndexEntry.
// Range index: check if key is outside range of indexEntry
// Individual index: check if key does not match indexEntry key
template <class IndexEntryT>
static bool
keyNotInIndexEntry(LedgerKey const& key, IndexEntryT const& indexEntry)
{
    if constexpr (std::is_same<IndexEntryT, BucketIndex::RangeEntry>::value)
    {
        return key < indexEntry.lowerBound || indexEntry.upperBound < key;
    }
    else
    {
        return !(key == indexEntry);
    }
}

// std::lower_bound predicate. Returns true if index comes "before" key and does
// not contain it
// If key is too small for indexEntry bounds: return false
// If key is contained within indexEntry bounds: return false
// If key is too large for indexEntry bounds: return true
template <class IndexEntryT>
static bool
lower_bound_pred(IndexEntryT const& indexEntry, LedgerKey const& key)
{
    if constexpr (std::is_same<IndexEntryT,
                               BucketIndex::RangeIndex::value_type>::value)
    {
        return indexEntry.first.upperBound < key;
    }
    else
    {
        return indexEntry.first < key;
    }
}

// std::upper_bound predicate. Returns true if key comes "before" and is not
// contained within the indexEntry.
// If key is too small for indexEntry bounds: return true
// If key is contained within indexEntry bounds: return false
// If key is too large for indexEntry bounds: return false
template <class IndexEntryT>
static bool
upper_bound_pred(LedgerKey const& key, IndexEntryT const& indexEntry)
{
    if constexpr (std::is_same<IndexEntryT,
                               BucketIndex::RangeIndex::value_type>::value)
    {
        return key < indexEntry.first.lowerBound;
    }
    else
    {
        return key < indexEntry.first;
    }
}

template <class BucketT>
std::unique_ptr<BucketIndex const>
BucketIndex::createIndex(BucketManager& bm,
                         std::filesystem::path const& filename,
                         Hash const& hash)
{
    BUCKET_TYPE_ASSERT(BucketT);

    ZoneScoped;
    auto const& cfg = bm.getConfig();
    releaseAssertOrThrow(cfg.isUsingBucketListDB());
    releaseAssertOrThrow(!filename.empty());
    auto pageSize = effectivePageSize(cfg, fs::size(filename.string()));

    try
    {
        if (pageSize == 0)
        {
            CLOG_DEBUG(Bucket,
                       "BucketIndex::createIndex() indexing individual keys in "
                       "bucket {}",
                       filename);
            return std::unique_ptr<BucketIndexImpl<IndividualIndex> const>(
                new BucketIndexImpl<IndividualIndex>(
                    bm, filename, 0, hash, typename BucketT::EntryT{}));
        }
        else
        {
            CLOG_DEBUG(Bucket,
                       "BucketIndex::createIndex() indexing key range with "
                       "page size "
                       "{} in bucket {}",
                       pageSize, filename);
            return std::unique_ptr<BucketIndexImpl<RangeIndex> const>(
                new BucketIndexImpl<RangeIndex>(bm, filename, pageSize, hash,
                                                typename BucketT::EntryT{}));
        }
    }
    // BucketIndexImpl throws if BucketManager shuts down before index finishes,
    // so return empty index instead of partial index
    catch (std::runtime_error&)
    {
        return {};
    }
}

std::unique_ptr<BucketIndex const>
BucketIndex::load(BucketManager const& bm,
                  std::filesystem::path const& filename, size_t bucketFileSize)
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
    ar(version, pageSize);

    // Make sure on-disk index was built with correct version and config
    // parameters before deserializing whole file
    if (version != BUCKET_INDEX_VERSION ||
        pageSize != effectivePageSize(bm.getConfig(), bucketFileSize))
    {
        return {};
    }

    if (pageSize == 0)
    {
        return std::unique_ptr<BucketIndexImpl<IndividualIndex> const>(
            new BucketIndexImpl<IndividualIndex>(bm, ar, pageSize));
    }
    else
    {
        return std::unique_ptr<BucketIndexImpl<RangeIndex> const>(
            new BucketIndexImpl<RangeIndex>(bm, ar, pageSize));
    }
}

template <class IndexT>
std::optional<std::streamoff>
BucketIndexImpl<IndexT>::lookup(LedgerKey const& k) const
{
    ZoneScoped;
    return scan(begin(), k).first;
}

template <class IndexT>
std::pair<std::optional<std::streamoff>, BucketIndex::Iterator>
BucketIndexImpl<IndexT>::scan(Iterator start, LedgerKey const& k) const
{
    ZoneScoped;
    ZoneValue(static_cast<int64_t>(mData.keysToOffset.size()));

    // Search for the key in the index before checking the bloom filter so we
    // return the correct iterator to the caller. This may be slightly less
    // effecient then checking the bloom filter first, but the filter's primary
    // purpose is to avoid disk lookups, not to avoid in-memory index search.
    auto internalStart = std::get<typename IndexT::const_iterator>(start);
    auto keyIter =
        std::lower_bound(internalStart, mData.keysToOffset.end(), k,
                         lower_bound_pred<typename IndexT::value_type>);

    // If the key is not in the bloom filter or in the lower bounded index
    // entry, return nullopt
    markBloomLookup();
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

template <class IndexT>
std::optional<std::pair<std::streamoff, std::streamoff>>
BucketIndexImpl<IndexT>::getOffsetBounds(LedgerKey const& lowerBound,
                                         LedgerKey const& upperBound) const
{
    // Get the index iterators for the bounds
    auto startIter = std::lower_bound(
        mData.keysToOffset.begin(), mData.keysToOffset.end(), lowerBound,
        lower_bound_pred<typename IndexT::value_type>);
    if (startIter == mData.keysToOffset.end())
    {
        return std::nullopt;
    }

    auto endIter = std::upper_bound(
        std::next(startIter), mData.keysToOffset.end(), upperBound,
        upper_bound_pred<typename IndexT::value_type>);

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

template <class IndexT>
std::vector<PoolID> const&
BucketIndexImpl<IndexT>::getPoolIDsByAsset(Asset const& asset) const
{
    static const std::vector<PoolID> emptyVec = {};

    auto iter = mData.assetToPoolID.find(asset);
    if (iter == mData.assetToPoolID.end())
    {
        return emptyVec;
    }

    return iter->second;
}

template <class IndexT>
std::optional<std::pair<std::streamoff, std::streamoff>>
BucketIndexImpl<IndexT>::getPoolshareTrustlineRange(
    AccountID const& accountID) const
{
    // Get the smallest and largest possible trustline keys for the given
    // accountID
    auto upperBound = getDummyPoolShareTrustlineKey(
        accountID, std::numeric_limits<uint8_t>::max());
    auto lowerBound = getDummyPoolShareTrustlineKey(
        accountID, std::numeric_limits<uint8_t>::min());

    return getOffsetBounds(lowerBound, upperBound);
}

template <class IndexT>
std::optional<std::pair<std::streamoff, std::streamoff>>
BucketIndexImpl<IndexT>::getOfferRange() const
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

    return getOffsetBounds(lowerBound, upperBound);
}

#ifdef BUILD_TESTS
template <class IndexT>
bool
BucketIndexImpl<IndexT>::operator==(BucketIndex const& inRaw) const
{
    if (getPageSize() != inRaw.getPageSize())
    {
        return false;
    }

    auto const& in = dynamic_cast<BucketIndexImpl<IndexT> const&>(inRaw);
    if (mData.keysToOffset.size() != in.mData.keysToOffset.size())
    {
        return false;
    }

    if constexpr (std::is_same<IndexT, RangeIndex>::value)
    {
        releaseAssert(mData.filter);
        releaseAssert(in.mData.filter);
        if (!(*(mData.filter) == *(in.mData.filter)))
        {
            return false;
        }
    }
    else
    {
        releaseAssert(!mData.filter);
        releaseAssert(!in.mData.filter);
    }

    for (size_t i = 0; i < mData.keysToOffset.size(); ++i)
    {
        auto const& lhsPair = mData.keysToOffset[i];
        auto const& rhsPair = in.mData.keysToOffset[i];
        if (!(lhsPair == rhsPair))
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

template <class IndexT>
void
BucketIndexImpl<IndexT>::markBloomMiss() const
{
}

template <>
void
BucketIndexImpl<BucketIndex::RangeIndex>::markBloomMiss() const
{
    mBloomMissMeter.Mark();
}

template <class IndexT>
void
BucketIndexImpl<IndexT>::markBloomLookup() const
{
}

template <>
void
BucketIndexImpl<BucketIndex::RangeIndex>::markBloomLookup() const
{
    mBloomLookupMeter.Mark();
}

template <class IndexT>
BucketEntryCounters const&
BucketIndexImpl<IndexT>::getBucketEntryCounters() const
{
    return mData.counters;
}

template std::unique_ptr<BucketIndex const>
BucketIndex::createIndex<LiveBucket>(BucketManager& bm,
                                     std::filesystem::path const& filename,
                                     Hash const& hash);
template std::unique_ptr<BucketIndex const>
BucketIndex::createIndex<HotArchiveBucket>(
    BucketManager& bm, std::filesystem::path const& filename, Hash const& hash);
}
