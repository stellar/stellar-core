// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketBase.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketMergeAdapter.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketUtils.h"
#include "bucket/LedgerCmp.h"
#include <medida/counter.h>

namespace stellar
{
namespace
{
void
countShadowedEntryType(MergeCounters& mc, BucketEntry const& e)
{
    switch (e.type())
    {
    case METAENTRY:
        ++mc.mMetaEntryShadowElisions;
        break;
    case INITENTRY:
        ++mc.mInitEntryShadowElisions;
        break;
    case LIVEENTRY:
        ++mc.mLiveEntryShadowElisions;
        break;
    case DEADENTRY:
        ++mc.mDeadEntryShadowElisions;
        break;
    }
}
}

void
LiveBucket::countNewEntryType(MergeCounters& mc, BucketEntry const& e)
{
    switch (e.type())
    {
    case METAENTRY:
        ++mc.mNewMetaEntries;
        break;
    case INITENTRY:
        ++mc.mNewInitEntries;
        break;
    case LIVEENTRY:
        ++mc.mNewLiveEntries;
        break;
    case DEADENTRY:
        ++mc.mNewDeadEntries;
        break;
    }
}
void
LiveBucket::countOldEntryType(MergeCounters& mc, BucketEntry const& e)
{
    switch (e.type())
    {
    case METAENTRY:
        ++mc.mOldMetaEntries;
        break;
    case INITENTRY:
        ++mc.mOldInitEntries;
        break;
    case LIVEENTRY:
        ++mc.mOldLiveEntries;
        break;
    case DEADENTRY:
        ++mc.mOldDeadEntries;
        break;
    }
}

bool
LiveBucket::updateMergeCountersForProtocolVersion(
    MergeCounters& mc, uint32_t protocolVersion,
    std::vector<LiveBucketInputIterator> const& shadowIterators)
{
    bool keepShadowedLifecycleEntries = true;

    if (protocolVersionIsBefore(
            protocolVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY))
    {
        ++mc.mPreInitEntryProtocolMerges;
        keepShadowedLifecycleEntries = false;
    }
    else
    {
        ++mc.mPostInitEntryProtocolMerges;
    }

    if (protocolVersionIsBefore(protocolVersion,
                                LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
    {
        ++mc.mPreShadowRemovalProtocolMerges;
    }
    else
    {
        if (!shadowIterators.empty())
        {
            throw std::runtime_error("Shadows are not supported");
        }
        ++mc.mPostShadowRemovalProtocolMerges;
    }

    return keepShadowedLifecycleEntries;
}

void
LiveBucket::maybePut(std::function<void(BucketEntry const&)> putFunc,
                     BucketEntry const& entry, MergeCounters& mc,
                     std::vector<LiveBucketInputIterator>& shadowIterators,
                     bool keepShadowedLifecycleEntries)
{
    // In ledgers before protocol 11, keepShadowedLifecycleEntries will be
    // `false` and we will drop all shadowed entries here.
    //
    // In ledgers at-or-after protocol 11, it will be `true` which means that we
    // only elide 'put'ing an entry if it is in LIVEENTRY state; we keep entries
    // in DEADENTRY and INITENTRY states, for two reasons:
    //
    //   - DEADENTRY is preserved to ensure that old live-or-init entries that
    //     were killed remain dead, are not brought back to life accidentally by
    //     having a newer shadow eliding their later DEADENTRY (tombstone). This
    //     is possible because newer shadowing entries may both refer to the
    //     same key as an older dead entry, and may occur as an INIT/DEAD pair
    //     that subsequently annihilate one another.
    //
    //     IOW we want to prevent the following scenario:
    //
    //       lev1:DEAD, lev2:INIT, lev3:DEAD, lev4:INIT
    //
    //     from turning into the following by shadowing:
    //
    //       lev1:DEAD, lev2:INIT, -elided-, lev4:INIT
    //
    //     and then the following by pairwise annihilation:
    //
    //       -annihilated-, -elided-, lev4:INIT
    //
    //   - INITENTRY is preserved to ensure that a DEADENTRY preserved by the
    //     previous rule does not itself shadow-out its own INITENTRY, but
    //     rather eventually ages and encounters (and is annihilated-by) that
    //     INITENTRY in an older level.  Thus preventing the accumulation of
    //     redundant tombstones.
    //
    // Note that this decision only controls whether to elide dead entries due
    // to _shadows_. There is a secondary elision of dead entries at the _oldest
    // level_ of the bucketlist that is accomplished through filtering at the
    // LiveBucketOutputIterator level, and happens independent of ledger
    // protocol version.

    if (keepShadowedLifecycleEntries &&
        (entry.type() == INITENTRY || entry.type() == DEADENTRY))
    {
        // Never shadow-out entries in this case; no point scanning shadows.
        putFunc(entry);
        return;
    }

    BucketEntryIdCmp<LiveBucket> cmp;
    for (auto& si : shadowIterators)
    {
        // Advance the shadowIterator while it's less than the candidate
        while (si && cmp(*si, entry))
        {
            ++mc.mShadowScanSteps;
            ++si;
        }
        // We have stepped si forward to the point that either si is exhausted,
        // or else *si >= entry; we now check the opposite direction to see if
        // we have equality.
        if (si && !cmp(entry, *si))
        {
            // If so, then entry is shadowed in at least one level.
            countShadowedEntryType(mc, entry);
            return;
        }
    }
    // Nothing shadowed.
    putFunc(entry);
}

template <typename InputSource>
void
LiveBucket::mergeCasesWithEqualKeys(
    MergeCounters& mc, InputSource& inputSource,
    std::function<void(BucketEntry const&)> putFunc, uint32_t protocolVersion,
    std::vector<LiveBucketInputIterator>& shadowIterators,
    bool keepShadowedLifecycleEntries)
{
    // Old and new are for the same key and neither is INIT, take the new
    // key. If either key is INIT, we have to make some adjustments:
    //
    //   old    |   new   |   result
    // ---------+---------+-----------
    //  INIT    |  INIT   |   error
    //  LIVE    |  INIT   |   error
    //  DEAD    |  INIT=x |   LIVE=x
    //  INIT=x  |  LIVE=y |   INIT=y
    //  INIT    |  DEAD   |   empty
    //
    //
    // What does this mean / why is it correct?
    //
    // Performing a merge between two same-key entries is about maintaining two
    // invariants:
    //
    //    1. From the perspective of a reader (eg. the database) the pre-merge
    //       pair of entries and post-merge single entry are indistinguishable,
    //       at least in terms that the reader/database cares about (liveness &
    //       value).  This is the most important invariant since it's what makes
    //       the database have the right values!
    //
    //    2. From the perspective of chronological _sequences_ of lifecycle
    //       transitions, if an entry is in INIT state then its (chronological)
    //       predecessor state is DEAD either by the next-oldest state being an
    //       _explicit_ DEAD tombstone, or by the INIT being the oldest state in
    //       the bucket list. This invariant allows us to assume that INIT
    //       followed by DEAD can be safely merged to empty (eliding the record)
    //       without revealing and reviving the key in some older non-DEAD state
    //       preceding the INIT.
    //
    // When merging a pair of non-INIT entries and taking the 'new' value,
    // invariant #1 is easy to see as preserved (an LSM tree is defined as
    // returning the newest value for an entry, so preserving the newest of any
    // pair is correct), and by assumption neither entry is INIT-state so
    // invariant #2 isn't relevant / is unaffected.
    //
    // When merging a pair with an INIT, we can go case-by-case through the
    // table above and see that both invariants are preserved:
    //
    //   - INIT,INIT and LIVE,INIT violate invariant #2, so by assumption should
    //     never be occurring.
    //
    //   - DEAD,INIT=x are indistinguishable from LIVE=x from the perspective of
    //     the reader, satisfying invariant #1. And since LIVE=x is not
    //     INIT-state anymore invariant #2 is trivially preserved (does not
    //     apply).
    //
    //   - INIT=x,LIVE=y is indistinguishable from INIT=y from the perspective
    //     of the reader, satisfying invariant #1.  And assuming invariant #2
    //     holds for INIT=x,LIVE=y, then it holds for INIT=y.
    //
    //   - INIT,DEAD is indistinguishable from absence-of-an-entry from the
    //     perspective of a reader, maintaining invariant #1, _if_ invariant #2
    //     also holds (the predecessor state _before_ INIT was
    //     absent-or-DEAD). And invariant #2 holds trivially _locally_ for this
    //     merge because there is no resulting state (i.e. it's not in
    //     INIT-state); and it holds slightly-less-trivially non-locally,
    //     because even if there is a subsequent (newer) INIT entry, the
    //     invariant is maintained for that newer entry too (it is still
    //     preceded by a DEAD state).

    BucketEntry const& oldEntry = inputSource.getOldEntry();
    BucketEntry const& newEntry = inputSource.getNewEntry();
    LiveBucket::checkProtocolLegality(oldEntry, protocolVersion);
    LiveBucket::checkProtocolLegality(newEntry, protocolVersion);
    countOldEntryType(mc, oldEntry);
    countNewEntryType(mc, newEntry);

    if (newEntry.type() == INITENTRY)
    {
        // The only legal new-is-INIT case is merging a delete+create to an
        // update.
        if (oldEntry.type() != DEADENTRY)
        {
            throw std::runtime_error(
                "Malformed bucket: old non-DEAD + new INIT.");
        }
        BucketEntry newLive;
        newLive.type(LIVEENTRY);
        newLive.liveEntry() = newEntry.liveEntry();
        ++mc.mNewInitEntriesMergedWithOldDead;
        maybePut(putFunc, newLive, mc, shadowIterators,
                 keepShadowedLifecycleEntries);
    }
    else if (oldEntry.type() == INITENTRY)
    {
        // If we get here, new is not INIT; may be LIVE or DEAD.
        if (newEntry.type() == LIVEENTRY)
        {
            // Merge a create+update to a fresher create.
            BucketEntry newInit;
            newInit.type(INITENTRY);
            newInit.liveEntry() = newEntry.liveEntry();
            ++mc.mOldInitEntriesMergedWithNewLive;
            maybePut(putFunc, newInit, mc, shadowIterators,
                     keepShadowedLifecycleEntries);
        }
        else
        {
            // Merge a create+delete to nothingness.
            ++mc.mOldInitEntriesMergedWithNewDead;
        }
    }
    else
    {
        // Neither is in INIT state, take the newer one.
        ++mc.mNewEntriesMergedWithOldNeitherInit;
        maybePut(putFunc, newEntry, mc, shadowIterators,
                 keepShadowedLifecycleEntries);
    }
    inputSource.advanceOld();
    inputSource.advanceNew();
}

bool
LiveBucket::containsBucketIdentity(BucketEntry const& id) const
{
    BucketEntryIdCmp<LiveBucket> cmp;
    LiveBucketInputIterator iter(shared_from_this());
    while (iter)
    {
        if (!(cmp(*iter, id) || cmp(id, *iter)))
        {
            return true;
        }
        ++iter;
    }
    return false;
}

size_t
LiveBucket::getIndexCacheSize() const
{
    if (mIndex)
    {
        return mIndex->getCurrentCacheSize();
    }
    return 0;
}

#ifdef BUILD_TESTS
void
LiveBucket::apply(Application& app) const
{
    ZoneScoped;
    std::unordered_set<LedgerKey> emptySet;
    BucketApplicator applicator(
        app, app.getConfig().LEDGER_PROTOCOL_VERSION,
        0 /*set to 0 so we always load from the parent to check state*/,
        0 /*set to a level that's not the bottom so we don't treat live entries
             as init*/
        ,
        shared_from_this(), emptySet);
    BucketApplicator::Counters counters(app.getClock().now());
    while (applicator)
    {
        applicator.advance(counters);
    }
    counters.logInfo("direct", 0, app.getClock().now());
}

size_t
LiveBucket::getMaxCacheSize() const
{
    if (!isEmpty())
    {
        return getIndex().getMaxCacheSize();
    }

    return 0;
}
#endif // BUILD_TESTS

std::optional<std::pair<std::streamoff, std::streamoff>>
LiveBucket::getRangeForType(LedgerEntryType type) const
{
    return getIndex().getRangeForType(type);
}

std::vector<BucketEntry>
LiveBucket::convertToBucketEntry(bool useInit,
                                 std::vector<LedgerEntry> const& initEntries,
                                 std::vector<LedgerEntry> const& liveEntries,
                                 std::vector<LedgerKey> const& deadEntries)
{
    std::vector<BucketEntry> bucket;
    for (auto const& e : initEntries)
    {
        BucketEntry ce;
        ce.type(useInit ? INITENTRY : LIVEENTRY);
        ce.liveEntry() = e;
        bucket.push_back(ce);
    }
    for (auto const& e : liveEntries)
    {
        BucketEntry ce;
        ce.type(LIVEENTRY);
        ce.liveEntry() = e;
        bucket.push_back(ce);
    }
    for (auto const& e : deadEntries)
    {
        BucketEntry ce;
        ce.type(DEADENTRY);
        ce.deadEntry() = e;
        bucket.push_back(ce);
    }

    BucketEntryIdCmp<LiveBucket> cmp;
    std::sort(bucket.begin(), bucket.end(), cmp);
    releaseAssert(std::adjacent_find(
                      bucket.begin(), bucket.end(),
                      [&cmp](BucketEntry const& lhs, BucketEntry const& rhs) {
                          return !cmp(lhs, rhs);
                      }) == bucket.end());
    return bucket;
}

std::shared_ptr<LiveBucket>
LiveBucket::fresh(BucketManager& bucketManager, uint32_t protocolVersion,
                  std::vector<LedgerEntry> const& initEntries,
                  std::vector<LedgerEntry> const& liveEntries,
                  std::vector<LedgerKey> const& deadEntries,
                  bool countMergeEvents, asio::io_context& ctx, bool doFsync,
                  bool storeInMemory, bool shouldIndex)
{
    ZoneScoped;
    // When building fresh buckets after protocol version 10 (i.e. version
    // 11-or-after) we differentiate INITENTRY from LIVEENTRY. In older
    // protocols, for compatibility sake, we mark both cases as LIVEENTRY.
    bool useInit = protocolVersionStartsFrom(
        protocolVersion, FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY);

    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;

    if (protocolVersionStartsFrom(
            protocolVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        meta.ext.v(1);
        meta.ext.bucketListType() = BucketListType::LIVE;
    }

    auto entries =
        convertToBucketEntry(useInit, initEntries, liveEntries, deadEntries);

    MergeCounters mc;
    LiveBucketOutputIterator out(bucketManager.getTmpDir(), true, meta, mc, ctx,
                                 doFsync);
    for (auto const& e : entries)
    {
        out.put(e);
    }

    if (countMergeEvents)
    {
        bucketManager.incrMergeCounters<LiveBucket>(mc);
    }

    if (storeInMemory)
    {
        return out.getBucket(bucketManager, nullptr, std::move(entries),
                             shouldIndex);
    }

    return out.getBucket(bucketManager);
}

void
LiveBucket::checkProtocolLegality(BucketEntry const& entry,
                                  uint32_t protocolVersion)
{
    if (protocolVersionIsBefore(
            protocolVersion,
            FIRST_PROTOCOL_SUPPORTING_INITENTRY_AND_METAENTRY) &&
        (entry.type() == INITENTRY || entry.type() == METAENTRY))
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("unsupported entry type {} in protocol {:d} bucket"),
            (entry.type() == INITENTRY ? "INIT" : "META"), protocolVersion));
    }
}

LiveBucket::LiveBucket(std::string const& filename, Hash const& hash,
                       std::unique_ptr<LiveBucket::IndexT const>&& index)
    : BucketBase(filename, hash, std::move(index))
{
}

LiveBucket::LiveBucket() : BucketBase()
{
    // Empty bucket is trivially stored in memory
    mEntries = std::vector<BucketEntry>();
}

uint32_t
LiveBucket::getBucketVersion() const
{
    LiveBucketInputIterator it(shared_from_this());
    return it.getMetadata().ledgerVersion;
}

void
LiveBucket::maybeInitializeCache(size_t totalBucketListAccountsSizeBytes,
                                 Config const& cfg) const
{
    releaseAssert(mIndex);
    mIndex->maybeInitializeCache(totalBucketListAccountsSizeBytes, cfg);
}

std::shared_ptr<LiveBucket>
LiveBucket::mergeInMemory(BucketManager& bucketManager,
                          uint32_t maxProtocolVersion,
                          std::shared_ptr<LiveBucket> const& oldBucket,
                          std::shared_ptr<LiveBucket> const& newBucket,
                          bool countMergeEvents, asio::io_context& ctx,
                          bool doFsync)
{
    ZoneScoped;
    releaseAssertOrThrow(oldBucket->hasInMemoryEntries());
    releaseAssertOrThrow(newBucket->hasInMemoryEntries());

    auto const& oldEntries = oldBucket->getInMemoryEntries();
    auto const& newEntries = newBucket->getInMemoryEntries();

    std::vector<BucketEntry> mergedEntries;
    mergedEntries.reserve(oldEntries.size() + newEntries.size());

    // Prepare metadata for the merged bucket
    BucketMetadata meta;
    meta.ledgerVersion = maxProtocolVersion;
    if (protocolVersionStartsFrom(
            maxProtocolVersion,
            LiveBucket::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
    {
        meta.ext.v(1);
        meta.ext.bucketListType() = BucketListType::LIVE;
    }

    // First level never has shadows
    std::vector<LiveBucketInputIterator> shadowIterators{};
    MergeCounters mc;
    bool keepShadowedLifecycleEntries = updateMergeCountersForProtocolVersion(
        mc, maxProtocolVersion, shadowIterators);

    MemoryMergeInput<LiveBucket> inputSource(oldEntries, newEntries);
    std::function<void(BucketEntry const&)> putFunc =
        [&mergedEntries](BucketEntry const& entry) {
            mergedEntries.emplace_back(entry);
        };

    mergeInternal(bucketManager, inputSource, putFunc, maxProtocolVersion, mc,
                  shadowIterators, keepShadowedLifecycleEntries);

    if (countMergeEvents)
    {
        bucketManager.incrMergeCounters<LiveBucket>(mc);
    }

    // Write merge output to a bucket and save to disk
    LiveBucketOutputIterator out(bucketManager.getTmpDir(),
                                 /*keepTombstoneEntries=*/true, meta, mc, ctx,
                                 doFsync);

    for (auto const& e : mergedEntries)
    {
        out.put(e);
    }

    // Store the merged entries in memory in the new bucket in case this
    // bucket sees another incoming merge as level 0 curr.
    return out.getBucket(bucketManager, nullptr, std::move(mergedEntries));
}

BucketEntryCounters const&
LiveBucket::getBucketEntryCounters() const
{
    releaseAssert(mIndex);
    return mIndex->getBucketEntryCounters();
}

bool
LiveBucket::isTombstoneEntry(BucketEntry const& e)
{
    return e.type() == DEADENTRY;
}

std::shared_ptr<LiveBucket::LoadT const>
LiveBucket::bucketEntryToLoadResult(std::shared_ptr<EntryT const> const& be)
{
    return isTombstoneEntry(*be)
               ? nullptr
               : std::make_shared<LedgerEntry>(be->liveEntry());
}

template void LiveBucket::mergeCasesWithEqualKeys<FileMergeInput<LiveBucket>>(
    MergeCounters& mc, FileMergeInput<LiveBucket>& inputSource,
    std::function<void(BucketEntry const&)> putFunc, uint32_t protocolVersion,
    std::vector<LiveBucketInputIterator>& shadowIterators,
    bool keepShadowedLifecycleEntries);

template void LiveBucket::mergeCasesWithEqualKeys<MemoryMergeInput<LiveBucket>>(
    MergeCounters& mc, MemoryMergeInput<LiveBucket>& inputSource,
    std::function<void(BucketEntry const&)> putFunc, uint32_t protocolVersion,
    std::vector<LiveBucketInputIterator>& shadowIterators,
    bool keepShadowedLifecycleEntries);
}