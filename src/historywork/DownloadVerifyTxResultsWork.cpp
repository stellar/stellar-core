// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadVerifyTxResultsWork.h"
#include "catchup/CatchupManager.h"
#include "history/HistoryManager.h"
#include "historywork/Progress.h"
#include "historywork/VerifyTxResultsSnapWork.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{
DownloadVerifyTxResultsWork::DownloadVerifyTxResultsWork(
    Application& app, WorkParent& parent, CheckpointRange range,
    TmpDir const& downloadDir)
    : BatchWork(app, parent,
                fmt::format("download-verify-results-{:08x}-{:08x}",
                            range.first(), range.last()))
    , mCheckpointRange(range)
    , mDownloadDir(downloadDir)
    , mNext(mCheckpointRange.last())
    , mDownloadStart(app.getMetrics().NewMeter(
          {"history", "download-results", "start"}, "event"))
    , mDownloadSuccess(app.getMetrics().NewMeter(
          {"history", "download-results", "success"}, "event"))
    , mDownloadFailure(app.getMetrics().NewMeter(
          {"history", "download-results", "failure"}, "event"))
{
}

DownloadVerifyTxResultsWork::~DownloadVerifyTxResultsWork()
{
    clearChildren();
}

std::string
DownloadVerifyTxResultsWork::getStatus() const
{
    if (mState == WORK_RUNNING || mState == WORK_PENDING)
    {
        auto task = "downloading and verifying tx result files";
        return fmtProgress(mApp, task, mCheckpointRange.first(),
                           mCheckpointRange.last(), mNext);
    }
    return Work::getStatus();
}

bool
DownloadVerifyTxResultsWork::hasNext()
{
    return mNext <= mCheckpointRange.last();
}

void
DownloadVerifyTxResultsWork::resetIter()
{
    mNext = mCheckpointRange.first();
}

std::shared_ptr<BatchableWork>
DownloadVerifyTxResultsWork::yieldMoreWork()
{
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    CLOG(DEBUG, "History")
        << "Downloading, unzipping, and verifying results for " << mNext;

    mDownloadStart.Mark();
    auto verifySnapshot = addWork<VerifyTxResultsSnapWork>(mDownloadDir, mNext);

    mNext += mApp.getHistoryManager().getCheckpointFrequency();
    return verifySnapshot;
}

void
DownloadVerifyTxResultsWork::notify(std::string const& child)
{
    auto work = mChildren.find(child);
    if (work != mChildren.end())
    {
        assert(work->second->isDone());
        if (work->second->getState() == Work::WORK_SUCCESS)
        {
            mDownloadSuccess.Mark();
        }
        else
        {
            mDownloadFailure.Mark();
        }
    }
    BatchWork::notify(child);
}
}
