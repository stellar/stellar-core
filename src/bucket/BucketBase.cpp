// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h" // IWYU pragma: keep
#include "bucket/BucketBase.h"
#include "bucket/BucketInputIterator.h"
#include "bucket/BucketManager.h"
#include "bucket/BucketOutputIterator.h"
#include "bucket/BucketUtils.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LedgerCmp.h"
#include "bucket/LiveBucket.h"
#include "bucket/MergeKey.h"
#include "crypto/Hex.h"
#include "crypto/Random.h"
#include "main/Application.h"
#include "medida/timer.h"
#include "util/Fs.h"
#include "util/GlobalChecks.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/types.h"
#include <Tracy.hpp>

namespace stellar
{

template <class BucketT, class IndexT>
IndexT const&
BucketBase<BucketT, IndexT>::getIndex() const
{
    ZoneScoped;
    releaseAssertOrThrow(!mFilename.empty());
    releaseAssertOrThrow(mIndex);
    return *mIndex;
}

template <class BucketT, class IndexT>
bool
BucketBase<BucketT, IndexT>::isIndexed() const
{
    return static_cast<bool>(mIndex);
}

template <class BucketT, class IndexT>
void
BucketBase<BucketT, IndexT>::setIndex(std::unique_ptr<IndexT const>&& index)
{
    releaseAssertOrThrow(!mIndex);
    mIndex = std::move(index);
}

template <class BucketT, class IndexT>
BucketBase<BucketT, IndexT>::BucketBase(std::string const& filename,
                                        Hash const& hash,
                                        std::unique_ptr<IndexT const>&& index)
    : mFilename(filename), mHash(hash), mIndex(std::move(index))
{
    releaseAssert(filename.empty() || fs::exists(filename));
    if (!filename.empty())
    {
        CLOG_TRACE(Bucket, "BucketBase::Bucket() created, file exists : {}",
                   mFilename);
        mSize = fs::size(filename);
    }
}

template <class BucketT, class IndexT> BucketBase<BucketT, IndexT>::BucketBase()
{
}

template <class BucketT, class IndexT>
Hash const&
BucketBase<BucketT, IndexT>::getHash() const
{
    return mHash;
}

template <class BucketT, class IndexT>
std::filesystem::path const&
BucketBase<BucketT, IndexT>::getFilename() const
{
    return mFilename;
}

template <class BucketT, class IndexT>
size_t
BucketBase<BucketT, IndexT>::getSize() const
{
    return mSize;
}

template <class BucketT, class IndexT>
bool
BucketBase<BucketT, IndexT>::isEmpty() const
{
    if (mFilename.empty() || isZero(mHash))
    {
        releaseAssertOrThrow(mFilename.empty() && isZero(mHash));
        return true;
    }

    return false;
}

template <class BucketT, class IndexT>
void
BucketBase<BucketT, IndexT>::freeIndex()
{
    mIndex.reset(nullptr);
}

template <class BucketT, class IndexT>
std::string
BucketBase<BucketT, IndexT>::randomFileName(std::string const& tmpDir,
                                            std::string ext)
{
    ZoneScoped;
    for (;;)
    {
        std::string name =
            tmpDir + "/tmp-bucket-" + binToHex(randomBytes(8)) + ext;
        std::ifstream ifile(name);
        if (!ifile)
        {
            return name;
        }
    }
}

template <class BucketT, class IndexT>
std::string
BucketBase<BucketT, IndexT>::randomBucketName(std::string const& tmpDir)
{
    return randomFileName(tmpDir, ".xdr");
}

template <class BucketT, class IndexT>
std::string
BucketBase<BucketT, IndexT>::randomBucketIndexName(std::string const& tmpDir)
{
    return randomFileName(tmpDir, ".index");
}

// The protocol used in a merge is the maximum of any of the protocols used in
// its input buckets, _including_ any of its shadows. We need to be strict about
// this for the same reason we change shadow algorithms along with merge
// algorithms: because once _any_ newer bucket levels have cut-over to merging
// with the new INITENTRY-supporting merge algorithm, there may be "INIT + DEAD
// => nothing" mutual annihilations occurring, which can "revive" the state of
// an entry on older levels. It's imperative then that older levels'
// lifecycle-event-pairing structure be preserved -- that the state-before INIT
// is in fact DEAD or nonexistent -- from the instant we begin using the new
// merge protocol: that the old lifecycle-event-eliding shadowing behaviour be
// disabled, and we switch to the more conservative shadowing behaviour that
// preserves lifecycle-events.
//
//     IOW we want to prevent the following scenario
//     (assuming lev1 and lev2 are on the new protocol, but 3 and 4
//      are on the old protocol):
//
//       lev1:DEAD, lev2:INIT, lev3:DEAD, lev4:LIVE
//
//     from turning into the following by shadowing
//     (using the old shadow algorithm on a lev3 merge):
//
//       lev1:DEAD, lev2:INIT, -elided-, lev4:LIVE
//
//     and then the following by pairwise annihilation
//     (using the new merge algorithm on new lev1 and lev2):
//
//       -annihilated-, -elided-, lev4:LIVE
//
// To prevent this, we cut over _all_ levels of the bucket list to the new merge
// and shadowing protocol simultaneously, the moment the first new-protocol
// bucket enters the youngest level. At least one new bucket is in every merge's
// shadows from then on in, so they all upgrade (and preserve lifecycle events).
template <class BucketT, class IndexT>
static void
calculateMergeProtocolVersion(
    MergeCounters& mc, uint32_t maxProtocolVersion,
    BucketInputIterator<BucketT> const& oi,
    BucketInputIterator<BucketT> const& ni,
    std::vector<BucketInputIterator<BucketT>> const& shadowIterators,
    uint32& protocolVersion, bool& keepShadowedLifecycleEntries)
{
    protocolVersion = std::max(oi.getMetadata().ledgerVersion,
                               ni.getMetadata().ledgerVersion);

    // Starting with FIRST_PROTOCOL_SHADOWS_REMOVED,
    // protocol version is determined as a max of curr, snap, and any shadow of
    // version < FIRST_PROTOCOL_SHADOWS_REMOVED. This means that a bucket may
    // still perform an old style merge despite the presence of the new protocol
    // shadows.
    for (auto const& si : shadowIterators)
    {
        auto version = si.getMetadata().ledgerVersion;
        if (protocolVersionIsBefore(version,
                                    LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
        {
            protocolVersion = std::max(version, protocolVersion);
        }
    }

    CLOG_TRACE(Bucket, "Bucket merge protocolVersion={}, maxProtocolVersion={}",
               protocolVersion, maxProtocolVersion);

    if (protocolVersion > maxProtocolVersion)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING(
                "bucket protocol version {:d} exceeds maxProtocolVersion {:d}"),
            protocolVersion, maxProtocolVersion));
    }

