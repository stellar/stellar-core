// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "historywork/BucketDownloadWork.h"

namespace stellar
{

struct HistoryArchiveState;

class RepairMissingBucketsWork : public BucketDownloadWork
{

    typedef std::function<void(asio::error_code const& ec)> handler;
    handler mEndHandler;

  public:
    RepairMissingBucketsWork(Application& app, WorkParent& parent,
                             HistoryArchiveState const& localState,
                             handler endHandler);
    ~RepairMissingBucketsWork();
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};
}
