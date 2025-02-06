// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/InMemoryIndex.h"
#include "bucket/BucketManager.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include "xdr/Stellar-ledger-entries.h"
#include <algorithm>

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
    std::optional<std::streamoff> firstContractEntry;
    std::optional<std::streamoff> lastContractEntry;

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

        // Populate contractEntryRange
        if (!firstContractEntry &&
            (lk.type() == CONTRACT_DATA || lk.type() == CONTRACT_CODE ||
             lk.type() == TTL))
        {
            CLOG_DEBUG(Ledger,
                       "Found *first* contract entry type {} at offset {}",
                       lk.type(), lastOffset);
            firstContractEntry = lastOffset;
        }
        else
        {
            if (firstContractEntry)
            {
                CLOG_DEBUG(Ledger, "Found contract entry type {} at offset {}",
                           lk.type(), lastOffset);
            }
            else
            {
                CLOG_DEBUG(Ledger, "Not a contract entry, type: {}", lk.type());
            }
        }
        if (!lastContractEntry && firstContractEntry && lk.type() > TTL)
        {
            CLOG_DEBUG(Ledger, "Found last contract entry at offset {}",
                       lastOffset);
            lastContractEntry = lastOffset;
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

    // TODO These bounds include config setting entries,
    // which are not contract entries.
    // Possibly consider adding more granular ranges for each contract type.
    if (firstContractEntry)
    {
        if (lastContractEntry)
        {
            mContractEntryRange = {*firstContractEntry, *lastContractEntry};
        }
        // If we didn't see any entries after contract entries, then the upper
        // bound is EOF
        else
        {
            CLOG_DEBUG(
                Ledger,
                "No last contract entry found, setting upper bound to EOF");
            mContractEntryRange = {*firstContractEntry,
                                   std::numeric_limits<std::streamoff>::max()};
        }
        CLOG_DEBUG(Ledger, "Found contract entry range from {} to {}",
                   *firstContractEntry, *lastContractEntry);
    }
    else
    {
        mContractEntryRange = std::nullopt;
        CLOG_DEBUG(Ledger, "No contract entry range found");
    }
}
}
