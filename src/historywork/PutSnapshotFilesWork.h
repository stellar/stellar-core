// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;

class PutSnapshotFilesWork : public Work
{
    std::shared_ptr<HistoryArchive const> mArchive;
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState mRemoteState;

    std::shared_ptr<Work> mGetHistoryArchiveStateWork;
    std::shared_ptr<Work> mPutFilesWork;
    std::shared_ptr<Work> mPutHistoryArchiveStateWork;

  public:
    PutSnapshotFilesWork(Application& app, WorkParent& parent,
                         std::shared_ptr<HistoryArchive const> archive,
                         std::shared_ptr<StateSnapshot> snapshot);
    ~PutSnapshotFilesWork();
    void onReset() override;
    Work::State onSuccess() override;
};
}
