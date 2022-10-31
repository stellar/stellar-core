// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"
#include <atomic>
#include <memory>

namespace stellar
{

class BucketManager;
class Bucket;

class IndexBucketsWork : public Work
{
    class IndexWork : public BasicWork
    {
        std::shared_ptr<Bucket> mBucket;
        bool mDone{false};
        std::atomic_bool mExit{false};

        void postWork();

      public:
        IndexWork(Application& app, std::shared_ptr<Bucket> b);

      protected:
        State onRun() override;
        bool onAbort() override;
    };

    bool mWorkSpawned{false};
    void spawnWork();

  public:
    IndexBucketsWork(Application& app);

  protected:
    State doWork() override;
    void doReset() override;
};
}
