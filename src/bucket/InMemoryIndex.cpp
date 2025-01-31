// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/InMemoryIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <algorithm>
#include <asm-generic/errno.h>

namespace stellar
{

void
InMemoryBucketState::pushBack(BucketEntry const& be)
{
    if (!mEntries.empty())
    {
        if (!BucketEntryIdCmp<LiveBucket>{}(*mEntries.back(), be))
        {
            throw std::runtime_error(
                "InMemoryBucketState::push_back: Inserted out of order entry!");
        }
    }

    mEntries.push_back(std::make_shared<BucketEntry>(be));
}

// Perform a binary search using start iter as lower bound for search key.
std::pair<IndexReturnT, InMemoryBucketState::IterT>
InMemoryBucketState::scan(IterT start, LedgerKey const& searchKey) const
{
    auto it =
        std::lower_bound(start, mEntries.end(), searchKey,
                         [](std::shared_ptr<BucketEntry const> const& element,
                            LedgerKey const& key) {
                             return getBucketLedgerKey(*element) < key;
                         });

    // If we found the key
    if (it != mEntries.end() && getBucketLedgerKey(**it) == searchKey)
    {
        return {IndexReturnT(*it), it};
    }

    return {IndexReturnT(), it};
}

InMemoryIndex::InMemoryIndex(BucketManager const& bm,
                             std::filesystem::path const& filename)
{
    XDRInputFileStream in;
    in.open(filename.string());
    BucketEntry be;
    size_t iter = 0;
    std::streamoff lastOffset = 0;
    std::optional<std::streamoff> firstOffer;
    std::optional<std::streamoff> lastOffer;

    while (in && in.readOne(be))
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
        mInMemoryState.pushBack(be);

        // Populate offerRange
        if (!firstOffer && lk.type() == OFFER)
        {
            firstOffer = lastOffset;
        }
        if (!lastOffer && lk.type() > OFFER)
        {
            lastOffer = lastOffset;
        }

        lastOffset = in.pos();
    }

    if (firstOffer)
    {
        if (lastOffer)
        {
            mOfferRange = {*firstOffer, *lastOffer};
        }
        // If we didn't see any entries after offers, then the upper bound is
        // EOF
        else
        {
            mOfferRange = {*firstOffer,
                           std::numeric_limits<std::streamoff>::max()};
        }
    }
    else
    {
        mOfferRange = std::nullopt;
    }
}
}