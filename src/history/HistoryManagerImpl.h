#pragma once

// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#include "bucket/PublishQueueBuckets.h"
#include "history/HistoryManager.h"
#include "util/TmpDir.h"
#include "work/Work.h"
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
    std::shared_ptr<BasicWork> mPublishWork;

    PublishQueueBuckets mPublishQueueBuckets;
    bool mPublishQueueBucketsFilled{false};

    int mPublishQueued{0};
    medida::Meter& mPublishSuccess;
    medida::Meter& mPublishFailure;

    medida::Timer& mEnqueueToPublishTimer;
    UnorderedMap<uint32_t, std::chrono::steady_clock::time_point> mEnqueueTimes;

    PublishQueueBuckets::BucketCount loadBucketsReferencedByPublishQueue();
#ifdef BUILD_TESTS
    bool mPublicationEnabled{true};
#endif

  public:
    HistoryManagerImpl(Application& app);
    ~HistoryManagerImpl() override;

    uint32_t getCheckpointFrequency() const override;

    void logAndUpdatePublishStatus() override;

    size_t publishQueueLength() const override;

    bool maybeQueueHistoryCheckpoint() override;

    void queueCurrentHistory() override;

    void takeSnapshotAndPublish(HistoryArchiveState const& has);

    uint32_t getMinLedgerQueuedToPublish() override;

    uint32_t getMaxLedgerQueuedToPublish() override;

    size_t publishQueuedHistory() override;

    std::vector<std::string>
    getMissingBucketsReferencedByPublishQueue() override;

    std::vector<std::string> getBucketsReferencedByPublishQueue() override;

    std::vector<HistoryArchiveState> getPublishQueueStates() override;

    void historyPublished(uint32_t ledgerSeq,
                          std::vector<std::string> const& originalBuckets,
                          bool success) override;

    InferredQuorum inferQuorum(uint32_t ledgerNum) override;

    std::string const& getTmpDir() override;

    std::string localFilename(std::string const& basename) override;

    uint64_t getPublishQueueCount() const override;
    uint64_t getPublishSuccessCount() const override;
    uint64_t getPublishFailureCount() const override;

#ifdef BUILD_TESTS
    void setPublicationEnabled(bool enabled) override;
#endif
};
}
