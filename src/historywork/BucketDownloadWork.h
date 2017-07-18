// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

class Bucket;
class TmpDir;

class BucketDownloadWork : public Work
{
  protected:
    HistoryArchiveState mLocalState;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

  public:
    BucketDownloadWork(Application& app, WorkParent& parent,
                       std::string const& uniqueName,
                       HistoryArchiveState const& localState,
                       size_t maxRetries = RETRY_A_FEW);
    ~BucketDownloadWork();
    void onReset() override;
    void takeDownloadDir(BucketDownloadWork& other);
};
}
