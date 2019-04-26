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

struct HistoryArchiveState;

class RepairMissingBucketsWork : public Work
{
    using Handler = std::function<void(asio::error_code const& ec)>;
    Handler mEndHandler;
    bool mChildrenStarted{false};

    HistoryArchiveState mLocalState;
    std::unique_ptr<TmpDir> mDownloadDir;
    std::map<std::string, std::shared_ptr<Bucket>> mBuckets;

  public:
    RepairMissingBucketsWork(Application& app,
                             HistoryArchiveState const& localState,
                             Handler endHandler);
    ~RepairMissingBucketsWork() = default;

  protected:
    void doReset() override;
    State doWork() override;

    void onFailureRaise() override;
    void onSuccess() override;
};
}
