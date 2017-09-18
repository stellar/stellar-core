// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/CatchupMinimalWork.h"
#include "catchup/DownloadAndApplyBucketsWork.h"
#include "catchup/DownloadAndVerifyLedgersWork.h"
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
        else if (mDownloadAndVerifyLedgersWork)
        {
            return mDownloadAndVerifyLedgersWork->getStatus();
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
    mDownloadAndVerifyLedgersWork.reset();
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

    // Phase 2: download and verify the ledgers.
    if (!mDownloadAndVerifyLedgersWork)
    {
        CLOG(INFO, "History")
            << "Catchup downloading and veryfing ledger chain for range ["
            << range.first() << ".." << range.last() << "]";
        auto verifyMode = mManualCatchup
                              ? VerifyLedgerMode::DO_NOT_VERIFY_BUFFERED_LEDGERS
                              : VerifyLedgerMode::VERIFY_BUFFERED_LEDGERS;
        mDownloadAndVerifyLedgersWork = addWork<DownloadAndVerifyLedgersWork>(
            range, verifyMode, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: download, verify and apply buckets themselves.
    if (!mDownloadAndApplyBucketsWork)
    {
        CLOG(INFO, "History")
            << "Catchup MINIMAL downloading, verifying and applying buckets";
        std::vector<std::string> buckets =
            mGetHistoryArchiveStateWork->getRemoteState().differingBuckets(
                mLocalState);
        mDownloadAndApplyBucketsWork = addWork<DownloadAndApplyBucketsWork>(
            mGetHistoryArchiveStateWork->getRemoteState(), buckets,
            mDownloadAndVerifyLedgersWork->getFirstVerified(), *mDownloadDir);
        return WORK_PENDING;
    }

    assert(mDownloadAndVerifyLedgersWork->getState() == WORK_SUCCESS);
    assert(mDownloadAndApplyBucketsWork->getState() == WORK_SUCCESS);

    CLOG(INFO, "History")
        << "Completed catchup MINIMAL to state "
        << LedgerManager::ledgerAbbrev(
               mDownloadAndVerifyLedgersWork->getFirstVerified())
        << " for nextLedger=" << nextLedger();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL,
                mDownloadAndVerifyLedgersWork->getFirstVerified());

    return WORK_SUCCESS;
}

void
CatchupMinimalWork::onFailureRaise()
{
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL,
                mDownloadAndVerifyLedgersWork
                    ? mDownloadAndVerifyLedgersWork->getLastVerified()
                    : LedgerHeaderHistoryEntry{});
}

LedgerHeaderHistoryEntry
CatchupMinimalWork::getFirstVerified() const
{
    return mVerifyLedgersWork ? mVerifyLedgersWork->getFirstVerified()
                              : LedgerHeaderHistoryEntry{};
}
}
