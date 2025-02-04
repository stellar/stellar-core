// Copyright 2024 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/LiveBucket.h"
#include "bucket/BucketApplicator.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketUtils.h"
#include "bucket/LedgerCmp.h"
#include "ledger/LedgerTypeUtils.h"
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

void
LiveBucket::maybePut(LiveBucketOutputIterator& out, BucketEntry const& entry,
                     std::vector<LiveBucketInputIterator>& shadowIterators,
                     bool keepShadowedLifecycleEntries, MergeCounters& mc)
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
        out.put(entry);
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
    out.put(entry);
}

void
LiveBucket::mergeCasesWithEqualKeys(
    MergeCounters& mc, LiveBucketInputIterator& oi, LiveBucketInputIterator& ni,
    LiveBucketOutputIterator& out,
    std::vector<LiveBucketInputIterator>& shadowIterators,
    uint32_t protocolVersion, bool keepShadowedLifecycleEntries)
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

    BucketEntry const& oldEntry = *oi;
    BucketEntry const& newEntry = *ni;
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
        maybePut(out, newLive, shadowIterators, keepShadowedLifecycleEntries,
                 mc);
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
            maybePut(out, newInit, shadowIterators,
                     keepShadowedLifecycleEntries, mc);
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
        maybePut(out, newEntry, shadowIterators, keepShadowedLifecycleEntries,
                 mc);
    }
    ++oi;
    ++ni;
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
#endif // BUILD_TESTS

std::optional<std::pair<std::streamoff, std::streamoff>>
LiveBucket::getOfferRange() const
{
    return getIndex().getOfferRange();
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
                  bool countMergeEvents, asio::io_context& ctx, bool doFsync)
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
        bucketManager.incrMergeCounters(mc);
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
}

uint32_t
LiveBucket::getBucketVersion() const
{
    LiveBucketInputIterator it(shared_from_this());
    return it.getMetadata().ledgerVersion;
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
}