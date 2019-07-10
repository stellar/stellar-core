// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/PutSnapshotFilesWork.h"
#include "bucket/BucketManager.h"
#include "history/HistoryArchiveManager.h"
#include "history/StateSnapshot.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/GzipAndPutFilesWork.h"
#include "historywork/PutHistoryArchiveStateWork.h"
#include "main/Application.h"
#include <util/format.h>

namespace stellar
{

PutSnapshotFilesWork::PutSnapshotFilesWork(
    Application& app, std::shared_ptr<StateSnapshot> snapshot)
    : Work(app,
           fmt::format("update-archives-{:08x}",
                       snapshot->mLocalState.currentLedger),
           // Each put-snapshot-sequence will retry correctly
           BasicWork::RETRY_NEVER)
    , mSnapshot(snapshot)
{
}

BasicWork::State
PutSnapshotFilesWork::doWork()
{
    if (!mStarted)
    {
        for (auto& writableArchive :
             mApp.getHistoryArchiveManager().getWritableHistoryArchives())
        {
            // Phase 1: fetch remote history archive state
            auto getState = std::make_shared<GetHistoryArchiveStateWork>(
                mApp, mRemoteState, 0, writableArchive);

            // Phase 2: put all requisite data files
            auto putFiles = std::make_shared<GzipAndPutFilesWork>(
                mApp, writableArchive, mSnapshot, mRemoteState);

            // Phase 3: update remote history archive state
            auto putState = std::make_shared<PutHistoryArchiveStateWork>(
                mApp, mSnapshot->mLocalState, writableArchive);

            std::vector<std::shared_ptr<BasicWork>> seq{getState, putFiles,
                                                        putState};
            // Each inner step will retry a lot, so only retry the sequence once
            // in case of an unexpected failure
            addWork<WorkSequence>("put-snapshot-sequence", seq,
                                  BasicWork::RETRY_ONCE);
        }
        mStarted = true;
    }
    else
    {
        if (allChildrenDone())
        {
            // Some children might fail, but at least the rest of the archives
            // are updated
            return anyChildRaiseFailure() ? State::WORK_FAILURE
                                          : State::WORK_SUCCESS;
        }

        if (!anyChildRunning())
        {
            return State::WORK_WAITING;
        }
    }
    return State::WORK_RUNNING;
}

void
PutSnapshotFilesWork::doReset()
{
    mStarted = false;
}
}
