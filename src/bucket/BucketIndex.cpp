// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketIndex.h"
#include "bucket/Bucket.h"
#include "bucket/LedgerCmp.h"
#include "main/Config.h"
#include "util/Fs.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/XDRStream.h"

#include <Tracy.hpp>
#include <fmt/chrono.h>
#include <fmt/format.h>
#include <ledger/LedgerHashUtils.h>

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

template <class IndexT>
BucketIndexImpl<IndexT>::BucketIndexImpl(std::filesystem::path const& filename,
                                         std::streamoff pageSize)
    : mPageSize(pageSize)
{
    ZoneScoped;

    // Assert pageSize is a power of 2 for disk IO optimization
    releaseAssert((pageSize & (pageSize - 1)) == 0);
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
        params.compute_optimal_parameters();
        mFilter = std::make_unique<bloom_filter>(params);
        estimatedIndexEntries = fileSize / mPageSize;
    }
    else
    {
        estimatedIndexEntries = estimatedNumElems;
    }

    mPositions.reserve(estimatedIndexEntries);
    mKeys.reserve(estimatedIndexEntries);

    XDRInputFileStream in;
    in.open(filename.string());
    std::streamoff pos = 0;
    std::streamoff pageUpperBound = 0;
    BucketEntry be;
    while (in && in.readOne(be))
    {
        if (be.type() != METAENTRY)
        {
            LedgerKey key = getBucketLedgerKey(be);
            if constexpr (std::is_same<IndexT, RangeIndex>::value)
            {
                if (pos >= pageUpperBound)
                {
                    pageUpperBound = roundDown(pos, mPageSize) + mPageSize;
                    mKeys.emplace_back(RangeEntry(key, key));
                    mPositions.emplace_back(pos);
                }
                else
                {
                    mKeys.back().upperBound = key;
                }

                mFilter->insert(std::hash<stellar::LedgerKey>()(key));
            }
            else
            {
                mKeys.emplace_back(key);
                mPositions.emplace_back(pos);
            }
        }

        pos = in.pos();
    }

    CLOG_DEBUG(Bucket, "Indexed {} positions in {}", mPositions.size(),
               filename.filename());
    ZoneValue(static_cast<int64_t>(mKeys.size()));
    releaseAssertOrThrow(mPositions.size() == mKeys.size());
}

// For range index, check if key is within range
static bool
operator!=(LedgerKey const& key,
           BucketIndex::RangeIndex::const_iterator const& iter)
{
    return key < iter->lowerBound || iter->upperBound < key;
}

// For individual index, check if key is equal to key that iterator points
// to
static bool
operator!=(LedgerKey const& key,
           BucketIndex::IndividualIndex::const_iterator const& iter)
{
    return !(key == *iter);
}

// std::lower_bound operator for searching for a key in the RangeIndex
static bool
operator<(LedgerKey const& key, BucketIndex::RangeEntry const& indexEntry)
{
    return key < indexEntry.lowerBound;
}

// std::upper_bound operator for searching for a key in the RangeIndex
static bool
operator<(BucketIndex::RangeEntry const& indexEntry, LedgerKey const& key)
{
    return indexEntry.upperBound < key;
}

std::unique_ptr<BucketIndex const>
BucketIndex::createIndex(Config const& cfg,
                         std::filesystem::path const& filename)
{
    ZoneScoped;
    releaseAssertOrThrow(cfg.EXPERIMENTAL_BUCKET_KV_STORE);
    releaseAssertOrThrow(!filename.empty());
    auto pageSize = cfg.EXPERIMENTAL_BUCKET_KV_STORE_INDEX_PAGE_SIZE;
    auto cutoff = cfg.EXPERIMENTAL_BUCKET_KV_STORE_INDEX_CUTOFF;
    if (pageSize == 0 || fs::size(filename.string()) < cutoff)
    {
        CLOG_INFO(Bucket,
                  "BucketIndex::createIndex() indexing individual keys in "
                  "bucket {}",
                  filename);
        return std::unique_ptr<BucketIndexImpl<IndividualIndex> const>(
            new BucketIndexImpl<IndividualIndex>(filename, 0));
    }
    else
    {
        CLOG_INFO(Bucket,
                  "BucketIndex::createIndex() indexing key range with "
                  "page size "
                  "{} in bucket {}",
                  pageSize, filename);
        return std::unique_ptr<BucketIndexImpl<RangeIndex> const>(
            new BucketIndexImpl<RangeIndex>(filename, pageSize));
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
    ZoneValue(static_cast<int64_t>(mKeys.size()));
    if (mFilter && !mFilter->contains(std::hash<stellar::LedgerKey>()(k)))
    {
        return {std::nullopt, start};
    }

    auto internalStart = std::get<typename IndexT::const_iterator>(start);
    auto keyIter = std::lower_bound(internalStart, mKeys.end(), k);
    if (keyIter == mKeys.end() || k != keyIter)
    {
        return {std::nullopt, keyIter};
    }
    else
    {
        return {std::make_optional(mPositions.at(keyIter - mKeys.begin())),
                keyIter};
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
    auto startIter = std::lower_bound(mKeys.begin(), mKeys.end(), lowerBound);
    if (startIter == mKeys.end())
    {
        return {};
    }

    auto endIter = std::upper_bound(startIter, mKeys.end(), upperBound);

    // Get file offsets based on lower and upper bound iterators
    std::streamoff startOff = mPositions.at(startIter - mKeys.begin());
    std::streamoff endOff = std::numeric_limits<std::streamoff>::max();

    // If we hit the end of the index then upper bound should be EOF
    if (endIter != mKeys.end())
    {
        endOff = mPositions.at(endIter - mKeys.begin());
    }

    return std::make_pair(startOff, endOff);
}
}