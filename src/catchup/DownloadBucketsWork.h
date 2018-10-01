// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "work/Work.h"

namespace medida
{
class Meter;
}

namespace stellar
{

class Bucket;
class TmpDir;

class DownloadBucketsWork : public Work
{
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;
    std::vector<std::string> mHashes;
    TmpDir const& mDownloadDir;
    bool mChildrenStarted{false};

  public:
    DownloadBucketsWork(Application& app, std::function<void()> callback,
                        std::map<std::string, std::shared_ptr<Bucket>>& buckets,
                        std::vector<std::string> hashes,
                        TmpDir const& downloadDir);
    ~DownloadBucketsWork() = default;

  protected:
    BasicWork::State doWork() override;
    void doReset() override;
};
}
