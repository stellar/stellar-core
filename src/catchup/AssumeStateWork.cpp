// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AssumeStateWork.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/IndexBucketsWork.h"
#include "history/HistoryArchive.h"
#include "work/WorkSequence.h"
#include "work/WorkWithCallback.h"

namespace stellar
{
AssumeStateWork::AssumeStateWork(Application& app,
                                 HistoryArchiveState const& has,
                                 uint32_t maxProtocolVersion)
    : Work(app, "assume-state", BasicWork::RETRY_NEVER)
    , mHas(has)
    , mMaxProtocolVersion(maxProtocolVersion)
{
    // Maintain reference to all Buckets in HAS to avoid garbage collection,
    // including future buckets that have already finished merging
    auto& bm = mApp.getBucketManager();
    for (uint32_t i = 0; i < BucketList::kNumLevels; ++i)
    {
        auto curr =
            bm.getBucketByHash(hexToBin256(mHas.currentBuckets.at(i).curr));
        auto snap =
            bm.getBucketByHash(hexToBin256(mHas.currentBuckets.at(i).snap));
        if (!(curr && snap))
        {
            throw std::runtime_error("Missing bucket files while "
                                     "assuming saved BucketList state");
        }

        mBuckets.emplace_back(curr);
        mBuckets.emplace_back(snap);
        auto& nextFuture = mHas.currentBuckets.at(i).next;
        if (nextFuture.hasOutputHash())
        {
            auto nextBucket =
                bm.getBucketByHash(hexToBin256(nextFuture.getOutputHash()));
            if (!nextBucket)
            {
                throw std::runtime_error("Missing future bucket files while "
                                         "assuming saved BucketList state");
            }

            mBuckets.emplace_back(nextBucket);
        }
    }
}

BasicWork::State
AssumeStateWork::doWork()
{
    if (!mWorkSpawned)
    {
        std::vector<std::shared_ptr<BasicWork>> seq;

        // Index Bucket files
        if (mApp.getConfig().isUsingBucketListDB())
        {
            seq.push_back(std::make_shared<IndexBucketsWork>(mApp, mBuckets));
        }

        // Add bucket files to BucketList and restart merges
        auto assumeStateCB = [&has = mHas,
                              maxProtocolVersion = mMaxProtocolVersion,
                              &buckets = mBuckets](Application& app) {
            app.getBucketManager().assumeState(has, maxProtocolVersion);

            // Drop bucket references once assume state complete since buckets
            // now referenced by BucketList
            buckets.clear();
            return true;
        };
        auto work = std::make_shared<WorkWithCallback>(mApp, "assume-state",
                                                       assumeStateCB);
        seq.push_back(work);

        addWork<WorkSequence>("assume-state-seq", seq, RETRY_NEVER);

        mWorkSpawned = true;
        return State::WORK_RUNNING;
    }

    return checkChildrenStatus();
}

void
AssumeStateWork::doReset()
{
    mWorkSpawned = false;
}
}