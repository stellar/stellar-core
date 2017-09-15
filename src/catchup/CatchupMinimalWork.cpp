// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupMinimalWork.h"
#include "catchup/DownloadAndApplyBucketsWork.h"
#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/VerifyBucketWork.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/Logging.h"

namespace stellar
{

CatchupMinimalWork::CatchupMinimalWork(Application& app, WorkParent& parent,
                                       uint32_t initLedger, bool manualCatchup,
                                       handler endHandler)
    : CatchupWork(app, parent, initLedger, "minimal", manualCatchup)
    , mEndHandler(endHandler)
{
}

uint32_t
CatchupMinimalWork::firstCheckpointSeq() const
{
    auto firstLedger = mInitLedger > mApp.getConfig().CATCHUP_RECENT
                           ? (mInitLedger - mApp.getConfig().CATCHUP_RECENT)
                           : 0;
    return mApp.getHistoryManager().nextCheckpointLedger(firstLedger) - 1;
}

std::string
CatchupMinimalWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mDownloadAndApplyBucketsWork)
        {
            return mDownloadAndApplyBucketsWork->getStatus();
        }
        else if (mVerifyLedgersWork)
        {
            return mVerifyLedgersWork->getStatus();
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

void
CatchupMinimalWork::onReset()
{
    CatchupWork::onReset();
    mDownloadLedgersWork.reset();
    mVerifyLedgersWork.reset();
    mDownloadAndApplyBucketsWork.reset();
}

Work::State
CatchupMinimalWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    auto range = CheckpointRange{firstCheckpointSeq(), lastCheckpointSeq()};

    // Phase 2: download the ledger chain that validates the state
    // we're about to assume.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL downloading ledger chain";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            range, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: Verify the ledger chain
    if (!mVerifyLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL verifying ledger chain";
        mVerifyLedgersWork = addWork<VerifyLedgerChainWork>(
            *mDownloadDir, range, mManualCatchup);
        return WORK_PENDING;
    }

    // Phase 4: download, verify and apply buckets themselves.
    if (!mDownloadAndApplyBucketsWork)
    {
        CLOG(INFO, "History")
            << "Catchup MINIMAL downloading, verifying and applying buckets";
        std::vector<std::string> buckets =
            mGetHistoryArchiveStateWork->getRemoteState().differingBuckets(
                mLocalState);
        mDownloadAndApplyBucketsWork = addWork<DownloadAndApplyBucketsWork>(
            mGetHistoryArchiveStateWork->getRemoteState(), buckets,
            mVerifyLedgersWork->getFirstVerified(), *mDownloadDir);
        return WORK_PENDING;
    }

    assert(mDownloadLedgersWork->getState() == WORK_SUCCESS);
    assert(mVerifyLedgersWork->getState() == WORK_SUCCESS);
    assert(mDownloadAndApplyBucketsWork->getState() == WORK_SUCCESS);

    CLOG(INFO, "History") << "Completed catchup MINIMAL to state "
                          << LedgerManager::ledgerAbbrev(
                                 mVerifyLedgersWork->getFirstVerified())
                          << " for nextLedger=" << nextLedger();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL,
                mVerifyLedgersWork->getFirstVerified());

    return WORK_SUCCESS;
}

void
CatchupMinimalWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL,
                mVerifyLedgersWork ? mVerifyLedgersWork->getLastVerified()
                                   : LedgerHeaderHistoryEntry{});
}

LedgerHeaderHistoryEntry
CatchupMinimalWork::getFirstVerified() const
{
    return mVerifyLedgersWork ? mVerifyLedgersWork->getFirstVerified()
                              : LedgerHeaderHistoryEntry{};
}
}
