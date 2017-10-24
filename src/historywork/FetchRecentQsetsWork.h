// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "work/Work.h"

namespace stellar
{

class TmpDir;
struct InferredQuorum;

class FetchRecentQsetsWork : public Work
{

    typedef std::function<void(asio::error_code const& ec)> handler;
    handler mEndHandler;
    std::unique_ptr<TmpDir> mDownloadDir;
    InferredQuorum& mInferredQuorum;
    HistoryArchiveState mRemoteState;
    std::shared_ptr<Work> mGetHistoryArchiveStateWork;
    std::shared_ptr<Work> mDownloadSCPMessagesWork;

  public:
    FetchRecentQsetsWork(Application& app, WorkParent& parent,
                         InferredQuorum& iq, handler endHandler);
    ~FetchRecentQsetsWork();
    void onReset() override;
    void onFailureRaise() override;
    Work::State onSuccess() override;
};
}
