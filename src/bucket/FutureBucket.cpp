// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h" // IWYU pragma: keep

#include "bucket/BucketListBase.h"
#include "bucket/BucketManager.h"
#include "bucket/FutureBucket.h"
#include "bucket/HotArchiveBucket.h"
#include "bucket/LiveBucket.h"
#include "bucket/MergeKey.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/GlobalChecks.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/ProtocolVersion.h"
#include "util/Thread.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/metrics_registry.h"

#include <chrono>
#include <memory>
#include <type_traits>

namespace stellar
{
template <class BucketT>
FutureBucket<BucketT>::FutureBucket(
    Application& app, std::shared_ptr<BucketT> const& curr,
    std::shared_ptr<BucketT> const& snap,
    std::vector<std::shared_ptr<BucketT>> const& shadows,
    uint32_t maxProtocolVersion, bool countMergeEvents, uint32_t level)
    : mState(FB_LIVE_INPUTS)
    , mInputCurrBucket(curr)
    , mInputSnapBucket(snap)
    , mInputShadowBuckets(shadows)
{
    ZoneScoped;
    // Constructed with a bunch of inputs, _immediately_ commence merging
    // them; there's no valid state for have-inputs-but-not-merging, the
    // presence of inputs implies merging, and vice-versa.
    releaseAssert(curr);
    releaseAssert(snap);
    mInputCurrBucketHash = binToHex(curr->getHash());
    mInputSnapBucketHash = binToHex(snap->getHash());
    if (protocolVersionStartsFrom(snap->getBucketVersion(),
                                  LiveBucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
    {
        if (!mInputShadowBuckets.empty())
        {
            throw std::runtime_error(
                "Invalid FutureBucket: ledger version doesn't support shadows");
        }
    }

    if constexpr (!std::is_same_v<BucketT, LiveBucket>)
    {
        if (!snap->isEmpty() &&
            protocolVersionIsBefore(
                snap->getBucketVersion(),
                HotArchiveBucket::
                    FIRST_PROTOCOL_SUPPORTING_PERSISTENT_EVICTION))
        {
            throw std::runtime_error(
                "Invalid ArchivalFutureBucket: ledger version doesn't support "
                "Archival BucketList");
        }
    }

    for (auto const& b : mInputShadowBuckets)
    {
        mInputShadowBucketHashes.push_back(binToHex(b->getHash()));
    }
    startMerge(app, maxProtocolVersion, countMergeEvents, level);
}

template <class BucketT>
void
FutureBucket<BucketT>::setLiveOutput(std::shared_ptr<BucketT> output)
{
    ZoneScoped;
    mState = FB_LIVE_OUTPUT;
    mOutputBucketHash = binToHex(output->getHash());
    mOutputBucket = output;
    checkState();
}

template <class BucketT>
static void
checkHashEq(std::shared_ptr<BucketT> const& b, std::string const& h)
{
    releaseAssert(b->getHash() == hexToBin256(h));
}

template <class BucketT>
void
FutureBucket<BucketT>::checkHashesMatch() const
{
    ZoneScoped;
    if (!mInputShadowBuckets.empty())
    {
        releaseAssert(mInputShadowBuckets.size() ==
                      mInputShadowBucketHashes.size());
        for (size_t i = 0; i < mInputShadowBuckets.size(); ++i)
        {
            checkHashEq(mInputShadowBuckets.at(i),
                        mInputShadowBucketHashes.at(i));
        }
    }
    if (mInputSnapBucket)
    {
        checkHashEq(mInputSnapBucket, mInputSnapBucketHash);
    }
    if (mInputCurrBucket)
    {
        checkHashEq(mInputCurrBucket, mInputCurrBucketHash);
    }
    if (mergeComplete() && !mOutputBucketHash.empty())
    {
        checkHashEq(mOutputBucket, mOutputBucketHash);
    }
}

/**
 * Note: not all combinations of state and values are valid; we want to limit
 * states to only those useful for the lifecycles in the program. In particular,
 * the different hash-only states are mutually exclusive with each other and
 * with live values.
 */
template <class BucketT>
void
FutureBucket<BucketT>::checkState() const
{
    switch (mState)
    {
    case FB_CLEAR:
        releaseAssert(mInputShadowBuckets.empty());
        releaseAssert(!mInputSnapBucket);
        releaseAssert(!mInputCurrBucket);
        releaseAssert(!mOutputBucket);
        releaseAssert(!mOutputBucketFuture.valid());
        releaseAssert(mInputShadowBucketHashes.empty());
        releaseAssert(mInputSnapBucketHash.empty());
        releaseAssert(mInputCurrBucketHash.empty());
        releaseAssert(mOutputBucketHash.empty());
        break;

    case FB_LIVE_INPUTS:
        releaseAssert(mInputSnapBucket);
        releaseAssert(mInputCurrBucket);
        releaseAssert(mOutputBucketFuture.valid());
        releaseAssert(!mOutputBucket);
        releaseAssert(mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_LIVE_OUTPUT:
        releaseAssert(mergeComplete());
        releaseAssert(mOutputBucket);
        releaseAssert(!mOutputBucketFuture.valid());
        releaseAssert(!mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_HASH_INPUTS:
        releaseAssert(!mInputSnapBucket);
        releaseAssert(!mInputCurrBucket);
        releaseAssert(!mOutputBucket);
        releaseAssert(!mOutputBucketFuture.valid());
        releaseAssert(!mInputSnapBucketHash.empty());
        releaseAssert(!mInputCurrBucketHash.empty());
        releaseAssert(mOutputBucketHash.empty());
        break;

    case FB_HASH_OUTPUT:
        releaseAssert(!mInputSnapBucket);
        releaseAssert(!mInputCurrBucket);
        releaseAssert(!mOutputBucket);
        releaseAssert(!mOutputBucketFuture.valid());
        releaseAssert(mInputSnapBucketHash.empty());
        releaseAssert(mInputCurrBucketHash.empty());
        releaseAssert(!mOutputBucketHash.empty());
        break;

    default:
        releaseAssert(false);
        break;
    }
}

template <class BucketT>
void
FutureBucket<BucketT>::clearInputs()
{
    mInputShadowBuckets.clear();
    mInputSnapBucket.reset();
    mInputCurrBucket.reset();

    mInputShadowBucketHashes.clear();
    mInputSnapBucketHash.clear();
    mInputCurrBucketHash.clear();
}

template <class BucketT>
void
FutureBucket<BucketT>::clearOutput()
{
    // NB: MSVC future<> implementation doesn't purge the task lambda (and
    // its captures) on invalidation (due to get()); must explicitly reset.
    mOutputBucketFuture = std::shared_future<std::shared_ptr<BucketT>>();
    mOutputBucketHash.clear();
    mOutputBucket.reset();
}

template <class BucketT>
void
FutureBucket<BucketT>::clear()
{
    mState = FB_CLEAR;
    clearInputs();
    clearOutput();
}

template <class BucketT>
bool
FutureBucket<BucketT>::isLive() const
{
    return (mState == FB_LIVE_INPUTS || mState == FB_LIVE_OUTPUT);
}

template <class BucketT>
bool
FutureBucket<BucketT>::isMerging() const
{
    return mState == FB_LIVE_INPUTS;
}

template <class BucketT>
bool
FutureBucket<BucketT>::hasHashes() const
{
    return (mState == FB_HASH_INPUTS || mState == FB_HASH_OUTPUT);
}

template <class BucketT>
bool
FutureBucket<BucketT>::isClear() const
{
    return mState == FB_CLEAR;
}

template <class BucketT>
bool
FutureBucket<BucketT>::mergeComplete() const
{
    ZoneScoped;
    releaseAssert(isLive());
    if (mOutputBucket)
    {
        return true;
    }

    return futureIsReady(mOutputBucketFuture);
}

template <class BucketT>
std::shared_ptr<BucketT>
FutureBucket<BucketT>::resolve()
{
    ZoneScoped;
    checkState();
    releaseAssert(isLive());

    if (mState == FB_LIVE_OUTPUT)
    {
        return mOutputBucket;
    }

    clearInputs();

    if (!mOutputBucket)
    {
        auto timer = LogSlowExecution("Resolving bucket");
        mOutputBucket = mOutputBucketFuture.get();
        mOutputBucketHash = binToHex(mOutputBucket->getHash());

        // Explicitly reset shared_future to ensure destruction of shared state.
        // Some compilers store packaged_task lambdas in the shared state,
        // keeping its captures alive as long as the future is alive.
        mOutputBucketFuture = std::shared_future<std::shared_ptr<BucketT>>();
    }

    mState = FB_LIVE_OUTPUT;
    checkState();
    return mOutputBucket;
}

template <class BucketT>
bool
FutureBucket<BucketT>::hasOutputHash() const
{
    if (mState == FB_LIVE_OUTPUT || mState == FB_HASH_OUTPUT)
    {
        releaseAssert(!mOutputBucketHash.empty());
        return true;
    }
    return false;
}

template <class BucketT>
std::string const&
FutureBucket<BucketT>::getOutputHash() const
{
    releaseAssert(mState == FB_LIVE_OUTPUT || mState == FB_HASH_OUTPUT);
    releaseAssert(!mOutputBucketHash.empty());
    return mOutputBucketHash;
}

template <class BucketT>
static std::chrono::seconds
getAvailableTimeForMerge(Application& app, uint32_t level)
{
    auto closeTime = app.getConfig().getExpectedLedgerCloseTime();
    if (level >= 1)
    {
        return closeTime * BucketListBase<BucketT>::levelHalf(level - 1);
    }
    return closeTime;
}

template <class BucketT>
void
FutureBucket<BucketT>::startMerge(Application& app, uint32_t maxProtocolVersion,
                                  bool countMergeEvents, uint32_t level)
{
    ZoneScoped;
    // NB: startMerge starts with FutureBucket in a half-valid state; the inputs
    // are live but the merge is not yet running. So you can't call checkState()
    // on entry, only on exit.

    releaseAssert(mState == FB_LIVE_INPUTS);

    std::shared_ptr<BucketT> curr = mInputCurrBucket;
    std::shared_ptr<BucketT> snap = mInputSnapBucket;
    std::vector<std::shared_ptr<BucketT>> shadows = mInputShadowBuckets;

    releaseAssert(curr);
    releaseAssert(snap);
    releaseAssert(!mOutputBucketFuture.valid());
    releaseAssert(!mOutputBucket);

    CLOG_TRACE(Bucket, "Preparing merge of curr={} with snap={}",
               hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

    BucketManager& bm = app.getBucketManager();
    auto& timer = app.getMetrics().NewTimer(
        {"bucket", "merge-time", "level-" + std::to_string(level)});

    std::vector<Hash> shadowHashes;
    shadowHashes.reserve(shadows.size());
    for (auto const& b : shadows)
    {
        shadowHashes.emplace_back(b->getHash());
    }

    // It's possible we're running a merge that's already running, for example
    // due to having been serialized to the publish queue and then immediately
    // deserialized. In this case we want to attach to the existing merge, which
    // will have left a std::shared_future behind in a shared cache in the
    // bucket manager.
    MergeKey mk{BucketListBase<BucketT>::keepTombstoneEntries(level),
                curr->getHash(), snap->getHash(), shadowHashes};

    std::shared_future<std::shared_ptr<BucketT>> f;
    f = bm.getMergeFuture<BucketT>(mk);

    if (f.valid())
    {
        CLOG_TRACE(Bucket,
                   "Re-attached to existing merge of curr={} with snap={}",
                   hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));
        mOutputBucketFuture = f;
        checkState();
        return;
    }
    asio::io_context& ctx = app.getWorkerIOContext();
    bool doFsync = !app.getConfig().DISABLE_XDR_FSYNC;
    std::chrono::seconds availableTime =
        getAvailableTimeForMerge<BucketT>(app, level);

    using task_t = std::packaged_task<std::shared_ptr<BucketT>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap, &bm, shadows, maxProtocolVersion, countMergeEvents, level,
         &timer, &ctx, doFsync, availableTime]() mutable {
            auto timeScope = timer.TimeScope();
            CLOG_TRACE(Bucket, "Worker merging curr={} with snap={}",
                       hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

            try
            {
                ZoneNamedN(mergeZone, "Merge task", true);
                ZoneValueV(mergeZone, static_cast<int64_t>(level));

                auto res = BucketT::merge(
                    bm, maxProtocolVersion, curr, snap, shadows,
                    BucketListBase<BucketT>::keepTombstoneEntries(level),
                    countMergeEvents, ctx, doFsync);

                if (res)
                {
                    CLOG_TRACE(
                        Bucket, "Worker finished merging curr={} with snap={}",
                        hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

                    std::chrono::duration<double> time(timeScope.Stop());
                    double timePct = time.count() / availableTime.count() * 100;
                    CLOG_DEBUG(
                        Perf,
                        "Bucket merge on level {} finished in {} seconds "
                        "({}% of available time)",
                        level, time.count(), timePct);
                }

                return res;
            }
            catch (std::exception const& e)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING("Error merging bucket curr={} with snap={}: "
                               "{}. {}"),
                    hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()),
                    e.what(), POSSIBLY_CORRUPTED_LOCAL_FS));
            };
        });

    mOutputBucketFuture = task->get_future().share();
    bm.putMergeFuture(mk, mOutputBucketFuture);
    app.postOnBackgroundThread(bind(&task_t::operator(), task),
                               "FutureBucket: merge");
    checkState();
}

template <class BucketT>
void
FutureBucket<BucketT>::makeLive(Application& app, uint32_t maxProtocolVersion,
                                uint32_t level)
{
    ZoneScoped;
    checkState();
    releaseAssert(!isLive());
    releaseAssert(hasHashes());
    auto& bm = app.getBucketManager();
    if (hasOutputHash())
    {
        auto b = bm.getBucketByHash<BucketT>(hexToBin256(getOutputHash()));

        setLiveOutput(b);
    }
    else
    {
        releaseAssert(mState == FB_HASH_INPUTS);
        mInputCurrBucket =
            bm.getBucketByHash<BucketT>(hexToBin256(mInputCurrBucketHash));
        mInputSnapBucket =
            bm.getBucketByHash<BucketT>(hexToBin256(mInputSnapBucketHash));

        releaseAssert(mInputShadowBuckets.empty());
        for (auto const& h : mInputShadowBucketHashes)
        {
            auto b = bm.getBucketByHash<BucketT>(hexToBin256(h));

            releaseAssert(b);
            CLOG_DEBUG(Bucket, "Reconstituting shadow {}", h);
            mInputShadowBuckets.push_back(b);
        }
        mState = FB_LIVE_INPUTS;
        startMerge(app, maxProtocolVersion, /*countMergeEvents=*/true, level);
        releaseAssert(isLive());
    }
}

template <class BucketT>
std::vector<std::string>
FutureBucket<BucketT>::getHashes() const
{
    ZoneScoped;
    std::vector<std::string> hashes;
    if (!mInputCurrBucketHash.empty())
    {
        hashes.push_back(mInputCurrBucketHash);
    }
    if (!mInputSnapBucketHash.empty())
    {
        hashes.push_back(mInputSnapBucketHash);
    }
    for (auto h : mInputShadowBucketHashes)
    {
        hashes.push_back(h);
    }
    if (!mOutputBucketHash.empty())
    {
        hashes.push_back(mOutputBucketHash);
    }
    return hashes;
}

template class FutureBucket<LiveBucket>;
template class FutureBucket<HotArchiveBucket>;
}
