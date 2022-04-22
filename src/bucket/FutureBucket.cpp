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
#include "util/GlobalChecks.h"
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
    , mProtocolVersion(app.getConfig().LEDGER_PROTOCOL_VERSION)
{
    ZoneScoped;
    // Constructed with a bunch of inputs, _immediately_ commence merging
    // them; there's no valid state for have-inputs-but-not-merging, the
    // presence of inputs implies merging, and vice-versa.
    releaseAssert(curr);
    releaseAssert(snap);
    mInputCurrBucketHash = binToHex(curr->getHashByProtocol(mProtocolVersion));
    mInputSnapBucketHash = binToHex(snap->getHashByProtocol(mProtocolVersion));
    if (protocolVersionStartsFrom(Bucket::getBucketVersion(snap),
                                  Bucket::FIRST_PROTOCOL_SHADOWS_REMOVED))
    {
        if (!mInputShadowBuckets.empty())
        {
            throw std::runtime_error(
                "Invalid FutureBucket: ledger version doesn't support shadows");
        }
    }
    for (auto const& b : mInputShadowBuckets)
    {
        mInputShadowBucketIDs.push_back(b->getHashID());
    }
    startMerge(app, maxProtocolVersion, countMergeEvents, level);
}

void
FutureBucket::setLiveOutput(std::shared_ptr<Bucket> output)
{
    ZoneScoped;
    mState = FB_LIVE_OUTPUT;
    mOutputBucketHash = binToHex(output->getHashByProtocol(mProtocolVersion));
    mOutputBucket = output;
    checkState();
}

void
FutureBucket::checkHashEq(std::shared_ptr<Bucket> const& b,
                          std::string const& h) const
{
    releaseAssert(b->getHashByProtocol(mProtocolVersion) == hexToBin256(h));
}

