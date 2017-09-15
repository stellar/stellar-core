// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadAndVerifyLedgersWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/BatchDownloadWork.h"
#include "util/Logging.h"

namespace stellar
{

DownloadAndVerifyLedgersWork::DownloadAndVerifyLedgersWork(
    Application& app, WorkParent& parent, CheckpointRange range,
    bool manualCatchup, TmpDir const& downloadDir)
    : Work(app, parent, "download-and-verify-ledgers")
    , mDownloadDir{downloadDir}
    , mRange{std::move(range)}
    , mManualCatchup{manualCatchup}
{
}

std::string
DownloadAndVerifyLedgersWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mVerifyLedgersWork)
        {
            return mVerifyLedgersWork->getStatus();
        }
        else if (mDownloadLedgersWork)
        {
            return mDownloadLedgersWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
DownloadAndVerifyLedgersWork::onReset()
{
    Work::onReset();
    clearChildren();
    mDownloadLedgersWork.reset();
    mVerifyLedgersWork.reset();
}

Work::State
DownloadAndVerifyLedgersWork::onSuccess()
{
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Downloading ledger chain";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            mRange, HISTORY_FILE_TYPE_LEDGER, mDownloadDir);
        return WORK_PENDING;
    }
    assert(mDownloadLedgersWork->getState() == WORK_SUCCESS);

    if (!mVerifyLedgersWork)
    {
        CLOG(INFO, "History") << "Verifying ledger chain";
        mVerifyLedgersWork = addWork<VerifyLedgerChainWork>(
            mDownloadDir, mRange, mManualCatchup);
        return WORK_PENDING;
    }
    assert(mVerifyLedgersWork->getState() == WORK_SUCCESS);

    return WORK_SUCCESS;
}

LedgerHeaderHistoryEntry
DownloadAndVerifyLedgersWork::getFirstVerified() const
{
    return mVerifyLedgersWork ? mVerifyLedgersWork->getFirstVerified()
                              : LedgerHeaderHistoryEntry{};
}

LedgerHeaderHistoryEntry
DownloadAndVerifyLedgersWork::getLastVerified() const
{
    return mVerifyLedgersWork ? mVerifyLedgersWork->getLastVerified()
                              : LedgerHeaderHistoryEntry{};
}
}
