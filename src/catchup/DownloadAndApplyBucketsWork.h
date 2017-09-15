// Copyright 2017 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

class Bucket;
class TmpDir;
struct LedgerHeaderHistoryEntry;

class DownloadAndApplyBucketsWork : public Work
{
  public:
    DownloadAndApplyBucketsWork(Application& app, WorkParent& parent,
                                HistoryArchiveState remoteState,
                                std::vector<std::string> bucketsHashes,
                                LedgerHeaderHistoryEntry applyAt,
                                TmpDir const& downloadDir);
    std::string getStatus() const override;
    void onReset() override;
    Work::State onSuccess() override;

  protected:
    TmpDir const& mDownloadDir;
    std::shared_ptr<Work> mDownloadBucketsWork;
    std::shared_ptr<Work> mApplyWork;
    HistoryArchiveState mRemoteState;
    std::vector<std::string> mBucketsHashes;
    LedgerHeaderHistoryEntry mApplyAt;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;
};
}
