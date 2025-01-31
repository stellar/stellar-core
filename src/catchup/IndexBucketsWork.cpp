// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "IndexBucketsWork.h"
#include "bucket/BucketManager.h"
#include "bucket/DiskIndex.h"
#include "bucket/LiveBucket.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include "util/UnorderedSet.h"
#include <Tracy.hpp>

namespace stellar
{
IndexBucketsWork::IndexWork::IndexWork(Application& app,
                                       std::shared_ptr<LiveBucket> b)
    : BasicWork(app, "index-work", BasicWork::RETRY_NEVER), mBucket(b)
{
}

BasicWork::State
IndexBucketsWork::IndexWork::onRun()
{
    if (mState == State::WORK_WAITING)
    {
        postWork();
    }

    return mState;
}

bool
IndexBucketsWork::IndexWork::onAbort()
{
    return true;
};

void
IndexBucketsWork::IndexWork::onReset()
{
    mState = BasicWork::State::WORK_WAITING;
}

void
IndexBucketsWork::IndexWork::postWork()
{
    Application& app = this->mApp;
    asio::io_context& ctx = app.getWorkerIOContext();

    std::weak_ptr<IndexWork> weak(
        std::static_pointer_cast<IndexWork>(shared_from_this()));
    app.postOnBackgroundThread(
        [&app, &ctx, weak]() {
            auto self = weak.lock();
            if (!self || self->isAborting())
            {
                return;
            }

            auto& bm = app.getBucketManager();
            auto indexFilename =
                bm.bucketIndexFilename(self->mBucket->getHash());

            if (bm.getConfig().BUCKETLIST_DB_PERSIST_INDEX &&
                fs::exists(indexFilename))
            {
                self->mIndex = loadIndex<LiveBucket>(bm, indexFilename,
                                                     self->mBucket->getSize());

                // If we could not load the index from the file, file is out of
                // date. Delete and create a new index.
                if (!self->mIndex)
                {
                    CLOG_WARNING(Bucket, "Outdated index file: {}",
                                 indexFilename);
                    std::remove(indexFilename.c_str());
                }
                else
                {
                    CLOG_DEBUG(Bucket, "Loaded index from file: {}",
                               indexFilename);
                }
            }

            if (!self->mIndex)
            {
                // TODO: Fix this when archive BucketLists assume state
                self->mIndex =
                    createIndex<LiveBucket>(bm, self->mBucket->getFilename(),
                                            self->mBucket->getHash(), ctx);
            }

            app.postOnMainThread(
                [weak]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        if (self->mIndex)
                        {
                            self->mState = BasicWork::State::WORK_SUCCESS;
                            if (!self->isAborting())
                            {
                                self->mApp.getBucketManager().maybeSetIndex(
                                    self->mBucket, std::move(self->mIndex));
                            }
                        }
                        else
                        {
                            self->mState = BasicWork::State::WORK_FAILURE;
                        }
                        self->wakeUp();
                    }
                },
                "IndexWork: finished");
        },
        "IndexWork: starting in background");
}

IndexBucketsWork::IndexBucketsWork(
    Application& app, std::vector<std::shared_ptr<LiveBucket>> const& buckets)
    : Work(app, "index-bucketList", BasicWork::RETRY_NEVER), mBuckets(buckets)
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
    UnorderedSet<Hash> indexedBuckets;
    auto spawnIndexWork = [&](std::shared_ptr<LiveBucket> const& b) {
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

    for (auto const& b : mBuckets)
    {
        spawnIndexWork(b);
    }

    mWorkSpawned = true;
}
}