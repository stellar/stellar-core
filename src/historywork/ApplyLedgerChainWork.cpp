// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/ApplyLedgerChainWork.h"
#include "herder/LedgerCloseData.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"
#include "lib/xdrpp/xdrpp/printer.h"
#include "main/Application.h"

namespace stellar
{

ApplyLedgerChainWork::ApplyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    uint32_t first, uint32_t last, LedgerHeaderHistoryEntry& lastApplied)
    : Work(app, parent, std::string("apply-ledger-chain"))
    , mDownloadDir(downloadDir)
    , mFirstSeq(first)
    , mCurrSeq(first)
    , mLastSeq(last)
    , mLastApplied(lastApplied)
{
}

std::string
ApplyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "applying checkpoint";
        return fmtProgress(mApp, task, mFirstSeq, mLastSeq, mCurrSeq);
    }
    return Work::getStatus();
}

void
ApplyLedgerChainWork::onReset()
{
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    uint32_t step = mApp.getHistoryManager().getCheckpointFrequency();
    auto& lm = mApp.getLedgerManager();
    CLOG(INFO, "History") << "Replaying contents of "
                          << (1 + ((mLastSeq - mFirstSeq) / step))
                          << " transaction-history files from LCL "
                          << LedgerManager::ledgerAbbrev(
                                 lm.getLastClosedLedgerHeader());
    mCurrSeq = mFirstSeq;
    mHdrIn.close();
    mTxIn.close();
}

void
ApplyLedgerChainWork::openCurrentInputFiles()
{
    mHdrIn.close();
    mTxIn.close();
    if (mCurrSeq > mLastSeq)
    {
        return;
    }
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
            CLOG(DEBUG, "History") << "Skipping txset for ledger "
                                   << mTxHistoryEntry.ledgerSeq;
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

    auto& lm = mApp.getLedgerManager();

    LedgerHeader const& previousHeader = lm.getLastClosedLedgerHeader().header;

    // If we are >1 before LCL, skip
    if (header.ledgerSeq + 1 < previousHeader.ledgerSeq)
    {
        CLOG(DEBUG, "History") << "Catchup skipping old ledger "
                               << header.ledgerSeq;
        return true;
    }

    // If we are one before LCL, check that we knit up with it
    if (header.ledgerSeq + 1 == previousHeader.ledgerSeq)
    {
        if (hHeader.hash != previousHeader.previousLedgerHash)
        {
            throw std::runtime_error(
                "replay failed to connect on hash of LCL predecessor");
        }
        CLOG(DEBUG, "History") << "Catchup at 1-before LCL ("
                               << header.ledgerSeq << "), hash correct";
        return true;
    }

    // If we are at LCL, check that we knit up with it
    if (header.ledgerSeq == previousHeader.ledgerSeq)
    {
        if (hHeader.hash != lm.getLastClosedLedgerHeader().hash)
        {
            throw std::runtime_error("replay at LCL disagreed on hash");
        }
        CLOG(DEBUG, "History") << "Catchup at LCL=" << header.ledgerSeq
                               << ", hash correct";
        return true;
    }

    // If we are past current, we can't catch up: fail.
    if (header.ledgerSeq != lm.getCurrentLedgerHeader().ledgerSeq)
    {
        throw std::runtime_error("replay overshot current ledger");
    }

    // If we do not agree about LCL hash, we can't catch up: fail.
    if (header.previousLedgerHash != lm.getLastClosedLedgerHeader().hash)
    {
        throw std::runtime_error(
            "replay at current ledger disagreed on LCL hash");
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
        throw std::runtime_error("replay txset hash differs from txset "
                                 "hash in replay ledger");
    }

    LedgerCloseData closeData(header.ledgerSeq, txset, header.scpValue);
    lm.closeLedger(closeData);

    CLOG(DEBUG, "History") << "LedgerManager LCL:\n"
                           << xdr::xdr_to_string(
                                  lm.getLastClosedLedgerHeader());
    CLOG(DEBUG, "History") << "Replay header:\n" << xdr::xdr_to_string(hHeader);
    if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
    {
        throw std::runtime_error("replay produced mismatched ledger hash");
    }
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
    if (mCurrSeq > mLastSeq)
    {
        return WORK_SUCCESS;
    }
    return WORK_RUNNING;
}
}
