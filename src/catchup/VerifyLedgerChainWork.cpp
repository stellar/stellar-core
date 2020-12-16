// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/VerifyLedgerChainWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"
#include "main/Application.h"
#include "main/ErrorMessages.h"
#include "util/FileSystemException.h"
#include "util/GlobalChecks.h"
#include "util/Thread.h"
#include "util/XDRStream.h"
#include "util/types.h"
#include <Tracy.hpp>
#include <fmt/format.h>
#include <fstream>
#include <medida/meter.h>
#include <medida/metrics_registry.h>

namespace stellar
{

static HistoryManager::LedgerVerificationStatus
verifyLedgerHistoryEntry(LedgerHeaderHistoryEntry const& hhe)
{
    ZoneScoped;
    Hash calculated = sha256(xdr::xdr_to_opaque(hhe.header));
    if (calculated != hhe.hash)
    {
        CLOG_ERROR(
            History,
            "Bad ledger-header history entry: claimed ledger {} actually "
            "hashes to {}",
            LedgerManager::ledgerAbbrev(hhe), hexAbbrev(calculated));
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
        CLOG_ERROR(
            History,
            "Bad hash-chain: {} wants prev hash {} but actual prev hash is {}",
            LedgerManager::ledgerAbbrev(curr),
            hexAbbrev(curr.header.previousLedgerHash), hexAbbrev(prev));
        return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
    }
    return HistoryManager::VERIFY_STATUS_OK;
}

static HistoryManager::LedgerVerificationStatus
verifyLastLedgerInCheckpoint(LedgerHeaderHistoryEntry const& ledger,
                             LedgerNumHashPair const& verifiedAhead)
{
    ZoneScoped;
    // When max ledger in the checkpoint is reached, verify its hash against the
    // numerically-greater checkpoint (that we should have an incoming hash-link
    // from).
    releaseAssert(ledger.header.ledgerSeq == verifiedAhead.first);
    auto trustedHash = verifiedAhead.second;
    if (!trustedHash)
    {
        CLOG_DEBUG(History, "No trusted hash provided to verify {} against.",
                   LedgerManager::ledgerAbbrev(ledger));
    }
    else
    {
        CLOG_DEBUG(History, "Verifying ledger {} against trusted hash {}",
                   ledger.header.ledgerSeq, hexAbbrev(*trustedHash));
        if (ledger.hash != *trustedHash)
        {
            return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
        }
    }
    return HistoryManager::VERIFY_STATUS_OK;
}

VerifyLedgerChainWork::VerifyLedgerChainWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    LedgerNumHashPair const& lastClosedLedger,
    std::shared_future<LedgerNumHashPair> trustedMaxLedger,
    std::shared_ptr<std::ofstream> outputStream)
    : BasicWork(app, "verify-ledger-chain", BasicWork::RETRY_NEVER)
    , mDownloadDir(downloadDir)
    , mRange(range)
    , mCurrCheckpoint(mRange.mCount == 0
                          ? 0
                          : mApp.getHistoryManager().checkpointContainingLedger(
                                mRange.last()))
    , mLastClosed(lastClosedLedger)
    , mTrustedMaxLedger(trustedMaxLedger)
    , mVerifiedMinLedgerPrevFuture(mVerifiedMinLedgerPrev.get_future().share())
    , mOutputStream(outputStream)
    , mVerifyLedgerSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger", "success"}, "event"))
    , mVerifyLedgerChainSuccess(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "success"}, "event"))
    , mVerifyLedgerChainFailure(app.getMetrics().NewMeter(
          {"history", "verify-ledger-chain", "failure"}, "event"))
{
    // LCL should be at-or-after genesis and we should have a hash.
    releaseAssert(lastClosedLedger.first >= LedgerManager::GENESIS_LEDGER_SEQ);
    releaseAssert(lastClosedLedger.second);
}

std::string
VerifyLedgerChainWork::getStatus() const
{
    if (!isDone() && !isAborting() && mRange.mCount != 0)
    {
        std::string task = "verifying checkpoint";
        return fmtProgress(mApp, task, mRange,
                           (mRange.last() - mCurrCheckpoint));
    }
    return BasicWork::getStatus();
}

void
VerifyLedgerChainWork::onReset()
{
    CLOG_INFO(History, "Verifying ledgers {}", mRange.toString());

    mVerifiedAhead = LedgerNumHashPair(0, nullptr);
    mMaxVerifiedLedgerOfMinCheckpoint = {};
    mVerifiedLedgers.clear();
    mCurrCheckpoint = mRange.mCount == 0
                          ? 0
                          : mApp.getHistoryManager().checkpointContainingLedger(
                                mRange.last());
}

