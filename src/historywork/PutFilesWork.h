// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/StateSnapshot.h"
#include "work/Work.h"

namespace stellar
{
class PutFilesWork : public Work
{
    std::shared_ptr<HistoryArchive> mArchive;
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState const& mRemoteState;

    bool mChildrenSpawned{false};

  public:
    PutFilesWork(Application& app, std::shared_ptr<HistoryArchive> archive,
                 std::shared_ptr<StateSnapshot> snapshot,
                 HistoryArchiveState const& remoteState);
    ~PutFilesWork() = default;

  protected:
    void doReset() override;
    State doWork() override;
};
}
