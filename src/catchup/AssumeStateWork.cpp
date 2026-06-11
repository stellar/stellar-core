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
            // For composite (sharded) buckets, reference the shard files;
            // the combined hash names no file of its own.
            auto addByHash = [&](std::string const& hash,
                                 std::vector<std::string> const& shardHashes) {
                std::vector<std::string> hashes;
                if (!shardHashes.empty())
                {
                    hashes = shardHashes;
                }
                else
                {
                    hashes.push_back(hash);
                }
                for (auto const& h : hashes)
                {
                    auto b = bm.getBucketByHash<BucketT>(hexToBin256(h));
                    if (!b)
                    {
                        throw std::runtime_error(
                            "Missing bucket files while "
                            "assuming saved BucketList state");
                    }
                    workBuckets.emplace_back(b);
                }
            };
            addByHash(hasBuckets.at(i).curr, hasBuckets.at(i).currShards);
            addByHash(hasBuckets.at(i).snap, hasBuckets.at(i).snapShards);
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

    if (has.hasHotArchiveBuckets())
    {
        processBuckets(mHas.hotArchiveBuckets, HotArchiveBucketList::kNumLevels,
                       mHotArchiveBuckets);
    }
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
            app.getBucketManager().assumeState(app, has, maxProtocolVersion,
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
