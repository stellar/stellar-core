#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/HistoryManager.h"
#include "util/TmpDir.h"
#include <memory>

namespace medida
{
class Meter;
}

namespace stellar
{

class Application;
class Work;

class HistoryManagerImpl : public HistoryManager
{
    Application& mApp;
    std::unique_ptr<TmpDir> mWorkDir;
    std::shared_ptr<Work> mPublishWork;
    std::shared_ptr<Work> mCatchupWork;

    medida::Meter& mPublishSkip;
    medida::Meter& mPublishQueue;
    medida::Meter& mPublishDelay;
    medida::Meter& mPublishStart;
    medida::Meter& mPublishSuccess;
    medida::Meter& mPublishFailure;

    medida::Meter& mCatchupStart;
    medida::Meter& mCatchupSuccess;
    medida::Meter& mCatchupFailure;

  public:
    HistoryManagerImpl(Application& app);
    ~HistoryManagerImpl() override;

    std::shared_ptr<HistoryArchive>
    selectRandomReadableHistoryArchive() override;

    uint32_t getCheckpointFrequency() override;
    uint32_t prevCheckpointLedger(uint32_t ledger) override;
    uint32_t nextCheckpointLedger(uint32_t ledger) override;
    uint64_t nextCheckpointCatchupProbe(uint32_t ledger) override;

    void logAndUpdateStatus(bool contiguous) override;

    size_t publishQueueLength() const override;

    bool maybeQueueHistoryCheckpoint() override;

    void queueCurrentHistory() override;

    void takeSnapshotAndPublish(HistoryArchiveState const& has);

    bool hasAnyWritableHistoryArchive() override;

    uint32_t getMinLedgerQueuedToPublish() override;

    uint32_t getMaxLedgerQueuedToPublish() override;

    size_t publishQueuedHistory() override;

    std::vector<std::string>
    getMissingBucketsReferencedByPublishQueue() override;

    std::vector<std::string> getBucketsReferencedByPublishQueue() override;

    std::vector<HistoryArchiveState> getPublishQueueStates();

    void historyPublished(uint32_t ledgerSeq, bool success) override;

    void historyCaughtup() override;

    void downloadMissingBuckets(
        HistoryArchiveState desiredState,
        std::function<void(asio::error_code const& ec)> handler) override;

    void catchupHistory(
        uint32_t initLedger, CatchupMode mode,
        std::function<void(asio::error_code const& ec, CatchupMode mode,
                           LedgerHeaderHistoryEntry const& lastClosed)>
            handler,
        bool manualCatchup) override;

    HistoryArchiveState getLastClosedHistoryArchiveState() const override;

    InferredQuorum inferQuorum() override;

    std::string const& getTmpDir() override;

    std::string localFilename(std::string const& basename) override;

    uint64_t getPublishSkipCount() override;
    uint64_t getPublishQueueCount() override;
    uint64_t getPublishDelayCount() override;
    uint64_t getPublishStartCount() override;
    uint64_t getPublishSuccessCount() override;
    uint64_t getPublishFailureCount() override;

    uint64_t getCatchupStartCount() override;
    uint64_t getCatchupSuccessCount() override;
    uint64_t getCatchupFailureCount() override;
};
}
