// Copyright 2022 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

class Bucket;
struct HistoryArchiveState;

class AssumeStateWork : public Work
{
    HistoryArchiveState const& mHas;
    uint32_t const mMaxProtocolVersion;
    bool mWorkSpawned{false};

    // Keep strong reference to buckets in HAS so they are not garbage
    // collected during indexing
    std::vector<std::shared_ptr<Bucket>> mBuckets{};

  public:
    AssumeStateWork(Application& app, HistoryArchiveState const& has,
                    uint32_t maxProtocolVersion);

  protected:
    State doWork() override;
    void doReset() override;
};
}