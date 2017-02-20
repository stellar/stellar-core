// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupCompleteImmediateWork.h"
#include "history/HistoryManager.h"
#include "historywork/CatchupTransactionsWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupCompleteImmediateWork::CatchupCompleteImmediateWork(Application& app,
                                                           WorkParent& parent,
                                                           uint32_t initLedger,
                                                           bool manualCatchup,
                                                           handler endHandler)
    : CatchupWork(app, parent, initLedger, "complete-immediate", manualCatchup)
    , mEndHandler(endHandler)
{
}

std::string
CatchupCompleteImmediateWork::getStatus() const
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
CatchupCompleteImmediateWork::archiveStateSeq() const
{
    return 0;
}

uint32_t
CatchupCompleteImmediateWork::firstCheckpointSeq() const
{
    auto firstLedger = mLocalState.currentLedger;
    return mApp.getHistoryManager().nextCheckpointLedger(firstLedger) - 1;
}

uint32_t
CatchupCompleteImmediateWork::lastCheckpointSeq() const
{
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    return mApp.getHistoryManager().nextCheckpointLedger(
               mRemoteState.currentLedger) -
           1;
}

void
CatchupCompleteImmediateWork::onReset()
{
    CatchupWork::onReset();
    mCatchupTransactionsWork.reset();
}

Work::State
CatchupCompleteImmediateWork::onSuccess()
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
            mManualCatchup, "COMPLETE_IMMEDIATE", "complete-immediate",
            0); // never retry
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup COMPLETE_IMMEDIATE to state "
                          << LedgerManager::ledgerAbbrev(
                                 mCatchupTransactionsWork->getLastApplied());
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_COMPLETE_IMMEDIATE,
                mCatchupTransactionsWork->getLastApplied());

    return WORK_SUCCESS;
}

void
CatchupCompleteImmediateWork::onFailureRaise()
{
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_COMPLETE_IMMEDIATE,
                mCatchupTransactionsWork
                    ? mCatchupTransactionsWork->getLastVerified()
                    : LedgerHeaderHistoryEntry{});
}
}
