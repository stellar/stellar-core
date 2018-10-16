// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace stellar
{

class HistoryArchive;
struct HistoryArchiveState;

class PutHistoryArchiveStateWork : public Work
{
    HistoryArchiveState const& mState;
    std::shared_ptr<HistoryArchive> mArchive;
    std::string const mLocalFilename;
    std::shared_ptr<WorkSequence> mPutRemoteFileWork;

    void spawnPublishWork();

  public:
    PutHistoryArchiveStateWork(Application& app, std::function<void()> callback,
                               HistoryArchiveState const& state,
                               std::shared_ptr<HistoryArchive> archive);
    ~PutHistoryArchiveStateWork() = default;

  protected:
    void doReset() override;
    State doWork() override;
    void onFailureRetry() override;
    void onFailureRaise() override;
};
}