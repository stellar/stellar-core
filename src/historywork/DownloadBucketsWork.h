// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "bucket/Bucket.h"
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
    std::map<std::string, std::shared_ptr<Bucket>>& mBuckets;
    std::vector<std::string> mHashes;
    std::vector<std::string>::const_iterator mNextBucketIter;
    TmpDir const& mDownloadDir;
    std::shared_ptr<HistoryArchive> mArchive;

  public:
    DownloadBucketsWork(Application& app,
                        std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                        std::vector<std::string> hashes,
                        TmpDir const& downloadDir,
                        std::shared_ptr<HistoryArchive> archive = nullptr);
    ~DownloadBucketsWork() = default;
    std::string getStatus() const override;

  protected:
    bool hasNext() const override;
    std::shared_ptr<BasicWork> yieldMoreWork() override;
    void resetIter() override;
};
}
