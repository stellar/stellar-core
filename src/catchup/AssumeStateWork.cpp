// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "AssumeStateWork.h"
#include "bucket/BucketManager.h"
#include "bucket/LiveBucketList.h"
#include "catchup/IndexBucketsWork.h"
#include "crypto/Hex.h"
#include "history/HistoryArchive.h"
#include "invariant/InvariantManager.h"
#include "work/WorkSequence.h"
#include "work/WorkWithCallback.h"

namespace stellar
{
AssumeStateWork::AssumeStateWork(Application& app,
                                 HistoryArchiveState const& has,
                                 uint32_t maxProtocolVersion,
                                 bool restartMerges)
    : Work(app, "assume-state", BasicWork::RETRY_NEVER)
    , mHas(has)
    , mMaxProtocolVersion(maxProtocolVersion)
    , mRestartMerges(restartMerges)
{
    // Maintain reference to all Buckets in HAS to avoid garbage collection,
    // including future buckets that have already finished merging
    auto processBuckets = [&bm = mApp.getBucketManager()](
                              auto const& hasBuckets, size_t expectedLevels,
                              auto& workBuckets) {
        releaseAssert(hasBuckets.size() == expectedLevels);
        using BucketT = typename std::decay_t<
            decltype(hasBuckets)>::value_type::bucket_type;
        for (uint32_t i = 0; i < expectedLevels; ++i)
        {
            auto curr =
                bm.getBucketByHash<BucketT>(hexToBin256(hasBuckets.at(i).curr));
            auto snap =
                bm.getBucketByHash<BucketT>(hexToBin256(hasBuckets.at(i).snap));
            if (!(curr && snap))
            {
                throw std::runtime_error("Missing bucket files while "
                                         "assuming saved BucketList state");
            }

            workBuckets.emplace_back(curr);
            workBuckets.emplace_back(snap);
            auto& nextFuture = hasBuckets.at(i).next;
            if (nextFuture.hasOutputHash())
            {
                auto nextBucket = bm.getBucketByHash<BucketT>(
                    hexToBin256(nextFuture.getOutputHash()));
                if (!nextBucket)
                {
                    throw std::runtime_error(
                        "Missing future bucket files while "
                        "assuming saved BucketList state");
                }

                workBuckets.emplace_back(nextBucket);
            }
        }
    };

    processBuckets(mHas.currentBuckets, LiveBucketList::kNumLevels,
                   mLiveBuckets);

#ifdef ENABLE_NEXT_PROTOCOL_VERSION_UNSAFE_FOR_PRODUCTION
    if (has.hasHotArchiveBuckets())
    {
        processBuckets(mHas.hotArchiveBuckets, HotArchiveBucketList::kNumLevels,
                       mHotArchiveBuckets);
    }
#endif
}

BasicWork::State
AssumeStateWork::doWork()
{
    if (!mWorkSpawned)
    {
        std::vector<std::shared_ptr<BasicWork>> seq;

        // Index Bucket files
        seq.push_back(
            std::make_shared<IndexBucketsWork<LiveBucket>>(mApp, mLiveBuckets));
        seq.push_back(std::make_shared<IndexBucketsWork<HotArchiveBucket>>(
            mApp, mHotArchiveBuckets));

        // Add bucket files to BucketList and restart merges
        auto assumeStateCB = [&has = mHas,
                              maxProtocolVersion = mMaxProtocolVersion,
                              restartMerges = mRestartMerges,
                              &liveBuckets = mLiveBuckets,
                              &hotArchiveBuckets =
                                  mHotArchiveBuckets](Application& app) {
            app.getBucketManager().assumeState(has, maxProtocolVersion,
                                               restartMerges);

            // Drop bucket references once assume state complete since buckets
            // now referenced by BucketList
            liveBuckets.clear();
            hotArchiveBuckets.clear();

            // Check invariants after state has been assumed
            app.getInvariantManager().checkAfterAssumeState(has.currentLedger);

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