HistoryManager::LedgerVerificationStatus
VerifyLedgerChainWork::verifyHistoryOfSingleCheckpoint()
{
    ZoneScoped;
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

    // The `curr`, `first` and `prev` variables are named for their positions in
    // the while-loop that follows: `curr` stores the value read from the input
    // stream; `first` will be set to `curr` only on the first iteration, and
    // `prev` will be set to `curr` at the end of the loop to make the previous
    // iteration's `curr` available during the loop.
    LedgerHeaderHistoryEntry curr;
    LedgerHeaderHistoryEntry first;
    LedgerHeaderHistoryEntry prev;

    CLOG_DEBUG(History, "Verifying ledger headers from {} for checkpoint {}",
               ft.localPath_nogz(), mCurrCheckpoint);

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
                CLOG_ERROR(History,
                           "Bad ledger-header history entry: claimed ledger {} "
                           "does not agree with LCL {}",
                           LedgerManager::ledgerAbbrev(curr),
                           LedgerManager::ledgerAbbrev(mLastClosed.first,
                                                       *mLastClosed.second));
                return HistoryManager::VERIFY_STATUS_ERR_BAD_HASH;
            }
        }
        // Verify LCL that is just before the first ledger in range
        else if (curr.header.ledgerSeq == mLastClosed.first + 1)
        {
            auto lclResult = verifyLedgerHistoryLink(*mLastClosed.second, curr);
            if (lclResult != HistoryManager::VERIFY_STATUS_OK)
            {
                CLOG_ERROR(History,
                           "Bad ledger-header history entry: claimed ledger {} "
                           "previous hash does not agree with LCL: {}",
                           LedgerManager::ledgerAbbrev(curr),
                           LedgerManager::ledgerAbbrev(mLastClosed.first,
                                                       *mLastClosed.second));
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

            // Save first ledger in the checkpoint, in case we use it below in
            // assignment to mVerifiedMinLedgerPrev.
            first = curr;
            beginCheckpoint = false;
        }
        else
        {
            uint32_t expectedSeq = prev.header.ledgerSeq + 1;
            if (curr.header.ledgerSeq < expectedSeq)
            {
                CLOG_ERROR(
                    History,
                    "History chain undershot expected ledger seq {}, got "
                    "{} instead",
                    expectedSeq, curr.header.ledgerSeq);
                return HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT;
            }
            else if (curr.header.ledgerSeq > expectedSeq)
            {
                CLOG_ERROR(History,
                           "History chain overshot expected ledger seq {}, got "
                           "{} instead",
                           expectedSeq, curr.header.ledgerSeq);
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
        CLOG_ERROR(History, "History chain did not end with {} or {}",
                   mCurrCheckpoint, mRange.last());
        return HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES;
    }

    // We just finished scanning a checkpoint. We first grab the _incoming_
    // hash-link our caller (or previous call to this method) saved for us.
    auto incoming = mVerifiedAhead;

    {
        // While scanning the checkpoint, we saved (in `first`) the first
        // (lowest-numbered) ledger in this checkpoint while we were scanning
        // past it.
        //
        // We now write back (to `mVerifiedAhead`) the _outgoing_ hash-link and
        // expected sequence number of the next-lowest ledger number before this
        // checkpoint, which we'll encounter on the next call to this method
        // (processing the numerically-lower checkpoint) in the code below that
        // checks that the last ledger in a checkpoint agrees with the expected
        // _incoming_ hash-link.
        //
        // Note `mVerifiedAhead` is written here after being read moments
        // before. We're currently writing the value to be used in the _next_
        // call to this method.
        auto hash = make_optional<Hash>(first.header.previousLedgerHash);
        mVerifiedAhead = LedgerNumHashPair(first.header.ledgerSeq - 1, hash);
    }

    // We check to see if we just finished the first call to this method in this
    // object's work (true when this checkpoint-end landed on the
    // highest-numbered ledger in the range)
    if (curr.header.ledgerSeq == mRange.last())
    {
        // If so, there should be no "saved" incoming hash-link value from
        // a previous iteration.
        releaseAssert(incoming.first == 0);
        releaseAssert(incoming.second.get() == nullptr);

        // Instead, there _should_ be a value in the shared_future this work
        // object reads its initial trust from. If anything went wrong upstream
        // we shouldn't have even been run.
        releaseAssert(futureIsReady(mTrustedMaxLedger));

        incoming = mTrustedMaxLedger.get();
        releaseAssert(incoming.first == curr.header.ledgerSeq);
        CLOG_INFO(History, "{} ledger {} against SCP hash",
                  (incoming.second ? "Verifying" : "Skipping verification for"),
                  LedgerManager::ledgerAbbrev(curr));
    }
    else
    {
        // Otherwise we just finished a checkpoint _after_ than the first call
        // to this method and the `incoming` value we read out of
        // `mVerifiedAhead` should have content, because the previous call
        // should have saved something in `mVerifiedAhead`.
        releaseAssert(incoming.second);
        releaseAssert(incoming.first != 0);
    }

    // In either case, the last ledger in the checkpoint needs to agree with the
    // incoming hash-link from the first ledger of a checkpoint numerically
    // higher than it.
    auto verifyTrustedHash = verifyLastLedgerInCheckpoint(curr, incoming);
    if (verifyTrustedHash != HistoryManager::VERIFY_STATUS_OK)
    {
        releaseAssert(incoming.second);
        CLOG_ERROR(
            History,
            "Checkpoint does not agree with checkpoint ahead: current {}, "
            "verified: {}",
            LedgerManager::ledgerAbbrev(curr),
            LedgerManager::ledgerAbbrev(incoming.first, *(incoming.second)));
        return verifyTrustedHash;
    }

    if (mCurrCheckpoint ==
        mApp.getHistoryManager().checkpointContainingLedger(mRange.mFirst))
    {
        // Write outgoing trust-link to shared write-once variable.
        LedgerNumHashPair outgoing;
        outgoing.first = first.header.ledgerSeq - 1;
        outgoing.second = make_optional<Hash>(first.header.previousLedgerHash);

        try
        {
            mVerifiedMinLedgerPrev.set_value(outgoing);
        }
        catch (std::future_error const& err)
        {
            // If outgoing link has already been set, ignore exception from
            // attempting to set it twice. This will happen if we're reset
            // and re-run.
            if (err.code() != std::future_errc::promise_already_satisfied)
            {
                throw;
            }
        }

        // Also write the max ledger in this min-valued checkpoint, as it
        // will be read by catchup as the ledger number for bucket-apply.
        mMaxVerifiedLedgerOfMinCheckpoint = curr;
    }

    mVerifiedLedgers.emplace_back(curr.header.ledgerSeq,
                                  make_optional<Hash>(curr.hash));
    return HistoryManager::VERIFY_STATUS_OK;
}

void
VerifyLedgerChainWork::onSuccess()
{
    if (mOutputStream)
    {
        for (auto const& pair : mVerifiedLedgers)
        {
            (*mOutputStream) << "\n[" << pair.first << ", \""
                             << binToHex(*pair.second) << "\"],";
        }
    }
}

BasicWork::State
VerifyLedgerChainWork::onRun()
{
    ZoneScoped;
    if (mRange.mCount == 0)
    {
        CLOG_INFO(History, "History chain [0,0) trivially verified");
        mVerifyLedgerChainSuccess.Mark();
        return BasicWork::State::WORK_SUCCESS;
    }

    if (mCurrCheckpoint <
        mApp.getHistoryManager().checkpointContainingLedger(mRange.mFirst))
    {
        throw std::runtime_error(
            "Verification undershot first ledger in the range.");
    }

    HistoryManager::LedgerVerificationStatus result;

    // Catch FS-related errors to gracefully fail Work instead of crashing
    try
    {
        result = verifyHistoryOfSingleCheckpoint();
    }
    catch (FileSystemException&)
    {
        CLOG_ERROR(History, "Catchup material failed verification");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_LOCAL_FS);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    }

    switch (result)
    {
    case HistoryManager::VERIFY_STATUS_OK:
        if (mCurrCheckpoint ==
            mApp.getHistoryManager().checkpointContainingLedger(mRange.mFirst))
        {
            CLOG_INFO(History, "History chain [{},{}] verified", mRange.mFirst,
                      mRange.last());
            mVerifyLedgerChainSuccess.Mark();
            return BasicWork::State::WORK_SUCCESS;
        }
        mCurrCheckpoint -= mApp.getHistoryManager().getCheckpointFrequency();
        return BasicWork::State::WORK_RUNNING;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_LEDGER_VERSION:
        CLOG_ERROR(History, "Catchup material failed verification - "
                            "unsupported ledger version, propagating "
                            "failure");
        CLOG_ERROR(History, "{}", UPGRADE_STELLAR_CORE);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    case HistoryManager::VERIFY_STATUS_ERR_BAD_HASH:
        CLOG_ERROR(History, "Catchup material failed verification - hash "
                            "mismatch, propagating failure");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    case HistoryManager::VERIFY_STATUS_ERR_OVERSHOT:
        CLOG_ERROR(History, "Catchup material failed verification - "
                            "overshot, propagating failure");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    case HistoryManager::VERIFY_STATUS_ERR_UNDERSHOT:
        CLOG_ERROR(History, "Catchup material failed verification - "
                            "undershot, propagating failure");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    case HistoryManager::VERIFY_STATUS_ERR_MISSING_ENTRIES:
        CLOG_ERROR(History, "Catchup material failed verification - "
                            "missing entries, propagating failure");
        CLOG_ERROR(History, "{}", POSSIBLY_CORRUPTED_HISTORY);
        mVerifyLedgerChainFailure.Mark();
        return BasicWork::State::WORK_FAILURE;
    default:
        releaseAssert(false);
        throw std::runtime_error("unexpected VerifyLedgerChainWork state");
    }
}
}
