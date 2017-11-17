// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyLedgerChainWork.h"
#include "herder/LedgerCloseData.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/Progress.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerManager.h"
#include "lib/xdrpp/xdrpp/printer.h"
#include "main/Application.h"
#include "util/format.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ApplyLedgerChainWork::ApplyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    LedgerRange range, LedgerHeaderHistoryEntry& lastApplied)
    : Work(app, parent, std::string("apply-ledger-chain"))
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mCurrSeq(
          mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
    , mLastApplied(lastApplied)
    , mApplyLedgerStart(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "start"}, "event"))
    , mApplyLedgerSkip(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "skip"}, "event"))
    , mApplyLedgerSuccess(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "success"}, "event"))
    , mApplyLedgerFailureInvalidHash(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "failure-invalid-hash"}, "event"))
    , mApplyLedgerFailurePastCurrent(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "failure-past-current"}, "event"))
    , mApplyLedgerFailureInvalidLCLHash(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "failure-LCL-hash"}, "event"))
    , mApplyLedgerFailureInvalidTxSetHash(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "failure-tx-set-hash"}, "event"))
    , mApplyLedgerFailureInvalidResultHash(app.getMetrics().NewMeter(
          {"history", "apply-ledger", "failure-result-hahs"}, "event"))
{
}

ApplyLedgerChainWork::~ApplyLedgerChainWork()
{
    clearChildren();
}

std::string
ApplyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "applying checkpoint";
        return fmtProgress(mApp, task, mRange.first(), mRange.last(), mCurrSeq);
    }
    return Work::getStatus();
}

void
ApplyLedgerChainWork::onReset()
{
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    auto& lm = mApp.getLedgerManager();
    auto& hm = mApp.getHistoryManager();
    CLOG(INFO, "History") << "Replaying contents of "
                          << CheckpointRange{mRange, hm}.count()
                          << " transaction-history files from LCL "
                          << LedgerManager::ledgerAbbrev(
                                 lm.getLastClosedLedgerHeader());
    mCurrSeq =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.first());
    mHdrIn.close();
    mTxIn.close();
}

void
ApplyLedgerChainWork::openCurrentInputFiles()
{
    mHdrIn.close();
    mTxIn.close();
    FileTransferInfo hi(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCurrSeq);
    FileTransferInfo ti(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS, mCurrSeq);
    CLOG(DEBUG, "History") << "Replaying ledger headers from "
                           << hi.localPath_nogz();
    CLOG(DEBUG, "History") << "Replaying transactions from "
                           << ti.localPath_nogz();
    mHdrIn.open(hi.localPath_nogz());
    mTxIn.open(ti.localPath_nogz());
    mTxHistoryEntry = TransactionHistoryEntry();
}

TxSetFramePtr
ApplyLedgerChainWork::getCurrentTxSet()
{
    auto& lm = mApp.getLedgerManager();
    auto seq = lm.getCurrentLedgerHeader().ledgerSeq;

    do
    {
        if (mTxHistoryEntry.ledgerSeq < seq)
        {
            CLOG(DEBUG, "History")
                << "Skipping txset for ledger " << mTxHistoryEntry.ledgerSeq;
        }
        else if (mTxHistoryEntry.ledgerSeq > seq)
        {
            break;
        }
        else
        {
            assert(mTxHistoryEntry.ledgerSeq == seq);
            CLOG(DEBUG, "History") << "Loaded txset for ledger " << seq;
            return std::make_shared<TxSetFrame>(mApp.getNetworkID(),
                                                mTxHistoryEntry.txSet);
        }
    } while (mTxIn && mTxIn.readOne(mTxHistoryEntry));

    CLOG(DEBUG, "History") << "Using empty txset for ledger " << seq;
    return std::make_shared<TxSetFrame>(lm.getLastClosedLedgerHeader().hash);
}

