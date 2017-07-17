// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BatchDownloadWork.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/Progress.h"
#include "lib/util/format.h"
#include "main/Application.h"

namespace stellar
{

BatchDownloadWork::BatchDownloadWork(Application& app, WorkParent& parent,
                                     uint32_t first, uint32_t last,
                                     std::string const& type,
                                     TmpDir const& downloadDir)
    : Work(app, parent,
           fmt::format("batch-download-{:s}-{:08x}-{:08x}", type, first, last))
    , mFirst(first)
    , mLast(last)
    , mNext(first)
    , mFileType(type)
    , mDownloadDir(downloadDir)
{
}

std::string
BatchDownloadWork::getStatus() const
{
    if (mState == WORK_RUNNING || mState == WORK_PENDING)
    {
        auto task = fmt::format("downloading {:s} files", mFileType);
        return fmtProgress(mApp, task, mFirst, mLast, mNext);
    }
    return Work::getStatus();
}

void
BatchDownloadWork::addNextDownloadWorker()
{
    if (mNext > mLast)
    {
        return;
    }

    FileTransferInfo ft(mDownloadDir, mFileType, mNext);
    if (fs::exists(ft.localPath_nogz()))
    {
        CLOG(DEBUG, "History") << "already have " << mFileType
                               << " for checkpoint " << mNext;
    }
    else
    {
        CLOG(DEBUG, "History") << "Downloading and unzipping " << mFileType
                               << " for checkpoint " << mNext;
        auto getAndUnzip = addWork<GetAndUnzipRemoteFileWork>(ft);
        assert(mRunning.find(getAndUnzip->getUniqueName()) == mRunning.end());
        mRunning.insert(std::make_pair(getAndUnzip->getUniqueName(), mNext));
    }
    mNext += mApp.getHistoryManager().getCheckpointFrequency();
}

void
BatchDownloadWork::onReset()
{
    mNext = mFirst;
    mRunning.clear();
    mFinished.clear();
    clearChildren();
    size_t nChildren = mApp.getConfig().MAX_CONCURRENT_SUBPROCESSES;
    while (mChildren.size() < nChildren && mNext <= mLast)
    {
        addNextDownloadWorker();
    }
}

void
BatchDownloadWork::notify(std::string const& childChanged)
{
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
        auto i = mRunning.find(d);
        assert(i != mRunning.end());

        CLOG(DEBUG, "History") << "Finished download of " << mFileType
                               << " for checkpoint " << i->second;

        mFinished.push_back(i->second);
        mRunning.erase(i);
        addNextDownloadWorker();
    }
    mApp.getHistoryManager().logAndUpdateStatus(true);
    advance();
}
}
