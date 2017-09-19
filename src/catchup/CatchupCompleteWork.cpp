// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupCompleteWork.h"
#include "catchup/CatchupTransactionsWork.h"
#include "history/HistoryManager.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupCompleteWork::CatchupCompleteWork(Application& app, WorkParent& parent,
                                         uint32_t initLedger,
                                         bool manualCatchup,
                                         ProgressHandler progressHandler)
    : CatchupWork(app, parent, initLedger, "complete", manualCatchup)
    , mProgressHandler(progressHandler)
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

    auto range = CheckpointRange{firstCheckpointSeq(), lastCheckpointSeq()};

    // Phase 2: do the catchup.
    if (!mCatchupTransactionsWork)
    {
        mCatchupTransactionsWork = addWork<CatchupTransactionsWork>(
            *mDownloadDir, range, mManualCatchup, "COMPLETE", "complete",
            0); // never retry
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup COMPLETE to state "
                          << LedgerManager::ledgerAbbrev(
                                 mCatchupTransactionsWork->getLastApplied())
                          << " for nextLedger=" << nextLedger();
    asio::error_code ec;
    mProgressHandler(ec, ProgressState::APPLIED_TRANSACTIONS,
                     mCatchupTransactionsWork->getLastApplied());
    mProgressHandler(ec, ProgressState::FINISHED,
                     mCatchupTransactionsWork->getLastApplied());

    return WORK_SUCCESS;
}

void
CatchupCompleteWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mProgressHandler(ec, ProgressState::FINISHED,
                     mCatchupTransactionsWork
                         ? mCatchupTransactionsWork->getLastVerified()
                         : LedgerHeaderHistoryEntry{});
}

LedgerHeaderHistoryEntry
CatchupCompleteWork::getLastApplied() const
{
    return mCatchupTransactionsWork ? mCatchupTransactionsWork->getLastApplied()
                                    : LedgerHeaderHistoryEntry{};
}
}
