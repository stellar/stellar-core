// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadVerifyTxResultsWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryArchive.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/Progress.h"
#include "historywork/VerifyTxResultsWork.h"
#include "work/WorkSequence.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

DownloadVerifyTxResultsWork::DownloadVerifyTxResultsWork(
    Application& app, CheckpointRange range, TmpDir const& downloadDir,
    std::shared_ptr<HistoryArchive> archive)
    : BatchWork(app, "download-verify-txresults")
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mCurrCheckpoint(range.mFirst)
    , mArchive(archive)
{
}

std::string
DownloadVerifyTxResultsWork::getStatus() const
{
    if (!isDone() && !isAborting())
    {
        auto task = fmt::format("Downloading and verifying {:s} files",
                                HISTORY_FILE_TYPE_RESULTS);
        return fmtProgress(mApp, task, mRange.getLedgerRange(),
                           mCurrCheckpoint);
    }
    return BatchWork::getStatus();
}

bool
DownloadVerifyTxResultsWork::hasNext() const
{
    return mCurrCheckpoint < mRange.limit();
}

void
DownloadVerifyTxResultsWork::resetIter()
{
    mCurrCheckpoint = mRange.mFirst;
}

std::shared_ptr<BasicWork>
DownloadVerifyTxResultsWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("Nothing to iterate over!");
    }

    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_RESULTS,
                        mCurrCheckpoint);
    auto w1 = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);
    auto w2 = std::make_shared<VerifyTxResultsWork>(mApp, mDownloadDir,
                                                    mCurrCheckpoint);
    std::vector<std::shared_ptr<BasicWork>> seq{w1, w2};
    auto w3 = std::make_shared<WorkSequence>(
        mApp,
        fmt::format(FMT_STRING("download-verify-results-{}"), mCurrCheckpoint),
        seq);

    mCurrCheckpoint += mApp.getHistoryManager().getCheckpointFrequency();
    return w3;
}
}
