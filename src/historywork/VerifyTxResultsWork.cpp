// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyTxResultsWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "main/Application.h"
#include <ledger/LedgerManager.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

VerifyTxResultsWork::VerifyTxResultsWork(Application& app, WorkParent& parent,
                                         TmpDir const& downloadDir,
                                         uint32_t checkpoint)
    : Work(app, parent, fmt::format("verify-results-{:d}", checkpoint))
    , mDownloadDir(downloadDir)
    , mCheckpoint(checkpoint)
    , mVerifyTxResultSetStart(app.getMetrics().NewMeter(
          {"history", "verify-results", "start"}, "event"))
    , mVerifyTxResultSetSuccess(app.getMetrics().NewMeter(
          {"history", "verify-results", "success"}, "event"))
    , mVerifyTxResultSetFailure(app.getMetrics().NewMeter(
          {"history", "verify-results", "failure"}, "event"))
{
}

VerifyTxResultsWork::~VerifyTxResultsWork()
{
    clearChildren();
}

void
VerifyTxResultsWork::onReset()
{
    mCurrLedger = mApp.getHistoryManager().prevCheckpointLedger(mCheckpoint);
    if (mCurrLedger == 0)
    {
        mCurrLedger = LedgerManager::GENESIS_LEDGER_SEQ;
    }
}

void
VerifyTxResultsWork::onStart()
{
    CLOG(DEBUG, "History") << "Verifying transaction results for checkpoint "
                           << mCheckpoint;
    mHdrIn.close();
    mResIn.close();

    FileTransferInfo hi(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCheckpoint);
    FileTransferInfo ri(mDownloadDir, HISTORY_FILE_TYPE_RESULTS, mCheckpoint);
    mHdrIn.open(hi.localPath_nogz());
    mResIn.open(ri.localPath_nogz());

    mTxResultEntry = TransactionHistoryResultEntry();
    mVerifyTxResultSetStart.Mark();
}

void
VerifyTxResultsWork::getTxResultSetForLedger(uint32_t seqNum)
{
    do
    {
        if (mTxResultEntry.ledgerSeq < seqNum)
        {
            CLOG(DEBUG, "History") << "Skipping tx results for ledger "
                                   << mTxResultEntry.ledgerSeq;
        }
        else if (mTxResultEntry.ledgerSeq >= seqNum)
        {
            // Loaded tx results or no tx results for this ledger
            CLOG(DEBUG, "History")
                << "Loaded tx result set for ledger " << seqNum;
            break;
        }
    } while (mResIn && mResIn.readOne(mTxResultEntry));
}

void
VerifyTxResultsWork::verifyTxResultsForSingleLedger()
{
    LedgerHeaderHistoryEntry hHeader;
    LedgerHeader& header = hHeader.header;

    if (!mHdrIn || !mHdrIn.readOne(hHeader))
    {
        throw std::runtime_error(
            fmt::format("Unable to read ledger header file for checkpoint {:d}",
                        mCheckpoint));
    }

    if (header.ledgerSeq != mCurrLedger)
    {
        throw std::runtime_error(
            fmt::format("Expected to parse ledger {:d} but got {:d} instead",
                        mCurrLedger, header.ledgerSeq));
    }

    getTxResultSetForLedger(header.ledgerSeq);
    auto resultSetHash = sha256(xdr::xdr_to_opaque(mTxResultEntry.txResultSet));

    if (resultSetHash != header.txSetResultHash)
    {
        throw std::runtime_error(
            fmt::format("Hash of result set does not agree with result "
                        "hash in ledger header: "
                        "txset result hash for {:d} is {:s}, but expected {:s}",
                        header.ledgerSeq, hexAbbrev(resultSetHash),
                        hexAbbrev(header.txSetResultHash)));
    }
}

Work::State
VerifyTxResultsWork::onSuccess()
{
    try
    {
        verifyTxResultsForSingleLedger();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "Tx Result verification failed: " << e.what();
        mVerifyTxResultSetFailure.Mark();
        return WORK_FAILURE_FATAL;
    }

    if (mCurrLedger == mCheckpoint)
    {
        mVerifyTxResultSetSuccess.Mark();
        return WORK_SUCCESS;
    }

    mCurrLedger += 1;
    return WORK_RUNNING;
}
}
