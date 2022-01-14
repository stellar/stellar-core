// Copyright 2019 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "catchup/DownloadApplyTxsWork.h"
#include "bucket/BucketList.h"
#include "bucket/BucketManager.h"
#include "catchup/ApplyCheckpointWork.h"
#include "history/FileTransferInfo.h"
#include "history/HistoryManager.h"
#include "historywork/GetAndUnzipRemoteFileWork.h"
#include "ledger/LedgerManager.h"
#include "work/ConditionalWork.h"
#include "work/WorkSequence.h"
#include "work/WorkWithCallback.h"

#include <Tracy.hpp>
#include <fmt/format.h>

namespace stellar
{

DownloadApplyTxsWork::DownloadApplyTxsWork(
    Application& app, TmpDir const& downloadDir, LedgerRange const& range,
    LedgerHeaderHistoryEntry& lastApplied, bool waitForPublish,
    std::shared_ptr<HistoryArchive> archive)
    : BatchWork(app, "download-apply-ledgers")
    , mRange(range)
    , mDownloadDir(downloadDir)
    , mLastApplied(lastApplied)
    , mCheckpointToQueue(
          app.getHistoryManager().checkpointContainingLedger(range.mFirst))
    , mWaitForPublish(waitForPublish)
    , mArchive(archive)
{
}

std::shared_ptr<BasicWork>
DownloadApplyTxsWork::yieldMoreWork()
{
    ZoneScoped;
    if (!hasNext())
    {
        throw std::runtime_error("Work has no more children to iterate over!");
    }

    CLOG_INFO(History,
              "Downloading, unzipping and applying {} for checkpoint {}",
              HISTORY_FILE_TYPE_TRANSACTIONS, mCheckpointToQueue);
    FileTransferInfo ft(mDownloadDir, HISTORY_FILE_TYPE_TRANSACTIONS,
                        mCheckpointToQueue);
    auto getAndUnzip =
        std::make_shared<GetAndUnzipRemoteFileWork>(mApp, ft, mArchive);

    auto const& hm = mApp.getHistoryManager();
    auto low = hm.firstLedgerInCheckpointContaining(mCheckpointToQueue);
    auto high = std::min(mCheckpointToQueue, mRange.last());

    TmpDir const& dir = mDownloadDir;
    uint32_t checkpoint = mCheckpointToQueue;
    auto getFileWeak = std::weak_ptr<GetAndUnzipRemoteFileWork>(getAndUnzip);

    OnFailureCallback cb = [getFileWeak, checkpoint, &dir]() {
        auto getFile = getFileWeak.lock();
        if (getFile)
        {
            auto archive = getFile->getArchive();
            if (archive)
            {
                FileTransferInfo ti(dir, HISTORY_FILE_TYPE_TRANSACTIONS,
                                    checkpoint);
                CLOG_ERROR(History, "Archive {} maybe contains corrupt file {}",
                           archive->getName(), ti.remoteName());
            }
        }
    };

    auto apply = std::make_shared<ApplyCheckpointWork>(
        mApp, mDownloadDir, LedgerRange::inclusive(low, high), cb);

    std::vector<std::shared_ptr<BasicWork>> seq{getAndUnzip};

    auto maybeWaitForMerges = [](Application& app) {
        if (app.getConfig().CATCHUP_WAIT_MERGES_TX_APPLY_FOR_TESTING)
        {
            auto& bl = app.getBucketManager().getBucketList();
            bl.resolveAnyReadyFutures();
            return bl.futuresAllResolved();
        }
        else
        {
            return true;
        }
    };

    if (mLastYieldedWork)
    {
        auto prev = mLastYieldedWork;
        bool pqFellBehind = false;
        auto predicate = [prev, pqFellBehind, waitForPublish = mWaitForPublish,
                          maybeWaitForMerges](Application& app) mutable {
            if (!prev)
            {
                throw std::runtime_error("Download and apply txs: related Work "
                                         "is destroyed unexpectedly");
            }

            // First, ensure download work is finished
            if (prev->getState() != State::WORK_SUCCESS)
            {
                return false;
            }

            // Second, check if publish queue isn't too far off
            bool res = true;
            if (waitForPublish)
            {
                auto& hm = app.getHistoryManager();
                auto length = hm.publishQueueLength();
                if (length <= CatchupWork::PUBLISH_QUEUE_UNBLOCK_APPLICATION)
                {
                    pqFellBehind = false;
                }
                else if (length > CatchupWork::PUBLISH_QUEUE_MAX_SIZE)
                {
                    pqFellBehind = true;
                }
                res = !pqFellBehind;
            }
            return res && maybeWaitForMerges(app);
        };
        seq.push_back(std::make_shared<ConditionalWork>(
            mApp, "conditional-" + apply->getName(), predicate, apply));
    }
    else
    {
        seq.push_back(std::make_shared<ConditionalWork>(
            mApp, "wait-merges" + apply->getName(), maybeWaitForMerges, apply));
    }

    seq.push_back(std::make_shared<WorkWithCallback>(
        mApp, "delete-transactions-" + std::to_string(mCheckpointToQueue),
        [ft](Application& app) {
            try
            {
                std::filesystem::remove(
                    std::filesystem::path(ft.localPath_nogz()));
                CLOG_DEBUG(History, "Deleted transactions {}",
                           ft.localPath_nogz());
                return true;
            }
            catch (std::filesystem::filesystem_error const& e)
            {
                CLOG_ERROR(History, "Could not delete transactions {}: {}",
                           ft.localPath_nogz(), e.what());
                return false;
            }
        }));

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
    if (mRange.mCount == 0)
    {
        return false;
    }
    auto last =
        mApp.getHistoryManager().checkpointContainingLedger(mRange.last());
    return mCheckpointToQueue <= last;
}

void
DownloadApplyTxsWork::onSuccess()
{
    mLastApplied = mApp.getLedgerManager().getLastClosedLedgerHeader();
}

std::string
DownloadApplyTxsWork::getStatus() const
{
    auto& hm = mApp.getHistoryManager();
    auto first = hm.checkpointContainingLedger(mRange.mFirst);
    auto last =
        (mRange.mCount == 0 ? first
                            : hm.checkpointContainingLedger(mRange.last()));

    auto checkpointsStarted =
        (mCheckpointToQueue - first) / hm.getCheckpointFrequency();
    auto checkpointsApplied = checkpointsStarted - getNumWorksInBatch();

    auto totalCheckpoints = (last - first) / hm.getCheckpointFrequency() + 1;
    return fmt::format(
        FMT_STRING("Download & apply checkpoints: num checkpoints left to "
                   "apply:{:d} ({:d}% done)"),
        totalCheckpoints - checkpointsApplied,
        100 * checkpointsApplied / totalCheckpoints);
}
}
