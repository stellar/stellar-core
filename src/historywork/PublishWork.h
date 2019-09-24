// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/WorkSequence.h"

namespace stellar
{

struct StateSnapshot;

class PublishWork : public WorkSequence
{
    std::shared_ptr<StateSnapshot> mSnapshot;
    std::vector<std::string> mOriginalBuckets;

  public:
    PublishWork(Application& app, std::shared_ptr<StateSnapshot> snapshot,
                std::vector<std::shared_ptr<BasicWork>> seq,
                std::vector<std::string> const& bucketHashes);
    ~PublishWork() = default;

  protected:
    void onFailureRaise() override;
    void onSuccess() override;
};
}
