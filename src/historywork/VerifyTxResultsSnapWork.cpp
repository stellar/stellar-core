// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyTxResultsSnapWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"

namespace stellar
{
VerifyTxResultsSnapWork::VerifyTxResultsSnapWork(Application& app,
                                                 WorkParent& parent,
                                                 TmpDir const& downloadDir,
                                                 uint32_t checkpoint)
    : BatchableWork(app, parent, fmt::format("verify-results-{:d}", checkpoint))
    , mCheckpoint(checkpoint)
    , mDownloadDir(downloadDir)
{
}

VerifyTxResultsSnapWork::~VerifyTxResultsSnapWork()
{
    clearChildren();
}

std::string
VerifyTxResultsSnapWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mDownloadVerifyWork)
        {
            return mDownloadVerifyWork->getStatus();
        }
    }

    return BatchableWork::getStatus();
}

void
VerifyTxResultsSnapWork::onReset()
{
    mDownloadVerifyWork.reset();
}

Work::State
VerifyTxResultsSnapWork::onSuccess()
{
    if (!downloadVerifyTxResults())
    {
        return Work::WORK_PENDING;
    }
    return Work::WORK_SUCCESS;
}

void
VerifyTxResultsSnapWork::unblockWork(BatchableWorkResultData const& data)
{
    // Do nothing: tx results verification is independent of other checkpoints
}

bool
VerifyTxResultsSnapWork::downloadVerifyTxResults()
{
    if (mDownloadVerifyWork)
    {
        assert(mDownloadVerifyWork->getState() == Work::WORK_SUCCESS);
        return true;
    }

    CLOG(DEBUG, "History")
        << "Downloading and verifying tx results for checkpoint "
        << mCheckpoint;
    FileTransferInfo ft{mDownloadDir, HISTORY_FILE_TYPE_RESULTS, mCheckpoint};

    mDownloadVerifyWork =
        addWork<VerifyTxResultsWork>(mDownloadDir, mCheckpoint);
    mDownloadVerifyWork->addWork<GetAndUnzipRemoteFileWork>(ft);

    return false;
}
}
