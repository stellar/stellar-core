// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "IndexBucketsWork.h"
#include "bucket/BucketIndex.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "util/HashOfHash.h"
#include "util/UnorderedSet.h"
#include "util/types.h"
#include "work/WorkWithCallback.h"
#include <Tracy.hpp>

namespace stellar
{
IndexBucketsWork::IndexWork::IndexWork(Application& app,
                                       std::shared_ptr<Bucket> b)
    : BasicWork(app, "index-work", BasicWork::RETRY_NEVER), mBucket(b)
{
}

BasicWork::State
IndexBucketsWork::IndexWork::onRun()
{
    if (mDone)
    {
        return State::WORK_SUCCESS;
    }

    postWork();
    return State::WORK_WAITING;
}

bool
IndexBucketsWork::IndexWork::onAbort()
{
    return true;
};

void
IndexBucketsWork::IndexWork::postWork()
{
    Application& app = this->mApp;

    std::weak_ptr<IndexWork> weak(
        std::static_pointer_cast<IndexWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, &b = mBucket, weak]() {
            auto self = weak.lock();
            if (!self || self->isAborting())
            {
                return;
            }

            b->setIndex(BucketIndex::createIndex(app.getBucketManager(),
                                                 b->getFilename()));

            app.postOnMainThread(
                [weak]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        self->mDone = true;
                        self->wakeUp();
                    }
                },
                "IndexWork: finished");
        },
        "IndexWork: starting in background");
}

IndexBucketsWork::IndexBucketsWork(Application& app)
    : Work(app, "index-bucketList", BasicWork::RETRY_NEVER)
{
}

BasicWork::State
IndexBucketsWork::doWork()
{
    if (!mWorkSpawned)
    {
        spawnWork();
    }

    return checkChildrenStatus();
}

void
IndexBucketsWork::doReset()
{
    mWorkSpawned = false;
}

void
IndexBucketsWork::spawnWork()
{
    auto& bm = mApp.getBucketManager();
    UnorderedSet<Hash> indexedBuckets;

    auto spawnIndexWork = [&](std::shared_ptr<Bucket> b) {
        // Don't index empty bucket or buckets that are already being
        // indexed. Sometimes one level's snap bucket may be another
        // level's future bucket. The indexing job may have started but
        // not finished, so check mIndexedBuckets. Some buckets may have
        // already been indexed when assuming LCL state, so also check
        // b->isIndexed.
        if (b->isEmpty() || b->isIndexed() ||
            !indexedBuckets.insert(b->getHash()).second)
        {
            return;
        }

        addWork<IndexWork>(b);
    };

    // Index all buckets in bucket list, including future buckets that
    // have already finished merging
    for (uint32_t i = 0; i < bm.getBucketList().kNumLevels; ++i)
    {
        auto& level = bm.getBucketList().getLevel(i);
        spawnIndexWork(level.getCurr());
        spawnIndexWork(level.getSnap());
        auto& nextFuture = level.getNext();
        if (nextFuture.hasOutputHash())
        {
            auto hash = hexToBin256(nextFuture.getOutputHash());
            spawnIndexWork(bm.getBucketByHash(hash));
        }
    }

    mWorkSpawned = true;
}
}