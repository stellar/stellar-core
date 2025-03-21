// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "bucket/LiveBucket.h"
#include "historywork/Progress.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"
#include "util/TmpDir.h"
#include "work/BatchWork.h"

namespace stellar
{

class HistoryArchive;

class DownloadBucketsWork : public BatchWork
{
    std::map<std::string, std::shared_ptr<LiveBucket>>& mBuckets;
    std::vector<std::string> mHashes;
    std::vector<std::string>::const_iterator mNextBucketIter;
    TmpDir const& mDownloadDir;
    std::shared_ptr<HistoryArchive> mArchive;

    // Store indexes of downloaded buckets
    std::map<int, std::unique_ptr<LiveBucketIndex const>> mIndexMap;

    // Must be held when accessing mIndexMap
    std::mutex mIndexMapMutex;
    int mIndexId{0};

  public:
    DownloadBucketsWork(
        Application& app,
        std::map<std::string, std::shared_ptr<LiveBucket>>& buckets,
        std::vector<std::string> hashes, TmpDir const& downloadDir,
        std::shared_ptr<HistoryArchive> archive = nullptr);
    ~DownloadBucketsWork() = default;
    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
};
}
