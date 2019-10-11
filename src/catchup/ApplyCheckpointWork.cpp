// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyCheckpointWork.h"
#include "bucket/BucketManager.h"
#include "herder/LedgerCloseData.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/Progress.h"
#include "invariant/InvariantDoesNotHold.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerManager.h"
#include "lib/xdrpp/xdrpp/printer.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/FileSystemException.h"

#include <lib/util/format.h>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

ApplyCheckpointWork::ApplyCheckpointWork(Application& app,
                                         TmpDir const& downloadDir,
                                         LedgerRange const& range)
    : BasicWork(app,
                "apply-ledgers-" +
                    fmt::format("{}-{}", range.mFirst, range.mLast),
                BasicWork::RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mLedgerRange(range)
    , mCheckpoint(
          app.getHistoryManager().checkpointContainingLedger(range.mFirst))
    , mApplyLedgerSuccess(app.getMetrics().NewMeter(
          {"history", "apply-ledger-chain", "success"}, "event"))
    , mApplyLedgerFailure(app.getMetrics().NewMeter(
          {"history", "apply-ledger-chain", "failure"}, "event"))
{
    // Ledger range check to enforce application of a single checkpoint
    auto const& hm = mApp.getHistoryManager();
    auto low = std::max(LedgerManager::GENESIS_LEDGER_SEQ,
                        hm.prevCheckpointLedger(mCheckpoint));
    if (mLedgerRange.mFirst != low)
    {
        throw std::runtime_error(
            "Ledger range start must be aligned with checkpoint start");
    }
    if (mLedgerRange.mLast > mCheckpoint)
    {
        throw std::runtime_error(
            "Ledger range must span at most 1 checkpoint worth of ledgers");
    }
}

std::string
ApplyCheckpointWork::getStatus() const
{
    if (getState() == State::WORK_RUNNING)
    {
        auto lcl = mApp.getLedgerManager().getLastClosedLedgerNum();
        return fmt::format("Last applied ledger: {}", lcl);
    }
    return BasicWork::getStatus();
}

void
ApplyCheckpointWork::onReset()
{
    mHdrIn.close();
    mTxIn.close();
    mFilesOpen = false;
}

void
ApplyCheckpointWork::openInputFiles()
{
    mHdrIn.close();
    mTxIn.close();
    FileTransferInfo hi(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCheckpoint);
    FileTransferInfo ti(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS,
                        mCheckpoint);
    CLOG(DEBUG, "History") << "Replaying ledger headers from "
                           << hi.localPath_nogz();
    CLOG(DEBUG, "History") << "Replaying transactions from "
                           << ti.localPath_nogz();
    mHdrIn.open(hi.localPath_nogz());
    mTxIn.open(ti.localPath_nogz());
    mTxHistoryEntry = TransactionHistoryEntry();
    mFilesOpen = true;
}

TxSetFramePtr
ApplyCheckpointWork::getCurrentTxSet()
{
    auto& lm = mApp.getLedgerManager();
    auto seq = lm.getLastClosedLedgerNum() + 1;

    // Check mTxHistoryEntry prior to loading next history entry.
    // This order is important because it accounts for ledger "gaps"
    // in the history archives (which are caused by ledgers with empty tx
    // sets, as those are not uploaded).
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
ApplyCheckpointWork::applyHistoryOfSingleLedger()
{
    LedgerHeaderHistoryEntry hHeader;
    LedgerHeader& header = hHeader.header;

    if (!mHdrIn || !mHdrIn.readOne(hHeader))
    {
        return false;
    }

    auto& lm = mApp.getLedgerManager();

    auto const& lclHeader = lm.getLastClosedLedgerHeader();

    // If we are >1 before LCL, skip
    if (header.ledgerSeq + 1 < lclHeader.header.ledgerSeq)
    {
        CLOG(DEBUG, "History")
            << "Catchup skipping old ledger " << header.ledgerSeq;
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
        return true;
    }

    // If we are at LCL, check that we knit up with it
    if (header.ledgerSeq == lclHeader.header.ledgerSeq)
    {
        if (hHeader.hash != lm.getLastClosedLedgerHeader().hash)
        {
            mApplyLedgerFailure.Mark();
            throw std::runtime_error(
                fmt::format("replay of {:s} at LCL {:s} disagreed on hash",
                            LedgerManager::ledgerAbbrev(hHeader),
                            LedgerManager::ledgerAbbrev(lclHeader)));
        }
        CLOG(DEBUG, "History")
            << "Catchup at LCL=" << header.ledgerSeq << ", hash correct";
        return true;
    }

    // If we are past current, we can't catch up: fail.
    if (header.ledgerSeq != lclHeader.header.ledgerSeq + 1)
    {
        mApplyLedgerFailure.Mark();
        throw std::runtime_error(
            fmt::format("replay overshot current ledger: {:d} > {:d}",
                        header.ledgerSeq, lclHeader.header.ledgerSeq + 1));
    }

    // If we do not agree about LCL hash, we can't catch up: fail.
    if (header.previousLedgerHash != lm.getLastClosedLedgerHeader().hash)
    {
        mApplyLedgerFailure.Mark();
        throw std::runtime_error(fmt::format(
            "replay at current ledger {:s} disagreed on LCL hash {:s}",
            LedgerManager::ledgerAbbrev(header.ledgerSeq - 1,
                                        header.previousLedgerHash),
            LedgerManager::ledgerAbbrev(lclHeader)));
    }

    auto txset = getCurrentTxSet();
    CLOG(DEBUG, "History") << "Ledger " << header.ledgerSeq << " has "
                           << txset->sizeTx() << " transactions";

    // We've verified the ledgerHeader (in the "trusted part of history"
    // sense) in CATCHUP_VERIFY phase; we now need to check that the
    // txhash we're about to apply is the one denoted by that ledger
    // header.
    if (header.scpValue.txSetHash != txset->getContentsHash())
    {
        mApplyLedgerFailure.Mark();
        throw std::runtime_error(fmt::format(
            "replay txset hash differs from txset hash in replay ledger: hash "
            "for txset for {:d} is {:s}, expected {:s}",
            header.ledgerSeq, hexAbbrev(txset->getContentsHash()),
            hexAbbrev(header.scpValue.txSetHash)));
    }

#ifdef BUILD_TESTS
    if (mApp.getConfig()
            .ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING)
    {
        auto& bm = mApp.getBucketManager();
        CLOG(INFO, "History")
            << "Forcing bucket manager to use version "
            << Config::CURRENT_LEDGER_PROTOCOL_VERSION << " with hash "
            << hexAbbrev(header.bucketListHash);
        bm.setNextCloseVersionAndHashForTesting(
            Config::CURRENT_LEDGER_PROTOCOL_VERSION, header.bucketListHash);
    }
#endif

    LedgerCloseData closeData(header.ledgerSeq, txset, header.scpValue);
    lm.closeLedger(closeData);

    CLOG(DEBUG, "History") << "LedgerManager LCL:\n"
                           << xdr::xdr_to_string(
                                  lm.getLastClosedLedgerHeader());
    CLOG(DEBUG, "History") << "Replay header:\n" << xdr::xdr_to_string(hHeader);
    if (lm.getLastClosedLedgerHeader().hash != hHeader.hash)
    {
        mApplyLedgerFailure.Mark();
        throw std::runtime_error(fmt::format(
            "replay of {:s} produced mismatched ledger hash {:s}",
            LedgerManager::ledgerAbbrev(hHeader),
            LedgerManager::ledgerAbbrev(lm.getLastClosedLedgerHeader())));
    }

    mApplyLedgerSuccess.Mark();
    return true;
}

BasicWork::State
ApplyCheckpointWork::onRun()
{
    try
    {
        if (!mFilesOpen)
        {
            openInputFiles();
        }

        auto result = applyHistoryOfSingleLedger();
        auto const& lm = mApp.getLedgerManager();
        auto done = lm.getLastClosedLedgerNum() == mLedgerRange.mLast;

        if (done)
        {
            return State::WORK_SUCCESS;
        }

        return result ? State::WORK_RUNNING : State::WORK_FAILURE;
    }
    catch (InvariantDoesNotHold&)
    {
        // already displayed e.what()
        CLOG(ERROR, "History") << "Replay failed";
        throw;
    }
    catch (FileSystemException&)
    {
        CLOG(ERROR, "History") << POSSIBLY_CORRUPTED_LOCAL_FS;
        return State::WORK_FAILURE;
    }
    catch (std::exception& e)
    {
        CLOG(ERROR, "History") << "Replay failed: " << e.what();
        return State::WORK_FAILURE;
    }
}
}
