// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/SearchableBucketSnapshot.h"
#include "bucket/Bucket.h"
#include "bucket/SearchableBucketListSnapshot.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"

#include "medida/counter.h"

namespace stellar
{
SearchableBucketSnapshot::SearchableBucketSnapshot(
    std::shared_ptr<Bucket const> const b)
    : mBucket(b)
{
}

SearchableBucketSnapshot::SearchableBucketSnapshot(
    SearchableBucketSnapshot const& b)
    : mBucket(b.mBucket), mStream(nullptr)
{
}

bool
SearchableBucketSnapshot::isEmpty() const
{
    return mBucket && mBucket->isEmpty();
}

std::optional<BucketEntry>
SearchableBucketSnapshot::getEntryAtOffset(LedgerKey const& k,
                                           std::streamoff pos,
                                           size_t pageSize) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return std::nullopt;
    }

    auto& stream = getStream();
    stream.seek(pos);

    BucketEntry be;
    if (pageSize == 0)
    {
        if (stream.readOne(be))
        {
            return std::make_optional(be);
        }
    }
    else if (stream.readPage(be, k, pageSize))
    {
        return std::make_optional(be);
    }

    // Mark entry miss for metrics
    mBucket->getIndex().markBloomMiss();
    return std::nullopt;
}

std::optional<BucketEntry>
SearchableBucketSnapshot::getBucketEntry(LedgerKey const& k) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return std::nullopt;
    }

    auto pos = mBucket->getIndex().lookup(k);
    if (pos.has_value())
    {
        return getEntryAtOffset(k, pos.value(),
                                mBucket->getIndex().getPageSize());
    }

    return std::nullopt;
}

// When searching for an entry, BucketList calls this function on every bucket.
// Since the input is sorted, we do a binary search for the first key in keys.
// If we find the entry, we remove the found key from keys so that later buckets
// do not load shadowed entries. If we don't find the entry, we do not remove it
// from keys so that it will be searched for again at a lower level.
void
SearchableBucketSnapshot::loadKeys(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                                   std::vector<LedgerEntry>& result) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return;
    }

    auto currKeyIt = keys.begin();
    auto const& index = mBucket->getIndex();
    auto indexIter = index.begin();
    while (currKeyIt != keys.end() && indexIter != index.end())
    {
        auto [offOp, newIndexIter] = index.scan(indexIter, *currKeyIt);
        indexIter = newIndexIter;
        if (offOp)
        {
            auto entryOp = getEntryAtOffset(*currKeyIt, *offOp,
                                            mBucket->getIndex().getPageSize());
            if (entryOp)
            {
                if (entryOp->type() != DEADENTRY)
                {
                    result.push_back(entryOp->liveEntry());
                }

                currKeyIt = keys.erase(currKeyIt);
                continue;
            }
        }

        ++currKeyIt;
    }
}

void
SearchableBucketSnapshot::loadPoolShareTrustLinessByAccount(
    AccountID const& accountID, UnorderedSet<LedgerKey>& deadTrustlines,
    UnorderedMap<LedgerKey, LedgerEntry>& liquidityPoolKeyToTrustline,
    LedgerKeySet& liquidityPoolKeys) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return;
    }

    // Takes a LedgerKey or LedgerEntry::_data_t, returns true if entry is a
    // poolshare trusline for the given accountID
    auto trustlineCheck = [&accountID](auto const& entry) {
        return entry.type() == TRUSTLINE &&
               entry.trustLine().asset.type() == ASSET_TYPE_POOL_SHARE &&
               entry.trustLine().accountID == accountID;
    };

    // Get upper and lower bound for poolshare trustline range associated
    // with this account
    auto searchRange =
        mBucket->getIndex().getPoolshareTrustlineRange(accountID);
    if (searchRange.first == 0)
    {
        // No poolshare trustlines, exit
        return;
    }

    BucketEntry be;
    auto& stream = getStream();
    stream.seek(searchRange.first);
    while (stream && stream.pos() < searchRange.second && stream.readOne(be))
    {
        LedgerEntry entry;
        switch (be.type())
        {
        case LIVEENTRY:
        case INITENTRY:
            entry = be.liveEntry();
            break;
        case DEADENTRY:
        {
            auto key = be.deadEntry();

            // If we find a valid trustline key and we have not seen the
            // key yet, mark it as dead so we do not load a shadowed version
            // later
            if (trustlineCheck(key))
            {
                deadTrustlines.emplace(key);
            }
            continue;
        }
        case METAENTRY:
        default:
            throw std::invalid_argument("Indexed METAENTRY");
        }

        // If this is a pool share trustline that matches the accountID and
        // is not shadowed, add it to results
        if (trustlineCheck(entry.data) &&
            deadTrustlines.find(LedgerEntryKey(entry)) == deadTrustlines.end())
        {
            auto const& poolshareID =
                entry.data.trustLine().asset.liquidityPoolID();

            LedgerKey key;
            key.type(LIQUIDITY_POOL);
            key.liquidityPool().liquidityPoolID = poolshareID;

            liquidityPoolKeyToTrustline.emplace(key, entry);
            liquidityPoolKeys.emplace(key);
        }
    }
}

XDRInputFileStream&
SearchableBucketSnapshot::getStream() const
{
    if (!mStream)
    {
        mStream = std::make_unique<XDRInputFileStream>();
        releaseAssertOrThrow(mBucket && !mBucket->isEmpty());
        mStream->open(mBucket->getFilename().string());
    }
    return *mStream;
}

std::shared_ptr<Bucket const>
SearchableBucketSnapshot::getRawBucket() const
{
    return mBucket;
}
}