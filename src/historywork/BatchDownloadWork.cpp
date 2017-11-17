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
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

BatchDownloadWork::BatchDownloadWork(Application& app, WorkParent& parent,
                                     CheckpointRange range,
                                     std::string const& type,
                                     TmpDir const& downloadDir)
    : Work(app, parent,
           fmt::format("batch-download-{:s}-{:08x}-{:08x}", type, range.first(),
                       range.last()))
    , mRange(range)
    , mNext(mRange.first())
    , mFileType(type)
    , mDownloadDir(downloadDir)
    , mDownloadCached(app.getMetrics().NewMeter(
          {"history", "download-" + type, "cached"}, "event"))
    , mDownloadStart(app.getMetrics().NewMeter(
          {"history", "download-" + type, "start"}, "event"))
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

void
BatchDownloadWork::addNextDownloadWorker()
{
    if (mNext > mRange.last())
    {
        return;
    }

    FileTransferInfo ft(mDownloadDir, mFileType, mNext);
    if (fs::exists(ft.localPath_nogz()))
    {
        CLOG(DEBUG, "History")
            << "already have " << mFileType << " for checkpoint " << mNext;
        mDownloadCached.Mark();
    }
    else
    {
        CLOG(DEBUG, "History") << "Downloading and unzipping " << mFileType
                               << " for checkpoint " << mNext;
        auto getAndUnzip = addWork<GetAndUnzipRemoteFileWork>(ft);
        assert(mRunning.find(getAndUnzip->getUniqueName()) == mRunning.end());
        mRunning.insert(std::make_pair(getAndUnzip->getUniqueName(), mNext));
        mDownloadStart.Mark();
    }
    mNext += mApp.getHistoryManager().getCheckpointFrequency();
}

void
BatchDownloadWork::onReset()
{
    mNext = mRange.first();
    mRunning.clear();
    mFinished.clear();
    clearChildren();
    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mChildren.size() < nChildren && mNext <= mRange.last())
    {
        addNextDownloadWorker();
    }
}

void
BatchDownloadWork::notify(std::string const& child)
{
    auto i = mChildren.find(child);
    if (i == mChildren.end())
    {
        CLOG(WARNING, "Work")
            << "BatchDownloadWork notified by unknown child " << child;
        return;
    }

    switch (i->second->getState())
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

    std::vector<std::string> done;
    for (auto const& c : mChildren)
    {
        if (c.second->getState() == WORK_SUCCESS)
        {
            done.push_back(c.first);
        }
    }
    for (auto const& d : done)
    {
        mChildren.erase(d);
        auto checkpoint = mRunning.find(d);
        assert(checkpoint != mRunning.end());

        CLOG(DEBUG, "History") << "Finished download of " << mFileType
                               << " for checkpoint " << checkpoint->second;

        mFinished.push_back(checkpoint->second);
        mRunning.erase(checkpoint);
        addNextDownloadWorker();
    }
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);
    advance();
}
}
