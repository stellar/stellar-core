// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/InMemoryIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucket.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

// Helper function to process a single bucket entry for InMemoryIndex
// construction
static void
processEntry(BucketEntry const& be, InMemoryBucketState& inMemoryState,
             AssetPoolIDMap& assetPoolIDMap, BucketEntryCounters& counters)
{
    counters.template count<LiveBucket>(be);

    // Populate assetPoolIDMap
    LedgerKey lk = getBucketLedgerKey(be);
    if (be.type() == INITENTRY)
    {
        if (lk.type() == LIQUIDITY_POOL)
        {
            auto const& poolParams = be.liveEntry()
                                         .data.liquidityPool()
                                         .body.constantProduct()
                                         .params;
            assetPoolIDMap[poolParams.assetA].emplace_back(
                lk.liquidityPool().liquidityPoolID);
            assetPoolIDMap[poolParams.assetB].emplace_back(
                lk.liquidityPool().liquidityPoolID);
        }
    }

    inMemoryState.insert(be);
}

// Helper function to update offset ranges
static void
updateRanges(LedgerKey const& lk, std::streamoff offset,
             std::optional<std::streamoff>& firstOffer,
             std::optional<std::streamoff>& lastOffer,
             std::optional<std::streamoff>& firstContractCode,
             std::optional<std::streamoff>& lastContractCode)
{
    if (!firstOffer && lk.type() == OFFER)
    {
        firstOffer = offset;
    }
    if (!lastOffer && lk.type() > OFFER)
    {
        lastOffer = offset;
    }

    if (!firstContractCode && lk.type() == CONTRACT_CODE)
    {
        firstContractCode = offset;
    }
    if (!lastContractCode && lk.type() > CONTRACT_CODE)
    {
        lastContractCode = offset;
    }
}

// Returns final range that will be stored in the index, accounting for ranges
// where no entry exists and eof.
static std::optional<std::pair<std::streamoff, std::streamoff>>
getFinalRange(std::optional<std::streamoff> const& first,
              std::optional<std::streamoff> const& last)
{
    if (!first)
    {
        return std::nullopt;
    }

    auto endOffset = last.value_or(std::numeric_limits<std::streamoff>::max());
    return std::make_pair(*first, endOffset);
}

void
InMemoryBucketState::insert(BucketEntry const& be)
{
    auto [_, inserted] = mEntries.insert(
        InternalInMemoryBucketEntry(std::make_shared<BucketEntry const>(be)));
    releaseAssertOrThrow(inserted);
}

// Perform a binary search using start iter as lower bound for search key.
std::pair<IndexReturnT, InMemoryBucketState::IterT>
InMemoryBucketState::scan(IterT start, LedgerKey const& searchKey) const
{
    ZoneScoped;
    auto it = mEntries.find(InternalInMemoryBucketEntry(searchKey));
    // If we found the key
    if (it != mEntries.end())
    {
        return {IndexReturnT(it->get()), mEntries.begin()};
    }

    return {IndexReturnT(), mEntries.begin()};
}

InMemoryIndex::InMemoryIndex(BucketManager& bm,
                             std::vector<BucketEntry> const& inMemoryState,
                             BucketMetadata const& metadata)
{
    ZoneScoped;

    // 4 bytes of size info between BucketEntries on disk
    constexpr std::streamoff xdrOverheadBetweenEntries = 4;

    // 4 bytes of BucketEntry overhead for METAENTRY
    constexpr std::streamoff xdrOverheadForMetaEntry = 4;

    std::streamoff lastOffset = xdr::xdr_size(metadata) +
                                xdrOverheadForMetaEntry +
                                xdrOverheadBetweenEntries;
    std::optional<std::streamoff> firstOffer;
    std::optional<std::streamoff> lastOffer;
    std::optional<std::streamoff> firstContractCode;
    std::optional<std::streamoff> lastContractCode;

    for (auto const& be : inMemoryState)
    {
        releaseAssertOrThrow(be.type() != METAENTRY);

        processEntry(be, mInMemoryState, mAssetPoolIDMap, mCounters);

        LedgerKey lk = getBucketLedgerKey(be);
        updateRanges(lk, lastOffset, firstOffer, lastOffer, firstContractCode,
                     lastContractCode);

        lastOffset += xdr::xdr_size(be) + xdrOverheadBetweenEntries;
    }

    mOfferRange = getFinalRange(firstOffer, lastOffer);
    mContractCodeRange = getFinalRange(firstContractCode, lastContractCode);
}

InMemoryIndex::InMemoryIndex(BucketManager const& bm,
                             std::filesystem::path const& filename,
                             SHA256* hasher)
{
    ZoneScoped;
    XDRInputFileStream in;
    in.open(filename.string());
    BucketEntry be;
    size_t iter = 0;
    std::streamoff lastOffset = 0;
    std::optional<std::streamoff> firstOffer;
    std::optional<std::streamoff> lastOffer;
    std::optional<std::streamoff> firstContractCode;
    std::optional<std::streamoff> lastContractCode;

    while (in && in.readOne(be, hasher))
    {
        if (++iter >= 1000)
        {
            iter = 0;
            if (bm.isShutdown())
            {
                throw std::runtime_error("Incomplete bucket index due to "
                                         "BucketManager shutdown");
            }
        }

        if (be.type() == METAENTRY)
        {
            lastOffset = in.pos();
            continue;
        }

        processEntry(be, mInMemoryState, mAssetPoolIDMap, mCounters);

        LedgerKey lk = getBucketLedgerKey(be);
        updateRanges(lk, lastOffset, firstOffer, lastOffer, firstContractCode,
                     lastContractCode);

        lastOffset = in.pos();
    }

    // Set ranges
    mOfferRange = getFinalRange(firstOffer, lastOffer);
    mContractCodeRange = getFinalRange(firstContractCode, lastContractCode);
}

#ifdef BUILD_TESTS
bool
InMemoryIndex::operator==(InMemoryIndex const& in) const
{
    return mInMemoryState == in.mInMemoryState &&
           mAssetPoolIDMap == in.mAssetPoolIDMap &&
           mOfferRange == in.mOfferRange && mCounters == in.mCounters;
}
#endif
}