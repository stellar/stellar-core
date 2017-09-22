// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadAndApplyTransactionsWork.h"
#include "catchup/ApplyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/BatchDownloadWork.h"
#include "util/Logging.h"

namespace stellar
{

DownloadAndApplyTransactionsWork::DownloadAndApplyTransactionsWork(
    Application& app, WorkParent& parent, CheckpointRange range,
    TmpDir const& downloadDir)
    : Work(app, parent, "download-and-apply-transactions")
    , mDownloadDir{downloadDir}
    , mRange{std::move(range)}
{
}

std::string
DownloadAndApplyTransactionsWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mDownloadTransactionsWork)
        {
            return mDownloadTransactionsWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
DownloadAndApplyTransactionsWork::onReset()
{
    Work::onReset();
    clearChildren();
    mDownloadTransactionsWork.reset();
    mApplyWork.reset();
}

Work::State
DownloadAndApplyTransactionsWork::onSuccess()
{
    if (!mDownloadTransactionsWork)
    {
        CLOG(INFO, "History") << "Catchup downloading transactions";
        mDownloadTransactionsWork = addWork<BatchDownloadWork>(
            mRange, HISTORY_FILE_TYPE_TRANSACTIONS, mDownloadDir);
        return WORK_PENDING;
    }
    assert(mDownloadTransactionsWork->getState() == WORK_SUCCESS);

    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup applying history";
        mApplyWork = addWork<ApplyLedgerChainWork>(mDownloadDir, mRange);
        return WORK_PENDING;
    }
    assert(mApplyWork->getState() == WORK_SUCCESS);

    return WORK_SUCCESS;
}

LedgerHeaderHistoryEntry
DownloadAndApplyTransactionsWork::getLastApplied() const
{
    assert(mApplyWork);
    return mApplyWork->getLastApplied();
}
}
