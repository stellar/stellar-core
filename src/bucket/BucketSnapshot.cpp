// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketSnapshot.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/SearchableBucketList.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"
#include "util/ProtocolVersion.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include <type_traits>

namespace stellar
{
template <class BucketT>
BucketSnapshotBase<BucketT>::BucketSnapshotBase(
    std::shared_ptr<BucketT const> const b)
    : mBucket(b)
{
    releaseAssert(mBucket);
}

template <class BucketT>
BucketSnapshotBase<BucketT>::BucketSnapshotBase(
    BucketSnapshotBase<BucketT> const& b)
    : mBucket(b.mBucket), mStream(nullptr)
{
    releaseAssert(mBucket);
}

template <class BucketT>
bool
BucketSnapshotBase<BucketT>::isEmpty() const
{
    releaseAssert(mBucket);
    return mBucket->isEmpty();
}

template <class BucketT>
std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
BucketSnapshotBase<BucketT>::getEntryAtOffset(LedgerKey const& k,
                                              std::streamoff pos,
                                              size_t pageSize) const
{
    ZoneScoped;
    releaseAssertOrThrow(pageSize > 0);
    if (isEmpty())
    {
        return {nullptr, false};
    }

    auto& stream = getStream();
    stream.seek(pos);

    typename BucketT::EntryT be;
    if (stream.readPage(be, k, pageSize))
    {
        auto entry = std::make_shared<typename BucketT::EntryT const>(be);
        mBucket->getIndex().maybeAddToCache(entry);
        return {entry, false};
    }

    mBucket->getIndex().markBloomMiss();
    return {nullptr, true};
}

template <class BucketT>
std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
BucketSnapshotBase<BucketT>::getBucketEntry(LedgerKey const& k) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return {nullptr, false};
    }

    auto indexRes = mBucket->getIndex().lookup(k);
    switch (indexRes.getState())
    {
    case IndexReturnState::CACHE_HIT:
        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            return {indexRes.cacheHit(), false};
        }
        else
        {
            throw std::runtime_error("Hot Archive reported cache hit");
        }
    case IndexReturnState::FILE_OFFSET:
        return getEntryAtOffset(k, indexRes.fileOffset(),
                                mBucket->getIndex().getPageSize());
    case IndexReturnState::NOT_FOUND:
        return {nullptr, false};
    }
}

// When searching for an entry, BucketList calls this function on every bucket.
// Since the input is sorted, we do a binary search for the first key in keys.
// If we find the entry, we remove the found key from keys so that later buckets
// do not load shadowed entries. If we don't find the entry, we do not remove it
// from keys so that it will be searched for again at a lower level.
template <class BucketT>
void
BucketSnapshotBase<BucketT>::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp>& keys,
    std::vector<typename BucketT::LoadT>& result) const
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
        // Scan for current key. Iterator returned is the lower_bound of the key
        // which will be our starting point for search for the next key
        auto [indexRes, newIndexIter] = index.scan(indexIter, *currKeyIt);
        indexIter = newIndexIter;

        // Check if the index actually found the key
        std::shared_ptr<typename BucketT::EntryT const> entryOp;
        switch (indexRes.getState())
        {

        // Index had entry in cache
        case IndexReturnState::CACHE_HIT:
            if constexpr (std::is_same_v<BucketT, LiveBucket>)
            {
                entryOp = indexRes.cacheHit();
            }
            else
            {
                throw std::runtime_error("Hot Archive reported cache hit");
            }
            break;
        // Index had entry offset, so we need to load the entry
        case IndexReturnState::FILE_OFFSET:
            std::tie(entryOp, std::ignore) =
                getEntryAtOffset(*currKeyIt, indexRes.fileOffset(),
                                 mBucket->getIndex().getPageSize());
            break;
        // Index did not have entry or offset, move on to search for next key
        case IndexReturnState::NOT_FOUND:
            ++currKeyIt;
            continue;
        }

        if (entryOp)
        {
            // Don't return tombstone entries, as these do not exist wrt
            // ledger state
            if (!BucketT::isTombstoneEntry(*entryOp))
            {
                // Only live bucket loads can be metered
                if constexpr (std::is_same_v<BucketT, LiveBucket>)
                {
                    bool addEntry = true;
                    if (addEntry)
                    {
                        result.push_back(entryOp->liveEntry());
                    }
                }
                else
                {
                    static_assert(std::is_same_v<BucketT, HotArchiveBucket>,
                                  "unexpected bucket type");
                    result.push_back(*entryOp);
                }
            }

            currKeyIt = keys.erase(currKeyIt);
            continue;
        }

        ++currKeyIt;
    }
}

std::vector<PoolID> const&
LiveBucketSnapshot::getPoolIDsByAsset(Asset const& asset) const
{
    static std::vector<PoolID> const emptyVec = {};
    if (isEmpty())
    {
        return emptyVec;
    }

    return mBucket->getIndex().getPoolIDsByAsset(asset);
}

