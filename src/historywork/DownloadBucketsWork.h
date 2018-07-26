// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0
#pragma once

#include "BatchWork.h"
#include "bucket/Bucket.h"
#include "historywork/Progress.h"
#include "medida/meter.h"
#include "medida/metrics_registry.h"

namespace stellar
{

class DownloadBucketsWork : public BatchWork
{
  public:
    DownloadBucketsWork(Application& app, WorkParent& parent,
                        std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                        std::vector<std::string> hashes,
                        TmpDir const& downloadDir);
    ~DownloadBucketsWork() override;
    bool hasNext() override;
    std::shared_ptr<BatchableWork> yieldMoreWork() override;
    void resetIter() override;
    std::string getStatus() const override;
    void notify(std::string const& child) override;

  private:
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;
    std::vector<std::string> mHashes;
    std::vector<std::string>::const_iterator mNextBucketIter;

    TmpDir const& mDownloadDir;

    // Download Metrics
    medida::Meter& mDownloadBucketStart;
    medida::Meter& mDownloadBucketSuccess;
    medida::Meter& mDownloadBucketFailure;
};
}
