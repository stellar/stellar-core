// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshot.h"
#include "bucket/Bucket.h"
#include "bucket/BucketListSnapshot.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"

#include "medida/counter.h"

namespace stellar
{
BucketSnapshot::BucketSnapshot(std::shared_ptr<Bucket const> const b)
    : mBucket(b)
{
    releaseAssert(mBucket);
}

BucketSnapshot::BucketSnapshot(BucketSnapshot const& b)
    : mBucket(b.mBucket), mStream(nullptr)
{
    releaseAssert(mBucket);
}

bool
BucketSnapshot::isEmpty() const
{
    releaseAssert(mBucket);
    return mBucket->isEmpty();
}

std::optional<BucketEntry>
BucketSnapshot::getEntryAtOffset(LedgerKey const& k, std::streamoff pos,
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
BucketSnapshot::getBucketEntry(LedgerKey const& k) const
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
BucketSnapshot::loadKeys(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
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

std::vector<PoolID> const&
BucketSnapshot::getPoolIDsByAsset(Asset const& asset) const
{
    static std::vector<PoolID> const emptyVec = {};
    if (isEmpty())
    {
        return emptyVec;
    }

    return mBucket->getIndex().getPoolIDsByAsset(asset);
}

XDRInputFileStream&
BucketSnapshot::getStream() const
{
    releaseAssertOrThrow(!isEmpty());
    if (!mStream)
    {
        mStream = std::make_unique<XDRInputFileStream>();
        mStream->open(mBucket->getFilename().string());
    }
    return *mStream;
}

std::shared_ptr<Bucket const>
BucketSnapshot::getRawBucket() const
{
    return mBucket;
}
}