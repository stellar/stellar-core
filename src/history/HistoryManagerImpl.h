#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "history/CheckpointBuilder.h"
#include "history/HistoryManager.h"
#include "util/TmpDir.h"
#include "work/Work.h"
#include <memory>

namespace medida
{
class Meter;
class Timer;
}

namespace stellar
{

class Application;
class Work;

class HistoryManagerImpl : public HistoryManager
{
    Application& mApp;
    std::unique_ptr<TmpDir> mWorkDir;
    std::shared_ptr<BasicWork> mPublishWork;

    std::atomic<int> mPublishQueued{0};
    medida::Meter& mPublishSuccess;
    medida::Meter& mPublishFailure;

    medida::Timer& mEnqueueToPublishTimer;
    UnorderedMap<uint32_t, std::chrono::steady_clock::time_point> mEnqueueTimes;
    CheckpointBuilder mCheckpointBuilder;

#ifdef BUILD_TESTS
    bool mPublicationEnabled{true};
#endif

  public:
    HistoryManagerImpl(Application& app);
    ~HistoryManagerImpl() override;

    void logAndUpdatePublishStatus() override;

    bool maybeQueueHistoryCheckpoint(uint32_t lcl) override;

    void queueCurrentHistory(uint32_t lcl) override;

    void takeSnapshotAndPublish(HistoryArchiveState const& has);

    size_t publishQueuedHistory() override;

    void maybeCheckpointComplete(uint32_t lcl) override;
    void dropSQLBasedPublish() override;

    std::vector<std::string>
    getMissingBucketsReferencedByPublishQueue() override;

    void historyPublished(uint32_t ledgerSeq,
                          std::vector<std::string> const& originalBuckets,
                          bool success) override;
    void appendTransactionSet(uint32_t ledgerSeq,
                              TxSetXDRFrameConstPtr const& txSet,
                              TransactionResultSet const& resultSet) override;
    void appendLedgerHeader(LedgerHeader const& header) override;
    void restoreCheckpoint(uint32_t lcl) override;

    Config const& getConfig() const override;
    std::string const& getTmpDir() override;

    std::string localFilename(std::string const& basename) override;

    uint64_t getPublishQueueCount() const override;
    uint64_t getPublishSuccessCount() const override;
    uint64_t getPublishFailureCount() const override;

#ifdef BUILD_TESTS
    void setPublicationEnabled(bool enabled) override;
    // Throw after inseting ledger `n` into a checkpoint
    uint32_t mThrowOnAppend{0};
    CheckpointBuilder&
    getCheckpointBuilder()
    {
        return mCheckpointBuilder;
    }
#endif
};
}
