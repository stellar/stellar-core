// Copyright 2025 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "util/types.h"
#include "work/Work.h"

namespace stellar
{

class LiveBucket;

class PopulateLedgerCacheWork : public Work
{
    std::vector<std::shared_ptr<LiveBucket>> mBucketsToProcess;
    uint32_t mBucketToProcessIndex;
    LedgerKeySet mDeadKeys;
    State advance();

  public:
    PopulateLedgerCacheWork(Application& app);

  protected:
    State doWork() override;
};

}