    // When merging buckets after protocol version 10 (i.e. version 11-or-after)
    // we switch shadowing-behaviour to a more conservative mode, in order to
    // support annihilation of INITENTRY and DEADENTRY pairs. See commentary
    // above in `maybePut`.
    keepShadowedLifecycleEntries = true;

    // Don't count shadow metrics for Hot Archive BucketList
    if constexpr (std::is_same_v<BucketT, HotArchiveBucket>)
    {
        return;
    }

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
}

// There are 4 "easy" cases for merging: exhausted iterators on either
// side, or entries that compare non-equal. In all these cases we just
// take the lesser (or existing) entry and advance only one iterator,
// not scrutinizing the entry type further.
template <class BucketT, class IndexT>
static bool
mergeCasesWithDefaultAcceptance(
    BucketEntryIdCmp<BucketT> const& cmp, MergeCounters& mc,
    BucketInputIterator<BucketT>& oi, BucketInputIterator<BucketT>& ni,
    BucketOutputIterator<BucketT>& out,
    std::vector<BucketInputIterator<BucketT>>& shadowIterators,
    uint32_t protocolVersion, bool keepShadowedLifecycleEntries)
{
    BUCKET_TYPE_ASSERT(BucketT);

    if (!ni || (oi && ni && cmp(*oi, *ni)))
    {
        // Either of:
        //
        //   - Out of new entries.
        //   - Old entry has smaller key.
        //
        // In both cases: take old entry.
        ++mc.mOldEntriesDefaultAccepted;
        BucketT::checkProtocolLegality(*oi, protocolVersion);
        BucketT::countOldEntryType(mc, *oi);
        BucketT::maybePut(out, *oi, shadowIterators,
                          keepShadowedLifecycleEntries, mc);
        ++oi;
        return true;
    }
    else if (!oi || (oi && ni && cmp(*ni, *oi)))
    {
        // Either of:
        //
        //   - Out of old entries.
        //   - New entry has smaller key.
        //
        // In both cases: take new entry.
        ++mc.mNewEntriesDefaultAccepted;
        BucketT::checkProtocolLegality(*ni, protocolVersion);
        BucketT::countNewEntryType(mc, *ni);
        BucketT::maybePut(out, *ni, shadowIterators,
                          keepShadowedLifecycleEntries, mc);
        ++ni;
        return true;
    }
    return false;
}

