// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "bucket/Bucket.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "bucket/FutureBucket.h"
#include "bucket/MergeKey.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/LogSlowExecution.h"
#include "util/Logging.h"
#include "util/Thread.h"
#include <Tracy.hpp>
#include <fmt/format.h>

#include "medida/metrics_registry.h"

#include <chrono>

namespace stellar
{

FutureBucket::FutureBucket(Application& app,
                           std::shared_ptr<Bucket> const& curr,
                           std::shared_ptr<Bucket> const& snap,
                           std::vector<std::shared_ptr<Bucket>> const& shadows,
                           uint32_t maxProtocolVersion, bool countMergeEvents,
                           uint32_t level)
    : mState(FB_LIVE_INPUTS)
    , mInputCurrBucket(curr)
    , mInputSnapBucket(snap)
    , mInputShadowBuckets(shadows)
{
    ZoneScoped;
    // Constructed with a bunch of inputs, _immediately_ commence merging
    // them; there's no valid state for have-inputs-but-not-merging, the
    // presence of inputs implies merging, and vice-versa.
    assert(curr);
    assert(snap);
    mInputCurrBucketHash = binToHex(curr->getHash());
    mInputSnapBucketHash = binToHex(snap->getHash());
    if (Bucket::getBucketVersion(snap) >=
        Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED)
    {
        if (!mInputShadowBuckets.empty())
        {
            throw std::runtime_error(
                "Invalid FutureBucket: ledger version doesn't support shadows");
        }
    }
    for (auto const& b : mInputShadowBuckets)
    {
        mInputShadowBucketHashes.push_back(binToHex(b->getHash()));
    }
    startMerge(app, maxProtocolVersion, countMergeEvents, level);
}

void
FutureBucket::setLiveOutput(std::shared_ptr<Bucket> output)
{
    ZoneScoped;
    mState = FB_LIVE_OUTPUT;
    mOutputBucketHash = binToHex(output->getHash());
    mOutputBucket = output;
    checkState();
}

static void
checkHashEq(std::shared_ptr<Bucket> const& b, std::string const& h)
{
    assert(b->getHash() == hexToBin256(h));
}

void
FutureBucket::checkHashesMatch() const
{
    ZoneScoped;
    if (!mInputShadowBuckets.empty())
    {
        assert(mInputShadowBuckets.size() == mInputShadowBucketHashes.size());
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
void
FutureBucket::checkState() const
{
    switch (mState)
    {
    case FB_CLEAR:
        assert(mInputShadowBuckets.empty());
        assert(!mInputSnapBucket);
        assert(!mInputCurrBucket);
        assert(!mOutputBucket);
        assert(!mOutputBucketFuture.valid());
        assert(mInputShadowBucketHashes.empty());
        assert(mInputSnapBucketHash.empty());
        assert(mInputCurrBucketHash.empty());
        assert(mOutputBucketHash.empty());
        break;

    case FB_LIVE_INPUTS:
        assert(mInputSnapBucket);
        assert(mInputCurrBucket);
        assert(mOutputBucketFuture.valid());
        assert(!mOutputBucket);
        assert(mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_LIVE_OUTPUT:
        assert(mergeComplete());
        assert(mOutputBucket);
        assert(!mOutputBucketFuture.valid());
        assert(!mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_HASH_INPUTS:
        assert(!mInputSnapBucket);
        assert(!mInputCurrBucket);
        assert(!mOutputBucket);
        assert(!mOutputBucketFuture.valid());
        assert(!mInputSnapBucketHash.empty());
        assert(!mInputCurrBucketHash.empty());
        assert(mOutputBucketHash.empty());
        break;

    case FB_HASH_OUTPUT:
        assert(!mInputSnapBucket);
        assert(!mInputCurrBucket);
        assert(!mOutputBucket);
        assert(!mOutputBucketFuture.valid());
        assert(mInputSnapBucketHash.empty());
        assert(mInputCurrBucketHash.empty());
        assert(!mOutputBucketHash.empty());
        break;

    default:
        assert(false);
        break;
    }
}

void
FutureBucket::clearInputs()
{
    mInputShadowBuckets.clear();
    mInputSnapBucket.reset();
    mInputCurrBucket.reset();

    mInputShadowBucketHashes.clear();
    mInputSnapBucketHash.clear();
    mInputCurrBucketHash.clear();
}

void
FutureBucket::clearOutput()
{
    // NB: MSVC future<> implementation doesn't purge the task lambda (and
    // its captures) on invalidation (due to get()); must explicitly reset.
    mOutputBucketFuture = std::shared_future<std::shared_ptr<Bucket>>();
    mOutputBucketHash.clear();
    mOutputBucket.reset();
}

void
FutureBucket::clear()
{
    mState = FB_CLEAR;
    clearInputs();
    clearOutput();
}

bool
FutureBucket::isLive() const
{
    return (mState == FB_LIVE_INPUTS || mState == FB_LIVE_OUTPUT);
}

bool
FutureBucket::isMerging() const
{
    return mState == FB_LIVE_INPUTS;
}

bool
FutureBucket::hasHashes() const
{
    return (mState == FB_HASH_INPUTS || mState == FB_HASH_OUTPUT);
}

bool
FutureBucket::isClear() const
{
    return mState == FB_CLEAR;
}

bool
FutureBucket::mergeComplete() const
{
    ZoneScoped;
    assert(isLive());
    if (mOutputBucket)
    {
        return true;
    }

    return futureIsReady(mOutputBucketFuture);
}

std::shared_ptr<Bucket>
FutureBucket::resolve()
{
    ZoneScoped;
    checkState();
    assert(isLive());

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
        mOutputBucketFuture = std::shared_future<std::shared_ptr<Bucket>>();
    }

    mState = FB_LIVE_OUTPUT;
    checkState();
    return mOutputBucket;
}

bool
FutureBucket::hasOutputHash() const
{
    if (mState == FB_LIVE_OUTPUT || mState == FB_HASH_OUTPUT)
    {
        assert(!mOutputBucketHash.empty());
        return true;
    }
    return false;
}

std::string const&
FutureBucket::getOutputHash() const
{
    assert(mState == FB_LIVE_OUTPUT || mState == FB_HASH_OUTPUT);
    assert(!mOutputBucketHash.empty());
    return mOutputBucketHash;
}

static std::chrono::seconds
getAvailableTimeForMerge(Application& app, uint32_t level)
{
    auto closeTime = app.getConfig().getExpectedLedgerCloseTime();
    if (level >= 1)
    {
        return closeTime * BucketList::levelHalf(level - 1);
    }
    return closeTime;
}

void
FutureBucket::startMerge(Application& app, uint32_t maxProtocolVersion,
                         bool countMergeEvents, uint32_t level)
{
    ZoneScoped;
    // NB: startMerge starts with FutureBucket in a half-valid state; the inputs
    // are live but the merge is not yet running. So you can't call checkState()
    // on entry, only on exit.

    assert(mState == FB_LIVE_INPUTS);

    std::shared_ptr<Bucket> curr = mInputCurrBucket;
    std::shared_ptr<Bucket> snap = mInputSnapBucket;
    std::vector<std::shared_ptr<Bucket>> shadows = mInputShadowBuckets;

    assert(curr);
    assert(snap);
    assert(!mOutputBucketFuture.valid());
    assert(!mOutputBucket);

    CLOG_TRACE(Bucket, "Preparing merge of curr={} with snap={}",
               hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

    BucketManager& bm = app.getBucketManager();
    auto& timer = app.getMetrics().NewTimer(
        {"bucket", "merge-time", "level-" + std::to_string(level)});
    auto& availableTime = app.getMetrics().NewTimer(
        {"bucket", "available-time", "level-" + std::to_string(level)});
    availableTime.Update(getAvailableTimeForMerge(app, level));

    // It's possible we're running a merge that's already running, for example
    // due to having been serialized to the publish queue and then immediately
    // deserialized. In this case we want to attach to the existing merge, which
    // will have left a std::shared_future behind in a shared cache in the
    // bucket manager.
    MergeKey mk{BucketList::keepDeadEntries(level), curr, snap, shadows};
    auto f = bm.getMergeFuture(mk);
    if (f.valid())
    {
        CLOG_TRACE(Bucket,
                   "Re-attached to existing merge of curr={} with snap={}",
                   hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));
        mOutputBucketFuture = f;
        checkState();
        return;
    }
    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap, &bm, shadows, maxProtocolVersion, countMergeEvents, level,
         &timer, &app]() mutable {
            auto timeScope = timer.TimeScope();
            CLOG_TRACE(Bucket, "Worker merging curr={} with snap={}",
                       hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

            try
            {
                ZoneNamedN(mergeZone, "Merge task", true);
                ZoneValueV(mergeZone, static_cast<int64_t>(level));

                auto res = Bucket::merge(
                    bm, maxProtocolVersion, curr, snap, shadows,
                    BucketList::keepDeadEntries(level), countMergeEvents,
                    app.getClock().getIOContext(),
                    !app.getConfig().DISABLE_XDR_FSYNC);

                if (res)
                {
                    CLOG_TRACE(
                        Bucket, "Worker finished merging curr={} with snap={}",
                        hexAbbrev(curr->getHash()), hexAbbrev(snap->getHash()));

                    std::chrono::duration<double> time(timeScope.Stop());
                    double timePct =
                        time.count() /
                        getAvailableTimeForMerge(app, level).count() * 100;
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
                    "Error merging bucket curr={} with snap={}: "
                    "{}. {}",
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

void
FutureBucket::makeLive(Application& app, uint32_t maxProtocolVersion,
                       uint32_t level)
{
    ZoneScoped;
    checkState();
    assert(!isLive());
    assert(hasHashes());
    auto& bm = app.getBucketManager();
    if (hasOutputHash())
    {
        setLiveOutput(bm.getBucketByHash(hexToBin256(getOutputHash())));
    }
    else
    {
        assert(mState == FB_HASH_INPUTS);
        mInputCurrBucket =
            bm.getBucketByHash(hexToBin256(mInputCurrBucketHash));
        mInputSnapBucket =
            bm.getBucketByHash(hexToBin256(mInputSnapBucketHash));
        assert(mInputShadowBuckets.empty());
        for (auto const& h : mInputShadowBucketHashes)
        {
            auto b = bm.getBucketByHash(hexToBin256(h));
            assert(b);
            CLOG_DEBUG(Bucket, "Reconstituting shadow {}", h);
            mInputShadowBuckets.push_back(b);
        }
        mState = FB_LIVE_INPUTS;
        startMerge(app, maxProtocolVersion, /*countMergeEvents=*/true, level);
        assert(isLive());
    }
}

std::vector<std::string>
FutureBucket::getHashes() const
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
}
