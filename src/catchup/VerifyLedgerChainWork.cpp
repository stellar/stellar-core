// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

static HistoryManager::LedgerVerificationStatus
verifyLedgerHistoryEntry(LedgerHeaderHistoryEntry const& hhe)
{
    Hash calculated = sha256(xdr::xdr_to_opaque(hhe.header));
    if (calculated != hhe.hash)
    {
        CLOG(ERROR, "History")
            << "Bad ledger-header history entry: claimed ledger "
            << LedgerManager::ledgerAbbrev(hhe) << " actually hashes to "
            << hexAbbrev(calculated);
        return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
    }
    return HistoryManager::VERIFY_STATUS_OK;
}

static HistoryManager::LedgerVerificationStatus
verifyLedgerHistoryLink(Hash const& prev, LedgerHeaderHistoryEntry const& curr)
{
    auto entryResult = verifyLedgerHistoryEntry(curr);
    if (entryResult != HistoryManager::VERIFY_STATUS_OK)
    {
        return entryResult;
    }
    if (prev != curr.header.previousLedgerHash)
    {
        CLOG(ERROR, "History")
            << "Bad hash-chain: " << LedgerManager::ledgerAbbrev(curr)
            << " wants prev hash " << hexAbbrev(curr.header.previousLedgerHash)
            << " but actual prev hash is " << hexAbbrev(prev);
        return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
    }
    return HistoryManager::VERIFY_STATUS_OK;
}

static HistoryManager::LedgerVerificationStatus
verifyLastLedgerInCheckpoint(LedgerHeaderHistoryEntry const& ledger,
                             LedgerNumHashPair const& verifiedAhead)
{
    // When last ledger in the checkpoint is reached, verify its hash against
    // the next checkpoint.
    assert(ledger.header.ledgerSeq == verifiedAhead.first);
    auto trustedHash = verifiedAhead.second;
    if (!trustedHash)
    {
        CLOG(DEBUG, "History")
            << "No trusted hash provided to verify "
            << LedgerManager::ledgerAbbrev(ledger) << " against.";
    }
    else
    {
        CLOG(DEBUG, "History")
            << "Verifying ledger " << ledger.header.ledgerSeq
            << " against trusted hash " << hexAbbrev(*trustedHash);
        if (ledger.hash != *trustedHash)
        {
            return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
        }
    }
    return HistoryManager::VERIFY_STATUS_OK;
}

VerifyLedgerChainWork::VerifyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    LedgerRange range, LedgerNumHashPair const& lastClosedLedger,
    LedgerNumHashPair ledgerRangeEnd)
    : Work(app, parent, "verify-ledger-chain")
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mCurrCheckpoint(
          mApp.getHistoryManager().checkpointContainingLedger(mRange.last()))
    , mLastClosed(lastClosedLedger)
    , mTrustedEndLedger(ledgerRangeEnd)
    , mVerifyLedgerSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "success"}, "event"))
    , mVerifyLedgerChainSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "success"}, "event"))
    , mVerifyLedgerChainFailure(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "failure"}, "event"))
{
    assert(range.last() == ledgerRangeEnd.first);
    assert(lastClosedLedger.second); // LCL hash must be provided
}

VerifyLedgerChainWork::~VerifyLedgerChainWork()
{
    clearChildren();
}

std::string
VerifyLedgerChainWork::getStatus() const
{
    if (mState == WORK_RUNNING)
    {
        std::string task = "verifying checkpoint";
        return fmtProgress(mApp, task, mRange.first(), mRange.last(),
                           (mRange.last() - mCurrCheckpoint));
    }
    return Work::getStatus();
}

void
VerifyLedgerChainWork::onReset()
{
    mVerifiedAhead = LedgerNumHashPair(0, nullptr);
    mVerifiedLedgerRangeStart = {};
    mCurrCheckpoint =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.last());
}

