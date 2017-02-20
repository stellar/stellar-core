// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupTransactionsWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/ApplyLedgerChainWork.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/VerifyLedgerChainWork.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupTransactionsWork::CatchupTransactionsWork(
    Application& app, WorkParent& parent, TmpDir& downloadDir,
    uint32_t firstSeq, uint32_t lastSeq, bool manualCatchup,
    std::string catchupTypeName, std::string const& name, size_t maxRetries)
    : Work{app, parent, "catchup-transactions-" + name, maxRetries}
    , mDownloadDir(downloadDir)
    , mFirstSeq{firstSeq}
    , mLastSeq{lastSeq}
    , mManualCatchup{manualCatchup}
    , mCatchupTypeName{std::move(catchupTypeName)}
{
}

std::string
CatchupTransactionsWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mVerifyWork)
        {
            return mVerifyWork->getStatus();
        }
        else if (mDownloadTransactionsWork)
        {
            return mDownloadTransactionsWork->getStatus();
        }
        else if (mDownloadLedgersWork)
        {
            return mDownloadLedgersWork->getStatus();
        }
    }
    return Work::getStatus();
}

void
CatchupTransactionsWork::onReset()
{
    mDownloadLedgersWork.reset();
    mDownloadTransactionsWork.reset();
    mVerifyWork.reset();
    mApplyWork.reset();
}

Work::State
CatchupTransactionsWork::onSuccess()
{
    // Phase 1: download and decompress the ledgers.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup " << mCatchupTypeName
                              << " downloading ledgers [" << mFirstSeq << ", "
                              << mLastSeq << "]";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            mFirstSeq, mLastSeq, HISTORY_FILE_TYPE_LEDGER, mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 2: download and decompress the transactions.
    if (!mDownloadTransactionsWork)
    {
        CLOG(INFO, "History") << "Catchup " << mCatchupTypeName
                              << " downloading transactions";
        mDownloadTransactionsWork = addWork<BatchDownloadWork>(
            mFirstSeq, mLastSeq, HISTORY_FILE_TYPE_TRANSACTIONS, mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: verify the ledger chain.
    if (!mVerifyWork)
    {
        CLOG(INFO, "History") << "Catchup " << mCatchupTypeName
                              << " verifying history";
        mLastVerified = mApp.getLedgerManager().getLastClosedLedgerHeader();
        mVerifyWork = addWork<VerifyLedgerChainWork>(
            mDownloadDir, mFirstSeq, mLastSeq, mManualCatchup, mFirstVerified,
            mLastVerified);
        return WORK_PENDING;
    }

    // Phase 4: apply the transactions.
    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup " << mCatchupTypeName
                              << " applying history";
        mApplyWork = addWork<ApplyLedgerChainWork>(mDownloadDir, mFirstSeq,
                                                   mLastSeq, mLastApplied);
        return WORK_PENDING;
    }

    return WORK_SUCCESS;
}

const LedgerHeaderHistoryEntry&
CatchupTransactionsWork::getFirstVerified() const
{
    return mFirstVerified;
}
const LedgerHeaderHistoryEntry&
CatchupTransactionsWork::getLastVerified() const
{
    return mLastVerified;
}
const LedgerHeaderHistoryEntry&
CatchupTransactionsWork::getLastApplied() const
{
    return mLastApplied;
}
}
