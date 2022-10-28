// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/UnorderedSet.h"
#include "work/Work.h"

namespace stellar
{

class BucketManager;

class IndexBucketsWork : public BasicWork
{
    UnorderedSet<Hash> mIndexedBuckets;
    bool mDone{false};

    void spawnWork();

  public:
    IndexBucketsWork(Application& app);

  protected:
    State onRun() override;
    bool onAbort() override;
};
}
