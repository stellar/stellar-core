// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

class HistoryArchive;
struct HistoryArchiveState;

class PutHistoryArchiveStateWork : public Work
{
    HistoryArchiveState mState;
    std::shared_ptr<HistoryArchive const> mArchive;
    std::string mLocalFilename;
    std::shared_ptr<Work> mPutRemoteFileWork;

  public:
    PutHistoryArchiveStateWork(Application& app, WorkParent& parent,
                               HistoryArchiveState state,
                               std::shared_ptr<HistoryArchive const> archive);
    void onReset() override;
    void onRun() override;
    Work::State onSuccess() override;
};
}
