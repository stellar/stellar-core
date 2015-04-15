// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

// ASIO is somewhat particular about when it gets included -- it wants to be the
// first to include <windows.h> -- so we try to include it before everything
// else.
#include "util/asio.h"

#include "bucket/FutureBucket.h"
#include "bucket/Bucket.h"
#include "bucket/BucketManager.h"
#include "crypto/Hex.h"
#include "main/Application.h"
#include "util/Logging.h"

#include <chrono>

namespace stellar
{

FutureBucket::FutureBucket(Application& app,
                           std::shared_ptr<Bucket> const& curr,
                           std::shared_ptr<Bucket> const& snap,
                           std::vector<std::shared_ptr<Bucket>> const& shadows)
    : mState(FB_LIVE_INPUTS)
    , mInputCurrBucket(curr)
    , mInputSnapBucket(snap)
    , mInputShadowBuckets(shadows)
{
    // Constructed with a bunch of inputs, _immediately_ commence merging
    // them; there's no valid state for have-inputs-but-not-merging, the
    // presence of inputs implies merging, and vice-versa.
    assert(curr);
    assert(snap);
    mInputCurrBucketHash = binToHex(curr->getHash());
    mInputSnapBucketHash = binToHex(snap->getHash());
    for (auto const& b : mInputShadowBuckets)
    {
        mInputShadowBucketHashes.push_back(binToHex(b->getHash()));
    }
    startMerge(app);
}

FutureBucket::FutureBucket(std::shared_ptr<Bucket> const& output)
    : mState(FB_LIVE_OUTPUT)
    , mOutputBucketHash(binToHex(output->getHash()))
{
    // Constructed with an output bucket, fake-up a promise for it connected to
    // the future so that it can be immediately retrieved.
    std::promise<std::shared_ptr<Bucket>> promise;
    mOutputBucket = promise.get_future().share();
    promise.set_value(output);
}

static void
checkHashEq(std::shared_ptr<Bucket> b, std::string const& h)
{
    assert(b->getHash() == hexToBin256(h));
}

void
FutureBucket::checkHashesMatch() const
{
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
    if (mOutputBucket.valid() && mergeComplete() && !mOutputBucketHash.empty())
    {
        checkHashEq(mOutputBucket.get(), mOutputBucketHash);
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
        assert(!mOutputBucket.valid());
        assert(mInputShadowBucketHashes.empty());
        assert(mInputSnapBucketHash.empty());
        assert(mInputCurrBucketHash.empty());
        assert(mOutputBucketHash.empty());
        break;

    case FB_LIVE_INPUTS:
        assert(mInputSnapBucket);
        assert(mInputCurrBucket);
        assert(mOutputBucket.valid());
        assert(mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_LIVE_OUTPUT:
        assert(mergeComplete());
        assert(mOutputBucket.valid());
        assert(!mOutputBucketHash.empty());
        checkHashesMatch();
        break;

    case FB_HASH_INPUTS:
        assert(!mInputSnapBucket);
        assert(!mInputCurrBucket);
        assert(!mOutputBucket.valid());
        assert(!mInputSnapBucketHash.empty());
        assert(!mInputCurrBucketHash.empty());
        assert(mOutputBucketHash.empty());
        break;

    case FB_HASH_OUTPUT:
        assert(!mInputSnapBucket);
        assert(!mInputCurrBucket);
        assert(!mOutputBucket.valid());
        assert(mInputSnapBucketHash.empty());
        assert(mInputCurrBucketHash.empty());
        assert(!mOutputBucketHash.empty());
        break;
    }
}

void
FutureBucket::clear()
{
    mState = FB_CLEAR;
    mInputShadowBuckets.clear();
    mInputSnapBucket.reset();
    mInputCurrBucket.reset();
    mOutputBucket = std::shared_future<std::shared_ptr<Bucket>>();

    mInputShadowBucketHashes.clear();
    mInputSnapBucketHash.clear();
    mInputCurrBucketHash.clear();
    mOutputBucketHash.clear();
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
FutureBucket::mergeComplete() const
{
    assert(isLive());
    auto status = mOutputBucket.wait_for(std::chrono::nanoseconds(1));
    return status == std::future_status::ready;
}

std::shared_ptr<Bucket>
FutureBucket::commit()
{
    checkState();
    assert(isLive());
    std::shared_ptr<Bucket> bucket = mOutputBucket.get();
    if (mOutputBucketHash.empty())
    {
        mOutputBucketHash = binToHex(bucket->getHash());
    }
    else
    {
        checkHashEq(bucket, mOutputBucketHash);
    }
    mState = FB_LIVE_OUTPUT;
    checkState();
    return bucket;
}

void
FutureBucket::startMerge(Application& app)
{
    // NB: startMerge starts with FutureBucket in a half-valid state; the inputs
    // are live but the merge is not yet running. So you can't call checkState()
    // on entry, only on exit.

    assert(mState == FB_LIVE_INPUTS);

    std::shared_ptr<Bucket> curr = mInputCurrBucket;
    std::shared_ptr<Bucket> snap = mInputSnapBucket;
    std::vector<std::shared_ptr<Bucket>> shadows = mInputShadowBuckets;

    assert(curr);
    assert(snap);
    assert(!mOutputBucket.valid());

    CLOG(TRACE, "Bucket")
        << "Preparing merge of curr=" << hexAbbrev(curr->getHash())
        << " with snap=" << hexAbbrev(snap->getHash());

    BucketManager& bm = app.getBucketManager();

    using task_t = std::packaged_task<std::shared_ptr<Bucket>()>;
    std::shared_ptr<task_t> task = std::make_shared<task_t>(
        [curr, snap, &bm, shadows]()
        {
            CLOG(TRACE, "Bucket")
            << "Worker merging curr=" << hexAbbrev(curr->getHash())
            << " with snap=" << hexAbbrev(snap->getHash());

            auto res = Bucket::merge(bm, curr, snap, shadows);

            CLOG(TRACE, "Bucket")
            << "Worker finished merging curr=" << hexAbbrev(curr->getHash())
            << " with snap=" << hexAbbrev(snap->getHash());

            return res;
        });

    mOutputBucket = task->get_future().share();
    app.getWorkerIOService().post(bind(&task_t::operator(), task));
    checkState();
}

void
FutureBucket::makeLive(Application& app)
{
    checkState();
    assert(!isLive());
    assert(hasHashes());
    auto& bm = app.getBucketManager();
    mInputCurrBucket = bm.getBucketByHash(hexToBin256(mInputCurrBucketHash));
    mInputSnapBucket = bm.getBucketByHash(hexToBin256(mInputSnapBucketHash));
    assert(mInputShadowBuckets.empty());
    for (auto const& h : mInputShadowBucketHashes)
    {
        auto b = bm.getBucketByHash(hexToBin256(h));
        mInputShadowBuckets.push_back(b);
    }
    mState = FB_LIVE_INPUTS;
    startMerge(app);
    assert(isLive());
}

std::shared_future<std::shared_ptr<Bucket>>
FutureBucket::getSharedFuture() const
{
    return mOutputBucket;
}


}
