// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/ApplyCheckpointWork.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyLedgerWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/Progress.h"
#include "invariant/InvariantDoesNotHold.h"
#include "ledger/CheckpointRange.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/FileSystemException.h"
#include "util/GlobalChecks.h"
#include "util/XDRCereal.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <optional>

namespace stellar
{

ApplyCheckpointWork::ApplyCheckpointWork(Application& app,
                                         TmpDir const& downloadDir,
                                         LedgerRange const& range,
                                         OnFailureCallback cb)
    : BasicWork(app,
                "apply-ledgers-" + fmt::format(FMT_STRING("{}-{}"),
                                               range.mFirst, range.limit()),
                BasicWork::RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mLedgerRange(range)
    , mCheckpoint(
          app.getHistoryManager().checkpointContainingLedger(range.mFirst))
    , mOnFailure(cb)
{
    // Ledger range check to enforce application of a single checkpoint
    auto const& hm = mApp.getHistoryManager();
    auto low = hm.firstLedgerInCheckpointContaining(mCheckpoint);
    if (mLedgerRange.mFirst != low)
    {
        throw std::runtime_error(
            "Ledger range start must be aligned with checkpoint start");
    }
    if (mLedgerRange.mCount > 0 && mLedgerRange.last() > mCheckpoint)
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
        return fmt::format(FMT_STRING("Last applied ledger: {:d}"), lcl);
    }
    return BasicWork::getStatus();
}

void
ApplyCheckpointWork::closeFiles()
{
    mHdrIn.close();
    mTxIn.close();
    mFilesOpen = false;
}

void
ApplyCheckpointWork::onReset()
{
    mConditionalWork.reset();
    closeFiles();
}

void
ApplyCheckpointWork::openInputFiles()
{
    ZoneScoped;
    mHdrIn.close();
    mTxIn.close();
    FileTransferInfo hi(mDownloadDir, FileType::HISTORY_FILE_TYPE_LEDGER,
                        mCheckpoint);
    FileTransferInfo ti(mDownloadDir, FileType::HISTORY_FILE_TYPE_TRANSACTIONS,
                        mCheckpoint);
    CLOG_DEBUG(History, "Replaying ledger headers from {}",
               hi.localPath_nogz());
    CLOG_DEBUG(History, "Replaying transactions from {}", ti.localPath_nogz());
    mHdrIn.open(hi.localPath_nogz());
    mTxIn.open(ti.localPath_nogz());
    mTxHistoryEntry = TransactionHistoryEntry();
    mHeaderHistoryEntry = LedgerHeaderHistoryEntry();
    mFilesOpen = true;
}

TxSetXDRFrameConstPtr
ApplyCheckpointWork::getCurrentTxSet()
{
    ZoneScoped;
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
            CLOG_DEBUG(History, "Skipping txset for ledger {}",
                       mTxHistoryEntry.ledgerSeq);
        }
        else if (mTxHistoryEntry.ledgerSeq > seq)
        {
            break;
        }
        else
        {
            releaseAssert(mTxHistoryEntry.ledgerSeq == seq);
            CLOG_DEBUG(History, "Loaded txset for ledger {}", seq);
            if (mTxHistoryEntry.ext.v() == 0)
            {
                return TxSetXDRFrame::makeFromWire(mTxHistoryEntry.txSet);
            }
            else
            {
                return TxSetXDRFrame::makeFromWire(
                    mTxHistoryEntry.ext.generalizedTxSet());
            }
        }
    } while (mTxIn && mTxIn.readOne(mTxHistoryEntry));

    CLOG_DEBUG(History, "Using empty txset for ledger {}", seq);
    return TxSetXDRFrame::makeEmpty(lm.getLastClosedLedgerHeader());
}

std::shared_ptr<LedgerCloseData>
ApplyCheckpointWork::getNextLedgerCloseData()
{
    ZoneScoped;
    if (!mHdrIn || !mHdrIn.readOne(mHeaderHistoryEntry))
    {
        throw std::runtime_error("No more ledgers to replay!");
    }

    LedgerHeader& header = mHeaderHistoryEntry.header;

    auto& lm = mApp.getLedgerManager();

    auto const& lclHeader = lm.getLastClosedLedgerHeader();

    // If we are >1 before LCL, skip
    if (header.ledgerSeq + 1 < lclHeader.header.ledgerSeq)
    {
        CLOG_DEBUG(History, "Catchup skipping old ledger {}", header.ledgerSeq);
        return nullptr;
    }

    // If we are one before LCL, check that we knit up with it
    if (header.ledgerSeq + 1 == lclHeader.header.ledgerSeq)
    {
        if (mHeaderHistoryEntry.hash != lclHeader.header.previousLedgerHash)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("replay of {:s} failed to connect on hash of LCL "
                           "predecessor {:s}"),
                LedgerManager::ledgerAbbrev(mHeaderHistoryEntry),
                LedgerManager::ledgerAbbrev(
                    lclHeader.header.ledgerSeq - 1,
                    lclHeader.header.previousLedgerHash)));
        }
        CLOG_DEBUG(History, "Catchup at 1-before LCL ({}), hash correct",
                   header.ledgerSeq);
        return nullptr;
    }

    // If we are at LCL, check that we knit up with it
    if (header.ledgerSeq == lclHeader.header.ledgerSeq)
    {
        if (mHeaderHistoryEntry.hash != lm.getLastClosedLedgerHeader().hash)
        {
            throw std::runtime_error(fmt::format(
                FMT_STRING("replay of {:s} at LCL {:s} disagreed on hash"),
                LedgerManager::ledgerAbbrev(mHeaderHistoryEntry),
                LedgerManager::ledgerAbbrev(lclHeader)));
        }
        CLOG_DEBUG(History, "Catchup at LCL={}, hash correct",
                   header.ledgerSeq);
        return nullptr;
    }

    // If we are past current, we can't catch up: fail.
    if (header.ledgerSeq != lclHeader.header.ledgerSeq + 1)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING("replay overshot current ledger: {:d} > {:d}"),
            header.ledgerSeq, lclHeader.header.ledgerSeq + 1));
    }

    // If we do not agree about LCL hash, we can't catch up: fail.
    if (header.previousLedgerHash != lm.getLastClosedLedgerHeader().hash)
    {
        throw std::runtime_error(fmt::format(
            FMT_STRING(
                "replay at current ledger {:s} disagreed on LCL hash {:s}"),
            LedgerManager::ledgerAbbrev(header.ledgerSeq - 1,
                                        header.previousLedgerHash),
            LedgerManager::ledgerAbbrev(lclHeader)));
    }

    auto txset = getCurrentTxSet();
    CLOG_DEBUG(History, "Ledger {} has {} transactions", header.ledgerSeq,
               txset->sizeTxTotal());

    // We've verified the ledgerHeader (in the "trusted part of history"
    // sense) in CATCHUP_VERIFY phase; we now need to check that the
    // txhash we're about to apply is the one denoted by that ledger
    // header.
    if (header.scpValue.txSetHash != txset->getContentsHash())
    {
        throw std::runtime_error(
            fmt::format(FMT_STRING("replay txset hash differs from txset hash "
                                   "in replay ledger: hash "
                                   "for txset for {:d} is {:s}, expected {:s}"),
                        header.ledgerSeq, hexAbbrev(txset->getContentsHash()),
                        hexAbbrev(header.scpValue.txSetHash)));
    }