bool
ApplyLedgerChainWork::applyHistoryOfSingleLedger()
{
    LedgerHeaderHistoryEntry hHeader;
    LedgerHeader& header = hHeader.header;

    if (!mHdrIn || !mHdrIn.readOne(hHeader))
    {
        return false;
    }

    mApplyLedgerStart.Mark();

    auto& lm = mApp.getLedgerManager();

    auto const& lclHeader = lm.getLastClosedLedgerHeader();

    // If we are >1 before LCL, skip
    if (header.ledgerSeq + 1 < lclHeader.header.ledgerSeq)
    {
        CLOG(DEBUG, "History")
            << "Catchup skipping old ledger " << header.ledgerSeq;
        mApplyLedgerSkip.Mark();
        return true;
    }

    // If we are one before LCL, check that we knit up with it
    if (header.ledgerSeq + 1 == lclHeader.header.ledgerSeq)
    {
        if (hHeader.hash != lclHeader.header.previousLedgerHash)
        {
            throw std::runtime_error(
                fmt::format("replay of {:s} failed to connect on hash of LCL "
                            "predecessor {:s}",
                            LedgerManager::ledgerAbbrev(hHeader),
                            LedgerManager::ledgerAbbrev(
                                lclHeader.header.ledgerSeq - 1,
                                lclHeader.header.previousLedgerHash)));
        }
        CLOG(DEBUG, "History") << "Catchup at 1-before LCL ("
                               << header.ledgerSeq << "), hash correct";
        mApplyLedgerSkip.Mark();
        return true;
    }

    // If we are at LCL, check that we knit up with it
    if (header.ledgerSeq == lclHeader.header.ledgerSeq)
    {
        if (hHeader.hash != lm.getLastClosedLedgerHeader().hash)
        {
            mApplyLedgerFailureInvalidHash.Mark();
            throw std::runtime_error(
                fmt::format("replay of {:s} at LCL {:s} disagreed on hash",
                            LedgerManager::ledgerAbbrev(hHeader),
                            LedgerManager::ledgerAbbrev(lclHeader)));
        }
        CLOG(DEBUG, "History")
            << "Catchup at LCL=" << header.ledgerSeq << ", hash correct";
        mApplyLedgerSkip.Mark();
        return true;
    }

    // If we are past current, we can't catch up: fail.
    if (header.ledgerSeq != lm.getCurrentLedgerHeader().ledgerSeq)
    {
        mApplyLedgerFailurePastCurrent.Mark();
        throw std::runtime_error(fmt::format(
            "replay overshot current ledger: {:d} > {:d}", header.ledgerSeq,
            lm.getCurrentLedgerHeader().ledgerSeq));
    }

    // If we do not agree about LCL hash, we can't catch up: fail.
    if (header.previousLedgerHash != lm.getLastClosedLedgerHeader().hash)
    {
        mApplyLedgerFailureInvalidLCLHash.Mark();
        throw std::runtime_error(fmt::format(
            "replay at current ledger {:s} disagreed on LCL hash {:s}",
            LedgerManager::ledgerAbbrev(header.ledgerSeq - 1,
                                        header.previousLedgerHash),
            LedgerManager::ledgerAbbrev(lclHeader)));
    }

    auto txset = getCurrentTxSet();
    CLOG(DEBUG, "History") << "Ledger " << header.ledgerSeq << " has "
                           << txset->size() << " transactions";

    // We've verified the ledgerHeader (in the "trusted part of history"
    // sense) in CATCHUP_VERIFY phase; we now need to check that the
    // txhash we're about to apply is the one denoted by that ledger
    // header.
    if (header.scpValue.txSetHash != txset->getContentsHash())
    {
        mApplyLedgerFailureInvalidTxSetHash.Mark();
        throw std::runtime_error(fmt::format(
            "replay txset hash differs from txset hash in replay ledger: hash "
            "for txset for {:d} is {:s}, expected {:s}",
            header.ledgerSeq, hexAbbrev(txset->getContentsHash()),
            hexAbbrev(header.scpValue.txSetHash)));
    }

    LedgerCloseData closeData(header.ledgerSeq, txset, header.scpValue);
    lm.closeLedger(closeData);

    CLOG(DEBUG, "History") << "LedgerManager LCL:\n"
                           << xdr::xdr_to_string(
                                  lm.getLastClosedLedgerHeader());
    CLOG(DEBUG, "History") << "Replay header:\n" << xdr::xdr_to_string(hHeader);
    if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
    {
        mApplyLedgerFailureInvalidResultHash.Mark();
        throw std::runtime_error(fmt::format(
            "replay of {:s} produced mismatched ledger hash {:s}",
            LedgerManager::ledgerAbbrev(hHeader),
            LedgerManager::ledgerAbbrev(lm.getLastClosedLedgerHeader())));
    }

    mApplyLedgerSuccess.Mark();
    mLastApplied = hHeader;
    return true;
}

void
ApplyLedgerChainWork::onStart()
{
    openCurrentInputFiles();
}

void
ApplyLedgerChainWork::onRun()
{
    try
    {
        if (!applyHistoryOfSingleLedger())
        {
            mCurrSeq += mApp.getHistoryManager().getCheckpointFrequency();
            openCurrentInputFiles();
        }
        scheduleSuccess();
    }
    catch (std::runtime_error& e)
    {
        CLOG(ERROR, "History") << "Replay failed: " << e.what();
        scheduleFailure();
    }
}

Work::State
ApplyLedgerChainWork::onSuccess()
{
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);

    auto& lm = mApp.getLedgerManager();
    auto const& lclHeader = lm.getLastClosedLedgerHeader();
    if (lclHeader.header.ledgerSeq == mRange.last())
    {
        return WORK_SUCCESS;
    }

    return WORK_RUNNING;
}
}
