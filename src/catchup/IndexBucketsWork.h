// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"
#include <memory>

namespace stellar
{

class Bucket;
class BucketIndex;
class BucketManager;

class IndexBucketsWork : public Work
{
    class IndexWork : public BasicWork
    {
        std::shared_ptr<Bucket> mBucket;
        std::unique_ptr<BucketIndex const> mIndex;
        bool mDone{false};

        void postWork();

      public:
        IndexWork(Application& app, std::shared_ptr<Bucket> b);

      protected:
        State onRun() override;
        bool onAbort() override;
    };

    std::vector<std::shared_ptr<Bucket>> const& mBuckets;

    bool mWorkSpawned{false};
    void spawnWork();

  public:
    IndexBucketsWork(Application& app,
                     std::vector<std::shared_ptr<Bucket>> const& buckets);

  protected:
    State doWork() override;
    void doReset() override;
};
}
