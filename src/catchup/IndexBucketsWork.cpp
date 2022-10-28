// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "IndexBucketsWork.h"
#include "bucket/BucketIndex.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "util/HashOfHash.h"
#include "util/types.h"
#include "work/WorkWithCallback.h"
#include <Tracy.hpp>

namespace stellar
{
IndexBucketsWork::IndexBucketsWork(Application& app)
    : BasicWork(app, "index-bucketList", BasicWork::RETRY_NEVER)
{
}

BasicWork::State
IndexBucketsWork::onRun()
{
    ZoneScoped;
    if (mDone)
    {
        return State::WORK_SUCCESS;
    }

    spawnWork();
    return State::WORK_WAITING;
}

bool
IndexBucketsWork::onAbort()
{
    return true;
}

void
IndexBucketsWork::spawnWork()
{
    Application& app = this->mApp;

    std::weak_ptr<IndexBucketsWork> weak(
        std::static_pointer_cast<IndexBucketsWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, &bm = app.getBucketManager(), weak]() {
            auto self = weak.lock();
            if (!self || self->isAborting())
            {
                return;
            }

            auto indexBucket = [&app, &indexedBuckets = self->mIndexedBuckets](
                                   std::shared_ptr<Bucket> b) {
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

                b->setIndex(BucketIndex::createIndex(app.getConfig(),
                                                     b->getFilename()));
            };

            // Index all buckets in bucket list, including future buckets that
            // have already finished merging
            for (uint32_t i = 0; i < bm.getBucketList().kNumLevels; ++i)
            {
                auto& level = bm.getBucketList().getLevel(i);
                indexBucket(level.getCurr());
                indexBucket(level.getSnap());
                auto& nextFuture = level.getNext();
                if (nextFuture.hasOutputHash())
                {
                    auto hash = hexToBin256(nextFuture.getOutputHash());
                    indexBucket(bm.getBucketByHash(hash));
                }
            }

            app.postOnMainThread(
                [weak]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        self->mDone = true;
                        self->wakeUp();
                    }
                },
                "IndexBuckets: finish");
        },
        "IndexBuckets: start in background");
}
}