void
FutureBucket::checkHashesMatch() const
{
    ZoneScoped;
    if (!mInputShadowBuckets.empty())
    {
        releaseAssert(mInputShadowBuckets.size() ==
                      mInputShadowBucketIDs.size());
        for (size_t i = 0; i < mInputShadowBuckets.size(); ++i)
        {
            releaseAssert(mInputShadowBuckets.at(i)->getHashID() ==
                          mInputShadowBucketIDs.at(i));
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
        releaseAssert(mInputShadowBuckets.empty());
        releaseAssert(!mInputSnapBucket);
        releaseAssert(!mInputCurrBucket);
        releaseAssert(!mOutputBucket);
        releaseAssert(!mOutputBucketFuture.valid());
        releaseAssert(mInputShadowBucketIDs.empty());
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

void
FutureBucket::clearInputs()
{
    mInputShadowBuckets.clear();
    mInputSnapBucket.reset();
    mInputCurrBucket.reset();

    mInputShadowBucketIDs.clear();
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
    releaseAssert(isLive());
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
        mOutputBucketHash =
            binToHex(mOutputBucket->getHashByProtocol(mProtocolVersion));

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
        releaseAssert(!mOutputBucketHash.empty());
        return true;
    }
    return false;
}

std::string const&
FutureBucket::getOutputHash() const
{
    releaseAssert(mState == FB_LIVE_OUTPUT || mState == FB_HASH_OUTPUT);
    releaseAssert(!mOutputBucketHash.empty());
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

    releaseAssert(mState == FB_LIVE_INPUTS);
    mProtocolVersion = app.getConfig().LEDGER_PROTOCOL_VERSION;
    std::shared_ptr<Bucket> curr = mInputCurrBucket;
    std::shared_ptr<Bucket> snap = mInputSnapBucket;
    std::vector<std::shared_ptr<Bucket>> shadows = mInputShadowBuckets;

    releaseAssert(curr);
    releaseAssert(snap);
    releaseAssert(!mOutputBucketFuture.valid());
    releaseAssert(!mOutputBucket);

    CLOG_TRACE(Bucket, "Preparing merge of curr={} with snap={}",
               hexAbbrev(curr->getHashByProtocol(mProtocolVersion)),
               hexAbbrev(snap->getHashByProtocol(mProtocolVersion)));

    BucketManager& bm = app.getBucketManager();
    auto& timer = app.getMetrics().NewTimer(
        {"bucket", "merge-time", "level-" + std::to_string(level)});

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
                   hexAbbrev(curr->getHashByProtocol(mProtocolVersion)),
                   hexAbbrev(snap->getHashByProtocol(mProtocolVersion)));
        mOutputBucketFuture = f;
        checkState();
        return;
    }
    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap, &bm, shadows, countMergeEvents, level, &timer, &app,
         maxProtocolVersion, protocolVersion = mProtocolVersion]() mutable {
            auto timeScope = timer.TimeScope();
            CLOG_TRACE(Bucket, "Worker merging curr={} with snap={}",
                       hexAbbrev(curr->getHashByProtocol(protocolVersion)),
                       hexAbbrev(snap->getHashByProtocol(protocolVersion)));

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
                        hexAbbrev(curr->getHashByProtocol(protocolVersion)),
                        hexAbbrev(snap->getHashByProtocol(protocolVersion)));

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
                    FMT_STRING("Error merging bucket curr={} with snap={}: "
                               "{}. {}"),
                    hexAbbrev(curr->getHashByProtocol(protocolVersion)),
                    hexAbbrev(snap->getHashByProtocol(protocolVersion)),
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
    releaseAssert(!isLive());
    releaseAssert(hasHashes());
    mProtocolVersion = app.getConfig().LEDGER_PROTOCOL_VERSION;
    auto& bm = app.getBucketManager();
    if (hasOutputHash())
    {
        setLiveOutput(bm.getBucketByHashID(HashID(getOutputHash())));
    }
    else
    {
        releaseAssert(mState == FB_HASH_INPUTS);
        mInputCurrBucket = bm.getBucketByHashID(HashID(mInputCurrBucketHash));
        mInputSnapBucket = bm.getBucketByHashID(HashID(mInputSnapBucketHash));
        releaseAssert(mInputShadowBuckets.empty());
        for (auto const& id : mInputShadowBucketIDs)
        {
            auto b = bm.getBucketByHashID(id);
            releaseAssert(b);
            CLOG_DEBUG(Bucket, "Reconstituting shadow {}", id.toHex());
            mInputShadowBuckets.push_back(b);
        }
        mState = FB_LIVE_INPUTS;
        startMerge(app, maxProtocolVersion, /*countMergeEvents=*/true, level);
        releaseAssert(isLive());
    }
}

std::vector<HashID>
FutureBucket::getBucketIDs() const
{
    ZoneScoped;
    std::vector<HashID> hashes;
    if (!mInputCurrBucketHash.empty())
    {
        if (mInputCurrBucket)
        {
            hashes.push_back(mInputCurrBucket->getHashID());
        }
        else
        {
            hashes.emplace_back(mInputCurrBucketHash);
        }
    }
    if (!mInputSnapBucketHash.empty())
    {
        if (mInputSnapBucket)
        {
            hashes.push_back(mInputSnapBucket->getHashID());
        }
        else
        {
            hashes.emplace_back(mInputSnapBucketHash);
        }
    }
    for (auto const& b : mInputShadowBuckets)
    {
        hashes.push_back(b->getHashID());
    }
    if (!mOutputBucketHash.empty())
    {
        if (mOutputBucket)
        {
            hashes.push_back(mOutputBucket->getHashID());
        }
        else
        {
            hashes.emplace_back(mOutputBucketHash);
        }
    }

    return hashes;
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
    for (auto b : mInputShadowBuckets)
    {
        hashes.push_back(binToHex(b->getHashByProtocol(mProtocolVersion)));
    }
    if (!mOutputBucketHash.empty())
    {
        hashes.push_back(mOutputBucketHash);
    }
    return hashes;
}
}
