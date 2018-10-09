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
    std::shared_ptr<HistoryArchive> mArchive;
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState mRemoteState;

    std::shared_ptr<WorkSequence> mPublishSnapshot;

  public:
    PutSnapshotFilesWork(Application& app, std::function<void()> callback,
                         std::shared_ptr<HistoryArchive> archive,
                         std::shared_ptr<StateSnapshot> snapshot);
    ~PutSnapshotFilesWork() = default;

  protected:
    void doReset() override;
    State doWork() override;
};

class GzipAndPutFilesWork : public Work
{
    std::shared_ptr<HistoryArchive> mArchive;
    std::shared_ptr<StateSnapshot> mSnapshot;
    HistoryArchiveState const& mRemoteState;

    bool mChildrenSpawned{false};

  public:
    GzipAndPutFilesWork(Application& app, std::function<void()> callback,
                        std::shared_ptr<HistoryArchive> archive,
                        std::shared_ptr<StateSnapshot> snapshot,
                        HistoryArchiveState const& remoteState);
    ~GzipAndPutFilesWork() = default;

  protected:
    void doReset() override;
    State doWork() override;
};
}
