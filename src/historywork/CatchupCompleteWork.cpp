// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupCompleteWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/ApplyLedgerChainWork.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/VerifyLedgerChainWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupCompleteWork::CatchupCompleteWork(Application& app, WorkParent& parent,
                                         uint32_t initLedger,
                                         bool manualCatchup, handler endHandler)
    : CatchupWork(app, parent, initLedger, "complete", manualCatchup)
    , mEndHandler(endHandler)
{
}

std::string
CatchupCompleteWork::getStatus() const
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
        else if (mGetHistoryArchiveStateWork)
        {
            return mGetHistoryArchiveStateWork->getStatus();
        }
    }
    return Work::getStatus();
}

uint32_t
CatchupCompleteWork::firstCheckpointSeq() const
{
    auto firstLedger = mLocalState.currentLedger;
    return mApp.getHistoryManager().nextCheckpointLedger(firstLedger) - 1;
}

void
CatchupCompleteWork::onReset()
{
    CatchupWork::onReset();
    mDownloadLedgersWork.reset();
    mDownloadTransactionsWork.reset();
    mVerifyWork.reset();
    mApplyWork.reset();
}

Work::State
CatchupCompleteWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    uint32_t firstSeq = firstCheckpointSeq();
    uint32_t lastSeq = lastCheckpointSeq();

    // Phase 2: download and decompress the ledgers.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE downloading ledgers ["
                              << firstSeq << ", " << lastSeq << "]";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: download and decompress the transactions.
    if (!mDownloadTransactionsWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE downloading transactions";
        mDownloadTransactionsWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_TRANSACTIONS, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 4: verify the ledger chain.
    if (!mVerifyWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE verifying history";
        mLastVerified = mApp.getLedgerManager().getLastClosedLedgerHeader();
        mVerifyWork = addWork<VerifyLedgerChainWork>(
            *mDownloadDir, firstSeq, lastSeq, mManualCatchup, mFirstVerified,
            mLastVerified);
        return WORK_PENDING;
    }

    // Phase 5: apply the transactions.
    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup COMPLETE applying history";
        mApplyWork = addWork<ApplyLedgerChainWork>(*mDownloadDir, firstSeq,
                                                   lastSeq, mLastApplied);
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup COMPLETE to state "
                          << LedgerManager::ledgerAbbrev(mLastApplied)
                          << " for nextLedger=" << nextLedger();
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, HistoryManager::CATCHUP_COMPLETE, mLastApplied);

    return WORK_SUCCESS;
}

void
CatchupCompleteWork::onFailureRaise()
{
    mApp.getHistoryManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, HistoryManager::CATCHUP_COMPLETE, mLastVerified);
}
}
