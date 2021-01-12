// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyTxResultsWork.h"
#include "history/FileTransferInfo.h"
#include "ledger/LedgerManager.h"
#include "main/ErrorMessages.h"
#include "util/FileSystemException.h"
#include "util/Fs.h"
#include "util/Logging.h"
#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

VerifyTxResultsWork::VerifyTxResultsWork(Application& app,
                                         TmpDir const& downloadDir,
                                         uint32_t checkpoint)
    : BasicWork(app, "verify-results-" + std::to_string(checkpoint),
                RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mCheckpoint(checkpoint)
{
}

void
VerifyTxResultsWork::onReset()
{
    mHdrIn.close();
    mResIn.close();
    mTxResultEntry = {};
    mDone = false;
    mEc = asio::error_code();
    mLastSeenLedger = 0;
}

BasicWork::State
VerifyTxResultsWork::onRun()
{
    if (mDone)
    {
        return mEc ? State::WORK_FAILURE : State::WORK_SUCCESS;
    }

    std::weak_ptr<VerifyTxResultsWork> weak(
        std::static_pointer_cast<VerifyTxResultsWork>(shared_from_this()));
    auto verify = [weak, checkpoint = mCheckpoint]() {
        auto self = weak.lock();
        if (self)
        {
            ZoneScoped;
            asio::error_code ec;
            auto verified = self->verifyTxResultsOfCheckpoint();
            CLOG_TRACE(History,
                       "Transaction results verification for checkpoint {}{}",
                       checkpoint,
                       (verified ? " successful"
                                 : (" failed: " +
                                    std::string(POSSIBLY_CORRUPTED_HISTORY))));
            if (!verified)
            {
                ec = std::make_error_code(std::errc::io_error);
            }

            // Wake up call is posted back on the main thread
            self->mApp.postOnMainThread(
                [weak, ec]() {
                    auto self = weak.lock();
                    if (self)
                    {
                        self->mEc = ec;
                        self->mDone = true;
                        self->wakeUp();
                    }
                },
                "VerifyTxResults: finish");
        }
    };

    mApp.postOnBackgroundThread(verify, "VerifyTxResults: start in background");
    return State::WORK_WAITING;
}

bool
VerifyTxResultsWork::verifyTxResultsOfCheckpoint()
{
    ZoneScoped;
    try
    {
        FileTransferInfo hi(mDownloadDir, HISTORY_FILE_TYPE_LEDGER,
                            mCheckpoint);
        FileTransferInfo ri(mDownloadDir, HISTORY_FILE_TYPE_RESULTS,
                            mCheckpoint);
        mHdrIn.open(hi.localPath_nogz());
        mResIn.open(ri.localPath_nogz());

        LedgerHeaderHistoryEntry curr;
        while (mHdrIn && mHdrIn.readOne(curr))
        {
            auto ledgerSeq = curr.header.ledgerSeq;
            auto txResultEntry = getCurrentTxResultSet(ledgerSeq);
            auto resultSetHash =
                sha256(xdr::xdr_to_opaque(txResultEntry.txResultSet));
            auto genesis = ledgerSeq == LedgerManager::GENESIS_LEDGER_SEQ &&
                           txResultEntry.txResultSet.results.empty();

            if (!genesis && resultSetHash != curr.header.txSetResultHash)
            {
                CLOG_ERROR(
                    History,
                    "Hash of result set does not agree with result "
                    "hash in ledger header: "
                    "txset result hash for {:d} is {:s}, but expected {:s}",
                    ledgerSeq, hexAbbrev(resultSetHash),
                    hexAbbrev(curr.header.txSetResultHash));
                return false;
            }
        }
    }
    catch (FileSystemException&)
    {
        CLOG_ERROR(History, "Transaction results failed verification: "
                            "are .xdr files downloaded?");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_LOCAL_FS);
        return false;
    }
    catch (std::runtime_error& e)
    {
        CLOG_ERROR(History, "Transaction results failed verification: {}",
                   e.what());
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
        return false;
    }

    return true;
}

TransactionHistoryResultEntry
VerifyTxResultsWork::getCurrentTxResultSet(uint32_t ledger)
{
    ZoneScoped;
    TransactionHistoryResultEntry trs;
    trs.ledgerSeq = ledger;

    auto readNextWithValidation = [&]() {
        auto res = mResIn.readOne(mTxResultEntry);
        if (res)
        {
            auto readLedger = mTxResultEntry.ledgerSeq;
            auto const& hm = mApp.getHistoryManager();

            auto low = hm.firstLedgerInCheckpointContaining(mCheckpoint);
            if (readLedger > mCheckpoint || readLedger < low)
            {
                throw std::runtime_error("Results outside of checkpoint range");
            }

            if (readLedger <= mLastSeenLedger)
            {
                throw std::runtime_error("Malformed or duplicate results: "
                                         "ledgers must be strictly increasing");
            }
            mLastSeenLedger = readLedger;
        }
        return res;
    };

    do
    {
        if (mTxResultEntry.ledgerSeq < ledger)
        {
            CLOG_DEBUG(History, "Processed tx results for ledger {}",
                       mTxResultEntry.ledgerSeq);
        }
        else if (mTxResultEntry.ledgerSeq > ledger)
        {
            // No tx results in this ledger
            break;
        }
        else
        {
            CLOG_DEBUG(History, "Loaded tx result set for ledger {}", ledger);
            trs.txResultSet = mTxResultEntry.txResultSet;
            return trs;
        }
    } while (mResIn && readNextWithValidation());

    return trs;
}
}