HistoryManager::LedgerVerificationStatus
VerifyLedgerChainWork::verifyHistoryOfSingleCheckpoint()
{
    // When verifying a checkpoint, we rely on the fact that the next checkpoint
    // has been verified (unless there's 1 checkpoint).
    // Once the end of the range is reached, ensure that the chain agrees with
    // trusted hash passed in. If LCL is reached, verify that it agrees with
    // the chain.

    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_LEDGER,
                        mCurrCheckpoint);
    XDRInputFileStream hdrIn;
    hdrIn.open(ft.localPath_nogz());

    bool beginCheckpoint = true;
    LedgerHeaderHistoryEntry prev;
    LedgerHeaderHistoryEntry curr;

    CLOG(DEBUG, "History") << "Verifying ledger headers from "
                           << ft.localPath_nogz() << " for checkpoint "
                           << mCurrCheckpoint;

    auto nextCheckpointFirstLedger = mVerifiedAhead;
    while (hdrIn && hdrIn.readOne(curr))
    {
        if (curr.header.ledgerVersion > Config::CURRENT_LEDGER_PROTOCOL_VERSION)
        {
            return HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION;
        }

        // Verify ledger with local state by comparing to LCL
        if (curr.header.ledgerSeq == mLastClosed.first)
        {
            if (sha256(xdr::xdr_to_opaque(curr.header)) != *mLastClosed.second)
            {
                CLOG(ERROR, "History")
                    << "Bad ledger-header history entry: claimed ledger "
                    << LedgerManager::ledgerAbbrev(curr)
                    << " does not agree with LCL "
                    << LedgerManager::ledgerAbbrev(mLastClosed.first,
                                                   *mLastClosed.second);
                return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
            }
        }
        // Verify LCL that is just before the first ledger in range
        else if (curr.header.ledgerSeq == mLastClosed.first + 1)
        {
            auto lclResult = verifyLedgerHistoryLink(*mLastClosed.second, curr);
            if (lclResult != HistoryManager::VERIFY_STATUS_OK)
            {
                CLOG(ERROR, "History")
                    << "Bad ledger-header history entry: claimed ledger "
                    << LedgerManager::ledgerAbbrev(curr)
                    << " previous hash does not agree with LCL: "
                    << LedgerManager::ledgerAbbrev(mLastClosed.first,
                                                   *mLastClosed.second);
                return lclResult;
            }
        }

        if (beginCheckpoint)
        {
            // At the beginning of checkpoint, we can't verify the link with
            // previous ledger, so at least verify that header content hashes to
            // correct value
            auto hashResult = verifyLedgerHistoryEntry(curr);
            if (hashResult != HistoryManager::VERIFY_STATUS_OK)
            {
                return hashResult;
            }

            // Remember first ledger in the checkpoint that will be used by the
            // next checkpoint
            auto hash = make_optional<Hash>(curr.header.previousLedgerHash);
            mVerifiedAhead = LedgerNumHashPair(curr.header.ledgerSeq - 1, hash);
            beginCheckpoint = false;
        }
        else
        {
            uint32_t expectedSeq = prev.header.ledgerSeq + 1;
            if (curr.header.ledgerSeq < expectedSeq)
            {
                CLOG(ERROR, "History")
                    << "History chain undershot expected ledger seq "
                    << expectedSeq << ", got " << curr.header.ledgerSeq
                    << " instead";
                return HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT;
            }
            else if (curr.header.ledgerSeq > expectedSeq)
            {
                CLOG(ERROR, "History")
                    << "History chain overshot expected ledger seq "
                    << expectedSeq << ", got " << curr.header.ledgerSeq
                    << " instead";
                return HistoryManager::VERIFY_STATUS_ERR_OVERSHOT;
            }
            auto linkResult = verifyLedgerHistoryLink(prev.hash, curr);
            if (linkResult != HistoryManager::VERIFY_STATUS_OK)
            {
                return linkResult;
            }
        }

        mVerifyLedgerSuccess.Mark();
        prev = curr;

        // No need to keep verifying if the range is covered
        if (curr.header.ledgerSeq == mRange.last())
        {
            break;
        }
    }

    if (curr.header.ledgerSeq != mCurrCheckpoint &&
        curr.header.ledgerSeq != mRange.last())
    {
        // We can end at mCurrCheckpoint if checkpoint was valid
        // or at mRange.last() if history chain file was valid and we
        // reached last ledger in the range. Any other ledger here means
        // that file is corrupted.
        CLOG(ERROR, "History") << "History chain did not end with "
                               << mCurrCheckpoint << " or " << mRange.last();
        return HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES;
    }

    if (curr.header.ledgerSeq == mRange.last())
    {
        assert(nextCheckpointFirstLedger.first == 0);
        nextCheckpointFirstLedger = mTrustedEndLedger;
        CLOG(INFO, "History")
            << (mTrustedEndLedger.second ? "Verifying"
                                         : "Skipping verification for")
            << "ledger " << LedgerManager::ledgerAbbrev(curr)
            << " against SCP hash";
    }
    else
    {
        assert(nextCheckpointFirstLedger.second);
        assert(nextCheckpointFirstLedger.first != 0);
    }

    // Last ledger in the checkpoint needs to agree with first ledger of a
    // checkpoint ahead of it
    auto verifyTrustedHash =
        verifyLastLedgerInCheckpoint(curr, nextCheckpointFirstLedger);
    if (verifyTrustedHash != HistoryManager::VERIFY_STATUS_OK)
    {
        assert(nextCheckpointFirstLedger.second);
        CLOG(ERROR, "History")
            << "Checkpoint does not agree with checkpoint ahead: "
            << "current " << LedgerManager::ledgerAbbrev(curr) << ", verified: "
            << LedgerManager::ledgerAbbrev(nextCheckpointFirstLedger.first,
                                           *(nextCheckpointFirstLedger.second));
        return verifyTrustedHash;
    }

    if (mCurrCheckpoint ==
        mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
    {
        mVerifiedLedgerRangeStart = curr;
    }

    return HistoryManager::VERIFY_STATUS_OK;
}