// Note: evicatbleKeys and keysInEvictableEntries both reference the same
// entries. evictableEntries in order of eviction, keysInEvictableEntries in an
// unordered but searchable set.
Loop
LiveBucketSnapshot::scanForEviction(
    EvictionIterator& iter, uint32_t& bytesToScan, uint32_t ledgerSeq,
    std::list<EvictionResultEntry>& evictableEntries,
    SearchableLiveBucketListSnapshot const& bl, uint32_t ledgerVers,
    UnorderedSet<LedgerKey>& keysInEvictableEntries) const
{
    ZoneScoped;
    if (isEmpty() || protocolVersionIsBefore(mBucket->getBucketVersion(),
                                             SOROBAN_PROTOCOL_VERSION))
    {
        // EOF, skip to next bucket
        return Loop::INCOMPLETE;
    }

    if (bytesToScan == 0)
    {
        // Reached end of scan region
        return Loop::COMPLETE;
    }

    std::list<EvictionResultEntry> maybeEvictQueue;
    LedgerKeySet keysToSearch;

    auto processQueue = [&]() {
        auto loadResult = populateLoadedEntries(
            keysToSearch, bl.loadKeys(keysToSearch, "eviction"));
        for (auto& e : maybeEvictQueue)
        {
            // If TTL entry has not yet been deleted
            if (auto ttl = loadResult.find(getTTLKey(e.entry))->second;
                ttl != nullptr)
            {
                // If TTL of entry is expired
                if (!isLive(*ttl, ledgerSeq))
                {
                    // If entry is expired but not yet deleted, add it to
                    // evictable entries
                    e.liveUntilLedger = ttl->data.ttl().liveUntilLedgerSeq;
                    evictableEntries.emplace_back(e);
                    keysInEvictableEntries.insert(LedgerEntryKey(e.entry));
                    releaseAssertOrThrow(evictableEntries.size() ==
                                         keysInEvictableEntries.size());
                }
            }
        }
    };

    // Start evicting persistent entries in p23
    auto isEvictableType = [ledgerVers](auto const& le) {
        if (protocolVersionIsBefore(
                ledgerVers,
                LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            return isTemporaryEntry(le);
        }
        else
        {
            return isSorobanEntry(le);
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

            // Make sure we don't redundantly search for a key we've already
            // evicted in the previous bucket.
            if (isEvictableType(le.data) &&
                keysInEvictableEntries.find(LedgerEntryKey(le)) ==
                    keysInEvictableEntries.end())
            {
                keysToSearch.emplace(getTTLKey(le));

                // Set lifetime to 0 as default, will be updated after TTL keys
                // loaded
                maybeEvictQueue.emplace_back(EvictionResultEntry(le, iter, 0));
            }
        }

        if (bytesRead >= bytesToScan)
        {
            // Reached end of scan region
            bytesToScan = 0;
            processQueue();
            return Loop::COMPLETE;
        }

        bytesToScan -= bytesRead;
    }

    // Hit eof
    processQueue();
    return Loop::INCOMPLETE;
}

// Scans entries of the specified type in the bucket.
Loop
LiveBucketSnapshot::scanForEntriesOfType(
    LedgerEntryType type,
    std::function<Loop(BucketEntry const&)> callback) const
{
    ZoneScoped;
    if (isEmpty())
    {
        return Loop::INCOMPLETE;
    }

    auto range = mBucket->getRangeForType(type);
    if (!range)
    {
        return Loop::INCOMPLETE;
    }

    auto& stream = getStream();
    stream.seek(range->first);

    BucketEntry be;
    while (stream.pos() < range->second && stream.readOne(be))
    {
        bool matchesType = false;
        if (be.type() == LIVEENTRY || be.type() == INITENTRY)
        {
            matchesType = be.liveEntry().data.type() == type;
        }
        else if (be.type() == DEADENTRY)
        {
            matchesType = be.deadEntry().type() == type;
        }

        if (matchesType)
        {
            if (callback(be) == Loop::COMPLETE)
            {
                return Loop::COMPLETE;
            }
        }
    }
    return Loop::INCOMPLETE;
}

template <class BucketT>
XDRInputFileStream&
BucketSnapshotBase<BucketT>::getStream() const
{
    releaseAssertOrThrow(!isEmpty());
    if (!mStream)
    {
        mStream = std::make_unique<XDRInputFileStream>();
        mStream->open(mBucket->getFilename().string());
    }
    return *mStream;
}

template <class BucketT>
std::shared_ptr<BucketT const>
BucketSnapshotBase<BucketT>::getRawBucket() const
{
    return mBucket;
}

HotArchiveBucketSnapshot::HotArchiveBucketSnapshot(
    std::shared_ptr<HotArchiveBucket const> const b)
    : BucketSnapshotBase<HotArchiveBucket>(b)
{
}

LiveBucketSnapshot::LiveBucketSnapshot(
    std::shared_ptr<LiveBucket const> const b)
    : BucketSnapshotBase<LiveBucket>(b)
{
}

HotArchiveBucketSnapshot::HotArchiveBucketSnapshot(
    HotArchiveBucketSnapshot const& b)
    : BucketSnapshotBase<HotArchiveBucket>(b)
{
}

LiveBucketSnapshot::LiveBucketSnapshot(LiveBucketSnapshot const& b)
    : BucketSnapshotBase<LiveBucket>(b)
{
}

template class BucketSnapshotBase<LiveBucket>;
template class BucketSnapshotBase<HotArchiveBucket>;
}