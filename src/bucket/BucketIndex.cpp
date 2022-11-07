// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndex.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "ledger/LedgerHashUtils.h"
#include "main/Config.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/XDRStream.h"

#include <Tracy.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>

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

// Index maps a range of BucketEntry's to the associated offset
// within the bucket file. Index stored as vector of pairs:
// First: LedgerKey/Key ranges sorted in the same scheme as LedgerEntryCmp
// Second: offset into the bucket file for a given key/ key range.
// pageSize determines how large, in bytes, each range should be. pageSize == 0
// indicates individual keys used instead of ranges.
template <class IndexT> class BucketIndexImpl : public BucketIndex
{
    IndexT mKeysToOffset{};
    std::streamoff const mPageSize{};
    std::unique_ptr<bloom_filter> mFilter{};

    BucketIndexImpl(BucketManager const& bm,
                    std::filesystem::path const& filename,
                    std::streamoff pageSize);

    friend std::unique_ptr<BucketIndex const>
    BucketIndex::createIndex(BucketManager const& bm,
                             std::filesystem::path const& filename);

  public:
    BucketIndexImpl() = default;

    virtual std::optional<std::streamoff>
    lookup(LedgerKey const& k) const override;

    virtual std::pair<std::optional<std::streamoff>, Iterator>
    scan(Iterator start, LedgerKey const& k) const override;

    virtual std::pair<std::streamoff, std::streamoff>
    getPoolshareTrustlineRange(AccountID const& accountID) const override;

    virtual std::streamoff
    getPageSize() const override
    {
        return mPageSize;
    }

    virtual Iterator
    begin() const override
    {
        return mKeysToOffset.begin();
    }

    virtual Iterator
    end() const override
    {
        return mKeysToOffset.end();
    }
};

