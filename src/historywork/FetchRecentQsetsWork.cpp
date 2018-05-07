// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/FetchRecentQsetsWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "main/Application.h"
#include "util/TmpDir.h"
#include "util/XDRStream.h"

namespace stellar
{

FetchRecentQsetsWork::FetchRecentQsetsWork(Application& app, WorkParent& parent,
                                           InferredQuorum& inferredQuorum)
    : Work(app, parent, "fetch-recent-qsets"), mInferredQuorum(inferredQuorum)
{
}

FetchRecentQsetsWork::~FetchRecentQsetsWork()
{
    clearChildren();
}

void
FetchRecentQsetsWork::onReset()
{
    clearChildren();
    mDownloadSCPMessagesWork.reset();
    mDownloadDir = std::make_unique<TmpDir>(
        mApp.getTmpDirManager().tmpDir(getUniqueName()));
}

Work::State
FetchRecentQsetsWork::onSuccess()
{
    // Phase 1: fetch remote history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
            "get-history-archive-state", mRemoteState, 0);
        return WORK_PENDING;
    }

    // Phase 2: download some SCP messages; for now we just pull the past
    // 100 checkpoints = 9 hours of history. A more sophisticated view
    // would survey longer time periods at lower resolution.
    uint32_t numCheckpoints = 100;
    uint32_t step = mApp.getHistoryManager().getCheckpointFrequency();
    uint32_t window = numCheckpoints * step;
    uint32_t lastSeq = mRemoteState.currentLedger;
    uint32_t firstSeq = lastSeq < window ? (step - 1) : (lastSeq - window);

    if (!mDownloadSCPMessagesWork)
    {
        CLOG(INFO, "History") << "Downloading recent SCP messages: ["
                              << firstSeq << ", " << lastSeq << "]";
        auto range = CheckpointRange{firstSeq, lastSeq, step};
        mDownloadSCPMessagesWork = addWork<BatchDownloadWork>(
            range, HISTORY_FILE_TYPE_SCP, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: extract the qsets.
    for (uint32_t i = firstSeq; i <= lastSeq; i += step)
    {
        CLOG(INFO, "History") << "Scanning for QSets in checkpoint: " << i;
        XDRInputFileStream in;
        FileTransferInfo fi(*mDownloadDir, HISTORY_FILE_TYPE_SCP, i);
        in.open(fi.localPath_nogz());
        SCPHistoryEntry tmp;
        while (in && in.readOne(tmp))
        {
            mInferredQuorum.noteSCPHistory(tmp);
        }
    }

    return WORK_SUCCESS;
}
}
