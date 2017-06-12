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
#include "util/make_unique.h"

namespace stellar
{

FetchRecentQsetsWork::FetchRecentQsetsWork(Application& app, WorkParent& parent,
                                           InferredQuorum& inferredQuorum,
                                           handler endHandler)
    : Work(app, parent, "fetch-recent-qsets")
    , mEndHandler(endHandler)
    , mInferredQuorum(inferredQuorum)
{
}

void
FetchRecentQsetsWork::onReset()
{
    clearChildren();
    mDownloadSCPMessagesWork.reset();
    mDownloadDir =
        make_unique<TmpDir>(mApp.getTmpDirManager().tmpDir(getUniqueName()));
}

void
FetchRecentQsetsWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec);
}

Work::State
FetchRecentQsetsWork::onSuccess()
{
    // Phase 1: fetch remote history archive state
    if (!mGetHistoryArchiveStateWork)
    {
        mGetHistoryArchiveStateWork = addWork<GetHistoryArchiveStateWork>(
            mRemoteState, 0, std::chrono::seconds(0));
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
        mDownloadSCPMessagesWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_SCP, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: extract the qsets.
    // use uint64_t for i to prevent overflows
    for (uint64_t i = firstSeq; i <= lastSeq; i += step)
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

    asio::error_code ec;
    mEndHandler(ec);
    return WORK_SUCCESS;
}
}
