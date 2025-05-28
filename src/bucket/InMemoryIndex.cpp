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

InMemoryIndex::InMemoryIndex(BucketManager const& bm,
                             std::filesystem::path const& filename,
                             SHA256* hasher)
{
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

        mCounters.template count<LiveBucket>(be);

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
                mAssetPoolIDMap[poolParams.assetA].emplace_back(
                    lk.liquidityPool().liquidityPoolID);
                mAssetPoolIDMap[poolParams.assetB].emplace_back(
                    lk.liquidityPool().liquidityPoolID);
            }
        }

        // Populate inMemoryState
        mInMemoryState.insert(be);

        // Track type boundaries
        LedgerEntryType currentType = lk.type();
        updateTypeBoundaries(currentType, lastOffset, typeStartOffsets,
                             typeEndOffsets, lastTypeSeen);

        lastOffset = in.pos();
    }

    // Build the final type ranges map
    mTypeRanges = buildTypeRangesMap(typeStartOffsets, typeEndOffsets);
}

std::optional<std::pair<std::streamoff, std::streamoff>>
InMemoryIndex::getRangeForTypes(std::set<LedgerEntryType> const& types) const
{
    return getRangeForTypesHelper(types, mTypeRanges);
}
}