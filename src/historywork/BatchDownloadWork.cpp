// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/BatchDownloadWork.h"
#include "catchup/CatchupManager.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/Progress.h"
#include "main/Application.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{
BatchDownloadWork::BatchDownloadWork(Application& app, CheckpointRange range,
                                     std::string const& type,
                                     TmpDir const& downloadDir,
                                     std::shared_ptr<HistoryArchive> archive)
    : BatchWork(app, fmt::format("batch-download-{:s}-{:08x}-{:08x}", type,
                                 range.mFirst, range.limit()))
    , mRange(range)
    , mNext(range.mFirst)
    , mFileType(type)
    , mDownloadDir(downloadDir)
    , mArchive(archive)
{
}

std::string
BatchDownloadWork::getStatus() const
{
    if (!isDone() && !isAborting())
    {
        auto task = fmt::format("downloading {:s} files", mFileType);
        return fmtProgress(mApp, task, mRange.getLedgerRange(), mNext);
    }
    return BatchWork::getStatus();
}

std::shared_ptr<BasicWork>
BatchDownloadWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        CLOG_WARNING(Work, "{} has no more children to iterate over! ",
                     getName());
        return nullptr;
    }

    FileTransferInfo ft(mDownloadDir, mFileType, mNext);
    CLOG_DEBUG(History, "Downloading and unzipping {} for checkpoint {}",
               mFileType, mNext);
    auto getAndUnzip =
        std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);
    mNext += mApp.getHistoryManager().getCheckpointFrequency();

    return getAndUnzip;
}

bool
BatchDownloadWork::hasNext() const
{
    return mNext < mRange.limit();
}

void
BatchDownloadWork::resetIter()
{
    mNext = mRange.mFirst;
}
}