template <class IndexT>
BucketIndexImpl<IndexT>::BucketIndexImpl(BucketManager const& bm,
                                         std::filesystem::path const& filename,
                                         std::streamoff pageSize)
    : mPageSize(pageSize)
{
    ZoneScoped;
    releaseAssert(!filename.empty());

    auto timer = LogSlowExecution("Indexing bucket");

    size_t const estimatedLedgerEntrySize =
        xdr::xdr_traits<BucketEntry>::serial_size(BucketEntry{});
    auto fileSize = fs::size(filename.string());
    auto estimatedNumElems = fileSize / estimatedLedgerEntrySize;
    size_t estimatedIndexEntries;

    // Initialize bloom filter for range index
    if constexpr (std::is_same<IndexT, RangeIndex>::value)
    {
        ZoneNamedN(bloomInit, "bloomInit", true);
        bloom_parameters params;
        params.projected_element_count = estimatedNumElems;
        params.false_positive_probability = 0.001; // 1 in 1000
        crypto_shorthash_keygen(params.random_seed.data());
        params.compute_optimal_parameters();
        mFilter = std::make_unique<bloom_filter>(params);
        estimatedIndexEntries = fileSize / mPageSize;
    }
    else
    {
        estimatedIndexEntries = estimatedNumElems;
    }

    mKeysToOffset.reserve(estimatedIndexEntries);

    XDRInputFileStream in;
    in.open(filename.string());
    std::streamoff pos = 0;
    std::streamoff pageUpperBound = 0;
    BucketEntry be;
    size_t iter = 0;
    while (in && in.readOne(be))
    {
        // peridocially check if bucket manager is exiting to stop indexing
        // gracefully
        if (++iter >= 1000)
        {
            iter = 0;
            if (bm.isShutdown())
            {
                return;
            }
        }

        if (be.type() != METAENTRY)
        {
            LedgerKey key = getBucketLedgerKey(be);
            if constexpr (std::is_same<IndexT, RangeIndex>::value)
            {
                if (pos >= pageUpperBound)
                {
                    pageUpperBound = roundDown(pos, mPageSize) + mPageSize;
                    mKeysToOffset.emplace_back(RangeEntry(key, key), pos);
                }
                else
                {
                    auto& rangeEntry = mKeysToOffset.back().first;
                    releaseAssert(rangeEntry.upperBound < key);
                    rangeEntry.upperBound = key;
                }

                mFilter->insert(std::hash<stellar::LedgerKey>()(key));
            }
            else
            {
                mKeysToOffset.emplace_back(key, pos);
            }
        }

        pos = in.pos();
    }

    CLOG_DEBUG(Bucket, "Indexed {} positions in {}", mKeysToOffset.size(),
               filename.filename());
    ZoneValue(static_cast<int64_t>(mKeysToOffset.size()));
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

std::unique_ptr<BucketIndex const>
BucketIndex::createIndex(BucketManager const& bm,
                         std::filesystem::path const& filename)
{
    ZoneScoped;
    auto const& cfg = bm.getConfig();
    releaseAssertOrThrow(cfg.EXPERIMENTAL_BUCKETLIST_DB);
    releaseAssertOrThrow(!filename.empty());

    auto pageSizeExp = cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_PAGE_SIZE_EXPONENT;
    releaseAssertOrThrow(pageSizeExp < 32);
    auto pageSize = pageSizeExp == 0 ? 0 : 1UL << pageSizeExp;

    // Conver to bytes
    auto cutoff = cfg.EXPERIMENTAL_BUCKETLIST_DB_INDEX_CUTOFF * 1000000;
    if (pageSize == 0 || fs::size(filename.string()) < cutoff)
    {
        CLOG_INFO(Bucket,
                  "BucketIndex::createIndex() indexing individual keys in "
                  "bucket {}",
                  filename);
        return std::unique_ptr<BucketIndexImpl<IndividualIndex> const>(
            new BucketIndexImpl<IndividualIndex>(bm, filename, 0));
    }
    else
    {
        CLOG_INFO(Bucket,
                  "BucketIndex::createIndex() indexing key range with "
                  "page size "
                  "{} in bucket {}",
                  pageSize, filename);
        return std::unique_ptr<BucketIndexImpl<RangeIndex> const>(
            new BucketIndexImpl<RangeIndex>(bm, filename, pageSize));
    }

    return {};
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
    ZoneValue(static_cast<int64_t>(mKeysToOffset.size()));

    auto internalStart = std::get<typename IndexT::const_iterator>(start);
    auto keyIter =
        std::lower_bound(internalStart, mKeysToOffset.end(), k,
                         lower_bound_pred<typename IndexT::value_type>);

    // If the key is not in the bloom filter or in the lower bounded index
    // entry, return nullopt
    if ((mFilter && !mFilter->contains(std::hash<stellar::LedgerKey>()(k))) ||
        keyIter == mKeysToOffset.end() || keyNotInIndexEntry(k, keyIter->first))
    {
        return {std::nullopt, keyIter};
    }
    else
    {
        return {keyIter->second, keyIter};
    }
}

template <class IndexT>
std::pair<std::streamoff, std::streamoff>
BucketIndexImpl<IndexT>::getPoolshareTrustlineRange(
    AccountID const& accountID) const
{
    // Get the smallest and largest possible trustline keys for the given
    // accountID
    auto upperBound = getDummyPoolShareTrustlineKey(
        accountID, std::numeric_limits<uint8_t>::max());
    auto lowerBound = getDummyPoolShareTrustlineKey(
        accountID, std::numeric_limits<uint8_t>::min());

    // Get the index iterators for the bounds
    auto startIter =
        std::lower_bound(mKeysToOffset.begin(), mKeysToOffset.end(), lowerBound,
                         lower_bound_pred<typename IndexT::value_type>);
    if (startIter == mKeysToOffset.end())
    {
        return {};
    }

    auto endIter =
        std::upper_bound(startIter, mKeysToOffset.end(), upperBound,
                         upper_bound_pred<typename IndexT::value_type>);

    // Get file offsets based on lower and upper bound iterators
    std::streamoff startOff = startIter->second;
    std::streamoff endOff = std::numeric_limits<std::streamoff>::max();

    // If we hit the end of the index then upper bound should be EOF
    if (endIter != mKeysToOffset.end())
    {
        endOff = endIter->second;
    }

    return std::make_pair(startOff, endOff);
}
}