// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshot.h"
#include "bucket/Bucket.h"
#include "bucket/BucketListSnapshot.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/XDRStream.h"

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

std::pair<std::optional<BucketEntry>, bool>
BucketSnapshot::getEntryAtOffset(LedgerKey const& k, std::streamoff pos,
                                 size_t pageSize) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return {std::nullopt, false};
    }

    auto& stream = getStream();
    stream.seek(pos);

    BucketEntry be;
    if (pageSize == 0)
    {
        if (stream.readOne(be))
        {
            return {std::make_optional(be), false};
        }
    }
    else if (stream.readPage(be, k, pageSize))
    {
        return {std::make_optional(be), false};
    }

    // Mark entry miss for metrics
    mBucket->getIndex().markBloomMiss();
    return {std::nullopt, true};
}

std::pair<std::optional<BucketEntry>, bool>
BucketSnapshot::getBucketEntry(LedgerKey const& k) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return {std::nullopt, false};
    }

    auto pos = mBucket->getIndex().lookup(k);
    if (pos.has_value())
    {
        return getEntryAtOffset(k, pos.value(),
                                mBucket->getIndex().getPageSize());
    }

    return {std::nullopt, false};
}

// When searching for an entry, BucketList calls this function on every bucket.
// Since the input is sorted, we do a binary search for the first key in keys.
// If we find the entry, we remove the found key from keys so that later buckets
// do not load shadowed entries. If we don't find the entry, we do not remove it
// from keys so that it will be searched for again at a lower level.
void
BucketSnapshot::loadKeysWithLimits(std::set<LedgerKey, LedgerEntryIdCmp>& keys,
                                   std::vector<LedgerEntry>& result,
                                   LedgerKeyMeter* lkMeter) const
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
        if (lkMeter)
        {
            auto keySize = xdr::xdr_size(*currKeyIt);
            if (!lkMeter->canLoad(*currKeyIt, keySize))
            {
                // If the transactions containing this key have a remaining
                // quota less than the size of the key, we cannot load the
                // entry, as xdr_size(key) <= xdr_size(entry). Here we consume
                // keySize bytes from the quotas of transactions containing the
                // key so that they will have zero remaining quota and
                // additional entries belonging to only those same transactions
                // will not be loaded even if they would fit in the remaining
                // quota before this update.
                lkMeter->updateReadQuotasForKey(*currKeyIt, keySize);
                currKeyIt = keys.erase(currKeyIt);
                continue;
            }
        }
        auto [offOp, newIndexIter] = index.scan(indexIter, *currKeyIt);
        indexIter = newIndexIter;
        if (offOp)
        {
            auto [entryOp, bloomMiss] = getEntryAtOffset(
                *currKeyIt, *offOp, mBucket->getIndex().getPageSize());
            if (entryOp)
            {
                if (entryOp->type() != DEADENTRY)
                {
                    bool addEntry = true;
                    if (lkMeter)
                    {
                        // Here, we are metering after the entry has been
                        // loaded. This is because we need to know the size of
                        // the entry to meter it. Future work will add metering
                        // at the xdr level.
                        auto entrySize = xdr::xdr_size(entryOp->liveEntry());
                        addEntry = lkMeter->canLoad(*currKeyIt, entrySize);
                        lkMeter->updateReadQuotasForKey(*currKeyIt, entrySize);
                    }
                    if (addEntry)
                    {
                        result.push_back(entryOp->liveEntry());
                    }
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

bool
BucketSnapshot::scanForEviction(EvictionIterator& iter, uint32_t& bytesToScan,
                                uint32_t ledgerSeq,
                                std::list<EvictionResultEntry>& evictableKeys,
                                SearchableBucketListSnapshot& bl) const
{
    ZoneScoped;
    if (isEmpty() || protocolVersionIsBefore(Bucket::getBucketVersion(mBucket),
                                             SOROBAN_PROTOCOL_VERSION))
    {
        // EOF, skip to next bucket
        return false;
    }

    if (bytesToScan == 0)
    {
        // Reached end of scan region
        return true;
    }

    std::list<EvictionResultEntry> maybeEvictQueue;
    LedgerKeySet keysToSearch;

    auto processQueue = [&]() {
        auto loadResult = populateLoadedEntries(
            keysToSearch, bl.loadKeysWithLimits(keysToSearch));
        for (auto& e : maybeEvictQueue)
        {
            // If TTL entry has not yet been deleted
            if (auto ttl = loadResult.find(getTTLKey(e.key))->second;
                ttl != nullptr)
            {
                // If TTL of entry is expired
                if (!isLive(*ttl, ledgerSeq))
                {
                    // If entry is expired but not yet deleted, add it to
                    // evictable keys
                    e.liveUntilLedger = ttl->data.ttl().liveUntilLedgerSeq;
                    evictableKeys.emplace_back(e);
                }
            }
        }
    };

    // Open new stream for eviction scan to not interfere with BucketListDB load
    // streams
    XDRInputFileStream stream{};
    stream.open(mBucket->getFilename());
    stream.seek(iter.bucketFileOffset);
    BucketEntry be;

    // First, scan the bucket region and record all temp entry keys in
    // maybeEvictQueue. After scanning, we will load all the TTL keys for these
    // entries in a single bulk load to determine
    //   1. If the entry is expired
    //   2. If the entry has already been deleted/evicted
    while (stream.readOne(be))
    {
        auto newPos = stream.pos();
        auto bytesRead = newPos - iter.bucketFileOffset;
        iter.bucketFileOffset = newPos;

        if (be.type() == INITENTRY || be.type() == LIVEENTRY)
        {
            auto const& le = be.liveEntry();
            if (isTemporaryEntry(le.data))
            {
                keysToSearch.emplace(getTTLKey(le));

                // Set lifetime to 0 as default, will be updated after TTL keys
                // loaded
                maybeEvictQueue.emplace_back(
                    EvictionResultEntry(LedgerEntryKey(le), iter, 0));
            }
        }

        if (bytesRead >= bytesToScan)
        {
            // Reached end of scan region
            bytesToScan = 0;
            processQueue();
            return true;
        }

        bytesToScan -= bytesRead;
    }

    // Hit eof
    processQueue();
    return false;
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