// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/BucketListSnapshot.h"
#include "bucket/BucketIndexUtils.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketListBase.h"
#include "bucket/LiveBucketList.h"
#include "ledger/LedgerTxn.h"
#include "ledger/LedgerTypeUtils.h"
#include "util/GlobalChecks.h"
#include "util/MetricsRegistry.h"
#include "util/ProtocolVersion.h"

#include <medida/counter.h>
#include <medida/meter.h>
#include <medida/timer.h>

namespace stellar
{

//
// BucketListSnapshotData
//

template <class BucketT>
BucketListSnapshotData<BucketT>::Level::Level(BucketLevel<BucketT> const& level)
    : curr(level.getCurr()), snap(level.getSnap())
{
}

template <class BucketT>
BucketListSnapshotData<BucketT>::Level::Level(
    std::shared_ptr<BucketT const> currBucket,
    std::shared_ptr<BucketT const> snapBucket)
    : curr(std::move(currBucket)), snap(std::move(snapBucket))
{
}

template <class BucketT>
BucketListSnapshotData<BucketT>::BucketListSnapshotData(
    BucketListBase<BucketT> const& bl)
    : levels([&bl]() {
        std::vector<Level> v;
        v.reserve(BucketListBase<BucketT>::kNumLevels);
        for (uint32_t i = 0; i < BucketListBase<BucketT>::kNumLevels; ++i)
        {
            v.emplace_back(bl.getLevel(i));
        }
        return v;
    }())
{
}

//
// SearchableBucketListSnapshot
//

template <class BucketT>
SearchableBucketListSnapshot<BucketT>::SearchableBucketListSnapshot(
    MetricsRegistry& metrics,
    std::shared_ptr<BucketListSnapshotData<BucketT> const> data,
    std::map<uint32_t, std::shared_ptr<BucketListSnapshotData<BucketT> const>>
        historicalSnapshots,
    uint32_t ledgerSeq)
    : mData(std::move(data))
    , mHistoricalSnapshots(std::move(historicalSnapshots))
    , mLedgerSeq(ledgerSeq)
    , mMetrics(metrics)
    , mBulkLoadMeter(
          metrics.NewMeter({BucketT::METRIC_STRING, "query", "loads"}, "query"))
{
    for (auto t : xdr::xdr_traits<LedgerEntryType>::enum_values())
    {
        auto const& label = xdr::xdr_traits<LedgerEntryType>::enum_name(
            static_cast<LedgerEntryType>(t));
        auto& metric = metrics.NewSimpleTimer({BucketT::METRIC_STRING, label},
                                              std::chrono::microseconds{1});
        mPointTimers.emplace(static_cast<LedgerEntryType>(t), metric);
    }
}

template <class BucketT>
SearchableBucketListSnapshot<BucketT>::SearchableBucketListSnapshot(
    SearchableBucketListSnapshot const& other)
    : mData(other.mData)
    , mHistoricalSnapshots(other.mHistoricalSnapshots)
    , mLedgerSeq(other.mLedgerSeq)
    // mStreams intentionally left empty — each copy gets its own stream cache
    , mMetrics(other.mMetrics)
    , mPointTimers(other.mPointTimers)
    , mBulkTimers(other.mBulkTimers)
    , mBulkLoadMeter(other.mBulkLoadMeter)
{
}

template <class BucketT>
SearchableBucketListSnapshot<BucketT>&
SearchableBucketListSnapshot<BucketT>::operator=(
    SearchableBucketListSnapshot const& other)
{
    if (this != &other)
    {
        mData = other.mData;
        mHistoricalSnapshots = other.mHistoricalSnapshots;
        mLedgerSeq = other.mLedgerSeq;
        mStreams.clear();
        mMetrics = other.mMetrics;
        mPointTimers = other.mPointTimers;
        mBulkTimers = other.mBulkTimers;
        mBulkLoadMeter = other.mBulkLoadMeter;
    }
    return *this;
}

// File streams are fairly expensive to create, so they are lazily created and
// stored in mStreams.
template <class BucketT>
XDRInputFileStream&
SearchableBucketListSnapshot<BucketT>::getStream(
    std::shared_ptr<BucketT const> const& bucket) const
{
    BucketT const* key = bucket.get();
    auto it = mStreams.find(key);
    if (it == mStreams.end())
    {
        auto stream = std::make_unique<XDRInputFileStream>();
        stream->open(bucket->getFilename());
        it = mStreams.emplace(key, std::move(stream)).first;
    }
    return *it->second;
}

// Loads an entry from the bucket file at the given offset. Returns a pair of
// (entry, bloomMiss) where bloomMiss is true if the bloom filter indicated the
// key might exist but it wasn't actually found (a false positive).
template <class BucketT>
std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
SearchableBucketListSnapshot<BucketT>::getEntryAtOffset(
    std::shared_ptr<BucketT const> const& bucket, LedgerKey const& k,
    std::streamoff pos, size_t pageSize) const
{
    ZoneScoped;
    releaseAssertOrThrow(pageSize > 0);
    if (bucket->isEmpty())
    {
        return {nullptr, false};
    }

    auto& stream = getStream(bucket);
    stream.seek(pos);

    typename BucketT::EntryT be;
    if (stream.readPage(be, k, pageSize))
    {
        auto entry = std::make_shared<typename BucketT::EntryT const>(be);
        bucket->getIndex().maybeAddToCache(entry);
        return {entry, false};
    }

    bucket->getIndex().markBloomMiss();
    return {nullptr, true};
}

// Looks up a single key in a bucket using its index. Returns (entry, bloomMiss)
// where entry is nullptr if not found, and bloomMiss indicates a bloom filter
// false positive (key appeared to exist but wasn't actually in the bucket).
template <class BucketT>
std::pair<std::shared_ptr<typename BucketT::EntryT const>, bool>
SearchableBucketListSnapshot<BucketT>::getBucketEntry(
    std::shared_ptr<BucketT const> const& bucket, LedgerKey const& k) const
{
    ZoneScoped;
    if (bucket->isEmpty())
    {
        return {nullptr, false};
    }

    auto indexRes = bucket->getIndex().lookup(k);
    switch (indexRes.getState())
    {
    // Index had entry in cache
    case IndexReturnState::CACHE_HIT:
        if constexpr (std::is_same_v<BucketT, LiveBucket>)
        {
            return {indexRes.cacheHit(), false};
        }
        else
        {
            throw std::runtime_error("Hot Archive reported cache hit");
        }
    // Index found file offset, load entry from disk
    case IndexReturnState::FILE_OFFSET:
        return getEntryAtOffset(bucket, k, indexRes.fileOffset(),
                                bucket->getIndex().getPageSize());
    // Key not in this bucket
    case IndexReturnState::NOT_FOUND:
        return {nullptr, false};
    }
}

// Bulk load multiple keys from a single bucket. Since the input keys are
// sorted, we do a binary search for the first key. If we find an entry, we
// remove it from keys so that later buckets do not load shadowed entries. If we
// don't find the entry, we keep it in keys so it will be searched for in lower
// levels.
template <class BucketT>
void
SearchableBucketListSnapshot<BucketT>::loadKeysFromBucket(
    std::shared_ptr<BucketT const> const& bucket,
    std::set<LedgerKey, LedgerEntryIdCmp>& keys,
    std::vector<typename BucketT::LoadT>& result) const
{
    ZoneScoped;
    if (bucket->isEmpty())
    {
        return;
    }

    auto currKeyIt = keys.begin();
    auto const& index = bucket->getIndex();
    auto indexIter = index.begin();
    while (currKeyIt != keys.end() && indexIter != index.end())
    {
        // Scan for current key. Iterator returned is the lower_bound of the
        // key which will be our starting point for the next key search.
        auto [indexRes, newIndexIter] = index.scan(indexIter, *currKeyIt);
        indexIter = newIndexIter;

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
        // Index found file offset, load entry from disk
        case IndexReturnState::FILE_OFFSET:
            std::tie(entryOp, std::ignore) =
                getEntryAtOffset(bucket, *currKeyIt, indexRes.fileOffset(),
                                 bucket->getIndex().getPageSize());
            break;
        // Key not in this bucket, try next key
        case IndexReturnState::NOT_FOUND:
            ++currKeyIt;
            continue;
        }

        if (entryOp)
        {
            if (!BucketT::isTombstoneEntry(*entryOp))
            {
                if constexpr (std::is_same_v<BucketT, LiveBucket>)
                {
                    result.push_back(entryOp->liveEntry());
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

template <class BucketT>
template <typename Func>
void
SearchableBucketListSnapshot<BucketT>::loopAllBuckets(
    Func&& f, BucketListSnapshotData<BucketT> const& snapshot) const
{
    for (auto const& level : snapshot.levels)
    {
        if (level.curr && !level.curr->isEmpty())
        {
            if (f(level.curr) == Loop::COMPLETE)
            {
                return;
            }
        }
        if (level.snap && !level.snap->isEmpty())
        {
            if (f(level.snap) == Loop::COMPLETE)
            {
                return;
            }
        }
    }
}

template <class BucketT>
template <typename Func>
void
SearchableBucketListSnapshot<BucketT>::loopAllBuckets(Func&& f) const
{
    releaseAssert(mData);
    loopAllBuckets(std::forward<Func>(f), *mData);
}

template <class BucketT>
std::shared_ptr<typename BucketT::LoadT const>
SearchableBucketListSnapshot<BucketT>::load(LedgerKey const& k) const
{
    ZoneScoped;
    releaseAssert(mData);

    auto timerIter = mPointTimers.find(k.type());
    releaseAssert(timerIter != mPointTimers.end());
    auto timer = timerIter->second.get().TimeScope();

    std::shared_ptr<typename BucketT::LoadT const> result{};

    // Search function called on each Bucket in BucketList until we find the key
    auto loadKeyBucketLoop = [&](std::shared_ptr<BucketT const> const& bucket) {
        auto [be, bloomMiss] = getBucketEntry(bucket, k);
        if (bloomMiss)
        {
            // Reset timer on bloom miss to avoid outlier metrics, since we
            // really only want to measure disk performance
            timer.Reset();
        }

        if (be)
        {
            result = BucketT::bucketEntryToLoadResult(be);
            return Loop::COMPLETE;
        }
        return Loop::INCOMPLETE;
    };

    loopAllBuckets(loadKeyBucketLoop);
    return result;
}

template <class BucketT>
std::optional<std::vector<typename BucketT::LoadT>>
SearchableBucketListSnapshot<BucketT>::loadKeysInternal(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    std::optional<uint32_t> ledgerSeq) const
{
    ZoneScoped;
    releaseAssert(mData);

    // Make a copy of the key set, this loop is destructive
    auto keys = inKeys;
    std::vector<typename BucketT::LoadT> entries;

    auto loadKeysLoop = [&](std::shared_ptr<BucketT const> const& bucket) {
        loadKeysFromBucket(bucket, keys, entries);
        return keys.empty() ? Loop::COMPLETE : Loop::INCOMPLETE;
    };

    if (!ledgerSeq || *ledgerSeq == mLedgerSeq)
    {
        loopAllBuckets(loadKeysLoop, *mData);
    }
    else
    {
        auto iter = mHistoricalSnapshots.find(*ledgerSeq);
        if (iter == mHistoricalSnapshots.end())
        {
            return std::nullopt;
        }
        releaseAssert(iter->second);
        loopAllBuckets(loadKeysLoop, *iter->second);
    }

    return entries;
}

template <class BucketT>
std::optional<std::vector<typename BucketT::LoadT>>
SearchableBucketListSnapshot<BucketT>::loadKeysFromLedger(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    uint32_t ledgerSeq) const
{
    return loadKeysInternal(inKeys, ledgerSeq);
}

template <class BucketT>
medida::Timer&
SearchableBucketListSnapshot<BucketT>::getBulkLoadTimer(
    std::string const& label, size_t numEntries) const
{
    if (numEntries != 0)
    {
        mBulkLoadMeter.get().Mark(numEntries);
    }

    auto iter = mBulkTimers.find(label);
    if (iter == mBulkTimers.end())
    {
        auto& metric =
            mMetrics.get().NewTimer({BucketT::METRIC_STRING, "bulk", label});
        iter = mBulkTimers.emplace(label, metric).first;
    }

    return iter->second.get();
}

template <class BucketT>
std::shared_ptr<BucketListSnapshotData<BucketT> const> const&
SearchableBucketListSnapshot<BucketT>::getSnapshotData() const
{
    return mData;
}

template <class BucketT>
std::map<uint32_t,
         std::shared_ptr<BucketListSnapshotData<BucketT> const>> const&
SearchableBucketListSnapshot<BucketT>::getHistoricalSnapshots() const
{
    return mHistoricalSnapshots;
}

//
// SearchableLiveBucketListSnapshot
//

SearchableLiveBucketListSnapshot::SearchableLiveBucketListSnapshot(
    MetricsRegistry& metrics,
    std::shared_ptr<BucketListSnapshotData<LiveBucket> const> data,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<LiveBucket> const>>
        historicalSnapshots,
    uint32_t ledgerSeq)
    : SearchableBucketListSnapshot<LiveBucket>(
          metrics, std::move(data), std::move(historicalSnapshots), ledgerSeq)
{
}

std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys,
    std::string const& label) const
{
    auto timer = getBulkLoadTimer(label, inKeys.size()).TimeScope();
    auto op = loadKeysInternal(inKeys, std::nullopt);
    releaseAssertOrThrow(op);
    return std::move(*op);
}

// This query has two steps:
//  1. For each bucket, determine what PoolIDs contain the target asset via the
//     assetToPoolID index
//  2. Perform a bulk lookup for all possible trustline keys, that is, all
//     trustlines with the given accountID and poolID from step 1
std::vector<LedgerEntry>
SearchableLiveBucketListSnapshot::loadPoolShareTrustLinesByAccountAndAsset(
    AccountID const& accountID, Asset const& asset) const
{
    ZoneScoped;
    releaseAssert(mData);

    LedgerKeySet trustlinesToLoad;

    auto trustLineLoop = [&](std::shared_ptr<LiveBucket const> const& bucket) {
        static std::vector<PoolID> const emptyVec = {};
        auto const& poolIDs = bucket->isEmpty()
                                  ? emptyVec
                                  : bucket->getIndex().getPoolIDsByAsset(asset);
        for (auto const& poolID : poolIDs)
        {
            LedgerKey trustlineKey(TRUSTLINE);
            trustlineKey.trustLine().accountID = accountID;
            trustlineKey.trustLine().asset.type(ASSET_TYPE_POOL_SHARE);
            trustlineKey.trustLine().asset.liquidityPoolID() = poolID;
            trustlinesToLoad.emplace(trustlineKey);
        }
        return Loop::INCOMPLETE;
    };

    loopAllBuckets(trustLineLoop);

    auto timer =
        getBulkLoadTimer("poolshareTrustlines", trustlinesToLoad.size())
            .TimeScope();

    std::vector<LedgerEntry> result;
    auto loadKeysLoop = [&](std::shared_ptr<LiveBucket const> const& bucket) {
        loadKeysFromBucket(bucket, trustlinesToLoad, result);
        return trustlinesToLoad.empty() ? Loop::COMPLETE : Loop::INCOMPLETE;
    };

    loopAllBuckets(loadKeysLoop);
    return result;
}

// This is a legacy query, should only be called by main thread during catchup.
std::vector<InflationWinner>
SearchableLiveBucketListSnapshot::loadInflationWinners(size_t maxWinners,
                                                       int64_t minBalance) const
{
    ZoneScoped;
    releaseAssert(mData);

    auto timer = getBulkLoadTimer("inflationWinners", 0).TimeScope();

    UnorderedMap<AccountID, int64_t> voteCount;
    UnorderedSet<AccountID> seen;

    auto countVotesInBucket =
        [&](std::shared_ptr<LiveBucket const> const& bucket) {
            for (LiveBucketInputIterator in(bucket); in; ++in)
            {
                BucketEntry const& be = *in;
                if (be.type() == DEADENTRY)
                {
                    if (be.deadEntry().type() == ACCOUNT)
                    {
                        seen.insert(be.deadEntry().account().accountID);
                    }
                    continue;
                }

                // Accounts are ordered first, so once we see a non-account
                // entry, no other accounts are left in the bucket
                LedgerEntry const& le = be.liveEntry();
                if (le.data.type() != ACCOUNT)
                {
                    break;
                }

                // Don't double count AccountEntry's seen in earlier levels
                AccountEntry const& ae = le.data.account();
                AccountID const& id = ae.accountID;
                if (!seen.insert(id).second)
                {
                    continue;
                }

                if (ae.inflationDest && ae.balance >= 1000000000)
                {
                    voteCount[*ae.inflationDest] += ae.balance;
                }
            }
            return Loop::INCOMPLETE;
        };

    loopAllBuckets(countVotesInBucket);

    std::vector<InflationWinner> winners;

    // Check if we need to sort the voteCount by number of votes
    if (voteCount.size() > maxWinners)
    {
        // Sort Inflation winners by vote count in descending order
        std::map<int64_t, UnorderedMap<AccountID, int64_t>::const_iterator,
                 std::greater<int64_t>>
            voteCountSortedByCount;
        for (auto iter = voteCount.cbegin(); iter != voteCount.cend(); ++iter)
        {
            voteCountSortedByCount[iter->second] = iter;
        }

        for (auto iter = voteCountSortedByCount.cbegin();
             winners.size() < maxWinners && iter->first >= minBalance; ++iter)
        {
            winners.push_back(
                InflationWinner{iter->second->first, iter->first});
        }
    }
    else
    {
        for (auto const& [id, count] : voteCount)
        {
            if (count >= minBalance)
            {
                winners.push_back({id, count});
            }
        }
    }

    return winners;
}

// Scans the BucketList for entries eligible for eviction. This runs in the
// background and returns candidates that may be invalidated during TX apply.
//
// We track evicted keys in two ways:
// 1. result linked list - maintains order of eviction candidates. Since some
//    candidates may be invalidated during TX apply, we track order so the main
//    thread knows the cutoff point for what actually gets evicted.
// 2. keysToEvict set - prevents evicting the same key twice. It's possible for
//    an entry to be expired with two different versions in different buckets.
//    We scan both versions but should only evict once.
std::unique_ptr<EvictionResultCandidates>
SearchableLiveBucketListSnapshot::scanForEviction(
    uint32_t ledgerSeq, EvictionMetrics& metrics, EvictionIterator evictionIter,
    std::shared_ptr<EvictionStatistics> stats, StateArchivalSettings const& sas,
    uint32_t ledgerVers) const
{
    ZoneScoped;
    releaseAssert(mData);
    releaseAssert(stats);

    auto getBucketFromIter =
        [&levels = mData->levels](
            EvictionIterator const& iter) -> std::shared_ptr<LiveBucket const> {
        auto& level = levels.at(iter.bucketListLevel);
        return iter.isCurrBucket ? level.curr : level.snap;
    };

    LiveBucketList::updateStartingEvictionIterator(
        evictionIter, sas.startingEvictionScanLevel, ledgerSeq);

    std::unique_ptr<EvictionResultCandidates> result =
        std::make_unique<EvictionResultCandidates>(sas, ledgerSeq, ledgerVers);
    UnorderedSet<LedgerKey> keysToEvict;
    auto startIter = evictionIter;
    auto scanSize = sas.evictionScanSize;

    for (;;)
    {
        auto bucket = getBucketFromIter(evictionIter);
        LiveBucketList::checkIfEvictionScanIsStuck(
            evictionIter, sas.evictionScanSize, bucket, metrics);

        // If we scan scanSize bytes before hitting bucket EOF, exit early
        if (scanForEvictionInBucket(bucket, evictionIter, scanSize, ledgerSeq,
                                    result->eligibleEntries, ledgerVers,
                                    keysToEvict) == Loop::COMPLETE)
        {
            break;
        }

        // If we return back to the Bucket we started at, exit
        if (LiveBucketList::updateEvictionIterAndRecordStats(
                evictionIter, startIter, sas.startingEvictionScanLevel,
                ledgerSeq, stats, metrics))
        {
            break;
        }
    }

    result->endOfRegionIterator = evictionIter;
    return result;
}

void
SearchableLiveBucketListSnapshot::scanForEntriesOfType(
    LedgerEntryType type,
    std::function<Loop(BucketEntry const&)> callback) const
{
    ZoneScoped;
    releaseAssert(mData);

    auto scanBucket = [&](std::shared_ptr<LiveBucket const> const& bucket) {
        if (bucket->isEmpty())
        {
            return Loop::INCOMPLETE;
        }

        auto range = bucket->getRangeForType(type);
        if (!range)
        {
            return Loop::INCOMPLETE;
        }

        auto& stream = getStream(bucket);
        stream.seek(range->first);

        BucketEntry be;
        while (stream.readOne(be))
        {
            if (!isBucketMetaEntry<LiveBucket>(be))
            {
                if (LedgerKey key = getBucketLedgerKey(be); key.type() > type)
                {
                    break;
                }
            }

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
    };

    loopAllBuckets(scanBucket);
}

// Helper function to handle scan logic in a single bucket.
Loop
SearchableLiveBucketListSnapshot::scanForEvictionInBucket(
    std::shared_ptr<LiveBucket const> const& bucket, EvictionIterator& iter,
    uint32_t& bytesToScan, uint32_t ledgerSeq,
    std::list<EvictionResultEntry>& evictableEntries, uint32_t ledgerVers,
    UnorderedSet<LedgerKey>& keysInEvictableEntries) const
{
    ZoneScoped;

    if (bucket->isEmpty() || protocolVersionIsBefore(bucket->getBucketVersion(),
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
        // Note: load from a stale snapshot here. Expired entries should
        // never be modified by the ltx, unless they're getting restored.
        // `resolveBackgroundEviction` checks that entries loaded from the
        // snapshot are indeed not touched by the ltx, so it should be safe
        // to evict these candidates.
        auto loadResult = populateLoadedEntries(
            keysToSearch, loadKeys(keysToSearch, "eviction"));
        for (auto& e : maybeEvictQueue)
        {
            // If TTL entry has not yet been deleted
            if (auto ttl = loadResult.find(getTTLKey(e.entry))->second;
                ttl != nullptr)
            {
                // If TTL of entry is expired
                if (!isLive(*ttl, ledgerSeq))
                {
                    // Note: There was a bug in protocol 23 where we would
                    // not check if an entry was the newest version and would
                    // evict whatever version was scanned.
                    if (protocolVersionStartsFrom(ledgerVers,
                                                  ProtocolVersion::V_24) &&
                        isPersistentEntry(e.entry.data))
                    {
                        // Make sure we only ever evict the most recent version
                        // of persistent entries. Make sure we use the entry
                        // from loadKeys, as they are guaranteed to be the
                        // newest version. We could have scanned and populated
                        // `e` with an older version.
                        auto newestVersionIter =
                            loadResult.find(LedgerEntryKey(e.entry));
                        releaseAssertOrThrow(newestVersionIter !=
                                             loadResult.end());
                        e.entry = *newestVersionIter->second;
                    }

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
    stream.open(bucket->getFilename().string());
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

                // For temp entries, we don't care if we evict the newest
                // version of the entry, since it's just a deletion. However,
                // for persistent entries, we need to make sure we evict the
                // newest version of the entry, since it will be persisted in
                // the hot archive. So we add persistent entries to our DB
                // lookup to find the newest version. Note that there was a
                // bug in protocol 23 where this check was not performed.
                if (isPersistentEntry(le.data) &&
                    protocolVersionStartsFrom(ledgerVers,
                                              ProtocolVersion::V_24))
                {
                    keysToSearch.emplace(LedgerEntryKey(le));
                }

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

    // Hit EOF
    processQueue();
    return Loop::INCOMPLETE;
}

//
// SearchableHotArchiveBucketListSnapshot
//

SearchableHotArchiveBucketListSnapshot::SearchableHotArchiveBucketListSnapshot(
    MetricsRegistry& metrics,
    std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const> data,
    std::map<uint32_t,
             std::shared_ptr<BucketListSnapshotData<HotArchiveBucket> const>>
        historicalSnapshots,
    uint32_t ledgerSeq)
    : SearchableBucketListSnapshot<HotArchiveBucket>(
          metrics, std::move(data), std::move(historicalSnapshots), ledgerSeq)
{
}

std::vector<HotArchiveBucketEntry>
SearchableHotArchiveBucketListSnapshot::loadKeys(
    std::set<LedgerKey, LedgerEntryIdCmp> const& inKeys) const
{
    auto op = loadKeysInternal(inKeys, std::nullopt);
    releaseAssertOrThrow(op);
    return std::move(*op);
}

void
SearchableHotArchiveBucketListSnapshot::scanAllEntries(
    std::function<Loop(HotArchiveBucketEntry const&)> callback) const
{
    ZoneScoped;
    releaseAssert(mData);

    auto scanBucket =
        [&](std::shared_ptr<HotArchiveBucket const> const& bucket) {
            if (bucket->isEmpty())
            {
                return Loop::INCOMPLETE;
            }

            for (HotArchiveBucketInputIterator iter(bucket); iter; ++iter)
            {
                if (callback(*iter) == Loop::COMPLETE)
                {
                    return Loop::COMPLETE;
                }
            }
            return Loop::INCOMPLETE;
        };

    loopAllBuckets(scanBucket);
}

// Explicit template instantiations
template struct BucketListSnapshotData<LiveBucket>;
template struct BucketListSnapshotData<HotArchiveBucket>;
template class SearchableBucketListSnapshot<LiveBucket>;
template class SearchableBucketListSnapshot<HotArchiveBucket>;

} // namespace stellar
