// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BatchDownloadWork.h"
#include "catchup/CatchupManager.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/Progress.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

BatchDownloadWork::BatchDownloadWork(Application& app, WorkParent& parent,
                                     CheckpointRange range,
                                     std::string const& type,
                                     TmpDir const& downloadDir)
    : BatchWork(app, parent,
                fmt::format("download-{:s}-{:08x}-{:08x}", type, range.first(),
                            range.last()))
    , mRange(range)
    , mNext(mRange.first())
    , mFileType(type)
    , mDownloadDir(downloadDir)
    , mDownloadSuccess(app.getMetrics().NewMeter(
          {"history", "download-" + type, "success"}, "event"))
    , mDownloadFailure(app.getMetrics().NewMeter(
          {"history", "download-" + type, "failure"}, "event"))
{
}

BatchDownloadWork::~BatchDownloadWork()
{
    clearChildren();
}

std::string
BatchDownloadWork::getStatus() const
{
    if (mState == WORK_RUNNING || mState == WORK_PENDING)
    {
        auto task = fmt::format("downloading {:s} files", mFileType);
        return fmtProgress(mApp, task, mRange.first(), mRange.last(), mNext);
    }
    return Work::getStatus();
}

bool
BatchDownloadWork::hasNext()
{
    return mNext <= mRange.last();
}

void
BatchDownloadWork::resetIter()
{
    mNext = mRange.first();
}

std::string
BatchDownloadWork::yieldMoreWork()
{
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    FileTransferInfo ft(mDownloadDir, mFileType, mNext);
    CLOG(DEBUG, "History") << "Downloading and unzipping " << mFileType << " "
                           << mNext;
    auto getAndUnzip = addWork<GetAndUnzipRemoteFileWork>(ft);

    mNext += mApp.getHistoryManager().getCheckpointFrequency();
    return getAndUnzip->getUniqueName();
}

void
BatchDownloadWork::notify(std::string const& child)
{
    auto work = mChildren.find(child);
    if (work != mChildren.end())
    {
        switch (work->second->getState())
        {
        case Work::WORK_SUCCESS:
            mDownloadSuccess.Mark();
            break;
        case Work::WORK_FAILURE_RETRY:
        case Work::WORK_FAILURE_FATAL:
        case Work::WORK_FAILURE_RAISE:
            mDownloadFailure.Mark();
            break;
        default:
            break;
        }
    }

    BatchWork::notify(child);
}
}