#ifdef BUILD_TESTS
    if (mApp.getConfig()
            .ARTIFICIALLY_REPLAY_WITH_NEWEST_BUCKET_LOGIC_FOR_TESTING)
    {
        auto& bm = mApp.getBucketManager();
        CLOG_INFO(History,
                  "Forcing bucket manager to use version {} with hash {}",
                  mApp.getConfig().LEDGER_PROTOCOL_VERSION,
                  hexAbbrev(header.bucketListHash));
        bm.setNextCloseVersionAndHashForTesting(
            mApp.getConfig().LEDGER_PROTOCOL_VERSION, header.bucketListHash);
    }
#endif

    return std::make_shared<LedgerCloseData>(
        header.ledgerSeq, txset, header.scpValue,
        std::make_optional<Hash>(mHeaderHistoryEntry.hash));
}

BasicWork::State
ApplyCheckpointWork::onRun()
{
    ZoneScoped;
    if (mConditionalWork)
    {
        mConditionalWork->crankWork();

        if (mConditionalWork->getState() == State::WORK_SUCCESS)
        {
            auto& lm = mApp.getLedgerManager();

            CLOG_DEBUG(History, "{}",
                       xdrToCerealString(lm.getLastClosedLedgerHeader(),
                                         "LedgerManager LCL"));

            CLOG_DEBUG(History, "{}",
                       xdrToCerealString(mHeaderHistoryEntry, "Replay header"));
            if (lm.getLastClosedLedgerHeader().hash != mHeaderHistoryEntry.hash)
            {
                throw std::runtime_error(fmt::format(
                    FMT_STRING(
                        "replay of {:s} produced mismatched ledger hash {:s}"),
                    LedgerManager::ledgerAbbrev(mHeaderHistoryEntry),
                    LedgerManager::ledgerAbbrev(
                        lm.getLastClosedLedgerHeader())));
            }

            mApp.getCatchupManager().txSetsApplied();
        }
        else
        {
            return mConditionalWork->getState();
        }
    }

    auto const& lm = mApp.getLedgerManager();
    auto done = (mLedgerRange.mCount == 0 ||
                 lm.getLastClosedLedgerNum() == mLedgerRange.last());

    if (done)
    {
        closeFiles();
        return State::WORK_SUCCESS;
    }

    if (!mFilesOpen)
    {
        openInputFiles();
    }

    auto lcd = getNextLedgerCloseData();
    if (!lcd)
    {
        return State::WORK_RUNNING;
    }

    auto applyLedger = std::make_shared<ApplyLedgerWork>(mApp, *lcd);

    auto predicate = [](Application& app) {
        auto& bl = app.getBucketManager().getBucketList();
        auto& lm = app.getLedgerManager();
        bl.resolveAnyReadyFutures();
        return bl.futuresAllResolved(
            bl.getMaxMergeLevel(lm.getLastClosedLedgerNum() + 1));
    };

    mConditionalWork = std::make_shared<ConditionalWork>(
        mApp,
        fmt::format(FMT_STRING("apply-ledger-conditional ledger({:d})"),
                    lcd->getLedgerSeq()),
        predicate, applyLedger, std::chrono::milliseconds(500));

    mConditionalWork->startWork(wakeSelfUpCallback());
    return State::WORK_RUNNING;
}

void
ApplyCheckpointWork::shutdown()
{
    ZoneScoped;
    if (mConditionalWork)
    {
        mConditionalWork->shutdown();
    }
    BasicWork::shutdown();
}

bool
ApplyCheckpointWork::onAbort()
{
    ZoneScoped;
    if (mConditionalWork && !mConditionalWork->isDone())
    {
        mConditionalWork->crankWork();
        return false;
    }
    return true;
}

void
ApplyCheckpointWork::onFailureRaise()
{
    if (mOnFailure)
    {
        mOnFailure();
    }
}
}
