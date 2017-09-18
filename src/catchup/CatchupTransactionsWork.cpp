// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupTransactionsWork.h"
#include "catchup/DownloadAndApplyTransactionsWork.h"
#include "catchup/DownloadAndVerifyLedgersWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupTransactionsWork::CatchupTransactionsWork(
    Application& app, WorkParent& parent, TmpDir& downloadDir,
    CheckpointRange range, bool manualCatchup, std::string catchupTypeName,
    std::string const& name, size_t maxRetries)
    : Work{app, parent, "catchup-transactions-" + name, maxRetries}
    , mDownloadDir(downloadDir)
    , mRange{range}
    , mManualCatchup{manualCatchup}
    , mCatchupTypeName{std::move(catchupTypeName)}
{
}

std::string
CatchupTransactionsWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mDownloadAndApplyTransactionsWork)
        {
            return mDownloadAndApplyTransactionsWork->getStatus();
        }
        else if (mDownloadAndVerifyLedgersWork)
        {
            return mDownloadAndVerifyLedgersWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
CatchupTransactionsWork::onReset()
{
    mDownloadAndVerifyLedgersWork.reset();
    mDownloadAndApplyTransactionsWork.reset();
}

Work::State
CatchupTransactionsWork::onSuccess()
{
    // Phase 1: download and verify the ledgers.
    if (!mDownloadAndVerifyLedgersWork)
    {
        CLOG(INFO, "History")
            << "Catchup downloading and veryfing ledger chain for range ["
            << mRange.first() << ".." << mRange.last() << "]";
        mDownloadAndVerifyLedgersWork = addWork<DownloadAndVerifyLedgersWork>(
            mRange, mManualCatchup, mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 2: download and apply the transactions.
    if (!mDownloadAndApplyTransactionsWork)
    {
        CLOG(INFO, "History")
            << "Catchup downloading and applying transactions for range ["
            << mRange.first() << ".." << mRange.last() << "]";
        mDownloadAndApplyTransactionsWork =
            addWork<DownloadAndApplyTransactionsWork>(mRange, mDownloadDir);
        return WORK_PENDING;
    }

    return WORK_SUCCESS;
}

LedgerHeaderHistoryEntry
CatchupTransactionsWork::getFirstVerified() const
{
    return mDownloadAndVerifyLedgersWork
               ? mDownloadAndVerifyLedgersWork->getFirstVerified()
               : LedgerHeaderHistoryEntry{};
}

LedgerHeaderHistoryEntry
CatchupTransactionsWork::getLastVerified() const
{
    return mDownloadAndVerifyLedgersWork
               ? mDownloadAndVerifyLedgersWork->getLastVerified()
               : LedgerHeaderHistoryEntry{};
}

LedgerHeaderHistoryEntry
CatchupTransactionsWork::getLastApplied() const
{
    return mDownloadAndApplyTransactionsWork
               ? mDownloadAndApplyTransactionsWork->getLastApplied()
               : LedgerHeaderHistoryEntry{};
}
}
