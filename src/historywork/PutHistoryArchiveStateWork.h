// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"
#include "work/WorkSequence.h"

namespace stellar
{

class HistoryArchive;
struct HistoryArchiveState;

class PutHistoryArchiveStateWork : public Work
{
    HistoryArchiveState const& mState;
    std::shared_ptr<HistoryArchive> mArchive;
    std::string mLocalFilename;
    std::shared_ptr<WorkSequence> mPutRemoteFileWork;

    void spawnPublishWork();

  public:
    PutHistoryArchiveStateWork(Application& app,
                               HistoryArchiveState const& state,
                               std::shared_ptr<HistoryArchive> archive);
    ~PutHistoryArchiveStateWork() = default;

  protected:
    void doReset() override;
    State doWork() override;
};
}
