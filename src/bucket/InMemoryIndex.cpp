// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/InMemoryIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketUtils.h"
#include "bucket/LiveBucket.h"
#include "util/GlobalChecks.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"

namespace stellar
{

namespace
{
// Helper function to process a single bucket entry for InMemoryIndex
// construction
void
processEntry(BucketEntry const& be, InMemoryBucketState& inMemoryState,
             AssetPoolIDMap& assetPoolIDMap, BucketEntryCounters& counters,
             std::streamoff& lastOffset,
             std::map<LedgerEntryType, std::streamoff>& typeStartOffsets,
             std::map<LedgerEntryType, std::streamoff>& typeEndOffsets,
             std::optional<LedgerEntryType>& lastTypeSeen)
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
    updateTypeBoundaries(lk.type(), lastOffset, typeStartOffsets,
                         typeEndOffsets, lastTypeSeen);
}
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

    std::map<LedgerEntryType, std::streamoff> typeStartOffsets;
    std::map<LedgerEntryType, std::streamoff> typeEndOffsets;
    std::optional<LedgerEntryType> lastTypeSeen = std::nullopt;

    for (auto const& be : inMemoryState)
    {
        releaseAssertOrThrow(be.type() != METAENTRY);

        processEntry(be, mInMemoryState, mAssetPoolIDMap, mCounters, lastOffset,
                     typeStartOffsets, typeEndOffsets, lastTypeSeen);

        lastOffset += xdr::xdr_size(be) + xdrOverheadBetweenEntries;
    }

    // Build the final type ranges map
    mTypeRanges = buildTypeRangesMap(typeStartOffsets, typeEndOffsets);
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
    std::map<LedgerEntryType, std::streamoff> typeStartOffsets;
    std::map<LedgerEntryType, std::streamoff> typeEndOffsets;
    std::optional<LedgerEntryType> lastTypeSeen = std::nullopt;

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

        processEntry(be, mInMemoryState, mAssetPoolIDMap, mCounters, lastOffset,
                     typeStartOffsets, typeEndOffsets, lastTypeSeen);
        lastOffset = in.pos();
    }

    // Build the final type ranges map
    mTypeRanges = buildTypeRangesMap(typeStartOffsets, typeEndOffsets);
}

std::optional<std::pair<std::streamoff, std::streamoff>>
InMemoryIndex::getRangeForType(LedgerEntryType type) const
{
    auto it = mTypeRanges.find(type);
    if (it != mTypeRanges.end())
    {
        return std::make_optional(it->second);
    }

    return std::nullopt;
}

#ifdef BUILD_TESTS
bool
InMemoryIndex::operator==(InMemoryIndex const& in) const
{
    return mInMemoryState == in.mInMemoryState &&
           mAssetPoolIDMap == in.mAssetPoolIDMap &&
           mTypeRanges == in.mTypeRanges && mCounters == in.mCounters;
}
#endif
}
