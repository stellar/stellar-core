// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

struct StateSnapshot;

class PutSnapshotFilesWork : public Work
{
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState mRemoteState;
    bool mStarted{false};

  public:
    PutSnapshotFilesWork(Application& app,
                         std::shared_ptr<StateSnapshot> snapshot);
    ~PutSnapshotFilesWork() = default;

  protected:
    State doWork() override;
    void doReset() override;
};
}
