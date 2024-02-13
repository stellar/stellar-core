// Copyright 2015 Stellar Development Foundation and contributors. Licensed
// under the Apache License, Version 2.0. See the COPYING file at the root
// of this distribution or at http://www.apache.org/licenses/LICENSE-2.0

#pragma once

#include "history/HistoryArchive.h"
#include "historywork/GetHistoryArchiveStateWork.h"
#include "work/Work.h"

namespace stellar
{

class TmpDir;

class FetchRecentQsetsWork : public Work
{
    std::shared_ptr<TmpDir> mDownloadDir;
    uint32_t mLedgerNum;
    std::shared_ptr<GetHistoryArchiveStateWork> mGetHistoryArchiveStateWork;
    std::shared_ptr<BasicWork> mDownloadSCPMessagesWork;

  public:
    FetchRecentQsetsWork(Application& app, uint32_t ledgerNum);
    ~FetchRecentQsetsWork() = default;
    void doReset() override;
    BasicWork::State doWork() override;
};
}
