// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/BasicWork.h"
#include "work/Work.h"
#include <memory>

namespace stellar
{

class Bucket;
class LiveBucketIndex;
class BucketManager;
class LiveBucket;

class IndexBucketsWork : public Work
{
    class IndexWork : public BasicWork
    {
        std::shared_ptr<LiveBucket> mBucket;
        std::unique_ptr<LiveBucketIndex const> mIndex;
        BasicWork::State mState{BasicWork::State::WORK_WAITING};

        void postWork();

      public:
        IndexWork(Application& app, std::shared_ptr<LiveBucket> b);

      protected:
        State onRun() override;
        bool onAbort() override;
        void onReset() override;
    };

    std::vector<std::shared_ptr<LiveBucket>> const& mBuckets;

    bool mWorkSpawned{false};
    void spawnWork();

  public:
    IndexBucketsWork(Application& app,
                     std::vector<std::shared_ptr<LiveBucket>> const& buckets);

  protected:
    State doWork() override;
    void doReset() override;
};
}
