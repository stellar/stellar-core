// Copyright 2018 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/LedgerChainVerificationSnapshotWork.h"
#include "LedgerChainVerificationSnapshotWork.h"
#include "history/FileTransferInfo.h"
#include "historywork/Progress.h"
#include "ledger/LedgerManager.h"

namespace stellar
{
LedgerChainVerificationSnapshotWork::LedgerChainVerificationSnapshotWork(
    Application& app, WorkParent& parent, TmpDir const& downloadDir,
    uint32_t checkpoint, LedgerRange const& range,
    LedgerHeaderHistoryEntry& firstVerified,
    LedgerHeaderHistoryEntry const& lastClosedLedger, optional<Hash> scpHash)
    : BatchableWork(app, parent,
                    fmt::format("checkpoint-download-verify-{:d}", checkpoint))
    , mCheckpoint(checkpoint)
    , mRange(range)
    , mFirstVerified(firstVerified)
    , mLastClosedLedger(lastClosedLedger)
    , mDownloadDir(downloadDir)
    , mTrustedHash(scpHash)
{
}

LedgerChainVerificationSnapshotWork::~LedgerChainVerificationSnapshotWork()
{
    clearChildren();
}

std::string
LedgerChainVerificationSnapshotWork::getStatus() const
{
    if (mState == WORK_PENDING)
    {
        if (mDownloadVerifyWork)
        {
            return mDownloadVerifyWork->getStatus();
        }
    }

    return BatchableWork::getStatus();
}

void
LedgerChainVerificationSnapshotWork::onReset()
{
    mDownloadVerifyWork.reset();
}

Work::State
LedgerChainVerificationSnapshotWork::onSuccess()
{
    if (!downloadAndVerifyChain())
    {
        return Work::WORK_PENDING;
    }

    mSnapshotData = BatchableWorkResultData{mVerifiedAhead};
    if (getDependent())
    {
        CLOG(DEBUG, "History")
            << "Checkpoint " << mCheckpoint
            << " completed verification: Notifying dependent work";
        notifyCompleted();
    }
    else
    {
        if (mCheckpoint !=
            mApp.getHistoryManager().checkpointContainingLedger(mRange.first()))
        {
            throw std::runtime_error("No dependent work found, even though the "
                                     "end of the chain was not reached.");
        }
        CLOG(DEBUG, "History")
            << "End of ledger chain: No dependent work to notify";
    }

    return Work::WORK_SUCCESS;
}

void
LedgerChainVerificationSnapshotWork::unblockWork(
    BatchableWorkResultData const& data)
{
    auto verified = data.mVerifiedAheadLedger;

    CLOG(DEBUG, "History")
        << "Snapshot for checkpoint " << mCheckpoint
        << " got notified by finished blocking work, starting verify";

    if (mVerifiedAhead.header.ledgerSeq != 0)
    {
        throw std::runtime_error(
            fmt::format("Verify work for {:s} checkpoint has already been set",
                        mCheckpoint));
    }
    if (verified.header.ledgerSeq == 0)
    {
        throw std::runtime_error(
            fmt::format("Blocking work for checkpoint {:s} passed invalid data",
                        mCheckpoint));
    }

    mVerifiedAhead = verified;

    if (mDownloadVerifyWork)
    {
        mDownloadVerifyWork->makeReady();
        advanceChildren();
    }
}

bool
LedgerChainVerificationSnapshotWork::downloadAndVerifyChain()
{
    if (mDownloadVerifyWork)
    {
        assert(mDownloadVerifyWork->getState() == Work::WORK_SUCCESS);
        return true;
    }

    CLOG(DEBUG, "History")
        << "Downloading and verifying ledgers for checkpoint " << mCheckpoint;
    FileTransferInfo ft{mDownloadDir, HISTORY_FILE_TYPE_LEDGER, mCheckpoint};

    mDownloadVerifyWork = addWork<VerifyLedgerChainWork>(
        mDownloadDir, mCheckpoint, mRange, mFirstVerified, mVerifiedAhead,
        mLastClosedLedger, mTrustedHash);
    mDownloadVerifyWork->addWork<GetAndUnzipRemoteFileWork>(ft);

    auto last =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.last());
    if (last == mCheckpoint)
    {
        mDownloadVerifyWork->makeReady();
    }

    return false;
}
}
