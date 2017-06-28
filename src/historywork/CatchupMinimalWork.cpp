// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/CatchupMinimalWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/ApplyBucketsWork.h"
#include "historywork/BatchDownloadWork.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "historywork/VerifyBucketWork.h"
#include "historywork/VerifyLedgerChainWork.h"
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
        if (mApplyWork)
        {
            return mApplyWork->getStatus();
        }
        else if (mDownloadBucketsWork)
        {
            return mDownloadBucketsWork->getStatus();
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
    mDownloadBucketsWork.reset();
    mApplyWork.reset();
}

Work::State
CatchupMinimalWork::onSuccess()
{
    // Phase 1 starts automatically in base class: CatchupWork::onReset
    // If we get here, phase 1 should be complete and we're moving on.
    assert(mGetHistoryArchiveStateWork);
    assert(mGetHistoryArchiveStateWork->getState() == WORK_SUCCESS);

    auto firstSeq = firstCheckpointSeq();
    auto lastSeq = lastCheckpointSeq();

    // Phase 2: download the ledger chain that validates the state
    // we're about to assume.
    if (!mDownloadLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL downloading ledger chain";
        mDownloadLedgersWork = addWork<BatchDownloadWork>(
            firstSeq, lastSeq, HISTORY_FILE_TYPE_LEDGER, *mDownloadDir);
        return WORK_PENDING;
    }

    // Phase 3: Verify the ledger chain
    if (!mVerifyLedgersWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL verifying ledger chain";
        mVerifyLedgersWork = addWork<VerifyLedgerChainWork>(
            *mDownloadDir, firstSeq, lastSeq, mManualCatchup, mFirstVerified,
            mLastVerified);
        return WORK_PENDING;
    }

    // Phase 4: download and verify the buckets themselves.
    if (!mDownloadBucketsWork)
    {
        CLOG(INFO, "History")
            << "Catchup MINIMAL downloading and verifying buckets";
        std::vector<std::string> buckets =
            mRemoteState.differingBuckets(mLocalState);
        mDownloadBucketsWork = addWork<Work>("download and verify buckets");
        for (auto const& hash : buckets)
        {
            FileTransferInfo ft(*mDownloadDir, HISTORY_FILE_TYPE_BUCKET, hash);
            // Each bucket gets its own work-chain of download->gunzip->verify

            auto verify = mDownloadBucketsWork->addWork<VerifyBucketWork>(
                mBuckets, ft.localPath_nogz(), hexToBin256(hash));
            verify->addWork<GetAndUnzipRemoteFileWork>(ft);
        }
        return WORK_PENDING;
    }

    assert(mDownloadLedgersWork->getState() == WORK_SUCCESS);
    assert(mVerifyLedgersWork->getState() == WORK_SUCCESS);
    assert(mDownloadBucketsWork->getState() == WORK_SUCCESS);

    // Phase 3: apply the buckets.
    if (!mApplyWork)
    {
        CLOG(INFO, "History") << "Catchup MINIMAL applying buckets for state "
                              << LedgerManager::ledgerAbbrev(mFirstVerified);
        mApplyWork =
            addWork<ApplyBucketsWork>(mBuckets, mRemoteState, mFirstVerified);
        return WORK_PENDING;
    }

    CLOG(INFO, "History") << "Completed catchup MINIMAL to state "
                          << LedgerManager::ledgerAbbrev(mFirstVerified)
                          << " for nextLedger=" << nextLedger();
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec;
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL, mFirstVerified);

    return WORK_SUCCESS;
}

void
CatchupMinimalWork::onFailureRaise()
{
    mApp.getCatchupManager().historyCaughtup();
    asio::error_code ec = std::make_error_code(std::errc::timed_out);
    mEndHandler(ec, CatchupManager::CATCHUP_MINIMAL, mLastVerified);
}
}
