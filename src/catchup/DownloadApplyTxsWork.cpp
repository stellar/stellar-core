// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadApplyTxsWork.h"
#include "catchup/ApplyCheckpointWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "work/ConditionalWork.h"
#include "work/WorkSequence.h"

namespace stellar
{

DownloadApplyTxsWork::DownloadApplyTxsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    LedgerHeaderHistoryEntry& lastApplied)
    : BatchWork(app, "download-apply-ledgers")
    , mRange(range)
    , mDownloadDir(downloadDir)
    , mLastApplied(lastApplied)
    , mCheckpointToQueue(
          app.getHistoryManager().checkpointContainingLedger(range.mFirst))
{
}

std::shared_ptr<BasicWork>
DownloadApplyTxsWork::yieldMoreWork()
{
    if (!hasNext())
    {
        throw std::runtime_error("Work has no more children to iterate over!");
    }

    CLOG(INFO, "History") << "Downloading, unzipping and applying "
                          << HISTORY_FILE_TYPE_TRANSACTIONS
                          << " for checkpoint " << mCheckpointToQueue;
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS,
                        mCheckpointToQueue);
    auto getAndUnzip = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft);

    auto const& hm = mApp.getHistoryManager();
    auto low = std::max(LedgerManager::GENESIS_LEDGER_SEQ,
                        hm.prevCheckpointLedger(mCheckpointToQueue));
    auto high = std::min(mCheckpointToQueue, mRange.mLast);
    auto apply = std::make_shared<ApplyCheckpointWork>(mApp, mDownloadDir,
                                                       LedgerRange{low, high});

    std::vector<std::shared_ptr<BasicWork>> seq{getAndUnzip};

    if (mLastYieldedWork)
    {
        auto prev = mLastYieldedWork;
        auto predicate = [prev]() {
            if (!prev)
            {
                throw std::runtime_error("Download and apply txs: related Work "
                                         "is destroyed unexpectedly");
            }
            return prev->getState() == State::WORK_SUCCESS;
        };
        seq.push_back(std::make_shared<ConditionalWork>(
            mApp, "conditional-" + apply->getName(), predicate, apply));
    }
    else
    {
        seq.push_back(apply);
    }

    auto nextWork = std::make_shared<WorkSequence>(
        mApp, "download-apply-" + std::to_string(mCheckpointToQueue), seq,
        BasicWork::RETRY_NEVER);
    mCheckpointToQueue += mApp.getHistoryManager().getCheckpointFrequency();
    mLastYieldedWork = nextWork;
    return nextWork;
}

void
DownloadApplyTxsWork::resetIter()
{
    mCheckpointToQueue =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.mFirst);
    mLastYieldedWork.reset();
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
}

bool
DownloadApplyTxsWork::hasNext() const
{
    auto last =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.mLast);
    return mCheckpointToQueue <= last;
}

void
DownloadApplyTxsWork::onSuccess()
{
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
}
}