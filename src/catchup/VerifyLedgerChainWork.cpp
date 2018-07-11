// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/Progress.h"
#include "ledger/LedgerHeaderFrame.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "util/XDRStream.h"
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

static HistoryManager::LedgerVerificationStatus
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

VerifyLedgerChainWork::VerifyLedgerChainWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    LedgerRange range, LedgerHeaderHistoryEntry& firstVerified,
    LedgerHeaderHistoryEntry const& lastClosedLedger, optional<Hash> scpHash)
    : Work(app, parent, "verify-ledger-chain")
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mCurrCheckpoint(
          mApp.getHistoryManager().checkpointContainingLedger(mRange.last()))
    , mFirstVerified(firstVerified)
    , mLastClosedLedger(lastClosedLedger)
    , mTrustedHash(scpHash)
    , mVerifyLedgerSuccessOld(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "success-old"}, "event"))
    , mVerifyLedgerSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "success"}, "event"))
    , mVerifyLedgerFailureLedgerVersion(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "failure-ledger-version"}, "event"))
    , mVerifyLedgerFailureOvershot(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "failure-overshot"}, "event"))
    , mVerifyLedgerFailureUndershot(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "failure-undershot"}, "event"))
    , mVerifyLedgerFailureLink(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "failure-link"}, "event"))
    , mVerifyLedgerChainSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "success"}, "event"))
    , mVerifyLedgerChainFailure(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "failure"}, "event"))
    , mVerifyLedgerChainFailureEnd(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "failure-end"}, "event"))
{
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
    // Note that mFirstVerified is not reset here, since it is set
    // in the end of verification, where work wouldn't fail

    if (mVerifiedAhead.header.ledgerSeq != 0)
    {
        mVerifiedAhead = {};
    }
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
            mVerifyLedgerFailureLedgerVersion.Mark();
            return HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION;
        }

        // Verify ledger with local state by comparing to LCL
        if (curr.header.ledgerSeq == mLastClosedLedger.header.ledgerSeq)
        {
            if (curr.hash != mLastClosedLedger.hash)
            {
                mVerifyLedgerFailureLink.Mark();
                return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
            }
        }

        // Remember first ledger in the checkpoint that will be used by the next
        // checkpoint
        if (beginCheckpoint)
        {
            mVerifiedAhead = curr;
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
                mVerifyLedgerFailureUndershot.Mark();
                return HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT;
            }
            else if (curr.header.ledgerSeq > expectedSeq)
            {
                CLOG(ERROR, "History")
                    << "History chain overshot expected ledger seq "
                    << expectedSeq << ", got " << curr.header.ledgerSeq
                    << " instead";
                mVerifyLedgerFailureOvershot.Mark();
                return HistoryManager::VERIFY_STATUS_ERR_OVERSHOT;
            }
            auto linkResult = verifyLedgerHistoryLink(prev.hash, curr);
            if (linkResult != HistoryManager::VERIFY_STATUS_OK)
            {
                mVerifyLedgerFailureLink.Mark();
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
        // We can end at mCurrCheckpoint if history chain file was valid
        // Or we can end at mRange.last() if history chain file was valid and we
        // reached last ledger that we should check.
        // Any other ledger here means that file is corrupted.
        CLOG(ERROR, "History") << "History chain did not end with "
                               << mCurrCheckpoint << " or " << mRange.last();
        mVerifyLedgerChainFailureEnd.Mark();
        return HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES;
    }

    if (curr.header.ledgerSeq == mRange.last())
    {
        // Verifying the most recent checkpoint means no other checkpoints are
        // verified yet
        assert(nextCheckpointFirstLedger.header.ledgerSeq == 0);
        auto verifyTrustedHash = verifyAgainstTrustedHash(curr);
        if (verifyTrustedHash != HistoryManager::VERIFY_STATUS_OK)
        {
            mVerifyLedgerChainFailure.Mark();
            return verifyTrustedHash;
        }
    }
    else if (curr.header.ledgerSeq == mCurrCheckpoint)
    {
        // If we reached the end of checkpoint, ensure there's a previously
        // verified checkpoint to compare against
        assert(nextCheckpointFirstLedger.header.ledgerSeq != 0);

        // Last ledger in the checkpoint needs to agree with first ledger of a
        // checkpoint ahead of it
        CLOG(INFO, "History")
            << "Verifying ledger " << LedgerManager::ledgerAbbrev(curr)
            << " against previously verified "
            << LedgerManager::ledgerAbbrev(nextCheckpointFirstLedger);
        auto linkNextCheckpoint =
            verifyLedgerHistoryLink(curr.hash, nextCheckpointFirstLedger);
        if (linkNextCheckpoint != HistoryManager::VERIFY_STATUS_OK)
        {
            mVerifyLedgerChainFailure.Mark();
            return linkNextCheckpoint;
        }
    }

    mVerifyLedgerChainSuccess.Mark();
    if (mCurrCheckpoint ==
        mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
    {
        mFirstVerified = curr;
    }

    return HistoryManager::VERIFY_STATUS_OK;
}

HistoryManager::LedgerVerificationStatus
VerifyLedgerChainWork::verifyAgainstTrustedHash(
    LedgerHeaderHistoryEntry& ledger)
{
    assert(ledger.header.ledgerSeq == mRange.last());
    if (!mTrustedHash)
    {
        CLOG(INFO, "History")
            << "No trusted hash provided to verify "
            << LedgerManager::ledgerAbbrev(ledger) << " against.";
    }
    else
    {
        CLOG(INFO, "History")
            << "Verifying ledger " << ledger.header.ledgerSeq
            << " against trusted hash " << hexAbbrev(*mTrustedHash);
        if (ledger.hash != *mTrustedHash)
        {
            return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
        }
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
            return WORK_SUCCESS;
        }

        mCurrCheckpoint -= mApp.getHistoryManager().getCheckpointFrequency();
        return WORK_RUNNING;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "unsupported ledger version, propagating "
                                  "failure";
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_HASH:
        CLOG(ERROR, "History") << "Catchup material failed verification - hash "
                                  "mismatch, propagating failure";
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_OVERSHOT:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "overshot, propagating failure";
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "undershot, propagating failure";
        return WORK_FAILURE_FATAL;
    case HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES:
        CLOG(ERROR, "History") << "Catchup material failed verification - "
                                  "missing entries, propagating failure";
        return WORK_FAILURE_FATAL;
    default:
        assert(false);
        throw std::runtime_error("unexpected VerifyLedgerChainWork state");
    }
}
}
