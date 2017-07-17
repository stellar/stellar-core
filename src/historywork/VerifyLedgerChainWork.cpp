// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "historywork/Progress.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRStream.h"

namespace stellar
{

static HistoryManager::VerifyHashStatus
verifyLedgerHistoryEntry(LedgerHeaderHistoryEntry const& hhe)
{
    LedgerHeaderFrame lFrame(hhe.header);
    Hash calculated = lFrame.getHash();
    if (calculated != hhe.hash)
    {
        CLOG(ERROR, "History")
            << "Bad ledger-header history entry: claimed ledger "
            << LedgerManager::ledgerAbbrev(hhe) << " actually hashes to "
            << hexAbbrev(calculated);
        return HistoryManager::VERIFY_HASH_BAD;
    }
    return HistoryManager::VERIFY_HASH_OK;
}

static HistoryManager::VerifyHashStatus
verifyLedgerHistoryLink(Hash const& prev, LedgerHeaderHistoryEntry const& curr)
{
    if (verifyLedgerHistoryEntry(curr) != HistoryManager::VERIFY_HASH_OK)
    {
        return HistoryManager::VERIFY_HASH_BAD;
    }
    if (prev != curr.header.previousLedgerHash)
    {
        CLOG(ERROR, "History")
            << "Bad hash-chain: " << LedgerManager::ledgerAbbrev(curr)
            << " wants prev hash " << hexAbbrev(curr.header.previousLedgerHash)
            << " but actual prev hash is " << hexAbbrev(prev);
        return HistoryManager::VERIFY_HASH_BAD;
    }
    return HistoryManager::VERIFY_HASH_OK;
}

VerifyLedgerChainWork::VerifyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    uint32_t first, uint32_t last, bool manualCatchup,
    LedgerHeaderHistoryEntry& firstVerified,
    LedgerHeaderHistoryEntry& lastVerified)
    : Work(app, parent, "verify-ledger-chain")
    , mDownloadDir(downloadDir)
    , mFirstSeq(first)
    , mCurrSeq(first)
    , mLastSeq(last)
    , mManualCatchup(manualCatchup)
    , mFirstVerified(firstVerified)
    , mLastVerified(lastVerified)
{
}

std::string
VerifyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "verifying checkpoint";
        return fmtProgress(mApp, task, mFirstSeq, mLastSeq, mCurrSeq);
    }
    return Work::getStatus();
}

void
VerifyLedgerChainWork::onReset()
{
    auto setLedger = mApp.getLedgerManager().getLastClosedLedgerHeader();
    if (setLedger.header.ledgerSeq < 2)
    {
        setLedger = {};
    }
    if (mFirstVerified.header.ledgerSeq != 0)
    {
        mFirstVerified = setLedger;
    }
    if (mLastVerified.header.ledgerSeq != 0)
    {
        mLastVerified = setLedger;
    }
    mCurrSeq = mFirstSeq;
}

HistoryManager::VerifyHashStatus
VerifyLedgerChainWork::verifyHistoryOfSingleCheckpoint()
{
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCurrSeq);
    XDRInputFileStream hdrIn;
    hdrIn.open(ft.localPath_nogz());

    LedgerHeaderHistoryEntry prev = mLastVerified;
    LedgerHeaderHistoryEntry curr;

    CLOG(DEBUG, "History") << "Verifying ledger headers from "
                           << ft.localPath_nogz() << " starting from ledger "
                           << LedgerManager::ledgerAbbrev(prev);

    while (hdrIn && hdrIn.readOne(curr))
    {

        if (prev.header.ledgerSeq == 0)
        {
            // When we have no previous state to connect up with
            // (eg. starting somewhere mid-chain like in CATCHUP_MINIMAL)
            // we just accept the first chain entry we see. We will
            // verify the chain continuously from here, and against the
            // live network.
            prev = curr;
            continue;
        }

        uint32_t expectedSeq = prev.header.ledgerSeq + 1;
        if (curr.header.ledgerSeq < expectedSeq)
        {
            // Harmless prehistory
            continue;
        }
        else if (curr.header.ledgerSeq > expectedSeq)
        {
            CLOG(ERROR, "History")
                << "History chain overshot expected ledger seq " << expectedSeq
                << ", got " << curr.header.ledgerSeq << " instead";
            return HistoryManager::VERIFY_HASH_BAD;
        }
        if (verifyLedgerHistoryLink(prev.hash, curr) !=
            HistoryManager::VERIFY_HASH_OK)
        {
            return HistoryManager::VERIFY_HASH_BAD;
        }
        prev = curr;
    }

    if (curr.header.ledgerSeq != mCurrSeq)
    {
        CLOG(ERROR, "History") << "History chain did not end with " << mCurrSeq;
        return HistoryManager::VERIFY_HASH_BAD;
    }

    auto status = HistoryManager::VERIFY_HASH_OK;
    if (mCurrSeq == mLastSeq)
    {
        CLOG(INFO, "History") << "Verifying catchup candidate " << mCurrSeq
                              << " with LedgerManager";
        status = mApp.getLedgerManager().verifyCatchupCandidate(curr);
        if ((status == HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE ||
             status == HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE) &&
            mManualCatchup)
        {
            CLOG(WARNING, "History")
                << "Accepting unknown-hash ledger due to manual catchup";
            status = HistoryManager::VERIFY_HASH_OK;
        }
    }

    if (status == HistoryManager::VERIFY_HASH_OK)
    {
        if (mCurrSeq == mFirstSeq)
        {
            mFirstVerified = curr;
        }
        mLastVerified = curr;
    }

    return status;
}

Work::State
VerifyLedgerChainWork::onSuccess()
{
    mApp.getHistoryManager().logAndUpdateStatus(true);

    if (mCurrSeq > mLastSeq)
    {
        throw std::runtime_error("Verification overshot target ledger");
    }

    // This is in onSuccess rather than onRun, so we can force a FAILURE_RAISE.
    switch (verifyHistoryOfSingleCheckpoint())
    {
    case HistoryManager::VERIFY_HASH_OK:
        if (mCurrSeq == mLastSeq)
        {
            CLOG(INFO, "History") << "History chain [" << mFirstSeq << ","
                                  << mLastSeq << "] verified";
            return WORK_SUCCESS;
        }

        mCurrSeq += mApp.getHistoryManager().getCheckpointFrequency();
        return WORK_RUNNING;
    case HistoryManager::VERIFY_HASH_UNKNOWN_RECOVERABLE:
        CLOG(WARNING, "History")
            << "Catchup material verification inconclusive, retrying";
        return WORK_FAILURE_RETRY;
    case HistoryManager::VERIFY_HASH_BAD:
    case HistoryManager::VERIFY_HASH_UNKNOWN_UNRECOVERABLE:
        CLOG(ERROR, "History")
            << "Catchup material failed verification, propagating failure";
        return WORK_FAILURE_FATAL;
    default:
        assert(false);
        throw std::runtime_error("unexpected VerifyLedgerChainWork state");
    }
}
}