template <class BucketT, class IndexT>
std::shared_ptr<BucketT>
BucketBase<BucketT, IndexT>::merge(
    BucketManager& bucketManager, uint32_t maxProtocolVersion,
    std::shared_ptr<BucketT> const& oldBucket,
    std::shared_ptr<BucketT> const& newBucket,
    std::vector<std::shared_ptr<BucketT>> const& shadows,
    bool keepTombstoneEntries, bool countMergeEvents, asio::io_context& ctx,
    bool doFsync)
{
    BUCKET_TYPE_ASSERT(BucketT);

    ZoneScoped;
    // This is the key operation in the scheme: merging two (read-only)
    // buckets together into a new 3rd bucket, while calculating its hash,
    // in a single pass.

    releaseAssert(oldBucket);
    releaseAssert(newBucket);

    MergeCounters mc;
    BucketInputIterator<BucketT> oi(oldBucket);
    BucketInputIterator<BucketT> ni(newBucket);
    std::vector<BucketInputIterator<BucketT>> shadowIterators(shadows.begin(),
                                                              shadows.end());

    uint32_t protocolVersion;
    bool keepShadowedLifecycleEntries;
    calculateMergeProtocolVersion<BucketT, IndexT>(
        mc, maxProtocolVersion, oi, ni, shadowIterators, protocolVersion,
        keepShadowedLifecycleEntries);

    auto timer = bucketManager.getMergeTimer().TimeScope();
    BucketMetadata meta;
    meta.ledgerVersion = protocolVersion;

    // If any inputs use the new extension of BucketMeta, the output should as
    // well
    if (ni.getMetadata().ext.v() == 1)
    {
        releaseAssertOrThrow(protocolVersionStartsFrom(
            maxProtocolVersion,
            BucketT::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));
        meta.ext = ni.getMetadata().ext;
    }
    else if (oi.getMetadata().ext.v() == 1)
    {
        releaseAssertOrThrow(protocolVersionStartsFrom(
            maxProtocolVersion,
            BucketT::FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION));
        meta.ext = oi.getMetadata().ext;
    }

    BucketOutputIterator<BucketT> out(bucketManager.getTmpDir(),
                                      keepTombstoneEntries, meta, mc, ctx,
                                      doFsync);

    BucketEntryIdCmp<BucketT> cmp;
    size_t iter = 0;

    while (oi || ni)
    {
        // Check if the merge should be stopped every few entries
        if (++iter >= 1000)
        {
            iter = 0;
            if (bucketManager.isShutdown())
            {
                // Stop merging, as BucketManager is now shutdown
                // This is safe as temp file has not been adopted yet,
                // so it will be removed with the tmp dir
                throw std::runtime_error(
                    "Incomplete bucket merge due to BucketManager shutdown");
            }
        }

        if (!mergeCasesWithDefaultAcceptance<BucketT, IndexT>(
                cmp, mc, oi, ni, out, shadowIterators, protocolVersion,
                keepShadowedLifecycleEntries))
        {
            BucketT::mergeCasesWithEqualKeys(mc, oi, ni, out, shadowIterators,
                                             protocolVersion,
                                             keepShadowedLifecycleEntries);
        }
    }
    if (countMergeEvents)
    {
        bucketManager.incrMergeCounters(mc);
    }

    std::vector<Hash> shadowHashes;
    shadowHashes.reserve(shadows.size());
    for (auto const& s : shadows)
    {
        shadowHashes.push_back(s->getHash());
    }

    MergeKey mk{keepTombstoneEntries, oldBucket->getHash(),
                newBucket->getHash(), shadowHashes};
    return out.getBucket(bucketManager, &mk);
}

template class BucketBase<LiveBucket, LiveBucket::IndexT>;
template class BucketBase<HotArchiveBucket, HotArchiveBucket::IndexT>;
}