Work::State
VerifyLedgerChainWork::onSuccess()
{
    mApp.getCatchupManager().logAndUpdateCatchupStatus(true);

    if (mCurrCheckpoint <
        mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
    {
        throw std::runtime_error(
            "Verification undershot first ledger in the range.");
    }

    // This is in onSuccess rather than onRun, so we can force a FAILURE_RAISE.
    switch (verifyHistoryOfSingleCheckpoint())
    {
    case HistoryManager::VERIFY_STATUS_OK:
        if (mCurrCheckpoint ==
            mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
        {
            CLOG(INFO, "History") << "History chain [" << mRange.first() << ","
                                  << mRange.last() << "] verified";
            mVerifyLedgerChainSuccess.Mark();
            return WORK_SUCCESS;
        }

        mCurrCheckpoint -= mApp.getHistoryManager().getCheckpointFrequency();
        return WORK_RUNNING;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "unsupported ledger version, propagating "
                                  "failure";
        mVerifyLedgerChainFailure.Mark();
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_HASH:
        CLOG(ERROR, "History") << "Catchup material failed verification - hash "
                                  "mismatch, propagating failure";
        mVerifyLedgerChainFailure.Mark();
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_OVERSHOT:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "overshot, propagating failure";
        mVerifyLedgerChainFailure.Mark();
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "undershot, propagating failure";
        mVerifyLedgerChainFailure.Mark();
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "missing entries, propagating failure";
        mVerifyLedgerChainFailure.Mark();
        return WORK_FAILURE_FATAL;
    default:
        assert(false);
        throw std::runtime_error("unexpected VerifyLedgerChainWork state");
    }
}
}
