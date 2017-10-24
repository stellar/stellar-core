// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;

class PublishWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;
    std::vector<std::string> mOriginalBuckets;

    std::shared_ptr<Work> mResolveSnapshotWork;
    std::shared_ptr<Work> mWriteSnapshotWork;
    std::shared_ptr<Work> mUpdateArchivesWork;

  public:
    PublishWork(Application& app, WorkParent& parent,
                std::shared_ptr<StateSnapshot> snapshot);
    ~PublishWork();
    std::string getStatus() const override;
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};
}
