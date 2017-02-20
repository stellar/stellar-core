// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupCompleteWork.h"
#include "history/HistoryManager.h"
#include "historywork/CatchupTransactionsWork.h"
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
        if (mCatchupTransactionsWork)
        {
            return mCatchupTransactionsWork->getStatus();
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
    mCatchupTransactionsWork.reset();
}

Work::State
CatchupCompleteWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    // Phase 2: do the catchup.
    if (!mCatchupTransactionsWork)
    {
        mCatchupTransactionsWork = addWork<CatchupTransactionsWork>(
            *mDownloadDir, firstCheckpointSeq(), lastCheckpointSeq(),
            mManualCatchup, "COMPLETE", "complete",
            0); // never retry
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup COMPLETE to state "
                          << LedgerManager::ledgerAbbrev(
                                 mCatchupTransactionsWork->getLastApplied())
                          << " for nextLedger=" << nextLedger();
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_COMPLETE,
                mCatchupTransactionsWork->getLastApplied());

    return WORK_SUCCESS;
}

void
CatchupCompleteWork::onFailureRaise()
{
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_COMPLETE,
                mCatchupTransactionsWork
                    ? mCatchupTransactionsWork->getLastVerified()
                    : LedgerHeaderHistoryEntry{});
}
}
