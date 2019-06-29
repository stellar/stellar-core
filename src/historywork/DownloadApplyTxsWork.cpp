// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "historywork/DownloadApplyTxsWork.h"
#include "catchup/ApplyCheckpointWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "work/WorkSequence.h"

namespace stellar
{

DownloadApplyTxsWork::DownloadApplyTxsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange range,
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
        CLOG(ERROR, "Work")
            << getName() << " has no more children to iterate over! ";
        return nullptr;
    }

    CLOG(INFO, "History") << "Downloading, unzipping and applying "
                          << HISTORY_FILE_TYPE_TRANSACTIONS
                          << " for checkpoint " << mCheckpointToQueue;
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS,
                        mCheckpointToQueue);
    auto getAndUnzip = std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft);
    auto apply = std::make_shared<ApplyCheckpointWork>(
        mApp, mDownloadDir, mCheckpointToQueue, mRange.mLast);

    std::vector<std::shared_ptr<BasicWork>> seq{getAndUnzip, apply};
    std::vector<ConditionFn> cond;

    if (!mRunning.empty())
    {
        auto prevWork = mRunning.back();
        auto predicate = [prevWork]() {
            if (!prevWork)
            {
                throw std::runtime_error("Download and apply txs: related Work "
                                         "is destroyed unexpectedly");
            }
            return prevWork->getState() == State::WORK_SUCCESS;
        };
        cond = {nullptr, predicate};
    }

    auto nextWork = std::make_shared<WorkSequence>(
        mApp, "download-apply-" + std::to_string(mCheckpointToQueue), seq, cond,
        RETRY_NEVER);
    mRunning.push_back(nextWork);
    mCheckpointToQueue += mApp.getHistoryManager().getCheckpointFrequency();

    return nextWork;
}

void
DownloadApplyTxsWork::wakeWaitingWork()
{
    while (!mRunning.empty())
    {
        auto work = mRunning.front();
        if (work->getState() == State::WORK_SUCCESS)
        {
            mRunning.pop_front();
            continue;
        }

        if (work->getState() != State::WORK_FAILURE)
        {
            // At this point, sequence download->apply is either:
            // - Still downloading files, in which case `wakeUp` will have
            // no effect, and it will simply proceed to applying when done
            // since the condition has been met OR
            // - Finished downloading, but is waiting for previous checkpoint
            // to finish applying; in this case, wakeUp will put it back into
            // RUNNING state and the sequence can proceed with applying
            work->wakeUp();
        }
        break;
    }
}

void
DownloadApplyTxsWork::resetIter()
{
    mCheckpointToQueue =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.mFirst);
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
    mRunning.clear();
}

bool
DownloadApplyTxsWork::hasNext()
{
    wakeWaitingWork();
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
